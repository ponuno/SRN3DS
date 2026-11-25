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
| `pilot_shims.c` | `f_081A65EC` | `_CIsqrt` CRT thunk for Vec3Normalize | part of `pilot_vec3normalize` |

`pilot_table.c` has nine entries (seven imported exports plus the internal
helper and the `_CIsqrt` shim) and `g_fn_count_pilot = 9`. The
`pilot_config.h` in that directory is the `D3DXMatrixMultiply` configuration
used by `pilot_real`; the other tests have their own directories:

- `pilot_out_mult/D3DXMatrixTranspose/gen`
- `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`
- `pilot_out_mult/D3DXPlaneTransform/gen`
- `pilot_out_mult/D3DXVec3TransformCoord/gen`
- `pilot_out_mult/D3DXVec3TransformNormal/gen`
- `pilot_out_mult/D3DXVec3Normalize/gen`

## Generate from the exact DLL

Use the DLL whose SHA-256 is
`f8b9cfab7fffbc8f98e41aa439d72921dc180634a1febca2a9d41a0df35d3472`
(preferred base `0x10000000`):

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
```

Each run prints the generated directory as `Use PILOT_GEN=...`. The tool
resolves the lazy-ILT export through the initializer's real-body table and
falls back to `entry+11` only when that address is a real body, not another
wrapper. It also recognises direct calls to `jmp [IAT]` CRT thunks and emits
`pilot_shims.c` for `_CIsqrt` (x87 ST(0) sqrt). `D3DXVec3TransformCoord` has
two generated functions: the entry plus its internal helper `f_802E9DB`.

## Merge the combined table

1. Copy the multiply `pilot_00.c` and `pilot_table.c`/`pilot_config.h`/
   `pilot_manifest.json` into `pilot_out_mult/D3DXMatrixMultiply/gen`.
2. Copy the transpose `pilot_00.c` there as `pilot_01.c`; copy the
   multiply-transpose `pilot_00.c` there as `pilot_02.c`; copy the
   plane-transform `pilot_00.c` there as `pilot_03.c`; copy the
   Vec3TransformCoord and Vec3TransformNormal sources as `pilot_04.c` and
   `pilot_05.c`; copy the Vec3Normalize source as `pilot_06.c`; copy its
   generated `pilot_shims.c`.
3. Merge the `funcs_pilot.h` declarations and the `pilot_table.c` entries; set
   `g_fn_count_pilot = 9`. Keep the table sorted by address: `0x802E9DB`,
   `0x802FA80`, `0x802FCE0`, `0x802FD9D`, `0x8032303`, `0x8032CBF`,
   `0x8032EAD`, `0x803306A`, `0x81A65EC`.
4. Copy the generated directories to
   `pilot_out_mult/D3DXMatrixTranspose/gen`,
   `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`,
   `pilot_out_mult/D3DXPlaneTransform/gen`,
   `pilot_out_mult/D3DXVec3TransformCoord/gen`,
   `pilot_out_mult/D3DXVec3TransformNormal/gen` and
   `pilot_out_mult/D3DXVec3Normalize/gen` so their individual `pilot_*` tests
   can find the matching `pilot_config.h`.
5. Do **not** run a single-export `prepare_pilot.py` directly into
   `pilot_out_mult/D3DXMatrixMultiply/gen`: it would replace the combined
   header/table with a one-entry table.

The `pilot_out_mult/README.md` copy of this note is local-only because the whole
directory is ignored; this tracked file is the durable reference.

## Verify

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only pilot_real,pilot_transpose,pilot_multranspose,pilot_planetransform,`
        pilot_vec3coord,pilot_vec3normal,pilot_vec3normalize
```

Each test must print `PASS (3/3)`. A TESV slice built with the combined table
logs `d3dx: 9 lifted function(s) registered` (seven exports plus the internal
Vec3TransformCoord helper and the `_CIsqrt` shim).

`pilot_vec3coord`/`pilot_vec3normal`/`pilot_vec3normalize` use a three-float
output; the shared pilot runner selects that by case 6/7/8.
`pilot_planetransform` uses four floats (case 11). The surrounding canaries are
still checked.

## Loader resolution

`pe_link_resolve`/`hotpatch_body` reads the export's `FF 25 <slot>` and the
wrapper's one-time initializer. It scans the initializer for the
`mov esi,src; mov edi/ebx,dst; ... rep movsd` table copy and uses the source
entry for this slot as the real body. This is required because some exports
(for example `D3DXVec3TransformCoord`) place another wrapper at `entry+11`; the
resolver must not bind that wrapper. If the initializer table is unavailable
and `entry+11` is another wrapper, it returns zero and the caller keeps the raw
export. `tests/pe_imports_smoke.c` covers both layouts, including the invalid
table entry negative case.

## Remaining imported bodies

Not lifted yet: `D3DXMatrixInverse` `0x0802fffd`,
`D3DXPlaneNormalize` `0x08032000`, and the plain-export
`D3DXLoadSurfaceFromSurface` `0x080bcc2d` /
`D3DXLoadSurfaceFromMemory` `0x080ba613` / `D3DXCompileShader` `0x080d95b3`.
The `_CIsqrt` shim is integrated for `Vec3Normalize`. `PlaneNormalize` uses the
same shim but its generated early-copy path/helper interaction misbalances the
guest stack in the pilot (`status -1`, final ESP `base+8`); it is generated but
not shipped. `MatrixInverse` needs the `_finite` shim and a determinant-output
test before it can be integrated. The plain exports still need lifting or
shims. A call to any of them still fails explicitly at its body address.
