# D3DX9_42 plain texture exports - host shims

The TESV IAT imports non-COM D3DX9_42 texture entry points. Before this
module the import linker resolved them to guest addresses inside the staged
`d3dx9_42.dll` body; those bodies are not lifted and not executable in the
slice. `source/d3dx_texture_shims.c` registers an extra import resolver so
these slots point at host thunks in the `0xfd000000` window. Ten exports are
registered: the four InMemory creators/inspectors, `D3DXGetImageInfoFromFileA`,
the 2D `ExA` loader, and the four file-path cube/volume loaders.

## Implemented (success is evidence-backed)

| Export | Behaviour |
|---|---|
| `D3DXGetImageInfoFromFileInMemory` | Validates a legacy 128-byte-header DDS and fills all seven `D3DXIMAGE_INFO` fields: width, height, depth, mip levels, D3DFORMAT, resource type, `D3DXIFF_DDS`. Non-DDS and unsupported pixel formats return `D3DXERR_INVALIDDATA` (`0x88760B59`), never `S_OK`. |
| `D3DXCreateTextureFromFileInMemory` | Parses the same DDS and creates a real `IDirect3DTexture9` through `d9_com_host_create_texture()` + per-level upload. Uses the file's format, the file's mip chain, `D3DPOOL_MANAGED`, usage 0. |
| `D3DXGetImageInfoFromFileA` | Maps the guest path below the `win32_init()` sandbox root, reads **only the 128-byte DDS header**, and fills the same seven fields. A valid header is reported even when the payload is larger than 64 MiB. |
| `D3DXCreateTextureFromFileExA` | Reads the whole file (64 MiB cap), parses the same DDS, accepts the real TESV tuple (Width/Height from the file, `MipLevels=D3DX_FROM_FILE`, `D3DUSAGE_DYNAMIC`, `A8R8G8B8`, `D3DPOOL_SYSTEMMEM`), and creates/uploads a real texture. BC1/3/5 files requested as `A8R8G8B8` are decoded through `d3d9_bc.c`. |
| `D3DXCreateCubeTextureFromFileInMemory` | Parses a six-face cube DDS (`CUBEMAP` plus all six face bits), validates square/pow2/<=1024 and the face-major mip chain, then creates a real `IDirect3DCubeTexture9` through the new core cube path and uploads every face/level. A8R8G8B8, X8R8G8B8, DXT1, DXT3, DXT5, ATI1/BC4 and ATI2/BC5 are accepted. |
| `D3DXCreateVolumeTextureFromFileInMemory` | Parses a legacy volume DDS (`DDSCAPS2_VOLUME`, `DDSD_DEPTH`, depth > 1), validates pow2 width/height/depth up to 1024 and the level-major slice chain, then creates a real `IDirect3DVolumeTexture9` and uploads every depth slice/mip level. A8R8G8B8, X8R8G8B8, DXT1, DXT3, DXT5, ATI1/BC4 and ATI2/BC5 are accepted. |
| `D3DXCreateCubeTextureFromFileA` | Reads a sandboxed file (64 MiB cap) and feeds the same cube DDS parser/creator; three arguments, `ret 0x0c`. |
| `D3DXCreateCubeTextureFromFileExA` | Same file-derived cube path with the strict tuple below; 13 arguments, `ret 0x34`. |
| `D3DXCreateVolumeTextureFromFileA` | Reads a sandboxed file and feeds the same volume DDS parser/creator; three arguments, `ret 0x0c`. |
| `D3DXCreateVolumeTextureFromFileExA` | Same file-derived volume path with the strict tuple below; 15 arguments, `ret 0x3c`. |

Supported DDS payloads: `A8R8G8B8`, `X8R8G8B8`, `DXT1`, `DXT3`, `DXT5`,
`ATI1`/`BC4U` (8-byte BC4 blocks) and `ATI2`/`BC5U` (16-byte, two BC4
channels); power-of-two dimensions up to 1024. Legacy DDS headers only; a
`DX10` header is rejected explicitly and named, including `BC7_UNORM`
(DXGI 98/99) with the reason that no BC7 decoder exists (see
`docs/TEXTURE_FORMATS.md`). Compressed files keep the bridge's 8x8 floor when
the texture format stays compressed; decoding to A8R8G8B8 allows smaller
levels. Uncompressed 24-bit `R8G8B8` and `A8B8G8R8` are named but not created.

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

### Cube/volume `ExA` accepted parameter set

The cube and volume `...FromFileExA` exports are file-derived only and share
one validator (`file_exa_common`) plus one sandbox reader/parser
(`load_sandbox_dds`); the InMemory entry points call the same
`create_cube_from_dds` / `create_volume_from_dds` creators.

