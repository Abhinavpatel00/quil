#include "picker.h"
#include "syntax.h"
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* src_name(PickerSource s)
{
    switch (s)
    {
    case PSRC_FILES: return "Files";
    case PSRC_BUFFERS: return "Buffers";
    case PSRC_GREP: return "Live Grep";
    case PSRC_SYMBOLS: return "Symbols";
    case PSRC_PROJECT_SYMBOLS: return "Project Symbols";
    case PSRC_RECENT: return "Recent";
    case PSRC_COMMANDS: return "Commands";
    case PSRC_KEYMAPS: return "Keymaps";
    case PSRC_HELP: return "Help";
    default: return "Picker";
    }
}

static int lower_ch(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool ci_contains(const char* hay, const char* needle)
{
    size_t n = strlen(needle), i, j;
    if (n == 0u) return true;
    for (i = 0u; hay[i]; ++i)
    {
        for (j = 0u; j < n && hay[i + j]; ++j)
            if (lower_ch((unsigned char)hay[i + j]) != lower_ch((unsigned char)needle[j]))
                break;
        if (j == n) return true;
    }
    return false;
}

int picker_fuzzy_score(const char* query, const char* text)
{
    int score = 0, streak = 0;
    uint32_t qi = 0, ti = 0, qn = (uint32_t)strlen(query);
    if (qn == 0u) return 0;
    while (text[ti] && qi < qn)
    {
        int a = lower_ch((unsigned char)query[qi]);
        int b = lower_ch((unsigned char)text[ti]);
        if (a == b)
        {
            streak++;
            score += 4 + streak * 2;
            if (ti == 0u || text[ti - 1u] == '/' || text[ti - 1u] == '_' || text[ti - 1u] == '-')
                score += 8;
            if (query[qi] == text[ti]) score += 1;
            qi++;
        }
        else
            streak = 0;
        ti++;
    }
    if (qi < qn) return -1;
    score -= (int)ti / 8;
    return score;
}

static int item_cmp(const void* a, const void* b)
{
    const PickerItem* x = (const PickerItem*)a;
    const PickerItem* y = (const PickerItem*)b;
    if (x->score != y->score) return y->score - x->score;
    return strcmp(x->label, y->label);
}

static void add_item(PickerItem* items, uint32_t* n, uint32_t cap,
                     const char* label, const char* path, uint32_t line,
                     uint32_t offset, uint8_t kind, int score)
{
    PickerItem* it;
    if (*n >= cap) return;
    it = &items[(*n)++];
    memset(it, 0, sizeof *it);
    { size_t l = strlen(label); if (l >= sizeof it->label) l = sizeof it->label - 1u; memcpy(it->label, label, l); it->label[l] = 0; }
    { const char* pp = path ? path : ""; size_t l = strlen(pp); if (l >= sizeof it->path) l = sizeof it->path - 1u; memcpy(it->path, pp, l); it->path[l] = 0; }
    it->line = line;
    it->offset = offset;
    it->kind = kind;
    it->score = score;
}

static bool skip_dir(const char* name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
           strcmp(name, ".git") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "node_modules") == 0 || strcmp(name, "third_party") == 0 ||
           strcmp(name, ".cache") == 0 || strcmp(name, "target") == 0;
}

static bool text_ext(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return true;
    return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 || strcmp(dot, ".rs") == 0 ||
           strcmp(dot, ".py") == 0 || strcmp(dot, ".js") == 0 || strcmp(dot, ".json") == 0 ||
           strcmp(dot, ".md") == 0 || strcmp(dot, ".txt") == 0 || strcmp(dot, ".toml") == 0 ||
           strcmp(dot, ".yaml") == 0 || strcmp(dot, ".yml") == 0 || strcmp(dot, ".sh") == 0 ||
           strcmp(dot, ".css") == 0 || strcmp(dot, ".html") == 0 || strcmp(dot, ".glsl") == 0 ||
           strcmp(dot, ".vert") == 0 || strcmp(dot, ".frag") == 0;
}

