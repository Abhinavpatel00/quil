#include "document.h"
#include "syntax.h"
#include "font.h"
#include "frame.h"
#include "vfx.h"
#include "render_vk.h"
#include "buffers.h"
#include "config.h"
#include "modal.h"
#include "picker.h"
#include "search.h"
#include "theme.h"
#include "session.h"
#include "terminal.h"

#include <GLFW/glfw3.h>
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <ctype.h>

typedef struct App
{
    BufferManager bm;
    Document*     doc;
    FontSystem*   fs;
    VkRenderer*   vk;
    DrawList*     dl;
    FrameScratch* scratch;
    LineLayout    layouts[QUILL_MAX_VISIBLE];
    uint32_t      nlayouts;
    FrameParams   params;
    GLFWwindow*   window;
    Platform      platform; /* mu_input + GLFW backend (adopts `window`) */
    SelectionSet  ss;
    ModalState    modal;
    Picker*       picker;
    MotionConfig  cfg;
    uint32_t      size_px;
    uint32_t      scale;
    bool          dragging;
    bool          redraw;
    uint64_t      parse_due;
    float         view_top;
    float         target_top;
    float         scroll_vel;
    /* Horizontal scroll: target_x is what the scroll policy asks for, view_x
     * is where the view actually is (they only differ while springing in
     * dramatic mode), and params.scroll_x is fed the rendered value so the
     * renderer and hit-testing always agree on what is on screen. */
    float         target_x;
    float         view_x;
    float         scroll_x_vel;
    uint64_t      last_frame_ns;
    bool          animating;
    bool          blink_active;
    CursorAnim    cursor_anim[QUILL_MAX_CURSORS];
    VfxState      vfx;
    bool          search_open;
    char          search_query[256];
    uint32_t      search_len;
    int           search_dir;
    Terminal*     term;
    bool          term_active;
    bool          term_esc_pending;  /* saw Ctrl-\, waiting for Ctrl-n to exit */
    bool          term_skip_char;    /* key callback handled/suppressed this char */
    char          root[1024];        /* picker root (folder given on the CLI) */
    bool          open_files_on_start; /* root was a directory: show the picker */
    /* Reusable clipboard buffer: copy/paste stage through here instead of
     * malloc/free on every operation. */
    char          clipboard_buf[QUILL_CLIPBOARD_CAP];
} App;

static const char SAMPLE_C[] =
"/* Quill -- a Vulkan text editor backed by mu_quill.\n"
" * Tree-sitter only: no LSP, no completion.\n"
" */\n"
"#include <stdio.h>\n"
"#include <stdint.h>\n"
"\n"
"typedef struct Point { float x, y; } Point;\n"
"\n"
"static float length(Point p)\n"
"{\n"
"    return sqrtf(p.x * p.x + p.y * p.y);\n"
"}\n"
"\n"
"int main(void)\n"
"{\n"
"    Point a = { 3.0f, 4.0f };\n"
"    const char* msg = \"hello, quill\";\n"
"    for (int i = 0; i < 10; ++i) {\n"
"        printf(\"%s %d: %.2f\\n\", msg, i, length(a));\n"
"    }\n"
"    return 0;\n"
"}\n";

static uint64_t app_now_ms(void)
{
    return (uint64_t)(mu_time_now() * 1000.0 / mu_time_freq());
}

static void note_edit(App* a)
{
    /* Small files reparse before the next render (0 ms). Large files use a
     * one-frame debounce; highlights stay visible (optimistic) meanwhile, so
     * this only affects how fast the edited span's color corrects, never
     * whether the region blanks. */
    a->parse_due = app_now_ms() + (a->doc->q.text_len <= 262144u ? 0u : 16u);
    a->redraw = true;
}

static uint32_t visible_rows(const App* a)
{
    float lh = font_line_height(a->fs);
    float status_h = lh + 16.0f;
    float h = (float)a->params.height > status_h ? (float)a->params.height - status_h : 0.0f;
    uint32_t rows = (uint32_t)(h / lh);
    return rows ? rows : 1u;
}

static float gutter_width(const App* a)
{
    uint32_t digits = 1u, t = a->doc->q.nlines ? a->doc->q.nlines : 1u;
    while (t >= 10u) { t /= 10u; digits++; }
    return a->params.line_numbers ? (float)digits * font_advance(a->fs) + 16.0f : 0.0f;
}

static void clamp_scroll(App* a)
{
    uint32_t n = a->doc->q.nlines ? a->doc->q.nlines : 1u;
    uint32_t rows = visible_rows(a);
    uint32_t max_top = n > rows ? n - rows : 0u;
    if (a->target_top > (float)max_top) a->target_top = (float)max_top;
    if (a->target_top < 0.0f) a->target_top = 0.0f;
    if (a->target_x < 0.0f) a->target_x = 0.0f;
    if (a->view_x < 0.0f) a->view_x = 0.0f;
}

static void set_target_top(App* a, float top)
{
    a->target_top = top;
    clamp_scroll(a);
    if ((a->cfg.dramatic ? a->cfg.dramatic_ms : a->cfg.scroll_ms) == 0u ||
        a->cfg.reduced_motion || a->cfg.style == MOTION_INSTANT)
    {
        a->view_top = a->target_top;
        a->scroll_vel = 0.0f;
    }
    a->redraw = true;
}

/* Cursor position within its line, measured with the renderer's own full-line
 * shaping: c.col is a byte offset, so tabs (which expand to their next stop)
 * and multi-byte UTF-8 would drift if this multiplied the nominal advance. */
typedef struct CursorMetrics
{
    float x;        /* cursor x inside the line, in pixels */
    float w;        /* cursor width (bar in Insert, cell in Normal) */
    float text_x;   /* left edge of the text area (after the gutter) */
    float text_w;   /* width of the text area */
} CursorMetrics;

static void cursor_metrics(const App* a, mu_quill_cursor c, CursorMetrics* m)
{
    uint8_t line[QUILL_MAX_LINE];
    uint32_t line_start = mu_quill_line_offset(&a->doc->q, c.line);
    uint32_t line_end = a->doc->q.text_len;
    uint32_t len;
    const ShapedGlyph* glyphs = NULL;
    uint32_t nglyphs = 0u;
    m->x = 0.0f;
    m->text_x = gutter_width(a) + 8.0f;
    m->text_w = (float)a->params.width > m->text_x ? (float)a->params.width - m->text_x : 0.0f;
    m->w = a->modal.mode == QMODE_INSERT ? 2.0f : font_advance(a->fs);
    if (c.line + 1u < a->doc->q.nlines)
        line_end = mu_quill_line_offset(&a->doc->q, c.line + 1u) - 1u;
    len = line_end - line_start;
    if (len > QUILL_MAX_LINE) len = QUILL_MAX_LINE;
    if (len == 0u) return;
    mu_quill_text(&a->doc->q, line_start, len, (char*)line);
    nglyphs = font_shape_line(a->fs, line, len, a->params.tab_width, &glyphs);
    m->x = font_line_x_for_byte(glyphs, nglyphs, c.col);
}

/* Dramatic camera: park the cursor at a fixed lead point of the viewport
 * instead of scrolling the minimum distance needed to reveal it, and let the
 * existing springs carry the view there. This is only a scroll *policy* -- it
 * writes the same target_top/scroll_x the normal path writes, so rendering,
 * hit-testing, selection and the cursor animation need no camera-aware code,
 * and the renderer's draw list keeps its final pixel coordinates.
 *
 * The 1/3 lead is what makes motion read as a camera pan rather than a jump:
 * every cursor move drags the page under a cursor that barely appears to
 * move. Clamped targets keep the page pinned at the top/left edges, so the
 * first and last screens of a file behave exactly like they do without it. */
static void dramatic_target(App* a)
{
    const SelCursor* p = ss_primary_const(&a->ss);
    mu_quill_cursor c = mu_quill_offset_to_cursor(&a->doc->q, p->head);
    uint32_t rows = visible_rows(a);
    CursorMetrics m;
    float lead_x = a->cfg.dramatic_lead_x;
    float lead_y = a->cfg.dramatic_lead_y;
    float top, scroll_x;

    if (lead_x < 0.0f) lead_x = 0.0f;
    if (lead_x > 0.5f) lead_x = 0.5f;
    if (lead_y < 0.0f) lead_y = 0.0f;
    if (lead_y > 0.5f) lead_y = 0.5f;

    top = (float)c.line - (float)rows * lead_y;
    if (top != a->target_top) set_target_top(a, top);
    else a->redraw = true;

    cursor_metrics(a, c, &m);
    scroll_x = m.x - lead_x * m.text_w;
    if (scroll_x < 0.0f) scroll_x = 0.0f;
    /* Never scroll past the end of the line: the last character stays at or
     * left of the right edge so short files do not drift into empty space. */
    if (scroll_x > m.x + m.w) scroll_x = m.x + m.w;
    if (scroll_x != a->target_x)
    {
        a->target_x = scroll_x;
        a->redraw = true;
    }
}

