# SHADERS — Skyrim LE shader packages, family classification and TEV feasibility

Owner: shader-TEV agent. This file is the only `docs/` file this work owns.
All numbers were measured on **2026-11-24** against the read-only install at
`E:\SteamLibrary\steamapps\common\Skyrim\Data` with the extractor in
`tools/skyrim/shaders.py`, the analysis in `tools/shaders/`, and the host tests
in `tests/tev_*.c`. Game bytes are never stored in this repository; only
derived names, counts and hashes are.

Primary data set: **`Skyrim - Shaders.bsa` — 122 `.fxp`, 10,475 chunks**
(the number quoted in `docs/ASSETS.md` §8). `Update.bsa` adds three packages
(`shaders002.fxp`, an identical `imagespace002.fxp`, and a larger
`lighting\059\p2600001.fxp`); the effective install is **123 packages /
12,201 chunks**. The base archive is used for every table below unless a row
says "Update".

## 1. What changed in the tooling

| File | Purpose |
|---|---|
| `tools/skyrim/shaders.py` | extended: whole-package container parser (`BSFX` / 8-byte / raw), per-chunk 36/78/86-byte metadata trailers, package/technique keys, override helper |
| `tools/shaders/asm.py` | independent D3D9 SM3 decoder: tokens, swizzles, `dcl`, CTAB constant table, feature extraction |
| `tools/shaders/packages.py` | family rules, dedup, package summary, TEV feasibility screen |
| `tools/shaders/report.py` | CLI; writes `shader_families.json`, `shader_menu.json`, and the local `tev_chunks.bin` dump |
| `source/d3d9_tev.c/.h` | real ps_3_0 → PICA200 TEV translator, rejection diagnostics, reference evaluator |
| `source/d3d9_shader.c/.h` | validated-stream hook; `D9_SHADER_TEV`, retained program API, explicit TEV rejection text |
| `source/d3d9_com.c/.h` | `CreatePixelShader` retention/acceptance and host regression introspection |
| `source/d3d9_menu_light.c/.h` | structural matcher + exact CPU evaluator + five-stage pre-baked logo prototype |
| `tests/tev_translate_smoke.c` | translation structure + rejection tests |
| `tests/tev_accuracy_smoke.c` | arithmetic accuracy against an independent D3D9 interpreter |
| `tests/tev_com_smoke.c` | end-to-end `CreatePixelShader` acceptance/rejection through the guest COM path |
| `tests/isolated/uicopy/` | synthetic exact `ui.copy` host mock + ARM/Azahar integrated draw and pixel readback |
| `tests/menu_light_smoke.c` | accepted-signature and CPU-reference tests for the logo family |
| `tests/tev_real_smoke.c` | measurement tool over extracted chunks, including menu-light signature counts |

Reproduce:

```powershell
$env:PYTHONPATH='tools'
.venv\Scripts\python.exe -m shaders.report all
.venv\Scripts\python.exe -m shaders.report dump          # 9,621 PS chunks
powershell -File tools\run_host_tests.ps1 -Only tev_translate,tev_accuracy,tev_com,menu_light,tev_real
```

Outputs: `E:\Skyrim_N3DS_build\assets\shaders\shader_families.json`,
`shader_menu.json`, `tev_chunks.bin` (local only).

## 2. Container and package structure (measured)

A `.fxp` is a Bethesda shader package, not HLSL source. Three layouts ship:

```
BSFX              12-byte header   u32 zero, u32 chunk_count        120 lighting files
u32 2, u32 2       8-byte header   (shaders001, shaders002)           2 files
raw                none           starts with BSVS/BSPS              imagespace002
```

Each chunk record is:

```
b"BSVS" | b"BSPS"   u32 meta   u32 size   bytecode[size]   trailer
bytecode := u32 version (0xFFFE0300 vs_3_0 / 0xFFFF0300 ps_3_0)
            comment tokens (0xFFFE, first is CTAB)
            instruction tokens
            u32 0xFFFF
```

Measured trailer sizes across all 10,475 chunks (the gap from the end of the
bytecode to the next tag, and to EOF for the last chunk):

| Trailer | Chunks | Meaning |
|---:|---:|---|
| 36 bytes | 853 VS + 1 | vertex reflection block (register binding slots, `0xab` padding) |
| 78 bytes | 9,492 PS + 122 last-chunk | pixel reflection block; may contain an 8–16 byte compiler source snippet |
| 86 bytes | 7 PS | same plus 8 extra bytes of source text |

The 78-byte block starts with a u16 register/binding table (`0x80` = unused),
then a 4-byte identifier, 18 `0x0f` mask bytes and a 2-byte tail. It is
reflection state; the translator does not need it and skips it. Seven chunks
carry visible fragments (`tFogAmount(float3 ...`, `FlarePS(v2fFlare IN)`) — the
only source-like text in the whole archive, far too short to reconstruct code.

### 2.1 Techniques and the 80-permutation packages

