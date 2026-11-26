# MENU_SWF — bounded StartMenu.swf summary

Status: the SWF **container, tag stream, structured font imports, exact-id font
resolution, flattened display list, solid fill colours, DefineEditText
parsing/layout, shape/glyph tessellation, A8 raster/atlas/batch and host CPU ->
COM indexed/UP menu submission** are implemented.  Real StartMenu declares
imported font ids 34/41 from `gfxfontlib.swf`; that URL is absent and the
`Interface/fonts_en.swf` fallback contains neither id, so all 57 text records are
named refusals rather than name-substituted.  An exact-id match is wired through
edit layout/tessellation into the COM geometry batch.  This is still not a menu
frame: focal/bitmap fill sampling, mask/clip rendering, AVM/AS2, full GFx
text/assets and PICA sampling remain open.

The input is the winning `Interface/StartMenu.swf` bytes from
`docs/MENU_BSA_RUNTIME.md` (`Update.bsa`, 60,284 B, CRC32 `0x6910248f`).

## 1. What was added

| File | Purpose |
|---|---|
| `source/menu_swf.h` / `.c` | FWS/CWS container handling, exact single-zlib-stream inflate, SWF frame header, recursive tag walk, bounded summary and structured ImportAssets/ImportAssets2 URL + id/symbol collection |
| `source/menu_swf_avm.h` / `.c` | Bounded DoAction/DoInitAction AVM1 decoder, caller-owned opcode histogram/byte inventory, unknown-opcode refusals and side-effect-free Stop/Play/GotoFrame display annotation |
| `source/menu_swf_display.h` / `.c` | Bounded PlaceObject2/3 + RemoveObject display state, per-ShowFrame snapshots, sprite summaries, selected-sprite frames, caller-owned DefineFont2/3 collection, exact-id imported-font resolution and DefineEditText collection |
| `source/menu_swf_display_list.h` / `.c` | First-frame flattened display list with recursive sprite expansion, accumulated 16.16 matrices/colour transforms, clip depth and cycle/depth limits |
| `source/menu_swf_clip.h` / `.c` | Clip-depth mask pairs/rectangles, active-mask lists, intersection, fully-clipped flags and rectangle mask quads |
| `source/menu_swf_shape.h` / `.c` | DefineShape/2/3/4 fill/line styles and shape records, path summaries, retained solid RGBA/gradient stops+matrix/bitmap ids, streaming geometry sink, and DefineFont2/3 glyph SHAPE contours/bounds |
| `source/menu_swf_tess.h` / `.c` | Bounded even-odd/non-zero tessellation of one shape or DefineFont2/3 glyph into caller-owned vertex/index/triangle-style pools, optional fill-style table and separate stroked line segments |
| `source/menu_swf_text.h` / `.c` | Latin-1 layout with advances/kerning, per-glyph placement/tessellation, 16.16 transform, ARGB vertices, GFx HTML runs (colour/size/face/flags/alignment), plus DefineEditText WordWrap/Multiline/alignment/Justify/indent/leading/Password layout |
| `source/menu_swf_raster.h` / `.c` | Bounded A8 coverage rasterisation of tessellated glyph triangles with 4x4 subpixel samples, caller buffers, 16.16-quantised matrix/pen and named refusals |
| `source/menu_swf_atlas.h` / `.c` | Deterministic shelf packing of A8 coverage rectangles into a caller atlas, zero-filled padding, per-glyph rect/UV and named refusals |
| `source/menu_swf_batch.h` / `.c` | Text quad/index batch with per-run colours and optional record CXFORM, plus shape triangle batch with solid colours and per-vertex linear/radial gradients from tess fill metadata, record CXFORM, optional axis-aligned viewport clipping, full/outside culling plus Sutherland-Hodgman partial clipping for axis-aligned masks, rotated-mask/text named refusals and count-before-write overflow |
| `source/d3d9_menu_text.h` / `.c` | Host/menu COM demo: A8 texture creation/upload and one DrawPrimitiveUP from the atlas+batch |
| `tests/menu_swf_smoke.c` | Synthetic FWS/CWS fixtures, short/long tags, truncation/overflow, bad zlib, declared mismatch, ZWS, missing End; optional real StartMenu read-only check |
| `tests/menu_swf_avm_smoke.c` | Synthetic Stop/Play/GotoFrame/GotoLabel/Wait/Push/variable/function/unknown/truncated cases, DoInitAction ownership, safe display annotation and exact real StartMenu histogram |
| `tests/menu_swf_display_smoke.c` | Synthetic PlaceObject2/3, RemoveObject2, sprite, ShowFrame and DefineFont3 fixtures; optional real display/font summary |
| `tests/menu_swf_display_list_smoke.c` | Synthetic 2-sprite nesting/matrices/colour/RemoveObject2/clip-depth plus cycle, overflow and broken-tag refusals; optional real StartMenu flatten |
| `tests/menu_swf_clip_smoke.c` | Synthetic 3-mask nesting/intersection/full-clip plus bad-pair, overflow, complex-mask and record-matrix cases; optional real display-list clip |
| `tests/menu_swf_shape_smoke.c` | Synthetic rectangle/curve/two-style/glyph shapes plus truncation, bitmap-id, segment-limit and AVM2/DefineFont4 refusals; optional real shape/glyph summary |
| `tests/menu_swf_tess_smoke.c` | Synthetic square in both orientations, even-odd hole, non-zero star/nested contours, curve, multiple styles, stroked square, empty contour, overflow and missing-shape refusals; optional real tessellation summary |
| `tests/menu_swf_text_smoke.c` | Synthetic font/edit-text fixtures: parse/collect, wrap/multiline, all alignments/Justify, HTML, empty bounds, Password/unknown-font/overflow refusals and GFx sentinels; optional real StartMenu text/import census |
| `tests/menu_swf_html_smoke.c` | Synthetic GFx HTML tags/entities/nesting/unknown tags, run layout size scaling, per-run batch colours/CXFORM and named refusals; optional real StartMenu HTML payload census |
| `tests/menu_swf_raster_smoke.c` | Synthetic square/triangle exact coverage, hole rules, subpixel/rotation/mirror/clip, empty glyph and named refusals; optional real code-33 32x32 and 64x64 coverage |
| `tests/menu_swf_atlas_smoke.c` | Synthetic varied-size pack/determinism/UV/composition/overflow/empty cases; optional real codes 33..36 coverage-sum comparison |
| `tests/menu_swf_batch_smoke.c` | Synthetic 3-glyph positions/UV/indices/winding/empty/viewport/overflow and optional real 33..36 atlas batch |
| `tests/menu_swf_fill_smoke.c` | Synthetic solid RGBA/loops, CXFORM, mirroring, accepted linear/radial batches, focal/bitmap refusals; optional real fill census and per-vertex colours |
| `tests/menu_swf_gradient_smoke.c` | Synthetic exact 2/3-stop linear, radial, pad/reflect/repeat, named invalid stops/matrix/interpolation/spread/focal/bitmap refusals; optional real StartMenu gradient census |
| `tests/menu_swf_mask_batch_smoke.c` | Synthetic clip records: fully inside/outside, axis-aligned partial clipping, rotated-style clipping, missing bounds and fully clipped; real StartMenu zero-mask batch equivalence |
| `tests/menu_swf_partial_mask_smoke.c` | Edge-crossing triangle positions/area/colour/winding, inside/outside, source dedup, partially clipped text UVs, rotated-mask and rotated-text named refusals, real StartMenu zero-mask equivalence |
| `tests/menu_text_draw_smoke.c` | COM demo: 2-glyph 8x8 A8 texture/upload, sampler/TSS/FVF, one DrawPrimitiveUP, empty/overflow and A8-refusal cases |
| `tests/menu_pipeline_e2e_smoke.c` | Real `Update.bsa` StartMenu: URL/fallback font load, exact-id resolve/refusals, solid-fill shape batch + resolved edit-text tessellation -> local-font raster/atlas control -> compact indexed or bounded UP COM submission; no frame claim |
| `tools/run_host_tests.ps1` | Registers `menu_swf`, `menu_swf_avm`, `menu_swf_display`, `menu_swf_display_list`, `menu_swf_shape`, `menu_swf_tess`, `menu_swf_text`, `menu_swf_html`, `menu_swf_raster`, `menu_swf_atlas`, `menu_swf_batch`, `menu_swf_fill`, `menu_swf_gradient`, `menu_swf_mask_batch`, `menu_text_draw` and `menu_pipeline_e2e`; `menu_bsa` now links `menu_swf.c` |
| `tests/menu_indexed_draw_smoke.c` | Core recording backend: exact compact vertices/indices, >4,095-index batch, 32-bit and 65,536-vertex fallbacks, named expansion limit and DevCaps 0x00082000 |
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

### Structured import records and exact-id font resolution

`menu_swf_collect_imports()` preserves each `ImportAssets` (57) /
`ImportAssets2` (71) URL with its ordered `UI16 id -> symbol-name` list.  It
walks nested `DefineSprite` tags, supports short/long tag headers and enforces
16 import records, 64 symbols per record, 256 URL bytes and 96 symbol-name
bytes.  Missing fields are `TRUNCATED`/`FORMAT`; duplicate imported ids,
malformed records and trailing payload bytes are `FORMAT`; caller/string
capacity overflow is a named `BUDGET` refusal.  `menu_swf_find_import()` looks
up only the numeric import id.

