#ifndef QUILL_SESSION_H
#define QUILL_SESSION_H

#include "buffers.h"

/* Persist the open buffers (path + cursor/anchor + scroll) so the next launch
 * can restore them. Only named, on-disk buffers are saved; the current
 * buffer's live view must be flushed into its Document before calling. */
bool session_save(const char* path, const BufferManager* bm);

/* Reopen the saved buffers into bm and restore their cursor/anchor/scroll.
 * Sets bm->current to the saved active buffer. Missing files are skipped.
 * Returns the number of buffers restored (0 if no session). */
uint32_t session_load(const char* path, BufferManager* bm);

#endif
