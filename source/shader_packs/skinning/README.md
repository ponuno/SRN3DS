# Shader pack `skinning`

Owner: `sm3-agent-skinning`

character/normal-map/tint-mask shaders and skinned VS

* census families: `skinning/characters`
* chunks: **2162** / unique hashes: **1194**
* pack catalog coverage: **83 translated**, **0 draw-enabled**, **1111 refused**
  (`plans.json` / `refusals.json`; `manifest.json` and
  `docs/shader_coverage.json` index the catalog since `8abe218`, while the
  16-slot runtime table stays untouched because no entry sets `"runtime": 1`)
* first step: land the VS skinning contract before any PS work

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## What is translated

`plans.json` is the pack catalog: one `d9-shader-plan-v1` entry per hash that
the shared planner proves offline.  It is generated, never hand-edited:

```powershell
python source\shader_packs\skinning\pack_plans.py generate --data F:\Skyrim\Data
python source\shader_packs\skinning\pack_plans.py verify --data F:\Skyrim\Data
python -m unittest discover -s source\shader_packs\skinning -p "test_*.py"
```

| stage | translated | draw-enabled | why no draw |
|---|---:|---:|---|
| VS `pica_vs` | 83 | 0 | native PICA VS code emitter does not exist |
| PS `tev` | 0 | 0 | every PS needs a shared literal-range rewrite |
| **total** | **83** | **0** | |

Every translated VS is a skinned contract: `position` + `blendweight` +
`blendindices` inputs, 1-6 varyings plus the position output (<=7 outputs
total), 8-16 uniform registers, no control flow, and `skinning: true`.  The
plan is a descriptor, not an executable shader; `draw_enabled` is 0 by the
schema rule and `codegen` stays `pending`.

The catalog is merged into coverage by `tools/fxp_compile.py manifest` /
`tools/shader_packs.py` since `8abe218`.  No entry sets `"runtime": 1`, so the
16-slot runtime table (`D9_SHADER_PLAN_MAX_ENTRIES`) keeps its four canonical
entries; the 83 contract-only descriptors exist for diagnostics and future
codegen, not for a draw.

## Named refusals (`refusals.json`)

| reason | hashes | blocking lever |
|---|---:|---|
| `def_literal_out_of_range` | 1044 | shared literal rewrite (subtract formulation); all 1044 actually read the out-of-range component, so clamping is not equivalent |
| `unsupported_opcode` | 42 | `cmp` / `if` / loop / `rcp` / derivatives / `texldd` lowering |
| `pica_vs_output_envelope` | 14 | planner accepted 7 varyings + position = 8 outputs, schema caps 7 |
| `too_many_vs_varyings (8 > 7)` | 9 | varying packing or multi-pass VS |
| `too_many_vs_varyings (9 > 7)` | 2 | varying packing or multi-pass VS |

Each row names the offending literal (register/component/value) or the
blocking lever.  The two VS classes are counted by the shared planner before
the position output is added; `pica_vs_output_envelope` is the schema gap the
pack refuses rather than hides.

## Shared fixes requested (not done here)

The pack stays inside its directory; these need the shared owner:

1. **Literal-range rewrite.** 1,044 PS hashes read a `def` component outside
   `[0,1]` (e.g. `c8 = (2, -1, 3.98, 0)`).  `plan_ps(..., ignore_def_range=True)`
   would clamp the literal in the descriptor and still pass its own symbolic
   check, so the pack refuses them instead.  A subtract/bias formulation or an
   extended literal path is needed before any of these can be equivalent.
2. **VS output envelope mismatch.**  `plan_vs` counts only `usage != 0`
   varyings against PICA's 7; `d9-shader-plan-v1` caps every output, and
   `position` is an output.  Fourteen VS descriptors (7 varyings + position)
   are therefore planner-accepted but schema-invalid.  Align `plan_vs` with the
   schema (or document the position exception) before those hashes are claimed.
3. ~~**Catalog integration.**~~  **Closed in `8abe218`**: pack catalogs are
   discovered and indexed for coverage; only `"runtime": 1` entries enter the
   shared table.  This pack contributes 83 `translated` rows and zero slots.
4. **Opcode lowering** for the 42 `cmp`/`if`/loop/`rcp`/derivative/`texldd`
   PS hashes, and **varying packing / multi-pass VS** for the 11 VS hashes
   that declare 8-9 varyings.

## Tests

* `test_pack.py` (7 tests): catalog/refusal partition of all 1,194 hashes,
  schema validation of all 83 descriptors, named refusals, PICA VS limits, the
  draw rule, and symbolic regeneration when the retail archive is present.
* `pack_plans.py verify --data` regenerates all 83 plans from bytecode and
  compares them field for field: **83/83 pass**.
* Shared targeted host suite: `shader_plan_table` (85 checks), `tev_translate`,
  `tev_accuracy`, `tev_com`, `tev_text_plan`, `tev_real` all pass unchanged.