`menu_swf_display_collect_fonts()` walks top-level/nested `DefineFont2` (48)
and `DefineFont3` (75) tags into a caller-owned `MenuSwfFontList`.  Malformed
fonts, duplicate stored ids and caller capacity overflow are fatal named
refusals.  An unrelated font whose glyph/kerning table exceeds the bounded
summary is skipped and recorded in `skipped`/`skip_reason`, allowing later
candidate ids to remain searchable.

`menu_swf_display_resolve_import_font()` requires **exact equality** between the
imported symbol id and a candidate `DefineFont` id.  Symbol/font names are
diagnostic only.  A miss returns `MENU_SWF_ERR_NOTIMPL` and the reason
`imported font id N is not present in <candidate_source>` plus symbol/URL
context; no same-name font is substituted.

The real e2e loader searches `Update.bsa`, `Skyrim - Interface.bsa`, then loose
`Data/Interface`.  It tries the declared URL first and, when that file is
absent, `Interface/fonts_en.swf`.  Multiple import URLs are not merged into one
candidate namespace.  For an exact-id match, nonempty DefineEditText sources
are laid out with the edit font height, tessellated, transformed by the display
record and appended to the white-A8 geometry batch before COM submission.

### AVM1/AS2 action inventory and safe frame control

`menu_swf_avm_collect()` walks top-level and nested sprite tags and decodes
`DoAction` (12) / `DoInitAction` (59) into caller-owned `MenuSwfAvmRecord`
items.  Each record retains its sprite/frame, payload/code byte counts,
opcode count, unknown/refused counters and top-level frame-control operations.
The list keeps a 256-entry histogram, aggregate byte/instruction counts and
named first refusal.  Limits are 256 records, 32 controls per record, 65,536
instructions and 16 nested function/with/try levels.

The decoder covers the AVM1 fixed-length and length-prefixed action forms,
including Push value types, ConstantPool, DefineFunction/2 external bodies,
With and Try block sizes.  Unknown length-prefixed opcodes are counted, named
and skipped by their declared length so later actions remain visible.  Unknown
fixed-length opcodes cannot be resumed safely and end that record with
`NOTIMPL`; malformed/truncated lengths remain named `FORMAT`/`TRUNCATED`
errors.  Function-body opcodes are inventoried but never become executable
top-level controls.

Only top-level `Stop` (`0x07`), `Play` (`0x06`) and in-range `GotoFrame`
(`0x81`) update `MenuSwfAvmControlState`.  `GotoLabel` (`0x8c`), `SetTarget`
(`0x8b`/`0x20`), WaitForFrame, Push/GetVariable/SetVariable, function
definitions, calls and other side effects are counted but not executed.
`menu_swf_avm_apply_display_controls()` annotates flattened records with
`stopped`/`frame_index` for their parent timeline, considering only actions on
the currently selected safe frame.  This is metadata/frame-control only: no
arbitrary ActionScript, visual navigation or rendered frame is produced.

Measured real StartMenu inventory:

```text
action records=87 (DoAction=53, DoInitAction=34)
payload bytes=85,933; code bytes=85,865; opcodes=15,549; unknown=0
Stop=53; Play=0; Goto=0; GotoLabel=0; SetTarget=0; Wait=0
Push=5,503; GetVariable=626; SetVariable=3; function definitions=368
calls=939; named not-executed opcodes=12,173
frame-scoped display annotation: stopped=66, playing=150, refused=0
```

Exact real opcode histogram:

| Opcode | Action | Count |
|---:|---|---:|
| `00` | End | 87 |
| `07` | Stop | 53 |
| `0b` | Subtract | 79 |
| `0c` | Multiply | 21 |
| `0d` | Divide | 17 |
| `12` | Not | 866 |
| `17` | Pop | 1,088 |
| `1c` | GetVariable | 626 |
| `1d` | SetVariable | 3 |
| `26` | Trace | 3 |
| `2b` | CastOp | 2 |
| `3a` | Delete | 16 |
| `3c` | DefineLocal | 12 |
| `3d` | CallFunction | 47 |
| `3e` | Return | 224 |
| `40` | NewObject | 39 |
| `42` | InitArray | 59 |
| `43` | InitObject | 57 |
| `44` | TypeOf | 2 |
| `45` | TargetPath | 4 |
| `47` | Add2 | 98 |
| `48` | Less2 | 47 |
| `49` | Equals2 | 377 |
| `4c` | PushDuplicate | 151 |
| `4e` | GetMember | 2,376 |
| `4f` | SetMember | 858 |
| `50` | Increment | 34 |
| `51` | Decrement | 6 |
| `52` | CallMethod | 892 |
| `53` | NewMethod | 9 |
| `54` | InstanceOf | 14 |
| `55` | Enumerate2 | 6 |
| `60` | BitAnd | 23 |
| `61` | BitOr | 6 |
| `62` | BitXor | 1 |
| `63` | BitLShift | 4 |
| `64` | BitRShift | 6 |
| `65` | BitURShift | 3 |
| `66` | StrictEquals | 62 |
| `67` | Greater | 48 |
| `69` | Extends | 13 |
| `87` | StoreRegister | 350 |
| `88` | ConstantPool | 25 |
| `8e` | DefineFunction2 | 328 |
| `96` | Push | 5,503 |
| `99` | Jump | 215 |
| `9b` | DefineFunction | 40 |
| `9d` | If | 749 |

All 53 frame-control actions are Stop operations.  They occur in sprites
38/46/64/67/68/70/74/77/84/93/100/101 and the top-level timeline, at action
frames including 0/1/4/5/7/9/10/14/19/22/29/36/39/44/51/59/66/74/81/89/96/104.
No retail Play, GotoFrame, GotoLabel, SetTarget or WaitForFrame is present.

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
  offsets, advance table, per-glyph bounds, ascent/descent/leading, kerning
  count and the first `MENU_SWF_DISPLAY_MAX_KERNING` = 256 `(code1, code2,
  adjustment)` pairs.  No glyph shape is rasterised and no atlas is built.
* `DefineFont4` (91, CFF) and `DoABC` (82, AVM2) are named `E_NOTIMPL`.
* Names/classes are capped at 48 bytes; glyphs at 256; kerning pairs at 256;
  sprites at 128; fonts at 2.  Truncation is explicit in the summary fields.

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
* `MenuSwfShapeFill` retains the parsed style instead of only counting it:
  kind (solid/linear/radial/focal/bitmap), solid RGBA, gradient matrix and up
  to eight `ratio+RGBA` stops (with the advertised stop count), focal point,
  bitmap character id and repeat flag.  Each shape has `fill_first` and
  `stored_fills`; `menu_swf_shape_fill()` resolves a 1-based style.  The fill
  pool holds 1,024 entries and is caller-owned; overflow sets `truncated`.
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
real: fills solid=40 linear=0 radial=0 focal=0 bitmap=0 stored_fills=70
real: shape2 fills=1 first_colors=ff000000
real: top_fill_shape=69 fills=3 colors=ff333333,ffcccccc,33ffffff
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

The 40 solid fills are the sum over each shape's latest `FILLSTYLEARRAY`;
70 raw fill records are retained because some shapes use `StateNewStyles`.
Shape 2's latest array is a single opaque black fill (`ff000000`), while its
merged tessellation table contains three entries from earlier arrays.  Shape
69 has three fills: `ff333333`, `ffcccccc` and `33ffffff`.  No retail
StartMenu shape uses a gradient or bitmap fill.

The shape layer itself does not tessellate, evaluate fill rules, rasterise or
upload an atlas.  It exposes `menu_swf_shape_walk()` to stream the decoded
contours to the next layer.

## 7. Path tessellation

`menu_swf_tess_shape()` consumes one shape character through
`menu_swf_shape_walk()` and writes triangles and stroked segments into buffers
supplied by the caller.  The tessellator performs no allocation; the shared SWF
view may still decompress a CWS body into its existing temporary buffer.

* Fill rules: `MENU_SWF_TESS_EVEN_ODD` toggles at every crossing;
  `MENU_SWF_TESS_NON_ZERO` accumulates the contour direction winding.
  Concentric contours therefore support holes, and same-direction nested
  contours fill under non-zero.
* Straight edges are used as-is.  Quadratic curves are flattened adaptively by
  recursive midpoint subdivision until the second difference is at most 2
  twips, with a depth cap of 8 (at most 256 segments per curve).
* Paths are closed implicitly for filling.  Stroked lines are emitted only for
  edges actually present in the shape, never for the implicit fill closure.
* Contours are grouped by their non-zero `fill0`/`fill1` style indices.  Each
  group is tessellated independently, so a shape with several fills produces
  triangles tagged with each style.  Stroke segments carry their `line` style.
* When `mesh->fills` is supplied, each parsed fill style is copied there and
  `triangle_fills` becomes a global 1-based index.  A `StateNewStyles` record
  appends a new array and shifts subsequent path indices by the previous table
  size, so old and new style 1 no longer collide.  `fill_count` is the merged
  table size; existing single-array callers still see styles 1..N.
