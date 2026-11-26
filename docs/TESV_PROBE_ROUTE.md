# TESV route behind the measurement probes (2026-11-25)

This note records what the engine does once the two honest gates no longer stop
it. Every value below is measured on the host slice with the probes enabled;
the probes are measurement-only and must never become shipping defaults.

## Probes

| Variable | Effect | Shipping |
|---|---|---|
| `TESV_D3D9_CAPS_PROBE=1` | `GetDeviceCaps` adds `D3DDEVCAPS_HWTRANSFORMANDLIGHT` | never |
| `TESV_D3D9_PP_PROBE=1` | **removed** (obsolete); `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is a real retained-target feature now | n/a |

A third probe, `TESV_D3D9_STAGE_PROBE=1`, reported `MaxTextureBlendStages =
MaxSimultaneousTextures = 2` before those were implemented. It is obsolete: the
COM caps now derive `2/2` from the backend's real `draw_two` entry point.

Each prints a one-shot `MEASUREMENT ONLY ... not shippable` line.

## Measured sequence (evidence: `E:\Skyrim_N3DS_build\analysis\stageprobe_tesv.err`)

1. `0x00c6d8c6` HW T&L gate passed (probe).
2. Adapter/format enumeration; `0x00cd5542` probes 80 texture/cube formats.
3. `CreateDevice` (factory slot 16, caller `0x00cd8b74`) -> **`S_OK`** with
   `400x240 X8R8G8B8`, `DISCARD`, 1 back buffer, no MSAA, windowed,
   `EnableAutoDepthStencil=0`, `AutoDepthStencilFormat=75`, `interval=ONE`,
   `Flags=0x1` (`LOCKABLE_BACKBUFFER`; with the retained back buffer this is now
   accepted without the removed PP probe and the pixels are real).
4. `IDirect3DDevice9::GetDeviceCaps` (caller `0x00cd2d32`).
5. Renderer checks caps at `0x00cd2d67` / `0x00cd2d73`:
   `[esi+0xd0] >= 2 && [esi+0xd4] >= 2`, where the caps copy starts at
   `[esi+0x3c]`, i.e. `D3DCAPS9.MaxTextureBlendStages` (148) and
   `MaxSimultaneousTextures` (152). With the honest `1/1` the engine prints
   "Unknown error creating the renderer." (`0x11585c0` / `0x1158568`).
6. With the stage probe (`2/2`) it proceeds:
   `GetRenderTarget` + surface `GetDesc` (`0x00ce8f49..80`),
   `CreateTexture` (caller `0x00ce69eb`): `Width=2, Height=1, Levels=1,
   Usage=0, Format=0x16 (A8R8G8B8), Pool=1 (MANAGED)`.
7. The engine then takes a level surface of that texture
   (`GetSurfaceLevel` slot 18, caller `0x00ce7135`), reads its `GetDesc`,
   `LockRect`/`UnlockRect` it (`0x00ce6a45..0x00ce6b80`) and releases it.
   This is the real reason the engine wants `LOCKABLE` style access: a RAM
   texture it fills through a level surface.
8. `SetTransform` VIEW (`0x00cd32d1`) and PROJECTION (`0x00cd34cc`), then
   `SetViewport` (`0x00cd3605`) -> `S_OK`.
9. The guest indirect call at `0x00f79dc2` targets `D3DXMatrixMultiply`.
   TESV's IAT was bound to the DLL export entry `0x0802fa75`, an MSVC
   lazy-ILT forwarder (`FF 25 <slot>` + five `CC` bytes). The slot holds the
   wrapper `0x0802fa5d`; the real body is `0x0802fa80` (entry+11). The wrapper
   and its one-time initializer are D3DX code, so binding the export entry
   traps on an address that no tier contains. `pe_link_resolve` now validates
   the exact hotpatch chain (export, five `CC`, wrapper `8B FF 55 8B EC / 6A 01
   / E8 rel32 / 5D / FF 25 <same slot>`, body prologue) and binds the body.
   The trace reads
   `[import] d3dx9_42.dll!D3DXMatrixMultiply -> 0802fa80 (export 0802fa75 hotpatch body)`.
   The slice registers the lifted `f_802FA80` (host and ARM); the multiply
   returns to `0x00cd3613` and `SetTransform`/`SetViewport` continue normally.

No explicit depth-stencil surface, no back-buffer `LockRect`, no draw and no
`Present` were reached yet.

## Next event after the hotpatch fix: the SM3 gate

With the multiply body dispatchable, the route now reaches renderer
initialization `0x00c6dbf0` and stops at the documented SM3 gate:

- `0x00c6e21e -> 0x00c71080` computes a shader level from the device's
  VS/PS version words. `0xc71080` returns 7 only when both major versions are
  `>= 3`; both `>= 2` returns 2.
- The honest caps are `VS_VERSION_2_0` / `PS_VERSION_2_0`
  (`source/d3d9_adapter.c`), so the level is 2.
- `0x00c6e22d: cmp dword ptr [0x01ba749c], 7` / `jge 0xc6e247`; otherwise
  `0x00c6e23d` pushes `0x114e04c` and jumps to the renderer failure path
  `0x00c6dfd3`.
- String at `0x114e04c`: **"ShaderModel3 required but not supported by your
  video card."** The guest then shows `Failed to initialize renderer.` and
  exits via `ExitProcess(0)` from `0x0069107d`.

This is an architectural gap, not a regression: the bridge reports VS/PS 2.0
and does not claim SM3 (`docs/MENU_SHADER_CLOSURE.md`). No draw or `Present`
occurs before this gate.

The bounded first-menu PS runtime is now wired
(`docs/MENU_SHADER_RUNTIME.md`): it selects only the exact certified
`p1200005` logo and menu-copy plans (plus fixed fog/tint contracts) and refuses
everything else by name, but it deliberately does not change the two version
words. This gate therefore still fails in the honest path; passing it requires
the real bounded SM3 execution path (VS family, material preconditions, PICA
texture units/vertex colour/constants), not a caps edit.

### Measurement-only post-SM3 census

`TESV_D3D9_SM3_GATE_PROBE=1` is a diagnostic, not a shipping path. The slice
byte-verifies `00c6e22d` (`83 3D 9C 74 BA 01 07`, `cmp [01ba749c],7`) and
`00c6e23b` (`7D 0A`, `jge 00c6e247`), then changes only the conditional jump to
`EB 0A` for the rest of that run. `GetDeviceCaps` and both version words stay
VS/PS 2.0. It exists solely to record the first post-gate D3D9 calls (shader
creates, depth, draws, `Present`) and is refused if the bytes differ. Do not
combine its output with a rendering claim.

Static post-gate chain (read-only analysis):

- `0x00c6dbf0` itself creates no shader, loads no asset and issues no draw or
  `Present`; success returns to `0x0069d97e -> 0x00696810 -> 0x00c74710`.
- `0x00c74710` opens `SHADERSFX/Shaders002.fxp`; the first record is `BSVS`
  `vs_3_0`, 436 bytes. `CreateVertexShader` is called at `0x00ccbbb3` and its
  HRESULT is ignored (nonfatal preload). The PS path later reads `BSPS` and
  calls `CreatePixelShader` at `0x00ccbfb3` / `0x00ccc3a7`.
- `0x00ca38d0` queues 124 lighting-package keys; while a menu closure is armed,
  any non-menu shader from that preload invalidates it. This is why the
  preflight-versus-observed certification order must be resolved before the
  closure can be the gate predicate.

Census result (104 s, exit 1, `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_census_final2.*`):

- The gate is passed and the SM3 message disappears; VS/PS stay 2.0 and the
  guest data word `[0x01ba749c]` remains `2`; the predecoded compare reads the
  probe's private shadow value `7`.
- **No shader is created.** `CreateVertexShader`/`CreatePixelShader` are not
  reached after the gate.
- The first post-gate D3D9 activity is the nonfatal `CheckDeviceFormat` probe
  loop at `0x00c6e447`, then `CreateTexture` with `D3DUSAGE_RENDERTARGET`
  (`caller 0x00ceca5e`, 1024x1024, levels 1, format `0x72` A32B32G32R32F,
  pool 0) returning `D3DERR_NOTAVAILABLE` (`0x8876086a`); the engine repeats
  1024/512/256 with formats `0x72`, `0x15` (A8R8G8B8) and `0x71`
  (A16B16G16R16F).
- After every target creation failed, the engine dereferenced the missing
  target at `0x00fb5bd0` and faulted on `[0x0000000c]`. Depth-stencil, draws and
  `Present` were never reached; only `RendererInfo.txt` was opened.

The LDR half of that census is now implemented: pow2 8..1024
X8R8G8B8/A8R8G8B8 render-target textures with one level and pool DEFAULT are
real Target-backed textures, `GetSurfaceLevel(0)` returns the target and
`SetTexture` samples the same colour buffer. Float/HDR render targets stay
named refusals. Re-running the census with this layer is the next measurement;
no shader or frame claim follows from the old trace.

### HDR fallback census (2026-11-26)

Base: `main` at `5e15944` (HDR fallback `ab4ec06`, size-checked depth descriptors
`5e15944`). No rebuild was performed for this run; the supplied binary was
`C:\Skyrim_N3DS_build\host_slice_hdrfb\run_tesv_slice_35040\tesv_slice.exe`
(480,013,824 bytes, built 05:49). The census used a 2,000,000,000-step budget and
ran for **92.56 s**, exit `1`:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_hdrfb_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_hdrfb_census.out`

