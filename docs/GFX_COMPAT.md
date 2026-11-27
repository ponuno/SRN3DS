# GFx / Scaleform D3D9 compatibility audit

Status: **static audit, no new runtime claim.** This note answers one question:
when `Interface/StartMenu.swf` finally opens, what does the *original* GFx9
renderer ask of our D3D9 shim, and what of that already works.

It is deliberately not about our AVM/display-list (`menu_swf_*`). The menu in
the shipping game is drawn by Scaleform inside the guest; AS2/AVM is GFx's
internal business. Our contract is **D3D9 + BSA/I/O + input**.

Baseline: `80006da` (`gfx-compat`); follow-up `d3d9-gfx-fixes` closes the
blockers marked **FIXED** below (lockable depth, the GFx render-state block,
stream frequency, in-frame `CreateTexture`, NPOT A8).  That follow-up is a host
COM/null-backend result only — no draw, frame or ARM pixel claim.

## 1. Method and reproduction

The retail image is the IR-container input, not the on-disk exe:

```text
image : E:\census_ir2\ir_container\_work\ir_container_input.bin
header: <IIII magic=IRM1, lo=0x401000, code_size=0xc6931a, entry=90900>
offset: 16 + VA - 0x401000        (code is VA-linear; .rdata/.data follow it)
```

Two repo tools reproduce every number below (`.venv` has capstone 5.0.7):

```text
python tools\gfx_calls.py 0xf80000 0xf8d000     # D3D9 vtable call sites (container)
python tools\a43f_dis.py 0xf882e0 0xf88480      # GFx resource init (container)
python tools\gfx_tables_pe.py                    # cross-check of the table VAs
```

Shader identities and byte counts come from
`source/shader_packs/ui/menu_ir.json` and `plans.json`, which
`tools/.../menu_plan_tool.py` extracted from the **runtime** image. The
on-disk `F:\Skyrim\TESV.exe` is *not* a trustworthy source for these tables:
`tools/gfx_tables_pe.py` finds the same pointer table at `0x12dfca8` but the
blobs it resolves are 17–19 KB token streams, not the measured 128–172 B GFx
programs. That mismatch is the concrete form of the warning in
`docs/MENU_SHADERS.md`. The container cannot help either: its header `size`
covers code only and the `.rdata`/`.data` tail is not VA-linear.

`tools/gfx_calls.py` tracks the GFx idiom

```asm
mov eax,[renderer+0x1ac]   ; pDevice
mov edx,[eax]              ; vtable
push ...
mov eax,[edx+0x16c]
call eax
```

and resolves the slot through the *same* device-vtable order our shim installs
(`source/d3d9_com.c`, `DEVICE` table). Three slots are load-bearing and all
three appear in the init function:

| slot | offset | method | call site |
|---:|---:|---|---|
| 86 | `0x158` | `CreateVertexDeclaration` | `0xf88338` |
| 91 | `0x16c` | `CreateVertexShader` | `0xf8838a` |
| 106 | `0x1a8` | `CreatePixelShader` | `0xf883dd`, `0xf88427` |

This also proves the guest is calling **our** vtable, not a wrapper: the offsets
line up with our reordered table, not with stock `d3d9.h`.

## 2. GFx9 resource init (`0xf882e0`, caller `0xf8838c`)

The function is a three-stage wall. A failure in any stage clears
`[esp+0x13]` and the function returns 0 at `0xf8844c`; the rest of the
renderer still runs, half-initialised (this is the state we log today: objects
created, no menu frame).

| Stage | Loop | Table VA | Creates | Condition |
|---|---|---|---|---|
| 1 | `0xf88320` | `0x12dfc8c` / `0x12dfc9c` | **4 vertex declarations** | always |
| 2 | `0xf88360` | `0x12dfca8` (+`+0x14`) | **5 VS**, or 10 when `[dev+0xec]&2` | always |
| 3 | `0xf883c0` | `0x12dfcd8`, 45 slots | **40 PS** (5 NULL) | `[ebx+0x30] != 0` |
| 4 | `0xf88410` | `0x12dfa80`, 25 slots | 24 PS level-3 | `[ebx+0x34] >= 3` |

Stage 3 is gated by `[ebx+0x30]`, a "has shaders" flag, not by caps. Stage 4 is
the PS level (`>= 3`) and is **not reached** while `GetDeviceCaps` advertises
PS 2.0 — keep it that way: docs/MENU_SHADERS.md already says so.

Two facts that matter for us:

* Stage 2 also creates the declarations first; a failed
  `CreateVertexDeclaration` is fatal for that stage, because the VS objects are
  created into slots `[ebx+0x60..0x6c]`.
* Stage 3 uses `jl` on the HRESULT (`0xf883e1`) — **any negative HRESULT from
  `CreatePixelShader` aborts the whole menu HAL init**, not just one object.

## 3. D3D9 surface actually used

