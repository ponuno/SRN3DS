# HW T&L decision: measurement, options and the current position

Date: 2026-11-25. Status: **not claimed; measurement complete, decision open.**

## 1. What the caps bit means

The Windows SDK (`shared/d3d9caps.h`) defines:

```c
#define D3DDEVCAPS_DRAWPRIMITIVES2     0x00002000L
#define D3DDEVCAPS_DRAWPRIMITIVES2EX   0x00008000L
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT 0x00010000L /* ... and DRAWPRIMITIVES2EX must be also */
```

`D3DDEVCAPS_HWTRANSFORMANDLIGHT` is a promise that the fixed-function
transform **and lighting** pipeline exists: `SetLight`, `LightEnable`,
`SetMaterial`, normals, per-vertex diffuse/specular accumulation, and the
`D3DCAPS9` fields that describe that pipeline (`MaxActiveLights`,
`MaxUserClipPlanes`, `ShadeCaps`, ...).

## 2. What this bridge implements

- One clip-matrix transform in the PICA200 vertex shader (`d9shader.v.pica`).
- No fixed-function light/material state, no normals in the fetch path, no
  per-vertex lighting. `SetLight`/`SetMaterial`/`LightEnable`/
  `MultiplyTransform` remain unimplemented and fail explicitly.
- `DevCaps = 0x00082000` (`DRAWPRIMITIVES2 | HWRASTERIZATION`). Both claimed
  bits are truthful: the four documented draw entry points run, and PICA200
  really rasterises the submitted triangles.

Claiming `0x10000` today would tell the engine that a lighting pipeline exists
that does not. That is the fake-success path this project forbids.

## 3. Measurement (measurement only, not shippable)

`TESV_D3D9_CAPS_PROBE=1` makes `GetDeviceCaps` additionally report
`D3DDEVCAPS_HWTRANSFORMANDLIGHT` and prints a one-shot
`MEASUREMENT ONLY ... not shippable` line. The default caps are unchanged.
Runs: TESV 2011 host slice, `TESV_MEMPROBE=1`, `TESV_D3D9_TRACE=1`,
2,000,000,000-step budget.

Observed sequence once the gate passes (`E:\Skyrim_N3DS_build\analysis\capsprobe_tesv4.err`):

1. `0x00c6d8c6` gate passed (no "Hardware T&L required" message).
2. VS/PS version gate `0x00c6d8c8/0x00c6d8db` passed with the reported 2.0 caps.
3. Adapter/mode/format enumeration completed (36 `CheckDeviceFormat`, 68
   `CheckDeviceMultiSampleType`, all from `0x00ce54f4`/`0x00ce560f`/`0x00cd2c1d`).
4. `0x00c6dd37` called `AdjustWindowRect` — and faulted on a NULL RECT because
   the shim read `dwStyle` as the rectangle pointer. **Fixed** (shim signature
   corrected; `win32_user` test updated).
5. `CreateDevice` (factory slot 16) was reached four times, caller
   `0x00cd8b74`, with:

```text
Adapter=0  DeviceType=1 (HAL)  hFocusWindow=0x00010001
BehaviorFlags=0x54 / 0x44 / 0x84 / 0x24   (MULTITHREADED | FPU_PRESERVE, then PURE, then SOFTWARE_VERTEXPROCESSING retries)
BackBuffer=400x240  Format=22 (X8R8G8B8)
BackBufferCount=1  MultiSample=0  Quality=0  SwapEffect=1 (DISCARD)
Windowed=1  EnableAutoDepthStencil=0  AutoDepthStencilFormat=75 (D24S8, unused)
Flags=0x00000001 (D3DPRESENTFLAG_LOCKABLE_BACKBUFFER)
Refresh=0  PresentationInterval=1 (ONE)
```

6. All four calls were rejected by **our own** `CreateDevice` validation with
   `0x8876086a` (`D3DERR_NOTAVAILABLE`), reason
   `presentation flags other than none are not implemented`.
7. The engine then showed "Failed to initialize renderer." and exited from
   `0x0069107d`.

Key facts:

- TESV does **not** ask for an automatic depth-stencil surface; it will create
  one explicitly after the device exists.
- The only unsupported presentation parameter is the lockable back buffer.
- No `SetLight`/`SetMaterial`/`LightEnable` call occurs before the device
  exists (they cannot: there is no device). Whether the engine ever issues them
  after `CreateDevice` is still unmeasured, because the device is not created
  yet. That is the one remaining question for option B below.

## 4. Options

**A. Implement real fixed-function lighting and claim the bit.**
Precompiled PICA vertex-shader variants for the FFP state subset (N
directional/point/spot lights, material, ambient, specular, fog), normal and
diffuse/specular fetch, truthful `D3DCAPS9` fields, `DRAWPRIMITIVES2EX`
conformance. Large, but the only way to claim `0x10000` without a lie. Not
started.

**B. Documented engine adaptation.**
If the post-device census shows TESV never uses FFP lighting/material state, the
port may treat the caps requirement as an engine check that does not match the
device class it actually drives, and patch that one branch (byte-verified,
logged, reversible) while keeping `SetLight`/`SetMaterial`/`LightEnable` as
explicit failures. This is an engine change, not a hardware lie, and needs
explicit approval. Requires the post-device census first.

**C. Stop at the gate.**
Keep the truthful caps and accept that TESV shows "Failed to initialize
renderer." This is the current shipping behaviour.

## 5. Position

- Shipping caps stay `0x00082000`; the probe is measurement-only.
- The renderer features both A and B need are now implemented:
  a **lockable retained back buffer** (offscreen RGBA8 target plus a present
  blit and real LockRect read/write) and **explicit depth-stencil surfaces**.
  See `docs/BACK_BUFFER_LOCKABLE.md`.
- `docs/HWTL_IMPLEMENTATION.md` records the minimum honest Option A scope
  (~4,000-6,000 LOC plus a pixel-proof campaign and DX7 `DRAWPRIMITIVES2EX`
  conformance). It does not fit the current pass, so it is not implemented.
- The 2026-11-26 census reached `CreateDevice` `S_OK` but the engine aborts at
  the SM3 gate `0x00c6e22d` before any render pass, depth surface, Clear or
  fixed-function lighting call. The A/B census is therefore still empty; SM3 is
  the next renderer blocker. Details: `docs/HWTL_IMPLEMENTATION.md` section 5.
- Do not decide A vs B before that census. If the engine issues no FFP lighting
  calls, Option B (a byte-verified engine adaptation) is preferred over a
  hardware-T&L claim. A software T&L implementation must keep the caps bit
  clear, because the bit means hardware T&L.