* The tessellator uses a bounded scan-line band decomposition.  Vertices are
  emitted per band interval (not deduplicated), so counts describe a
  render-ready triangle list and not a welded mesh.
* Limits: 65,536 vertices, 196,608 indices, 65,536 triangles, 65,536 lines,
  65,536 scratch edges, 131,072 bands and 4,096 contours per shape.  Any hard
  limit or caller capacity overflow is a named `E_BUDGET`; a missing character
  is `E_ARGUMENT`.

Measured real tessellation (read-only `Update.bsa` winner, even-odd):

```text
real: tess shapes=27/27 vertices=71605 triangles=35759 lines=11
      curve_segments=3118
real: shape2 tess bounds=0,522,0,520
```

Shape 2's declared `ShapeBounds` is `2..522,0..520`; the tessellated contour
extents are `0..522,0..520`, because the declared rectangle is slightly padded.
The 27 shapes produce 35,759 triangles and 11 stroke segments.  Triangles are
emitted with positive signed area in stored coordinates.  Glyph tessellation
uses the same core through `menu_swf_tess_glyph()`; there is no rasterisation,
atlas upload or frame.

## 8. Glyph tessellation and text layout

`menu_swf_tess_glyph()` walks the DefineFont2/3 glyph SHAPE table through
`menu_swf_shape_walk_fonts()`, filters by font id and character code, and feeds
the contours to the same scan-line tessellator.  Glyph fills are tagged style 1,
strokes are suppressed, and coordinates stay in raw font units (Font3 20x,
Font2 1x).  A missing font/code is `E_ARGUMENT`; an empty glyph is a valid
zero-triangle result.

`menu_swf_text_layout()` then performs simple Latin-1 layout:

* Each byte is a 0..255 character code.  The glyph table is searched by code;
  advances come from the DefineFont advance table.  The first matching
  `(previous, current)` kerning pair is applied before the glyph, so pen
  positions include kerning.  Newlines and control bytes have no special
  line-breaking semantics.
* Every glyph is tessellated into a caller-supplied one-glyph scratch
  `MenuSwfTessMesh`, transformed by the caller's 16.16 matrix plus translation,
  and appended to the caller's vertex/index buffers.  `MenuSwfTextGlyph`
  records the transformed placement, raw pen, advance, kerning, glyph index,
  vertex/triangle ranges and transformed bounds.
* `MenuSwfTextStyle` supplies the 16.16 matrix, translation and ARGB colour;
  every output vertex carries that colour.  Font3 raw units are 20x, so a
  caller wanting pixel units sets the matrix accordingly.
* A known empty glyph keeps its placement/advance and emits no triangles.  An
  unknown code keeps a placement, reports `glyph_index =
  MENU_SWF_TEXT_MISSING_GLYPH`, emits no triangles and advances by zero.
* Limits: 4,096 characters/glyphs, 262,144 vertices, 786,432 indices and
  262,144 triangles, plus the tess scratch limits.  Caller or hard capacity
  overflow is a named `E_BUDGET`.

Measured real text (read-only `Update.bsa` winner, codes 33..36, identity
16.16 matrix):

```text
real: text glyphs=4 vertices=2818 triangles=1408 missing=0 pen=32500
      kerning=94
real: first glyph code=33 index=1 x=0 y=0 advance=5040 tris=159
      bounds=570,4460,-15440,250
```

The local font has 243 glyphs, EM 1024, Font3 scale 20 and 94 kerning pairs.
The four selected glyphs produce 1,408 triangles with no missing codes.  This
is CPU geometry and placement only; rasterisation, glyph atlas upload, shaping
beyond advance+kerning, bidi and a frame remain absent.

## 9. A8 coverage rasterisation

`menu_swf_raster_mesh()` unions the triangles from `menu_swf_tess_glyph()` (or
any compatible `MenuSwfTessMesh`) into a caller-owned A8 coverage buffer:

* The tessellator has already applied its non-zero (or even-odd) fill rule, so
  the rasteriser only unions non-overlapping triangles.  A hole is created by
  the chosen tessellation rule, not by a second winding pass.
* Each pixel uses a 4x4 sample grid (16 samples at 1/8, 3/8, 5/8, 7/8), giving
  sub-pixel coverage.  Coverage is `round(inside * 255 / 16)`, so a fully
  covered pixel is 255 and a half-covered pixel is 128.
* Vertices are transformed with a caller matrix/pen, quantised to signed 16.16
  before use, and landed in 1/256-pixel coordinates.  Negative determinants
  are valid mirrors; setting `reject_negative_scale` returns
  `MENU_SWF_RASTER_ERR_NEGATIVE_SCALE`.  A zero determinant is
  `MENU_SWF_RASTER_ERR_SCALE`.
* The target is validated before any write: zero size, `stride < width` or a
  NULL pixel buffer is `ERR_TOO_SMALL`; dimensions over 4096, stride over
  1 MiB, more than 1,536 triangles or transformed coordinates over the safe
  range are `ERR_OVERFLOW`.  Non-finite matrix terms are `ERR_NAN`.
* The buffer is cleared (width bytes per row, honouring stride) and only
  `width x height` is written.  The result reports the used rectangle
  (`x1`/`y1` exclusive), coverage sum, covered-pixel count, maximum coverage
  and the echoed advance.  An empty mesh is valid: zero coverage and the
  declared advance are still returned.
* No allocation, atlas, upload, cache, GPU call or frame is involved.

Measured real code-33 coverage (fit with a 2-pixel margin, non-zero glyph
tessellation, advance 5,040):

```text
real: glyph 33 32x32 sum=41464 covered=182
      bounds=12,1,20,31 max=255
real: glyph 33 64x64 sum=178003 covered=726
      bounds=24,1,40,63 max=255
```

The coverage sums stay below `width * height * 255` and the used rectangles
stay inside the target.  This is CPU coverage data, not a rendered frame,
texture upload or atlas entry.

## 10. A8 atlas packing

`menu_swf_atlas_pack()` packs the used rectangles of raster coverage bitmaps:

* The order is deterministic: descending height, descending width, ascending
  glyph index, then input order.  A simple shelf/row packer places each rect
  at the current shelf cursor, advances by `w + 1`, and starts a new shelf at
  `previous_shelf_y + shelf_height + 1`.  There is a 1px gap between rects and
  no rotation.
* A glyph larger than the atlas is `ERR_TOO_LARGE`; failure to fit a shelf is
  `ERR_OVERFLOW`; more than 512 glyphs or too small an entry array is
  `ERR_TOO_MANY`.  Planning happens before any atlas/entry write, so a refusal
  leaves both untouched.
* Empty glyphs (width 0 or height 0) get a zero rect, `has_rect=0` and zero
  UVs; they consume no space and are counted separately.
* On success the full `width x height` atlas is cleared (stride padding
  untouched), coverage rows are copied into each rect, and all bytes outside
  rects remain zero.  Guard bytes outside the atlas are never touched.
* Per-glyph entries carry `(x,y,w,h)` and 16.16 UVs normalized to the atlas,
  computed as differences so `u+uw`/`v+vh` are exact at the right/bottom edge.
  The result reports used height, occupied glyph pixels (padding excluded),
  16.16 occupancy over `width * used_height`, and placed/empty counts.
* No allocation, upload, texture object, cache or GPU call is involved.

Measured real pack of codes 33..36 into 256x256 (coverage from the 64x64
raster pass):

```text
real: atlas used_height=62 occupied=8618 occupancy16=35584 placed=4 empty=0
real: glyph 33 rect=69,0,16,62  sum=178003
real: glyph 34 rect=86,0,62,56  sum=681816
real: glyph 35 rect=0,0,36,62   sum=302803
real: glyph 36 rect=37,0,31,62  sum=226181
```

Every atlas-rect coverage sum equals the corresponding raster sum.  Occupancy
is 8,618 glyph pixels over `256 * 62`, i.e. 35,584 in 16.16 (about 54.3%).  The
rectangle order reflects height/width sorting (35, 36, 33 at height 62, then
34 at height 56), not input order.

## 11. Text quad batches

`menu_swf_batch_text()` turns a `MenuSwfTextMesh` and an aligned array of
`MenuSwfAtlasEntry` into caller vertex/index buffers:

* One non-empty glyph becomes four vertices (TL, TR, BR, BL) and two triangles
  `(0,1,2)`/`(0,2,3)`.  Empty, missing and zero-size glyphs are skipped without
  leaving index holes.  Glyph order follows the text mesh's pen order.
* The style matrix/translation is quantised to signed 16.16 and applied to the
  layout-space glyph bounds.  Every vertex carries its glyph run's ARGB;
  glyphs without an explicit run colour fall back to `style->color`.  Optional
  `color_mult`/`color_add` applies the record CXFORM to the selected run
  colour.  A zero or negative determinant is `ERR_SCALE`; non-finite terms are
  `ERR_NAN`.
* Positions are rounded to integer pixel edges with no +0.5 bias.  Atlas UVs
  map texel edges to those quad edges, which is the half-pixel convention used
  by the raster/atlas path.  Positive-area winding is maintained in
  x-right/y-down coordinates.
* An optional half-open viewport rect clips axis-aligned glyph rectangles;
  UVs are interpolated over the clipped fraction.  Fully clipped glyphs are
  omitted.  A rotated/skewed matrix with viewport clipping is the named
  `ERR_CLIP` rather than a silently wrong AABB clip.
