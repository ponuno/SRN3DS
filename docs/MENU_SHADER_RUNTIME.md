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
- input semantics only `position0`, `texcoord0..4`, `color0`;
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

`SetVertexShaderConstantF` now accepts c0..c7 so this evidence can exist. The
advertised `MaxVertexShaderConst` stays **4**; c4..c7 are stored only as
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

The fixed PICA vertex shader still emits position+texcoord0 only, so the
`menu.generic.vs` family is selected and then refused at draw time by name.  A
material loader must still supply normal/tangent evidence and the c8 ambient
value.  No menu frame or pixel proof is claimed.

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

Result: **80 passed, 1 diagnostic, 0 failed**. The diagnostic is `tesv_slice`
hitting the documented guest memory gate (`Not enough memory to run
application.`), not a rendering PASS.

Note: `-OutDir` must be absolute. With a relative `-OutDir` the harness sets
`TEMP` to a relative path and `steam`/`registry` exit 2 from `tmpfile_s`
before running any check; both pass when `TEMP` is absolute.