Call sites in `0xf80000..0xf8d000`, deduplicated. "N" is static call sites, not
runtime frequency.

| Method | N | Representative sites | Arguments observed |
|---|---:|---|---|
| `SetRenderState` | 95 | `0xf879ce`, `0xf84c46`, `0xf87d9e` | ids 7,14,15,19,20,22,23,24,25,27,28,29,136,137,152,168,174 |
| `CreateTexture` | 15 | `0xf8228d`, `0xf82e45`, `0xf87d6f` | `A8` (0x1c), `A8L8` (0x33), `A4R4G4B4` (0x1a), `A8R8G8B8` (0x15) |
| `SetSamplerState` | 13 | `0xf87a37`…`0xf87ae8` | stage 0/1, `MINFILTER`/`MAGFILTER`/`MIPFILTER`/`MIPLODBIAS`/`ELEMENTINDEX`, LINEAR |
| `SetTexture` | 13 | `0xf87bbc`…`0xf87bf7` | stages 0..3, `NULL` clears on 0/2/3 |
| `SetTextureStageState` | 9 | `0xf87afc`, `0xf87c0c` | stages 0/1, `COLORARG0`(11), `TEXTURETRANSFORMFLAGS`(24) |
| `SetVertexShader` | 8 | `0xf87cc5`, `0xf859d1` | GFx VS object |
| `SetVertexDeclaration` | 7 | `0xf87ca8`, `0xf859a1` | one of the 4 menu decls |
| `SetStreamSource` | 7 | `0xf82847`, `0xf867c2` | stream 0/1, dynamic VB |
| `DrawPrimitive` | 7 | `0xf87e53` | **type 5 (TRIANGLESTRIP), start, 2** |
| `DrawIndexedPrimitive` | 2 | `0xf82bc1`, `0xf82cf8` | cached mesh path |
| `SetStreamSourceFreq` | 1 | `0xf87cf3` | setting = `ebx` (indexed-data bit expected) |
| `SetIndices` | 1 | `0xf87d04` | 16-bit IB (`D3DFMT_INDEX16` at create) |
| `CreateVertexBuffer` | 1 | `0xf82665` | `0x60000` B, usage `0x208` (WRITEONLY\|DYNAMIC), FVF 0, pool `DEFAULT` |
| `CreateIndexBuffer` | 1 | `0xf826a6` | `0x30000` B, usage `0x208`, **fmt 0x65 = INDEX16**, pool `DEFAULT` |
| `CreateDepthStencilSurface` | 1 | `0xf82616` | **fmt 0x4b = D24S8, MS 0, quality 0, Lockable 1** |
| `CreateRenderTarget` | 0 | — | not used on the menu path |
| `CreateStateBlock`/`Begin`/`End` | 0 | — | GFx sets states directly |
| `UpdateSurface` | 1 | `0xf832b4` | A8/DEFAULT shadow surface |
| `Clear` | 3 | `0xf852fc`, `0xf85383`, `0xf8885f` | depth-only pass has `COLORWRITEENABLE=0` |
| `BeginScene`/`EndScene` | 2/2 | `0xf87c8a`, `0xf88b55` | one pair per renderer |
| `SetVertexShaderConstantF` | 4 | `0xf8473e`, `0xf86007`, `0xf8615d` | transform matrix in c0..c3 |
| `SetPixelShaderConstantF` | 7 | `0xf847ed`, `0xf86e56` | colour transform c2/c3 (text) |
| `SetViewport` | 4 | `0xf87888`, `0xf8882e` | full target |
| `SetRenderTarget`/`SetDepthStencilSurface` | 3/4 | `0xf88792`, `0xf887b1` | off-screen pass |
| Gamma / cursor / swapchain | 12 | `0xf886e3`, `0xf88498` | `Get/SetGammaRamp`, `SetCursorProperties`, `GetSwapChain` |

**The menu quad.** `0xf87e53` is `DrawPrimitive(TRIANGLESTRIP, start, 2)` — one
quad, two triangles, six vertices after strip assembly. There is no
`DrawIndexedPrimitiveUP` and no `DrawPrimitiveUP` on this path: GFx writes into
its own dynamic VB/IB. Any "UP-only" assumption in the shim is irrelevant here.

### 3.1 The state block GFx actually sets (`0xf879c0`…`0xf87c90`)

In order, ignoring repeats:

