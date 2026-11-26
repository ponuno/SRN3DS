# Cube render target and the `0x00fb5bd0` fault (analysis, 2026-11-26)

Status: **analysis only**. No production change. This note corrects the
cube-map conclusion in `docs/FLOAT_RENDER_TARGETS.md` using the 64 MiB census
and a fresh stack match. The short version: **no `IDirect3DCubeTexture9` is
created in this run**. The fault is inside the guest's
`NiDX9RenderedCubeMapData::CreateSurf` *before* `CreateCubeTexture`; the receiver
is a guest `NiRenderedCubeMap` object whose face-0 pointer is invalid.

## 1. What the 64 MiB census actually did

All nine 2D render targets were created (`S_OK`): 5 x 1024 R32F, 3 x 512 A8,
1 x 512 A16F. The last D3D9 call was
`IDirect3DTexture9::Release` on `0x500022c0`, refcount 3, caller `0x00cecb99`.
Then:

```text
[guest] unmapped addr=0000000c bytes=4 last_call=00fb5bd0
[guest] esp=1007f8f0 stack=545b4c50 545b4c50 54114d58 00000002 00000002 00000002
Guest fault at 0000000c: unmapped guest memory
```

Cube-related counts are **zero**: `CreateCubeTexture` (slot 25),
`GetCubeMapSurface`, `SetRenderTarget`, `CreateDepthStencilSurface`,
`SetDepthStencilSurface`, `SetTexture`, any draw and `Present`. The only
resource-type-5 `CheckDeviceFormat` calls are the 40 nonfatal `usage=0x20000`
probes at caller `0x00cd5542`; none is render-target usage.

`g_last_call` is the guest function the dispatcher entered
(`source/n3ds_lift_runtime.c:379` and `:422`), so `0x00fb5bd0` is the callee.
The fault is inside its two-load body.

## 2. The fault is the guest cube-data prologue, not the group builder

`0x00fb5bd0` is `NiRenderedCubeMap` vtable `0x011b0064` slot 35 (`0x8c`):

```asm
00fb5bd0: 8b 41 40   mov eax,[ecx+0x40]   ; m_pkBuffer[0] / face 0 (Ni2DBuffer*)
00fb5bd3: 8b 40 08   mov eax,[eax+8]      ; face width
00fb5bd6: c3         ret
```

This is the only dword `0x00fb5bd0` in the image (slot 36 is the height getter
`0x00fb5be0`). So the receiver is a `NiRenderedCubeMap` **object**, not a
render-target group, depth buffer or 2D texture wrapper.

The stack matches `NiDX9RenderedCubeMapData::CreateSurf` at `0x00ced3e0`,
prologue `sub esp,0xc; push esi; push edi; push ebp`, at the call
`0x00ced404`:

```asm
00ced3e0: 83 ec 0c             sub esp,0xc
00ced3e3: 56                   push esi            ; saved esi = data object
00ced3e4: 8b 74 24 14          mov esi,[esp+0x14]  ; esi = arg1 = cube map
00ced3e8: 57                   push edi            ; saved edi = cube map
00ced3e9: 8b f9                mov edi,ecx         ; edi = data object
00ced3f9: 8b 06                mov eax,[esi]       ; cube-map vtable
00ced3fb: 8b 90 8c000000       mov edx,[eax+0x8c]  ; GetWidth
00ced401: 55                   push ebp            ; saved ebp = cube map
00ced402: 8b ce                mov ecx,esi
00ced404: ff d2                call edx            ; <- fault
```

Stack slots at the call:

| slot | value | meaning |
| --- | --- | --- |
| `[esp]` | `0x545b4c50` | saved ebp = `0xfb5c20`'s ebp = cube map |
| `[esp+4]` | `0x545b4c50` | saved edi = `0xcd4e90`'s edi = cube map |
| `[esp+8]` | `0x54114d58` | saved esi = data object allocated in `0xced4b0` |
| `[esp+0xc..]` | `2 2 2` | `CreateSurf` locals / FormatPrefs copies |

