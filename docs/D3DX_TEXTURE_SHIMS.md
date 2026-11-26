# D3DX9_42 plain texture exports - host shims

The TESV IAT imports six non-COM D3DX9_42 texture entry points. Before this
module the import linker resolved them to guest addresses inside the staged
`d3dx9_42.dll` body; those bodies are not lifted and not executable in the
slice. `source/d3dx_texture_shims.c` registers an extra import resolver so
these six slots now point at host thunks in the `0xfd000000` window.

## Implemented (success is evidence-backed)

| Export | Behaviour |
|---|---|
| `D3DXGetImageInfoFromFileInMemory` | Validates a legacy 128-byte-header DDS and fills all seven `D3DXIMAGE_INFO` fields: width, height, depth, mip levels, D3DFORMAT, resource type, `D3DXIFF_DDS`. Non-DDS and unsupported pixel formats return `D3DXERR_INVALIDDATA` (`0x88760B59`), never `S_OK`. |
| `D3DXCreateTextureFromFileInMemory` | Parses the same DDS, creates a real `IDirect3DTexture9` through `d9_com_host_create_texture()` (same usage/pool/layout checks as the COM `CreateTexture` slot), then uploads every mip through `LockRect`/`UnlockRect` in `d3d9_core`. Uses `D3DPOOL_MANAGED`, usage 0, as D3DX does. |
| `D3DXGetImageInfoFromFileA` | Maps the guest path below the `win32_init()` sandbox root, reads at most 64 MiB into a host buffer, then runs the exact InMemory parser and fills the same seven fields. Missing/unreadable/oversized files and non-DDS input are named failures; the info structure is zeroed before any read. |
| `D3DXCreateTextureFromFileExA` | Same file read and parser, then the same 2D texture creation/upload path. Only the file-derived subset is accepted: Width=0, Height=0, MipLevels=0, Usage=0, Format=D3DFMT_UNKNOWN, Pool=D3DPOOL_MANAGED, Filter/MipFilter=D3DX_DEFAULT or 0, ColorKey=0. Everything else is a named `D3DERR_NOTAVAILABLE` before any resource exists. |

Supported DDS payloads for texture creation: `A8R8G8B8`, `X8R8G8B8`,
`DXT1`, `DXT3`, `DXT5`; power-of-two dimensions up to 1024 (the existing
`d3d9_core` limit), with a compressed base level of at least 8x8. A
non-power-of-two size or a compressed level below 8x8 is rejected with a named
`D3DERR_NOTAVAILABLE` before any resource is created. Legacy DDS headers only;
a `DX10` header is rejected explicitly rather than guessed.

`D3DXGetImageInfoFromFileA`/`ExA` ignore the `PALETTEENTRY*` argument because
only non-paletted 32-bit/BC formats reach the create path.

### Real ABI of `D3DXCreateTextureFromFileExA`

The export in `D3DX9_42.dll` has **14** arguments and ends with `ret 0x38`
(56 bytes of arguments) - verified by disassembling the export at RVA
`0xBE4ED` and reading its epilogue. The parameter list is therefore
`device, file, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter,
ColorKey, pSrcInfo, pPalette, ppTexture`; `pPalette` is argument 12 and
`ppTexture` is argument 13. A 13-argument shim would leave four bytes on the
guest stack on every call.

## File path sandbox

