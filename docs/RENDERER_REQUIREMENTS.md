# Renderer capability requirements (D3D9 → PICA200)

Status: **decision module integrated with the adapter and covered by the host
suite.** The module lives in:

- `source/d3d9_renderer_requirements.h`
- `source/d3d9_renderer_requirements.c`
- `tests/renderer_requirements_smoke.c`

The adapter implements the supported LDR decisions directly; this module is the
named contract/probe view used by tests and diagnostics. There is no pixel claim
in this document. A later pass added the single LDR render-target surface rule
(`format.render_target_ldr`); separate core/COM/PICA implementation and synthetic
pixel evidence are recorded in `docs/RENDER_TARGET_EVIDENCE.md`.

The purpose is narrower than "support Skyrim's renderer". It answers one
question honestly:

> Given the literal arguments of a real `CheckDeviceType`,
> `CheckDeviceFormat`, `CheckDepthStencilMatch` or `GetDeviceCaps` call, may the
> bridge answer `D3D_OK`, must it answer `D3DERR_NOTAVAILABLE`, or is there a
> named alternative that is *not* a D3D9 capability?

The matrix never turns a fallback into a fake success. Every `FALLBACK` and
`UNSUPPORTED` rule answers `NOTAVAILABLE`; the smoke test and
`d9_req_matrix_validate()` enforce that invariant.

---

## 1. Evidence baseline

The original planning baseline was a static decode. A later executed TESV host
route now confirms the adapter enumeration and all 18
`RENDERTARGET/SURFACE` probes at `0x00ce54f4`; see
`docs/RENDER_TARGET_EVIDENCE.md`. Static call sites remain marked **S** unless a
runtime trace observed them.

Legend: **M** = measured at runtime, **S** = static decode/disassembly,
**I** = inferred. The 2026-11-25 TESV host route adds **M** evidence for the
format loop and the subsequent caps rejection.

| Call site | Query | Arguments recorded | Evidence |
|---|---|---|---|
| `0xc6e0a7` | factory slot 10, `CheckDeviceFormat` | usage `0x00080000`, format `0x71` = `A16B16G16R16F` | S |
| `0xc6e26b`, `0xc6e293`, `0xc6e2c0`, `0xc6e328`, `0xc6e445` | renderer capability stages | described in `INIT_TRACE.md` as ATOC/RESZ four-character probes | S, exact operands not re-confirmed here |
| `0xc6de65` | factory slot 11, `CheckDeviceMultiSampleType` | conditional | S |
| `0xc6dbf0` ordering | adapter enumeration → window → second factory → device | first COM calls are adapter enumeration, not `CreateDevice` | S |
| `GetDeviceCaps` consumer | engine expects SM3 | docs/COMPATIBILITY.md: all 10,475 shipped PS chunks are SM3 | S, consumer behaviour not measured |

### 1.1 Correct the usage-bit label before trusting it

`docs/INIT_TRACE.md` line 746 labels usage `0x80000` as
`D3DUSAGE_QUERY_SRGBREAD`. The Windows SDK `d3d9types.h` does not agree:

| Constant | Value |
|---|---|
| `D3DUSAGE_QUERY_SRGBREAD` | `0x00010000` |
| `D3DUSAGE_QUERY_FILTER` | `0x00020000` |
| `D3DUSAGE_QUERY_SRGBWRITE` | `0x00040000` |
| `D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING` | `0x00080000` |
| `D3DUSAGE_QUERY_WRAPANDMIP` | `0x00200000` |

So the recorded A16 probe is a **post-pixel-shader-blending** query on a float
format, not an sRGB-read query. The module evaluates both bits separately rather
than propagating the label:

- `0x00080000` on an LDR format → `format.usage`, unsupported.
- `0x00010000` on an LDR texture → `format.srgb_read`, fallback contract.
- `A16B16G16R16F` with either bit → `format.a16b16g16r16f`, unsupported first.

If the real trace shows `0x00010000`, the sRGB-read path applies. If it shows
`0x00080000`, do not silently relabel it. This is exactly the kind of assumption
that must be settled by an actual call.

---

## 2. Decision semantics