* `menu_swf_batch_text_clip()` additionally applies a menu clip record:
  glyphs wholly outside the intersected `clip_rect` are omitted and wholly
  inside glyphs are kept unchanged.  For an axis-aligned style matrix a partial
  glyph is clipped to the mask rectangle as a smaller quad, interpolating the
  atlas UVs over the kept fraction; the run colour is unchanged.  A partial
  glyph under a rotated/skewed style is the named
  `partial clip-mask culling of rotated text is not implemented` refusal, and a
  partial overlap with a rotated mask is
  `partial clip-mask culling of rotated masks is not implemented`.  The clip
  rect is screen twips; positions are compared as pixels*20 and the mask edge
  is converted to output pixels for the sub-quad.
* The required quad/vertex/index counts are computed before any write.
  Capacity or hard-limit overflow is `ERR_OVERFLOW` and leaves both buffers
  untouched.  Limits: 4,096 quads, 16,384 vertices, 24,576 indices.
* No allocation, upload, texture bind, cache or GPU call occurs.

Measured real batch of codes 33..36 (atlas above, fit into a 128x64 viewport):

```text
real: batch quads=4 vertices=16 indices=24 bounds=1,104,1,63
real: first quad x=1..14 y=6..57 uv=17664,0..21760,15872
```

All four quads are inside the viewport and do not overlap; their UV corners
match the packed atlas entries.  This is CPU vertex/index data only.

### Shape triangle batches

`menu_swf_batch_shape()` consumes a tessellated shape and its merged fill
table.  Each triangle uses the 1-based style in `triangle_fills[]`:

* A solid fill's RGBA is transformed per channel by the record CXFORM
  (`mult` 8.8, `add` 8-bit) and written to its shared vertex.  The same white
  texel UV is written to every vertex.
* Linear gradients invert the fill matrix, normalize the gradient-square x
  axis from `-16384..16384`, apply the SWF spread mode (pad/reflect/repeat),
  interpolate up to eight ratio+RGBA stops and then the record CXFORM.  Radial
  gradients use the distance from the gradient center divided by `16384`;
  the gradient matrix's scale/rotate are 16.16 and its translation is twips.
  Colours are evaluated per source vertex, so large triangles are an explicit
  approximation (no gradient texture or TEV gradient).  Focal gradients and
  bitmap fills are named refusals (`focal gradient fills are not implemented`,
  `bitmap fills are not implemented`); invalid/truncated stops, unsupported
  interpolation/spread and a singular matrix are named too.
* The batch reuses `mesh->vertices[]` and `mesh->indices[]`: each source vertex
  is emitted once, and copied only when the same vertex is referenced by
  triangles with different fills.  This keeps the real StartMenu at **57,480**
  vertices, below the 65,536-vertex 16-bit indexed limit, instead of expanding
  to 86,154 vertices.  Indices are local; the caller adds the append base.  A
  mirroring (negative determinant) matrix is allowed and reverses each
  triangle's winding; a zero determinant is `ERR_SCALE`.
* Limits are 262,144 vertices and 786,432 indices; one indexed draw additionally
  requires at most 65,536 vertices.  The caller capacity must cover the
  worst-case `vertex_count + index_count` output vertices (actual StartMenu
  output is 57,480).  Counts and styles are validated before any write;
  refusals leave the buffers untouched.
* `menu_swf_batch_shape_clip()` applies the record's intersected clip rect:
  a fully clipped record returns an empty batch without writing, wholly outside
  triangles are culled, and wholly inside triangles keep the existing
  winding/colour/dedup.  A partially overlapping triangle is clipped in screen
  space with Sutherland-Hodgman against the four rectangle edges; positions are
  rounded back to output pixels, per-vertex ARGB and the white-texel UV are
  interpolated along cut edges, duplicate/zero-area results are dropped, and
  the fan preserves the same positive winding as the unclipped path (mask
  mirrors reverse it back).  Source corners keep the existing per-fill dedup;
  only new edge intersections become appended vertices.
* Partial clipping is supported only for axis-aligned masks.  A mask whose
  transformed quad is rotated/skewed is flagged
  (`MenuSwfClipMask.axis_aligned=0`,
  `MenuSwfClipRecord.has_rotated_mask=1`); a partial overlap with it is the
  exact named refusal
  `partial clip-mask culling of rotated masks is not implemented`.  Its AABB is
  still used for the fully-inside/outside decisions, so those remain the
  documented AABB approximation rather than an overdrawing clip.
* The first pass computes the exact output vertex/index counts including the
  up-to-four generated intersections per partial triangle.  Capacity uses
  `vertex_count + index_count + new_intersections` vertices (source vertices,
  the existing one-copy-per-index dedup bound and intersections) and the exact
  kept index count; `ERR_OVERFLOW` still precedes any write.  The caller's
  65,536-vertex 16-bit indexed limit applies to the expanded count.
* The real StartMenu has zero masks, so its batch is byte-for-byte unchanged at
  57,480 vertices.
* No allocation, texture, upload or GPU call is performed.

The real pipeline tessellates 28,718 triangles and emits 57,480 shape vertices
and 86,154 indices, all carrying a non-white fill colour (`57480/57480`).  The
full combined batch is 57,484 vertices / 86,160 indices / 28,720 triangles after
the control glyph.

## 12. Host COM A8 draw

`d3d9_menu_text` closes the CPU atlas/batch onto the host D3D9 COM demo:

* Core 2D textures accept `D3DFMT_A8` (28): pitch is width bytes, level bytes
  are `width*height`, COM LockRect uses 1 byte/pixel, and A8 UnlockRect passes
  the raw coverage bytes to the backend.  No A8 texture is replaced by
  A8R8G8B8.  If the backend cannot create/upload A8, init fails with
  `MENU_TEXT_DRAW_ERR_A8`/`ERR_UPLOAD` and no fallback texture is created.
* The citro3d/PICA backend maps A8 to `GPU_A8` through the appended
  `texture_create_format`/`texture_upload_format` callbacks and uploads one
  byte/texel with `C3D_TexLoadImage`; RGBA8/RGB8 continue through the existing
  path.  Compile-only ARM evidence exists; PICA A8 sampling in a real draw/TEV
  is not runtime-tested here.
* `menu_text_draw_init()` creates the A8 texture through the COM device path,
  uploads the atlas with exact pitch, and maps a caller guest scratch region
  for expanded vertices.
* `menu_text_draw_batch()` expands the source vertices once into 24-byte
  `XYZ|DIFFUSE|TEX1` vertices (float3 position, D3DCOLOR, float2 UV), then:
  - `SetTexture(0, texture)`;
  - sampler stage 0: MIN/MAG POINT, MIP NONE, ADDRESS U/V CLAMP;
  - stage 0 COLOROP/ALPHAOP = MODULATE, ARG1 = TEXTURE, ARG2 = DIFFUSE;
  - `SetFVF(0x142)`;
  - one `DrawIndexedPrimitiveUP(TRIANGLELIST, 16-bit)` when the backend
    provides `draw_menu_indexed` and the whole source vertex set / index list
    fits the scratch; otherwise one bounded `DrawPrimitiveUP` expansion.
  The caller must have an active BeginScene; the module does not claim a frame.
* Empty batches, invalid indices, indexed scratch overflow, more than 1,365
  triangles on the UP fallback, source vertices above 65,536, state/FVF/draw
  failures and A8 creation/upload failures are named.  The API never logs a
  successful batch as a rendered frame.

Measured COM-demo result (2 synthetic glyphs, 8x8 atlas, 8x8 A8 texture):

```text
texture format=28 width=8 height=8 pitch=8, lock bytes == atlas coverage
draw: 12 expanded vertices, 1 DrawPrimitiveUP, FVF=0x142
state: POINT/CLAMP, COLOROP=MODULATE, ARG1=TEXTURE, ARG2=DIFFUSE
```

The test also verifies a texture-create refusal is `ERR_A8`, an empty batch is
`ERR_EMPTY`, and an oversized index count is `ERR_OVERFLOW`.  There is no
sampling result, frame, or GPU pixel claim.

## 13. Flattened display list

`menu_swf_display_list_build()` processes the first ShowFrame of the top-level
timeline and recursively expands sprite placements:

* `PlaceObject2` (26) flags cover HasCharacter, HasMatrix, HasColorTransform,
  HasRatio, HasName and HasClipDepth; `PlaceObject3` (70) is accepted through
  the same validated display parser.  `RemoveObject2` (28) and `RemoveObject`
  (5) delete a depth.
* Live objects are kept sorted by depth.  Each sprite placement is emitted as a
  record and its first timeline frame is walked immediately, so the result is a
  depth-first, depth-sorted flat array.  Sprite cycles are `ERR_TAG`; more than
  32 sprite levels, 128 live objects, 128 sprite definitions or 8,192 records
  are named `ERR_BUDGET`/refusals.
* Matrices are 16.16 and compose `parent * child`; translations are in twips.
  Colour transforms use 8.8 multipliers and additive channels and compose
  `parent.mult * child` with the parent addend.  Every record carries the final
  matrix/colour, local depth, clip depth, ratio, name, parent sprite id and
  sprite depth.
