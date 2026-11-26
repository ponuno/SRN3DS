# Offline SM3 -> PICA200 compiler (variant B)

Status: **offline compiler, census and generated runtime table connected as a
lookup module; no runtime SM3 translation and no new draw path.** A rendered
menu frame is still not claimed.

The port has two honest ways to run a shipped shader. Variant A keeps a
bounded runtime selector for the two certified menu programs. Variant B,
described here, moves the work offline: every `.fxp` bytecode blob is decoded,
classified and (where the subset allows) turned into a native PICA200
descriptor before the game runs. The runtime only looks up a descriptor by
exact SHA-256; it never interprets SM3 and never builds a "close enough" plan.

The global capability report is unchanged:

```text
GetDeviceCaps: DevCaps 0x00082000, VS 2.0 (`0xfffe0200`), PS 2.0
               (`0xffff0200`), SM3 slots 0, MaxVertexShaderConst 4
```

`D3DXCompileShader` remains a named `E_NOTIMPL`. Shaders are precompiled in
the retail FXP files; the compiler consumes those chunks, not HLSL.

## 1. Pipeline

```text
Skyrim - Shaders.bsa
        |
        |  tools/fxp_census.py           (B0: census)
        v
 decode BSVS/BSPS -> SM3 IR (tools/shaders/compile.py)
        |
        |  classifier (named accept/refuse)
        |
        +--> TEV planner -----> D9TevProgram descriptor --+
        +--> PICA VS contract -> input/output/uniform set -+
                                                           |
        |  tools/fxp_compile.py manifest                    |
        v                                                   |
 docs/shader_plans.json  --(emit-c)--> source/d3d9_shader_plan_table.inc
                                                           |
                                                           v
                                      d9_shader_plan_lookup(stage, sha256)
                                      d9_shader_plan_draw_allowed(plan)
```

The compiler is deterministic: the same archive produces the same
`docs/shader_plans.json` and the same `.inc` byte for byte. The host test
regenerates the include and compares it to the committed file.

## 2. B0 census (real `Skyrim - Shaders.bsa`)

Tool: `tools/fxp_census.py`. It reads the retail archive read-only and writes
derived metadata only:

* `E:\Skyrim_N3DS_build\assets\shaders\fxp_census.json` -- full per-hash table;
* `docs/SHADER_CENSUS.json` -- the summary below.

Measured 2026-11-26 over the 122-package base archive (no `Update.bsa`):

| Quantity | Value |
|---|---:|
| Packages | 122 |
| Chunks | 10,475 (9,621 PS / 854 VS) |
| Distinct bytecode hashes | 6,659 (6,350 PS / 309 VS) |
| Versions | `ps_3_0` / `vs_3_0` only |
| Distinct instruction streams | 6,655 |

### 2.1 Pixel shaders

| Class | Hashes | Chunks | Meaning |
|---|---:|---:|---|
| `tev_direct` | 7 | 16 | plan generated and structurally verified |
| `tev_prebake` | 7 | 18 | only blocker is the `dp4` normal/basis pre-transform |
| `tev_menu_light` | 1 | 1 | certified `p1200005` five-stage plan |
| `def_literal_out_of_range` | 6,166 | 9,019 | a `def` component is outside the RGBA8 [0,1] range |
| `unsupported_opcode` | 126 | 478 | `cmp`, loop/`if`, `rcp`, derivatives, `texldd`, ... |
| `dataflow` | 39 | 81 | stale/duplicate temp, temp not the previous stage |
| `non_uniform_swizzle` | 2 | 5 | `.xzy`, `.xxy`-style per-channel swizzle on one operand |
| `multiple_output_writes` | 1 | 2 | partial `oC0` write followed by another write |
| `input_not_a_colour_varying` | 1 | 1 | reads a texcoord as if it were a colour varying |

Generated PS coverage: **15 hashes / 35 chunks** (0.24 % of PS hashes,
0.36 % of PS chunks). The remaining 6,335 hashes / 9,586 chunks are named
software refusals. This is not a claim that the other shaders are impossible;
it is the measured envelope of the pilot compiler.

The largest single lever is the literal range: 9,019 chunks define a constant
such as `{1,-1,0,0}` or `{2,-1,1,0}`. PICA TEV stage constants are RGBA8
`[0,1]`, so the generic planner refuses instead of silently clamping. The
census re-runs the planner with clamped literals as an **upper bound only**:
8,997 of those chunks still fail on opcodes/dataflow, so a literal rewrite
alone buys only 1 direct plan and 11 basis-prebake plans. The honest next
steps are per-channel TEV source selectors (the hardware supports them; the
current `D9TevStage` descriptor is per-source) and subtracting formulations
for negative constants.

### 2.2 Vertex shaders

