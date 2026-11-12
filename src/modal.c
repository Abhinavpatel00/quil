#include "modal.h"
#include "search.h"
#include "syntax.h"
#include <GLFW/glfw3.h>
#include <ctype.h>

static uint32_t count_or(const ModalState* m, uint32_t def)
{
    return m->count ? m->count : def;
}

static uint32_t line_start(Document* d, uint32_t off)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    return mu_quill_line_offset(&d->q, c.line);
}

static uint32_t line_end(Document* d, uint32_t off)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    if (c.line + 1u < d->q.nlines)
        return mu_quill_line_offset(&d->q, c.line + 1u) - 1u;
    return d->q.text_len;
}

static uint32_t line_no(Document* d, uint32_t off)
{
    return mu_quill_offset_to_cursor(&d->q, off).line;
}

/* Arrow-key motions wrap at line boundaries, unlike the Vim-style h/l
 * motions. Right at the end of a line goes to the start of the next line;
 * in Normal mode it skips the newline-offset slot so a press on the last
 * character moves straight on, while Insert mode keeps its after-last-char
 * insertion point. Left at the start of a line goes to the end of the
 * previous one. Counts repeat the wrap, so a long press keeps walking. */
static uint32_t arrow_right(Document* d, uint32_t off, uint32_t count, bool allow_eol)
{
    uint32_t i;
    for (i = 0u; i < count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        uint32_t hi = line_end(d, off);
        if (allow_eol ? off < hi : off + 1u < hi)
            off++;
        else if (c.line + 1u < d->q.nlines)
            off = mu_quill_line_offset(&d->q, c.line + 1u);
        else
        {
            if (off < hi) off = hi;
            break;
        }
    }
    return off;
}

static uint32_t arrow_left(Document* d, uint32_t off, uint32_t count)
{
    uint32_t i;
    for (i = 0u; i < count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        uint32_t lo = mu_quill_line_offset(&d->q, c.line);
        if (off > lo)
            off--;
        else if (c.line > 0u)
            off = lo - 1u; /* the previous line's newline, i.e. its end */
        else
            break;
    }
    return off;
}

static void clamp_off(Document* d, uint32_t* off)
{
    if (*off > d->q.text_len) *off = d->q.text_len;
}

static bool is_word_ch(uint8_t c)
{
    return isalnum(c) || c == '_' || c >= 0x80u;
}

static uint32_t word_next(Document* d, uint32_t off, uint32_t count)
{
    uint32_t i;
    for (i = 0u; i < count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        uint32_t lo = mu_quill_line_offset(&d->q, c.line);
        uint32_t hi = line_end(d, off);
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
        uint32_t col = off - lo;
        if (col > n) col = n;
        if (col >= n)
        {
            off = hi < d->q.text_len ? hi + 1u : hi;
            continue;
        }
        if (is_word_ch(buf[col]))
            while (col < n && is_word_ch(buf[col])) col++;
        else if (!isspace(buf[col]))
            while (col < n && !is_word_ch(buf[col]) && !isspace(buf[col])) col++;
        while (col < n && isspace(buf[col])) col++;
        if (col >= n && hi < d->q.text_len) col = n + 1u;
        off = lo + col;
    }
    clamp_off(d, &off);
    return off;
}

static uint32_t word_prev(Document* d, uint32_t off, uint32_t count)
{
    uint32_t i;
    for (i = 0u; i < count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        uint32_t lo = mu_quill_line_offset(&d->q, c.line);
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
        uint32_t col = off - lo;
        if (col > n) col = n;
        if (col == 0u)
        {
            if (lo > 0u) off = lo - 1u;
            continue;
        }
        col--;
        while (col > 0u && isspace(buf[col])) col--;
        if (is_word_ch(buf[col]))
            while (col > 0u && is_word_ch(buf[col - 1u])) col--;
        else
            while (col > 0u && !is_word_ch(buf[col - 1u]) && !isspace(buf[col - 1u])) col--;
        off = lo + col;
    }
    clamp_off(d, &off);
    return off;
}

static uint32_t word_end(Document* d, uint32_t off, uint32_t count)
{
    uint32_t i;
    for (i = 0u; i < count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
        uint32_t lo = mu_quill_line_offset(&d->q, c.line);
        uint32_t hi = line_end(d, off);
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
        uint32_t col = off - lo;
        if (col >= n)
        {
            off = hi < d->q.text_len ? hi + 1u : hi;
            continue;
        }
        col++;
        while (col < n && isspace(buf[col])) col++;
        if (col < n)
        {
            if (is_word_ch(buf[col]))
                while (col + 1u < n && is_word_ch(buf[col + 1u])) col++;
            else
                while (col + 1u < n && !is_word_ch(buf[col + 1u]) && !isspace(buf[col + 1u])) col++;
        }
        off = lo + col;
    }
    clamp_off(d, &off);
    return off;
}

static uint32_t find_char(Document* d, uint32_t off, int ch, int dir, int till, uint32_t count)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    uint32_t lo = mu_quill_line_offset(&d->q, c.line);
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf);
    uint32_t col = off - lo, i, found = 0u, pos = off;
    if (col > n) col = n;
    if (dir > 0)
    {
        for (i = col + 1u; i < n; ++i)
            if (buf[i] == (uint8_t)ch)
            {
                pos = lo + i;
                if (++found >= count) break;
            }
    }
    else
    {
        i = col;
        while (i > 0u)
        {
            --i;
            if (buf[i] == (uint8_t)ch)
            {
                pos = lo + i;
                if (++found >= count) break;
            }
        }
    }
    if (found == 0u) return off;
    if (till) pos = dir > 0 ? pos - 1u : pos + 1u;
    return pos;
}

static uint32_t match_bracket(Document* d, uint32_t off)
{
    uint8_t ch;
    int dir = 0, depth = 0;
    uint32_t i;
    if (off >= d->q.text_len) return off;
    mu_quill_text(&d->q, off, 1u, (char*)&ch);
    switch (ch)
    {
    case '(': dir = 1; break; case ')': dir = -1; break;
    case '[': dir = 1; break; case ']': dir = -1; break;
    case '{': dir = 1; break; case '}': dir = -1; break;
    default: return off;
    }
    for (i = off; i < d->q.text_len; )
    {
        uint8_t c;
        mu_quill_text(&d->q, i, 1u, (char*)&c);
        if (dir > 0)
        {
            if (c == ch) depth++;
            else if ((ch == '(' && c == ')') || (ch == '[' && c == ']') || (ch == '{' && c == '}')) depth--;
            if (depth == 0) return i;
            i++;
        }
        else
        {
            if (c == ch) depth++;
            else if ((ch == ')' && c == '(') || (ch == ']' && c == '[') || (ch == '}' && c == '{')) depth--;
            if (depth == 0) return i;
            if (i == 0u) break;
            i--;
        }
    }
    return off;
}

static uint32_t paragraph(Document* d, uint32_t off, int dir, uint32_t count)
{
    uint32_t line = line_no(d, off), i;
    for (i = 0u; i < count; ++i)
    {
        if (dir > 0)
        {
            while (line + 1u < d->q.nlines)
            {
                uint32_t lo = mu_quill_line_offset(&d->q, line);
                uint32_t hi = line_end(d, lo);
                if (hi == lo) break;
                line++;
            }
            while (line + 1u < d->q.nlines)
            {
                uint32_t lo = mu_quill_line_offset(&d->q, line + 1u);
                uint32_t hi = line_end(d, lo);
                if (hi != lo) break;
                line++;
            }
            if (line + 1u < d->q.nlines) line++;
        }
        else
        {
            while (line > 0u)
            {
                uint32_t lo = mu_quill_line_offset(&d->q, line - 1u);
                uint32_t hi = line_end(d, lo);
                if (hi == lo) break;
                line--;
            }
            while (line > 0u)
            {
                uint32_t lo = mu_quill_line_offset(&d->q, line - 1u);
                uint32_t hi = line_end(d, lo);
                if (hi != lo) break;
                line--;
            }
        }
    }
    return mu_quill_line_offset(&d->q, line);
}

