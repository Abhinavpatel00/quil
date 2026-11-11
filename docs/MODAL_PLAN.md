# Quill modal editing, multicursor, smooth motion, and picker plan

Status: **approved plan, not implemented yet**.

This plan was discussed before implementation. The agreed scope is:

- Neovim-like **fast-editing core**, not full Neovim compatibility.
- **Hybrid multicursor**: Vim-native visual/search workflows plus VS Code-style add-next/mouse.
- **Full Telescope-like picker**.
- **Configurable smooth scrolling/cursor motion**, default subtle/fast.
- Default leader: **Space**.
- Multicursor add-next: **Ctrl-n**, keeping `Ctrl-d`/`Ctrl-u` for half-page scroll.
- No LSP. No autocompletion.

---

## 1. Mode model

Modes:

| Mode | Purpose |
|---|---|
| Normal | motions, operators, counts, registers, search, mode entry |
| Insert | text entry, `Esc` / `Ctrl-[` to Normal |
| Visual char | `v` characterwise selection |
| Visual line | `V` linewise selection |
| Visual block | `Ctrl-v` rectangular selection |
| Operator-pending | operator waiting for motion/text object |

Multicursor is modeled as a **selection set**, not a separate buffer or unrelated editing mode.
Normal/Insert/Visual operate over the active selection set. With one cursor, behavior is ordinary Vim-like editing.
With multiple cursors, the same modes/operators apply across the set.

Selection set sketch:

```text
SelectionSet {
    cursors[]: anchor, head, preferred_col, animation_state
    primary_index
    count
}
```

Edits are applied in stable order, usually descending by offset.
Other selections are remapped through each edit.
A multicursor edit is one undo group.

---

## 2. Fast-editing core

### Motions

| Binding | Meaning |
|---|---|
| `h j k l` | basic movement |
| `w W b B e E` | word/WORD motions |
| `0 ^ $` | line start / first non-blank / line end |
| `gg G` | buffer start/end |
| `{count}j/k` | counted line movement |
| `f F t T` | find character forward/backward |
| `; ,` | repeat last character find |
| `%` | matching bracket |
| `{ }` | paragraph movement |
| `Ctrl-d Ctrl-u` | half-page down/up |
| `Ctrl-f Ctrl-b` | full-page down/up |
| `zz zt zb` | center/top/bottom cursor in viewport |
| `/ ? n N * #` | search and search-word navigation |

### Operators

| Binding | Meaning |
|---|---|
| `d c y` | delete/change/yank |
| `dd cc yy` | linewise delete/change/yank |
| `D C Y` | to end of line |
| `x X` | delete char under/before cursor |
| `s S` | substitute char/line |
| `r` | replace char |
| `p P` | put after/before |
| `J` | join lines |
| `~` | toggle case |
| `gu gU g~` | lowercase/uppercase/toggle case |
| `> <` | indent/unindent |
| `==` | simple auto-indent |
| `.` | repeat last change |

### Classic text objects

| Binding | Meaning |
|---|---|
| `iw aw` | inner/around word |
| `iW aW` | inner/around WORD |
| `i" a"` | double quotes |
| `i' a'` | single quotes |
| `` i` a` `` | backticks |
| `i( a(` | parentheses |
| `i[ a[` | brackets |
| `i{ a{` | braces |
| `i< a<` | angle brackets |
| `ip ap` | paragraph |

### Registers

| Register | Meaning |
|---|---|
| unnamed `"` | default yank/delete; synced to the system clipboard |
| `0` | last yank |
| `a-z` | named registers |
| `+` / `*` | clipboard |
| `_` | black hole (never touches the clipboard) |
| `-` | small delete |

Numbered registers and expression registers are deferred.

### Search

- `/` and `?` open a prompt overlay.
- Incremental search while typing.
- `n` / `N` navigate matches.
- `*` / `#` search word under cursor.
- Smartcase by default.
- Highlight visible matches.
- `gn` selects next search match as a text object.
- `:` opens command palette instead of full Ex command-line mode.

---

## 3. Hybrid multicursor

### Default bindings

