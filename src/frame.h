#ifndef QUILL_FRAME_H
#define QUILL_FRAME_H

#include "document.h"
#include "syntax.h"
#include "font.h"
#include "selection_set.h"
#include "modal.h"
#include "config.h"

typedef struct SolidQuad
{
    float x, y, w, h;
    float r, g, b, a;
} SolidQuad;

typedef struct VfxQuad
{
    float x, y, w, h;
    float r, g, b, a;
    float shape, stroke, soft, pad;
} VfxQuad;

typedef struct GlyphQuad
{
    float x, y, w, h;
    float u0, v0, u1, v1;
    float r, g, b, a;
} GlyphQuad;

typedef struct DrawList
{
    SolidQuad solids[QUILL_MAX_SOLIDS];
    uint32_t  nsolids;
    GlyphQuad glyphs[QUILL_MAX_GLYPHS];
    uint32_t  nglyphs;
    VfxQuad   vfx[QUILL_MAX_VFX];
    uint32_t  nvfx;
    uint32_t  overflow;
} DrawList;

typedef struct LineLayout
{
    uint32_t line;
    uint32_t offset;
    uint32_t len;
    uint32_t nglyphs;
    float    y;
    const ShapedGlyph* glyphs;
} LineLayout;

#define QUILL_CURSOR_TRAIL 8u

typedef struct CursorAnim
{
    float    x, y, w, h;
    float    vx, vy, vw, vh;
    uint64_t last_ms;
    bool     valid;
    float    trail_x[QUILL_CURSOR_TRAIL];
    float    trail_y[QUILL_CURSOR_TRAIL];
    float    trail_w[QUILL_CURSOR_TRAIL];
    float    trail_h[QUILL_CURSOR_TRAIL];
    uint64_t trail_t[QUILL_CURSOR_TRAIL];
    uint32_t trail_head, trail_count;
} CursorAnim;

typedef struct FrameParams
{
    uint32_t width, height;
    uint32_t top_line;
    float    scroll_x;
    float    scroll_y_offset;
    uint32_t tab_width;
    bool     line_numbers;
    const SelectionSet* selections;
    const ModalState*   modal;
    const char*         search_query;
    uint64_t            now_ms;
    uint32_t            cursor_ms;
    const MotionConfig* motion;
    CursorAnim*         cursor_anim;
    struct VfxState*    vfx;
    float               blink_alpha;
    bool*               animating;
} FrameParams;

typedef struct FrameScratch
{
    HighlightSpan spans[QUILL_MAX_SPANS];
    uint8_t       color[QUILL_MAX_LINE];
    uint8_t       line[QUILL_MAX_LINE];
    char          status[512];
} FrameScratch;

void frame_build(Document* d, FontSystem* fs, const FrameParams* p,
                 DrawList* dl, FrameScratch* scratch,
                 LineLayout* layouts, uint32_t* nlayouts);
uint32_t frame_hit_test(const FrameParams* p, const LineLayout* layouts,
                        uint32_t nlayouts, float line_height, float gutter,
                        float x, float y, Document* d);


void frame_rect(DrawList* dl, float x, float y, float w, float h, const float c[4]);
void frame_text(FontSystem* fs, DrawList* dl, const char* text, uint32_t len,
                float x, float baseline_y, const float c[4], float clip_x1);

#endif
