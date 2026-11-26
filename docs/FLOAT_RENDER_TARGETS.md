# Float render targets in TESV's D3D9 renderer (analysis, 2026-11-26)

Status: **analysis only**. This note records what the engine asks for, who asks
for it, and what the PICA200 can honestly do about it. It changes no production
behaviour. The `TESV_D3D9_HDR_LDR_FALLBACK=1` switch in `source/d3d9_com.c` is a
measurement probe, not a policy.

Evidence is read-only static disassembly of `E:\skyr\TESV.exe`
(SHA-256 `D97EB0CC0CEEF0923D2234D42B382C0D033853356528B271BC043542B1F5E605`,
17,462,064 bytes, image base `0x400000`) plus the recorded post-SM3 census in
`E:\Skyrim_N3DS_build\analysis\sm3_gate_probe_census_final2.err` and the BSA
inventory of `Skyrim - Meshes.bsa`. No new frame is claimed.

## 1. What the census actually requested

The nine `CreateTexture` calls after the SM3 gate, all from the same call site
`0x00ceca5e` inside `NiDX9RenderedTextureData::Create` (`0x00cec960`), are:

| Count | Size | Levels | Usage | Format | Pool | Meaning |
| ---: | --- | ---: | ---: | --- | ---: | --- |
| 5 | 1024x1024 | 1 | 1 (`RENDERTARGET`) | `0x72` | 0 (`DEFAULT`) | `D3DFMT_R32F` |
| 3 | 512x512 | 1 | 1 (`RENDERTARGET`) | `0x15` | 0 (`DEFAULT`) | `D3DFMT_A8R8G8B8` |
| 1 | 512x512 | 1 | 1 (`RENDERTARGET`) | `0x71` | 0 (`DEFAULT`) | `D3DFMT_A16B16G16R16F` |

All nine returned `D3DERR_NOTAVAILABLE` (`0x8876086a`). Immediately afterwards
the guest faulted at `0x00fb5bd0` reading guest address `0x0000000c`.

### Format-code correction

`D3DFMT_R32F` is **114 = 0x72**; `D3DFMT_A32B32G32R32F` is **116 = 0x74**
(`d3d9types.h` order `R16F 111, G16R16F 112, A16B16G16R16F 113, R32F 114,
G32R32F 115, A32B32G32R32F 116`). The engine's own `CheckDeviceFormat` loop at
`0x00ce54f4` enumerates `0x71, 0x72, 0x73, 0x74` as separate formats, so the
`0x72` request cannot be `A32B32G32R32F`. The five 1024 targets are **R32F**.
`docs/TESV_PROBE_ROUTE.md` labels `0x72` as `A32B32G32R32F`; that label should
be read as `R32F`.

The requested formats are not what the engine passes straight through. At
`0x00c91d19..0x00c91d40` (`NiRenderTargetGroup` build helper `0x00c91ba0`):

- requested `D3DFMT_A16B16G16R16` (`0x24`) -> pass `0x71`
  (`A16B16G16R16F`), `0x00c91d38`;
- requested `D3DFMT_R16F` (`0x6f`) -> pass `0x72` (`R32F`), `0x00c91d23`;
- everything else passes unchanged (`0x00c91d44`).

So the five 1024 requests started life as **R16F (0x6f)** and the lone 512
request as **A16B16G16R16 (0x24)**. The 0x72/0x71 values in the trace are the
substituted formats, chosen because the device-capability flags
`[0x01ba747c..0x01ba748b]` said the base formats were unavailable.

## 2. Owning system

These are not asset render targets. A raw-prefix scan of all **17,216** NIFs in
`Skyrim - Meshes.bsa` finds **zero** files declaring `NiRenderedTexture`,
`NiRenderedCubeMap` or `NiRenderTargetGroup`. The objects are engine-created
during renderer initialisation.

Chain:

1. `0x006968f0` copies the render target manager pointer into `[0x01ba76fc]`
   and calls the renderer init body `0x00c74710`.
2. `0x00c747ca` (inside `0x00c74710`) calls `0x00c92410` with that manager and
   the renderer singleton `[0x01ba7490]`. This is renderer initialisation,
   not a per-frame path.
3. `0x00c92410` builds the render-target sets:
   - `0x00c9242b` and the loop `0x00c92440..0x00c92469` request the 1024
     targets with format argument `0x6f` and dimensions from
     `[0x01babe50]`;
   - `0x00c92577`, `0x00c92612`, `0x00c926d8` request the 512 `A8R8G8B8`
     (`0x15`) targets;
   - `0x00c9263b` requests 512x512 with format argument `0x24`.