* The caller owns the record array; overflow reports the number written and
  sets `truncated`.  A broken PlaceObject/RemoveObject tag is `ERR_FORMAT`.

Measured real `Update.bsa` StartMenu first frame:

```text
real: records=216 sprites=71 sprite_records=106 top=1
      placements=216 removals=0 max_depth=7
```

The top frame has one object; 71 sprite definitions produce 106 sprite records
and 216 total flattened records, with a maximum sprite nesting depth of 7.
This is CPU metadata only: masks/clip rendering, AVM and font layout are still
outside the module, and no frame is produced.

## 14. Clip-depth masks

`menu_swf_clip_build()` derives rectangles from the flat display-list records:

* A record with ClipDepth is a mask active for depths
  `(mask.depth, mask.clip_depth]`.  The builder uses the flat record-depth ranges
  (it does not reconstruct separate sprite-scope ownership) and intersects every
  active mask.  Up to eight nested masks are allowed; more is
  `MENU_SWF_CLIP_ERR_NESTING`.  Mask depth >= clip depth is `ERR_PAIR`.
* `MenuSwfClipShape` supplies local shape/glyph bounds aligned with records.
  Mask bounds must be present and `is_rect=1`; a complex shape is the named
  `"mask shape tessellation is not implemented"` refusal.  Record bounds are
  optional for non-mask records; without them `has_rect=0`.
* Each output record gets its transformed screen-space AABB, the intersection
  of all active mask AABBs, active mask indices, `fully_clipped`, and the record
  index.  Masks are emitted in record order with their four transformed quad
  corners and AABB.  Each mask also records `axis_aligned` (its four quad edges
  parallel to the screen axes); a record with any rotated/skewed active mask
  sets `has_rotated_mask`.  These flags let the batch layer clip axis-aligned
  partial overlaps and refuse the rotated case by name instead of clipping to an
  over-approximating AABB.
* `menu_swf_clip_mask_quad()` exposes the four transformed rectangle corners
  independently.  `menu_swf_clip_rect_intersect()` is the axis-aligned helper.
* Limits: 8,192 records, 256 masks, 8 active masks per record.  Pair, mask,
  nesting and capacity validation completes before any output write, so a
  refusal leaves the caller arrays untouched.
* No mask tessellation, AVM, GPU clip or frame is produced.

Measured real `Update.bsa` StartMenu (flat display list above):

```text
real: clip records=216 masks=0 clipped=0 max=0
```

The retail StartMenu has no ClipDepth pairs on the first frame.  Synthetic
coverage uses three nested masks: depth 4 receives all three and intersects to
`40..60`; depth 5 receives two and intersects to `30..70`; an off-mask record is
fully clipped.  Bad pairs, nine-mask nesting, complex masks and capacity
overflows are named.

## 15. DefineEditText and real text sources

`menu_swf_display_parse_edit_text()` parses tag 37 into `MenuSwfEditText`:
character id, twip bounds, all standard flags (WordWrap, Multiline, Password,
ReadOnly, text colour/max length/font/layout/select/border/static/HTML/outlines),
font id/height, RGBA, alignment, margins, signed indent/leading, variable name
and initial text.  `menu_swf_display_collect_edit_texts()` walks top-level and
nested sprite tags into a caller-owned array.  Limits are **64 definitions**,
**4,096 initial-text bytes** and **256 variable-name bytes**; malformed data is
`FORMAT`, capacity/string overflow is `BUDGET`, and unknown alignment is
`NOTIMPL`.  The measured retail sentinels `alignment=0xff` and
`left/right margin=0xffff` normalize to left/zero.  Empty initial text may omit
its NUL at tag end.  Trailing bytes are reported explicitly.

`menu_swf_text_edit_plain()` extracts the bounded plain source without a font.
`menu_swf_text_edit_runs()` parses the same source into `MenuSwfTextRun`
records.  For the measured GFx convention the payload lives in the
variable-name slot and its first opening `<` is omitted; the parser normalizes
that one case.  Supported tags are `<font color="#RRGGBB" size="N" face="...">`,
`<b>`, `<i>`, `<u>`, `<br>`, `<p align=...>` and `<li>`; entities are
`amp/lt/gt/quot/apos/nbsp`.  Runs retain colour, size, face, bold/italic/
underline and alignment.  Unknown tags are skipped while their inner text is
preserved and counted; malformed tags and unknown entities are named refusals.
Face names are metadata only (no face-to-font match in this layer).  The plain
extractor decodes the same entities and refuses unsupported ones.

`menu_swf_text_layout_edit()` requires an axis-aligned positive raw-font-to-
pixel style and matching font id.  It applies bounds/margins, first-line indent,
font height/leading, explicit Multiline newlines, greedy WordWrap, Left/Right/
Center/Justify (extra space after spaces on non-final lines), kerning and
advances.  Password text becomes `*` only when the font has that glyph,
otherwise named `NOTIMPL`; unknown font id is named `ARGUMENT`; output capacity
and line/glyph limits are named `BUDGET`.  Empty text emits zero glyphs/vertices
and one empty line while preserving the transformed edit bounds.  HTML runs are
laid out in order: a run's `size` scales advances and glyph outlines relative
to the edit font height, line ascent/height use the largest run, alignment comes
from the run/paragraph state, and every glyph/vertex carries the run's ARGB.
`MenuSwfTextMesh.runs` reports caller-owned spans with glyph ranges.

Measured real `Update.bsa` StartMenu:

```text
DefineEditText definitions=22 ids=22
HTML-flagged definitions=22; payloads=4 (3 nonempty + 1 empty <p>)
HTML runs=3 tags=14 unknown=0 colours=3 sizes=3 faces=3 entities=0
id=82  "$Loading extra content."                 23 chars
id=86  "$Loading downloadable content..."        32 chars
id=95  "Start a New Game?"                       17 chars
display-list text records=57; nonempty record instances=3
explicit source lines=3; source glyph slots=72
import url=gfxfontlib.swf id=34 symbol=$EverywhereMediumFont
import url=gfxfontlib.swf id=41 symbol=$EverywhereFont
font source=Skyrim - Interface.bsa!interface/fonts_en.swf bytes=417809 fallback=1
candidate DefineFont3 ids=1,3,5,7,9,11,13,15,19,23 (10 stored)
skipped DefineFont3 ids=17 (Falmer, 28 glyphs, 621 kerning),
                      21 (Mage Script, 29 glyphs, 621 kerning)
resolved records=0; resolved real-text glyphs=0; real-text vertices added=0
```

Both imported fonts come from `gfxfontlib.swf`.  That URL is absent from
`Update.bsa`, `Skyrim - Interface.bsa` and loose `Data/Interface`.  The fallback
`fonts_en.swf` is present in `Skyrim - Interface.bsa` (417,809 B, SHA-256
`97e45bbdfe5e030ec48f652204686862b3791dcf69b1e43151330cb2c838f331`) and
contains 12 DefineFont3 definitions.  Ten fit the 256-glyph/256-kerning summary
limits; ids 17 and 21 each have 621 kerning records and are explicitly skipped.
The usable ids are 1/3/5/7/9/11/13/15/19/23, so **neither imported id 34 nor 41
is present**.  All 57 records therefore receive the exact named refusal
`imported font id 34/41 is not present in Skyrim - Interface.bsa!interface/fonts_en.swf`
with symbol/URL context.

The local StartMenu font id 1 `Futura Condensed` (243 glyphs) and fallback
same/similar-name fonts such as ids 1/3/5 are **not** substituted.  The e2e has
an exact-id resolved-text path into layout/tessellation/COM, but the retail
census does not exercise it; measured resolved real-text output remains zero.
The existing local-font `!` remains the raster/atlas/batch/COM control.

## 16. Tests

