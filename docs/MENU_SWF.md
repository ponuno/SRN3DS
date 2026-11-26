# MENU_SWF — bounded StartMenu.swf summary

Status: the SWF **container, tag stream, bounded display-state snapshots and
DefineFont3 metrics** are parsed.  This is still not a menu: there is no
renderable display list, AVM/AS2 execution, glyph rasterisation, image decode or
draw.

The input is the winning `Interface/StartMenu.swf` bytes from
`docs/MENU_BSA_RUNTIME.md` (`Update.bsa`, 60,284 B, CRC32 `0x6910248f`).

## 1. What was added

| File | Purpose |
|---|---|
| `source/menu_swf.h` / `.c` | FWS/CWS container handling, exact single-zlib-stream inflate, SWF frame header, tag walk (including DefineSprite nesting), bounded summary |
| `source/menu_swf_display.h` / `.c` | Bounded PlaceObject2/3 + RemoveObject display state, per-ShowFrame snapshots, sprite summaries, selected-sprite frames, DefineFont2/3 metrics |
| `source/menu_swf_shape.h` / `.c` | DefineShape/2/3/4 fill/line styles and shape records, path summaries, and DefineFont2/3 glyph SHAPE contours/bounds |
| `tests/menu_swf_smoke.c` | Synthetic FWS/CWS fixtures, short/long tags, truncation/overflow, bad zlib, declared mismatch, ZWS, missing End; optional real StartMenu read-only check |
| `tests/menu_swf_display_smoke.c` | Synthetic PlaceObject2/3, RemoveObject2, sprite, ShowFrame and DefineFont3 fixtures; optional real display/font summary |
| `tests/menu_swf_shape_smoke.c` | Synthetic rectangle/curve/two-style/glyph shapes plus truncation, bitmap-id, segment-limit and AVM2/DefineFont4 refusals; optional real shape/glyph summary |
| `tools/run_host_tests.ps1` | Registers `menu_swf`, `menu_swf_display` and `menu_swf_shape`; `menu_bsa` now links `menu_swf.c` |
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

## 5. Display state and DefineFont3 metrics

`menu_swf_display_summary()` walks the same container/tag stream and models the
top-level display state:

* `PlaceObject2` (26): flags, depth, character, MATRIX, CXFORMWITHALPHA, ratio,
  name, clip depth; clip actions are recorded as an opaque byte count, not
  executed.
* `PlaceObject3` (70): the 16 flag bits, depth, class name, character, matrix,
  colour, ratio, name, clip depth, blend mode, bitmap-cache flag, visible flag
  and background.  A filter list is recorded as an opaque bounded byte span; if
  filter data is followed by blend/cache/visible/clip-action fields the tag is a
  named `E_NOTIMPL` because those fields cannot be located without decoding the
  filters.
* `RemoveObject` (5) and `RemoveObject2` (28) update the depth set.
* `ShowFrame` snapshots the live objects in depth order.  Up to
  `MENU_SWF_DISPLAY_MAX_FRAMES` = 8 frames and 12 objects per frame are stored;
  total frame/object counts and truncation flags are still reported.
* `DefineSprite` (39) is walked recursively for summaries (depth 16), and the
  first sprite with placements is re-walked for its own frame snapshots.
* `DefineFont2` (48) and `DefineFont3` (75) metrics: font id/name, flags,
  EM square 1024, Font3 shape scale 20, glyph count, code table, shape-table
  offsets, advance table, per-glyph bounds, ascent/descent/leading and kerning
  count.  No glyph shape is rasterised and no atlas is built.
* `DefineFont4` (91, CFF) and `DoABC` (82, AVM2) are named `E_NOTIMPL`.
* Names/classes are capped at 48 bytes; glyphs at 256; sprites at 128; fonts at
  2.  Truncation is explicit in the summary fields.

Measured real display/font summary:

```text
real: Update.bsa StartMenu display: top frames=1 objects=1 max=1 sprites=71/71
      selected=0 selected_frames=1 fonts=1/1
real: top frame0 objects=1 [d=1 c=101 name='MenuHolder' tx=25600 ty=14400]
real: selected sprite id=3 declared_frames=1 frames=1 placements=1 removals=0
      max_objects=1 depth=0
real: selected frame0 objects=1 [d=3 c=2 name='' sx=1.00 sy=1.00 tx=0 ty=0]
real: font id=1 glyphs=243 stored=243 type=3 layout=1 scale=20 em=1024
      ascent=20280 descent=5340 leading=5140 advance0=5020 code0=32 shape0=512
      bounds0=0,0,0,0 kerning=94 trailing=0
```

