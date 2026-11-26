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

The measurement probe now also:

- NOPs `0xc92698` (`89 44 B4 40`, store the cube group) and `0xc926a6` (`46`,
  inc `esi`) so the skipped cube slot is not stored and the caller's count
  stays consistent;
- shadows a 4-byte `0xffffffff` read inside a `NiRenderTargetGroup`
  (`tesv_cube_probe_ptr`, vtables `0x011b019c`/`0x011539d8`);
- runs a conditional `0xfb6090`/`0xfb6070` dispatch shadow; and
- with `TESV_CUBE_PROBE_VTABLE=1`, points vtable slots 33/34
  (`0x011b0220` -> `0xfb6070`, `0x011b0224` -> `0xfb6090`) at the synthetic
  host entries `0x30000000`/`0x30000010`. No guest stub bytes are written;
  the synthetic targets are bound as host `FnEntry` functions.

The conditional stub classifies the receiver and index first
(`tesv_cube_probe_slot_state`): only a real group (vtables
`0x011b019c`/`0x011539d8`) with an in-range, readable `Ni2DBuffer` pointer in
`[group+0xc+arg*4]` is allowed to run the real container getter, so early
renderer width/height queries keep their real results. Sentinel slots (`0`,
`0xffffffff`, small integers below `0x10000`) and unreadable receivers return
`eax=0` and emulate the callee's `ret 4`. Without the env the old bytes and
behavior remain.

Host test `tesv_cube_probe` PASS: exact 15 code bytes, the two 4-byte vtable
entries, only the expected bytes change, repeat refusal, wrong
flag/push/call/target/epilogue/store/inc/vtable bytes, valid slot -> real
field (0x08/0x0c), sentinel -> -1, small integer -> -1, unreadable slot -> 0,
bad `this`/vtable -> 0, group-sentinel shadow, and unmapped `GuestMemory`.

Final census on a fresh `main` `0e0b460` slice (`growth_probe/gen01`) with
MEMPROBE + CAPS + TRACE + SM3 + HDR fallback + budget64 + cube skip + shader
skip + `TESV_CUBE_PROBE_VTABLE=1`, 2e9 steps: **216.513 s**, exit 1.

- **`0xfb6090`, `0xfb6070` and `0x30000000` are gone from the fault log.**
  `CreateTexture` rises from 3 to **32**; the renderer gets past the
  `NiRenderTargetGroup` getter.
- First tuple: `Direct3DCreate9 -> 0x50002010`. Last tuple before the fault:
  `IDirect3DSurface9::Release slot=2 this=0x50002190 caller=0x00ce715b ->
  S_OK` (4x4 texture path).
- New first blocker: the pre-existing `NiDX9TextureBufferData` descriptor copy
  in `f_CECDC0` at **`0x00ced19d rep movsd`**, count `ecx=0x11` dwords
  (68 bytes), `esi=0`, destination `[ebp+0x14]`, fault address `0`; registers
  `eax=1 ebx=0x54115280 ecx=0x11 edx=0x408486b0 esi=0 edi=0x54515a14
  ebp=0x54515a00 esp=0x1007fab0`; symbolized backtrace `f_CECDC0 ->
  f_CD4E40 -> f_AE7CD0 -> f_C739B0 -> f_C74710`. This is the same guest
  `[esp+0x18]`/NULL descriptor-slot blocker documented in the earlier
  cube descriptor-slot section, not a getter, cube-create HRESULT or PICA
  refusal.
- Still 0: `CreateCubeTexture`, `GetCubeMapSurface`,
  `CreateDepthStencilSurface`, `SetRenderTarget`, `SetDepthStencilSurface`,
  `CreateVertexShader`, `CreatePixelShader`, `SetTexture`, both draw entry
  points and `Present` (the only `Present` string is the capture-enable line).
  `SetMaterial`/`SetLight` are 0.

Because the conditional stub reached the next real layer, the dummy
`Ni2DBuffer` alternative was not added. The probe is measurement-only and not
shippable; no cube or frame is claimed.

Next layer: initialize/validate the `f_CECDC0` `[esp+0x18]` base array before
`0x00ced04b` (and the `0xced170` source slot before the 68-byte copy), as
recorded earlier; that is a guest stack/path precondition, not a D3D9 cube
create HRESULT.