No entry-point or technique name is stored in the bytecode. The only CTAB
names are constant/variable names. The engine's package keys come from strings
in `TESV.exe`:

```
shadersfx\%s\%03d\V0%s.fxp        shadersfx\%s\%03d\P0%s.fxp
SHADERSFX/Shaders%03d.fxp         SHADERSFX/Imagespace%03d.fxp
data\shadersfx\%s\%03d            data\shadersfx\%s
```

So a technique is **(package directory, `%03d` index, stem)**. In the shipped
data this is exactly the file path, and every lighting stem is a hexadecimal
flag mask `p<hex>`. The 120 lighting packages are 108 packages with 80 chunks
each plus 12 packages with 1 chunk (all 80 permutations compiled to the same
bytecode). The chunk `meta` word encodes the variant:

```
meta = stem_flags + variant_bits + (sub_index << 4)
variant 0..7   variant_bits = 0x0000,  sub = variant
variant 8..39  variant_bits = 0x8000,  sub = variant-8
variant 40..79 variant_bits = 0xC000,  sub = variant-40
```

Example: `p1001` has metas `0x1001, 0x1011, ... 0x1071, 0x9081, ...,
0x9271, 0xd001, ... 0xd271`. The stem bits that change the CTAB are measured,
not guessed:

| Stem bit | Observed effect (packages where it is set) |
|---:|---|
| 0x001 | always set (base lit technique) |
| 0x002 / 0x004 | no CTAB change (geometry/runtime flag: skinning, vertex colour/alpha) |
| 0x400 | adds `SpecularColor` / `SpecularSampler` |
| 0x800 | adds `SubSurfaceSampler` + `LightingEffectParams` |
| 0x1000 | adds `SubSurfaceSampler` + `LightingEffectParams` (second soft-light variant) |
| 0x2000 | adds `BackLightMaskSampler` |
| 0x10000 | adds `ProjectedNoiseSampler` + `ProjectedUVParams(2)` |
| 0x20000 | no CTAB change |
| 0x80000 | adds `WorldMapOverlay*` |
| 0x200000 | adds `EnvSampler` / `EnvMaskSampler` / `EnvmapData` (with 0x400000/0x800000 variants) |
| 0x1000000 / 0x2000000 | no CTAB change |

The remaining flag bits are unproven; they are reported as unknown rather than
named.

## 3. Deduplication (base archive, 10,475 chunks)

| Grouping | Count | Definition |
|---|---:|---|
| (a) identical bytecode | **6,659** | SHA-1 of the whole chunk (version + CTAB + instructions + END) |
| (b) identical instruction stream, CTAB ignored | **6,655** | SHA-1 over version + instruction tokens *and operands*, comments removed |
| (b′) identical opcode sequence | **6,455** | SHA-1 over the opcode list only (operands ignored) — this reproduces the figure in `docs/ASSETS.md` §8.3 |
| (c) package path + technique | **122** | one per `.fxp`; the lighting stems are the flag masks above |
| chunks per distinct bytecode | 1.573 | |
| chunks per distinct opcode sequence | 1.623 | |

The difference between (b) and (b′) is 200 distinct instruction streams that
share an opcode sequence with another stream but differ in operands
(constants/registers). Both counts are useful: (b) is a semantic count, (b′)
is the historical one.

The 7 chunks that fit the old `dcl/mov/m4x4/texld` subset are one instruction
stream (`dcl_texcoord t0.xy`, `dcl_2d s0`, `texld oC0,t0,s0`) but **three
distinct bytecode blobs**: 184 bytes (imagespace, CTAB `Image`), 188 bytes
(imagespace, CTAB `HeightMap`) and 188 bytes (shaders001, CTAB `BaseSampler`).
The "3 distinct programs" in `docs/ASSETS.md` §8.3 is that bytecode-level
count; the semantic count is one program, which is why the translator handles
all 7 with one rule.

Most duplicates are the 20 repeated permutations inside each 80-chunk lighting
package (60 distinct programs per package). There is **no bytecode overlap
between two different lighting packages**: changing one stem flag changes the
compiled code, even when the CTAB is identical.

## 4. Family classification

Rules are deterministic and ordered; the first match wins. They live in
`tools/shaders/packages.py` `FAMILY_RULES` and are quoted here.

| Family | Rule |
|---|---|
| post-process/imagespace | package path contains `imagespace` |
| water | PS uses any `WaterParams` / reflection / refraction CTAB name |
| sky | PS uses `PParams` or a sky colour range and is not water |
| terrain/landscape | PS uses `MultiTexture*`, `LODNoise*`, `WorldMapOverlay*`, `HeightSampler` |
| skinning/characters | VS declares blendweight+blendindices, or PS uses membrane/facegen/tint-mask CTAB names |
| particles/effects | PS uses flare/blood/diffuse/alpha effect names, or VS is colour+texcoord billboard |
| blit/copy | PS has ≤3 instructions, ≤1 texld and no lighting CTAB names |
| ui/interface | no lighting CTAB names and none of the above |
| lighting/deferred/shadows | all remaining lit material shaders |
| grass/trees | no shipped package (reported as 0, see §4.2) |