static bool motion_target(Document* d, uint32_t off, int key, uint32_t count,
                          ModalState* m, uint32_t* target, bool* linewise, bool* inclusive)
{
    mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
    uint32_t lo = mu_quill_line_offset(&d->q, c.line);
    uint32_t hi = line_end(d, off);
    /* line_end() points AT the newline.  Vim's '$' and 'l' stop on the last
     * character, so keep a separate value for anything cursor-like. */
    uint32_t last = hi > lo ? hi - 1u : lo;
    *linewise = false;
    *inclusive = false;
    *target = off;
    switch (key)
    {
    case 'h': *target = off - lo >= count ? off - count : lo; return true;
    case 'l': *target = last - off >= count ? off + count : last; return true;
    case 'j':
        *linewise = true;
        if (c.line + count < d->q.nlines)
            *target = mu_quill_cursor_to_offset(&d->q, c.line + count, c.col);
        else
            *target = d->q.text_len;
        return true;
    case 'k':
        *linewise = true;
        *target = mu_quill_cursor_to_offset(&d->q, c.line >= count ? c.line - count : 0u, c.col);
        return true;
    case '0': *target = lo; return true;
    case '^':
    {
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t n = mu_quill_line_text(&d->q, c.line, (char*)buf, sizeof buf), i = 0u;
        while (i < n && isspace(buf[i])) i++;
        *target = lo + i;
        return true;
    }
    case '$': *target = last; *inclusive = true; return true;
    /* W/B/E are documented as the same word-class motion as w/b/e for now. */
    case 'w': case 'W': *target = word_next(d, off, count); return true;
    case 'b': case 'B': *target = word_prev(d, off, count); return true;
    case 'e': case 'E': *target = word_end(d, off, count); *inclusive = true; return true;
    case 'G': *target = d->q.text_len; *linewise = true; return true;
    case 'g': *target = 0u; *linewise = true; return true; /* gg */
    case '{': *target = paragraph(d, off, -1, count); return true;
    case '}': *target = paragraph(d, off, 1, count); return true;
    case '%': *target = match_bracket(d, off); *inclusive = true; return true;
    case 'f': case 'F': case 't': case 'T':
        if (m->last_find_char == 0) return false;
        *target = find_char(d, off, m->last_find_char,
                            (key == 'f' || key == 't') ? 1 : -1,
                            (key == 't' || key == 'T'), count);
        *inclusive = true;
        return true;
    case ';':
        if (m->last_find_char == 0) return false;
        *target = find_char(d, off, m->last_find_char, m->last_find_dir, m->last_find_till, count);
        *inclusive = true;
        return true;
    case ',':
        if (m->last_find_char == 0) return false;
        *target = find_char(d, off, m->last_find_char, -m->last_find_dir, m->last_find_till, count);
        *inclusive = true;
        return true;
    default: return false;
    }
}

static bool textobj_range(Document* d, uint32_t off, int obj, bool around,
                          uint32_t* start, uint32_t* end)
{
    uint32_t lo = line_start(d, off);
    uint8_t buf[QUILL_MAX_LINE];
    uint32_t n = mu_quill_line_text(&d->q, line_no(d, off), (char*)buf, sizeof buf);
    uint32_t col = off - lo, a, b;
    if (col > n) col = n;
    *start = off; *end = off;
    switch (obj)
    {
    case 'w': case 'W':
        quill_word_range(d, off, start, end);
        if (around)
        {
            uint32_t e = *end - lo;
            while (e < n && isspace(buf[e])) e++;
            *end = lo + e;
        }
        return true;
    case '"': case '\'': case '`':
    {
        /* Quote objects are strictly within one line.  vim's rule: find the
         * opening quote at or before the cursor and the closing one after it.
         * The previous version walked back to the quote but then used the
         * cursor's own column as the start, so di" on "hi there" produced "h"
         * instead of the empty string. */
        uint8_t q = (uint8_t)obj;
        uint32_t open = 0u, close = 0u;
        bool found = false;

        /* treat a quote under the cursor as the opening one, but fall back to
         * searching backwards when that quote has no partner after it */
        for (uint32_t from = col + 1u; from-- > 0u; )
        {
            uint32_t t;
            if (buf[from] != q) continue;
            for (t = from + 1u; t < n && buf[t] != q; ++t) { }
            if (t < n) { open = from; close = t; found = true; break; }
        }
        if (!found)
        {
            /* cursor is between quotes that are both before it: pair the last
             * quote before the cursor with the first one after it */
            uint32_t o = col, c;
            while (o > 0u && buf[o - 1u] != q) o--;
            if (!(o == 0u && buf[0] != q)) o = o > 0u ? o - 1u : 0u;
            else return false;
            for (c = o + 1u; c < n && buf[c] != q; ++c) { }
            if (c >= n) return false;
            open = o; close = c; found = true;
        }
        (void)found;

        *start = lo + open;
        *end = lo + close + 1u;
        if (!around) { (*start)++; (*end)--; }
        return true;
    }
    case '(': case ')': case 'b':
    case '[': case ']':
    case '{': case '}': case 'B':
    case '<': case '>':
    {
        uint32_t i, depth = 0;
        uint8_t open = obj, close = obj;
        if (obj == '(' || obj == ')') { open = '('; close = ')'; }
        else if (obj == '[' || obj == ']') { open = '['; close = ']'; }
        else if (obj == '{' || obj == '}' || obj == 'B') { open = '{'; close = '}'; }
        else if (obj == '<' || obj == '>') { open = '<'; close = '>'; }
        else { open = '('; close = ')'; }
        for (i = col; i > 0u; --i)
        {
            if (buf[i - 1u] == close) depth++;
            else if (buf[i - 1u] == open)
            {
                if (depth == 0u) break;
                depth--;
            }
        }
        if (i == 0u && buf[0] != open) return false;
        a = i > 0u ? i - 1u : 0u;
        depth = 0u;
        for (i = col; i < n; ++i)
        {
            if (buf[i] == open) depth++;
            else if (buf[i] == close)
            {
                if (depth == 0u) break;
                depth--;
            }
        }
        if (i >= n) return false;
        b = i;
        *start = lo + a;
        *end = lo + b + 1u;
        if (!around) { (*start)++; (*end)--; }
        return true;
    }
    case 'p':
        a = line_no(d, off);
        b = a;
        while (a > 0u)
        {
            uint32_t l = mu_quill_line_offset(&d->q, a - 1u);
            if (line_end(d, l) == l) break;
            a--;
        }
        while (b + 1u < d->q.nlines)
        {
            uint32_t l = mu_quill_line_offset(&d->q, b + 1u);
            if (line_end(d, l) == l) break;
            b++;
        }
        *start = mu_quill_line_offset(&d->q, a);
        *end = line_end(d, mu_quill_line_offset(&d->q, b));
        if (around && *end < d->q.text_len) (*end)++;
        return true;
    default: return false;
    }
}

static void reg_set(ModalState* m, int reg, const char* text, uint32_t len, bool linewise)
{
    uint32_t idx = (uint32_t)reg;
    if (reg == '+' || reg == '*')
    {
        if (m->window) glfwSetClipboardString(m->window, text);
        return;
    }
    /* The unnamed register mirrors the system clipboard so that yank/delete
     * (e.g. `yy`, `y` in Visual mode, `dd`, `x`) can be pasted into other
     * applications -- on Wayland as well as on X11. */
    if (reg == '"' && len > 0u && m->window) glfwSetClipboardString(m->window, text);
    if (idx >= QUILL_MAX_REGISTERS) return;
    if (len > QUILL_REG_CAP - 1u) len = QUILL_REG_CAP - 1u;
    memcpy(m->reg_data[idx], text, len);
    m->reg_data[idx][len] = 0;
    m->reg_len[idx] = len;
    m->reg_linewise[idx] = linewise;
    if (reg != '_' && reg != '"')
    {
        memcpy(m->reg_data['"'], m->reg_data[idx], len + 1u);
        m->reg_len['"'] = len;
        m->reg_linewise['"'] = linewise;
    }
}

