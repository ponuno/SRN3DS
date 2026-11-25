# D3D9 UP draws and HW T&L decision

Date: 2026-11-25.

## Implemented

`DrawPrimitiveUP` (device slot 83) and `DrawIndexedPrimitiveUP` (slot 84) are
implemented for the existing FVF `XYZ|TEX1` / declaration subset. They borrow
guest stream-zero vertices (and optional UP indices) for one call, decode through
the same core fetch path as buffer draws, and do not modify the bound stream 0,
index buffer, texture, transforms, render state or COM refcounts.

Malformed primitive types/counts, null or out-of-range guest ranges, zero/oversize
strides, invalid index formats, vertex-range overflow and indices outside
`[MinVertexIndex, MinVertexIndex+NumVertices)` fail with
`D3DERR_INVALIDCALL` and a named diagnostic.

Tests:

- `up_draw`: list/strip/fan, INDEX16/32, padded/declaration strides, malformed
  inputs, state/refcount/budget preservation.
- `com`: slots 83/84 through the real guest x86 vtable.
- `adapter`: `DevCaps == 0x00082000`; `0x8000` and `0x10000` remain clear and
  have explicit positive/negative reason strings.
- ASan: clean for the UP test.

## Why `HWTRANSFORMANDLIGHT` is still not claimed

The Windows SDK defines `D3DDEVCAPS_HWTRANSFORMANDLIGHT` as hardware
transformation **and lighting**, and requires `DRAWPRIMITIVES2EX`. The PICA path
has one clip-matrix vertex transform. `SetLight`, `LightEnable`, `SetMaterial`,
normals and per-vertex lighting are absent. Setting the bit would tell TESV that
the fixed-function lighting pipeline exists, then fail on the next lighting
calls. It remains clear.

`DRAWPRIMITIVES2EX` is also not claimed: it is a DX7 extended-DrawPrimitives2
conformance claim, not merely four working entry points. `DRAWPRIMITIVES2`
(`0x2000`) is now truthful because all four documented draw entry points run.

## Related COM slot fix

`GetViewport` is slot 48. It had been registered at slot 46
(`MultiplyTransform`), leaving the real slot unsupported. This is corrected;
tests call slot 48.

## GFx pre-transformed batch admission

The Scaleform UI batches are now admitted without changing the caps:

- `CreateVertexBuffer`/`CreateIndexBuffer` accept `D3DUSAGE_DYNAMIC` (`0x208`)
  as CPU-staged storage: every `Lock`/`Unlock` copies, the upload is deferred
  to the draw.
- `CreateTexture` accepts `D3DUSAGE_DYNAMIC` (`0x200`) and re-uploads the level
  on `UnlockRect`.
- `SetFVF`/`d9_decl_from_fvf` admit `0x102`, `0x104`, `0x142`, `0x144`
  (`XYZ`/`XYZRHW` × optional `DIFFUSE` × `TEX1`). `XYZRHW` requires `FLOAT4`;
  the viewport→PICA clip mapping is `positiont_matrix`.
- `COLOR0` is decoded as normalized RGBA (`D3DCOLOR`, `UBYTE4`, `UBYTE4N`,
  `FLOAT4`). `COLOR1` is accepted as a declaration element but is not a primary
  colour source (specular lighting input is not implemented).
- `SetTextureStageState` (slot 67) stores stage-0
  `DISABLE/SELECTARG1/SELECTARG2/MODULATE` over `DIFFUSE/TEXTURE/CURRENT` and
  requires stages 1..7 to be `DISABLE`.
- A draw that needs `COLOR0` is refused with `D3DERR_NOTAVAILABLE` when the
  backend has no `draw_colored` entry, naming `COLOR0`; the tint is never
  dropped silently.

## Measurement-only caps probe

`TESV_D3D9_CAPS_PROBE=1` makes `GetDeviceCaps` additionally report
`D3DDEVCAPS_HWTRANSFORMANDLIGHT` and prints a one-shot
`MEASUREMENT ONLY ... not shippable` line. It exists only to record what the
engine does once the gate passes; the default caps value is unchanged
(`0x00082000`) and no shipping build may set the variable. Evidence and the
resulting architectural decision are in `docs/HWTL_DECISION.md`.

## Remaining work for an honest HW T&L claim

- real fixed-function transform/lighting or a documented equivalent that the
  engine actually uses;
- material/light APIs, normals, DrawPrimitives2EX conformance;
- then SM3/menu closure work, which is independent and still not a global SM3
  claim.
