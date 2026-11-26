# TESV route behind the measurement probes (2026-11-25)

This note records what the engine does once the two honest gates no longer stop
it. Every value below is measured on the host slice with the probes enabled;
the probes are measurement-only and must never become shipping defaults.

## Probes

| Variable | Effect | Shipping |
|---|---|---|
| `TESV_D3D9_CAPS_PROBE=1` | `GetDeviceCaps` adds `D3DDEVCAPS_HWTRANSFORMANDLIGHT` | never |
| `TESV_D3D9_PP_PROBE=1` | **removed** (obsolete); `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is a real retained-target feature now | n/a |

A third probe, `TESV_D3D9_STAGE_PROBE=1`, reported `MaxTextureBlendStages =
MaxSimultaneousTextures = 2` before those were implemented. It is obsolete: the
COM caps now derive `2/2` from the backend's real `draw_two` entry point.

Each prints a one-shot `MEASUREMENT ONLY ... not shippable` line.

## Measured sequence (evidence: `E:\Skyrim_N3DS_build\analysis\stageprobe_tesv.err`)

1. `0x00c6d8c6` HW T&L gate passed (probe).
2. Adapter/format enumeration; `0x00cd5542` probes 80 texture/cube formats.
3. `CreateDevice` (factory slot 16, caller `0x00cd8b74`) -> **`S_OK`** with
   `400x240 X8R8G8B8`, `DISCARD`, 1 back buffer, no MSAA, windowed,
   `EnableAutoDepthStencil=0`, `AutoDepthStencilFormat=75`, `interval=ONE`,
   `Flags=0x1` (`LOCKABLE_BACKBUFFER`; with the retained back buffer this is now
   accepted without the removed PP probe and the pixels are real).
4. `IDirect3DDevice9::GetDeviceCaps` (caller `0x00cd2d32`).
5. Renderer checks caps at `0x00cd2d67` / `0x00cd2d73`:
   `[esi+0xd0] >= 2 && [esi+0xd4] >= 2`, where the caps copy starts at
   `[esi+0x3c]`, i.e. `D3DCAPS9.MaxTextureBlendStages` (148) and
   `MaxSimultaneousTextures` (152). With the honest `1/1` the engine prints
   "Unknown error creating the renderer." (`0x11585c0` / `0x1158568`).
6. With the stage probe (`2/2`) it proceeds:
   `GetRenderTarget` + surface `GetDesc` (`0x00ce8f49..80`),
   `CreateTexture` (caller `0x00ce69eb`): `Width=2, Height=1, Levels=1,
   Usage=0, Format=0x16 (A8R8G8B8), Pool=1 (MANAGED)`.
7. The engine then takes a level surface of that texture
   (`GetSurfaceLevel` slot 18, caller `0x00ce7135`), reads its `GetDesc`,
   `LockRect`/`UnlockRect` it (`0x00ce6a45..0x00ce6b80`) and releases it.
   This is the real reason the engine wants `LOCKABLE` style access: a RAM
   texture it fills through a level surface.
8. `SetTransform` VIEW (`0x00cd32d1`) and PROJECTION (`0x00cd34cc`), then
   `SetViewport` (`0x00cd3605`) -> `S_OK`.
9. The guest indirect call at `0x00f79dc2` targets `D3DXMatrixMultiply`.
   TESV's IAT was bound to the DLL export entry `0x0802fa75`, an MSVC
   lazy-ILT forwarder (`FF 25 <slot>` + five `CC` bytes). The slot holds the
   wrapper `0x0802fa5d`; the real body is `0x0802fa80` (entry+11). The wrapper
   and its one-time initializer are D3DX code, so binding the export entry
   traps on an address that no tier contains. `pe_link_resolve` now validates
   the exact hotpatch chain (export, five `CC`, wrapper `8B FF 55 8B EC / 6A 01
   / E8 rel32 / 5D / FF 25 <same slot>`, body prologue) and binds the body.
   The trace reads
   `[import] d3dx9_42.dll!D3DXMatrixMultiply -> 0802fa80 (export 0802fa75 hotpatch body)`.
   The slice registers the lifted `f_802FA80` (host and ARM); the multiply
   returns to `0x00cd3613` and `SetTransform`/`SetViewport` continue normally.

No explicit depth-stencil surface, no back-buffer `LockRect`, no draw and no
`Present` were reached yet.

## Next event after the hotpatch fix: the SM3 gate

With the multiply body dispatchable, the route now reaches renderer
initialization `0x00c6dbf0` and stops at the documented SM3 gate:

- `0x00c6e21e -> 0x00c71080` computes a shader level from the device's
  VS/PS version words. `0xc71080` returns 7 only when both major versions are
  `>= 3`; both `>= 2` returns 2.
- The honest caps are `VS_VERSION_2_0` / `PS_VERSION_2_0`
  (`source/d3d9_adapter.c`), so the level is 2.
- `0x00c6e22d: cmp dword ptr [0x01ba749c], 7` / `jge 0xc6e247`; otherwise
  `0x00c6e23d` pushes `0x114e04c` and jumps to the renderer failure path
  `0x00c6dfd3`.
- String at `0x114e04c`: **"ShaderModel3 required but not supported by your
  video card."** The guest then shows `Failed to initialize renderer.` and
  exits via `ExitProcess(0)` from `0x0069107d`.

This is an architectural gap, not a regression: the bridge reports VS/PS 2.0
and does not claim SM3 (`docs/MENU_SHADER_CLOSURE.md`). No draw or `Present`
occurs before this gate.

The bounded first-menu PS runtime is now wired
(`docs/MENU_SHADER_RUNTIME.md`): it selects only the exact certified
`p1200005` logo and menu-copy plans (plus fixed fog/tint contracts) and refuses
everything else by name, but it deliberately does not change the two version
words. This gate therefore still fails in the honest path; passing it requires
the real bounded SM3 execution path (VS family, material preconditions, PICA
texture units/vertex colour/constants), not a caps edit.

### Measurement-only post-SM3 census

`TESV_D3D9_SM3_GATE_PROBE=1` is a diagnostic, not a shipping path. The slice
byte-verifies `00c6e22d` (`83 3D 9C 74 BA 01 07`, `cmp [01ba749c],7`) and
`00c6e23b` (`7D 0A`, `jge 00c6e247`), then changes only the conditional jump to
`EB 0A` for the rest of that run. `GetDeviceCaps` and both version words stay
VS/PS 2.0. It exists solely to record the first post-gate D3D9 calls (shader
creates, depth, draws, `Present`) and is refused if the bytes differ. Do not
combine its output with a rendering claim.

Static post-gate chain (read-only analysis):

- `0x00c6dbf0` itself creates no shader, loads no asset and issues no draw or
  `Present`; success returns to `0x0069d97e -> 0x00696810 -> 0x00c74710`.
- `0x00c74710` opens `SHADERSFX/Shaders002.fxp`; the first record is `BSVS`
  `vs_3_0`, 436 bytes. `CreateVertexShader` is called at `0x00ccbbb3` and its
  HRESULT is ignored (nonfatal preload). The PS path later reads `BSPS` and
  calls `CreatePixelShader` at `0x00ccbfb3` / `0x00ccc3a7`.
- `0x00ca38d0` queues 124 lighting-package keys; while a menu closure is armed,
  any non-menu shader from that preload invalidates it. This is why the
  preflight-versus-observed certification order must be resolved before the
  closure can be the gate predicate.

Census result (104 s, exit 1, `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_census_final2.*`):

- The gate is passed and the SM3 message disappears; VS/PS stay 2.0 and the
  guest data word `[0x01ba749c]` remains `2`; the predecoded compare reads the
  probe's private shadow value `7`.
- **No shader is created.** `CreateVertexShader`/`CreatePixelShader` are not
  reached after the gate.
- The first post-gate D3D9 activity is the nonfatal `CheckDeviceFormat` probe
  loop at `0x00c6e447`, then `CreateTexture` with `D3DUSAGE_RENDERTARGET`
  (`caller 0x00ceca5e`, 1024x1024, levels 1, format `0x72` **`R32F`**, pool 0)
  returning `D3DERR_NOTAVAILABLE` (`0x8876086a`); the engine repeats the
  1024/512/256 shadow set with formats `0x72` (R32F), `0x15` (A8R8G8B8) and
  `0x71` (A16B16G16R16F).
- After the last 2D creation the engine queries a `NiRenderedCubeMap` face
  table through `0x00fb5bd0` (vtable slot `0x8c`, `[[this+0x40]+8]`) and faults
  on `[0x0000000c]`; this is a stale/invalid face entry, not the failed R32F
  texture (a plain NULL at `+0x40` would fault at `0x8`). Depth-stencil, draws
  and `Present` were never reached; only `RendererInfo.txt` was opened.

The LDR half of that census is now implemented: pow2 8..1024
X8R8G8B8/A8R8G8B8 render-target textures with one level and pool DEFAULT are
real Target-backed textures, `GetSurfaceLevel(0)` returns the target and
`SetTexture` samples the same colour buffer. Float/HDR render targets stay
named refusals. Re-running the census with this layer is the next measurement;
no shader or frame claim follows from the old trace.

### HDR fallback census (2026-11-26)

Base: `main` at `5e15944` (HDR fallback `ab4ec06`, size-checked depth descriptors
`5e15944`). No rebuild was performed for this run; the supplied binary was
`C:\Skyrim_N3DS_build\host_slice_hdrfb\run_tesv_slice_35040\tesv_slice.exe`
(480,013,824 bytes, built 05:49). The census used a 2,000,000,000-step budget and
ran for **92.56 s**, exit `1`:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_hdrfb_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_hdrfb_census.out`