| Parameter | Accepted |
|---|---|
| Cube `Size` / volume `Width`, `Height`, `Depth` | 0, `D3DX_DEFAULT`, or exactly the file's dimension. A mismatch is `D3DERR_NOTAVAILABLE`; there is no resize path. |
| `MipLevels` | `0` or `D3DX_FROM_FILE`; the file's DDS chain is used. Other values are rejected because there is no mip generator. |
| `Usage` | 0 only; dynamic/autogen cube and volume usage is named `D3DERR_NOTAVAILABLE`. |
| `Format` | 0 or exactly the file format. There is no cube/volume format conversion (unlike 2D DXT-to-A8). |
| `Pool` | `D3DPOOL_DEFAULT` (0) or `D3DPOOL_MANAGED` (1). |
| `Filter`, `MipFilter` | `D3DX_DEFAULT` (-1) or real D3DX values 1..5 with optional MIRROR/WRAP bits; no-op without resize. |
| `ColorKey`, `pPalette` | Must be 0/NULL: colour-keyed and paletted cube/volume textures are named `D3DERR_NOTAVAILABLE`. |
| `pSrcInfo` | NULL or a valid 28-byte guest pointer, filled only after a successful parse. |

Verified epilogues in `D3DX9_42.dll`: cube `ExA` RVA `0xBE70C` is `ret 0x34`
(13 arguments), volume `ExA` RVA `0xBE91F` is `ret 0x3c` (15 arguments); both
`FromFileA` exports are 3 arguments with `ret 0x0c`. The test probe pushes the
documented argument count and fails on any stack mismatch.

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

* `D3DXERR_INVALIDDATA` for not-DDS, DX10 (including the named BC7/BC6H
  refusal), unsupported pixel formats, truncated payloads, and
  unreadable/missing files.
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
accepts a cube and `GetTexture` returns it. The cube's CPU faces are real. A
cube-bound draw uses the optional backend `draw_cube` callback (see below); a
backend without it refuses by name and never substitutes face 0.

