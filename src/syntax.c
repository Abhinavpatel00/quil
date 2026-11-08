#include "syntax.h"
#include "syntax_queries.h"

const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);

typedef struct LangDef
{
    const char* ext;
    const char* name;
    const TSLanguage* (*lang)(void);
    const char* query;
} LangDef;

static const LangDef LANGS[] = {
    { "c",    "C",          tree_sitter_c,          quill_query_c },
    { "h",    "C",          tree_sitter_c,          quill_query_c },
    { "json", "JSON",       tree_sitter_json,       quill_query_json },
    { "rs",   "Rust",       tree_sitter_rust,       quill_query_rust },
    { "py",   "Python",     tree_sitter_python,     quill_query_python },
    { "js",   "JavaScript", tree_sitter_javascript, quill_query_javascript },
    { "jsx",  "JavaScript", tree_sitter_javascript, quill_query_javascript },
    { "mjs",  "JavaScript", tree_sitter_javascript, quill_query_javascript },
    { "cjs",  "JavaScript", tree_sitter_javascript, quill_query_javascript },
};

struct Syntax
{
    const LangDef* def;
    TSParser*      parser;
    TSTree*        tree;
    TSQuery*       query;
    TSQueryCursor* cursor;
    uint8_t        capture_color[256];
    uint8_t        cap_is_var[256];   /* capture is plain @variable (identifier) */
    uint32_t       keys[QUILL_MAX_SPANS];
    uint32_t       order[QUILL_MAX_SPANS];
    uint64_t       range_keys[QUILL_MAX_SPANS];
    uint16_t       pri[QUILL_MAX_SPANS];
    HighlightSpan  tmp[QUILL_MAX_SPANS];
    /* Result cache: idle blink/animation redraws re-query an unchanged view
     * every frame. Keyed by range + tree versions so edits, reparses, and
     * scrolling miss, but identical-view frames reuse the processed spans. */
    HighlightSpan  cache[QUILL_MAX_SPANS];
    uint32_t       cache_n, cache_start, cache_end;
    uint64_t       cache_edit, cache_parsed;
    bool           cache_valid;
    uint64_t       edit_version;
    uint64_t       parsed_version;
    bool           needs_parse;
};

/* ALL_CAPS identifier heuristic: C macros/enum constants used at a reference
 * site are plain (identifier) nodes to Tree-sitter (no semantic resolution),
 * so they would otherwise color as variables. The universal C convention is
 * that ALL_CAPS names are constants; reclassify them deterministically here
 * instead of relying on a query #match? predicate the vendored API ignores. */
