# Quill memory audit — measured on the real binary

Debian 13 (trixie), x86-64, gcc, `quil` @ `f200b67`, `mu_lib` @ `bfc1f48`
(the commit that added the `mu_quill` view/cursor helpers — the tree builds
before and after my changes; `make check` passes 17/17).

**This document replaces the earlier version, which was built on per-subsystem
harnesses and an estimate. Everything below is now measured on `./quill`.**

---

## 0. How to reproduce

```sh
make -j4 MYLIB=../mu_lib GLFW_LIB="$(pkg-config --libs glfw3)"

# heap attribution (LD_PRELOAD malloc interposer, dumps at peak)
cc -shared -fPIC -O2 -o /tmp/libheapprof.so docs/harnesses/heapprof.c
LD_PRELOAD=/tmp/libheapprof.so ./quill --headless assets/sample.c --screenshot /tmp/s.png

# per-mapping RSS/PSS at peak
python3 docs/harnesses/measure.py "headless" -- ./quill --headless assets/sample.c --screenshot /tmp/s.png

# glyph/solid ring utilisation (real per-frame counts)
# see §4 — instrument the vkCmdDraw path, not frame_reset
```

`[heap]` is the number that corresponds to "heap alloc". **`Private_Dirty` is
the number that matters** — it is the memory that is yours alone and that you
can actually shrink. `Rss` and `Pss` in this environment are dominated by
`libLLVM.so` (53.88 MiB) that Mesa's llvmpipe software renderer pulls in;
`filter_unneeded_vulkan_drivers()` skips that on a machine with a real GPU.

---

## 1. Interactive baseline: `[heap]` = 9.99 MiB, Private_Dirty = 35.00 MiB

`Xvfb 1280x800`, `./quill --no-session assets/sample.c`, settled 2.5 s, 3 runs:

| mapping | RSS | PSS |
|---|---:|---:|
| libLLVM.so.19.1 (llvmpipe) | 53.88M | 53.88M |
| memfd:allocation (Mesa) | 11.82M | 11.82M |
| **`[heap]`** | **9.99M** | **9.99M** |
| libvulkan_lvp.so | 6.15M | 6.15M |
| `[anon]` | 4.21M | 4.21M |
| quill (binary) | 1.25M | 1.25M |
| libX11 / libglfw / libharfbuzz / libstdc++ | ~4M | ~4M |

So `[heap]` **is** your ~15 MB figure, minus the driver. On a GPU machine the
`libLLVM` and `memfd` rows disappear and you are left with roughly
`[heap] 10 + anon 4 + quill 1.25 + libs 2 + stack 1` ≈ **18–19 MiB RSS**, of
which **~12 MiB is private**.

## 2. Heap attribution: two thirds of it is the software driver

`heapprof` at peak live heap, headless:

| site | live MiB | allocs |
|---|---:|---:|
| `libvulkan_lvp.so+0xee465` (Mesa llvmpipe) | 27.503 | 5781 |
| `quill+0x376eb` `stbi_write_png_to_mem` | 3.907 | 1 |
| `quill+0x3a981` `vk_render_screenshot` | 3.906 | 1 |
| `libvulkan_lvp.so+0x292f61` | 1.147 | 161 |
| `quill+0x10ec6` `main` → `DrawList` | 1.063 | 1 |
| `quill+0x1ea4f` `font_create` → atlas | 1.000 | 1 |
| `libvulkan_lvp.so+0x329a9c` | 0.507 | 161 |
| `quill+0x1cf5f` `syntax_create_for_path` | 0.329 | 1 |

I removed the `libheapprof.so+0xcdf` row from the raw output: that is my own
interposer's lookup table (1.37 MiB), not your program.

Reading it:

- **27.5 MiB is Mesa.** Not yours, and it is gone on a GPU machine.
- **The two 3.9 MiB rows are the headless screenshot path only** —
  `vk_render_screenshot()` mallocs a full-frame BGRA copy *and*
  `stbi_write_png_to_mem()` allocates a compressed-output buffer of the same
  order. ~7.8 MiB of transient peak that never appears in an interactive
  session. It only affects `make check` and CI. Worth fixing for CI memory
  (encode tiles or row-by-row), but it is not your 15 MB.
- **Your actual fixed structures are small**: `DrawList` 1.063 MiB, font atlas
  1.000 MiB, `Syntax` 0.329 MiB. That is ~2.4 MiB of the ~10 MiB `[heap]`.

The remaining ~4 MiB of `[heap]` is glibc arena fragmentation and allocator
metadata from the churn of those many small Mesa/fontconfig allocations — not
live objects. That is why `quill_trim_allocator()` exists in your code.

## 3. Fix applied #1 — shaped-line cache budget: 48 MiB → 2 MiB

`src/font.c`. The arena reserves 64 MiB of address space, commits 64 KiB at a
time, and `shape_reset()` calls `mu_vm_arena_reset()`, which only sets
`head = 0` and deliberately keeps committed pages warm. So the eviction budget,
not the reservation, set the resident high-water — and 48 MiB let it reach
~4 MiB and never return.

Measured, 20 000 distinct 40-char lines through `font_shape_line`:

