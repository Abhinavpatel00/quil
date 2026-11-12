#include "font.h"
#include <assert.h>

int main(void)
{
    FontSystem* fs = font_create(16u, 1u);
    const ShapedGlyph *g, *g2;
    uint32_t n, n2, gen, si, ci;
    const AtlasEntry *e0, *e1;
    static const uint32_t sizes[] = { 12u, 16u, 24u };
    static const char mono_probe[] = "iIlL10mMW.:;";
    assert(fs);
    assert(font_line_height(fs) > 0.0f);
    assert(font_baseline(fs) > 0.0f);
    assert(font_advance(fs) > 0.0f);

    /* A missing preferred family must never silently substitute a
     * proportional face: narrow i/l and wide m/W would break the code grid.
     * Probe several sizes because fontconfig substitution and hinting can
     * differ by pixel size. */
    for (si = 0u; si < sizeof sizes / sizeof sizes[0]; ++si)
    {
        font_set_size(fs, sizes[si], 1u);
        for (ci = 0u; mono_probe[ci]; ++ci)
        {
            n = font_shape_line(fs, (const uint8_t*)&mono_probe[ci], 1u, 4u, &g);
            assert(n == 1u);
            assert(g[0].face == 0u);
            assert(g[0].x_advance == font_advance(fs));
        }
    }
    font_set_size(fs, 16u, 1u);

    n = font_shape_line(fs, (const uint8_t*)"int main", 8u, 4u, &g);
    assert(n == 8u);
    n2 = font_shape_line(fs, (const uint8_t*)"int main", 8u, 4u, &g2);
    assert(n2 == n && g == g2); /* cache hit */

    /* The cache key must distinguish tab width and text length. Terminal
     * cells are shaped one at a time with tab width 1, while editor text is
     * shaped with tab width 4. Hashing only the bytes made collisions
     * systematic: hash("x", tab 1) == hash("{", tab 2) == hash("}", tab 4).
     * So once a lone brace was cached, a terminal 'x' was drawn as that
     * brace -- exactly the "x looks like {" report. */
    {
        uint32_t x_glyph, lbrace_glyph, rbrace_glyph;
        n = font_shape_line(fs, (const uint8_t*)"x", 1u, 3u, &g);
        assert(n == 1u);
        x_glyph = g[0].glyph;
        n = font_shape_line(fs, (const uint8_t*)"{", 1u, 1u, &g);
        assert(n == 1u);
        lbrace_glyph = g[0].glyph;
        n = font_shape_line(fs, (const uint8_t*)"}", 1u, 4u, &g);
        assert(n == 1u);
        rbrace_glyph = g[0].glyph;
        assert(lbrace_glyph != x_glyph && rbrace_glyph != x_glyph);
        assert(lbrace_glyph != rbrace_glyph);
        /* A '{' must never inherit the cached '}' shape... */
        n = font_shape_line(fs, (const uint8_t*)"{", 1u, 2u, &g);
        assert(n == 1u);
        assert(g[0].glyph == lbrace_glyph);
        /* ...and a terminal cell must keep its own glyph. */
        n = font_shape_line(fs, (const uint8_t*)"x", 1u, 1u, &g);
        assert(n == 1u);
        assert(g[0].glyph == x_glyph);
    }

    n = font_shape_line(fs, (const uint8_t*)"a\tb", 3u, 4u, &g);
    assert(n >= 3u);
    assert(g[0].cluster == 0u);
    assert(g[n - 1u].cluster == 2u);

    e0 = font_atlas_get(fs, g[0].face, g[0].glyph, 0u);
    e1 = font_atlas_get(fs, g[0].face, g[0].glyph, 1u);
    assert(e0 && e1);
    assert(e0->w > 0u && e0->h > 0u);
    assert(font_atlas_dirty_count(fs) > 0u);
    font_atlas_clear_dirty(fs);
    assert(font_atlas_dirty_count(fs) == 0u);

    gen = font_atlas_generation(fs);
    font_set_size(fs, 24u, 1u);
    assert(font_atlas_generation(fs) != gen);
    assert(font_size(fs) == 24u);

    /* Unicode fallback path must not crash. */
    n = font_shape_line(fs, (const uint8_t*)"\xCE\xB1\xCE\xB2 \xF0\x9F\x98\x80", 9u, 4u, &g);
    assert(n > 0u);

    font_destroy(fs);
    puts("test_font: PASS");
    return 0;
}