static uint32_t reg_get(ModalState* m, int reg, char* out, uint32_t cap, bool* linewise)
{
    uint32_t idx = (uint32_t)reg, len;
    /* The unnamed register mirrors the system clipboard on yank/delete, so a
     * bare p/P must read the clipboard too. Otherwise text copied in another
     * application stays invisible to p until quill itself yanks something. */
    if (reg == '+' || reg == '*' || reg == '"')
    {
        const char* s = m->window ? glfwGetClipboardString(m->window) : NULL;
        if (s && s[0])
        {
            uint32_t unnamed = (uint32_t)'"';
            len = (uint32_t)strlen(s);
            if (len > cap) len = cap;
            memcpy(out, s, len);
            if (linewise)
            {
                /* Keep a linewise paste when the clipboard still holds exactly
                 * what quill yanked/deleted (the unnamed register mirrors it).
                 * Text put there by another application is pasted charwise. */
                *linewise = reg == '"' && m->reg_linewise[unnamed] &&
                            m->reg_len[unnamed] == len &&
                            memcmp(m->reg_data[unnamed], out, len) == 0;
            }
            return len;
        }
        if (reg == '+' || reg == '*')
        {
            if (linewise) *linewise = false;
            return 0u;
        }
        /* Clipboard empty/unavailable (e.g. headless tests): fall through to
         * the local unnamed register so p still works. */
    }
    if (idx >= QUILL_MAX_REGISTERS) return 0u;
    len = m->reg_len[idx];
    if (len > cap) len = cap;
    memcpy(out, m->reg_data[idx], len);
    if (linewise) *linewise = m->reg_linewise[idx];
    return len;
}

static void dot_keys(ModalState* m, const char* keys)
{
    if (m->dot.replaying) return;
    snprintf(m->dot.keys, sizeof m->dot.keys, "%s", keys);
    m->dot.len = (uint32_t)strlen(m->dot.keys);
    m->dot.recording_insert = false;
}

static void dot_begin_insert(ModalState* m, const char* prefix)
{
    if (m->dot.replaying) return;
    snprintf(m->dot.keys, sizeof m->dot.keys, "%s", prefix);
    m->dot.len = (uint32_t)strlen(m->dot.keys);
    m->dot.recording_insert = true;
}

static void dot_append(ModalState* m, const char* s, uint32_t n)
{
    if (!m->dot.recording_insert || m->dot.replaying) return;
    if (m->dot.len + n >= sizeof m->dot.keys) return;
    memcpy(m->dot.keys + m->dot.len, s, n);
    m->dot.len += n;
    m->dot.keys[m->dot.len] = 0;
}

void modal_enter_insert(ModalState* m, Document* d, SelectionSet* ss, int how, uint64_t now)
{
    uint32_t i;
    char prefix[2] = { (char)how, 0 };
    (void)now;
    if (how == 'o' || how == 'O')
    {
        uint32_t offs[QUILL_MAX_CURSORS];
        doc_begin_group(d);
        for (i = 0u; i < ss->count; ++i)
        {
            uint32_t off = ss->c[i].head;
            uint32_t pos = how == 'o' ? line_end(d, off) : line_start(d, off);
            offs[i] = pos;
            doc_replace(d, pos, 0u, "\n", 1u, now);
        }
        doc_end_group(d);
        for (i = 0u; i < ss->count; ++i)
        {
            ss->c[i].anchor = ss->c[i].head = offs[i] + 1u;
            ss->c[i].anim_valid = false;
        }
        ss_sync_doc(ss, d);
    }
    else if (how == 'I')
    {
        for (i = 0u; i < ss->count; ++i)
        {
            uint32_t pos = line_start(d, ss->c[i].head);
            uint8_t buf[QUILL_MAX_LINE];
            uint32_t n = mu_quill_line_text(&d->q, line_no(d, pos), (char*)buf, sizeof buf), j = 0u;
            while (j < n && isspace(buf[j])) j++;
            ss->c[i].anchor = ss->c[i].head = pos + j;
        }
        ss_sync_doc(ss, d);
    }
    else if (how == 'A')
    {
        for (i = 0u; i < ss->count; ++i)
            ss->c[i].anchor = ss->c[i].head = line_end(d, ss->c[i].head);
        ss_sync_doc(ss, d);
    }
    else if (how == 'a')
    {
        for (i = 0u; i < ss->count; ++i)
            if (ss->c[i].head < line_end(d, ss->c[i].head))
                ss->c[i].head++, ss->c[i].anchor = ss->c[i].head;
        ss_sync_doc(ss, d);
    }
    m->mode = QMODE_INSERT;
    m->op = 0;
    m->pending = 0;
    m->count = 0u;
    dot_begin_insert(m, prefix);
}

void modal_escape(ModalState* m, Document* d, SelectionSet* ss)
{
    if (m->mode == QMODE_INSERT)
    {
        m->mode = QMODE_NORMAL;
        m->dot.recording_insert = false;
        ss_sync_doc(ss, d);
        /* Auto-save on leaving Insert mode when enabled (config default: true). */
        if (m->auto_save_on_mode_change && d->dirty && d->path[0])
            doc_save_file(d, NULL);
        return;
    }
    if (m->mode == QMODE_VISUAL || m->mode == QMODE_VISUAL_LINE || m->mode == QMODE_VISUAL_BLOCK)
    {
        uint32_t i;
        m->mode = QMODE_NORMAL;
        for (i = 0u; i < ss->count; ++i)
            ss->c[i].anchor = ss->c[i].head;
        ss_sync_doc(ss, d);
        return;
    }
    if (ss->count > 1u)
        ss_collapse_primary(ss);
    m->op = 0;
    m->pending = 0;
    m->count = 0u;
    ss_sync_doc(ss, d);
}

static void set_ranges(SelectionSet* ss, const uint32_t* starts, const uint32_t* ends, uint32_t n)
{
    uint32_t i;
    ss->count = n;
    for (i = 0u; i < n; ++i)
    {
        ss->c[i].anchor = starts[i];
        ss->c[i].head = ends[i];
        ss->c[i].anim_valid = false;
    }
    ss_normalize(ss);
}

static void yank_ranges(ModalState* m, Document* d, const uint32_t* starts,
                        const uint32_t* ends, uint32_t n, bool linewise)
{
    char out[QUILL_REG_CAP];
    uint32_t len = 0u, i;
    int reg = m->reg ? m->reg : '"';
    for (i = 0u; i < n; ++i)
    {
        uint32_t l = ends[i] - starts[i];
        if (len + l + 1u >= sizeof out) break;
        if (i > 0u) out[len++] = '\n';
        mu_quill_text(&d->q, starts[i], l, out + len);
        len += l;
    }
    out[len] = 0;
    reg_set(m, reg, out, len, linewise);
    if (reg != '0' && reg != '_')
        reg_set(m, '0', out, len, linewise);
    m->reg = 0;
}

static void delete_ranges(ModalState* m, Document* d, SelectionSet* ss,
                          uint32_t* starts, uint32_t* ends, uint32_t n,
                          bool linewise, bool change, uint64_t now)
{
    uint32_t i;
    yank_ranges(m, d, starts, ends, n, linewise);
    set_ranges(ss, starts, ends, n);
    ss_replace_all(ss, d, "", 0u, now);
    if (change)
        modal_enter_insert(m, d, ss, 'i', now);
    else
    {
        m->mode = QMODE_NORMAL;
        for (i = 0u; i < ss->count; ++i)
            ss->c[i].anchor = ss->c[i].head;
        ss_sync_doc(ss, d);
    }
}

static void expand_linewise(Document* d, uint32_t* start, uint32_t* end)
{
    /* Grow [start,end) to cover whole lines.  The last line is derived from the
     * offset itself rather than from line_end()+1: line_end() points AT the
     * newline, and stepping past that offset lands in the NEXT line, which is
     * how '>>' ended up indenting one line too many. */
    uint32_t firstline = mu_quill_offset_to_cursor(&d->q, *start).line;
    uint32_t lastline  = mu_quill_offset_to_cursor(&d->q, *end).line;
    uint32_t le;
    *start = mu_quill_line_offset(&d->q, firstline);
    le = line_end(d, mu_quill_line_offset(&d->q, lastline));
    if (le < d->q.text_len) le++;      /* include the newline */
    *end = le;
}