| Decision | Meaning | D3D9 answer |
|---|---|---|
| `D9REQ_SUPPORTED` | The bridge really implements the requested capability. | `D3D_OK` (`0`) |
| `D9REQ_FALLBACK` | The D3D9 capability is absent, but a named non-D3D9 path exists. The caller may use it only under that contract. | `D3DERR_NOTAVAILABLE` (`0x8876086A`) |
| `D9REQ_UNSUPPORTED` | No path exists. Reject or rewrite the feature. | `D3DERR_NOTAVAILABLE` (`0x8876086A`) |
| `D9REQ_INVALID` | Malformed adapter/device/kind request. | `D3DERR_INVALIDCALL` (`0x8876086C`) |

The evaluator returns a bounded reason and contract string. `d3d9=OK` is only
possible for `SUPPORTED`. A fallback is never advertised through
`CheckDeviceFormat`, `CheckDeviceType`, `CheckDepthStencilMatch` or
`GetDeviceCaps`.

Requests also carry the adapter ordinal and device type. The bridge has one
adapter (ordinal 0) and `D3DDEVTYPE_HAL`; any other value is `INVALID` /
`INVALIDCALL`, matching the existing adapter checks. The D3D9 `BOOL` windowed
argument is treated by boolean value, so any non-zero value is TRUE.

---

## 3. Requirement matrix

This is the same data the C module evaluates. The table is a summary; the C
matrix is authoritative for exact matching order.

| Requirement id | Actual input shape | PICA200 reality / alternative | Decision | D3D9 answer |
|---|---|---|---|---|
| `device_type.windowed_ldr` | HAL, windowed, display `X8R8G8B8`, back buffer `X8R8G8B8`/`A8R8G8B8`/UNKNOWN | One 400×240 LDR display mode | supported | OK |
| `device_type.exclusive` | HAL, fullscreen | No display-mode ownership | unsupported | NOTAVAILABLE |
| `device_type.display_format` | display ≠ `X8R8G8B8` | No other display format | unsupported | NOTAVAILABLE |
| `device_type.backbuffer_format` | back buffer not X8/A8/UNKNOWN | No HDR/float back buffer | unsupported | NOTAVAILABLE |
| `format.adapter` | adapter format ≠ `X8R8G8B8` | Single adapter format | unsupported | NOTAVAILABLE |
| `format.texture_ldr` | `A8R8G8B8`/`X8R8G8B8`/DXT1/3/5, usage ⊆ `QUERY_FILTER|QUERY_WRAPANDMIP` | Real BC decode + tiled RGBA8 upload | supported | OK |
| `format.surface_ldr` | surface, usage 0, X8/A8 | Plain off-screen 32-bit surface | supported | OK |
| `format.render_target_ldr` | surface, usage exactly `D3DUSAGE_RENDERTARGET` (1), X8/A8, adapter X8, HAL | One LDR render-target surface | supported | OK |
| `format.a16b16g16r16f` | `A16B16G16R16F` (0x71) | No float texture/render target; LDR downgrade only | unsupported | NOTAVAILABLE |
| `format.srgb_read` | `QUERY_SRGBREAD` (0x10000) on LDR texture | Proposed CPU sRGB→linear conversion at import/upload (not implemented) | fallback | NOTAVAILABLE |
| `format.srgb_write` | `QUERY_SRGBWRITE` (0x40000) | No framebuffer sRGB encode | unsupported | NOTAVAILABLE |
| `format.atoc` | `D3DFMT_ATOC` (`'ATOC'`) | Binary PICA alpha test; no coverage mask | fallback | NOTAVAILABLE |
| `format.resz` | `D3DFMT_RESZ` (`'RESZ'`) | No depth resolve/texture path | unsupported | NOTAVAILABLE |
| `format.depth` | D16/D24S8/D24X8/D32/... | No D3D9 depth-stencil surfaces | unsupported | NOTAVAILABLE |
| `format.usage` | any other usage on a known LDR format, combined bits, or a texture render target | No texture render target, combined usage, MSAA, depth, auto-mip, dynamic or sRGB write | unsupported | NOTAVAILABLE |
| `format.resource` | resource type not SURFACE/TEXTURE | No other resource class | unsupported | NOTAVAILABLE |
| `format.unknown` | unrecognised numeric format | No PICA bridge format | unsupported | NOTAVAILABLE |
| `depth_stencil.unsupported` | `CheckDepthStencilMatch` | No depth-stencil surface object | unsupported | NOTAVAILABLE |
| `caps.sm2_templates` | shader-model request 2.0 | Two exact SM2 templates | supported | OK |
| `caps.sm3_family_only` | shader-model request 3.x | Specialized TEV/menu-light replacement only; not advertised | fallback | NOTAVAILABLE |
| `caps.shader_other` | SM1 or >SM3 | No path | unsupported | NOTAVAILABLE |
| `caps.mrt_supported` | render targets ≤ 1 | One active target: implicit or one explicit LDR surface | supported | OK |
| `caps.mrt_unsupported` | render targets ≥ 2 | No MRT | unsupported | NOTAVAILABLE |
| `caps.stages_supported` | texture stages ≤ 1 | One stage | supported | OK |
| `caps.stages_unsupported` | texture stages ≥ 2 | No multi-stage sampler | unsupported | NOTAVAILABLE |
| `caps.texture_size_supported` | dimension ≤ 1024 | Power-of-two up to 1024² | supported | OK |
| `caps.texture_size_unsupported` | dimension > 1024 | No larger texture | unsupported | NOTAVAILABLE |
| `caps.vs_constants_supported` | constants ≤ 4 | c0..c3 | supported | OK |
| `caps.vs_constants_unsupported` | constants > 4 | No more constants | unsupported | NOTAVAILABLE |
| `caps.depth_test_supported` | depth test 0/1 | PICA z-test/z-write on attached depth | supported | OK |
| `caps.depth_test_unsupported` | depth test > 1 | Boolean request only | unsupported | NOTAVAILABLE |
| `caps.sm3_slots_supported` | SM3 slots = 0 | Caps honestly report 0 | supported | OK |
| `caps.sm3_slots_unsupported` | SM3 slots ≥ 1 | No SM3 slots | unsupported | NOTAVAILABLE |
| `caps.unknown` | unrecognised caps probe | Not part of the contract | unsupported | NOTAVAILABLE |