| budget | peak RSS | 600 × 256-row viewport |
|---|---:|---:|
| 48 MiB (original) | 11.22 MiB | 0.041 ms/frame |
| 8 MiB | 11.21 MiB | 0.056 ms/frame |
| 4 MiB | 10.02 MiB | 0.025 ms/frame |
| **2 MiB (applied)** | **8.09 MiB** | **0.026 ms/frame** |
| 1 MiB | 7.23 MiB | 0.026 ms/frame |

2 MiB caches ~500 full-width lines — twice the 256-row viewport — so overlays
and splits still hit. **~3.1 MiB saved in long sessions, no measurable
frame-time cost.**

I also tested calling `mu_vm_arena_release()` in `shape_reset()` to hand pages
back: **it does not help** (11.29 → 11.36 MiB), because the next allocation
faults them straight back in. Lowering the budget is the right lever.

## 4. Fix applied #2 — glyph ring: `QUILL_MAX_GLYPHS` 16384 → 8192

The glyph vertex ring is `QUILL_MAX_GLYPHS * 6 * 32` bytes and is persistently
mapped, so it is resident process memory. I instrumented the `vkCmdDraw` path
to report **real** per-frame utilisation (do not instrument `frame_reset()` —
it runs before the list is rebuilt and reports zeros):

| window | file | glyphs used |
|---|---|---:|
| 1280x800 | assets/sample.c | 364 / 16384 |
| 1920x1080 | assets/sample.c | 364 / 16384 |
| 2560x1600 | assets/sample.c | 364 / 16384 |
| 3840x2160 | assets/sample.c | 364 / 16384 |
| 2560x1600 | src/main.c | 1786 / 16384 |
| 2560x1600 | src/render_vk.c | 2036 / 16384 |
| 3840x2160 | src/document.c | 2357 / 16384 |
| **3840x2160** | **src/main.c** | **2530 / 16384** |

Worst observed frame is **2530**, i.e. the old cap was ~6.5× oversized.
`emit_glyph()` already drops quads past the cap instead of overrunning, so an
underestimate degrades rather than corrupts. 8192 keeps a **2.4× margin over the
4K worst case**.

**~1.9 MiB saved** (glyph ring 3.00 → 1.50 MiB, `DrawList` glyph array
768 → 384 KiB), and `quill` binary 1.25 → 1.22 MiB.

## 5. Combined result, measured

Interactive, single buffer, 3 runs each:

| build | RSS | PSS | **Private_Dirty** | `[heap]` |
|---|---:|---:|---:|---:|
| pristine (16384 / 48 MiB) | 100.36 | 76.38 | **35.00** | 9.99 |
| both fixes (8192 / 2 MiB) | 100.21 | 76.25 | **33.71** | 9.87 |

**−1.29 MiB Private_Dirty on a single small file.** `[heap]` barely moves
because glibc retains the freed pages; `Private_Dirty` is the honest figure.

The two fixes are not additive on one small file: the shape cache only grows
when you shape *many distinct lines*, so its ~3 MiB shows up in a long editing
session, not in a 2-second launch on `sample.c`. Separately measured long-session
peak was 11.22 → 8.09 MiB.

`make check`: **17/17 pass**, including the three Khronos-validation renders.

---

## 6. Still on the table (ranked, not applied)

| # | change | est. saving | note |
|---|---|---:|---|
| 3 | `QUILL_REG_CAP` 4096 → 1024, allocate `reg_data` lazily | 0.38 MiB | 512 KiB of yank registers is allocated eagerly at startup for 128 registers you may never fill |
| 4 | `QUILL_MAX_SPANS` 8192 → 2048 | 0.29 MiB **per open document** | it is a viewport bound used for 5 arrays in a per-`Document` struct; 10 open buffers = 3 MiB |
| 5 | headless screenshot: tile the PNG encode | ~7.8 MiB peak | CI only — fixes `make check` memory, not the editor |
| 6 | `Picker` 2.3 MiB is never trimmed after first open | 2.3 MiB | add `malloc_trim(0)` after `picker_destroy`, or allocate its arrays from a releasable `mu_vm_arena` |
| 7 | size `Syntax` arrays to visible rows, not `QUILL_MAX_SPANS` | 0.29 MiB/doc | same struct as #4 |
| — | drop fontconfig | ~0.9 MiB on disk | also unhooks expat/libpng/bz2/brotli; see the startup bug below |
| — | FreeType → `stb_truetype` | ~1–2 MiB PSS at best | **not worth it**: loses GSUB ligatures (Fira Code `->`, `!=`) and fallback faces |
| — | drop HarfBuzz | ~0.5 MiB PSS | loses all shaping |
| — | drop tree-sitter | ~0.25 MiB PSS | absurd |

**The libraries were never the problem.** The whole third-party text stack
(FreeType + HarfBuzz + fontconfig) measured **2.12 MiB PSS** in isolation, and
tree-sitter with **all four grammars** added only **1.95 MiB PSS** — because
grammar tables are `.rodata` served from page cache, which is shared, clean and
reclaimable. Optimising there is spending real complexity for ~2 MiB.