4. `0x00c6df76` is what puts 1024 in `[0x01babe50]`: it reads the settings
   default at `0x012cfb84` (1024) and stores it, then doubles it into
   `[0x01babe54]` (2048). That settings entry is
   `"iShadowMapResolution"` (`0x01153b0c`), immediately followed in the table
   by `"iShadowMaskQuarter"` (`0x01153b2c`) and
   `"bFloatPointRenderTarget"` (`0x01153b48`).

**Owner: the renderer/shadow-map setup in `NiDX9Renderer` initialisation.** The
five 1024 R32F targets are the shadow maps (the 4-iteration loop plus the
adjacent slot-0 group); the 512 set is a quarter-resolution shadow-mask /
deferred working set (the adjacent settings entry is `iShadowMaskQuarter`);
the 512 `A16B16G16R16F` is the same group's 16-bit target. They are not menu, logo, fog or water NIF resources.

## 3. Creation and failure branches

### 3.1 The 2D path (all nine measured calls)

```
0x00c92410  render-target set assembly
  -> 0x00c91ba0   NiRenderTargetGroup build/validate
       -> 0x00c91540   create one NiRenderedTexture; sets 0x012e1864=format
                       and 0x01bbfba0=1 ("use explicit format")
            -> 0x00c903b0   named wrapper -> 0x00fb6580 / 0x00fb6760
                 -> 0x00fb5e10  NiRenderedTexture ctor
                 -> 0x00cd4e00  register renderer data
                      -> 0x00cecaa0  NiDX9RenderedTextureData::Create
                           -> 0x00cec960  (0x00ceca5e CreateTexture)
```

Why the failure is clean here:

- `0x00cec960` logs
  `"NiDX9RenderedTextureData::Create> Failed CreateImageSurface - %s"`
  (`0x0115a390`) and returns 0 (`0x00cec960+0x11a`).
- `0x00cecaa0` releases the temporary data object and returns 0
  (`0x00cecaeb..0x00cecafd`).
- `0x00cd4e00` returns `false` when that create returns 0.
- `0x00fb6580` releases the `NiRenderedTexture` and returns 0
  (`0x00fb6678..0x00fb6688`); `0x00fb66a0` and the `0x00fb6760` wrapper behave
  the same way.
- `0x00c91540` returns 0, and `0x00c91ba0` stores 0 in its target slot.

The shadow-map loop does not dereference those null slots. The renderer's
`BeginUsingRenderTargetGroup` path has an explicit null-target diagnostic
(`0x01153a60`: `"...Failed - NULL pointer for render target..."`) rather than a
blind dereference. That is why the five 0x72 refusals are **tolerated**: they
are optional shadow-map slots, checked before use.

### 3.2 The cube-map path (the fault)

`0x00c91540` has a second mode: when its flag word has bit `0x10`
(`0x00c9163e: test dl,0x10`), it does not build a 2D texture. It calls
`0x00fb5c20` (`NiRenderedCubeMap::Create`), which:

- validates the size as a non-zero power of two (`0x00fb5c21..0x00fb5c43`);
- allocates a `NiRenderedCubeMap` via `0x00fb5bf0`, which zeroes the six face
  slots `+0x40..+0x54` and `+0x3c`;
- creates six `Ni2DBuffer` faces with `0x00fb69d0(w,h)` and stores them at
  `+0x40..+0x54`;
- registers the cube data through `0x00cd4e90` -> `0x00ced4b0` ->
  `0x00ced3e0`, which calls device slot `0x64` (`CreateCubeTexture`) and then
  `CreateCubeFace`; on failure it logs
  `"NiDX9RenderedCubeMapData::Create> NiDX9TextureBufferData::CreateCubeFace - %s"`
  (`0x0115a6f8`), destroys the data and returns 0;
- on registration failure `0x00fb5c20` releases the cube map and returns 0
  (`0x00fb5d01..0x00fb5d15`).

The fault is not a clean null from this path. `0x00fb5bd0` is
`NiRenderedCubeMap` vtable `0x011b0064` slot 35 (offset `0x8c`):

```
00fb5bd0: 8b 41 40   mov eax, [ecx+0x40]   ; first cube face (Ni2DBuffer*)
00fb5bd3: 8b 40 08   mov eax, [eax+8]      ; face width
00fb5bd6: c3         ret
```

