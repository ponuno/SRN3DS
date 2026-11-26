# D3DX9_42 plain texture exports - host shims

The TESV IAT imports six non-COM D3DX9_42 texture entry points. Before this
module the import linker resolved them to guest addresses inside the staged
`d3dx9_42.dll` body; those bodies are not lifted and not executable in the
slice. `source/d3dx_texture_shims.c` registers an extra import resolver so
these six slots point at host thunks in the `0xfd000000` window.

## Implemented (success is evidence-backed)

| Export | Behaviour |
|---|---|
| `D3DXGetImageInfoFromFileInMemory` | Validates a legacy 128-byte-header DDS and fills all seven `D3DXIMAGE_INFO` fields: width, height, depth, mip levels, D3DFORMAT, resource type, `D3DXIFF_DDS`. Non-DDS and unsupported pixel formats return `D3DXERR_INVALIDDATA` (`0x88760B59`), never `S_OK`. |
| `D3DXCreateTextureFromFileInMemory` | Parses the same DDS and creates a real `IDirect3DTexture9` through `d9_com_host_create_texture()` + per-level upload. Uses the file's format, the file's mip chain, `D3DPOOL_MANAGED`, usage 0. |
| `D3DXGetImageInfoFromFileA` | Maps the guest path below the `win32_init()` sandbox root, reads **only the 128-byte DDS header**, and fills the same seven fields. A valid header is reported even when the payload is larger than 64 MiB. |
| `D3DXCreateTextureFromFileExA` | Reads the whole file (64 MiB cap), parses the same DDS, accepts the real TESV tuple (Width/Height from the file, `MipLevels=D3DX_FROM_FILE`, `D3DUSAGE_DYNAMIC`, `A8R8G8B8`, `D3DPOOL_SYSTEMMEM`), and creates/uploads a real texture. BC1/3/5 files requested as `A8R8G8B8` are decoded through `d3d9_bc.c`. |
| `D3DXCreateCubeTextureFromFileInMemory` | Parses a six-face cube DDS (`CUBEMAP` plus all six face bits), validates square/pow2/<=1024 and the face-major mip chain, then creates a real `IDirect3DCubeTexture9` through the new core cube path and uploads every face/level. A8R8G8B8, X8R8G8B8, DXT1, DXT3 and DXT5 are accepted. |

Supported DDS payloads: `A8R8G8B8`, `X8R8G8B8`, `DXT1`, `DXT3`, `DXT5`;
power-of-two dimensions up to 1024. Legacy DDS headers only; a `DX10` header is
rejected explicitly. Compressed files keep the bridge's 8x8 floor when the
texture format stays compressed; decoding to A8R8G8B8 allows smaller levels.

### `D3DXCreateTextureFromFileExA` accepted parameter set

| Parameter | Accepted |
|---|---|
| `Width`, `Height` | 0 (use file) or exactly the file dimensions. Mismatch is `D3DERR_NOTAVAILABLE`; there is no resize path. |
| `MipLevels` | `0` or `D3DX_FROM_FILE` (-3 / `0xFFFFFFFD`). Both use the DDS chain (`d.mips`): this bridge has no mip generator, so generating missing levels would be a fake success. |
| `Usage` | 0 or `D3DUSAGE_DYNAMIC` (`0x200`); passed to the bridge. |
| `Format` | 0 (file format), exactly the file format, or `D3DFMT_A8R8G8B8` when the file is DXT1/3/5 (decoded). |
| `Pool` | `D3DPOOL_MANAGED` (1) or `D3DPOOL_SYSTEMMEM` (2); passed to the bridge. |
| `Filter`, `MipFilter` | `D3DX_DEFAULT` (-1) or real D3DX values 1..5 with optional MIRROR/WRAP bits (`0x000F0000`). No-op without resize; 0 and other values are rejected. |
| `ColorKey` | 0 only. |
| `pSrcInfo`, `pPalette` | NULL or a valid guest pointer. `pSrcInfo` is filled on successful parse; `pPalette` is ignored because only non-paletted formats reach the create path. |

