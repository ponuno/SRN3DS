# ASSETS — measured Skyrim 2011 asset data and the conversion pipeline

Owner: asset-analysis agent. This file is the only `docs/` file this work owns.
All numbers were measured in this workspace on **2026-11-24** against the
read-only install at `E:\SteamLibrary\steamapps\common\Skyrim\Data`. Numbers are
measured unless a passage explicitly presents a projection or an assumption.

Hard constraints taken from `docs/MEMORY_BUDGET.md`:

* VRAM is **6 MiB** total and is the only pool PICA can sample.
* The port budgets **4 MiB of textures** (768 KiB reserve; framebuffers/depth
  take ~0.9 MiB), so textures are **ETC1 (4 bpp)** or **ETC1A4 (8 bpp)**.
  BC1/BC2/BC3/BC4/BC5 cannot be sampled directly.
* Geometry lives in the **linear heap**, configured at **8–20 MiB**.
* The SD card is storage; a cache miss is a file read, not free memory.

Everything below is derived under `E:\Skyrim_N3DS_build\assets`. No game bytes
are stored in the repository, and the game install is opened read-only.

## 1. What was added

New Python package (read-only analysis/conversion prototype):

| File | Purpose |
|---|---|
| `tools/skyrim/bsa.py` | BSA v104/v105 reader: header, folders, records, embedded names, deflate |
| `tools/skyrim/dds.py` | DDS header parser + BC1/BC2/BC3/BC4/BC5 → RGBA8 decoders |
| `tools/skyrim/convert.py` | DDS → PNG and the documented `tex3ds` ETC1 hand-off |
| `tools/skyrim/nif.py` | Skyrim NIF header/block table + `NiTriShapeData` counts |
| `tools/skyrim/hkx.py` | Havok packfile detection/classification only |
| `tools/skyrim/esm.py` | Minimal TES4/ESM walker for real per-cell file lists |
| `tools/skyrim/budget.py` | LRU cache and 4 MiB / 8–20 MiB working-set models |
| `tools/skyrim/shaders.py` | `.fxp` shader package inventory and D3D9 tokenizer |
| `tools/skyrim/report.py` | CLI that produced every measured number here |
| `tests/skyrim_assets_test.py` | Pure-Python synthetic fixtures + optional real checks |

Run the reports:

```powershell
.venv\Scripts\python.exe -m skyrim.report all
# or one part:
.venv\Scripts\python.exe -m skyrim.report textures
.venv\Scripts\python.exe -m skyrim.report cell --cell Riverwood01
.venv\Scripts\python.exe -m skyrim.report shaders
```

The CLI needs `PYTHONPATH=tools` (the test inserts it automatically). Outputs
go to `E:\Skyrim_N3DS_build\assets\{archives,textures,meshes,budget,shaders}`.

## 2. Archive inventory (measured)

All 13 shipped archives are **BSA version 104**, header offset 36, together
**176,811 files / 8.16 GiB**. The reader implements version 105 too (same
layout), but the 2011 install contains no v105 file, so the v105 path is
**unverified against real data**.

| Archive | Files | Folders | Archive size | Stored file bytes | Flags | Compressed | Embedded names |
|---|---:|---:|---:|---:|---|---|---|
| `Skyrim - Textures.bsa` | 18,670 | 406 | 1,388.9 MB | 1,323.9 MiB | `0x0107` | yes | yes |
| `Skyrim - Meshes.bsa` | 23,424 | 818 | 1,520.9 MB | 1,449.6 MiB | `0x0087` | yes | no |
| `Skyrim - Shaders.bsa` | 122 | 2 | 25.7 MB | 24.5 MiB | `0x0003` | no | no |
| `Skyrim - Sounds.bsa` | 4,917 | 1,395 | 953.8 MB | 909.3 MiB | `0x0013` | no | no |
| `Skyrim - Animations.bsa` | 7,366 | 1,185 | 42.4 MB | 40.1 MiB | `0x0007` | yes | no |
| `Skyrim - Interface.bsa` | 197 | 7 | 34.7 MB | 33.1 MiB | `0x0003` | no | no |
| `Skyrim - Misc.bsa` | 16,434 | 2 | 177.5 MB | 168.7 MiB | `0x0013` | no | no |
| `Skyrim - Voices.bsa` | 61,211 | 103 | 1,458.6 MB | 1,388.0 MiB | `0x0003` | no | no |
| `Skyrim - VoicesExtra.bsa` | 438 | 1 | 13.0 MB | 12.4 MiB | `0x0003` | no | no |
| `Update.bsa` | 255 | 35 | 17.4 MB | 16.6 MiB | `0x0003` | no | no |
| `Dawnguard.bsa` | 14,704 | 655 | 1,187.5 MB | 1,131.8 MiB | `0x0093` | no | no |
| `HearthFires.bsa` | 2,328 | 90 | 140.3 MB | 133.6 MiB | `0x0093` | no | no |
| `Dragonborn.bsa` | 26,745 | 755 | 1,803.1 MB | 1,718.3 MiB | `0x0093` | no | no |

