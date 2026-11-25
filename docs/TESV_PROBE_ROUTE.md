# TESV route behind the measurement probes (2026-11-25)

This note records what the engine does once the two honest gates no longer stop
it. Every value below is measured on the host slice with the probes enabled;
the probes are measurement-only and must never become shipping defaults.

## Probes

| Variable | Effect | Shipping |
|---|---|---|
| `TESV_D3D9_CAPS_PROBE=1` | `GetDeviceCaps` adds `D3DDEVCAPS_HWTRANSFORMANDLIGHT` | never |
| `TESV_D3D9_PP_PROBE=1` | accepts `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`, gives the back buffer a zero-filled shadow, accepts a no-op depth surface | never |
| `TESV_D3D9_STAGE_PROBE=1` | reports `MaxTextureBlendStages = MaxSimultaneousTextures = 2` (backend still binds one) | never |

Each prints a one-shot `MEASUREMENT ONLY ... not shippable` line.

## Measured sequence (evidence: `E:\Skyrim_N3DS_build\analysis\stageprobe_tesv.err`)

1. `0x00c6d8c6` HW T&L gate passed (probe).
2. Adapter/format enumeration; `0x00cd5542` probes 80 texture/cube formats.
3. `CreateDevice` (factory slot 16, caller `0x00cd8b74`) -> **`S_OK`** with
   `400x240 X8R8G8B8`, `DISCARD`, 1 back buffer, no MSAA, windowed,
   `EnableAutoDepthStencil=0`, `AutoDepthStencilFormat=75`, `interval=ONE`,
   `Flags=0x1` (`LOCKABLE_BACKBUFFER`, PP probe).
4. `IDirect3DDevice9::GetDeviceCaps` (caller `0x00cd2d32`).
5. Renderer checks caps at `0x00cd2d67` / `0x00cd2d73`:
   `[esi+0xd0] >= 2 && [esi+0xd4] >= 2`, where the caps copy starts at
   `[esi+0x3c]`, i.e. `D3DCAPS9.MaxTextureBlendStages` (148) and
   `MaxSimultaneousTextures` (152). With the honest `1/1` the engine prints
   "Unknown error creating the renderer." (`0x11585c0` / `0x1158568`).
6. With the stage probe (`2/2`) it proceeds:
   `GetRenderTarget` + surface `GetDesc` (`0x00ce8f49..80`),
   `CreateTexture` (caller `0x00ce69eb`): `Width=2, Height=1, Levels=1,
   Usage=0, Format=0x16 (A8R8G8B8), Pool=1 (MANAGED)` -> our
   `D3DERR_INVALIDCALL` (we reject sizes below 8 or non-power-of-two).
   The engine continues after this failure.
7. `SetTransform` VIEW (`0x00cd32d1`) and PROJECTION (`0x00cd34cc`), then
   `SetViewport` (`0x00cd3605`) -> `S_OK`.
8. Immediately after, a guest indirect call to **`0x0802fa75`** faults with
   `unknown indirect function`. That address is inside the `d3dx9_42.dll`
   image (`0x08000000`), 11 bytes before the known `D3DXMatrixMultiply` body at
   `0x0802fa80`; it is not a registered function start.

No explicit depth-stencil surface, no back-buffer `LockRect`, no draw and no
`Present` were reached yet.

## Ordered next blockers

1. **Two texture stages (honest).** Claim `2/2` only with real stage 1:
   `SetTexture(1)`, `SetTextureStageState(1, …)`, two bound textures in the
   PICA backend and a TEV combine. Until then the renderer refuses the device
   (step 5), so nothing else matters.
2. **Small textures.** Accept power-of-two sizes below 8 (the engine asks for
   `2x1`) by rounding the allocation up to the PICA minimum (8x8) while keeping
   the logical dimensions and row pitch honest. Do not silently clamp the
   reported size.
3. **Guest indirect call `0x0802fa75`.** A D3DX internal target is called that
   the generated function table does not contain. Either it must be discovered
   and lifted, or the call site must resolve to the hotpatch body
   `0x0802fa80`. This is a recompilation-coverage gap, not a D3D9 capability.

## Acceptance for the next patch

- Host tests for stage 1 state validation, two-texture draws and small-texture
  creation (logical size preserved, allocation rounded, explicit refusals).
- With the probes on, the route must reach the `0x0802fa75` call with
  `CreateTexture` returning `S_OK` and no renderer error; then record the next
  device call.
- Shipping behaviour is unchanged: still gated at `0x00c6d8c6` until the
  `HW T&L` decision (`docs/HWTL_DECISION.md`).