Slot 36 (`0x8c+4`) is the matching height getter at `0x00fb5be0`. The caller is
the group builder at `0x00c91ba0`:

```
00c91d5e: 8b 07        mov eax,[edi]          ; this->buffer array
00c91d60: 8b 3c a8     mov edi,[eax+ebp*4]    ; buffer[i]
00c91d63: 8b 37        mov esi,[edi]
00c91d65: 8b 4e 30     mov ecx,[esi+0x30]     ; owning NiTexture
00c91d68: 83 c6 30     add esi,0x30
00c91d6b: 85 c9        test ecx,ecx
00c91d6d: 74 7d        je 0xc91dec
00c91d6f: 8b 11        mov edx,[ecx]          ; texture vtable
00c91d71: 8b 82 8c000000 mov eax,[edx+0x8c]   ; virtual GetWidth
00c91d77: ff d0        call eax               ; <- 0x00fb5bd0 for a cube map
```

There is **no check that the face table was populated** before this dispatch.
For a `NiRenderedCubeMap` receiver whose `+0x40` is not a live `Ni2DBuffer`,
the second load dereferences `[face+8]`.

The recorded fault address is `0x0000000c`, not `0x00000008`. A plain NULL at
`+0x40` would fault at `0x8`; `0xc` means either the first load had
`ecx = 0xffffffcc` (32-bit wrap) or the second load had `[ecx+0x40] = 4`.
Both mean the face entry is a stale/invalid small value, not a live buffer.
This is consistent with a cube map that was constructed (or released and
re-used) while its face registration had failed. The exact moment `+0x40`
became invalid is the one thing the static text cannot pin down; see section 5.

### 3.3 So why does 0x71 appear to be the fatal one?

The 0x71 request is the **last 2D creation before the cube-map group build**.
In the init sequence the 512 `A16B16G16R16F` group at `0x00c9263b` is followed
by the group at `0x00c92676`, whose flag `0x70` has bit `0x10` set and therefore
takes the cube-map path. The group builder for that cube-map group immediately
runs the width query above. The five 0x72 shadow-map slots are checked and
skipped; the 0x71 slot is the last successful-looking 2D step, and the very
next step is the unguarded cube-map width query. The fatal receiver is a
**cube map**, not the 0x71 texture itself. The format codes are how the init
sequence is ordered, not two different error handlers.

This also means the two "branches" are:

- **checked 2D branch** (`0x00cec960` -> `0x00cd4e00`, and the null-target
  diagnostic): 0x72 shadow maps survive a refusal;
- **unchecked cube-map width branch** (`0x00c91ba0` -> `0x00fb5bd0`): any
  invalid face table is fatal, regardless of which 2D target preceded it.

## 4. Do these targets feed the first visible menu draw?

**Not proven, and the available evidence says no.** The census never reached a
draw or `Present`: after the last `CreateTexture` refusal it faulted in the
cube-map width query. The objects are created in renderer init (`0x00c74710`
-> `0x00c92410`), not by the menu scene; no NIF in the archive declares them;
and the first menu/logo assets (`meshes\interface\logo\logo.nif`,
`meshes\interface\...`) carry only `BSFadeNode`, `BSLightingShaderProperty`,
`BSShaderTextureSet`, `NiTriShape`/`NiTriShapeData`.

To prove binding and reads, the exact trace is:

1. Instrument `CreateTexture` at caller `0x00ceca5e` to record the returned
   `IDirect3DTexture9*` handle per (size, format), and `CreateCubeTexture`
   (device slot `0x64`) per size.
2. Record every `SetRenderTarget(slot, handle)` (device slot 37), every
   `SetDepthStencilSurface`, every `SetTexture(stage, handle)` (slot 65) and
   every `DrawPrimitive`/`DrawIndexedPrimitive` (slots 81/82) for the first
   visible menu frame.
3. A target "feeds" the first menu draw only if its handle is the active
   render target (not the implicit back buffer) or is bound to a sampler used
   by the draw's pixel shader.
4. `TESV_D3D9_CAPTURE_DRAWS` already records the active target identity and
   sampler metadata (`docs/FIRST_DRAW_CAPTURE.md`); it is the right harness for
   this, once the route reaches a draw.

