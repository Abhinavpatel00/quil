#include "selection_set.h"
#include "search.h"

void ss_init(SelectionSet* ss)
{
    memset(ss, 0, sizeof *ss);
    ss->count = 1u;
}

void ss_single(SelectionSet* ss, uint32_t off)
{
    memset(ss, 0, sizeof *ss);
    ss->count = 1u;
    ss->c[0].anchor = ss->c[0].head = off;
}

void ss_from_doc(SelectionSet* ss, const Document* d)
{
    ss_single(ss, d->cursor);
    ss->c[0].anchor = d->anchor;
}

void ss_sync_doc(const SelectionSet* ss, Document* d)
{
    const SelCursor* p = &ss->c[ss->primary < ss->count ? ss->primary : 0u];
    d->anchor = p->anchor;
    d->cursor = p->head;
    d->prefer_col = p->prefer_col;
}

uint32_t ss_min(const SelCursor* c) { return c->anchor < c->head ? c->anchor : c->head; }
uint32_t ss_max(const SelCursor* c) { return c->anchor > c->head ? c->anchor : c->head; }

void ss_normalize(SelectionSet* ss)
{
    uint32_t i, j, n;
    if (ss->count == 0u) { ss_single(ss, 0u); return; }
    for (i = 1u; i < ss->count; ++i)
    {
        SelCursor t = ss->c[i];
        j = i;
        while (j > 0u && ss_min(&ss->c[j - 1u]) > ss_min(&t))
        {
            ss->c[j] = ss->c[j - 1u];
            --j;
        }
        ss->c[j] = t;
    }
    n = 0u;
    for (i = 0u; i < ss->count; ++i)
    {
        if (n > 0u && ss_min(&ss->c[i]) <= ss_max(&ss->c[n - 1u]))
        {
            uint32_t a = ss_min(&ss->c[n - 1u]);
            uint32_t b = ss_max(&ss->c[n - 1u]);
            if (ss_min(&ss->c[i]) < a) a = ss_min(&ss->c[i]);
            if (ss_max(&ss->c[i]) > b) b = ss_max(&ss->c[i]);
            ss->c[n - 1u].anchor = a;
            ss->c[n - 1u].head = b;
        }
        else
        {
            ss->c[n++] = ss->c[i];
        }
    }
    ss->count = n;
    ss->primary = 0u;
}

void ss_add(SelectionSet* ss, uint32_t anchor, uint32_t head)
{
    if (ss->count >= QUILL_MAX_CURSORS) return;
    ss->c[ss->count].anchor = anchor;
    ss->c[ss->count].head = head;
    ss->c[ss->count].prefer_col = 0u;
    ss->c[ss->count].anim_valid = false;
    ss->count++;
    ss_normalize(ss);
}

void ss_remove_last(SelectionSet* ss)
{
    if (ss->count > 1u)
        ss->count--;
    ss->primary = 0u;
}

void ss_collapse_primary(SelectionSet* ss)
{
    SelCursor p = ss->c[ss->primary < ss->count ? ss->primary : 0u];
    ss_single(ss, p.head);
}

SelCursor* ss_primary(SelectionSet* ss)
{
    return &ss->c[ss->primary < ss->count ? ss->primary : 0u];
}

const SelCursor* ss_primary_const(const SelectionSet* ss)
{
    return &ss->c[ss->primary < ss->count ? ss->primary : 0u];
}

void ss_word_at(SelectionSet* ss, Document* d, uint32_t off)
{
    uint32_t a, b;
    quill_word_range(d, off, &a, &b);
    ss_single(ss, b);
    ss->c[0].anchor = a;
    ss_sync_doc(ss, d);
}

bool ss_add_next(SelectionSet* ss, Document* d)
{
    SelCursor* p = ss_primary(ss);
    uint32_t a = ss_min(p), b = ss_max(p);
    char query[256];
    uint32_t len = b - a, s, e;
    if (len == 0u)
    {
        ss_word_at(ss, d, p->head);
        return true;
    }
    if (len >= sizeof query) return false;
    mu_quill_text(&d->q, a, len, query);
    query[len] = 0;
    if (!quill_find(d, query, b, 1, &s, &e))
        if (!quill_find(d, query, 0u, 1, &s, &e))
            return false;
    if (s == a && e == b) return false;
    ss_add(ss, s, e);
    ss_sync_doc(ss, d);
    return true;
}

bool ss_add_all(SelectionSet* ss, Document* d, uint32_t range_start, uint32_t range_end)
{
    SelCursor* p = ss_primary(ss);
    uint32_t a = ss_min(p), b = ss_max(p);
    char query[256];
    uint32_t len = b - a;
    SearchRange ranges[QUILL_MAX_CURSORS];
    uint32_t n, i;
    if (range_end == 0u) range_end = d->q.text_len;
    if (len == 0u)
    {
        quill_word_range(d, p->head, &a, &b);
        len = b - a;
    }
    if (len == 0u || len >= sizeof query) return false;
    mu_quill_text(&d->q, a, len, query);
    query[len] = 0;
    n = quill_find_all(d, query, range_start, range_end, ranges, QUILL_MAX_CURSORS);
    if (n == 0u) return false;
    ss->count = 0u;
    for (i = 0u; i < n; ++i)
    {
        ss->c[ss->count].anchor = ranges[i].start;
        ss->c[ss->count].head = ranges[i].end;
        ss->c[ss->count].anim_valid = false;
        ss->count++;
    }
    ss->primary = 0u;
    ss_sync_doc(ss, d);
    return true;
}

bool ss_replace_all(SelectionSet* ss, Document* d, const char* text, uint32_t len, uint64_t now)
{
    uint32_t offs[QUILL_MAX_CURSORS], lens[QUILL_MAX_CURSORS];
    uint32_t i;
    int64_t delta = 0;
    if (ss->count == 0u) return false;
    ss_normalize(ss);
    for (i = 0u; i < ss->count; ++i)
    {
        offs[i] = ss_min(&ss->c[i]);
        lens[i] = ss_max(&ss->c[i]) - offs[i];
    }
    doc_begin_group(d);
    for (i = ss->count; i > 0u; --i)
    {
        uint32_t k = i - 1u;
        if (!doc_replace(d, offs[k], lens[k], text, len, now))
        {
            doc_end_group(d);
            return false;
        }
    }
    doc_end_group(d);
    for (i = 0u; i < ss->count; ++i)
    {
        uint32_t pos = (uint32_t)((int64_t)offs[i] + delta + (int64_t)len);
        ss->c[i].anchor = ss->c[i].head = pos;
        ss->c[i].anim_valid = false;
        delta += (int64_t)len - (int64_t)lens[i];
    }
    ss->primary = 0u;
    ss_sync_doc(ss, d);
    return true;
}