The SM3 gate is passed and the fallback is active. Render-target requests from
`caller=0x00ceca5e`, all levels `1`, usage `1`, pool `0`:

| Size | Format | Count | HRESULT |
|---|---|---:|---|
| 1024x1024 | `0x72` R32F | 5 | `D3DERR_OUTOFVIDEOMEMORY` (`0x8876017c`) |
| 512x512 | `0x15` A8R8G8B8 | 3 | `S_OK` |
| 512x512 | `0x71` A16B16G16R16F | 1 | `D3DERR_OUTOFVIDEOMEMORY` (`0x8876017c`) |

These are the renderer-init shadow-map set, not HDR/bloom: the five 1024
targets are `R32F` shadow maps (requested `R16F 0x6f`, promoted at
`0xc91d19`), and the 512 group is the quarter-res shadow-mask set. The fatal
step after the last 2D creation is the `NiRenderedCubeMap` face-table query at
`0x00fb5bd0`, not the failed `0x71` texture.

The HDR fallback logs confirm that `0x72` and `0x71` were accepted as formats and
rewritten to A8R8G8B8 storage; creation then failed in the resource-budget layer.
The host null backend limit is 4 MiB (`source/d3d9_null.h:10`). The retained
400x240 back buffer occupies a 512x256 target (524,288 bytes). Three 512x512 LDR
targets add 3,145,728 bytes, leaving 524,288 bytes; the final 512x512 HDR target
needs 1,048,576 bytes. A 1024x1024 target needs 4,194,304 bytes and also cannot
fit alongside the retained back buffer.

