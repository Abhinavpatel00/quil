#include "frame.h"
#include <assert.h>

int main(void)
{
    Document d;
    FontSystem* fs;
    DrawList* dl;
    FrameScratch* sc;
    LineLayout layouts[QUILL_MAX_VISIBLE];
    uint32_t nl = 0u;
    FrameParams p;
    SelectionSet ss;
    ModalState m;
    MotionConfig cfg;
    CursorAnim ca;
    bool anim = false;
    float x0, y0, lh;
    uint32_t i;
    char text[4096];
    uint32_t len = 0u;

    for (i = 0u; i < 100u; ++i)
        len += (uint32_t)snprintf(text + len, sizeof text - len, "hello world %u\n", i);
    assert(doc_init(&d, 1u << 20));
    assert(doc_set_text(&d, text, len));
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    sc = (FrameScratch*)MU_CALLOC(1u, sizeof *sc);
    assert(fs && dl && sc);
    config_defaults(&cfg);
    ss_init(&ss);
    ss_single(&ss, 0u);
    modal_init(&m);
    memset(&ca, 0, sizeof ca);
    memset(&p, 0, sizeof p);
    p.width = 800u; p.height = 600u; p.tab_width = 4u;
    p.selections = &ss;
    p.modal = &m;
    p.motion = &cfg;
    p.cursor_anim = &ca;
    p.animating = &anim;
    p.blink_alpha = 1.0f;
    p.now_ms = 1000u;
    lh = font_line_height(fs);

    frame_build(&d, fs, &p, dl, sc, layouts, &nl);
    assert(ca.valid);
    x0 = ca.x; y0 = ca.y;

    ss_single(&ss, mu_quill_cursor_to_offset(&d.q, 10u, 6u));
    p.now_ms += 16u;
    anim = false;
    frame_build(&d, fs, &p, dl, sc, layouts, &nl);
    assert(anim);
    assert(ca.y > y0 + 0.1f);
    assert(ca.y < y0 + 10.0f * lh - 0.1f);

    for (i = 0u; i < 120u; ++i)
    {
        p.now_ms += 16u;
        anim = false;
        frame_build(&d, fs, &p, dl, sc, layouts, &nl);
    }
    assert(!anim);
    assert(fabsf(ca.y - (y0 + 10.0f * lh)) < 0.2f);
    assert(fabsf(ca.x - x0) > 0.1f);

    cfg.reduced_motion = true;
    ss_single(&ss, 0u);
    p.now_ms += 16u;
    anim = false;
    frame_build(&d, fs, &p, dl, sc, layouts, &nl);
    assert(!anim);
    assert(fabsf(ca.y - y0) < 0.2f);

    {
        float x = 0.0f, v = 0.0f;
        for (i = 0u; i < 10u; ++i) quill_spring_step(&x, &v, 100.0f, 0.016f, 150.0f);
        assert(x > 0.0f && x < 100.0f);
        for (i = 0u; i < 120u; ++i) quill_spring_step(&x, &v, 100.0f, 0.016f, 150.0f);
        assert(fabsf(x - 100.0f) < 0.1f);
        quill_spring_step(&x, &v, 42.0f, 0.016f, 0.0f);
        assert(x == 42.0f && v == 0.0f);
    }

    MU_FREE(dl); MU_FREE(sc); font_destroy(fs); doc_destroy(&d);
    puts("test_animation: PASS");
    return 0;
}
