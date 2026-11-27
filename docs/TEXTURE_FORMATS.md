# Texture formats needed by the world (retail corpus + installed mods)

Status: **measurement + format support note**, 2026-11-27. No frame is
claimed. `DevCaps` is unchanged (`0x00082000`). R8G8B8, A8B8G8R8, L8,
DX10 BC7 and the 1x1 eye cube are now handled; only direct
`CreateTexture(R8G8B8/A8B8G8R8)` remains source-only, and the measured menu
call sites prove the engine does not use that path.

The question is which DDS pixel formats the bridge must recognise for the
retail game and for what is actually installed, not which formats D3D9 in
general allows. Headers only were read; no pixels were decoded during the
census.

## 1. Method

`tools/texture_formats.py` (new, read-only) walks:

* every texture-bearing BSA in `E:\SteamLibrary\steamapps\common\Skyrim\Data`
  (`Skyrim - Textures`, `Update`, `Dawnguard`, `HearthFires`, `Dragonborn`);
* loose `Data\textures\**`;
* `E:\SkyrimMods\mods\*\textures\**` and
  `E:\Vortex Mods\skyrimse\mods\*\textures\**` (the second root is empty here).

It parses the 128-byte legacy header plus the optional 20-byte DX10 header and
classifies the FourCC / DXGI format, dimensions, mips, cube and volume caps.
Results were written outside the repository to
`E:\Skyrim_N3DS_build\assets\textures\format_census.json`.

```
python tools/texture_formats.py --json E:/Skyrim_N3DS_build/assets/textures/format_census.json
```

## 2. Census result (33,957 DDS, 0 header errors)

| Format | Files | Stored bytes | Where |
| --- | ---: | ---: | --- |
| DXT1 (BC1) | 27,412 | 1,234.0 MiB | retail BSA 27,070; loose 333; UBE 9 |
| DXT5 (BC3) | 5,038 | 1,677.0 MiB | retail BSA 3,208; loose 1,623; mods 207 |
| DXT3 (BC2) | 1,117 | 732.9 MiB | retail BSA 625; loose 330; mods 162 |
| R8G8B8 (24-bit) | 247 | 571.2 MiB | retail 189; UBE 58 |
| BC7_UNORM (DXGI 98) | 120 | 580.4 MiB | UBE 2.0 only |
| A8B8G8R8 (32-bit) | 13 | 196.0 MiB | UBE 2.0 only |
| ATI1/BC4 | 7 | 18.7 MiB | UBE 2.0 only |
| A8R8G8B8 | 2 | 0.5 MiB | one retail `effects\highfrequencynormals.dds`, one UBE brow |
| L8 | 1 | 1.3 MiB | retail volume `effects\noisevolume.dds` |

Cube: 56 resources — 54 DXT1, 1 DXT5, 1 A8B8G8R8 (the last is a 1x1 UBE eye
cube). Volume: exactly one, the retail L8 `noisevolume.dds`.

### What is actually needed

* **DXT1/DXT3/DXT5 cover the retail world.** They are 98.8% of retail DDS
  files and every retail cube. The existing decoder already handles them.
* **BC5/ATI2 is absent from this corpus.** Zero retail and zero installed-mod
  files use ATI2/BC5U. Normal maps ship as DXT5. The decoder was added anyway
  for completeness and for future mod packs, but no shippable claim should say
  "BC5 is exercised by the corpus": only synthetic reference blocks test it.
* **BC4/ATI1 is present (7 UBE tint masks)**; it is the single-channel half of
  BC5 and is now decoded too.
* **BC7 is present only in UBE 2.0 (120 files, eyes/body/head/paint/nails).**
  The DX10 DXGI 98/99 formats are decoded with the vendored MIT/Unlicense
  `bcdec` header; no conversion to ETC1/BC5 is attempted. See §4.
* **Uncompressed 24-bit RGB (247 files) is now supported by the D3DX shim.**
  Retail usage is limited (189 files, all 256x256 1-mip tint masks in
  `Skyrim - Textures.bsa`); every file expands to A8R8G8B8 on upload with
  A=255. See §2a.