---

## 7. Two bugs found while measuring

### 7.1 Rendering is not deterministic run-to-run

The same binary, same input, three consecutive runs of
`./quill --headless assets/sample.c --screenshot ...` produce **different
pixels**: 186–187 differing pixels, confined to rows 0–11, ~10 px per row.
Compare a build against itself and you get the same 186 px — so this is
pre-existing and unrelated to these changes.

**Consequence: `make check`'s screenshot step cannot be used as a golden-image
regression test.** It passes today because validation only checks Vulkan API
correctness, not pixels. If you ever want image tests, this has to be found and
fixed first. (Rows 0–11 with a constant ~10 px/row suggests the first text line
or the status/hint line, possibly an uninitialised field in the first frame or
an atlas page uploaded on the first frame with stale contents.)

### 7.2 `font_create()` cannot start on a clean container

It tries a hardcoded list of absolute paths and returns NULL if none exist:

```c
"/usr/share/fonts/TTF/FiraCode-Regular.ttf",
"/usr/share/fonts/truetype/fira-code/FiraCode-Regular.ttf",
"/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf", ...
```

On a stock Debian container with none of those packages installed, the editor
**fails to start** — `font_create failed`, no fallback. fontconfig is used only
for the *secondary* face. To get the measurements above I had to place a font at
one of those paths. So "drop fontconfig" and "make font discovery work" are the
same task, and dropping it saves ~0.9 MiB of mostly-shared library pages.

### 7.3 (minor) `DrawList.overflow` is never read

`frame_rect`, `emit_glyph`, the terminal and vfx all do
`if (count >= MAX) { dl->overflow++; return; }`, and `overflow` is reset every
frame — but nothing ever reads it to warn. With a 2.4× margin over the 4K worst
case that is fine, but a one-line stderr warning when `overflow != 0` would turn
a silent visual truncation into a diagnosable event.

---

# Round 2 — usability fixes and fontconfig removal

Five changes, all verified on the real binary. `make check` 17/17.

## A. Picker was unreadable — layering bug, not transparency

The picker is drawn *after* `frame_build()`, but the renderer batches by
primitive type:

```
solids  (all)  ->  glyphs (all)  ->  vfx (all)
```

So a panel appended to the draw list after the editor had its solids drawn in
the **solids batch, before every editor glyph**. Editor text painted on top of
the picker. That is the "transparent picker": it was not alpha, it was
z-ordering. Confirmed by screenshot: `src/main.c` line text visible through the
panel, list labels overlapping code.

Two fixes:

1. **`frame_overlay_begin()/frame_overlay_end()`** (`src/frame.h`, `src/frame.c`).
   Sets `dl->in_overlay` so `emit_solid`/`emit_glyph` route into
   `ov_solids`/`ov_glyphs`; the renderer uploads them after the base layer and
   issues a second draw batch with a first-vertex offset. Cost: one extra
   `vkCmdDraw` per type, only when an overlay is open.
2. **Surface colour** (`src/picker.c`). The panel used `status_bg`, which is
   `0x000000` in Noir — literally the editor background, so the panel had no
   visible edge even when correctly layered. Now `gutter_bg` (`0x121212`) with
   `gutter_fg` rules, and the selection bar is forced opaque (`selection` is
   `0x30303066`, 40% alpha) and lifted toward the foreground so the selected
   row is findable at a glance.

## B. VFX "doesn't work properly" — capacity invariant was broken

The pool and the quad budget were mutually unsatisfiable:

```
VFX_TRAIL_COUNT (11) x QUILL_VFX_POOL (512) = 5632 particles
QUILL_MAX_VFX                               = 4096 quads
```

With several modes enabled the list saturated and `emit_vfx()` silently dropped
the remainder, so **whole modes disappeared** — whichever were rendered last.
Measured before the fix: snow, confetti and smoke all pinned at exactly
`4096/4096`.

Fixed by:

- `QUILL_VFX_POOL` 512 → 256, so `11 x 256 = 2816` fits inside 4096.
- A `#error` in `quill.h` so the two constants cannot drift apart again.
- `vfx_render()` now culls **proportionally per mode** (`stride`) when a frame
  cannot hold everything, instead of filling the list and starving later modes.

Verified with the diagnostic renderer:

| case | quads | overflow |
|---|---:|---:|
| snow (before) | 4096/4096 | saturating |
| snow (after) | 136/4096 | 0 |
| confetti / smoke / ember / railgun | 132 / 146 / 107 / 100 | 0 |
| **all 17 modes at once** | **1012/4096** | **0** |

## C. fontconfig removed — and the startup bug with it

`src/font.c` no longer links fontconfig. Face discovery is a filesystem search:

1. `$QUILL_FONT` / `$QUILL_FONT_MONO` (explicit path)
2. `$XDG_DATA_HOME/quill/fonts`, `~/.local/share/quill/fonts`
3. compiled-in distro paths
4. a depth-limited scan of `/usr/share/fonts` and friends, filtered to
   `.ttf/.otf/.ttc`, then re-ordered so monospace-looking basenames come first
   (FreeType still verifies `FT_IS_FIXED_WIDTH` before accepting a face)