Synthetic-only:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_batch,menu_swf_fill,menu_swf_gradient,menu_swf_mask_batch,menu_swf_atlas,menu_swf_raster,menu_swf_text,menu_swf_html,menu_swf_tess,menu_swf_shape,menu_swf_clip,menu_swf_display_list,menu_swf_display,menu_swf,menu_swf_avm,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe -TestTimeoutSec 300
```

Result: `menu_swf_batch` **21 checks, 0 failures**; `menu_swf_fill` **34
checks, 0 failures**; `menu_swf_mask_batch` **13 checks, 0 failures**;
`menu_swf_atlas` **27
checks, 0 failures**; `menu_swf_raster` **44 checks, 0 failures**;
`menu_swf_text` **76 checks, 0 failures**; `menu_swf_html` **31 checks, 0
failures**; `menu_swf_tess` **47 checks, 0
failures**; `menu_swf_shape` **28 checks, 0 failures**; `menu_swf_clip` **17
checks, 0 failures**; `menu_swf_display_list` **15 checks, 0 failures**;
`menu_swf_display` **38 checks, 0 failures**; `menu_swf` **95 checks, 0
failures**; `menu_swf_avm` **26 checks, 0 failures**; `menu_bsa` **277 checks,
0 failures**.

Real read-only:

```powershell
$env:SKYRIM_N3DS_REAL_DATA='F:\Skyrim\Data'
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_batch,menu_swf_fill,menu_swf_gradient,menu_swf_mask_batch,menu_swf_atlas,menu_swf_raster,menu_swf_text,menu_swf_html,menu_swf_tess,menu_swf_shape,menu_swf_clip,menu_swf_display_list,menu_swf_display,menu_swf,menu_swf_avm,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe -TestTimeoutSec 300
```

Result: `menu_swf_batch` **34 checks, 0 failures**; `menu_swf_fill` **45
checks, 0 failures**; `menu_swf_mask_batch` **20 checks, 0 failures**;
`menu_swf_atlas` **37
checks, 0 failures**; `menu_swf_raster` **60 checks, 0 failures**;
`menu_swf_text` **93 checks, 0 failures**; `menu_swf_html` **58 checks, 0
failures**; `menu_swf_tess` **55 checks, 0
failures**; `menu_swf_shape` **40 checks, 0 failures**; `menu_swf_clip` **24
checks, 0 failures**; `menu_swf_display_list` **23 checks, 0 failures**;
`menu_swf_display` **57 checks, 0 failures**; `menu_swf` **114 checks, 0
failures**; `menu_swf_avm` **35 checks, 0 failures**; `menu_bsa` **286 checks,
0 failures**.

Synthetic coverage includes FWS and CWS with identical summaries, short and
long tag headers, a recursive DefineSprite, ExportAssets, ImportAssets,
ImportAssets2, DefineFontName, truncated tag/long header, tag length past the
body, missing End, nonzero End, ZWS `E_NOTIMPL`, bad CWS zlib header, declared
output mismatch, trailing compressed bytes, oversized declared size and null
input.  Structured-import coverage preserves both tag variants and URLs,
recurses into sprites, finds numeric ids and refuses capacity, truncation and
duplicate ids.  The AVM test adds Stop/Play/GotoFrame/GotoLabel/WaitForFrame,
Push/GetVariable/SetVariable, function-body scope, unknown-opcode skip,
truncated/malformed refusals, DoInitAction ownership and frame-scoped display
annotation; its real check asserts the complete histogram above.  The display
test adds PlaceObject2 move/update, RemoveObject2,
PlaceObject3 class name + opaque filter list, PlaceObject3 blend/cache/visible,
clip-action presence, sprite frames, DefineFont3 codes/advances/bounds and the
named DefineFont4/AVM2/filter-then-field refusals.  The shape test adds a
two-style closed rectangle, a quadratic curve, Shape4 edge bounds, a glyph
outline, truncation, bitmap fill without id, unsupported fill type, segment
limit and AVM2/DefineFont4 refusals.  The flattened display-list test adds two
nested sprites, accumulated matrices/colour, RemoveObject2, clip depth, depth
order, cycle/overflow and broken-PlaceObject refusals.  The tessellation test
adds a 4-vertex /
2-triangle square in both contour orientations, an even-odd hole, a non-zero
star and same-direction nested contours, an adaptive curve, two fill styles, a
stroked square, an empty contour, vertex/index/edge overflow and a missing
shape; the real check tessellates all 27 shapes.  The text test adds a 3-glyph
font with an A/V kerning pair, direct glyph tessellation, placement/colour and
half-scale layout, an empty space glyph, an unknown code, and vertex/index/
glyph/scratch/length overflow; the real check lays out four StartMenu glyphs
and verifies one real kerning pair, collects all 22 DefineEditText definitions,
verifies the three plain sources total 72 chars and reports the missing imported
fonts.  Caller-owned font collection is tested for single-tag parsing, nested
DefineSprite recursion, malformed/duplicate/capacity refusals and a skipped
257-pair oversized font; resolver tests prove exact-id success and named misses
without same-name substitution.  DefineEditText synthetic coverage adds
field parsing, caller-owned collection, WordWrap, Multiline/leading,
Left/Right/Center/Justify, HTML plain extraction, empty bounds,
Password/unknown-font/glyph/variable/capacity refusals and retail
alignment/margin sentinels.  The raster test adds exact full-coverage
2x2 and 16x16 squares, a sampled triangle (coverage sum 573), subpixel,
90-degree, mirror and border-clipping cases, a non-zero vs even-odd hole,
empty-mesh advance and NaN/overflow/scale/negative-scale/too-small refusals;
the real check rasterises code 33 at 32x32 and 64x64.  The atlas test packs 12
varied synthetic glyphs twice and in reversed order (byte-identical), verifies
rects/UVs/no overlap/zero padding/guards, empty glyphs, too-large/overflow/
too-many/short-stride refusals without mutation, and compares real 33..36
atlas sums against their raster sums.  The batch test adds three synthetic
glyphs (one empty) with exact positions/UV/indices/winding/colors, partial and
full viewport clipping, overflow without buffer mutation, NaN/negative-scale/
rotated-clip refusals, and real 33..36 atlas quads.  The fill test adds a
solid `80402010` fill with exact per-vertex ARGB, an 8.8 CXFORM result
`808a2003`, mirrored winding, accepted linear/radial gradient batches, named
focal/bitmap refusals, and a short-output refusal that leaves the buffer
untouched; the real check reports 40 solid fills with no gradients/bitmaps, a
black shape 2, and 552/552 shape-2 vertices carrying fill colours.
`menu_swf_gradient` adds exact two- and three-stop linear colours, radial
center/mid/radius, pad/reflect/repeat at `t=1.25`, and named descending-stop,
truncated-stop, interpolation, spread and singular-matrix refusals.  The HTML
test adds `<font color/size/face>`, `<b>`, `<i>`, `<u>`, `<br>`, `<p align>`,
all supported entities, nested style restore, unknown-tag text preservation,
malformed/unknown-entity and capacity refusals; it lays out a 24px run at twice
the 12px advance, checks per-run glyph and batch colours plus CXFORM, and the
real check reports 22 HTML-flagged definitions, 4 payloads (3 nonempty), 3
runs, 14 tags and 0 unknown tags.  The mask-batch test adds a synthetic clip
record with a fully inside triangle (kept unchanged), a fully outside triangle
(culled), an axis-aligned partial triangle (clipped fan with exact edge
vertices/area/colour), a rotated-style partial triangle (screen-space clip), a
fully-clipped record and a clip without record bounds; a partially clipped text
glyph checks the sub-quad and interpolated UVs.  The new partial-mask test adds
the edge-crossing triangle area/winding, inside/outside byte equality, source
dedup beside a clipped triangle, rotated-mask and rotated-text named refusals;
the real StartMenu zero-mask clip records leave the shape-2 batch counts
identical (552 vertices / 828 indices for shape 2).

Full host suite in this worktree with `SKYRIM_N3DS_REAL_DATA=F:\Skyrim\Data`,
the optional pilot/closure fixtures and `-TestTimeoutSec 300`:
**146 passed, 0 diagnostic, 0 failed**.  The main-tree run with the generated
slice/IR container additionally reports the known `tesv_slice` diagnostic
(**145 passed, 1 diagnostic, 0 failed**).

## 17. Real StartMenu CPU -> COM pipeline

`tests/menu_pipeline_e2e_smoke.c` is the integration smoke for the existing
CPU layers and the real COM demo.  It reads the winning
`Interface/StartMenu.swf` from `Update.bsa` through the same
`SKYRIM_N3DS_REAL_DATA` path as the other real-data tests, then:

1. builds the first flattened display-list frame;
2. classifies every record as sprite, mask/clip, shape, DefineEditText or
   unknown character;
3. tessellates every unmasked shape record with its accumulated 16.16 matrix,
   appends solid-fill triangle-list vertices/indices with parsed RGBA * record
   CXFORM and counts skipped stroke segments;
4. collects structured ImportAssets2 records, loads the declared font URL then
   `Interface/fonts_en.swf` and `Interface/fontconfig.txt`, collects candidate
   DefineFont2/3 definitions, and resolves imported ids first by exact id and
   then by fontconfig symbol -> face/style -> one DefineFont3 (logged
   substitution); unprovable mappings are named refusals.  Nonempty edit text
   is laid out/tessellated with its resolved primary and per-run faces into its
   own real geometry;
5. lays out one synthetic string (`"!"`) using the local 243-glyph DefineFont3,
   rasterises its 159 triangles, packs the used A8 rectangle beside one white
   texel and appends the resulting control text quad;
6. creates/uploads the 64x64 A8 atlas through `d3d9_menu_text`, uses one
   16-bit `DrawIndexedPrimitiveUP` when the callback is available and the source
   has at most 65,536 vertices, otherwise keeps 4,095-index `DrawPrimitiveUP`
   chunks; verifies FVF `0x142`, POINT/NONE/CLAMP,
   MODULATE(TEXTURE,DIFFUSE), A8 identity/upload/lock bytes, compact indexed
   counts or expanded UP counts, and named empty / bad-index refusals.  The
   indexed assertions remain active once the parallel vertex-dedup fix brings
   the retail batch under the 16-bit range.

Measured real result (2026-11-26):

```text
source=Update.bsa StartMenu.swf bytes=60284 tags=1186 avm_skipped=53
records=216 sprites=71 sprite_records=106 shape_definitions=27
shape_records=53 text_records=57 mask_records=0 unsupported_records=0
edit_text_definitions=22 edit_text_ids=22 nonempty_definitions=3
source_records=57 nonempty_records=3 source_lines=3 source_glyph_slots=72
refused_records=0 resolved_records=57 exact_records=0 substituted_records=57
layout_records=3 refused_font_mask=00000000
resolved_text_glyphs=72 missing_glyphs=0
real_text_vertices=3024 real_text_indices=4320 real_text_triangles=1440
html_defs=22 payloads=4 nonempty=3 runs=3 tags=14 unknown=0
html_colors=3 html_sizes=3 html_faces=3 html_entities=0
empty_shapes=0 degenerate_shapes=0 strokes_skipped=182
tessellated_vertices=57480 triangles=28718
shape_colour_vertices=57480/57480
shape_batch_vertices=57480 shape_batch_indices=86154
glyph=33 tess_vertices=319 triangles=159
raster=64x64 rect=24,1,40,63 sum=178003 atlas_used_height=62
batch_vertices=57484 indices=86160 triangles=28720
limits=262144 vertices / 786432 indices
COM draw_calls=1 expanded_vertices=0 indexed_draws=1
indexed_vertices=57484 indexed_indices=86160
FVF=0x142 A8=64x64 control_text_quads=1; 74 checks, 0 failures
```

Imported ids 34/41 are absent from `fonts_en.swf`.  The shipped
`fontconfig.txt` maps `$EverywhereMediumFont` to `Futura Condensed` Normal and
`$EverywhereFont` to `Futura CondensedLight` Normal; those are DefineFont3 ids
3 and 1 (187 glyphs each).  All 57 text records resolve and the three authored
labels (`$Loading extra content.`, `$Loading downloadable content...`,
`Start a New Game?`, 72 source slots) produce **72 real glyph placements, 0
missing, 3,024 vertices / 4,320 indices / 1,440 triangles**.  The four HTML
payloads (three nonempty plus one empty `<p>`) parse into three runs and 14
tags; each run face resolves to the same real face.  Resolved text geometry is
kept separate from the shape batch so the fixed census stays 57,484 / 86,160;
combined shape + real text would be 60,508 vertices / 90,480 indices, still
16-bit-safe.  The single local-font `!` control still exercises the
raster/atlas/COM path.
Shape fills use the atlas's white texel and the parsed solid-fill RGBA
multiplied by the record CXFORM; linear/radial gradient fills are evaluated per
vertex, focal/bitmap fills are named refusals, and the 182 stroke segments are
counted and skipped.  The deduped batch is 57,484 vertices / 86,160 indices,
within the 16-bit indexed range, and is submitted as one indexed draw.  The
host mock `Present` only closes the COM
scene state machine so releases are not deferred.  No pixel readback, GPU/PICA
sampling, visual equivalence or frame is claimed.

Verification:

* targeted `menu_swf_avm,menu_swf,menu_swf_display_list,menu_pipeline_e2e`
  with real data: **4 C targets passed; harness 14 passed, 0 diagnostic, 0
  failed**; check counts 35, 114, 23 and 60;
* targeted
  `menu_swf_html,menu_swf_text,menu_swf_batch,menu_swf_fill,menu_indexed_draw,menu_pipeline_e2e`
  with real data: **16 passed, 0 diagnostic, 0 failed**; check counts 58, 93,
  34, 45, indexed PASS and 60;
* targeted
  `menu_swf_partial_mask,menu_swf_mask_batch,menu_swf_clip,menu_swf_batch,menu_swf_shape,menu_pipeline_e2e`
  with real data: **6/6 C targets passed, 0 failed**; check counts 27, 28, 24,
  34, 40 and 60 (plus 10 Python targets, all passing);
* mask + real e2e under ASan: **20 + 60 checks, 0 failures; harness 12
  passed, 0 diagnostic, 0 failed**;
* AVM + SWF + display-list + real e2e under ASan: **35 + 114 + 23 + 60 checks,
  0 failures; harness 14 passed, 0 diagnostic, 0 failed**;
* HTML + real e2e under ASan: **58 + 60 checks, 0 failures; harness 12
  passed, 0 diagnostic, 0 failed**;
* full host suite with `SKYRIM_N3DS_REAL_DATA=F:\Skyrim\Data`, the optional
  pilot/closure fixtures and `-TestTimeoutSec 300`: **146 passed, 0
  diagnostic, 0 failed**; the main-tree slice/IR run additionally reports the
  known `tesv_slice` diagnostic (**145 passed, 1 diagnostic, 0 failed**);
* ARM compile-only (`arm-none-eabi-gcc`, no link/run): all changed C
  translation units succeeded; `menu_swf_batch.o` text **15,958 B** and
  `menu_swf_clip.o` text **3,423 B** (devkitARM 15.2.0, `-O2`, `-march=armv6k`);
* ARM `-Mode com -NoClean -Jobs 8`: `.elf` 6,294,280 B, SHA256
  `927684b1328d9661dc39ad8167973c11da76c0df45d828b15b6fc5cd89990d65`;
  `.3dsx` 936,180 B, SHA256
  `ca08df4e5ca93a48c17b0ccf78b85f23284627117128357b443ebaad8b297bd1`;
  compile/link only, no hardware or frame claim;
* ARM compile-only (`arm-none-eabi-gcc`, no link/run): 5/5 changed TUs;
  `menu_swf_font.o` 6,572 B, `menu_swf_display.o` 19,900 B,
  `menu_swf_shape.o` 15,796 B, `menu_swf_tess.o` 10,192 B,
  `menu_swf_text.o` 22,996 B;
* targeted
  `menu_font_resolve,menu_swf_text,menu_swf_atlas,menu_swf_glyph,menu_pipeline_e2e`
  with real data: **15 passed, 0 diagnostic, 0 failed**; check counts 31, 93,
  37, 8 and 74;
* real font resolution: ids 34/41 -> fontconfig faces -> DefineFont3 3/1,
  **72 glyphs / 3,024 vertices / 4,320 indices / 1,440 triangles**, 0 missing;
  shape batch remains **57,484 vertices / 86,160 indices / 28,720 triangles**;
  `menu_swf_font.o` 6,572 B, `menu_swf_display.o` 19,900 B,
  `menu_swf_shape.o` 15,796 B, `menu_swf_tess.o` 10,192 B,
  `menu_swf_text.o` 22,996 B;
* ARM `-Mode com -Isolated menu_fonts -NoClean -Jobs 8`: `.elf` 6,293,872 B,
  SHA256 `879ec6036de2c476a8b0a1573dee9ded4a4051165cbd6aeb33e46a8e3f13730b`;
  `.3dsx` 936,180 B, SHA256
  `ca08df4e5ca93a48c17b0ccf78b85f23284627117128357b443ebaad8b297bd1`.
  Build-only; no frame claim.

* ARM compile-only `-Mode com -Isolated edit_text`: `.elf` 6,178,376 B,
  SHA256 `2fd3e1af9ec6de706e762f20012340a90f74ad61dec2b64ccdf339cfecc4191e`;
  `.3dsx` 913,460 B, SHA256
  `27fd1752fdc0b8e645a32c73d8f4c9b3c362d95fb08e09e12014a2db6004e5d6`.

## 18. What remains

1. **Renderable display list** — bounded state, flattened CPU transform records,
   shape geometry, solid/gradient fills, tessellation, full/outside and
   axis-aligned partial clip-mask clipping, plus a host COM triangle batch
   exist, but this is not a GFx display list: exact rotated-mask clipping (a
   rotated-mask partial overlap is currently a named refusal using its AABB),
   bitmap fill sampling, clip rendering, clipping layers, button states, frame
   scripts and real draw submission remain.
2. **Arbitrary AVM/AS2 execution** — the bounded inventory and safe
   Stop/Play/in-range GotoFrame metadata are implemented, but constants,
   variables, property/member access, function calls, event handlers, branches,
   exceptions and the menu's full script behaviour are not executed.
   `GotoLabel`, SetTarget, WaitForFrame and all side effects remain named
   refusals/counters.
3. **Atlas cache/real GFx fonts** — `gfxfontlib.swf` (imported font ids 34/41)
   is absent, but the shipped `Interface/fontconfig.txt` now maps 34/41 to
   real `fonts_en.swf` faces (ids 3/1) and all three authored labels produce
   real glyph geometry.  There is still no persistent texture/glyph cache,
   `fonts_console.swf` is not merged, and richer shaping (ligatures,
   bidi, multi-font fallback), letter-spacing/class spans
   and text metrics beyond advance+kerning remain.  The host COM demo can
   create/upload an A8 atlas and draw the combined shape/control-text batch;
   there is no persistent cache, PICA A8 sampling or real GFx text object yet.
4. **NIF/DDS/shaders** — exported DDS/NIF assets, ETC1 conversion, logo/fog
   meshes and their shader closure.
5. **Draw path** - real GFx display list/masks, dynamic vertex buffer/FVF,
   TEV/PICA submission and the first real frame.  The host COM indexed/UP draw
   is not a frame claim.

## 19. Integration

```c
/* r came from menu_bsa_resolve_startmenu(&heap, ...). */
MenuSwfSummary summary;
MenuSwfDisplaySummary display; /* ~56 KiB; caller-owned, can live on heap */
MenuSwfShapeResult shapes;     /* ~113 KiB; caller-owned, can live on heap */
MenuSwfTessMesh tess;          /* buffers and capacities are caller-owned */
MenuSwfTessMesh glyph_tess;    /* one glyph for rasterisation */
MenuSwfTextMesh text;          /* output and one-glyph scratch are caller-owned */
MenuSwfRasterTarget raster;    /* width/height/stride/A8 pixels */
MenuSwfRasterResult coverage;
MenuSwfAtlasTarget atlas;      /* width/height/stride/A8 pixels */
MenuSwfAtlasEntry atlas_entries[glyph_count];
MenuSwfAtlasResult atlas_result;
MenuSwfBatchVertex batch_vertices[glyph_count * 4];
uint32_t batch_indices[glyph_count * 6];
MenuSwfBatchResult batch_result;
MenuSwfDisplayListRecord list_records[216]; /* caller-sized */
MenuSwfDisplayListResult flat;
MenuSwfClipRecord clip_records[216];
MenuSwfClipMask clip_masks[16];
MenuSwfClipResult clip_result;
char reason[BSA_REASON_MAX];
const uint8_t *bytes = guest_ptr(heap.memory, r.guest_address, r.size);
MenuSwfStatus st = menu_swf_summary(bytes, r.size, &summary, reason,
                                    sizeof reason);