The three successful LDR textures are `0x500021c0`, `0x500021e0`, `0x50002200`.
For each, `QueryInterface` (`caller 0x00cecb44`), `AddRef` (`0x00ce88c3`),
`GetSurfaceLevel(0)` (`0x00ce88fd`) and `GetDesc` (`0x00ce893f`) return `S_OK`;
the target handles are `15`, `17` and `19`, each 512x512.

Renderer initialization does **not** get past the final 512x512 `0x71` request.
Not reached: `CreateDepthStencilSurface`, `SetDepthStencilSurface`,
`SetRenderTarget`, `Clear`, `BeginScene`, `CreateVertexShader`,
`CreatePixelShader`, `SetTexture`, any draw, `Present`, `ValidateDevice`,
`SetMaterial` or `SetLight`. No FXP is opened and the first-draw capture contains
no records.

Consequently, sampling of the fallback textures is **not proven**: the `0x72`
requests never produced texture handles, the `0x71` request never produced a
handle, and no draw occurred. There are no `SetTexture` handles or draw-capture
entries to correlate.

The first failed call is the first 1024x1024 `0x72` creation (`OUTOFVIDEOMEMORY`),
which the engine tolerates while falling back. The fatal sequence is the final
512x512 `0x71` `OUTOFVIDEOMEMORY`, followed by a guest fault at PC `0x00fb5bd0`
on address `0x0000000c`. The last successful D3D9 call is
`IDirect3DTexture9::Release` on `0x50002200`, caller `0x00cecb99`, returning
refcount `3`; the last successful creation is the third 512x512 `0x15` target.

The next concrete blocker is the census backend's 4 MiB target budget, not HDR
format rejection. A larger measurement-only budget (or an env-gated budget
probe) is required before depth/scene/shader/draw sampling can be measured. The
fallback remains measurement-only and this is not a rendered-frame claim.