| Binding | Action |
|---|---|
| `Alt-click` | add cursor at mouse position |
| `Ctrl-n` | add next occurrence of current selection or word under cursor |
| `Ctrl-Alt-n` | skip current match and add next |
| `Ctrl-Alt-p` | remove last added cursor |
| `<Space>ma` | add all matches in buffer, or inside Visual selection if active |
| `<Space>mc` | clear multicursor, keep primary |
| `<Space>mn` | Vim-style alias for add-next |
| `gn` | select next search match as text object |
| `Esc` | exit Insert/Visual; if already Normal with multiple cursors, collapse to primary |

`Ctrl-d` and `Ctrl-u` remain half-page scroll.

### Semantics

- Insert mode types into all cursors.
- Normal-mode motions apply per cursor.
- Operators apply per cursor/selection.
- Visual mode can create selections per cursor.
- Yank from multiple cursors joins with newlines into the register.
- Undo/redo restores the whole multicursor edit group.
- Single-cursor dot repeat comes first; multicursor dot repeat can be refined later.

---

## 4. Smooth scrolling and cursor motion

Motion is time-based, configurable, and damage-driven when idle.

Viewport state:

```text
Viewport {
    scroll_y_current_px
    scroll_y_target_px
    scroll_x_current_px
    scroll_x_target_px
}
```

Cursor animation state:

```text
CursorAnim {
    x_current, y_current
    x_target, y_target
}
```

Behavior:

- Wheel, `Ctrl-d/u`, `Ctrl-f/b`, `gg`, `G`, `zz/zt/zb` set scroll targets.
- Cursor movement sets cursor target.
- Animation uses configurable ease-out or spring.
- Renderer draws fractional viewport offsets and extra lines above/below.
- Cursor rectangle glides between positions.
- Cursor blink is mode-aware.
- When animation settles, frame submission stops.
- Text input is never delayed by animation.
- Reduced motion disables or shortens animation.

Proposed defaults:

```text
motion.style = ease
motion.scroll_ms = 140
motion.cursor_ms = 90
motion.reduced_motion = auto
cursor.blink_ms = 530
picker.animation_ms = 120
```

---

## 5. Full Telescope-like picker

### Sources and default bindings

| Picker | Binding |
|---|---|
| Files | `<Space>ff` |
| Buffers | `<Space>fb` |
| Live grep | `<Space>fg` |
| Document symbols | `<Space>fs` |
| Project symbols | `<Space>fS` |
| Recent files | `<Space>fr` |
| Commands | `<Space>fc` |
| Keymaps | `<Space>fk` |
| Help / keymap discoverability | `<Space>fh` |

Aliases:

| Binding | Action |
|---|---|
| `Ctrl-p` | files picker |
| `:` | command palette |
| `/` | search prompt overlay |
| `?` | reverse search prompt overlay |

### Picker keys

| Key | Action |
|---|---|
| `Up/Down` or `Ctrl-n/Ctrl-p` | move selection |
| `Enter` | open result |
| `Esc` | close |
| `Tab` | toggle preview |
| `Ctrl-u` | clear query |
| Mouse click | select/open |

### UI

- Dimmed editor background.
- Floating centered panel.
- Prompt with query text.
- Result list with fuzzy-match highlighting.
- Preview pane for files, grep hits, symbols.
- Footer showing available keys.
- Optional icons if a Nerd Font is available.
- Rounded/bordered panel if a small SDF rounded-rect shader is added; otherwise clean bordered rectangles.
- Keyboard-first, mouse optional.

### Backend

- Native C fuzzy matcher, no required external dependency.
- Optional `fd`/`rg` acceleration if installed, with native fallback.
- Async scanning/search so picker typing does not block editor.
- Cancellable jobs with generation counters.
- Project symbol search uses Tree-sitter queries, not LSP.
- Recent files persisted locally.
- Buffer picker requires a small buffer manager.

---

## 6. Architecture changes

New modules:

| Module | Responsibility |
|---|---|
| `modal.c/h` | mode state, counts, operator-pending, register selection, key dispatch |
| `keymap.c/h` | trie/table-driven keymaps, leader sequences, which-key metadata |
| `motions.c/h` | Vim motions |
| `operators.c/h` | delete/change/yank/indent/case/put/comment |
| `textobj.c/h` | classic + Tree-sitter text objects |
| `selection_set.c/h` | multicursor storage, edit remapping, grouped undo |
| `animate.c/h` | scroll/cursor animation, reduced motion, frame scheduler |
| `picker.c/h` | picker UI state and rendering |
| `fuzzy.c/h` | fuzzy scoring and match highlighting |
| `picker_sources.c/h` | files/buffers/grep/symbols/recent/commands/keymaps |
| `async_jobs.c/h` | cancellable background picker jobs |
| `buffer_manager.c/h` | multiple open buffers, recent files |
| `config.c/h` | motion/keymap/picker settings |