**The startup bug is fixed as a side effect.** With no font at any of the old
hardcoded paths, the old binary exits 1 printing nothing at all — verified. The
new one finds DejaVu Sans Mono by scanning and renders correctly. A missing
monospace face now produces an actionable message naming the package and the
env var instead of silence.

`ldd quill` before → after: `libfontconfig.so.1` and `libexpat.so.1` gone.

Caveat: `libpng16`, `libbz2` and `libbrotli*` remain, because **FreeType and
HarfBuzz link them directly** — they were never fontconfig's.

| build | RSS | PSS | Private_Dirty | `[heap]` |
|---|---:|---:|---:|---:|
| pristine (16384 glyphs, 48 MiB shapes, fontconfig) | 100.36 | 76.38 | 35.00 | 9.99 |
| now (8192, 2 MiB shapes, no fontconfig, overlay layer) | 99.71 | 76.03 | **33.44** | 9.69 |

Libraries load lazily, so most of the saving lands in `Private_Dirty` (1.56 MiB
total across all five changes) rather than RSS. As before, `libLLVM` (53.88 MiB)
is the software Vulkan driver and is not yours.

## D. Copy/paste lag — not reproducible here; instrumentation shipped

I could not reproduce it in this sandbox, and two plausible causes are ruled
out by measurement:

- **Clipboard round-trip is not the cost.** `glfwSetClipboardString` +
  `glfwGetClipboardString` of 200 KB measured **0.1 ms** under Xvfb. On a real
  Wayland session with no clipboard manager the *first* paste can block on the
  compositor handshake, but that is GLFW/driver behaviour, not quill.
- **The synchronous reparse is not the cost.** Pasting 100 KB into files of
  100–170 KB measures 8–16 ms for the reparse (`/tmp/pastebench.c`).

What is left is the per-frame path, and there is a concrete inefficiency there:
after any edit `note_edit()` sets `parse_due = now + 0` for files under 256 KB,
so the *very next* frame reparses and then **re-shapes every visible line from
scratch** — `font_shape_line` is keyed on a hash of the whole line, so all ~256
visible lines miss the cache at once. Work that should be invisible becomes a
visible hitch on the frame after a paste.

Recommended next step, not yet implemented: on `paste_clipboard()`, if the pasted
length exceeds a threshold, set `parse_due = now + 16` (the existing
large-file debounce) so the frame that shows the pasted text is not the frame
that also re-parses and re-shapes. Highlight colours correct one frame later,
which is what the existing code already does for large files.

---

# Round 3 — the `p` lag: cause found and fixed

`p` ruled out the clipboard entirely, so this is the **edit → frame** path. Three
things were individually innocent and jointly expensive.

## What was ruled out by measurement

| suspect | measured | verdict |
|---|---|---|
| clipboard round-trip (200 KB) | 0.1 ms | innocent |
| modal `p` itself (yank + put) | 0.01 ms | innocent |
| tree-sitter reparse after the edit | 0.07–0.55 ms | innocent |
| `frame_build` after the edit (1228-line file) | 0.10 ms | innocent |
| VFX update + render, 17 modes | 0.021 ms | innocent |

Everything relevant is **sub-millisecond**. The lag was not the work; it was
*how long the app kept working*.

## Cause 1 — the cursor spring chased an unreachable target

Glyphs are emitted at `quill_floorf(x)`, but the cursor rectangle was animated
toward the **unrounded** advance (`font_line_advance = 9.6 px` for Fira Code at
16px, not an integer). The spring therefore never converged, and the settle test
below it required `|pos - target| < 0.05 px` — stricter than the spring's own
tail:

| cursor move | frames to satisfy the old test | time |
|---|---:|---:|
| one column (12.5 px) | 227 | 908 ms |
| one line (16 px) | 195 | 780 ms |

So **every single `p` (or `j`, `h`, click) kept the app in its animating loop for
~0.8 s**, rendering continuously. Then:

## Cause 2 — the loop woke at 250 Hz

`glfwWaitEventsTimeout(0.004)` while animating — four times faster than any
display refreshes. Each wakeup rebuilt the whole draw list and submitted a
Vulkan frame. 227 wakeups per keystroke instead of ~113.

## Cause 3 — smooth blink burned a core at idle

Not the `p` path, but it compounds it: `smooth_blink` polls at 33 Hz and
rebuilds the **entire viewport** to change one quad's alpha.

| idle configuration | jiffies/s (100 = 1 core) |
|---|---:|
| `blink_ms = 0` | 0 |
| step blink, default cadence | 24 |
| smooth blink, default cadence | 23 |

## Fixes applied

1. **`src/frame.c`** — snap the spring target and span to whole device pixels
   (`tx = quill_floorf(tx)`), matching how glyphs are drawn. The spring now has
   a reachable target, and the cursor no longer lands between pixels.
2. **`src/frame.c`** — settle threshold `0.05 px` → `0.5 px` and velocity
   `0.5` → `5.0`, i.e. "within half a pixel and no longer visibly moving"
   instead of chasing the spring's asymptotic tail.