### 4.1 Counts (base archive)

"streams" = distinct instruction streams (b); "opseq" = distinct opcode
sequences (b′); "PS/VS" = chunk kinds.

| Family | Chunks | PS | VS | Distinct streams | Distinct opseq | Packages | Branches | Mean instr | Max instr |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ui/interface | 314 | 195 | 119 | 113 | 112 | 1 | 0 | 28.2 | 64 |
| sky | 10 | 10 | 0 | 7 | 7 | 1 | 0 | 8.9 | 14 |
| water | 72 | 72 | 0 | 38 | 38 | 1 | 24 | 64.6 | 132 |
| terrain/landscape | 327 | 327 | 0 | 247 | 247 | 11 | 60 | 106.9 | 177 |
| lighting/deferred/shadows | 7,175 | 6,902 | 273 | 4,824 | 4,700 | 91 | 3,300 | 109.9 | 264 |
| skinning/characters | 2,162 | 1,872 | 290 | 1,194 | 1,127 | 18 | 480 | 86.9 | 237 |
| particles/effects | 284 | 172 | 112 | 167 | 167 | 3 | 0 | 108.9 | 258 |
| grass/trees | 0 | 0 | 0 | 0 | 0 | 0 | 0 | — | — |
| post-process/imagespace | 120 | 60 | 60 | 61 | 58 | 1 | 4 | 25.0 | 220 |
| blit/copy | 11 | 11 | 0 | 5 | 3 | 1 | 0 | 2.45 | 3 |
| **total** | **10,475** | **9,621** | **854** | **6,655** | **6,455** | **122** | | | |

Per-family distinct-stream counts sum to 6,656; one instruction stream appears in two families (a shared utility program), while the global dedup is 6,655. Per-family opcode-sequence counts sum to 6,459; four opcode sequences are shared across families.

Texture-sample histograms (PS):

| Family | texld per chunk |
|---|---|
| ui/interface | 0:144, 1:73, 2:50, 3:29, 4:9, 5:9 |
| sky | 1:8, 2:2 |
| water | 1:13, 2:8, 3:14, 4:18, 5:7, 6:8, 7:4 |
| terrain/landscape | 3:25, 4:136, 5:2, 6:2, 8:1, 9:1, 12:12, 13:68, 14:12, 15:68 |
| lighting/deferred/shadows | 0:273 (VS), 1:4, 2:125, 3:850, 4:1,871, 5:2,116, 6:888, 7:480, 8:432, 9:136 |
| skinning/characters | 0:290 (VS), 1:32, 2:200, 3:672, 4:572, 5:248, 6:80, 7:68 |
| particles/effects | 0:117 (VS), 1:5, 2:2, 6:24, 7:136 |
| post-process/imagespace | 0:63 (VS), 1:7, 2:3, 3:11, 4:5, 5:5, … 16–18: 7 |
| blit/copy | 0:7, 1:4 |

### 4.2 What the families really are

* **lighting/deferred/shadows** is one compiled shader family: 120 flag packages under `shadersfx\lighting\059`, plus its generic vertex
  transforms (found in `shaders001`). Sky, water, grass, trees and most
  effects are *not* separate compiled packages; their materials either use the
  same lighting permutation or a `BSEffectShaderProperty` that has **no
  `.fxp` at all** (see §5).
* **terrain/landscape** is the `MultiTextureDiffuse0..5` /
  `MultiTextureNormal0..5` group: 11 packages, 327 chunks, 13–15 texture
  samples for 136 of them.
* **skinning/characters** is 17 lighting packages with `TintColor`/tint-mask
  constants (armour/clothing/skin) plus the `shaders001` membrane/facegen
  programs (`BaseSampler`, `MembraneVars`, `PropertyColor`). The
  blendweight/blendindices vertex signatures are counted here too.
* **ui/interface** is 119 generic 2D vertex transforms and 195 small pixel
  programs in `shaders001`. The game's actual UI (menus, HUD, text) is
  Scaleform/Interface.bsa, not `.fxp`; this family is the 3D-menu/flare/
  billboard utility set.
* **blit/copy** is 11 programs: four `dcl,dcl,texld` full-screen copies and
  seven constant-colour fills. The other three `dcl,dcl,texld` copies live in
  the post-process family (`imagespace002`), so the shipped set is seven
  texture copies in total. All four copies in this family translate; the three
  rejected fills use a non-uniform `.xxy` swizzle.
* **grass/trees** is genuinely **0 chunks**: no package or CTAB name mentions
  grass/tree/foliage. Grass and foliage ride `lighting/059` with alpha test and
  vertex colour; their reason for existing as a separate family would be asset
  metadata, not shader bytecode.

## 5. The main-menu subset

### 5.1 Evidence