```text
SetRenderState(0x1b, 0|1)          ALPHABLENDENABLE
SetRenderState(0x0f, 0)            ALPHATESTENABLE
SetRenderState(0xae, 0)            SCISSORTESTENABLE = off
SetSamplerState(0, 5|6|7, 2)       MAG/MIN/MIP = LINEAR
SetSamplerState(0, 8, bias)        MIPLODBIAS
SetSamplerState(0|1, 0x0c, 0)      ELEMENTINDEX
SetTextureStageState(0|1, 11, ..)  COLORARG0
SetTextureStageState(0|1, 24, ..)  TEXTURETRANSFORMFLAGS
SetRenderState(0x0e, 0)            ZWRITEENABLE
SetRenderState(0x07, 0)            ZENABLE
SetRenderState(0x1c, 0)            FOGENABLE
SetRenderState(0x1d, 0)            SPECULARENABLE
SetRenderState(0x16, 0|1)          CULLMODE
SetRenderState(0x17, 8)            ZFUNC = ALWAYS
SetRenderState(0xa8, 0x0f)         COLORWRITEENABLE = RGBA
SetTexture(0..3, NULL) then binds
```

Then `BeginScene`, the quad, `EndScene`. The `SetRenderState(0x1b, 1)` that
matters for visibility is issued **after** the alpha gate at `0xf87d3e`
(`cmp byte [esp+0xbb],0 ; jbe 0xf87e55`) — i.e. GFx decides "transparent" before
it ever asks us to blend. That gate is the reason there are zero menu draws
today (`docs/PORT_LOG.md`, `d084c470`), and it is **not** a host refusal.

### 3.2 Alpha-0 gate: guest-side, no host heuristic

The port never inspects a colour alpha to decide whether to submit a draw. Every
`Draw*` entry point goes straight from `shader_gate()`/declaration checks to
`d9_draw_*`; there is no `if (alpha == 0) return` anywhere on the path. The gate
in `0xf87d3e` above is **TESV's own rectangle wrapper**, which returns before
calling `SetRenderState(0x1b,..)`/`DrawPrimitive` when its ARGB argument is
transparent. Disabling that behaviour would mean rewriting guest code, and it
would not create menu geometry: the two skipped quads per frame are the
fade/overlay wrapper, not GFx. `tests/alpha_scissor_smoke.c` pins the host
contract — a `COLOR0` vertex with alpha `0.0` still reaches the backend and the
draw counter increments.

Consequences that remain: the missing StartMenu mesh is a **GFx submission /
closure** problem (no `DrawPrimitiveUP`, no menu stream source, `menu_closure`
unarmed), not a host draw filter.

### 3.3 Scissor (implemented 2026-11-27)

`SetScissorRect` is no longer state-only. `D3DRS_SCISSORTESTENABLE` (174) now
pushes the rectangle to the backend, and the PICA backend programs
`C3D_SetScissor(GPU_SCISSOR_NORMAL,...)` before every draw:

* the rectangle is stored in logical D3D surface pixels and mapped with the
  inverse of the exact clip permutation the geometry uses
  (`d9_scissor_window_rect()`): rotated-screen mapping for the implicit LCD
  target, `orient_clip()` mapping for explicit/retained targets;
* the programmed PICA viewport is the map extent, so the SR scaled target and
  bounded 512x512 native targets keep the same framing;
* `C3D_SetViewport`/`C3D_FrameDrawOn` reset PICA scissor to `DISABLE`, so the
  state is re-issued per draw instead of once per state change;
* `SetRenderTarget`/device reset restore the D3D9 full-surface rectangle, and a
  full-surface rectangle is pushed as `DISABLE` (no cost when GFx leaves the
  scissor test off);
* a backend without `set_scissor` still records the rectangle and logs one named
  diagnostic (`this backend has no scissor stage`) instead of claiming clipping.

Limits: PICA scissor is a single axis-aligned window rectangle. GFx clip-depth
masks that need two or more simultaneous rectangles still go through the
`menu_swf_clip` CPU path; nested rotated masks remain the named refusal. D3D9
scissor is undefined outside `[0,surface]`, and so is the implementation.

### 3.4 Blend, alpha test and colour write (PICA mapping)

| D3D9 state | id | PICA |
|---|---:|---|
| `ALPHABLENDENABLE` | 27 | `C3D_AlphaBlend` enabled factors |
| `SRCBLEND`/`DESTBLEND` | 19/20 | `GPU_BLENDFACTOR` 1..11 exactly (ZERO/ONE/SRCCOLOR/INVSRCCOLOR/SRCALPHA/INVSRCALPHA/DSTALPHA/INVDSTALPHA/DSTCOLOR/INVDSTCOLOR/SRCALPHASAT) |
| `BLENDOP` | 171 | ADD/SUBTRACT/REVSUBTRACT/MIN/MAX (1..5) |
| `SEPARATEALPHABLENDENABLE` | 206 | selects independent alpha factors |
| `SRCBLENDALPHA`/`DESTBLENDALPHA` | 207/208 | alpha-side factors; replicated from RGB when 206=0 |
| `BLENDOPALPHA` | 209 | alpha-side equation |
| `ALPHATESTENABLE`/`ALPHAFUNC`/`ALPHAREF` | 15/25/24 | `C3D_AlphaTest(enable, GPU_TESTFUNC, ref)` on the final TEV alpha |
| `COLORWRITEENABLE` | 168 | the 4-bit mask maps 1:1 to `GPU_WRITE_RED|GREEN|BLUE|ALPHA` |

