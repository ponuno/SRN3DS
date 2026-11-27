# NPOT render-target textures

GFx (Scaleform) asks D3D9 for offscreen render-target textures at the screen's
logical size along with small helper targets (measured by the trace-enabled
census: `400x240`-class menu surfaces plus `2x2` and `1x1` helper targets).
The PICA200 / citro3d allocation API
(`C3D_TexInitWithParams`) only accepts power-of-two texture dimensions, so the
bridge stores an NPOT surface in a larger POT allocation and treats the logical
surface as a sub-rect.

## Contract

For `CreateTexture(w, h, levels=1, usage=D3DUSAGE_RENDERTARGET,
format=A8R8G8B8|X8R8G8B8, pool=D3DPOOL_DEFAULT)`:

* `w,h` are the D3D logical surface size (1..1024); `GetDesc`, `GetRenderTarget`,
  `GetRenderTargetData`, `LockRect`, scissor and viewport all use it.
* Native storage is `storage_w = ceil_pow2(max(w,8))`,
  `storage_h = ceil_pow2(max(h,8))`, clamped to 8..1024 per axis.  Sub-8
  logical surfaces (the scaleform 2x2 and 1x1 helper targets) therefore get an
  8x8 allocation.  If either axis would exceed 512 the historical
  bounded/down-scaled path is used instead (native = `min(logical, cap)` and
  the scaled viewport fills the whole storage).
* The logical image occupies the **top-left** `w x h` corner.  The padding
  columns/rows are never rasterized or sampled, but they belong to the
  allocation.
* Rendering (`SetRenderTarget` + `BeginScene`/`Draw`) programs a viewport of
  `w x h`; scissor rectangles are therefore expressed in logical pixels.
* Sampling a render-target texture scales normalized UVs by
  `w / storage_w` and `h / storage_h` in the core draw path (`D9Vertex.u/v`
  and `D9ColorVertex.u/v`).  Guest UVs in `0..1` map to the logical
  sub-rect only; UV coordinates can never reach the padding.
* `StretchRect` and `GetRenderTargetData`/`LockRect` address the logical
  sub-rect, not the whole POT allocation.  No `TESV_D3D9_SCALED_READBACK`
  opt-in is needed for a pot-padded target because the copy is exact.
* `Clear` writes the whole POT allocation.  This is safe because the padding
  is not sampleable; edge alpha of the logical image is unaffected.

## Examples

| logical | storage | UV scale |
|---|---|---|
| 400x240 | 512x256 | 0.78125, 0.9375 |
| 320x180 | 512x256 | 0.625, 0.703125 |
| 100x100 | 128x128 | 0.78125, 0.78125 |
| 2x2 | 8x8 | 0.25, 0.25 |
| 1x1 | 8x8 | 0.125, 0.125 |
| 256x256 | 256x256 | 1, 1 (exact, unchanged path) |
| 1024x1024 | 512x512 | bounded/down-scaled (unchanged path) |

## Implementation map

* `d9_create_render_texture` (`source/d3d9_core.c`) computes the POT storage,
  creates a `Target` with `pot_pad`, and records `storage_w/storage_h` on the
  sampling `Texture`.
* `apply_target_viewport` uses the logical size for `pot_pad` targets.
* `d9_render_target_read` / `d9_render_target_write` use the exact sub-rect.
* `d9_blit` already passes logical + storage dimensions in `D9BlitSurface`;
  the citro3d and null backends map logical rectangles into the storage and
  sample with the storage-sized UV denominator.
* `d9_citro3d.c` needs no new callback: `target_create` receives POT
  dimensions, `target_blit`/`target_read` use the storage fields.

## Diagnostics

Set `TESV_D3D9_RT_TRACE=1` to log every render-target `CreateTexture` request
and its resulting storage:

```
[rt-trace] CreateTexture w=400 h=240 levels=1 format=0x15 pool=0 usage=0x1
[rt-trace]   -> OK logical 400x240 storage 512x256
[rt-trace] CreateTexture w=1 h=1 levels=1 format=0x71 pool=0 usage=0x1
[rt-trace]   -> OK logical 1x1 storage 8x8
```

## Limits

* One level, `D3DPOOL_DEFAULT`, `D3DUSAGE_RENDERTARGET`,
  `A8R8G8B8`/`X8R8G8B8` only (float/HDR stays a named refusal or the
  measurement-only LDR fallback).
* A logical axis above 512 uses the capped/down-scaled storage, so sampling
  fills the whole native image instead of a sub-rect.
* `UpdateSurface`/`ColorFill` on a pot-padded target use the same exact
  sub-rect read/modify/write path as `UnlockRect`.
