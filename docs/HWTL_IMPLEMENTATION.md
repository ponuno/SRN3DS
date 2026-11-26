# HW T&L: implementation scope, decision and alternatives

Date: 2026-11-26. Status: **CPU T&L prototype implemented behind a test flag;
no caps claim; the hardware pipeline is not implemented.**
Companion to `docs/HWTL_DECISION.md` (measurement and options) and
`docs/BACK_BUFFER_LOCKABLE.md` (the feature that now lets the engine get past
`CreateDevice`).

## 1. What the caps bit promises

From `shared/d3d9caps.h`:

```c
#define D3DDEVCAPS_DRAWPRIMITIVES2     0x00002000L
#define D3DDEVCAPS_DRAWPRIMITIVES2EX   0x00008000L
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT 0x00010000L /* ... DRAWPRIMITIVES2EX must be also */
```

`D3DDEVCAPS_HWTRANSFORMANDLIGHT` is a claim that the **hardware** fixed-function
transform *and* lighting pipeline exists. Read literally, claiming it also
requires `DRAWPRIMITIVES2EX` conformance, which is the DX7 extended
`DrawPrimitives2` protocol (command-buffer streaming, state blocks and vertex
buffer management through one entry point), not just the D3D9 draw entry
points. The bridge implements neither.

Current shipping `DevCaps` stay `0x00082000`
(`DRAWPRIMITIVES2 | HWRASTERIZATION`). `TESV_D3D9_CAPS_PROBE=1` remains a
measurement-only override and is not shippable.

## 2. Minimum real implementation (Option A)

### 2.1 Core vertex pipeline (`source/d3d9_core.*`, `source/d3d9_vertex.*`)

1. Object-space input: keep `XYZ` vertices plus `NORMAL`, `DIFFUSE`, `SPECULAR`
   declaration/FVF support. Today `fetch()` decodes position/UV/colour only and
   `D9Vertex` carries a single post-transform clip matrix. A T&L path needs a
   second stream shape (`D9LitVertex {pos3, normal3, diffuse4, specular4, uv…}`)
   and draw routes that hand object-space data to the backend.
2. Transform stack: `D3DTS_WORLD` (with `MultiplyTransform`), `VIEW`,
   `PROJECTION`, `TEXTURE0..7` texture transforms, `D3DRS_TEXTURETRANSFORMFLAGS`
   and `D3DTTFF_COUNTn/PROJECTED`. World/view/projection exist; the stack state
   and texture transforms do not.
3. Fixed-function lighting evaluation model: 8 light slots
   (`D3DLIGHT9`: directional/point/spot, diffuse/specular/ambient, range,
   attenuation0/1/2, falloff, theta/phi), `SetLight`/`GetLight`/`LightEnable`/
   `GetLightEnable`, material diffuse/ambient/specular/emissive/power,
   `SetMaterial`/`GetMaterial`, `D3DRS_AMBIENT`, `D3DRS_COLORVERTEX` with
   `D3DMCS_MATERIAL/COLOR1/COLOR2`, `D3DRS_LOCALVIEWER`, `D3DRS_SPECULARENABLE`,
   `D3DRS_NORMALIZENORMALS`, two-sided lighting, and the `D3DRS_LIGHTING` master
   switch.
4. Fog (part of the fixed pipeline TESV will use): `D3DRS_FOGENABLE`, `FOGCOLOR`,
   `FOGTABLEMODE`, `FOGSTART/END/DENSITY`, `D3DRS_RANGEFOGENABLE`; either CPU
   per-vertex fog or a PICA fog stage.
5. User clip planes if `MaxUserClipPlanes` is claimed: `SetClipPlane`,
   `GetClipPlane`, `D3DRS_CLIPPLANEENABLE`. PICA has no fixed clip-plane unit;
   this must be shader-side (clip distance emulation) or the caps field must be 0.

### 2.2 PICA implementation (`source/d3dshader*.v.pica`, `d3d9_citro3d.c`)

The current vertex shader is one `m4x4` + `mov` with the clip matrix in `c0..c3`.
Real T&L needs:

- normal attribute (and diffuse/specular attributes) in the attribute layout;
- per-vertex lighting: ambient + Σ(diffuse·NdotL) with attenuation/spot factor,
  specular with half-vector or reflection, local-viewer and two-sided variants;
