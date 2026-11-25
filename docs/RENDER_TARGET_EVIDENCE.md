# Render-target path evidence

Date: 2026-11-25. Scope: one explicit LDR `D3DUSAGE_RENDERTARGET` /
`D3DRTYPE_SURFACE`, not a Skyrim frame.

## Implemented contract

- Adapter: `Adapter=0`, `HAL`, adapter format `X8R8G8B8`, usage exactly `1`,
  resource type `SURFACE (1)`, format `A8R8G8B8 (0x15)` or `X8R8G8B8 (0x16)`
  returns `D3D_OK`.
- Core: up to four explicit handles, one active target, handle zero selects the
  implicit back buffer, exact aggregate memory accounting and native teardown.
- COM: `CreateRenderTarget`, `SetRenderTarget` including `NULL` restore,
  `GetRenderTarget`, `GetDesc`, `GetContainer`, and read-only `LockRect` for
  lockable targets. The selected COM surface holds a device-side reference.
  A queued Clear cannot be redirected to another target, and BeginScene clears
  an explicit target only when Clear was requested.
- PICA: VRAM `GPU_RGBA8` texture plus `C3D_RenderTargetCreateFromTex(..., -1)`.
  Explicit targets have no depth attachment. Readback uses synchronized texture
  copy and the inverse PICA tile transform.
- Null backend: real handles/lifetime/counters, non-lockable targets only, no
  readback callback, and no invented pixels.

Rejected explicitly: texture render targets, combined usage bits, MSAA,
non-power-of-two or 8..1024-out-of-range dimensions, shared handles, lockable
targets without backend readback, writable RT locks, depth/stencil, MRT, resolve,
`StretchRect`, `ColorFill`, and RT sampling.

## Host regression

Full host suite: **57 passed, 0 diagnostic, 0 failed** on 2026-11-25 after the
UP-draw and GetViewport fixes. Targeted ASan/UBSan passes 12/12 for
`render_target`, `null_backend` and `d3d9_capture`; the UP test is also
ASan-clean.

- `render_target`: swizzle/unswizzle round-trip, rectangle readback, independent
  target clears, back-buffer restore, budget, slot limit, backend failure and
  teardown.
- `null_backend`: COM target creation/description/container, bound-target
  lifetime, restore, lock policy, reset rejection while resources are alive,
  capture integration and null readback refusal.
- `adapter`, `device_create`, `renderer_requirements`: exact accepted/rejected
  format shapes and absence of backend capability with an old six-callback
  backend.
- `d3d9_capture`: schema version 2 target section and standalone Present files.

## ARM/Azahar pixel proof

Artifact: `tests/isolated/uicopy/rt_switch_arm.3dsx`

- Size: 730,412 bytes
- SHA-256: `bdc43ec480275cbb1447e2e4fb94e4e851fb344cce64af9384c11a747d1074a5`
- Evidence: `build_azahar/20261125-211721-rt_switch_arm/`

Result:

```text
render-target ARM: PASS (38 checks, 0 errors)
target_a=0x50002070 target_b=0x50002080 back=0x50002020 size=64x64
red=1 green=1 a_unchanged=1 b_unchanged=1 restore=1 identity=1
draw_calls=1 scene=0 active_target=0
PIXEL_PROOF=PASS detail=unknown=0 bad_counts=0 bad_shapes=0 expected_each=64
pixel_fnv1a32=54d9adc5
```

The existing `ui.copy` proof remains unchanged in result: 128x128, 16 checks,
FNV `e34b4dc5` (`build_azahar/20261125-211740-uicopy_arm`).

## TESV host route

Source trace: `E:\Skyrim_N3DS_build\analysis\rt_tesv_host2.err`.
Full before/after table: `E:\Skyrim_N3DS_build\analysis\D3D9_RENDER_TARGET_TRACE.md`.

- Same `E:\skyr\TESV.exe`, `F:\Skyrim` data/settings, IR container, memory probe,
  capture setting and 2,000,000,000-step budget.
- Host binary SHA-256:
  `d31fe77ee96372122867aa2a7e2c70d7c0eb970bb8b640250f7a8fc5a56ec665`.
- Wall time: 190.1 s on the final binary with UP draws and GetViewport slot 48.
- Before: all 18 `RENDERTARGET/SURFACE` probes from TESV `0x00ce54f4` returned
  `D3DERR_NOTAVAILABLE`.
- After: `0x15` and `0x16` return `D3D_OK`; the other 16 remain unavailable.
  TESV enters the success-only depth/MSAA probes at `0x00ce560f`, which remain
  correctly unavailable.
- Next blocker: HAL caps check at `0x00c6d8c6` requires
  `D3DCAPS9.DevCaps & 0x00010000` (`HWTRANSFORMANDLIGHT`). The bridge does not
  claim it without the full transform/lighting and `DRAWPRIMITIVES2EX` contract.
  A later check at `0x00c6e22d` requires SM3.
- `CreateDevice`, the possible `0x00cd8c04` retry, first draw and first Present
  were not reached. Capture produced no files, correctly reflecting the route.

## ARM TESV artifact

- `.3dsx`: `Skyrim_N3DS_tesv_rtup.3dsx`, 2,012,180 bytes, SHA-256
  `8b594039cea37b305fc05947fb82d063a6d28167caf9cd8e33cea86467865518`.
- ELF SHA-256:
  `5528ca8b9794a8ae2c5c1b46e7ec35e11b474f52862ef47d75fb3c2e2cb5eae3`.
- 178 MiB-mode CIA: `F:\cias\Skyrim_N3DS_tesv_rtup.cia`, 1,175,488 bytes,
  SHA-256 `f1b755bedce2f70fafc22f19057e7cb4d2798f2cae805ed396001b726d81cc61`.
- Built with `TESV_ARM_STEPS=2000000000` and the measurement-only memory probe.
  CIA packaging is not physical-device validation.

## Azahar TESV 2B parity

The final UP/GetViewport ARM binary with the same 2B budget completed the route
in Azahar within the 900-second window
(`build_azahar/20261125-214451-tesv_rtup_2b`):

```text
run rc=0
guest_used=52578496 steps=2000000000
Guest ExitProcess with status 0
exit code=0 from=0069107d
IR cache: blocks=48568 ... hits=552276848 misses=48568 evicted=4186192
```

This is the same critical exit as the host route: the renderer caps gate at
`0x0069107d`. The runner labels it FAIL only because `-AllowFaultReport` expects
a PASS marker or a guest fault; the application log itself records a clean
diagnostic exit. The earlier pre-UP binary produced the same 2B result.

## Limits

No physical New 3DS/CIA validation was performed for this change. Offscreen
orientation/scale is not proven; the pixel proof uses a square target and a
fullscreen triangle. Explicit targets have no depth. The host TESV route uses a
measurement-only 512 MiB commit probe and is not a shippable memory claim.
