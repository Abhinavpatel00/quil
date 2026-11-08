# Quill memory use after opening `src/main.c`

Measured on this machine (Intel UHD Graphics TGL GT2, Mesa Intel Vulkan).
Command:

```sh
./quill --headless src/main.c --screenshot /tmp/quill.png
```

With automatic Vulkan ICD filtering, headless max RSS is about **46 MiB**.
An interactive 1280×800 window opening `assets/sample.c` is about **48 MiB**
RSS (~29 MiB PSS after shared library pages are apportioned). The file size
barely matters: a 400-byte C file and `src/main.c` use essentially the same
amount; it is not the text buffer.

## Why the Vulkan driver used to dominate

The Vulkan loader eagerly loads every installed ICD before Quill picks a
device. On this machine `/usr/local/share/vulkan/icd.d` contains Mesa
lavapipe (`libvulkan_lvp.so`, software) and `libvulkan_radeon.so` (AMD), and
both link `libLLVM.so.21.1` (~92 MiB resident). The loader loaded them even
though the selected device was the Intel GPU, which does not need LLVM.

`filter_unneeded_vulkan_drivers()` in `src/render_vk.c` runs before
`vkCreateInstance` on Linux. When a usable `/dev/dri/renderD*` node exists, it
sets `VK_LOADER_DRIVERS_DISABLE` to skip lavapipe/SwiftShader and hardware
drivers whose vendor has no DRM device. It respects any explicit
`VK_LOADER_DRIVERS_DISABLE`, `VK_LOADER_DRIVERS_SELECT`, `VK_DRIVER_FILES`, or
`VK_ICD_FILENAMES` set by the user, and does nothing when there is no usable
hardware render node (so software Vulkan still works in CI).

Result on this machine:

| Case | Headless max RSS | LLVM/lavapipe mapped |
|---|---:|---|
| Before (all ICDs) | ~86 MiB | yes |
| Auto-filter (default) | ~46 MiB | no |
| `VK_DRIVER_FILES=intel_icd.json` | ~33 MiB | no |

## Resident memory breakdown (interactive, after filtering)

From `/proc/<pid>/smaps` for `./quill assets/sample.c --no-session`:

| Mapping | RSS |
|---|---:|
| process heap (Vulkan buffers, font atlas, editor state) | ~16 MiB |
| Intel Vulkan driver | ~13 MiB |
| GTK/GLFW/fontconfig/HarfBuzz/FreeType/system libs | ~15 MiB |
| anonymous/other mappings | ~2 MiB |
| Quill binary | ~1.3 MiB |
| stack | ~1 MiB |
| **total RSS / PSS** | **~48 MiB / ~29 MiB** |

The C file itself contributes under 1 MiB; opening `main.c` instead of
`sample.c` does not change the total appreciably.

## Wayland libdecor is disabled

On a Wayland session GLFW would normally load `libdecor-gtk`, pulling in
GTK/GLib/ICU just to draw window decorations. Quill sets
`GLFW_WAYLAND_LIBDECOR` to `GLFW_WAYLAND_DISABLE_LIBDECOR` before
`glfwInit()`, so that stack is never loaded. Decorations come from the
compositor or GLFW's fallback.

| Interactive `assets/sample.c` | RSS | PSS |
|---|---:|---:|
| Earlier, with libdecor-gtk | ~72 MiB | ~39 MiB |
| Now (libdecor disabled, 1024 atlas/16384 glyph cap/2-image swapchain) | ~48 MiB | ~29 MiB |

## Renderer fixed costs

- `QUILL_FRAMES = 1`, so there is one command buffer, fence, acquire
  semaphore, and set of solid/glyph vertex rings. This trades away
  CPU/GPU overlap for a smaller fixed footprint.
- Solid and glyph vertex buffers are the only persistently mapped frame
  buffers. The glyph ring is `16384 * 6 * 32` ≈ 3 MiB and the solid ring is
  `4096 * 6 * 24` ≈ 0.56 MiB. The swapchain requests two images when the
  surface permits it, falling back to the surface minimum when needed.
- VFX vertex storage (~1.3 MiB) is allocated only when a frame first
  contains VFX quads. Without an active VFX mode there is no VFX buffer
  reservation after renderer creation.
- Atlas staging is transient: an upload allocates at most a 1 MiB chunked
  staging buffer, uploads the dirty rectangles with immediate submit/wait
  batches, and frees it before the main frame command buffer is recorded.
- The font atlas starts at 1024×1024 R8 (1 MiB CPU and GPU) and grows to
  2048×2048 on demand. `atlas_upgrade()` preserves existing entries by copying
  the old pixels into the new top-left quadrant, marks the larger atlas dirty,
  and the renderer recreates the sampleable image and descriptor when the size
  changes. Startup still avoids clearing or marking the atlas dirty; the first
  glyphs upload only the small rectangles they actually sample. Font resizes
  still reset the atlas and drain through the same chunked upload path.
- `quill_trim_allocator()` tightens glibc trim behavior and calls
  `malloc_trim(0)` after font creation, renderer creation, and the first
  successful frame. This returns initialization-heavy heap pages to the OS.
- `DrawList` is ~1.4 MiB, `ModalState` is ~513 KiB, and `VfxState` is
  ~220 KiB; these are fixed but small compared with the driver and
  windowing stack.

## Known fixed allocations

- `Picker` is ~2.3 MiB but is allocated lazily on first picker open.
- `ModalState` register file is 128 × 4096 = 512 KiB.
- `DrawList` is ~1.4 MiB after reducing caps to 16384 glyphs / 4096 solids.
- Largest frame buffers: glyph ~3 MiB, solid ~0.56 MiB, lazy VFX ~1.3 MiB.
- Transient atlas staging: at most 1 MiB, allocated only during uploads.
- Font atlas: starts at 1024×1024 R8 (1 MiB CPU + GPU) and grows to
  2048×2048 (4 MiB CPU + GPU) only when the small atlas fills. Most CPU pages
  are untouched until glyphs are rasterized.
- Headless target + readback at 1280×800 BGRA8: ~3.9 MiB each.

## Virtual memory is not RSS

`Document.payload` reserves 64 MiB of address space for undo payloads but had
**0 bytes committed** after merely opening `main.c`. `mu_quill`'s text arena
reserved/committed 1 MiB. The Vulkan driver also reserves large virtual
ranges. VSZ can exceed 1 GiB while RSS is ~48 MiB.

## Reductions made

- Picker is lazy: no picker arrays/thread until first picker use.
- `QUILL_MAX_GLYPHS` 65536 → 16384 and `QUILL_MAX_SOLIDS` 8192 → 4096.
  This quarters the original glyph vertex-buffer memory and reduces DrawList
  size while still covering a 1280×800 code viewport plus picker overlay.
- Vulkan ICD filtering skips lavapipe and drivers for GPUs that are not
  present. This removes the 92 MiB `libLLVM` mapping and drops interactive
  RSS from ~105 MiB to ~72 MiB, and headless RSS from ~86 MiB to ~52 MiB.
- A single frame is kept in flight, VFX vertex buffers are created only when
  VFX quads are present, atlas staging is transient and chunked at 1 MiB, and
  startup uploads only dirty glyph rectangles.
- The font atlas starts at 1024×1024 and upgrades to 2048×2048 on demand,
  the glyph cap is 16384, swapchain creation requests two images, and
  allocator trimming returns freed heap pages after initialization/first
  frame. Those changes take the latest interactive measurement to ~48 MiB
  RSS / ~29 MiB PSS and headless max RSS remains ~46 MiB on this machine.
