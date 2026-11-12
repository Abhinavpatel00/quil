# Quill modal editing, multicursor, motion, and picker

This document describes the implemented Round-11 feature set from
`MODAL_PLAN.md`. The editor remains Vulkan + `mu_quill` + Tree-sitter only;
there is no LSP, completion, Ex parser, Vimscript, macros, splits, terminal,
marks, or jump-list.

## Modes

- **Normal** is the default mode.
- **Insert**: `i a o I A O s S`, operator `c`, and dot-replayed inserts.
- **Visual char**: `v`.
- **Visual line**: `V`.
- **Visual block**: `Ctrl-v`.
- **Operator-pending**: `d c y > < g u/U/~ g c` plus a motion or text object.
- `Esc` returns to Normal; in Normal with multiple cursors it collapses to the
  primary cursor.

The status line shows the mode, path, dirty flag, line/column, language, and
cursor count.

## Motions and counts

Counts are supported before operators and motions, e.g. `3j`, `2dw`, `d3w`,
`5gg`.

Implemented motions:

```
h j k l w W b B e E 0 ^ $ gg G
f{char} F{char} t{char} T{char} ; ,
% { }
Ctrl-d Ctrl-u Ctrl-f Ctrl-b
zz zt zb
/ ? n N * # gn
```

`W/B/E` currently use the same word-class motion as `w/b/e` (punctuation is not
yet a separate WORD class). Search is single-line literal with smartcase and
wrap-around.

## Operators and text objects

```
d c y dd cc yy D C Y x X s S r{char} p P J ~ gu gU g~ > < == gc .
```

Classic text objects:

```
iw aw iW aW i" a" i' a' i` a`
i( a( i) a) ib ab i[ a[ i] a] i{ a{ i} a} iB aB i< a< i> a>
ip ap
```

Multicursor edits are applied through `SelectionSet` and grouped undo: one
`u` undoes the whole batch.

## Registers

Implemented register file:

```
" 0 a-z + * _ -
```

`+` and `*` use the GLFW clipboard when a window exists. The unnamed register
`"` is synced to the same clipboard on yank/delete, so `yy` / `y` in Visual
mode puts text into the system clipboard -- including on Wayland -- where it
can be pasted into other applications. Conversely, `p` / `P` read the system
clipboard, so text copied from another application pastes directly; when the
clipboard is empty or unavailable they fall back to the local unnamed
register. A linewise yank/delete stays linewise on paste as long as the
clipboard still holds that same text. The black-hole register `_` discards
yanks/deletes without touching the clipboard. Registers are fixed-capacity
(`QUILL_REG_CAP = 4096`) and stored in `ModalState`.

## Multicursor

Hybrid selection-set model:

- `Ctrl-n`: select word under cursor, then add next occurrence.
- `Ctrl-Alt-p`: remove last cursor.
- `Alt-click`: add a cursor.
- `<Space>ma`: select all occurrences of the current word/selection.
- `<Space>mc`: collapse to primary.
- `<Space>mn`: add next occurrence.

`SelectionSet` keeps anchors/heads, normalizes overlapping ranges, and applies
multi-range replacements in descending offset order inside one undo group.

## Picker

Leader is `Space`.

```
<Space><Space>   recent (top five)
<Space>f         file picker
<Space>s w       grep the word under the cursor across the workspace
Ctrl-p           files
Space c          commands
Space k          keymaps
Space h          help
Space t          terminal
Space w          save
Space q          quit
:                command palette
```

`<Space>s w` seeds the live grep with the word under the cursor, so the picker
opens on real hits; a visual selection is used instead of the single word when
one is active. With the cursor on whitespace (or an empty buffer) it falls back
to the plain grep prompt. Editing the query narrows the results as usual.

Picker features:

- fuzzy matcher for files/commands/symbols;
- literal case-insensitive live grep;
- Tree-sitter document symbols;
- async project symbol scan;
- preview pane;
- cancellable pthread worker with generation-based cancellation;
- buffer manager and recent-file list.

## Motion configuration

`quill.conf` (or `--config path`) supports:

```ini
style = ease              # ease | linear | instant
scroll_ms = 300
scroll_far_lines = 1
cursor_ms = 150
trail_size = 1.0
blink_ms = 530
smooth_blink = true
animate_insert = true
picker_ms = 120
reduced_motion = false
dramatic = false          # opt-in dramatic camera
dramatic_lead_x = 0.30
dramatic_lead_y = 0.33
dramatic_ms = 180
```

Motion is Neovide-style rather than a fixed ease from a start time:

- Scroll uses a critically damped spring on `view_top` toward `target_top`.
  Jumps larger than `scroll_far_lines` snap all but the last line, so `gg/G`
  and large wheel jumps stay responsive while short scrolls animate.
- The frame receives an integer `top_line` plus fractional `scroll_y_offset`.
- Cursor visibility scrolls both axes: `scroll_x` follows the primary cursor
  horizontally by the smallest necessary amount, using the renderer's shaped
  lines so tabs and UTF-8 byte columns land at the correct pixel position.
- Each cursor rectangle (`x/y/w/h`) is spring-animated, including the
  Insert-bar ↔ Normal-block shape change.
