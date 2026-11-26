# D3DX hotpatch body table (generated locally)

`pilot_out*/` is intentionally ignored by git: the lifted body sources are
derived from the local `D3DX9_42.dll` and must not be distributed. The TESV
slice expects the combined table in:

`pilot_out_mult/D3DXMatrixMultiply/gen`

| Source | Entry | Export | Test |
|---|---|---|---|
| `pilot_00.c` | `f_802FA80` | `D3DXMatrixMultiply` | `pilot_real` |
| `pilot_01.c` | `f_802FCE0` | `D3DXMatrixTranspose` | `pilot_transpose` |
| `pilot_02.c` | `f_802FD9D` | `D3DXMatrixMultiplyTranspose` | `pilot_multranspose` |
| `pilot_03.c` | `f_8032303` | `D3DXPlaneTransform` | `pilot_planetransform` |
| `pilot_04.c` | `f_802E9DB` | `D3DXVec3TransformCoord` internal helper | part of `pilot_vec3coord` |
| `pilot_04.c` | `f_8032EAD` | `D3DXVec3TransformCoord` (real body) | `pilot_vec3coord` |
| `pilot_05.c` | `f_803306A` | `D3DXVec3TransformNormal` (real body) | `pilot_vec3normal` |
| `pilot_06.c` | `f_8032CBF` | `D3DXVec3Normalize` (real body) | `pilot_vec3normalize` |
| `pilot_07.c` | `f_802FFFD` | `D3DXMatrixInverse` (real body) | `pilot_matrixinverse` |
| `pilot_08.c` | `f_8032000` | `D3DXPlaneNormalize` (real body) | `pilot_planenormalize` |
| `pilot_shims.c` | `f_081A65EC` | `_CIsqrt` CRT thunk for Vec3/Plane normalize | part of `pilot_vec3normalize`/`pilot_planenormalize` |
| `pilot_shims.c` | `f_081A6694` | `_finite` CRT thunk for MatrixInverse | part of `pilot_matrixinverse` |

`pilot_table.c` has twelve entries (nine imported exports plus the internal
helper and the two CRT shims) and `g_fn_count_pilot = 12`. The
`pilot_config.h` in that directory is the `D3DXMatrixMultiply` configuration
used by `pilot_real`; the other tests have their own directories:

- `pilot_out_mult/D3DXMatrixTranspose/gen`
- `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`
- `pilot_out_mult/D3DXPlaneTransform/gen`
- `pilot_out_mult/D3DXVec3TransformCoord/gen`
- `pilot_out_mult/D3DXVec3TransformNormal/gen`
- `pilot_out_mult/D3DXVec3Normalize/gen`
- `pilot_out_mult/D3DXMatrixInverse/gen`
- `pilot_out_mult/D3DXPlaneNormalize/gen`

## Generate from the exact DLL

Use the DLL whose SHA-256 is
`f8b9cfab7fffbc8f98e41aa439d72921dc180634a1febca2a9d41a0df35d3472`.
Its original `ImageBase` is `0x10000000`; the staged/generated image and the
loader use the relocated base `0x08000000`, so addresses in generated manifests
are `0x08...` (subtract `0x08000000` from the original-base address).

```powershell
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-multiply> --export D3DXMatrixMultiply
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-transpose> --export D3DXMatrixTranspose
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-multranspose> --export D3DXMatrixMultiplyTranspose
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-planetransform> --export D3DXPlaneTransform
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-vec3coord> --export D3DXVec3TransformCoord
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-vec3normal> --export D3DXVec3TransformNormal
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-vec3normalize> --export D3DXVec3Normalize
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-matrixinverse> --export D3DXMatrixInverse
.venv\Scripts\python.exe tools\prepare_pilot.py `
  C:\Windows\SysWOW64\D3DX9_42.dll staged <scratch-planenormalize> --export D3DXPlaneNormalize