From `TESV.exe` strings: `SHADERSFX/Shaders%03d.fxp`,
`SHADERSFX/Imagespace%03d.fxp`, `Interface/Logo/Logo.nif`,
`Interface/Logo/CameraPath`, `fUIMistModel_*`, `fUIMistMenu_*`,
`BGS_LOGO.BIK`, `sMainMenuMusic`.

From `Skyrim - Meshes.bsa`:

| Mesh | Size | Shader property | Textures | Geometry |
|---|---:|---|---|---|
| `meshes\interface\logo\logo.nif` | 237,735 | 1 × `BSLightingShaderProperty`, flags1=`0x82400301`, flags2=`0x8021` | `Logo02_d.dds`, `Logo02_n.dds` | 3,012 v / 3,346 tri |
| `meshes\interface\intmenufogparticles.nif` | 50,446 | 2 × `BSEffectShaderProperty` | `FXSteamThinAnim.dds`, `GradSteamThin.dds` | 538 v / 200 tri |

So the main menu needs exactly three things:

1. a **full-screen copy/tint** pass (imagespace / fade / composite),
2. a **lit material** for the logo (diffuse + normal + dir light + fog,
   vertex colours present in the geometry),
3. a **BSEffectShaderProperty** fog/smoke pass (particles, alpha blended).

The logo's lighting package cannot be resolved to one exact file from shipped
data: the engine's material-flags → package-stem function is executable code,
not data. What can be measured is the feature class: diffuse + normal, no
shadow-mask sampler, no point light, no envmap, no specular sampler, fog on.
That is the 8-constant base-lit class (`DiffuseSampler, NormalSampler,
DirLightColor, DirLightDirection, DirectionalAmbient, EmitColor, FogColor,
MaterialData`). The smallest concrete member is `p1200005` (25 instructions,
2 texld, 852 bytes); `p5` has a 24-instruction 840-byte variant. Packages that
add `LODNoiseSampler`/`WorldMapOverlay*` (`p2400005`, `p2480005`) are terrain/
map LOD variants, not the logo.

### 5.2 Package list with bytecode sizes

Base archive:

| Package | File bytes | Chunks (VS/PS) | Distinct streams | Chunk bytes (min/med/max) | Role in the menu |
|---|---:|---:|---:|---:|---|
| `shadersfx\imagespace002.fxp` | 81,872 | 120 (60/60) | 61 | 136 / 280 / 3,744 | post-process: copy, bloom, blur, DOF, colour range; contains three copies of the 184-byte blit |
| `shadersfx\shaders001.fxp` | 1,766,270 | 1,703 (794/909) | 585 | 136 / 888 / 3,252 | all generic VS; 4 copies of the 188-byte blit; flare (`FlarePS`, 340 B), membrane/facegen, water, sky |
| `shadersfx\lighting\059\p1.fxp` | 159,844 | 80 (0/80) | 60 | 936 / 1,960 / 2,668 | base lit material (specular off, no subsurface) |
| `shadersfx\lighting\059\p5.fxp` | 152,164 | 80 (0/80) | 60 | 936 / 1,960 / 2,668 | base lit with one extra runtime flag; simplest 840 B / 24 instr program |
| `shadersfx\lighting\059\p1200005.fxp` | 954 | 1 (0/1) | 1 | 852 | smallest base-lit program (25 instr, 2 texld, no branch) |
| `shadersfx\lighting\059\p1a00001.fxp` | 1,022 | 1 (0/1) | 1 | 920 | base lit variant (30 instr) |
| `shadersfx\lighting\059\p1e00001.fxp` | 1,022 | 1 (0/1) | 1 | 920 | base lit variant (30 instr) |
| `shadersfx\lighting\059\p2400005.fxp` | 1,270 | 1 (0/1) | 1 | 1,168 | base lit + `LODNoiseSampler`; not the logo |
| `shadersfx\lighting\059\p2480005.fxp` | 2,734 | 1 (0/1) | 1 | 2,632 | LOD + world-map overlay; not the logo |

`Update.bsa` (effective install):

| Package | File bytes | Chunks (VS/PS) | Note |
|---|---:|---:|---|
| `shadersfx\shaders002.fxp` | 1,786,692 | 1,726 (814/912) | index 002 of `Shaders%03d`; same role as shaders001 |
| `shadersfx\imagespace002.fxp` | 81,872 | 120 (60/60) | byte-identical override |
| `shadersfx\lighting\059\p2600001.fxp` | 305,444 | 80 (0/80) | replaces the base 292,644-byte terrain/map package |

### 5.3 Ranked "must implement first"