`d9_resolve_blend()` owns the D3D9 replication rule and is host-tested; the
`texkill` shader alpha test is handled separately by `apply_shader_alpha_test()`
and refuses a conflicting draw-state alpha test by name.


## 4. Textures GFx asks for

`CreateTexture` is called with the standard 7 arguments; the format immediate
is pushed right before `Pool`.

| Format | Value | Where | Our state |
|---|---:|---|---|
| `D3DFMT_A8` | `0x1c` | `0xf822a7`, `0xf822d9`, `0xf82312` (font atlas), `0xf82e39` | **works** — `d9_texture_layout` size 1 B/px, `GPU_A8` backend path; **NPOT allowed** (8..1024, one level, PICA CLAMP) |
| `D3DFMT_A8L8` | `0x33` | `0xf82e9d` | **refused** — not in `d9_texture_layout` (latent fallback only) |
| `D3DFMT_A4R4G4B4` | `0x1a` | `0xf82f01` | **refused** (latent fallback only) |
| `D3DFMT_A8R8G8B8` | `0x15` | `0xf82f66` | **works** — power-of-two only |
| `D3DFMT_R8G8B8` | `0x14` | D3DX file loaders (189 retail + 58 mod DDS) | **D3DX-only** — 24bpp B,G,R rows with `(w*3+3)&~3` pitch expand to A8R8G8B8 (A=255); direct `CreateTexture`/`CheckDeviceFormat` refused by name |
| `D3DFMT_A8B8G8R8` | `0x20` | D3DX file loaders (13 UBE files) | **D3DX-only** — DDS memory R,G,B,A is repacked to B,G,R,A on upload |
| `D3DFMT_DXT1/3/5` | `'DXT1/3/5'` | not on this path | accepted by layout, **decoded** by `d9_bc_to_pica` |
| `ATI2` / `BC5U` | `'ATI2'`/`'BC5U'` | no retail/mod file measured | accepted, decoded to R,G / B=0,A=255; see `docs/TEXTURE_FORMATS.md` |
| `ATI1` / `BC4U` | `'ATI1'`/`'BC4U'` | 7 UBE tint masks (mod) | accepted, decoded to R / G=B=0,A=255 |
| `BC7` (DX10 DXGI 98/99) | DX10 header | 120 UBE 2.0 files | **works**: vendored `bcdec`, decoded to PICA RGBA8 at upload (no BC7 → ETC1 conversion) |
| `D24S8` (depth) | `0x4b` | `0xf82616` | format ok, **Lockable=1 now creates a CPU D24S8 shadow** (FIXED) |

Important nuance: `0xf8205d`…`0xf83b28` is a **fallback chain**, not four
independent requests. The code caches the chosen format at `[obj+0x1c]` and,
if creation failed, retries `0x1c -> 0x33 -> 0x1a` (and `0x15` in the
render-target variant). So A8L8/A4R4G4B4 are only reached when **A8 creation
failed**. A8 currently succeeds, so those two are latent, not immediate.

### Float/HDR render targets

PICA200 has no float colour attachment, so `R16F` (0x6f),
`A16B16G16R16F` (0x71) and `R32F` (0x72) cannot be real render targets.
`CheckDeviceFormat`/the adapter return `D3DERR_NOTAVAILABLE` with a named
reason for all three; the `TESV_D3D9_HDR_LDR_FALLBACK=1` mapping is a
measurement-only probe (same-size A8R8G8B8) and is never a shipping default.
The evidence and VRAM budget are in `docs/FLOAT_RENDER_TARGETS.md`; the corpus
format census is in `docs/TEXTURE_FORMATS.md`.

Font atlases are created at half the target size with `Levels=1` and
`Pool=DEFAULT`; the dimension comes from `[obj+0x36c/0x370]`, so they inherit
whatever the render target size is.  `d9_texture_layout` now accepts a
non-power-of-two **A8** size in 8..1024 with exactly one level; the guest keeps
its logical size and PICA CLAMP sampling addresses it.  RGBA8/XRGB8 and every
BC format keep the power-of-two rule.  A8L8/A4R4G4B4 stay refused (they are only
reached if A8 creation fails).

**Render-target textures** (`D3DUSAGE_RENDERTARGET`, `A8R8G8B8`/`X8R8G8B8`)
also accept NPOT logical sizes in 1..1024.  citro3d can only allocate
power-of-two PICA storage, so the surface is rounded up per axis, the logical
image occupies the top-left `w x h` corner, draws use a logical viewport, and
the sampler scales normalized UVs by `w/storage_w`, `h/storage_h`.  Clear
covers the allocation (the padding is never sampled), while
StretchRect/GetRenderTargetData copy the logical sub-rect.  Allocations larger
than 512 per axis keep the historical bounded/down-scaled path.  See
`docs/NPOT_RENDER_TARGETS.md`.

