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

`pilot_table.c` has seven entries (six imported exports plus the internal
helper) and `g_fn_count_pilot = 7`. The `pilot_config.h` in that directory is
the `D3DXMatrixMultiply` configuration used by `pilot_real`; the other tests
have their own directories:

- `pilot_out_mult/D3DXMatrixTranspose/gen`
- `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`
- `pilot_out_mult/D3DXPlaneTransform/gen`
- `pilot_out_mult/D3DXVec3TransformCoord/gen`
- `pilot_out_mult/D3DXVec3TransformNormal/gen`

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
```

Each run prints the generated directory as `Use PILOT_GEN=...`. The tool
resolves the lazy-ILT export through the initializer's real-body table and
falls back to `entry+11` only when that address is a real body, not another
wrapper. `D3DXVec3TransformCoord` has two generated functions: the entry plus
its internal helper `f_802E9DB`.

## Merge the combined table

1. Copy the multiply `pilot_00.c` and `pilot_table.c`/`pilot_config.h`/
   `pilot_manifest.json` into `pilot_out_mult/D3DXMatrixMultiply/gen`.
2. Copy the transpose `pilot_00.c` there as `pilot_01.c`; copy the
   multiply-transpose `pilot_00.c` there as `pilot_02.c`; copy the
   plane-transform `pilot_00.c` there as `pilot_03.c`; copy the
   Vec3TransformCoord and Vec3TransformNormal sources as `pilot_04.c` and
   `pilot_05.c`.
3. Merge the `funcs_pilot.h` declarations and the `pilot_table.c` entries; set
   `g_fn_count_pilot = 7`. Keep the table sorted by address: `0x802E9DB`,
   `0x802FA80`, `0x802FCE0`, `0x802FD9D`, `0x8032303`, `0x8032EAD`,
   `0x803306A`.
4. Copy the generated directories to
   `pilot_out_mult/D3DXMatrixTranspose/gen`,
   `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`,
   `pilot_out_mult/D3DXPlaneTransform/gen`,
   `pilot_out_mult/D3DXVec3TransformCoord/gen` and
   `pilot_out_mult/D3DXVec3TransformNormal/gen` so their individual `pilot_*`
   tests can find the matching `pilot_config.h`.
5. Do **not** run a single-export `prepare_pilot.py` directly into
   `pilot_out_mult/D3DXMatrixMultiply/gen`: it would replace the combined
   header/table with a one-entry table.

The `pilot_out_mult/README.md` copy of this note is local-only because the whole
directory is ignored; this tracked file is the durable reference.

## Verify

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only pilot_real,pilot_transpose,pilot_multranspose,pilot_planetransform,`
        pilot_vec3coord,pilot_vec3normal
```

Each test must print `PASS (3/3)`. A TESV slice built with the combined table
logs `d3dx: 7 lifted function(s) registered` (six exports plus the internal
Vec3TransformCoord helper).

`pilot_vec3coord`/`pilot_vec3normal` use a three-float output; the shared pilot
runner selects that by case 6/7. `pilot_planetransform` uses four floats (case
11). The surrounding canaries are still checked.

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

Not lifted yet: `D3DXVec3Normalize` (real body `0x08032cbf`),
`D3DXMatrixInverse` `0x0802fffd`, `D3DXPlaneNormalize` `0x08032000`, and the
plain-export `D3DXLoadSurfaceFromSurface` `0x080bcc2d` /
`D3DXLoadSurfaceFromMemory` `0x080ba613` / `D3DXCompileShader` `0x080d95b3`.
The conservative pilot gate currently rejects the three remaining hotpatch
bodies because they call CRT thunks: `Vec3Normalize` and `PlaneNormalize` call
`_CIsqrt` through the thunk at `0x101a65ec` (`jmp [0x100012dc]`), while
`MatrixInverse` calls `_finite` through `0x101a6694` (`jmp [0x100012a4]`). The
thunk is a direct call target, but its `jmp [IAT]` is an indirect branch.
Supporting them needs an x87 ST(0) sqrt shim and a finite predicate in the
pilot runtime. A call to any of them still fails explicitly at its body address.
