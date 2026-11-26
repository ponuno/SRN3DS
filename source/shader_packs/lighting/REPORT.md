# Lighting pack, phase 2: real forms and the base-lit recipes

Status: **host-only. No `DevCaps` change, no new draw slot, no rendered frame
is claimed.** `DevCaps` is still `0x00082000`, VS/PS 2.0, no SM3 slots. The
pack now has two verified TEV recipes in `plans.json`; both are `translated`
(`draw_enabled == 0`, no `runtime` flag, no entry in the 16-slot runtime table).

Reproduce:

```powershell
python source\shader_packs\lighting\classify.py
python source\shader_packs\lighting\lighting_plans.py
python source\shader_packs\lighting\test_lighting_plans.py
python tools\shader_packs.py coverage
python tools\shader_packs.py check
python tools\shaders\plan_verify.py --data "E:\SteamLibrary\steamapps\common\Skyrim\Data"
```

## 1. What the pack actually contains

The phase-1 class `def_literal_out_of_range` (4,804 hashes) is the *first*
refusal of one planner, not a shape. Grouped by bytecode and CTAB semantics the
4,991 hashes / 7,459 chunks are:

| form | hashes | chunks | meaning |
|---|---:|---:|---|
| `lit_subsurface` | 1,320 | 1,920 | SubSurfaceSampler (skin/SSS permutation) |
| `lit_backlight` | 720 | 1,120 | BackLightMaskSampler (flag permutation) |
| `lit_projected` | 662 | 962 | ProjectedUV/Noise (deferred/decal pass) |
| `lit_glow_sparkle` | 720 | 960 | Glow/Sparkle particles |
| `lit_parallax` | 600 | 800 | MultiLayerParallax material |
| `lit_pointlight` | 262 | 448 | no SM3 loop, but `rcp`/`rsq` attenuation |
| `lit_envmap` | 240 | 400 | Env/EnvMask |
| `lit_specular` | 300 | 400 | SpecularColor/SpecularSampler |
| `vs_pica_contract` | 130 | 333 | VS fits the PICA envelope, no emitter |
| `vs_too_many_varyings` | 22 | 52 | 8–9 varyings vs PICA's 7 |
| `lit_other` | 9 | 33 | remaining one-off pixel shaders |
| `lit_shadow` | 3 | 20 | shadow mask only, no loop |
| `base_lit` | **2** | **6** | p5 MAD + p1200005 ADD, no shadow |
| `base_lit_shadow` | **1** | **5** | p5 + one projected shadow mask |

Feature overlap (a hash can have several): shadow 4,160 hashes / 5,916 chunks;
point light 4,640 / 6,090; specular 2,580 / 3,440; subsurface 2,700 / 4,000;
envmap 1,740 / 2,480; backlight 1,200 / 1,760; projected 902 / 1,282;
parallax 600 / 800; glow/sparkle 720 / 960. Control flow (`rep`/`ifc`/`if`) is
present in **2,808 PS hashes / 3,300 chunks**; `rcp`+`rsq`+`cmp` in 2,808 /
3,300. The corpus is a procedural light loop, not a pile of base-lit shaders.
`coverage.json` carries the full per-form hash examples.

## 2. Translated subset (10 chunks, 2 hashes)

| hash | chunks | recipe | stages | constants |
|---|---:|---|---:|---|
| `8699c0204453a1dd2189a7252766f2a9591b296370467869b106e049576ea9f1` | 5 | `lighting.base_lit.ps` | 6 | c3,c4,c5,c7 |
| `a9942b49966b60249d8a9aeb4e049d1976086fe7b9293168f70c25aeb8477642` | 5 | `lighting.base_lit_shadow.ps` | 5 | c3,c4,c5 |

Both are `source/shader_packs/lighting/plans.json`; `draw_enabled` is 0 and
`runtime` is omitted, so `fxp_compile manifest` does **not** copy them into the
shared 16-slot table. `shader_packs` coverage does count them:
lighting is now 4 translated / 0 draw-enabled / 4,987 refused (was 2 / 0 /
4,989). The two new hashes are `p5.fxp`; `p5` also has 58 other PS permutations
that use point lights/shadow/specular and are *not* accepted.

`base_lit` recipe (six stages, exact under the contract below):

```text
0  DOT3_RGBA  TEX1 . c3                       -> NdotL
1  MAD        PREV . c4 + v2                  -> light
2  MODULATE   TEX0 . PREV ; a = TEX0.a*v1.a
3  MODULATE   PREV . v1     ; a *= c5.z
4  INTERPOLATE PREV, v2, v2.a                 -> fog lerp
5  MODULATE   PREV . c7.a
```

`base_lit_shadow` inserts the projected shadow mask without exceeding six
stages by carrying NdotL in the alpha channel:

```text
0  DOT3_RGBA   TEX1 . c3                      -> NdotL (rgb+a)
1  MODULATE    TEX2.r * c4 ; a = PREV.a       -> shadow*light, keep NdotL
2  MAD         PREV.rgb * PREV.a + v2         -> shadow*light*NdotL + ambient
3  MODULATE    TEX0 * PREV ; a = TEX0.a*v1.a
4  MODULATE    PREV * v1    ; a *= c5.z
```