static void ensure_cursor_visible(App* a)
{
    const SelCursor* p;
    mu_quill_cursor c;
    uint32_t rows;
    float top;
    CursorMetrics m;

    if (a->cfg.dramatic)
    {
        dramatic_target(a);
        return;
    }

    p = ss_primary_const(&a->ss);
    c = mu_quill_offset_to_cursor(&a->doc->q, p->head);
    rows = visible_rows(a);
    top = a->target_top;
    if ((float)c.line < top) top = (float)c.line;
    else if ((float)c.line >= top + (float)rows) top = (float)c.line - (float)rows + 1.0f;
    if (top != a->target_top) set_target_top(a, top);

    /* Scroll by the smallest amount in either direction, so moving left off
     * the viewport reveals that character without resetting the whole line. */
    cursor_metrics(a, c, &m);
    {
        float scroll_x = a->target_x;
        if (m.x < scroll_x)
            scroll_x = m.x;
        else if (m.text_w > m.w && m.x + m.w > scroll_x + m.text_w)
            scroll_x = m.x + m.w - m.text_w;
        if (scroll_x < 0.0f) scroll_x = 0.0f;
        if (scroll_x != a->target_x)
        {
            a->target_x = scroll_x;
            a->redraw = true;
        }
    }
}

static void sync_after_edit(App* a)
{
    ss_sync_doc(&a->ss, a->doc);
    ensure_cursor_visible(a);
    note_edit(a);
}

/* Flush the live scroll position into the active Document so it survives a
 * buffer switch or a session save. */
static void save_current_view(App* a)
{
    if (!a->doc) return;
    a->doc->view_top = a->target_top;
    a->doc->scroll_x = a->target_x;
}

/* Switch the active buffer, preserving each buffer's own scroll position. */
static void app_set_doc(App* a, Document* d)
{
    if (!d) return;
    if (d != a->doc)
    {
        save_current_view(a);
        a->doc = d;
        a->view_top = a->target_top = d->view_top;
        a->scroll_vel = 0.0f;
        a->target_x = a->view_x = a->params.scroll_x = d->scroll_x;
        clamp_scroll(a);
    }
    ss_from_doc(&a->ss, d);
    if (a->picker) picker_set_doc(a->picker, d);
    ensure_cursor_visible(a);
    a->redraw = true;
}

/* ---- integrated terminal ------------------------------------------------ */

static void term_grid_size(App* a, uint32_t* cols, uint32_t* rows)
{
    float advance = font_advance(a->fs);
    float lh = font_line_height(a->fs);
    float status_h = lh + 16.0f;
    float h = (float)a->params.height > status_h ? (float)a->params.height - status_h : 0.0f;
    *cols = advance > 0.0f ? (uint32_t)((float)a->params.width / advance) : 80u;
    *rows = lh > 0.0f ? (uint32_t)(h / lh) : 24u;
    if (*cols < 1u) *cols = 1u;
    if (*rows < 1u) *rows = 1u;
}

static void term_open(App* a)
{
    uint32_t cols, rows;
    if (a->term) { a->term_active = true; a->redraw = true; return; }
    term_grid_size(a, &cols, &rows);
    a->term = terminal_create(cols, rows);
    if (!a->term) return;
    a->term_active = true;
    a->term_esc_pending = false;
    a->redraw = true;
}

static void term_close(App* a)
{
    if (a->term) { terminal_destroy(a->term); a->term = NULL; }
    a->term_active = false;
    a->term_esc_pending = false;
    a->redraw = true;
}

static void render_term_status(App* a)
{
    float lh = font_line_height(a->fs);
    float status_h = lh + 16.0f;
    float sy = (float)a->params.height - status_h;
    float bg[4], fg[4], accent[4];
    char status[300];
    int len;
    quill_color_linear(QUILL_THEME.status_bg, bg);
    quill_color_linear(QUILL_THEME.status_fg, fg);
    quill_color_linear(QUILL_THEME.accent, accent);
    frame_rect(a->dl, 0.0f, sy, (float)a->params.width, status_h, bg);
    len = snprintf(status, sizeof status, "TERMINAL  %s%s  %s",
                   terminal_title(a->term),
                   terminal_exited(a->term) ? "  [exited]" : "",
                   terminal_scrolled(a->term) ? "[scroll]  " : "");
    if (len < 0) len = 0;
    frame_text(a->fs, a->dl, status, (uint32_t)len, 8.0f,
               sy + (status_h - lh) * 0.5f + font_baseline(a->fs), fg, (float)a->params.width - 8.0f);
    (void)accent;
}

static void open_picker_source_items(App* a, Picker* picker, PickerSource src, uint32_t max_items);

static Picker* ensure_picker(App* a)
{
    if (!a->picker)
    {
        a->picker = (Picker*)MU_CALLOC(1u, sizeof *a->picker);
        if (!a->picker) return NULL;
        picker_init(a->picker, a->doc, a->root[0] ? a->root : NULL);
    }
    return a->picker;
}

static bool is_word_byte(uint8_t c)
{
    if (c >= 0x80u) return true; /* non-ASCII bytes belong to the word */
    return c == '_' || isalnum((int)c) != 0;
}

static bool doc_byte_at(const Document* d, uint32_t off, uint8_t* out)
{
    if (off >= d->q.text_len) return false;
    mu_quill_text(&d->q, off, 1u, (char*)out);
    return true;
}

/* Seed for Space s w: the word under the primary cursor, or the visual
 * selection when there is one (so a selected identifier, or a phrase, can be
 * grepped as-is). The result is trimmed, single-line and literal -- the picker
 * grep is a case-insensitive substring search, not a regex -- so nothing needs
 * escaping. Returns false when there is nothing useful to search for. */
static bool word_under_cursor(App* a, char* out, size_t cap)
{
    const SelCursor* p;
    uint32_t start, end;
    uint8_t left = 0u, right = 0u;
    size_t len, skip;

    if (!a->doc || cap < 2u) return false;
    out[0] = 0;
    p = ss_primary_const(&a->ss);

    if (ss_min(p) != ss_max(p) || (a->modal.mode == QMODE_VISUAL && p->anchor != p->head))
    {
        start = ss_min(p);
        end = ss_max(p);
        /* Visual char mode is inclusive of the head character. */
        if (a->modal.mode == QMODE_VISUAL && end < a->doc->q.text_len) end++;
    }
    else
    {
        uint32_t anchor = p->head;
        /* quill_word_range() only reports the byte *before* a cursor that sits
         * on whitespace, punctuation or the end of the line, so resolve the
         * word from the word byte itself: the one under the cursor, else the
         * one it sits just past. In a blank column there is no word at all and
         * Space s w opens a plain grep prompt instead of searching for a
         * single stray character. */
        if (!(doc_byte_at(a->doc, p->head, &right) && is_word_byte(right)))
        {
            if (p->head == 0u) return false;
            if (!doc_byte_at(a->doc, p->head - 1u, &left) || !is_word_byte(left)) return false;
            anchor = p->head - 1u;
        }
        quill_word_range(a->doc, anchor, &start, &end);
        if (end <= start) return false;
    }

    len = end - start;
    if (len > cap - 1u) len = cap - 1u;
    mu_quill_text(&a->doc->q, start, (uint32_t)len, out);
    out[len] = 0;

    /* Grep is line oriented: stop at the first line break. */
    for (skip = 0u; skip < len; ++skip)
        if (out[skip] == '\n' || out[skip] == '\r') { out[skip] = 0; len = skip; break; }
    /* Trim the blanks a selection may carry on either edge. */
    while (len > 0u && isspace((unsigned char)out[len - 1u])) out[--len] = 0;
    skip = 0u;
    while (skip < len && isspace((unsigned char)out[skip])) skip++;
    if (skip > 0u) memmove(out, out + skip, len - skip + 1u);
    return out[0] != 0;
}

static void open_picker_source(App* a, PickerSource src, uint32_t max_items)
{
    Picker* picker = ensure_picker(a);
    if (picker) open_picker_source_items(a, picker, src, max_items);
}

/* Same as open_picker_source(), but seeds the query first (Space s w). */
static void open_picker_query(App* a, PickerSource src, const char* query)
{
    Picker* picker = ensure_picker(a);
    if (!picker) return;
    picker_open_query(picker, src, query);
    a->redraw = true;
}

static void open_picker_source_items(App* a, Picker* picker, PickerSource src, uint32_t max_items)
{
    if (src == PSRC_BUFFERS)
    {
        PickerItem items[QUILL_MAX_BUFFERS];
        uint32_t i;
        for (i = 0u; i < a->bm.count; ++i)
        {
            memset(&items[i], 0, sizeof items[i]);
            snprintf(items[i].label, sizeof items[i].label, "%.240s%s", a->bm.docs[i]->path[0] ? a->bm.docs[i]->path : "[No Name]", a->bm.docs[i]->dirty ? " *" : "");
            snprintf(items[i].path, sizeof items[i].path, "%s", a->bm.docs[i]->path);
            items[i].offset = i;
        }
        picker_open_items(picker, src, items, a->bm.count);
    }
    else if (src == PSRC_RECENT)
    {
        PickerItem items[QUILL_MAX_BUFFERS];
        uint32_t i, n = a->bm.nrecent;
        if (max_items != 0u && n > max_items) n = max_items;
        for (i = 0u; i < n; ++i)
        {
            memset(&items[i], 0, sizeof items[i]);
            snprintf(items[i].label, sizeof items[i].label, "%.250s", a->bm.recent[i]);
            snprintf(items[i].path, sizeof items[i].path, "%s", a->bm.recent[i]);
        }
        picker_open_items(picker, src, items, n);
    }
    else
        picker_open(picker, src);
    a->redraw = true;
}

