#include "document.h"
#include "syntax.h"

bool doc_init(Document* d, size_t reserve_bytes)
{
    memset(d, 0, sizeof *d);
    if (!mu_quill_init(&d->q, reserve_bytes))
        return false;
    if (!mu_vm_arena_init(&d->payload, 64u << 20, 64u << 10))
    {
        mu_quill_destroy(&d->q);
        return false;
    }
    mu_small_vec_init_growable(&d->undo, d->undo_inline, 64u, sizeof(UndoOp));
    mu_small_vec_init_growable(&d->redo, d->redo_inline, 64u, sizeof(UndoOp));
    d->syntax = NULL;
    return true;
}

void doc_destroy(Document* d)
{
    if (!d) return;
    syntax_destroy(d->syntax);
    d->syntax = NULL;
    mu_small_vec_destroy(&d->undo);
    mu_small_vec_destroy(&d->redo);
    mu_vm_arena_destroy(&d->payload);
    mu_quill_destroy(&d->q);
}

void doc_attach_syntax(Document* d, Syntax* s)
{
    if (d->syntax && d->syntax != s)
        syntax_destroy(d->syntax);
    d->syntax = s;
}

bool doc_set_text(Document* d, const char* text, uint32_t len)
{
    if (d->q.text_len != 0u && !mu_quill_erase(&d->q, 0u, d->q.text_len))
        return false;
    if (!mu_quill_load(&d->q, text, len))
        return false;
    d->anchor = d->cursor = 0u;
    d->prefer_col = 0u;
    d->dirty = false;
    d->version++;
    mu_small_vec_clear(&d->undo);
    mu_small_vec_clear(&d->redo);
    mu_vm_arena_reset(&d->payload);
    if (d->syntax)
        syntax_reset(d->syntax);
    return true;
}

bool doc_load_file(Document* d, const char* path)
{
    FILE* f;
    long n;
    char* buf;
    bool ok;

    f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    buf = (char*)MU_MALLOC((size_t)n + 1u);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n)
    {
        MU_FREE(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    buf[n] = 0;
    ok = doc_set_text(d, buf, (uint32_t)n);
    MU_FREE(buf);
    if (ok)
    {
        snprintf(d->path, sizeof d->path, "%s", path);
        doc_attach_syntax(d, syntax_create_for_path(path));
        if (d->syntax)
            syntax_parse(d->syntax, &d->q);
    }
    return ok;
}

bool doc_save_file(Document* d, const char* path)
{
    FILE* f;
    uint32_t off = 0u;
    const char* p = (path && path[0]) ? path : d->path;
    if (!p || !p[0]) return false;
    f = fopen(p, "wb");
    if (!f) return false;
    while (off < d->q.text_len)
    {
        uint32_t n;
        const uint8_t* s = mu_quill_span(&d->q, off, &n);
        if (!s || n == 0u) { fclose(f); return false; }
        if (fwrite(s, 1u, n, f) != n) { fclose(f); return false; }
        off += n;
    }
    if (fclose(f) != 0) return false;
    snprintf(d->path, sizeof d->path, "%s", p);
    d->dirty = false;
    return true;
}

uint32_t doc_sel_start(const Document* d)
{
    return d->anchor < d->cursor ? d->anchor : d->cursor;
}

uint32_t doc_sel_end(const Document* d)
{
    return d->anchor > d->cursor ? d->anchor : d->cursor;
}

void doc_set_cursor(Document* d, uint32_t off, bool extend)
{
    if (off > d->q.text_len) off = d->q.text_len;
    d->cursor = off;
    if (!extend) d->anchor = off;
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        d->prefer_col = c.col;
    }
}

void doc_select_all(Document* d)
{
    d->anchor = 0u;
    d->cursor = d->q.text_len;
}

void doc_move_line(Document* d, int delta, bool extend)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, d->cursor);
    uint32_t line = c.line;
    if (delta < 0)
        line = (uint32_t)(-delta) > line ? 0u : line - (uint32_t)(-delta);
    else
    {
        uint32_t n = d->q.nlines;
        line = (uint32_t)delta >= n - line ? n - 1u : line + (uint32_t)delta;
    }
    doc_set_cursor(d, mu_quill_cursor_to_offset(&d->q, line, d->prefer_col), extend);
}