Cube creation requires the backend `texture_create_cube/upload_cube`
callbacks. The null backend advertises them and counts every face/level upload;
the PICA backend also implements them with real `C3D_TexInitCube` /
`C3D_TexLoadImage` faces and a `texture_delete_cube` release. PICA maps
A8R8G8B8 to `GPU_RGBA8` and X8R8G8B8 to `GPU_RGB8` (X8 is expanded from the
core's 4-byte store at upload); DXT/ETC cube formats, edges outside 8..1024 and
VRAM failures are named refusals (`"cube backend does not support format ..."` /
`"... could not allocate ... (VRAM)"`) rather than a silent substitution.

The D3DX loader requires all six `DDSCAPS2_CUBEMAP_*` face bits, a square
power-of-two edge (compressed faces at least 8x8), and a complete face-major
payload. Missing bits, non-square edges, truncation and non-cube DDS return
`D3DXERR_INVALIDDATA`. A cube DDS passed to the 2D loaders still returns
`E_NOTIMPL`.

Remaining in the cube layer: PICA cube-map sampling and cube render targets
are not implemented.

### Cube sampling in a draw

`D9Backend` has one append-only callback after the volume callbacks:

```c
int (*draw_cube)(void *ctx, const D9ColorVertex *vertices, size_t count,
                 const unsigned *faces, const D9Matrix *clip,
                 const D9CubeView *cube, const D9DrawState *state);
```

The core selects one D3D cube face per vertex before the callback:

* `D3DCUBEMAP_FACES` order `+X,-X,+Y,-Y,+Z,-Z` (indices 0..5).
* With a NORMAL element, the object normal is transformed by the world normal
  matrix and the largest absolute component selects the face.
* Without a usable normal, the documented 2D-emulation direction is
  `(u-0.5, v-0.5, 0)`; UV centre therefore selects `+Z`. A zero normal also
  takes this fallback.
* Only stage 0, one active colour stage, no pixel shader and no CPU T&L
  lighting are accepted; each other combination is a named refusal.

The null backend really samples the core's six CPU face stores and records the
selected face plus the sampled ARGB texel. It point-samples level 0 with
`D3DTADDRESS_WRAP/MIRROR/CLAMP` and supports A8R8G8B8, X8R8G8B8 and
DXT1/DXT3/DXT5. PICA leaves `draw_cube` NULL, so a cube-bound draw returns
`D3DERR_NOTAVAILABLE` with `"requires a backend draw_cube callback"`. The FVF
NORMAL gate remains owned by the test-only T&L prototype; cube tests use an
explicit vertex declaration carrying NORMAL, so DevCaps and T&L behavior are
unchanged.

## Volume textures

`d3d9_core` also owns a `VolumeTexture` with width x height x depth slice
storage for every mip level. `d3d9_com` exposes object kind `VOLUME` /
`IDirect3DVolumeTexture9`: `CreateVolumeTexture` (device slot 24),
`GetLevelCount`, `GetLevelDesc`, `LockBox`/`UnlockBox` and `AddDirtyBox`.
`LockBox` maps a sliced box into guest memory and `UnlockBox` re-uploads the
level; compressed volumes use block-aligned row geometry. `SetTexture` accepts
a volume and `GetTexture` returns it. A draw with a volume bound fails with
`D3DERR_NOTAVAILABLE` and `"volume sampling is not implemented"`.

Volume creation requires the append-only backend
`texture_create_volume/upload_volume` callbacks. The null backend advertises
them and stores/counts every slice level; the PICA backend does not, so
`CreateVolumeTexture` returns `D3DERR_NOTAVAILABLE` with
`"this backend has no volume-texture API"`.

`IDirect3DVolume9` / `GetVolumeLevel` is deliberately not implemented: it
returns `D3DERR_NOTAVAILABLE` and directs callers to `LockBox`/`UnlockBox`.
The D3DX loader requires `DDSCAPS2_VOLUME` + `DDSD_DEPTH`, depth > 1, pow2
dimensions <=1024 and a complete level-major slice chain. Missing depth,
truncation, non-volume DDS and unsupported formats are named failures.

Remaining in this layer: volume sampling and PICA volume faces. Volume
textures keep a CPU slice store per mip level and are not sampled by the draw
path.

## Residency and accounting

Cube and volume objects are registered in the device's `D9TexCache` like 2D
textures. The guest copy is the full CPU store (six faces or every depth slice,
for every mip level) and the GPU copy is the backend native plus its estimated
native byte cost. `d9_memory_used()`, `d9_texture_cache_stats()` and eviction
therefore use one sum for 2D textures, cubes and volumes. LDR render-target
textures still stay outside the cache and are counted as targets, so the
`rt_target` path is unchanged.

Non-render-target cube/volume entries are evictable and carry a rehydrate token:
after LRU trim the core can recreate the native from the retained CPU pixels and
reload every face/level or slice. Render-target cubes have no CPU copy of their
rendered faces, so they stay `no_evict` and are skipped by LRU reserve/trim. The
guest pixels are retained for every rehydratable entry. `d9_cube_resource_info()`
and `d9_volume_resource_info()` expose the split guest/GPU bytes for diagnostics.

Release and device teardown go through the cache, so each native is deleted
exactly once. A backend release failure returns `D9_RELEASE_FAILED` and leaves
the remaining copy retryable, matching the 2D contract; already-released copies
are detached from the core slot to avoid a double free.

## Named unsupported

* `IDirect3DVolume9` / `IDirect3DVolumeTexture9::GetVolumeLevel` - no volume
  surface interface; use `LockBox`/`UnlockBox`.
* `D3DXCreateTextureFromFile(InMemory|ExA)` on a cube or volume DDS - the 2D
  loader returns `E_NOTIMPL` and never creates a 2D texture.
* `D3DXCreateCubeTextureFromFileInMemory` on a volume DDS - `E_NOTIMPL`; the
  dedicated volume loader is named in the diagnostic.
* Cube sampling on a backend without `draw_cube` (PICA) - `D3DERR_NOTAVAILABLE`;
  volume sampling in any draw is still a named refusal.

`cube` DDS passed to `D3DXCreateVolumeTextureFromFileInMemory` is rejected as
`D3DXERR_INVALIDDATA`; a volume DDS passed to the cube loader returns
`E_NOTIMPL`, and a volume DDS passed to a 2D loader also returns `E_NOTIMPL`
(the 2D/cube loaders never create the wrong resource type).

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
* `source/d3d9_core.c` owns `CubeTexture` (six faces, shared mips, unified
  `D9TexCache` accounting) and only creates one when the backend advertises
  `texture_create_cube/upload_cube`; a cube draw additionally needs `draw_cube`.
* `source/d3d9_null.c` provides those callbacks, counts face uploads and
  point-samples the six CPU faces (`draw_cube`); `source/d3d9_citro3d.c`
  allocates real `C3D_Tex` cube faces (A8/X8) and leaves `draw_cube` NULL, so
  PICA cube sampling remains the named refusal path.
* `source/d3d9_bc.c` already provides `d9_bc_decode_block`; the shim uses it and
  converts RGBA to `A8R8G8B8` byte order (B,G,R,A) per level.
* Thunk windows stay disjoint: texture `0xfd000000..0xfd000024`, surface
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
