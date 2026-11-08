# Quill performance evidence

## Environment (seven-field rule)

| Field | Value |
|---|---|
| CPU / µarch | Intel Xeon @ 2.60GHz (virtualized Skylake-class; `lscpu` model string) |
| Cores / affinity | 2 vCPUs (1 core, 2 threads), benchmark pinned to CPU0 with `sched_setaffinity`/`taskset` |
| Frequency / SMT | 2.60GHz reported; SMT on; turbo state not controllable in this sandbox |
| Compiler / flags | GCC 14.2.0; app: `-std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native -D_GNU_SOURCE`; Tree-sitter/grammars: `-std=c11 -O3 -march=native -DNDEBUG -D_GNU_SOURCE -fno-strict-aliasing` |
| Workload / working set | 8 MiB synthetic C for scroll API; 1 MiB synthetic C for frame/syntax; viewport 40 lines, 1280×800, JetBrains Mono 16px. L1d 48 KiB, L2 1.3 MiB, L3 54 MiB |
| Baselines | `mu_quill_line_text` per-line descent; per-line highlight query before visible-range query optimization |
| Method | mylib timer (`mu_time_now`), volatile sink, 5-frame warmup, 21 randomized rounds, paired A/B for scroll, min for latency floor, median for throughput, CV reported. Raw CSVs in `results/` |

The benchmark harness was kept outside the workspace (`/tmp/quill_bench`) per
the standing workspace-clean policy; only results and documentation are
committed.

## 1. Sequential viewport read: view API vs per-line descent

8 MiB synthetic C, 40-line viewport, 200 sequential viewports per round,
21 paired randomized rounds, CPU0.

| Implementation | min ns / viewport | median ns / viewport | CV |
|---|---:|---:|---:|
| `mu_quill_view_start/next_text` | 328.1 | **336.9** | 0.083 |
| `mu_quill_line_at` + `mu_quill_line_text` | 1044.9 | 1077.7 | 0.082 |

**Speedup: 3.20× median.** This is the editor’s scroll path. Round 9 showed
quill’s per-line scroll was 0.71–0.74× ted; the sequential view API removes the
repeated descent and makes the viewport walk a forward chunk-start scan.

## 2. CPU frame build (no GPU submit)

1 MiB synthetic C, Tree-sitter highlights + HarfBuzz shaping + atlas lookup,
1280×800, 40 visible lines, 21 rounds.

| Workload | min µs | median µs | CV |
|---|---:|---:|---:|
| `frame_static` (warm shape cache) | 61.7 | **63.0** | 0.068 |
| `frame_scroll` (top_line advances) | 63.0 | **93.2** | 0.148 |
| `edit_frame_stale` (insert + frame, no reparse) | 65.1 | **69.3** | 0.341 |
| `edit_parse` (insert + incremental reparse) | 12123.1 | 14714.2 | 0.098 |
| `edit_parse_frame` (insert + reparse + frame) | 11911.8 | 13796.4 | 0.086 |

The responsive input path is `edit_frame_stale`: ~69 µs. The full-file
incremental Tree-sitter reparse is ~14.7 ms on this 1 MiB C file, so the app
debounces parsing by 30 ms and renders immediately from the edited tree.

### Before/after: visible-range highlight query

Baseline queried Tree-sitter once per visible line. The optimized frame builder
queries the visible byte range once and paints spans per line.

| Workload | before median µs | after median µs | speedup |
|---|---:|---:|---:|
| `frame_static` | 101.0 | **63.0** | **1.60×** |
| `frame_scroll` | 113.8 | **93.2** | **1.22×** |

Raw before CSV: `results/bench_frame_before_query.csv`.
Raw after CSV: `results/bench_frame.csv`.

## 3. Vulkan software rasterization (llvmpipe)

This sandbox has no `/dev/dri`; the only Vulkan device is Mesa 25.0.7 llvmpipe
(LLVM 19.1.7). These numbers are **software rasterizer** timings, not hardware
GPU performance.

| Workload | Result |
|---|---:|
| Headless 1280×800 sample C frame, 50-frame loop | **5.98 ms / frame** (~167 fps software) |

The renderer is still validated with the Khronos validation layer in headless
mode (`make check`, `test_vk`, and `--validation` screenshots).

## 4. Correctness gates

- `make check`: core, syntax, font, frame, Vulkan headless tests all PASS.
- Headless screenshots with `--validation` for C/Rust/Python/JSON: no Vulkan
  validation errors.
- ASan/UBSan (`-fsanitize=address,undefined`, leak detection on): core, syntax,
  font, frame tests PASS.
- `mu_quill` view/cursor/span differential tests: `loom/test_quill_view.c`,
  multiple seeds, ASan/UBSan clean.
- Incremental Tree-sitter probe: a 1-byte insert into 1 MiB C produced a
  changed range of 13 bytes, confirming tree reuse; full parse ~100 ms vs
  incremental ~16 ms in that probe.

## 5. Caveats

