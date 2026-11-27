# First-menu shader runtime: bounded closure selection

Status: **runtime selector connected; menu frame not claimed.** This note
records what is now wired into `source/d3d9_com.c` on top of the investigated
closure in `docs/MENU_SHADER_CLOSURE.md`. The global D3D9 capability report is
unchanged: `GetDeviceCaps` still says VS 2.0 / PS 2.0 with zero SM3 slots.

## 1. What is connected

New module: `source/d3d9_menu_shader_runtime.{c,h}`.

`IDirect3DDevice9::CreatePixelShader` now asks the selector before the generic
TEV path:

| Role | Selection key | Plan | Required before draw |
|---|---|---|---|
| `menu.logo.ps` | exact certified SHA-256 `8725393b...5ff9` (p1200005) **and** `p1200005_ADD_NORMAL` structure | five-stage menu-light plan, 2 textures, c3/c4/c7 | normal prebaked, tangent basis prebaked, ambient+emissive varying, material alpha in c4, c0..c7 constants, LDR, two 2D textures |
| `menu.composite.ps` | exact certified SHA-256 `6022044e...45de` (184-byte copy) | one `REPLACE(TEXTURE0)` stage, 1 texture | structural + PICA descriptor only (LDR, 2D sampler s0); no material mask |
| `menu.fog.fixed` | explicit fixed-material request; no `.fxp` | two-stage contract: `TEX0*PRIMARY`, then `TEX1*PREV` | LDR, two 2D textures, vertex colour |
| `menu.tint.fixed` | explicit fixed-material request; no `.fxp` | one `TEX0*PRIMARY` stage | LDR, one 2D texture, vertex colour |
| `menu.generic.vs` | bounded structural VS family + current declaration/FVF | fixed PICA vertex transform (position+UV); no arbitrary VS execution | VS inputs present in the declaration; fixed backend currently lacks color0/color1 varyings |

The two bytecode-backed entries are accepted only by their **exact certified
hash**, then re-checked structurally:

- logo must reproduce the `p1200005_ADD_NORMAL` instruction signature and the
  five-stage plan (the final stage is the retail c7.a fog/output tint);
- copy must reproduce the exact three-instruction `dcl,dcl,texld` stream and
  the one-stage `REPLACE(TEXTURE0)` plan.

A structural family match alone is not sufficient. In particular `p5` is a
valid menu-light signature but is **not** the certified logo key and is not
selected by this runtime path. A `nop` added to the copy, a changed literal, a
wrong swizzle, a wrong opcode or a wrong stage is refused by name; the output
match is zeroed and no shader object is created from a partial plan.

### 1.1 Vertex-shader family

`CreateVertexShader` now asks `d9_menu_shader_match_vs()`. A single VS hash is
not the contract because the engine selects a declaration-compatible program
from a shipped family. The runtime contract is:

- `vs_3_0`, no texture sample, no branch/loop;
- opcodes restricted to the measured static menu set
  (`mov/add/mad/mul/rcp/rsq/dp3/dp4/min/max/slt/sge/frc/dcl/pow/sincos/mova/def`);
- input semantics only `position0`, `normal0`, `texcoord0..4`, `tangent0`,
  `binormal0`, `color0`, `color1` (the measured static set);
- output declarations for `position0`, `texcoord0`, `color0`, `color1`;
- 1..96 instructions.

The manifest's older "390 candidates" count used output declarations to satisfy
its input requirement. The runtime tightens it to the static menu subset: **83
base members** in `shaders001.fxp` (83 more in the Update `shaders002.fxp`),
26 distinct base hashes (28 including the Update override). This is a bounded
family, not arbitrary vs_3_0 acceptance.

`SetVertexShader` checks the selected VS against the current explicit
declaration or FVF. A missing `color0`/texcoord input is a named refusal and
the old VS stays bound. At draw time the same check runs again, so changing the
declaration after binding cannot bypass it. The fixed PICA VS still produces
only position+texcoord0, so every member of this family is selected but draw
refused by name until the backend carries color0/color1 varyings.

### 1.2 FXP key mapping

`d9_menu_shader_lookup_fxp(stage, package, key)` maps the measured
package+technique keys deterministically:

| Package | Key | Entry |
|---|---|---|
| `shadersfx\lighting\059\p1200005.fxp` | `0x01200005` | `menu.logo.ps` |
| `shadersfx\imagespace002.fxp` | `0x00000000` | `menu.composite.ps` |
| `shadersfx\shaders001.fxp` | any measured VS key | `menu.generic.vs` |
| `shadersfx\shaders002.fxp` | any measured VS key | `menu.generic.vs` |

