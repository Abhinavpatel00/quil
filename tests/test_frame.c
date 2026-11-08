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
