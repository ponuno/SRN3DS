# Shader pack `terrain`

Owner: `sm3-agent-terrain`

landscape multitexture, LOD and world-map overlay

* census families: `terrain/landscape`, `grass/trees`
* chunks: **327** / unique hashes: **247**
* coverage: 0 translated, 0 draw-enabled, 247 refused
* first step: reduce 6-way multitexture to the 6-stage TEV budget

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