if (st == MENU_SWF_OK)
    st = menu_swf_display_summary(bytes, r.size, &display, reason,
                                  sizeof reason);
if (st == MENU_SWF_OK)
    st = menu_swf_display_list_build(bytes, r.size, list_records, 216,
                                     &flat, reason, sizeof reason);
/* shapes may be NULL when no clip-mask bounds are needed. */
if (st == MENU_SWF_OK)
    (void)menu_swf_clip_build(list_records, flat.record_count, NULL, 0,
                              clip_masks, 16, clip_records, 216,
                              &clip_result, reason, sizeof reason);
if (st == MENU_SWF_OK)
    st = menu_swf_shape_summary(bytes, r.size, &shapes, reason,
                                sizeof reason);
/* After the caller supplies vertex/index/style/line/scratch buffers: */
if (st == MENU_SWF_OK)
    st = menu_swf_tess_shape(bytes, r.size, shape_id,
                             MENU_SWF_TESS_NON_ZERO, &tess, reason,
                             sizeof reason);
/* After supplying text output and glyph-scratch buffers: */
if (st == MENU_SWF_OK)
    st = menu_swf_text_layout(bytes, r.size, &display.fonts[0], "Menu", 4,
                              &text_style, &text, reason, sizeof reason);
/* Per-glyph rasterisation after menu_swf_tess_glyph() and an A8 target: */
if (st == MENU_SWF_OK)
    (void)menu_swf_raster_mesh(&glyph_tess, &raster_xf, &raster, &coverage,
                               reason, sizeof reason);
