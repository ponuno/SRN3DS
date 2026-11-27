# Shader pack `terrain`

Owner: `sm3-agent-terrain`

landscape multitexture, LOD and world-map overlay

* census families: `terrain/landscape`, `grass/trees`
* chunks: **327** / unique hashes: **247** (all PS; measured rows are all
  `terrain/landscape`, no shipped `grass/trees` rows)
* exact-subset coverage: 0 translated, 0 draw-enabled, 247 refused
* port-lite coverage: **138 translated / 190 chunks, 0 draw-enabled, 109 refused**
* generated IR table: [`terrain_classes.json`](terrain_classes.json)
* named refusals: [`refusals.json`](refusals.json) (109 remaining hashes)
* plan catalogue: [`plans.json`](plans.json) (138 explicit approximations; no runtime entries)
* approximation report: [`approximations.json`](approximations.json)
* reproducible classifier: [`classify.py`](classify.py)
* approximation generator: [`approximate.py`](approximate.py)
* pack-local tests: [`test_terrain_classes.py`](test_terrain_classes.py),
  [`test_approximate_plans.py`](test_approximate_plans.py)

## Phase-2 result: the requested simple subset is empty

The canonical MojoShader IR was classified hash by hash.  A hash is in the
requested simple subset only if it has 1–3 sampled textures, a static TEV stage
lower bound of at most 6, no control flow, only direct opcodes, and no `def`
component outside `[0,1]` other than `0`, `1` or `-1`.

**Exact-subset result: 0 hashes / 0 chunks.**  No original-equivalent plan was
emitted.  The port-lite follow-up explicitly approximates 138 hashes instead of
refusing the whole family; see below.  `def_literal_out_of_range` was only the
census's first rejection, and fixing literal range alone unlocks none of the
original shaders.

| Measured property | Hashes | Chunks |
|---|---:|---:|
| 3 sampled textures | 19 | 27 |
| 4 sampled textures | 107 | 139 |
| 5 sampled textures | 1 | 1 |
| 12 sampled textures | 8 | 12 |
| 13 sampled textures | 52 | 68 |
| 14 sampled textures | 8 | 12 |
| 15 sampled textures | 52 | 68 |
| Static stage lower bound `25+` | 247 | 327 |
| Control flow (`rep`/`ifc`) | 54 | 60 |
| `dp4` normal/basis transform | 247 | 327 |
| At least one hard out-of-range `def` value | 247 | 327 |
| Named `terrain.needs_multipass` (`>3` textures) | 228 | 300 |
| Named `terrain.stage_budget` (`<=3` textures) | 19 | 27 |

`stage_demand_min = executable_instructions - texld_count`.  It is a lower
bound, not a claim that this many TEV stages were emitted.  Every hash already
exceeds the six-stage budget before scheduling, constant materialisation or
alpha fixup.

## Real form classes

| Class | Hashes | Chunks | Textures | Stage lower bound | `def` shape | Flow | Primary refusal |
|---|---:|---:|---|---|---|---:|---|
| `terrain.height_layers` | 120 | 160 | 3–4 | 25–116 (median 81) | usually `(0.08,-0.04,2,-1)`; loop variants add integer vectors | 54 | 104 multi-pass, 16 stage-budget |
| `terrain.multitexture_12_13` | 60 | 80 | 12–13 | 36–127 (median 94) | `(2,-1,1,0)` | 0 | multi-pass |
| `terrain.multitexture_14_15_lod` | 60 | 80 | 14–15 | 44–135 (median 102) | `(2,-1,3,0)` + `(0.833333,0.37,0,0)` | 0 | multi-pass |
| `terrain.lod_noise_overlay` | 1 | 1 | 3 | 28 | three vectors, including `-2`, `3`, `-0.5` | 0 | stage-budget |
| `terrain.world_map_overlay` | 5 | 5 | 3–4 | 45–84 (median 62) | 2–7 vectors; varied | 0 | 3 multi-pass, 2 stage-budget |
| `terrain.lod_world_map_overlay` | 1 | 1 | 5 | 89 | eight vectors, including `20` | 0 | multi-pass |

The three overlay classes are kept separate because their pipeline contract is
source-alpha blending.  Alpha blending is D3D9 render state, not a PS opcode;
the generated table marks it as `required_by_overlay_contract` rather than
pretending the IR proves it.  `HeightSampler` classes are parallax/height work,
not automatically alpha-blended overlays.

`terrain_classes.json` carries every SHA-256, chunk count, package/meta, opcode
histogram, sampled units, exact `def` vectors, CTAB vocabulary, stage lower
bound, blockers and named refusal.  `refusals.json` is the compact per-hash
named-refusal catalogue for the 109 still-unplanned hashes.  Neither stores
retail bytecode.

## Port-lite adaptation: 138 translated hashes / 190 chunks

Every entry in `plans.json` has `quality: "approx"`, an explicit `approximation`
object, a declared simplified specification and a symbolic verification against
that specification.  No entry claims equivalence to the retail shader.  All 138
plans carry a measured static material contract; 10 one-texture plans are
runtime candidates, but every entry stays `draw_enabled: 0` / `runtime: 0`.

