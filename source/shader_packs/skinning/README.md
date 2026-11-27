# Shader pack `skinning`

Owner: `sm3-agent-skinning`

character/normal-map/tint-mask shaders and skinned VS

* census families: `skinning/characters`
* chunks: **2162** / unique hashes: **1194**
* pack catalog coverage: **80 translated**, **3 draw-enabled**, **1111 refused**
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

## Emitted code (3 of 83)

`pica_vs_emit.py` lowers the canonical vs_3_0 IR of three representatives to
picasso `.v.pica` and verifies them three ways: outputs/inputs/uniform/opcode
envelope, a D3D IR reference interpreter, and an interpreter over the emitted
PICA text on 256 deterministic samples.

| plan | varyings | palette | uniform rows | emitted ins | max abs pos | max abs varying | picasso |
|---|---|---:|---:|---:|---:|---:|---|
| `skin.vs.f8ffaecd` | position | 4 bones | 16 | 53 | 0 | 0 | pass |
| `skin.vs.20f4db07` | position + UV1 | 4 bones | 16 | 55 | 0 | 0 | pass |
| `skin.vs.60dbd11a` | position + UV0 + UV2 | 4 bones | 17 | 57 | 0 | 0 | pass |

Tolerance is `1e-4`; all three measure `0.0` on 256 samples.  The emitted
epilogue remaps D3D9 clip space to the backend's rotated PICA convention
(`x'=y, y'=-x, z'=z-w, w'=w`, the same map as `d9_pica_transform()`), and the
four blend influences select their own `a0` component with a `mova` before each
indexed row (the retail address token is `a0.x/y/z/w`, not always `a0.x`).
Uniform rows other than the palette (including c184) are filled with distinct
random values during verification, so a skipped UV transform cannot pass as
`0 == 0`.

Emitter policy found by compiling the real assembly:

* the retail index idiom `mul x=v*c; frc f=frac(x); mad a=v*c-f` is exactly
  `floor(v*c)`; PICA VS has `flr` but no `frc`, so the three-instruction idiom
  folds to `mul` + `flr` (verified against the original IR);
* PICA allows no indexed uniform in `mad`; a dynamic `mad` lowers to `mul`
  through a second scratch plus `add`;
* `mad` needs its first source in a register (the second may be a constant);
* the uniform bank is `c0..c11` for 4 bones (`a0 = 3*bone`) and `c12..c15`
  for D3D `c180..c183` (WVP), i.e. 16 rows.  A larger palette is a draw-time
  window contract, not a maths approximation.

### Why the other 80 stay `codegen: pending`

The emitter is shape-specific on purpose: outputs must map to
`position`/`texcoord0..2`/`color0`, relative constants to `c0..c2` with an
`a0.x/y/z/w` address token, and the body to the six supported opcodes.  The
other 80 contracts use texcoord3-5, colour-carry outputs, different constant
groups or extra opcodes; each needs its own mapping and its own 256-sample
proof.  No claim is made for them.
The 22 eight/nine-varying hashes remain named refusals: no normal output
exists in this pack to pack, and an octant/hemisphere encoding was not
adopted without a measured error bound.

### Draw gate (implemented)

The three emitted plans now carry `draw_enabled: 1`.  The schema allows that
only with `codegen == "emitted-verified"` and a complete `emitter` block
(`generated/*.shbin`, its SHA-256 and byte count, the plan inputs and the
PICA uniform row map).  `tools/shaders/plan_verify.py` mirrors the rule, and
`pack_plans.py verify` re-hashes the committed `.shbin` on every run.

`source/d3d9_pica_vs.{h,c}` is the runtime side:

* the `.shbin` bytes are embedded by `pack_plans.py emit-runtime` and selected
  by exact vs_3_0 SHA-256; `d9_pica_vs_validate` checks the DVLB header, the
  main-function word count and the pinned SHA-256;
* `d9_pica_vs_declare` maps the active D3D9 declaration to the emitted
  loaders — position FLOAT3, blendweight FLOAT4, blendindices UBYTE4N and the
  optional texcoord/color inputs — and refuses any other shape by name;
* `d9_pica_vs_bank` lays out the 4-bone window (c0..c11 palette, c12..c15
  WVP, c16 for the 17-row program) and `d9_pica_vs_bank_ready` refuses when a
  D3D register was never set;
* `d9_pica_vs_device_open/bind` (3DS only) calls `DVLB_ParseFile`,
  `shaderProgramSetVsh`, `AttrInfo_AddLoader` and `C3D_FVUnifSet`.

The draw gate in `d3d9_com.c` runs every one of those checks per draw and
names the first missing piece.  The skinned stream is wired: `d3d9_core.c`
decodes only BLENDWEIGHT/BLENDINDICES vertices into `D9SkinVertex`, refuses any
bone index outside the 4-bone window by name, and submits through
`draw_pica_vs`; the emitted shader never passes through the ordinary fixed
fetch/T&L path.  UBYTE4N attributes are decoded to normalised floats before
submission because PICA integer loaders do not normalise.  The draw is still
gated by the declaration, the full uniform bank (including c184 for the 17-row
program), the backend callback and the fragment-stage refusal; no frame is
claimed here.

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
