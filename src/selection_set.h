#ifndef QUILL_SELECTION_SET_H
#define QUILL_SELECTION_SET_H

#include "document.h"

typedef struct SelCursor
{
    uint32_t anchor, head, prefer_col;
    float    anim_x, anim_y;
    uint64_t anim_start;
    bool     anim_valid;
} SelCursor;

typedef struct SelectionSet
{
    SelCursor c[QUILL_MAX_CURSORS];
    uint32_t  count;
    uint32_t  primary;
} SelectionSet;

void ss_init(SelectionSet* ss);
void ss_single(SelectionSet* ss, uint32_t off);
void ss_from_doc(SelectionSet* ss, const Document* d);
void ss_sync_doc(const SelectionSet* ss, Document* d);
void ss_normalize(SelectionSet* ss);
void ss_add(SelectionSet* ss, uint32_t anchor, uint32_t head);
void ss_remove_last(SelectionSet* ss);
void ss_collapse_primary(SelectionSet* ss);
SelCursor* ss_primary(SelectionSet* ss);
const SelCursor* ss_primary_const(const SelectionSet* ss);
uint32_t ss_min(const SelCursor* c);
uint32_t ss_max(const SelCursor* c);
void ss_word_at(SelectionSet* ss, Document* d, uint32_t off);
bool ss_add_next(SelectionSet* ss, Document* d);
bool ss_add_all(SelectionSet* ss, Document* d, uint32_t range_start, uint32_t range_end);
bool ss_replace_all(SelectionSet* ss, Document* d, const char* text, uint32_t len, uint64_t now);

#endif