/* Space s w: live grep seeded with the word under the cursor. With nothing to
 * search for (empty buffer, cursor in blank space) it degrades to the plain
 * grep prompt rather than doing nothing. */
static void open_picker_grep_word(App* a)
{
    char word[128];
    if (!ensure_picker(a)) return;
    if (!word_under_cursor(a, word, sizeof word))
    {
        open_picker_source(a, PSRC_GREP, 0u);
        return;
    }
    open_picker_query(a, PSRC_GREP, word);
}

static void execute_command(App* a, const char* cmd)
{
    if (strcmp(cmd, "Save") == 0) doc_save_file(a->doc, NULL);
    else if (strcmp(cmd, "Quit") == 0) { if (a->window) glfwSetWindowShouldClose(a->window, 1); }
    else if (strcmp(cmd, "Toggle Line Numbers") == 0) a->params.line_numbers = !a->params.line_numbers;
    else if (strcmp(cmd, "Dramatic Mode") == 0)
    {
        a->cfg.dramatic = !a->cfg.dramatic;
        /* Turning it off drops the view exactly where the minimal-scroll
         * policy expects it, instead of leaving it mid-slide. */
        if (!a->cfg.dramatic)
        {
            a->target_x = a->view_x;
            a->scroll_x_vel = 0.0f;
            ensure_cursor_visible(a);
        }
    }
    else if (strcmp(cmd, "Zoom In") == 0) { if (a->size_px < 64u) { a->size_px += 2u; font_set_size(a->fs, a->size_px, a->scale); } }
    else if (strcmp(cmd, "Zoom Out") == 0) { if (a->size_px > 8u) { a->size_px -= 2u; font_set_size(a->fs, a->size_px, a->scale); } }
    else if (strcmp(cmd, "Find Files") == 0) open_picker_source(a, PSRC_FILES, 0u);
    else if (strcmp(cmd, "Buffers") == 0) open_picker_source(a, PSRC_BUFFERS, 0u);
    else if (strcmp(cmd, "Live Grep") == 0) open_picker_source(a, PSRC_GREP, 0u);
    else if (strcmp(cmd, "Live Grep Word Under Cursor") == 0) open_picker_grep_word(a);
    else if (strcmp(cmd, "Document Symbols") == 0) open_picker_source(a, PSRC_SYMBOLS, 0u);
    else if (strcmp(cmd, "Project Symbols") == 0) open_picker_source(a, PSRC_PROJECT_SYMBOLS, 0u);
    else if (strcmp(cmd, "Recent Files") == 0) open_picker_source(a, PSRC_RECENT, 0u);
    else if (strcmp(cmd, "Keymaps") == 0) open_picker_source(a, PSRC_KEYMAPS, 0u);
    else if (strcmp(cmd, "Help") == 0) open_picker_source(a, PSRC_HELP, 0u);
    else if (strncmp(cmd, "Cursor VFX: ", 12u) == 0)
    {
        a->cfg.vfx_modes = vfx_parse_modes(cmd + 12);
        vfx_reset(&a->vfx);
    }
    else if (strncmp(cmd, "Editor Opacity: ", 16u) == 0)
    {
        float op = (float)atoi(cmd + 16) / 100.0f;
        if (op < 0.0f) op = 0.0f;
        if (op > 1.0f) op = 1.0f;
        a->cfg.editor_opacity = op;
    }
    else if (strcmp(cmd, "Background Image: None") == 0)
    {
        a->cfg.background_image[0] = 0;
        if (a->vk) vk_render_set_background(a->vk, NULL, a->cfg.background_fit);
    }
    else if (strcmp(cmd, "Theme: Reload") == 0)
    {
        theme_load_file(a->cfg.theme);
    }
    else if (strcmp(cmd, "Theme: Default") == 0)
    {
        a->cfg.theme[0] = 0;
        theme_reset();
    }
    else if (strcmp(cmd, "Terminal: Open") == 0) term_open(a);
    else if (strcmp(cmd, "Terminal: Close") == 0) term_close(a);
    a->redraw = true;
}

static void accept_picker(App* a)
{
    const PickerItem* it = &a->picker->result;
    if (a->picker->source == PSRC_COMMANDS)
    {
        execute_command(a, it->label);
        return;
    }
    if (a->picker->source == PSRC_BUFFERS)
    {
        bm_switch(&a->bm, it->offset);
        app_set_doc(a, bm_current(&a->bm));
        return;
    }
    if (it->path[0])
    {
        char full[1024];
        Document* d;
        if (it->path[0] == '/') snprintf(full, sizeof full, "%s", it->path);
        else snprintf(full, sizeof full, "%s/%s", a->picker->root, it->path);
        d = bm_open(&a->bm, full);
        if (d)
        {
            app_set_doc(a, d);
            if (it->offset > 0u || it->line > 0u)
            {
                uint32_t off = it->offset;
                if (off == 0u && it->line > 0u)
                    off = mu_quill_line_offset(&d->q, it->line);
                ss_single(&a->ss, off);
                ss_sync_doc(&a->ss, d);
                ensure_cursor_visible(a);
            }
        }
    }
    else if (a->picker->source == PSRC_SYMBOLS && it->offset > 0u)
    {
        ss_single(&a->ss, it->offset);
        ss_sync_doc(&a->ss, a->doc);
        ensure_cursor_visible(a);
    }
    a->redraw = true;
}

static void handle_request(App* a)
{
    QRequest r = a->modal.request;
    uint32_t rows = visible_rows(a);
    const SelCursor* p = ss_primary_const(&a->ss);
    mu_quill_cursor c = mu_quill_offset_to_cursor(&a->doc->q, p->head);
    a->modal.request = QREQ_NONE;
    switch (r)
    {
    case QREQ_PICKER_FILES: open_picker_source(a, PSRC_FILES, 0u); break;
    case QREQ_PICKER_BUFFERS: open_picker_source(a, PSRC_BUFFERS, 0u); break;
    case QREQ_PICKER_GREP: open_picker_source(a, PSRC_GREP, 0u); break;
    case QREQ_PICKER_GREP_WORD: open_picker_grep_word(a); break;
    case QREQ_PICKER_SYMBOLS: open_picker_source(a, PSRC_SYMBOLS, 0u); break;
    case QREQ_PICKER_PROJECT_SYMBOLS: open_picker_source(a, PSRC_PROJECT_SYMBOLS, 0u); break;
    case QREQ_PICKER_RECENT: open_picker_source(a, PSRC_RECENT, 5u); break; /* top five recents */
    case QREQ_PICKER_COMMANDS: open_picker_source(a, PSRC_COMMANDS, 0u); break;
    case QREQ_PICKER_KEYMAPS: open_picker_source(a, PSRC_KEYMAPS, 0u); break;
    case QREQ_PICKER_HELP: open_picker_source(a, PSRC_HELP, 0u); break;
    case QREQ_SEARCH_FWD:
    case QREQ_SEARCH_REV:
        a->search_open = true;
        a->search_len = 0u;
        a->search_query[0] = 0;
        a->search_dir = r == QREQ_SEARCH_FWD ? 1 : -1;
        break;
    case QREQ_SAVE: doc_save_file(a->doc, NULL); break;
    case QREQ_QUIT: if (a->window) glfwSetWindowShouldClose(a->window, 1); break;
    case QREQ_TOGGLE_LINE_NUMBERS: a->params.line_numbers = !a->params.line_numbers; break;
    case QREQ_SCROLL_CENTER: set_target_top(a, (float)c.line - (float)rows * 0.5f); break;
    case QREQ_SCROLL_TOP: set_target_top(a, (float)c.line); break;
    case QREQ_SCROLL_BOTTOM: set_target_top(a, (float)c.line - (float)rows + 1.0f); break;
    case QREQ_CYCLE_VFX:
    {
        static const uint32_t cycle[] = { VFX_NONE, VFX_RAILGUN, VFX_TORPEDO, VFX_PIXIEDUST,
                                          VFX_SONICBOOM, VFX_RIPPLE, VFX_WIREFRAME,
                                          VFX_COMET, VFX_EMBER, VFX_AURORA, VFX_GLITCH, VFX_PULSE,
                                          VFX_SNOW, VFX_CONFETTI, VFX_SPARK, VFX_SMOKE,
                                          VFX_SHOCKWAVE, VFX_HALO };
        uint32_t i, n = sizeof cycle / sizeof cycle[0], next = VFX_NONE;
        for (i = 0u; i < n; ++i)
            if (a->cfg.vfx_modes == cycle[i]) { next = cycle[(i + 1u) % n]; break; }
        a->cfg.vfx_modes = next;
        vfx_reset(&a->vfx);
        break;
    }
    case QREQ_TERMINAL: term_open(a); break;
    default: break;
    }
    a->redraw = true;
}

