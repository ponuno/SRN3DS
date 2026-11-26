# Device bring-up notes (Azahar 2126.2, New 3DS)

This note records what the port actually does when launched as a `.3dsx` in
Azahar with the real game data on `sdmc:/Skyrim`. It is not a playable build
and no frame or pixel result is claimed.

## Boot path reached on the device

1. CIA/3DSX starts, PICA200 backend opens, D3DX shims register, the IR
   container loads from `sdmc:/Skyrim/ir/tesv.irc`.
2. Guest memory initialises. The first device run failed because the guest
   slab table was capped at 8 (`guest_slab_commit` could not find the 9th
   reservation). Raising `GUEST_MAX_SLABS` to 32 and the newlib heap to
   96 MiB (guest budget 92 MiB) removed that failure.
3. The engine opens `Skyrim - Interface.bsa`, `Animations.bsa`, `Meshes.bsa`
   and the other archives. Paths arrive both as `C:\Skyrim\...` and as
   `sdmc:/Skyrim\...` with mixed separators and trailing `\`; the sandbox
   mapper now accepts all of those forms (and strips the duplicated
   `Skyrim` component).
4. Renderer initialisation then stops on the documented honest gates:
   - `Hardware T&L required but not supported by Adapter.`
   - after the T&L gate: `ShaderModel3 required but not supported by your
     video card.`
   Both are intentional: `DevCaps` stays `0x00082000` and SM3 execution does
   not exist.

## Measurement-only probe switch

`source/tesv_slice_main.c` checks for `sdmc:/Skyrim/tesv_caps_probe.flag`.
When the file exists, it sets:

```text
TESV_D3D9_CAPS_PROBE=1        # advertise HW T&L (fictional)
TESV_D3D9_SM3_GATE_PROBE=1    # pass the 0x00c6e23b SM3 gate
TESV_D3D9_SHADER_SKIP_PROBE=1 # code-less shader objects
```

Every probe logs that it is measurement-only and not shippable. With the flag
present the device passes the T&L and SM3 checks, creates the shader objects
and then faults at `Guest fault at 00000030: unmapped guest memory` — a NULL
renderer pointer past the shader layer, before a frame.

`stderr` is mirrored to `sdmc:/Skyrim_N3DS_stderr.log` for these runs; the
final `sdmc:/Skyrim_N3DS_tesv.log` carries `rc`, exit code/from and the
unresolved-import list.

## Host census state (same route, host slice)

The host census now reaches `SetRenderTarget` and `SetDepthStencilSurface`.
The engine binds a 512x1024 depth while the implicit back buffer is 400x240;
the bridge returns `D3DERR_INVALIDCALL` with both sizes. The engine then
recurses through `NiDX9Renderer::BeginUsingRenderTargetGroup` until the 64 KiB
worker stack overflows at `0x1d1ffffc`. The remaining work is the pair itself
(a real 512x1024 colour target/group registration), not the error code.

## Not implemented / not claimed

- No HW T&L and no SM3 execution; both gates are honest refusals.
- No 512x1024 colour render target/group for the depth pair.
- No frame, no pixel comparison, no hardware run.
