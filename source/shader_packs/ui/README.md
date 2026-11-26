# Shader pack `ui`

Owner: `sm3-agent-ui`

menu/UI pixel and vertex shaders: A8 atlas copies, text tint, simple colour math

* census families: `ui/interface`
* chunks: **314** / unique hashes: **113**
* coverage: 0 translated, 0 draw-enabled, 113 refused
* first step: re-plug the certified A8 text plan; keep draw blocked

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