The 24-bit corpus is handled at the D3DX layer, not by `CreateTexture`
(`docs/TEXTURE_FORMATS.md` §2a): `R8G8B8` DDS rows are B,G,R with a
`(w*3+3)&~3` pitch and expand to A8R8G8B8 with A=255; `A8B8G8R8` DDS bytes
R,G,B,A are repacked to B,G,R,A.  Mips, cube faces and volume slices are
converted independently.  `CheckDeviceFormat` reports both as source-only
with the named "expanded to A8R8G8B8" reason.  Remaining: the 1x1 A8B8G8R8
UBE eye cube used to hit the backend's 8x8 floor; it is now padded by
replicating the 1x1 texel over 8x8 faces.  The retail L8 `noisevolume.dds`
volume expands per slice/mip to A8R8G8B8.  DX10 BC7 (120 UBE files) decodes
through vendored `bcdec`.  Direct `CreateTexture(R8G8B8/A8B8G8R8)` stays
source-only, and the 15 measured menu call sites never request those formats
(only A8/A8L8/A4R4G4B4/A8R8G8B8 above).

`CreateTexture` with `w<8||h<8` requires RGBA8/XRGB8
(`d9_create_texture_ex`).  **CreateTexture is now legal inside
`BeginScene`/`EndScene`** (FIXED): the CPU store and native allocation happen
immediately, `LockRect`/`UnlockRect` writes stay in the CPU store, and the GPU
level is published at the real frame boundary (`Present` for COM, `EndScene`
for the core API).  A texture created and filled in frame *N* binds from frame
*N+1*; binding it inside the same scene is still a named INVALIDCALL.

## 5. Shader set: what is real, what is covered

### 5.1 Vertex shaders (5 objects, 5 distinct full blobs)

All are `vs_3_0`, 6–10 instructions, `dp4`-only plus `mov`, no samplers, no
defs. They are plain matrix transforms:

| full SHA-256 | ins | inputs | outputs |
|---|---:|---|---|
| `264c2065` | 6 | position0, texcoord0, color0 | position0, texcoord0, color0 |
| `6186c545` | 6 (×3 objects) | position0 | position0, texcoord0 |
| `0975cc50` | 10 | position0, color0, color1 | position0, color0, color1, texcoord0, texcoord1 |
| `4999d543` | — | (not in measured 10) | — |
| `73904d3f` | — | (not in measured 10) | — |

Constants: c0..c3 (and c0..c7 for the 10-instruction variant).

**Coverage: 3 of 5 executable (4 of 5 objects).** `source/shader_packs/ui/menu_vs_emit.py`
reads the five real `vs_3_0` blobs from table `0x12dfca8`, emits PICA VS
(`dp4` matrix rows, `dp4` texgen pairs, `mov` passthroughs, PICA clip remap),
runs them against the D3D IR interpreter on 256 samples (`max |dpos| = 0`) and
compiles each with picasso. The three verified programs are appended to
`source/d3d9_pica_vs_menu_table.inc`/`_menu_shbin.inc` (exact hash,
`codegen=emitted-verified`, `runtime=1`, `draw_enabled=1`):

| full SHA-256 | plan | objects |
|---|---|---:|
| `264c2065` | `menu.gfx.vs.264c2065` | 1 |
| `6186c545` | `menu.gfx.vs.6186c545` | 3 |
| `4999d543` | `menu.gfx.vs.4999d543` | 1 |

Two programs (`0975cc50`, `73904d3f`) write `color0` **and** `color1`; picasso
accepts exactly one `color` output and PICA TEV has a single primary raster
colour. They are now emitted as **declared approximations**: `color0`,
position and texcoord are emitted and numerically verified against the
simplified spec (`max |dpos| = 0`), `color1` is dropped and the plan carries
`quality: "approx"` plus an `approximation` string. So all five VS are
draw-enabled (`runtime=1`), three exact and two approx.

The menu runtime contract reuses `d3d9_pica_vs` with `bone_rows == 0` meaning
"menu": the uniform bank is the identity `c0..cN` (no bone/WVP remap) and the
declaration may carry extra elements the program does not read, while every
emitted input is still required. Skin programs are unchanged.

### 5.2 Pixel shaders (40 objects, 38 distinct full blobs)

The 10 measured prefix groups resolve to these representatives
(`source/shader_packs/ui/menu_ir.json`):

| full SHA-256 | ins | samplers | body | plan | runtime |
|---|---:|---:|---|---|---|
| `bfc0b3e0` | 1 | 0 | `mov` | `oC0 = c0` | **1** |
| `0eb58f00` | 3 | 1 | `texld,mov,mad` | `oC0 = tex0*c2+c3` | **1** |
| `0cce461b` | 4 | 0 | `mov,mad,mul,mov` | exact | 0 |
| `5b990161` | 5 | 2 | `texld,texld,lrp,mov,mad` | exact | 0 |
| `ee79ab27` | 7 | 1 | `texld,dp4×4,add,mad` | **approx** (DOT3 only) | 0 |
| `0a244f54` | 17 | 3 | YUV, 2 defs | — | refused |
| `0b031a8a` | 17 | 4 | YUV, 2 defs | — | refused |
| + 24 more | | | >6 stages, `c0.zwzw`, two-constant stages | — | refused |