The top-level menu is one `MenuHolder` instance at depth 1.  The first sprite
with placements is id 3 (declared 1 frame); its first frame has one object at
depth 3.  The local font has 243 glyphs, a full 16-bit code table, a 20x Font3
shape scale, a 94-entry kerning table and no trailing bytes.  The retail
`FontBoundsTable` parses but its RECT values are zero in this movie; the advance
and kerning tables are populated.  Raw layout values are in Font3's 20x units
(`ascent=20280` is about 1014 * 20); `em_square` stays 1024.

## 6. Shape and glyph geometry

`menu_swf_shape_summary()` walks all top-level and nested `DefineShape` tags and
all `DefineFont2/3` glyph SHAPE records:

* `DefineShape` (2), `DefineShape2` (22), `DefineShape3` (32) and
  `DefineShape4` (83): ShapeId, ShapeBounds, Shape4 EdgeBounds/flags,
  FILLSTYLEARRAY (solid RGB/RGBA, linear/radial/focal gradient, bitmap ids),
  LINESTYLE/LINESTYLE2 arrays, style changes, move-to, straight edges and
  quadratic curved edges.
* Move-to is treated as an absolute position in shape coordinates, matching the
  measured shape bounds.  Edges are deltas from the current position.
* Points are emitted in twips (1/20 px).  Curves contribute control and anchor
  points; bounds are the conservative control-point bounds.
* Each path summary records start/end, point/segment counts, closed flag,
  conservative bounds, `fill0`/`fill1`/`line` style indices, curve and
  mid-path-style-change flags.  Paths are stored in a shared bounded pool.
* `DefineFont2/3` glyph offsets are linked to the code table and each glyph
  SHAPE is reduced to the same path summaries plus glyph bounds.  Font3 shape
  coordinates retain their 20x scale; no glyph is rasterised.
* Limits: 256 shapes, 512 shape paths, 4096 segments per shape/glyph, 256
  glyphs, 1024 glyph paths, sprite depth 16.  Segment overflow, shape/glyph
  limits and unsupported fill/line types are named refusals.

Measured real geometry (read-only `Update.bsa` winner):

```text
real: shapes=27/27 fonts=1/1 glyphs=243/243 shape_paths=151
      shape_segments=1712 shape_points=2362 glyph_paths=420
real: shape2 paths=5 segs=60 pts=105 bounds=2,522,0,520
real: MenuHolder char101 is shape=no (sprite id)
real: glyphs_with_geometry=241 glyph_segments=5292
real: first glyph code=33 paths=2 segs=13 pts=23
      bounds=570,4460,-15440,250 offset=514
```

The selected sprite 3 places character 2, and shape 2 has five paths / 60
segments with bounds `x=2..522`, `y=0..520` twips.  `MenuHolder` is sprite 101,
not a DefineShape, so it has no direct shape bounds; its child sprite hierarchy
is outside this bounded shape layer.  241 of 243 glyphs have non-empty outlines;
the two empty glyphs are recorded honestly with zero paths.  The first non-empty
glyph is code 33 (`!`), whose shape offset is 514 and whose bounds include the
tall bar (`y=-15440..250` in Font3 20x units).

No tessellation, fill rule evaluation, rasterisation or atlas upload occurs.

## 7. Tests

