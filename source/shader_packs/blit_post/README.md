# Shader pack `blit_post`

Owner: `sm3-agent-blit-post`

copy/blit and image-space post effects (bloom, adaptation, colour grading)

* census families: `blit/copy`, `post-process/imagespace`
* chunks: **131** / unique hashes: **69**
* catalog: **67** descriptors - **62 translated**, **7 draw-enabled**, **0 refused**
  (the other 2 hashes are the shared runtime-certified `menu.composite.ps` and
  `game.vs.0ccc7542` entries, so all **69/69** hashes have a plan)
* runtime table: **0** entries added (`"runtime": 0` everywhere; promotion is a
  separate measured step)
* first step: certify one-stage copy plans, then full-screen passes

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class. Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours. Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## What is translated

`plans.json` is the pack catalog, generated from the host SM3 corpus by
`pack_plans.py` (no retail bytes are stored):

```powershell
python source\shader_packs\blit_post\pack_plans.py
python -m unittest discover -s source\shader_packs\blit_post -p "test_*.py"
```

| census class | hashes | planned | policy |
|---|---:|---:|---|
| `tev_direct` | 6 | 5 (+1 runtime) | exact one-stage TEV, `draw_enabled: 1` |
| `pica_vs` | 9 | 8 (+1 runtime) | `pica_vs`, codegen pending, draw 0 |
| `def_literal_out_of_range` | 19 | 19 | `constant_ops` derived c7; 4 also two-pass |
| `dataflow` | 18 | 18 | two accumulating 0.5 passes, one centre tap |
| `unsupported_opcode` | 13 | 13 | DOT3 luma or 50/50 `INTERPOLATE` |
| `non_uniform_swizzle` | 2 | 2 | one exact fill, one partial-write approximation |
| `tev_prebake` | 1 | 1 | recorded precondition + DOT3 luma |
| `input_not_a_colour_varying` | 1 | 1 | primary-colour rebind |
| **total** | **69** | **67 (+2 runtime)** | 1 copy + 1 VS already in the runtime table |

`draw_enabled: 1` is used only for a **full material contract** whose declared
simplified spec is symbolically equal to the IR: one texture (or zero), no
varyings beyond UV, literal constants only, one TEV stage, alpha fixup not
needed. Six new hashes meet that bar; `menu.composite.ps`/`game.vs.0ccc7542`
already own the two existing runtime slots.

Every other plan is `quality: "approx"`, carries an `approximation` string and
stays `draw_enabled: 0`, `runtime: 0`. There are no silent clamps.

## Escape hatches used for the former refusals

The 35 former refusals now have descriptors; none is draw-enabled or promoted.

| class | hashes | plan |
|---|---:|---|
| `dataflow` | 14 | two-pass accumulator: one centred tap at 0.5 in each pass, ADD blending keeps the sum at 1.0; per-tap offsets are not TEV-addressable, so N-1 taps are dropped |
| `unsupported_opcode` | 11 | 7 luma (`cmp`/`lerp` tone curve -> fixed DOT3 luma); 4 two-texture 50/50 `INTERPOLATE` (`cmp`/`lrp`/`texldd` select chain flattened) |
| `def_literal_out_of_range` | 8 | `constant_ops` derives c7 from c0 (pack, optional negate) and the TEV stage reads c7 instead of an out-of-range `def`; 4 of these also use the two-pass accumulator |
| `tev_prebake` | 1 | `preconditions: ["tev_prebake_guest_state", ...]` plus a DOT3-luma simplified plan; guest prebaked TEV selectors are not read back |
| `input_not_a_colour_varying` | 1 | texcoord1 scalar rebound as the TEV primary colour and multiplied by literal `(0,1,0,0)`; matches only 0/1 masks |

The derived register uses the schema's `dst_reg` (c0..c7) and the plan's
`constants` list names both the source and the derived register, so
`plan_verify.py` can prove the source exists.

## Approximations and numbers

| simplification | hashes | declared loss |
|---|---:|---|
| 49-tap loop (7x7) -> 1 tap | 1 | soft box blur becomes a copy |
| 16-tap separable blur -> 1 tap | 2 | edges lose the wide kernel |
| 3-9 tap blur/sharpen/Sobel -> 1 tap | 6 | sharpening/derivative terms dropped |
| 5-tap radial/bloom -> 1 tap | 2 | no radial streak, no bloom halo |
| 14-tap luma blur -> 1 luma tap | 1 | DOT3 luma kept, 13 taps dropped |
| 3 textures + rcp/cmp adaptation -> 2-tex 50/50 lerp | 2 | eye adaptation fixed at 0.5 |
| 16-slice LUT grade -> slice 0 | 1 | 15 co-bound slices dropped |
| `dp3`/`cmp` grade -> exposure 0.8 | 1 | no hue/saturation curve |
| `oC0.xy`-only constant fill | 1 | undefined alpha declared as 0 |
| def outside RGBA8 -> derived c7 | 8 | literal becomes a uniform/derived value |
| N-tap dataflow -> two 0.5 passes | 18 | one centre tap, N-1 taps dropped |
| cmp/lrp/texldd select -> fixed 0.5 | 4 | select threshold is constant |
| cmp tone curve -> DOT3 luma | 7 | bloom/adaptation weighting lost |
| dp4 prebake -> DOT3 luma | 1 | 4th channel folded into alpha |
| texcoord1 -> primary colour | 1 | correct only for 0/1 masks |

A 16x16x16 LUT needs 256x16 2D packing (one sample) and would fit three
texture units, but `8fb14655` binds **16 separate 2D slices** and blends them
with an `lrp` chain. PICA has no indirect stage, so the plan samples slice 0
and records the loss; the packed-strip route is the follow-up.

## Physical limit

PICA200 has six TEV stages per pass and three texture units; a plan may replay
2..4 passes. The remaining `blit_post` refusals were not a DevCaps gap: they
were per-tap UV offsets that only the guest VS/constants can supply, and
`texldd`/`cmp` selects with no TEV equivalent. The plan records that boundary
instead of claiming a wider capability.

## Named refusals (`refusals.json`)

`refusals.json` now has `refused: 0` and no rows. `contract_only` stays 0
because all 9 fullscreen-quad VS hashes have a descriptor; VS codegen is still
pending, so none is draw-enabled.

## Tests

`test_blit_post_plans.py` (16 tests, host-only) checks the manifest partition,
schema/semantics, the draw contract (one stage, <=1 texture, no varyings),
named approximations, VS limits, plan-vs-IR identity for copy/constant fills,
real tap reduction for the blur approximations, the `constant_ops`/`passes`
coverage of every refusal class, and four negative cases (draw-enabled
approximation, missing approximation, premature `runtime: 1`, unknown hash).