```

Each run prints the generated directory as `Use PILOT_GEN=...`. The tool
resolves the lazy-ILT export through the initializer's real-body table and
falls back to `entry+11` only when that address is a real body, not another
wrapper. It also recognises direct calls to `jmp [IAT]` CRT thunks and emits
`pilot_shims.c` for `_CIsqrt` (x87 ST(0) sqrt) and `_finite` (an 8-byte guest
double at `[esp+4]`). **Every generated host shim must pop the guest return
address** (`c->r[4] += 4`) because `call_direct` pushes it before the call;
the Win32 shims do the same. Bodies that restore `esp` from `ebp` hide a
missing pop, but a frameless epilogue such as `D3DXPlaneNormalize`
(`pop esi; pop ebp; ret 8`) returns 4 bytes short and `n3ds_lift_invoke`
reports status -1 with final ESP `base+8`. `D3DXVec3TransformCoord` has two
generated functions: the entry plus its internal helper `f_802E9DB`.

## Merge the combined table

1. Copy the multiply `pilot_00.c` and `pilot_table.c`/`pilot_config.h`/
   `pilot_manifest.json` into `pilot_out_mult/D3DXMatrixMultiply/gen`.
2. Copy the transpose `pilot_00.c` there as `pilot_01.c`; copy the
   multiply-transpose `pilot_00.c` there as `pilot_02.c`; copy the
   plane-transform `pilot_00.c` there as `pilot_03.c`; copy the
   Vec3TransformCoord and Vec3TransformNormal sources as `pilot_04.c` and
   `pilot_05.c`; copy the Vec3Normalize source as `pilot_06.c`; copy the
   MatrixInverse source as `pilot_07.c`; copy the PlaneNormalize source as
   `pilot_08.c`; merge `pilot_shims.c` so it contains both `_CIsqrt` and
   `_finite` **with the return-address pop**. **Strip the duplicate
   `f_802E9DB` definition from `pilot_06.c`**: Vec3Normalize's generated source
   also contains the shared internal helper, which must stay defined only in
   `pilot_04.c`. Copying it verbatim gives a duplicate symbol.
3. Merge the `funcs_pilot.h` declarations and the `pilot_table.c` entries,
   de-duplicating by address (not by `(address, function)` pair), and set
   `g_fn_count_pilot = 12`. Keep the table sorted by address: `0x802E9DB`,
   `0x802FA80`, `0x802FCE0`, `0x802FD9D`, `0x802FFFD`, `0x8032000`,
   `0x8032303`, `0x8032CBF`, `0x8032EAD`, `0x803306A`, `0x81A65EC`,
   `0x81A6694`.
4. Copy the generated directories to
   `pilot_out_mult/D3DXMatrixTranspose/gen`,
   `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`,
   `pilot_out_mult/D3DXPlaneTransform/gen`,
   `pilot_out_mult/D3DXVec3TransformCoord/gen`,
   `pilot_out_mult/D3DXVec3TransformNormal/gen`,
   `pilot_out_mult/D3DXVec3Normalize/gen`,
   `pilot_out_mult/D3DXMatrixInverse/gen` and
   `pilot_out_mult/D3DXPlaneNormalize/gen` so their individual `pilot_*` tests
   can find the matching `pilot_config.h`.
5. Do **not** run a single-export `prepare_pilot.py` directly into
   `pilot_out_mult/D3DXMatrixMultiply/gen`: it would replace the combined
   header/table with a one-entry table.
6. The Makefile and `run_host_tests.ps1` glob `pilot_*.c` from the combined
   directory (guarded by `pilot_table.c` and `pilot_00.c`), so a new body does
   not need a hard-coded source-list edit.

The `pilot_out_mult/README.md` note is local-only because the whole directory
is ignored; this tracked file is the durable reference. If the local note
disagrees with this file, this file wins.

## Verify

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only pilot_real,pilot_transpose,pilot_multranspose,pilot_planetransform,`
        pilot_vec3coord,pilot_vec3normal,pilot_vec3normalize,pilot_planenormalize,`
        pilot_matrixinverse
```