void doc_move_col(Document* d, int delta, bool extend)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, d->cursor);
    uint32_t lo = mu_quill_line_offset(&d->q, c.line);
    uint32_t hi = (c.line + 1u < d->q.nlines)
        ? mu_quill_line_offset(&d->q, c.line + 1u) - 1u
        : d->q.text_len;
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t n, col, off;

    if (delta > 0 && d->cursor >= hi)
    {
        if (c.line + 1u < d->q.nlines)
            doc_set_cursor(d, hi + 1u, extend);
        return;
    }
    if (delta < 0 && d->cursor <= lo)
    {
        if (c.line > 0u)
            doc_set_cursor(d, lo - 1u, extend);
        return;
    }
    n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
    col = d->cursor - lo;
    if (col > n) col = n;
    if (delta > 0)
        off = lo + quill_utf8_next(buf, n, col);
    else
        off = lo + quill_utf8_prev(buf, n, col);
    doc_set_cursor(d, off, extend);
}

static bool is_word(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80u;
}

void doc_move_word(Document* d, int delta, bool extend)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, d->cursor);
    uint32_t lo = mu_quill_line_offset(&d->q, c.line);
    uint32_t hi = (c.line + 1u < d->q.nlines)
        ? mu_quill_line_offset(&d->q, c.line + 1u) - 1u
        : d->q.text_len;
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
    uint32_t col = d->cursor - lo;
    if (col > n) col = n;
    if (delta > 0)
    {
        while (col < n && is_word(buf[col])) col++;
        while (col < n && !is_word(buf[col])) col++;
        if (col == n && hi < d->q.text_len) col = n + 1u;
    }
    else
    {
        if (col > n) col = n;
        while (col > 0u && !is_word(buf[col - 1u])) col--;
        while (col > 0u && is_word(buf[col - 1u])) col--;
    }
    doc_set_cursor(d, lo + col, extend);
}

void doc_home(Document* d, bool extend)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, d->cursor);
    doc_set_cursor(d, mu_quill_line_offset(&d->q, c.line), extend);
}

void doc_end(Document* d, bool extend)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, d->cursor);
    uint32_t hi = (c.line + 1u < d->q.nlines)
        ? mu_quill_line_offset(&d->q, c.line + 1u) - 1u
        : d->q.text_len;
    doc_set_cursor(d, hi, extend);
}

TSPoint doc_point(Document* d, uint32_t off)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    TSPoint p;
    p.row = c.line;
    p.column = c.col;
    return p;
}

static char* payload_reserve(Document* d, uint32_t len, uint32_t* off)
{
    char* p;
    *off = (uint32_t)mu_vm_arena_used(&d->payload);
    if (len == 0u) return NULL;
    p = (char*)mu_vm_arena_alloc(&d->payload, len, 1u);
    if (!p) *off = UINT32_MAX;
    return p;
}

static uint32_t payload_put(Document* d, const char* text, uint32_t len)
{
    uint32_t off;
    char* p = payload_reserve(d, len, &off);
    if (len == 0u) return 0u;
    if (!p) return UINT32_MAX;
    memcpy(p, text, len);
    return off;
}

static bool perform_edit(Document* d, uint32_t off, uint32_t del_len,
                         const char* ins, uint32_t ins_len, UndoOp* op)
{
    TSPoint sp, oep, nep;
    uint32_t old_pay = 0u, new_pay = 0u;
    uint32_t anchor_before = d->anchor, cursor_before = d->cursor;

    if (off > d->q.text_len) off = d->q.text_len;
    if (del_len > d->q.text_len - off) del_len = d->q.text_len - off;

    sp  = doc_point(d, off);
    oep = doc_point(d, off + del_len);

    if (del_len != 0u)
    {
        char* old_text = payload_reserve(d, del_len, &old_pay);
        if (!old_text) return false;
        if (mu_quill_text(&d->q, off, del_len, old_text) != del_len)
            return false;
    }
    if (ins_len != 0u)
    {
        new_pay = payload_put(d, ins, ins_len);
        if (new_pay == UINT32_MAX) return false;
    }

    if (del_len != 0u && !mu_quill_erase(&d->q, off, del_len))
        return false;
    if (ins_len != 0u && !mu_quill_insert(&d->q, off, ins, ins_len))
        return false;

    nep = doc_point(d, off + ins_len);
    if (d->syntax)
        syntax_edit(d->syntax, off, off + del_len, off + ins_len, sp, oep, nep);

    d->anchor = d->cursor = off + ins_len;
    d->prefer_col = nep.column;
    d->dirty = true;
    d->version++;

    if (op)
    {
        op->off = off;
        op->old_len = del_len;
        op->new_len = ins_len;
        op->old_pay = old_pay;
        op->new_pay = new_pay;
        op->anchor_before = anchor_before;
        op->cursor_before = cursor_before;
        op->anchor_after = d->anchor;
        op->cursor_after = d->cursor;
        op->time_ms = 0u;
        op->group = d->active_group ? d->active_group : ++d->group_counter;
    }
    return true;
}