### 64 MiB budget census (2026-11-26)

Base: `main` at `7537db0` (budget override `949701b`). A fresh slice was built
from `E:\Skyrim_N3DS_build\analysis\growth_probe\gen01` in **149.8346974 s**.
The census used a 2,000,000,000-step budget and ran for **92.5110594 s**, exit `1`.
The binary was
`C:\Users\vasya\AppData\Local\Temp\opencode\sm3-gate-probe-budget64-census\run_tesv_slice_31548\tesv_slice.exe`;
arguments were `F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160`:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_BUDGET_MB=64
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_budget64_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_budget64_census.out`

All measured render-target creations now succeed:

| Size | Format | Count | HRESULT | Texture / surface handles | Backend target IDs |
|---|---|---:|---|---|---|
| 1024x1024 | `0x72` R32F | 5 | `S_OK` | `0x500021c0`/`0x500021d0`, `0x500021e0`/`0x500021f0`, `0x50002200`/`0x50002210`, `0x50002220`/`0x50002230`, `0x50002240`/`0x50002250` | `15`, `17`, `19`, `21`, `23` |
| 512x512 | `0x15` A8R8G8B8 | 3 | `S_OK` | `0x50002260`/`0x50002270`, `0x50002280`/`0x50002290`, `0x500022a0`/`0x500022b0` | `25`, `27`, `29` |
| 512x512 | `0x71` A16B16G16R16F | 1 | `S_OK` | `0x500022c0`/`0x500022d0` | `31` |

All come from `caller=0x00ceca5e`, levels `1`, usage `1`, pool `0`. For all
nine, `QueryInterface` (`0x00cecb44`), `GetSurfaceLevel(0)` (`0x00ce88fd`) and
`GetDesc` (`0x00ce893f`) return `S_OK`; `AddRef` (`0x00ce88c3`) and `Release`
(`0x00cecb99`) return refcount `3`. No 256x256 render-target request occurs.
The nine new allocations consume 25,165,824 bytes (24 MiB); including the
retained 512x256 back buffer (524,288 bytes), the total is 25,690,112 bytes
(24.5 MiB).

Despite successful allocation, renderer initialization still stops before any
scene state. Not reached: `CreateDepthStencilSurface`,
`SetDepthStencilSurface`, `SetRenderTarget`, `Clear`, `BeginScene`,
`CreateVertexShader`, `CreatePixelShader`, `SetTexture`, any draw, `Present`,
`ValidateDevice`, `SetMaterial` or `SetLight`. No FXP is opened and the
first-draw capture has no records.

The first fatal event is therefore **not a failed D3D9 call**. Immediately after
the final target's `GetDesc`/`Release`, the guest faults at PC `0x00fb5bd0` on
address `0x0000000c`. At and immediately after that PC, the accessor is:

```asm
mov eax,[ecx+0x40]
mov eax,[eax+8]
```

The log has no register dump, so it cannot prove which load faulted. The fault
address is consistent with `[ecx+0x40]` yielding `4` and the second load reading
`[4+8]`. Guest stack:

```text
esp=1007f8f0
stack=545b4c50 545b4c50 54114d58 00000002 00000002 00000002
```

The last successful D3D9 call is `IDirect3DTexture9::Release` on `0x500022c0`,
caller `0x00cecb99`, returning refcount `3`; the last successful creation is the
512x512 `0x71` target.

Sampling of `0x72` or `0x71` is **not proven**: no `SetTexture` and no draw
occurred, so there are no capture entries to correlate with the new handles.
The next concrete blocker is the guest render-target-group/accessor state at
`0x00fb5bd0`, after target allocation and before depth/scene setup. The fallback
and 64 MiB budget remain measurement-only; no frame is claimed.

### Cube-capable / bounded-RT census (2026-11-26)

Base: `main` at `061736c`, including six-face cube support `f23f03b`, bounded
native render targets `7256169`, the budget override `949701b`, HDR fallback
`ab4ec06`, depth descriptors `5e15944` and the R32F correction `68a3159`.
A fresh slice was built from `E:\Skyrim_N3DS_build\analysis\growth_probe\gen01`
with the generated D3DX table from
`C:\Users\vasya\Documents\Skyrim_N3DS\pilot_out_mult\D3DXMatrixMultiply\gen`
and six clang jobs. The build/test script took **163.7035036 s** and reported
**10 passed, 1 diagnostic, 0 failed**; the diagnostic is the slice's expected
200k-step harness run. The resulting binary was
`C:\Users\vasya\AppData\Local\Temp\opencode\sm3-gate-probe-cube-census-061736c-pilot\run_tesv_slice_37408\tesv_slice.exe`
(480,036,352 bytes).

The census used a 2,000,000,000-step budget and ran for **91.5521216 s**, exit `1`.
Arguments were `F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160`:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_BUDGET_MB=64
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_census.run.txt`