### CECDC0 call-site skip probe (2026-11-26)

`TESV_CECDC0_SKIP=1` is host-slice only and not shippable. It targets the
direct calls to `0x00cecdc0` (`NiDX9TextureBufferData` creation):

- **Primary `0x00cd4e63` (f_CD4E40):** verified `E8 58 7F 01 00` (target
  `0x00cecdc0`), continuation `83 C4 08`, and the NULL check `8B F8` /
  `85 FF` / `0F 95 C0`. The five call bytes become
  `33 C0 90 90 90` (`xor eax,eax` + NOPs); the caller's existing
  `add esp,8` removes the two pushed arguments, so the stack is balanced and
  `eax=0` reaches the tested failure path.
- **Secondary `0x00ce77f8`:** verified `E8 C3 55 00 00` then
  `83 C4 08 5E 83 C4 48 C3`; the caller discards EAX (no test), so it is
  deliberately **not patched** and the probe prints a named line.

The AOT/IR shadow `tesv_cecdc0_probe_shadow()` covers the generated
`f_CD4E40` call at return address `0x00cd4e68`: it returns `eax=0`, pops only
the return address (`c->r[4] += 4`), and lets the existing `add esp,8` remove
the arguments. The measurement build renames that one generated
`f_CECDC0(c)` call with `tools/instrument_cecdc0.py`; every other caller runs
the real function. Host test `tesv_cecdc0_probe` PASS (exact primary
call/target/continuation/NULL-test bytes, only the five call bytes change,
repeat refusal, wrong opcode/target/continuation/NULL-test refusal, secondary
site untouched, unmapped memory).

Census on a fresh `0ecb6a9` slice (`growth_probe/gen01`) with MEMPROBE + CAPS +
TRACE + SM3 + HDR fallback + budget64 + cube skip + shader skip +
`TESV_CECDC0_SKIP=1`, 2e9 steps: **4.053 s**, exit 1, and **no D3D trace
lines at all** (`CreateTexture` = 0). The first fatal event is:

```text
[guest] unmapped addr=00000008 bytes=4 last_call=00ac6400
eax=0 ebx=3 ecx=0 edx=011191d8 esi=0 edi=01b37fb0 ebp=01b37fe4 esp=1007fcf4
backtrace: f_A67590 -> f_A68770 -> f_69D1D0 -> f_F568A1
```

The fault is `[this+0x34] == NULL` at the caller's texture-list walk, before
`Direct3DCreate9`; the skipped `f_CD4E40`/`f_CECDC0` call is the **first**
texture/cube data creation, not the water-reflection cube. Thus the
unconditional call-site skip is not cube-specific: it breaks the first cube
data object and the process dies earlier. `CreateCubeTexture`,
`GetCubeMapSurface`, `CreateDepthStencilSurface`, `SetRenderTarget`,
`SetDepthStencilSurface`, `CreateVertexShader`, `CreatePixelShader`,
`SetTexture`, both draw entry points and `Present` are all 0; no frame is
claimed.

**Route closed for this call-site skip.** The only honest next option is a
conditional skip inside `f_CECDC0` when the descriptor lookup is actually
NULL (`0x00ced19d` / the `[esp+0x18]` base slot), or initialize that slot
before `0x00ced04b`; both are different probes, not an unconditional call-site
replacement.

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

### CheckDeviceFormat fallback census (2026-11-26)

Base: `main` at `c9e0ced`. Under `TESV_D3D9_HDR_LDR_FALLBACK=1`,
`d9_adapter_check_device_format` now returns `S_OK` for measured float formats
`0x71`/`0x72` with `D3DUSAGE_RENDERTARGET` for `D3DRTYPE_SURFACE`,
`D3DRTYPE_TEXTURE` and `D3DRTYPE_CUBETEXTURE`. It logs
`MEASUREMENT ONLY ... not shippable`; without the env the previous refusals are
unchanged. `D9Caps9`, SM2 versions, SM3 slots and T&L bits are not modified.
`tests/adapter_smoke.c` covers default-off refusals, all six env-on format/type
combinations, negative usage/type/format cases, and a byte-for-byte caps
comparison. Targeted adapter tests reported **11 passed, 0 failed** with
`-TestTimeoutSec 300`; `render_texture_hdr` also passed.

