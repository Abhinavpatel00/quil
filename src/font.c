#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <ctype.h>

#define FONT_MAX_FACES   8u
#define FONT_SHAPE_CAP   8192u
#define FONT_ATLAS_CAP   16384u
#define FONT_SUBPIXEL    4u
#define FONT_MAX_CANDIDATES 64u
/* Shaped-line cache budget. The shape arena reserves 64 MiB of address space,
 * but only what is touched becomes resident, and shape_reset() rewinds the
 * arena WITHOUT releasing pages -- so the resident high-water is set by this
 * budget, not by the 64 MiB reservation. The old 48 MiB budget let that
 * high-water reach ~4 MiB and never return it. 2 MiB keeps ~500 full-width
 * lines (2x the ~256-row viewport, so overlays and splits still hit) and
 * measured 11.2 -> 8.1 MiB peak RSS over 20k distinct lines with no
 * measurable frame-time cost (0.026 ms/frame, 600x256-row viewport). */
#define FONT_SHAPE_BYTES (2u << 20)

typedef struct FontFace
{
    FT_Face   ft;
    hb_font_t* hb;
    char      path[512];
} FontFace;

typedef struct ShapeEntry
{
    uint64_t key;
    uint32_t offset;
    uint32_t count;
    uint32_t text_offset; /* source bytes, compared on lookup */
    uint32_t len;         /* source bytes; validated on lookup */
    uint32_t tab_width;   /* tab expansion used for this shape */
} ShapeEntry;

struct FontSystem
{
    FT_Library    ft;
    FontFace      faces[FONT_MAX_FACES];
    uint32_t      nfaces;
    uint32_t      size_px;
    uint32_t      scale;
    float         line_height;
    float         baseline;
    float         advance;

    uint8_t*      atlas_pixels;
    uint32_t      atlas_size;
    AtlasEntry*   entries;
    uint32_t      nentries;
    uint32_t      entry_cap;
    mu_hash32_t   atlas_map;
    uint32_t      shelf_x, shelf_y, shelf_h;
    uint32_t      generation;
    DirtyRect     dirty[512];
    uint32_t      ndirty;

    mu_hash32_t   shape_map;
    ShapeEntry*   shapes;
    uint32_t      nshapes;
    mu_vm_arena   shape_arena;
    hb_buffer_t*  hb_buf;
};