typedef struct WalkCtx
{
    Picker*  p;
    uint32_t gen;
    char     root[512];
    char     query[256];
} WalkCtx;

static bool walk_cancelled(Picker* p, uint32_t gen)
{
    bool dead;
    pthread_mutex_lock(&p->mu);
    dead = (p->generation != gen);
    pthread_mutex_unlock(&p->mu);
    return dead;
}

static void walk_files(WalkCtx* ctx, const char* rel, uint32_t depth,
                       PickerItem* items, uint32_t* n, uint32_t cap,
                       int mode)
{
    char dirpath[1024];
    DIR* dir;
    struct dirent* ent;
    if (depth > 12u || *n >= cap) return;
    if (rel[0]) snprintf(dirpath, sizeof dirpath, "%s/%s", ctx->root, rel);
    else snprintf(dirpath, sizeof dirpath, "%s", ctx->root);
    dir = opendir(dirpath);
    if (!dir) return;
    while ((ent = readdir(dir)) != NULL)
    {
        char child[1024], full[1536];
        struct stat st;
        if (skip_dir(ent->d_name)) continue;
        if (rel[0]) snprintf(child, sizeof child, "%s/%s", rel, ent->d_name);
        else snprintf(child, sizeof child, "%s", ent->d_name);
        snprintf(full, sizeof full, "%s/%s", ctx->root, child);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
        {
            walk_files(ctx, child, depth + 1u, items, n, cap, mode);
        }
        else if (S_ISREG(st.st_mode) && text_ext(ent->d_name))
        {
            if (mode == 0)
            {
                int score = picker_fuzzy_score(ctx->query, child);
                if (score >= 0)
                    add_item(items, n, cap, child, child, 0u, 0u, 0u, score);
            }
            else if (mode == 1)
            {
                FILE* f = fopen(full, "r");
                char line[1024];
                uint32_t ln = 0u;
                if (!f) continue;
                while (fgets(line, sizeof line, f) && *n < cap)
                {
                    ln++;
                    if (ctx->query[0] == 0 || ci_contains(line, ctx->query))
                    {
                        char label[768];
                        line[strcspn(line, "\r\n")] = 0;
                        snprintf(label, sizeof label, "%.200s:%u: %.400s", child, ln, line);
                        add_item(items, n, cap, label, child, ln - 1u, 0u, 0u, 0);
                    }
                    if ((ln & 1023u) == 0u && walk_cancelled(ctx->p, ctx->gen))
                    { fclose(f); closedir(dir); return; }
                }
                fclose(f);
            }
            else if (mode == 2)
            {
                Document tmp;
                if (doc_init(&tmp, 64u << 10) && doc_load_file(&tmp, full))
                {
                    QuillSymbol syms[256];
                    uint32_t ns = syntax_symbols(tmp.syntax, &tmp.q, syms, 256u), i;
                    for (i = 0u; i < ns && *n < cap; ++i)
                    {
                        char label[768];
                        snprintf(label, sizeof label, "%.200s:%u: %.120s", child, syms[i].line + 1u, syms[i].name);
                        add_item(items, n, cap, label, child, syms[i].line, syms[i].offset, syms[i].kind,
                                 picker_fuzzy_score(ctx->query, syms[i].name));
                    }
                }
                doc_destroy(&tmp);
            }
        }
        if ((*n & 63u) == 0u && walk_cancelled(ctx->p, ctx->gen))
        { closedir(dir); return; }
        if (*n >= cap) break;
    }
    closedir(dir);
}