A fresh slice was built from `growth_probe/gen01` with six clang jobs and the
generated D3DX table. The build/test script took **157.9543572 s** and reported
**10 passed, 1 expected diagnostic, 0 failed**. The generated slice already
references `tesv_cube_probe_fb5c20`, so the host build linked the required
`source/tesv_cube_probe_slice.c` through a temporary build-script copy; the
repository build script was not changed. The census took **93.6686395 s**,
exit `1`, with a 2,000,000,000-step budget:

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

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_checkformat_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_checkformat_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_checkformat_census.run.txt`

The first measured float RT queries now succeed:

```text
CheckDeviceFormat(adapter=0, device=1, adapter_format=0x16,
                  usage=1, resource_type=1, format=0x71) -> S_OK
CheckDeviceFormat(adapter=0, device=1, adapter_format=0x16,
                  usage=1, resource_type=1, format=0x72) -> S_OK
```

Both are from `caller=0x00ce54f4`. The host test also covers RT TEXTURE and
CUBETEXTURE, but the retail cube path does **not** issue an RT usage query.
Its descriptor lookup uses `usage=0`:

```text
CheckDeviceFormat(adapter=0, device=1, adapter_format=0x16,
                  usage=0, resource_type=5, format=0x71) -> 0x8876086a
CheckDeviceFormat(adapter=0, device=1, adapter_format=0x16,
                  usage=0, resource_type=5, format=0x72) -> 0x8876086a
```

These calls are from `caller=0x00cd5542` and remain `D3DERR_NOTAVAILABLE` by
design because this probe only extends RENDERTARGET usage. Consequently the
expected `CreateCubeTexture(512,1,1,0x71,0)` is **not reached**. Counts remain
zero for `CreateCubeTexture`, `GetCubeMapSurface`, `SetRenderTarget`,
`CreateDepthStencilSurface`, `SetDepthStencilSurface`, `CreateVertexShader`,
`CreatePixelShader`, `SetTexture`, draw and `Present`; capture has no records.

The first fatal event is unchanged. Symbolizing the host backtrace still identifies
`f_CED3E0` and the actual fault at **`0x00ced444`** (`mov ecx,[ebp+0xc]`, with
`ebp=0`), address `0x0000000c`; `last_call=0x00fb5bd0` remains the earlier width
getter. The cube descriptor refusal leaves `0x00ce9a40` with no matching
descriptor, so it returns NULL and `CreateSurf` dereferences it before
`CreateCubeTexture` at `0x00ced472`.

The next blocker is therefore the **usage-0 CUBETEXTURE descriptor query** for
`0x71`/`0x72`, not the RT capability check. A further measurement-only probe
would have to admit that descriptor query (or implement a real cube descriptor)
before the cube create/surface/depth path can be measured. This env is not
shippable; no cube or frame is claimed.

### Usage-0 cube descriptor fallback census (2026-11-26)

Base: `main` at `4204b1a`. Under `TESV_D3D9_HDR_LDR_FALLBACK=1`,
`d9_adapter_check_device_format` now also accepts `usage=0`,
`D3DRTYPE_CUBETEXTURE`, formats `0x71`/`0x72`. This is the exact descriptor
query used by the retail cube path. Default-off refusals remain unchanged;
`D9Caps9`, SM2/SM3 words and T&L bits are not modified. The adapter smoke test
covers env-on/off descriptor checks, negative format/type/usage cases and the
byte-for-byte caps comparison. Targeted `adapter,render_texture,render_texture_hdr`
reported **13 passed, 0 failed** with `-TestTimeoutSec 300`.

A fresh slice was built from `growth_probe/gen01` with six clang jobs and the
generated D3DX table. As before, the temporary host build linked the required
`source/tesv_cube_probe_slice.c` for the already-instrumented generated calls.
The build/test script took **164.2214211 s** and reported **10 passed, 1 expected
diagnostic, 0 failed**. The census took **95.9922741 s**, exit `1`, with a
2,000,000,000-step budget:

```text
TESV_MEMPROBE=1
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_BUDGET_MB=64
TESV_FAULT_REGS=1
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
```

Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_desc_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_desc_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_desc_census.run.txt`

The measured cube descriptor queries now succeed:

