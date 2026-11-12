# Quill architecture

## Module map

| Module | Responsibility | Hot-path allocation |
|---|---|---|
| `document.c` | `mu_quill` buffer, cursor/selection, undo/redo, file I/O | none (undo payload arena) |
| `syntax.c` | Tree-sitter parser/query, incremental edit, highlight spans | none after create |
| `font.c` | FreeType + HarfBuzz + fontconfig, shaping cache, R8 atlas | none after create (arena/cache reset) |
| `frame.c` | Viewport walk, highlight paint, draw-list build | none (caller-owned draw list/scratch) |
| `render_vk.c` | Vulkan swapchain/headless, pipelines, atlas upload, submit | none (mapped rings) |
| `main.c` | GLFW input, damage-driven loop, deferred parse | none |

## Buffer: `mu_quill`

The document buffer is `mu_quill`, a chunked gap buffer with eager newline
counts and lazy line-start arrays. Round 10 added three reusable APIs to
`mylib/mu/mu_quill.h`:

- `mu_quill_span(q, off, &len)` — zero-copy contiguous physical run. This feeds
  Tree-sitter’s `TSInput.read` without flattening the document.
- `mu_quill_cursor_to_offset` / `mu_quill_offset_to_cursor` — byte cursor
  mapping with correct chunk-boundary line ownership.
- `mu_quill_view_start/next/next_text` — one-descent sequential viewport walk.
  This is the scroll path and is 3.20× faster than per-line descent in the
  measured 8 MiB workload.

## Data layout

- `DrawList` is two flat arrays: `SolidQuad[]` and `GlyphQuad[]`. The renderer
  writes six vertices per quad into persistently mapped buffers and issues two
  draws. No per-quad flags or branches in the GPU submit path.
- `FrameScratch` owns the visible highlight span array, per-line color table,
  line text buffer, and status string. It is allocated once and reused.
- `LineLayout[]` caches shaped glyphs for visible lines so mouse hit-testing
  does not reshape on every event.
- Shaping cache: hash of line bytes + length + size + tab width -> arena
  offset/count. The source bytes are stored alongside the glyphs and compared
  on lookup, so a hash collision (or the same bytes shaped with a different tab
  width) can never return the wrong glyphs. Atlas cache:
  face|glyph|size|phase -> shelf rectangle. Both reset by generation when full
  or on zoom. Atlas UVs are computed from the atlas size at emit time: the
  atlas can grow mid-frame, and a size captured before the glyph loop would
  make later quads sample the wrong region.
- Undo: `mu_small_vec` stacks of `UndoOp` + append-only `mu_vm_arena` payload.
  Adjacent short inserts coalesce within 800 ms. No malloc in the edit path.

## Frame pipeline

```text
GLFW event / resize / deferred parse
  -> clamp scroll, ensure cursor visible
  -> frame_build:
       mu_quill_view walk visible lines
       syntax_highlight(visible byte range) once
       paint spans into per-line color table
       font_shape_line (cache hit or HarfBuzz + FreeType)
       emit solid quads (bg, gutter, selection, cursor, status)
       emit glyph quads (atlas lookup, subpixel phase, cull)
  -> vk_render_frame:
       wait frame fence
       acquire swapchain image (or headless target)
       write mapped vertex rings
       upload atlas dirty rects
       record solid draw + glyph draw
       submit / present (or readback for headless)
```

## Tree-sitter only

`syntax.c` owns a `TSParser`, `TSTree`, and `TSQuery` per language. Edits call
`ts_tree_edit` with byte and point deltas computed from `mu_quill`. The frame
builder does **not** parse; it highlights the current tree. The app schedules a
parse 30 ms after the last edit, so typing renders immediately and syntax catches
up on pause. There is no LSP client, no completion engine, no semantic database.

## Vulkan synchronization

- `QUILL_FRAMES = 1` command buffer/fence/acquire semaphore/vertex ring.
- `renderFinished` semaphores are per swapchain image.
- Atlas image stays in `GENERAL` layout after the initial transition; dirty
  rects are copied from a transient staging buffer (allocated for the upload
  and released afterwards) and guarded by transfer->shader barriers. The CPU
  and GPU atlases start at 1024×1024 and grow to 2048×2048 when the small
  atlas fills; existing entries are preserved and the descriptor is
  re-pointed after the image resize.
- sRGB attachment gives linear-light blending; shader outputs linear color.
- Headless mode renders to an offscreen sRGB image and copies to a host-visible
  buffer for PNG output. This is how CI tests Vulkan without a display.

## AGENT.md / mylib contract notes

- IDs/handles: atlas and shaping caches use integer indices/offsets, not
  long-lived pointers into reallocable storage.
- Existence-based processing: dirty bitset in `mu_quill`, atlas dirty rects,
  syntax `needs_parse` flag outside hot loops.
- No pointer chasing in the viewport walk: chunk starts are scanned forward.
- Explicit ownership: document arena, font arena, caller-owned draw list,
  Vulkan mapped rings. No malloc in frame build or submit except for the rare
  font-atlas growth from 1024×1024 to 2048×2048.
- Measured before/after: visible-range query optimization and view API speedup
  are recorded in `PERFORMANCE.md`.