The cube path is **not reached**:

| Operation | Calls | Observed arguments / handles / HRESULT |
|---|---:|---|
| `IDirect3DDevice9::CreateCubeTexture` (slot 25) | 0 | none; no edge, levels, usage, format, pool, caller, HRESULT or handle |
| `IDirect3DCubeTexture9::GetCubeMapSurface` (slot 18) | 0 | none; no face, level, surface handle or HRESULT |
| `SetRenderTarget` with a cube surface | 0 | `SetRenderTarget` itself is not called |
| `CreateDepthStencilSurface` (slot 29) | 0 | none |
| `SetDepthStencilSurface` (slot 39) | 0 | none |
| `CreateVertexShader` / `CreatePixelShader` | 0 / 0 | none |
| `SetTexture` | 0 | none |
| `DrawPrimitive` / `DrawIndexedPrimitive` | 0 / 0 | none |
| `Present` | 0 | none |

The first-draw capture has no records. The D3D trace through the fault is
normalized-identical to the previous 64 MiB census (1245 lines, ignoring
volatile argument-stack slots and the host backtrace), so the cube and bounded-RT
merges do not change the observed route before the fault.

The same nine 2D render-target textures are created successfully. Bounded native
storage now caps each 1024x1024 logical target at 512x512:

| Logical size | Requested format | Count | Native size | Texture / surface handles | Backend target IDs |
|---|---|---:|---|---|---|
| 1024x1024 | `0x72` R32F | 5 | 512x512 | `0x500021c0`/`0x500021d0`, `0x500021e0`/`0x500021f0`, `0x50002200`/`0x50002210`, `0x50002220`/`0x50002230`, `0x50002240`/`0x50002250` | `15`, `17`, `19`, `21`, `23` |
| 512x512 | `0x15` A8R8G8B8 | 3 | 512x512 | `0x50002260`/`0x50002270`, `0x50002280`/`0x50002290`, `0x500022a0`/`0x500022b0` | `25`, `27`, `29` |
| 512x512 | `0x71` A16B16G16R16F | 1 | 512x512 | `0x500022c0`/`0x500022d0` | `31` |

All come from `caller=0x00ceca5e`, levels `1`, usage `1`, pool `0`, and return
`S_OK`. For all nine, `QueryInterface`, `GetSurfaceLevel(0)` and `GetDesc` return
`S_OK`; `AddRef` and `Release` return refcount `3`. The nine new native
allocations consume **9,437,184 bytes (9 MiB)**; including the retained 512x256
back buffer (524,288 bytes), the total is **9,961,472 bytes (9.5 MiB)**.

The first failed HRESULT in the complete trace is the nonfatal device-enumeration
call `IDirect3D9::CheckDeviceType` (slot 9), `caller=0x00ce58bf`, arguments
`[0, 1, 0x16, 0x16, 0]`, returning `D3DERR_NOTAVAILABLE` (`0x8876086a`). No cube,
depth or scene call fails because none is reached; all nine render-target
creations succeed.

The first fatal event remains a guest fault, immediately after the final
target's `GetDesc`/`Release`:

```text
PC=00fb5bd0
unmapped addr=0000000c bytes=4
esp=1007f8f0
stack=545b4c50 545b4c50 54114d58 00000002 00000002 00000002
```

Thus `0x00fb5bd0` is unchanged after the cube merge. The last successful D3D9
call is `IDirect3DTexture9::Release` on `0x500022c0`, caller `0x00cecb99`,
returning refcount `3`. The accessor at the fault is still:

```asm
mov eax,[ecx+0x40]
mov eax,[eax+8]
```

