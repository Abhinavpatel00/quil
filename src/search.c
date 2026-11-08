#include "search.h"
#include <ctype.h>

bool quill_smartcase(const char* query)
{
    const unsigned char* p = (const unsigned char*)query;
    for (; *p; ++p)
        if (isupper(*p)) return true;
    return false;
}

static bool match_at(const uint8_t* line, uint32_t len, uint32_t at,
                     const char* q, uint32_t ql, bool cs)
{
    uint32_t i;
    if (at + ql > len) return false;
    for (i = 0u; i < ql; ++i)
    {
        uint8_t a = line[at + i];
        uint8_t b = (uint8_t)q[i];
        if (!cs)
        {
            a = (uint8_t)tolower(a);
            b = (uint8_t)tolower(b);
        }
        if (a != b) return false;
    }
    return true;
}

void quill_word_range(Document* d, uint32_t off, uint32_t* start, uint32_t* end)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    uint32_t lo = mu_quill_line_offset(&d->q, c.line);
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
    uint32_t col = off - lo, a, b;
    if (col > n) col = n;
    a = b = col;
    if (n == 0u) { *start = *end = lo; return; }
    if (b < n && (isalnum(buf[b]) || buf[b] == '_' || buf[b] >= 0x80u))
    {
        while (a > 0u && (isalnum(buf[a - 1u]) || buf[a - 1u] == '_' || buf[a - 1u] >= 0x80u)) a--;
        while (b < n && (isalnum(buf[b]) || buf[b] == '_' || buf[b] >= 0x80u)) b++;
    }
    else if (b > 0u)
    {
        a = b - 1u;
    }
    *start = lo + a;
    *end = lo + b;
}

bool quill_find(Document* d, const char* query, uint32_t from, int dir,
                uint32_t* start, uint32_t* end)
{
    uint32_t ql = (uint32_t)strlen(query);
    bool cs;
    uint32_t line, nlines, first;
    uint8_t buf[QUILL_MAX_LINE];
    mu_quill_cursor c;

    if (ql == 0u || d->q.nlines == 0u) return false;
    cs = quill_smartcase(query);
    c = mu_quill_offset_to_cursor(&d->q, from);
    line = c.line;
    nlines = d->q.nlines;
    first = line;

    for (;;)
    {
        uint32_t lo = mu_quill_line_offset(&d->q, line);
        uint32_t n = mu_quill_line_text(&d->q, line, (char*)buf, sizeof buf);
        if (dir >= 0)
        {
            uint32_t col = (line == first) ? c.col : 0u;
            uint32_t i;
            if (col > n) col = n;
            for (i = col; i + ql <= n; ++i)
                if (match_at(buf, n, i, query, ql, cs))
                {
                    *start = lo + i;
                    *end = lo + i + ql;
                    return true;
                }
        }
        else
        {
            uint32_t col = (line == first) ? c.col : n;
            uint32_t i;
            if (col > n) col = n;
            if (col >= ql)
                for (i = col - ql + 1u; i > 0u; --i)
                    if (match_at(buf, n, i - 1u, query, ql, cs))
                    {
                        *start = lo + i - 1u;
                        *end = lo + i - 1u + ql;
                        return true;
                    }
            if (col >= ql && match_at(buf, n, 0u, query, ql, cs) && col > 0u)
            {
                /* handled by loop when i==1 */
            }
        }
        if (dir >= 0) line = (line + 1u) % nlines;
        else line = line == 0u ? nlines - 1u : line - 1u;
        if (line == first) break;
    }
    return false;
}

uint32_t quill_find_all(Document* d, const char* query, uint32_t start, uint32_t end,
                        SearchRange* out, uint32_t cap)
{
    uint32_t ql = (uint32_t)strlen(query);
    bool cs;
    mu_quill_view v;
    mu_quill_line l;
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t nout = 0u, line;
    mu_quill_cursor c;

    if (ql == 0u || cap == 0u) return 0u;
    cs = quill_smartcase(query);
    c = mu_quill_offset_to_cursor(&d->q, start);
    line = c.line;
    if (!mu_quill_view_start(&d->q, line, &v)) return 0u;
    while (mu_quill_view_next_text(&d->q, &v, (char*)buf, sizeof buf, &l))
    {
        uint32_t n = l.len < QUILL_MAX_LINE ? l.len : QUILL_MAX_LINE;
        uint32_t i;
        if (l.offset >= end) break;
        for (i = 0u; i + ql <= n; ++i)
        {
            uint32_t a = l.offset + i, b = a + ql;
            if (a < start || b > end) continue;
            if (match_at(buf, n, i, query, ql, cs))
            {
                out[nout].start = a;
                out[nout].end = b;
                if (++nout >= cap) return nout;
                i += ql - 1u;
            }
        }
        ++line;
    }
    return nout;
}