static void case_ranges(Document* d, SelectionSet* ss, uint32_t* starts, uint32_t* ends,
                        uint32_t n, int op, uint64_t now)
{
    char buf[QUILL_REG_CAP];
    uint32_t i;
    doc_begin_group(d);
    for (i = n; i > 0u; --i)
    {
        uint32_t k = i - 1u, l = ends[k] - starts[k], j;
        if (l >= sizeof buf) continue;
        mu_quill_text(&d->q, starts[k], l, buf);
        for (j = 0u; j < l; ++j)
        {
            if (op == 'u') buf[j] = (char)tolower((unsigned char)buf[j]);
            else if (op == 'U') buf[j] = (char)toupper((unsigned char)buf[j]);
            else buf[j] = isupper((unsigned char)buf[j]) ? (char)tolower((unsigned char)buf[j]) : (char)toupper((unsigned char)buf[j]);
        }
        doc_replace(d, starts[k], l, buf, l, now);
    }
    doc_end_group(d);
    for (i = 0u; i < ss->count; ++i)
        ss->c[i].anchor = ss->c[i].head = starts[i];
    ss_sync_doc(ss, d);
}

static void indent_ranges(Document* d, SelectionSet* ss, uint32_t* starts, uint32_t* ends,
                          uint32_t n, bool outdent, uint64_t now)
{
    uint32_t lines[QUILL_MAX_LINE], nl = 0u, i, j;
    for (i = 0u; i < n; ++i)
    {
        /* ranges are half-open [start,end), so the last line covered is the one
         * holding byte end-1.  Using line_no(end) picks up the FOLLOWING line
         * whenever the range ends on a newline, which is exactly how '>>'
         * indented one line too many. */
        uint32_t a = line_no(d, starts[i]);
        uint32_t b = line_no(d, ends[i] > starts[i] ? ends[i] - 1u : ends[i]);
        for (j = a; j <= b; ++j)
        {
            uint32_t k, dup = 0u;
            for (k = 0u; k < nl; ++k) if (lines[k] == j) dup = 1u;
            if (!dup && nl < QUILL_MAX_LINE) lines[nl++] = j;
        }
    }
    doc_begin_group(d);
    for (i = nl; i > 0u; --i)
    {
        uint32_t lo = mu_quill_line_offset(&d->q, lines[i - 1u]);
        if (!outdent)
            doc_replace(d, lo, 0u, "    ", 4u, now);
        else
        {
            uint8_t buf[QUILL_MAX_LINE];
            uint32_t nch = mu_quill_line_text(&d->q, lines[i - 1u], (char*)buf, sizeof buf), k = 0u;
            while (k < nch && k < 4u && buf[k] == ' ') k++;
            if (k > 0u) doc_replace(d, lo, k, "", 0u, now);
        }
    }
    doc_end_group(d);
    ss_sync_doc(ss, d);
}

static const char* comment_prefix(Document* d)
{
    const char* name = syntax_name(d->syntax);
    if (strcmp(name, "Python") == 0) return "# ";
    if (strcmp(name, "JSON") == 0) return "";
    return "// ";
}

static void comment_ranges(Document* d, SelectionSet* ss, uint32_t* starts, uint32_t* ends,
                           uint32_t n, uint64_t now)
{
    const char* pre = comment_prefix(d);
    uint32_t plen = (uint32_t)strlen(pre), i, j;
    uint32_t lines[QUILL_MAX_LINE], nl = 0u;
    if (plen == 0u) return;
    for (i = 0u; i < n; ++i)
    {
        uint32_t a = line_no(d, starts[i]), b = line_no(d, ends[i]);
        for (j = a; j <= b; ++j)
        {
            uint32_t k, dup = 0u;
            for (k = 0u; k < nl; ++k) if (lines[k] == j) dup = 1u;
            if (!dup && nl < QUILL_MAX_LINE) lines[nl++] = j;
        }
    }
    doc_begin_group(d);
    for (i = nl; i > 0u; --i)
    {
        uint32_t lo = mu_quill_line_offset(&d->q, lines[i - 1u]);
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t nch = mu_quill_line_text(&d->q, lines[i - 1u], (char*)buf, sizeof buf), k = 0u;
        while (k < nch && isspace(buf[k])) k++;
        if (k + plen <= nch && memcmp(buf + k, pre, plen) == 0)
            doc_replace(d, lo + k, plen, "", 0u, now);
        else
            doc_replace(d, lo + k, 0u, pre, plen, now);
    }
    doc_end_group(d);
    ss_sync_doc(ss, d);
}

static void execute_op(ModalState* m, Document* d, SelectionSet* ss,
                       uint32_t* starts, uint32_t* ends, uint32_t n,
                       bool linewise, uint64_t now)
{
    uint32_t i;
    int op = m->op;
    if (m->case_op) op = m->case_op;
    for (i = 0u; i < n; ++i)
        if (linewise) expand_linewise(d, &starts[i], &ends[i]);
    if (op == 'y')
    {
        yank_ranges(m, d, starts, ends, n, linewise);
        for (i = 0u; i < ss->count; ++i) ss->c[i].anchor = ss->c[i].head = starts[i];
        m->mode = QMODE_NORMAL;
        ss_sync_doc(ss, d);
        dot_keys(m, "y");
    }
    else if (op == 'd' || op == 'c')
    {
        delete_ranges(m, d, ss, starts, ends, n, linewise, op == 'c', now);
        {
            /* '.' must replay the whole command. Recording a bare "d"/"c" only
             * re-arms the operator, and the next key then becomes its motion --
             * e.g. dd, '.', j silently deletes two lines. */
            char keys[16];
            size_t k = 0u;
            if (m->op_motion_count > 1u)
                k = (size_t)snprintf(keys, sizeof keys, "%u", m->op_motion_count);
            if (m->op_motion_char)
                snprintf(keys + k, sizeof keys - k, "%c%c", op == 'd' ? 'd' : 'c',
                         m->op_motion_char);
            else
                snprintf(keys + k, sizeof keys - k, "%c%c", op == 'd' ? 'd' : 'c',
                         op == 'd' ? 'd' : 'c');
            dot_keys(m, keys);
        }
    }
    else if (op == '>' || op == '<')
    {
        indent_ranges(d, ss, starts, ends, n, op == '<', now);
        m->mode = QMODE_NORMAL;
        dot_keys(m, op == '>' ? ">" : "<");
    }
    else if (op == 'u' || op == 'U' || op == '~')
    {
        case_ranges(d, ss, starts, ends, n, op, now);
        m->mode = QMODE_NORMAL;
    }
    else if (op == 'C')
    {
        comment_ranges(d, ss, starts, ends, n, now);
        m->mode = QMODE_NORMAL;
        dot_keys(m, "gc");
    }
    m->op = 0;
    m->case_op = 0;
    m->pending = 0;
    m->count = 0u;
    m->op_motion_char = 0;
    m->op_motion_count = 0u;
}

static bool operator_motion(ModalState* m, Document* d, SelectionSet* ss,
                            int key, uint64_t now)
{
    m->op_motion_char = key;
    m->op_motion_count = count_or(m, 1u);
    uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
    uint32_t i, n = ss->count, count = count_or(m, 1u);
    bool linewise = false, any_linewise = false;
    if (m->mode == QMODE_VISUAL || m->mode == QMODE_VISUAL_LINE || m->mode == QMODE_VISUAL_BLOCK)
    {
        for (i = 0u; i < n; ++i)
        {
            starts[i] = ss_min(&ss->c[i]);
            ends[i] = ss_max(&ss->c[i]);
            if (m->mode == QMODE_VISUAL && ends[i] < d->q.text_len) ends[i]++;
        }
        any_linewise = (m->mode == QMODE_VISUAL_LINE);
    }
    else
    {
        for (i = 0u; i < n; ++i)
        {
            uint32_t off = ss->c[i].head, target;
            bool inc = false;
            if (!motion_target(d, off, key, count, m, &target, &linewise, &inc))
                return false;
            if (inc && target < d->q.text_len) target++;
            starts[i] = off < target ? off : target;
            ends[i] = off > target ? off : target;
            if (linewise) any_linewise = true;
        }
    }
    execute_op(m, d, ss, starts, ends, n, any_linewise, now);
    return true;
}

