#include "terminal.h"
#include "theme.h"

#include <vterm.h>
#include <vterm_keycodes.h>
#include <GLFW/glfw3.h>

#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <math.h>

#define TERM_MAX_COLS    512u
#define TERM_MAX_ROWS    256u
#define TERM_SCROLLBACK  2000u
#define TERM_READ_CHUNK  65536u
#define TERM_READ_BUDGET 16u   /* max chunks drained per poll, keeps UI responsive */

/* Per-cell attributes we actually render. */
#define TERM_ATTR_BOLD      1u
#define TERM_ATTR_UNDERLINE 2u
#define TERM_ATTR_REVERSE   4u
#define TERM_ATTR_STRIKE    8u

/* One screen cell. AoS: the renderer reads every field per cell, so keeping
 * them together is cache-friendly. Colors are 0xRRGGBB (alpha added at draw). */
typedef struct TermCell
{
    uint32_t cp;      /* codepoint; 0 or UINT32_MAX = blank */
    uint32_t fg, bg;
    uint8_t  attrs;
    uint8_t  width;   /* 1, 2 (wide), or 0 (continuation of a wide char) */
    uint16_t pad;
} TermCell;

struct Terminal
{
    VTerm*       vt;
    VTermScreen* screen;
    VTermState*  state;
    int          master_fd;
    pid_t        pid;
    uint32_t     cols, rows;

    TermCell*    grid;        /* rows*cols, refreshed from libvterm on damage */
    TermCell*    scrollback;  /* TERM_SCROLLBACK*cols ring */
    uint32_t     sb_head, sb_count;
    uint32_t     scroll_offset; /* 0 = live; >0 = lines scrolled into history */

    int          cursor_row, cursor_col;
    bool         cursor_visible;
    bool         damaged;
    bool         exited;
    bool         title_changed;
    char         title[256];
    uint32_t     title_len;
    uint32_t     default_fg, default_bg;
};

/* ---- color helpers ------------------------------------------------------ */

static uint32_t rgb_of(uint32_t rgba) { return rgba >> 8; } /* 0xRRGGBBAA -> 0xRRGGBB */
/* Grid colors are 0xRRGGBB; quill_color_linear wants 0xRRGGBBAA (alpha low). */
#define TERM_RGBA(rgb) ((((uint32_t)(rgb)) << 8) | 0xFFu)

static uint32_t vcolor_to_rgb(Terminal* t, VTermColor c)
{
    vterm_screen_convert_color_to_rgb(t->screen, &c);
    return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | (uint32_t)c.rgb.blue;
}

static void convert_cell(Terminal* t, const VTermScreenCell* sc, TermCell* dc)
{
    uint32_t fg, bg;
    dc->cp = sc->chars[0];
    dc->width = (uint8_t)sc->width;
    fg = vcolor_to_rgb(t, sc->fg);
    bg = vcolor_to_rgb(t, sc->bg);
    dc->attrs = 0u;
    if (sc->attrs.bold)      dc->attrs |= TERM_ATTR_BOLD;
    if (sc->attrs.underline) dc->attrs |= TERM_ATTR_UNDERLINE;
    if (sc->attrs.strike)    dc->attrs |= TERM_ATTR_STRIKE;
    if (sc->attrs.reverse)
    {
        uint32_t tmp = fg; fg = bg; bg = tmp;
        dc->attrs |= TERM_ATTR_REVERSE;
    }
    dc->fg = fg;
    dc->bg = bg;
    dc->pad = 0u;
}

/* ---- libvterm callbacks ------------------------------------------------- */