Existing modules:

| Module | Changes |
|---|---|
| `document.c` | grouped edits, selection-set-aware operations |
| `frame.c` | fractional scroll, cursor shapes, multicursor rendering, overlay composition |
| `render_vk.c` | overlay/clip support, possibly rounded-rect SDF pipeline |
| `main.c` | modal input router, animation-aware event loop |

---

## 7. Phased implementation

### Phase 1 — Modal fast-editing core

Deliver:

- Normal/Insert/Visual char/line/block.
- Operator-pending.
- Counts.
- Core motions.
- Core operators.
- Classic text objects.
- Registers.
- Dot repeat.
- Search prompt + `n/N/*/#`.
- Mode-aware statusline and cursor shape.

Acceptance:

- Table-driven key sequence tests.
- Visual block tests.
- Operator + motion + count tests.
- Register tests.
- Dot repeat tests.
- No LSP/completion paths.

### Phase 2 — Smooth motion

Deliver:

- Pixel scroll targets.
- Cursor glide.
- Configurable animation style/duration.
- Reduced motion.
- Damage-driven animation scheduler.
- Mode-aware cursor blink/shape.

Acceptance:

- Deterministic animation tests with fixed time steps.
- No continuous frame submission when idle.
- Headless screenshots at intermediate animation states.
- Input latency benchmark before/after.

### Phase 3 — Hybrid multicursor

Deliver:

- Selection set.
- Add next occurrence.
- Alt-click add cursor.
- Skip/remove/clear.
- Shared Insert typing.
- Operators across cursors.
- Grouped undo/redo.
- Visual/search-driven multicursor workflows.

Acceptance:

- Randomized multicursor edit/remap fuzz tests.
- Undo/redo restores exact prior state.
- 100-cursor frame-build benchmark.
- No selection drift after mixed insert/delete.

### Phase 4 — Full picker

Deliver:

- Picker overlay UI.
- Fuzzy matcher.
- Files, buffers, commands.
- Live grep.
- Document/project symbols.
- Recent files.
- Keymap/help picker.
- Preview pane.
- Async/cancellable jobs.
- Buffer manager.

Acceptance:

- Fuzzy scoring golden tests.
- Async cancellation tests.
- Headless picker screenshots.
- Picker query latency benchmark.
- Project symbol search does not block UI.

### Phase 5 — Tree-sitter fast-editing extras

Deliver:

- Tree-sitter text objects.
- Tree-sitter motions.
- `gc` comment operator.
- Optional surround operator: `ys`, `ds`, `cs`.

Acceptance:

- Text object tests per language.
- Comment operator tests.
- Query fallback behavior when grammar lacks a capture.

---

## 8. Deliberately deferred

- Full Ex command-line parser.
- Vimscript/Neovim runtime.
- Macros.
- Splits/tabs/windows.
- Terminal buffer.
- LSP/completion.
- Full marks/jump-list machinery.
- Expression registers.
- Complex formatting engines.

---

## 9. Performance and correctness gates

All performance claims continue to follow the project evidence rule:

- mylib timer.
- Warmup.
- At least 21 paired randomized reps where comparing paths.
- Min latency floor, median throughput, CV.
- Explicit workload and environment.
- Before/after measurements for optimizations.

Specific gates:

| Area | Gate |
|---|---|
| Modal input | key sequence -> expected mode/buffer/selection/register |
| Multicursor | randomized edit/remap fuzz, grouped undo/redo exactness |
| Smooth motion | deterministic fixed-time animation, idle stops frame submission |
| Picker | fuzzy golden tests, async cancellation, headless screenshots |
| Frame build | 100-cursor viewport benchmark |
| Picker latency | query-to-result latency on large project fixture |
| Vulkan | headless validation-layer screenshots for overlays and animation states |