static bool operator_textobj(ModalState* m, Document* d, SelectionSet* ss,
                             int obj, bool around, uint64_t now)
{
    uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
    uint32_t i, n = ss->count;
    for (i = 0u; i < n; ++i)
        if (!textobj_range(d, ss->c[i].head, obj, around, &starts[i], &ends[i]))
            return false;
    execute_op(m, d, ss, starts, ends, n, false, now);
    return true;
}

static void visual_update(ModalState* m, Document* d, SelectionSet* ss)
{
    uint32_t i;
    if (m->mode == QMODE_VISUAL_LINE)
    {
        for (i = 0u; i < ss->count; ++i)
        {
            uint32_t a = line_start(d, ss->c[i].anchor);
            uint32_t b = line_end(d, ss->c[i].head);
            ss->c[i].anchor = a;
            ss->c[i].head = b;
        }
    }
    else if (m->mode == QMODE_VISUAL_BLOCK)
    {
        uint32_t al = line_no(d, m->block_anchor), hl = line_no(d, ss->c[0].head);
        uint32_t ac = m->block_anchor - line_start(d, m->block_anchor);
        uint32_t hc = ss->c[0].head - line_start(d, ss->c[0].head);
        uint32_t lo = al < hl ? al : hl, hi = al > hl ? al : hl;
        uint32_t c0 = ac < hc ? ac : hc, c1 = ac > hc ? ac : hc;
        uint32_t line, n = 0u;
        ss->count = 0u;
        for (line = lo; line <= hi && n < QUILL_MAX_CURSORS; ++line)
        {
            uint32_t ls = mu_quill_line_offset(&d->q, line);
            uint32_t le = line_end(d, ls);
            uint32_t a = ls + c0, b = ls + c1 + 1u;
            if (a > le) a = le;
            if (b > le) b = le;
            ss->c[n].anchor = a;
            ss->c[n].head = b;
            ss->c[n].anim_valid = false;
            n++;
        }
        ss->count = n ? n : 1u;
        ss->primary = 0u;
    }
    ss_sync_doc(ss, d);
}

static void simple_insert(ModalState* m, Document* d, SelectionSet* ss, int how, uint64_t now)
{
    if (how == 's' || how == 'S')
    {
        uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS], i;
        for (i = 0u; i < ss->count; ++i)
        {
            starts[i] = ss->c[i].head;
            ends[i] = how == 's' ? starts[i] + 1u : line_end(d, starts[i]);
            if (ends[i] > d->q.text_len) ends[i] = d->q.text_len;
        }
        delete_ranges(m, d, ss, starts, ends, ss->count, how == 'S', true, now);
        return;
    }
    modal_enter_insert(m, d, ss, how, now);
}

static void put_register(ModalState* m, Document* d, SelectionSet* ss, bool before, uint64_t now)
{
    char text[QUILL_REG_CAP];
    bool linewise = false;
    uint32_t len = reg_get(m, m->reg ? m->reg : '"', text, sizeof text - 1u, &linewise);
    uint32_t i, offs[QUILL_MAX_CURSORS];
    text[len] = 0;
    if (len == 0u) return;
    doc_begin_group(d);
    for (i = ss->count; i > 0u; --i)
    {
        uint32_t k = i - 1u, off = ss->c[k].head, pos;
        if (linewise)
        {
            pos = before ? line_start(d, off) : line_end(d, off);
            if (!before && pos < d->q.text_len) pos++;
            offs[k] = pos;
            doc_replace(d, pos, 0u, text, len, now);
            if (text[len - 1u] != '\n')
                doc_replace(d, pos + len, 0u, "\n", 1u, now);
        }
        else
        {
            pos = before ? off : off + 1u;
            if (pos > d->q.text_len) pos = d->q.text_len;
            offs[k] = pos;
            doc_replace(d, pos, 0u, text, len, now);
        }
    }
    doc_end_group(d);
    for (i = 0u; i < ss->count; ++i)
    {
        ss->c[i].anchor = ss->c[i].head = offs[i] + len;
        ss->c[i].anim_valid = false;
    }
    ss_sync_doc(ss, d);
    m->reg = 0;
    dot_keys(m, before ? "P" : "p");
}

static void replace_char(ModalState* m, Document* d, SelectionSet* ss, int ch, uint64_t now)
{
    uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS], i;
    char text[2] = { (char)ch, 0 };
    for (i = 0u; i < ss->count; ++i)
    {
        starts[i] = ss_min(&ss->c[i]);
        ends[i] = ss_max(&ss->c[i]);
        if (starts[i] == ends[i])
        {
            ends[i] = starts[i] + 1u;
            if (ends[i] > d->q.text_len) ends[i] = d->q.text_len;
        }
    }
    set_ranges(ss, starts, ends, ss->count);
    ss_replace_all(ss, d, text, 1u, now);
    m->mode = QMODE_NORMAL;
    m->pending = 0;
    dot_keys(m, "r");
}

static void join_lines(ModalState* m, Document* d, SelectionSet* ss, uint64_t now)
{
    uint32_t i;
    doc_begin_group(d);
    for (i = ss->count; i > 0u; --i)
    {
        uint32_t off = ss->c[i - 1u].head;
        uint32_t le = line_end(d, off);
        uint32_t ln = line_no(d, off);
        uint8_t buf[QUILL_MAX_LINE];
        uint32_t n, k = 0u;
        if (le >= d->q.text_len || ln + 1u >= d->q.nlines) continue;
        n = mu_quill_line_text(&d->q, ln + 1u, (char*)buf, sizeof buf);
        while (k < n && isspace(buf[k])) k++;
        doc_replace(d, le, 1u + k, " ", 1u, now);
    }
    doc_end_group(d);
    ss_sync_doc(ss, d);
    dot_keys(m, "J");
}

static void toggle_case_char(ModalState* m, Document* d, SelectionSet* ss, uint64_t now)
{
    uint32_t i;
    doc_begin_group(d);
    for (i = ss->count; i > 0u; --i)
    {
        uint32_t off = ss->c[i - 1u].head;
        uint8_t ch;
        if (off >= d->q.text_len) continue;
        mu_quill_text(&d->q, off, 1u, (char*)&ch);
        ch = isupper(ch) ? (uint8_t)tolower(ch) : (uint8_t)toupper(ch);
        doc_replace(d, off, 1u, (char*)&ch, 1u, now);
    }
    doc_end_group(d);
    ss_sync_doc(ss, d);
    dot_keys(m, "~");
}

void modal_scroll(ModalState* m, Document* d, SelectionSet* ss, int lines, uint32_t visible_rows)
{
    uint32_t i;
    (void)m;
    for (i = 0u; i < ss->count; ++i)
    {
        mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, ss->c[i].head);
        uint32_t line = c.line;
        if (lines > 0) line = line + (uint32_t)lines < d->q.nlines ? line + (uint32_t)lines : d->q.nlines - 1u;
        else
        {
            uint32_t n = (uint32_t)(-lines);
            line = n > line ? 0u : line - n;
        }
        ss->c[i].head = mu_quill_cursor_to_offset(&d->q, line, c.col);
        if (m->mode == QMODE_NORMAL) ss->c[i].anchor = ss->c[i].head;
        ss->c[i].anim_valid = false;
    }
    (void)visible_rows;
    ss_sync_doc(ss, d);
}

const char* modal_mode_name(const ModalState* m)
{
    switch (m->mode)
    {
    case QMODE_INSERT: return "INSERT";
    case QMODE_VISUAL: return "VISUAL";
    case QMODE_VISUAL_LINE: return "V-LINE";
    case QMODE_VISUAL_BLOCK: return "V-BLOCK";
    default: return "NORMAL";
    }
}

void modal_search_set(ModalState* m, const char* query, int dir)
{
    snprintf(m->search, sizeof m->search, "%s", query);
    m->search_dir = dir;
}

bool modal_search_next(ModalState* m, Document* d, SelectionSet* ss, int dir)
{
    uint32_t s, e;
    uint32_t from = ss_primary(ss)->head;
    if (m->search[0] == 0) return false;
    if (dir > 0 && from < d->q.text_len) from++;
    if (!quill_find(d, m->search, from, dir, &s, &e))
        if (!quill_find(d, m->search, dir > 0 ? 0u : d->q.text_len, dir, &s, &e))
            return false;
    ss_single(ss, s);
    ss_sync_doc(ss, d);
    return true;
}

