#include "buffers.h"

void bm_init(BufferManager* bm)
{
    memset(bm, 0, sizeof *bm);
}

void bm_destroy(BufferManager* bm)
{
    uint32_t i;
    for (i = 0u; i < bm->count; ++i)
    {
        if (bm->docs[i])
        {
            doc_destroy(bm->docs[i]);
            MU_FREE(bm->docs[i]);
        }
    }
    memset(bm, 0, sizeof *bm);
}

Document* bm_current(BufferManager* bm)
{
    if (bm->count == 0u) return NULL;
    return bm->docs[bm->current < bm->count ? bm->current : 0u];
}

Document* bm_open(BufferManager* bm, const char* path)
{
    uint32_t i;
    Document* d;
    for (i = 0u; i < bm->count; ++i)
        if (strcmp(bm->docs[i]->path, path) == 0)
        {
            bm->current = i;
            bm_touch_recent(bm, path);
            return bm->docs[i];
        }
    if (bm->count >= QUILL_MAX_BUFFERS) return NULL;
    d = (Document*)MU_CALLOC(1u, sizeof *d);
    if (!d) return NULL;
    if (!doc_init(d, 64u << 10))
    {
        MU_FREE(d);
        return NULL;
    }
    if (path && path[0])
    {
        if (!doc_load_file(d, path))
        {
            /* Keep an empty buffer with the requested path. */
            snprintf(d->path, sizeof d->path, "%s", path);
        }
        bm_touch_recent(bm, path);
    }
    bm->docs[bm->count] = d;
    bm->current = bm->count;
    bm->count++;
    return d;
}

bool bm_switch(BufferManager* bm, uint32_t idx)
{
    if (idx >= bm->count) return false;
    bm->current = idx;
    return true;
}

bool bm_close(BufferManager* bm, uint32_t idx)
{
    uint32_t i;
    if (idx >= bm->count) return false;
    doc_destroy(bm->docs[idx]);
    MU_FREE(bm->docs[idx]);
    for (i = idx; i + 1u < bm->count; ++i)
        bm->docs[i] = bm->docs[i + 1u];
    bm->count--;
    if (bm->count == 0u) bm->current = 0u;
    else if (bm->current >= bm->count) bm->current = bm->count - 1u;
    return true;
}

void bm_touch_recent(BufferManager* bm, const char* path)
{
    uint32_t i, j;
    if (!path || !path[0]) return;
    for (i = 0u; i < bm->nrecent; ++i)
        if (strcmp(bm->recent[i], path) == 0) break;
    if (i == bm->nrecent && bm->nrecent < QUILL_MAX_BUFFERS)
        bm->nrecent++;
    for (j = i; j > 0u; --j)
        memcpy(bm->recent[j], bm->recent[j - 1u], sizeof bm->recent[j]);
    snprintf(bm->recent[0], sizeof bm->recent[0], "%s", path);
}
