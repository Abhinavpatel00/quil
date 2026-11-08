#include "frame.h"
#include "render_vk.h"
#include "config.h"
#include <assert.h>

/* The renderer TU owns the stb_image_write implementation. */
#include "stb/stb_image_write.h"

/* Write a small solid-color PNG so the background loader has a real file. */
static void write_test_png(const char* path)
{
    unsigned char pixels[64 * 64 * 3];
    uint32_t y, x;
    for (y = 0u; y < 64u; ++y)
        for (x = 0u; x < 64u; ++x)
        {
            uint32_t i = (y * 64u + x) * 3u;
            pixels[i + 0u] = 200u;
            pixels[i + 1u] = 90u;
            pixels[i + 2u] = 60u;
        }
    assert(stbi_write_png(path, 64, 64, 3, pixels, 64 * 3));
}

int main(void)
{
    Document      d;
    FontSystem*   fs;
    DrawList*     dl;
    FrameScratch* scratch;
    LineLayout    layouts[QUILL_MAX_VISIBLE];
    uint32_t      nlayouts = 0u;
    FrameParams   p;
    MotionConfig  cfg;
    VkRenderer*   vk;
    VkRenderCreateInfo ci;
    static const char SRC[] = "int main(void)\n{\n    return 0;\n}\n";
    float bg_alpha_opaque, bg_alpha_half, glyph_alpha;

    /* Defaults are the zero-cost path: no image, full opacity. */
    config_defaults(&cfg);
    assert(cfg.background_image[0] == 0);
    assert(cfg.editor_opacity == 1.0f);
    assert(cfg.background_fit == BG_FIT_COVER);

    assert(doc_init(&d, 16u << 20));
    assert(doc_set_text(&d, SRC, (uint32_t)strlen(SRC)));
    doc_attach_syntax(&d, syntax_create_for_path("x.c"));
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *scratch);
    assert(fs && dl && scratch);

    memset(&p, 0, sizeof p);
    p.width = 800u; p.height = 600u; p.tab_width = 4u; p.line_numbers = true;
    p.motion = &cfg;

    /* Opaque: the full-screen background solid keeps alpha 1. */
    cfg.editor_opacity = 1.0f;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(dl->nsolids > 0u && dl->nglyphs > 0u);
    bg_alpha_opaque = dl->solids[0].a;
    glyph_alpha = dl->glyphs[0].a;
    assert(bg_alpha_opaque > 0.99f);

    /* Half opacity: chrome alpha halves, glyph alpha is untouched. */
    cfg.editor_opacity = 0.5f;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    bg_alpha_half = dl->solids[0].a;
    assert(bg_alpha_half > 0.49f && bg_alpha_half < 0.51f);
    assert(dl->glyphs[0].a == glyph_alpha);

    /* Out-of-range opacity is clamped, never negative. */
    cfg.editor_opacity = -3.0f;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(dl->solids[0].a == 0.0f);
    cfg.editor_opacity = 1.0f;

    /* Background image: load, render, disable, and reject a bad path. */
    write_test_png("build/test_bg.png");
    memset(&ci, 0, sizeof ci);
    ci.headless = true; ci.width = 320u; ci.height = 240u; ci.validation = true;
    vk = vk_render_create(&ci);
    assert(vk);
    assert(vk_render_set_background(vk, "build/test_bg.png", BG_FIT_COVER));
    p.width = 320u; p.height = 240u;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(vk_render_frame(vk, dl, fs));
    assert(vk_render_screenshot(vk, "build/test_appearance.png"));
    assert(vk_render_set_background(vk, NULL, BG_FIT_COVER));   /* disable */
    assert(vk_render_frame(vk, dl, fs));
    assert(!vk_render_set_background(vk, "build/does_not_exist.png", BG_FIT_COVER));
    assert(vk_render_frame(vk, dl, fs));                        /* still renders */
    vk_render_destroy(vk);

    MU_FREE(dl); MU_FREE(scratch); font_destroy(fs); doc_destroy(&d);
    puts("test_appearance: PASS");
    return 0;
}
