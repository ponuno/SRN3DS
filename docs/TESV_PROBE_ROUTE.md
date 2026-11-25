# TESV route behind the measurement probes (2026-11-25)

This note records what the engine does once the two honest gates no longer stop
it. Every value below is measured on the host slice with the probes enabled;
the probes are measurement-only and must never become shipping defaults.

## Probes

| Variable | Effect | Shipping |
|---|---|---|
| `TESV_D3D9_CAPS_PROBE=1` | `GetDeviceCaps` adds `D3DDEVCAPS_HWTRANSFORMANDLIGHT` | never |
| `TESV_D3D9_PP_PROBE=1` | accepts `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`, gives the back buffer a zero-filled shadow | never |

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
   Usage=0, Format=0x16 (A8R8G8B8), Pool=1 (MANAGED)`.
7. The engine then takes a level surface of that texture
   (`GetSurfaceLevel` slot 18, caller `0x00ce7135`), reads its `GetDesc`,
   `LockRect`/`UnlockRect` it (`0x00ce6a45..0x00ce6b80`) and releases it.
   This is the real reason the engine wants `LOCKABLE` style access: a RAM
   texture it fills through a level surface.
8. `SetTransform` VIEW (`0x00cd32d1`) and PROJECTION (`0x00cd34cc`), then
   `SetViewport` (`0x00cd3605`) -> `S_OK`.
9. Immediately after, a guest indirect call to **`0x0802fa75`** faults with
   `unknown indirect function`. That address is inside the `d3dx9_42.dll`
   image (`0x08000000`), 11 bytes before the known `D3DXMatrixMultiply` body at
   `0x0802fa80`; it is not a registered function start.

No explicit depth-stencil surface, no back-buffer `LockRect`, no draw and no
`Present` were reached yet.

## Ordered next blockers

1. **Guest indirect call `0x0802fa75`.** A D3DX internal target is called that
   the generated function table does not contain. Either it must be discovered
   and lifted, or the call site must resolve to the hotpatch body
   `0x0802fa80`. This is a recompilation-coverage gap, not a D3D9 capability.
2. **Two texture stages (honest).** Already implemented: a backend with
   `draw_two` makes the COM caps report `2/2`; without it the renderer refuses
   the device (step 5 is the historical failure).
3. **Small textures.** Implemented: power-of-two sizes below 8 are stored in a
   minimum 8x8 allocation with replicated texels and point sampling at level 0;
   logical dimensions and row pitch stay honest.
4. **Level surfaces.** Implemented: `GetSurfaceLevel` exposes a CPU-backed view
   of the level; writing it re-uploads the parent texture. BC levels are
   refused by name.

## Acceptance for the next patch

- Host tests for stage 1 state validation, two-texture draws, small-texture
  creation and level-surface lock/unlock/upload.
- With the probes on, the route reaches the `0x0802fa75` call with
  `CreateTexture` and `GetSurfaceLevel` returning `S_OK` and no renderer error;
  then record the next device call.
- Shipping behaviour is unchanged: still gated at `0x00c6d8c6` until the
  `HW T&L` decision (`docs/HWTL_DECISION.md`).