| Rank | Item | Bytecode | Why |
|---:|---|---:|---|
| 1 | **full-screen copy** `dcl t0.xy` / `dcl_2d s0` / `texld oC0,t0,s0` | 184 B (imagespace) / 188 B (shaders001) | seven shipped copies in the base archive (plus four in Update's `shaders002`); the composite/fade primitive; **implemented and validated** (§7.3) |
| 2 | **base lit logo PS** (diffuse + normal + dir light + fog) | 840–852 B | `Interface/Logo/Logo.nif` is a `BSLightingShaderProperty` with `Logo02_d/n`; the feature class is the 8-constant base-lit set |
| 3 | **BSEffectShaderProperty fog** | no `.fxp` | `intmenufogparticles.nif` uses it with `FXSteamThinAnim`/`GradSteamThin`; implement as a fixed TEV pipeline (texture × vertex colour, alpha blend) |
| 4 | **simple tint / flare** | 340 B (FlarePS), 204 B (DebugColor fills) | menu logo flare and full-screen tint; 19 chunks of this shape translate today |
| 5 | **shadow / point-light / envmap lighting permutations** | 1–2 KB each | not needed by the menu scene: the logo material has no shadow-mask/point-light/envmap constants |

## 6. TEV feasibility per family

PICA200 facilities available to a translator:

* **6 TEV combiner stages**, **3 texture units**. Each stage has three sources
  (`PRIMARY`, `FRAGMENT_PRIMARY/SECONDARY`, `TEXTURE0..2`, `CONSTANT`,
  `PREVIOUS`), each with an operand selector (colour, alpha, R/G/B and
  one-minus forms), a combine function (replace, modulate, add, add_signed,
  interpolate, subtract, dot3_rgb, dot3_rgba, multiply_add, add_multiply) and
  a 1/2/4 scale.
* Per-stage constant colour is **RGBA8**; stage outputs are clamped. This is
  an LDR pipeline.
* Hardware alpha test, depth test, blending and fog are outside the combiner
  and are directly usable.
* The vertex shader has 16 attributes, up to 7 varying outputs and 96 float
  uniform registers; 4-bone skinning, normal transforms and fog factors are
  expressible.

What is **not** available: dynamic branching, loops, reciprocal/square
root/power/log/exp/normalise, compare/select, per-channel arbitrary swizzles,
random-access temporaries (only the immediately previous stage is readable),
more than 3 textures or 6 stages, and float-precision constants/accumulators.

Measured translator acceptance over the extracted chunks (the translator in
`source/d3d9_tev.c`, run with `tests/tev_real_smoke.c`):

| Family | PS chunks | Accepted | Top rejection causes |
|---|---:|---:|---|
| lighting/deferred/shadows | 6,902 | 0 (0.0 %) | first cause: `defi`/loops 3,300; `rcp` 2,150; VPOS read 695; >3 input vectors 338; `rsq` 154 |
| skinning/characters | 1,872 | 0 (0.0 %) | stale temp 580; `defi` 480; `rcp` 184; >3 input vectors 76; VPOS 59 |
| terrain/landscape | 327 | 0 (0.0 %) | `rsq` 101; stale temp 41; VPOS 26; `rcp` 15; >3 input vectors 3 |
| ui/interface | 195 | 0 (0.0 %) | 96 need two live intermediates; 38 UVs computed in temps; 25 `rcp`; 15 `texkill`; 9 `dp4` |
| particles/effects | 172 | 5 (2.9 %) | 160 use non-uniform `.zwz` sprite swizzles; 3 `texkill`; 1 `rcp`; 1 out-of-range `def` |
| post-process/imagespace | 60 | 4 (6.7 %) | 22 UVs in temps; 16 non-uniform swizzles; 6 `rcp`; 3 `defi`; 2 use >3 textures; 1 out-of-range `def` |
| blit/copy | 11 | **8 (72.7 %)** | 3 `mov .xxy` fills (non-uniform swizzle) |
| sky | 10 | 2 (20.0 %) | 2 `frc`; 6 multi-temp |
| water | 72 | 0 (0.0 %) | 24 loops; 4–7 texld; `rcp`/`rsq` |
| grass/trees | 0 | — | no shipped bytecode |

The literal-range audit reduced aggregate acceptance from 21 to 19: one
particles/effects chunk and one post-process/imagespace chunk had previously
been accepted after silently clamping an out-of-range `def` component. Both are
now rejected by name; the simple-copy and sky counts are unchanged.

The accepted column is the generic translator only. The separate fixed-pipeline
matcher in §7.4 recognises 6 additional base-lit chunks (5 `p5_MAD_NORMAL`, 1
`p1200005_ADD_NORMAL`); they remain rejected by the generic translator and are
not silently accepted by `CreatePixelShader`.

Verdicts:

* **lighting**: a universal SM3→TEV translation is impossible (loops, `rcp`,
  `rsq`, `nrm`, `cmp`, 4+ live values, up to 9 textures). A **pre-specialised**
  per-permutation translation is feasible for the simple lit model: fold the
  flags, unroll the light loop, emit `dp3` lighting + `mad` + `c7.a` output
  modulation = 4–6 TEV stages with 2 textures. Shadow-mask/point-light/envmap
  permutations exceed 6 stages and need multiple passes.
* **characters**: skinning is a **vertex-shader** task (PICA VS matrix
  palette), not TEV. The membrane/facegen PS programs are flat/simple and fit
  2–4 stages; tint colours are constants (16 tint colours, not 16 textures), so
  the 3-texture limit is not the blocker. The blockers are multi-temp dataflow
  and alpha test, both solvable by pre-specialisation + hardware alpha test.
* **terrain**: 13–15 texture samples cannot fit 3 texture units. Verdict:
  **multi-pass (2 × 3 textures) or pre-baked blended layers**; a single-pass
  TEV implementation is not possible.
* **ui**: the blit/tint subset (19 accepted chunks across families) is expressible;
  most of the rest needs two live intermediates, `rcp`, `dp4` or
  `texkill`. Verdict: implement the simple subset; reject the rest by name.
* **effects**: 136 of 172 PS use 7 texture samples and 160 use `.zwz` sprite
  swizzles. Verdict: implement a fixed billboard pipeline (texture × vertex
  colour, alpha/additive blend) rather than translating; that is what the menu
  fog actually needs.

## 7. First implementation: simple unlit TEV passes

Files: `source/d3d9_tev.h`, `source/d3d9_tev.c`; tests `tests/tev_*.c`.
This is a **real translation** of the token stream, not a hash template.

### 7.1 Accepted subset

| Opcode | TEV mapping |
|---|---|
| `dcl_texcoord tN` / `dcl_2d sN` | records input / 2D sampler; `dcl_position vPos` is accepted and ignored until read |
| `def cN, x,y,z,w` | finite LDR literal in [0,1], packed RGBA8; other values rejected |
| `texld rN, tM, sK` | allocates texture unit (≤3); value is `TEXTURE0..2` |
| `mov` | `REPLACE` |
| `mul` | `MODULATE` |
| `add` | `ADD` |
| `sub` | `SUBTRACT` |
| `mad` | `MULTIPLY_ADD` (`a*b+c`) |
| `lrp` | `INTERPOLATE` (`a*c + b*(1-c)`) |
| `dp3` | `DOT3_RGBA` |
| `nop` | dropped |

Every other opcode is rejected with its name, including `cmp`, `rcp`, `rsq`,
`pow`, `nrm`, `dp4`, `min`, `max`, `texkill`, `texldd`, `texldl`, `defi` and
all branch/loop/call instructions.

Further limits: straight-line code only; at most 6 stages; at most 3 texture
units; at most 2 distinct constants in one instruction (one may be loaded into
`PREVIOUS` by an extra stage); a temp may be read while it is the immediately
previous stage's output, otherwise the value is stale; RGB swizzles must be
identity or a replicated component (`.xxxx`, `.yyyy`, `.zzzz`, `.wwww`); source
modifiers must be none or negate; `texld` UV/sampler operands must be
unmodified, non-relative register tokens, with a UV register below 16; 2D
samplers only; no MRT, no depth output. `oC0.rgb` must be fully defined; an
undefined alpha is forced to 1 with one extra `REPLACE` stage. Literal `def`
components must be finite and in [0,1]; non-finite or out-of-range values are
rejected with the constant and component named rather than silently clamped.

### 7.2 Integrated shader hook and COM retention

```c
D9TevProgram program;
char why[256];
if (d9_tev_translate_pixel(code, words, &program, why, sizeof why) == 0) {
    /* store program in the pixel-shader object */
}
```

The descriptors use libctru's numeric encodings (`GPU_TEVSRC`,
`GPU_TEVOP_RGB/A`, `GPU_COMBINEFUNC`, `GPU_TEVSCALE`), so a citro3d backend
feeds them to `C3D_TexEnv` directly. `D9TevStage` is the `C3D_TexEnv` shape:
`src_rgb[3]`, `op_rgb[3]`, `func_rgb`, `src_a[3]`, `op_a[3]`, `func_a`,
`scale_rgb/alpha`, and `const_rgba8`.