`ETC1/ETC1A4` is a real PICA upload path used by the texture container, but it
is not a D3D9 `CheckDeviceFormat` format; it appears in the contract text only
as an internal alternative.

`GetDeviceCaps` has no capability argument. The `caps.*` requests are therefore
**requirements** ("needs VS/PS 3.0", "needs two render targets"), compared
against the caps this bridge actually reports: VS/PS 2.0, one render target, one
texture stage, 1024×1024, four VS constants, ZTEST, zero SM3 slots. Those values
are pinned by `tests/adapter_smoke.c`; if they ever change, the matrix rows must
be revalidated rather than assumed.

---

## 4. Feature details and exact differences

### 4.1 `A16B16G16R16F` / HDR

- The bridge has no `A16B16G16R16F` texture or render target. The PICA render
  formats used here are LDR RGBA8-family; `d3d9_core` and `d3d9_citro3d` do not
  allocate or sample 16-bit float surfaces.
- The capability query must answer **`D3DERR_NOTAVAILABLE`**. The evaluator
  returns `UNSUPPORTED`, not `FALLBACK`, because an HDR guarantee cannot be
  satisfied by LDR data.
- The only honest alternative is a **pipeline rewrite**: `X8R8G8B8` back buffer,
  `A8R8G8B8`/DXT/ETC1 textures, TEV LDR arithmetic, no tone mapping, no values
  outside `[0,1]`, no float alpha/blending precision. That is a visual and
  numerical downgrade, not A16 support.
- If the game treats HDR as optional, the LDR path may be acceptable; if it
  hard-requires HDR, the renderer must fail or select its own LDR fallback. The
  module does not decide that policy; it only refuses to lie about the format.

### 4.2 `SRGBREAD` / color space

- `D3DUSAGE_QUERY_SRGBREAD` asks whether sampling can decode sRGB to linear in
  hardware. The bridge has no such per-sample decode; it must answer
  `D3DERR_NOTAVAILABLE`.
- The proposed fallback is **CPU conversion at texture import/upload**: decode
  sRGB to linear once, store LDR linear RGBA8, then sample normally. This is not
  implemented in growth23 (the current import path rejects sRGB inputs
  explicitly; see `docs/MENU_PREPROCESS.md`), and it is the only defensible
  location for the conversion: there is no TEV gamma stage and no sRGB
  framebuffer write.