The VS family has many technique keys; the package rule identifies the family
and the bytecode still has to pass `d9_menu_shader_match_vs()`. Lookup is
independent of creation order; tests resolve the same package/key in different
orders and get the same entry.

### 1.3 Material evidence and draw refusal

`shader_state()` now derives the observable part of the menu material mask from
the current device state:

- `texture0_2d` / `texture1_2d` when the bound texture units hold 2D LDR
  textures (A8R8G8B8, X8R8G8B8 or DXT1/3/5);
- `ldr_quality` when texture 0 is LDR and texture 1 (if bound) is LDR;
- `vertex_color` when the explicit declaration or FVF provides `COLOR0`;
- `material_constants_c0_c7` when the guest has uploaded c0..c7;
- `material_alpha_c4` when c4 was uploaded with a finite alpha in `[0,1]`.

`normal_prebaked`, `tangent_basis_prebaked` and
`ambient_emissive_varying` are **not** derived from textures or declarations.
They remain explicit-only and must be reported by the material/preprocessing
loader via `d9_menu_shader_note_preconditions()`. Until then the logo draw
stays refused by name even with textures, colour and constants present.

`SetPixelShaderConstantF` now writes the real c0..c7 TEV bank (`slots
109`/`110`, with honest `GetPixelShaderConstantF`).  The window is eight
registers because PICA exposes eight external TEV constants; wider D3D9
ranges return named `D3DERR_INVALIDCALL` without a partial write.  The
nine-row bank passed to `draw_menu()` is authoritative: PS rows override the
legacy VS/ambient upload, so a later `SetVertexShaderConstantF` cannot clobber
a material c3/c4/c7.  The observed mask admits c0..c7 evidence from either
bank, and alpha evidence from PS c4 when present.  `SetPixelShaderConstantI/B`
also round-trip through Get but are not claimed as draw inputs: the accepted
ps_3_0 subset has no integer, predicate or flow-control opcode that could
consume them.  The same bounded 8-register treatment applies to
`SetVertexShaderConstantI/B` (`slots 96..99`); the accepted vertex subset also
has no integer/predicate/flow-control opcode, so those values never affect a
draw.  Bool values are normalised to 0/1 on write, and the VS float
c0..c8 bank remains separate from both integer/bool banks and from PS
constants.

`SetVertexShaderConstantF` still accepts c0..c7 so this evidence can exist.
The advertised `MaxVertexShaderConst` stays **4**; c4..c7 are stored only as
menu-material evidence and the fixed clip path still consumes c0..c3. This is
not an SM3 or shader-capability claim.

A selected plan can still be undrawable. `shader_state()` checks the VS
declaration contract, the combined explicit+observed mask, and the PICA
descriptor validation before any core draw. If any fails, the draw returns
`D3DERR_NOTAVAILABLE` and `d3d9_com_last_diagnostic()` names the entry and the
missing input/precondition/backend gap. Nothing is submitted: no partial batch,
no fallback texture and no fake success. The explicit mask starts empty and is
reset on device creation and `Reset`.

PICA backend status after this layer:

- two 2D texture units (`s0`/`s1`) are validated and bound;
- `color0` is routed through the existing colour vertex shader as
  `GPU_PRIMARY_COLOR`;
- external c0..c7 are materialised per TEV stage from the COM constant bank;
- the logo's `NdotL*c4 + color1` stage is split into `MODULATE(c4)` +
  `ADD(c8)` and `color1` is supplied as the explicit
  `d9_com_menu_set_ambient_emissive()` value (internal c8).

This makes the copy, logo, fog and tint plans PICA-valid (`menu_closure`
checks 6 stages/2 textures/ambient fold for the logo).  It does **not** provide
per-vertex color1 interpolation: PICA has one interpolated vertex colour and the
fragment-lighting source is a different, material/light-driven value.  The
menu-light plan therefore uses the explicit c8 fold documented above.

Measured retail permutations (dumped with `TESV_MENU_SHADER_DUMP=<dir>`) add
`normal0`/`color1` inputs and opcode `0x23` (`abs`) to the static family.
`color0`/`color1` outputs are optional: the five emitted GFx programs include a
position+texgen member without them.  A member is marked CPU-executable only
when its *visible* outputs are exactly the COM fixed transform:

- position0 = four `dp4` rows `c0..c3` against POSITION0;
- texcoord0 = `mov` from a texcoord input, or `dp4 c4/c5` against POSITION0
  (the core reproduces the latter from the VS constant bank);
- color0 = `mov` from COLOR0, with identity or `zyxw` (red/blue swap).

Logo-lighting work on NORMAL and COLOR1 is allowed but must not feed those
three outputs.  Members that do not match remain a named refusal.  If an exact
emitted PICA VS exists in `d3d9_pica_vs_menu_table.inc`, it is kept as the
authoritative descriptor unless the CPU-safe path above applies.  The global
caps are unchanged.  No menu frame or pixel proof is claimed.

### 1.5 In-scene Clear

Retail GFx calls `IDirect3DDevice9::Clear` between BeginScene and EndScene.
Strict D3D9 refuses it; the COM bridge now clears the active colour/depth
target immediately instead of dropping the frame's reset.  This is a
deliberate diagnostic relaxation (documented in `source/d3d9_com.c`), not a
silent no-op: backend failures and rectangle clears remain named refusals.

Render-target textures use an explicit `D9Backend.target_texture()` sample view;
core stores that handle in `Texture.native` instead of assuming a target is a
texture.  This is required for a menu plan that samples a rendered LDR target.
The citro3d view is embedded in `NativeTarget` and shares the target's storage;
the target remains the only owner and the only delete.

### 1.4 Caps and closure separation

- `d9_menu_closure_caps_claim()` and `d9_adapter_caps()` remain
  `0xFFFE0200` / `0xFFFF0200`, SM3 slots `0`, `MaxVertexShaderConst = 4`.
- The closure predicate is still the only thing the TESV level-7 branch may
  consult. This runtime selector never touches a caps field.
- An armed closure still invalidates on any shader outside the certified set.

## 2. `D3DXCompileShader` is not the SM3 menu path

`D3DXCompileShader` remains a named **`E_NOTIMPL`** shim
(`source/d3dx_surface_shims.c`):

- it zeroes `ppShader`, `ppErrors` and `ppConstants`;
- it dereferences no guest bytecode output and never traps;
- it does not produce an empty successful shader.

The existing `tests/d3dx_surface_shims_smoke.c` covers that contract (valid
outputs cleared, all-NULL call, unmapped outputs, no trap). It is not duplicated
here.

Measured trace evidence (`E:\Skyrim_N3DS_build\analysis\trace12.err`): the name
appears only in two `[import] d3dx9_42.dll!D3DXCompileShader -> 080d95b3`
bindings; there is no `[d3dx] D3DXCompileShader(...)` call before the SM3 gate.
TESV loads precompiled FXP/bytecode and the SM3 work is in
`CreatePixelShader`/`CreateVertexShader`.

To make a future run auditable, the shim now increments
`d3dx_surface_compile_shader_calls()` and includes `call #N` in its diagnostic.
Zero is the expected value for the precompiled-FXP menu path. This is
diagnostic only; no compiler, no bytecode cache and no fake success were added.

## 3. What is not connected

- **VS execution.** Selection and declaration/FVF gating are connected, but the
  fixed PICA VS only transforms position+UV. It does not produce color0/color1
  or the extra texcoords the family declares, so family draws are refused by
  name until the backend grows those varyings.
- **Automatic material selection.** Observable texture/declaration/constant
  evidence is derived automatically. A NIF/material loader still has to
  validate and report the normal map, tangent basis and the explicit c8
  ambient+emissive value; without those bits the logo draw is refused by name.
- **Other measured blobs.** The closure covers two PS entries and one fixed fog
  contract. The remaining ~6.6k distinct bytecode blobs are rejected while the
  closure is armed; there is no generic SM3 translation and no fallback.
- **PICA backend features still absent:** per-vertex color1 interpolation and
  color0/color1 VS varyings beyond position+texcoord0.  Two texture units,
  color0, external c0..c7 and the c8 menu ambient fold are now implemented.
- **Exact retail fog/tint stage semantics.** No `.fxp` ships for
  `BSEffectShaderProperty`; the fog/tint plans above are explicit contract
  plans, not bytecode matches. The logo's own c7.a tint stage is part of the
  measured p1200005 plan.
- **Capture audit of the entry id.** The selector exposes the id but the
  first-draw capture does not yet record it.
- **A rendered main-menu frame or pixel proof.**

### 1.6 Residual `menu.generic.vs` inventory (314 dumped hashes)

