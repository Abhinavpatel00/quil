#include "document.h"
#include "syntax.h"
#include <assert.h>

static int compare_spans(const HighlightSpan* a, uint32_t na,
                         const HighlightSpan* b, uint32_t nb)
{
    uint32_t i;
    if (na != nb) return 0;
    for (i = 0u; i < na; ++i)
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].color != b[i].color)
            return 0;
    return 1;
}

int main(void)
{
    Document d;
    char* big;
    uint32_t n = 0u, i;
    HighlightSpan inc[4096], full[4096];
    uint32_t ni, nf;

    assert(doc_init(&d, 64u << 20));
    big = (char*)MU_MALLOC(1u << 20);
    assert(big);
    for (i = 0u; i < 20000u; ++i)
        n += (uint32_t)snprintf(big + n, (1u << 20) - n, "int v%u = %u;\n", i, i);
    assert(doc_set_text(&d, big, n));
    MU_FREE(big);
    doc_attach_syntax(&d, syntax_create_for_path("big.c"));
    assert(d.syntax);
    assert(syntax_parse(d.syntax, &d.q));

    /* Edit near a chunk boundary, then incremental reparse. */
    assert(doc_insert(&d, 16384u, "/*x*/", 5u, 1u));
    assert(syntax_parse(d.syntax, &d.q));
    ni = syntax_highlight(d.syntax, &d.q, 16000u, 17000u, inc, 4096u);

    syntax_reset(d.syntax);
    assert(syntax_parse(d.syntax, &d.q));
    nf = syntax_highlight(d.syntax, &d.q, 16000u, 17000u, full, 4096u);
    assert(ni > 0u);
    assert(compare_spans(inc, ni, full, nf));

    /* Undo restores content and incremental tree stays consistent. */
    assert(doc_undo(&d));
    assert(syntax_parse(d.syntax, &d.q));
    ni = syntax_highlight(d.syntax, &d.q, 16000u, 17000u, inc, 4096u);
    syntax_reset(d.syntax);
    assert(syntax_parse(d.syntax, &d.q));
    nf = syntax_highlight(d.syntax, &d.q, 16000u, 17000u, full, 4096u);
    assert(compare_spans(inc, ni, full, nf));

    doc_destroy(&d);

    /* Stale tree must not paint: an edit invalidates highlights until reparse. */
    {
        Document s;
        HighlightSpan spans[256];
        uint32_t ns, i;
        const char* code = "int foo(void) { return 0; }\n";
        assert(doc_init(&s, 1u << 20));
        assert(doc_set_text(&s, code, (uint32_t)strlen(code)));
        doc_attach_syntax(&s, syntax_create_for_path("tie.c"));
        assert(s.syntax);
        assert(syntax_parse(s.syntax, &s.q));
        ns = syntax_highlight(s.syntax, &s.q, 0u, (uint32_t)strlen(code), spans, 256u);
        assert(ns > 0u);
        for (i = 0u; i < ns; ++i)
            if (spans[i].start == 4u && spans[i].end == 7u)
                assert(spans[i].color == QC_FUNCTION);
        /* Optimistic: an edit must NOT blank highlights. The edit-adjusted
         * tree keeps existing tokens colored (shifted) until reparse. */
        assert(doc_insert(&s, 0u, "/*c*/ ", 6u, 1u));
        ns = syntax_highlight(s.syntax, &s.q, 0u, 64u, spans, 256u);
        assert(ns > 0u);
        for (i = 0u; i < ns; ++i)
            if (spans[i].start == 10u && spans[i].end == 13u)
                assert(spans[i].color == QC_FUNCTION); /* foo shifted +6 */
        assert(syntax_parse(s.syntax, &s.q));
        ns = syntax_highlight(s.syntax, &s.q, 0u, 64u, spans, 256u);
        assert(ns > 0u);
        doc_destroy(&s);
    }

    /* Language registry + queries. */
    {
        static const char* paths[] = { "a.c", "a.h", "a.rs", "a.py", "a.json", "a.js" };
        for (i = 0u; i < 6u; ++i)
        {
            Syntax* s = syntax_create_for_path(paths[i]);
            assert(s);
            syntax_destroy(s);
        }
    }
    puts("test_syntax: PASS");
    return 0;
}
