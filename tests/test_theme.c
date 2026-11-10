#include "theme.h"
#include "config.h"
#include <assert.h>

/* Values may carry inline comments, as in quill.conf.example.  The comment must
 * not become part of the value: paths failed to resolve and booleans/names
 * never matched. */
static void write_config(const char* path)
{
    FILE* f = fopen(path, "w");
    assert(f);
    fputs("style = linear              # ease | linear | instant\n"
          "smooth_blink = true         # cosine fade\n"
          "reduced_motion = true       # no animation\n"
          "cursor_vfx_mode = railgun,ripple  # modes\n"
          "background_image = ../assets/sample.c   # a readable path\n"
          "theme = none                # built-in\n"
          "editor_opacity = 0.5        # 0..1\n"
          "background_dim = 0.6        # wallpaper darkening\n"
          "background_vignette = 0.25  # edge falloff\n"
          "window_opacity = 0.8        # whole window\n"
          "indent_guides = false       # guide rules\n", f);
    fclose(f);
}

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
          "search = #12AB34CD\n"
          "type = #102030       # inline comment after a color\n"
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
    /* Search matches used to be a hardcoded yellow; the theme slot keeps that
     * exact default so existing screenshots do not change. */
    assert(QUILL_THEME.search == 0xF9E2AF55u);
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xEEEEEEFFu);
    assert(QUILL_THEME.syntax[QC_STRING] == 0x8A8A8AFFu);

    /* Config defaults: no theme file, session restore on. */
    config_defaults(&cfg);
    assert(cfg.theme[0] == 0);
    assert(cfg.session_restore == true);
    assert(cfg.session_file[0] != 0);
    /* New appearance/motion keys default to the old behaviour. */
    assert(cfg.background_dim == 0.0f);
    assert(cfg.background_vignette == 0.0f);
    assert(cfg.window_opacity == 1.0f);
    assert(cfg.indent_guides == true);

    /* Load a partial palette: present keys override, absent keys keep the
     * default, unknown keys are ignored, invalid colors are rejected. */
    write_theme("build/test_theme.theme");
    assert(theme_load_file("build/test_theme.theme"));
    assert(QUILL_THEME.bg == 0x112233FFu);              /* #RRGGBB -> opaque */
    assert(QUILL_THEME.fg == 0x445566FFu);              /* 0x prefix */
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xAABBCCDDu); /* #RRGGBBAA */
    assert(QUILL_THEME.syntax[QC_COMMENT] == 0x778899FFu); /* bare hex */
    assert(QUILL_THEME.syntax[QC_STRING] == 0xFF0000FFu);  /* #RGB expanded */
    assert(QUILL_THEME.search == 0x12AB34CDu);             /* chrome search slot */
    assert(QUILL_THEME.syntax[QC_TYPE] == 0x102030FFu);    /* inline comment stripped */
    assert(QUILL_THEME.syntax[QC_NUMBER] == 0xFF0000FFu);  /* invalid -> default */
    assert(QUILL_THEME.gutter_bg == 0x121212FFu);          /* absent -> default */

    /* Inline comments are stripped from config values. */
    write_config("build/test_config.conf");
    config_load_file(&cfg, "build/test_config.conf");
    assert(cfg.style == MOTION_LINEAR);
    assert(cfg.smooth_blink == true);
    assert(cfg.reduced_motion == true);
    assert(cfg.vfx_modes == (VFX_RAILGUN | VFX_RIPPLE));
    assert(cfg.background_image[0] != 0);
    assert(strstr(cfg.background_image, "sample.c") != NULL);
    assert(cfg.theme[0] == 0);
    assert(cfg.editor_opacity == 0.5f);
    assert(cfg.background_dim == 0.6f);
    assert(cfg.background_vignette == 0.25f);
    assert(cfg.window_opacity == 0.8f);
    assert(cfg.indent_guides == false);

    /* Out-of-range values are clamped to [0, 1]. */
    {
        FILE* f = fopen("build/test_config_clamp.conf", "w");
        assert(f);
        fputs("background_dim = 4.5\n"
              "background_vignette = -2\n"
              "window_opacity = 3\n", f);
        fclose(f);
        config_load_file(&cfg, "build/test_config_clamp.conf");
        assert(cfg.background_dim == 1.0f);
        assert(cfg.background_vignette == 0.0f);
        assert(cfg.window_opacity == 1.0f);
    }

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

    /* Every shipped palette must parse successfully and install its own
     * foreground/background instead of silently retaining Noir defaults. */
    {
        static const struct { const char* path; uint32_t bg, fg; } bundled[] = {
            { "themes/noir.theme",            0x000000FFu, 0xBCBCBCFFu },
            { "themes/catppuccin.theme",      0x1E1E2EFFu, 0xCDD6F4FFu },
            { "themes/gruvbox-dark.theme",    0x282828FFu, 0xEBDBB2FFu },
            { "themes/nord.theme",            0x2E3440FFu, 0xD8DEE9FFu },
            { "themes/light.theme",           0xFAF4EDFFu, 0x575279FFu },
            { "themes/naysayer.theme",        0x062625FFu, 0xD0B892FFu },
            { "themes/dracula.theme",         0x282A36FFu, 0xF8F8F2FFu },
            { "themes/tokyo-night.theme",     0x1A1B26FFu, 0xA9B1D6FFu },
            { "themes/one-dark.theme",        0x282C34FFu, 0xABB2BFFFu },
            { "themes/solarized-dark.theme",  0x002B36FFu, 0x839496FFu },
            { "themes/solarized-light.theme", 0xFDF6E3FFu, 0x657B83FFu },
        };
        uint32_t i;
        for (i = 0u; i < sizeof bundled / sizeof bundled[0]; ++i)
        {
            assert(theme_load_file(bundled[i].path));
            assert(QUILL_THEME.bg == bundled[i].bg);
            assert(QUILL_THEME.fg == bundled[i].fg);
        }
    }

    /* The annotated template is directly loadable, including inline comments. */
    assert(theme_load_file("theme.conf.example"));
    assert(QUILL_THEME.bg == 0x1E1E2EFFu);
    assert(QUILL_THEME.search == 0xF9E2AF55u);

    /* Naysayer keeps the defining colors from the linked upstream palette. */
    assert(theme_load_file("themes/naysayer.theme"));
    assert(QUILL_THEME.syntax[QC_KEYWORD] == 0xFFFFFFFFu);
    assert(QUILL_THEME.syntax[QC_COMMENT] == 0x53D549FFu);
    assert(QUILL_THEME.syntax[QC_STRING] == 0x3AD0B5FFu);
    assert(QUILL_THEME.status_bg == 0xD3B58EFFu);
    theme_reset();

    puts("test_theme: PASS");
    return 0;
}