- Moving cursors emit a short fading trail (`trail_size`, 0 disables).
- Blink is a cosine fade when `smooth_blink = true`; `blink_ms` is the
  half-period.
- `Ctrl-d/u/f/b` and PgUp/PgDn move the cursor and scroll the viewport by the
  same amount, preserving screen-relative cursor position.
- `reduced_motion = true` or `style = instant` snaps scroll/cursor and disables
  blink/trail animation.

### Dramatic camera

`dramatic = true` swaps the scroll *policy* for a camera: instead of scrolling
the minimum distance needed to reveal the cursor, the view parks the cursor at a
fixed lead point of the viewport (`dramatic_lead_x` of the text area,
`dramatic_lead_y` of the viewport) and lets the springs carry the page there, on
both axes, over `dramatic_ms`. Every cursor move drags the page under a cursor
that barely appears to move; targets are clamped at the top and left edges and
at the end of the line, so the first and last screens of a file behave exactly
as they do without it.

It is a pure scroll policy: it writes the same `target_top` and target `scroll_x`
the normal path writes, so rendering, hit-testing, selection, and the cursor
animation need no camera-aware code and the draw list keeps final pixel
coordinates. Toggle it at runtime with the `Dramatic Mode` palette command.

Off is the zero-overhead path and stays the default: no spring state is
consulted, no extra primitives are emitted, and the only cost is one predictable
branch per frame. `test_dramatic` pins both halves down -- the lead park, the
clamps, spring convergence and reduced-motion snapping when it is on, and
"view follows target immediately, no spring" when it is off.

The main loop uses `glfwWaitEventsTimeout`: ~4 ms while motion is active,
~30 ms for picker/blink, and 100 ms when fully idle. `test_animation` verifies
spring convergence, intermediate cursor positions, trail settle, and
reduced-motion snapping.

## Modules

- `search.{c,h}`: word range, literal find, find-all.
- `selection_set.{c,h}`: multicursor selection set and grouped multi-replace.
- `modal.{c,h}`: mode machine, motions, operators, registers, dot, requests.
- `picker.{c,h}`: fuzzy picker, async worker, preview, overlay rendering.
- `buffers.{c,h}`: buffer manager and recent files.
- `config.{c,h}`: motion configuration.
- `syntax_symbols`: Tree-sitter symbol extraction.
- `frame`: multicursor/selection rendering, smooth scroll offset, mode-aware
  cursor/status, search-match highlights, exported overlay draw helpers.

## Tests

`make check` now runs:

```
test_core test_syntax test_font test_frame test_modal test_picker test_animation test_vk
```

plus C/Rust/Python headless validation screenshots. `test_modal` covers insert,
undo, `dd`, `yy/P`, `dw`, counts, `x`, visual delete, multicursor `Ctrl-n` +
grouped undo, `diw`, search, and basic motions. `test_picker` covers fuzzy
scoring, config defaults, buffer manager, command picker, and async file
picker open/query/close. `test_animation` covers cursor springs, trail settle, and reduced motion.

ASan/UBSan runs for `test_modal` and `test_picker` pass in the development
sandbox. GPU screenshots are llvmpipe software renders.

## Known gaps

- Dot repeat is reliable for inserts and simple Normal commands; complex
  operator+motion replay is not fully recorded yet.
- `W/B/E` are not yet true WORD motions.
- No Ex command-line parser; `:` opens the command palette.
- No macros, marks, jumps, splits, tabs, terminal, expression registers.
- Picker project-symbol scan parses files sequentially in the worker and is
  capped by `QUILL_PICKER_RESULTS`.

## Round-11 measured overhead

Raw data: `results/bench_modal.csv`. Harness lived in `/tmp` and is not
committed. Environment: Debian 13 sandbox, Intel Xeon @ 2.60 GHz, 2 vCPUs,
pinned CPU 0 (`taskset -c 0`), llvmpipe software Vulkan device (not used for
these CPU timings), GCC 14.2.0, app flags `-std=c99 -O2 -Wall -Wextra -Werror
-fno-strict-aliasing -march=native -D_GNU_SOURCE`. Workload: 1 MiB C document
(~31k lines of `int foo_bar(int x) { return x; }`), 1280×800 viewport, 40-line
visible frame, shaping/highlight caches warm. Method: `mu_time` monotonic
timer, volatile sink, one warmup frame, 21 paired randomized round-robin reps.

| Path | min | median | CV |
|---|---:|---:|---:|
| frame, 1 cursor | 165.9 µs | 185.3 µs | 7.4% |
| frame, 64 cursors | 242.5 µs | 264.1 µs | 9.5% |
| multi-replace, 1 range | 7.7 µs | 10.1 µs | 46.4% |
| multi-replace, 64 ranges | 5.8 µs | 7.0 µs | 173.6% |
| fuzzy score, 1 query/path | 32.7 ns | 33.7 ns | 4.7% |

The 64-cursor frame adds about **1.48×** median frame-build cost versus one
cursor in this synthetic workload (geomean of paired ratios). This is a
CPU-side draw-list build measurement, not a GPU presentation measurement.
The multi-replace microbench is noisy in this sandbox (high CV); it is kept as
raw evidence rather than a speed claim.