Sampling of `0x72` or `0x71` is **not proven**: there is no `SetTexture`, no draw
and no capture record to correlate with the handles. The next blocker is still
the guest render-target-group / `NiRenderedCubeMap` face-table accessor state at
`0x00fb5bd0`, after the 2D target allocations but before the guest issues any D3D9
cube, depth or scene call. This is not evidence of a missing host cube API. The
fallback, bounded native targets and 64 MiB budget remain measurement-only; no
frame is claimed.

### Water-off config census (2026-11-26)

A fresh slice was built from `main` at `bc23f42`
(`E:\Skyrim_N3DS_build\analysis\growth_probe\gen01`; 85.7 s census, exit 1).
`F:\Skyrim\Documents\My Games\Skyrim\SkyrimPrefs.ini` was backed up and a
`[Water]` section was added with `bUseWaterReflections=0`,
`bUseCubeMapReflections=0`, `bReflectSky=0`, `bReflectLand=0`,
`bReflectLODLand=0`, `bReflectLODObjects=0`, `bReflectLODTrees=0`. HDR/shadow
keys were not touched, and the INI was restored byte-for-byte afterwards
(SHA-256 `C0768CCC...D938CD`).

Result: **the fault is identical** тАФ `last_call=00fb5bd0`, address
`0000000c`, stack `545b4c50 545b4c50 54114d58 2 2 2`, last D3D call
`Release(0x500022c0)` refcount 3. `CreateCubeTexture`, `GetCubeMapSurface`,
`SetRenderTarget`, `CreateDepthStencilSurface`, `SetDepthStencilSurface`,
`CreateVertexShader`, `CreatePixelShader`, `SetTexture`, both draw entry points
and `Present` are all still 0. The nine 2D RTs are still created.

Reason: the `0xc92676` group is built unconditionally in `0xc92410`; the
renderer init reads the water-reflect dimensions (`iWaterReflectWidth/Height`
via `0x12cfa2c/0x12cfa30`) but not the `bUseWaterReflections` boolean (table
entry `0x126ef84`). The boolean is consumed later by the water pass. A
config-only skip is therefore not possible. The minimal honest skip is the
cube branch in `0xc91540` (`0xc9163e test dl,0x10` / `0xc91650 call 0xfb5c20`):
return a checked NULL before `0xfb5c20` when water reflections are off, or skip
the `0xc92676` call and store 0. A port-side D3D refusal cannot intercept
because the fault is before `CreateCubeTexture`. Full analysis:
`docs/CUBE_RENDER_TARGETS.md` section 10.

### Cube-skip measurement probe (2026-11-26)

`TESV_D3D9_CUBE_SKIP_PROBE=1` is **host-slice only and not shippable**. It
byte-verifies the retail window in `0xc91540` and patches the 10 bytes at
`0xc91650`:

```text
00c9163e: F6 C2 10              test dl,0x10        (verified)
00c9164d: 52 51 50              push edx/ecx/eax    (verified)
00c91650: E8 CB 45 32 00        call 0x00fb5c20     (verified, target checked)
00c916d1: 83 7C 24 38 00        cmp [esp+0x38],0    (verified)

patch:    83 C4 0C 33 F6 E9 77 00 00 00
          add esp,0xc; xor esi,esi; jmp 0x00c916d1
```

The AOT/IR path never re-fetches patched instruction bytes, so the slice also
links a narrow call-site shadow: `tesv_cube_probe_fb5c20()` returns `eax=0`
when the probe is active and the guest return address is `0x00c91655`, and
calls the real `f_FB5C20()` otherwise. The measurement build renames the three
generated `f_FB5C20(c)` call sites in `gen01/tesv_16.c` (the lifted
`f_C91540`, `f_C915AC`, `f_C91630` entries) to `tesv_cube_probe_fb5c20(c)` and
adds the prototype; `gen01/tesv_16.c` is restored byte-for-byte after the
build. Without that rename the AOT function bypasses the guest byte patch.
Both paths reach `0xc916d1` with `esi=0`, so `0xc91ba0` stores a NULL group and
the renderer's null-target diagnostic handles it. Caps, SM3 words and T&L are
unchanged; the probe prints `MEASUREMENT ONLY ... not shippable`.

Host test `tesv_cube_probe` (registered in `run_host_tests.ps1`) passes: exact
10-byte patch, only those bytes changed, repeat call refuses without writing,
wrong flag/push/call/target/epilogue bytes refuse without writing, and an
unmapped `GuestMemory` refuses. The fresh slice was built from `main` at
`bc6ae69` (`growth_probe/gen01`) and the census ran with the SM3, HDR-fallback
and 64 MiB budget probes for **90.243 s**, exit 1.

