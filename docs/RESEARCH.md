# Quill font-rendering and editor research

Quill is a Vulkan code/text editor whose buffer is `mu_quill`. The only language
service is Tree-sitter. There is deliberately **no LSP and no autocompletion**:
those add latency, memory, and background work that this editor does not need.

## 1. What “high quality text” means here

A code editor spends most of its visual budget on small, hinted, monospaced
glyphs. The important properties are:

1. **Correct shaping, not just `FT_Load_Char` per codepoint.** HarfBuzz turns
   Unicode into positioned glyphs using the font’s GSUB/GPOS tables; it is a
   shaper, not a rasterizer, and a full layout system still needs rasterization,
   font fallback, and (for mixed-direction text) BiDi itemization
   [1](https://harfbuzz.github.io/) [2](https://www.reddit.com/r/programming/comments/1m6a7xo/we_maintain_harfbuzz_the_text_shaping_engine_used/).
2. **Hinting that preserves shape at small sizes.** FreeType’s
   `FT_LOAD_TARGET_LIGHT` snaps vertically while preserving horizontal shape,
   which is the usual choice for grayscale UI/code text
   [1](http://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html).
3. **Coverage blending in linear light.** FreeType grayscale output is a
   coverage mask. If that mask is blended directly against sRGB-encoded colors,
   light-on-dark text looks thin and dark-on-light text looks heavy. Skia and
   Chromium compensate with mask-gamma/contrast; Chromium’s Linux values are
   `SK_GAMMA_EXPONENT=1.2`, `SK_GAMMA_CONTRAST=0.2`
   [2](https://github.com/noctalia-dev/noctalia/issues/5037).
4. **HiDPI and subpixel positioning.** Atlas keys must include font face, glyph,
   pixel size, and subpixel phase. A monospace code grid can snap advances to
   whole pixels to maximize atlas reuse; fractional positioning is still useful
   for fallback scripts and smooth scrolling.
5. **Incremental syntax, not a language server.** Tree-sitter can reuse an old
   tree if the edit is described in both bytes and `(row, byte-column)` points
   and the old tree is passed back to the parser
   [3](https://emacs-tree-sitter.github.io/api/parsing/) [1](https://dev.to/shrsv/making-sense-of-tree-sitters-c-api-2318).

## 2. Implemented font pipeline

```text
UTF-8 line (mu_quill view)
  -> tab expansion + original-byte cluster map
  -> fontconfig fallback face selection
  -> HarfBuzz shaping (hb-ft, FT_LOAD_TARGET_LIGHT)
  -> integer code-grid snap (whole-pixel advances)
  -> FreeType grayscale raster at exact device pixel size
  -> R8 shelf atlas, key = face|glyph|size|subpixel-phase
  -> glyph shader: coverage -> alpha, linear text color
  -> sRGB swapchain: hardware linearizes dst, blends, re-encodes
```

### Rasterization and atlas

- Primary face: the first available fixed-width face from JetBrains Mono,
  Noto Sans Mono, DejaVu Sans Mono. Fontconfig matching is constrained to
  `FC_SPACING=FC_MONO`, and the loaded FreeType face must have
  `FT_FACE_FLAG_FIXED_WIDTH`. Family names alone are not sufficient: a missing
  preferred family can otherwise be substituted with proportional DejaVu Sans,
  whose 4–16 px advances at 16 px corrupt a 14 px code/terminal grid.
- Unicode fallback: Noto Sans / DejaVu Sans via fontconfig. The atlas is
  `VK_FORMAT_R8_UNORM`, starts at 1024×1024, grows to 2048×2048 only when
  needed, and has 1 px padding.
- Glyphs are rasterized at `size_px * scale`, not scaled from a nominal atlas.
  This keeps hinted stems correct at each zoom/HiDPI step.
- The atlas key includes a quarter-pixel subpixel phase. The code grid snaps
  advances to integers, so normal ASCII code uses phase 0 and reuses one glyph
  per codepoint. Fractional scroll/fallback can still use other phases.
- Dirty rectangles are uploaded through a transient, chunked staging buffer.
  When the small shelf atlas fills, `atlas_upgrade()` preserves the existing
  entries and grows the atlas; once at 2048×2048, a full atlas resets and the
  visible frame repopulates it.

### Gamma / contrast

Quill renders to an sRGB attachment (`B8G8R8A8_SRGB` + `SRGB_NONLINEAR` when
available). The Vulkan spec linearizes sRGB destination color before blending
and re-encodes afterward; alpha is never sRGB-encoded
[1](https://renderdoc.org/vkspec_chunked/chap30.html). The fragment shader
therefore outputs **linear** text color and a coverage-derived alpha. This gives
physically correct source-over blending without a second gamma step.

The shader exposes `gamma` and `contrast` push constants. Defaults are
`gamma=1`, `contrast=0` (pure linear coverage). The Skia-style mask-gamma hack
is intentionally not applied by default because it is a compensation for
encoded-space blending; applying it on top of an sRGB framebuffer would
double-correct. Users who prefer heavier light-on-dark text can raise contrast.

### Shaping limitations (honest)

- HarfBuzz shaping is enabled, including ligatures/mark positioning, but the
  editor snaps the final code grid to whole pixels.
- BiDi itemization is **not** fully implemented yet; lines are shaped LTR with
  fallback faces. FriBidi-based paragraph/run itemization is the next step for
  mixed RTL/LTR prose.
- Color emoji are rasterized through FreeType but folded into grayscale
  coverage in the R8 atlas. A true RGBA color-emoji atlas is future work.
- LCD subpixel rendering is not enabled; grayscale AA avoids color fringing and
  works under rotation/scaling.

## 3. Vulkan renderer

- Two graphics pipelines: solid quads (background/selection/cursor/status) and
  glyph quads. This keeps the hot draw path branch-free per primitive type.
- Two frames in flight. `renderFinished` semaphores are indexed by **swapchain
  image**, while acquire semaphores/fences are indexed by frame-in-flight, as
  recommended by the Vulkan tutorial
  [1](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/03_Drawing/03_Frames_in_flight.html).
- Persistently mapped host-visible vertex rings and staging rings; no allocation
  in the frame path.
- Damage-driven redraw: the interactive loop waits on GLFW events and only
  records/submits when input, scroll, resize, or a deferred parse dirties the
  frame.
- Headless offscreen target + PNG readback for CI/tests. In this sandbox the
  only Vulkan device is Mesa llvmpipe (software), so any GPU timing is labelled
  software, not hardware GPU performance.

## 4. Tree-sitter integration

- `TSInput.read` is fed by `mu_quill_span`, a zero-copy contiguous physical run
  from the chunked gap buffer. No whole-document flatten.
- Every edit computes `TSInputEdit` from `mu_quill` byte offsets and cursor
  points, calls `ts_tree_edit`, and reparses with the old tree
  [3](https://emacs-tree-sitter.github.io/api/parsing/).
- Highlight queries are the grammars’ `highlights.scm`, compiled once per
  language. The frame builder queries the **visible byte range once**, then
  paints spans into a per-line color table.
- Parsing is debounced (30 ms) in the interactive app. The edited tree is still
  positionally updated by `ts_tree_edit`, so the editor can render immediately
  with stale-but-shifted highlights and reparse when the user pauses.
- Languages: C, JSON, Rust, Python, JavaScript. No completion, no diagnostics
  server, no semantic indexing.

## 5. Evidence summary

See `PERFORMANCE.md` and `results/bench_frame.csv`. Highlights:

- Sequential 40-line viewport read: `mu_quill_view` is **3.20×** faster than
  per-line `mu_quill_line_text` on an 8 MiB synthetic C file.
- CPU frame build (1 MiB C, Tree-sitter + shaping, 1280×800): **63 µs** static,
  **93 µs** scrolling median.
- Responsive edit path (insert + frame with stale tree): **69 µs** median.
- Full-file incremental Tree-sitter reparse on 1 MiB C: **14.7 ms** median;
  this is why parse is debounced off the input path.
- llvmpipe headless 1280×800 sample frame: **5.98 ms** software rasterization.
  This is not a hardware GPU number.