The SM3 gate is passed and the fallback is active. Render-target requests from
`caller=0x00ceca5e`, all levels `1`, usage `1`, pool `0`:

| Size | Format | Count | HRESULT |
|---|---|---:|---|
| 1024x1024 | `0x72` A32B32G32R32F | 5 | `D3DERR_OUTOFVIDEOMEMORY` (`0x8876017c`) |
| 512x512 | `0x15` A8R8G8B8 | 3 | `S_OK` |
| 512x512 | `0x71` A16B16G16R16F | 1 | `D3DERR_OUTOFVIDEOMEMORY` (`0x8876017c`) |

The HDR fallback logs confirm that `0x72` and `0x71` were accepted as formats and
rewritten to A8R8G8B8 storage; creation then failed in the resource-budget layer.
The host null backend limit is 4 MiB (`source/d3d9_null.h:10`). The retained
400x240 back buffer occupies a 512x256 target (524,288 bytes). Three 512x512 LDR
targets add 3,145,728 bytes, leaving 524,288 bytes; the final 512x512 HDR target
needs 1,048,576 bytes. A 1024x1024 target needs 4,194,304 bytes and also cannot
fit alongside the retained back buffer.

The three successful LDR textures are `0x500021c0`, `0x500021e0`, `0x50002200`.
For each, `QueryInterface` (`caller 0x00cecb44`), `AddRef` (`0x00ce88c3`),
`GetSurfaceLevel(0)` (`0x00ce88fd`) and `GetDesc` (`0x00ce893f`) return `S_OK`;
the target handles are `15`, `17` and `19`, each 512x512.