* **A8B8G8R8 (13 mod files, UBE 2.0)** is likewise expanded on upload, with
  the B/R bytes swapped from the DDS R,G,B,A memory order. See §2a.
* **The L8 volume** is the only volume format in the corpus. The volume
  loader now sizes L8 at one byte per texel and expands each depth slice and
  mip to A8R8G8B8 (`L,L,L,255`) on upload; the real 128x128x128
  `noisevolume.dds` (8 mips) loads. See §2b.
* **The 1x1 A8B8G8R8 UBE eye cube** is now padded: the loader creates an
  8x8 cube (the backend floor) and replicates each source texel over its
  8/edge block for every level and face, so the constant-colour eye cube
  samples correctly. See §2c.

## 2a. 24-bit and BGR-first DDS loaders (closed)

Verified on the real corpus, not guessed:

* Retail R8G8B8: `khajiitnose01.dds` and `argoniancheeks03.dds` (extracted from
  `Skyrim - Textures.bsa`) have masks R=`0x00ff0000`, G=`0x0000ff00`,
  B=`0x000000ff`, bits=24. Little-endian DDS pixels are therefore **B,G,R**.
  The first nonzero pixel of `argoniancheeks03.dds` is bytes `2d 52 4f`
  (B=45, G=82, R=79), proving the order. Rows are padded to four bytes:
  `pitch = (w*3 + 3) & ~3` (256x256 -> 768; a 17-pixel row would be 52).
* Mod A8B8G8R8: `eye01_outer_m.dds` (16x16, 5 mips) has masks R=`0xff`,
  G=`0xff00`, B=`0xff0000`, A=`0xff000000`; memory bytes are **R,G,B,A** and
  the first row is `ff ff 00 00 ...` (R=255,G=255,B=0,A=0).
* PICA samples RGBA8 (`GPU_RGBA8`, byte order in the tiled store A,B,G,R), so
  both source layouts are expanded to the bridge's A8R8G8B8 CPU layout
  (B,G,R,A) before upload:
  * `d9_rgb24_to_bgra8` copies B,G,R and writes A=255, honouring the DDS row
    pitch including the 4-byte padding.
  * `d9_abgr8_to_bgra8` swaps R and B and keeps G,A.
* Every mip level is converted separately from its own source pitch/offset.
  Cube faces are converted face by face and volume slices slice by slice.
  The created resource and its `GetLevelDesc` report A8R8G8B8, which is what
  the PICA path can actually store; the DDS/image info still reports the file
  format.

`CheckDeviceFormat` deliberately does **not** claim direct storage for these
formats: `CreateTexture(R8G8B8/A8B8G8R8)` is not implemented (the bridge has
no 24-bit/BGR-first guest layout). The answer is
`D3DERR_NOTAVAILABLE` with the reason "expanded to A8R8G8B8 by the D3DX
texture loader". This is not a guess: the deduplicated `CreateTexture`
call-site census in `docs/GFX_COMPAT.md` §3/§4 records only `A8` (0x1c),
`A8L8` (0x33), `A4R4G4B4` (0x1a) and `A8R8G8B8` (0x15) as immediates at the
15 menu call sites; no `R8G8B8` (0x14) or `A8B8G8R8` (0x20) request exists.
Both corpus formats reach the bridge only through the D3DX file loaders.

## 2b. L8 volume (closed)

`level_layout(D3DFMT_L8)` is one byte per texel with `pitch = w`. The D3DX
volume loader already walks mips and depth slices; each slice now goes through
`d9_l8_to_bgra8`, which writes `B=G=R=L, A=255` into the bridge's A8R8G8B8
store (`storage_format` is A8R8G8B8, reported by `GetLevelDesc`). This keeps
the PICA path unchanged: there is no L8 sampler format to plumb through the
BC/RGBA upload code, and D3D9 L8 sampling semantics are exactly the expanded
value.

