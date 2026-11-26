# PICA200 / TEV / SM3 research: what to reuse, what to avoid

Status: **read-only research pass**. No renderer behaviour changed by this
document. The worktree is `pica-research` at `f136eb0`; all statements about
other projects are tied to the commits listed in section 10. Game bytes are not
stored in this repository.

Scope: the existing D3D9 → PICA200 layer (`source/d3d9_tev.c`,
`d3d9_pica_tev.c`, `d3d9_pica_citro3d.c`, `d3d9_citro3d.c`), the offline
compiler/census (`tools/shaders/`, `docs/SHADER_COMPILER.md`), the pre-baked
plan table (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`), and the PICA VS files
(`source/d9shader*.v.pica`).

The short version:

1. **A8 is decoded as `(0,0,0,A)` on PICA200 and on D3D9.** The live core
   menu-text plan (`core_text_plan()` in `d3d9_core.c`) already uses the correct
   Jazz² `Silhouette` shape: RGB `REPLACE(PRIMARY)`, alpha
   `MODULATE(TEXTURE0.a, PRIMARY.a)`. The **stale duplicate**
   `d9_tev_text_plan()` builder and its tests still encode
   `MODULATE(TEXTURE0, PRIMARY)` for RGB and claim to mirror the core. That
   contract drift is the first thing to fix. The synthetic stage state in
   `d3d9_menu_text.c` also says `MODULATE`, which is colour-destroying for A8
   on real D3D9; the live core special-cases it instead of using the generic
   `SELECTARG2` path.
2. **MojoShader can be the canonical offline IR for all 10,475 chunks.** It
   parsed 9,621/9,621 PS and 854/854 VS chunks with zero errors in this
   worktree; its `d3d` profile already prints operands, swizzles, write masks,
   source modifiers and relative addressing. Keep it offline (zlib, host-only),
   not in the 3DS binary.
3. **PICA TEV has per-source selectors, not per-output-channel selectors and no
   swap table.** `D9TevStage` already exposes the full selector space. Widening
   it will not unlock `.xzy`/`.xxy` shaders. The route for those is a
   pre-swizzled asset or a pre-baked plan, exactly as Jazz² pre-swizzles its
   normal map and tevsl needs the GX swap table that PICA does not have.
4. **The missing TEV piece is a scheduler, not a wider descriptor.** PICA has
   six stages and three sources per stage, but only one computed value
   (`PREVIOUS`) is readable. An expression DAG with more than one live computed
   value must be recomputed or refused. tevsl's canonical `(accum ± mix(a,b,c)) * scale` form and our
   existing symbolic verifier are the right starting points.
5. **SwiftShader is a semantics oracle, not a runtime.** Its `alphaTest` and
   `TEXKILL` are small and exact; use them to generate golden tests for the
   cases we lower and the cases we refuse. Do not port its JIT.

## 1. The layers in this repository

| Layer | Files | Current state |
|---|---|---|
| Offline SM3 decode/classify/plan | `tools/shaders/asm.py`, `compile.py`, `packages.py`, `tools/fxp_census.py` | 10,475 chunks, 6,655 distinct instruction streams; 15 PS hashes generate a plan, 276 VS hashes fit a contract but have no emitter |
| Runtime translator | `source/d3d9_tev.c/.h` | straight-line subset, 19 of 9,621 PS chunks |
| Backend gate/descriptor | `source/d3d9_pica_tev.c/.h` | 1..6 stages, 0..2 textures, literal/external constants, named rejections |
| citro3d apply | `source/d3d9_pica_citro3d.c` | re-applies six `C3D_TexEnv` stages per draw; no state cache |
| citro3d textures/draw | `source/d3d9_citro3d.c` | A8 via `C3D_TexLoadImage`, RGBA8/RGB8, cube, bounded targets |
| Plan table | `docs/shader_plans.json`, `source/d3d9_shader_plan_table.inc` | exact SHA-256 lookup; disabled plans carry a reason |
| Vertex shaders | `source/d9shader*.v.pica` | pass-through + clip transform; `MaxVertexShaderConst = 4` |
| Tests | `tests/tev_*`, `tests/isolated/*` | host + ARM/Azahar state and synthetic pixel proofs |

## 2. Jazz² Resurrection (GPL-3.0): the fixed-function table architecture

Repository: <https://github.com/deathkiller/jazz2-native>, commit
`99ece0ea4a08` (2026-11-23). Relevant files:

* `Sources/nCine/Graphics/RHI/FixedFunctionPass.h`
* `Sources/nCine/Graphics/RHI/PICA/PicaDevice.cpp`
* `Sources/nCine/Graphics/RHI/PICA/PicaTexture.cpp`
* `Sources/Shaders/Generated/PicaGeneratedEffects.h`
* `Sources/Utilities/ShaderCompiler/ConsoleFixedFunction.h`

### 2.1 What it actually solves

Jazz² **does not translate SM3 to TEV**. It has a small authored
`fixed_function([targets]) { ... }` block in each `.shader` file, and
`ShaderCompiler --emit-fixed-function pica` transpiles that block into a C++
function `void <Program>_Effect(EffectContext&)`. The function builds
`FixedFunctionPass` descriptors (`Color`, `OffsetColor`, `Blend`, `Tev`
preset, optional strip geometry) and submits them through the PICA backend's
`EffectContext`. That is a different problem from ours, but the *table and
dispatch architecture* is directly applicable.

### 2.2 The table key and dispatch

Each generated-table entry carries the program name, the variant define, an
`UsesOffsetColor` flag, a statically computed `Requirements` bitmask, an
optional pipeline `Intrinsic`, and the transpiled effect function pointer.

* The key is `(program name, variant define)`, not a bytecode hash. The runtime
  calls `PicaShaderProgram::SetProgramIdentity(program, variant)` at link time
  and resolves the entry once.
* Byte-identical function bodies are emitted once and shared by every row.
* `Requirements` is a statically computed bitmask (`NeedsTexelStep`,
  `NeedsUniforms`, `NeedsStripBuilder`, `NeedsQuadAxes`, `SamplesTexture`) so
  the per-draw dispatch can skip setup the effect can never touch.
* A program absent from the table is skipped with a one-time warning; it is
  never "approximated" by a nearby effect.

Our equivalent already exists (`d9_shader_plan_lookup(stage, sha256)`), but it
has one dimension (hash). A useful addition is a **variant dimension** for
material state that changes the plan without changing bytecode: alpha test
on/off, vertex colour present/absent, A8 vs RGBA texture, fog on/off. That
avoids multiplying hashes and makes the "same shader, different state" case
explicit.

### 2.3 TEV presets the PICA backend implements

`PicaDevice.cpp` `ApplyTexEnv()` is the concrete C3D code:

| Preset | RGB | Alpha |
|---|---|---|
| `Untextured` | `REPLACE(PRIMARY)` | `REPLACE(PRIMARY)` |
| `Modulate` | `TEXTURE0 * PRIMARY` | `TEXTURE0 * PRIMARY` |
| `ModulateX2/X4` | same, RGB scale 2/4 | same |
| `Silhouette` | `REPLACE(PRIMARY)` | `TEXTURE0.a * PRIMARY.a` |
| `TintMix` | `INTERPOLATE(PRIMARY, TEXTURE0, PRIMARY.a)` | `REPLACE(CONSTANT)` |

`Silhouette` is exactly the A8 text/mask plan we need. `TintMix` is a useful
example of using the third source operand's selector as the interpolation
weight (`GPU_TEVOP_RGB_SRC_ALPHA`), which our descriptor can already express.

### 2.4 Texture tiling and A8

`PicaTexture.cpp` is a complete, readable PICA texture-store implementation:

* 8×8 tiles in Morton order: `MortonOffset(x,y)` interleaves the three low bits
  of `x` and `y`, `x` in the even positions (`TileBand16`).
* Bottom-up: texture coordinate `v = 0` is the **last** row in memory. The
  source's first row goes into the last memory row; padding rows go first.
* Power-of-two page sizes, at least 8×8, with the page split at
  `MaxPageDimension` (1024).
* 128-byte alignment for the GPU store.
* RGBA8 is converted to RGBA4 for the store; RGB8/RGBA8 with alpha uses
  `Rgba4FromRgba(...)`, and alpha-only content is baked as colour + alpha.

We already use `C3D_TexLoadImage`, which performs the same tiling for A8, so
we should **not** hand-roll a second tiler. The useful part is the *algorithm*
if we ever need a sub-rectangle upload or a CPU-side A8 atlas: the same
`tile = ((y>>3)*paddedWidth/8 + (x>>3))*64` byte offset, Morton offset inside
the tile, and bottom-up `y` flip. NovaGL has the same formula independently
(section 5), which is a good cross-check.

### 2.5 What not to take

Jazz² is **GPL-3.0**. Do not copy the transpiler, the generated effect
functions, the `EffectContext` implementation, or the PICA device code into
this repository. The architecture (table key, requirements mask, preset names,
tiling algorithm) is usable as an idea; code is not. There is no patent or
license grant that lets GPL code be linked into this port.

## 3. MojoShader (zlib): the canonical offline IR for 10,475 chunks

Repository: <https://github.com/icculus/mojoshader>, commit `ad5dff84830c`
(2026-09-03). Relevant files:

* `mojoshader.c`, `mojoshader.h`, `mojoshader_internal.h`
* `profiles/mojoshader_profile_d3d.c`
* `profiles/mojoshader_profile.h`

### 3.1 Measured result

In this worktree, a host-only dumper was built from the zlib sources with
Clang and run over the project's chunk dumps:

| Input | Chunks | Parsed OK | Parse failures | Instructions |
|---|---:|---:|---:|---:|
| `tev_chunks.bin` (PS only, base archive) | 9,621 | **9,621** | **0** | 987,684 |
| `vs_chunks.bin` (VS, base archive, extracted read-only) | 854 | **854** | **0** | 32,185 |
| **Total** | **10,475** | **10,475** | **0** | **1,019,869** |

The VS dump was produced by slicing the `BSVS` payloads with the existing
`tools/skyrim/shaders.py` parser; only derived counts are kept. This is
parser coverage, not translation coverage: it says MojoShader understands
every retail token stream, including the 320 VS chunks with relative-addressed
constants that broke our old walker.

### 3.2 Why the `d3d` profile is already an IR

`MOJOSHADER_PROFILE_D3D` prints a canonical disassembly with:

* explicit destination write masks (`mov r1.xyz, ...`, `mul r2.zw, ...`);
* explicit source swizzles, including replicated components and `_abs`;
* source modifiers (`-`, `_sat`, `_abs`);
* relative addressing (`c10[a0.x]`, `c0[aL+2]`) because
  `SourceArgInfo` carries `relative`, `relative_regtype`, `relative_regnum`,
  `relative_component` and `relative_array`;
* `def`/`defi`/`defb` literals and `dcl_*` declarations;
* SM3 control flow (`if_lt`, `if_ge`, `else`, `endif`, `loop`, `rep`,
  `breakc`) and SM3 texture ops (`texldl`, `texldd`).

A sample retail chunk (3,744 bytes) prints 252 instructions
with `texldl`, live temporaries `r0..r9`, `if_lt` nesting, `rcp`, `cmp`,
`mad` and `_sat` writes. That is a much better planner input than a token walk, and
it explains why the current single-instruction-per-stage planner refuses it.

### 3.3 Recommended integration

1. Vendor the zlib sources under `tools/mojoshader/` (offline only). Add a
   `THIRD_PARTY_NOTICES` entry; do not link it into the 3DS runtime.
2. Build `tools/sm3_dump` (host) with only `SUPPORT_PROFILE_D3D=1`; the other
   profiles are not needed. The measured build used
   `-DMOJOSHADER_NO_VERSION_INCLUDE=1` and all other `SUPPORT_PROFILE_*` off.
3. Read the existing chunk-dump format (`u32 count; count * {u32 size; bytes}`)
   and emit one canonical line per instruction, or keep the `d3d` text and
   parse it. A custom profile that dumps `ctx->dest_arg` and
   `ctx->source_args[]` directly would avoid a second text parser, but the
   `d3d` profile is zero-maintenance and already covers the fields above.
4. Run a **field-level differential** against `tools/shaders/asm.py` over the
   6,655 distinct instruction streams: opcode, destination register/mask,
   saturate bit, each source register/index/swizzle/modifier, relative
   address, and instruction length. Do not compare raw
   `instruction_count`: MojoShader and our parser count `dcl`/`def`
   differently. In this worktree, a naive count comparison reported 8,921 of
   9,621 PS chunks as different for that reason alone, with no parse error on
   either side. Compare the decoded fields instead.
5. Store the normalized IR by SHA-256 in a generated, git-ignored cache. The
   planner consumes the IR; the runtime keeps only the accepted plan table.

MojoShader is zlib-licensed: keep the copyright notice, do not claim you wrote
it, and mark modified profile files. That is compatible with this project.

## 4. SwiftShader `legacy-d3d9` (Apache-2.0): SM3 and alpha/texkill semantics

Repository: <https://github.com/google/swiftshader>, branch `legacy-d3d9`,
commit `8bf6acb05387` (2022-06-20). Relevant files:

* `src/Shader/PixelRoutine.cpp` — per-fragment routines, alpha test
* `src/Shader/PixelProgram.cpp` — per-opcode pixel program, `TEXKILL`
* `src/Shader/Shader.cpp` — token parser and operand decoder
* `src/Shader/SamplerCore.cpp` — A8 decode is `(0,0,0,A)`

### 4.1 Alpha test

`PixelRoutine::alphaTest` implements all eight D3D9 compare modes on the
8-bit alpha value (`ALPHA_ALWAYS`, `NEVER`, `EQUAL`, `NOTEQUAL`, `LESS`,
`GREATEREQUAL`, `LESSEQUAL`, `GREATER`). The exact predicates are a good
golden table for `tests/tev_accuracy_smoke.c` and for the PICA
`C3D_AlphaTest` mapping.

### 4.2 TEXKILL

`PixelProgram::TEXKILL` is only a few lines:

```text
kill = all ones
for each masked component c:
    kill &= (src.c >= 0)   // i.e. kill the pixel if src.c < 0
cMask &= kill
```

The mask is the instruction's destination write mask; only selected components
participate. This is the exact predicate to use in a pattern matcher. It also
shows why arbitrary `texkill` is not lowerable to PICA alpha test: PICA's TEV
clamps to `[0,1]` before the alpha test, so a negative test cannot be
represented after a stage. Only a threshold/cutout form can be lowered.

### 4.3 A8 decode

`SamplerCore.cpp` `FORMAT_A8` sets `c.w = c.x; c.x = c.y = c.z = 0`. This is
independent confirmation that D3D9 A8 is `(0,0,0,A)`, not white RGB. The
PICA200 emulator Azahar does the same
(`src/video_core/texture/texture_decode.cpp`, `TextureFormat::A8` returns
`{0, 0, 0, *source_ptr}`). That matters for section 6.1.

### 4.4 How to use it without porting it

Do **not** port SwiftShader's Reactor/JIT or its software rasterizer. Use it
as an offline oracle:

* copy the eight alpha-test predicates and the TEXKILL predicate into small,
  independent host tests (Apache-2.0 notice if code is copied);
* generate golden vectors for `mad`, `lrp`, `dp3`, `dp4`, `cmp`, `if_lt`,
  `if_ge`, `texldl` (explicit LOD), `texldd` (derivatives), `saturate`,
  `abs`, `neg`, partial `oC0` writes;
* compare our scalar evaluator and the TEV plan against those vectors before
  enabling a plan.

The cleanest licensing route is to write the tests from the D3D9 specification
and use SwiftShader only during development to confirm the expected values;
then no Apache-2.0 code needs to enter the repository.

## 5. NovaGL (MIT claimed): GLES1.1 → citro3d TEV compiler

Repository: <https://github.com/efimandreev0/NovaGL>, commit `eff4fb353df8`
(2026-08-04). Relevant files:

* `src/texture.c` — `nova_update_fast_tev`, A8 handling, Morton upload
* `src/utils.c` — `apply_gpu_state`, A8 TEV override, alpha test
* `README.md` — states MIT, but the repository has **no `LICENSE` file**

### 5.1 The fast TEV compiler

`nova_update_fast_tev(unit)` compiles GL `GL_COMBINE` state into packed PICA
masks once per state change:

* `src_rgb[3]` / `src_a[3]` (with `0xFF` as a "previous" marker),
* `op` = three 4-bit RGB selectors + three 4-bit alpha selectors,
* `func` = RGB function in the low 16 bits, alpha in the high 16,
* `scale` = RGB scale in the low nibble, alpha in the high nibble,
* `uses_const`.

`apply_gpu_state` writes the fields directly into `C3D_TexEnv`
(`env->srcRgb`, `env->srcAlpha`, `env->opAll`, `env->funcRgb`,
`env->funcAlpha`, `env->scaleRgb`, `env->scaleAlpha`, `env->color`) and calls
`C3D_DirtyTexEnv(env)`. That is the same descriptor our `D9PicaTevStage`
already has, but with a much cheaper apply path than six
`C3D_TexEnvSrc/Op/Func/Scale` calls per stage. It also caches the last applied
TEV state and only re-emits when the state key changes. The fields are public
in the current citro3d header but are not a stable ABI; gate the direct writes
on a citro3d version check or keep the API path as a fallback.

### 5.2 The A8 fix

The comment in `src/context.h` is explicit:

> PICA samples A8 RGB as 0, so `apply_gpu_state` rewrites the RGB half of the
> unit's TEV stage to `REPLACE(previous)` whenever this is set.

The override in `src/utils.c`, when the slot is alpha-only and the mode is not
`GL_COMBINE`, sets all three RGB sources to the previous colour, sets the RGB
function to `REPLACE`, and clears the RGB operand selectors. This is the same
`Silhouette` form as Jazz² and the same fix our menu text needs.

### 5.3 Tiling

NovaGL's `morton_offset_local(x, y, pot_w, pot_h)` first flips `y` bottom-up
(`fy = pot_h - 1 - y`), then computes the byte offset as the 8×8 tile index
(`(fy >> 3) * (pot_w >> 3) + (x >> 3)`) times 64 plus the Morton interleave of
`(x & 7, fy & 7)`, with a precomputed 64-entry Morton LUT and 8×8 upload loops.
This is the same layout as Jazz² and Azahar's `MortonInterleave`. Use it as a
cross-check, not as a second tiler.

### 5.4 License risk

The README says MIT, but there is no `LICENSE` file and the source files carry
no copyright header. Treat the code as **not safely reusable** until the
author adds a license file. The algorithms (Morton order, bottom-up, alpha-only
TEV override, field packing) are standard and independently documented; use
those ideas, not copied code.

## 6. picasso / nihstro (MIT / BSD-3): PICA VS and SHBIN

* picasso: <https://github.com/devkitPro/picasso>, commit `d522455ea59c`
  (2023-10-05), MIT.
* nihstro: <https://github.com/neobrain/nihstro>, commit `a969b1a6849b`
  (2026-03-18), BSD-3-Clause.

### 6.1 Register model and constraints

picasso's manual documents the PICA VS register file:

* `v0..v15` inputs, `r0..r15` scratch, `o0..o15` outputs (`o7..o15` VS-only),
* `c0..c95` float uniforms, `i0..i3` integer uniforms, `b0..b15` booleans,
* source operands support swizzles and static/dynamic indexing;
  destination operands support write masks but no reordering;
* uniform space is **shared across VS files** in one SHBIN; `.fvec` allocates
  sequentially;
* errata: only one input register may be referenced reliably at a time in the
  source operands of an instruction; `mova` is finicky (two consecutive
  `mova` freeze the GPU); padding NOPs are needed at some control-flow ends.

nihstro's instruction-set notes add the arithmetic constraints that matter for
codegen:

* `add`/`mul`: at most one source may be a float uniform and/or use dynamic
  indexing;
* `mad`: at most two sources may be float uniforms; no dynamic indexing at all;
* source swizzles are free; destination masks cannot reorder/duplicate.

These constraints, not the instruction mnemonics, are what will drive a VS
register allocator. A naive `mad` with two uniforms and a dynamic index is
illegal and must be split.

### 6.2 SHBIN

nihstro's `include/nihstro/shader_binary.h` defines `DVLBHeader`, `DVLPHeader`,
`DVLEHeader`, `OutputRegisterInfo`, `UniformInfo` and the symbol tables. It is
a clean reference for a host-side SHBIN writer/validator. For this port, the
lower-risk route is still:

1. emit picasso assembly text from the plan,
2. assemble it at build time with the existing picasso toolchain,
3. validate the resulting SHBIN on the host with a small nihstro-derived
   parser or the existing `bin2s`/DVLB path.

Runtime SHBIN generation on ARM is not recommended: it adds a large writer and
a new failure surface to the 3DS binary for no runtime benefit.

### 6.3 Uniform remapping

For generated VS plans:

* keep `c0..c3` reserved for the clip matrix (current convention),
* allocate each D3D `c#` used by the plan to a fixed PICA `c#` in first-use
  order, and store the `d3d_reg -> pica_reg` map in the plan,
* upload with `C3D_FVUnifWrite` by PICA index, not by DVLE name lookup,
* keep `MaxVertexShaderConst = 4` until the emitter exists; afterwards raise
  it to the plan's maximum (up to 96) and update `GetDeviceCaps` and the tests
  in the same change.

This is the only way to make `SetVertexShaderConstantF` cheap: the backend
writes only the live constants at their mapped indices instead of uploading a
256-register bank.

## 7. tevsl (no license): TEV expression and scheduler prior art

Repository: <https://github.com/crtc-demos/tevsl>, commit `5f228ac0f93d`
(2013-01-27). **No license file and no copyright header**; do not copy code.
The README is still valuable as prior art because GX TEV and PICA TEV have the
same combiner shape.

The canonical form is an accumulator plus an optional lerp term plus a bias,
multiplied by a scale: `out = (accum ± lerp(a,b,c) + bias) * scale`, where the
lerp is `(1-c)*a + c*b`. It has:

* `accum` = `PREVIOUS` (or zero),
* `a`, `b`, `c` = texture/raster/constant channels,
* per-channel selectors (`.rgb`, `.a`, `.rrr`, `.ggg`, etc.),
* `bias` in `{-0.5, 0, +0.5}`,
* `scale` in `{1, 2, 4}`,
* simplified forms (`tev + chan0`, `tev * chan1`, `mix(a,b,t)`, `clamp(...)`)
  rewritten into the canonical form.

The GX-only parts that do **not** transfer to PICA:

* swap tables (arbitrary per-channel remapping of a source),
* indirect texture lookups (PICA has no indirect stage),
* the `s10` accumulation annotations and the GX-specific constant registers.

The transferable parts are the canonical expression, the simplification
matcher, and the idea that the scheduler should be a **tree matcher with a cost
model**, not a token-to-stage loop.

## 8. Cross-project findings

### 8.1 A8: the stale menu-text builder is colour-destroying

Evidence:

* Azahar `texture_decode.cpp` decodes `TextureFormat::A8` as `{0,0,0,A}`.
* SwiftShader `SamplerCore.cpp` `FORMAT_A8` does the same.
* Microsoft's `D3DFORMAT` documentation says A8 is "initialized to 000 for the
  three color channels"
  (<https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dformat>).
* NovaGL's `TexSlot.alpha_only` comment and override exist precisely because
  "PICA samples A8 RGB as 0".
* Jazz² uses `Silhouette` (RGB `REPLACE(PRIMARY)`, alpha
  `TEXTURE0.a * PRIMARY.a`) for flat masks, shadows and glows.

What the code actually does today:

* `core_text_plan()` in `d3d9_core.c` emits the **correct** plan for the live
  menu route:
  RGB `REPLACE(PRIMARY)`, alpha `MODULATE(TEX0.a, PRIMARY.a)`. Its comment
  explicitly says "the menu atlas is A8: it carries coverage in alpha only, so
  its RGB is zero". The live menu route uses this function.
* `d9_tev_text_plan()` in `d3d9_tev.c` still emits `MODULATE(TEX0, PRIMARY)`
  for both channels, and its header comment/test still describe that shape.
  The core comment says it "mirrors the certified `d9_tev_text_plan()` shape
  exactly"; that is no longer true. This is **contract drift**, not a live
  black-text bug: the duplicate builder and `tests/tev_text_plan_smoke.c`
  would reintroduce the wrong plan if a new caller used them.
* `source/d3d9_menu_text.c` still sets `COLOROP = MODULATE`,
  `COLORARG1 = TEXTURE`, `COLORARG2 = DIFFUSE`. On real D3D9, A8 RGB = 0, so
  that state would produce black text. The core's `cand` gate accepts exactly
  this state and substitutes `core_text_plan()`; it does not use the generic
  `d9_color_tev_stage0()` path. The synthetic state and the live plan therefore
  disagree about what the guest asked for.

The correct single-stage plan is:

| Channel | Function | Sources | Selectors |
|---|---|---|---|
| RGB | `REPLACE` | `PRIMARY` | `SRC_COLOR` |
| Alpha | `MODULATE` | `TEXTURE0`, `PRIMARY` | `SRC_ALPHA`, `SRC_ALPHA` |

That is one stage, one 2D sampler, no constants, no second texture. Jazz²'s
`Silhouette` preset is exactly this shape.

Recommended fix:

1. Make `d9_tev_text_plan()` emit the table above (or delete it and expose one
   shared builder used by both core and tests). Update its comment and
   `tests/tev_text_plan_smoke.c`.
2. Change the synthetic menu-text stage state to `COLOROP = SELECTARG2` with
   `COLORARG2 = DIFFUSE`, `ALPHAOP = MODULATE` with `ALPHAARG1 = TEXTURE`,
   `ALPHAARG2 = DIFFUSE`. Update the core `cand` gate in the same change to
   accept that shape (or route the A8 case through the generic
   `d9_color_tev_stage0()` path), otherwise the draw falls through to the
   non-menu fixed-function route.
3. Add a pixel test with a **non-white** vertex colour on an A8 atlas. If the
   live core route already renders coloured text, the test documents it; if it
   renders black, the special-case gate or the plan is still wrong.
   `docs/MENU_SWF.md` records that all 57,480 text vertices carry a non-white
   fill colour, so a black result is not the design intent.

### 8.2 Per-channel selectors are not a PICA feature

PICA TEV has three source operands per stage. Each operand has a 4-bit
selector: `SRC_COLOR`, `1-SRC_COLOR`, `SRC_ALPHA`, `1-SRC_ALPHA`, `SRC_R`,
`SRC_G`, `SRC_B` (and 1-minus forms). libctru encodes this as
`GPU_TEVOP_RGB_*` / `GPU_TEVOP_A_*` and packs the three sources with
`GPU_TEVOPERANDS(a,b,c)`; the selected value is a vector or a replicated
scalar. There is **no swap register and no per-output-channel write mask**
(that is a GX TEV feature, which tevsl uses).

Consequences:

* `D9TevStage`/`D9PicaTevStage` already expose the full selector space
  (`src_rgb[3]`, `op_rgb[3]`, `src_a[3]`, `op_a[3]`). There is no descriptor
  field to add for "per-channel selectors".
* An arbitrary non-uniform swizzle such as `.xzy` or `.xxy` cannot be
  synthesized by widening the descriptor. Each TEV stage writes all RGB
  channels together; you cannot assemble `(a.r, b.g, c.b)` from three sources
  in one stage.
* The 5 `non_uniform_swizzle` chunks and the `p1200005` `.xzyw` normal decode
  need one of:
  1. a pre-swizzled texture at asset load (Jazz²'s normal map is
     pre-decoded/pre-swizzled for exactly this reason),
  2. a pre-baked plan that bakes the swizzle into the asset or the vertex
     data,
  3. a named refusal.

This corrects `docs/SHADER_COMPILER.md` section 6 item 2 ("per-channel TEV source
selectors ... the main route to the 5 non-uniform chunks"). The descriptor is
already per-source; the route is asset pre-swizzling or pre-baking, not a wider
descriptor. Update that document in a separate change so no one spends time on
a field that cannot help.

### 8.3 TEV dataflow is a one-accumulator machine

PICA can read three sources per stage but only one computed value:
`PREVIOUS`. Textures, constants and varyings are readable at any stage, so
they do not consume the accumulator. A computed subexpression is either:

* the immediately previous stage's result,
* recomputed in a later stage (costly but legal),
* unavailable (refuse).

This is why the current planner's "temp must be the previous stage" rule is
correct but too weak. A scheduler should:

1. Build a DAG for `oC0.rgb` and `oC0.a` separately.
2. Mark leaves (texture, constant, varying) as always available.
3. Traverse the DAG in an order that keeps at most one computed value live.
4. Fuse nodes into one stage where the PICA combiner has a matching function:
   `MULTIPLY_ADD` (`a*b+c`), `ADD_MULTIPLY` (`(a+b)*c`), `INTERPOLATE`
   (`lrp`), `DOT3_RGB/RGBA` (`dp3`), `SUBTRACT` (`a-b`), `ADD_SIGNED` (`a+b-0.5`),
   and the 1/2/4 scale.
5. Carry RGB and alpha in parallel when their schedules align; otherwise emit
   an alpha-only stage with RGB `REPLACE(PREVIOUS)` (or vice versa).
6. Verify the emitted stage list symbolically against the IR, including the
   LDR clamp after every stage. Reject on any mismatch.
7. Reject if >6 stages or if a node needs two computed values that cannot be
   recomputed within the budget.

Constant range is a scheduling problem, not just a clamp: a `def` component
outside `[0,1]` can sometimes be represented as a subtraction or a scaled
constant. The census already shows that clamping alone unlocks almost nothing
(8,997 of 9,019 still fail on opcodes/dataflow), so do not build the scheduler
around literal rewriting. Build it around opcode coverage and dataflow, and
use the range rewrite as an optional pass.

### 8.4 Alpha test and texkill

* PICA `C3D_AlphaTest` tests the **final** fragment alpha after all TEV
  stages. D3D9 alpha test also tests the final `oC0.a`. A shader that writes
  `oC0.a` and relies on `D3DRS_ALPHATESTENABLE` maps directly; our core already
  forwards the eight compare modes.
* `texkill` is a discard on an arbitrary temp, not on the final alpha. PICA has
  no discard. Lower only the cutout pattern:
  `texkill` on a value that is (or can be routed to) `oC0.a`, with a simple
  threshold, becomes an alpha test. Everything else is a named refusal.
* SwiftShader's `TEXKILL` predicate (masked components, kill if any `< 0`) is
  the oracle for the pattern matcher. Note that TEV clamps to `[0,1]`, so a
  negative test cannot survive a stage; do not pretend it can.

### 8.5 Software fallback

SwiftShader is a full software D3D9 rasterizer, but its value here is the
per-opcode semantics and the token decoder, not the JIT. A scalar SM3
interpreter for tests can be built from the same opcode table and checked
against SwiftShader-generated golden vectors. That gives us an independent
oracle for the plans we accept and, equally important, for the plans we refuse.

## 9. Recommendation table

| Our layer | Foreign component | What to take (algorithm / format / test) | Risk / license |
|---|---|---|---|
| `d9_tev_text_plan`, `d3d9_menu_text.c` | Jazz² `Silhouette`; NovaGL `alpha_only`; Azahar A8 decode | Unify the stale builder with the already-correct `core_text_plan()`: RGB `REPLACE(PRIMARY)`, alpha `MODULATE(TEX.a, PRIMARY.a)`; D3D stage `COLOROP=SELECTARG2`, `ALPHAOP=MODULATE`; non-white pixel test | No code copied; low risk. This is contract cleanup, not a proven live black-text bug |
| `tools/shaders/asm.py`, `compile.py` | MojoShader `d3d` profile + `SourceArgInfo`/`DestArgInfo` | Canonical offline IR; field-level differential over 6,655 streams; keep MojoShader host-only | zlib; keep notice. Medium effort, high confidence |
| `D9TevProgram` planner | tevsl canonical form; our `_stage_result` verifier | Expression DAG → one-accumulator scheduler; fuse `MAD`, `ADD_MULTIPLY`, `INTERPOLATE`, `DOT3`; carry RGB/alpha in parallel; 6-stage budget | tevsl has no license: ideas only. Medium/high effort |
| `D9TevStage` descriptor | PICA TEV operand encoding (libctru enums) | No new field for "per-channel"; use per-source selectors; pre-swizzle assets or pre-bake for `.xzy`/`.xxy` | No license issue; corrects a wrong assumption |
| `d3d9_pica_citro3d.c` apply | NovaGL raw `C3D_TexEnv` writes; Jazz² `ApplyDrawState` cache | Write `srcRgb/srcAlpha/opAll/funcRgb/funcAlpha/scaleRgb/scaleAlpha/color` directly + `C3D_DirtyTexEnv`; cache last plan/state and skip redundant applies | MIT claimed but no LICENSE file: algorithm only, no copy. Low risk |
| `d3d9_citro3d.c` A8/tiling | Jazz² `MortonOffset`/`TileBand16`; NovaGL `morton_offset_local`; Azahar `MortonInterleave` | Keep `C3D_TexLoadImage`; use the Morton + bottom-up + 8×8 formula only for sub-rect/CPU atlas work; cross-check with Azahar | No code copied; low risk |
| Alpha test / `texkill` | SwiftShader `PixelRoutine::alphaTest`, `PixelProgram::TEXKILL` | Eight compare modes; masked `<0` kill predicate; cutout pattern matcher; golden vectors | Apache-2.0 if copied: add NOTICE. Prefer independent tests |
| VS codegen / uniforms | picasso `Manual.md`; nihstro `shader_binary.h`, `instruction_set.md` | Offline picasso asm emission; `c#`→`c#` map; `C3D_FVUnifWrite` by index; enforce one-uniform-per-`add`/`mul`, two-per-`mad`, no dynamic `mad`; avoid `mova` | MIT / BSD-3: attribution if vendored. High effort |
| Plan table / dispatch | Jazz² `(program, variant)` table; `Requirements` bitmask; one-time skip | Add a variant dimension for material state; precompute required textures/varyings/uniforms; skip setup the plan cannot use | GPL-3.0: architecture only. Low risk |
| Software fallback tests | SwiftShader per-opcode semantics | Scalar SM3 oracle for `mad/lrp/dp3/cmp/if/texldl/texkill`; compare TEV plans and refusals | Apache-2.0 if copied. Low/medium effort |

## 10. Priority actions

Estimates are engineer-days for one person familiar with the code; they include
host tests but not a real hardware frame.

### P0 — Unify the A8 menu-text plan and fix the synthetic stage state (0.5–1 day)

`core_text_plan()` already emits the correct shape (RGB `REPLACE(PRIMARY)`,
alpha `MODULATE(TEX0.a, PRIMARY.a)`), but `d9_tev_text_plan()` and
`tests/tev_text_plan_smoke.c` still emit/assert `MODULATE(TEX0, PRIMARY)` and
claim to mirror the core. Make them one plan: either make
`d9_tev_text_plan()` emit the correct table and update the test, or delete the
duplicate and expose one shared builder used by core and tests.

Then change the synthetic `d3d9_menu_text.c` stage state to
`COLOROP=SELECTARG2` / `COLORARG2=DIFFUSE`, `ALPHAOP=MODULATE` /
`ALPHAARG1=TEXTURE` / `ALPHAARG2=DIFFUSE`. The generic
`d9_color_tev_stage0()` already emits the Silhouette shape for `SELECTARG2`, so
the special `cand` gate should be updated in the same change (or removed once
the generic path is selected); otherwise the draw falls through to the non-menu
route. Add a host pixel test and
one ARM/Azahar readback with a **non-white** vertex colour on an A8 atlas;
expect red RGB with coverage alpha, not black. Update `tests/menu_text_*`, the
"Menu text TEV plan" section of `PICA_BACKEND.md`, and the `MODULATE` wording
in `MENU_SWF.md` section 21.

Acceptance: `d9_tev_text_plan()` and `core_text_plan()` are field-for-field
identical; `func_rgb=REPLACE`, `src_rgb[0]=PRIMARY`, `func_a=MODULATE`,
`src_a[0]=TEX0`, `src_a[1]=PRIMARY`; the pixel test distinguishes the old
MODULATE-RGB plan from the new one; the synthetic stage state no longer asks
for a colour-destroying D3D9 operation.

### P1 — MojoShader offline IR and differential census (2–3 days)

Vendor the zlib sources under `tools/mojoshader/`, add `tools/sm3_dump`, and
run it over the existing chunk dumps. Add a Python differential that compares
decoded fields (not instruction counts) against `tools/shaders/asm.py` for all
6,655 distinct streams. Record any divergence in a small allowlist with a
reason.

Acceptance: 10,475/10,475 chunks parse; the differential reports zero
unexplained field differences; the generated cache is reproducible byte for
byte; the 3DS binary does not link MojoShader.

### P1 — TEV expression scheduler v2 (4–6 days)

Replace the one-instruction-per-stage planner with a DAG scheduler:

* legalize the MojoShader IR (straight-line, SSA-like),
* match `MAD`, `ADD_MULTIPLY`, `INTERPOLATE`, `DOT3`, `SUBTRACT`, `ADD_SIGNED`,
  scale 2/4,
* carry RGB and alpha in parallel,
* run the existing symbolic verifier after every candidate,
* keep the 6-stage budget and the named refusal taxonomy.

Measure with `tools/shaders/report.py` and `tests/tev_real_smoke.c`; do not
chase the 9,019 literal-range chunks first.

Acceptance: every newly accepted plan is symbolically equivalent under the LDR
clamp model; the census delta is reported per family; no plan is enabled on the
draw path until its material contract is satisfied.

### P2 — Alpha test / texkill lowering rules and SwiftShader golden tests (1–2 days)

Add a plan flag for alpha-test-from-output and a `texkill` pattern matcher:

* final-alpha cutout → `C3D_AlphaTest` with the equivalent compare mode,
* `texkill` on any other temp/component → named refusal,
* negative-test forms after a TEV clamp → named refusal.

Add golden vectors for the eight alpha-test modes and the TEXKILL predicate;
compare our evaluator and the PICA alpha-test mapping.

Acceptance: the cutout pattern lowers and passes the golden vectors; every
other `texkill` shape is refused by name and covered by a negative test.

### P3 — TEV apply fast path and state cache (1–2 days)

Write the `C3D_TexEnv` fields directly (NovaGL's shape) and cache the last
applied plan plus the texture/format/sampler key. Skip re-applying six stages
when the same plan is used by consecutive draws (the menu batch is one draw,
but the game path will batch).

Acceptance: `uicopy`/`rt_switch` pixels unchanged; the number of
`C3D_TexEnv*` calls per repeated draw drops to zero; no state leak across
`restore_tev()`.

### Deferred — VS codegen via picasso/nihstro (1–2 weeks)

Emit picasso asm per VS plan, assemble offline, validate the SHBIN, and map
`c#` to `c#` with `C3D_FVUnifWrite`. Enforce the picasso/nihstro arithmetic
constraints. Do not raise `MaxVertexShaderConst` until this exists. This is the
largest remaining coverage item (276 VS hashes / 757 chunks, 88.6% of VS
chunks), but it is not on the menu-scene critical path.

## 11. Evidence and reproduction

### 11.1 MojoShader census (host only, game bytes not stored)

The measured run used a throwaway host dumper (`dump_chunks.c`) with the same
API; the command below shows the intended permanent tool. Built with Clang from
the zlib sources:

```powershell
clang -std=c99 -O1 -DMOJOSHADER_NO_VERSION_INCLUDE=1 `
  -DSUPPORT_PROFILE_D3D=1 -DSUPPORT_PROFILE_BYTECODE=0 `
  -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_GLSL=0 `
  -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=0 `
  -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_ARB1=0 `
  -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_METAL=0 `
  -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 `
  -I. -Iprofiles mojoshader.c mojoshader_common.c `
  profiles/mojoshader_profile_common.c profiles/mojoshader_profile_d3d.c `
  tools/sm3_dump.c -o sm3_dump.exe
```

Results in this worktree:

```text
PS: chunks=9621 parsed_ok=9621 parse_fail=0 total_instructions=987684
VS: chunks=854  parsed_ok=854  parse_fail=0 total_instructions=32185
```

The PS input is `E:\Skyrim_N3DS_build\assets\shaders\tev_chunks.bin`
(`u32 count; count * {u32 size; bytes}`). The VS input was produced read-only
from the retail `.fxp` files with `tools/skyrim/shaders.py`; only derived
counts are kept in this repository.

### 11.2 Existing project evidence referenced

* `docs/SHADERS.md` section 6 — translator acceptance per family, 19 PS chunks.
* `docs/SHADER_COMPILER.md` section 2 — 10,475 chunks, 6,659 hashes, 15 PS plan
  hashes / 35 chunks, 276 VS contract hashes / 757 chunks.
* `docs/MENU_SWF.md` section 12 and section 21 — A8 atlas, 57,484 vertices / 86,160
  indices, one indexed draw; the live core plan is the Silhouette shape, while
  the synthetic stage state and the stale builder still say
  `MODULATE(TEXTURE0,PRIMARY)`.
* `docs/PICA_BACKEND.md` section 2 and section 5 — descriptor, A8 upload, TEV apply/restore,
  bounded targets.
* `docs/SHADER_CENSUS.json` — per-class counts.
* `tests/tev_real_smoke.c`, `tests/tev_accuracy_smoke.c`,
  `tests/menu_light_smoke.c`, `tests/isolated/uicopy/` — reference evaluator,
  accuracy and synthetic pixel evidence.

### 11.3 External references

| Project | Commit | License | Key files |
|---|---|---|---|
| Jazz² Resurrection | `99ece0ea4a08` | GPL-3.0 | `Sources/nCine/Graphics/RHI/FixedFunctionPass.h`, `PICA/PicaDevice.cpp`, `PICA/PicaTexture.cpp`, `Sources/Shaders/Generated/PicaGeneratedEffects.h`, `Sources/Utilities/ShaderCompiler/ConsoleFixedFunction.h` |
| MojoShader | `ad5dff84830c` | zlib | `mojoshader.c`, `profiles/mojoshader_profile_d3d.c`, `profiles/mojoshader_profile.h` |
| SwiftShader `legacy-d3d9` | `8bf6acb05387` | Apache-2.0 | `src/Shader/PixelRoutine.cpp`, `src/Shader/PixelProgram.cpp`, `src/Shader/Shader.cpp`, `src/Shader/SamplerCore.cpp` |
| NovaGL | `eff4fb353df8` | MIT claimed, no LICENSE file | `src/texture.c`, `src/utils.c`, `src/context.h` |
| picasso | `d522455ea59c` | MIT | `Manual.md`, `source/picasso_assembler.cpp` |
| nihstro | `a969b1a6849b` | BSD-3-Clause | `docs/instruction_set.md`, `include/nihstro/shader_binary.h` |
| tevsl | `5f228ac0f93d` | none declared | `README.md`, `tevsl.ml` |
| Azahar (A8 decode) | master at time of writing | GPL-3.0 | `src/video_core/texture/texture_decode.cpp` |

Azahar is cited only as an independent description of the PICA texture format
(`TextureFormat::A8` → `{0,0,0,A}`); no Azahar code is copied.

## 12. Explicit non-goals

* No GPL-3.0 Jazz² code, generated effect functions, transpiler or PICA device
  code is copied. Only the architecture and the documented TEV presets are
  used as ideas.
* No tevsl code is copied; the repository has no license.
* No Scaleform SDK, leaked or otherwise, is used. The menu text atlas is our
  own SWF rasterizer; the only external facts used are the public D3D9 A8
  sampling semantics and the PICA A8 decode.
* No claim of a rendered Skyrim frame or hardware parity is made by this
  document. The MojoShader census was run on host over the existing chunk
  dumps; the A8 finding is a static/decoder finding that still needs the P0
  pixel test.
