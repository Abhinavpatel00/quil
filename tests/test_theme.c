#include "theme.h"
#include "config.h"
#include <assert.h>

/* Write a palette file exercising every accepted color spelling, an unknown
 * key, and an invalid color (which must leave the slot at its default). */
static void write_theme(const char* path)
{
    FILE* f = fopen(path, "w");
    assert(f);
    fputs("name = Test Palette\n"
          "bg = #112233\n"
          "fg = 0x445566\n"
          "keyword = #AABBCCDD\n"
          "comment = 778899\n"
          "string = #F00\n"
          "bad_key = #FFFFFF\n"
          "number = notacolor\n", f);
    fclose(f);
}

int main(void)
{
    MotionConfig cfg;

    /* Built-in default is Noir, initialized statically so tests
     * that call frame_build directly never see an uninitialized palette. */
    assert(QUILL_THEME.bg == 0x000000FFu);
    assert(QUILL_THEME.fg == 0xBCBCBCFFu);
    assert(QUILL_THEME.accent == 0xEEEEEEFFu);
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xEEEEEEFFu);
    assert(QUILL_THEME.syntax[QC_STRING] == 0x8A8A8AFFu);

    /* Config defaults: no theme file, session restore on. */
    config_defaults(&cfg);
    assert(cfg.theme[0] == 0);
    assert(cfg.session_restore == true);
    assert(cfg.session_file[0] != 0);

    /* Load a partial palette: present keys override, absent keys keep the
     * default, unknown keys are ignored, invalid colors are rejected. */
    write_theme("build/test_theme.theme");
    assert(theme_load_file("build/test_theme.theme"));
    assert(QUILL_THEME.bg == 0x112233FFu);              /* #RRGGBB -> opaque */
    assert(QUILL_THEME.fg == 0x445566FFu);              /* 0x prefix */
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xAABBCCDDu); /* #RRGGBBAA */
    assert(QUILL_THEME.syntax[QC_COMMENT] == 0x778899FFu); /* bare hex */
    assert(QUILL_THEME.syntax[QC_STRING] == 0xFF0000FFu);  /* #RGB expanded */
    assert(QUILL_THEME.syntax[QC_NUMBER] == 0xFF0000FFu);  /* invalid -> default */
    assert(QUILL_THEME.gutter_bg == 0x121212FFu);          /* absent -> default */

    /* Reset returns to the built-in. */
    theme_reset();
    assert(QUILL_THEME.bg == 0x000000FFu);
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xEEEEEEFFu);

    /* NULL / empty path resets to the built-in and succeeds. */
    QUILL_THEME.bg = 0x112233FFu;
    assert(theme_load_file(NULL));
    assert(QUILL_THEME.bg == 0x000000FFu);
    QUILL_THEME.bg = 0x112233FFu;
    assert(theme_load_file(""));
    assert(QUILL_THEME.bg == 0x000000FFu);

    /* A missing file fails but still leaves the built-in palette. */
    QUILL_THEME.bg = 0x112233FFu;
    assert(!theme_load_file("build/does_not_exist.theme"));
    assert(QUILL_THEME.bg == 0x000000FFu);

    puts("test_theme: PASS");
    return 0;
}
