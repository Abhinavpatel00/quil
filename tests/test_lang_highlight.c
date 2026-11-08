/* Cross-language semantic correctness regression. Guards against the
 * ignored-predicate bug class: a #match?/#eq? pattern the vendored Tree-sitter
 * API does not evaluate would capture EVERY identifier and mis-color the whole
 * language. Asserts variables stay variables, constants/types/functions land
 * correctly, in Rust, Python, JavaScript, and JSON. */
#include "document.h"
#include "syntax.h"
#include <assert.h>

typedef struct { const char* src; HighlightSpan sp[4096]; uint32_t n; } Ctx;

static void load(Ctx* c, const char* path, const char* code)
{
    Document d;
    c->src = code;
    assert(doc_init(&d, 1u << 20));
    assert(doc_set_text(&d, (char*)code, (uint32_t)strlen(code)));
    doc_attach_syntax(&d, syntax_create_for_path(path));
    assert(d.syntax);
    assert(syntax_parse(d.syntax, &d.q));
    c->n = syntax_highlight(d.syntax, &d.q, 0u, (uint32_t)strlen(code), c->sp, 4096u);
    assert(c->n > 0u);                 /* query compiled and produced spans */
    doc_destroy(&d);
}

static uint32_t nth_off(const Ctx* c, const char* tok, int occ)
{
    const char* p = c->src;
    int k = 0;
    while ((p = strstr(p, tok)) != NULL)
    {
        if (k == occ) return (uint32_t)(p - c->src);
        ++k; p += 1;
    }
    fprintf(stderr, "token '%s' occ %d not found\n", tok, occ);
    assert(false);
    return 0u;
}

static void expect(const Ctx* c, const char* tok, int occ, uint8_t color)
{
    uint32_t off = nth_off(c, tok, occ);
    uint32_t len = (uint32_t)strlen(tok);
    uint32_t i;
    for (i = 0u; i < c->n; ++i)
        if (c->sp[i].start == off && c->sp[i].end == off + len)
        {
            if (c->sp[i].color != color)
                fprintf(stderr, "'%s' occ %d: want %u got %u\n", tok, occ, color, c->sp[i].color);
            assert(c->sp[i].color == color);
            return;
        }
    fprintf(stderr, "no span for '%s' occ %d\n", tok, occ);
    assert(false);
}

int main(void)
{
    static Ctx c;

    load(&c, "a.rs",
        "const MAX: i32 = 10;\nstruct Point { x: f64 }\nenum Color { Red, Green }\n"
        "fn main() {\n    let count = 0;\n    let p = Point { x: 1.0 };\n"
        "    println!(\"{}\", count);\n}\n");
    expect(&c, "MAX", 0, QC_CONSTANT);
    expect(&c, "i32", 0, QC_TYPE);
    expect(&c, "f64", 0, QC_TYPE);
    expect(&c, "Point", 0, QC_TYPE);
    expect(&c, "Color", 0, QC_TYPE);
    expect(&c, "Red", 0, QC_CONSTANT);
    expect(&c, "Green", 0, QC_CONSTANT);
    expect(&c, "count", 0, QC_VARIABLE);
    expect(&c, "p", 0, QC_VARIABLE);
    expect(&c, "main", 0, QC_FUNCTION);
    expect(&c, "println", 0, QC_FUNCTION);
    expect(&c, "10", 0, QC_NUMBER);
    expect(&c, "1.0", 0, QC_NUMBER);

    load(&c, "a.py",
        "MAX = 10\nclass Point:\n    def __init__(self, x):\n        self.x = x\n"
        "def main():\n    count = 0\n    p = Point(1)\n    print(count)\n");
    expect(&c, "MAX", 0, QC_CONSTANT);
    expect(&c, "Point", 0, QC_TYPE);
    expect(&c, "self", 0, QC_VARIABLE);
    expect(&c, "x", 0, QC_VARIABLE);
    expect(&c, "count", 0, QC_VARIABLE);
    expect(&c, "p", 0, QC_VARIABLE);
    expect(&c, "__init__", 0, QC_FUNCTION);
    expect(&c, "main", 0, QC_FUNCTION);
    expect(&c, "print", 0, QC_FUNCTION);
    expect(&c, "10", 0, QC_NUMBER);

    load(&c, "a.js",
        "const MAX = 10;\nclass Point { constructor(x) { this.x = x; } }\n"
        "function main() {\n  let count = 0;\n  const p = new Point(1);\n"
        "  console.log(count);\n}\n");
    expect(&c, "MAX", 0, QC_CONSTANT);
    expect(&c, "Point", 0, QC_TYPE);
    expect(&c, "count", 0, QC_VARIABLE);
    expect(&c, "p", 0, QC_VARIABLE);
    expect(&c, "console", 0, QC_VARIABLE);
    expect(&c, "main", 0, QC_FUNCTION);
    expect(&c, "log", 0, QC_FUNCTION);
    expect(&c, "10", 0, QC_NUMBER);

    load(&c, "a.json", "{ \"name\": \"quill\", \"count\": 3, \"ok\": true }\n");
    expect(&c, "\"quill\"", 0, QC_STRING);
    expect(&c, "3", 0, QC_NUMBER);
    expect(&c, "true", 0, QC_CONSTANT);

    puts("test_lang_highlight: PASS");
    return 0;
}
