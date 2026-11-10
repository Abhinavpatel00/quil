#ifndef QUILL_TERMINAL_H
#define QUILL_TERMINAL_H

#include "quill.h"
#include "font.h"
#include "frame.h"

/* Integrated terminal: a PTY-backed shell/command whose output is parsed by
 * the vendored libvterm into a cell grid and drawn with the editor's own
 * font/draw-list pipeline. No graphics protocols (kitty/sixel) — text only.
 *
 * Data layout: a flat row-major TermCell grid (AoS; every field is read per
 * cell during render) plus a fixed scrollback ring. All buffers are allocated
 * at create/resize time, never in the frame. */

typedef struct Terminal Terminal;

/* Spawn $SHELL (or a login shell) on a new PTY sized cols x rows. NULL on
 * failure. The child is reaped automatically when it exits. */
Terminal* terminal_create(uint32_t cols, uint32_t rows);
void      terminal_destroy(Terminal* t);

/* Resize the grid and tell the child (TIOCSWINSZ). Clears scrollback. */
void terminal_resize(Terminal* t, uint32_t cols, uint32_t rows);

/* Non-blocking: drain PTY output into libvterm, refresh the grid, reap the
 * child. Returns true if a redraw is needed. Call once per loop iteration
 * while a terminal is visible. */
bool terminal_poll(Terminal* t);

/* Input. terminal_send_char takes a layout-produced codepoint (char callback).
 * terminal_send_key takes a GLFW key code + GLFW modifier bits (key callback)
 * and encodes it via libvterm (arrows, Ctrl/Alt chords, function keys, ...). */
void terminal_send_char(Terminal* t, uint32_t codepoint);
void terminal_send_key(Terminal* t, int glfw_key, int glfw_mods);
/* Paste UTF-8 clipboard text, using bracketed paste when enabled by the
 * application running in the terminal. Newlines are encoded as Return. */
void terminal_paste(Terminal* t, const char* utf8, size_t len);

/* Mouse selection in viewport cell coordinates. The returned selection text
 * is UTF-8, allocated with MU_MALLOC; the caller owns and must MU_FREE it. */
void terminal_select_start(Terminal* t, uint32_t row, uint32_t col);
void terminal_select_extend(Terminal* t, uint32_t row, uint32_t col);
void terminal_select_clear(Terminal* t);
bool terminal_has_selection(const Terminal* t);
char* terminal_selection_text(const Terminal* t);

/* Scroll the viewport through the scrollback ring: positive = up (into
 * history), negative = down (toward live). Clamped to available history. */
void terminal_scroll(Terminal* t, int lines);

/* Render the grid (or a scrollback window) into dl over the pixel rect. */
void terminal_render(Terminal* t, FontSystem* fs, DrawList* dl,
                     float x, float y, float w, float h);

bool        terminal_exited(const Terminal* t);
const char* terminal_title(const Terminal* t);
uint32_t    terminal_cols(const Terminal* t);
uint32_t    terminal_rows(const Terminal* t);
bool        terminal_scrolled(const Terminal* t);

/* Read a live-screen cell (codepoint / 0xRRGGBB fg). For tests & tooling. */
uint32_t    terminal_cell_cp(const Terminal* t, uint32_t row, uint32_t col);
uint32_t    terminal_cell_fg(const Terminal* t, uint32_t row, uint32_t col);
/* Codepoint at the current viewport position (honors the scroll offset). */
uint32_t    terminal_view_cp(const Terminal* t, uint32_t row, uint32_t col);

#endif