- texture-coordinate transform before sampling;
- world/view/projection as separate uniforms (or a CPU-composed WVP plus a
  CPU-composed normal matrix, which is cheaper and equivalent for lighting);
- fog factor output.

Because picasso shaders are precompiled binaries embedded with `bin2s`, each
state combination must be a **variant**: active light count (0..8), light types,
specular on/off, local viewer, two-sided, normalize normals, per-vertex colour
sources, texture transform count, fog mode. Naively that is dozens of binaries.
A bounded design can reduce it to a small matrix:

| Variant key | Values |
| --- | --- |
| light count | 0, 1, 2, 4, 8 |
| specular | off/on |
| local viewer | off/on |
| two-sided | off/on |
| texture transform | none, count2, count3, count4/projected |
| fog | none, linear, exponential |

That is still ~hundreds of shader bodies unless uniforms + a fixed 8-light loop
with zeroed unused lights are used (8 lights × ~8 instructions each is within
PICA's vertex instruction budget; the loop must be fully unrolled because PICA
has no dynamic uniform indexing in the vertex stage on this toolchain). The
CPU-composed normal matrix avoids a separate inverse-transpose uniform.

### 2.3 Backend and COM surface

- `D9Backend`: a new `draw_lit()` entry (object-space vertices, four matrices,
  material/light uniforms, state) plus a `caps` query so the core never
  advertises a capability the backend lacks. Existing `draw`/`draw_colored`
  stay.
- `d3d9_com.c`: `MultiplyTransform` (slot 46), `SetMaterial` (49), `GetMaterial`
  (50), `SetLight` (51), `GetLight` (52), `LightEnable` (53), `GetLightEnable`
  (54), `GetTransform` (45), `SetClipPlane` (55), `GetClipPlane` (56). Each must
  validate the full D3DLIGHT9/D3DMATERIAL9 layout (104/68 bytes) in guest
  memory, reject non-finite values and invalid types/ranges, and stay
  unimplemented-with-a-name only if the caps bit is not claimed.
- `d3d9_adapter.c`: `max_active_lights`, `shade_caps`, `max_user_clip_planes`,
  `max_point_size`/`max_vertex_w` and the `DRAWPRIMITIVES2EX` bit must then be
  set truthfully.

### 2.4 Size and risk

| Area | Estimated new/changed LOC | Risk |
| --- | --- | --- |
| Core transforms, fetch, fog, state validation | 1,200-1,800 | medium |
| PICA shader variants + uniforms | 600-1,000 (plus asm) | **high** (variant explosion, VS uniform budget) |
| Backend lit draw + caps | 500-800 | medium |
| COM methods + 32-bit struct validation | 800-1,200 | low-medium |
| Host tests (golden math, state matrix, caps honesty) | 1,000-1,500 | medium |
| Azahar pixel proof (directional, point, specular, fog) | runs only | medium |

**Total ≈ 4,000-6,000 LOC** plus a new pixel-proof campaign, and
`DRAWPRIMITIVES2EX` conformance is a separate large protocol. This does not fit
the current pass. It is not one more incremental feature; it is a second
renderer pipeline.

### 2.5 CPU T&L prototype (implemented behind a test flag)

`TESV_D3D9_TNL_PROTO=1` (read once at `d9_com_init`; host/test only) enables:

- core state: world/view/projection (existing), `MultiplyTransform`
  (`World = World * matrix`, row-vector), material diffuse/ambient/emissive,
  up to eight directional or point lights, `LightEnable`, global ambient,
  `NORMALIZENORMALS`, `COLORVERTEX` and the diffuse material source;
- per-vertex CPU lighting: world position, inverse-transpose world normal,
  ambient + Σ(diffuse·N·L·attenuation), emissive; the result is fed to the
  existing colour draw path (`draw_colored`) so the PICA rasteriser still draws
  the triangles;
- normal fetch in the declaration/FVF path: only when the flag is on may an FVF
  carry `D3DFVF_NORMAL` (0x010), and the admitted FVF set widens by exactly that
  bit. The production path still rejects it;
- honest refusals: spot lights, specular, local viewer, two-sided lighting,
  texture transforms, extra texture stages, lighting without a NORMAL element,
  a singular world matrix, non-finite light/material values, indices >= 8;
- COM slots 45/46/49/50/51/52/53/54.

Defaults are the D3D9 fixed-function ones (lighting on, material diffuse/ambient
white, no light enabled, global ambient black, diffuse source COLOR1). With the
flag off, every new entry point is a named `D3DERR_NOTAVAILABLE` refusal and
`SetFVF(0x012)` is rejected, exactly as before.

