# Texture formats needed by the world (retail corpus + installed mods)

Status: **measurement + format support note**, 2026-11-27. No frame is
claimed. `DevCaps` is unchanged (`0x00082000`). R8G8B8 and A8B8G8R8 are now
decoded (see §2a); BC7, the L8 volume and the 1x1 eye cube remain open.

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
  These are DX10-header assets with no BC7 decoder in this tree. They are
  refused by name; see section 4.
* **Uncompressed 24-bit RGB (247 files) is now supported by the D3DX shim.**
  Retail usage is limited (189 files, all 256x256 1-mip tint masks in
  `Skyrim - Textures.bsa`); every file expands to A8R8G8B8 on upload with
  A=255. See §2a.
* **A8B8G8R8 (13 mod files, UBE 2.0)** is likewise expanded on upload, with
  the B/R bytes swapped from the DDS R,G,B,A memory order. See §2a.
* **The L8 volume** is the only volume format in the corpus; the volume loader
  still handles only BC/RGBA/expanded formats, not L8. Open.
* **The 1x1 A8B8G8R8 UBE eye cube** is refused by name: the PICA/backend cube
  floor is 8x8 and no 1x1 cube padding exists. Open.

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
texture loader". This is honest: the game reaches these files through D3DX,
not through `CreateTexture`.

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

## 4. BC7: honest named refusal

BC7 is real in the installed corpus, so it is not silently downgraded and not
guessed as BC3:

* The host DDS parser rejects the DX10 header and names the exact cause:
  `BC7 (DXGI format 98) is not supported by this pipeline: there is no BC7
  decoder or BC7-to-ETC1 conversion` (`tools/skyrim/dds.py`).
* The D3DX shim rejects it as `D3DXERR_INVALIDDATA` with
  `DX10 DXGI format 98 (BC7_UNORM) has no decoder in this bridge (BC7 is not
  converted to ETC1/BC5)`.
* No BC7 → ETC1/BC5 conversion is attempted. A correct BC7 decoder is a
  substantial (8 modes, partitions, endpoint/weight grids) piece of work and
  PICA has no BC7 hardware; the honest options are a real decoder later or a
  documented mod-side re-export to DXT5/BC5.

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
| L8 volume (1 retail file) | volume upload only implements block/RGBA/expanded formats |
| BC7 (120 mod files) | named refusal; requires a real BC7 decoder or re-export |
| 1x1 A8B8G8R8 eye cube (1 UBE file) | backend cube floor is 8x8; no 1x1 cube padding/upscale path |
| Direct `CreateTexture` R8G8B8/A8B8G8R8 | source-only formats: D3DX loaders expand them, direct storage is not implemented and `CheckDeviceFormat` says so |
| True float RT | impossible on PICA200; LDR fallback stays measurement-only |

**Closed by this change:** R8G8B8 (247 files: 189 retail + 58 mod) and
A8B8G8R8 2D/cube>=8 (13 mod files) load through the D3DX shim into A8R8G8B8.
