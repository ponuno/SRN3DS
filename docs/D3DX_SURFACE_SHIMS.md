# D3DX9_42 plain surface/shader shims

Three D3DX9_42 exports are plain names (not MSVC hotpatch wrappers), so the
loader would otherwise bind their real bodies at `0x080b...`/`0x080d...` and
fault there. `source/d3dx_surface_shims.c` resolves them to host thunks:

| Export | Guest args (stdcall pops) | Behaviour |
|---|---|---|
| `D3DXLoadSurfaceFromMemory` | 10 (`0x2c`) | same-format `X8R8G8B8`/`A8R8G8B8` pixels copied through surface `LockRect`/`UnlockRect` |
| `D3DXLoadSurfaceFromSurface` | 8 (`0x24`) | same-format source/destination surface copy through the CPU surface path |
| `D3DXCompileShader` | 10 (`0x2c`) | `E_NOTIMPL` (`0x80004001`), outputs zeroed, named diagnostic |

Thunks live at `0xfd010000 + 4*i`, below `WIN32_THUNK_BASE` and outside the
D3D9 bridge window (`0xfe000000`). `d3dx_surface_module()` returns the
three-entry `FnModule` that must be passed to `n3ds_lift_bind_modules()` via
`n3ds_boot_prepare()`.

## Deliberate limits

* Only `X8R8G8B8` and `A8R8G8B8` are accepted, and source/destination formats
  must match exactly. `X8R8G8B8` ↔ `A8R8G8B8` conversion returns
  `D3DERR_NOTAVAILABLE` with `"format conversion is not implemented"`.
* Scaling, `D3DX_FILTER_TRIANGLE`/`BOX`/…, and non-zero colour keys return
  `D3DERR_NOTAVAILABLE`. The caller never gets half-converted pixels.
* Rectangles are validated against the real surface dimensions; a bad pitch or
  a source span outside guest memory returns `D3DERR_INVALIDCALL`.
* `D3DXCompileShader` never produces bytecode. Shader Model 3 is an
  architectural blocker; the diagnostic says so and the three output pointers
  are cleared when they are valid guest addresses.
* GPU render targets remain read-back-only: `LockRect` already forbids writing
  them, so they can be a source but not a destination. This is reported as
  `D3DERR_NOTAVAILABLE`, not silently ignored. StretchRect is not used because
  the null backend accepts copies without producing pixels.

The last reason is available to host tests through
`d3dx_surface_last_error()`.

## Host integration

`source/d3d9_com.c` exposes a small host API used by the shim (no guest stack,
no COM vtable frame):

* `d9_com_surface_info()`, `d9_com_surface_lock()`, `d9_com_surface_unlock()`
  reuse the existing `lock_surface()`/`unlock_surface()` implementation;
* `d9_com_memory()` returns the bound guest memory.

The import resolver is installed in the same `win32_add_external_resolver()`
chain that `d3dx_texture_shims.c` uses: `win32_resolve()` consults the primary
D3D9 resolver first, then the extra chain, then the unresolved trap. There is
one mechanism for both D3DX modules and no resolver is consulted twice.

Registration in `source/tesv_slice_main.c`:

```c
FnModule tables[6] = {..., {"d3dx-texture", NULL, 0}, {"d3dx-surface", NULL, 0}};
...
if (d3dx_texture_shims_install(&b.memory) == 0) { /* after d9_com_init() */
    tables[table_count++] = d3dx_texture_shims_module();
}
if (d3dx_surface_init(&b.memory) == 0) {          /* after d9_com_init() */
    tables[table_count++] = d3dx_surface_module();
}
```

`d3dx_surface_init()` binds the memory and installs the resolver; shutdown calls
`d3dx_surface_shutdown()` and `d3dx_texture_shims_uninstall()` before
`d9_com_shutdown()`.

## Merging with the texture/image shim module

The two modules were merged into one tree; the rules that keep them compatible:

1. Both resolver functions stay separate and filter `d3dx9_42.dll` plus their
   own export names. They live in the shared win32 extra-resolver chain.
2. Both `FnModule`s are appended to `tables[]` after `d9_com_init()`. The chain
   accepts up to four resolvers, so there is no last-wins conflict.
3. Thunk windows stay disjoint: texture uses `0xfd000000..0xfd000014`, this
   module uses `0xfd010000..0xfd010008`, the D3D9 bridge uses `0xfe000000`,
   and win32 shims start at `0xff000000`.
4. One name maps to one thunk; neither module implements the other's exports.

## Test

```
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only d3dx_surface_shims -OutDir build_host_d3dx_surface
```

`tests/d3dx_surface_shims_smoke.c` creates real CPU-backed
`IDirect3DSurface9` objects on the null backend, resolves the shims through
`win32_resolve`, and asserts:

* 16×16 whole-surface and 8×8 sub-rectangle pixel copies (RGBA words);
* 8×8 same-format surface-to-surface copies;
* named failures for `X8R8G8B8`/`A8R8G8B8` conversion, short pitch, scaling,
  invalid filter, same surface and out-of-bounds rectangles;
* `D3DXCompileShader` returns `E_NOTIMPL`, clears valid outputs and does not
  trap for NULL or unmapped outputs.