So `ecx` at `0xfb5bd0` is the cube map `0x545b4c50`; the data object is
`0x54114d58`. The call is **before** `CreateCubeTexture` (`0xced472`) and
before any `GetCubeMapSurface` (`0xce8a1e`). That is why the trace has zero
cube calls: the guest never reaches the D3D entry points.

This also rules out `0xc91ba0+0xd77` as the faulting site. That site is real and
has the same shape (`group->+0x30` texture, vtable slot `0x8c`), but its saved
registers are not the `0xced3e0` pattern (data object `0x545b4c50` / cube map
`0x54114d58`). The group builder is not the caller.

### 2.1 The tuple the guest would request

If `0xced404` returned, `0xced3e0` would call device slot 25 with:

```text
CreateCubeTexture(
    EdgeLength      = 512,          ; max(width,height), pow2
    Levels          = 1,
    Usage           = 1,            ; D3DUSAGE_RENDERTARGET
    Format          = 0x71,         ; A16B16G16R16F
    Pool            = 0,            ; D3DPOOL_DEFAULT
    ppCubeTexture   = &out,
    pSharedHandle   = NULL)
```

The six faces would each be `GetCubeMapSurface(face, 0)` (slot 18, called at
`0xce8a1e`), then `SetRenderTarget` (slot 37) and a 512x512 depth attachment
for the face. None of this happens in the census: the guest faults at the
width query before `CreateCubeTexture`.

## 3. Which object and which field are stale

`NiRenderedCubeMap` layout (ctor `0x00fb5bf0`):

| offset | field | writer |
| ---: | --- | --- |
| `+0x00` | vtable `0x011b0064` | `0xfb5bfa` |
| `+0x08/+0x0c/+0x10` | FormatPrefs (default `6,3,2`) | `0xfb5e1f..0x2d`, `0xfb5c71..0x7f` |
| `+0x14` | renderer data | base ctor zeroes; `0xcd4e90` registers |
| `+0x24` | unused 2D buffer field | zeroed |
| `+0x30/+0x31` | flags | `0xfb5cdb..0x5cf7` |
| `+0x34` | explicit D3DFMT | `0xfb5cee` |
| `+0x3c` | face/context index | `0xfb5c12` zeroes; group code uses it |
| `+0x40..+0x54` | six `Ni2DBuffer*` faces | `0xfb5bf0` zeroes at `0xfb5c00`; `0xfb5c20` fills at `0xfb5cc1..0xfb5cd9` |

The fault address is `0xc`, not `0x8`. A NULL at `+0x40` would fault at
`0x8`; `0xc` means either `[ecx+0x40] = 4` and the second load read `[4+8]`, or
`ecx = 0xffffffcc`. The stack match says `ecx = 0x545b4c50`, so the face-0
entry is **4**, not a live `Ni2DBuffer*`.

The cube map was allocated by `0xfb5c20` (`ebp`, saved on the stack), so the
face loop should have written six heap pointers. The `+0x40 = 4` is therefore
not a valid face. Either the loop did not run for this instance, its
`0xfb69d0(size,size)` returned a non-pointer, or `+0x40` was overwritten after
the loop. The static text cannot distinguish these; section 7 gives a one-run
probe.

For comparison, the 2D texture getter is `0x00fb67a0`
(`NiRenderedTexture` vtable `0x011affc4` slot 35):

```asm
00fb67a0: 8b 41 24   mov eax,[ecx+0x24]   ; 2D buffer
00fb67a3: 85 c0      test eax,eax
00fb67a5: 74 04      je 0xfb67ab
00fb67a7: 8b 40 08   mov eax,[eax+8]
00fb67aa: c3         ret
00fb67ab: 33 c0      xor eax,eax
00fb67ad: c3         ret
```

A 2D wrapper with a bad `+0x24` would fault at `0xfb67a0`, not `0xfb5bd0`.
The receiver is therefore the cube-map wrapper, not the 2D wrapper.