Full pack census (`source/shader_packs/ui/plans.json`): **14 translated,
3 `runtime: 1`, 24 named refusals** of the 38. The third runtime program,
`9c152cf8`, is **not** in the StartMenu prefix set (it comes from the wider
UI pack), so the effective StartMenu coverage is:

> **2 of 38 distinct PS (≈2–4 of 40 objects) are draw-enabled.**

`CreatePixelShader` currently returns `S_OK` for all 40 only because
`TESV_D3D9_SHADER_SKIP_PROBE` turns uncertified shaders into
`shader_probe_object()` (non-executable, `has_tev=0`, draws refused). Without
that env the same 36 return `D3DERR_NOTAVAILABLE`, and because stage 3 tests
`jl` the whole menu HAL init returns 0.

So: **creating 5 VS/40 PS today is a measurement artefact, not compatibility.**

### 5.3 PS critical four: TEV expressibility vs PICA colour sources

All four critical programs now have exact TEV descriptors (or a concrete stage
scheme), and `0cce461b`'s four-stage translation is exact. The remaining block
for `0cce461b` and `5b990161` is the *second vertex colour*, not TEV:

| program | TEV stages | blocker |
|---|---|---|
| `bfc0b3e0` | 1 | none — `runtime=1`, draw-enabled |
| `0eb58f00` | 3 | none — `runtime=1`, draw-enabled |
| `0cce461b` | 4 (`mov c2`, `mad color0*c2+c3`, `mul α·color1.a`, `mov rgb`) | reads `color1.a`; PICA has one raster colour |
| `5b990161` | 5 (see below) | reads `color1.z` as the lerp factor |

`5b990161` is expressible in one 5-stage TEV program (previous refusal was a
planner `temp_not_previous`, not hardware):

```text
s0 TEX1 <- texcoord1        REPLACE(TEX1)
s1 TEX0 <- texcoord0        REPLACE(TEX0)
s2 INTERPOLATE(TEX0, PRIMARY=color1, TEX1)   ; tex0*c + tex1*(1-c)
s3 MODULATE(PREV, c2)
s4 ADD(PREV, c3)                            ; oC0 = s2*c2 + c3
```

Both still need `color1` at draw time. picasso rejects an output semantic
`color1` and PICA's TEV has a single primary colour, so there is no exact
vertex-colour producer for `color1`. The options are: (a) accept these plans
as create-time TEV objects (not probe) and keep the draw refused by name, or
(b) bind a proxy 256x1 identity texture and read `color1` through a texcoord
varying — exact only up to 8-bit quantisation and an extra texture slot. This
change takes (a): `CreatePixelShader` returns `S_OK` with a real
`has_tev=1` object for every translated plan, but `runtime`/`draw_enabled`
stay 0 unless the whole stage contract can be produced.

Measured result: **all 38 PS have a pack-side TEV plan and all 38 are
draw-enabled/runtime**; three of them (`d1465bec`, `916e67f1`, `a23f8b39`) are
2-pass programs (6+1/2 stages) in the `passes[]` format, replayed by
`d9_draw_plan_passes`. 8 plans carry `constant_ops` derived constants for
`c0.zwzw`-style swizzles; 26 are declared approximations (`color1`→`color0`,
dropped source negation, two-constant collapse, derived pack, or a named
dropped trailing instruction). No plan is labelled exact unless the unchanged
IR is reproduced.

## 6. Status matrix

Legend: **OK** = real implementation; **PART** = works with a documented
restriction; **STUB** = returns success but does nothing; **REF** = named
refusal with a non-`S_OK` HRESULT.