Key extension counts:

* Textures: **18,580 `.dds`** + 90 `.png` (the `.png` files are UI/source data,
  not texture-set members).
* Meshes: 17,216 `.nif`, 4,416 `.btr`, 834 `.bto`, 569 `.tri`, 353 `.btt`.
* Animations: 6,145 `.hkx`; Shaders: **122 `.fxp` and nothing else**.
* DLC archives add 1,751/425/2,147 NIFs and 3,122/143/9,241 DDSs
  (Dawnguard/HearthFires/Dragonborn).

### Verified BSA layout

The reader was validated against the real archives: exact name/record counts,
absolute offsets, decompression of known DDS/NIF/HKX files, and the embedded
name of every file checked in the real-data test.

```
header (36) | folder records (16*F) |
for each folder: bstring name (u8 len **includes the NUL**) + file records (16*n) |
file-name block (NUL-terminated base names, one per record in order) |
file data (file-record offsets are absolute)
```

Details that matter and are easy to get wrong:

* Folder names are interleaved with their file records in the shipped files;
  there are not two separate name/record blocks. `FolderRecord.offset` is not
  needed (and does not map to a simple block base); `count` is authoritative.
* Archive flag `0x100` (**embed file names**) prefixes each stored file with
  `u8 name_len + name`. The length **excludes** the NUL and there is no NUL,
  unlike folder-name bstrings. The stored `size` includes this prefix.
* Compression is archive-level (`0x004`). Compressed data is
  `u32 unpacked_size + zlib stream`. The high bit (`0x40000000`) of a file
  offset is **not** a per-file compression flag in these archives:
  `Textures.bsa` is 1.39 GB, so thousands of legitimate offsets have bit 30 set.
* `Skyrim - Textures.bsa` is fully compressed and fully name-embedded. The
  directory block and embedded prefixes agree for every file read during this
  work; the reader validates the prefix against the directory entry on each
  read.
* Empty files are handled: an uncompressed empty file can be stored with
  `size = 0` (or a 4-byte payload in the observed archives), and a compressed
  empty file is `u32 0 + zlib stream`. The real test reads a known DDS/NIF/HKX
  from each relevant archive.

## 3. Textures: what is actually shipped

Measured over all **18,580 DDS** files in `Skyrim - Textures.bsa` (headers only;
no pixel decode except the conversion demo).

### 3.1 Format and size distribution

| Format | Files | BC bytes | ETC1 bytes | Alpha needed |
|---|---:|---:|---:|---|
| DXT1 / BC1 | 15,379 | 1,066.6 MiB | 1,066.6 MiB | no |
| DXT5 / BC3 | 2,517 | 921.3 MiB | 460.6 MiB | yes |
| DXT3 / BC2 | 493 | 226.0 MiB | 113.0 MiB | yes |
| BGR24 (uncompressed) | 189 | 35.4 MiB | 5.9 MiB | no |
| BGRA8 | 1 | 21.3 KiB | 2.7 KiB | no |
| R8 | 1 | 21.3 KiB | 10.7 KiB | no |
| **Total** | **18,580** | **2,249.3 MiB** | **1,646.1 MiB** | |

* ETC1 / BC size ratio: **0.732**. This is almost entirely format mix: BC1 is
  already 4 bpp, so it is 1:1; BC3/BC2 are 8 bpp, so ETC1 halves them.
* If alpha-bearing formats are encoded as **ETC1A4** (8 bpp) rather than
  dropping alpha, the mixed total is **2,219.8 MiB, ratio 0.987**. In other
  words, preserving alpha saves almost nothing; the budget question is
  *surface count*, not bit-rate.
* Median per-texture BC size = median ETC1 size = **43,704 bytes** — exactly a
  256×256 full mip chain. Per-file source sizes are therefore not a good proxy;
  headers/dimensions are.

Dimensions (top 8 by count):

| Dimensions | Files | ETC1 all mips |
|---|---:|---:|
| 256×256 | 13,918 | 578.1 MiB |
| 512×512 | 2,789 | 464.7 MiB |
| 1024×1024 | 612 | — |
| 128×128 | 373 | — |
| 512×256 | 164 | — |
| 256×512 | 162 | — |
| 256×64 | 97 | — |
| 64×64 | 61 | — |

Large surfaces are rare but expensive: 43 files are 2048×2048, 4 are
2048×1024, and one is 4096×1024. Mip counts are dominated by 9 levels
(13,911 files), then 10 (3,165), 11 (684), 12 (58); 203 files have 1 level.

### 3.2 Texture sets and the 4 MiB VRAM budget

A "texture set" here is a folder + stem after stripping common material
suffixes (`_n`, `_s`, `_e`, `_msn`, `_sk`, `_m`, ...). This merges albedo /
normal / specular / glow maps and LOD variants. It is a deliberately coarse
grouping; the ranking is dominated by character/creature 2K maps.

