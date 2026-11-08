#ifndef QUILL_CONFIG_H
#define QUILL_CONFIG_H

#include "quill.h"

typedef enum MotionStyle { MOTION_EASE = 0, MOTION_LINEAR, MOTION_INSTANT } MotionStyle;
typedef enum BackgroundFit { BG_FIT_COVER = 0, BG_FIT_CONTAIN, BG_FIT_STRETCH } BackgroundFit;

#define QUILL_BG_PATH 512u

typedef struct MotionConfig
{
    MotionStyle style;
    uint32_t    scroll_ms;
    uint32_t    cursor_ms;
    uint32_t    blink_ms;
    uint32_t    picker_ms;
    uint32_t    scroll_far_lines;
    float       trail_size;
    bool        smooth_blink;
    bool        animate_insert;
    uint32_t    vfx_modes;
    float       vfx_opacity;
    float       vfx_lifetime;
    float       vfx_highlight_lifetime;
    float       vfx_density;
    float       vfx_speed;
    float       vfx_phase;
    float       vfx_curl;
    bool        reduced_motion;
    /* Appearance. Defaults are the zero-cost path: no image, opacity 1. */
    char        background_image[QUILL_BG_PATH];
    BackgroundFit background_fit;
    float       editor_opacity;
    /* Theme: palette file; empty keeps the built-in Noir. */
    char        theme[QUILL_BG_PATH];
    /* Session: reopen last buffers + cursor/scroll on launch. */
    bool        session_restore;
    char        session_file[QUILL_BG_PATH];
} MotionConfig;

void config_defaults(MotionConfig* cfg);
void config_load_file(MotionConfig* cfg, const char* path);

#endif