The real retail file is 128x128x128 with 8 mips: header flags
`0x820007`, `DDPF_LUMINANCE`, bits=8, `R=0xff`, caps2 `0x200000` (volume),
depth 128. `tests/d3dx_volume_texture_smoke.c` creates a synthetic 4x4x2 L8
volume with two mips and asserts the level-0 and level-1 locked texels;
`tests/d3dx_texture_shims_smoke.c` copies the real `noisevolume.dds` into the
sandbox and loads it through `D3DXCreateVolumeTextureFromFileA`, checking
8 levels, 128^3 dimensions, a 512-byte row / 65,536-byte slice pitch and
that the whole first slice is opaque `L,L,L`.

## 2c. 1x1 eye cube (closed by padding)

`create_cube_from_dds` still refuses compressed cubes below 8x8, but an
uncompressed edge of 1, 2 or 4 is padded to 8. The created cube reports
edge 8; for every face and level the source `w x w` level is replicated by
`8/edge` into the destination `side x side` level, so every texel in a face
is the same colour the original cube would sample. The UBE eye cube is 1x1
A8B8G8R8; it is expanded to A8R8G8B8 first, then replicated. Tests cover a
synthetic 1x1 A8B8G8R8 cube (six distinct face colours, all 64 pixels per
face equal) and a 1x1 A8R8G8B8 cube.

Tests: `tests/bc_shader_smoke.c` checks the 24-bit row pitch (17->52, 16->48),
both byte orders, and truncated/undersized input; `tests/d3dx_texture_shims_smoke.c`
loads a 16x16 two-mip R8G8B8 and an 8x8 two-mip A8B8G8R8, verifies
each locked mip against the converter, creates an 8x8 R8G8B8 cube, an 8x8
A8B8G8R8 cube and an 8x8x2 R8G8B8 volume, refuses the 1x1 A8B8G8R8 cube by
name, and (when the samples exist) loads the real 256x256 retail R8G8B8
`khajiitnose01.dds` (69,559 nonzero bytes of 262,144) and the 16x16 UBE
A8B8G8R8 `eye01_outer_m.dds` (512 of 1,024). `tests/check_format_matrix_smoke.c`
asserts the source-only `CheckDeviceFormat` answer.

## 3. BC5 / BC4 decoder

`source/d3d9_bc.c`:

* `d9_bc_block_bytes`: DXT1/ATI1/BC4U = 8, DXT3/DXT5/ATI2/BC5U = 16.
* One shared `bc4_block()` expands the two-mode BC4 palette (8-value mode when
  `a0 > a1`, 6-value + 0/255 mode otherwise). DXT5 alpha now uses it too, so
  the palette has one implementation.
* BC5 decodes channel 0 to R and channel 1 to G, sets **B = 0 and A = 255**
  (the D3D decode rule; normal-map shaders reconstruct Z). BC4 decodes to
  (R, 0, 0, 1).

Integration:

* `d3d9_core.h` defines `D9_ATI1/D9_BC4U/D9_ATI2/D9_BC5U`.
* `d9_texture_layout` and `d9_bc_to_pica` accept them automatically through
  `d9_bc_block_bytes`; PICA tiling is unchanged.
* `d3d9_adapter.c` accepts all four as 2D/cube/volume sampler formats.
* `d3d9_null.c` samples BC4/BC5 cube texels.
* `d3dx_texture_shims.c` maps the legacy FourCCs `ATI1`, `BC4U`, `ATI2`,
  `BC5U`, sizes their levels and reports names `ATI1/BC4`, `BC4U`,
  `ATI2/BC5`, `BC5U`.

Reference blocks in `tests/bc_shader_smoke.c` assert both palette modes,
channel independence, B/A fill, block-size validation and the 8/16-byte
layouts. `tests/d3dx_texture_shims_smoke.c` creates a 16x16 ATI2 DDS and
checks the 16-byte block layout survives `LockRect`.

## 4. BC7: decoded (closed)

BC7 is real in the installed corpus, so it is decoded rather than downgraded:

* `third_party/bcdec/bcdec.h` (v0.985, unmodified, dual MIT/Unlicense,
  copyright Sergii Kudlai) supplies `bcdec_bc7`; see
  `third_party/bcdec/LICENSE` and `README.md`.