| Ranking | Sets | BC bytes | ETC1 bytes | ETC1 MiB |
|---|---:|---:|---:|---:|
| Top 10 | 10 | 85.42 MiB | 78,381,456 | 74.75 |
| Top 25 | 25 | 152.25 MiB | 123,995,040 | 118.25 |
| Top 50 | 50 | 221.24 MiB | 162,602,160 | 155.07 |

The largest single set is `textures\actors\alduin\alduin` (3 files,
2048×2048): BC 13.33 MiB → ETC1 8.00 MiB, twice the entire texture budget.
A single 4096×1024 DXT5 texture (`skyhavenremanswall02_n.dds`) needs
2.67 MiB ETC1.

**What 4 MiB buys** (4,194,304 bytes; ETC1 = 8 bytes per 4×4 block):

| Surface | Top mip only | Full mip chain |
|---|---:|---:|
| 256×256 | 128 | 95 (43,704 B each) |
| 512×512 | 32 (131,072 B) | **23** (174,776 B) |
| 1024×1024 | 8 (524,288 B) | 5 (699,064 B) |
| 2048×2048 | 2 (2,097,152 B) | 1 (2,796,120 B) |

The `MEMORY_BUDGET.md` remark "~8 megapixels, about 64 512×512 surfaces" is a
factor-of-two slip: 4 MiB at 4 bpp is 8.39 megapixels, which is **32** 512×512
top-level surfaces (or 23 with full mips), not 64. The 6 MiB total VRAM would
be 12.58 megapixels / ~48 top-level surfaces, before framebuffers.

## 4. DDS → PNG → ETC1 pipeline (no ETC1 encoder written)

`tools/skyrim/convert.py` implements:

1. `dds.parse_dds` / `dds.decode_level` → tightly packed top-down RGBA8.
2. `convert.write_png` → 8-bit RGBA PNG (zlib; no colour management).
3. `convert.tex3ds_convert` → delegated to devkitPro **tex3ds v2.3.0**
   (`C:\devkitPro\tools\bin\tex3ds.exe`) with `-f etc1` / `-f etc1a4`.

Measured demo (derived PNG/`.t3x` under
`E:\Skyrim_N3DS_build\assets\textures\converted`; `-z none`, no generated mips,
so the numbers are surface bytes plus a small container header):

| Texture | Source | Size | BC top | ETC1 top | T3X | T3X / BC |
|---|---|---:|---:|---:|---:|---:|
| `textures\actors\fox\fox.dds` | DXT1 512² | 131,072 | 131,072 | 131,072 | 131,093 | 1.0002 |
| `textures\clothes\psiijic\psiijicrobes.dds` | DXT1 1024² | 524,288 | 524,288 | 524,288 | 524,309 | 1.0000 |
| `textures\actors\fox\fox_n.dds` | DXT5 512² | 262,144 | 262,144 | 131,072 | 262,165 | 1.0001 (ETC1A4) |
| `textures\actors\dog\dog.dds` | DXT5 1024² | 1,048,576 | 1,048,576 | 524,288 | 1,048,597 | 1.0000 (ETC1A4) |

Interpretation:

* ETC1 vs BC1 is **1:1** (both 4 bpp); tex3ds adds 21 bytes per single-texture
  package.
* ETC1 vs BC3 is **0.5×** only if alpha is discarded. With alpha preserved,
  ETC1A4 matches BC3 byte-for-byte. The measured demo deliberately uses
  ETC1A4 for DXT5, so its T3X/BC is 1.0.
* For shipping sizes, `tex3ds -z lz11` may compress the `.t3x` on the SD card,
  but it does **not** reduce VRAM: the PICA still samples the decompressed
  ETC1 surface.
* There is intentionally **no from-scratch ETC1 encoder** in this tree. The
  production mip/alpha/bounded-memory pipeline and its measured quality limits
  are in `docs/TEXTURES.md`; the numbers above are the earlier top-mip demo.

## 5. Geometry / NIF

### 5.1 Format work

`nif.py` parses the Skyrim 20.2.0.7 header and block table. The layout was
verified against 400 real NIFs with exact byte accounting (every file's block
sizes + header + 8-byte trailer equal the file length):

```
39-byte "Gamebryo File Format, Version 20.2.0.7\n"
u32 version (0x14020007), u8 endian=1, u32 userVersion=12,
u32 numBlocks, u32 bsVersion=83,
bstring author/process/export (u8 len including NUL),
u16 numBlockTypes, (u32 len + chars)*,
u16 blockTypeIndex[numBlocks], u32 blockSize[numBlocks],
u32 numStrings, u32 maxStringLength, (u32 len + chars)*,
u32 reserved (=0),
blocks..., u32 numRoots (=1), u32 rootIndex (=0)
```

This differs from several public descriptions: the BS header sits before the
block-type table and the file ends with an 8-byte root list. The exact-size
check and the decoded block contents (e.g. `NiTriShape` data refs) agree with
the observed layout.

Implemented:

* block-type histogram for **every** NIF;
* `NiTriShape`/`BSLODTriShape`/`BSSegmentedTriShape` → `NiTriShapeData`
  resolution and exact `numVertices` / `numTriangles` extraction (the walk
  consumes the block exactly; 891/891 geometry blocks validated during format
  work);