The dumps under `TESV_MENU_SHADER_DUMP` (314 unique `vs_*.bin`, captured while
the StartMenu assets load) were replayed with
`tools/shaders/menu_vs_residual.py`.  It mirrors `vs_input_allowed()`,
`vs_opcode_allowed()` and `vs_cpu_safe()`, and taints the visible outputs from
`tangent0`/`binormal0`.  Result:

| Class | Unique hashes | Runtime decision |
|---|---:|---|
| CPU-safe fixed transform | 5 | selected and drawable |
| `tangent0`/`binormal0` family, non-fixed position | 46 | selected, draw refused by name |
| skinning `blendweight0`/`blendindices0` | 37 | named creation refusal |
| `nrm` opcode | 92 | named creation refusal |
| `lrp` (0x12) | 10 | named creation refusal |
| structural (no texcoord0, >96 instructions) | 20 | named creation refusal |

The five CPU-safe hashes are exactly the fixed position/UV/colour members:

```
0975cc503f63d9b1  uvgen=1
264c20659a094788  uvgen=0 zyxw
4999d5438e8c7758  uvgen=1
6186c545f488d0f8  uvgen=1
73904d3f32bd5eec  uvgen=1
```

`tangent0` and `binormal0` are now measured input declarations
(`vs_input_allowed()`), because the taint pass shows that in all 46 members
they reach only normal/tangent-space lighting outputs (`texcoord1..7`,
`color1`) and never `position0`, `texcoord0` or `color0`.  They still do not
become drawable: every one of the 46 builds its clip position through a
temporary (`dp4 c0..c3, rT`, where `rT` is a `cN`-scaled/`NORMAL`-displaced
copy of POSITION0).  `vs_cpu_safe()` therefore keeps `backend_cpu_safe = 0`
and the draw is refused by name as "not the fixed CPU transform".  This is a
dataflow proof, not a hash allow-list.

`lrp` (0x12) stays a named refusal: `lrp` is a vertex arithmetic instruction,
the fixed COM path is not a VS interpreter, and there is no PICA VS emitter for
it.  Each dumped `lrp` member also fails the fixed-position proof, so accepting
the opcode would not make a single draw possible.  The diagnostic now says
`opcode 'lrp' (0x12) has no accepted PICA VS equivalent`.

None of the 46 tangent/binormal, 92 `nrm` or 10 `lrp` members is needed for the
StartMenu GFx draw: the measured run creates the menu VS objects but submits
only the 8 background primitives, no menu draw.  They are world/effect shaders
loaded while the menu is resident; they remain bounded, named refusals rather
than being routed through a generic interpreter.

### 1.7 Offscreen render targets for GFx

The real run logs were checked for `CreateRenderTarget`, `CreateTexture`
(`D3DUSAGE_RENDERTARGET`), `SetRenderTarget`, `StretchRect` and
`GetRenderTargetData`:

- `E:\menu_draw\last_run.log` and `E:\slice_b7c\last_run.log` contain **no**
  `CreateRenderTarget`, `SetRenderTarget`, `StretchRect` or
  `GetRenderTargetData` call (successes and named failures would both be
  logged by the COM bridge).
- Both logs contain exactly **three**
  `CreateTexture: render-target textures must be power-of-two 8..1024`
  refusals, immediately after the menu texture loads and before the menu VS
  creation.  GFx therefore *does* request offscreen render-target textures
  (Scaleform mask/filter targets), and the current POT-only gate is the hole.

What is already supported: one-level, `D3DPOOL_DEFAULT`,
`D3DUSAGE_RENDERTARGET`, `A8R8G8B8`/`X8R8G8B8`; the texture is sampleable and
bindable as a target; `StretchRect` handles same-format surface blits; depth
attachments are supported for explicit targets.  What is missing is a
non-power-of-two logical target.  `Target` already separates logical `w/h`
from native `nw/nh` and `apply_target_viewport()` already has a
`target_viewport_scaled` path, but `create_target()` rejects NPOT before that
path, and a padded POT storage view would need UV sub-rect scaling for the
sampled target.  That is not implemented in this change; the three requests
stay named `D3DERR_NOTAVAILABLE` refusals.  **Offscreen RTs are required for
the GFx masks; this is the next RT task, not a shader task.**  No `DevCaps`
field is changed by that gap.

## 4. Tests

Targeted command:

```powershell
powershell -File tools\run_host_tests.ps1 `
  -Only menu_closure,tev_real,color_tev,two_stage `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe `
  -OutDir build_host_targeted
