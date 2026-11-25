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

`pilot_table.c` has four entries and `g_fn_count_pilot = 4`. The
`pilot_config.h` in that directory is the `D3DXMatrixMultiply` configuration
used by `pilot_real`; the other tests have their own directories:

- `pilot_out_mult/D3DXMatrixTranspose/gen`
- `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen`
- `pilot_out_mult/D3DXPlaneTransform/gen`

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
```

Each run prints the generated directory as `Use PILOT_GEN=...`. The tool
resolves the lazy-ILT export to `entry+11` and rejects a chain that does not
match the verified hotpatch layout.

## Merge the combined table

1. Copy the multiply `pilot_00.c` and `pilot_table.c`/`pilot_config.h`/
   `pilot_manifest.json` into `pilot_out_mult/D3DXMatrixMultiply/gen`.
2. Copy the transpose `pilot_00.c` there as `pilot_01.c`; copy the
   multiply-transpose `pilot_00.c` there as `pilot_02.c`; copy the
   plane-transform `pilot_00.c` there as `pilot_03.c`.
3. Merge the `funcs_pilot.h` declarations and the `pilot_table.c` entries; set
   `g_fn_count_pilot = 4`.
4. Copy the transpose, multiply-transpose and plane-transform generated
   directories to `pilot_out_mult/D3DXMatrixTranspose/gen`,
   `pilot_out_mult/D3DXMatrixMultiplyTranspose/gen` and
   `pilot_out_mult/D3DXPlaneTransform/gen` so their individual `pilot_*` tests
   can find the matching `pilot_config.h`.
5. Do **not** run a single-export `prepare_pilot.py` directly into
   `pilot_out_mult/D3DXMatrixMultiply/gen`: it would replace the combined
   header/table with a one-entry table.

The `pilot_out_mult/README.md` copy of this note is local-only because the whole
directory is ignored; this tracked file is the durable reference.

## Verify

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only pilot_real,pilot_transpose,pilot_multranspose,pilot_planetransform
```

Each test must print `PASS (3/3)`. A TESV slice built with the combined table
logs `d3dx: 4 lifted function(s) registered`.

`pilot_planetransform` uses the four-float plane output length; the shared
pilot runner selects it by case 11 and still checks the surrounding canaries.

## Remaining imported bodies

Not lifted yet: `D3DXVec3TransformCoord` `0x0802f294`,
`D3DXVec3TransformNormal` `0x0802f2da`, `D3DXVec3Normalize` `0x0802ef4c`,
`D3DXMatrixInverse` `0x0802fffd`, `D3DXPlaneNormalize` `0x08032000`, and the
plain-export `D3DXLoadSurfaceFromSurface` `0x080bcc2d` /
`D3DXLoadSurfaceFromMemory` `0x080ba613` / `D3DXCompileShader` `0x080d95b3`.
The conservative pilot gate currently rejects the five hotpatch bodies above:
`Vec3TransformCoord` and `Vec3TransformNormal` contain indirect branches,
`Vec3Normalize` contains an indirect call, and `MatrixInverse` /
`PlaneNormalize` branch through an indirect target near `0x081a6xxx`. They need
a bounded jump-table/call closure before they can be generated. A call to any
of them still fails explicitly at its body address.