3. **`src/main.c`** — animating wake interval `4 ms` → `8 ms` (matches a 120 Hz
   display; halves the frame count during motion).
4. **`src/main.c`** — step blink sleeps to the next blink edge instead of
   polling; smooth blink paced 33 Hz → 20 Hz.

## Measured result

| metric | before | after |
|---|---:|---:|
| CPU, 1 s after one `p` | 51 jiffies/s | **29 jiffies/s** (−43%) |
| CPU, idle, step blink | 24 jiffies/s | **2 jiffies/s** (−92%) |
| CPU, idle, smooth blink | 23 jiffies/s | **15 jiffies/s** (−35%) |
| cursor animating duration | ~800 ms | settles within the first frame |
| headless single frame | animated continuously | 0 animating frames |

`make check` 17/17, screenshots unchanged, cursor still crisp.

## If `p` is still slow for you, the remaining suspect is `[heap]` churn

All the above is CPU-side and is fixed. The one thing this sandbox cannot
measure is your **RSS trend**. `p` inserts the register payload and grows the
undo vector; if `[heap]` climbs while you paste repeatedly, the cost is glibc
arena churn, not the frame path. Check with:

```sh
while :; do grep -E '^(Rss|Private_Dirty):' /proc/$(pgrep -n quill)/smaps_rollup; sleep 2; done
```

If `Private_Dirty` grows monotonically across `p` presses, that is a separate
allocator bug and I would want the numbers before changing anything.

---

# Round 4 — background image from config

Reported: `background_image = ~/hm.png` in the config does not load.

## There were two bugs, and the second is why it looked like a loading failure

### 1. `~` was never expanded

`config_load_file()` stored the value verbatim and handed it to `stbi_load()`,
which knows nothing about `~`:

```
background: cannot load '~/hm.png'
```

It also meant a *relative* path resolved against the process's current working
directory — so the same config worked when launched from `$HOME` and failed from
anywhere else.

**Fixed** with one `config_resolve_path()` used by every path-valued key
(`background_image`, `theme`, `session_file`) *and* by the `--background` /
`--theme` CLI flags:

- a leading `~` becomes `$HOME` (`~user` is rejected rather than silently wrong)
- a relative path resolves against **the config file's own directory**
- the result is checked with `access(R_OK)` before use
- an unresolvable path now prints the value *and* the base directory it was
  resolved against, then falls back to no background:
  `config: background_image '~/nope.png' not found (resolved relative to '/tmp'); ignoring`

### 2. At the default `editor_opacity = 1.0` the image is loaded and then hidden

`editor_opacity` fades only the chrome, and it defaults to **1.0**. So with a
background configured and no opacity set, the image is decoded, uploaded to a
`VK_FORMAT_R8G8B8A8_SRGB` texture, bound, drawn — and then the editor's
background quad paints over it, fully opaque. The screen looks identical to
"the image did not load", which is exactly how it was reported.

This was documented behaviour (`docs/APPEARANCE.md` says to lower the opacity),
but discovering it required reading the docs after concluding the file was not
loading.

**Fixed**: if a background is configured and `editor_opacity` was *never
explicitly set*, it now defaults to **0.85** instead of 1.0, and says so:

```
config: background_image set with editor_opacity = 1.0 (the image would be
hidden); using 0.85. Set editor_opacity explicitly to silence this.
```

An explicit `editor_opacity` is always respected — including an explicit `1.0`,
which still hides the image, because that is what was asked for.

### 3. Bonus: headless ignored `--config` entirely

`run_headless()` never called `config_load_file()`, so a config-driven
background or theme silently did nothing in `--headless` runs and in CI
screenshots. It now loads the config and lets CLI flags override individual
keys, which is what the interactive path already did.

## Verified

| case | before | after |
|---|---|---|
| `--background '~/hm.png'` | `cannot load` | renders |
| config `background_image = ~/hm.png` | `cannot load` (GUI) / ignored (headless) | renders |
| `--config` + `--background` in headless | config ignored | config loaded, CLI wins |
| relative path, launched from `/` | failed | resolves vs config dir |
| missing file | one bare line | names the value and the base dir |
| no `quill.conf` present | n/a | no longer complains about the default |

Screenshot with `background_fit = cover` on a 1280x800 window confirms the
texture, the sRGB conversion and the editor-chrome fade all composite as
intended. `make check` 17/17.

---

# Round 5 — vendored GLFW: static, Wayland only

`third_party/glfw` is now a git submodule pinned to **3.4** (matching the system
3.4.0 the numbers below are compared against). The Makefile builds it in-tree
into `build/glfw/src/libglfw3.a` with `GLFW_BUILD_WAYLAND=ON`,
`GLFW_BUILD_X11=OFF`, `GLFW_LIBRARY_TYPE=STATIC`, examples/tests/docs off.

This also removed a real breakage: the old `GLFW_LIB` was hardcoded to
`/home/lk/myprojects/voxelfun/mu_editor/build/glfw/src/libglfw3.a`, an absolute
path that exists on exactly one machine.

```
make              # static, Wayland-only (default)
make GLFW=system  # distro libglfw.so, for A/B
```

