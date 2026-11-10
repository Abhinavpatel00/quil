#include "theme.h"

/* Built-in Noir, a grayscale/red adaptation of vim-256noir
 * (https://github.com/andreasvc/vim-256noir). Written once as an initializer
 * macro so the active palette and the reset target cannot drift apart. */
#define QUILL_THEME_DEFAULT_INIT { \
    0x000000FFu, /* bg */ \
    0xBCBCBCFFu, /* fg */ \
    0x121212FFu, /* gutter_bg */ \
    0x585858FFu, /* gutter_fg */ \
    0x12121266u, /* line_hl */ \
    0x30303066u, /* selection */ \
    0xF9E2AF55u, /* search */ \
    0xEEEEEEFFu, /* cursor */ \
    0x000000FFu, /* status_bg */ \
    0x8A8A8AFFu, /* status_fg */ \
    0xEEEEEEFFu, /* accent */ \
    { \
        0xBCBCBCFFu, /* default */ \
        0xEEEEEEFFu, /* keyword */ \
        0xEEEEEEFFu, /* function */ \
        0x8A8A8AFFu, /* string */ \
        0x585858FFu, /* comment */ \
        0xFF0000FFu, /* number */ \
        0xEEEEEEFFu, /* type */ \
        0xBCBCBCFFu, /* variable */ \
        0xEEEEEEFFu, /* operator */ \
        0xBCBCBCFFu, /* punctuation */ \
        0xBCBCBCFFu, /* property */ \
        0xD0D0D0FFu, /* constant */ \
        0xEEEEEEFFu, /* attribute */ \
        0xEEEEEEFFu, /* label */ \
        0xFF0000FFu, /* escape */ \
        0xFF0000FFu, /* error */ \
    } \
}

static const QuillTheme THEME_DEFAULT = QUILL_THEME_DEFAULT_INIT;
QuillTheme QUILL_THEME = QUILL_THEME_DEFAULT_INIT;

void theme_reset(void)
{
    QUILL_THEME = THEME_DEFAULT;
}

/* Map a palette key to its uint32_t slot. Load-time only, so the linear
 * strcmp chain is fine and keeps the file format flat and obvious. */
static uint32_t* theme_slot(QuillTheme* t, const char* k)
{
    if (strcmp(k, "bg") == 0) return &t->bg;
    if (strcmp(k, "fg") == 0) return &t->fg;
    if (strcmp(k, "gutter_bg") == 0) return &t->gutter_bg;
    if (strcmp(k, "gutter_fg") == 0) return &t->gutter_fg;
    if (strcmp(k, "line_hl") == 0) return &t->line_hl;
    if (strcmp(k, "selection") == 0) return &t->selection;
    if (strcmp(k, "search") == 0) return &t->search;
    if (strcmp(k, "cursor") == 0) return &t->cursor;
    if (strcmp(k, "status_bg") == 0) return &t->status_bg;
    if (strcmp(k, "status_fg") == 0) return &t->status_fg;
    if (strcmp(k, "accent") == 0) return &t->accent;
    if (strcmp(k, "default") == 0) return &t->syntax[QC_DEFAULT];
    if (strcmp(k, "keyword") == 0) return &t->syntax[QC_KEYWORD];
    if (strcmp(k, "function") == 0) return &t->syntax[QC_FUNCTION];
    if (strcmp(k, "string") == 0) return &t->syntax[QC_STRING];
    if (strcmp(k, "comment") == 0) return &t->syntax[QC_COMMENT];
    if (strcmp(k, "number") == 0) return &t->syntax[QC_NUMBER];
    if (strcmp(k, "type") == 0) return &t->syntax[QC_TYPE];
    if (strcmp(k, "variable") == 0) return &t->syntax[QC_VARIABLE];
    if (strcmp(k, "operator") == 0) return &t->syntax[QC_OPERATOR];
    if (strcmp(k, "punctuation") == 0) return &t->syntax[QC_PUNCTUATION];
    if (strcmp(k, "property") == 0) return &t->syntax[QC_PROPERTY];
    if (strcmp(k, "constant") == 0) return &t->syntax[QC_CONSTANT];
    if (strcmp(k, "attribute") == 0) return &t->syntax[QC_ATTRIBUTE];
    if (strcmp(k, "label") == 0) return &t->syntax[QC_LABEL];
    if (strcmp(k, "escape") == 0) return &t->syntax[QC_ESCAPE];
    if (strcmp(k, "error") == 0) return &t->syntax[QC_ERROR];
    return NULL;
}

/* Parse #RGB / #RRGGBB / #RRGGBBAA (also 0x-prefixed or bare hex). Stored as
 * 0xRRGGBBAA; a 6-digit form gets full alpha, a 3-digit form is expanded. */
static bool parse_color(const char* v, uint32_t* out)
{
    uint32_t val = 0u;
    uint32_t digits = 0u;
    if (v[0] == '#') v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    for (; *v; ++v)
    {
        char c = *v;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A') + 10u;
        else return false;
        val = (val << 4) | d;
        digits++;
        if (digits > 8u) return false;
    }
    if (digits == 3u)
    {
        uint32_t r = (val >> 8) & 0xFu, g = (val >> 4) & 0xFu, b = val & 0xFu;
        val = (r * 17u) << 16 | (g * 17u) << 8 | (b * 17u);
        digits = 6u;
    }
    if (digits == 6u) { *out = (val << 8) | 0xFFu; return true; }
    if (digits == 8u) { *out = val; return true; }
    return false;
}

bool theme_load_file(const char* path)
{
    FILE* f;
    char line[512];
    theme_reset();
    if (!path || !path[0]) return true;
    f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof line, f))
    {
        char* eq = strchr(line, '=');
        char* k = line;
        char* v;
        char* end;
        uint32_t* slot;
        uint32_t color;
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        end = k + strlen(k);
        while (end > k && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
            *--end = 0;
        while (*v == ' ' || *v == '\t') v++;
        /* A '#' starts an inline comment only after whitespace; the leading
         * '#' in #RRGGBB / #RRGGBBAA is part of the color itself. */
        for (end = v; *end; ++end)
        {
            if (*end == '#' && end > v && (end[-1] == ' ' || end[-1] == '\t'))
            {
                *end = 0;
                break;
            }
        }
        end = v + strlen(v);
        while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
            *--end = 0;
        if (k[0] == '#' || k[0] == 0) continue;
        slot = theme_slot(&QUILL_THEME, k);
        if (!slot) continue; /* unknown keys (e.g. "name") are ignored */
        if (parse_color(v, &color)) *slot = color;
    }
    fclose(f);
    return true;
}
