# Shader pack `skinning`

Owner: `sm3-agent-skinning`

character/normal-map/tint-mask shaders and skinned VS

* census families: `skinning/characters`
* chunks: **2162** / unique hashes: **1194**
* coverage: 0 translated, 0 draw-enabled, 1194 refused
* first step: land the VS skinning contract before any PS work

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