```

Result: **14 passed, 0 diagnostic, 0 failed** (the run also executes the Python
lane). New `menu_closure` coverage: 271 checks, 0 failures, including

- exact logo selection, five-stage plan and c7.a tint stage;
- exact copy selection and one-stage `REPLACE(TEXTURE0)`;
- no plan for a changed literal, an extra `nop`, a synthetic structural copy
  without the certified bytes, a vertex program, a wrong sampler swizzle, a
  wrong opcode or a missing stage;
- fixed fog/tint plans and named missing-precondition refusals;
- VS family selection, required output semantics and named refusals for a
  missing color1 output, normal input or texture sample;
- declaration/FVF compatibility (0x142 accepted, 0x102 refused by name);
- FXP key lookup for both PS entries and the VS package family, including
  stage-mismatch, wrong-key and order-independence cases;
- observable-only material bits cannot satisfy the logo's explicit
  normal/tangent/ambient requirements;
- caps unchanged, `MaxVertexShaderConst = 4`, zero SM3 slots after selection;
- PICA descriptor accepts the exact logo plan (6 stages, 2 textures, ambient
  fold) and the fog/tint fixed plans.

`pica_tev` covers the wider descriptor tier directly: logo/fog/tint acceptance,
2 texture units, color0/ambient flags, c3/c4/c7/c8 constant list, and negative
third texture/duplicate sampler/external c9/color1-without-fold cases.
`two_stage` submits a synthetic 2-texture + color0 + external-c7 menu plan
through the core's `draw_menu` route and asserts the null backend received both
textures and the constant bank.

`tev_com` additionally drives the COM path: synthetic VS create, named
SetVertexShader refusal on an incompatible FVF, compatible bind, named
draw-time fixed-VS refusal with no backend draw, missing color1 create refusal
with a zero output slot, observable c0..c7/texture/colour mask, and the explicit
c8 ambient setter (NaN refused).

Real-chunk measurement (`tev_real`, read-only data under
`E:\Skyrim_N3DS_build`): 9,621 chunks, 19 accepted by the generic translator,
5 p5 signatures and exactly 1 p1200005 signature. No game bytes are committed.

The `d3dx_surface_shims` test additionally asserts the compile counter and the
`E_NOTIMPL` outputs.

Full registered suite (same gen/irc/exe parameters as the main tree):

```powershell
powershell -File tools\run_host_tests.ps1 `
  -PilotGen pilot_out_mult\D3DXMatrixMultiply\gen `
  -TesvSliceGen E:\Skyrim_N3DS_build\analysis\growth13\gen01 `
  -TesvIrc F:\Skyrim\ir\tesv.irc `
  -TesvExe E:\skyr\TESV.exe -AllowDiagnosticFault `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe `
  -OutDir C:\Users\vasya\Documents\Skyrim_N3DS_wt\menu-shader-vs\build_host_full_menu_abs
```

Result: **102 passed, 1 diagnostic, 0 failed**. The diagnostic is `tesv_slice`
hitting the documented guest memory gate (`Not enough memory to run
application.`), not a rendering PASS.

Note: `-OutDir` must be absolute. With a relative `-OutDir` the harness sets
`TEMP` to a relative path and `steam`/`registry` exit 2 from `tmpfile_s`
before running any check; both pass when `TEMP` is absolute.

### 4.1 Residual-family coverage

`menu_vs_family` now covers the measured input additions and real dumped
bytecode:

- synthetic `tangent0`/`binormal0` declaration used only in a temp:
  selected, `backend_cpu_safe = 1` (dataflow keeps it off the visible path);
- the same declaration feeding a position row: selected, `cpu_safe = 0`;
- real `6186c545...` (position+texgen, no colour): `cpu_safe = 1`, `uvgen = 1`;
- real `10d46b0a...` (tangent/binormal): selected, `cpu_safe = 0`;
- real `08865907...` (`lrp`): named refusal containing `lrp` and `0x12`.

The dump bytes stay outside the repository. The test reads
`%TESV_MENU_VS_DUMPS%\vs_<sha>.bin` (default `E:\menu_draw_dump`) and prints a
skip notice when the directory is absent.

Targeted run in this worktree (`com`, `render_texture`, `rt_readback`,
`d3dx_*`, `menu_vs_family`, `menu_closure`): **8 passed, 0 failed** for the
C tests plus **menu_vs_family PASS** and **menu_closure 279 checks, 0
failures**.  `tools/shaders/runtime_audit.py` reports 128 draw-enabled plans,
128 runtime paths and 30 valid PICA VS programs (0 unmatched).