## 4. Group fields (for the probe and the next layer)

The renderer-side group is built by `0xc8ff40` (vtable `0x011539d8`, 0x40 bytes):

| offset | field |
| ---: | --- |
| `+0x08` | single buffer (2D group) |
| `+0x0c..+0x20` | six face sub-groups (`NiRenderTargetGroup*`) for a cube |
| `+0x2c` | `-1` sentinel |
| `+0x30` | owning `NiTexture*` (the cube map for a cube group) |
| `+0x34/+0x38` | ctor args (count/format) |
| `+0x3c` | current face index used by `0xc8fc90` |

`0xc8fc90` reads `[group+0x30]`; if that is a `NiRenderedCubeMap` (class
comparison at `0xc8fca1`), it uses `[cube+0x3c]` to select
`[group + face*4 + 8]`; otherwise it returns `[group+8]`. The group builder
`0xc91ba0` at `0xc91d5e..0xc91d77` follows the same `[group+0x30] -> vtable
slot 0x8c` shape.

The 2D surface wrappers are separate and succeed: `0x00ce8860` creates a
`NiDX9TextureBufferData` around an `IDirect3DSurface9`, and `0x00cecb70`/
`0x00cecb99` release the temporary surface after `GetSurfaceLevel(0)`/
`GetDesc`. The engine does expect a surface there, and the port returns one
(`S_OK`). The fault is not in that wrapper and is not caused by a wrong COM
type. The cube path would use `IDirect3DCubeTexture9::GetCubeMapSurface`
(slot 18, `0x48`) at `0xce8a1e`; it is not reached.

## 5. Depth-stencil

No `CreateDepthStencilSurface`/`SetDepthStencilSurface` occurs before the
fault. The group builder only reads the renderer's current target size through
`0xcd2230` (`[renderer+0x628]`) and the renderer's `[+0x84]/[+0x88]` methods;
it does not create depth.

The cube group's six 512x512 faces would need a depth attachment (shared or
per face) once the engine binds them. That depth setup is downstream of the
colour targets (device slot 29, `CreateDepthStencilSurface`) and is not
reached. In the port, `d9_set_render_target` accepts only `Target` handles
(`source/d3d9_core.c:587`); a `GetCubeMapSurface` view is a CPU-backed
`SURFACE`, not a `Target`, so cube-face render-target binding is not implemented
even after `CreateCubeTexture`. A probe should log slot 29 and the depth handle;
the expected tuple is 512x512 D24S8 (or D16) matching the colour face.

## 6. Is this cube needed for the first visible menu?

No. The group at `0xc92676` is the **water-reflection** set:

- width/height come from `[0x12cfa2c]`/`[0x12cfa30]`, written at
  `0xc6e0ca`/`0xc6e0d0` from the settings defaults `0x12cf6e8`/`0x12cf6f4`;
- those settings are `iWaterReflectWidth` and `iWaterReflectHeight`
  (`0x114e39c`/`0x114e3c0`), default **512**;
- the group format is `0x71` (`A16B16G16R16F`), flags `0x70`
  (`0x50 | 0x20`, cube bit `0x10`);
- the same size/format pair appears in the kind-11 descriptor at `0xc90f34`
  (width/height from the same globals, flags `0x50`, format `0x71`), which is
  the water/reflection descriptor used by `0xc92330` callers.

The first visible menu is GFx UI plus an optional 3D background; it uses the
2D/back-buffer path, not a water-reflection cube. The engine still builds the
group at renderer init (`0xc747ca -> 0xc92410 -> 0xc92676`) unconditionally, so
a config-only `bUseWaterReflections=0` (`0x010c73cc`) does not skip the
creation unless the group code observes it. The honest menu path is:

1. make the cube creation return a checked NULL (as `0xfb5c20` already does on
   its early exits) and let the renderer's null-target diagnostic
   (`0x01153a60`) handle the group; or
2. skip the kind-11 water-reflection group when water is disabled, before
   `0xc92410`; do not fabricate a cube.

