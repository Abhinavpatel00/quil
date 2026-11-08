#ifndef QUILL_DOCUMENT_H
#define QUILL_DOCUMENT_H

#include "quill.h"
#include <tree_sitter/api.h>

typedef struct Syntax Syntax;

typedef struct UndoOp
{
    uint32_t off, old_len, new_len;
    uint32_t old_pay, new_pay;
    uint32_t anchor_before, cursor_before;
    uint32_t anchor_after, cursor_after;
    uint64_t time_ms;
    uint32_t group;
} UndoOp;

typedef struct Document
{
    mu_quill        q;
    mu_vm_arena     payload;
    mu_small_vec    undo;
    mu_small_vec    redo;
    UndoOp          undo_inline[64];
    UndoOp          redo_inline[64];
    uint32_t        anchor, cursor;
    uint32_t        prefer_col;
    /* Per-buffer view state, saved/restored on buffer switch and by session
     * restore. Zero-initialized by doc_init. */
    float           view_top;
    float           scroll_x;
    char            path[512];
    bool            dirty;
    uint64_t        version;
    Syntax*         syntax;
    uint32_t        active_group;
    uint32_t        group_counter;
} Document;

bool doc_init(Document* d, size_t reserve_bytes);
void doc_destroy(Document* d);
bool doc_load_file(Document* d, const char* path);
bool doc_save_file(Document* d, const char* path);
bool doc_set_text(Document* d, const char* text, uint32_t len);

uint32_t doc_sel_start(const Document* d);
uint32_t doc_sel_end(const Document* d);
void doc_set_cursor(Document* d, uint32_t off, bool extend);
void doc_select_all(Document* d);
void doc_move_line(Document* d, int delta, bool extend);
void doc_move_col(Document* d, int delta, bool extend);
void doc_move_word(Document* d, int delta, bool extend);
void doc_home(Document* d, bool extend);
void doc_end(Document* d, bool extend);

bool doc_replace(Document* d, uint32_t off, uint32_t len,
                 const char* text, uint32_t nlen, uint64_t now_ms);
bool doc_insert(Document* d, uint32_t off, const char* text, uint32_t nlen, uint64_t now_ms);
bool doc_erase(Document* d, uint32_t off, uint32_t len, uint64_t now_ms);
bool doc_undo(Document* d);
bool doc_redo(Document* d);
void doc_begin_group(Document* d);
void doc_end_group(Document* d);

TSPoint doc_point(Document* d, uint32_t off);
void doc_attach_syntax(Document* d, Syntax* s);

#endif