* a printable-string scan for `textures\...dds` paths.

Not implemented (reported, not guessed):

* full vertex/index/skinning decode, collision (`bhk*`), animation, particles,
  material/shader semantics;
* `NiTriStripsData` strip walks (79 shapes in the 400-NIF sample);
* `BSTriShape` / `BSSubIndexTriShape` (Skyrim SE/Fallout 4 only; absent from
  the LE archive);
* 2 shapes in the sample had a geometry ref that did not resolve to
  `NiTriShapeData`; they are counted as unresolved rather than approximated.

### 5.2 Measured geometry

| Measurement | Value |
|---|---:|
| NIFs in `Skyrim - Meshes.bsa` | 17,216 |
| NIFs sampled (first 400 in archive order) | 400 |
| vertices decoded | 526,663 |
| triangles decoded | 590,567 |
| uncompressed NIF payload (sample) | 51.2 MiB |
| estimated GPU geometry (24 B/vertex + 3×2 B/triangle) | **15.43 MiB** |

Per-NIF geometry is not negligible: the sample averages ~39 KiB at 24 B/vertex
(≈1,317 vertices and ≈1,476 triangles per NIF). Top block types in the sample:
`NiNode` 2,724; `BSLightingShaderProperty` 1,104; `NiTriShapeData` 1,073;
`NiTriShape` 1,022; `BSShaderTextureSet` 993; `NiAlphaProperty` 383;
`NiSkinData`/`NiSkinPartition` 300 each; `BSDismemberSkinInstance` 290;
`BSFadeNode` 272; `bhkRigidBody` 233; `BSXFlags` 224.

The 24 B/vertex figure is the FNV port's sizing convention (position + normal +
UV, 16-bit indices). Skyrim's actual vertex layouts can be larger (skinning,
tangents, colours); the per-cell numbers below are therefore a **floor** for
the static parts we decode, not a worst case.

## 6. HKX

`hkx.py` classifies only:

* **211/211 sampled HKX files are `packed-binary`** (`tag0=0x57E0E057`,
  `tag1=0x10C0C010`).
* Contents version string: **`hk_2010.2.0-r1`** (all samples).
* Header fields: user tag 0, file version 8, layout rules `0x01000104`,
  **3 sections**, contents section index 2, class-name section index 0.
* No XML HKX and no unknown format appeared in the 211-file sample drawn from
  `Animations.bsa` (6,145 files total) plus Meshes/DLC samples.

A full reader would need: the section table after the 64-byte packfile header
(tag/offset/end per section), the `__classnames__`/`__types__` tables, object
tables, virtual fixups (section-relative, low bit marks arrays), type
descriptors with member offsets/sizes, 16-byte alignment handling, and the
Skyrim layout-rules id to select class layouts. None of that is implemented.

## 7. Per-cell working set and streaming budget

### 7.1 Real cell file lists

`esm.py` derives real lists from `Skyrim.esm` (read-only, 249 MB). It walks
TES4/GRUP records, decompresses compressed records, reads CELL `EDID`/`XCLC`,
REFR `NAME`/`DATA`, and `MODL` on placeable base objects. Two **exterior**
cells in the Tamriel worldspace were used:

| Cell | Grid | REFRs | Unique meshes | Unique textures |
|---|---|---:|---:|---:|
| `Riverwood01` | (4, −11) | 92 | 48 | 86 |
| `Riverwood04` | (5, −12) | 294 | 75 | 80 |

Limitations of the derivation (stated, not hidden): `LAND` terrain, actors
(`ACHR`/NPC bodies), leveled lists, navmesh, scripts, collision and effects
referenced only through other records are **not** included. The mesh list is
the `MODL` set of placed statics and the texture list is the `.dds` strings
found in those NIFs. Geometry is therefore a lower bound for a fully rendered
cell; the texture list may include unused material slots, so treat it as the
NIF-declared set rather than a draw-call trace.

### 7.2 Measured working set

| Metric | Riverwood01 | Riverwood04 |
|---|---:|---:|
| REFRs | 92 | 294 |
| unique meshes | 48 | 75 |
| decoded vertices | 55,715 | 75,338 |
| decoded triangles | 44,416 | 58,350 |
| **geometry at 24 B/v + 6 B/tri (all decoded, incl. BSLOD variants)** | **1.529 MiB** | **2.058 MiB** |
| base geometry (no `BSLODTriShape`) | 0.843 MiB | 1.242 MiB |
| LOD-only geometry | 0.686 MiB | 0.816 MiB |
| uncompressed NIF payload | 4.38 MiB | 5.95 MiB |
| unique textures | 86 | 80 |
| BC bytes (as shipped) | 25.70 MiB | 26.95 MiB |
| **ETC1 bytes (all mips, alpha dropped)** | **16.26 MiB** | **17.96 MiB** |
| **ETC1/A4 mix (alpha preserved)** | **25.70 MiB** | **26.95 MiB** |
| fits 4 MiB texture budget? | **no** | **no** |
| geometry fits 8/12/16/20 MiB? | yes / yes / yes / yes | yes / yes / yes / yes |