Relevant settings/strings: `bUseWaterReflections` (`0x010c73cc`),
`bUseCubeMapReflections` (`0x010c7408`), `fCubeMapRefreshRate` (`0x010c74e8`),
`BSCubeMapCamera` (`0x01156868`), `WaterReflectionData` RTTI (`0x0126f1b4`),
`TESWaterReflections` (`0x0126f328`). The `0xc91800` callers at `0x5a4e58` and
`0x87ef57` create a separate 256x256 kind-32 source texture for sky/sun, not
this init cube.

## 7. Measurement-only probe (one run)

The harness already has a four-byte guest watchpoint
(`source/guest.c`, `N3DS_WATCH_ADDR`/`N3DS_WATCH_VALUE`). It polls after every
instruction via `guest_watch_set_pc` and prints the writing PC, old/new value
and the heap region/slab.

Run the same census with:

```text
N3DS_WATCH_ADDR=0x545b4c90      # cube map 0x545b4c50 + 0x40 (face 0)
N3DS_WATCH_VALUE=0x4
```

The first `[watch]` record with `new=00000004` names the producer. If it is
`0xfb5cc1`/`0xfb5cd1`, the face loop wrote a non-pointer (allocation issue);
any other PC is the real corruptor. The address is stable while the allocation
sequence and budget are unchanged; if it moves, first run the fault dump below.

Fault-time dump (proposal, env `N3DS_RTFAULT_PROBE=1`, in `n3ds_guest_ptr` when
`g_last_call==0x00fb5bd0`, before returning NULL):

- registers: `eax,ebx,ecx,edx,esi,edi,ebp,esp`;
- `[ecx]`, `[ecx+0x08]`, `[ecx+0x0c]`, `[ecx+0x10]`, `[ecx+0x14]`,
  `[ecx+0x24]`, `[ecx+0x30]`, `[ecx+0x31]`, `[ecx+0x34]`, `[ecx+0x3c]`,
  `[ecx+0x40..0x54]`, `[[ecx+0x40]]`, `[[ecx+0x40]+8]`;
- a hex dump of `ecx..ecx+0x60`;
- the caller frame `[esp..esp+0x40]`;
- if `[ecx+0x40]` is mapped, its vtable and `+8/+0xc/+0x10/+0x30`.

Optional function hooks (env-gated breakpoints): `0x00fb5c20` entry (size,
renderer), `0x00fb5cc1`/`0x00fb5cd1` (face index, new buffer), `0x00ced404`
(cube map, face 0), `0x00c91d77` (texture), `0x00c8ff40` (group `+0x30`),
`0x00ce8860` (surface/buffer data), device slots 25/18/29.

## 8. Minimal honest PICA200 policy

PICA200 does support cube-map textures, and the devkitPro headers expose the
whole path: `C3D_TexInitCube(tex, cube, w, h, fmt)` with `GPU_TEX_CUBE_MAP`,
`C3D_TexCubeGetImagePtr`, `C3D_TexLoadImage(tex, data, face, level)` and
`C3D_RenderTargetCreateFromTex(tex, face, level, depthFmt)`. The bridge now
owns real cube source textures: `texture_create_cube`, `texture_upload_cube`
and `texture_delete_cube` allocate six `C3D_Tex` faces with
`C3D_TexInitCube` and upload each face/mip with `C3D_TexLoadImage`
(A8R8G8B8 -> `GPU_RGBA8`, X8R8G8B8 -> `GPU_RGB8`; DXT/ETC, invalid edges and
VRAM failures are named refusals). What is still missing is the render side:
`GetCubeMapSurface` is not bindable as a target and PICA `draw_cube` is NULL,
so a cube-bound draw is still `D3DERR_NOTAVAILABLE`. PICA render targets are
integer formats only, so `A16B16G16R16F` must be downgraded under the LDR
render-target policy and never reported as a native cube RT format.

