# Shader pack `sky_water`

Owner: `sm3-agent-sky-water`

sky dome/atmosphere and water surfaces

* census families: `sky`, `water`
* chunks: **82** / unique hashes: **45**
* coverage: 45 translated, 0 draw-enabled, 0 refused
* first step: map water refraction/reflection samplers to TEV stages

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
