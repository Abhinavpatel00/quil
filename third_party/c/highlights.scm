; C highlights for Quill. Avoids #match? predicates (the vendored Tree-sitter
; C API does not evaluate them), so semantic captures are explicit.

(identifier) @variable

"break" @keyword
"case" @keyword
"const" @keyword
"continue" @keyword
"default" @keyword
"do" @keyword
"else" @keyword
"enum" @keyword
"extern" @keyword
"for" @keyword
"if" @keyword
"inline" @keyword
"return" @keyword
"sizeof" @keyword
"static" @keyword
"struct" @keyword
"switch" @keyword
"typedef" @keyword
"union" @keyword
"volatile" @keyword
"while" @keyword

"#define" @keyword
"#elif" @keyword
"#else" @keyword
"#endif" @keyword
"#if" @keyword
"#ifdef" @keyword
"#ifndef" @keyword
"#include" @keyword
(preproc_directive) @keyword

"--" @operator
"-" @operator
"-=" @operator
"->" @operator
"=" @operator
"!=" @operator
"*" @operator
"&" @operator
"&&" @operator
"+" @operator
"++" @operator
"+=" @operator
"<" @operator
"==" @operator
">" @operator
"||" @operator
"/" @operator
"/=" @operator
"%" @operator
"%=" @operator
"<<" @operator
">>" @operator
"<<=" @operator
">>=" @operator
"^" @operator
"^=" @operator
"|" @operator
"|=" @operator
"~" @operator
"!" @operator
"?" @operator
":" @operator

"." @delimiter
"," @delimiter
";" @delimiter
"(" @punctuation.bracket
")" @punctuation.bracket
"[" @punctuation.bracket
"]" @punctuation.bracket
"{" @punctuation.bracket
"}" @punctuation.bracket

(string_literal) @string
(system_lib_string) @string
(char_literal) @string
(escape_sequence) @escape

(null) @constant
(number_literal) @number

(field_identifier) @property
(statement_identifier) @label
(type_identifier) @type
(primitive_type) @type
(sized_type_specifier) @type

(call_expression
  function: (identifier) @function)
(call_expression
  function: (field_expression
    field: (field_identifier) @function))
(function_declarator
  declarator: (identifier) @function)
(function_declarator
  declarator: (pointer_declarator
    declarator: (identifier) @function))
(preproc_function_def
  name: (identifier) @function.special)
(preproc_def
  name: (identifier) @constant)
(enumerator
  name: (identifier) @constant)

(comment) @comment