`git submodule update --init --recursive` is required after a fresh clone; the
Makefile fails with that instruction rather than a confusing link error.

## Linkage verified

| | `ldd` shows glfw | GLFW version string |
|---|---|---|
| static | **no** | `3.4.0 Wayland Null EGL OSMesa` |
| system | `libglfw.so.3` | `3.4.0 Wayland X11 GLX Null EGL OSMesa` |

No X11 objects are linked in the static build. Binary grows 3,393,680 ->
3,706,312 bytes (**+313 KB**), i.e. +42 KB more than the 271 KB of actual
`.text` because of section alignment.

## RAM: no meaningful difference

Same compositor (`weston --backend=x11` nested in Xvfb, so a real `wl_seat`
exists), same 1280x800 window, same `assets/sample.c`, 3 runs each, sampled 4 s
after launch:

| metric (kB) | static Wayland-only | system shared | delta |
|---|---:|---:|---:|
| VmRSS | 96,976 | 97,100 | **-124** |
| PSS | 55,104 | 55,326 | **-221** |
| Private_Dirty | 23,789 | 23,705 | +84 |
| RssAnon | 22,012 | 22,189 | -177 |
| RssFile | 65,427 | 65,689 | -263 |

Run-to-run stdev is 76-253 kB, so the delta is at or below noise. **Static
linking does not save RAM here.** The shared `libglfw.so.3.4` is 397 KB on disk
and only ~270 KB of it is ever faulted in; the static binary faults in the same
code, just from its own segments. Static linking's benefit is deployability (one
binary, no `libglfw` dependency), not memory: `+313 KB` on disk and
`-124 kB` RSS.

## The cost: Wayland-only really is Wayland-only

`GLFW_BUILD_X11=OFF` is a hard cut, not a preference. Verified on an X11-only
session:

```
$ DISPLAY=:77 quill assets/sample.c
glfw: init failed: Wayland: Failed to connect to display (0x10008)
glfw: no usable display server. This build was compiled for: 3.4.0 Wayland Null EGL OSMesa
glfw: WAYLAND_DISPLAY=(unset) DISPLAY=:77
```

So the resulting binary cannot start under X11, XWayland alone, or plain Xorg.
If anything in this project's CI or your own workflow runs on X11, use
`make GLFW=system` (or add `-DGLFW_BUILD_X11=ON`).

## Two bugs found while getting this to run

1. **Silent exit on GLFW init failure.** `main()` was `if (!glfwInit()) return 1;`
   with no message, so a Wayland-only binary on an X11 desktop exited 1 printing
   nothing at all. Now it prints the GLFW error, its numeric code, the platform
   list the binary was compiled for, and `WAYLAND_DISPLAY`/`DISPLAY`.
2. **`weston --backend=headless` has no `wl_seat`.** `libglfw` then calls
   `wl_proxy_get_version(NULL)` inside `_glfwInitWayland` and segfaults — with
   *any* GLFW, shared or static, our build or the distro's. Not a Quill bug and
   not a vendoring bug; use a nested X11 (or real) compositor for headless tests.

Handy for future measurement: a 12-line `glfwInit()` program is enough to tell
"no display server" apart from "Quill is broken" in one shot.

## GLFW_PLATFORMS build flag (follow-up)

The platform is now a flag rather than a hardcoded Wayland-only choice, and each
choice gets its own build directory under `build/` so switching never leaves a
stale library behind:

```
make                        # static GLFW, Wayland only   <- default
make GLFW_PLATFORMS=x11     # static GLFW, X11 only
make GLFW_PLATFORMS=both    # Wayland + X11, chosen at runtime
make GLFW=system            # distro libglfw.so, for A/B
```

An unknown value fails loudly instead of silently defaulting:

```
Makefile:37: *** GLFW_PLATFORMS must be one of: wayland, x11, both (got 'darwin').  Stop.
```

### Verified matrix

| build | on Wayland | on X11 | version string |
|---|---|---|---|
| wayland | **runs** | `Wayland: Failed to connect to display (0x10008)` | `3.4.0 Wayland Null EGL OSMesa` |
| x11 | `X11: DISPLAY is missing (0x1000E)` | **runs** | `3.4.0 X11 GLX Null EGL OSMesa monotonic` |
| both | **runs** | **runs** | `3.4.0 Wayland X11 GLX Null EGL OSMesa monotonic` |

### Cost of carrying both backends (the "overhead on Wayland" question)

Same Wayland session, 3 runs each:

| metric | wayland-only | both, on Wayland | delta |
|---|---:|---:|---:|
| VmRSS (kB) | 96,931 | 97,152 | **+221** |
| PSS (kB) | 55,402 | 55,534 | +131 |
| Private_Dirty (kB) | 23,836 | 23,975 | +139 |
| RssAnon (kB) | 22,019 | 22,205 | +187 |
| CPU (ms/s) | 396 | 429 | +33 |
| binary | 3,706,312 | 3,792,144 | +85,832 bytes |

So the dual-backend build costs roughly **+220 kB RSS**, ~+85 kB of binary, and
CPU inside noise (33 ms/s against a stdev of 24) because the unused X11 code is
never executed.