- **Measurement only:** a host/null six-face target set can be created to reach
  the next layer. It must be env-gated (the existing HDR/budget probes or a new
  `TESV_D3D9_CUBE_RT_PROBE=1`), never a shipping default.
- **Shipping today:** `CreateCubeTexture` with `D3DUSAGE_RENDERTARGET`, and
  `GetCubeMapSurface` used as a render target, are named refusals until the cube
  backend and face targets land. A CPU-only cube must not be sampled or passed
  off as a device cube. Disable water reflections / the kind-11 group for the
  menu; keep the 2D targets.
- **VRAM:** a 512 cube at `A16B16G16R16F` is `6 * 512^2 * 8 = 12 MiB`; even an
  RGBA8 cube is `6 MiB`, equal to the whole original-3DS VRAM, before colour
  targets, depth or textures. The 2D set already needs 24.5 MiB in the 64 MiB
  measurement budget. Do not add a cube on device.
- **If water reflection is required later:** six 256x256 RGBA8 faces
  (1.5 MiB) with a shader-side cube-to-2D mapping, or a prefiltered 2D lookup;
  no A16F cube, no `GetWidth` on a face table that is not fully populated.

## 9. Next layer

1. Run the watchpoint above; if `0xfb5cc1` wrote 4, inspect
   `0xa48d60`/`0xfb69d0` allocation for this size; otherwise fix the named
   writer.
2. In parallel, skip the kind-11 water-reflection group in the menu (checked
   NULL or config path) and re-run the census to reach the first
   depth/`SetRenderTarget`/draw. The 2D RT set is already real.
3. Log device slot 29 (`CreateDepthStencilSurface`) and the depth handle; the
   cube faces need a shared/per-face depth before any face render.

## 10. Config-only test: `bUseWaterReflections=0` (2026-11-26)

Tested on a fresh host slice built from `main` at `bc23f42`
(`E:\Skyrim_N3DS_build\analysis\growth_probe\gen01`), with the same census
environment (`TESV_MEMPROBE=1`, `TESV_D3D9_CAPS_PROBE=1`,
`TESV_D3D9_TRACE=1`, `TESV_D3D9_SM3_GATE_PROBE=1`,
`TESV_D3D9_CAPTURE_DRAWS=8`, `TESV_D3D9_HDR_LDR_FALLBACK=1`,
`TESV_D3D9_BUDGET_MB=64`, `N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc`) and
args `F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160`.

`F:\Skyrim\Documents\My Games\Skyrim\SkyrimPrefs.ini` was backed up, then a
`[Water]` section was added:

```ini
[Water]
bUseWaterReflections=0
bUseCubeMapReflections=0
bReflectSky=0
bReflectLand=0
bReflectLODLand=0
bReflectLODObjects=0
bReflectLODTrees=0
```

Result (85.7 s, exit 1):

- **The fault is unchanged**: `[guest] unmapped addr=0000000c ... last_call=00fb5bd0`,
  same stack `545b4c50 545b4c50 54114d58 2 2 2`, same last D3D call
  `Release(0x500022c0)` refcount 3.
- `CreateCubeTexture` = 0, `GetCubeMapSurface` = 0, `SetRenderTarget` = 0,
  `CreateDepthStencilSurface` = 0, `SetDepthStencilSurface` = 0,
  `CreateVertexShader` = 0, `CreatePixelShader` = 0, `SetTexture` = 0,
  `DrawPrimitive`/`DrawIndexedPrimitive` = 0. There is no
  `IDirect3DDevice9::Present`; the only `Present` string is the capture-enable
  line.
- The nine 2D RTs are still created; the 0x71 `0x500022c0` is still the last
  one.

The INI was restored to its original bytes (SHA-256
`C0768CCC224FB3A8E7F8433BF28EA5B368C12A86EA293D99D0B6AB6DD4D938CD`)
and the `[Water]` section is gone.

### Why the config did not help

