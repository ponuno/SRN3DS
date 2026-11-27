# Shader pack `skinning`

Owner: `sm3-agent-skinning`

character/normal-map/tint-mask shaders and skinned VS

* census families: `skinning/characters`
* chunks: **2162** / unique hashes: **1194**
* pack catalog coverage: **177 translated**, **3 draw-enabled**, **1017 refused**
  (`plans.json` / `refusals.json`; `manifest.json` and
  `docs/shader_coverage.json` index the catalog, while the 16-slot runtime
  table stays at its three committed PICA VS entries)

## What is translated

`plans.json` is the pack catalog: one `d9-shader-plan-v1` entry per hash that
the pack can prove or explicitly approximate.  It is generated, never
hand-edited:

```powershell
python source\shader_packs\skinning\pack_plans.py generate --data F:\Skyrim\Data
python source\shader_packs\skinning\pack_plans.py verify   --data F:\Skyrim\Data
python source\shader_packs\skinning\pica_vs_emit.py emit-all --data F:\Skyrim\Data
python -m unittest discover -s source\shader_packs\skinning -p "test_*.py"
```

| stage | plans | quality | draw-enabled | runtime |
|---|---:|---|---:|---:|
| VS `pica_vs` contract | 83 | exact | 0 | 0 |
| VS `pica_vs` emitted-verified | 29 (of 83) | exact (numeric proof) | 3 | 3 |
| PS `tev` basic tint | 94 | **approx** | 0 | 0 |
| **total** | **177** | | **3** | **3** |

Every plan carries `quality` (`exact`/`approx`); approximate plans also carry
an `approximation` string.  Only the three committed PICA VS representatives
are draw-enabled and set `"runtime": 1`; all other emitted-verified programs
set `"runtime": 0` and `draw_enabled: 0`, so the C upload table does not grow.

### PS: the basic tint / membrane / facegen class (`ps_tint.py`)

The shared `plan_ps` accepts only a strict single-temp straight-line graph and
an RGBA8 `def`, so **none** of the 1,086 PS hashes could be translated by it:
the census class `def_literal_out_of_range` is only the first refusal, and
every one of those 1,044 shaders also reads `nrm`/`rcp`/`rsq`/`pow`/`cmp` or
reuses a temp across stages (the shared planner's `temp_not_previous` /
`written twice` limits).  A 1:1 lowering needs a multi-temp TEV scheduler and
opcode library; that remains open.

What this pack lands is the well-understood **basic albedo tint**: shaders
with straight-line control flow (no `ifc`/`if`/`rep`) and at most three 2D
textures become one TEV stage per texture plus one MODULATE by the vertex
colour.  Normal, specular, lighting and the `texkill` alpha test are dropped,
so the plan is `quality: "approx"`, names exactly what was dropped in
`approximation`, and is never draw-enabled.  No literal `def` is baked, so the
RGBA8 range limit does not apply to this class.  **94 hashes** translate this
way (1-3 textures each).

`cmp`/`if` folding and `def` -> uniform/scale for the remaining PS are still
open; they need the scheduler first, because folding does not remove the
multi-temp dataflow.

### VS: emitted code (29 of 83)

`pica_vs_emit.py` lowers the canonical vs_3_0 IR of **29** contracts to
picasso `.v.pica` and verifies them three ways: outputs/inputs/uniform/opcode
envelope, a D3D IR reference interpreter, and an interpreter over the emitted
PICA text on 256 deterministic samples.

| metric | value |
|---|---|
| emitted-verified contracts | 29 |
| max abs position error | **0.0** (all 29) |
| max abs varying error | 4.9e-10 (tolerance 1e-4) |
| picasso | pass, non-empty `.shbin` |
| draw-enabled / runtime | 3 (`f8ffaecd`, `20f4db07`, `60dbd11a`) |

The 29 cover position-only, position+UV0/1/2, vertex tint, normal transforms,
`dp3`/`dp4`/`add`/`sub`/`min`/`max`/`slt`/`sge`, relative palette rows, and
`abs`/`nrm`/`lrp` synthesis.  `nrm` lowers to `dp3`+`rsq`+`mul`; `abs` to
`max(x,-x)`; `lrp` to `add`+`mad`; `sub` to `add` with a negated source.

Two emitter bugs were fixed on the way:

* upstream `asm.py` moved the `a0.<component>` token onto `Src.relative`;
  the old emitter still looked for a following `REG_ADDR` pseudo-source and
  refused every skinned program (`relative_constant_without_address_token`).
  The three committed representatives did not regenerate before this fix.
* PICA has a single raster colour: `color1` outputs stay a named refusal
  (`output_color1_unmapped`, 39 contracts), as do `texcoord3+` semantics.

The other 54 contracts stay `codegen: pending`: 39 declare a second colour,
9 use `frc` outside the index-floor idiom, 3 declare `texcoord3+`, and 3 hit an
interpreter operand form.  Each would need a packing contract (pack
normal+colour into one varying, octant encoding) or a source fix; no claim is
made for them.

### Draw gate

The three representatives carry `draw_enabled: 1`, `runtime: 1` and the full
`emitter` block (`generated/*.shbin`, SHA-256 and byte count, plan inputs and
the PICA uniform row map).  `pack_plans.py emit-runtime` only emits entries
that are both `emitted-verified` and `draw_enabled == 1`, so the runtime table
and `source/d3d9_pica_vs_*.inc` are unchanged.  `DevCaps` and any frame claim
are untouched.

## Named refusals (`refusals.json`)

| class | hashes | reason / blocking lever |
|---|---:|---|
| `def_literal_out_of_range` | 950 | `ps_texture_budget` (4+ textures) or `ps_not_straight_line` (branches); basic tint only covers <=3 textures |
| `unsupported_opcode` | 42 | `pow`/`texkill`/`lrp` outside the straight-line class |
| `pica_vs` | 14 | 8 outputs (7 varyings + position); needs varying packing |
| `too_many_vs_varyings (8 > 7)` | 9 | varying packing or multi-pass VS |
| `too_many_vs_varyings (9 > 7)` | 2 | varying packing or multi-pass VS |

The refusal keeps the manifest `class` (the census label is fixed); its
`reason`/`needs` name the *actual* remaining lever, not just the first one.

## Tests

* `test_pack.py` (7 tests): catalog/refusal partition of all 1,194 hashes,
  schema validation of all 177 descriptors, quality/approximation fields, the
  draw/runtime rule, named refusals, and symbolic regeneration from the
  retail archive.
* `test_emitter.py` (7 tests): report/catalog agreement for 29
  emitted-verified programs, the four-weight palette proof, the real c184
  uniform row, and the "empty/skipped is not verified" rule.
* `pack_plans.py verify --data` regenerates all 177 plans: **177/177 pass**.
* `tools/shader_packs.py check` and
  `tools/shaders/plan_verify.py --plans docs/shader_plans.json` are green.
