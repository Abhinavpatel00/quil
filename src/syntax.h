#ifndef QUILL_SYNTAX_H
#define QUILL_SYNTAX_H

#include "quill.h"
#include <tree_sitter/api.h>

typedef struct HighlightSpan
{
    uint32_t start, end;
    uint8_t  color;
    uint8_t  pad[3];
} HighlightSpan;

typedef struct Syntax Syntax;

Syntax* syntax_create_for_path(const char* path);
void syntax_destroy(Syntax* s);
void syntax_reset(Syntax* s);
const char* syntax_name(const Syntax* s);
void syntax_edit(Syntax* s, uint32_t start_byte, uint32_t old_end_byte,
                 uint32_t new_end_byte, TSPoint start_point,
                 TSPoint old_end_point, TSPoint new_end_point);
bool syntax_parse(Syntax* s, mu_quill* q);
uint32_t syntax_highlight(Syntax* s, mu_quill* q, uint32_t start_byte,
                          uint32_t end_byte, HighlightSpan* out, uint32_t cap);


typedef struct QuillSymbol
{
    uint32_t line, offset;
    char     name[128];
    uint8_t  kind; /* 1 function, 2 type, 3 method, 4 variable, 5 module */
    uint8_t  pad[3];
} QuillSymbol;

uint32_t syntax_symbols(Syntax* s, mu_quill* q, QuillSymbol* out, uint32_t cap);

#endif