* `source/d3d9_bc.c` defines `D9_BC7` (internal id `0x37434244`, no D3D9
  enum), adds it to `d9_bc_block_bytes` as 16 bytes, and routes only BC7
  through `bcdec_bc7`. BC1/2/3/4/5 keep the existing hand-written decoder so
  their byte-exact tests do not change.
* The D3DX DDS parser accepts a DX10 header with DXGI 98 (BC7_UNORM) or 99
  (BC7_UNORM_SRGB), consumes the 20-byte DX10 header (`data_offset=148`), and
  rejects other DXGI formats/array sizes by name. The path export reads 148
  header bytes so image info works for DX10 files too.
* Upload stores the BC7 blocks in the CPU level, then the existing
  `d9_bc_to_pica` path decodes them to PICA-tiled RGBA8 at publication. PICA
  has no BC7 hardware, so no BC7 → ETC1/BC5 conversion is attempted or
  claimed.

Tests: the D3DX shim creates a synthetic 8x8 DX10 BC7 texture, loads the real
UBE `eye01_m.dds` (128x128, 8 mips) through `D3DXCreateTextureFromFileInMemory`
and decodes blocks from the real `eye01.dds` base-colour file. The 120-file
BC7 count remains a census fact, not a claim that all 120 were individually
decoded in CI.

## 5. Float render targets / `R16F`, `A16B16G16R16F`

PICA200 has **no float colour attachment**: the only render-target formats are
RGBA8/RGB8/RGBA4444/RGB565/RGBA5551, and depth is fixed-point. A float target
therefore cannot be supported on the real GPU. `docs/FLOAT_RENDER_TARGETS.md`
is the full evidence note (owner: renderer/shadow-map init; five 1024 R16F
shadow maps, one 512 A16B16G16R16F; 6 MiB VRAM total).

What changed here is only truthfulness:

* `R16F` (0x6f) is now in the renderer-requirement float rule together with
  `A16B16G16R16F` (0x71) and `R32F` (0x72), so `CheckDeviceFormat` returns
  `D3DERR_NOTAVAILABLE` with the named reason instead of a catch-all.
* The adapter's HDR fallback probe (`TESV_D3D9_HDR_LDR_FALLBACK=1`, measurement
  only, never a shipping default) covers 0x6f as well; the requested format is
  still reported through `GetLevelDesc`.
* Default behaviour is unchanged: refusing by name, no silent downcast. A
  shipping LDR substitution still requires the shader-side reformulation
  described in `FLOAT_RENDER_TARGETS.md` §5.
* `tests/render_texture_hdr_smoke.c` now also creates a 256x256 R16F target on
  the measurement path, checks the reported format and releases it.
* `tests/check_format_matrix_smoke.c` asserts R16F RT/plain/texture refusals
  and BC5 cube/volume acceptance.

**Not implemented, on purpose:** a real float/HDR render target. The LDR
fallback is a probe, not a capability.

## 6. Remaining gaps

| Gap | Why it remains |
| --- | --- |
| Direct `CreateTexture` R8G8B8/A8B8G8R8 | D3DX loaders expand them; direct storage is not implemented. The measured 15 menu `CreateTexture` sites request only A8/A8L8/A4R4G4B4/A8R8G8B8 (`GFX_COMPAT.md` §4), so this is a latent, unreached path. `CheckDeviceFormat` keeps the named NOTAVAILABLE answer. |
| BC7 below 8x8 (1 UBE file: `eye01_sk.dds`, 4x4) | the shared compressed-texture floor is 8x8; a 4x4 BC7 is still refused by name. Only the 1x1 *uncompressed* cube is padded. |
| True float RT | impossible on PICA200; LDR fallback stays measurement-only |

**Closed by the texture-leftovers change:** L8 volume (real 128^3 retail
`noisevolume.dds`), DX10 BC7 (120 UBE files, real samples tested), and the
1x1 A8B8G8R8 UBE eye cube (padded to 8x8). Earlier: R8G8B8 (247 files: 189
retail + 58 mod) and A8B8G8R8 2D/cube>=8 (13 mod files).