Result:

- **`0xfb5bd0` is gone.** The cube branch no longer calls
  `NiRenderedCubeMap::Create`.
- **New first blocker:** `[guest] unmapped addr=0000000b bytes=4
  last_call=00fb6090`. `0xfb6090` is `NiRenderTargetGroup` vtable
  `0x011b019c` slot 34 (`0x88`), `mov eax,[ecx+eax*4+0xc]; mov eax,[eax+0xc]`;
  the selected buffer slot is the `0xffffffff` sentinel, so `[[-1]+0xc]`
  faults at `0xb`. The active group is `[0x1b913f0+0x628]`; the skipped cube
  group's face/buffer entry was left as the `-1` sentinel rather than a zeroed
  checked NULL.
- No `CreateDepthStencilSurface`, `SetRenderTarget`, `CreateVertexShader`,
  `CreatePixelShader`, `SetTexture`, draw or `Present` yet. The last D3D call
  is still `Release(0x500022c0)` (the 512x512 `0x71` 2D target).
- `CreateCubeTexture` and `GetCubeMapSurface` are still 0 (the cube creation is
  skipped, not replaced by a fake cube).

Next layer: make the skipped group's buffer array a real zeroed checked NULL
(or skip the `0xc92676` group build and its slot bookkeeping) so the
`GetBufferData` query at `0xfb6090` sees NULL and returns 0; then re-run the
census for depth/shaders/draw.

### Fault-register / face-writer probe (2026-11-26)

Base: `main` at `ca60984` (default native render-target cap 256). Added the
measurement-only `TESV_FAULT_REGS=1` diagnostic in
`source/n3ds_lift_runtime.c` and the registered host smoke test
`tests/tesv_fault_regs_smoke.c`. The test checks the unchanged default output,
the enabled register/object dump, and an unreadable `ECX`; the targeted run
reported **11 passed, 0 diagnostic, 0 failed** with `-TestTimeoutSec 300`.