static void search_key(App* a, int key, int mods)
{
    (void)mods;
    if (key == GLFW_KEY_ESCAPE)
    {
        a->search_open = false;
    }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
    {
        a->search_open = false;
        modal_search_set(&a->modal, a->search_query, a->search_dir);
        modal_search_next(&a->modal, a->doc, &a->ss, a->search_dir);
        ensure_cursor_visible(a);
    }
    else if (key == GLFW_KEY_BACKSPACE)
    {
        if (a->search_len > 0u)
        {
            a->search_len--;
            while (a->search_len > 0u && (a->search_query[a->search_len] & 0xC0) == 0x80)
                a->search_len--;
            a->search_query[a->search_len] = 0;
        }
    }
    a->redraw = true;
}

static void search_char(App* a, unsigned int cp)
{
    char utf8[4];
    uint32_t n = 0u;
    if (cp < 0x80u) utf8[n++] = (char)cp;
    else if (cp < 0x800u) { utf8[n++] = (char)(0xC0u | (cp >> 6)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else if (cp < 0x10000u) { utf8[n++] = (char)(0xE0u | (cp >> 12)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else { utf8[n++] = (char)(0xF0u | (cp >> 18)); utf8[n++] = (char)(0x80u | ((cp >> 12) & 63)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    if (a->search_len + n >= sizeof a->search_query) return;
    memcpy(a->search_query + a->search_len, utf8, n);
    a->search_len += n;
    a->search_query[a->search_len] = 0;
    a->redraw = true;
}

static uint32_t line_start_offset(App* a, uint32_t off)
{
    uint32_t i;
    if (off > a->doc->q.text_len) off = a->doc->q.text_len;
    for (i = off; i > 0u; --i)
    {
        char ch;
        mu_quill_text(&a->doc->q, i - 1u, 1u, &ch);
        if (ch == '\n') return i;
    }
    return 0u;
}

static uint32_t line_end_offset(App* a, uint32_t off)
{
    uint32_t i;
    if (off > a->doc->q.text_len) off = a->doc->q.text_len;
    for (i = off; i < a->doc->q.text_len; ++i)
    {
        char ch;
        mu_quill_text(&a->doc->q, i, 1u, &ch);
        if (ch == '\n') return i;
    }
    return a->doc->q.text_len;
}

static void set_ranges_for_clipboard(App* a, const uint32_t* starts, const uint32_t* ends)
{
    uint32_t i;
    for (i = 0u; i < a->ss.count; ++i)
    {
        a->ss.c[i].anchor = starts[i];
        a->ss.c[i].head = ends[i];
        a->ss.c[i].anim_valid = false;
    }
    ss_normalize(&a->ss);
}

/* ---- clipboard ---------------------------------------------------------- */

/* Copy `len` bytes of `src` (NUL-terminated) into the reusable clipboard
 * buffer, discarding whatever was there before. No malloc is involved.
 * Returns a NUL-terminated pointer into a->clipboard_buf, or NULL if `src`
 * is NULL. The buffer is reused on every call, so the previous content is
 * simply overwritten -- no allocator latency on copy or paste.
 *
 * `glfwSetClipboardString` copies the string internally (on Wayland it keeps
 * a local mirror via wl_data_source), so the staged buffer only needs to be
 * valid during this call. */
static const char* clipboard_stage(App* a, const char* src, uint32_t len)
{
    if (!src) return NULL;
    if (len >= QUILL_CLIPBOARD_CAP) len = QUILL_CLIPBOARD_CAP - 1u;
    memcpy(a->clipboard_buf, src, len);
    a->clipboard_buf[len] = '\0';
    return a->clipboard_buf;
}

/* Set the system clipboard from a NUL-terminated string. Guarded so we never
 * call GLFW with a NULL window or a NULL string. On Wayland this goes through
 * wl_data_device_set_selection, which requires a valid event serial; all
 * call sites are inside key/button callbacks so a serial is current. */
static bool clipboard_set(GLFWwindow* w, const char* text)
{
    if (!w || !text) return false;
    glfwSetClipboardString(w, text);
    return true;
}

/* Copy the system clipboard contents into the reusable buffer (no malloc).
 * Returns byte length, or -1 if unavailable/empty. Copies immediately to
 * avoid a use-after-free: glfwGetClipboardString() points at GLFW-internal
 * storage that is freed on the next clipboard-set call. */
static int clipboard_get(App* a, GLFWwindow* w)
{
    const char* s;
    uint32_t len;
    if (!w) return -1;
    s = glfwGetClipboardString(w);
    if (!s) return -1;
    len = (uint32_t)strlen(s);
    if (len == 0u) return -1;
    return clipboard_stage(a, s, len) ? (int)len : -1;
}

static bool copy_selection_to_clipboard(App* a, bool cut)
{
    uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
    uint32_t total = 0u, i, at = 0u;

    if (!a->window) return false;
    if (a->ss.count == 0u) return false;

    for (i = 0u; i < a->ss.count; ++i)
    {
        starts[i] = ss_min(&a->ss.c[i]);
        ends[i] = ss_max(&a->ss.c[i]);
        if (starts[i] == ends[i])
        {
            starts[i] = line_start_offset(a, a->ss.c[i].head);
            ends[i] = line_end_offset(a, a->ss.c[i].head);
            if (ends[i] < a->doc->q.text_len) ends[i]++;
        }
        if (i > 0u)
        {
            total += 1u;
        }
        total += ends[i] - starts[i];
    }

    if (total < QUILL_CLIPBOARD_CAP)
    {
        /* Fast path: stage into the reusable buffer, no heap at all. */
        for (i = 0u; i < a->ss.count; ++i)
        {
            uint32_t len = ends[i] - starts[i];
            if (i > 0u) a->clipboard_buf[at++] = '\n';
            mu_quill_text(&a->doc->q, starts[i], len, a->clipboard_buf + at);
            at += len;
        }
        a->clipboard_buf[at] = '\0';
        clipboard_set(a->window, a->clipboard_buf);
    }
    else
    {
        /* Rare path: a single copy larger than the reusable buffer.
         * Allocate once instead of silently truncating. */
        char* data = (char*)MU_MALLOC((size_t)total + 1u);
        if (!data) return false;
        for (i = 0u; i < a->ss.count; ++i)
        {
            uint32_t len = ends[i] - starts[i];
            if (i > 0u) data[at++] = '\n';
            mu_quill_text(&a->doc->q, starts[i], len, data + at);
            at += len;
        }
        data[at] = '\0';
        clipboard_set(a->window, data);
        MU_FREE(data);
    }
    if (cut)
    {
        set_ranges_for_clipboard(a, starts, ends);
        ss_replace_all(&a->ss, a->doc, "", 0u, app_now_ms());
        sync_after_edit(a);
    }
    a->redraw = true;
    return true;
}

static bool paste_clipboard(App* a)
{
    int len;
    if (!a->window) return false;
    len = clipboard_get(a, a->window);
    if (len < 0) return false;
    if (!ss_replace_all(&a->ss, a->doc, a->clipboard_buf, (uint32_t)len, app_now_ms()))
        return false;
    sync_after_edit(a);
    a->redraw = true;
    return true;
}

static bool term_copy_clipboard(App* a)
{
    char* text;
    if (!a->window || !a->term) return false;
    text = terminal_selection_text(a->term);
    if (!text) return false;
    clipboard_set(a->window, clipboard_stage(a, text, (uint32_t)strlen(text)));
    MU_FREE(text);
    a->redraw = true;
    return true;
}

static bool term_paste_clipboard(App* a)
{
    int len;
    if (!a->window || !a->term) return false;
    len = clipboard_get(a, a->window);
    if (len < 0) return false;
    terminal_paste(a->term, a->clipboard_buf, (size_t)len);
    a->redraw = true;
    return true;
}

static void app_on_key(App* a, int key, int scancode, int action, int mods)
{
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    bool alt = (mods & GLFW_MOD_ALT) != 0;
    bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    uint64_t now = app_now_ms();
    (void)scancode;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

    /* GLFW delivers the Unicode character after the key callback. This flag
     * belongs to that one key event, not to the terminal session: leaving it
     * set after a chord which produces no char would swallow a later key. */
    a->term_skip_char = false;

    if (a->term_active && a->term)
    {
        /* Standard terminal clipboard chords. Keep plain Ctrl-C as SIGINT and
         * plain Ctrl-V as the shell's quoted-insert command. */
        if ((ctrl && shift && key == GLFW_KEY_C) ||
            (ctrl && !shift && key == GLFW_KEY_INSERT))
        {
            term_copy_clipboard(a);
            a->term_skip_char = true;
            return;
        }
        if ((ctrl && shift && key == GLFW_KEY_V) ||
            (shift && !ctrl && !alt && key == GLFW_KEY_INSERT))
        {
            term_paste_clipboard(a);
            a->term_skip_char = true;
            return;
        }
        /* Ctrl+: is the terminal toggle. While the terminal is focused it
         * returns to the editor; the shell keeps running. Suppress the ':'
         * char callback as well, or it would immediately open the palette. */
        if (ctrl && key == GLFW_KEY_SEMICOLON)
        {
            a->term_active = false;
            a->term_esc_pending = false;
            a->term_skip_char = true;
            a->redraw = true;
            return;
        }
        /* <C-\><C-n> returns to the editor (Neovim convention). A lone C-\
         * followed by anything else is forwarded to the shell. */
        if (a->term_esc_pending)
        {
            a->term_esc_pending = false;
            if (ctrl && key == GLFW_KEY_N) { a->term_active = false; a->redraw = true; return; }
            terminal_send_key(a->term, GLFW_KEY_BACKSLASH, GLFW_MOD_CONTROL);
        }
        if (ctrl && key == GLFW_KEY_BACKSLASH) { a->term_esc_pending = true; return; }
        if (key == GLFW_KEY_PAGE_UP && (mods & GLFW_MOD_SHIFT))
        { terminal_scroll(a->term, (int)visible_rows(a)); a->redraw = true; return; }
        if (key == GLFW_KEY_PAGE_DOWN && (mods & GLFW_MOD_SHIFT))
        { terminal_scroll(a->term, -(int)visible_rows(a)); a->redraw = true; return; }
        /* Plain printable positions defer to the char callback (layout-aware).
         * Ctrl+Alt is AltGr on many layouts and must defer too: on Bépo, for
         * example, AltGr+X produces '{', not a physical Ctrl+Alt+X chord.
         * Plain Ctrl and Alt keep their normal terminal key encodings. */
        if (((key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT) || key == GLFW_KEY_UNKNOWN)
            && ((!ctrl && !alt) || (ctrl && alt)))
            return;
        terminal_send_key(a->term, key, mods);
        a->term_skip_char = true;
        return;
    }

    if (a->picker && a->picker->open)
    {
        bool was = a->picker->open;
        picker_key(a->picker, key, mods);
        if (was && !a->picker->open && a->picker->accepted)
            accept_picker(a);
        a->redraw = true;
        return;
    }
    if (a->search_open)
    {
        search_key(a, key, mods);
        return;
    }

    if (ctrl && !alt)
    {
        switch (key)
        {
        case GLFW_KEY_C: copy_selection_to_clipboard(a, false); return;
        case GLFW_KEY_X: copy_selection_to_clipboard(a, true); return;
        case GLFW_KEY_V: paste_clipboard(a); return;
        case GLFW_KEY_S: doc_save_file(a->doc, NULL); a->redraw = true; return;
        case GLFW_KEY_P: open_picker_source(a, PSRC_FILES, 0u); return;
        case GLFW_KEY_D: { int n = (int)(visible_rows(a) / 2u); modal_scroll(&a->modal, a->doc, &a->ss, n, visible_rows(a)); set_target_top(a, a->target_top + (float)n); a->redraw = true; return; }
        case GLFW_KEY_U: { int n = (int)(visible_rows(a) / 2u); modal_scroll(&a->modal, a->doc, &a->ss, -n, visible_rows(a)); set_target_top(a, a->target_top - (float)n); a->redraw = true; return; }
        case GLFW_KEY_F: { int n = (int)visible_rows(a); modal_scroll(&a->modal, a->doc, &a->ss, n, visible_rows(a)); set_target_top(a, a->target_top + (float)n); a->redraw = true; return; }
        case GLFW_KEY_B: { int n = (int)visible_rows(a); modal_scroll(&a->modal, a->doc, &a->ss, -n, visible_rows(a)); set_target_top(a, a->target_top - (float)n); a->redraw = true; return; }
        case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD: execute_command(a, "Zoom In"); return;
        case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT: execute_command(a, "Zoom Out"); return;
        case GLFW_KEY_SEMICOLON:
            term_open(a);
            a->term_skip_char = true; /* don't type ':' into the new terminal */
            return;
        default: break;
        }
    }
    if (key == GLFW_KEY_PAGE_DOWN)
    {
        { int n = (int)visible_rows(a); modal_scroll(&a->modal, a->doc, &a->ss, n, visible_rows(a)); set_target_top(a, a->target_top + (float)n); }
        a->redraw = true;
        return;
    }
    if (key == GLFW_KEY_PAGE_UP)
    {
        { int n = (int)visible_rows(a); modal_scroll(&a->modal, a->doc, &a->ss, -n, visible_rows(a)); set_target_top(a, a->target_top - (float)n); }
        a->redraw = true;
        return;
    }

    /* Layout-aware input: printable keys are handled by the char callback,
     * which receives the user's keyboard layout output (e.g. ':' on any
     * layer/position), not the QWERTY physical key. Defer every printable
     * position — and GLFW_KEY_UNKNOWN, which some layouts emit for keys with
     * no US counterpart — to the char callback. Keep physical Ctrl shortcuts
     * and Ctrl-Alt multicursor chords here. Non-char command keys (arrows,
     * F-keys, Enter, ...) are NOT deferred so they still reach modal_key. */
    if ((key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT) || key == GLFW_KEY_UNKNOWN)
    {
        bool ctrl_chord = ctrl && !alt;
        bool mc_chord = ctrl && alt && (key == GLFW_KEY_N || key == GLFW_KEY_P);
        if (!ctrl_chord && !mc_chord)
            return;
    }

    if (modal_key(&a->modal, a->doc, &a->ss, key, mods, now))
    {
        if (a->modal.request != QREQ_NONE) handle_request(a);
        sync_after_edit(a);
    }
}

static void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    app_on_key(a, key, scancode, action, mods);
    /* Feed the same key edge into the mu_input platform state so
     * platform_key_down()/platform_key_pressed() stay current. */
    platform_push_key(&a->platform, (uint32_t)(scancode >= 0 ? scancode : key),
                      action == GLFW_PRESS);
}

static void app_on_char(App* a, unsigned int cp)
{
    uint64_t now = app_now_ms();
    /* Check this before dispatching to the newly focused view. Ctrl+: can
     * switch focus between the key and char callbacks; its ':' must go to
     * neither the shell nor the editor command palette. */
    if (a->term_skip_char) { a->term_skip_char = false; return; }
    if (a->term_active && a->term)
    {
        if (cp < 0x20u) return; /* control chars are encoded by the key callback */
        terminal_send_char(a->term, cp);
        return;
    }
    if (a->picker && a->picker->open)
    {
        picker_char(a->picker, cp);
        a->redraw = true;
        return;
    }
    if (a->search_open)
    {
        search_char(a, cp);
        return;
    }
    if (modal_wants_char(&a->modal))
    {
        modal_char(&a->modal, a->doc, &a->ss, cp, now);
        sync_after_edit(a);
    }
    else if (a->modal.mode != QMODE_INSERT)
    {
        if (modal_printable(&a->modal, a->doc, &a->ss, cp, now))
        {
            if (a->modal.request != QREQ_NONE) handle_request(a);
            sync_after_edit(a);
        }
    }
}

static void char_callback(GLFWwindow* w, unsigned int cp)
{
    app_on_char((App*)glfwGetWindowUserPointer(w), cp);
}

static void term_cell_at_pixel(const App* a, double x, double y,
                               uint32_t* row, uint32_t* col)
{
    float advance = font_advance(a->fs);
    float lh = font_line_height(a->fs);
    uint32_t cols = terminal_cols(a->term);
    uint32_t rows = terminal_rows(a->term);
    if (advance <= 0.0f) advance = 1.0f;
    if (lh <= 0.0f) lh = 1.0f;
    *col = x <= 0.0 ? 0u : (uint32_t)(x / (double)advance);
    *row = y <= 0.0 ? 0u : (uint32_t)(y / (double)lh);
    if (*col >= cols) *col = cols - 1u;
    if (*row >= rows) *row = rows - 1u;
}

static void mouse_callback(GLFWwindow* w, int button, int action, int mods)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    double x, y;
    uint32_t off;
    bool alt = (mods & GLFW_MOD_ALT) != 0;
    bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    glfwGetCursorPos(w, &x, &y);
    /* Picker: click an item to select and close. */
    if (a->picker && a->picker->open)
    {
        int hit = picker_hit_test(a->picker, a->params.width, a->params.height,
                                  font_line_height(a->fs), (float)x, (float)y);
        if (action == GLFW_PRESS && hit >= 0)
        {
            bool was = a->picker->open;
            a->picker->selected = (uint32_t)hit;
            a->picker->result = a->picker->items[hit];
            a->picker->accepted = true;
            a->picker->open = false;
            if (was && !a->picker->open && a->picker->accepted)
                accept_picker(a);
            a->redraw = true;
        }
        return;
    }
    if (a->term_active && a->term)
    {
        uint32_t row, col;
        term_cell_at_pixel(a, x, y, &row, &col);
        if (action == GLFW_PRESS)
        {
            terminal_select_start(a->term, row, col);
            a->dragging = true;
        }
        else if (action == GLFW_RELEASE)
            a->dragging = false;
        a->redraw = true;
        return;
    }
    off = frame_hit_test(&a->params, a->layouts, a->nlayouts,
                         font_line_height(a->fs), gutter_width(a),
                         (float)x, (float)y + a->params.scroll_y_offset, a->doc);
    if (action == GLFW_PRESS)
    {
        if (alt)
            ss_add(&a->ss, off, off);
        else if (shift)
            ss_primary(&a->ss)->head = off;
        else
            ss_single(&a->ss, off);
        ss_sync_doc(&a->ss, a->doc);
        a->dragging = !alt;
        a->redraw = true;
    }
    else if (action == GLFW_RELEASE)
        a->dragging = false;
}

static void cursor_callback(GLFWwindow* w, double x, double y)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    uint32_t off;
    if (!a->dragging) return;
    if (a->term_active && a->term)
    {
        uint32_t row, col;
        term_cell_at_pixel(a, x, y, &row, &col);
        terminal_select_extend(a->term, row, col);
        a->redraw = true;
        return;
    }
    off = frame_hit_test(&a->params, a->layouts, a->nlayouts,
                         font_line_height(a->fs), gutter_width(a),
                         (float)x, (float)y + a->params.scroll_y_offset, a->doc);
    ss_primary(&a->ss)->head = off;
    ss_sync_doc(&a->ss, a->doc);
    a->redraw = true;
}

static void scroll_callback(GLFWwindow* w, double dx, double dy)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    int lines = (int)(-dy * 3.0);
    if (a->term_active && a->term)
    {
        if (lines != 0) terminal_scroll(a->term, -lines);
        a->redraw = true;
        return;
    }
    if (lines != 0)
        set_target_top(a, a->target_top + (float)lines);
    if (dx != 0.0)
    {
        a->target_x += (float)(dx * font_advance(a->fs));
        if (a->target_x < 0.0f) a->target_x = 0.0f;
    }
    clamp_scroll(a);
    a->redraw = true;
}

static void size_callback(GLFWwindow* w, int width, int height)
{
    App* a = (App*)glfwGetWindowUserPointer(w);
    if (width <= 0 || height <= 0) return;
    a->params.width = (uint32_t)width;
    a->params.height = (uint32_t)height;
    vk_render_resize(a->vk, (uint32_t)width, (uint32_t)height);
    if (a->term)
    {
        uint32_t cols, rows;
        term_grid_size(a, &cols, &rows);
        terminal_resize(a->term, cols, rows);
    }
    clamp_scroll(a);
    a->redraw = true;
}

static void render_search_prompt(App* a)
{
    float fg[4], bg[4], accent[4];
    float lh = font_line_height(a->fs);
    float y = (float)a->params.height - lh * 2.0f - 16.0f;
    char prompt[300];
    quill_color_linear(QUILL_THEME.status_bg, bg);
    quill_color_linear(QUILL_THEME.fg, fg);
    quill_color_linear(QUILL_THEME.accent, accent);
    bg[3] = 0.95f * a->cfg.editor_opacity;
    frame_rect_rounded(a->dl, 8.0f, y, (float)a->params.width - 16.0f, lh + 16.0f, bg, 8.0f);
    snprintf(prompt, sizeof prompt, "%s%s", a->search_dir > 0 ? "/" : "?", a->search_query);
    frame_text(a->fs, a->dl, prompt, (uint32_t)strlen(prompt), 16.0f, y + 8.0f + font_baseline(a->fs), accent, (float)a->params.width - 16.0f);
    (void)fg;
}

/* Step the view toward its targets. Vertical uses the configured spring;
 * horizontal only moves at all in dramatic mode, where it uses the same
 * spring so both axes settle together instead of one lagging the other. */
static void advance_view(App* a, float dt, bool* scroll_active)
{
    uint32_t ms = a->cfg.dramatic ? a->cfg.dramatic_ms : a->cfg.scroll_ms;
    bool snap = (ms == 0u || a->cfg.reduced_motion || a->cfg.style == MOTION_INSTANT);

    if (!a->cfg.dramatic)
    {
        a->view_x = a->target_x;
        a->scroll_x_vel = 0.0f;
    }
    if (snap)
    {
        a->view_top = a->target_top;
        a->scroll_vel = 0.0f;
        a->view_x = a->target_x;
        a->scroll_x_vel = 0.0f;
    }
    else
    {
        float far = (float)a->cfg.scroll_far_lines;
        float diff = a->target_top - a->view_top;
        if (far > 0.0f && quill_fabsf(diff) > far)
        {
            a->view_top = a->target_top - (diff > 0.0f ? far : -far);
            a->scroll_vel = 0.0f;
        }
        quill_spring_step(&a->view_top, &a->scroll_vel, a->target_top, dt, (float)ms);
        if (quill_fabsf(a->view_top - a->target_top) < 0.001f && quill_fabsf(a->scroll_vel) < 0.01f)
        {
            a->view_top = a->target_top;
            a->scroll_vel = 0.0f;
        }
        else
            *scroll_active = true;

        if (a->cfg.dramatic)
        {
            quill_spring_step(&a->view_x, &a->scroll_x_vel, a->target_x, dt, (float)ms);
            if (quill_fabsf(a->view_x - a->target_x) < 0.01f && quill_fabsf(a->scroll_x_vel) < 0.1f)
            {
                a->view_x = a->target_x;
                a->scroll_x_vel = 0.0f;
            }
            else
                *scroll_active = true;
        }
    }
    if (a->view_top < 0.0f) a->view_top = 0.0f;
    if (a->view_x < 0.0f) a->view_x = 0.0f;
    /* The renderer and hit-testing read params.scroll_x, so it always carries
     * the rendered offset rather than the target. Outside dramatic mode the
     * two are the same value, making this a plain copy. */
    a->params.scroll_x = a->view_x;
}

static void render_app(App* a)
{
    uint64_t now = app_now_ms();
    uint64_t now_ns = mu_time_now();
    float lh = font_line_height(a->fs);
    /* Nanosecond frame delta.  The old millisecond clock quantised every
     * frame to 1 ms (16 or 17 ms at 60 Hz), which is enough to make particle
     * speeds visibly uneven at high refresh rates. */
    float dt = a->last_frame_ns ? (float)((double)(now_ns - a->last_frame_ns) * 1e-9) : 0.016f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    bool scroll_active = false;
    if (a->term_active && a->term)
    {
        float status_h = lh + 16.0f;
        float th = (float)a->params.height > status_h ? (float)a->params.height - status_h : 0.0f;
        a->dl->nsolids = 0u;
        a->dl->nglyphs = 0u;
        a->dl->nvfx = 0u;
        a->dl->nov_solids = 0u;
        a->dl->nov_glyphs = 0u;
        a->dl->in_overlay = false;
        a->dl->overflow = 0u;
        a->animating = false;
        a->blink_active = false;
        terminal_render(a->term, a->fs, a->dl, 0.0f, 0.0f, (float)a->params.width, th);
        render_term_status(a);
        if (a->picker && a->picker->open)
            picker_render(a->picker, a->fs, a->dl, a->params.width, a->params.height, a->cfg.editor_opacity);
        vk_render_frame(a->vk, a->dl, a->fs);
        a->last_frame_ns = now_ns;
        return;
    }
    clamp_scroll(a);
    a->last_frame_ns = now_ns;
    advance_view(a, dt, &scroll_active);
    a->params.top_line = (uint32_t)a->view_top;
    a->params.scroll_y_offset = (a->view_top - (float)a->params.top_line) * lh;
    a->params.selections = &a->ss;
    a->params.modal = &a->modal;
    a->params.search_query = a->modal.search[0] ? a->modal.search : NULL;
    a->params.now_ms = now;
    a->params.dt = dt;
    a->params.cursor_ms = a->cfg.reduced_motion ? 0u : a->cfg.cursor_ms;
    a->params.motion = &a->cfg;
    a->params.cursor_anim = a->cursor_anim;
    a->params.vfx = &a->vfx;
    a->params.animating = &a->animating;
    a->animating = false;
    a->blink_active = false;
    if (a->cfg.blink_ms > 0u && !a->cfg.reduced_motion)
    {
        a->blink_active = true;
        if (a->cfg.smooth_blink)
        {
            float period = (float)a->cfg.blink_ms * 2.0f;
            float phase = (float)(now % (uint64_t)period) / period;
            a->params.blink_alpha = 0.5f + 0.5f * quill_cosf(6.28318530718f * phase);
        }
        else
        {
            a->params.blink_alpha = ((now / a->cfg.blink_ms) & 1u) ? 0.0f : 1.0f;
        }
    }
    else
        a->params.blink_alpha = 1.0f;

    frame_build(a->doc, a->fs, &a->params, a->dl, a->scratch, a->layouts, &a->nlayouts);
    /* The renderer batches by primitive type, so an overlay appended to the
     * same list would have its panel drawn under the editor glyphs. Start a new
     * layer for anything that must appear above the editor. */
    if ((a->picker && a->picker->open) || a->search_open)
    {
        frame_overlay_begin(a->dl);
        if (a->picker && a->picker->open)
            picker_render(a->picker, a->fs, a->dl, a->params.width, a->params.height, a->cfg.editor_opacity);
        if (a->search_open)
            render_search_prompt(a);
        frame_overlay_end(a->dl);
    }
    vk_render_frame(a->vk, a->dl, a->fs);
    a->animating = a->animating || scroll_active;
}

static bool is_directory(const char* path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool app_init_doc(App* a, const char* file)
{
    bm_init(&a->bm);
    if (file && is_directory(file))
    {
        /* `quill <folder>`: start on an empty buffer and show the file picker
         * rooted at that folder. */
        snprintf(a->root, sizeof a->root, "%s", file);
        a->open_files_on_start = true;
        a->doc = bm_open(&a->bm, NULL);
    }
    else if (file)
        a->doc = bm_open(&a->bm, file);
    else if (a->cfg.session_restore && session_load(a->cfg.session_file, &a->bm) > 0u)
        a->doc = bm_current(&a->bm);
    else
    {
        a->doc = bm_open(&a->bm, NULL);
        if (a->doc)
        {
            doc_set_text(a->doc, SAMPLE_C, (uint32_t)strlen(SAMPLE_C));
            doc_attach_syntax(a->doc, syntax_create_for_path("sample.c"));
            if (a->doc->syntax) syntax_parse(a->doc->syntax, &a->doc->q);
            snprintf(a->doc->path, sizeof a->doc->path, "sample.c");
        }
    }
    if (!a->doc) return false;
    ss_init(&a->ss);
    ss_from_doc(&a->ss, a->doc);
    /* Restore the active buffer's saved scroll (clamped on first render). */
    a->view_top = a->target_top = a->doc->view_top;
    a->target_x = a->view_x = a->params.scroll_x = a->doc->scroll_x;
    modal_init(&a->modal);
    a->modal.window = a->window;
    a->modal.auto_save_on_mode_change = a->cfg.auto_save_on_mode_change;
    return true;
}

static int run_headless(int argc, char** argv)
{
    App a;
    const char* cfgpath = NULL;
    const char* file = NULL;
    const char* shot = "quill.png";
    const char* vfx = NULL;
    const char* bg = NULL;
    const char* bg_fit = NULL;
    const char* opacity = NULL;
    const char* theme = NULL;
    uint32_t width = 1280u, height = 800u, size = 16u, scale = 1u, top = 0u;
    bool validation = false;
    bool vfx_demo = false;
    bool dramatic = false, dramatic_set = false;
    int i;
    VkRenderCreateInfo ci;

    memset(&a, 0, sizeof a);
    vfx_init(&a.vfx);
    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) cfgpath = argv[++i];
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) size = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--top-line") == 0 && i + 1 < argc) top = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validation") == 0) validation = true;
        else if (strcmp(argv[i], "--dramatic") == 0) { dramatic = true; dramatic_set = true; }
        else if (strcmp(argv[i], "--no-dramatic") == 0) { dramatic = false; dramatic_set = true; }
        else if (strcmp(argv[i], "--vfx") == 0 && i + 1 < argc) vfx = argv[++i];
        else if (strcmp(argv[i], "--vfx-demo") == 0) vfx_demo = true;
        else if (strcmp(argv[i], "--background") == 0 && i + 1 < argc) bg = argv[++i];
        else if (strcmp(argv[i], "--background-fit") == 0 && i + 1 < argc) bg_fit = argv[++i];
        else if (strcmp(argv[i], "--editor-opacity") == 0 && i + 1 < argc) opacity = argv[++i];
        else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) theme = argv[++i];
        else if (argv[i][0] != '-') file = argv[i];
    }
    /* Headless used to ignore --config entirely, so a config-driven background
     * or theme silently did nothing in screenshots. Load it, then let the CLI
     * flags below override individual keys. */
    config_load_file(&a.cfg, cfgpath ? cfgpath : "quill.conf");
    a.cfg.session_restore = false; /* headless screenshots are deterministic */
    if (vfx) a.cfg.vfx_modes = vfx_parse_modes(vfx);
    /* Screenshots of the camera lead are only reproducible with the mode
     * explicitly set; otherwise the config file decides. */
    if (dramatic_set) a.cfg.dramatic = dramatic;
    if (vfx_demo && a.cfg.vfx_modes == VFX_NONE) a.cfg.vfx_modes = VFX_RAILGUN | VFX_RIPPLE;
    /* CLI paths get the same ~ expansion as config paths; relative ones are
     * taken from the current directory, which is what a shell would do. */
    if (bg)
    {
        if (!config_resolve_path(bg, NULL, a.cfg.background_image,
                                 sizeof a.cfg.background_image))
        {
            a.cfg.background_image[0] = 0;
            fprintf(stderr, "background: cannot find '%s'\n", bg);
        }
    }
    if (bg_fit)
    {
        if (strcmp(bg_fit, "contain") == 0) a.cfg.background_fit = BG_FIT_CONTAIN;
        else if (strcmp(bg_fit, "stretch") == 0) a.cfg.background_fit = BG_FIT_STRETCH;
        else a.cfg.background_fit = BG_FIT_COVER;
    }
    if (opacity)
    {
        float op = (float)strtod(opacity, NULL);
        if (op < 0.0f) op = 0.0f;
        if (op > 1.0f) op = 1.0f;
        a.cfg.editor_opacity = op;
    }
    if (theme)
    {
        char tpath[512];
        if (config_resolve_path(theme, NULL, tpath, sizeof tpath))
            theme_load_file(tpath);
        else
            fprintf(stderr, "theme: cannot find '%s'\n", theme);
    }
    else theme_load_file(NULL);
    /* Same rule as the config path: a background with a fully opaque editor is
     * invisible, so reveal it unless --editor-opacity was given. */
    if (a.cfg.background_image[0] && !opacity && a.cfg.editor_opacity >= 1.0f)
        a.cfg.editor_opacity = 0.85f;
    if (!app_init_doc(&a, file)) return 1;
    a.fs = font_create(size, scale);
    if (!a.fs) { bm_destroy(&a.bm); return 1; }
    a.dl = (DrawList*)MU_CALLOC(1u, sizeof *a.dl);
    a.scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *a.scratch);
    memset(&ci, 0, sizeof ci);
    ci.headless = true;
    ci.width = width;
    ci.height = height;
    ci.validation = validation;
    ci.window_opacity = a.cfg.window_opacity;
    a.vk = vk_render_create(&ci);
    if (!a.vk || !a.dl || !a.scratch)
    {
        fprintf(stderr, "headless Vulkan init failed\n");
        vk_render_destroy(a.vk);
        font_destroy(a.fs);
        bm_destroy(&a.bm);
        return 1;
    }
    if (a.cfg.background_image[0])
        vk_render_set_background(a.vk, a.cfg.background_image, a.cfg.background_fit);
    vk_render_set_background_style(a.vk, a.cfg.background_dim, a.cfg.background_vignette);
    a.params.width = width;
    a.params.height = height;
    a.params.tab_width = QUILL_TAB_WIDTH;
    a.params.line_numbers = true;
    a.view_top = a.target_top = (float)top;
    if (a.doc->syntax) syntax_parse(a.doc->syntax, &a.doc->q);
    render_app(&a);
    if (vfx_demo && a.doc->q.text_len > 0u)
    {
        uint32_t f;
        for (f = 0u; f < 48u; ++f)
        {
            uint32_t off = (f * 97u) % a.doc->q.text_len;
            struct timespec ts = { 0, 16000000 };
            ss_single(&a.ss, off);
            ss_sync_doc(&a.ss, a.doc);
            ensure_cursor_visible(&a);
            nanosleep(&ts, NULL);
            render_app(&a);
        }
    }
    if (!vk_render_screenshot(a.vk, shot))
        fprintf(stderr, "screenshot failed\n");
    else
        printf("wrote %s (%ux%u, device: %s)\n", shot, width, height, vk_render_device_name(a.vk));
    vk_render_destroy(a.vk);
    MU_FREE(a.dl);
    MU_FREE(a.scratch);
    font_destroy(a.fs);
    bm_destroy(&a.bm);
    return 0;
}

