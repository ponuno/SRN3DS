# GFx / Scaleform D3D9 compatibility audit

Status: **static audit, no new runtime claim.** This note answers one question:
when `Interface/StartMenu.swf` finally opens, what does the *original* GFx9
renderer ask of our D3D9 shim, and what of that already works.

It is deliberately not about our AVM/display-list (`menu_swf_*`). The menu in
the shipping game is drawn by Scaleform inside the guest; AS2/AVM is GFx's
internal business. Our contract is **D3D9 + BSA/I/O + input**.

Baseline: `80006da` (`gfx-compat`).

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

## 4. Textures GFx asks for

`CreateTexture` is called with the standard 7 arguments; the format immediate
is pushed right before `Pool`.

| Format | Value | Where | Our state |
|---|---:|---|---|
| `D3DFMT_A8` | `0x1c` | `0xf822a7`, `0xf822d9`, `0xf82312` (font atlas), `0xf82e39` | **works** — `d9_texture_layout` size 1 B/px, `GPU_A8` backend path |
| `D3DFMT_A8L8` | `0x33` | `0xf82e9d` | **refused** — not in `d9_texture_layout` |
| `D3DFMT_A4R4G4B4` | `0x1a` | `0xf82f01` | **refused** |
| `D3DFMT_A8R8G8B8` | `0x15` | `0xf82f66` | **works** |
| `D3DFMT_DXT1/3/5` | `'DXT1/3/5'` | not on this path | accepted by layout, **decoded** by `d9_bc_to_pica` |
| `D24S8` (depth) | `0x4b` | `0xf82616` | format ok, **lockable bit refused** |

Important nuance: `0xf8205d`…`0xf83b28` is a **fallback chain**, not four
independent requests. The code caches the chosen format at `[obj+0x1c]` and,
if creation failed, retries `0x1c -> 0x33 -> 0x1a` (and `0x15` in the
render-target variant). So A8L8/A4R4G4B4 are only reached when **A8 creation
failed**. A8 currently succeeds, so those two are latent, not immediate.

Font atlases are created at half the target size with `Levels=1` and
`Pool=DEFAULT`; the dimension comes from `[obj+0x36c/0x370]`, so they inherit
whatever the render target size is. Our `pow2(w)&&pow2(h)` requirement is
satisfied for 512/1024, but a non-power-of-two UI target would turn the atlas
into `D3DERR_INVALIDCALL`.

`CreateTexture` with `w<8||h<8` requires RGBA8/XRGB8
(`d9_create_texture_ex`), and **no texture may be created inside
`BeginScene`/`EndScene`** (`!d->scene`). GFx creates outside the scene, so both
are fine today — but a lazier texture manager will hit the second.

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

**Coverage: 0 of 5 executable.** `source/d3d9_generated_vs_table.inc` emits
exactly 3 PICA programs and all three are engine `game.vs.*`
(`0ccc7542`, `31d368ce`, `5b644ad2`) from `imagespace002.fxp` — none is a GFx
program. `d9_menu_shader_match_vs` will *select* a member of the structural
family, but `set_vs` then requires the current declaration to feed
`position0/texcoord0/color0/color1`, and the fixed PICA VS only emits
`position + texcoord0`. So today the family is accepted and the colours are
dropped, or the declaration check refuses by name.

What GFx VS codegen needs is trivial — a 4-row `dp4` matrix transform plus
passthrough of the declared varyings, with `oPos = dp4(c0..c3, v0)`:

```text
o0 = dp4(c0,v0), dp4(c1,v0), dp4(c2,v0), dp4(c3,v0)   ; position
o1 = v1 (texcoord) / v2 (color) as the declaration says
```

`tools/vs_emit.py` already emits that shape for the engine programs; a
`menu.gfx.vs.<sha8>` entry per full hash is a table addition, not new codegen.

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

## 6. Status matrix

Legend: **OK** = real implementation; **PART** = works with a documented
restriction; **STUB** = returns success but does nothing; **REF** = named
refusal with a non-`S_OK` HRESULT.

| Method / format | Status | Blocker | Priority | Patch sketch |
|---|---|---|---|---|
| `CreateDepthStencilSurface(D24S8, lockable=1)` | **REF** | `A(6)` → `UNAVAILABLE`; GFx gets no depth | **P0** | accept lockable as a descriptor flag (ignore it, or back it with a readback surface); keep D24S8/D24X8-only |
| `CreatePixelShader` (36/38) | **REF**/probe | HAL init `jl` aborts | **P0** | add `mov,mad,mul` (0cce461b) and `texld,texld,lrp` (5b990161) plans; keep refusals named |
| `SetVertexShader` GFx family | **PART** | decl needs color0+color1; backend emits pos+uv only | **P0** | relax output requirement to "declared ⊆ supported" and add the 5 VS plans |
| `SetStreamSourceFreq` | **PART** | accepts only `Setting==1` | P0 | accept `1` and `0x1000001`, store the divisor; indexed-data bit is what GFx sets |
| `SetRenderState` ids 28/29/136/137/152/174 | **REF** (`INVALIDCALL`) | only 7,14,15,19,20,22,23,24,25,27,168,171 exist | **P1** | shadow-store unknown ids, return `S_OK`; only refusal that must stay is a value that changes a TEV draw |
| `CreateTexture` inside a scene | **REF** | `!d->scene` required | P1 | queue the create or defer binding; document it |
| `CreateTexture` non-pow2 / <8 px | **REF** | `pow2`, `padded` rules | P1 | allow non-pow2 for A8/RGBA8 by rounding up and keeping the guest size |
| `CreateTexture A8L8 / A4R4G4B4` | **REF** | layout table has A8/RGBA8/XRGB8/BC only | P2 | only reachable as fallback; map A8L8→A8+RGBA8 pair or expand to RGBA8 |
| `CreateTexture DXT1/3/5` | **OK** (decode) | needs pow2 ≥8, ≤1024 | P2 | already decodes in `d9_bc_to_pica`; add a test for 2 mips |
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

1. **`CreateDepthStencilSurface` is refused because GFx asks for a lockable
   D24S8** (`0xf82616`, `push 1`). The renderer binds no depth; the off-screen
   pass has nothing to test against. *P0, one-line fix in
   `create_depth_stencil`.*
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
5. **`SetStreamSourceFreq` accepts only `1`.** GFx sets the indexed-data bit on
   its dynamic VB before `SetIndices`. If the immediate is not exactly 1, the
   shim returns `UNAVAILABLE` and the rect wrapper bails before the draw.
   *P0 — verify with `TESV_DRAW_PROBE`, then widen.*
6. **Unsupported `SetRenderState` ids return `INVALIDCALL`.** GFx sets
   `FOGENABLE(28)`, `SPECULARENABLE(29)`, `SCISSORTESTENABLE(174)` and three
   vendor/tessellation ids (`136`, `137`, `152`) as part of its standard block.
   Even if GFx ignores the HRESULT, scissor/clipping state is silently absent,
   which matters as soon as the menu clips to a viewport. *P1.*
7. **A8 is the only font path and it is good — but the atlas size follows the
   target.** A non-power-of-two RT turns every glyph atlas into
   `D3DERR_INVALIDCALL` (`0xf822a7`). *P1: relax `pow2` for A8 or clamp the
   atlas to the next power of two.*
8. **`CreateTexture` while a scene is open is refused.** Today GFx creates
   before `BeginScene`; the moment the renderer lazily realises a texture in
   the middle of a frame the draw is lost. *P1, cheap to make non-fatal.*
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
   `S_OK`.
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
