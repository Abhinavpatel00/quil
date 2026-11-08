#include "picker.h"
#include "buffers.h"
#include "config.h"
#include <assert.h>
#include <GLFW/glfw3.h>
#include <unistd.h>

int main(void)
{
    Picker p;
    BufferManager bm;
    MotionConfig cfg;
    Document* d;
    FontSystem* fs;
    DrawList* dl;
    uint32_t i;
    bool panel_opaque = false;

    assert(picker_fuzzy_score("abc", "a_b_c") >= 0);
    assert(picker_fuzzy_score("abc", "acb") < 0);
    assert(picker_fuzzy_score("", "anything") == 0);

    config_defaults(&cfg);
    assert(cfg.scroll_ms == 300u);
    assert(cfg.cursor_ms == 150u);
    assert(cfg.trail_size == 1.0f);

    bm_init(&bm);
    d = bm_open(&bm, "assets/sample.c");
    assert(d && bm.count == 1u);
    assert(bm_current(&bm) == d);
    assert(bm.nrecent >= 1u);

    picker_init(&p, d, ".");
    picker_open(&p, PSRC_COMMANDS);
    assert(p.open && p.count > 0u);

    /* The palette stays opaque even when editor chrome is translucent. */
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    assert(fs && dl);
    picker_render(&p, fs, dl, 1000u, 600u, 0.35f);
    for (i = 0u; i < dl->nsolids; ++i)
    {
        const SolidQuad* s = &dl->solids[i];
        if (s->w > 800.0f && s->w < 900.0f && s->h > 450.0f && s->h < 550.0f && s->a > 0.99f)
            panel_opaque = true;
    }
    assert(panel_opaque);

    /* Closing must clear the overlay layer. render_app() only calls
     * frame_overlay_begin() while something is open, so the overlay counters
     * have to be reset by frame_build(); otherwise the renderer keeps drawing
     * the last picker frame after p.open goes false. */
    {
        FrameScratch* scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *scratch);
        LineLayout layouts[QUILL_MAX_VISIBLE];
        uint32_t nlayouts = 0u;
        FrameParams fp;
        assert(scratch);
        memset(&fp, 0, sizeof fp);
        fp.width = 1000u; fp.height = 600u; fp.tab_width = 4u; fp.line_numbers = true;
        frame_overlay_begin(dl);
        picker_render(&p, fs, dl, fp.width, fp.height, 0.35f);
        frame_overlay_end(dl);
        assert(dl->nov_solids > 0u);
        assert(dl->nov_glyphs > 0u);
        picker_close(&p);
        frame_build(d, fs, &fp, dl, scratch, layouts, &nlayouts);
        assert(dl->nov_solids == 0u);
        assert(dl->nov_glyphs == 0u);
        MU_FREE(scratch);
    }
    MU_FREE(dl);
    font_destroy(fs);

    picker_open(&p, PSRC_COMMANDS);
    assert(picker_key(&p, GLFW_KEY_DOWN, 0));
    assert(p.selected == 1u);
    assert(picker_key(&p, GLFW_KEY_ENTER, 0));
    assert(!p.open && p.accepted);

    picker_open(&p, PSRC_FILES);
    for (i = 0u; i < 200u && p.count == 0u; ++i)
    {
        picker_poll(&p);
        usleep(10000);
    }
    picker_poll(&p);
    assert(p.count > 0u);
    picker_char(&p, 'm');
    for (i = 0u; i < 200u; ++i)
    {
        picker_poll(&p);
        usleep(10000);
    }
    picker_close(&p);
    picker_destroy(&p);
    bm_destroy(&bm);
    puts("test_picker: PASS");
    return 0;
}
