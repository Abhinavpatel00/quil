#ifndef QUILL_MODAL_H
#define QUILL_MODAL_H

#include "document.h"
#include "selection_set.h"

struct GLFWwindow;

typedef enum QRequest
{
    QREQ_NONE = 0,
    QREQ_PICKER_FILES, QREQ_PICKER_BUFFERS, QREQ_PICKER_GREP,
    QREQ_PICKER_SYMBOLS, QREQ_PICKER_PROJECT_SYMBOLS, QREQ_PICKER_RECENT,
    QREQ_PICKER_COMMANDS, QREQ_PICKER_KEYMAPS, QREQ_PICKER_HELP,
    QREQ_SEARCH_FWD, QREQ_SEARCH_REV,
    QREQ_SAVE, QREQ_QUIT, QREQ_TOGGLE_LINE_NUMBERS,
    QREQ_ZOOM_IN, QREQ_ZOOM_OUT,
    QREQ_SCROLL_CENTER, QREQ_SCROLL_TOP, QREQ_SCROLL_BOTTOM, QREQ_CYCLE_VFX,
    QREQ_TERMINAL
} QRequest;

typedef struct DotChange
{
    char     keys[256];
    uint32_t len;
    bool     recording_insert;
    bool     replaying;
} DotChange;

typedef struct ModalState
{
    QMode    mode;
    uint32_t count;
    int      op;
    int      pending;
    int      reg;
    int      case_op;
    int      last_find_char, last_find_dir, last_find_till;
    /* Motion key that terminated the last operator, kept so '.' can record and
     * replay "operator+motion" instead of a bare operator. 0 = linewise (dd/cc). */
    int      op_motion_char;
    uint32_t op_motion_count;
    uint32_t block_anchor;
    char     search[256];
    int      search_dir;
    QRequest request;
    DotChange dot;
    char     reg_data[QUILL_MAX_REGISTERS][QUILL_REG_CAP];
    uint32_t reg_len[QUILL_MAX_REGISTERS];
    bool     reg_linewise[QUILL_MAX_REGISTERS];
    struct GLFWwindow* window;
    /* Auto-save buffer when leaving Insert mode (config: auto_save_on_mode_change). */
    bool     auto_save_on_mode_change;
} ModalState;

void modal_init(ModalState* m);
bool modal_key(ModalState* m, Document* d, SelectionSet* ss, int key, int mods, uint64_t now);
bool modal_char(ModalState* m, Document* d, SelectionSet* ss, unsigned int cp, uint64_t now);
bool modal_printable(ModalState* m, Document* d, SelectionSet* ss, unsigned int cp, uint64_t now);
bool modal_wants_char(const ModalState* m);
void modal_scroll(ModalState* m, Document* d, SelectionSet* ss, int lines, uint32_t visible_rows);
const char* modal_mode_name(const ModalState* m);
void modal_search_set(ModalState* m, const char* query, int dir);
bool modal_search_next(ModalState* m, Document* d, SelectionSet* ss, int dir);
void modal_enter_insert(ModalState* m, Document* d, SelectionSet* ss, int how, uint64_t now);
void modal_escape(ModalState* m, Document* d, SelectionSet* ss);

#endif