The cell texture mixes are **DXT5-heavy** (Riverwood01: 48 DXT5, 30 DXT1,
8 DXT3), so preserving alpha costs ~2× ETC1 and still exceeds 4 MiB by 6×.

### 7.3 LRU simulation (bounded caches)

`budget.simulate_lru` was run with a 4 MiB texture cache and a 16 MiB geometry
cache over the request stream (one mesh request per REFR, then that mesh's
textures):

| Simulation | Riverwood01 | Riverwood04 |
|---|---:|---:|
| texture cache: unique-set pass | 86 requests, 0 hits, 71 evictions, peak 3.99 MiB | 80 requests, 0 hits, 63 evictions, peak 3.92 MiB |
| residents after unique pass | 15 / 86 textures | 17 / 80 textures |
| texture request stream | 324 requests, 177 hits (54.6%), 147 misses, 35.3 MiB loaded, 129 evictions | 881 requests, 556 hits (63.1%), 325 misses, 88.4 MiB loaded, 312 evictions |
| geometry request stream (16 MiB) | 83 requests, 35 hits (42.2%), 48 misses, 1.53 MiB loaded, 0 evictions | 260 requests, 185 hits (71.2%), 75 misses, 2.06 MiB loaded, 0 evictions |

### 7.4 Conclusion

* **Geometry: plausible, but LOD choices matter.** One exterior cell needs
  **0.84–1.24 MiB** of base geometry (no `BSLODTriShape`) and **1.53–2.06 MiB**
  if every `BSLODTriShape` LOD variant is kept. A 5-cell ring is therefore
  ~4.2–6.2 MiB base or ~7.7–10.3 MiB with all LODs: the base set fits even an
  8 MiB heap, while the all-LOD set wants 16 MiB. The caveat is that Skyrim's
  real vertex format is richer than 24 B/vertex; budget 2–3× for
  position+normal+tangent+UV+skin and still 16–20 MiB is reasonable for a few
  cells, not for the whole world. A streaming loader should drop unused
  `BSLODTriShape` variants or load one LOD per shape.
* **Textures: 4 MiB does not hold one cell's unique set.** The cell's unique
  texture set is 16–18 MiB as ETC1 (26–27 MiB with alpha). A 4 MiB LRU cache
  holds only 15–17 of 80–86 textures; the request stream thrashes (63–312
  evictions) and loads 35–88 MiB per pass.
* Therefore the renderer must either (a) stream and accept eviction (the LRU
  numbers above are the shape of that cost), or (b) reduce texture resolution
  aggressively (e.g. 256² for most, 512² for hero assets, mips only where
  visible), or (c) both. A 512×512 ETC1 surface with full mips is 174,776 B;
  4 MiB is only 23 of them. The shipped median texture is exactly one 256²
  full chain (43,704 B), so even a 4 MiB cache holds at most 95 median
  textures — a cell has 80–86 unique textures plus the rest of the visible
  world.
* The 4 MiB budget and the 8–20 MiB geometry budget are **individually
  plausible for streaming**, but not simultaneously sufficient to make one
  exterior cell resident without resolution reduction. This matches the
  `MEMORY_BUDGET.md` warning that the GPU budget must be measured per scene.

## 8. Shader inventory

### 8.1 Files: 122 `.fxp`, no readable sources

`Skyrim - Shaders.bsa` (25,687,502 bytes, 122 files) contains **only `.fxp`
shader packages**. There are **no `.hlsl`, `.fx`, `.fxh`, `.vsh`, `.psh`,
`.vs`, `.ps`, `.glsl`, or `.cg` files** in any of the 13 archives, and no
source-like markers (`float4`, `technique`, `sampler2D`, ...) were found inside
the `.fxp` payloads. The "human-readable sources" part of the request therefore
has an empty table: **0 source files, 0 techniques/passes declared in source,
0 source entry points.** The retail game ships precompiled bytecode, not HLSL.

Counts by extension: `fxp` 122 (100 %). By directory:
`shadersfx` 2, `shadersfx\lighting\059` 120. Size range: 954 B minimum,
214,580 B median, 1,766,270 B maximum.

### 8.2 Package and chunk format

Each `.fxp` is a Bethesda shader package containing D3D9 bytecode chunks:

```
chunk := b"BSVS" | b"BSPS"   (vertex / pixel)
         u32 meta
         u32 size            (bytes of the following D3D9 program)
         bytecode[size]
bytecode := u32 version_token
            (comments/CTAB and instruction tokens)
            u32 0xFFFF (END)
```

The version token is `0xFFFE0300` for vs_3_0 and `0xFFFF0300` for ps_3_0
(major in bits 8..15, minor in bits 0..7). Comments use the D3D9 comment token
`0xFFFE` with the comment length in bits 16..30 (DWORDs), followed immediately
by the data; the first comment is normally `CTAB` (constant table). Bytecode
chunks carry **no entry-point symbol** (the CTAB comment lists constant names,
not functions), so entry-point names cannot be recovered from the retail data.
The parser validated **10,475/10,475 chunks** with zero tokenizer errors and
exact consumption of each declared chunk size.