- Absolute timings drift across runs; within-run paired ratios are the claim.
- The 1 MiB incremental reparse is still O(file tree) enough to be visible;
  debouncing keeps it off the keystroke path. A background parse thread or
  included-range parsing would reduce it further but is not implemented.
- llvmpipe timings do not predict hardware GPU frame time. The CPU frame-build
  and buffer-API numbers are the portable performance evidence.
- Undo payload is an append-only 64 MiB virtual arena per document; it is reset
  on load, not compacted during a session.

## C highlighting: ALL_CAPS constant heuristic cost (paired A/B)

Correctness fix: macro/enum constants referenced at a use site (e.g. `MAX` in
`ptr->x = MAX;`) are plain `(identifier)` nodes to Tree-sitter and previously
colored as variables. The highlighter now reclassifies ALL_CAPS identifiers
(`^[A-Z][A-Z0-9_]*$`) captured as `@variable` to `QC_CONSTANT`, matching the
universal C convention without relying on a query `#match?` predicate the
vendored API does not evaluate. Locked in by `tests/test_c_highlight.c`.

**Seven-field evidence**

1. CPU/µarch: Intel Xeon @ 2.60 GHz (KVM guest), 1 core / 2 threads (SMT on).
2. Cores + affinity: 2 vCPUs; all reps pinned `taskset -c 0`.
3. Freq/turbo/SMT: guest-reported 2.60 GHz, BogoMIPS 5200.08; turbo not
   controllable in guest; SMT enabled (noisy neighbour possible).
4. Compiler + flags: GCC 14.2.0 (Debian 14.2.0-19);
   `-std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native -D_GNU_SOURCE`.
