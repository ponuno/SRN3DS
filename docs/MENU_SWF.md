# MENU_SWF — bounded StartMenu.swf summary

Status: the SWF **container and tag stream** are parsed and summarized.  This is
still not a menu: there is no display-list construction, AVM/AS2 execution,
glyph decode, image decode or draw.

The input is the winning `Interface/StartMenu.swf` bytes from
`docs/MENU_BSA_RUNTIME.md` (`Update.bsa`, 60,284 B, CRC32 `0x6910248f`).

## 1. What was added

| File | Purpose |
|---|---|
| `source/menu_swf.h` / `.c` | FWS/CWS container handling, exact single-zlib-stream inflate, SWF frame header, tag walk (including DefineSprite nesting), bounded summary |
| `tests/menu_swf_smoke.c` | Synthetic FWS/CWS fixtures, short/long tags, truncation/overflow, bad zlib, declared mismatch, ZWS, missing End; optional real StartMenu read-only check |
| `tools/run_host_tests.ps1` | Registers `menu_swf`; `menu_bsa` now links `menu_swf.c` |
| `source/bsa_reader.h` / `.c` | Adds `bsa_inflate_zlib_exact()` for one complete zlib stream with exact output length and no trailing compressed bytes |

The SWF header types/parser moved from `menu_bsa_runtime` into `menu_swf`, so
the BSA resolver and the summary share one implementation.

## 2. Container handling

| Signature | Behaviour |
|---|---|
| `FWS` | parsed in place; declared size must equal the file size |
| `CWS` | zlib body expanded with `bsa_inflate_zlib_exact`; output cap is exactly `declared_bytes - 8` |
| `ZWS` | named `E_NOTIMPL` (LZMA), `MENU_SWF_ERR_NOTIMPL` |
| other | named format/version refusal |

Checks before any tag is walked:

* signature and SWF version 1..32;
* declared size >= 8 and <= `MENU_SWF_MAX_DECLARED` (64 MiB);
* CWS CMF/FLG, no preset dictionary, one complete deflate stream, Adler-32, no
  trailing compressed bytes, produced length equals the declared body length;
* the frame-size `RECT` (5-bit Nbits, `(5 + 4*Nbits + 7)/8` bytes) and the
  following frame-rate/frame-count bytes are present.

Tag stream:

* `u16` header: tag code in bits 6..15, length in bits 0..5;
* length `0x3f` means a following `u32` long length;
* every tag length is checked against the remaining body;
* parsing stops at `End` (code 0, length 0); a missing End, nonzero End payload,
  truncated header/payload or length past the body is a named refusal;
* `DefineSprite` payloads are walked recursively (`UI16 spriteId`,
  `UI16 frameCount`, nested tag stream) up to 16 levels and a global 1,000,000
  tag cap.  Nested tags are counted; nested bytes are not double-counted in the
  top-level payload total.

## 3. Summary fields

`MenuSwfSummary` reports:

* `header`: format, version, declared size, SWF file size;
* `tag_count`: all tags, including nested sprite tags;
* `top_level_tag_count`, `nested_tag_count`;
* counts for `DefineShape` (codes 2/22/32/83), `DefineFont2` (48),
  `DefineFont3` (75), `DefineSprite` (39), `PlaceObject2` (26),
  `PlaceObject3` (70), `ShowFrame` (1), `DoAction` (12),
  `ExportAssets` (56), `ImportAssets`/`ImportAssets2` (57/71),
  `DefineFontName` (88), `DefineBitsLossless` (20/36) and
  `DefineBitsJpeg` (6/21/35/90);
* `tag_stream_bytes`: top-level tag bytes from the first tag through End;
* `tag_payload_bytes`: sum of top-level tag payload lengths;
* `frame_header_bytes`: RECT + frame rate/count;
* `first_show_frame`: first **top-level** ShowFrame tag index, and its
  tag-stream-relative byte offset;
* `trailing_bytes`: bytes after the top-level End (recorded, not parsed);
* bounded names (`MENU_SWF_MAX_NAMES` = 64, `MENU_SWF_NAME_MAX` = 96):
  `ExportAssets` names, `ImportAssets`/`ImportAssets2` URL and symbols, and
  `DefineFontName` font names.  `name_total` counts all occurrences;
  `names_truncated` marks dropped/truncated strings.

