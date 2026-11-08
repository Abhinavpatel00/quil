#include "session.h"
#include "buffers.h"
#include <assert.h>
#include <unistd.h>
#include <limits.h>

static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    BufferManager bm;
    Document*     d;
    uint32_t      n;
    char          abs1[PATH_MAX];
    FILE*         f;
    const char*   SESS = "build/test_session";
    const char*   F1 = "build/test_sess_a.c";
    const char*   F2 = "build/test_sess_b.c";

    write_file(F1, "int a;\nint b;\nint c;\n");
    write_file(F2, "int x;\nint y;\n");

    /* Save two buffers with distinct cursor/anchor/scroll; F1 is active. */
    bm_init(&bm);
    d = bm_open(&bm, F1);
    assert(d);
    d->cursor = 7u; d->anchor = 4u; d->view_top = 1.5f; d->scroll_x = 3.0f;
    d = bm_open(&bm, F2);
    assert(d);
    d->cursor = 5u; d->anchor = 5u; d->view_top = 0.5f; d->scroll_x = 0.0f;
    bm.current = 0u;
    assert(session_save(SESS, &bm));
    bm_destroy(&bm);

    /* Restore into a fresh manager: buffers, positions and active buffer. */
    bm_init(&bm);
    n = session_load(SESS, &bm);
    assert(n == 2u && bm.count == 2u);
    assert(bm.current == 0u);
    d = bm_current(&bm);
    assert(d && strstr(d->path, "test_sess_a.c"));
    assert(d->cursor == 7u && d->anchor == 4u);
    assert(d->view_top == 1.5f && d->scroll_x == 3.0f);
    assert(d->syntax != NULL); /* Tree-sitter reattached on reopen */
    bm_destroy(&bm);

    /* A buffer whose file was deleted is skipped. */
    unlink(F2);
    bm_init(&bm);
    n = session_load(SESS, &bm);
    assert(n == 1u && bm.count == 1u);
    bm_destroy(&bm);

    /* No session file restores nothing. */
    bm_init(&bm);
    n = session_load("build/no_such_session", &bm);
    assert(n == 0u && bm.count == 0u);
    bm_destroy(&bm);

    /* Cursor/anchor past EOF are clamped to the restored text length. */
    assert(realpath(F1, abs1));
    f = fopen(SESS, "w");
    assert(f);
    fprintf(f, "quill-session 1\ncurrent %s\nbegin\npath %s\n"
               "cursor 999999\nanchor 999999\ntop 0\nscrollx 0\nend\n", abs1, abs1);
    fclose(f);
    bm_init(&bm);
    n = session_load(SESS, &bm);
    assert(n == 1u);
    d = bm_current(&bm);
    assert(d->cursor == d->q.text_len);
    assert(d->anchor == d->q.text_len);
    bm_destroy(&bm);

    unlink(F1);
    unlink(SESS);
    puts("test_session: PASS");
    return 0;
}