static uint64_t hash_bytes(const uint8_t* s, uint32_t n, uint64_t seed)
{
    uint64_t h = 1469598103934665603ull ^ seed;
    uint32_t i;
    for (i = 0u; i < n; ++i)
    {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Font discovery without fontconfig.
 *
 * fontconfig used to resolve the FALLBACK face (and, via FcNameParse, was the
 * reason a user-specified family worked at all). It costs a process-wide config
 * load plus libexpat, libpng, libbz2 and libbrotlidec, and it was only ever
 * consulted for the secondary face -- the primary face was always a hardcoded
 * path. Dropping it removes that whole dependency chain and lets the same code
 * path report a useful error instead of failing to start.
 *
 * Search order, first hit wins:
 *   1. $QUILL_FONT             explicit file path, trusted as given
 *   2. $XDG_DATA_HOME/quill/fonts and ~/.local/share/quill/fonts  user faces
 *   3. the compiled-in list below (distro package locations)
 *   4. a scan of the usual system font directories
 *
 * The scan is deliberately shallow and only accepts .ttf, .otf and .ttc
 * files, which is a handful of readdir calls: no config parse, no cache. */
#define QUILL_FONT_DIRS_MAX 8

static const char* const SYS_FONT_DIRS[] = {
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "~/.local/share/fonts",
    "~/.fonts",
    "/usr/share/fonts/truetype",
    "/usr/share/fonts/opentype",
    "/System/Library/Fonts",
    "/Library/Fonts",
};

typedef struct FontPathList
{
    char   item[FONT_MAX_CANDIDATES][512];
    size_t n;
} FontPathList;

static void list_push(FontPathList* l, const char* path)
{
    size_t i;
    if (!path || !path[0] || l->n >= FONT_MAX_CANDIDATES) return;
    for (i = 0u; i < l->n; ++i)
        if (strcmp(l->item[i], path) == 0) return;
    snprintf(l->item[l->n], sizeof l->item[l->n], "%s", path);
    l->n++;
}

static bool has_font_ext(const char* name)
{
    size_t n;
    if (!name) return false;
    n = strlen(name);
    if (n < 4u) return false;
    return strcasecmp(name + n - 4u, ".ttf") == 0 ||
           strcasecmp(name + n - 4u, ".otf") == 0 ||
           strcasecmp(name + n - 4u, ".ttc") == 0;
}

static void expand_home(char* out, size_t cap, const char* path)
{
    const char* home;
    if (path[0] != '~') { snprintf(out, cap, "%.*s", (int)(cap - 1u), path); return; }
    home = getenv("HOME");
    if (!home) home = "";
    snprintf(out, cap, "%.*s%s", (int)(cap / 2u), home, path + 1);
}

/* Shallow recursive collect of font files under `dir` (depth-limited so a
 * pathological tree cannot stall startup). */
static void scan_dir(FontPathList* l, const char* dir, int depth)
{
    char real[512];
    DIR* d;
    struct dirent* e;
    if (depth <= 0) return;
    expand_home(real, sizeof real, dir);
    d = opendir(real);
    if (!d) return;
    while ((e = readdir(d)) != NULL)
    {
        char path[512];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%.*s/%.*s",
                 (int)(sizeof path / 2u), real,
                 (int)(sizeof path / 2u - 2u), e->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(l, path, depth - 1);
        else if (S_ISREG(st.st_mode) && has_font_ext(e->d_name)) list_push(l, path);
    }
    closedir(d);
}

/* True if `name` (lowercased, no separators) appears in the basename. */
static bool name_matches(const char* path, const char* needle)
{
    const char* base = strrchr(path, '/');
    char low[512];
    size_t i;
    base = base ? base + 1 : path;
    snprintf(low, sizeof low, "%.*s", (int)(sizeof low - 1u), base);
    for (i = 0u; low[i]; ++i) low[i] = (char)tolower((unsigned char)low[i]);
    return strstr(low, needle) != NULL;
}

static void collect_candidates(FontPathList* l, bool monospace)
{
    static const char* const MONO_NEEDLES[] = {
        "fira code", "firacode", "jetbrainsmono", "jetbrains mono",
        "notosansmono", "noto sans mono", "dejavusansmono", "dejavu sans mono",
        "liberationmono", "sourcecodepro", "source code pro", "cascadia",
        "iosevka", "hack", "ubuntumono", "courier",
    };
    static const char* const MONO_FILES[] = {
        "/usr/share/fonts/truetype/fira-code/FiraCode-Regular.ttf",
        "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
        "/usr/share/fonts/truetype/JetBrainsMono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    static const char* const FALLBACK_FILES[] = {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    size_t i;
    const char* env = getenv(monospace ? "QUILL_FONT_MONO" : "QUILL_FONT");
    if (env && env[0]) list_push(l, env);
    {
        const char* xdg = getenv("XDG_DATA_HOME");
        char dir[512];
        if (xdg && xdg[0]) { snprintf(dir, sizeof dir, "%s/quill/fonts", xdg); scan_dir(l, dir, 2); }
        scan_dir(l, "~/.local/share/quill/fonts", 2);
    }
    if (monospace)
        for (i = 0u; i < sizeof MONO_FILES / sizeof MONO_FILES[0]; ++i) list_push(l, MONO_FILES[i]);
    else
        for (i = 0u; i < sizeof FALLBACK_FILES / sizeof FALLBACK_FILES[0]; ++i) list_push(l, FALLBACK_FILES[i]);

    /* Only pay for the directory scan if the well-known paths were all absent
     * or unusable; the caller re-runs this lazily on failure. */
    for (i = 0u; i < sizeof SYS_FONT_DIRS / sizeof SYS_FONT_DIRS[0]; ++i)
        scan_dir(l, SYS_FONT_DIRS[i], 3);

    /* Prefer faces whose basename looks monospace; keep the rest as a last
     * resort because ft.c verifies FT_IS_FIXED_WIDTH before accepting one. */
    if (monospace && l->n > 1u)
    {
        char tmp[FONT_MAX_CANDIDATES][512];
        size_t nt = 0u, k;
        for (k = 0u; k < l->n; ++k)
            for (i = 0u; i < sizeof MONO_NEEDLES / sizeof MONO_NEEDLES[0]; ++i)
                if (name_matches(l->item[k], MONO_NEEDLES[i]))
                {
                    snprintf(tmp[nt], sizeof tmp[nt], "%s", l->item[k]);
                    nt++;
                    break;
                }
        for (k = 0u; k < l->n && nt < FONT_MAX_CANDIDATES; ++k)
        {
            size_t j; bool dup = false;
            for (j = 0u; j < nt; ++j) if (strcmp(tmp[j], l->item[k]) == 0) { dup = true; break; }
            if (!dup) { snprintf(tmp[nt], sizeof tmp[nt], "%s", l->item[k]); nt++; }
        }
        for (k = 0u; k < nt; ++k)
            snprintf(l->item[k], sizeof l->item[k], "%.*s",
                     (int)(sizeof l->item[k] - 1u), tmp[k]);
        l->n = nt;
    }
}

static bool load_face(FontSystem* fs, const char* path, bool fixed_width)
{
    FontFace* f;
    if (fs->nfaces >= FONT_MAX_FACES) return false;
    f = &fs->faces[fs->nfaces];
    if (FT_New_Face(fs->ft, path, 0, &f->ft) != 0)
        return false;
    /* Verify the actual face, not just fontconfig's ranking. A proportional
     * primary silently corrupts both editor and terminal cell alignment. */
    if (fixed_width && !FT_IS_FIXED_WIDTH(f->ft))
    {
        FT_Done_Face(f->ft);
        f->ft = NULL;
        return false;
    }
    snprintf(f->path, sizeof f->path, "%s", path);
    if (FT_Set_Pixel_Sizes(f->ft, 0, fs->size_px * fs->scale) != 0)
    {
        FT_Done_Face(f->ft);
        return false;
    }
    f->hb = hb_ft_font_create_referenced(f->ft);
    if (!f->hb)
    {
        FT_Done_Face(f->ft);
        return false;
    }
    hb_ft_font_set_load_flags(f->hb, FT_LOAD_TARGET_LIGHT);
    fs->nfaces++;
    return true;
}

static void recompute_metrics(FontSystem* fs)
{
    FT_Face f = fs->faces[0].ft;
    float ascent = (float)(f->size->metrics.ascender >> 6);
    float descent = (float)(-(f->size->metrics.descender >> 6));
    float height = (float)(f->size->metrics.height >> 6);
    FT_UInt gi = FT_Get_Char_Index(f, 'M');
    fs->baseline = ascent;
    fs->line_height = quill_ceilf(height > ascent + descent ? height : ascent + descent);
    fs->advance = 0.0f;
    if (gi && FT_Load_Glyph(f, gi, FT_LOAD_TARGET_LIGHT) == 0)
        fs->advance = (float)(f->glyph->advance.x >> 6);
    if (fs->advance <= 0.0f)
        fs->advance = (float)(fs->size_px * fs->scale) * 0.6f;
}

static void atlas_reset(FontSystem* fs)
{
    memset(fs->atlas_pixels, 0, (size_t)fs->atlas_size * fs->atlas_size);
    fs->nentries = 0u;
    fs->shelf_x = fs->shelf_y = fs->shelf_h = 0u;
    fs->generation++;
    mu_hash32_clear(&fs->atlas_map);
    fs->ndirty = 1u;
    fs->dirty[0].x = 0u;
    fs->dirty[0].y = 0u;
    fs->dirty[0].w = fs->atlas_size;
    fs->dirty[0].h = fs->atlas_size;
}

static bool atlas_upgrade(FontSystem* fs)
{
    uint32_t old_size = fs->atlas_size;
    uint32_t new_size = old_size * 2u;
    uint32_t y;
    uint8_t* pixels;
    if (old_size == 0u || new_size > QUILL_ATLAS_SIZE) return false;
    if (new_size > QUILL_ATLAS_SIZE) new_size = QUILL_ATLAS_SIZE;
    pixels = (uint8_t*)MU_CALLOC((size_t)new_size * new_size, 1u);
    if (!pixels) return false;
    for (y = 0u; y < old_size; ++y)
        memcpy(pixels + (size_t)y * new_size,
               fs->atlas_pixels + (size_t)y * old_size, old_size);
    MU_FREE(fs->atlas_pixels);
    fs->atlas_pixels = pixels;
    fs->atlas_size = new_size;
    fs->generation++;
    fs->ndirty = 1u;
    fs->dirty[0].x = 0u;
    fs->dirty[0].y = 0u;
    fs->dirty[0].w = new_size;
    fs->dirty[0].h = new_size;
    return true;
}

static void atlas_init(FontSystem* fs)
{
    /* atlas_pixels comes from MU_CALLOC, so it is already zero.  Do not
     * touch all 1 MiB or mark it fully dirty: the first glyphs will add
     * small dirty rectangles and upload only what the frame samples.
     * If the small atlas fills, atlas_upgrade() grows it to 2048x2048,
     * mirroring the old pixels and preserving existing entries. */
    fs->atlas_size = QUILL_ATLAS_SMALL_SIZE;
    fs->nentries = 0u;
    fs->shelf_x = fs->shelf_y = fs->shelf_h = 0u;
    fs->generation = 1u;
    mu_hash32_clear(&fs->atlas_map);
    fs->ndirty = 0u;
}

static void shape_reset(FontSystem* fs)
{
    mu_vm_arena_reset(&fs->shape_arena);
    mu_hash32_clear(&fs->shape_map);
    fs->nshapes = 0u;
}

FontSystem* font_create(uint32_t size_px, uint32_t scale)
{
    FontSystem* fs;
    FontPathList* list;
    size_t ci;
    bool fallback_loaded = false;

    fs = (FontSystem*)MU_CALLOC(1u, sizeof *fs);
    if (!fs) return NULL;
    fs->size_px = size_px ? size_px : 16u;
    fs->scale = scale ? scale : 1u;
    if (FT_Init_FreeType(&fs->ft) != 0)
        goto fail;

    /* Candidate discovery is shared by both faces. Allocated once and freed
     * before returning so it never occupies a permanent slot. */
    list = (FontPathList*)MU_CALLOC(1u, sizeof *list);
    if (!list) goto fail;

    /* Primary face: the first candidate that actually loads and reports
     * FT_IS_FIXED_WIDTH. A proportional primary would corrupt editor and
     * terminal cell alignment, so it is rejected rather than used. */
    collect_candidates(list, true);
    for (ci = 0u; ci < list->n && fs->nfaces == 0u; ++ci)
    {
        FontFace* f = &fs->faces[0];
        if (FT_New_Face(fs->ft, list->item[ci], 0, &f->ft) != 0)
            continue;
        if (!FT_IS_FIXED_WIDTH(f->ft) ||
            FT_Set_Pixel_Sizes(f->ft, 0, fs->size_px * fs->scale) != 0)
        {
            FT_Done_Face(f->ft);
            f->ft = NULL;
            continue;
        }
        f->hb = hb_ft_font_create_referenced(f->ft);
        if (!f->hb)
        {
            FT_Done_Face(f->ft);
            f->ft = NULL;
            continue;
        }
        hb_ft_font_set_load_flags(f->hb, FT_LOAD_TARGET_LIGHT);
        snprintf(f->path, sizeof f->path, "%s", list->item[ci]);
        fs->nfaces = 1u;
    }
    if (fs->nfaces == 0u)
    {
        fprintf(stderr,
                "quill: no monospace font found. Install one (e.g. "
                "fonts-jetbrains-mono or fonts-firacode) or set QUILL_FONT_MONO "
                "to a .ttf path.\n");
        MU_FREE(list);
        goto fail;
    }

    /* Secondary face for glyphs the primary lacks. Optional: an editor with
     * only a monospace face still renders every ASCII codepoint. */
    list->n = 0u;
    collect_candidates(list, false);
    for (ci = 0u; ci < list->n; ++ci)
    {
        if (strcmp(list->item[ci], fs->faces[0].path) == 0) continue;
        if (load_face(fs, list->item[ci], false))
        {
            fallback_loaded = true;
            break;
        }
    }
    MU_FREE(list);
    (void)fallback_loaded; /* absence is fine: primary covers ASCII */

    fs->atlas_pixels = (uint8_t*)MU_CALLOC((size_t)QUILL_ATLAS_SMALL_SIZE * QUILL_ATLAS_SMALL_SIZE, 1u);
    fs->entry_cap = 1024u;
    fs->entries = (AtlasEntry*)MU_MALLOC((size_t)fs->entry_cap * sizeof *fs->entries);
    fs->shapes = (ShapeEntry*)MU_MALLOC((size_t)FONT_SHAPE_CAP * sizeof *fs->shapes);
    if (!fs->atlas_pixels || !fs->entries || !fs->shapes)
        goto fail;
    if (!mu_hash32_init(&fs->atlas_map, FONT_ATLAS_CAP)) goto fail;
    if (!mu_hash32_init(&fs->shape_map, FONT_SHAPE_CAP)) goto fail;
    if (!mu_vm_arena_init(&fs->shape_arena, 64u << 20, 64u << 10)) goto fail;
    fs->hb_buf = hb_buffer_create();
    if (!fs->hb_buf) goto fail;
    recompute_metrics(fs);
    atlas_init(fs);
    quill_trim_allocator();
    return fs;

fail:
    font_destroy(fs);
    return NULL;
}

void font_destroy(FontSystem* fs)
{
    uint32_t i;
    if (!fs) return;
    for (i = 0u; i < fs->nfaces; ++i)
    {
        if (fs->faces[i].hb) hb_font_destroy(fs->faces[i].hb);
        if (fs->faces[i].ft) FT_Done_Face(fs->faces[i].ft);
    }
    mu_hash32_destroy(&fs->atlas_map);
    mu_hash32_destroy(&fs->shape_map);
    if (fs->hb_buf) hb_buffer_destroy(fs->hb_buf);
    mu_vm_arena_destroy(&fs->shape_arena);
    MU_FREE(fs->atlas_pixels);
    MU_FREE(fs->entries);
    MU_FREE(fs->shapes);
    if (fs->ft) FT_Done_FreeType(fs->ft);
    MU_FREE(fs);
}

void font_set_size(FontSystem* fs, uint32_t size_px, uint32_t scale)
{
    uint32_t i;
    if (!fs) return;
    size_px = size_px ? size_px : 16u;
    scale = scale ? scale : 1u;
    if (fs->size_px == size_px && fs->scale == scale) return;
    fs->size_px = size_px;
    fs->scale = scale;
    for (i = 0u; i < fs->nfaces; ++i)
    {
        FT_Set_Pixel_Sizes(fs->faces[i].ft, 0, size_px * scale);
        hb_ft_font_changed(fs->faces[i].hb);
    }
    recompute_metrics(fs);
    atlas_reset(fs);
    shape_reset(fs);
}

uint32_t font_size(const FontSystem* fs) { return fs->size_px * fs->scale; }
float font_line_height(const FontSystem* fs) { return fs->line_height; }
float font_baseline(const FontSystem* fs) { return fs->baseline; }
float font_advance(const FontSystem* fs) { return fs->advance; }
uint32_t font_atlas_width(const FontSystem* fs) { return fs ? fs->atlas_size : QUILL_ATLAS_SMALL_SIZE; }
uint32_t font_atlas_height(const FontSystem* fs) { return fs ? fs->atlas_size : QUILL_ATLAS_SMALL_SIZE; }
const uint8_t* font_atlas_pixels(const FontSystem* fs) { return fs->atlas_pixels; }
uint32_t font_atlas_dirty_count(const FontSystem* fs) { return fs->ndirty; }
const DirtyRect* font_atlas_dirty(const FontSystem* fs) { return fs->dirty; }
void font_atlas_clear_dirty(FontSystem* fs) { fs->ndirty = 0u; }
uint32_t font_atlas_generation(const FontSystem* fs) { return fs->generation; }

static uint64_t atlas_key(uint16_t face, uint32_t glyph, uint32_t size, uint32_t phase)
{
    return (uint64_t)face | ((uint64_t)glyph << 8) |
           ((uint64_t)size << 40) | ((uint64_t)phase << 56);
}

static bool atlas_pack(FontSystem* fs, uint32_t w, uint32_t h,
                       uint32_t* x, uint32_t* y)
{
    if (w > QUILL_ATLAS_SIZE || h > QUILL_ATLAS_SIZE)
        return false;
    if (fs->shelf_x + w > fs->atlas_size)
    {
        fs->shelf_y += fs->shelf_h;
        fs->shelf_x = 0u;
        fs->shelf_h = 0u;
    }
    if (fs->shelf_y + h > fs->atlas_size)
    {
        if (fs->atlas_size < QUILL_ATLAS_SIZE && atlas_upgrade(fs))
        {
            /* The upgraded atlas preserves old entries and shelves, so the
             * pending glyph can be packed immediately. */
        }
        else
        {
            atlas_reset(fs);
            if (h > fs->atlas_size) return false;
        }
    }
    *x = fs->shelf_x;
    *y = fs->shelf_y;
    fs->shelf_x += w;
    if (h > fs->shelf_h) fs->shelf_h = h;
    return true;
}

static void add_dirty(FontSystem* fs, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (fs->ndirty >= sizeof fs->dirty / sizeof fs->dirty[0])
    {
        /* Merge into a full upload rather than dropping updates. */
        fs->ndirty = 1u;
        fs->dirty[0].x = 0u;
        fs->dirty[0].y = 0u;
        fs->dirty[0].w = fs->atlas_size;
        fs->dirty[0].h = fs->atlas_size;
        return;
    }
    fs->dirty[fs->ndirty].x = x;
    fs->dirty[fs->ndirty].y = y;
    fs->dirty[fs->ndirty].w = w;
    fs->dirty[fs->ndirty].h = h;
    fs->ndirty++;
}

const AtlasEntry* font_atlas_get(FontSystem* fs, uint16_t face_id, uint32_t glyph,
                                 uint32_t phase)
{
    uint64_t key;
    uint32_t idx;
    AtlasEntry* e;
    FontFace* f;
    FT_GlyphSlot slot;
    FT_Matrix ident = { 0x10000L, 0L, 0L, 0x10000L };
    FT_Vector delta;
    uint32_t w, h, ax, ay, row;
    const uint8_t* src;
    uint8_t* dst;
    uint8_t tmp[256 * 256];
    bool color = false;

    if (!fs || face_id >= fs->nfaces) return NULL;
    phase %= FONT_SUBPIXEL;
    key = atlas_key(face_id, glyph, font_size(fs), phase);
    if (mu_hash32_get(&fs->atlas_map, key, &idx) && idx != 0u &&
        idx <= fs->nentries && fs->entries[idx - 1u].key == key)
        return &fs->entries[idx - 1u];

    f = &fs->faces[face_id];
    delta.x = (FT_Pos)(phase * 16); /* quarter-pixel in 26.6 */
    delta.y = 0;
    FT_Set_Transform(f->ft, &ident, &delta);
    if (FT_Load_Glyph(f->ft, glyph, FT_LOAD_TARGET_LIGHT) != 0)
    {
        FT_Set_Transform(f->ft, &ident, NULL);
        return NULL;
    }
    if (FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL) != 0)
    {
        FT_Set_Transform(f->ft, &ident, NULL);
        return NULL;
    }
    FT_Set_Transform(f->ft, &ident, NULL);
    slot = f->ft->glyph;
    if (slot->bitmap.width == 0u || slot->bitmap.rows == 0u)
    {
        /* Space / zero-ink glyph: cache a 1x1 empty entry. */
        w = 1u; h = 1u;
        if (!atlas_pack(fs, w + 2u, h + 2u, &ax, &ay)) return NULL;
        if (fs->nentries >= fs->entry_cap)
        {
            uint32_t nc = fs->entry_cap * 2u;
            AtlasEntry* ne = (AtlasEntry*)MU_REALLOC(fs->entries, (size_t)nc * sizeof *ne);
            if (!ne) return NULL;
            fs->entries = ne;
            fs->entry_cap = nc;
        }
        e = &fs->entries[fs->nentries++];
        memset(e, 0, sizeof *e);
        e->key = key;
        e->x = ax + 1u; e->y = ay + 1u; e->w = w; e->h = h;
        e->left = 0; e->top = 0;
        mu_hash32_set(&fs->atlas_map, key, fs->nentries);
        add_dirty(fs, ax, ay, w + 2u, h + 2u);
        return e;
    }

    src = slot->bitmap.buffer;
    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA)
    {
        color = true;
        if ((size_t)slot->bitmap.width * slot->bitmap.rows > sizeof tmp)
            return NULL;
        for (row = 0u; row < slot->bitmap.rows; ++row)
        {
            const uint8_t* p = src + (size_t)row * slot->bitmap.pitch;
            uint8_t* q = tmp + (size_t)row * slot->bitmap.width;
            uint32_t col;
            for (col = 0u; col < slot->bitmap.width; ++col)
            {
                uint32_t b = p[col * 4u + 0u], g = p[col * 4u + 1u],
                         r = p[col * 4u + 2u], a = p[col * 4u + 3u];
                uint32_t lum = (r * 54u + g * 183u + b * 19u) >> 8;
                q[col] = (uint8_t)(a > lum ? a : lum);
            }
        }
        src = tmp;
    }

    w = slot->bitmap.width;
    h = slot->bitmap.rows;
    if (!atlas_pack(fs, w + 2u, h + 2u, &ax, &ay))
        return NULL;
    if (fs->nentries >= fs->entry_cap)
    {
        uint32_t nc = fs->entry_cap * 2u;
        AtlasEntry* ne = (AtlasEntry*)MU_REALLOC(fs->entries, (size_t)nc * sizeof *ne);
        if (!ne) return NULL;
        fs->entries = ne;
        fs->entry_cap = nc;
    }
    e = &fs->entries[fs->nentries++];
    e->key = key;
    e->x = ax + 1u;
    e->y = ay + 1u;
    e->w = w;
    e->h = h;
    e->left = (int16_t)slot->bitmap_left;
    e->top = (int16_t)slot->bitmap_top;
    dst = fs->atlas_pixels + (size_t)(ay + 1u) * fs->atlas_size + (ax + 1u);
    for (row = 0u; row < h; ++row)
    {
        const uint8_t* p = src + (size_t)row * (color ? w : (uint32_t)slot->bitmap.pitch);
        memcpy(dst + (size_t)row * fs->atlas_size, p, w);
    }
    mu_hash32_set(&fs->atlas_map, key, fs->nentries);
    add_dirty(fs, ax, ay, w + 2u, h + 2u);
    return e;
}

static uint32_t decode_utf8(const uint8_t* s, uint32_t len, uint32_t i, uint32_t* cp)
{
    uint32_t n = 1u;
    uint32_t c = s[i];
    if ((c & 0xE0u) == 0xC0u && i + 1u < len) { c &= 0x1Fu; n = 2u; }
    else if ((c & 0xF0u) == 0xE0u && i + 2u < len) { c &= 0x0Fu; n = 3u; }
    else if ((c & 0xF8u) == 0xF0u && i + 3u < len) { c &= 0x07u; n = 4u; }
    else { *cp = c; return 1u; }
    {
        uint32_t k;
        for (k = 1u; k < n; ++k)
        {
            if ((s[i + k] & 0xC0u) != 0x80u) { *cp = s[i]; return 1u; }
            c = (c << 6) | (s[i + k] & 0x3Fu);
        }
    }
    *cp = c;
    return n;
}

static uint16_t face_for_cp(FontSystem* fs, uint32_t cp)
{
    uint16_t i;
    for (i = 0u; i < fs->nfaces; ++i)
        if (FT_Get_Char_Index(fs->faces[i].ft, cp) != 0u)
            return i;
    return 0u;
}

uint32_t font_shape_line(FontSystem* fs, const uint8_t* text, uint32_t len,
                         uint32_t tab_width, const ShapedGlyph** out)
{
    uint8_t exp[QUILL_MAX_LINE * 2];
    uint32_t map[QUILL_MAX_LINE * 2];
    uint32_t n = 0u, i, col = 0u;
    uint64_t key;
    uint32_t idx;
    ShapeEntry* se;
    ShapedGlyph* glyphs;
    uint32_t count, cap;
    hb_buffer_t* buf = fs->hb_buf;
    uint32_t pos;

    *out = NULL;
    if (!fs || len == 0u) return 0u;
    if (len > QUILL_MAX_LINE) len = QUILL_MAX_LINE;
    if (tab_width == 0u) tab_width = QUILL_TAB_WIDTH;

    key = hash_bytes(text, len, ((uint64_t)font_size(fs) << 32) |
                                 ((uint64_t)len << 16) | (uint64_t)tab_width);
    if (mu_hash32_get(&fs->shape_map, key, &idx) && idx != 0u &&
        idx <= fs->nshapes && fs->shapes[idx - 1u].key == key &&
        fs->shapes[idx - 1u].len == len &&
        fs->shapes[idx - 1u].tab_width == tab_width &&
        memcmp((const uint8_t*)mu_vm_arena_base(&fs->shape_arena) +
                   fs->shapes[idx - 1u].text_offset, text, len) == 0)
    {
        se = &fs->shapes[idx - 1u];
        *out = (const ShapedGlyph*)((const uint8_t*)mu_vm_arena_base(&fs->shape_arena) + se->offset);
        return se->count;
    }

    /* Expand tabs; map each display byte back to its original byte. */
    for (i = 0u; i < len; ++i)
    {
        if (text[i] == '\t')
        {
            uint32_t stop = ((col / tab_width) + 1u) * tab_width;
            while (col < stop && n < QUILL_MAX_LINE * 2)
            {
                exp[n] = ' ';
                map[n] = i;
                n++; col++;
            }
        }
        else
        {
            if (n >= QUILL_MAX_LINE * 2) break;
            exp[n] = text[i];
            map[n] = i;
            n++;
            if ((text[i] & 0xC0u) != 0x80u) col++;
        }
    }
    if (n == 0u) return 0u;

    /* Upper bound: one glyph per display byte, plus slack for shaping, plus
     * the source bytes kept for collision-free lookup. */
    cap = n + 16u;
    if (fs->nshapes >= FONT_SHAPE_CAP ||
        mu_vm_arena_used(&fs->shape_arena) + (size_t)cap * sizeof(ShapedGlyph) +
            (size_t)len + 16u > FONT_SHAPE_BYTES)
        shape_reset(fs);

    glyphs = (ShapedGlyph*)mu_vm_arena_alloc(&fs->shape_arena,
                                             (size_t)cap * sizeof *glyphs, 16u);
    if (!glyphs) return 0u;
    count = 0u;
    pos = 0u;
    while (pos < n)
    {
        uint32_t cp, cn = decode_utf8(exp, n, pos, &cp);
        uint16_t face = face_for_cp(fs, cp);
        uint32_t run_start = pos, run_end = pos + cn;
        uint32_t j;
        /* Extend run while the same face covers the codepoints. */
        while (run_end < n)
        {
            uint32_t cp2, n2 = decode_utf8(exp, n, run_end, &cp2);
            if (face_for_cp(fs, cp2) != face) break;
            run_end += n2;
        }
        hb_buffer_reset(buf);
        hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_add_utf8(buf, (const char*)exp + run_start, (int)(run_end - run_start),
                           0, (int)(run_end - run_start));
        hb_buffer_guess_segment_properties(buf);
        hb_shape(fs->faces[face].hb, buf, NULL, 0);
        {
            unsigned ng = hb_buffer_get_length(buf);
            const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, NULL);
            const hb_glyph_position_t* pp = hb_buffer_get_glyph_positions(buf, NULL);
            for (j = 0u; j < ng; ++j)
            {
                uint32_t cluster = run_start + info[j].cluster;
                ShapedGlyph* g;
                if (count >= cap) break;
                g = &glyphs[count++];
                g->glyph = info[j].codepoint;
                g->cluster = cluster < n ? map[cluster] : 0u;
                g->face = face;
                g->pad = 0u;
                g->x_offset = (float)pp[j].x_offset / 64.0f;
                g->y_offset = (float)pp[j].y_offset / 64.0f;
                g->x_advance = (float)pp[j].x_advance / 64.0f;
            }
        }
        pos = run_end;
    }

    /* Bake cumulative run offsets and snap the code grid to whole pixels.
     * Integer advances maximize atlas reuse (one subpixel phase) and keep
     * hinted stems crisp; fractional shaping is still used internally. */
    {
        float pen = 0.0f;
        uint32_t k;
        for (k = 0u; k < count; ++k)
        {
            float x = quill_roundf(pen + glyphs[k].x_offset);
            pen += glyphs[k].x_advance;
            glyphs[k].x_offset = x;
            glyphs[k].x_advance = quill_roundf(pen) - x;
            if (glyphs[k].x_advance < 0.0f)
                glyphs[k].x_advance = 0.0f;
        }
    }

    {
        uint8_t* base = (uint8_t*)mu_vm_arena_base(&fs->shape_arena);
        uint8_t* src_copy = (uint8_t*)mu_vm_arena_alloc(&fs->shape_arena, len, 1u);
        if (src_copy)
        {
            memcpy(src_copy, text, len);
            se = &fs->shapes[fs->nshapes++];
            se->key = key;
            se->offset = (uint32_t)((uint8_t*)glyphs - base);
            se->count = count;
            se->text_offset = (uint32_t)(src_copy - base);
            se->len = len;
            se->tab_width = tab_width;
            mu_hash32_set(&fs->shape_map, key, fs->nshapes);
        }
    }
    *out = glyphs;
    return count;
}

float font_line_x_for_byte(const ShapedGlyph* g, uint32_t n, uint32_t byte)
{
    float pen = 0.0f;
    uint32_t i;
    for (i = 0u; i < n; ++i)
    {
        if (g[i].cluster >= byte)
            return pen;
        pen += g[i].x_advance;
    }
    return pen;
}