Synthetic-only:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_shape,menu_swf_display,menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf_shape` **28 checks, 0 failures**; `menu_swf_display` **38
checks, 0 failures**; `menu_swf` **86 checks, 0 failures**; `menu_bsa` **277
checks, 0 failures**.

Real read-only:

```powershell
$env:SKYRIM_N3DS_REAL_DATA='F:\Skyrim\Data'
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_shape,menu_swf_display,menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf_shape` **40 checks, 0 failures**; `menu_swf_display` **57
checks, 0 failures**; `menu_swf` **102 checks, 0 failures**; `menu_bsa` **286
checks, 0 failures**.

Synthetic coverage includes FWS and CWS with identical summaries, short and
long tag headers, a recursive DefineSprite, ExportAssets, ImportAssets,
ImportAssets2, DefineFontName, truncated tag/long header, tag length past the
body, missing End, nonzero End, ZWS `E_NOTIMPL`, bad CWS zlib header, declared
output mismatch, trailing compressed bytes, oversized declared size and null
input.  The display test adds PlaceObject2 move/update, RemoveObject2,
PlaceObject3 class name + opaque filter list, PlaceObject3 blend/cache/visible,
clip-action presence, sprite frames, DefineFont3 codes/advances/bounds and the
named DefineFont4/AVM2/filter-then-field refusals.  The shape test adds a
two-style closed rectangle, a quadratic curve, Shape4 edge bounds, a glyph
outline, truncation, bitmap fill without id, unsupported fill type, segment
limit and AVM2/DefineFont4 refusals.

Full host suite with the main-tree generated slice and IR container:
**83 passed, 1 diagnostic (`tesv_slice`), 0 failed**.  `menu_swf_shape`
contributes 28 synthetic checks, `menu_swf_display` 38 and `menu_swf` 86; no
real-data checks are run in that suite.

## 8. What remains

1. **Renderable display list** — the bounded state and shape geometry exist, but
   this is not a GFx display list: sprite/movie-clip materialisation, nested
   transforms, masks, clipping layers, button states and frame scripts remain.
2. **AVM/AS2** — action blocks, constants, function calls, event handlers and
   the menu's script behaviour.  `DoAction`/clip-action payloads are not
   executed.
3. **Fonts raster/atlas** — `fonts_en.swf`/`fonts_console.swf`,
   `FontConfig.txt`, glyph layout/rasterisation and atlas upload.  Shape
   outlines and metrics are parsed, but no glyph is rendered.
4. **NIF/DDS/shaders** — exported DDS/NIF assets, ETC1 conversion, logo/fog
   meshes and their shader closure.
5. **Draw path** — GFx batch contract, dynamic vertex buffer/FVF,
   `SetTextureStageState`, TEV/PICA submission and the first real frame.

## 9. Integration

```c
/* r came from menu_bsa_resolve_startmenu(&heap, ...). */
MenuSwfSummary summary;
MenuSwfDisplaySummary display; /* ~53 KiB; caller-owned, can live on heap */
MenuSwfShapeResult shapes;     /* ~113 KiB; caller-owned, can live on heap */
char reason[BSA_REASON_MAX];
const uint8_t *bytes = guest_ptr(heap.memory, r.guest_address, r.size);
MenuSwfStatus st = menu_swf_summary(bytes, r.size, &summary, reason,
                                    sizeof reason);
if (st == MENU_SWF_OK)
    st = menu_swf_display_summary(bytes, r.size, &display, reason,
                                  sizeof reason);
if (st == MENU_SWF_OK)
    st = menu_swf_shape_summary(bytes, r.size, &shapes, reason,
                                sizeof reason);
if (st == MENU_SWF_OK) {
    /* Gate the future GFx loader with display.top, display.sprites[],
     * display.fonts[].glyphs and shapes/ shape_paths. Do not report this as a
     * rendered movie. */
}
heap_free(&heap, r.guest_address);
```

The summary/display/shape structures are metadata and geometry only.  They must
not be reported as a parsed movie, renderable display list or frame.

## 10. Honest claim

* StartMenu.swf is validated as one CWS/zlib stream with the declared size,
  frame header and complete top-level tag stream through End.
* The measured 1,186-tag total, 236 top-level / 950 nested split, export/font
  names and 144,607-byte top-level tag stream are reproducible.
* The bounded display model reports one top-level `MenuHolder`, 71 sprites, a
  selected sprite timeline, and the local 243-glyph DefineFont3 code/advance/
  bounds/kerning data.
* The bounded shape model reports 27 shapes, 151 paths, 1,712 shape segments,
  2,362 shape points, 243 glyphs (241 non-empty) and 5,292 glyph segments.
* No AVM action is executed, no glyph is rasterised, no atlas or renderable
  display list is built, and no frame/pixel/Azahar/hardware result is claimed.
