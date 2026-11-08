/* Semantic correctness regression for C highlighting. Asserts the effective
 * color of specific tokens, including the ALL_CAPS macro-use heuristic and
 * function/type/constant captures that previously mis-colored. */
#include "document.h"
#include "syntax.h"
#include <assert.h>

static const char* SAMPLE =
"#include <stdio.h>\n"
"#include \"local.h\"\n"
"#define MAX 100\n"
"#define SQ(x) ((x)*(x))\n"
"typedef struct Point { float x, y; } Point;\n"
"enum Color { RED, GREEN, BLUE };\n"
"static int counter = 0;\n"
"int *make_point(float x, float y);\n"
"int main(void) {\n"
"    Point p = { 1.0f, 2.5f };\n"
"    Point *ptr = &p;\n"
"    ptr->x = MAX;\n"
"    int arr[3] = { 0x1F, 077, 42 };\n"
"    char c = '\\n';\n"
"    const char *msg = \"hello\\tworld\";\n"
"    for (int i = 0; i < 10; ++i) {\n"
"        counter += SQ(i);\n"
"    }\n"
"    if (ptr != NULL) goto done;\n"
"done:\n"
"    printf(\"%s %d\\n\", msg, counter);\n"
"    return sizeof(Point);\n"
"}\n";

static HighlightSpan SP[4096];
static uint32_t NSP;

static uint32_t nth_off(const char* tok, int n)
{
    const char* p = SAMPLE;
    int k = 0;
    while ((p = strstr(p, tok)) != NULL)
    {
        if (k == n) return (uint32_t)(p - SAMPLE);
        ++k;
        p += 1;
    }
    assert(!"token not found");
    return 0u;
}

static void expect(const char* tok, int occ, uint8_t color)
{
    uint32_t off = nth_off(tok, occ);
    uint32_t len = (uint32_t)strlen(tok);
    uint32_t i;
    for (i = 0u; i < NSP; ++i)
        if (SP[i].start == off && SP[i].end == off + len)
        {
            if (SP[i].color != color)
                fprintf(stderr, "token '%s' occ %d: want color %u got %u\n",
                        tok, occ, color, SP[i].color);
            assert(SP[i].color == color);
            return;
        }
    fprintf(stderr, "no span for '%s' occ %d at %u\n", tok, occ, off);
    assert(false);
}

int main(void)
{
    Document d;
    assert(doc_init(&d, 1u << 20));
    assert(doc_set_text(&d, (char*)SAMPLE, (uint32_t)strlen(SAMPLE)));
    doc_attach_syntax(&d, syntax_create_for_path("c_highlight.c"));
    assert(d.syntax);
    assert(syntax_parse(d.syntax, &d.q));
    NSP = syntax_highlight(d.syntax, &d.q, 0u, (uint32_t)strlen(SAMPLE), SP, 4096u);
    assert(NSP > 0u);

    /* Preprocessor + macros */
    expect("#include", 0, QC_KEYWORD);
    expect("<stdio.h>", 0, QC_STRING);
    expect("\"local.h\"", 0, QC_STRING);
    expect("#define", 0, QC_KEYWORD);
    expect("MAX", 0, QC_CONSTANT);          /* definition */
    expect("MAX", 1, QC_CONSTANT);          /* USE site — the heuristic fix */
    expect("SQ", 0, QC_FUNCTION);           /* function-like macro name */
    expect("SQ", 1, QC_FUNCTION);           /* macro call */

    /* Types */
    expect("Point", 0, QC_TYPE);            /* struct tag */
    expect("Point", 1, QC_TYPE);            /* typedef name */
    expect("float", 0, QC_TYPE);
    expect("void", 0, QC_TYPE);
    expect("Color", 0, QC_TYPE);            /* enum tag */

    /* Enum constants + NULL */
    expect("RED", 0, QC_CONSTANT);
    expect("GREEN", 0, QC_CONSTANT);
    expect("BLUE", 0, QC_CONSTANT);
    expect("NULL", 0, QC_CONSTANT);

    /* Functions: definition, declaration, calls */
    expect("make_point", 0, QC_FUNCTION);
    expect("main", 0, QC_FUNCTION);
    expect("printf", 0, QC_FUNCTION);

    /* Variables vs properties */
    expect("counter", 0, QC_VARIABLE);
    expect("ptr", 0, QC_VARIABLE);
    expect("msg", 0, QC_VARIABLE);

    /* Keywords / operators / numbers / strings / escapes / labels */
    expect("typedef", 0, QC_KEYWORD);
    expect("sizeof", 0, QC_KEYWORD);
    expect("return", 0, QC_KEYWORD);
    expect("->", 0, QC_OPERATOR);
    expect("+=", 0, QC_OPERATOR);
    expect("++", 0, QC_OPERATOR);
    expect("0x1F", 0, QC_NUMBER);
    expect("1.0f", 0, QC_NUMBER);
    expect("42", 0, QC_NUMBER);
    expect("\"hello\\tworld\"", 0, QC_STRING);
    expect("\\t", 0, QC_ESCAPE);
    expect("done", 0, QC_LABEL);

    /* Optimistic stability: an edit must not blank, and a function token must
     * keep its color after the edit-adjusted shift (before reparse). */
    {
        uint32_t off = nth_off("main", 0);
        uint32_t i;
        int found = 0;
        assert(doc_insert(&d, 0u, "/*x*/ ", 6u, 1u));
        NSP = syntax_highlight(d.syntax, &d.q, 0u, (uint32_t)d.q.text_len, SP, 4096u);
        assert(NSP > 0u);
        for (i = 0u; i < NSP; ++i)
            if (SP[i].start == off + 6u && SP[i].end == off + 6u + 4u)
            {
                assert(SP[i].color == QC_FUNCTION);
                found = 1;
            }
        assert(found);
    }

    doc_destroy(&d);
    puts("test_c_highlight: PASS");
    return 0;
}