### 8.3 Compiled shader counts and targets

| Metric | Value |
|---|---:|
| bytecode chunks | **10,475** |
| vertex / pixel chunks | 854 / 9,621 |
| targets | **vs_3_0 854, ps_3_0 9,621** |
| SM2 (`vs_2_0`/`ps_2_0`) chunks | **0** |
| distinct bytecode blobs (SHA-1) | **6,659** |
| distinct instruction programs (ignoring CTAB comments) | **6,455** |
| texture-sample instructions (`texld`/`texldd`/`texldl`) | 46,158 |
| chunks with branching | 3,868 (36.9 %) |
| chunks with loops | 3,867 (36.9 %) |
| chunks with MRT writes (pixel shader `oC1`+) | **0** |
| parser errors | 0 |

**The number the port needs:** of 10,475 compiled chunks, **7 chunks (0.07 %)
use only the current `dcl`/`mov`/`m4x4`/`texld` opcode subset**, and they are
only **3 distinct programs** (3 of 6,455 = 0.046 %; all trivial 3-instruction
`ps_3_0` `dcl,dcl,texld` blit shaders; 7 occurrences). With `nop`/`def`
admitted the count is 16 chunks (0.15 %). **Zero shipped shaders are SM2**, so
the port's two exact SM2 templates match **0 %** of Skyrim's compiled shaders.

Top opcodes across all chunks:

| Opcode | Count | Opcode | Count |
|---|---:|---|---:|
| `mul` | 222,954 | `cmp` | 71,231 |
| `mad` | 153,018 | `texld` | 46,140 |
| `dcl` | 130,180 | `rcp` | 39,411 |
| `dp3` | 101,891 | `dp4` | 30,991 |
| `add` | 80,267 | `rsq` | 21,934 |
| `mov` | 72,822 | `nrm` | 21,318 |
| `def` | 20,586 | `pow` | 12,561 |
| `lrp` | 8,504 | `rep`/`endrep` | 3,869 each |
| `defi` | 3,867 | `ifc`/`endif` | 2,314 each |

Full per-chunk rows (10,475) are in
`E:\Skyrim_N3DS_build\assets\shaders\shader_chunks.json` and
`shader_chunks.csv`; the aggregate is `shader_inventory.json`.

### 8.4 Per-file inventory (all 122 files)

`chunks` = BSVS+BSPS chunks; `VS`/`PS` = kind counts; `texld`, `branch`,
`loop`, `MRT` = chunk counts with at least one such feature. All chunks in
every file are `vs_3_0` or `ps_3_0`; there are no other targets. MRT is **0 in
every file**: 18,440 pixel-shader instructions write `oC0`, and none writes
`oC1` or higher (vertex outputs use register type 6 and are not render
targets).