static void search_word(ModalState* m, Document* d, SelectionSet* ss, int dir)
{
    uint32_t a, b;
    char query[256];
    uint32_t len;
    quill_word_range(d, ss_primary(ss)->head, &a, &b);
    len = b - a;
    if (len == 0u || len >= sizeof query) return;
    mu_quill_text(&d->q, a, len, query);
    query[len] = 0;
    modal_search_set(m, query, dir);
    modal_search_next(m, d, ss, dir);
}

static int key_ascii(int key, int mods)
{
    bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        return (shift ? 'A' : 'a') + (key - GLFW_KEY_A);
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
    {
        if (!shift) return '0' + (key - GLFW_KEY_0);
        switch (key)
        {
        case GLFW_KEY_3: return '#';
        case GLFW_KEY_8: return '*';
        default: return 0;
        }
    }
    switch (key)
    {
    case GLFW_KEY_SPACE: return ' ';
    case GLFW_KEY_APOSTROPHE: return shift ? '"' : '\'';
    case GLFW_KEY_COMMA: return shift ? '<' : ',';
    case GLFW_KEY_MINUS: return shift ? '_' : '-';
    case GLFW_KEY_PERIOD: return shift ? '>' : '.';
    case GLFW_KEY_SLASH: return shift ? '?' : '/';
    case GLFW_KEY_SEMICOLON: return shift ? ':' : ';';
    case GLFW_KEY_EQUAL: return shift ? '+' : '=';
    case GLFW_KEY_LEFT_BRACKET: return shift ? '{' : '[';
    case GLFW_KEY_RIGHT_BRACKET: return shift ? '}' : ']';
    case GLFW_KEY_BACKSLASH: return shift ? '|' : '\\';
    case GLFW_KEY_GRAVE_ACCENT: return shift ? '~' : '`';
    default: return 0;
    }
}

bool modal_wants_char(const ModalState* m)
{
    return m->mode == QMODE_INSERT || m->pending == 'r' ||
           m->pending == 'f' || m->pending == 'F' || m->pending == 't' || m->pending == 'T' ||
           m->pending == '"';
}