- Exact differences: 8-bit linear storage loses precision relative to a hardware
  sRGB read; generated render targets cannot be converted per sample; blending
  and TEV math run in the stored space, not automatically in linear space;
  alpha is unchanged. If the game only uses sRGB textures as static art, the
  fallback is viable; if it relies on hardware decode during blending, it is not.
- `D3DUSAGE_QUERY_SRGBWRITE` is separately **unsupported**: output remains in
  display gamma space. Do not answer yes for write conversion.

### 4.3 `ATOC`

- `D3DFMT_ATOC` (four-character `'ATOC'`) and alpha-to-coverage need a
  multisample coverage mask. The bridge has no MSAA and no ATOC surface.
- The candidate PICA alternative is the existing **binary alpha test**
  (`C3D_AlphaTest` with a compare function and reference value; see
  `source/d3d9_citro3d.c`). Exact differences:
  - ATOC keeps a fraction of samples proportional to alpha; alpha test keeps or
    discards the whole fragment at a threshold.
  - ATOC softens cutout edges and interacts with the depth buffer per sample;
    alpha test produces hard, aliased edges and one depth write.
  - ATOC needs a resolve and a multisample render target; neither exists.
  - Alpha blending after a passing alpha test still works, but it is not
    equivalent to per-sample coverage.
- Therefore the evaluator reports `FALLBACK` with `d3d9=NOTAVAILABLE`. A caller
  may use binary alpha test only if hard cutouts are acceptable.

### 4.4 `RESZ`

- `RESZ` means a depth resolve path (typically a driver four-character format
  for resolving multisample depth for depth-texture reads). The bridge has no
  depth-stencil surface, no multisample depth, no depth texture and no resolve.
- The evaluator reports **`UNSUPPORTED`**, not a silent no-op. Do not emulate
  depth with a color texture and do not claim `RESZ` because a depth test works.
- Menu/UI geometry may never need it; that must be shown by the actual call
  trace, not assumed from the feature name.

### 4.5 Shader model 3

- `GetDeviceCaps` reports VS 2.0 / PS 2.0 (`0xFFFE0200` / `0xFFFF0200`),
  `MaxVertexShaderConst=4`, and zero SM3 instruction slots. It does not
  advertise SM3.
- A request for shader model 3.x is therefore **`FALLBACK`**, with
  `d3d9=NOTAVAILABLE`. The contract is deliberately narrow: only a **specific
  shader family** may be replaced by a pre-tested TEV or menu-light plan.
  - The generic ps_3_0 → TEV translator accepts a small documented subset of
    real chunks (19 of 9,621 in `docs/COMPATIBILITY.md`; `docs/PORT_LOG.md`
    records a later 21), rejecting unsupported opcodes by name.
  - A separate structural menu-light matcher covers 6 retail logo signatures
    under explicit material/vertex preconditions and is not auto-selected.
  - All 10,475 shipped PS chunks are SM3. **There is no universal SM3
    translator**, and the matrix does not claim one.
- A caller that gets this fallback must inspect the actual shader family and
  preconditions. If they do not match, the correct result is `UNSUPPORTED`, not
  an attempt at generic translation.

### 4.6 Depth-stencil: what native support would be required

The current answer is `UNSUPPORTED` for `CheckDepthStencilMatch` and for every
depth format. The PICA backend already creates a private
`GPU_RB_DEPTH24_STENCIL8` attachment on its render target
(`C3D_RenderTargetCreate(..., GPU_RB_DEPTH24_STENCIL8)` in
`source/d3d9_citro3d.c`), but that is a backend-owned attachment, not a D3D9
depth-stencil surface. That is why `caps.depth_test_supported` and
`depth_stencil.unsupported` are both true: z-testing works, the D3D9 surface API
does not. This is consistent with the current COM surface:

- `CreateDepthStencilSurface`, `SetDepthStencilSurface` and
  `GetDepthStencilSurface` return failure with a named reason.
- `CheckDepthStencilMatch` always returns `D3DERR_NOTAVAILABLE`.
- `Clear` rejects depth/stencil flags because no surface is attached.
- `CreateDevice` rejects automatic depth-stencil surfaces.

To make a truthful `SUPPORTED` later, all of this is needed, not just a depth
test:

1. **Native attachment**: a citro3d/PICA render target with a depth buffer
   (`GPU_DEPTHBUF`/depth attachment, 24-bit depth and stencil as available) and
   correct memory accounting.