static int cb_damage(VTermRect rect, void* user)
{
    Terminal* t = (Terminal*)user;
    (void)rect;
    t->damaged = true;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void* user)
{
    Terminal* t = (Terminal*)user;
    (void)oldpos;
    t->cursor_row = pos.row;
    t->cursor_col = pos.col;
    t->cursor_visible = visible != 0;
    t->damaged = true;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue* val, void* user)
{
    Terminal* t = (Terminal*)user;
    if (prop == VTERM_PROP_TITLE)
    {
        if (val->string.initial) t->title_len = 0u;
        if (t->title_len + val->string.len < sizeof t->title)
        {
            memcpy(t->title + t->title_len, val->string.str, val->string.len);
            t->title_len += (uint32_t)val->string.len;
        }
        t->title[t->title_len] = 0;
        t->title_changed = true;
    }
    else if (prop == VTERM_PROP_CURSORVISIBLE)
    {
        t->cursor_visible = val->boolean != 0;
        t->damaged = true;
    }
    return 1;
}

static int cb_bell(void* user) { (void)user; return 1; }

static int cb_resize(int rows, int cols, void* user)
{
    (void)rows; (void)cols; (void)user;
    return 1; /* grid is (re)allocated by terminal_resize, not here */
}

static int cb_sb_pushline(int cols, const VTermScreenCell* cells, void* user)
{
    Terminal* t = (Terminal*)user;
    TermCell* dst;
    int c;
    if ((uint32_t)cols != t->cols) return 0;
    dst = t->scrollback + (size_t)t->sb_head * t->cols;
    for (c = 0; c < cols; ++c) convert_cell(t, &cells[c], &dst[c]);
    t->sb_head = (t->sb_head + 1u) % TERM_SCROLLBACK;
    if (t->sb_count < TERM_SCROLLBACK) t->sb_count++;
    if (t->scroll_offset > 0u && t->scroll_offset < t->sb_count) t->scroll_offset++;
    return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell* cells, void* user)
{
    Terminal* t = (Terminal*)user;
    int c;
    if (t->sb_count == 0u || (uint32_t)cols != t->cols) return 0;
    t->sb_head = (t->sb_head + TERM_SCROLLBACK - 1u) % TERM_SCROLLBACK;
    t->sb_count--;
    {
        const TermCell* src = t->scrollback + (size_t)t->sb_head * t->cols;
        for (c = 0; c < cols; ++c)
        {
            memset(&cells[c], 0, sizeof cells[c]);
            cells[c].chars[0] = src[c].cp;
            cells[c].width = (char)src[c].width;
            vterm_color_rgb(&cells[c].fg, (uint8_t)(src[c].fg >> 16), (uint8_t)(src[c].fg >> 8), (uint8_t)src[c].fg);
            vterm_color_rgb(&cells[c].bg, (uint8_t)(src[c].bg >> 16), (uint8_t)(src[c].bg >> 8), (uint8_t)src[c].bg);
        }
    }
    if (t->scroll_offset > t->sb_count) t->scroll_offset = t->sb_count;
    return 1;
}

static int cb_sb_clear(void* user)
{
    Terminal* t = (Terminal*)user;
    t->sb_head = 0u; t->sb_count = 0u; t->scroll_offset = 0u;
    return 1;
}

static const VTermScreenCallbacks SCREEN_CBS = {
    .damage      = cb_damage,
    .moverect    = NULL,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell        = cb_bell,
    .resize      = cb_resize,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = cb_sb_popline,
    .sb_clear    = cb_sb_clear,
};