Until that trace exists, the honest statement is: these are renderer-init
shadow/HDR working buffers, they are **not** shown to be read by the first menu
draw, and the current route cannot reach a draw to test it.

## 5. Minimal honest PICA200 policy

PICA200 has no float render-target format. The only usable colour formats are
RGBA8/RGB8/RGBA4444/RGB565/RGBA5551 (and ETC1 for textures); depth is the
fixed-point depth attachment. So a float target cannot be "supported"; it can
only be disabled, resized, or replaced by an LDR target with a shader-side
reformulation.

### 5.1 Budget

| Target | Bytes/px | 1024x1024 | 512x512 |
| --- | ---: | ---: | ---: |
| `R32F` (0x72) | 4 | 4 MiB | 1 MiB |
| `A16B16G16R16F` (0x71) | 8 | 8 MiB | 2 MiB |
| `A8R8G8B8` (0x15) / `X8R8G8B8` (0x16) | 4 | 4 MiB | 1 MiB |
| `D16` depth | 2 | 2 MiB | 0.5 MiB |
| `D24S8` depth | 4 | 4 MiB | 1 MiB |
| RGBA8 + D24S8 | — | **8 MiB** | 2 MiB |
| RGBA8 + D16 | — | **6 MiB** | 1.5 MiB |

The original 3DS has **6 MiB** of VRAM total. A single 1024 RGBA8 colour target
plus a 1024 `D24S8` depth target (8 MiB) does not fit, before any texture or
command buffer. Even 1024 RGBA8 + D16 (6 MiB) consumes the whole budget. A
naive "map float to RGBA8 at the same size" is therefore not viable.

### 5.2 What may be configurable or off

- **Shadow maps (`0x72`, five 1024 R32F):** configurable/off. They are optional
  and already null-checked. Default: honour `bDrawShadows=0` /
  `bDeferredShadows=0`; otherwise use a **depth-only** or **packed 8-bit**
  substitute at 512x512 (0.5 MiB D16 or 0.25 MiB R8), never a 1024 RGBA8+D24S8.
  The substitution must be named and behind an explicit setting, and the
  shadow shader must be rewritten to read the substitute (packed-depth unpack
  or hardware depth compare), not just fed the old float sampler.
- **Quarter-res shadow-mask set (`0x15` 512 A8R8G8B8):** allowed as real PICA
  RGBA8 (1 MiB each). This is the honest LDR part.
- **`0x71` 512 `A16B16G16R16F`:** configurable/off. At 512 it is 2 MiB; an LDR
  substitution is only allowed if the consuming pass is proven LDR-safe
  (clamped/bloom threshold reformulated, no HDR accumulation read back).
  Default: refuse by name, do not silently downcast.
- **Cube maps:** no PICA cube texture. If a cube map is required, use six 2D
  faces (6 x size^2 x 4) or a prefiltered 2D lookup; otherwise disable the
  effect. Do not present a cube map object whose face table is not fully
  populated.

### 5.3 What must not be substituted

- Anything whose result is **read back** or used as **data** (shadow depth
  comparisons, sun-glare occlusion, HDR eye adaptation, bloom threshold
  accumulation) must not be silently replaced by an 8-bit colour target. The
  substituted value range and precision change the algorithm; that needs a
  shader/algorithm change and a named decision.
- Do not substitute a target that the engine will later query for width/height
  or bind as a cube face (the `0x00fb5bd0` path). If the substitute cannot be
  fully constructed, return a checked null and keep the caller on the
  null-target diagnostic path.
- Do not enable the existing `TESV_D3D9_HDR_LDR_FALLBACK=1` mapping as a
  shipping default. It maps `0x71/0x72` to `A8R8G8B8` at the same dimensions
  and gives the guest a target it believes is float; that is a measurement
  tool, not an honest PICA policy.

## 6. Open items (exact probes)

1. Watch `0x00fb5bf0`, `0x00fb5c20`, `0x00cd4e90`, `0x00ced3e0` and the width
   call at `0x00c91d77`; print `ecx`, `[ecx]`, `[ecx+0x40]`, the buffer at
   `[edi]` and the group index. This decides whether `+0x40` is stale from a
   released cube map or was never a face pointer.
2. With the cube-map path disabled, re-run the post-SM3 census and capture the
   first draw/Present: record target and sampler handles (section 4).
3. Measure the actual VRAM cost of the current LDR path (the 1024 `0x16`
   texture plus its depth attachment) before proposing any shadow-map size.
