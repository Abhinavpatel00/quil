#ifndef QUILL_THEME_H
#define QUILL_THEME_H

#include "quill.h"

/* The active palette. Starts as the built-in Noir; a theme file
 * can override any subset of keys. Reading it costs the same as the old
 * compile-time constant, so themes add no per-frame overhead. */
extern QuillTheme QUILL_THEME;

/* Restore the built-in palette. */
void theme_reset(void);

/* Load a palette file into QUILL_THEME. Keys that are absent keep the
 * built-in default. NULL/empty path just resets to the default. Returns
 * false only if a non-empty path could not be opened. */
bool theme_load_file(const char* path);

#endif