### And the thing the numbers actually show

The interesting number was not Wayland-vs-both, it was **Wayland vs X11**:

| | Wayland | X11 | delta |
|---|---:|---:|---:|
| VmRSS | 96,931 | **102,499** | Wayland is 5,568 lower |
| PSS | 55,402 | **64,780** | Wayland is 9,377 lower |
| Private_Dirty | 23,836 | **36,411** | Wayland is 12,575 lower |

X11 costs ~5.5 MB more RSS and ~12.6 MB more Private_Dirty (Xlib/XCB per-connection
buffers and the xkb context are process-private, and llvmpipe's Xlib WSI keeps
client-side staging buffers). So the *reason to prefer Wayland is memory*, and the
wayland-only build is already the right default. But Wayland burns about 2x the CPU
(396 vs 187 ms/s) at idle in this llvmpipe setup — worth knowing, not worth
changing anything over yet.

---

# Round 6 — pulled correctness suites, and the bugs they found

Two suites were pulled rather than written from scratch, because both test code
we ship and both had been thrown away in vendoring.

## 1. libvterm's own upstream test suite

`third_party/libvterm/` ships `src/` and `include/` only — upstream's entire
`t/` directory was dropped at vendoring time, so the terminal emulator had **zero
coverage** despite being the component most likely to be subtly wrong.

Pulled from upstream at commit **`d8a68a8`** (2023-08-03), which is the exact
commit our `src/screen.c` and `src/state.c` are byte-identical to. Running a
newer suite would have tested `VTERM_PROP_FOCUSREPORT`, `fallback.c` and the
`premove` callback, none of which we vendor.

`tests/upstream/libvterm/` now holds 42 `.test` files, `harness.c`, `run-test.pl`
and the MIT licence. `tests/run_upstream_libvterm.sh` compiles the harness
against `build/vterm_*.o` — our objects, not upstream's — and runs every file.
Wired into `make check` and available as `make upstream-vterm`.

**42/42 files, 386 individual test cases pass.** Coverage includes cursor
movement, scroll regions, edit operations, wrapping, tab stops, save/restore,
resize, mouse, pen attributes, double-width/high characters, and the classic
`vttest` screen renders.

Verified the suite is not a no-op two ways:

| check | result |
|---|---|
| corrupt an expectation in a `.test` file | exit 1, `# line 12: Test failed` |
| sabotage CUU with an off-by-one in `state.c` | 39/42 pass, 3 files fail |

## 2. `docs/MODAL.md` conformance — `tests/test_modal_spec.c`

`test_modal.c` drives 14 command sequences. MODAL.md documents far more, so the
doc and the editor were free to drift. The new suite drives the documented
commands `test_modal.c` never touches: **95 checks**, in `make check`.

It feeds input the way `src/main.c` actually routes it. This matters: printable
keys are deferred from the key callback to the char callback and reach
`modal_printable()`, not `modal_key()`. `test_modal.c` calls `modal_key()`
directly, so it cannot see that path at all — which is why none of the bugs
below had ever been caught.

It reports every discrepancy in one run instead of aborting on the first.

### Bugs found and fixed

| # | symptom | cause | fix |
|---|---|---|---|
| 1 | `dd` then `.` then `j` **deleted two lines** | `execute_op` recorded a bare `"d"`; replaying it arms the operator without a motion, so the next key becomes a destructive motion | record operator **and** motion (`"dd"`, `"dw"`, `"2dw"`); added `op_motion_char`/`op_motion_count` to `ModalState` |
| 2 | `>>` indented **the whole buffer** | `>>` fell through to `operator_motion('>')`, and `'>'` is also a bracket text object, so it resolved to a motion running to end-of-buffer | `>>`/`<<`/`==` take the linewise branch; count consumes *n* lines |
| 3 | `>>` indented **one line too many** | ranges are half-open `[start,end)` but `indent_ranges` used `line_no(end)`; `line_end()` points *at* the newline, and that offset maps to the next line | derive the last line from `end-1`, and made `expand_linewise` line-accurate |
| 4 | `$` landed **on the newline**, so `x` joined lines | `motion_target('$')` assigned `line_end()` | added a `last` (last character) offset used by `$` and `l` |
| 5 | `di"` inside `"hi there"` produced `"h"` | quote object walked back to the opening quote, then used the cursor's column as the start | rewrote the quote text object to pair opening/closing quotes properly |
| 6 | `W`, `B`, `E` did **nothing** | `motion_target()` handled them, but the plain-motion dispatcher's `if` never listed them | added to the dispatch list (word class still shared with `w/b/e`, as documented) |

`dd`→`.`→`j` is the serious one: silent, reproducible data loss with no undo
prompt, on the most ordinary vim sequence there is.

### Recorded as known discrepancies, not asserted as correct

* **Ctrl-d / Ctrl-u** are listed under motions in MODAL.md but only raise a
  scroll request; the cursor does not move. The test asserts the current
  behaviour and names the doc discrepancy, so fixing it will flip obviously.
* **`ip`/`ap`** do not follow vim's blank-line rules.
* **`gu`/`gU`/`g~`** are not replayed by `.`.
* **`W`/`B`/`E`** share the `w`/`b`/`e` word class — documented as such.