The real export has **14** arguments (`pPalette` at A(12), `ppTexture` at
A(13)) and a `ret 0x38` epilogue, verified by disassembling RVA `0xBE4ED` in
`D3DX9_42.dll`. The shim pops all 14 slots.

### Real ABI and `D3DX_FROM_FILE`

`D3DX_FROM_FILE` is `0xFFFFFFFD`. TESV calls
`D3DXCreateTextureFromFileExA` with width/height from
`D3DXGetImageInfoFromFileA`, `MipLevels=D3DX_FROM_FILE`,
`Usage=D3DUSAGE_DYNAMIC`, `Format=D3DFMT_A8R8G8B8`, `Pool=D3DPOOL_SYSTEMMEM`,
`Filter/MipFilter=D3DX_DEFAULT`, `ColorKey=0`, `pSrcInfo=NULL`,
`pPalette=NULL`.

## File path sandbox

`source/win32_shims.c` exposes `win32_guest_path()` around the same
`path_for()` used by `CreateFileA`. `win32_init()` normalises trailing
separators (including `C:\Games\Skyrim\`), and the path builder also tolerates a
separator-terminated root. It accepts relative paths, `Data\Textures\...`
subdirectories, and the recognised `C:\Skyrim\` virtual-root prefix.

Distinct failure reasons are returned (`WIN32_GUEST_PATH_*`):
no root configured, empty path, longer than 259 bytes, outside the sandbox, or
invalid. `..`, other drives and absolute paths outside the virtual root are
outside/invalid.

`D3DXGetImageInfoFromFileA` needs only the fixed 128-byte header; the 64 MiB
limit applies only to texture creation, which must buffer the full payload.
Files shorter than 128 bytes are reported as such.

## Error naming

* `D3DXERR_INVALIDDATA` for not-DDS, DX10, unsupported pixel formats, truncated
  payloads, and unreadable/missing files.
* `D3DERR_INVALIDCALL` for bad guest pointers/rectangles and sandbox-path
  rejection.
* `D3DERR_NOTAVAILABLE` for unsupported conversions, resize/filter, usage,
  pool, mip selection, and requested-vs-file dimension/format mismatch.
* `D3DERR_OUTOFVIDEOMEMORY` (`0x8876017C`) for host buffer allocation failure;
  the earlier name `D3DERR_OUTOFMEMORY` was wrong.

## Cube textures

`d3d9_core` now owns a `CubeTexture` type with six face stores and a shared mip
chain. `d3d9_com` exposes object kind `CUBE` /
`IDirect3DCubeTexture9`: `CreateCubeTexture` (device slot 25),
`GetLevelCount`, `GetLevelDesc`, `GetCubeMapSurface`, `LockRect`/`UnlockRect`
and `AddDirtyRect`. `GetCubeMapSurface` returns a surface view into one
face/level; compressed views use block-aligned LockRect geometry. `SetTexture`
accepts a cube and `GetTexture` returns it. The cube's CPU faces are real; a
draw with a cube bound fails with `D3DERR_NOTAVAILABLE` and
`"cube-map sampling is not implemented"` rather than silently sampling face 0.

Cube creation requires the backend `texture_create_cube/upload_cube` callbacks.
The null backend advertises them and counts every face/level upload; the PICA
backend does not, so `CreateCubeTexture` returns `D3DERR_NOTAVAILABLE` with
`"this backend has no cube-map texture API"`. This keeps the PICA claim honest.

The D3DX loader requires all six `DDSCAPS2_CUBEMAP_*` face bits, a square
power-of-two edge (compressed faces at least 8x8), and a complete face-major
payload. Missing bits, non-square edges, truncation and non-cube DDS return
`D3DXERR_INVALIDDATA`. A cube DDS passed to the 2D loaders still returns
`E_NOTIMPL`.

Remaining in this layer: volume textures (`IDirect3DVolumeTexture9`) are still
`E_NOTIMPL`; cube objects are not registered in the 2D texture cache
(`D9TexCache`) and cube-map sampling/PICA cube faces are not implemented.

## Named unsupported

`D3DXCreateVolumeTextureFromFileInMemory` still resolves to a host thunk that
returns `E_NOTIMPL` and records the missing layer:

* `D3DXCreateVolumeTextureFromFileInMemory` - no `IDirect3DVolumeTexture9`
  object type, depth slices or volume upload.

Cube/volume DDS passed to `D3DXCreateTextureFromFile(InMemory|ExA)` is also
rejected, never silently created as 2D.

### Missing core layer for volume

`d3d9_core` now has a six-face `CubeTexture`, but no depth-slice object or
volume backend API. Volume support needs a new core volume type, depth slices,
COM interfaces, backend entry points and `SetTexture`/sampler acceptance - a
separate change.

## Integration

* `source/tesv_slice_main.c` installs the resolver and appends the thunk table
  before `n3ds_boot_prepare()`.
* `source/win32_shims.c` exposes `win32_guest_path()`; file exports need
  `win32_init()` to have run with the game root.
* `source/d3d9_com.c` exposes `d9_com_host_create_texture`,
  `d9_com_host_upload_texture_level` and `d9_com_host_release`; pool 2 and usage
  `0x200` are accepted by the existing bridge and stored in the texture object.
  For cube maps it exposes `d9_com_host_create_cube_texture` and
  `d9_com_host_upload_cube_face_level`, backed by the core six-face store.
* `source/d3d9_core.c` owns `CubeTexture` (six faces, shared mips,
  `cube_used` budget accounting) and only creates one when the backend
  advertises `texture_create_cube/upload_cube`.
* `source/d3d9_null.c` provides those callbacks and counts face uploads;
  `source/d3d9_citro3d.c` leaves them NULL, which is the PICA refusal path.
* `source/d3d9_bc.c` already provides `d9_bc_decode_block`; the shim uses it and
  converts RGBA to `A8R8G8B8` byte order (B,G,R,A) per level.
* Thunk windows stay disjoint: texture `0xfd000000..0xfd000014`, surface
  `0xfd010000..0xfd010008`, D3D9 bridge `0xfe000000`, win32 `0xff000000`.

## Regression test

`tests/d3dx_texture_shims_smoke.c` binds a synthetic `d3dx9_42.dll` import
table through `pe_import_bind`, then exercises the shims through the guest
stdcall probe (which checks the exact `4 + 4*args` stack cleanup):

* 8x8 A8R8G8B8 + 2 mips: image info, levels, LockRect pixels, round-trip.
* 16x16 DXT1: compressed layout/pixels; 4x4 DXT1 below the BC floor.
* Exact TESV tuple on A8 and DXT1 files: pool 2, usage 0x200, `MipLevels=-3`,
  `Format=A8R8G8B8`; the DXT1 file is decoded and every pixel is checked.
* `MipLevels=0` uses the file chain; filters 1..4 accepted; 0/6 rejected.
* Width/Height mismatch, unsupported format conversion, bad usage/pool/mip,
  color key, invalid `pSrcInfo`/`ppTexture`/device.
* Temporary sandbox outside the repository: valid files, a `Data\Textures`
  subdirectory, trailing-separator install root, `C:\Skyrim\` alias, missing
  file, `..\escape.dds`, `C:\Windows\...`, non-DDS, and a 64 MiB+1 file whose
  header still yields image info while texture creation hits the size limit.
* Path reasons: no root, empty, too long (300 bytes and no terminator), and
  outside the sandbox.

`tests/d3dx_cube_texture_smoke.c` covers the cube path: 8x8 A8 with two mips
and all six faces, X8, DXT1, DXT3 (2 mips) and DXT5 (alpha 0x80), every face
via `GetCubeMapSurface` + `LockRect`, `SetTexture`/`GetTexture`/refcount,
incomplete face bits, truncated payload, non-square edge, non-cube DDS, the 2D
loader's cube refusal, and a backend without cube callbacks returning
`D9_UNSUPPORTED`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only "d3dx_texture_shims,d3dx_cube_texture_shims" `
  -OutDir C:\Users\vasya\AppData\Local\Temp\d3dxcube
```