| Method / format | Status | Blocker | Priority | Patch sketch |
|---|---|---|---|---|
| `CreateDepthStencilSurface(D24S8, lockable=1)` | **OK** (FIXED) | descriptor owns a CPU D24S8 shadow; LockRect/UnlockRect round-trip, binding still uses the embedded PICA attachment | **P0** | done in `d3d9-gfx-fixes`; no depth upload op exists on PICA, so the shadow is lock-authoritative |
| `CreatePixelShader` (36/38) | **REF**/probe | HAL init `jl` aborts | **P0** | add `mov,mad,mul` (0cce461b) and `texld,texld,lrp` (5b990161) plans; keep refusals named |
| `SetVertexShader` GFx family | **PART** | decl needs color0+color1; backend emits pos+uv only | **P0** | relax output requirement to "declared ⊆ supported" and add the 5 VS plans |
| `SetStreamSourceFreq` | **OK** (FIXED) | accepts `1`, indexed-data `0x80000001` and any indexed-data count; stores per stream; `INSTANCEDATA`/count 0 are named refusals | P0 | done; the PICA draw still renders one instance |
| `SetRenderState` ids 26/28/29/52..59/136/137/152/174 | **OK** (FIXED) | shadow-stored with range validation; GetRenderState and state blocks round-trip. Fog/specular/clip-plane/scissor effects have no PICA fixed-function stage and are recorded only | **P1** | done in `render_slot` |
| `CreateTexture` inside a scene | **OK** (FIXED) | deferred publication at Present/EndScene | P1 | done; binds from the next frame |
| `CreateTexture` non-pow2 / <8 px | **PART** (FIXED for A8, RT) | A8 and RT textures 1..1024 NPOT (RT storage min 8); plain RGBA8/BC keep pow2 | P1 | done |
| `CreateTexture A8L8 / A4R4G4B4` | **REF** | layout table has A8/RGBA8/XRGB8/BC only | P2 | only reachable as fallback; map A8L8→A8+RGBA8 pair or expand to RGBA8 |
| `CreateTexture DXT1/3/5` | **OK** (decode) | needs pow2 ≥8, ≤1024 | P2 | already decodes in `d9_bc_to_pica`; add a test for 2 mips |
| `CreateTexture ATI2/BC5U (BC5)` | **OK** (decode) | needs pow2 ≥8, ≤1024; two BC4 channels, B=0/A=255 | P2 | corpus has no ATI2; synthetic block test only |
| `CreateTexture ATI1/BC4U (BC4)` | **OK** (decode) | needs pow2 ≥8, ≤1024; single channel to R | P2 | 7 UBE tint masks |
| `CreateTexture BC7 (DX10)` | **OK** (decode) | DXGI 98/99, 16-byte blocks decoded by vendored `bcdec`; pow2 ≥8 | P3 | 4x4 `eye01_sk.dds` remains below the 8x8 floor |
| `CreateTexture A8` | **OK** | `GPU_A8`, 1 B/px | — | — |
| `CreateVertexBuffer/IndexBuffer` (DYNAMIC, DEFAULT, FVF 0) | **OK** | — | — | — |
| `SetIndices` + `SetStreamSource` (16-bit) | **OK** | — | — | — |
| `DrawPrimitive(TRIANGLESTRIP,2)` | **OK** path exists | never reached (alpha gate) | — | do not mask the gate |
| `DrawIndexedPrimitive` | **OK** | cached-mesh path | — | — |
| `SetVertexDeclaration` (4 menu decls) | **PART** | `d9_decl_init_why` is strict | P1 | accept the GFx decls' unused/`UBYTE4N` element types |
| `SetVertexShaderConstantF` c0..c7 | **OK** | — | — | — |
| `SetPixelShaderConstantF` | **OK** | — | — | — |
| `SetTextureStageState` stage 0/1 | **OK** | — | — | — |
| `SetSamplerState` | **OK** (LINEAR) | ANISOTROPIC/BORDER refused, not used | — | — |
| `UpdateSurface` | **PART** | A8/DEFAULT shadow path | P1 | verify A8 source+dest pair |
| `CreateStateBlock` / capture / apply | **OK** | GFx does not use it | — | — |
| `SetRenderTarget` / off-screen pass | **PART** | X8R8G8B8/A8R8G8B8 only | P2 | — |
| `Get/SetGammaRamp`, `SetCursorProperties`, `GetSwapChain`, `SetDialogBoxMode`, `GetRasterStatus` | **OK/STUB** | video/cursor subsystem, not the quad | P3 | return plausible values, never `E_FAIL` |
| `Clear` with color writes off | **OK** | depth-only pass | — | — |

## 7. Top-10 blockers that appear the moment StartMenu opens

1. **`CreateDepthStencilSurface` refused the lockable D24S8 GFx asks for**
   (`0xf82616`, `push 1`). **FIXED**: the descriptor owns a zeroed CPU D24S8
   shadow; `LockRect`/`UnlockRect` read/write it, and the bound GPU usage is
   still the target's embedded `DEPTH24_STENCIL8` attachment. A non-lockable
   depth descriptor still refuses LockRect. *P0.*
2. **36 of 38 StartMenu PS are not executable.** With the skip probe off,
   `CreatePixelShader` returns `NOTAVAILABLE` and `0xf883e1` aborts the whole
   menu HAL. The first missing plan that matters is `0cce461b`
   (`mov,mad,mul,mov` — vertex colour × colour transform), then `5b990161`
   (`texld,texld,lrp`). *P0.*
3. **`SetVertexShader` refuses the GFx VS family on declarations.** Every GFx
   VS outputs at most `position0+texcoord0+color0`; the runtime demands
   `color0` *and* `color1`. Two of the three measured unique VS fail by name.
   *P0.*
