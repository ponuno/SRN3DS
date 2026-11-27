# Shader pack `sky_water`

Owner: `sm3-agent-sky-water`

sky dome/atmosphere and water surfaces

* census families: `sky`, `water`
* chunks: **82** / unique hashes: **45**
* coverage: 14 translated, 0 draw-enabled, 31 refused
* first step: map water refraction/reflection samplers to TEV stages

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## Phase 2 (port-lite)

`plans.json` holds **14 translated hashes / 24 chunks** (3 exact, 11 approx) and
`refusals.json` names the remaining **31 hashes / 58 chunks**.  Nothing is
draw-enabled or runtime-enabled.  Recipes are 1-2 TEV stages:

* sky: texture x vertex colour + scalar bias; two-texture `INTERPOLATE` lerp;
* water: one scene/cube sample x prebaked fresnel + one constant highlight;
  fog: texture/fog `INTERPOLATE` at 0.9.

Every approximate row says what it drops.  See `REPORT.md` for the class table,
visual cost, constant-slot policy and shared requests, and
`tests/test_sky_water_pack.py` for the plan/IR and negative tests.

