# Shader pack `blit_post`

Owner: `sm3-agent-blit-post`

copy/blit and image-space post effects (bloom, adaptation, colour grading)

* census families: `blit/copy`, `post-process/imagespace`
* chunks: **131** / unique hashes: **69**
* catalog: **34** descriptors - **27 translated**, **7 draw-enabled**, **35 refused**
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

| class | hashes | planned | policy |
|---|---:|---:|---|
| copy / tint / constant clear | 9 | 8 | exact one-stage TEV, `draw_enabled: 1` |
| blend / composite | 7 | 3 | 50/50 `INTERPOLATE` approximation |
| blur / bloom / DOF / sharpen | 41 | 12 | one centred tap instead of N |
| tonemap / colour-grade | 3 | 2 | constant exposure or one LUT slice |
| PICA VS contracts | 9 | 8 | `pica_vs`, codegen pending, draw 0 |
| **total** | **69** | **34** | 1 copy + 1 VS already in the runtime table |

`draw_enabled: 1` is used only for a **full material contract** whose declared
simplified spec is symbolically equal to the IR: one texture (or zero), no
varyings beyond UV, literal constants only, one TEV stage, alpha fixup not
needed. Six new hashes meet that bar; `menu.composite.ps`/`game.vs.0ccc7542`
already own the two existing runtime slots.

Every other plan is `quality: "approx"`, carries an `approximation` string and
stays `draw_enabled: 0`, `runtime: 0`. There are no silent clamps.

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

A 16x16x16 LUT needs 256x16 2D packing (one sample) and would fit three
texture units, but `8fb14655` binds **16 separate 2D slices** and blends them
with an `lrp` chain. PICA has no indirect stage, so the plan samples slice 0
and records the loss; the packed-strip route is the follow-up.

## Named refusals (`refusals.json`)

35 hashes stay refused. The classes are the census classes; the dominant ones
are tap-count/dataflow and `def` literals outside RGBA8. `contract_only` is 0
because all 9 fullscreen-quad VS hashes now have a descriptor, but VS codegen
is still pending, so none is draw-enabled.

## Tests

`test_blit_post_plans.py` (15 tests, host-only) checks the manifest partition,
schema/semantics, the draw contract (one stage, <=1 texture, no varyings),
named approximations, VS limits, plan-vs-IR identity for copy/constant fills,
real tap reduction for the blur approximations, and four negative cases
(draw-enabled approximation, missing approximation, premature `runtime: 1`,
unknown hash).