The hook is now implemented in `source/d3d9_shader.c`. After the token walk
reaches a valid END, an unknown pixel shader is passed to
`d9_tev_translate_pixel` with the exact consumed word count. Success sets
`D9_SHADER_TEV`; failure copies the translator's named reason into
`D9ShaderInfo.tev_reason`, and `d9_shader_diagnostic` emits it verbatim after
`TEV rejected:`. Vertex shaders and the two exact SM2 templates keep their
historical kinds.

`d9_shader_analyze_tev` is the COM-facing API. `source/d3d9_com.c` stores the
returned `D9TevProgram` inline in the pixel-shader object, accepts
`D9_SHADER_TEV` in `CreatePixelShader`, and exposes host-only regression
introspection (`d9_com_pixel_shader_tev`, `d9_com_last_shader_rejection`).
Unsupported programs still return `D3DERR_NOTAVAILABLE`, leave the guest output
pointer null, and log the exact named reason. `tests/tev_com_smoke.c` drives the
real COM vtable/stdcall path: the shipped blit and tint programs create TEV
objects, while `cmp` fails with
`TEV rejected: unsupported opcode 'cmp' at word 7`.

### 7.3 Measured accuracy

`tests/tev_accuracy_smoke.c` assembles seven ps_3_0 programs and compares
`d9_tev_eval_pixel` (over the translated TEV stages) with an independent D3D9
interpreter over the original tokens. Constants are RGBA8-quantised in both
paths; inputs are LDR.