/* libvterm -> PTY (keyboard echo, device responses). */
static void cb_output(const char* bytes, size_t len, void* user)
{
    Terminal* t = (Terminal*)user;
    size_t off = 0u;
    while (off < len)
    {
        ssize_t n = write(t->master_fd, bytes + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;
    }
}

/* ---- palette / theme ---------------------------------------------------- */

static const uint32_t ANSI_16[16] = {
    0x000000u, 0xCD0000u, 0x00CD00u, 0xCDCD00u, 0x0000EEu, 0xCD00CDu, 0x00CDCDu, 0xE5E5E5u,
    0x7F7F7Fu, 0xFF0000u, 0x00FF00u, 0xFFFF00u, 0x5C5CFFu, 0xFF00FFu, 0x00FFFFu, 0xFFFFFFu,
};

static void apply_theme_colors(Terminal* t)
{
    VTermColor fg, bg;
    int i;
    t->default_fg = rgb_of(QUILL_THEME.fg);
    t->default_bg = rgb_of(QUILL_THEME.bg);
    vterm_color_rgb(&fg, (uint8_t)(t->default_fg >> 16), (uint8_t)(t->default_fg >> 8), (uint8_t)t->default_fg);
    vterm_color_rgb(&bg, (uint8_t)(t->default_bg >> 16), (uint8_t)(t->default_bg >> 8), (uint8_t)t->default_bg);
    vterm_screen_set_default_colors(t->screen, &fg, &bg);
    for (i = 0; i < 16; ++i)
    {
        VTermColor c;
        vterm_color_rgb(&c, (uint8_t)(ANSI_16[i] >> 16), (uint8_t)(ANSI_16[i] >> 8), (uint8_t)ANSI_16[i]);
        vterm_state_set_palette_color(t->state, i, &c);
    }
    vterm_state_set_bold_highbright(t->state, 1);
}

/* ---- grid --------------------------------------------------------------- */

static bool alloc_grid(Terminal* t)
{
    MU_FREE(t->grid);
    MU_FREE(t->scrollback);
    t->grid = (TermCell*)MU_CALLOC((size_t)t->rows * t->cols, sizeof *t->grid);
    t->scrollback = (TermCell*)MU_CALLOC((size_t)TERM_SCROLLBACK * t->cols, sizeof *t->scrollback);
    t->sb_head = 0u; t->sb_count = 0u; t->scroll_offset = 0u;
    return t->grid && t->scrollback;
}

static void refresh_grid(Terminal* t)
{
    uint32_t r, c;
    VTermPos cp;
    for (r = 0u; r < t->rows; ++r)
    {
        for (c = 0u; c < t->cols; ++c)
        {
            VTermScreenCell sc;
            VTermPos pos = { (int)r, (int)c };
            vterm_screen_get_cell(t->screen, pos, &sc);
            convert_cell(t, &sc, &t->grid[(size_t)r * t->cols + c]);
        }
    }
    vterm_state_get_cursorpos(t->state, &cp);
    t->cursor_row = cp.row;
    t->cursor_col = cp.col;
}

/* ---- lifecycle ---------------------------------------------------------- */

Terminal* terminal_create(uint32_t cols, uint32_t rows)
{
    Terminal* t;
    struct winsize ws;
    const char* shell;
    if (cols == 0u || rows == 0u) return NULL;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    t = (Terminal*)MU_CALLOC(1u, sizeof *t);
    if (!t) return NULL;
    t->cols = cols; t->rows = rows;
    t->master_fd = -1;
    t->cursor_visible = true;

    t->vt = vterm_new((int)rows, (int)cols);
    if (!t->vt) { MU_FREE(t); return NULL; }
    vterm_set_utf8(t->vt, 1);
    t->screen = vterm_obtain_screen(t->vt);
    t->state = vterm_obtain_state(t->vt);
    vterm_screen_set_callbacks(t->screen, &SCREEN_CBS, t);
    vterm_output_set_callback(t->vt, cb_output, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_set_damage_merge(t->screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(t->screen, 1);
    apply_theme_colors(t);
    if (!alloc_grid(t)) { terminal_destroy(t); return NULL; }

    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    t->pid = forkpty(&t->master_fd, NULL, NULL, &ws);
    if (t->pid < 0) { terminal_destroy(t); return NULL; }
    if (t->pid == 0)
    {
        /* child */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        shell = getenv("SHELL");
        if (!shell || !shell[0]) shell = "/bin/sh";
        execl(shell, shell, (char*)NULL);
        _exit(127);
    }
    /* parent */
    fcntl(t->master_fd, F_SETFL, O_NONBLOCK);
    refresh_grid(t);
    t->damaged = false;
    return t;
}

void terminal_destroy(Terminal* t)
{
    if (!t) return;
    if (t->pid > 0)
    {
        kill(t->pid, SIGHUP);
        waitpid(t->pid, NULL, 0);
    }
    if (t->master_fd >= 0) close(t->master_fd);
    if (t->vt) vterm_free(t->vt);
    MU_FREE(t->grid);
    MU_FREE(t->scrollback);
    MU_FREE(t);
}

void terminal_resize(Terminal* t, uint32_t cols, uint32_t rows)
{
    struct winsize ws;
    if (!t || cols == 0u || rows == 0u) return;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols == t->cols && rows == t->rows) return;
    t->cols = cols; t->rows = rows;
    vterm_set_size(t->vt, (int)rows, (int)cols);
    alloc_grid(t);
    if (t->master_fd >= 0)
    {
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ws.ws_xpixel = 0; ws.ws_ypixel = 0;
        ioctl(t->master_fd, TIOCSWINSZ, &ws);
    }
    refresh_grid(t);
    t->damaged = false;
}

/* ---- pump --------------------------------------------------------------- */

bool terminal_poll(Terminal* t)
{
    char buf[TERM_READ_CHUNK];
    bool changed = false;
    uint32_t budget = TERM_READ_BUDGET;
    if (!t) return false;

    while (budget-- > 0u)
    {
        ssize_t n = read(t->master_fd, buf, sizeof buf);
        if (n > 0)
        {
            vterm_input_write(t->vt, buf, (size_t)n);
            changed = true;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else break; /* EOF / EIO: child gone */
    }

    if (!t->exited && t->pid > 0)
    {
        int status;
        pid_t r = waitpid(t->pid, &status, WNOHANG);
        if (r == t->pid)
        {
            t->exited = true;
            t->pid = -1;
            changed = true;
        }
    }

    if (t->damaged)
    {
        refresh_grid(t);
        t->damaged = false;
        changed = true;
    }
    return changed;
}

/* ---- input -------------------------------------------------------------- */

void terminal_send_char(Terminal* t, uint32_t codepoint)
{
    if (!t || t->exited) return;
    vterm_keyboard_unichar(t->vt, codepoint, VTERM_MOD_NONE);
}

static VTermKey map_key(int k)
{
    switch (k)
    {
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: return VTERM_KEY_ENTER;
    case GLFW_KEY_TAB:         return VTERM_KEY_TAB;
    case GLFW_KEY_BACKSPACE:   return VTERM_KEY_BACKSPACE;
    case GLFW_KEY_ESCAPE:      return VTERM_KEY_ESCAPE;
    case GLFW_KEY_UP:          return VTERM_KEY_UP;
    case GLFW_KEY_DOWN:        return VTERM_KEY_DOWN;
    case GLFW_KEY_LEFT:        return VTERM_KEY_LEFT;
    case GLFW_KEY_RIGHT:       return VTERM_KEY_RIGHT;
    case GLFW_KEY_INSERT:      return VTERM_KEY_INS;
    case GLFW_KEY_DELETE:      return VTERM_KEY_DEL;
    case GLFW_KEY_HOME:        return VTERM_KEY_HOME;
    case GLFW_KEY_END:         return VTERM_KEY_END;
    case GLFW_KEY_PAGE_UP:     return VTERM_KEY_PAGEUP;
    case GLFW_KEY_PAGE_DOWN:   return VTERM_KEY_PAGEDOWN;
    default: break;
    }
    if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F25) return (VTermKey)VTERM_KEY_FUNCTION(k - GLFW_KEY_F1 + 1);
    return VTERM_KEY_NONE;
}

void terminal_send_key(Terminal* t, int glfw_key, int glfw_mods)
{
    VTermModifier mod = VTERM_MOD_NONE;
    VTermKey key;
    if (!t || t->exited) return;
    if (glfw_mods & GLFW_MOD_SHIFT)   mod |= VTERM_MOD_SHIFT;
    if (glfw_mods & GLFW_MOD_CONTROL) mod |= VTERM_MOD_CTRL;
    if (glfw_mods & GLFW_MOD_ALT)     mod |= VTERM_MOD_ALT;
    key = map_key(glfw_key);
    if (key != VTERM_KEY_NONE)
    {
        vterm_keyboard_key(t->vt, key, mod);
        return;
    }
    /* Ctrl/Alt + a printable position: encode the base character. Letters go
     * lowercase so Ctrl chords map to the right control codes on any layout. */
    {
        uint32_t c = 0u;
        if (glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z) c = (uint32_t)(glfw_key + 32);
        else if (glfw_key >= GLFW_KEY_SPACE && glfw_key <= GLFW_KEY_GRAVE_ACCENT) c = (uint32_t)glfw_key;
        if (c) vterm_keyboard_unichar(t->vt, c, mod);
    }
}

void terminal_scroll(Terminal* t, int lines)
{
    if (!t) return;
    if (lines > 0)
    {
        t->scroll_offset += (uint32_t)lines;
        if (t->scroll_offset > t->sb_count) t->scroll_offset = t->sb_count;
    }
    else if (lines < 0)
    {
        uint32_t n = (uint32_t)(-lines);
        t->scroll_offset = n > t->scroll_offset ? 0u : t->scroll_offset - n;
    }
}

/* ---- render ------------------------------------------------------------- */

static void term_emit_glyph(DrawList* dl, float x, float y, const AtlasEntry* e,
                            const float c[4], uint32_t aw, uint32_t ah)
{
    GlyphQuad* q;
    if (!e || e->w == 0u || e->h == 0u) return;
    if (dl->nglyphs >= QUILL_MAX_GLYPHS) { dl->overflow++; return; }
    q = &dl->glyphs[dl->nglyphs++];
    q->x = x; q->y = y; q->w = (float)e->w; q->h = (float)e->h;
    q->u0 = (float)e->x / (float)aw;
    q->v0 = (float)e->y / (float)ah;
    q->u1 = (float)(e->x + e->w) / (float)aw;
    q->v1 = (float)(e->y + e->h) / (float)ah;
    q->r = c[0]; q->g = c[1]; q->b = c[2]; q->a = c[3];
}

static uint32_t encode_utf8(uint32_t cp, uint8_t out[4])
{
    if (cp < 0x80u) { out[0] = (uint8_t)cp; return 1u; }
    if (cp < 0x800u)
    {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u)
    {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4u;
}

/* The scrollback/live line shown at viewport row i. */
static const TermCell* viewport_line(const Terminal* t, uint32_t i)
{
    int idx = (int)i - (int)t->scroll_offset;
    if (idx >= 0) return t->grid + (size_t)idx * t->cols;
    {
        int si = ((int)t->sb_head + idx) % (int)TERM_SCROLLBACK;
        if (si < 0) si += (int)TERM_SCROLLBACK;
        return t->scrollback + (size_t)si * t->cols;
    }
}

/* Shape one cell's codepoint and emit it at a fixed grid position. Per-cell
 * shaping (not per-row) so font ligatures never merge adjacent cells. */
static void draw_cell_glyph(Terminal* t, FontSystem* fs, DrawList* dl,
                            const TermCell* cell, float cx, float ry,
                            float baseline, uint32_t aw, uint32_t ah,
                            const float fg[4])
{
    uint8_t utf8[4];
    uint32_t len, j, n;
    const ShapedGlyph* g;
    (void)t;
    if (cell->width == 0u || cell->cp == 0u || cell->cp == UINT32_MAX || cell->cp < 0x20u) return;
    len = encode_utf8(cell->cp, utf8);
    n = font_shape_line(fs, utf8, len, 1u, &g);
    for (j = 0u; j < n; ++j)
    {
        const AtlasEntry* e = font_atlas_get(fs, g[j].face, g[j].glyph, 0u);
        float gx, gy;
        if (!e || (e->w <= 1u && e->h <= 1u)) continue;
        gx = quill_floorf(cx + g[j].x_offset) + (float)e->left;
        gy = ry + baseline - (float)e->top - g[j].y_offset;
        term_emit_glyph(dl, gx, gy, e, fg, aw, ah);
    }
}

void terminal_render(Terminal* t, FontSystem* fs, DrawList* dl,
                     float x, float y, float w, float h)
{
    float advance, lh, baseline, bg[4];
    uint32_t aw, ah, i, c;
    if (!t || !fs || !dl) return;
    advance = font_advance(fs);
    lh = font_line_height(fs);
    baseline = font_baseline(fs);
    aw = font_atlas_width(fs);
    ah = font_atlas_height(fs);

    quill_color_linear(TERM_RGBA(t->default_bg), bg);
    frame_rect(dl, x, y, w, h, bg);

    for (i = 0u; i < t->rows; ++i)
    {
        const TermCell* line = viewport_line(t, i);
        float ry = y + (float)i * lh;
        if (ry >= y + h) break;
        for (c = 0u; c < t->cols; ++c)
        {
            const TermCell* cell = &line[c];
            float cx = x + (float)c * advance;
            float cw = advance * (cell->width == 2u ? 2.0f : 1.0f);
            float fg[4];
            if (cx >= x + w) break;
            if (cell->bg != t->default_bg)
            {
                float cb[4];
                quill_color_linear(TERM_RGBA(cell->bg), cb);
                frame_rect(dl, cx, ry, cw, lh, cb);
            }
            quill_color_linear(TERM_RGBA(cell->fg), fg);
            draw_cell_glyph(t, fs, dl, cell, cx, ry, baseline, aw, ah, fg);
            if (cell->attrs & TERM_ATTR_UNDERLINE)
                frame_rect(dl, cx, ry + lh - 2.0f, cw, 1.0f, fg);
            if (cell->attrs & TERM_ATTR_STRIKE)
                frame_rect(dl, cx, ry + lh * 0.5f, cw, 1.0f, fg);
        }
    }

    /* Block cursor (live view only): fill the cell, then redraw its glyph in
     * the background color so the character stays readable. */
    if (t->scroll_offset == 0u && t->cursor_visible && !t->exited &&
        t->cursor_row >= 0 && t->cursor_row < (int)t->rows &&
        t->cursor_col >= 0 && t->cursor_col < (int)t->cols)
    {
        const TermCell* cell = &t->grid[(size_t)t->cursor_row * t->cols + (uint32_t)t->cursor_col];
        float cx = x + (float)t->cursor_col * advance;
        float ry = y + (float)t->cursor_row * lh;
        float cc[4], inv[4];
        quill_color_linear(QUILL_THEME.cursor, cc);
        frame_rect(dl, cx, ry, advance, lh, cc);
        quill_color_linear(TERM_RGBA(t->default_bg), inv);
        draw_cell_glyph(t, fs, dl, cell, cx, ry, baseline, aw, ah, inv);
    }
}

/* ---- queries ------------------------------------------------------------ */

bool terminal_exited(const Terminal* t) { return !t || t->exited; }
const char* terminal_title(const Terminal* t) { return t && t->title[0] ? t->title : "terminal"; }
uint32_t terminal_cols(const Terminal* t) { return t ? t->cols : 0u; }
uint32_t terminal_rows(const Terminal* t) { return t ? t->rows : 0u; }
bool terminal_scrolled(const Terminal* t) { return t && t->scroll_offset > 0u; }

uint32_t terminal_cell_cp(const Terminal* t, uint32_t row, uint32_t col)
{
    if (!t || row >= t->rows || col >= t->cols) return 0u;
    return t->grid[(size_t)row * t->cols + col].cp;
}

uint32_t terminal_cell_fg(const Terminal* t, uint32_t row, uint32_t col)
{
    if (!t || row >= t->rows || col >= t->cols) return 0u;
    return t->grid[(size_t)row * t->cols + col].fg;
}

uint32_t terminal_view_cp(const Terminal* t, uint32_t row, uint32_t col)
{
    if (!t || row >= t->rows || col >= t->cols) return 0u;
    return viewport_line(t, row)[col].cp;
}
