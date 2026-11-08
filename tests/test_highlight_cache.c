/* Highlight cache coherency: a warm hit must equal the cold result, and the
 * cache must invalidate on edit, scroll (range change), and reparse. */
#include "document.h"
#include "syntax.h"
#include <assert.h>

static int same(const HighlightSpan* a, uint32_t na, const HighlightSpan* b, uint32_t nb)
{
    uint32_t i;
    if (na != nb) return 0;
    for (i = 0u; i < na; ++i)
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].color != b[i].color)
            return 0;
    return 1;
}

static int has_color(const HighlightSpan* s, uint32_t n, uint32_t a, uint32_t b, uint8_t c)
{
    uint32_t i;
    for (i = 0u; i < n; ++i)
        if (s[i].start == a && s[i].end == b && s[i].color == c) return 1;
    return 0;
}

int main(void)
{
    Document d;
    HighlightSpan A[1024], B[1024], C[1024], E[1024];
    uint32_t na, nb, nc, ne;
    const char* code = "int foo(void) { return 0; }\n";
    uint32_t len = (uint32_t)strlen(code);

    assert(doc_init(&d, 1u << 20));
    assert(doc_set_text(&d, (char*)code, len));
    doc_attach_syntax(&d, syntax_create_for_path("cache.c"));
    assert(syntax_parse(d.syntax, &d.q));

    na = syntax_highlight(d.syntax, &d.q, 0u, len, A, 1024u);   /* cold */
    nb = syntax_highlight(d.syntax, &d.q, 0u, len, B, 1024u);   /* warm hit */
    assert(na > 0u && same(A, na, B, nb));
    assert(has_color(A, na, 4u, 7u, QC_FUNCTION));

    /* Scroll to a sub-range, then back: original range must still be correct. */
    nc = syntax_highlight(d.syntax, &d.q, 4u, len, C, 1024u);
    assert(nc > 0u);
    nb = syntax_highlight(d.syntax, &d.q, 0u, len, B, 1024u);
    assert(same(A, na, B, nb));

    /* Edit invalidates: optimistic result reflects the shift, not stale cache. */
    assert(doc_insert(&d, 0u, "/*c*/ ", 6u, 1u));
    ne = syntax_highlight(d.syntax, &d.q, 0u, len + 6u, E, 1024u);
    assert(ne > 0u);
    assert(has_color(E, ne, 10u, 13u, QC_FUNCTION));   /* foo shifted +6 */
    assert(!has_color(E, ne, 4u, 7u, QC_FUNCTION));    /* old position no longer foo */

    /* Reparse invalidates: the new comment becomes colored. */
    assert(syntax_parse(d.syntax, &d.q));
    ne = syntax_highlight(d.syntax, &d.q, 0u, len + 6u, E, 1024u);
    assert(has_color(E, ne, 0u, 5u, QC_COMMENT));

    doc_destroy(&d);
    puts("test_highlight_cache: PASS");
    return 0;
}