| File | Bytes | chunks | VS | PS | texld | branch | loop | MRT |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `imagespace002.fxp` | 81872 | 120 | 60 | 60 | 400 | 4 | 3 | 0 |
| `059/p1.fxp` | 159844 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/p1000001.fxp` | 259684 | 80 | 0 | 80 | 1028 | 0 | 0 | 0 |
| `059/p10001.fxp` | 182516 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p1001.fxp` | 176828 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p10401.fxp` | 193172 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p10801.fxp` | 268700 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p10c01.fxp` | 237012 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p1200005.fxp` | 954 | 1 | 0 | 1 | 2 | 0 | 0 | 0 |
| `059/p12001.fxp` | 211276 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p12401.fxp` | 209332 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p1280005.fxp` | 2538 | 1 | 0 | 1 | 8 | 0 | 0 | 0 |
| `059/p1401.fxp` | 197652 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p1600001.fxp` | 230284 | 80 | 0 | 80 | 548 | 0 | 0 | 0 |
| `059/p1600401.fxp` | 242908 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p1600801.fxp` | 317996 | 80 | 0 | 80 | 628 | 0 | 0 | 0 |
| `059/p1600c01.fxp` | 288100 | 80 | 0 | 80 | 628 | 60 | 60 | 0 |
| `059/p1601001.fxp` | 244028 | 80 | 0 | 80 | 628 | 60 | 60 | 0 |
| `059/p1601401.fxp` | 267428 | 80 | 0 | 80 | 628 | 60 | 60 | 0 |
| `059/p1602001.fxp` | 259004 | 80 | 0 | 80 | 628 | 0 | 0 | 0 |
| `059/p1602401.fxp` | 259028 | 80 | 0 | 80 | 628 | 60 | 60 | 0 |
| `059/p1603001.fxp` | 260068 | 80 | 0 | 80 | 708 | 60 | 60 | 0 |
| `059/p1603401.fxp` | 283468 | 80 | 0 | 80 | 708 | 60 | 60 | 0 |
| `059/p1800001.fxp` | 158564 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/p1800401.fxp` | 169268 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `059/p1800801.fxp` | 244780 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p1800c01.fxp` | 213540 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p1801.fxp` | 209284 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p1810801.fxp` | 267100 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p1810c01.fxp` | 235412 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p1a00001.fxp` | 1022 | 1 | 0 | 1 | 2 | 0 | 0 | 0 |
| `059/p1a10001.fxp` | 1306 | 1 | 0 | 1 | 3 | 0 | 0 | 0 |
| `059/p1a80001.fxp` | 1638 | 1 | 0 | 1 | 5 | 0 | 0 | 0 |
| `059/p1a90001.fxp` | 2134 | 1 | 0 | 1 | 6 | 0 | 0 | 0 |
| `059/p1c01.fxp` | 230044 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p1c10001.fxp` | 284612 | 80 | 0 | 80 | 548 | 0 | 0 | 0 |
| `059/p1c10401.fxp` | 302724 | 80 | 0 | 80 | 548 | 0 | 0 | 0 |
| `059/p1e00001.fxp` | 1022 | 1 | 0 | 1 | 2 | 0 | 0 | 0 |
| `059/p1e10001.fxp` | 1338 | 1 | 0 | 1 | 3 | 0 | 0 | 0 |
| `059/p1e80001.fxp` | 2022 | 1 | 0 | 1 | 5 | 0 | 0 | 0 |
| `059/p1e90001.fxp` | 2334 | 1 | 0 | 1 | 6 | 0 | 0 | 0 |
| `059/p2000001.fxp` | 188004 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p200001.fxp` | 186404 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p20001.fxp` | 159844 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/p2000801.fxp` | 276124 | 80 | 0 | 80 | 468 | 0 | 0 | 0 |
| `059/p2000c01.fxp` | 246340 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p2001.fxp` | 188604 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p200401.fxp` | 199028 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p200801.fxp` | 272604 | 80 | 0 | 80 | 468 | 0 | 0 | 0 |
| `059/p200c01.fxp` | 243284 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p201001.fxp` | 201148 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p201401.fxp` | 224532 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p201801.fxp` | 233972 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p201c01.fxp` | 256636 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p202001.fxp` | 215164 | 80 | 0 | 80 | 468 | 0 | 0 | 0 |
| `059/p202401.fxp` | 215188 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p203001.fxp` | 217188 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p203401.fxp` | 240572 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p20401.fxp` | 194108 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `059/p20801.fxp` | 246060 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p20c01.fxp` | 237420 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p210001.fxp` | 209076 | 80 | 0 | 80 | 468 | 0 | 0 | 0 |
| `059/p210401.fxp` | 221652 | 80 | 0 | 80 | 468 | 60 | 60 | 0 |
| `059/p21801.fxp` | 209284 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p21c01.fxp` | 252644 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p220001.fxp` | 186404 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p220401.fxp` | 222588 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p223001.fxp` | 217188 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p223401.fxp` | 263772 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p23001.fxp` | 192868 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p23401.fxp` | 236908 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p2400005.fxp` | 1270 | 1 | 0 | 1 | 3 | 0 | 0 | 0 |
| `059/p2401.fxp` | 186708 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p2480005.fxp` | 2734 | 1 | 0 | 1 | 9 | 0 | 0 | 0 |
| `059/p2600001.fxp` | 292644 | 80 | 0 | 80 | 1188 | 0 | 0 | 0 |
| `059/p3001.fxp` | 192868 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p30801.fxp` | 268700 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p30c01.fxp` | 259612 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p3401.fxp` | 213692 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p3801.fxp` | 223916 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p3c01.fxp` | 245428 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p400001.fxp` | 166244 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p400401.fxp` | 176948 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p400801.fxp` | 252460 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p400c01.fxp` | 221220 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p401.fxp` | 170548 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `059/p401001.fxp` | 183228 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p401401.fxp` | 204052 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p402001.fxp` | 195004 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p402401.fxp` | 193108 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p405.fxp` | 169268 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p407.fxp` | 176948 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p410001.fxp` | 188916 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p410401.fxp` | 199572 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p5.fxp` | 152164 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/p600001.fxp` | 173924 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p600401.fxp` | 182676 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/p7.fxp` | 159844 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/p800001.fxp` | 184484 | 80 | 0 | 80 | 388 | 0 | 0 | 0 |
| `059/p800401.fxp` | 195140 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/p800807.fxp` | 270812 | 80 | 0 | 80 | 468 | 0 | 0 | 0 |
| `059/p800c07.fxp` | 244676 | 80 | 0 | 80 | 548 | 60 | 60 | 0 |
| `059/p801.fxp` | 246060 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p805.fxp` | 238364 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/p807.fxp` | 246060 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/pa00001.fxp` | 169444 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/pa00401.fxp` | 181076 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `059/pa00801.fxp` | 257692 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/pa00805.fxp` | 250012 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/pa00807.fxp` | 257692 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/pa00c01.fxp` | 225956 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/pa00c07.fxp` | 232676 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/pc00001.fxp` | 164612 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/pc00401.fxp` | 176532 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `059/pc00801.fxp` | 250652 | 80 | 0 | 80 | 308 | 0 | 0 | 0 |
| `059/pc00c01.fxp` | 220660 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/pc01.fxp` | 214820 | 80 | 0 | 80 | 308 | 60 | 60 | 0 |
| `059/pc05.fxp` | 214340 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/pc07.fxp` | 222020 | 80 | 0 | 80 | 388 | 60 | 60 | 0 |
| `059/pc20001.fxp` | 164612 | 80 | 0 | 80 | 228 | 0 | 0 | 0 |
| `059/pc20401.fxp` | 222700 | 80 | 0 | 80 | 228 | 60 | 60 | 0 |
| `shaders001.fxp` | 1766270 | 1703 | 794 | 909 | 2440 | 24 | 24 | 0 |

(The `059/` prefix means `shadersfx\lighting\059\`; the other two are
`shadersfx\` files.)

### 8.5 Implication for the renderer

Zero shipped shaders are SM2 and only three distinct programs fall inside the
current opcode subset, so the current two-template shader path covers
**none of the shipped material shaders**. The renderer work needs either a real
SM3→PICA translation path, or an asset-side shader pre-bake that emits a small
set of fixed pipelines (albedo×vertex colour, albedo+normal, etc.) and treats
Skyrim's thousands of compiled variants as data-driven state, not as code to
translate.

## 9. Tests and reproducibility

`tests/skyrim_assets_test.py` uses synthetic fixtures for every parser (BSA
v104/v105 × compressed/embed matrix, DDS BC1/BC2/BC3/BC4/BC5/RGBA8/BGR24,
NIF header + geometry, HKX classification, synthetic ESM with compressed
records, LRU and VRAM arithmetic, FXP chunk parsing, PNG writer). Real-data checks run only when
`SKYRIM_N3DS_REAL_DATA` is set (to `1` or to the Data directory).

Measured output on 2026-11-24:

```
skyrim asset tests
  ok: BSA v104/v105 x compressed/embedded matrix
  ok: BSA embedded-name-only archives and clear failure otherwise
  ok: BSA rejects bad magic/version/truncation
  ok: DDS header + BC1/BC2/BC3/BC4/BC5/RGBA8/BGR24 decode + ETC1 sizing
  ok: NIF header, block histogram, NiTriShapeData counts, texture scan
  ok: NIF texture scan is byte-level
  ok: HKX packed-binary/XML/unknown classification
  ok: ESM CELL/REFR/MODL extraction and compressed records
  ok: LRU model, VRAM arithmetic, working-set aggregates
  ok: FXP chunk discovery, D3D9 tokenizer, subset classification
  ok: DDS -> PNG writer
  ok: real Textures.bsa (18670 files) and dog.dds header
  ok: real Meshes.bsa NIF header/geometry
  ok: real Shaders.bsa inventory
  ok: real HKX classification
