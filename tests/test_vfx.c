#include "vfx.h"
#include <assert.h>

int main(void)
{
    MotionConfig cfg;
    VfxState v;
    DrawList* dl;
    float base[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
    uint32_t i;

    assert(vfx_parse_modes("railgun,ripple") == (VFX_RAILGUN | VFX_RIPPLE));
    assert(vfx_parse_modes("none") == VFX_NONE);
    assert(vfx_parse_modes("comet glitch pulse") == (VFX_COMET | VFX_GLITCH | VFX_PULSE));
    assert(strcmp(vfx_mode_name(VFX_TORPEDO), "torpedo") == 0);

    config_defaults(&cfg);
    cfg.vfx_modes = VFX_RAILGUN | VFX_RIPPLE | VFX_COMET;
    vfx_init(&v);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    assert(dl);

    /* First update establishes previous position; no trail particles yet. */
    (void)vfx_update(&v, &cfg, 0.016f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
    assert(v.trail[0].count == 0u);
    /* Move far enough to spawn. */
    assert(vfx_update(&v, &cfg, 0.016f, 80.0f, 32.0f, 8.0f, 16.0f, false, base));
    assert(v.trail[0].count > 0u);
    assert(v.trail[3].count > 0u);
    assert(v.hl[1].active);
    memset(dl, 0, sizeof *dl);
    vfx_render(&v, &cfg, dl, 8.0f, 16.0f);
    assert(dl->nvfx > 0u);

    /* Deterministic: same seed + same motion => same particle state. */
    {
        VfxState v2;
        vfx_init(&v2);
        vfx_update(&v2, &cfg, 0.016f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        vfx_update(&v2, &cfg, 0.016f, 80.0f, 32.0f, 8.0f, 16.0f, false, base);
        assert(v2.trail[0].count == v.trail[0].count);
        for (i = 0u; i < v.trail[0].count; ++i)
        {
            assert(v2.trail[0].x[i] == v.trail[0].x[i]);
            assert(v2.trail[0].life[i] == v.trail[0].life[i]);
        }
    }

    /* Lifetime expiry drains pools. */
    for (i = 0u; i < 120u; ++i)
        vfx_update(&v, &cfg, 0.016f, 80.0f, 32.0f, 8.0f, 16.0f, false, base);
    assert(!vfx_active(&v));

    /* Immediate movement suppresses trail spawn. */
    vfx_reset(&v);
    vfx_update(&v, &cfg, 0.016f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
    vfx_update(&v, &cfg, 0.016f, 800.0f, 320.0f, 8.0f, 16.0f, true, base);
    assert(v.trail[0].count == 0u);

    /* New modes: parse, name, spawn, render, drain. */
    assert(vfx_parse_modes("snow confetti spark smoke shockwave halo") ==
           (VFX_SNOW | VFX_CONFETTI | VFX_SPARK | VFX_SMOKE | VFX_SHOCKWAVE | VFX_HALO));
    assert(strcmp(vfx_mode_name(VFX_CONFETTI), "confetti") == 0);
    assert(strcmp(vfx_mode_name(VFX_HALO), "halo") == 0);
    {
        MotionConfig c2;
        VfxState nv;
        config_defaults(&c2);
        c2.vfx_modes = VFX_SNOW | VFX_CONFETTI | VFX_SPARK | VFX_SMOKE | VFX_SHOCKWAVE | VFX_HALO;
        vfx_init(&nv);
        vfx_update(&nv, &c2, 0.016f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        assert(vfx_update(&nv, &c2, 0.016f, 120.0f, 48.0f, 8.0f, 16.0f, false, base));
        assert(nv.trail[7].count > 0u);   /* snow */
        assert(nv.trail[8].count > 0u);   /* confetti */
        assert(nv.trail[9].count > 0u);   /* spark */
        assert(nv.trail[10].count > 0u);  /* smoke */
        assert(nv.hl[4].active);          /* shockwave */
        assert(nv.hl[5].active);          /* halo */
        memset(dl, 0, sizeof *dl);
        vfx_render(&nv, &c2, dl, 8.0f, 16.0f);
        assert(dl->nvfx > 0u);
        for (i = 0u; i < 200u; ++i)
            vfx_update(&nv, &c2, 0.016f, 120.0f, 48.0f, 8.0f, 16.0f, false, base);
        assert(!vfx_active(&nv));
    }

    /* Smoothness: the same elapsed time must produce the same simulation
     * whether it arrives as one 60 Hz frame or two 120 Hz frames.  Before the
     * fixed sub-stepping / exponential damping this diverged (per-frame
     * multipliers and one variable Euler step), which is what made trails
     * shimmer when the compositor changed pace. */
    {
        MotionConfig c3;
        VfxState a, b;
        uint32_t m;
        config_defaults(&c3);
        c3.vfx_modes = VFX_COMET | VFX_CONFETTI | VFX_EMBER | VFX_AURORA |
                       VFX_GLITCH | VFX_SNOW | VFX_SPARK | VFX_SMOKE;
        vfx_init(&a);
        vfx_init(&b);
        /* Establish the previous destination, then move once. */
        vfx_update(&a, &c3, 1.0f / 60.0f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        vfx_update(&b, &c3, 1.0f / 120.0f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        vfx_update(&b, &c3, 1.0f / 120.0f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        vfx_update(&a, &c3, 1.0f / 60.0f, 120.0f, 48.0f, 8.0f, 16.0f, false, base);
        vfx_update(&b, &c3, 1.0f / 120.0f, 60.0f, 24.0f, 8.0f, 16.0f, false, base);
        vfx_update(&b, &c3, 1.0f / 120.0f, 120.0f, 48.0f, 8.0f, 16.0f, false, base);
        for (i = 0u; i < 30u; ++i)
            vfx_update(&a, &c3, 1.0f / 60.0f, 120.0f, 48.0f, 8.0f, 16.0f, false, base);
        for (i = 0u; i < 60u; ++i)
            vfx_update(&b, &c3, 1.0f / 120.0f, 120.0f, 48.0f, 8.0f, 16.0f, false, base);
        for (m = 0u; m < VFX_TRAIL_COUNT; ++m)
        {
            assert(a.trail[m].count == b.trail[m].count);
            for (i = 0u; i < a.trail[m].count; ++i)
            {
                assert(a.trail[m].x[i] == b.trail[m].x[i]);
                assert(a.trail[m].y[i] == b.trail[m].y[i]);
                assert(a.trail[m].vx[i] == b.trail[m].vx[i]);
                assert(a.trail[m].vy[i] == b.trail[m].vy[i]);
                assert(a.trail[m].life[i] == b.trail[m].life[i]);
            }
        }
    }

    /* Smooth scrolling moves the destination every frame without moving the
     * logical cursor; highlights must keep expanding, not restart each frame. */
    {
        MotionConfig c4;
        VfxState h;
        float t_after_move, t_after_scroll;
        config_defaults(&c4);
        c4.vfx_modes = VFX_RIPPLE;
        vfx_init(&h);
        vfx_notify_cursor(&h, true);
        vfx_update(&h, &c4, 0.016f, 0.0f, 0.0f, 8.0f, 16.0f, false, base);
        assert(h.hl[1].active);
        vfx_notify_cursor(&h, true);
        vfx_update(&h, &c4, 0.016f, 10.0f, 0.0f, 8.0f, 16.0f, false, base);
        t_after_move = h.hl[1].t;
        assert(t_after_move > 0.0f);
        vfx_notify_cursor(&h, false);
        vfx_update(&h, &c4, 0.016f, 20.0f, 0.0f, 8.0f, 16.0f, false, base);
        t_after_scroll = h.hl[1].t;
        assert(t_after_scroll > t_after_move);
        assert(h.hl[1].cx == 20.0f + 4.0f);
    }

    MU_FREE(dl);
    puts("test_vfx: PASS");
    return 0;
}
