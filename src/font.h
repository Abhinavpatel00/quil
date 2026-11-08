#ifndef QUILL_FONT_H
#define QUILL_FONT_H

#include "quill.h"

typedef struct ShapedGlyph
{
    uint32_t glyph;
    uint32_t cluster;   /* original byte offset in the line */
    float    x_offset;
    float    y_offset;
    float    x_advance;
    uint16_t face;
    uint16_t pad;
} ShapedGlyph;

typedef struct AtlasEntry
{
    uint64_t key;
    uint32_t x, y, w, h;
    int16_t  left, top;
    uint16_t pad;
} AtlasEntry;

typedef struct DirtyRect
{
    uint32_t x, y, w, h;
} DirtyRect;

typedef struct FontSystem FontSystem;

FontSystem* font_create(uint32_t size_px, uint32_t scale);
void font_destroy(FontSystem* fs);
void font_set_size(FontSystem* fs, uint32_t size_px, uint32_t scale);
uint32_t font_size(const FontSystem* fs);
float font_line_height(const FontSystem* fs);
float font_baseline(const FontSystem* fs);
float font_advance(const FontSystem* fs);

/* Returns cached shaped glyphs; cluster values are original byte offsets.
 * Tabs are expanded to spaces with tab_width; the cache key includes the
 * original bytes so edits invalidate naturally. */
uint32_t font_shape_line(FontSystem* fs, const uint8_t* text, uint32_t len,
                         uint32_t tab_width, const ShapedGlyph** out);
float font_line_x_for_byte(const ShapedGlyph* g, uint32_t n, uint32_t byte);

/* Atlas access for the renderer. */
const AtlasEntry* font_atlas_get(FontSystem* fs, uint16_t face, uint32_t glyph,
                                 uint32_t phase);
uint32_t font_atlas_width(const FontSystem* fs);
uint32_t font_atlas_height(const FontSystem* fs);
const uint8_t* font_atlas_pixels(const FontSystem* fs);
uint32_t font_atlas_dirty_count(const FontSystem* fs);
const DirtyRect* font_atlas_dirty(const FontSystem* fs);
void font_atlas_clear_dirty(FontSystem* fs);
uint32_t font_atlas_generation(const FontSystem* fs);

#endif