The group at `0xc92676` is built unconditionally in `0xc92410`. The
`bUseWaterReflections` value is a `:Water` setting (table entry `0x126ef84`,
default **0**); `bUseCubeMapReflections` is at `0x126ef9c`, default **1**.
Setting both off still built the group. The renderer-init path reads the
water-reflect **dimensions**
(`0x12cfa2c`/`0x12cfa30`, set from `iWaterReflectWidth/Height` at
`0xc6e0ca`/`0xc6e0d0`) but not the reflection boolean. The boolean is consumed
by the water rendering pass later, after the group has been built. A
config-only change therefore cannot skip the cube creation/fault.

### Minimal honest skip (no fake cube)

The fault is before `CreateCubeTexture`, so a port-side D3D refusal cannot
intercept it. The minimal guest-side point is `0xc91540`'s cube branch:
`0xc9163e test dl,0x10` -> `0xc91650 call 0xfb5c20`. If water reflections are
off, return a **checked NULL** there (skip the `0xfb5c20` call and fall through
to the `0xc916d1` epilogue) so `0xc91ba0` stores 0 and the renderer's
null-target diagnostic (`0x01153a60`) handles the group. The alternative is to
skip the `0xc92676` call and store 0 in its group slot; that requires the
slot/count bookkeeping to stay consistent. Do not fabricate a cube face.

A config-only path would require the engine to read `bUseWaterReflections`
before `0xc92676`; it does not today.

## 11. Cube-skip probe census (2026-11-26)

`TESV_D3D9_CUBE_SKIP_PROBE=1` implements the checked-NULL skip as a
measurement-only host-slice probe (`source/tesv_cube_probe.c` and
`source/tesv_cube_probe_slice.c`; test `tests/tesv_cube_probe_smoke.c`). It
byte-verifies `0xc9163e` (`F6 C2 10`), `0xc9164d` (`52 51 50`), `0xc91650`
(`E8 CB 45 32 00`, target `0xfb5c20`) and `0xc916d1` (`83 7C 24 38 00`), then
writes the ten-byte patch:

```text
83 C4 0C 33 F6 E9 77 00 00 00
add esp,0xc; xor esi,esi; jmp 0x00c916d1
```

The AOT/IR shadow `tesv_cube_probe_fb5c20()` returns `eax=0` for the same call
site (return address `0xC91655`) so the predecoded `f_C91540`/`f_C915AC`/
`f_C91630` path takes the existing `test eax,eax` -> `0xC916D1` branch. The
measurement build renames the three generated `f_FB5C20(c)` call sites in
`gen01/tesv_16.c` to the shadow and restores the generated file after the
build. The probe prints `MEASUREMENT ONLY ... not shippable`; caps, SM3 words
and T&L are unchanged.

The probe was extended with byte-verified measurement-only pieces: NOPs at
`0xc92698` (`89 44 B4 40`, store the cube group) and `0xc926a6` (`46`, inc
`esi`); a group-sentinel guest-memory shadow (`tesv_cube_probe_ptr`, group
vtables `0x011b019c`/`0x011539d8`); a runtime dispatch shadow for `0xfb6090`;
and vtable slots 33/34 (`0x011b0220`/`0x011b0224`) pointed at a synthetic host
stub `0x30000000` (the vtable part is opt-in via `TESV_CUBE_PROBE_VTABLE=1`).
Host test `tesv_cube_probe` PASS (exact 15 code bytes plus
the two vtable entries and the five-byte stub site, repeat refusal, wrong
bytes, group shadow, unmapped memory). Registered in
`tools/run_host_tests.ps1`.

Two fresh-slice censuses from `main` `bc6ae69` (`growth_probe/gen01`) with the
SM3, HDR-fallback and 64 MiB budget probes:

1. **Dispatch-guard variant (default, no vtable stub): 210.657 s, exit 1.** Nine 2D
   RTs created; `0xfb5bd0` gone; first blocker still
   `last_call=0x00fb6090`, address `0xb` (`GetBufferData`, vtable slot 34,
   buffer slot `-1`/`3`). The store/inc slot skip and the sentinel shadow did
   not remove it: the getter is reached through the IR container, not the
   skipped cube group's outer slot.
