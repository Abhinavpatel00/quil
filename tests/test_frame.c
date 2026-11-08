#include "frame.h"
#include <assert.h>

int main(void)
{
    Document d;
    FontSystem* fs;
    DrawList* dl;
    FrameScratch* scratch;
    LineLayout layouts[QUILL_MAX_VISIBLE];
    uint32_t nlayouts = 0u, off;
    FrameParams p;
    static const char SRC[] = "int main(void)\n{\n    return 0;\n}\n";

    assert(doc_init(&d, 16u << 20));
    assert(doc_set_text(&d, SRC, (uint32_t)strlen(SRC)));
    doc_attach_syntax(&d, syntax_create_for_path("x.c"));
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *scratch);
    assert(fs && dl && scratch);

    memset(&p, 0, sizeof p);
    p.width = 800u; p.height = 600u; p.tab_width = 4u; p.line_numbers = true;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(dl->nsolids > 0u);
    assert(dl->nglyphs > 0u);
    assert(nlayouts == 5u);
    assert(dl->overflow == 0u);

    off = frame_hit_test(&p, layouts, nlayouts, font_line_height(fs),
                         16.0f + font_advance(fs), 16.0f + font_advance(fs) * 2.0f,
                         1.0f, &d);
    assert(off <= d.q.text_len);

    doc_set_cursor(&d, 0u, false);
    doc_set_cursor(&d, 3u, true);
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(dl->nsolids > 1u);

    p.top_line = 2u;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(nlayouts == 3u);

    /* Indent guides: a line indented to two tab stops draws one rule per stop
     * (one at column 4, none at column 0), and turning them off removes them
     * again. The rules are solids on the left of the text area, so the solid
     * count is the observable difference. */
    {
        MotionConfig cfg;
        uint32_t with_guides, without_guides;
        static const char INDENTED[] = "void f(void)\n{\n        deep();\n}\n";
        config_defaults(&cfg);
        assert(cfg.indent_guides == true);
        assert(doc_set_text(&d, INDENTED, (uint32_t)strlen(INDENTED)));
        p.top_line = 0u;
        p.motion = &cfg;
        frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
        with_guides = dl->nsolids;
        cfg.indent_guides = false;
        frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
        without_guides = dl->nsolids;
        assert(with_guides > without_guides);
        /* The 8-space line draws a rule at column 4 and at column 8; the
         * other three lines are unindented and draw none. */
        assert(with_guides - without_guides == 2u);
        cfg.indent_guides = true;
        p.motion = NULL;
    }

    /* Status bar mode chip: the accent-coloured rect must exist, and the mode
     * text sits on it. Before this, `accent` never appeared in the editor. */
    {
        uint32_t i;
        bool chip = false;
        float accent[4];
        assert(doc_set_text(&d, SRC, (uint32_t)strlen(SRC)));
        p.top_line = 0u;
        frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
        quill_color_linear(QUILL_THEME.accent, accent);
        for (i = 0u; i < dl->nsolids; ++i)
        {
            const SolidQuad* q = &dl->solids[i];
            if (q->x == 0.0f && q->y > 550.0f && q->w > 30.0f && q->w < 200.0f &&
                q->r == accent[0] && q->g == accent[1] && q->b == accent[2])
                chip = true;
        }
        assert(chip);
    }

    /* Very long line: display caps at QUILL_MAX_LINE without crashing. */
    {
        char* longline = (char*)MU_MALLOC(20000u);
        assert(longline);
        memset(longline, 'a', 20000u);
        assert(doc_set_text(&d, longline, 20000u));
        MU_FREE(longline);
        p.top_line = 0u;
        frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
        assert(nlayouts == 1u);
        assert(layouts[0].len == 20000u);
    }

    MU_FREE(dl);
    MU_FREE(scratch);
    font_destroy(fs);
    doc_destroy(&d);
    puts("test_frame: PASS");
    return 0;
}