int main(int argc, char** argv)
{
    App a;
    const char* file = NULL;
    const char* cfgpath = NULL;
    const char* vfx_cli = NULL;
    const char* bg_cli = NULL;
    const char* bg_fit_cli = NULL;
    const char* opacity_cli = NULL;
    const char* theme_cli = NULL;
    bool no_session = false;
    int i;
    float xscale = 1.0f, yscale = 1.0f;
    VkRenderCreateInfo ci;
    int fbw, fbh;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--headless") == 0)
            return run_headless(argc, argv);
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            cfgpath = argv[++i];
        if (strcmp(argv[i], "--vfx") == 0 && i + 1 < argc)
            vfx_cli = argv[++i];
        if (strcmp(argv[i], "--background") == 0 && i + 1 < argc)
            bg_cli = argv[++i];
        if (strcmp(argv[i], "--background-fit") == 0 && i + 1 < argc)
            bg_fit_cli = argv[++i];
        if (strcmp(argv[i], "--editor-opacity") == 0 && i + 1 < argc)
            opacity_cli = argv[++i];
        if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
            theme_cli = argv[++i];
        if (strcmp(argv[i], "--no-session") == 0)
            no_session = true;
        if (strcmp(argv[i], "--help") == 0)
        {
            printf("quill [file|directory] [--config path] [--theme file] [--no-session]\n"
                   "      [--background img] [--background-fit cover|contain|stretch] [--editor-opacity 0..1]\n"
                   "quill --headless [file] --screenshot out.png [--width W --height H] [--size px] [--scale n] [--theme file] [--validation]\n");
            return 0;
        }
        if (argv[i][0] != '-') file = argv[i];
    }

    memset(&a, 0, sizeof a);
    vfx_init(&a.vfx);
    config_load_file(&a.cfg, cfgpath ? cfgpath : "quill.conf");
    if (vfx_cli) a.cfg.vfx_modes = vfx_parse_modes(vfx_cli);
    if (bg_cli &&
        !config_resolve_path(bg_cli, NULL, a.cfg.background_image,
                             sizeof a.cfg.background_image))
    {
        a.cfg.background_image[0] = 0;
        fprintf(stderr, "background: cannot find '%s'\n", bg_cli);
    }
    if (bg_fit_cli)
    {
        if (strcmp(bg_fit_cli, "contain") == 0) a.cfg.background_fit = BG_FIT_CONTAIN;
        else if (strcmp(bg_fit_cli, "stretch") == 0) a.cfg.background_fit = BG_FIT_STRETCH;
        else a.cfg.background_fit = BG_FIT_COVER;
    }
    if (opacity_cli)
    {
        float op = (float)strtod(opacity_cli, NULL);
        if (op < 0.0f) op = 0.0f;
        if (op > 1.0f) op = 1.0f;
        a.cfg.editor_opacity = op;
    }
    if (theme_cli &&
        !config_resolve_path(theme_cli, NULL, a.cfg.theme, sizeof a.cfg.theme))
    {
        a.cfg.theme[0] = 0;
        fprintf(stderr, "theme: cannot find '%s'\n", theme_cli);
    }
    if (no_session) a.cfg.session_restore = false;
    theme_load_file(a.cfg.theme);