| Class | Hashes | Chunks | Meaning |
|---|---:|---:|---|
| `pica_vs` | 276 | 757 | opcodes/uniforms/inputs fit a PICA VS contract |
| `too_many_vs_varyings (8 > 7)` | 26 | 82 | declares 8 varying outputs |
| `too_many_vs_varyings (9 > 7)` | 7 | 15 | declares 9 varying outputs |

PICA VS expressibility: **276 hashes / 757 chunks (88.6 % of VS chunks)**.
The 276 contracts use at most 96 float uniforms (measured maximum 18), 16
inputs and 7 varying outputs. 95 of them contain an opcode that lowers
mechanically (`abs`, `nrm`, `lrp`); the rest use the direct set.

**No VS is executable yet.** `plan_vs()` emits a contract descriptor, not
PICA VS code, and the runtime table marks every VS entry `draw_enabled = 0`
with the named reason `PICA VS code emitter is absent`. Counting a contract as
"PICA VS covered" is a statement about the envelope, not about a draw.

## 3. B1 compiler subset

`tools/shaders/compile.py` builds a straight-line IR:

* `dcl`, `def`, `nop` are declarations;
* single-assignment temps only, no relative addressing, no source modifiers;
* destination modifiers 0/1/2 (saturate and partial-precision hints);
* PS writes exactly one `oC0`, as the **last** instruction, with mask `0xF`;
* `texld` coordinates must be an input register (no dependent reads);
* 1..3 distinct 2D samplers; at most 6 stages and 8 external constants.

Direct PS ops: `mov`, `add`, `sub`, `mad`, `mul`, `dp3`, `texld`, `def`, and
`min/max` only in `min(x,1)` / `max(x,0)` clamp form. `dp3` maps to
`DOT3_RGBA` (or `DOT3_RGB` for an `.xyz` write) and requires unswizzled
operands. Each source must be a texture, a colour varying, a constant, or the
immediately previous stage result; a per-source swizzle must be identity or a
single replicated component, matching the existing `d9_tev_translate_pixel`
descriptor model.

The planner emits a `D9TevProgram`-shaped plan, then **verifies it
symbolically**: it re-evaluates the stage list and compares the final `oC0`
expression with the IR expression. A mismatch is `plan_mismatch`; no partial
plan is returned.

Every refusal is a stable string, for example:

```text
unsupported_opcode: rcp
dataflow: dependent texture read (coord is r0)
def_literal_out_of_range: c0
dp4_basis_requires_prebake: normal/tangent basis must be prebaked
temp_not_previous: mad at word 96 reads r0; TEV can only read the previous stage
too_many_stages (>6)
multiple_output_writes (2)
```

### 3.1 Differential check against the existing translator

The C translator in `source/d3d9_tev.c` accepts 19 of the 9,621 retail PS
chunks. The Python direct planner was compared chunk by chunk against that
measured set:

* 16 accept (same one-stage and two-stage straight-line shapes);
* 2 refuse `multiple_output_writes`: the C translator accepts them but its
  final stage recomputes alpha instead of carrying the earlier `oC0.w`
  write, then forces alpha to 1. The Python refusal is deliberately stricter;
* 1 refuses `input_not_a_colour_varying`: the C translator maps a
  `texcoord1` input to `PRIMARY`, which is not the declared semantic.

So the compiler reproduces the proven C subset and refuses the three cases
where the old translator's plan is not an exact implementation.

### 3.2 Menu-light reference

The p1200005 retail signature has no direct TEV form (three `dp4` basis rows,
`.xzyw` swizzles, multiple live temporaries). `menu_light_plan()` recognises
the exact signature and emits the same five stages as
`d9_menu_light_plan()`:

| Stage | Function | Sources | Constant |
|---:|---|---|---|
| 0 | `dot3_rgba` | `TEX1, c3` | c3 |
| 1 | `multiply_add` | `PREV, c4, FRAG_PRIMARY` | c4 |
| 2 | `modulate` | `TEX0, PREV`; alpha `TEX0.a, PRIMARY.a` | -- |
| 3 | `modulate` | `PREV, PRIMARY`; alpha `*c4.a` | c4 |
| 4 | `modulate` | `PREV, c7.a`; alpha replace | c7 |

The test compares the generator output field for field with the reference
quoted from `source/d3d9_menu_light.c`. The plan stays `draw_enabled = 0`
because the material path must still supply the normal/tangent prebake and
the c8 ambient+emissive value.

## 4. B2 pilot manifest and runtime table

`tools/fxp_compile.py manifest` produces `docs/shader_plans.json`; `emit-c`
turns it into `source/d3d9_shader_plan_table.inc`. Four entries:

| Id | SHA-256 (prefix) | Source | Result | Draw |
|---|---|---|---|---|
| `menu.logo.ps` | `8725393b...5ff9` | `p1200005.fxp` key `0x01200005` | 5-stage TEV, 2 textures, c3/c4/c7 | disabled: preconditions |
| `menu.composite.ps` | `6022044e...45de` | `imagespace002.fxp` +4534 | 1-stage `REPLACE(TEX0)` | enabled |
| `game.ps.f5239354` | `f5239354...` | `shaders001.fxp` key `0x00040002` | 2-stage generic TEV | disabled: not certified |
| `game.vs.0ccc7542` | `0ccc7542...` | `imagespace002.fxp` +4808 | PICA VS contract, 2 ops | disabled: no codegen |

The runtime module is `source/d3d9_shader_plan.{c,h}`:

* `d9_shader_plan_lookup(stage, sha256, ...)` is exact-hash only and accepts
  upper/lower case; a miss returns `0` and names the stage and hash prefix.
* `d9_shader_plan_draw_allowed(plan, ...)` returns `0` only for a
  draw-enabled, internally consistent TEV plan. A disabled plan returns `-1`
  with its `note`; a VS descriptor returns `-1` naming the absent code
  emitter.
* `d9_shader_plan_count()` / `d9_shader_plan_at()` expose the table for
  diagnostics and tests.

The table does **not** change `CreatePixelShader`/`CreateVertexShader` yet.
Wiring it in is a separate step: a plan must first be certified equivalent to
the existing runtime path or to a material contract, exactly as
`menu.composite.ps` is. The current selector in
`source/d3d9_menu_shader_runtime.c` remains authoritative for the two
certified menu entries. Uncompiled hashes continue to take the legacy path
and are refused by name there.

## 5. Tests

Host, Python (`tests/fxp_compile_test.py`, 13 tests):

* synthetic fixtures: IR parse, copy/tint/mad/clamp plans, `pica_vs`
  descriptor, deterministic JSON and C output;
* named refusals: `rcp`, `if`, dependent read, out-of-range `def`, >6 stages,
  `dp4` as `tev_prebake`;
* real archive: logo hash -> five-stage plan, copy hash -> one stage,
  committed `.inc` equals regenerated output, census hashes and coverage
  lower bounds.

Host, C (`tests/shader_plan_table_smoke.c`, 53 checks):

* exact-hash hit for logo and copy, upper-case equivalence;
* the copy entry's used stage fields equal the descriptor built by
  `d9_tev_translate_pixel` from the same three-instruction stream;
* foreign hash -> named miss; wrong stage -> named miss; short/NULL hash ->
  invalid;
* logo draw refused by preconditions, copy draw allowed, pilot PS/VS draw
  refused by name;
* DevCaps exactly `0x00082000`, VS/PS 2.0, zero SM3 slots,
  `MaxVertexShaderConst = 4` after lookups.

Targeted run:

```powershell
powershell -File tools\run_host_tests.ps1 -Only shader_plan_table -SkipPython `
  -OutDir C:\Users\vasya\Documents\Skyrim_N3DS_wt\shader-compiler-b\build_host_targeted
```

The existing menu e2e is unchanged: `menu_pipeline_e2e` still reports
**57,484 vertices / 86,160 indices / 28,720 triangles, one indexed draw**
(shape-only census 57,480 / 86,154), and no new draw is submitted.

## 6. What is not done

1. **PICA VS code emitter.** The 276 accepted VS contracts are descriptors
   only. `draw_enabled = 0` is enforced; the next step is a PICA VS binary
   emitter plus a shader-binary upload, with the same symbolic equivalence
   test used for TEV.
2. **Per-channel TEV selectors.** `D9TevStage.op_rgb` is per source, so a
   uniform swizzle is required. PICA hardware supports a selector per output
   channel; widening the descriptor is the main route to the 5 non-uniform
   chunks and the `.xzyw` menu signature (the latter is already covered by
   the pre-specialised plan).
3. **Literal range rewrite.** 9,019 chunks are refused because a `def`
   component is outside `[0,1]`. A subtract/1-minus formulation covers some
   negative constants, but the census shows most of those chunks also use
   `cmp`/loops/`rcp`, so this alone does not unlock them.
4. **Multiple `oC0` writes.** The two chunks where the C translator loses an
   earlier alpha write need an explicit channel-carry plan before either
   planner may accept them.
5. **>7 varying VS.** 33 hashes / 97 chunks declare 8-9 varyings. PICA VS has
   7 varying outputs; these need a plan that packs or drops unused varyings,
   or a multi-pass route.
6. **Draw integration.** `CreatePixelShader`/`CreateVertexShader` do not
   consult `d3d9_shader_plan_lookup` yet. The table is intentionally not on
   the draw path until a plan is certified equivalent.
7. **A rendered first menu frame.** Not claimed by this layer.

The compiler is a coverage tool, not a capability flag: no `GetDeviceCaps`
field, no SM3 advertisement and no silent fallback was added.