| Program | TEV stages | Textures | Vectors | Max abs error |
|---|---:|---:|---:|---:|
| blit (`dcl,dcl,texld`) | 1 | 1 | 4,000 | 0.000000000 |
| tint (`tex*c0`) | 1 | 1 | 4,000 | 0.000000000 |
| lrp of 2 textures | 1 | 2 | 4,000 | 0.000000000 |
| mad with 2 constants | 2 | 1 | 4,000 | 0.000000000 |
| 3-stage chain | 3 | 1 | 4,000 | 0.000000000 |
| dp3 lighting | 1 | 1 | 4,000 | 0.000000000 |
| texture × vertex colour | 1 | 1 | 4,000 | 0.000000000 |
| **total** | | | **28,000** | **0.000000000** |

The blit fixture has exactly the same tokens as the shipped 3-instruction
program (`dcl_texcoord t0.xy`, `dcl_2d s0`, `texld oC0,t0,s0`), so the
identity path is validated against the extracted program's arithmetic, not
just a synthetic shape. `tests/tev_real_smoke.c` additionally translates all
9,621 shipped pixel chunks and reports the acceptance/rejection table in §6;
`tests/tev_translate_smoke.c` exercises the rejection diagnostics (cmp, if,
cube sampler, 4 textures, MRT, non-uniform swizzle, abs modifier, >6 stages,
alpha fixup), plus malformed/out-of-range `texld` operands and non-finite or
out-of-[0,1] `def` literals. The literal regression includes the previous wrong
output: D3D9 `mad(-1,2,2)` is 0, while clamping the literals to RGBA8 first
produced 1; the shader is now rejected.

The translator does **not** claim bit-exact PICA output: TEV quantises stage
outputs and constants to 8 bits and clamps every stage. The accuracy figure is
the exactness of the *algebraic mapping* on LDR inputs and RGBA8 constants;
hardware 8-bit rounding is a separate, expected error of at most 1 LSB per
stage.

### 7.4 Pre-specialised simple-lit logo/menu prototype

`source/d3d9_menu_light.c/.h` recognises exactly two **structural** instruction
signatures (no bytecode blob or hash lookup):

| Signature | Retail occurrences | Instructions | Normal decode |
|---|---:|---:|---|
| `p5_MAD_NORMAL` | 5 | 24 | `normal.xyz = normal.xzyw * 2 - 1` |
| `p1200005_ADD_NORMAL` | 1 | 25 | `normal.xyz = (normal.xzyw - 0.5) * 2` |

Both require `texcoord0 v0`, `color0 v1`, `color1 v2`, `2d s0`, `2d s1`, the
same diffuse/normal samples, three `dp4` basis-row transforms, saturated `dp3`
directional lighting, emissive add, vertex-colour multiply, secondary-colour
blend and final `c7.a` multiply. A changed opcode, register, swizzle, mask,
source modifier, sampler or `def c8` literal is rejected with a named mismatch.
`tests/tev_real_smoke.c` measured **5 + 1 = 6** matching chunks in the 9,621 PS
chunks.

The exact CPU evaluator follows those instructions. `tests/menu_light_smoke.c`
assembles both signatures from semantic fields and compares against an
independent D3D9 token interpreter over 2,000 random LDR vectors per signature:

| Evaluator | Vectors | Max abs error |
|---|---:|---:|
| exact `p5_MAD_NORMAL` | 2,000 | 0.000000000 |
| exact `p1200005_ADD_NORMAL` | 2,000 | 0.000000000 |
| five-stage pre-baked plan | 4,000 | 0.000000000 |

The hardware-oriented prototype is a **five-stage, two-texture TEV plan**:

1. `dot3(pre-baked normal, c3)` -> saturated NdotL;
2. `NdotL*c4 + (ambient+emissive)` varying;
3. `diffuse*light`, alpha `diffuse.a*vertex.a`;
4. RGB `*vertex.rgb`, alpha `*c4.a` (material alpha);
5. secondary RGB `*c7.a`, with alpha passthrough (no `c7.rgb` interpolation).

The final stage is `MODULATE(PREVIOUS, CONSTANT.a)` for RGB and `REPLACE` for
alpha, matching the retail evaluator's final multiply. The regression fixture
sets `c7.rgb` to unrelated values so an accidental return to fog interpolation
is detected.