skyrim assets: all checks passed
```

The test is registered in `tools/run_host_tests.ps1` with the other Python
tests (`$pyTests`). It is pure Python and writes no game bytes into the repo.

## 10. Derived outputs

All under `E:\Skyrim_N3DS_build\assets` (not committed):

* `archives/bsa_statistics.json` — per-archive header/flags/counts/extensions.
* `textures/texture_inventory.json` — DDS histogram, BC/ETC1 totals, top sets,
  VRAM arithmetic, largest textures.
* `textures/textures.json` — one row per DDS (header-derived sizes).
* `textures/conversion_demo.json`, `textures/converted/*.png`, `*.t3x` —
  measured DDS→PNG→ETC1 demo.
* `meshes/nif_summary.json` — block histogram and sampled geometry.
* `meshes/hkx_summary.json` — HKX classification sample.
* `budget/cell_budgets.json` — Riverwood01/Riverwood04 working set, per-asset
  details, LRU results.
* `shaders/shader_inventory.json`, `shader_chunks.json`, `shader_chunks.csv` —
  10,475 chunk rows and the aggregate.

## 11. Explicit limitations

* v105 BSA support is implemented but not exercised (the install is v104).
* DDS decode supports the formats present (BC1/BC2/BC3/BC4/BC5 and a few
  legacy uncompressed masks); cubemaps/arrays/volume textures are rejected.
* NIF geometry extraction is scalar counts only; no vertex buffers, skinning,
  strips, collision or shaders.
* HKX is header/classification only.
* ESM cell derivation is partial: no LAND, actors, leveled lists, scripts,
  effects, or master records other than `Skyrim.esm`. Geometry counts are a
  lower bound; the NIF-declared texture set may include unused slots.
* The 24 B/vertex geometry estimate is a port convention, not Skyrim's actual
  vertex stride; skinned/tangent-heavy meshes will be larger.
* Shader analysis is bytecode metadata only: no semantic translation, no
  technique/pass reconstruction from source because no source ships.
* "Distinct shaders" means distinct bytecode blobs; the instruction-program
  count ignores CTAB constant tables and is the better semantic count.
