#ifndef QUILL_BUFFERS_H
#define QUILL_BUFFERS_H

#include "document.h"

typedef struct BufferManager
{
    Document* docs[QUILL_MAX_BUFFERS];
    uint32_t  count;
    uint32_t  current;
    char      recent[QUILL_MAX_BUFFERS][512];
    uint32_t  nrecent;
} BufferManager;

void bm_init(BufferManager* bm);
void bm_destroy(BufferManager* bm);
Document* bm_current(BufferManager* bm);
Document* bm_open(BufferManager* bm, const char* path);
bool bm_switch(BufferManager* bm, uint32_t idx);
bool bm_close(BufferManager* bm, uint32_t idx);
void bm_touch_recent(BufferManager* bm, const char* path);

#endif