4. **No GFx VS is emitted.** The 3 programs in
   `d3d9_generated_vs_table.inc` are engine image-space VS. Vertex colour
   therefore never reaches TEV → text and shapes lose their tint even if the
   draw runs. *P0/P1 (needed for correct colour, not for the first pixel).*
5. **`SetStreamSourceFreq` accepted only `1`.** **FIXED**: the setting is
   stored per stream; `1`, `0x80000001` (indexed data, one instance) and any
   indexed-data count are accepted. `D3DSTREAMSOURCE_INSTANCEDATA` and a zero
   count are named refusals. The PICA draw still emits one instance. *P0 — no
   longer blocks the rect wrapper; instanced draws are not claimed.*
6. **Unsupported `SetRenderState` ids returned `INVALIDCALL`.** **FIXED**:
   `26/28/29/52..59/136/137/152/174` are in `render_slot` with range checks;
   GetRenderState and state blocks round-trip. Fog/specular/clip-plane have no
   PICA fixed-function stage and the menu path has no scissor stage, so those
   effects are recorded, not applied. *P1.*
7. **A non-power-of-two RT turned every A8 glyph atlas into `INVALIDCALL`.**
   **FIXED**: A8 is NPOT 8..1024 with one level (PICA CLAMP); RGBA8/BC keep the
   pow2 rule. *P1.*
8. **`CreateTexture` while a scene was open was refused.** **FIXED**:
   creation and CPU Lock/Unlock are legal in frame; the GPU level is published
   at Present (COM) / EndScene (core), so the texture binds from the next frame.
   *P1.*
9. **`A8L8`/`A4R4G4B4` are latent landmines.** They are only requested if A8
   creation fails (`0x1c -> 0x33 -> 0x1a` fallback at `0xf8205d`), but when
   that happens the fallback itself fails and GFx ends with no shadow surface.
   *P2 — map both to RGBA8.*
10. **The alpha gate at `0xf87d3e` is the real reason for "no menu draw".** It
    tests the high byte of the wrapper's first argument before any D3D9 call
    that matters. Nothing in the shim should "fix" this; it must be traced to
    the colour transform that produces alpha 0. Until it opens, none of the
    above can even be measured at draw time. *P0 for diagnosis, not for
    patching.*

## 8. What is critical before the first menu `Draw`

Only four things must be true for *something* to appear, in this order:

1. `CreateDepthStencilSurface(400x240 or pow2, D24S8, lockable=1)` returns
   `S_OK`. **FIXED** — verified by `tests/gfx_d3d9_menu_compat_smoke.c`.
2. `CreatePixelShader` returns `S_OK` for `bfc0b3e0`, `0eb58f00`,
   `0cce461b`, `5b990161` (solid, text, modulate, two-texture lerp) — enough
   for rects, glyphs, tinted shapes and the simple two-map case.
3. `SetVertexShader` accepts the GFx family against the 4 menu declarations
   and the backend emits `position0/texcoord0` at minimum; `color0` is needed
   for correct tint.
4. The alpha gate at `0xf87d3e` opens, i.e. the colour transform produces a
   non-zero alpha for the first StartMenu quad.

Everything else (A8L8, DXT, state-block, sRGB, RT formats, gamma) is either
already working, unreachable, or cosmetic for the first frame.

## 9. Non-goals / open questions

* The `0xf80000..0xf84600` cluster (swapchain, cursor, gamma, video-mode) is
  Scaleform's device-manager layer. Its `ShowCursor`/`GetRasterStatus` calls
  are real device calls but never touch the menu quad; they are P3.
* Whether `[ebx+0x30]` (PS stage 3 gate) can ever be 0 is unknown; we have
  only observed it non-zero.
* The four vertex declarations at `0x12dfc8c/0x12dfc9c` could not be decoded
  from the on-disk `TESV.exe` (the shipped tables differ from the runtime
  image, as `docs/MENU_SHADERS.md` warns). Their element usage must be read
  from the runtime via `TESV_D3D9_TRACE`, not guessed.

## 10. Texture residency: frame pinning and limits

PICA executes the frame's commands after `EndScene`, so a texture sampled by a
submitted draw is protected until `C3D_FrameSync` proves completion. The core
pins textures on every sampler bind and again on draw submission, and releases
the pins in `Present` after the backend `frame_sync` hook (citro3d:
`C3D_FrameSync`). A pool that cannot evict anything because all candidates are
pinned refuses the allocation with a named `pinned` diagnosis instead of
fencing a native the GPU may still read. See `docs/TEXTURES.md` section 9.

`N3DS_TEX_GUEST_LIMIT` / `N3DS_TEX_GPU_LIMIT` (bytes or K/M/G, 0 = unlimited)
cap the cache, not `DevCaps` or the device budget. They are a residency policy,
not a format or capability change; `DevCaps` is untouched.
