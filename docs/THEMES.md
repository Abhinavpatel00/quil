# Custom themes

Quill's built-in palette is Noir, a grayscale/red adaptation of
[vim-256noir](https://github.com/andreasvc/vim-256noir). It is also a loadable
**theme file**: a flat `key = color` palette that replaces the built-in. The
active palette lives in one global (`QUILL_THEME`); loading a theme happens
**once at startup** (or on demand from the palette), so there is **no per-frame
cost** — the renderer reads the same global it always did.

Ready-made palettes ship in `themes/`:

| File | Look |
|---|---|
| `themes/noir.theme` | 256noir-inspired grayscale/red (the built-in default) |
| `themes/naysayer.theme` | Naysayer — dark green-blue, warm tan text, green comments |
| `themes/catppuccin.theme` | Catppuccin Mocha |
| `themes/dracula.theme` | Dracula |
| `themes/gruvbox-dark.theme` | Gruvbox Dark |
| `themes/nord.theme` | Nord |
| `themes/tokyo-night.theme` | Tokyo Night |
| `themes/one-dark.theme` | Atom One Dark |
| `themes/solarized-dark.theme` | Solarized Dark |
| `themes/light.theme` | Rosé Pine Dawn (light) |
| `themes/solarized-light.theme` | Solarized Light |

Naysayer's palette is adapted from
[RostislavArts/naysayer.nvim](https://github.com/RostislavArts/naysayer.nvim),
including its `#062625` background, `#D0B892` text, `#53D549` comments, and
`#3AD0B5` strings. The repository's `quill.conf` selects it by default; use
`Theme: Default` from the command palette or `theme = none` for built-in Noir.

`theme.conf.example` is an annotated template listing every key.

## File format

Plain text, one `key = value` per line. `#` starts a comment; blank lines and
unknown keys (e.g. `name`) are ignored. Only the keys you set are overridden —
everything else keeps the built-in default, so a theme can be a partial override.

Colors accept `#RRGGBB`, `#RRGGBBAA`, `0x…`, or bare hex. A 6-digit value is fully
opaque; append two hex digits for alpha. A 3-digit `#RGB` form is expanded
(`#F00` → `#FF0000`). An invalid color leaves that slot at its default.

```
name = My Theme
bg      = #1E1E2E
fg      = #CDD6F4
cursor  = #F5E0DC
keyword = #CBA6F7
string  = #A6E3A1
```

### Keys

Chrome: `bg`, `fg`, `gutter_bg`, `gutter_fg`, `line_hl`, `selection`, `search`,
`cursor`, `status_bg`, `status_fg`, `accent`.

Syntax (Tree-sitter capture classes): `default`, `keyword`, `function`, `string`,
`comment`, `number`, `type`, `variable`, `operator`, `punctuation`, `property`,
`constant`, `attribute`, `label`, `escape`, `error`.

`line_hl`, `selection` and `search` are where alpha matters most (they are
translucent highlights; `search` backs search matches). `status_bg` also backs
the picker panel and search prompt; `accent` fills the status-bar mode chip and
the picker accent; `cursor` is the base color for the cursor, trail,
and VFX.

## Ways to configure

Config file (`quill.conf`, see `quill.conf.example`):
```
theme = themes/naysayer.theme
```

CLI (both GUI and `--headless`):
```
quill --theme themes/nord.theme
./quill --headless assets/sample.c --screenshot out.png --theme themes/light.theme
```

Command palette (`:`): `Theme: Reload` (re-reads the configured file) and
`Theme: Default` (back to built-in Noir).

## Pairing with background images

Themes and the background image / chrome opacity features compose: a light or
low-contrast palette plus `editor_opacity < 1` lets a wallpaper show through
while text stays opaque. See `docs/APPEARANCE.md`.

## Implementation notes

- `src/theme.c` defines `QUILL_THEME` with a static Noir initializer (so
  tests that call `frame_build` directly never see an uninitialized palette)
  and a `THEME_DEFAULT` reset target. `theme_load_file()` resets to the
  default, then applies the keys present in the file. `theme_reset()`
  restores the built-in.
- `src/frame.c`, `src/picker.c`, `src/main.c`, `src/render_vk.c` all read
  `QUILL_THEME`; the previously hardcoded picker/search colors now route through
  it too, so a theme applies everywhere.
- The Vulkan clear color is `QUILL_THEME.bg`, so the theme covers letterbox bars
  and the area behind translucent chrome.

## Verification

- `tests/test_theme.c`: built-in default, partial override, every color spelling,
  unknown-key and invalid-color handling, reset, NULL/empty/missing-file paths.
- `make check`: 16/16 pass + 3 llvmpipe screenshots.
- ASan + UBSan clean over the loader and config paths.
- Default render is pixel-identical to the pre-theme commit outside the
  time-dependent cursor blink (0 differing pixels elsewhere).