2. **Vtable-stub variant (`TESV_CUBE_PROBE_VTABLE=1`): 231.052 s, exit 1.** Fault moves to
   `last_call=0x30000000`, address `0`; `CreateTexture` is only 3, so the
   renderer aborts before the nine-RT set. The stub returns NULL for legitimate
   early `GetBufferData` calls too, so it is too broad.

Next minimal step: zero the skipped group's buffer array (or skip the
`0xc92676` group build and its slot bookkeeping) so `GetBufferData` at
`0xfb6090` sees NULL and returns 0; then re-run for depth/shaders/draw. Do not
fabricate a cube.

## 12. Where the cube format descriptor comes from (2026-11-26, static)

The fault register probe moved the fault to  x00ced444: CreateSurf calls
 x00ce9a40 and dereferences [eax+0x0c] without a NULL check. Static reading
of the lifted slice explains the NULL.

- The renderer singleton is  x001B418B0. Its constructor _CD58F0 creates
  four format tables by calling _CD4F10(this, table, flag, type):
  +0x4A4 = (0, 3) texture, +0x4FC = (1, 3) render-target texture,
  +0x554 = (0, 5) cube, +0x5AC = (1, 5) render-target cube.
  D3DRTYPE_TEXTURE is 3 and D3DRTYPE_CUBETEXTURE is 5, so +0x5AC is the
  table CreateSurf needs for the water-reflection cube target.
- _CED3E0 passes 
enderer+0x5AC to _CE9A40. That helper looks up the
  requested format/type pair in the table and returns a format-descriptor
  pointer, or NULL when the table has no entry. _CED3E0 then reads
  [descriptor+0x0C] (the D3DFORMAT) before checking the pointer.
- The constructor fills those tables from measured device capability: the
  CheckDeviceFormat calls captured in the census come from this path. The
  census at 4204b1a shows the retail query for the cube descriptor uses
  usage=0: CheckDeviceFormat(0, 1, 0x16, usage=0, type=CUBETEXTURE,
  format=0x71) from  x00cd5542 returns D3DERR_NOTAVAILABLE, so +0x5AC
  stays empty and CreateCubeTexture is never reached.

Implication: accepting only D3DUSAGE_RENDERTARGET is not enough for the
descriptor query. A measurement-only run has to accept the usage=0
CUBETEXTURE descriptor check as well (and, if the engine still probes by
creating a trial resource, let that probe succeed) before the table entry can
exist. This remains measurement-only; the shipping answer stays the named
refusal until PICA cube render targets exist.
Both: `CreateCubeTexture`, `GetCubeMapSurface`, `SetRenderTarget`,
`CreateDepthStencilSurface`, `SetDepthStencilSurface`, `CreateVertexShader`,
`CreatePixelShader`, `SetTexture`, both draw entry points and `Present` are 0.
No depth/shader/texture-bind/draw was reached; no frame is claimed.

Next minimal step: make the getter stub **conditional** — call the real
container `0xfb6070`/`0xfb6090` for a valid `this`/slot and return NULL only
for the `-1`/small-integer sentinel — or give the group a valid dummy
`Ni2DBuffer` (width/height 0). The guest-byte-only paths cannot fix this
because the getter is executed by the IR container. Do not fabricate a cube.

## 13. Cube-face render targets (2026-11-26)

The first half of the shippable cube render-target layer is implemented.

### Backend contract

`D9Backend` gains two append-only callbacks after `texture_delete_cube`:

```c
int  (*target_create_cube_face)(void *ctx, void *native_cube, unsigned face,
                                unsigned level, unsigned depth_flags,
                                void **native_target);
void (*target_delete_cube_face)(void *ctx, void *native_target);
```