Tests: `tests/tnl_proto_smoke.c` runs twice
(`tnl_proto_off`/`tnl_proto`). The null backend records the exact RGBA handed to
the rasteriser, so the test proves:

- directional light: normals facing/opposing the light give red/black;
- point light: a linear attenuation of 0.5 at distance 2 gives 0.5 red;
- ambient: material ambient (0.5) × global ambient (0x40) gives 0.125;
- material/light round-trips; `GetTransform`/`MultiplyTransform` composition;
- refusals: spot, index 8, NaN light, specular, local viewer, lighting without
  a normal, singular world matrix;
- `D3DCAPS9.DevCaps == 0x00082000` (no `0x10000`, no `0x8000`) with the flag off
  and on.

This is **not** hardware T&L and **not** a rendered frame: it is CPU math feeding
the existing draw path, with caps unchanged. It does not satisfy TESV's gate and
must not be described as such.

### 2.6 What remains to claim `HWTRANSFORMANDLIGHT` honestly

After the prototype the state validation, the lighting math, the fetch shape and
the test harness already exist; what remains is moving the math into PICA and
conforming to the DX7 protocol:

1. **PICA vertex-shader variants** for the supported state matrix (light count,
   specular, local viewer, two-sided, texture transform, fog) or a single
   unrolled 8-light shader; normal/colour attributes; uniforms for
   material/lights. Estimated 600-1,000 LOC plus picasso asm, **high** risk
   (variant explosion, uniform/instruction budget).
2. **Fog** (vertex or PICA stage) and **user clip planes** (shader-side, or
   `MaxUserClipPlanes = 0`).
3. **`DRAWPRIMITIVES2EX`** conformance: extended `DrawPrimitives2` streaming,
   state blocks, vertex-buffer management. Separate large protocol.
4. **Truthful caps fields**: `MaxActiveLights`, `ShadeCaps`, `MaxUserClipPlanes`,
   `MaxPointSize`, and `DRAWPRIMITIVES2EX`, only after 1-3 pass.
5. **Azahar pixel proof** for directional/point/specular/fog plus the conformance
   tests.

Estimated remaining effort: ~2,000-3,500 LOC plus the pixel/conformance campaign
(4-8 weeks at the current pace), dominated by the PICA variants and
`DRAWPRIMITIVES2EX`. The prototype reduces the core/state/test portion, not the
shader or protocol risk.

## 3. Decision for this pass

- **Do not** set `D3DDEVCAPS_HWTRANSFORMANDLIGHT`, do not set
  `DRAWPRIMITIVES2EX`, and do not add placeholder `SetLight` successes in the
  production path.
- Keep the truthful `0x00082000` caps.
- The CPU T&L prototype (`TESV_D3D9_TNL_PROTO=1`) implements the state and math
  behind a test flag without changing `DevCaps`; it is a stepping stone, not a
  capability claim (section 2.5).
- Finish the features that Option A and Option B both need first (done:
  lockable retained back buffer, Present blit, explicit depth-stencil).
- The post-device census reached `CreateDevice` `S_OK` but the engine aborts at
  the SM3 gate `0x00c6e22d` before any render pass, depth surface, Clear or
  fixed-function lighting call (section 5). The A/B census is therefore still
  empty; SM3 is the next renderer blocker, not T&L.

## 4. Alternatives

### 4.1 Option B - documented engine adaptation (preferred if the census is empty)

If the post-device census shows TESV never uses fixed-function lighting
material state (the engine ships its own shaders), the caps gate at
`0x00c6d8c6` is an engine check that does not match the device class it drives.
The honest fix is then a byte-verified, logged, reversible patch of that one
branch while `SetLight`/`SetMaterial`/`LightEnable` remain explicit failures.
This is an engine change, not a hardware lie, and needs explicit approval.

### 4.2 Option C - software T&L with honest caps

Implement the fixed-function pipeline on the CPU (object-space vertices are
already decoded in the core) and feed the PICA rasteriser the result, while
**keeping `HWTRANSFORMANDLIGHT` clear**. This is useful if the engine is ever
patched or selects software vertex processing, and it is the right stepping
stone toward Option A: the lighting math and state validation can be written and
tested once, then moved into a PICA vertex shader. It does not satisfy TESV's
gate and must not be described as hardware T&L.