Retail `DefineFontName` carries a copyright string after the font name; the
font name is kept and the trailing bytes are ignored.  `ImportAssets2` is the
retail variant of `ImportAssets` (two reserved bytes before the symbol count).

No AVM action payload is decoded.  `DoAction` is counted only.

## 4. Measured real summary

Read-only from `F:\Skyrim\Data` (`Update.bsa` winner):

```text
real: Update.bsa StartMenu.swf 1186 tags (236 top, 950 nested), shapes=27
      fonts2=0 fonts3=1 fontnames=1 sprites=71 place2=326 place3=21
      show=428 doaction=53 export=47 import=2 lossless=0 jpeg=0
      names=52/52 first_show=234 frame_header=13 tag_bytes=144607 trailing=0
```

Interpretation:

* `1186` total tags matches the earlier census; `236` are top-level and `950`
  are inside 71 `DefineSprite` timelines.
* 47 `ExportAssets` tags are present; the bounded name list contains 52 names
  (47 exports + 2 imported font symbols + 2 import URLs + 1 font name).
* the local font is `DefineFont3` plus `DefineFontName`; the imported symbols
  come from `gfxfontlib.swf` via `ImportAssets2`.
* no `DefineBitsLossless`/`DefineBitsJpeg` (0/0) appear; the menu uses exported
  DDS/texture assets instead of embedded bitmaps.
* the first top-level `ShowFrame` is tag 234; the top-level tag stream is
  144,607 bytes after a 13-byte frame header.

## 5. Tests

Synthetic-only:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf` **86 checks, 0 failures**; `menu_bsa` **277 checks,
0 failures**.

Real read-only:

```powershell
$env:SKYRIM_N3DS_REAL_DATA='F:\Skyrim\Data'
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf` **102 checks, 0 failures**; `menu_bsa` **286 checks,
0 failures**.

Synthetic coverage includes FWS and CWS with identical summaries, short and
long tag headers, a recursive DefineSprite, ExportAssets, ImportAssets,
ImportAssets2, DefineFontName, truncated tag/long header, tag length past the
body, missing End, nonzero End, ZWS `E_NOTIMPL`, bad CWS zlib header, declared
output mismatch, trailing compressed bytes, oversized declared size and null
input.

Full host suite with the main-tree generated slice and IR container:
**72 passed, 1 diagnostic (`tesv_slice`), 0 failed**.  `menu_swf` contributes
86 synthetic checks and no real-data checks in that run.

## 6. What remains

1. **Display list** — interpret `PlaceObject2/3`, sprite timelines, matrices,
   colour transforms, clipping and `ShowFrame` ordering.
2. **AVM/AS2** — action blocks, constants, function calls, event handlers and
   the menu's script behaviour.  `DoAction` is currently only counted.
3. **Fonts** — `fonts_en.swf`/`fonts_console.swf`, `FontConfig.txt`,
   `DefineFont3` glyph extraction, layout/rasterisation and glyph atlas upload.
   The summary reports font definitions and names, not glyphs.
4. **Images/geometry** — exported DDS/NIF assets, ETC1 conversion, logo/fog
   meshes and their shader closure.
5. **Draw path** — GFx batch contract, dynamic vertex buffer/FVF,
   `SetTextureStageState`, TEV/PICA submission and the first real frame.

## 7. Integration

```c
/* r came from menu_bsa_resolve_startmenu(&heap, ...). */
MenuSwfSummary summary;
char reason[BSA_REASON_MAX];
MenuSwfStatus st = menu_swf_summary(
    guest_ptr(heap.memory, r.guest_address, r.size), r.size,
    &summary, reason, sizeof reason);

if (st == MENU_SWF_OK) {
    /* Use summary.header, counts and names to gate the future GFx loader. */
}
heap_free(&heap, r.guest_address);
```

The summary is metadata only.  It must not be reported as a parsed movie,
display list or frame.

## 8. Honest claim

* StartMenu.swf is validated as one CWS/zlib stream with the declared size,
  frame header and complete top-level tag stream through End.
* The measured 1,186-tag total, 236 top-level / 950 nested split, export/font
  names and 144,607-byte top-level tag stream are reproducible.
* No display list, AVM action, glyph, image, NIF, DDS or draw is produced.
* No frame, pixel proof, Azahar run or hardware run is claimed.