The **exact** shadow+fog form needs seven stages (shadow, light colour, NdotL,
diffuse, vertex, fog lerp, fog scale), which is one over PICA's six. The recipe
therefore declares `fog_off_secondary_zero` (v2.w = 0, final c8.w = 1) and says
so in the note; the seven-stage form is a named refusal, not an approximation
silently accepted.

## 3. Material contract

Both plans are equivalent only under these preconditions (also stored in the
plan entries):

* `normal_prebaked` — `NormalSampler` stores the decoded tangent normal; the
  retail `2*n-1` (p5) or `(n-0.5)*2` (p1200005) decode is folded into the asset.
* `tangent_basis_prebaked` — the c0..c2 `dp4` basis rows are prebaked into that
  map or the vertex stream.
* `ambient_emissive_varying` — the secondary colour v2 carries
  `basis * normal + c6` (the C menu-light evaluator's `secondary_color`).
* `shadow_uv_projected` — the shadow sampler reads a pre-projected UV varying;
  retail derives it from `vPos` + c7, which TEV cannot compute.
* `fog_off_secondary_zero` — shadow recipe only, as above.
* `material_constants_c0_c7`, `ldr_quality`, `texture0_2d`, `texture1_2d`,
  `vertex_color`.

`lighting_plans.verify()` interprets the contracted IR instruction by
instruction (skipping only the normal decode and basis rows), substitutes v2
for the basis+c6 add, and compares the final `oC0` against the TEV stage
evaluator on 256 deterministic samples under the `ldr_quality` premise
(non-negative LDR colours, light colour ≤ 0.5). Maximum absolute error is
`1.11e-16` (floating-point zero) for both recipes. The test also flips the
ambient addend and asserts the verification fails, so the contract is doing
real work.

## 4. Tests run

| Test | Result |
|---|---|
| `source/shader_packs/lighting/test_lighting_plans.py` (6) | PASS |
| `tools/shaders/plan_verify.py` | PASS, 4 runtime + 2 pack plans, 0 schema errors |
| `tools/shaders/plan_verify.py --data E:\SteamLibrary\...\Data` | PASS, symbolic `pass`, 0 problems |
| `tools/shader_packs.py check` | PASS, 10,475 chunks / 6,659 hashes |
| `shader_plan_table` | PASS, 85 checks |
| `tev_text_plan` | PASS |
| `tev_real` | PASS, 9,621 chunks; `p5_MAD=5 p1200005_ADD=1` |
| `menu_light` | PASS |
| Full host suite | **143 passed, 0 diagnostic, 4 environment-gap failures** (see below) |
| ARM compile-only (`d3d9_tev.c`, `d3d9_core.c`) | rc 0, no new warnings; no C file edited |

The four full-suite failures are environment prerequisites, not pack changes:
`tests/slice_pe_test.py` needs `capstone`; `tests/texture_pipeline_test.py`
needs `C:\devkitPro\tools\bin\tex3ds.exe`; `tests/ir_container_test.py` and
`tests/ir_container_growth_test.py` need `C:\Program Files\LLVM\bin\clang.exe`.
The shader, plan-table, TEV and Python plan tests all pass. The fresh worktree
also needed `staged/` copied from the main tree (git-ignored), exactly as
`docs/SM3_PHASE1.md` §8 says.
`tev_real` independently reports five p5 chunks matched by
`d9_menu_light_match()` (`p5_MAD=5`); the offline planner now reproduces that
same shader as a checked plan. The runtime matcher still emits the older
five-stage plan, so this recipe is a table/catalog translation until the
runtime is wired to it (section 5).

## 5. Next blocker and shared-fix request

1. **Runtime route for the six-stage base-lit plan.** The C authority
   `d9_menu_light_match()` already accepts the p5 hash and
   `d9_menu_light_plan()` emits five stages (it folds material alpha to c4.a and
   drops the fog lerp). The pack plan is the six-stage form. To draw it, either
   route the hash through the plan table (the 16-slot table is append-only and
   currently has room) or update `d9_menu_light_plan()` to the six-stage form.
   That is a shared C change, not a pack edit.
2. **Shadow+fog is seven stages.** PICA has six. The options are a two-pass
   plan, baking the fog factor into the vertex/fog colour, or keeping the
   fog-off contract. Not attempted here.
3. **Point-light permutations.** 4,640 hashes reference point lights and
   2,808 hashes contain SM3 control flow; the dominant per-light sequence is
   `rcp`/`rsq` attenuation + `cmp` shadow. TEV has no reciprocal, and PICA has
   one accumulator, so this needs the expression-DAG scheduler
   (`docs/PICA_RESEARCH.md` §8.3) plus a canonical loop-unroll/binding contract.
   This is the real blocker for the remaining 4,987 lighting hashes.
4. **VS emitter.** 130 VS hashes fit the PICA envelope but have no emitter; 22
   declare 8–9 varyings against the 7 limit and need packing/multi-pass.
5. **Literal range.** The two accepted hashes were rejected first by
   `def_literal_out_of_range` (c8/c9 = `{2,-1,1,0}`). The recipes fold that
   decode into the normal asset. A generic literal-range rewrite is *not* the
   lever for the rest: every remaining class also hits control flow or
   `rcp`/`rsq`/`cmp`.

No core/TEV file, `docs/shader_plans.json` entry or runtime slot was changed.
