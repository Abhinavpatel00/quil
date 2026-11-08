#ifndef QUILL_VFX_H
#define QUILL_VFX_H

#include "quill.h"
#include "config.h"
#include "frame.h"

/* Homogeneous particle pools: one pool per trail mode, so the hot update and
 * render loops never branch on a per-particle mode flag. Dead particles are
 * swap-removed; count is existence. */
typedef struct VfxPool
{
    float    x[QUILL_VFX_POOL], y[QUILL_VFX_POOL];
    float    vx[QUILL_VFX_POOL], vy[QUILL_VFX_POOL];
    float    rot[QUILL_VFX_POOL], life[QUILL_VFX_POOL];
    float    size[QUILL_VFX_POOL];
    float    cr[QUILL_VFX_POOL], cg[QUILL_VFX_POOL], cb[QUILL_VFX_POOL];
    uint32_t count;
    float    remainder;
    float    prev_x, prev_y;
    bool     prev_valid;
    uint64_t rng;
} VfxPool;

typedef struct VfxHighlight
{
    float cx, cy, t;
    bool  active;
} VfxHighlight;

typedef struct VfxState
{
    VfxPool      trail[VFX_TRAIL_COUNT]; /* railgun torpedo pixiedust comet ember aurora glitch snow confetti spark smoke */
    VfxHighlight hl[VFX_HL_COUNT];       /* sonicboom ripple wireframe pulse shockwave halo */
    float        dest_x, dest_y;
    bool         dest_valid;
    /* Set by the frame layer every frame: did the logical cursor/selection
     * target move?  Highlights restart on that, not on the pixel destination,
     * which also moves during smooth scrolling and would reset them forever. */
    bool         move_hint_valid;
    bool         cursor_moved;
} VfxState;

uint32_t vfx_parse_modes(const char* s);
const char* vfx_mode_name(uint32_t bit);
void vfx_init(VfxState* v);
void vfx_reset(VfxState* v);
void vfx_notify_cursor(VfxState* v, bool moved);
bool vfx_update(VfxState* v, const MotionConfig* cfg, float dt,
                float dest_x, float dest_y, float w, float h,
                bool immediate, const float base_lin[4]);
void vfx_render(const VfxState* v, const MotionConfig* cfg, DrawList* dl,
                float font_w, float font_h);
bool vfx_active(const VfxState* v);

#endif
