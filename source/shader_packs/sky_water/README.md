# Shader pack `sky_water`

Owner: `sm3-agent-sky-water`

sky dome/atmosphere and water surfaces

* census families: `sky`, `water`
* chunks: **82** / unique hashes: **45**
* coverage: **45/45 planned** — 3 exact, 42 approximate, 45 draw-enabled
  catalog rows, 0 refusals
* runtime: no shared runtime-table slot is claimed here; all 45 rows are
  `runtime_candidate: 1`, `runtime: 0`
* first step: select the lite water/sky policy at draw time

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## Phase 3 (port-lite complete)

`plans.json` holds all **45 hashes / 82 chunks**.  Three sky rows are exact
(`contract: full`); the other 42 are explicit `quality: approx` rows with a
written `approximation` and a `def_policy` that records the original literals
and states `clamped: 0`.  No row claims a rendered frame or a `DevCaps` change.

| recipe | hashes | stages | quality | what it keeps / drops |
|---|---:|---:|---|---|
| `sky_tex_vcolor_bias` | `91ab7cd7` | 1 | exact | texture x vertex colour + scalar bias |
| `sky_tex_vcolor_bias_alphascale` | `250481a5` | 2 | exact | same + separate alpha scale |
| `sky_two_tex_lerp` | `37902aa7` | 2 | exact | two-texture main/cloud lerp + vertex tint |
| `sky_tex_vcolor_fade_lite` | `fa8e6439` | 1 | approx | second-UV fade dropped |
| `sky_tex_vcolor_x1p5_lite` | `013ad862` | 1 | approx | 1.5 gain dropped (TEV scale is 1/2/4/0.5) |
| `water_surface_fresnel_lite` | 17 | 3 | approx | one scene/base texture + prebaked fresnel; normal map, light loop, reflection pair dropped |
| `water_surface_cube_lite` | 2 | 3 | approx | one cube reflection sample + prebaked fresnel |
| `water_fog_lerp_lite` | 2 | 1 | approx | one fog colour instead of the 3-colour `cmp` gradient |
| `water_surface_normal_lite` | 10 | 3 | approx | 4-tap normal blur folded to one base texture |
| `water_surface_dither_lite` | 3 | 3 | approx | dither/`texkill`/dependent taps dropped |
| `water_refraction_uv_flat_lite` | 4 | 1 | approx | flat centre UV instead of `rcp`/`min`/`cmp` distortion |
| `sky_star_dither_lite` | 1 | 1 | approx | dither dropped, texture x vertex tint kept |
| `sky_cloud_dither_lite` | 1 | 2 | approx | fixed half two-texture lerp + tint, dither dropped |

### Water lite policy

Every water surface uses one texture and a prebaked fresnel tint (vertex colour
where the IR declares one, otherwise material constant `c0`), plus one constant
sun highlight, and a final REPLACE stage forces alpha to 1.  **Reflection,
refraction render targets, per-pixel normal maps and the 2-3 light loop are not
reconstructed**; the loss is named per hash in `approximation`.  The
refraction-UV shaders, whose colour target is not a colour, are replaced with a
flat `(0.5, 0.5)` no-distortion UV.

### Sky plans

The exact sky pair/lerp rows and the two lite fades remain; the two dither sky
shaders now have plans that keep the star/cloud texture and vertex tint and
drop only the ordered dither.  Gradient, sun, stars and cloud hashes therefore
all have a descriptor.

### `def` literals outside [0,1]

No literal is clamped.  Every row carries `def_policy` with
`original_literals`, `outside_unit_range`, `treatment` and `clamped: 0`.
Sign-expansion values (`-8192`, `2`, `-1`, `-2`) fold into TEV selectors; other
out-of-range scalars (`-0.5`, `0.04`, `1.8`, `0.1`, `255`, `1/255`, ...) are
dropped together with the expression they came from.  The three exact rows now
build `c0.yyyy` / `c0.xxxx` in `constant_ops` (`c7`/`c6`) instead of relying on
a split runtime binding.

### Stage/texture budget

No plan needs more than 3 stages and 2 textures, so the pack does not use the
`passes[]` escape hatch: the >6-stage / >2-texture retail programs are collapsed
to the one-texture lite recipe, and that loss is stated per row.

## Checks

```powershell
python tools\shader_packs.py check
python tools\shader_packs.py coverage --out <tmp>\coverage.json
python tools\shaders\plan_verify.py
python tests\test_sky_water_pack.py
```
