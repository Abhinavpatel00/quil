#ifndef QUILL_PICKER_H
#define QUILL_PICKER_H

#include "document.h"
#include "font.h"
#include "frame.h"
#include <pthread.h>

typedef enum PickerSource
{
    PSRC_FILES = 0, PSRC_BUFFERS, PSRC_GREP, PSRC_SYMBOLS, PSRC_PROJECT_SYMBOLS,
    PSRC_RECENT, PSRC_COMMANDS, PSRC_KEYMAPS, PSRC_HELP
} PickerSource;

typedef struct PickerItem
{
    char     label[256];
    char     path[512];
    uint32_t line;
    uint32_t offset;
    int32_t  score;
    uint8_t  kind;
    uint8_t  pad[3];
} PickerItem;

typedef struct Picker
{
    bool         open;
    bool         accepted;
    PickerSource source;
    char         query[256];
    uint32_t     query_len;
    uint32_t     selected;
    uint32_t     count;
    PickerItem   items[QUILL_PICKER_RESULTS];
    PickerItem   base[QUILL_PICKER_RESULTS];
    uint32_t     base_count;
    PickerItem   result;
    char         preview[4096];
    Document*    doc;
    char         root[512];

    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            thread_started;
    bool            thread_run;
    bool            job_pending;
    bool            job_done;
    uint32_t        generation;
    uint32_t        job_gen;
    PickerSource    job_source;
    char            job_query[256];
    PickerItem      job_items[QUILL_PICKER_RESULTS];
    uint32_t        job_count;
} Picker;

void picker_init(Picker* p, Document* doc, const char* root);
void picker_destroy(Picker* p);
void picker_set_doc(Picker* p, Document* doc);
void picker_open(Picker* p, PickerSource src);
void picker_open_items(Picker* p, PickerSource src, const PickerItem* items, uint32_t n);
void picker_close(Picker* p);
void picker_poll(Picker* p);
bool picker_key(Picker* p, int key, int mods);
void picker_char(Picker* p, unsigned int cp);
const PickerItem* picker_selected(const Picker* p);
void picker_render(const Picker* p, FontSystem* fs, DrawList* dl, uint32_t width, uint32_t height,
                   float editor_opacity);
int picker_fuzzy_score(const char* query, const char* text);
int picker_hit_test(const Picker* p, uint32_t width, uint32_t height,
                    float font_line_height, float x, float y);

#endif