Renderer initialization does **not** get past the final 512x512 `0x71` request.
Not reached: `CreateDepthStencilSurface`, `SetDepthStencilSurface`,
`SetRenderTarget`, `Clear`, `BeginScene`, `CreateVertexShader`,
`CreatePixelShader`, `SetTexture`, any draw, `Present`, `ValidateDevice`,
`SetMaterial` or `SetLight`. No FXP is opened and the first-draw capture contains
no records.

Consequently, sampling of the fallback textures is **not proven**: the `0x72`
requests never produced texture handles, the `0x71` request never produced a
handle, and no draw occurred. There are no `SetTexture` handles or draw-capture
entries to correlate.

The first failed call is the first 1024x1024 `0x72` creation (`OUTOFVIDEOMEMORY`),
which the engine tolerates while falling back. The fatal sequence is the final
512x512 `0x71` `OUTOFVIDEOMEMORY`, followed by a guest fault at PC `0x00fb5bd0`
on address `0x0000000c`. The last successful D3D9 call is
`IDirect3DTexture9::Release` on `0x50002200`, caller `0x00cecb99`, returning
refcount `3`; the last successful creation is the third 512x512 `0x15` target.

The next concrete blocker is the census backend's 4 MiB target budget, not HDR
format rejection. A larger measurement-only budget (or an env-gated budget
probe) is required before depth/scene/shader/draw sampling can be measured. The
fallback remains measurement-only and this is not a rendered-frame claim.

