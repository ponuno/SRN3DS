# Shader pack `lighting`

Owner: `sm3-agent-lighting`

lit material shaders, deferred/shadow passes and particle/effect shaders

* census families: `lighting/deferred/shadows`, `particles/effects`
* chunks: **7459** / unique hashes: **4991**
* coverage: 2 translated, 0 draw-enabled, 4989 refused
* first step: split the normal/basis dp4 prebake out of the PS plan

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