bool doc_replace(Document* d, uint32_t off, uint32_t len,
                 const char* text, uint32_t nlen, uint64_t now)
{
    UndoOp op;
    UndoOp* top;
    uint32_t n;

    if (!perform_edit(d, off, len, text, nlen, &op))
        return false;
    op.time_ms = now;

    /* Coalesce adjacent single/double byte inserts into one undo step. */
    n = mu_small_vec_count(&d->undo);
    if (len == 0u && nlen <= 2u && n > 0u && d->active_group == 0u)
    {
        top = (UndoOp*)mu_small_vec_at(&d->undo, n - 1u);
        if (top->old_len == 0u && top->off + top->new_len == off &&
            now - top->time_ms < 800u)
        {
            const char* base = (const char*)mu_vm_arena_base(&d->payload);
            uint32_t combined_len = top->new_len + nlen;
            uint32_t pay;
            char* combined = payload_reserve(d, combined_len, &pay);
            if (combined)
            {
                memcpy(combined, base + top->new_pay, top->new_len);
                memcpy(combined + top->new_len, text, nlen);
                top->new_len = combined_len;
                top->new_pay = pay;
                top->anchor_after = op.anchor_after;
                top->cursor_after = op.cursor_after;
                top->time_ms = now;
                mu_small_vec_clear(&d->redo);
                return true;
            }
        }
    }
    mu_small_vec_push(&d->undo, &op);
    mu_small_vec_clear(&d->redo);
    return true;
}

bool doc_insert(Document* d, uint32_t off, const char* text, uint32_t nlen, uint64_t now)
{
    return doc_replace(d, off, 0u, text, nlen, now);
}

bool doc_erase(Document* d, uint32_t off, uint32_t len, uint64_t now)
{
    return doc_replace(d, off, len, NULL, 0u, now);
}

static bool apply_inverse(Document* d, const UndoOp* op, bool undo)
{
    const char* base = (const char*)mu_vm_arena_base(&d->payload);
    uint32_t del_len = undo ? op->new_len : op->old_len;
    uint32_t ins_len = undo ? op->old_len : op->new_len;
    const char* ins = undo ? base + op->old_pay : base + op->new_pay;
    uint32_t anchor = undo ? op->anchor_before : op->anchor_after;
    uint32_t cursor = undo ? op->cursor_before : op->cursor_after;

    if (!perform_edit(d, op->off, del_len, ins_len ? ins : NULL, ins_len, NULL))
        return false;
    d->anchor = anchor;
    d->cursor = cursor;
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, cursor);
        d->prefer_col = c.col;
    }
    return true;
}

bool doc_undo(Document* d)
{
    uint32_t n = mu_small_vec_count(&d->undo);
    uint32_t group;
    UndoOp op;
    if (n == 0u) return false;
    memcpy(&op, mu_small_vec_at(&d->undo, n - 1u), sizeof op);
    group = op.group;
    while (mu_small_vec_count(&d->undo) != 0u)
    {
        n = mu_small_vec_count(&d->undo);
        memcpy(&op, mu_small_vec_at(&d->undo, n - 1u), sizeof op);
        if (op.group != group) break;
        mu_small_vec_pop(&d->undo);
        if (!apply_inverse(d, &op, true))
            return false;
        mu_small_vec_push(&d->redo, &op);
    }
    return true;
}

bool doc_redo(Document* d)
{
    uint32_t n = mu_small_vec_count(&d->redo);
    uint32_t group;
    UndoOp op;
    if (n == 0u) return false;
    memcpy(&op, mu_small_vec_at(&d->redo, n - 1u), sizeof op);
    group = op.group;
    while (mu_small_vec_count(&d->redo) != 0u)
    {
        n = mu_small_vec_count(&d->redo);
        memcpy(&op, mu_small_vec_at(&d->redo, n - 1u), sizeof op);
        if (op.group != group) break;
        mu_small_vec_pop(&d->redo);
        if (!apply_inverse(d, &op, false))
            return false;
        mu_small_vec_push(&d->undo, &op);
    }
    return true;
}

void doc_begin_group(Document* d)
{
    if (d->active_group == 0u)
        d->active_group = ++d->group_counter;
}

void doc_end_group(Document* d)
{
    d->active_group = 0u;
}
