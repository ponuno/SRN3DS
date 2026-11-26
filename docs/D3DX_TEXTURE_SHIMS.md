# D3DX9_42 plain texture exports — host shims

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

Supported DDS payloads for texture creation: `A8R8G8B8`, `X8R8G8B8`,
`DXT1`, `DXT3`, `DXT5`; power-of-two dimensions up to 1024 (the existing
`d3d9_core` limit), with a compressed base level of at least 8×8. A
non-power-of-two size or a compressed level below 8×8 is rejected with a named
`D3DERR_NOTAVAILABLE` before any resource is created. Legacy DDS headers only;
a `DX10` header is rejected explicitly rather than guessed.

`D3DXGetImageInfoFromFileInMemory` also reports cube and volume DDS metadata
(resource type 5 / 4, volume depth), but their pixel upload is not implemented.

## Named unsupported

These exports resolve to host thunks that return `E_NOTIMPL` and record the
export name plus guest path in `d3dx_texture_shims_last_error()`:

* `D3DXGetImageInfoFromFileA` — file-path shim pending.
* `D3DXCreateTextureFromFileExA` — file-path shim and the resize/filter
  arguments are pending.
* `D3DXCreateCubeTextureFromFileInMemory` — cube upload pending.
* `D3DXCreateVolumeTextureFromFileInMemory` — volume upload pending.

These are real limits, not a closed texture pipeline: file-path loads and
cube/volume creation fail closed with a named error instead of faking a
texture. TESV's cubemaps (environment/skyspec) therefore still need one of
these implementations.

Unknown D3DX names (for example `D3DXMatrixMultiply`) are deliberately not
captured; the existing staged-DLL/hotpatch resolution keeps handling them.

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
* File-path and dedicated cube/volume exports: `E_NOTIMPL`.
* Invalid device pointer: `D3DERR_INVALIDCALL`, no texture emitted.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only d3dx_texture_shims -OutDir C:\Users\vasya\AppData\Local\Temp\d3dxshim
```

The same run also compiles the existing `com` and `device_create` tests; both
remain green. The Python tests in the shared runner need `capstone`, `tex3ds`
and the Windows clang path and are unrelated to this module.