2. **D3D9 object model**: a real depth-stencil surface object with format,
   dimensions, multisample type/quality, lock rules and lifetime; creation,
   set/get, and reset behaviour.
3. **Format queries**: `CheckDeviceFormat` with `D3DUSAGE_DEPTHSTENCIL` and
   `CheckDepthStencilMatch` must reflect the same native formats.
4. **Clear**: `D3DCLEAR_ZBUFFER`/`D3DCLEAR_STENCIL` must map to the native depth
   and stencil clear values.
5. **State mapping**: D3D9 depth func/write and stencil func/ops must map to PICA
   state. The existing reverse-depth mapping for draws is necessary but not
   sufficient; it currently operates without a D3D9 depth surface contract.
6. **Depth reads/resolve**: depth-texture sampling and `RESZ` need a separate
   path; a depth attachment alone does not provide it. Without that, keep
   `RESZ` explicitly unsupported.
7. **MSAA**: ATOC/RESZ/depth resolve require multisampling; the bridge currently
   supports only `D3DMULTISAMPLE_NONE`.

The matrix intentionally separates `caps.depth_test_supported` (z-test on the
attached buffer) from `depth_stencil.unsupported` (D3D9 surface API). Do not use
the first to answer the second.

### 4.7 Single LDR render-target surface

- The contract accepts exactly one render-target shape. All of the following
  must hold at once:
  - `CheckDeviceFormat` (device type HAL, adapter `X8R8G8B8`);
  - `Usage` is exactly `D3DUSAGE_RENDERTARGET` (`1`), with no other bit;
  - `RType` is exactly `D3DRTYPE_SURFACE` (`1`);
  - `Format` is `A8R8G8B8` or `X8R8G8B8`.
- That request reports `claim=supported`, `d3d9=OK` and
  `id=format.render_target_ldr`. It is one LDR surface that can be a render
  target.
- Equality is intentional. Every other shape keeps its existing rejection with
  `D3DERR_NOTAVAILABLE`:
  - a usage-0 LDR surface is the separate plain-surface rule, not this one;
  - any combined usage (`RENDERTARGET|DEPTHSTENCIL`,
    `RENDERTARGET|QUERY_FILTER`, ...) is `format.usage`;
  - `D3DRTYPE_TEXTURE` with `RENDERTARGET` remains `format.usage`; textures
    cannot be render targets;
  - a non-LDR format keeps its format-specific rejection (`format.a16b16g16r16f`,
    `format.depth`, ...);
  - another resource type is `format.resource`;
  - a non-`X8R8G8B8` adapter format is `format.adapter`.
- **No MSAA claim.** `CheckDeviceMultiSampleType` has no request kind in this
  matrix, so no multisample, resolve or quality-level capability is added by
  this rule. `D3DMULTISAMPLE_NONE` remains the only implemented mode.
- The capability rule is backed by core/COM/PICA wiring and a separate synthetic
  Azahar switching pixel proof. It still does not assert MSAA, texture render
  targets, depth attachment, resolve, arbitrary offscreen orientation or a
  Skyrim frame.
- Single target only: `caps.mrt_supported` still means at most one simultaneous
  render target, and `caps.mrt_unsupported` keeps multiple targets rejected.
  This rule does not add MRT.

---

## 5. Bounded probe/log format

### 5.1 API

```c
D9ReqRequest q = d9_req_check_device_format(
    D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_QUERY_SRGBREAD,
    D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
D9ReqVerdict v;
char line[D9REQ_LOG_MAX];

d9_req_evaluate(&q, &v);
d9_req_format_probe(&q, &v, line, sizeof line);
/* later, in the real renderer trace: trace_detail("%s", line); */
```

`d9_req_format_probe` returns the `snprintf`-style required length excluding the
NUL. It always NUL-terminates when `cap > 0`, never writes outside `cap`, and
accepts `buf == NULL` only when `cap == 0`. There is no allocation and no hidden
state. `D9REQ_LOG_MAX` is 640; a longer reason/contract is truncated at the
buffer boundary, and the return value still reports the full length.

### 5.2 Field order

`req=... | claim=... | d3d9=... | id=... | reason=... | contract=...`

`contract` is omitted when empty. Numeric formats/usage are always hex; known
names are appended for readability.

### 5.3 Real examples from the module

