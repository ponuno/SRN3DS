# Shader pack `blit_post`

Owner: `sm3-agent-blit-post`

copy/blit and image-space post effects (bloom, adaptation, colour grading)

* census families: `blit/copy`, `post-process/imagespace`
* chunks: **131** / unique hashes: **69**
* coverage: 1 translated, 1 draw-enabled, 67 refused
* first step: certify one-stage copy plans, then full-screen passes

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