bool modal_char(ModalState* m, Document* d, SelectionSet* ss, unsigned int cp, uint64_t now)
{
    char utf8[4];
    uint32_t n = 0u;
    if (cp < 0x80u) utf8[n++] = (char)cp;
    else if (cp < 0x800u) { utf8[n++] = (char)(0xC0u | (cp >> 6)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else if (cp < 0x10000u) { utf8[n++] = (char)(0xE0u | (cp >> 12)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }
    else { utf8[n++] = (char)(0xF0u | (cp >> 18)); utf8[n++] = (char)(0x80u | ((cp >> 12) & 63)); utf8[n++] = (char)(0x80u | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80u | (cp & 63)); }

    if (m->pending == 'r') { replace_char(m, d, ss, (int)cp, now); return true; }
    if (m->pending == 'f' || m->pending == 'F' || m->pending == 't' || m->pending == 'T')
    {
        m->last_find_char = (int)cp;
        m->last_find_dir = (m->pending == 'f' || m->pending == 't') ? 1 : -1;
        m->last_find_till = (m->pending == 't' || m->pending == 'T');
        if (m->op)
        {
            operator_motion(m, d, ss, m->pending, now);
        }
        else
        {
            uint32_t heads[QUILL_MAX_CURSORS], i;
            for (i = 0u; i < ss->count; ++i)
                heads[i] = find_char(d, ss->c[i].head, m->last_find_char, m->last_find_dir, m->last_find_till, count_or(m, 1u));
            for (i = 0u; i < ss->count; ++i)
            {
                ss->c[i].head = heads[i];
                if (m->mode == QMODE_NORMAL) ss->c[i].anchor = heads[i];
            }
            ss_sync_doc(ss, d);
            m->pending = 0;
            m->count = 0u;
        }
        return true;
    }
    if (m->pending == '"')
    {
        m->reg = (int)cp;
        m->pending = 0;
        return true;
    }
    if (m->mode == QMODE_INSERT)
    {
        ss_replace_all(ss, d, utf8, n, now);
        dot_append(m, utf8, n);
        return true;
    }
    return false;
}

static void dot_repeat(ModalState* m, Document* d, SelectionSet* ss, uint64_t now)
{
    uint32_t i;
    if (m->dot.len == 0u) return;
    m->dot.replaying = true;
    if (m->dot.keys[0] == 'i' || m->dot.keys[0] == 'a' || m->dot.keys[0] == 'o' || m->dot.keys[0] == 'O' || m->dot.keys[0] == 'I' || m->dot.keys[0] == 'A')
    {
        modal_enter_insert(m, d, ss, m->dot.keys[0], now);
        if (m->dot.len > 1u)
            ss_replace_all(ss, d, m->dot.keys + 1u, m->dot.len - 1u, now);
        modal_escape(m, d, ss);
    }
    else
    {
        for (i = 0u; i < m->dot.len; ++i)
        {
            char ch = m->dot.keys[i];
            int key = 0;
            if (ch >= 'a' && ch <= 'z') key = GLFW_KEY_A + (ch - 'a');
            else if (ch >= 'A' && ch <= 'Z') key = GLFW_KEY_A + (ch - 'A');
            else if (ch == '.') key = GLFW_KEY_PERIOD;
            else if (ch == '~') key = GLFW_KEY_GRAVE_ACCENT;
            if (key) modal_key(m, d, ss, key, ch >= 'A' && ch <= 'Z' ? GLFW_MOD_SHIFT : 0, now);
        }
    }
    m->dot.replaying = false;
}

static bool modal_key_ch(ModalState* m, Document* d, SelectionSet* ss,
                         int ch, int key, int mods, uint64_t now)
{
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    bool alt = (mods & GLFW_MOD_ALT) != 0;
    uint32_t i;

    m->request = QREQ_NONE;

    if (key == GLFW_KEY_ESCAPE || (ctrl && key == GLFW_KEY_LEFT_BRACKET))
    {
        modal_escape(m, d, ss);
        return true;
    }

    if (m->mode == QMODE_INSERT)
    {
        if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
            key == GLFW_KEY_UP || key == GLFW_KEY_DOWN ||
            key == GLFW_KEY_HOME || key == GLFW_KEY_END)
        {
            for (i = 0u; i < ss->count; ++i)
            {
                uint32_t off = ss->c[i].head;
                mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, off);
                if (key == GLFW_KEY_LEFT)
                    off = arrow_left(d, off, 1u);
                else if (key == GLFW_KEY_RIGHT)
                    off = arrow_right(d, off, 1u, true);
                else if (key == GLFW_KEY_UP)
                    off = mu_quill_cursor_to_offset(&d->q, c.line ? c.line - 1u : 0u, c.col);
                else if (key == GLFW_KEY_DOWN)
                    off = mu_quill_cursor_to_offset(&d->q, c.line + 1u < d->q.nlines ? c.line + 1u : c.line, c.col);
                else if (key == GLFW_KEY_HOME)
                    off = mu_quill_line_offset(&d->q, c.line);
                else
                    off = line_end(d, off);
                ss->c[i].anchor = ss->c[i].head = off;
                ss->c[i].anim_valid = false;
            }
            ss_sync_doc(ss, d);
            return true;
        }
        if (key == GLFW_KEY_BACKSPACE)
        {
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            for (i = 0u; i < ss->count; ++i)
            {
                uint32_t off = ss->c[i].head;
                starts[i] = off > 0u ? off - 1u : off;
                ends[i] = off;
            }
            set_ranges(ss, starts, ends, ss->count);
            ss_replace_all(ss, d, "", 0u, now);
            return true;
        }
        if (key == GLFW_KEY_DELETE)
        {
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            for (i = 0u; i < ss->count; ++i)
            {
                starts[i] = ss->c[i].head;
                ends[i] = starts[i] < d->q.text_len ? starts[i] + 1u : starts[i];
            }
            set_ranges(ss, starts, ends, ss->count);
            ss_replace_all(ss, d, "", 0u, now);
            return true;
        }
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
        {
            ss_replace_all(ss, d, "\n", 1u, now);
            dot_append(m, "\n", 1u);
            return true;
        }
        if (key == GLFW_KEY_TAB)
        {
            ss_replace_all(ss, d, "    ", 4u, now);
            dot_append(m, "    ", 4u);
            return true;
        }
        if (ctrl && key == GLFW_KEY_W)
        {
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            for (i = 0u; i < ss->count; ++i)
            {
                ends[i] = ss->c[i].head;
                starts[i] = word_prev(d, ends[i], 1u);
            }
            set_ranges(ss, starts, ends, ss->count);
            ss_replace_all(ss, d, "", 0u, now);
            return true;
        }
        return false;
    }

    if (ctrl && key == GLFW_KEY_N && !alt)
    {
        ss_add_next(ss, d);
        return true;
    }
    if (ctrl && alt && key == GLFW_KEY_P)
    {
        ss_remove_last(ss);
        ss_sync_doc(ss, d);
        return true;
    }
    if (ctrl && key == GLFW_KEY_R)
    {
        doc_redo(d);
        ss_from_doc(ss, d);
        return true;
    }

    if (m->pending == 1)
    {
        m->pending = 0;
        if (ch == ' ') { m->request = QREQ_PICKER_RECENT; return true; } /* leader space -> recent (top five) */
        if (ch == 'f') { m->request = QREQ_PICKER_FILES; return true; }  /* leader f -> file picker */
        if (ch == 's') { m->pending = 's'; return true; }               /* leader s -> search submenu */
        if (ch == 'm') { m->pending = 3; return true; }
        if (ch == 'c') { m->request = QREQ_PICKER_COMMANDS; return true; }
        if (ch == 'k') { m->request = QREQ_PICKER_KEYMAPS; return true; }
        if (ch == 'h') { m->request = QREQ_PICKER_HELP; return true; }
        if (ch == 't') { m->request = QREQ_TERMINAL; return true; }
        if (ch == 'w') { m->request = QREQ_SAVE; return true; }
        if (ch == 'q') { m->request = QREQ_QUIT; return true; }
        return true;
    }
    if (m->pending == 's')
    {
        /* Leader s: workspace search. s w greps the word (or the visual
         * selection) under the cursor instead of opening an empty prompt. */
        m->pending = 0;
        if (ch == 'w') m->request = QREQ_PICKER_GREP_WORD;
        return true;
    }
    if (m->pending == 3)
    {
        m->pending = 0;
        if (ch == 'n') ss_add_next(ss, d);
        else if (ch == 'a') ss_add_all(ss, d, 0u, 0u);
        else if (ch == 'c') { ss_collapse_primary(ss); ss_sync_doc(ss, d); }
        return true;
    }

    if (m->pending == 'r')
    {
        if (ch) replace_char(m, d, ss, ch, now);
        else m->pending = 0;
        return true;
    }
    if (m->pending == 'f' || m->pending == 'F' || m->pending == 't' || m->pending == 'T')
    {
        if (ch)
        {
            m->last_find_char = ch;
            m->last_find_dir = (m->pending == 'f' || m->pending == 't') ? 1 : -1;
            m->last_find_till = (m->pending == 't' || m->pending == 'T');
            if (m->op) operator_motion(m, d, ss, m->pending, now);
            else
            {
                uint32_t heads[QUILL_MAX_CURSORS];
                for (i = 0u; i < ss->count; ++i)
                    heads[i] = find_char(d, ss->c[i].head, ch, m->last_find_dir, m->last_find_till, count_or(m, 1u));
                for (i = 0u; i < ss->count; ++i)
                {
                    ss->c[i].head = heads[i];
                    if (m->mode == QMODE_NORMAL) ss->c[i].anchor = heads[i];
                }
                ss_sync_doc(ss, d);
                m->pending = 0;
                m->count = 0u;
            }
        }
        else m->pending = 0;
        return true;
    }
    if (m->pending == '"')
    {
        if (ch) m->reg = ch;
        m->pending = 0;
        return true;
    }
    if (m->pending == 'g')
    {
        m->pending = 0;
        if (ch == 'g')
        {
            uint32_t target = 0u;
            if (m->count > 0u)
            {
                uint32_t line = m->count - 1u;
                if (line >= d->q.nlines) line = d->q.nlines ? d->q.nlines - 1u : 0u;
                target = mu_quill_line_offset(&d->q, line);
            }
            if (m->op)
            {
                uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
                for (i = 0u; i < ss->count; ++i)
                {
                    uint32_t off = ss->c[i].head;
                    starts[i] = off < target ? off : target;
                    ends[i] = off > target ? off : target;
                }
                execute_op(m, d, ss, starts, ends, ss->count, true, now);
            }
            else
            {
                for (i = 0u; i < ss->count; ++i) ss->c[i].anchor = ss->c[i].head = target;
                ss_sync_doc(ss, d);
                m->count = 0u;
            }
            return true;
        }
        if (ch == 'u' || ch == 'U' || ch == '~')
        {
            m->case_op = ch;
            m->op = 'g';
            return true;
        }
        if (ch == 'c')
        {
            m->op = 'C';
            return true;
        }
        if (ch == 'n')
        {
            uint32_t s, e;
            if (m->search[0] && quill_find(d, m->search, ss_primary(ss)->head, 1, &s, &e))
            {
                ss_single(ss, e);
                ss->c[0].anchor = s;
                ss_sync_doc(ss, d);
            }
            return true;
        }
        return true;
    }
    if (m->pending == 'z')
    {
        m->pending = 0;
        if (ch == 'z') m->request = QREQ_SCROLL_CENTER;
        else if (ch == 't') m->request = QREQ_SCROLL_TOP;
        else if (ch == 'b') m->request = QREQ_SCROLL_BOTTOM;
        return true;
    }

    if (m->op && m->pending == 0)
    {
        if (ch >= '1' && ch <= '9')
        {
            m->count = m->count * 10u + (uint32_t)(ch - '0');
            return true;
        }
        if (ch == '0' && m->count != 0u)
        {
            m->count *= 10u;
            return true;
        }
        if (ch == m->op)
        {
            m->op_motion_char = 0;     /* linewise: dd / cc / yy / >> / << */
            m->op_motion_count = 0u;
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            /* >> / << / == with count > 1 touch count lines; the count must be
             * consumed here or execute_op hands it to the indenter, which then
             * indents that many lines starting from the buffer start. */
            uint32_t extra = count_or(m, 1u);
            for (i = 0u; i < ss->count; ++i)
            {
                starts[i] = line_start(d, ss->c[i].head);
                mu_quill_cursor c = mu_quill_offset_to_cursor(&d->q, starts[i]);
                uint32_t lastline = c.line + extra - 1u;
                if (lastline >= d->q.nlines) lastline = d->q.nlines - 1u;
                /* End on the last CHARACTER, not the newline.  line_end() points
                 * AT '\n', and line_no() maps that offset to the FOLLOWING line,
                 * so ending there made '>>' indent the next line too. */
                uint32_t le = line_end(d, mu_quill_line_offset(&d->q, lastline));
                ends[i] = le > 0u ? le - 1u : 0u;
            }
            m->count = 0u;
            execute_op(m, d, ss, starts, ends, ss->count, true, now);
            return true;
        }
        if (ch == 'i' || ch == 'a')
        {
            m->pending = ch;
            return true;
        }
        if (ch == 'f' || ch == 'F' || ch == 't' || ch == 'T')
        {
            m->pending = ch;
            return true;
        }
        if (ch == 'g')
        {
            m->pending = 'g';
            return true;
        }
        if (ch && operator_motion(m, d, ss, ch, now))
            return true;
        if (key == GLFW_KEY_LEFT) return operator_motion(m, d, ss, 'h', now);
        if (key == GLFW_KEY_RIGHT) return operator_motion(m, d, ss, 'l', now);
        if (key == GLFW_KEY_UP) return operator_motion(m, d, ss, 'k', now);
        if (key == GLFW_KEY_DOWN) return operator_motion(m, d, ss, 'j', now);
        m->op = 0;
        m->case_op = 0;
        return true;
    }
    if (m->op && (m->pending == 'i' || m->pending == 'a'))
    {
        bool around = m->pending == 'a';
        m->pending = 0;
        if (ch) operator_textobj(m, d, ss, ch, around, now);
        return true;
    }

    if (ch >= '1' && ch <= '9')
    {
        m->count = m->count * 10u + (uint32_t)(ch - '0');
        return true;
    }
    if (ch == '0' && m->count != 0u)
    {
        m->count = m->count * 10u;
        return true;
    }

    if (ch == ' ')
    {
        m->pending = 1;
        return true;
    }
    if (ch == '"')
    {
        m->pending = '"';
        return true;
    }
    if (ch == 'd' || ch == 'c' || ch == 'y' || ch == '>' || ch == '<')
    {
        if (m->mode == QMODE_VISUAL || m->mode == QMODE_VISUAL_LINE || m->mode == QMODE_VISUAL_BLOCK)
        {
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            for (i = 0u; i < ss->count; ++i)
            {
                starts[i] = ss_min(&ss->c[i]);
                ends[i] = ss_max(&ss->c[i]);
                if (m->mode == QMODE_VISUAL && ends[i] < d->q.text_len) ends[i]++;
            }
            m->op = ch;
            execute_op(m, d, ss, starts, ends, ss->count, m->mode == QMODE_VISUAL_LINE, now);
            return true;
        }
        if (m->op == ch)
        {
            uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
            for (i = 0u; i < ss->count; ++i)
            {
                starts[i] = line_start(d, ss->c[i].head);
                ends[i] = line_end(d, ss->c[i].head);
            }
            execute_op(m, d, ss, starts, ends, ss->count, true, now);
        }
        else
        {
            m->op = ch;
            m->pending = 0;
        }
        return true;
    }
    if (ch == 'g' || ch == 'z' || ch == 'r' || ch == 'f' || ch == 'F' || ch == 't' || ch == 'T')
    {
        m->pending = ch;
        return true;
    }
    if (ch == 'v' || ch == 'V' || (ctrl && key == GLFW_KEY_V))
    {
        if (ctrl && key == GLFW_KEY_V)
        {
            m->mode = QMODE_VISUAL_BLOCK;
            m->block_anchor = ss_primary(ss)->head;
        }
        else if (ch == 'V') m->mode = QMODE_VISUAL_LINE;
        else m->mode = QMODE_VISUAL;
        for (i = 0u; i < ss->count; ++i) ss->c[i].anchor = ss->c[i].head;
        visual_update(m, d, ss);
        return true;
    }
    if (ch == 'i' || ch == 'I' || ch == 'a' || ch == 'A' || ch == 'o' || ch == 'O')
    {
        simple_insert(m, d, ss, ch, now);
        return true;
    }
    if (ch == 's' || ch == 'S')
    {
        simple_insert(m, d, ss, ch, now);
        return true;
    }
    if (ch == 'x' || ch == 'X')
    {
        uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
        for (i = 0u; i < ss->count; ++i)
        {
            uint32_t off = ss->c[i].head;
            starts[i] = ch == 'x' ? off : (off > line_start(d, off) ? off - 1u : off);
            ends[i] = ch == 'x' ? off + 1u : off;
            if (ends[i] > d->q.text_len) ends[i] = d->q.text_len;
        }
        delete_ranges(m, d, ss, starts, ends, ss->count, false, false, now);
        dot_keys(m, ch == 'x' ? "x" : "X");
        return true;
    }
    if (ch == 'D' || ch == 'C' || ch == 'Y')
    {
        uint32_t starts[QUILL_MAX_CURSORS], ends[QUILL_MAX_CURSORS];
        for (i = 0u; i < ss->count; ++i)
        {
            starts[i] = ss->c[i].head;
            ends[i] = line_end(d, starts[i]);
        }
        m->op = ch == 'D' ? 'd' : (ch == 'C' ? 'c' : 'y');
        m->op_motion_char = '$';
        m->op_motion_count = 0u;
        execute_op(m, d, ss, starts, ends, ss->count, false, now);
        return true;
    }
    if (ch == 'p' || ch == 'P')
    {
        put_register(m, d, ss, ch == 'P', now);
        return true;
    }
    if (ch == 'J')
    {
        join_lines(m, d, ss, now);
        return true;
    }
    if (ch == '~')
    {
        toggle_case_char(m, d, ss, now);
        return true;
    }
    if (ch == 'u')
    {
        doc_undo(d);
        ss_from_doc(ss, d);
        return true;
    }
    if (ch == '.')
    {
        dot_repeat(m, d, ss, now);
        return true;
    }
    if (ch == '/')
    {
        m->request = QREQ_SEARCH_FWD;
        return true;
    }
    if (ch == '?')
    {
        m->request = QREQ_SEARCH_REV;
        return true;
    }
    if (ch == '*') { search_word(m, d, ss, 1); return true; }
    if (ch == '#') { search_word(m, d, ss, -1); return true; }
    if (ch == 'n') { modal_search_next(m, d, ss, m->search_dir); return true; }
    if (ch == 'N') { modal_search_next(m, d, ss, -m->search_dir); return true; }
    if (ch == ':' ) { m->request = QREQ_PICKER_COMMANDS; return true; }

    if (ch == 'h' || ch == 'j' || ch == 'k' || ch == 'l' ||
        ch == 'w' || ch == 'b' || ch == 'e' || ch == 'W' || ch == 'B' || ch == 'E' ||
        ch == '0' || ch == '^' || ch == '$' || ch == '{' || ch == '}' || ch == '%' || ch == ';' || ch == ',' ||
        ch == 'G')
    {
        uint32_t heads[QUILL_MAX_CURSORS];
        bool lw, inc;
        for (i = 0u; i < ss->count; ++i)
            motion_target(d, ss->c[i].head, ch, count_or(m, 1u), m, &heads[i], &lw, &inc);
        for (i = 0u; i < ss->count; ++i)
        {
            ss->c[i].head = heads[i];
            if (m->mode == QMODE_NORMAL) ss->c[i].anchor = heads[i];
            ss->c[i].anim_valid = false;
        }
        visual_update(m, d, ss);
        m->count = 0u;
        return true;
    }
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT || key == GLFW_KEY_UP || key == GLFW_KEY_DOWN ||
        key == GLFW_KEY_HOME || key == GLFW_KEY_END)
    {
        int mk = key == GLFW_KEY_LEFT ? 'h' : key == GLFW_KEY_RIGHT ? 'l' :
                 key == GLFW_KEY_UP ? 'k' : key == GLFW_KEY_DOWN ? 'j' :
                 key == GLFW_KEY_HOME ? '0' : '$';
        uint32_t heads[QUILL_MAX_CURSORS];
        bool lw, inc;
        for (i = 0u; i < ss->count; ++i)
        {
            if (key == GLFW_KEY_LEFT)
                heads[i] = arrow_left(d, ss->c[i].head, count_or(m, 1u));
            else if (key == GLFW_KEY_RIGHT)
                heads[i] = arrow_right(d, ss->c[i].head, count_or(m, 1u), false);
            else
                motion_target(d, ss->c[i].head, mk, count_or(m, 1u), m, &heads[i], &lw, &inc);
        }
        for (i = 0u; i < ss->count; ++i)
        {
            ss->c[i].head = heads[i];
            if (m->mode == QMODE_NORMAL) ss->c[i].anchor = heads[i];
            ss->c[i].anim_valid = false;
        }
        visual_update(m, d, ss);
        m->count = 0u;
        return true;
    }
    return false;
}

bool modal_key(ModalState* m, Document* d, SelectionSet* ss, int key, int mods, uint64_t now)
{
    return modal_key_ch(m, d, ss, key_ascii(key, mods), key, mods, now);
}

bool modal_printable(ModalState* m, Document* d, SelectionSet* ss, unsigned int cp, uint64_t now)
{
    int ch = cp < 128u ? (int)cp : 0;
    if (m->mode == QMODE_INSERT)
        return modal_char(m, d, ss, cp, now);
    return modal_key_ch(m, d, ss, ch, 0, 0, now);
}

void modal_init(ModalState* m)
{
    memset(m, 0, sizeof *m);
    m->mode = QMODE_NORMAL;
    m->search_dir = 1;
}