```text
req=check_device_format adapter=X8R8G8B8 usage=0x00080000(QUERY_POSTPIXELSHADER_BLENDING) resource=texture format=A16B16G16R16F | claim=unsupported | d3d9=NOTAVAILABLE | id=format.a16b16g16r16f | reason=A16B16G16R16F is not a PICA200 texture or render-target format in this bridge | contract=LDR X8R8G8B8/A8R8G8B8 or DXT/ETC1 only; this is a downgrade, not A16 support
req=check_device_format adapter=X8R8G8B8 usage=0x00010000(QUERY_SRGBREAD) resource=texture format=A8R8G8B8 | claim=fallback | d3d9=NOTAVAILABLE | id=format.srgb_read | reason=no per-sample sRGB decode for QUERY_SRGBREAD | contract=proposed CPU sRGB-to-linear conversion at texture import/upload; sampling sees LDR data
req=check_device_format adapter=X8R8G8B8 usage=0x00000000(none) resource=texture format=ATOC | claim=fallback | d3d9=NOTAVAILABLE | id=format.atoc | reason=D3DFMT_ATOC surface/coverage is not implemented | contract=binary PICA alpha test at a threshold; no multisample coverage or per-sample resolve
req=check_device_format adapter=X8R8G8B8 usage=0x00000000(none) resource=texture format=RESZ | claim=unsupported | d3d9=NOTAVAILABLE | id=format.resz | reason=D3DFMT_RESZ depth resolve is not implemented | contract=none; no depth texture, multisample depth or resolve path exists
req=get_device_caps cap=shader_model value=0x00000300 | claim=fallback | d3d9=NOTAVAILABLE | id=caps.sm3_family_only | reason=GetDeviceCaps reports VS/PS 2.0; SM3 is not advertised | contract=specialized shader-family replacement only (TEV/menu-light); no universal SM3 translator
req=check_device_format adapter=X8R8G8B8 usage=0x00000000(none) resource=texture format=A8R8G8B8 | claim=supported | d3d9=OK | id=format.texture_ldr | reason=LDR RGBA8/BC texture with only QUERY_FILTER/QUERY_WRAPANDMIP
req=check_device_format adapter=X8R8G8B8 usage=0x00000001(RENDERTARGET) resource=surface format=X8R8G8B8 | claim=supported | d3d9=OK | id=format.render_target_ldr | reason=single LDR X8R8G8B8/A8R8G8B8 render-target surface with usage D3DUSAGE_RENDERTARGET
req=check_depth_stencil display=X8R8G8B8 backbuffer=X8R8G8B8 depth=D24S8 | claim=unsupported | d3d9=NOTAVAILABLE | id=depth_stencil.unsupported | reason=no D3D9 depth-stencil surfaces; CheckDepthStencilMatch cannot succeed | contract=none; requires a native PICA depth attachment and a D3D9 surface object
```

---

## 6. Tests

`tests/renderer_requirements_smoke.c` covers:

- static matrix validation (ids, kinds, non-empty reasons, fallback contracts,
  usage masks, no fake success);
- accepted LDR menu paths: windowed X8/A8/UNKNOWN back buffer, X8/A8 plain
  surfaces, A8/X8/DXT1/3/5 textures with no usage / `QUERY_FILTER` /
  `QUERY_WRAPANDMIP` / both, the single LDR render-target surface (X8/A8 with
  usage exactly `D3DUSAGE_RENDERTARGET`), and the supported `GetDeviceCaps`
  limits;
- the render-target probe line reports `claim=supported`, `d3d9=OK`,
  `id=format.render_target_ldr`, the literal `usage=0x00000001(RENDERTARGET)`
  and no contract text;
- explicit rejection of `A16B16G16R16F`, HDR back buffers, sRGB write, depth
  formats, `CheckDepthStencilMatch`, `RESZ`, texture render targets, combined
  render-target usage bits, render targets on non-LDR formats or unknown
  resource types, multiple render targets/stages, oversized textures, excess VS
  constants, SM3 slots and unknown caps;
- the recorded literal probe (`0x71` + `0x00080000`) and the SDK-correct
  distinction from `QUERY_SRGBREAD` (`0x00010000`);
- `ATOC` and SM3 returning fallback with `d3d9=NOTAVAILABLE`, never `OK`;
- bounded-log behavior at cap 0, cap 1, a short window with guard bytes, and
  deterministic repeat output;