static bool is_all_caps(const char* s, uint32_t len)
{
    uint32_t i;
    if (len == 0u || !(s[0] >= 'A' && s[0] <= 'Z')) return false;
    for (i = 1u; i < len; ++i)
    {
        char ch = s[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
            return false;
    }
    return true;
}

static uint8_t color_for_capture(const char* name, uint32_t len)
{
    char buf[64];
    uint32_t n = len < sizeof buf - 1u ? len : (uint32_t)sizeof buf - 1u;
    memcpy(buf, name, n);
    buf[n] = 0;
    if (strstr(buf, "keyword")) return QC_KEYWORD;
    if (strstr(buf, "function") || strstr(buf, "method")) return QC_FUNCTION;
    if (strstr(buf, "string") || strstr(buf, "regex")) return QC_STRING;
    if (strstr(buf, "comment")) return QC_COMMENT;
    if (strstr(buf, "number") || strstr(buf, "float")) return QC_NUMBER;
    if (strstr(buf, "boolean")) return QC_CONSTANT;
    if (strstr(buf, "type") || strstr(buf, "class") || strstr(buf, "interface")) return QC_TYPE;
    if (strstr(buf, "constant") || strstr(buf, "null") || strstr(buf, "none")) return QC_CONSTANT;
    if (strstr(buf, "property") || strstr(buf, "field")) return QC_PROPERTY;
    if (strstr(buf, "attribute") || strstr(buf, "annotation")) return QC_ATTRIBUTE;
    if (strstr(buf, "operator")) return QC_OPERATOR;
    if (strstr(buf, "punctuation") || strstr(buf, "delimiter")) return QC_PUNCTUATION;
    if (strstr(buf, "label")) return QC_LABEL;
    if (strstr(buf, "escape")) return QC_ESCAPE;
    if (strstr(buf, "variable") || strstr(buf, "parameter")) return QC_VARIABLE;
    if (strstr(buf, "error")) return QC_ERROR;
    return QC_DEFAULT;
}

static const char* ts_read(void* payload, uint32_t byte_offset, TSPoint position,
                           uint32_t* bytes_read)
{
    mu_quill* q = (mu_quill*)payload;
    uint32_t n;
    const uint8_t* p;
    (void)position;
    p = mu_quill_span(q, byte_offset, &n);
    *bytes_read = n;
    return (const char*)p;
}

Syntax* syntax_create_for_path(const char* path)
{
    const char* dot;
    const char* ext;
    Syntax* s;
    uint32_t i, nc, qerr_off;
    TSQueryError qerr;

    if (!path) return NULL;
    dot = strrchr(path, '.');
    if (!dot) return NULL;
    ext = dot + 1;
    for (i = 0u; i < sizeof LANGS / sizeof LANGS[0]; ++i)
        if (strcmp(ext, LANGS[i].ext) == 0)
            break;
    if (i == sizeof LANGS / sizeof LANGS[0])
        return NULL;

    s = (Syntax*)MU_CALLOC(1u, sizeof *s);
    if (!s) return NULL;
    s->def = &LANGS[i];
    s->parser = ts_parser_new();
    s->cursor = ts_query_cursor_new();
    if (!s->parser || !s->cursor ||
        !ts_parser_set_language(s->parser, s->def->lang()))
    {
        syntax_destroy(s);
        return NULL;
    }
    s->query = ts_query_new(s->def->lang(), s->def->query,
                            (uint32_t)strlen(s->def->query), &qerr_off, &qerr);
    if (!s->query)
    {
        /* Highlighting is optional; parsing still works without a query. */
        s->query = NULL;
    }
    else
    {
        nc = ts_query_capture_count(s->query);
        if (nc > 256u) nc = 256u;
        for (i = 0u; i < nc; ++i)
        {
            uint32_t len;
            const char* name = ts_query_capture_name_for_id(s->query, i, &len);
            s->capture_color[i] = color_for_capture(name, len);
            s->cap_is_var[i] = (len == 8u && memcmp(name, "variable", 8u) == 0) ? 1u : 0u;
        }
    }
    s->needs_parse = true;
    return s;
}

void syntax_destroy(Syntax* s)
{
    if (!s) return;
    if (s->tree) ts_tree_delete(s->tree);
    if (s->query) ts_query_delete(s->query);
    if (s->cursor) ts_query_cursor_delete(s->cursor);
    if (s->parser) ts_parser_delete(s->parser);
    MU_FREE(s);
}

void syntax_reset(Syntax* s)
{
    if (!s) return;
    if (s->tree)
    {
        ts_tree_delete(s->tree);
        s->tree = NULL;
    }
    s->edit_version++;
    s->needs_parse = true;
}

const char* syntax_name(const Syntax* s)
{
    return s && s->def ? s->def->name : "Text";
}

void syntax_edit(Syntax* s, uint32_t start_byte, uint32_t old_end_byte,
                 uint32_t new_end_byte, TSPoint start_point,
                 TSPoint old_end_point, TSPoint new_end_point)
{
    TSInputEdit e;
    if (!s || !s->tree) return;
    e.start_byte = start_byte;
    e.old_end_byte = old_end_byte;
    e.new_end_byte = new_end_byte;
    e.start_point = start_point;
    e.old_end_point = old_end_point;
    e.new_end_point = new_end_point;
    ts_tree_edit(s->tree, &e);
    s->edit_version++;
    s->needs_parse = true;
}

bool syntax_parse(Syntax* s, mu_quill* q)
{
    TSInput input;
    TSTree* next;
    if (!s) return false;
    if (s->tree && !s->needs_parse) return true;
    input.payload = q;
    input.read = ts_read;
    input.encoding = TSInputEncodingUTF8;
    next = ts_parser_parse(s->parser, s->tree, input);
    if (!next)
        return false;
    if (s->tree)
        ts_tree_delete(s->tree);
    s->tree = next;
    s->parsed_version = s->edit_version;
    s->needs_parse = false;
    return true;
}

uint32_t syntax_highlight(Syntax* s, mu_quill* q, uint32_t start_byte,
                          uint32_t end_byte, HighlightSpan* out, uint32_t cap)
{
    TSQueryMatch m;
    TSNode root;
    uint32_t n = 0u, i, priority = 0u;

    if (!s || !s->tree || !s->query || cap == 0u)
        return 0u;
    if (s->cache_valid && s->cache_start == start_byte && s->cache_end == end_byte &&
        s->cache_edit == s->edit_version && s->cache_parsed == s->parsed_version)
    {
        uint32_t m = s->cache_n < cap ? s->cache_n : cap;
        memcpy(out, s->cache, (size_t)m * sizeof out[0]);
        return m;
    }
    /* Optimistic highlighting: do NOT blank while a reparse is pending.
     * syntax_edit() already called ts_tree_edit(), which shifts every node's
     * byte range into the new buffer coordinates, so querying the edit-adjusted
     * tree keeps existing tokens colored and correctly aligned during the
     * debounce window. Only the freshly edited span is briefly uncolored until
     * the incremental reparse lands. Blanking here (returning 0) was the cause
     * of the whole-region color flicker on every keystroke. */
    root = ts_tree_root_node(s->tree);
    ts_query_cursor_set_byte_range(s->cursor, start_byte, end_byte);
    ts_query_cursor_exec(s->cursor, s->query, root);
    while (ts_query_cursor_next_match(s->cursor, &m))
    {
        for (i = 0u; i < m.capture_count; ++i)
        {
            const TSQueryCapture* c = &m.captures[i];
            uint32_t a = ts_node_start_byte(c->node);
            uint32_t b = ts_node_end_byte(c->node);
            if (a < start_byte) a = start_byte;
            if (b > end_byte) b = end_byte;
            if (a >= b || n >= cap) continue;
            out[n].start = a;
            out[n].end = b;
            out[n].color = c->index < 256u ? s->capture_color[c->index] : QC_DEFAULT;
            if (c->index < 256u && s->cap_is_var[c->index])
            {
                uint32_t raw_a = ts_node_start_byte(c->node);
                uint32_t raw_b = ts_node_end_byte(c->node);
                uint32_t tlen = raw_b - raw_a;
                if (tlen >= 1u && tlen <= 64u && raw_a >= start_byte && raw_b <= end_byte)
                {
                    char tok[64];
                    mu_quill_text(q, raw_a, tlen, tok);
                    if (is_all_caps(tok, tlen)) out[n].color = QC_CONSTANT;
                }
            }
            {
                uint32_t len = b - a;
                uint32_t plen = len > 65535u ? 65535u : len;
                uint32_t pri = m.pattern_index;
                /* Longer spans first; for equal length, later query patterns
                 * win (pattern_index), not tree-walk order. This makes
                 * function/keyword captures stable against (identifier)
                 * @variable regardless of match enumeration order. */
                s->keys[n] = ((65535u - plen) << 16) | pri;
                s->pri[n] = (uint16_t)pri;
                s->range_keys[n] = ((uint64_t)a << 32) | b;
            }
            (void)priority;
            ++n;
        }
    }
    (void)q;
    if (n > 1u)
    {
        uint32_t w = 0u;
        /* First collapse exact duplicate ranges by highest query priority.
         * Sorting by range makes duplicates adjacent regardless of length. */
        for (i = 0u; i < n; ++i) s->order[i] = i;
        mu_sort_indices_u64(s->range_keys, s->order, n);
        memcpy(s->tmp, out, (size_t)n * sizeof out[0]);
        for (i = 0u; i < n; ++i)
        {
            uint32_t idx = s->order[i];
            if (w > 0u && out[w - 1u].start == s->tmp[idx].start && out[w - 1u].end == s->tmp[idx].end)
            {
                if (s->pri[idx] >= s->pri[s->order[i - 1u]])
                    out[w - 1u].color = s->tmp[idx].color;
            }
            else
            {
                out[w] = s->tmp[idx];
                s->keys[w] = s->keys[idx];
                s->pri[w] = s->pri[idx];
                ++w;
            }
        }
        n = w;
        /* Paint order: longer spans first, higher priority later within a
         * length so shorter/inner captures override outer ones. */
        for (i = 0u; i < n; ++i) s->order[i] = i;
        mu_sort_indices_u32(s->keys, s->order, n);
        for (i = 0u; i < n; ++i) s->tmp[i] = out[s->order[i]];
        memcpy(out, s->tmp, (size_t)n * sizeof out[0]);
    }
    s->cache_n = n;
    s->cache_start = start_byte;
    s->cache_end = end_byte;
    s->cache_edit = s->edit_version;
    s->cache_parsed = s->parsed_version;
    memcpy(s->cache, out, (size_t)n * sizeof out[0]);
    s->cache_valid = true;
    return n;
}

static bool symbol_type(const char* t, uint8_t* kind)
{
    if (strcmp(t, "function_definition") == 0 || strcmp(t, "function_item") == 0 ||
        strcmp(t, "function_declaration") == 0 || strcmp(t, "function_signature") == 0)
    { *kind = 1; return true; }
    if (strcmp(t, "method_definition") == 0 || strcmp(t, "method_declaration") == 0)
    { *kind = 3; return true; }
    if (strcmp(t, "class_definition") == 0 || strcmp(t, "class_declaration") == 0 ||
        strcmp(t, "class_specifier") == 0 || strcmp(t, "struct_specifier") == 0 ||
        strcmp(t, "struct_item") == 0 || strcmp(t, "enum_specifier") == 0 ||
        strcmp(t, "enum_item") == 0 || strcmp(t, "enum_declaration") == 0 ||
        strcmp(t, "type_item") == 0 || strcmp(t, "type_alias_declaration") == 0 ||
        strcmp(t, "trait_item") == 0 || strcmp(t, "interface_declaration") == 0)
    { *kind = 2; return true; }
    if (strcmp(t, "impl_item") == 0 || strcmp(t, "mod_item") == 0 ||
        strcmp(t, "module") == 0 || strcmp(t, "namespace_definition") == 0)
    { *kind = 5; return true; }
    if (strcmp(t, "const_item") == 0 || strcmp(t, "static_item") == 0 ||
        strcmp(t, "lexical_declaration") == 0 || strcmp(t, "variable_declaration") == 0)
    { *kind = 4; return true; }
    return false;
}

static TSNode find_name_node(TSNode node)
{
    TSNode n = ts_node_child_by_field_name(node, "name", 4u);
    uint32_t i, nc;
    if (!ts_node_is_null(n)) return n;
    n = ts_node_child_by_field_name(node, "declarator", 10u);
    if (!ts_node_is_null(n)) return find_name_node(n);
    nc = ts_node_named_child_count(node);
    for (i = 0u; i < nc; ++i)
    {
        TSNode c = ts_node_named_child(node, i);
        const char* t = ts_node_type(c);
        if (strcmp(t, "identifier") == 0 || strcmp(t, "field_identifier") == 0 ||
            strcmp(t, "type_identifier") == 0 || strcmp(t, "property_identifier") == 0)
            return c;
    }
    return node;
}

static void symbol_walk(TSNode node, mu_quill* q, QuillSymbol* out, uint32_t cap, uint32_t* n)
{
    uint32_t i, nc;
    uint8_t kind;
    const char* t = ts_node_type(node);
    if (*n < cap && symbol_type(t, &kind))
    {
        TSNode name = find_name_node(node);
        uint32_t a = ts_node_start_byte(name), b = ts_node_end_byte(name);
        uint32_t len = b - a;
        if (len > 0u && len < sizeof out[*n].name)
        {
            QuillSymbol* s = &out[*n];
            mu_quill_text(q, a, len, s->name);
            s->name[len] = 0;
            s->line = ts_node_start_point(node).row;
            s->offset = ts_node_start_byte(node);
            s->kind = kind;
            (*n)++;
        }
    }
    nc = ts_node_named_child_count(node);
    for (i = 0u; i < nc && *n < cap; ++i)
        symbol_walk(ts_node_named_child(node, i), q, out, cap, n);
}

uint32_t syntax_symbols(Syntax* s, mu_quill* q, QuillSymbol* out, uint32_t cap)
{
    uint32_t n = 0u;
    if (!s || !s->tree || !q || cap == 0u) return 0u;
    symbol_walk(ts_tree_root_node(s->tree), q, out, cap, &n);
    return n;
}