A fresh slice was built from `growth_probe/gen01` with the generated D3DX table,
six clang jobs, `N3DS_WATCH_STORE_CHECKS=1`, and a temporary PC-aware diagnostic
copy (1,582,082 block labels). The final build/test script took
**130.2141012 s** and reported **10 passed, 1 expected diagnostic, 0 failed**.
The census took **91.3600875 s**, exit `1`, with a 2,000,000,000-step budget:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_BUDGET_MB=64
TESV_FAULT_REGS=1
N3DS_WATCH_ADDR=0x545b4c90
N3DS_WATCH_VALUE=4
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_fault_watch_writer_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_fault_watch_writer_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_fault_watch_writer_census.run.txt`

The fault dump was:

```text
eax=00000000 ebx=545b4c50 ecx=40848c5c edx=00000002
esi=545b4c50 edi=54114d58 ebp=00000000 esp=1007f8f0 eip=00fb5bd0
[ecx+0x40]=00000000 [[ecx+0x40]]=unreadable [ecx+0x8]=00000000
stack=545b4c50 545b4c50 54114d58 00000002 00000002 00000002
unmapped addr=0000000c bytes=4
```

`ECX` is not the cube-map `this` in this AOT frame; the cube pointer
`0x545b4c50` is visible in `EBX` and `ESI`. The CPU model has no architectural
EIP, so the printed `eip=0x00fb5bd0` is the last dispatched virtual getter, not
the faulting instruction. Symbolizing the host backtrace identifies
`f_CED3E0` and the actual fault at **`0x00ced444`**:

```asm
mov ecx,[ebp+0xc]   ; EBP=0, so the effective address is 0x0c
```

The getter call at `0x00ced404` had just returned from `0x00fb5bd0`; the later
call to `0x00ce9a40` returned `NULL`, and `f_CED3E0` dereferenced it without a
check. `CreateCubeTexture` at `0x00ced472` is therefore still not reached.

The watchpoint found **no write of `4`** to `cube+0x40`. A targeted print after
the face-table store confirmed the actual writer and value:

```text
[cube-store] pc=00fb5cc1 addr=545b4c90 value=54882ec8
```

Thus face 0 (`m_pkBuffer[0]`) is written by the loop in `f_FB5C20` at
**`0x00fb5cc1`**, with a live `Ni2DBuffer*`-shaped guest pointer `0x54882ec8`.
`0x00fb5cd1` is the next-face loop step, not the face-0 writer. The generic
watchpoint first observed the changed field at `0x00fb5cc7` during a subsequent
`InterlockedIncrement` check; that PC is an observation point, not the store.
No field write was observed at `0x00c8ff40` or `0x00c91d77`; the latter is the
earlier group-builder width-getter call site.

This corrects the previous inference: the value `4` was not read from
`[cube+0x40]`. The fault address `0x0c` comes from `[EBP+0x0c]` with `EBP=0`.
The cap-256 build still faults at the same guest address and before any cube,
depth, scene, shader, draw or `Present` call. The next blocker is the unchecked
`NULL` returned by `0x00ce9a40` in `NiDX9RenderedCubeMapData::CreateSurf`, not
corruption of the face-0 pointer. This is measurement-only; no cube or frame is
claimed.

## Ordered next blockers

1. **SM3 renderer gate (`0x00c6e22d`).** Requires level 7 (VS/PS 3.0) while the
   bridge honestly reports 2.0/2.0. Flipping the two version words would only
   pass this branch; it would not make the device accept arbitrary `ps_3_0`
   programs, so the documented decision is to keep 2.0/2.0 until a real SM3
   execution path exists.
2. **Other D3DX hotpatch bodies.** The loader now follows the one-time
   initializer's real-body table, so it no longer mistakes `entry+11` for the
   body on exports such as `D3DXVec3TransformCoord`. All nine lazy-ILT imports
   are lifted: `D3DXMatrixMultiply` `0x0802fa80`, `D3DXMatrixTranspose`
   `0x0802fce0`, `D3DXMatrixMultiplyTranspose` `0x0802fd9d`,
   `D3DXPlaneTransform` `0x08032303`, `D3DXVec3TransformCoord` `0x08032ead`,
   `D3DXVec3TransformNormal` `0x0803306a`, `D3DXVec3Normalize` `0x08032cbf`,
   `D3DXPlaneNormalize` `0x08032000` and `D3DXMatrixInverse` `0x0802fffd`
   (plus the internal helper `0x0802e9db` and the `_CIsqrt`/`_finite` shims
   `0x081a65ec`/`0x081a6694`; twelve table entries). Each has its own 3/3
   pilot test. The nine plain D3DX exports (image/texture load, surface load,
   `D3DXCompileShader`) are not lazy-ILT thunks and still need host shims or a
   lifted closure. Generation, merge and per-body test details are in
   `docs/D3DX_PILOT_TABLE.md`; `pilot_out*/` is generated locally and ignored.
3. **HW T&L shipping gate (`0x00c6d8c6`).** Still deferred
   (`docs/HWTL_DECISION.md`); the probe is measurement-only.
4. **Two texture stages (honest).** Implemented: a backend with `draw_two`
   makes the COM caps report `2/2`; without it the renderer refuses the device
   (step 5 is the historical failure).
5. **Small textures.** Implemented: power-of-two sizes below 8 are stored in a
   minimum 8x8 allocation with replicated texels and point sampling at level 0;
   logical dimensions and row pitch stay honest.
6. **Level surfaces.** Implemented: `GetSurfaceLevel` exposes a CPU-backed view
   of the level; writing it re-uploads the parent texture. BC levels are
   refused by name.

## Status of this patch

- Hotpatch resolution has a host regression test (positive chain, a one-byte
  padding negative and a `push 2` initializer-guard negative). The Windows
  suite is **61 passed, 1 diagnostic, 0 failed**; the diagnostic is
  `tesv_slice` hitting the harness's 200k-step budget, not a fault in the fixed
  route (the CAPS+PP probe uses the larger budget and reaches the SM3 gate).
- With the probes on, the route reaches the multiply body, executes it, and
  continues to the SM3 gate with `CreateTexture`/`GetSurfaceLevel`/`LockRect`
  returning `S_OK`.
- A final rerun on `d6f5bf3` (`E:\Skyrim_N3DS_build\analysis\trace4.err`,
  copied to `F:\Skyrim\diag\20261126_hotpatch\`) reproduces the import binding
  and the same SM3 message.
- Shipping behaviour is unchanged: still gated at `0x00c6d8c6` until the
  `HW T&L` decision (`docs/HWTL_DECISION.md`).
