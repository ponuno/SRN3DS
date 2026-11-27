# Shader pack `ui`

Owner: `sm3-agent-ui`

menu/UI pixel and vertex shaders: A8 atlas copies, text tint, simple colour math

* census families: `ui/interface`
* chunks: **314** / unique hashes: **113**
* pack catalog coverage: **113 translated** (24 `translated` + 89
  draw-enabled), 0 refused
* first step: re-plug the certified A8 text plan; keep draw blocked

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## BSA `ui/interface` catalogue (113/113 planned)

`plans.json` is the pack catalog.  It carries the ten StartMenu/GFx prefix
groups measured from `TESV.exe` (unchanged, see below) **and** one
`d9-shader-plan-v1` entry for every BSA hash in `manifest.json`:

```powershell
python source\shader_packs\ui\bsa_pack_plans.py generate --data F:\Skyrim\Data
python source\shader_packs\ui\bsa_pack_plans.py verify   --data F:\Skyrim\Data --symbolic
python -m unittest discover -s source\shader_packs\ui -p "test_*.py"
```

| stage | plans | exact | approx | draw-enabled / `runtime: 1` |
|---|---:|---:|---:|---:|
| VS `pica_vs` contract | 40 | 40 | 0 | 0 |
| VS emitted-verified | 16 (of 40) | 16 | 0 | **16** |
| PS TEV | 73 | 1 | 72 | **73** |
| **total** | **113** | **41** | **72** | **89** |

Every entry has `quality`.  An approximate PS also has an `approximation`
string and explicit `preconditions`; a draw-enabled PS carries a complete
`material_contract` (textures, constants, derived `constant_values`,
varyings).  Approximate PS entries are draw-enabled **for the declared
simplified spec only**; their `note` says so.  No frame claim is made and
`DevCaps` is untouched.

### PS: TEV plans, derived constants and declared approximations

`bsa_ps.py` plans the retail UI/text PS in layers:

* **exact** вЂ” the shared planner accepts it (1 hash, `817b1245`:
  `oC0.rgb = tex0 * colour * c0`, `oC0.a = tex0.a`).
* **relaxed** (5 hashes) вЂ” `menu_relaxed` schedules temp reuse, split
  `oC0.xyz`/`oC0.w` and `lrp` -> TEV `INTERPOLATE`, while a literal `def`
  outside RGBA8 ([1.5], [2,1.5], [-0.5]) becomes a **derived uniform
  constant** in `constant_values` (never clamped).  One of these is exact;
  the other four are `approx` because a texture-coordinate varying operand was
  dropped.
* **basic UI tint** (46 hashes) вЂ” the retail text shaders keep
  1-2 atlas samplers and a constant tint in the multiplicative prefix, then
  modulate by the vertex colour.  Contrast/`lrp`/`cmp`/`rcp`/`texkill` terms
  are dropped by name.  Programs with no texture read declare a solid
  constant colour.
* **DP4 prebake** (22 hashes) вЂ” the `tev_prebake` class plus the DP4 rows in
  the `def_literal_out_of_range`/`unsupported_opcode` classes.  TEV has no
  4-component dot and cannot hold four per-channel constants in one stage;
  the declared simplified spec is one `DOT3` row plus the alpha channel.  The
  plan carries `preconditions: normal_tangent_basis_prebaked` and
  `texcoord_basis_varying`; the other rows and the alpha/offset term are
  named in `approximation`.

PICA TEV has no varying source except the two vertex colours.  Any PS that
reads `v0.z`/`v0.w` arithmetically, or samples with a PS-computed coordinate,
is therefore physically inexpressible 1:1.  Those terms are replaced by 1.0 /
the base varying and recorded; nothing is silently clamped or dropped.

### VS: 16 emitted and numerically verified, 24 exact contracts still pending

`bsa_vs_emit.py` (adapted from the skinning emitter, `bones = 0`) emits the
canonical vs_3_0 IR to picasso and verifies it three ways: structure/picasso,
a D3D vs_3_0 reference interpreter, and an interpreter over the emitted PICA
text on 256 samples (tolerance 1e-4).  `bsa_vs_report.json` pins the
`.shbin` SHA-256 and byte count for every pass.  The 16 cover
position/texgen/UI-colour programs plus `frc` (lowered to `x - flr(x)`) and
the `rcp`/`rsq` normalisation programs.  The other 24 keep an **exact
`pica_vs` contract** (`codegen: pending`, draw 0) with a named lever:

| reason | hashes | physical lever |
|---|---:|---|
| `texcoord3+` output | 19 | PICA has only `outtc0..2`; needs varying packing or a multi-pass VS |
| second colour together with colour0 | 2 | PICA has one raster colour |
| `sincos` | 1 | no PICA VS trigonometric opcode |
| numeric proof failed (2 pow/abs programs) | 2 | emitter precision gap; plan stays pending, not claimed |

The 19 `texcoord3+` programs are the physical ceiling, not a scheduler gap:
picasso rejects `texcoord3` as an output type (`invalid output type`), and
`outclr`+`outtc0..2`+`outpos` are the only VS outputs on PICA200.

The emitted-program table is compiled for ARM11 (compile-only, no link):

```powershell
python source\shader_packs\ui\emit_arm_table.py
arm-none-eabi-gcc -c -O2 -std=gnu11 -march=armv6k -mfloat-abi=hard `
    source\shader_packs\ui\generated\ui_vs_table_check.c -o ui_vs_table.o