#if defined(GLFW_WAYLAND_LIBDECOR) && defined(GLFW_WAYLAND_DISABLE_LIBDECOR)
    /* libdecor-gtk pulls in GTK/GLib/ICU (~20 MiB RSS) just to draw window
     * decorations.  Always use the compositor/GLFW fallback instead. */
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
#endif
    if (!glfwInit()) {
        /* With a platform-specific GLFW (e.g. the vendored Wayland-only build)
         * a missing display server is a common, silent failure.  Say what is
         * missing instead of exiting 1 with no output. */
        const char* desc = NULL;
        int code = glfwGetError(&desc);
        if (desc && code) fprintf(stderr, "glfw: init failed: %s (0x%X)\n", desc, code);
        else if (desc)     fprintf(stderr, "glfw: init failed: %s\n", desc);
        else               fprintf(stderr, "glfw: init failed (no diagnostic)\n");
        fprintf(stderr, "glfw: no usable display server. This build was compiled "
                        "for: %s\n", glfwGetVersionString());
        fprintf(stderr, "glfw: WAYLAND_DISPLAY=%s DISPLAY=%s\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    /* Without this hint GLFW marks the Wayland surface opaque and the
     * compositor ignores the alpha channel, so window_opacity would do
     * nothing. Opaque windows keep the default (cheaper) surface. */
    if (a.cfg.window_opacity < 0.999f)
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    a.window = glfwCreateWindow(1280, 800, "Quill", NULL, NULL);
    if (!a.window) {
        const char* desc = NULL;
        glfwGetError(&desc);
        fprintf(stderr, "glfw: cannot create window: %s\n",
                desc ? desc : "unknown error");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowUserPointer(a.window, &a);
    if (!platform_init(&a.platform, a.window))
    {
        fprintf(stderr, "platform: cannot initialise input backend");
        glfwDestroyWindow(a.window);
        glfwTerminate();
        return 1;
    }
    glfwGetWindowContentScale(a.window, &xscale, &yscale);
    a.scale = (uint32_t)(xscale > yscale ? xscale : yscale);
    if (a.scale < 1u) a.scale = 1u;
    a.size_px = 16u;
    if (a.cfg.background_image[0] && !opacity_cli && a.cfg.editor_opacity >= 1.0f)
        a.cfg.editor_opacity = 0.85f;
    if (!app_init_doc(&a, file)) return 1;
    a.fs = font_create(a.size_px, a.scale);
    a.dl = (DrawList*)MU_CALLOC(1u, sizeof *a.dl);
    a.scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *a.scratch);
    glfwGetFramebufferSize(a.window, &fbw, &fbh);
    memset(&ci, 0, sizeof ci);
    ci.window = a.window;
    ci.width = (uint32_t)fbw;
    ci.height = (uint32_t)fbh;
    ci.validation = getenv("QUILL_VK_VALIDATION") != NULL;
    ci.window_opacity = a.cfg.window_opacity;
    a.vk = vk_render_create(&ci);
    if (!a.fs || !a.dl || !a.scratch || !a.vk)
    {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    if (a.cfg.background_image[0])
        vk_render_set_background(a.vk, a.cfg.background_image, a.cfg.background_fit);
    vk_render_set_background_style(a.vk, a.cfg.background_dim, a.cfg.background_vignette);
    a.params.width = (uint32_t)fbw;
    a.params.height = (uint32_t)fbh;
    a.params.tab_width = QUILL_TAB_WIDTH;
    a.params.line_numbers = true;
    a.redraw = true;

    glfwSetKeyCallback(a.window, key_callback);
    glfwSetCharCallback(a.window, char_callback);
    glfwSetMouseButtonCallback(a.window, mouse_callback);
    glfwSetCursorPosCallback(a.window, cursor_callback);
    glfwSetScrollCallback(a.window, scroll_callback);
    glfwSetFramebufferSizeCallback(a.window, size_callback);

    /* Started on a folder: show the file picker for that folder immediately. */
    if (a.open_files_on_start)
        open_picker_source(&a, PSRC_FILES, 0u);

    while (!glfwWindowShouldClose(a.window))
    {
        /* Wake interval while motion is in flight. This was 0.004s (250Hz),
         * four times faster than any display refreshes: the extra wakeups cost
         * a full frame_build + Vulkan submit each and bought nothing visible.
         * 0.008s matches a 120Hz display and halves the render count for a
         * scroll or cursor spring without changing how it looks. */
        double timeout = 0.1;
        if (a.term_active && a.term && !terminal_exited(a.term)) timeout = 0.008;
        else if (a.animating) timeout = 0.008;
        else if (a.picker && a.picker->open) timeout = 0.03;
        /* Cursor blink. A stepped blink only changes state twice per period, so
         * wake just past the next edge instead of polling at 33Hz. A smooth
         * blink does need frames, but the whole draw list is rebuilt and
         * re-submitted each time purely to change one quad's alpha, so it is
         * paced to 20Hz: the fade is still continuous to the eye and the idle
         * cost drops by a third. Step blink was 60% of idle CPU on its own. */
        else if (a.blink_active)
            timeout = a.cfg.smooth_blink ? 0.05
                                         : (double)(a.cfg.blink_ms % 1000u) / 1000.0;
        glfwWaitEventsTimeout(timeout);
        platform_poll(&a.platform);
        if (a.term_active && a.term && terminal_poll(a.term)) a.redraw = true;
        if (a.picker) picker_poll(a.picker);
        if (a.parse_due && app_now_ms() >= a.parse_due)
        {
            if (a.doc->syntax) syntax_parse(a.doc->syntax, &a.doc->q);
            a.parse_due = 0u;
            a.redraw = true;
        }
        if (a.redraw)
        {
            render_app(&a);
            a.redraw = a.animating || a.blink_active;
        }
    }

    if (a.cfg.session_restore)
    {
        save_current_view(&a);
        session_save(a.cfg.session_file, &a.bm);
    }

    if (a.term) terminal_destroy(a.term);

    if (a.picker)
    {
        picker_destroy(a.picker);
        MU_FREE(a.picker);
    }
    vk_render_destroy(a.vk);
    MU_FREE(a.dl);
    MU_FREE(a.scratch);
    font_destroy(a.fs);
    bm_destroy(&a.bm);
    platform_destroy(&a.platform);
    glfwDestroyWindow(a.window);
    glfwTerminate();
    return 0;
}