```text
CheckDeviceFormat(0,1,0x16,usage=0,type=CUBETEXTURE,format=0x71) -> S_OK
CheckDeviceFormat(0,1,0x16,usage=0,type=CUBETEXTURE,format=0x72) -> S_OK
caller=0x00cd5542
```

The old `0x00ced444` NULL dereference is gone. However, no
`CreateCubeTexture(512,1,1,0x71,0)` is issued, so the conditional CPU-backed
cube-create fallback was not added. `GetCubeMapSurface`, `SetRenderTarget`,
`CreateDepthStencilSurface`, `SetDepthStencilSurface`, `CreateVertexShader`,
`CreatePixelShader`, `SetTexture`, draw and `Present` all remain at zero.

The first fatal event is now a guest NULL source in `f_CECDC0`:

```text
last_call=004b7a20
eax=00000001 ebx=54115280 ecx=00000011 edx=408486b0
esi=00000000 edi=54515a14 ebp=54515a00 esp=1007fab0
unmapped addr=00000000 bytes=68
```

The symbolized backtrace is `f_CECDC0 -> f_CD4E40 -> f_AE7CD0 -> f_C739B0 ->
f_C74710`. The actual fault is **`0x00ced19d rep movsd`**, count `0x11` dwords
(68 bytes), with `ESI=0` and destination `[EBP+0x14]`. The zero source slot is
loaded at `0x00ced170` (`mov eax,[ecx+eax*4]`) and moved to `[esp+0x14]` at
`0x00ced176`; `0x00ced18b` then copies it to `ESI`.

Thus the next blocker is guest format/buffer-table bookkeeping in
`NiDX9TextureBufferData` (`f_CECDC0`): after the usage-0 cube descriptor is
admitted, a later 68-byte descriptor copy reads a NULL table slot. It is not a
`CreateCubeTexture` HRESULT and not a PICA cube-callback refusal. The env remains
measurement-only and not shippable; no cube or frame is claimed.

### Cube descriptor-slot probe (2026-11-26)

Base: branch head `47a256f`. Added the separate env-gated
`TESV_CUBE_DESC_PROBE=1` helper `n3ds_cube_desc_probe()` and
`tools/instrument_cube_desc.py`. The tool instruments a disposable generated
slice copy immediately after the `L_CED041` and `L_CED160` slot computations;
it found 7 and 8 copies respectively in `gen01`. The checked-in generated
sources are untouched. `tests/tesv_cube_desc_probe_smoke.c` covers default-off
silence, the enabled NULL-slot dump, the first eight base dwords, and the
`0x16` sentinel comparison. Targeted `tesv_cube_desc_probe,adapter` reported
**12 passed, 0 failed** with `-TestTimeoutSec 300`.