`ip`/`ap` and the `.`-replay gaps are left as findings rather than "fixed",
because guessing intended behaviour is how a conformance suite starts asserting
bugs.

## Verification

* `make check` — **18/18** (17 existing + `test_modal_spec`) plus 42/42 libvterm.
* Screenshots unchanged: 48–74 differing pixels of 1,024,000 against a
  pre-fix build. That is the established animation noise floor (an *identical*
  binary differs by 186 px); a real layout change shows up in the thousands.
* `test_modal`'s 14 original sequences still pass, so the operator changes did
  not regress what was already covered.

---

# Round 7 — third_party size

## What was actually removable

Most of `third_party` is not removable. The four multi-megabyte files are
**generated tree-sitter grammars** that the build compiles directly, and there is
no smaller form without regenerating them from an npm toolchain:

| file | size | verdict |
|---|---:|---|
| `rust/parser.c` | 6.3 MB | compiled by `$(BUILD)/rust_parser.o` — keep |
| `c/parser.c` | 3.7 MB | compiled — keep |
| `python/parser.c` | 3.3 MB | compiled — keep |
| `javascript/parser.c` | 2.8 MB | compiled — keep |
| `tree-sitter/*.c` | 708 KB | `lib.c` is one TU including all 13 — keep |
| `stb/stb_image.h` | 283 KB | `render_vk.c` needs it — keep |
| grammar `LICENSE` / `highlights.scm` / `tree_sitter/*.h` | ~200 KB | compiled or licence obligation — keep |

Everything prunable was inside the glfw submodule.

## GLFW: 5.2 MB -> 2.2 MB

| removed | size | why it was dead |
|---|---:|---|
| `deps/nuklear.h` + `nuklear_glfw_gl2.h` | 984 KB | example-only UI toolkit, no `#include` in `src/` |
| `deps/glad/` | 732 KB | example-only GL loader |
| `deps/mingw/` | 124 KB | Windows cross-build headers, Linux-only project |
| `deps/stb_image_write.h` | 72 KB | examples write screenshots; GLFW core does not |
| `deps/tinycthread.*`, `linmath.h`, `getopt.*` | 60 KB | shared-memory + example helpers |
| `docs/` | 565 KB | glfw.org HTML |
| `tests/` | 292 KB | upstream test programs |
| `examples/` | 209 KB | upstream example programs |

`deps/wayland/*.xml` is **not** pruneable even though it looks like junk:
`src/CMakeLists.txt:80` feeds those files to `wayland-scanner` and
`src/CMakeLists.txt:74` hard-fails if the directory is missing. GLFW also ships
its own copies rather than using `/usr/share/wayland-protocols` — the
`xdg-shell.xml` checksums differ — so they must stay.

The prune is a Makefile rule (`prune-glfw`), stamped with
`third_party/glfw/.pruned`, and runs automatically before the library is
configured. That matters: pruning by hand is not reproducible, because a fresh
clone checks out the submodule clean and gets all 5.2 MB back.

## Submodule history: 19 MB -> 1.9 MB

`.git/modules/third_party/glfw` held glfw's full **4799-commit** history and was
the single largest thing in a checkout — invisible to `du third_party` because it
lives under `.git`. Fixed with `git submodule update --init --depth 1
--single-branch`, now what `make submodule` runs.

Note `submodule.third_party/glfw.shallow = true` in `.gitmodules` alone does
**not** work: a fresh `git submodule update --init --recursive` was measured
fetching all 4799 commits (16 MB) anyway. The `--depth 1` flag in the target is
what actually does it.

## Also fixed: the submodule pin was wrong

Adding the submodule had recorded glfw **master** (`92dcf4ce`, "Start 3.6") as
the gitlink, not the 3.4 tag. Round 5 measured and documented 3.4, so the repo
recorded a commit nobody had tested. The gitlink is now
`7b6aead9fb88b3623e3b3725ebb42670cbe4c579` (3.4) and `.gitmodules` carries
`branch = 3.4`.

## Totals

| | before | after |
|---|---:|---:|
| `third_party/` | 23 MB | 20 MB |
| of which glfw | 5.2 MB | 2.2 MB |
| glfw submodule history | 19 MB | 1.9 MB |
| **whole checkout** | **45 MB** | **25 MB** |

## Verified

* `make` for `GLFW_PLATFORMS=wayland|x11|both` — all build; sizes
  3,706,312 / 3,694,360 / 3,792,144, unchanged from Round 5.
* `make check` — 18/18, plus libvterm 42/42.
* The linked library reports `3.4.0 Wayland Null EGL OSMesa`, confirming the
  pruned tree still produces the intended build.
* End-to-end from a synthetic fresh clone: `make submodule` -> 2.2 MB tree +
  1.9 MB history, then `make` produced a byte-identical 3,706,312-byte binary.
* One scary-looking failure during this round was **not** the prune:
  `Xinerama headers not found` came from the sandbox losing its X11 dev packages,
  not from anything deleted here. Worth recording because the error surfaces at
  the same CMake line either way.
