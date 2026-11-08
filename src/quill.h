/* quill.h -- shared types for the Quill Vulkan editor.
 * Data layout first; no LSP, no completion. Tree-sitter is the only
 * language service. */
#ifndef QUILL_H
#define QUILL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#if defined(__linux__)
#include <malloc.h>
#endif

#include "../third_party/mu_lib/mu/mu_quill.h"
#include "../third_party/mu_lib/mu/mu_vm_arena.h"
#include "../third_party/mu_lib/mu/mu_small_vec.h"
#include "../third_party/mu_lib/mu/mu_hash_table.h"
#include "../third_party/mu_lib/mu/mu_frame_allocator.h"
#include "../third_party/mu_lib/mu/mu_perf.h"
#include "../third_party/mu_lib/mu/mu_sort.h"

#define QUILL_MAX_LINE      8192u
#define QUILL_MAX_VISIBLE   256u
#define QUILL_MAX_SPANS     8192u
/* Glyph ring capacity. Measured worst-case usage with the diag harness across
 * 1280x800 .. 3840x2160 on sample.c, src/main.c, src/document.c and
 * src/render_vk.c is 2530 glyphs at 3840x2160 (src/main.c); 1280x800 uses 364.
 * The previous 16384 was ~6.5x the worst observed frame. 8192 keeps a 2.4x
 * margin over the worst case while still covering a full 4K viewport plus the
 * picker overlay, and halves the glyph vertex ring (3.00 -> 1.50 MiB) and the
 * DrawList glyph array (768 -> 384 KiB). emit_glyph() already drops quads past
 * the cap rather than overrunning, so an underestimate degrades instead of
 * corrupting. */
#define QUILL_MAX_GLYPHS    8192u
#define QUILL_MAX_SOLIDS    4096u
#define QUILL_TAB_WIDTH     4u
#define QUILL_ATLAS_SMALL_SIZE 1024u
#define QUILL_ATLAS_SIZE    2048u
#define QUILL_FRAMES        1u
#define QUILL_MAX_CURSORS   256u
#define QUILL_MAX_REGISTERS 128u
#define QUILL_REG_CAP       4096u
#define QUILL_PICKER_RESULTS 1024u
#define QUILL_PICKER_FILES  20000u
#define QUILL_MAX_BUFFERS   64u
/* VFX quad budget and per-mode particle cap.
 *
 * These are coupled: the old pair (MAX_VFX 4096, VFX_POOL 512) could not
 * coexist, because 11 trail modes x 512 particles = 5632 particles can be
 * alive at once while the draw list holds only 4096 quads. With several modes
 * enabled at once the list saturated and emit_vfx() silently discarded the
 * remainder, which is why multi-mode VFX looked like it was "not working" --
 * whole modes vanished, whichever happened to be rendered last.
 *
 * 256 particles per mode still covers a fast cursor sweep (a sweep of N cells
 * spawns ~N * density particles, capped at 256 per update, and they decay), and
 * 11 x 256 = 2816 fits inside 4096 with room for the 6 highlight modes.
 * The static assert below keeps the two from drifting apart again. */
#define QUILL_MAX_VFX       4096u
#define QUILL_VFX_POOL      256u

/* Cursor VFX bits. Neovide's six modes plus Quill extras. Multiple bits may
 * be enabled at once, matching Neovide's mode-list behavior. */
#define VFX_NONE       0u
#define VFX_RAILGUN    (1u << 0)
#define VFX_TORPEDO    (1u << 1)
#define VFX_PIXIEDUST  (1u << 2)
#define VFX_SONICBOOM  (1u << 3)
#define VFX_RIPPLE     (1u << 4)
#define VFX_WIREFRAME  (1u << 5)
#define VFX_COMET      (1u << 6)
#define VFX_EMBER      (1u << 7)
#define VFX_AURORA     (1u << 8)
#define VFX_GLITCH     (1u << 9)
#define VFX_PULSE      (1u << 10)
#define VFX_SNOW       (1u << 11)
#define VFX_CONFETTI   (1u << 12)
#define VFX_SPARK      (1u << 13)
#define VFX_SMOKE      (1u << 14)
#define VFX_SHOCKWAVE  (1u << 15)
#define VFX_HALO       (1u << 16)

#define VFX_TRAIL_COUNT 11u
#define VFX_HL_COUNT    6u

/* Trail + highlight quads must fit the draw list, or modes get culled. */
#if (VFX_TRAIL_COUNT * QUILL_VFX_POOL + VFX_HL_COUNT) > QUILL_MAX_VFX
#error "VFX particle pool exceeds the quad budget: modes will be silently dropped"
#endif

typedef enum QMode
{
    QMODE_NORMAL = 0,
    QMODE_INSERT,
    QMODE_VISUAL,
    QMODE_VISUAL_LINE,
    QMODE_VISUAL_BLOCK
} QMode;

/* sRGB 0xRRGGBBAA; converted to linear before upload. */
/* Overlay layer buffers, defined in quill.h next to the other caps because
 * DrawList (frame.h) is what stores them. Keep in sync with QUILL_MAX_SOLIDS /
 * QUILL_MAX_GLYPHS: an overlay is never larger than a full frame. */
#define QUILL_MAX_OVERLAY_SOLIDS 1024u
#define QUILL_MAX_OVERLAY_GLYPHS 8192u

typedef struct QuillTheme
{
    uint32_t bg, fg, gutter_bg, gutter_fg, line_hl, selection, cursor;
    uint32_t status_bg, status_fg, accent;
    uint32_t syntax[16];
} QuillTheme;