/* After collecting the per-glyph coverage sources: */
if (st == MENU_SWF_OK)
    (void)menu_swf_atlas_pack(atlas_glyphs, glyph_count, &atlas,
                              atlas_entries, glyph_count, &atlas_result,
                              reason, sizeof reason);
if (st == MENU_SWF_OK)
    (void)menu_swf_batch_text(&text, atlas_entries, glyph_count,
                              &batch_style, &viewport, batch_vertices,
                              glyph_count * 4, batch_indices,
                              glyph_count * 6, &batch_result, reason,
                              sizeof reason);
if (st == MENU_SWF_OK) {
    /* Gate the future GFx loader with display.top, display.sprites[],
     * display.fonts[].glyphs, shapes/ shape_paths, tess/ triangles,
     * text/ placements+triangles, coverage/ used rect, atlas/ entries and
     * batch/ vertices+indices. Do not report this as a rendered movie. */
}
heap_free(&heap, r.guest_address);
```

The summary/display/display-list/shape/tess/text/raster/atlas/batch structures
are metadata, CPU geometry, A8 coverage/atlas memory and CPU vertex/index data
only.  They must not be reported as a parsed movie, renderable display list or
frame.

## 20. Honest claim

* StartMenu.swf is validated as one CWS/zlib stream with the declared size,
  frame header and complete top-level tag stream through End.
* The measured 1,186-tag total, 236 top-level / 950 nested split, export/font
  names and 144,607-byte top-level tag stream are reproducible.
* The bounded display model reports one top-level `MenuHolder`, 71 sprites, a
  selected sprite timeline, and the local 243-glyph DefineFont3 code/advance/
  bounds/kerning data.
* The flattened display list reports **216 records**, 71 sprite definitions,
  106 sprite records, one top-level object and a maximum sprite depth of **7**.
* The clip layer finds **0 ClipDepth pairs** in the real StartMenu first frame;
  synthetic three-mask nesting intersects to `40..60`, two-mask to `30..70`,
  and an off-mask record is fully clipped.  Complex mask shapes are a named
  refusal.
* The bounded shape model reports 27 shapes, 151 paths, 1,712 shape segments,
  2,362 shape points, 243 glyphs (241 non-empty) and 5,292 glyph segments.
* The bounded tessellator turns all 27 shapes into 71,605 vertices / 35,759
  triangles / 11 line segments, with 3,118 flattened curve segments under the
  even-odd rule; shape 2's actual contour bounds are `0..522,0..520`.
* The bounded text layer lays out the four StartMenu codes 33..36 into 2,818
  vertices / 1,408 triangles with no missing glyphs, applying the 94-pair
  kerning table; the first glyph's outline bounds are `570,4460,-15440,250`.
* DefineEditText parsing collects 22 definitions.  Three nonempty plain sources
  yield 72 source glyph slots across three record instances.  Structured imports
  declare ids 34/41 from `gfxfontlib.swf`; the URL is absent and the
  `fonts_en.swf` library lacks both exact ids, so the shipped
  `Interface/fontconfig.txt` maps `$EverywhereMediumFont` -> Futura Condensed
  Normal -> DefineFont3 id 3 and `$EverywhereFont` -> Futura CondensedLight
  Normal -> DefineFont3 id 1.  All 57 records resolve, and the three labels
  produce **72 real glyph placements, 0 missing, 3,024 vertices / 4,320
  indices / 1,440 triangles**.  Every substitution is logged; no face or glyph
  is invented.  Shape + resolved text would be 60,508 vertices / 90,480
  indices, inside the 16-bit range; the fixed indexed shape census remains
  57,484 / 86,160.
* The bounded A8 rasteriser puts code 33 into 32x32 coverage with sum **41,464**
  over 182 pixels (used rect `12,1..20,31`) and into 64x64 with sum **178,003**
  over 726 pixels (used rect `24,1..40,63`); the advance 5,040 is preserved.
* The bounded atlas packs real codes 33..36 into 256x256 with used height
  **62**, **8,618** occupied glyph pixels, occupancy **35,584/65,536**, and
  exact atlas-rect coverage sums matching the raster outputs.
* The bounded batch turns those four atlas glyphs into **4 quads / 16 vertices
  / 24 indices**, bounds `1,104,1,63`; the first quad is `x=1..14,y=6..57`
  with UV `17664,0..21760,15872`, and all quads stay inside the 128x64
  viewport without overlap.
* The host COM demo creates an **8x8 D3DFMT_A8** texture with pitch **8**,
  uploads coverage byte-exactly, sets POINT/CLAMP + MODULATE(TEXTURE,DIFFUSE)
  + FVF `0x142`, and issues one DrawPrimitiveUP for **12 expanded vertices**.
  A refusing backend is `ERR_A8` with no RGBA substitution.
* The real StartMenu integration classifies all 216 flattened records,
  tessellates 53 shape records into 28,718 triangles, appends one local-font
  control text quad and submits 57,484 batch vertices / 86,160 indices as
  **one 16-bit indexed draw** (the fallback without the callback is 22 bounded
  DrawPrimitiveUP calls).  The shape layer reuses its 57,480 shared mesh
  vertices instead of expanding to 86,154.  Sprite/text/mask/stroke/style gaps
  are counted, not silently rendered.
* The AVM1 inventory decodes 87 action records / 15,549 opcodes with no
   unknown opcode, finds 53 Stop controls and no Play/Goto/SetTarget/Wait, and
   applies only frame-scoped Stop metadata to 66 of 216 display records.  No
   arbitrary ActionScript or side effect is executed.
* No texture cache or real GFx draw/frame is built, and no
   frame/pixel/Azahar/hardware result is claimed.

## 17. SWF bitmap tags (2026-11-26)

`source/menu_swf_bitmap.[ch]` decodes `DefineBitsLossless` (20) formats 3/4/5
and `DefineBitsLossless2` (36) formats 3/5 from the shared
`bsa_inflate_zlib_exact()` inflater. PIX15 is big-endian
`0RRRRRGGGGGBBBBB`; PIX24 is a 4-byte `0RGB` word, not three packed bytes;
palette rows are 4-byte aligned. `DefineBitsLossless2` ARGB is returned
premultiplied (`premultiplied_alpha=1`), the palette path is straight alpha.
Bad headers/formats/palette indices/zlib/output sizes are named refusals, and
the output is written only after validation. JPEG tags (6/21/35/90) are a named
`JPEG bitmap decode is not implemented` refusal.

Real `Update.bsa` StartMenu has no bitmap tags (`total=0`), so the logo and
background are not bitmap characters in this asset. Synthetic coverage is in
`tests/menu_swf_bitmap_smoke.c` (23 checks) plus 27 real-data checks.