## Ordered next blockers

1. **SM3 renderer gate (`0x00c6e22d`).** Requires level 7 (VS/PS 3.0) while the
   bridge honestly reports 2.0/2.0. Flipping the two version words would only
   pass this branch; it would not make the device accept arbitrary `ps_3_0`
   programs, so the documented decision is to keep 2.0/2.0 until a real SM3
   execution path exists.
2. **Other D3DX hotpatch bodies.** The loader now follows the one-time
   initializer's real-body table, so it no longer mistakes `entry+11` for the
   body on exports such as `D3DXVec3TransformCoord`. All nine lazy-ILT imports
   are lifted: `D3DXMatrixMultiply` `0x0802fa80`, `D3DXMatrixTranspose`
   `0x0802fce0`, `D3DXMatrixMultiplyTranspose` `0x0802fd9d`,
   `D3DXPlaneTransform` `0x08032303`, `D3DXVec3TransformCoord` `0x08032ead`,
   `D3DXVec3TransformNormal` `0x0803306a`, `D3DXVec3Normalize` `0x08032cbf`,
   `D3DXPlaneNormalize` `0x08032000` and `D3DXMatrixInverse` `0x0802fffd`
   (plus the internal helper `0x0802e9db` and the `_CIsqrt`/`_finite` shims
   `0x081a65ec`/`0x081a6694`; twelve table entries). Each has its own 3/3
   pilot test. The nine plain D3DX exports (image/texture load, surface load,
   `D3DXCompileShader`) are not lazy-ILT thunks and still need host shims or a
   lifted closure. Generation, merge and per-body test details are in
   `docs/D3DX_PILOT_TABLE.md`; `pilot_out*/` is generated locally and ignored.
3. **HW T&L shipping gate (`0x00c6d8c6`).** Still deferred
   (`docs/HWTL_DECISION.md`); the probe is measurement-only.
4. **Two texture stages (honest).** Implemented: a backend with `draw_two`
   makes the COM caps report `2/2`; without it the renderer refuses the device
   (step 5 is the historical failure).
5. **Small textures.** Implemented: power-of-two sizes below 8 are stored in a
   minimum 8x8 allocation with replicated texels and point sampling at level 0;
   logical dimensions and row pitch stay honest.
6. **Level surfaces.** Implemented: `GetSurfaceLevel` exposes a CPU-backed view
   of the level; writing it re-uploads the parent texture. BC levels are
   refused by name.

## Status of this patch

- Hotpatch resolution has a host regression test (positive chain, a one-byte
  padding negative and a `push 2` initializer-guard negative). The Windows
  suite is **61 passed, 1 diagnostic, 0 failed**; the diagnostic is
  `tesv_slice` hitting the harness's 200k-step budget, not a fault in the fixed
  route (the CAPS+PP probe uses the larger budget and reaches the SM3 gate).
- With the probes on, the route reaches the multiply body, executes it, and
  continues to the SM3 gate with `CreateTexture`/`GetSurfaceLevel`/`LockRect`
  returning `S_OK`.
- A final rerun on `d6f5bf3` (`E:\Skyrim_N3DS_build\analysis\trace4.err`,
  copied to `F:\Skyrim\diag\20261126_hotpatch\`) reproduces the import binding
  and the same SM3 message.
- Shipping behaviour is unchanged: still gated at `0x00c6d8c6` until the
  `HW T&L` decision (`docs/HWTL_DECISION.md`).