static void* picker_worker(void* arg)
{
    Picker* p = (Picker*)arg;
    for (;;)
    {
        PickerSource src;
        uint32_t gen;
        WalkCtx ctx;
        pthread_mutex_lock(&p->mu);
        while (!p->job_pending && p->thread_run)
            pthread_cond_wait(&p->cv, &p->mu);
        if (!p->thread_run)
        {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        p->job_pending = false;
        src = p->job_source;
        gen = p->job_gen;
        snprintf(ctx.query, sizeof ctx.query, "%s", p->job_query);
        pthread_mutex_unlock(&p->mu);

        ctx.p = p;
        ctx.gen = gen;
        snprintf(ctx.root, sizeof ctx.root, "%s", p->root);
        p->job_count = 0u;
        if (src == PSRC_FILES)
            walk_files(&ctx, "", 0u, p->job_items, &p->job_count, QUILL_PICKER_RESULTS, 0);
        else if (src == PSRC_GREP)
            walk_files(&ctx, "", 0u, p->job_items, &p->job_count, QUILL_PICKER_RESULTS, 1);
        else if (src == PSRC_PROJECT_SYMBOLS)
            walk_files(&ctx, "", 0u, p->job_items, &p->job_count, QUILL_PICKER_RESULTS, 2);
        if (src == PSRC_FILES)
            qsort(p->job_items, p->job_count, sizeof p->job_items[0], item_cmp);

        pthread_mutex_lock(&p->mu);
        if (p->generation == gen)
            p->job_done = true;
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

static void start_job(Picker* p, PickerSource src)
{
    pthread_mutex_lock(&p->mu);
    p->generation++;
    p->job_gen = p->generation;
    p->job_source = src;
    snprintf(p->job_query, sizeof p->job_query, "%s", p->query);
    p->job_done = false;
    p->job_pending = true;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void filter_static(Picker* p)
{
    uint32_t i;
    p->count = 0u;
    for (i = 0u; i < p->base_count; ++i)
    {
        int score = picker_fuzzy_score(p->query, p->base[i].label);
        if (score >= 0)
        {
            p->items[p->count] = p->base[i];
            p->items[p->count].score = score;
            p->count++;
        }
    }
    if (p->count > 1u)
        qsort(p->items, p->count, sizeof p->items[0], item_cmp);
    if (p->selected >= p->count) p->selected = p->count ? p->count - 1u : 0u;
}

static void build_preview(Picker* p)
{
    const PickerItem* it;
    FILE* f;
    char line[512];
    uint32_t ln = 0u, len = 0u, start, end;
    p->preview[0] = 0;
    if (p->count == 0u) return;
    it = &p->items[p->selected];
    if (it->path[0] == 0) return;
    f = fopen(it->path, "r");
    if (!f) return;
    start = it->line > 10u ? it->line - 10u : 0u;
    end = start + 40u;
    while (fgets(line, sizeof line, f) && ln < end)
    {
        if (ln >= start)
        {
            uint32_t l = (uint32_t)strlen(line);
            if (len + l + 1u >= sizeof p->preview) break;
            memcpy(p->preview + len, line, l);
            len += l;
            p->preview[len++] = '\n';
        }
        ln++;
    }
    p->preview[len] = 0;
    fclose(f);
}

void picker_init(Picker* p, Document* doc, const char* root)
{
    memset(p, 0, sizeof *p);
    p->doc = doc;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    if (root && root[0]) snprintf(p->root, sizeof p->root, "%s", root);
    else if (!getcwd(p->root, sizeof p->root)) snprintf(p->root, sizeof p->root, ".");
    p->thread_run = true;
    if (pthread_create(&p->thread, NULL, picker_worker, p) == 0)
        p->thread_started = true;
}

void picker_destroy(Picker* p)
{
    if (p->thread_started)
    {
        pthread_mutex_lock(&p->mu);
        p->thread_run = false;
        pthread_cond_signal(&p->cv);
        pthread_mutex_unlock(&p->mu);
        pthread_join(p->thread, NULL);
    }
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
}

void picker_set_doc(Picker* p, Document* doc) { p->doc = doc; }

void picker_open_items(Picker* p, PickerSource src, const PickerItem* items, uint32_t n)
{
    uint32_t i;
    p->open = true;
    p->accepted = false;
    p->source = src;
    p->query[0] = 0;
    p->query_len = 0u;
    p->selected = 0u;
    p->base_count = n < QUILL_PICKER_RESULTS ? n : QUILL_PICKER_RESULTS;
    for (i = 0u; i < p->base_count; ++i) p->base[i] = items[i];
    filter_static(p);
    build_preview(p);
}

void picker_open(Picker* p, PickerSource src)
{
    p->open = true;
    p->accepted = false;
    p->source = src;
    p->query[0] = 0;
    p->query_len = 0u;
    p->selected = 0u;
    p->count = 0u;
    p->base_count = 0u;
    p->preview[0] = 0;
    if (src == PSRC_FILES || src == PSRC_GREP || src == PSRC_PROJECT_SYMBOLS)
    {
        start_job(p, src);
        return;
    }
    if (src == PSRC_SYMBOLS && p->doc)
    {
        QuillSymbol syms[QUILL_PICKER_RESULTS];
        uint32_t n, i;
        if (p->doc->syntax) syntax_parse(p->doc->syntax, &p->doc->q);
        n = syntax_symbols(p->doc->syntax, &p->doc->q, syms, QUILL_PICKER_RESULTS);
        for (i = 0u; i < n; ++i)
            add_item(p->base, &p->base_count, QUILL_PICKER_RESULTS, syms[i].name,
                     p->doc->path, syms[i].line, syms[i].offset, syms[i].kind, 0);
        filter_static(p);
        build_preview(p);
        return;
    }
    if (src == PSRC_COMMANDS)
    {
        static const char* cmds[] = {
            "Save", "Quit", "Toggle Line Numbers", "Dramatic Mode", "Zoom In", "Zoom Out",
            "Find Files", "Buffers", "Live Grep", "Live Grep Word Under Cursor",
            "Document Symbols",
            "Project Symbols", "Recent Files", "Keymaps", "Help",
            "Cursor VFX: none", "Cursor VFX: railgun", "Cursor VFX: torpedo",
            "Cursor VFX: pixiedust", "Cursor VFX: sonicboom", "Cursor VFX: ripple",
            "Cursor VFX: wireframe", "Cursor VFX: comet", "Cursor VFX: ember",
            "Cursor VFX: aurora", "Cursor VFX: glitch", "Cursor VFX: pulse",
            "Cursor VFX: snow", "Cursor VFX: confetti", "Cursor VFX: spark",
            "Cursor VFX: smoke", "Cursor VFX: shockwave", "Cursor VFX: halo",
            "Editor Opacity: 100%", "Editor Opacity: 90%", "Editor Opacity: 80%",
            "Editor Opacity: 70%", "Editor Opacity: 60%", "Editor Opacity: 50%",
            "Background Image: None",
            "Theme: Reload", "Theme: Default",
            "Terminal: Open", "Terminal: Close"
        };
        uint32_t i;
        for (i = 0u; i < sizeof cmds / sizeof cmds[0]; ++i)
            add_item(p->base, &p->base_count, QUILL_PICKER_RESULTS, cmds[i], "", 0u, i, 0u, 0);
    }
    else if (src == PSRC_KEYMAPS)
    {
        static const char* keys[] = {
            "Space leader", "Space <Space>  recent (top 5)", "Space f  files",
            "Space s w  grep word under cursor", "Space c  commands",
            "Space k  keymaps", "Space h  help",
            "Space t  terminal", "Space w  save", "Space q  quit",
            "Ctrl-n  add next cursor", "Ctrl-Alt-p  remove cursor", "Alt-click  add cursor",
            "i a o I A O  insert", "v V Ctrl-v  visual", "d c y  operators",
            "p P  put", "u Ctrl-r  undo/redo", ".  repeat", "/ ? n N  search",
            "zz zt zb  scroll", "gc  comment", ">> <<  indent", "Space m v  cycle cursor VFX",
            "Space t  terminal", "Ctrl-:  open/close terminal", "Ctrl-\\ Ctrl-n  leave terminal", "Shift-PgUp/PgDn  terminal scroll"
        };
        uint32_t i;
        for (i = 0u; i < sizeof keys / sizeof keys[0]; ++i)
            add_item(p->base, &p->base_count, QUILL_PICKER_RESULTS, keys[i], "", 0u, 0u, 0u, 0);
    }
    else if (src == PSRC_HELP)
    {
        static const char* help[] = {
            "Quill modal editor", "Normal mode is the default.", "Press i to insert, Esc to return.",
            "Operators combine with motions: dw, d$, dd, yw, yy.", "Text objects: iw, aw, i\", a(, ip.",
            "Registers: \"ayy yanks into a, \"ap puts.", "Multicursor: Ctrl-n adds next occurrence.",
            "Picker: Space f files, Space s w grep word under cursor.",
            "Motion is configurable in quill.conf."
        };
        uint32_t i;
        for (i = 0u; i < sizeof help / sizeof help[0]; ++i)
            add_item(p->base, &p->base_count, QUILL_PICKER_RESULTS, help[i], "", 0u, 0u, 0u, 0);
    }
    filter_static(p);
    build_preview(p);
}

/* Opens a query-driven source with the prompt already filled in. Used by
 * Space s w to grep the word under the cursor: the picker comes up on real
 * hits instead of an empty prompt, and editing the query narrows it further
 * through the usual query_changed() path. */
void picker_open_query(Picker* p, PickerSource src, const char* query)
{
    size_t len;
    picker_open(p, src);
    if (!query || !query[0]) return;
    len = strlen(query);
    if (len >= sizeof p->query) len = sizeof p->query - 1u;
    memcpy(p->query, query, len);
    p->query[len] = 0;
    p->query_len = (uint32_t)len;
    /* picker_open() already launched a job for the empty query. Bumping the
     * generation through start_job() cancels that one and re-walks with the
     * seed, so no stale "match everything" result can land later. */
    if (src == PSRC_FILES || src == PSRC_GREP || src == PSRC_PROJECT_SYMBOLS)
        start_job(p, src);
    else
    {
        filter_static(p);
        build_preview(p);
    }
}

void picker_close(Picker* p)
{
    p->open = false;
    pthread_mutex_lock(&p->mu);
    p->generation++;
    p->job_pending = false;
    pthread_mutex_unlock(&p->mu);
}

/* Returns the index of the item at screen coordinates (x, y), or -1 if outside
 * the list. Used by mouse clicks to select and close the picker. */
int picker_hit_test(const Picker* p, uint32_t width, uint32_t height,
                    float font_line_height, float x, float y)
{
    float w, h, px, py, rowh, pad, list_w;
    uint32_t maxrows, start, end, i;
    if (!p->open || p->count == 0u) return -1;
    w = (float)width * 0.86f;
    h = (float)height * 0.82f;
    if (w > 1100.0f) w = 1100.0f;
    if (h > 720.0f) h = 720.0f;
    px = ((float)width - w) * 0.5f;
    py = ((float)height - h) * 0.5f;
    pad = 10.0f;
    rowh = font_line_height + 4.0f;
    list_w = w * 0.55f;
    if (x < px || x > px + list_w || y < py || y > py + h) return -1;
    maxrows = (uint32_t)((h - 2.0f * pad - font_line_height - 10.0f) / rowh);
    if (maxrows == 0u) maxrows = 1u;
    start = p->selected > maxrows / 2u ? p->selected - maxrows / 2u : 0u;
    end = start + maxrows;
    if (end > p->count) end = p->count;
    for (i = start; i < end; ++i)
    {
        float ry = py + pad + font_line_height + 10.0f + (float)(i - start) * rowh;
        if (y >= ry && y < ry + rowh) return (int)i;
    }
    return -1;
}

void picker_poll(Picker* p)
{
    pthread_mutex_lock(&p->mu);
    if (p->job_done && p->job_gen == p->generation)
    {
        memcpy(p->items, p->job_items, p->job_count * sizeof p->items[0]);
        p->count = p->job_count;
        p->job_done = false;
        if (p->selected >= p->count) p->selected = p->count ? p->count - 1u : 0u;
        pthread_mutex_unlock(&p->mu);
        build_preview(p);
        return;
    }
    pthread_mutex_unlock(&p->mu);
}

const PickerItem* picker_selected(const Picker* p)
{
    if (!p->open || p->count == 0u) return NULL;
    return &p->items[p->selected < p->count ? p->selected : 0u];
}

static void query_changed(Picker* p)
{
    p->selected = 0u;
    if (p->source == PSRC_FILES || p->source == PSRC_GREP || p->source == PSRC_PROJECT_SYMBOLS)
        start_job(p, p->source);
    else
    {
        filter_static(p);
        build_preview(p);
    }
}

void picker_char(Picker* p, unsigned int cp)
{
    char utf8[4];
    uint32_t n = 0u;
    if (!p->open) return;
    if (cp < 0x80u) utf8[n++] = (char)cp;
    else if (cp < 0x800u) { utf8[n++] = (char)(0xC0u | (cp >> 6)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else if (cp < 0x10000u) { utf8[n++] = (char)(0xE0u | (cp >> 12)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else { utf8[n++] = (char)(0xF0u | (cp >> 18)); utf8[n++] = (char)(0x80u | ((cp >> 12) & 63)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    if (p->query_len + n >= sizeof p->query) return;
    memcpy(p->query + p->query_len, utf8, n);
    p->query_len += n;
    p->query[p->query_len] = 0;
    query_changed(p);
}

bool picker_key(Picker* p, int key, int mods)
{
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    if (!p->open) return false;
    if (key == GLFW_KEY_ESCAPE)
    {
        picker_close(p);
        return true;
    }
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
    {
        if (p->count > 0u)
        {
            p->result = p->items[p->selected];
            p->accepted = true;
        }
        p->open = false;
        return true;
    }
    if (key == GLFW_KEY_UP || (ctrl && key == GLFW_KEY_P))
    {
        if (p->selected > 0u) p->selected--;
        build_preview(p);
        return true;
    }
    if (key == GLFW_KEY_DOWN || (ctrl && key == GLFW_KEY_N) || key == GLFW_KEY_TAB)
    {
        if (p->selected + 1u < p->count) p->selected++;
        build_preview(p);
        return true;
    }
    if (key == GLFW_KEY_BACKSPACE)
    {
        if (p->query_len > 0u)
        {
            p->query_len--;
            while (p->query_len > 0u && (p->query[p->query_len] & 0xC0) == 0x80)
                p->query_len--;
            p->query[p->query_len] = 0;
            query_changed(p);
        }
        return true;
    }
    return true; /* consume all other keys while open */
}

void picker_render(const Picker* p, FontSystem* fs, DrawList* dl, uint32_t width, uint32_t height,
                   float editor_opacity)
{
    float panel[4], border[4], fg[4], dim[4], selbg[4], accent[4];
    float w, h, x, y, lh, pad, rowh, list_w, prev_x;
    uint32_t i, maxrows, start, end;
    char header[320];
    if (!p->open) return;
    /* The picker is a modal surface: it always renders opaque, independent of
     * the editor chrome opacity, so text stays legible over a background
     * image or a translucent editor. */
    (void)editor_opacity;
    /* Surface colours. `status_bg` is the bare background in Noir (0x000000),
     * so a panel painted with it is invisible against the editor regardless of
     * layering: there is no edge, only the scrim. Use `gutter_bg` for the panel
     * and `gutter_fg` for rules so the surface reads as a distinct plane.
     * Both are opaque, and `selection` is forced opaque here: an alpha of 0x66
     * (40%) over a dark panel leaves the selected row almost indistinguishable
     * from the rows around it, which is what made the list hard to scan. */
    quill_color_linear(QUILL_THEME.gutter_bg, panel);
    quill_color_linear(QUILL_THEME.gutter_fg, border);
    quill_color_linear(QUILL_THEME.fg, fg);
    quill_color_linear(QUILL_THEME.gutter_fg, dim);
    quill_color_linear(QUILL_THEME.selection, selbg);
    quill_color_linear(QUILL_THEME.accent, accent);
    panel[3] = 1.0f;
    border[3] = 0.75f;
    selbg[3] = 1.0f;
    /* Lift the selected row above the panel surface; with selection at 0x303030
     * opaque the contrast against gutter_bg 0x121212 is only ~1.3:1, too flat
     * to find at a glance, so blend it toward the foreground. */
    selbg[0] = selbg[0] * 0.55f + fg[0] * 0.45f * 0.18f;
    selbg[1] = selbg[1] * 0.55f + fg[1] * 0.45f * 0.18f;
    selbg[2] = selbg[2] * 0.55f + fg[2] * 0.45f * 0.18f;
    lh = font_line_height(fs);
    pad = 10.0f;
    w = (float)width * 0.86f;
    h = (float)height * 0.82f;
    if (w > 1100.0f) w = 1100.0f;
    if (h > 720.0f) h = 720.0f;
    x = ((float)width - w) * 0.5f;
    y = ((float)height - h) * 0.5f;
    frame_rect(dl, 0.0f, 0.0f, (float)width, (float)height, (float[4]){0.0f,0.0f,0.0f,0.45f});
    frame_rect_rounded(dl, x, y, w, h, panel, 10.0f);
    /* Inset accent rule so it follows the rounded top edge instead of
     * sticking out past the corners. */
    frame_rect_rounded(dl, x + 10.0f, y + 1.0f, w - 20.0f, 2.0f, accent, 1.0f);
    rowh = lh + 4.0f;
    snprintf(header, sizeof header, "%s  > %s", src_name(p->source), p->query);
    frame_text(fs, dl, header, (uint32_t)strlen(header), x + pad, y + pad + font_baseline(fs), fg, x + w - pad);
    frame_rect(dl, x + pad, y + pad + lh + 4.0f, w - 2.0f * pad, 1.0f, border);
    list_w = w * 0.55f;
    prev_x = x + list_w + pad;
    maxrows = (uint32_t)((h - 2.0f * pad - lh - 10.0f) / rowh);
    if (maxrows == 0u) maxrows = 1u;
    start = p->selected > maxrows / 2u ? p->selected - maxrows / 2u : 0u;
    end = start + maxrows;
    if (end > p->count) end = p->count;
    for (i = start; i < end; ++i)
    {
        float ry = y + pad + lh + 10.0f + (float)(i - start) * rowh;
        if (i == p->selected)
            frame_rect_rounded(dl, x + pad * 0.5f, ry, list_w - pad, rowh, selbg, 4.0f);
        frame_text(fs, dl, p->items[i].label, (uint32_t)strlen(p->items[i].label),
                   x + pad, ry + font_baseline(fs) + 2.0f,
                   i == p->selected ? accent : fg, x + list_w - pad);
    }
    if (p->preview[0])
    {
        char line[512];
        uint32_t pos = 0u, row = 0u;
        frame_rect(dl, prev_x - pad * 0.5f, y + pad + lh + 6.0f, 1.0f, h - 2.0f * pad - lh - 12.0f, border);
        while (p->preview[pos] && row < maxrows)
        {
            uint32_t l = 0u;
            while (p->preview[pos + l] && p->preview[pos + l] != '\n' && l < sizeof line - 1u) l++;
            memcpy(line, p->preview + pos, l);
            line[l] = 0;
            frame_text(fs, dl, line, l, prev_x, y + pad + lh + 10.0f + (float)row * rowh + font_baseline(fs),
                       dim, x + w - pad);
            pos += l;
            if (p->preview[pos] == '\n') pos++;
            row++;
        }
    }
}