enum
{
    QC_DEFAULT = 0, QC_KEYWORD, QC_FUNCTION, QC_STRING, QC_COMMENT,
    QC_NUMBER, QC_TYPE, QC_VARIABLE, QC_OPERATOR, QC_PUNCTUATION,
    QC_PROPERTY, QC_CONSTANT, QC_ATTRIBUTE, QC_LABEL, QC_ESCAPE, QC_ERROR
};

/* Active palette; loadable at runtime via theme.h (defaults to Noir). */
extern QuillTheme QUILL_THEME;

static inline float quill_srgb_to_linear(float c)
{
    /* Avoid a libm dependency: pow(x, 2.4) is x^2 * x^(2/5), computed by
     * Newton iterations for the fifth root. */
    float x, z, y;
    int i;
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    if (c <= 0.04045f) return c / 12.92f;
    x = (c + 0.055f) / 1.055f;
    z = x * x;
    y = 1.0f - 0.5f * (1.0f - x);
    for (i = 0; i < 8; ++i)
    {
        float y2 = y * y;
        y = (4.0f * y + z / (y2 * y2)) / 5.0f;
    }
    return z * y;
}

static inline void quill_color_linear(uint32_t rgba, float out[4])
{
    out[0] = quill_srgb_to_linear((float)((rgba >> 24) & 255u) / 255.0f);
    out[1] = quill_srgb_to_linear((float)((rgba >> 16) & 255u) / 255.0f);
    out[2] = quill_srgb_to_linear((float)((rgba >> 8) & 255u) / 255.0f);
    out[3] = (float)(rgba & 255u) / 255.0f;
}

static inline float quill_fabsf(float x) { return x < 0.0f ? -x : x; }

static inline float quill_floorf(float x)
{
    long i = (long)x;
    if (x < 0.0f && (float)i != x) return (float)(i - 1);
    return (float)i;
}

static inline float quill_ceilf(float x)
{
    long i = (long)x;
    if (x > 0.0f && (float)i != x) return (float)(i + 1);
    return (float)i;
}

static inline float quill_roundf(float x)
{
    return x >= 0.0f ? quill_floorf(x + 0.5f) : quill_ceilf(x - 0.5f);
}

static inline float quill_sqrtf(float x)
{
    float reduced, y;
    int i;
    if (x <= 0.0f) return 0.0f;
    /* Range-reduce to [1,4) by factors of four, then Newton on sqrt. */
    reduced = x;
    y = x;
    while (y >= 4.0f) { y *= 0.25f; reduced *= 0.25f; }
    while (y < 1.0f) { y *= 4.0f; reduced *= 4.0f; }
    y = 0.5f * (1.0f + y);
    for (i = 0; i < 7; ++i) y = 0.5f * (y + reduced / y);
    while (x >= 4.0f) { y *= 2.0f; x *= 0.25f; }
    while (x < 1.0f) { y *= 0.5f; x *= 4.0f; }
    return y;
}

/* Return free heap pages to the OS after initialization-heavy phases. */
static inline void quill_trim_allocator(void)
{
#if defined(__linux__)
    mallopt(M_TRIM_THRESHOLD, 64 * 1024);
    mallopt(M_TOP_PAD, 0);
    (void)malloc_trim(0);
#endif
}

/* UTF-8 helpers: offsets are byte offsets. */
static inline uint32_t quill_utf8_next(const uint8_t* s, uint32_t len, uint32_t i)
{
    uint32_t n = 1u;
    if (i >= len) return len;
    if ((s[i] & 0xE0u) == 0xC0u) n = 2u;
    else if ((s[i] & 0xF0u) == 0xE0u) n = 3u;
    else if ((s[i] & 0xF8u) == 0xF0u) n = 4u;
    return i + n <= len ? i + n : len;
}

static inline uint32_t quill_utf8_prev(const uint8_t* s, uint32_t len, uint32_t i)
{
    if (i == 0u || i > len) return 0u;
    --i;
    while (i > 0u && (s[i] & 0xC0u) == 0x80u)
        --i;
    return i;
}

static inline float quill_cosf(float x)
{
    /* Small cosine approximation for cursor blink; avoid libm. */
    const float TWO_PI = 6.28318530718f;
    float x2;
    if (x > 3.14159265f) x -= TWO_PI;
    if (x < -3.14159265f) x += TWO_PI;
    if (x > 1.57079633f) return -quill_cosf(x - 3.14159265f);
    if (x < -1.57079633f) return -quill_cosf(x + 3.14159265f);
    x2 = x * x;
    return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f))));
}

static inline float quill_exp_neg(float x)
{
    float r, r2, p;
    if (x <= 0.0f) return 1.0f;
    if (x > 12.0f) return 0.0f;
    r = x / 16.0f;
    r2 = r * r;
    p = 1.0f - r + r2 * 0.5f - r2 * r * (1.0f / 6.0f) + r2 * r2 * (1.0f / 24.0f) - r2 * r2 * r * (1.0f / 120.0f);
    p *= p; p *= p; p *= p; p *= p;
    return p;
}

/* Critically damped spring step used by Neovide-style motion.
 * length_ms is the approximate settling time; 0 snaps. */
static inline void quill_spring_step(float* x, float* v, float target,
                                     float dt, float length_ms)
{
    float omega, e, delta, temp;
    if (length_ms <= 0.0f || dt <= 0.0f)
    {
        *x = target;
        *v = 0.0f;
        return;
    }
    if (dt > 0.05f) dt = 0.05f;
    omega = 6.28318530718f / (length_ms * 0.001f);
    e = quill_exp_neg(omega * dt);
    delta = *x - target;
    temp = (*v + omega * delta) * dt;
    *x = target + (delta + temp) * e;
    *v = (*v - omega * temp) * e;
}

#endif /* QUILL_H */
