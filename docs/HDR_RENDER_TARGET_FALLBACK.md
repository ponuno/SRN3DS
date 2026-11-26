# Float render targets: measurement fallback

Status: **measurement only, not shippable, disabled by default.**

TESV's Gamebryo renderer creates `NiRenderedTexture` objects through
`NiDX9RenderedTextureData::Create` (`0x00cec960`). During renderer
initialisation the measured set is:

| Size | Format | Count | Result without fallback |
|---|---|---|---|
| 1024x1024 | `0x72` `R32F` | 5 | `D3DERR_NOTAVAILABLE` (tolerated) |
| 512x512 | `0x15` `A8R8G8B8` | 3 | `S_OK` |
| 512x512 | `0x71` `A16B16G16R16F` | 1 | `D3DERR_NOTAVAILABLE`, then a null dereference at `0x00fb5bd0` |

`bDoHighDynamicRange=0`, `bUseBlurShader=1` and `iShadowMode=0` do **not** remove
these requests. They are renderer-init shadow resources, not HDR/bloom: five
1024 `R32F` shadow maps plus a quarter-res shadow-mask set. The five `0x72`
failures are checked (the renderer logs a null target); after the last 2D
creation the engine queries a `NiRenderedCubeMap` face table through
`0x00fb5bd0` and faults on a stale entry, before any shader, draw or `Present`.

## The switch

`TESV_D3D9_HDR_LDR_FALLBACK=1` is read once in `d9_com_init` (host/test only).
When set, `CreateTexture` with `Usage=RENDERTARGET` accepts the measured float
tuples and backs them with a real `A8R8G8B8` target:

- `GetLevelDesc`/`GetDesc` report the **requested** format (`0x71`/`0x72`), as
  D3D9 requires for the created resource;
- the storage is RGBA8, so float/shadow values are clamped to `[0,1]` and
  negative values are lost;
- `GetSurfaceLevel(0)`, `SetRenderTarget`, `SetTexture` and sampling use the
  same target-backed texture path as LDR render targets;
- pool must be `DEFAULT`, levels `1`, dimensions pow2 8..1024.

Every accepted float target logs:

```
[d3d9] MEASUREMENT ONLY: float render targets are stored as A8R8G8B8; not shippable
```

The default path still refuses `0x71/0x72` by name
(`tests/render_texture_smoke.c`); the fallback path is covered by
`tests/render_texture_hdr_smoke.c`.

## Why this is not the shipping answer

- It changes what the engine's image-space/HDR shaders see; it does not make
  PICA200 an HDR renderer.
- A 1024x1024 RGBA8 target plus its `DEPTH24_STENCIL8` attachment is about
  8 MiB, larger than the ~6 MiB available VRAM. The PICA backend therefore
  rejects those targets with `D9_BACKEND`; a bounded native-size policy is a
  separate task.
- It must never be combined with a caps claim (`DevCaps` stays `0x00082000`).

The switch exists to let the post-SM3 census pass renderer initialisation and
record whether any menu draw actually samples these targets. The measured
result belongs in `docs/TESV_PROBE_ROUTE.md`.