Each test must print `PASS (3/3)`. A TESV slice built with the combined table
logs `d3dx: 12 lifted function(s) registered` (nine exports plus the internal
Vec3TransformCoord helper and the two CRT shims).

To check a real traced run instead of trusting the table, run
`tools/check_import_bindings.py <trace>` on stderr captured with
`N3DS_IMPORT_TRACE=1`. It fails on a missing export, a wrapper/plain target
where a hotpatch body is required, or (with `--require-shims`) a plain D3DX
export that still points into the guest DLL. The trace also prints host-shim
bindings (`-> fd... (host shim)`), so plain exports remain visible when the
builtin resolver wins before PE module lookup.

`pilot_vec3coord`/`pilot_vec3normal`/`pilot_vec3normalize` use a three-float
output; the shared pilot runner selects that by case 6/7/8.
`pilot_planenormalize` and `pilot_planetransform` use four floats (case
10/11). For those shorter results the runner also verifies that the bytes
between `out_len*4` and the full 16-float buffer are untouched, so a body that
writes a whole matrix cannot pass a three-float test. `pilot_matrixinverse`
uses three invertible source matrices (variant 1 forces a row swap at the first
pivot), compares all 16 output floats and checks the determinant written
through the second argument. The surrounding canaries are still checked.

## Loader resolution

`pe_link_resolve`/`hotpatch_body` reads the export's `FF 25 <slot>` and the
wrapper's one-time initializer. It scans every
`mov esi,src; mov edi/ebx,dst; ... rep movsd` copy, uses a preceding
`mov ecx,imm32` as the copied-dword count (falling back to the readable
`src`/`dst` span, capped at 256), and takes the source entry for this slot as
the real body. A candidate must start with the full `8B FF 55 8B EC` prologue
and must not be the wrapper's `push 1; call` tail; `entry` and `wrapper` are
rejected explicitly. Every read is bounds-checked against the image, so an
entry near the end cannot borrow a prologue from the next page. This is
required because some exports (for example `D3DXVec3TransformCoord`) place
another wrapper at `entry+11`; the resolver must not bind that wrapper. If the
initializer table is unavailable and `entry+11` is another wrapper, it returns
zero and the caller keeps the raw export. `tests/pe_imports_smoke.c` covers
both layouts, a wrapper-valued table entry, the copied-count bound, a bogus
first memcpy followed by the real one, the end-of-image case and the
invalid-table negative; `tests/hotpatch_resolver_test.py` mirrors them.

## Remaining imported bodies

The lazy-ILT/hotpatch layer is **complete**: all nine imported bodies are
lifted and pass 3/3 (`MatrixMultiply`, `MatrixTranspose`,
`MatrixMultiplyTranspose`, `PlaneTransform`, `Vec3TransformCoord`,
`Vec3TransformNormal`, `Vec3Normalize`, `PlaneNormalize`, `MatrixInverse`),
plus the internal helper and the two CRT shims. The PlaneNormalize stack bug
was the missing guest-return-address pop in the pilot shims; the fixed shims
are in the combined table.

Remaining D3DX work is only the nine plain exports:
`D3DXGetImageInfoFromFileInMemory`, `D3DXCreateTextureFromFileInMemory`,
`D3DXCreateCubeTextureFromFileInMemory`, `D3DXCreateVolumeTextureFromFileInMemory`,
`D3DXCompileShader`, `D3DXGetImageInfoFromFileA`, `D3DXCreateTextureFromFileExA`,
`D3DXLoadSurfaceFromSurface`, `D3DXLoadSurfaceFromMemory`. They are not
lazy-ILT thunks; they need host shims over the D3D9 COM/texture/surface layers
or a lifted closure. A call to any of them still fails explicitly.
