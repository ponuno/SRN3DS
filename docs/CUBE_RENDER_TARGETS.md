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

PICA200 has no cube render-target format and no cube-map sampling in this
bridge (a draw with a cube bound is `D3DERR_NOTAVAILABLE`).

- **Measurement only:** a host/null six-face target set can be created to reach
  the next layer. It must be env-gated (the existing HDR/budget probes or a new
  `TESV_D3D9_CUBE_RT_PROBE=1`), never a shipping default.
- **Shipping:** `CreateCubeTexture` with `D3DUSAGE_RENDERTARGET`, and
  `GetCubeMapSurface` used as a render target, are named refusals. Disable water
  reflections / the kind-11 group for the menu; keep the 2D targets.
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