```

## StartMenu / GFx priority set (phase 2)

`docs/MENU_SHADERS.md` names 10 hashes created on the way to
`Interface/StartMenu.swf`: 5 VS objects and 40 PS objects.  Those blobs are
not in `Skyrim - Shaders.bsa`; they are in `TESV.exe` in the GFx renderer
tables (VA `0x12dfca8`, 6 slots, and VA `0x12dfcd8`, 45 slots).
`menu_plan_tool.py` reads them in place (nothing retail is copied) and writes:

* `menu_plans.json` вЂ” the ten measured prefix groups (5 descriptors, 5 named
  refusals), with both the measured prefix hash and the true full-blob SHA-256;
* `plans.json` вЂ” the shared catalogue, one entry per distinct full blob
  (5 VS + 14 PS translated, 24 PS refused); only exact, fully contracted PS
  set `runtime: 1`;
* `menu_ir.json` вЂ” derived IR metadata only (opcode names, declarations,
  symbolic final expression); no bytecode.

### Measured finding: the 10 hashes are not 10 programs

The usage log hashes `high16(second_token) * 4` bytes from offset 0.  In these
D3DX blobs the second token is the leading `CTAB` comment, so that length is
the comment size, not the shader size.  Walking tokens to `END` gives the real
blob.  On the same run:

| | usage-log prefix hashes | distinct full blobs | objects |
|---|---:|---:|---:|
| GFx VS | 3 | **5** | 5 |
| GFx PS | 7 | **38** | 40 |

So the "3 VS + 7 PS unique" statement collapses distinct code.  The 10 plans
here are the 10 *measured prefix groups*; `menu_plans.json` records
`measurement.prefix_hash_defect` and the full-blob counts.  This is a
measurement defect in the log, not a translation result.

### Coverage of the GFx catalogue (38 PS / 5 VS full blobs)

| Status | VS | PS |
|---|---:|---:|
| translated | **5** | **14** |
| draw-enabled / `runtime: 1` | 0 | **3** |
| named refusal | 0 | 24 |

Runtime entries are exact descriptors with a complete declared material
contract:

* `bfc0b3e0` вЂ” `oC0 = c0` (solid colour; c0 is the GFx colour transform).
* `0eb58f00` вЂ” `oC0 = tex0 * c2 + c3` (sampler0 2D, c2/c3 colour transform).
* `9c152cf8` вЂ” `oC0 = c0` variant with a second stage.

Four PS are declared approximations (`quality: approx`): the `8ced9de9` group
(`ee79ab27`, `95d3d096`, `71d4da27`, `ed5e7b5d`).  TEV has no 4-component dot,
so the 4x4 `dp4` basis is collapsed to one `DOT3` row; the other rows and the
c4 alpha/offset term are dropped.  The plan is exact for that declared spec
and carries the text in `approximation`.

Remaining 24 GFx PS refusals are named: `non_uniform_swizzle: c0.zwzw` (YUV
rows), `too_many_stages (>6)`, `stage_needs_two_constants`, and
`plan_mismatch` for the alpha-recombine patterns.

### Verify (GFx measurement)

```text
python source\shader_packs\ui\menu_plan_tool.py check --tesv <TESV.exe>
python source\shader_packs\ui\test_menu_plans.py --tesv <TESV.exe>
```

`check` replays each TEV descriptor to its final `oC0` expression and compares
it with the IR expression; with `--tesv` it re-extracts the blobs, re-runs
`tools/shaders/compile.py` and regenerates the plans from retail bytes.

### Shared integration

Plans live in two files:

* `plans.json` вЂ” schema `d9-shader-plan-v1`, the catalog the shared tools
  discover.  The 43 GFx entries and 113 BSA entries are disjoint by SHA-256.
* `menu_plans.json` / `menu_ir.json` вЂ” the measurement record for all ten
  prefix groups: named refusals, both hash forms, and the IR expression the
  symbolic check compares against.

Checked against this tree:

```text
python -m unittest discover -s source\shader_packs\ui -p "test_*.py"   -> 9 tests OK
python source\shader_packs\ui\bsa_pack_plans.py verify --symbolic\
    --data F:\Skyrim\Data                                              -> ok, 113/113
python tools\shader_packs.py check                                     -> OK
python tools\shaders\plan_verify.py --plans <BSA-only doc>             -> 0 schema errors
```

`plan_verify` on the **whole** `plans.json` still reports 8 pre-existing GFx
rows (`menu.gfx.ps.*`): their `constant_ops` uses `dst: 9` (D3D c9) where the
current shared schema (`tools/shaders/plan_verify.py`) requires
`dst_reg: 0..7`.  That is the GFx measurement pass's record, unchanged here
and identical in the main tree; the 113 BSA entries contribute **zero**
schema errors.  Reconcile the GFx derived-constant encoding in its own pass.

The ten menu hashes are outside the BSA census; the BSA catalog now covers
all 113 `ui/interface` hashes.  `docs/shader_coverage.json` and
`source/shader_packs/index.json` are generated by the shared tool and are
intentionally not rewritten from this pack; run
`python tools\shader_packs.py manifest` + `coverage` at integration time to
refresh the aggregate totals (the pack manifest itself is already updated:
113 translated, 89 draw-enabled, 0 refused).

