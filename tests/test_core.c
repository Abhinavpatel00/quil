#include "document.h"
#include <assert.h>

int main(void)
{
    Document d;
    uint64_t now = 1000u;
    assert(doc_init(&d, 16u << 20));
    assert(doc_set_text(&d, "hello\nworld\n", 12u));
    assert(d.q.text_len == 12u);
    assert(d.q.nlines == 3u);

    doc_set_cursor(&d, 5u, false);
    assert(doc_insert(&d, d.cursor, "!!", 2u, now));
    assert(d.q.text_len == 14u);
    assert(d.cursor == 7u);
    assert(doc_undo(&d));
    assert(d.q.text_len == 12u);
    assert(doc_redo(&d));
    assert(d.q.text_len == 14u);

    /* coalesced typing */
    doc_set_cursor(&d, 0u, false);
    assert(doc_insert(&d, 0u, "a", 1u, now));
    assert(doc_insert(&d, 1u, "b", 1u, now + 10u));
    assert(doc_insert(&d, 2u, "c", 1u, now + 20u));
    assert(doc_undo(&d));
    assert(d.q.text_len == 14u);

    /* selection replace */
    doc_set_cursor(&d, 0u, false);
    doc_set_cursor(&d, 5u, true);
    assert(doc_replace(&d, 0u, 5u, "XYZ", 3u, now + 1000u));
    assert(d.q.text_len == 12u);
    assert(doc_undo(&d));
    assert(d.q.text_len == 14u);

    /* UTF-8 movement */
    assert(doc_set_text(&d, "a\xCE\xB1z\n", 5u));
    doc_set_cursor(&d, 1u, false);
    doc_move_col(&d, 1, false);
    assert(d.cursor == 3u);
    doc_move_col(&d, -1, false);
    assert(d.cursor == 1u);

    /* save/load */
    assert(doc_save_file(&d, "build/test_core.txt"));
    {
        Document e;
        assert(doc_init(&e, 16u << 20));
        assert(doc_load_file(&e, "build/test_core.txt"));
        assert(e.q.text_len == d.q.text_len);
        doc_destroy(&e);
    }

    /* view API smoke through editor buffer */
    {
        mu_quill_view v;
        mu_quill_line l;
        assert(mu_quill_view_start(&d.q, 0u, &v));
        assert(mu_quill_view_next(&d.q, &v, &l));
        assert(l.offset == 0u && l.len == 4u);
    }

    doc_destroy(&d);
    puts("test_core: PASS");
    return 0;
}
