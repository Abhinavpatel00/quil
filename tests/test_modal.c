#include "modal.h"
#include "search.h"
#include <assert.h>
#include <GLFW/glfw3.h>

static Document d;
static SelectionSet ss;
static ModalState m;
static uint64_t now = 1000u;

static void setup(const char* text)
{
    doc_destroy(&d);
    assert(doc_init(&d, 1u << 20));
    assert(doc_set_text(&d, text, (uint32_t)strlen(text)));
    ss_init(&ss);
    ss_single(&ss, 0u);
    modal_init(&m);
    now += 100u;
}

static void key(int k, int mods)
{
    now += 10u;
    modal_key(&m, &d, &ss, k, mods, now);
}

static void ch(unsigned cp)
{
    now += 10u;
    modal_char(&m, &d, &ss, cp, now);
}

static void text_is(const char* want)
{
    char buf[4096];
    uint32_t n = d.q.text_len;
    assert(n < sizeof buf);
    mu_quill_text(&d.q, 0u, n, buf);
    buf[n] = 0;
    if (strcmp(buf, want) != 0)
    {
        fprintf(stderr, "want: %s\ngot:  %s\n", want, buf);
        assert(false);
    }
}

static int key_for(char c)
{
    if (c >= 'a' && c <= 'z') return GLFW_KEY_A + (c - 'a');
    if (c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
    if (c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
    switch (c)
    {
    case ' ': return GLFW_KEY_SPACE;
    case '.': return GLFW_KEY_PERIOD;
    case ',': return GLFW_KEY_COMMA;
    case ';': return GLFW_KEY_SEMICOLON;
    case '/': return GLFW_KEY_SLASH;
    case '"': return GLFW_KEY_APOSTROPHE;
    case '\'': return GLFW_KEY_APOSTROPHE;
    case '-': return GLFW_KEY_MINUS;
    case '=': return GLFW_KEY_EQUAL;
    case '[': return GLFW_KEY_LEFT_BRACKET;
    case ']': return GLFW_KEY_RIGHT_BRACKET;
    case '\\': return GLFW_KEY_BACKSLASH;
    case '`': return GLFW_KEY_GRAVE_ACCENT;
    case '~': return GLFW_KEY_GRAVE_ACCENT;
    case '*': return GLFW_KEY_8;
    case '#': return GLFW_KEY_3;
    case '$': return GLFW_KEY_4;
    case '^': return GLFW_KEY_6;
    default: return 0;
    }
}

static void feed(const char* s)
{
    uint32_t i;
    for (i = 0u; s[i]; ++i)
    {
        char c = s[i];
        int mods = 0;
        int k = key_for(c);
        if ((c >= 'A' && c <= 'Z') || c == '~' || c == '*' || c == '#' || c == '$' || c == '^' || c == '"')
            mods = GLFW_MOD_SHIFT;
        if (c == 27)
        {
            key(GLFW_KEY_ESCAPE, 0);
            continue;
        }
        if (m.mode == QMODE_INSERT)
        {
            ch((unsigned char)c);
            continue;
        }
        if (k) key(k, mods);
    }
}

int main(void)
{
    assert(doc_init(&d, 1u << 20));

    setup("alpha beta gamma\nalpha beta\n");
    feed("iX\x1b");
    text_is("Xalpha beta gamma\nalpha beta\n");
    key(GLFW_KEY_U, 0);
    text_is("alpha beta gamma\nalpha beta\n");

    setup("alpha beta gamma\nalpha beta\n");
    feed("dd");
    text_is("alpha beta\n");
    key(GLFW_KEY_U, 0);
    text_is("alpha beta gamma\nalpha beta\n");

    setup("alpha beta gamma\nalpha beta\n");
    feed("yyP");
    text_is("alpha beta gamma\nalpha beta gamma\nalpha beta\n");

    setup("alpha beta gamma\n");
    feed("dw");
    text_is("beta gamma\n");

    setup("alpha beta gamma\n");
    feed("3l");
    assert(ss.c[0].head == 3u);
    feed("x");
    text_is("alpa beta gamma\n");

    setup("alpha beta gamma\n");
    feed("2dw");
    text_is("gamma\n");

    setup("abcdef\n");
    feed("vllld");
    text_is("ef\n");

    setup("foo foo foo\n");
    key(GLFW_KEY_N, GLFW_MOD_CONTROL);
    assert(ss.count == 1u);
    assert(ss_max(&ss.c[0]) == 3u);
    key(GLFW_KEY_N, GLFW_MOD_CONTROL);
    assert(ss.count == 2u);
    feed("iX\x1b");
    text_is("X X foo\n");
    key(GLFW_KEY_U, 0);
    text_is("foo foo foo\n");

    setup("hello world\n");
    ss_single(&ss, 7u);
    feed("diw");
    text_is("hello \n");

    setup("one two three\n");
    modal_search_set(&m, "two", 1);
    assert(modal_search_next(&m, &d, &ss, 1));
    assert(ss.c[0].head == 4u);

    setup("a\nb\nc\n");
    feed("j");
    assert(mu_quill_offset_to_cursor(&d.q, ss.c[0].head).line == 1u);
    feed("k");
    assert(mu_quill_offset_to_cursor(&d.q, ss.c[0].head).line == 0u);
    feed("G");
    assert(ss.c[0].head == d.q.text_len);
    feed("gg");
    assert(ss.c[0].head == 0u);

    /* Arrow keys wrap across line boundaries instead of sticking at EOL. */
    setup("ab\ncd\n");
    key(GLFW_KEY_RIGHT, 0);
    assert(ss.c[0].head == 1u);   /* last character of the line */
    key(GLFW_KEY_RIGHT, 0);
    assert(ss.c[0].head == 3u);   /* straight on to the next line */
    assert(mu_quill_offset_to_cursor(&d.q, ss.c[0].head).line == 1u);
    key(GLFW_KEY_LEFT, 0);
    assert(ss.c[0].head == 2u);   /* back to the previous line's end */
    key(GLFW_KEY_LEFT, 0);
    assert(ss.c[0].head == 1u);

    /* The same wrapping applies in INSERT mode. */
    setup("ab\ncd\n");
    feed("i");
    assert(m.mode == QMODE_INSERT);
    key(GLFW_KEY_RIGHT, 0);
    key(GLFW_KEY_RIGHT, 0);
    key(GLFW_KEY_RIGHT, 0);
    assert(ss.c[0].head == 3u);
    assert(mu_quill_offset_to_cursor(&d.q, ss.c[0].head).line == 1u);
    key(GLFW_KEY_LEFT, 0);
    assert(ss.c[0].head == 2u);

    /* Layout-produced characters drive commands, not physical keycodes. */
    setup("abc\n");
    now += 10u;
    assert(modal_printable(&m, &d, &ss, ':', now));
    assert(m.request == QREQ_PICKER_COMMANDS);
    m.request = QREQ_NONE;
    now += 10u;
    assert(modal_printable(&m, &d, &ss, 'i', now));
    assert(m.mode == QMODE_INSERT);
    now += 10u;
    assert(modal_printable(&m, &d, &ss, 'x', now));
    text_is("xabc\n");
    now += 10u;
    assert(modal_printable(&m, &d, &ss, 0x00E9u, now)); /* non-ASCII insert */
    text_is("x\xc3\xa9" "abc\n");

    doc_destroy(&d);
    puts("test_modal: PASS");
    return 0;
}
