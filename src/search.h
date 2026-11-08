#ifndef QUILL_SEARCH_H
#define QUILL_SEARCH_H

#include "document.h"

typedef struct SearchRange { uint32_t start, end; } SearchRange;

void quill_word_range(Document* d, uint32_t off, uint32_t* start, uint32_t* end);
bool quill_find(Document* d, const char* query, uint32_t from, int dir,
                uint32_t* start, uint32_t* end);
uint32_t quill_find_all(Document* d, const char* query, uint32_t start, uint32_t end,
                        SearchRange* out, uint32_t cap);
bool quill_smartcase(const char* query);

#endif
