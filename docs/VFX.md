# Cursor VFX

Quill ports all six Neovide cursor VFX modes and adds eleven original modes.
Effects are off by default (`cursor_vfx_mode = none`), matching Neovide.
Enable one or combine several with commas:

```conf
cursor_vfx_mode = railgun,ripple
```

You can also cycle modes in Normal mode with `Space m v`, or pick
`Cursor VFX: <mode>` from the command palette (`:`). Headless screenshots
accept `--vfx railgun,ripple`.

## Modes

| Mode | Kind | Shape | Notes |
|---|---|---|---|
| `railgun` | trail | stroked oval | Neovide sine/cosine phase + curl |
| `torpedo` | trail | stroked oval | Neovide backward-biased random burst |
| `pixiedust` | trail | filled rect | Neovide upward-biased sparkle |
| `sonicboom` | highlight | filled oval | expanding ring, ease-in-quad fade |
| `ripple` | highlight | stroked oval | expanding ring outline |
| `wireframe` | highlight | stroked rect | expanding rectangle outline |
| `comet` | trail | filled oval | Quill: cool blue head, drag behind motion |
| `ember` | trail | filled rect | Quill: warm sparks with buoyancy + sway |
| `aurora` | trail | filled oval | Quill: tall green waves, sinusoidal drift |
| `glitch` | trail | filled rect | Quill: RGB-split jitter, short lifetime |
| `pulse` | highlight | filled rect | Quill: soft box flash around the cursor |
| `snow` | trail | filled oval | Quill: pale flakes drifting down with a gentle sway |
| `confetti` | trail | filled rect | Quill: multicolor tumbling bits, gravity + sway |
| `spark` | trail | filled oval | Quill: bright radial firework burst with gravity |
| `smoke` | trail | filled oval | Quill: gray puffs rising and expanding, low alpha |
| `shockwave` | highlight | stroked oval | Quill: fast double expanding ring |
| `halo` | highlight | stroked oval | Quill: soft thick ring, slow expand + gentle fade |

## Tunables

Defaults follow Neovide:

```conf
cursor_vfx_opacity = 200
cursor_vfx_particle_lifetime = 0.5
cursor_vfx_particle_highlight_lifetime = 0.2
cursor_vfx_particle_density = 0.7
cursor_vfx_particle_speed = 10.0
cursor_vfx_particle_phase = 1.5
cursor_vfx_particle_curl = 1.0
```

`reduced_motion = true` disables VFX along with other animation.

## Implementation

- `src/vfx.c` keeps one homogeneous SoA particle pool per trail mode and a
  small highlight table. Dead particles are swap-removed; `count` is
  existence. Update/render loops branch on the pool's mode once, not per
  particle.
- A seeded PCG-style RNG makes particle motion deterministic for tests and
  reproducible screenshots.
- `shaders/vfx.{vert,frag}` draw SDF ovals/rects (filled or stroked) with
  screen-space anti-aliasing. The Vulkan renderer expands `VfxQuad` draw-list
  entries into a dedicated vertex buffer/pipeline.
- VFX follow the primary cursor's animated destination. Immediate jumps
  (`cursor_ms = 0`, reduced motion, far scroll snaps) suppress trail spawn but
  still fire highlight modes, matching Neovide's `immediate_movement` rule.

## Examples

Headless demo renders (cursor swept across `assets/sample.c`):

- `assets/vfx_confetti.png` — `confetti,spark`
- `assets/vfx_snow.png` — `snow,smoke`
- `assets/vfx_demo.png` — `railgun,ripple`

Reproduce with `quill --headless assets/sample.c --screenshot out.png --vfx "confetti,spark" --vfx-demo`.