| Policy | Hashes | Chunks | TEV stages | Textures | Declared simplification / visual effect |
|---|---:|---:|---:|---:|---|
| `unlit_diffuse_baseline` | 11 | 23 | 1 | 1 | Keep `DiffuseSampler`; drop normal lighting, height/parallax, shadows and every extra layer. Flat unlit albedo. |
| `unlit_diffuse_overlay_pass` | 7 | 7 | 1 | 1 | Keep base diffuse; drop world-map normal, LOD-noise and lighting detail. Source-alpha overlay state is preserved externally. |
| `dominant_three_layers` | 60 | 80 | 3 | 3 | Keep fixed texture units 0..2 of the 12/13-layer multitexture shaders with a quantized equal-ish mix; drop the other 9–10 layers, all normal/height samplers and lighting. |
| `lod_ladder_default_near3` | 60 | 80 | 3 | 3 | Same three-unit mix for the 14/15-layer LOD shaders, declared as the **near** tier of a near/mid/far = 3/2/1 ladder. One plan per hash; mid/far are runtime distance tiers, not separate committed plans. |

The three-layer policies use two PICA interpolations with literals `127/255` and
`170/255`; effective weights are approximately `0.332026 / 0.334641 / 0.333333`.
Maximum deviation from equal thirds is **0.001307**.  The corpus has no per-cell
weight histogram, so the **error against the original weights is reported as
`not_measured`**, not invented.  Layer selection is fixed to units 0..2; no layer
is claimed to be dominant by weight.  The original-error bound remains a
conservative **1.0 per channel** because the dropped layers, normal maps and
lighting are unmeasured.

Each plan also records its original `def` policy.  `0`/`1`/`-1` are named as
foldable selectors; `0.08`, `-0.04`, `2`, `3`, `20` and the other out-of-range
literals are listed as `uniform_or_approximated` and are dropped together with
the original expression.  Nothing is silently clamped.

Ten one-texture plans have a complete static material contract and are marked
`runtime_candidate: 1` (five were already translated before this expansion).
They remain `draw_enabled: 0` / `runtime: 0`: the runtime cannot yet select the
terrain approximation policy and no live texture/format/sampler binding was
observed.  The three-texture plans also need the generic backend's multi-texture
gate; overlay plans need the external source-alpha contract wired explicitly.

Atlas/prebake was not selected: the corpus does not contain a baked atlas or a
measured common-UV/material table.  Multi-pass was not selected: every committed
approximation fits PICA's 3-unit / 6-stage envelope in one pass.

## `def` values and the schema request

The most common out-of-range values by hash count are:

| Value | Hashes containing it |
|---:|---:|
| `2` | 246 |
| `-1` | 244 |
| `-0.04` | 120 |
| `3` | 81 |
| `-2` | 49 |
| `-3` | 40 |
| `-4` | 26 |
| `4` | 20 |

`0`/`1` can be literal RGBA8 values when selected; `-1` can sometimes be
lowered through a proved subtract/one-minus formulation.  The remaining values
cannot be clamped or silently packed.  The current plan contract cannot carry
them honestly:

1. `stage_data` has one `const_reg` / `const_is_literal` / `const_rgba8` slot.
   It cannot name two constants for one expression, and `const_rgba8` is LDR
   only.
2. Top-level `constants` is only a list of D3D register indices.  It has no
   float4 value bank, binding source or per-stage slot map.
3. There is no multi-pass representation.  A TEV entry has one `stage_data`
   array and one texture/input/constant set, with no pass blend/depth/target
   state.  `D9ShaderPlan` likewise contains one `D9TevProgram`.

**Exact request to the shared-schema owner (no core files changed here):**

* Extend `d9-shader-plan-v1` with either a TEV `passes[]` array or a plan-group
  form.  Each pass must carry its own `stage_data` (max 6), textures (max 3),
  constants, inputs, preconditions, blend mode, depth test/write and target.
  Add the matching runtime representation/dispatch before any multi-pass entry
  is marked draw-enabled.
* Add a named float4 constant bank with explicit values/bindings and a
  per-stage constant-slot map.  Keep RGBA8 literals for values that are proved
  in range; represent `0`/`1`/`-1` through source selectors or an explicitly
  verified rewrite where possible.  Anything still outside the LDR contract is
  a named refusal, not a clamp.
* Add a structured material contract for texture unit/format/sampler binding,
  constant values, varying routing and blend state.  String preconditions alone
  are not enough for `draw_enabled=1`.

## Why no original-equivalent plan was accepted

All 247 hashes contain the retail `dp4` normal/tangent-basis transform.  The
smallest three-texture candidates still have 25–45 static non-texture
instructions and need normal prebake plus a one-accumulator DAG scheduler.
Many also contain `nrm`, `rsq`, `rcp`, `pow`, `cmp`, `dp2add` or loops.  The
12–15-texture classes additionally require a real multi-pass contract.

The 138 port-lite entries above therefore verify only their declared simplified
specifications, never the original `oC0`.  The remaining 109 hashes stay named
`terrain.needs_multipass` (103) or `terrain.stage_budget` (6).

## Reproduce and test

```powershell
python source\shader_packs\terrain\classify.py --corpus E:\host_sm3
python source\shader_packs\terrain\approximate.py --corpus E:\host_sm3 --data F:\Skyrim\Data
python source\shader_packs\terrain\test_terrain_classes.py
python source\shader_packs\terrain\test_approximate_plans.py
python tools\shader_packs.py check
python tools\shaders\plan_verify.py --plans source\shader_packs\terrain\plans.json
```

The pack-local IR regression rebuilds the complete classification table from
`corpus.json`, `ps_ir.bin` and the read-only retail CTABs, then compares it
field for field.  The approximation regression rebuilds all 138 plans and
re-runs their symbolic simplified-spec verification.  Original-shader
equivalence is deliberately not claimed.

Only this directory was edited.  `DevCaps` remains `0x00082000`; no SM3 slots,
frame, retail bytecode or new draw path are claimed.