Preconditions are explicit: the normal texture is pre-decoded/pre-swizzled and
its tangent-basis transform is asset/vertex work; the second colour varying
carries `ambient+emissive`; `c4.a` carries material alpha. The exact retail
`xzyw` swizzle, three `dp4` basis products and multiple live pixel temporaries
are **not** directly expressible by six PICA TEV stages, so this is a
pre-baked menu variant, not a claim that the generic translator accepts the
retail programs. It is deliberately not wired into `CreatePixelShader` as a
silent fallback; those chunks still fail the generic translator with their
named `dp4`/swizzle reason until the material path explicitly selects this
fixed pipeline and satisfies its preconditions.

## 8. Limitations and next steps

* The first family covers 19 of 9,621 shipped PS chunks (0.20 %). It is the
  largest straight-line subset that needs no pre-specialisation; the menu's
  final copy is inside it.
* The logo's exact base-lit PS is now prototyped as a structural matcher,
  exact CPU evaluator and five-stage pre-baked TEV plan (section 7.4). It still needs
  asset-side normal/tangent preparation before the fixed pipeline can be used.
* `BSEffectShaderProperty` (menu fog, most particles) has no shipped bytecode
  at all. It must be a fixed pipeline, not a translation.
* Terrain cannot be single-pass on PICA (13–15 textures); plan two passes or a
  pre-baked blend.
* The family classifier is heuristic on CTAB vocabulary; it is reproducible
  because the vocabulary and rules are in `tools/shaders/packages.py`, but
  names like "sky" and "grass" are semantic labels, not engine identifiers.
* Update.bsa changes the effective chunk set (123 packages / 12,201 chunks).
  All headline counts above use the 122-package base archive to match
  `docs/ASSETS.md`; the report JSON can be regenerated with `--update`.

## 9. Token-walk contract for `CreateVertexShader`

The retail archive has 854 `BSVS` chunks and 9,621 `BSPS` chunks; every one
is `vs_3_0`/`ps_3_0`. The D3D9 instruction-length field (`D3DSP_INSTLENGTH`,
bits 24..27, "extra operand count") is authoritative, and the per-opcode table
in `source/d3d9_shader.c` is only a nominal floor/ceiling.

A source operand with `D3DSP_ADDRESSMODE` (bit 13) set is followed by one
address-register token (`a0`/`aL`), and **both** tokens count in the length
field. fxc emits this for relative-addressed constants:

```
mul o3.xyz, r0,  c10[a0.x]              -> 5 words (nominal 4)
mad r1.xyz, c0[a0.x], v1, c0[a0.x]      -> 7 words (nominal 5)
```

Before this layer the walker compared the raw token count with the nominal
width and called the surplus "bad length"; a read-only scan of the archive
shows that rule rejects **320 of the 854** retail VS chunks (all 854 VS and
9,621 PS chunks validate after counting operands). The census shows the
consequence: of its 320 `CreateVertexShader` `D3DERR_NOTAVAILABLE` calls, **166
printed `menu.generic.vs: malformed vs token stream`** — this parser bug — while
149 named `input normal0 is outside the static menu family`, 4 named opcode
`0x23`, and 1 named `color1`. The latter 154 are valid streams refused by the
menu family gate, a separate bug in the skip probe (see below).
`def`/`defi`/`defb` payloads are literal data and are excluded from the
relative scan, and a relative source without its address token still fails
(the nominal operand count is then short).

Creation status for a valid stream is a separate decision from parsing:

| stream | normal run | `TESV_D3D9_SHADER_SKIP_PROBE=1` |
|---|---|---|
| not a D3D9 token stream (bad version/framing/unknown opcode) | `D3DERR_INVALIDCALL` | `D3DERR_INVALIDCALL` |
| valid, inside the executable closure | `S_OK` | `S_OK` |
| valid, outside the closure (any retail SM3 shader) | named `D3DERR_NOTAVAILABLE` | `S_OK`, non-executable object; `GetFunction` returns the real tokens and every draw is refused by name |

The probe no longer stops at the closure certifier: streams that *look* like
`menu.generic.vs` but fail its runtime family check (missing color0/color1,
`normal0`/`blendweight0` input, opcode `abs` at 0x23) now also become
non-executable objects instead of `D3DERR_NOTAVAILABLE`. The probe is a
measurement flag only; DevCaps stay `0x00082000` and VS/PS caps stay 2.0.

`CreateVertexDeclaration` (`source/d3d9_vertex.c`) now names the first rejected
element through `d9_decl_init_why()` — stream, type, usage index, offset
overlap/order, POSITION/POSITIONT/COLOR/TEXCOORD component rules, and the
`D3DDECL_END` terminator — instead of returning a bare `D3DERR_INVALIDCALL`.
The measured census had 8 refusals among 368 declarations: **4 with COLOR0 at
element 1 and 4 with COLOR0 at element 3, all `D3DDECLTYPE_FLOAT16_4` (16)**.
The old colour whitelist (COLOR/UBYTE4/UBYTE4N/FLOAT4) omitted the half-float
RGBA form even though `d9_decl_decode()` already decodes it; admitting
FLOAT16_4 fixes the false `D3DERR_INVALIDCALL` without widening the tint
contract to 3-component colours. Other admission rules are unchanged.