`source/win32_shims.c` now exposes `win32_guest_path()` around the same
`path_for()` used by `CreateFileA`. It accepts a relative path or the recognised
`C:\Skyrim\` virtual-root prefix and rejects:

* `..` segments, empty segments and absolute paths outside the root;
* drives other than the virtual `C:\Skyrim\` alias;
* paths longer than 259 bytes or a guest string without a terminator inside
  that bound.

The shim additionally refuses files over 64 MiB and only reads regular files.
Nothing is written back to the sandbox.

## Named unsupported

These exports resolve to host thunks that return `E_NOTIMPL` and record the
export name plus the exact missing layer in `d3dx_texture_shims_last_error()`:

* `D3DXCreateCubeTextureFromFileInMemory` - the bridge has no
  `IDirect3DCubeTexture9` object type, cube-face surfaces or six-face backend
  upload.
* `D3DXCreateVolumeTextureFromFileInMemory` - the bridge has no
  `IDirect3DVolumeTexture9` object type, depth slices or volume upload.

A cube/volume DDS passed to `D3DXCreateTextureFromFile(InMemory|ExA)` is also
rejected with `E_NOTIMPL`; it is never silently created as a 2D texture.

### Missing core layer for cube/volume

Cube and volume support is not a small shim addition:

* `source/d3d9_core.c` has one 2D `Texture` with a single `native` handle,
  2D `Level` records and `D9Backend.texture_create(ctx, w, h, levels, native)`.
  There is no face/slice dimension, cube flag or backend cube API.
* `source/d3d9_com.c` object kinds stop at `TEX`; the device
  `CreateCubeTexture`/`CreateVolumeTexture` slots are unsupported (no handler),
  and there is no `IDirect3DCubeTexture9`/`IDirect3DVolumeTexture9` vtable,
  `GetLevelDesc` or `GetCubeMapSurface`.
* `source/d3d9_null.c` and the PICA backend only create/upload 2D textures.

Implementing cube would require a new core texture type with six faces,
texcache accounting, backend entry points, a COM interface and
`SetTexture` acceptance. That is a separate architectural change and is not
claimed here.

## Integration

* `source/tesv_slice_main.c` installs the resolver and appends the thunk table
  before `n3ds_boot_prepare()`.
* `source/win32_shims.c` supports extra external resolvers, consulted after
  the primary (D3D9) resolver and before the unresolved-import trap, so the
  D3D9 bridge and the `d3dx` hotpatch path are unchanged. This is the single
  resolver mechanism shared with `d3dx_surface_shims.c`; that module owns
  `0xfd010000..0xfd010008`, this one owns `0xfd000000..0xfd000014`.
  `d3dx_texture_shims_install()` returns `-1` if the fixed four-slot chain is
  full, so it never reports a shim whose thunks are unreachable. The surface
  module installs even without a D3D9 session (so `D3DXCompileShader` keeps
  refusing by name); its two surface copies then return
  `D3DERR_NOTAVAILABLE`.
* `source/win32_shims.c` exposes `win32_guest_path()`; file exports need
  `win32_init()` to have run with the game root.
* `source/d3d9_com.c` exposes `d9_com_host_create_texture`,
  `d9_com_host_upload_texture_level` and `d9_com_host_release`; these are the
  only new entry points and they reuse the existing object table and D9Device.

## Regression test

`tests/d3dx_texture_shims_smoke.c` binds a synthetic `d3dx9_42.dll` import
table through `pe_import_bind`, then exercises the shims through the guest
stdcall probe (which checks the exact `4 + 4*args` stack cleanup):

* 8x8 A8R8G8B8 + 2 mips: image-info fields, `GetLevelCount`,
  `GetLevelDesc`, `LockRect` pixels for both levels, guest write round-trip.
* 16x16 DXT1: format and compressed `LockRect` layout/pixels.
* Non-DDS: `D3DXERR_INVALIDDATA` with a zeroed/cleared output.
* Cube and volume DDS: image info works; creation is `E_NOTIMPL`.
* File-path info/create in a temporary sandbox: valid `menu.dds`, the
  `C:\Skyrim\` alias, missing file, `..\escape.dds`, `C:\Windows\...`,
  non-DDS, oversized file, NULL path; `ExA` success and every rejected Ex
  parameter; cube/volume file inputs.
* Invalid device pointer: `D3DERR_INVALIDCALL`, no texture emitted.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only d3dx_texture_shims -OutDir C:\Users\vasya\AppData\Local\Temp\d3dxshim
```

The same runner also covers `com` and `texture_surface` regressions. The
Python tests in the shared runner need `capstone`, `tex3ds` and the Windows
clang path and are unrelated to this module.