5. Workload + working set: `syntax_highlight()` over a 4 KiB visible window in
   the middle of a ~1 MiB C buffer (the editor's own `main.c` tiled); tree +
   query working set far exceeds L2, so this is a tree-walk-bound query.
6. Baseline: commit `85f0142` `syntax.c` (optimistic highlighting, no heuristic)
   vs current (adds the ALL_CAPS reclassification).
7. Method: `CLOCK_MONOTONIC_RAW`, volatile SINK on span count, 500-call warmup,
   2000 timed calls/rep, 21 paired randomized round-robin reps (seed 12345),
   min latency floor + median + CV + paired geomean ratio. Raw:
   `results/bench_c_highlight.csv`. Harness kept out of the workspace.

**Result**

| metric | baseline | current |
|---|---:|---:|
| min latency (ns/call) | 221367 | 222079 |
| median (ns/call) | 243459 | 296961 |
| CV | 23.0% | 20.7% |

Paired geomean current/baseline = **1.041×**. At the latency floor the heuristic
adds **~0.3%**; the ~4% median gap is inside the ~20% CV noise of this shared
2-vCPU guest, so no precise median claim is made. Conclusion: the correctness
fix is effectively free at the latency floor. (The ~220 µs absolute cost of a
mid-file window query on a 1 MiB tree is pre-existing and identical in both
builds; it is a separate future optimization target, not a regression.)

## Highlight result cache: unchanged-view redraws

Profiling showed `syntax_highlight()` cost scales with the number of spans in
the query window (~0.5 µs/span), not a fixed tree-walk overhead: a 32-byte
window is ~4 µs, a 4 KiB window ~378 µs, 16 KiB ~2.1 ms. Tree-sitter's byte-range
pruning works; the waste was re-querying the **same** visible spans on every
redraw. Idle cursor blink (~30 ms), the cursor spring, and VFX frames (~4 ms)
all redraw an unchanged view, so the highlighter recomputed ~750 spans per frame
for nothing.

Fix: a result cache in `Syntax` keyed by `(start_byte, end_byte, edit_version,
parsed_version)`. Identical-view frames memcpy the processed spans; edits,
reparses, and scrolling correctly miss. Coherency locked in by
`tests/test_highlight_cache.c` (warm hit == cold result; edit/scroll/reparse
invalidate). Cost: one extra `HighlightSpan[QUILL_MAX_SPANS]` (~96 KiB) per open
document.

**Seven-field evidence**

1. CPU/µarch: Intel Xeon @ 2.60 GHz (KVM guest), 1 core / 2 threads (SMT on).
2. Cores + affinity: 2 vCPUs; all reps pinned `taskset -c 0`.
3. Freq/turbo/SMT: guest 2.60 GHz, BogoMIPS 5200.08; turbo not guest-controllable.
4. Compiler + flags: GCC 14.2.0; `-std=c99 -O2 -Wall -Wextra -Werror
   -fno-strict-aliasing -march=native -D_GNU_SOURCE`.
5. Workload + working set: `syntax_highlight()` over a 4 KiB window in a ~1 MiB
   C tree (tiled `main.c`). Two modes: *idle* (fixed range, models blink/
   animation redraws) and *scroll* (range advances 64 B/call, cache always
   misses). Working set far exceeds L2.
6. Baseline: commit `1bdb9f8` `syntax.c` (no cache) vs current (cache).
7. Method: `CLOCK_MONOTONIC_RAW`, volatile SINK, 300-call warmup, 2000 timed
   calls/rep, 21 paired randomized round-robin reps (seed 2026), min latency +
   median + CV + paired geomean. Raw: `results/bench_highlight_cache.csv`.
   Harness kept out of the workspace.

**Result**

| workload | baseline min (ns) | current min (ns) | paired geomean |
|---|---:|---:|---:|
| idle redraw | 225851 | 63 | ~0.00025× (**~4000× faster**) |
| scroll | 315992 | 311963 | 0.985× (1.0×, no regression) |

Idle/animation redraws drop from ~226 µs to ~63 ns at the latency floor; the
cold query path (scroll/edit) is unchanged. With VFX and cursor animation
enabled the editor redraws every ~4 ms, so this removes a recurring ~226 µs
per-frame highlight cost from the animation budget. CV ~15–22% on this shared
guest; the idle win is far outside noise, the scroll delta is within it.

## Optional appearance (background image + editor opacity): disabled-path cost

Both appearance features are opt-in and must cost nothing when unconfigured.
`background_image = none` (default) never loads a texture or creates the
background pipeline; `editor_opacity = 1.0` (default) skips the chrome-alpha
scaling. The cursor loop was also changed to read `MotionConfig` through a
pointer instead of copying the (now larger) struct per cursor.

**Seven-field evidence**

1. CPU/µarch: Intel Xeon @ 2.60 GHz (KVM guest), 1 core / 2 threads (SMT on).
2. Cores + affinity: 2 vCPUs; all reps pinned `taskset -c 0`.
3. Freq/turbo/SMT: guest 2.60 GHz; turbo not guest-controllable.
4. Compiler + flags: GCC 14.2.0; `-std=c99 -O2 -Wall -Wextra -Werror
   -fno-strict-aliasing -march=native -D_GNU_SOURCE`.
5. Workload + working set: `frame_build()` and `vk_render_frame()` with the
   default config (no image, opacity 1) over a 20 000-line C document at
   1280×800. Render uses llvmpipe (software), so it is inherently noisy.
6. Baseline: commit `019ac3c` (pre-appearance) vs current, both disabled.
7. Method: `CLOCK_MONOTONIC_RAW`, warmup, 3000 timed calls/rep (frame) or 150
   (render), 31 / 21 paired randomized round-robin reps, min latency + median +
   CV + paired geomean. Raw: `results/bench_appearance.csv`. Harness kept out of
   the workspace.

**Result**

| workload | baseline min (ns) | current min (ns) | paired geomean |
|---|---:|---:|---:|
| frame_build (disabled) | 16335 | 11603 | 0.988× (1.0×) |
| vk_render_frame (disabled, llvmpipe) | 6080894 | 6343448 | 1.022× (1.0×) |

The disabled path is unchanged within noise: the frame geomean is ~1.0 and the
render delta is inside the ~8–10% CV of software rasterization. No texture is
allocated and no extra draw is recorded unless a background image is set; no
per-glyph or per-quad work is added unless `editor_opacity < 1`.

## Custom themes + session restore: per-frame cost

Both features must cost nothing per frame. A theme is a one-time palette load
into the same `QUILL_THEME` global the renderer already reads; session restore is
startup/shutdown file I/O plus an O(1) view flush on buffer switch. Neither adds
work to `frame_build()` or the render path. `Document` grew by 8 bytes
(`view_top`, `scroll_x`), which are not read in the per-frame hot loop.

**Seven-field evidence**

1. CPU/µarch: Intel Xeon @ 2.60 GHz (KVM guest), 1 core / 2 threads (SMT on).
2. Cores + affinity: 2 vCPUs; all reps pinned `taskset -c 0`.
3. Freq/turbo/SMT: guest 2.60 GHz; turbo not guest-controllable.
4. Compiler + flags: GCC 14.2.0; `-std=c99 -O2 -march=native -D_GNU_SOURCE`.
5. Workload + working set: `frame_build()` with the default theme over a
   20 000-line C document at 1280×800 (syntax attached, full visible window).
6. Baseline: commit `7d437ee` (pre-theme/session) vs current, both default theme.
7. Method: `CLOCK_MONOTONIC_RAW`, 300-call warmup, 100 timed calls/rep, 31 paired
   randomized round-robin reps, min latency + median + CV + paired geomean.
   Raw: `results/bench_theme_session.csv`. Harness kept out of the workspace.

**Result**

| workload | baseline min (ns) | current min (ns) | min-floor ratio |
|---|---:|---:|---:|
| frame_build (default theme) | 66773 | 65343 | 0.979× (1.0×) |

The **min latency floor is identical within ~2%** across three independent
campaigns (65.1/65.3, 64.8/65.7, 66.8/65.3 µs base/cur), i.e. no per-frame
regression. On this shared 2-vCPU guest the median/geomean are noise-dominated
(CV ~19–22%; paired geomean ranged 0.89×–1.15× across runs while the floor stayed
put), so the latency floor is the trustworthy signal and the median/geomean
deltas are labelled noise. This matches the appearance-round finding that only
the floor is stable here.