### 4.3 Staged Option A (if the census shows real FFP use)

1. Directional + point lights, material diffuse/ambient, per-vertex colour, no
   specular/fog/spot, 4 lights, still with no caps claim.
2. Add specular, spot, fog, 8 lights, texture transforms.
3. Add `DRAWPRIMITIVES2EX`, then claim `0x10000 | 0x8000` only after the
   conformance tests and an Azahar pixel proof pass.

## 5. Census result (measured 2026-11-26)

With the lockable back buffer implemented, the host slice no longer needs the
removed `TESV_D3D9_PP_PROBE`. Run:

- `TESV_MEMPROBE=1`, `TESV_D3D9_CAPS_PROBE=1` (measurement only),
  `TESV_D3D9_TRACE=1`, `N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc`;
- `tesv_slice.exe F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160`;
- exit 0, `Guest ExitProcess(0)` from `0x0069107d`, 216,914-byte trace,
  no guest fault.

Observed after `CreateDevice` (factory slot 16, caller `0x00cd8b74`,
`400x240 X8R8G8B8`, `DISCARD`, interval ONE, `Flags=0x1`): **`S_OK`**.

Then, in order:

1. `GetDeviceCaps`; `SetTransform` VIEW (`0x00cd32d1`) and PROJECTION
   (`0x00cd34cc`); `SetViewport` (`0x00cd3605`) -> `S_OK`.
2. `CreateTexture(2x1, 1 level, A8R8G8B8, MANAGED)` -> `S_OK`, then
   `GetSurfaceLevel`, `GetDesc`, `LockRect`/`UnlockRect`, `Release` -> all
   `S_OK` (the RAM-texture level-surface path from the earlier route).
3. Five `IDirect3DDevice9::AddRef` calls (`0x00cec195`, `0x00ceafca`,
   `0x00cec7ba`, `0x00cec857` twice).
4. Four `CheckDeviceMultiSampleType` calls from `0x00c6de67` -> all
   `D3DERR_NOTAVAILABLE` (only `D3DMULTISAMPLE_NONE`).
5. `D3DPERF_SetOptions(1)` (no-op).
6. `CheckDeviceFormat(HAL, X8R8G8B8, D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
   D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)` from `0x00c6e0a9` -> unavailable.
   Disassembly shows the result only sets a feature byte (0x01ba73ff); it is
   not the fatal check.
7. **Fatal gate `0x00c6e22d`**: `cmp [0x1ba749c], 7; jge ...`, else it loads
   the string `"ShaderModel3 required but not supported by your video card."`
   (`0x114e04c`) and continues to the generic "Failed to initialize renderer."
   message box.

Therefore, with the truthful SM2 caps:

- `CreateDepthStencilSurface` / `SetDepthStencilSurface` / `Clear` /
  `BeginScene` / `DrawPrimitive` / `Present` are **not reached by TESV yet**;
  the engine aborts at the SM3 gate before creating the depth surface.
- No `SetLight`, `GetLight`, `LightEnable`, `SetMaterial`, `GetMaterial`,
  `MultiplyTransform` or `SetClipPlane` call occurs at all. **The FFP-lighting
  census is still empty because the run ends before any render pass.**
- The depth-stencil feature is exercised by `tests/back_buffer_lockable_smoke.c`
  and the adapter/requirements matrices, not by TESV yet.

Consequence: A vs B cannot be decided from this census. The next renderer
blocker is SM3 (`0x00c6e22d`), not T&L. Options are unchanged; do not claim
`HWTRANSFORMANDLIGHT` on the basis of this run.

## 6. Census instructions (next action)

Re-run the same trace once the SM3 gate is either implemented (the menu shader
family) or adapted, and record:

- `CreateDepthStencilSurface` arguments and result;
- every `SetLight`/`GetLight`/`LightEnable`/`GetLightEnable`/`SetMaterial`/
  `GetMaterial`/`MultiplyTransform`/`SetClipPlane` call with arguments;
- `Clear` flags and `BeginScene`/`EndScene`/`Present`/`LockRect` on the back
  buffer;
- the first fixed-function draw state (`D3DRS_LIGHTING`, `ZENABLE`, FVF/declaration);
- the first non-D3D9 fault after device creation.

The result belongs in `docs/HWTL_DECISION.md` section 5 and decides A vs B.
Until then this file records scope and the decision not to claim the bit.
