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

### 1.1 Draw-time refusal

A selected plan can still be undrawable. `shader_state()` checks, before any
core draw call:

1. the process-wide material evidence mask
   (`d9_menu_shader_note_preconditions()`), and
2. the PICA descriptor validation of the plan.

If either fails, the draw returns `D3DERR_UNSUPPORTED` and
`d3d9_com_last_diagnostic()` names the entry and the missing preconditions or
backend gap. Nothing is submitted: no partial batch, no fallback texture and no
fake success. The mask starts empty and is reset on device creation and
`Reset`.

Current backend gap (honest): the PICA descriptor path still accepts only one
texture unit, no vertex colour and no external constants. Therefore the copy
plan is the only one of the four that is currently PICA-valid; logo, fog and
tint are selected, precondition-gated and then refused by name. This is not a
rendered-menu claim.

### 1.2 Caps and closure separation

- `d9_menu_closure_caps_claim()` and `d9_adapter_caps()` remain
  `0xFFFE0200` / `0xFFFF0200`, SM3 slots `0`.
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

- **`vs_3_0` path.** The closure still describes a bounded family of 390
  shipped VS programs, but the exact runtime key comes from the engine's
  vertex-declaration path. No specialised VS selection is wired here.
- **Automatic material selection.** Nothing in the runtime currently sets the
  precondition mask. A future NIF/material loader must validate the normal,
  tangent basis, c0..c7, alpha and texture formats and then call
  `d9_menu_shader_note_preconditions()`. Until then the logo/fog/tint draws are
  refused by name.
- **Other measured blobs.** The closure covers two PS entries and one fixed fog
  contract. The remaining ~6.6k distinct bytecode blobs are rejected while the
  closure is armed; there is no generic SM3 translation and no fallback.
- **PICA backend features needed by the selected plans:** two texture units,
  vertex colour, external constants, and the fixed fog/tint blend/depth state.
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

Result: **13 passed, 0 diagnostic, 0 failed** (the run also executes the Python
lane). New `menu_closure` coverage: 200 checks, 0 failures, including

- exact logo selection, five-stage plan and c7.a tint stage;
- exact copy selection and one-stage `REPLACE(TEXTURE0)`;
- no plan for a changed literal, an extra `nop`, a synthetic structural copy
  without the certified bytes, a vertex program, a wrong sampler swizzle, a
  wrong opcode or a missing stage;
- fixed fog/tint plans and named missing-precondition refusals;
- caps unchanged and zero SM3 slots after selection.

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
  -OutDir C:\Users\vasya\Documents\Skyrim_N3DS_wt\menu-shaders\build_host_full_menu_abs
```

Result: **71 passed, 1 diagnostic, 0 failed**. The diagnostic is `tesv_slice`
hitting the documented guest memory gate (`Not enough memory to run
application.`), not a rendering PASS.

Note: `-OutDir` must be absolute. With a relative `-OutDir` the harness sets
`TEMP` to a relative path and `steam`/`registry` exit 2 from `tmpfile_s`
before running any check; both pass when `TEMP` is absolute.