- malformed requests (`INVALIDCALL`) and kind-specific argument isolation.

Manual compile/run (the Makefile and `tools/run_host_tests.ps1` were deliberately
not edited):

```powershell
& "C:\Program Files\LLVM\bin\clang.exe" -std=gnu11 -O1 -g -Isource `
  source/d3d9_renderer_requirements.c tests/renderer_requirements_smoke.c `
  -o build/renderer_requirements_smoke.exe
.\build\renderer_requirements_smoke.exe
```

Expected final line:

```text
renderer requirements: LDR menu paths and one LDR render-target surface accepted; HDR/SRGB/ATOC/RESZ/SM3/depth decisions truthful; bounded probe passed
```

This is a decision test. It does not create a device, upload a texture, draw, or
present.

---

## 7. What might be unnecessary for the main menu

The following are **hypotheses to check against actual calls**, not conclusions:

| Feature | Why it might not be needed for the main menu | What to measure |
|---|---|---|
| HDR / `A16B16G16R16F` | UI/menu images are usually LDR; a logo or menu background may not use an HDR target. | Whether the `0xc6e0a7` result gates renderer creation, or only selects an optional quality path. |
| `SRGBREAD` | Menu art may be authored in display space or pre-converted offline. | Whether the engine queries `0x00010000` (not just `0x80000`) and what it does with a no. |
| `ATOC` | Alpha-to-coverage is usually foliage/hair, not menu UI. | Whether the capability stage is reached before the menu and whether failure is fatal. |
| `RESZ` | Depth resolve is associated with depth textures/shadows, not flat UI. | Whether the `RESZ` probe happens during init or only for a scene quality mode. |
| Depth-stencil surfaces | Many UI renderers draw with depth test disabled and never create a D3D9 depth surface. | `CreateDepthStencilSurface`/`SetDepthStencilSurface`/`Clear` calls and whether the menu path issues them. |
| SM3 | A particular menu shader may match a specialized family even though the engine expects SM3 generally. | The exact bytecode family and whether a match/plan is available before refusing. |
| MSAA | The bridge already supports only `D3DMULTISAMPLE_NONE`; menu UI may not require it. | `CheckDeviceMultiSampleType` quality arguments and the code path taken on failure. |

Two cautions:

1. A renderer can probe a feature during init even when the menu does not use it.
   A `no` can disable a quality option *or* abort initialization depending on the
   caller. Only the actual call trace shows which.
2. Do not infer a requirement from a PC INI name, a shader name, or "the PC
   version does this". The matrix is driven by the literal D3D9 arguments.

The right order is: attach the bounded log line to the real trace, run the
renderer gate, then classify each probe as accepted / fallback / fatal. Only
after that can any feature be called unnecessary for the main menu.

---

## 8. Integration plan (not performed in growth23)

1. Keep `d9_adapter_check_*` authoritative until every new verdict is compared
   against the existing adapter result in `tests/adapter_smoke.c`.
2. In `d3d9_adapter.c`, build the request from the actual arguments, call
   `d9_req_evaluate`, attach `d9_req_format_probe` to the existing trace, and map
   `verdict.answer` to the adapter return code. Do not change the D3D9 answer for
   any fallback.
3. Do the same at the COM slot (`source/d3d9_com.c`) so one line per actual call
   is visible with the reason.
4. Only then wire texture creation policy (sRGB/ATOC/HDR) and consider any depth
   surface work. Add an exact `tests/adapter_smoke.c` assertion for every matrix
   rule before removing the temporary duplication.
5. Update `tools/run_host_tests.ps1` and the Makefile only when the module is
   actually part of a production target.

---

## 9. Non-claims

- This document makes no pixel/frame claim; separate evidence is in
  `docs/RENDER_TARGET_EVIDENCE.md`.
- No claim that LDR is an acceptable substitute for HDR in Skyrim.
- No claim that the capability rule alone proves MSAA, texture render targets,
  depth attachment, resolve or a Skyrim frame; it accepts only the exact
  `CheckDeviceFormat` shape listed in §4.7.
- No claim that binary alpha test is ATOC.
- No claim that a depth test is a depth-stencil surface, or that a depth
  attachment is `RESZ`.
- No universal SM3 translator and no claim that the specialized families cover
  the menu.
- No claim that every decoded call site has been executed; the matrix is built
  to accept the real values as they are observed.