`face` is 0..5 in +X,-X,+Y,-Y,+Z,-Z order and `level` is the cube mip level.
`depth_flags` is `D9_CUBE_FACE_DEPTH24_STENCIL8` (1) for the D3D depth policy.
The callback returns 0 and a target view, `-2` for an unsupported
face/level/format, `-3` for VRAM/allocation failure, or another negative value.
The view owns only its render-target/depth attachment; the cube texture owns
the colour storage. Missing callbacks are a named refusal and face 0 is never
substituted.

PICA implements this with `C3D_RenderTargetCreateFromTex(&cube->tex,
(GPU_TEXFACE)face, level, GPU_RB_DEPTH24_STENCIL8)` and
`C3D_RenderTargetDelete`. The cube's `C3D_Tex` is never deleted by the face
view. The host null backend records the face/level/depth flags and keeps a
small CPU colour buffer so bind/clear recording works.

### Core and COM

- `CreateCubeTexture(..., D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT)` creates a
  real cube texture. Under `TESV_D3D9_HDR_LDR_FALLBACK=1`, formats `0x71`/`0x72`
  become A8R8G8B8 storage (the same measurement-only downgrade as the 2D RT
  path) while `GetLevelDesc` keeps the logical format. Without the fallback the
  request is a named `D3DERR_NOTAVAILABLE`; DXT/other formats, MANAGED pools and
  combined usage bits are also refused by name.
- `GetCubeMapSurface(face, level)` lazily creates and caches one target per
  face/level (`CubeTexture.face_target[6][11]`); repeated calls reuse the target
  and return a new surface view. The target handle belongs to the cube and is
  released by the cube, never by `Release` of the surface view. Lazy creation is
  refused by name inside `BeginScene`/`EndScene`; call it during resource setup.
- `SetRenderTarget(0, faceSurface)` binds the face target; `GetRenderTarget`
  returns the same surface. The viewport is the face edge. A depth-stencil
  surface of matching size can be bound; switching to a different-sized target
  without unbinding it is a named `D3DERR_NOTAVAILABLE`.
- Refusals: face/level out of range, a non-render-target cube view, a depth
  surface used as a render target, float/HDR without the fallback, unsupported
  formats/pools/usage, and a backend without face callbacks. The last one names
  the missing callback rather than substituting face 0.
- `Target` carries `is_cube_face`; device teardown and cube release destroy face
  views before the cube native. `MAX_TARGETS` is 32 so six faces times two
  levels fit alongside ordinary targets. The 2D target count test was updated
  to fill the new table.

### Host evidence

New `tests/cube_rt_smoke.c` (registered as `cube_rt`) creates a 2-level
`0x71` RT cube under the LDR fallback, checks all six faces at both levels,
target reuse, `Set/GetRenderTarget`, `GetViewport`/`SetViewport`, depth binding
and clear, surface/cube release ordering, face/level refusals, the no-callback
refusal, the non-RT cube refusal, float-off refusal, unsupported
format/pool/usage refusals, and unchanged caps/SM3/T&L.

- Targeted `cube_rt,pica_cube_texture,cube_volume_residency,cube_sampling,com,
  render_texture,device_extra_methods`: **17 passed, 0 failed**.
- Full suite: **110 passed, 0 failed** (the full runner does not register
  `tesv_slice` without `-TesvSliceGen`).
- ARM `-Mode com -Isolated cube_rt`: `.elf` 5,986,328 B SHA256
  `d2dc52ad7ac526190c465103f67c40fca1ddd58bc4ca088a9dd50ff04d204c68`;
  `.3dsx` 881,952 B SHA256
  `f8c612c0a339e5b0dd4a74f5eae7e5b1e4fe3c4cbbf05a9b0bfa2f9815dfb1ba`;
  `.text` 864,020 B.

### Remaining

- Cube sampling/draw: an RT cube is not yet marked sample-ready and
  `draw_cube` is still NULL on the PICA backend. `SetTexture` with an RT cube
  therefore remains a named refusal rather than a silent substitute.
- The full TESV engine path/census still has to reach `CreateCubeTexture` and
  the cube pass; no engine census was run for this layer.
- No Azahar or hardware run was performed. No frame or pixel-equivalence claim
  is made.