The instrumented slice build took **163.961826 s** and reported **10 passed,
1 expected diagnostic, 0 failed**. The census took **112.7162557 s**, exit `1`,
with a 2,000,000,000-step budget and the usual SM3/HDR-fallback/budget64 env plus
`TESV_CUBE_DESC_PROBE=1`. Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_slot_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_slot_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_cube_slot_census.run.txt`

The exact slot dump was:

```text
[cube-desc] arg=408486b0 index=2 base=40848c04 slot=00000000
slot_null=yes base_null=no index_eq_0x16=no index_ge_0x16=no
source_obj=548b6a50 source_index=00000002 source_ok=yes
[cube-desc] base8=00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
```

This rules out an out-of-range or sentinel index: `index=2` is below `0x16`,
and the source object at `0x548b6a50` also supplies `2` from `+0x10`. The index
is initialized by the `f_CD2EA0`/`f_CD2F16`/`f_CD2F4D` family, while the
renderer constructor `f_CD2620` sets the `+0x624` default sentinel to `0x16`.

The NULL is instead the local base array at `[esp+0x18]`. It points into the
guest stack (`0x40848c04`), and all eight inspected dwords are zero. Static
disassembly finds no write to `[esp+0x18]` in `f_CECDC0` before the read at
`0x00ced04b`; the first write is at `0x00ced42e`, after the fault. Therefore
this is an uninitialized stack/base-slot path, not a missing `0x71` format
record and not an invalid index. Expanding usage-0 `CheckDeviceFormat` to
TEXTURE/SURFACE was not added because the measured dump excludes case (a).

The behavior is unchanged: `CreateCubeTexture` is still absent, all cube/depth/
scene/shader/texture/draw/Present counts remain zero, and the first fatal event
is still `0x00ced19d rep movsd` with `ESI=0`, count `0x11`, address `0`. The next
blocker is to initialize or validate the `f_CECDC0` `[esp+0x18]` base array
before `0x00ced04b`; the index itself is already populated correctly. This is a
guest stack-slot/path-precondition issue, not a D3D9 format capability or cube
create HRESULT. The probe is measurement-only; no cube or frame is claimed.

### Final 0x71-cube descriptor diagnostic (2026-11-26)

Base: branch head `ae3e3dc`. Under the same measurement env,
`CheckDeviceFormat` now also accepts `usage=0` for SURFACE, TEXTURE and
CUBETEXTURE formats `0x71`/`0x72`. Default-off refusals and byte-identical
`D9Caps9` remain covered by `adapter_smoke`; caps/SM/T&L are unchanged. Targeted
`tesv_cube_desc_probe,adapter` reported **12 passed, 0 failed** with
`-TestTimeoutSec 300`.

A fresh instrumented probe-slice build took **158.0464922 s** and reported
**10 passed, 1 expected diagnostic, 0 failed**. The final census took
**86.4307506 s**, exit `1`, with the usual fallback+budget64 environment plus
`TESV_CUBE_DESC_PROBE=1`. Logs:

- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_final_diagnostic_census.err`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_final_diagnostic_census.out`
- `E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_final_diagnostic_census.run.txt`

All measured usage-0 SURFACE/TEXTURE/CUBETEXTURE queries for `0x71`/`0x72` now
return `S_OK` at `caller=0x00cd5542`. The descriptor-array dump is unchanged:

```text
[cube-desc] arg=408486b0 index=2 base=40848c04 slot=00000000
slot_null=yes base_null=no index_eq_0x16=no index_ge_0x16=no
source_obj=548b6a50 source_index=00000002 source_ok=yes
[cube-desc] base8=00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
```

The short fault frame is:

```text
esp=1007fab0
[esp+0x00]=541154d8 [esp+0x04]=408486b0 [esp+0x08]=541153e8
[esp+0x0c]=408486cc [esp+0x10]=54115280 [esp+0x14]=00000000
[esp+0x18]=40848c04  (base array; first eight dwords all zero)
[esp+0x54]=408486b0  (argument/renderer object)
ebp=54515a00
backtrace: f_CECDC0 -> f_CD4E40 -> f_AE7CD0 -> f_C739B0 -> f_C74710
```

The probe fires in `L_CED041`, which is reached only when `[ebp+0x70]` is
nonzero (the error/fallback branch sets it to `1`). That branch indexes the
uninitialized local array at `[esp+0x18]`. The other branch, `[ebp+0x70]==0` at
`L_CED067`, instead copies the same 68-byte descriptor from `ebx+0x14`. No call
in `f_CECDC0` populates `[esp+0x18]` before `0x00ced04b`; its first write is at
`0x00ced42e`, after the copy. `f_CD4E40` calls `f_CECDC0` with two pushed
arguments and does not pass a descriptor-array pointer.

Therefore expanding `CheckDeviceFormat` does not build this engine-local array.
The `0x71`-cube measurement path leads to an engine path whose descriptor array
is never built; this is a **dead end for the measurement hack**, not a concrete
missing bridge D3D9 call. No guest code was patched and no fake descriptor data
was supplied. The supported measurement route is the existing cube-skip probe.

The final fault remains `0x00ced19d rep movsd`, count `0x11`, `ESI=0`, address
`0`; `CreateCubeTexture`, depth, scene, shaders, `SetTexture`, draw and `Present`
remain unreached. No cube or frame is claimed.

### Shader-skip probe (2026-11-26)

`TESV_D3D9_SHADER_SKIP_PROBE=1` is host-slice-only and **MEASUREMENT ONLY**. It
does not change `GetDeviceCaps`, the SM3 words or T&L, and it must never become
a shipping default.

- With the env unset, `CreateVertexShader`/`CreatePixelShader` keep the existing
  closure and legacy behavior: unknown token streams still return
  `D3DERR_NOTAVAILABLE`/`D3DERR_INVALIDCALL` and no object is created.
- With the env set, a token stream that fails `d9_menu_closure_certify()` is
  turned into a named object: the existing token walk supplies the END length,
  the original token stream is retained only for `GetFunction`, and the object
  carries `probe_skip=1`, `has_menu=0`, `has_tev=0` and no draw plan. The device
  is retained and Create* returns `S_OK`. One `MEASUREMENT ONLY ...` line is
  printed for the first such object.
- `SetVertexShader`/`SetPixelShader` accept the object through the normal
  lookup. `shader_state()` checks `probe_skip` before every menu/legacy
  precondition and refuses any draw with `D3DERR_NOTAVAILABLE` and
  `shader-skip probe: draw with a code-less shader is refused (measurement only)`.
  No batch is submitted and no pixels are fabricated.

Host test `tests/shader_skip_probe_smoke.c` (registered as `shader_skip_probe`)
covers env-off rejection, env-on create/`GetFunction`, QI `IUnknown`,
`AddRef`/`Release`, repeated create/release, `Set*`, the named draw refusal,
caps `0x00082000` unchanged, the unchanged NORMAL-FVF refusal, and shutdown.
Targeted run PASS; full suite **106 passed, 0 failed** (the full suite does not
register `tesv_slice` without `-TesvSliceGen`; its separate targeted build was
10 passed, 1 expected diagnostic).

#### Census (evidence: `E:\Skyrim_N3DS_build\analysis\shader_skip_probe_census2.err`)

Slice `E:\Skyrim_N3DS_build\analysis\growth_probe\gen01` (its three
`tesv_cube_probe_fb5c20` call-site renames are present; the companion
`source/tesv_cube_probe_slice.c` is now in the `tesv_slice` source list).
Targeted slice build: **10 passed, 1 expected diagnostic**, 153 s. Run:

```text
TESV_MEMPROBE=1 TESV_D3D9_CAPS_PROBE=1 TESV_D3D9_TRACE=1
TESV_D3D9_SM3_GATE_PROBE=1 TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_BUDGET_MB=64 TESV_D3D9_CUBE_SKIP_PROBE=1
TESV_D3D9_SHADER_SKIP_PROBE=1 N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
tesv_slice.exe F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160
```

Result: **94.791 s**, exit `1`. First trace tuple `Direct3DCreate9 ->
0x50002000`; the next reported tuple is `GetAdapterCount -> 1`. The last D3D
tuple before the fault is
`IDirect3DSurface9::Release slot=2 this=0x50002190 caller=0x00ce715b -> S_OK`.
Then the first fatal event is
`[guest] unmapped addr=00000000 bytes=68 last_call=0x004b7a20` with
`esp=1007fab0 stack=541154d8 408486b0 541153e8 408486cc 54115280 00000000`,
i.e. the same `0x00ced19d rep movsd` (count `0x11`, `ESI=0`) format/buffer-table
blocker recorded above.

Counts in this run: `CreateDepthStencilSurface` 0, `SetRenderTarget` 0,
`SetDepthStencilSurface` 0, `CreateVertexShader` 0, `CreatePixelShader` 0,
`SetTexture` 0, both draw entry points 0, `Present` 0, and `CreateCubeTexture`
0. The engine therefore never creates a probe object: the census dies in the
format descriptor path before any shader creation. The shader-skip behavior is
covered by the host test, not by this census. Cube-skip is active; it does not
fault at `0xfb6090` here because the earlier format/descriptor path dies first.

Next blocker: initialize or validate the `f_CECDC0` `[esp+0x18]` base array
before `0xced04b` (or skip the `0xc92676` group build and its slot bookkeeping)
so the 68-byte descriptor copy stops reading NULL. Only then can the census
reach `CreateCubeTexture`/depth/render-target/shaders/`SetTexture`/draw/`Present`.
The probe is measurement-only; no cube or frame is claimed.

ARM `-Mode com -Isolated shader_skip`: `.elf` 5,912,604 B SHA256
`ef05ee582eb21aed7050b48e6da687f39869644a6bd88b22166ef583a63054f4`;
`.3dsx` 867,460 B SHA256
`7b33e74dc3a1338dfdcc87be8105fea982ecceafe0a7dbdd4911bbf782b78e5a`;
`.text` 849,836 B.

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
