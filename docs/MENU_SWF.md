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
| `source/menu_swf_display.h` / `.c` | Bounded PlaceObject2/3 + RemoveObject display state, per-ShowFrame snapshots, sprite summaries, selected-sprite frames, DefineFont2/3 metrics and kerning pairs |
| `source/menu_swf_shape.h` / `.c` | DefineShape/2/3/4 fill/line styles and shape records, path summaries, streaming geometry sink, and DefineFont2/3 glyph SHAPE contours/bounds |
| `source/menu_swf_tess.h` / `.c` | Bounded even-odd/non-zero tessellation of one shape or DefineFont2/3 glyph into caller-owned vertex/index/triangle-style pools plus separate stroked line segments |
| `source/menu_swf_text.h` / `.c` | Latin-1 layout with advances/kerning, per-glyph placement, glyph tessellation through `menu_swf_tess`, 16.16 transform, ARGB vertices and per-glyph triangle ranges |
| `source/menu_swf_raster.h` / `.c` | Bounded A8 coverage rasterisation of tessellated glyph triangles with 4x4 subpixel samples, caller buffers, 16.16-quantised matrix/pen and named refusals |
| `source/menu_swf_atlas.h` / `.c` | Deterministic shelf packing of A8 coverage rectangles into a caller atlas, zero-filled padding, per-glyph rect/UV and named refusals |
| `source/menu_swf_batch.h` / `.c` | Text quad/index batch from layout placements + atlas UVs, 16.16 style transform, optional axis-aligned viewport clipping and count-before-write overflow |
| `source/d3d9_menu_text.h` / `.c` | Host/menu COM demo: A8 texture creation/upload and one DrawPrimitiveUP from the atlas+batch |
| `tests/menu_swf_smoke.c` | Synthetic FWS/CWS fixtures, short/long tags, truncation/overflow, bad zlib, declared mismatch, ZWS, missing End; optional real StartMenu read-only check |
| `tests/menu_swf_display_smoke.c` | Synthetic PlaceObject2/3, RemoveObject2, sprite, ShowFrame and DefineFont3 fixtures; optional real display/font summary |
| `tests/menu_swf_shape_smoke.c` | Synthetic rectangle/curve/two-style/glyph shapes plus truncation, bitmap-id, segment-limit and AVM2/DefineFont4 refusals; optional real shape/glyph summary |
| `tests/menu_swf_tess_smoke.c` | Synthetic square in both orientations, even-odd hole, non-zero star/nested contours, curve, multiple styles, stroked square, empty contour, overflow and missing-shape refusals; optional real tessellation summary |
| `tests/menu_swf_text_smoke.c` | Synthetic 3-glyph font with kerning, direct glyph tessellation, placement/transform/colour, empty/unknown glyph and overflow; optional real StartMenu text |
| `tests/menu_swf_raster_smoke.c` | Synthetic square/triangle exact coverage, hole rules, subpixel/rotation/mirror/clip, empty glyph and named refusals; optional real code-33 32x32 and 64x64 coverage |
| `tests/menu_swf_atlas_smoke.c` | Synthetic varied-size pack/determinism/UV/composition/overflow/empty cases; optional real codes 33..36 coverage-sum comparison |
| `tests/menu_swf_batch_smoke.c` | Synthetic 3-glyph positions/UV/indices/winding/empty/viewport/overflow and optional real 33..36 atlas batch |
| `tests/menu_text_draw_smoke.c` | COM demo: 2-glyph 8x8 A8 texture/upload, sampler/TSS/FVF, one DrawPrimitiveUP, empty/overflow and A8-refusal cases |
| `tools/run_host_tests.ps1` | Registers `menu_swf`, `menu_swf_display`, `menu_swf_shape`, `menu_swf_tess`, `menu_swf_text`, `menu_swf_raster`, `menu_swf_atlas`, `menu_swf_batch` and `menu_text_draw`; `menu_bsa` now links `menu_swf.c` |
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
  layout-space glyph bounds.  Every vertex carries the caller's ARGB colour.
  A zero or negative determinant is `ERR_SCALE`; non-finite terms are
  `ERR_NAN`.
* Positions are rounded to integer pixel edges with no +0.5 bias.  Atlas UVs
  map texel edges to those quad edges, which is the half-pixel convention used
  by the raster/atlas path.  Positive-area winding is maintained in
  x-right/y-down coordinates.
* An optional half-open viewport rect clips axis-aligned glyph rectangles;
  UVs are interpolated over the clipped fraction.  Fully clipped glyphs are
  omitted.  A rotated/skewed matrix with viewport clipping is the named
  `ERR_CLIP` rather than a silently wrong AABB clip.
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
* `menu_text_draw_batch()` expands the indexed batch into 24-byte
  `XYZ|DIFFUSE|TEX1` vertices (float3 position, D3DCOLOR, float2 UV), then:
  - `SetTexture(0, texture)`;
  - sampler stage 0: MIN/MAG POINT, MIP NONE, ADDRESS U/V CLAMP;
  - stage 0 COLOROP/ALPHAOP = MODULATE, ARG1 = TEXTURE, ARG2 = DIFFUSE;
  - `SetFVF(0x142)`;
  - exactly one `DrawPrimitiveUP(TRIANGLELIST)`.
  The caller must have an active BeginScene; the module does not claim a frame.
* Empty batches, invalid indices, more than 1,365 triangles, scratch overflow,
  state/FVF/draw failures and A8 creation/upload failures are named.  The API
  never logs a successful batch as a rendered frame.

Measured COM-demo result (2 synthetic glyphs, 8x8 atlas, 8x8 A8 texture):

```text
texture format=28 width=8 height=8 pitch=8, lock bytes == atlas coverage
draw: 12 expanded vertices, 1 DrawPrimitiveUP, FVF=0x142
state: POINT/CLAMP, COLOROP=MODULATE, ARG1=TEXTURE, ARG2=DIFFUSE
```

The test also verifies a texture-create refusal is `ERR_A8`, an empty batch is
`ERR_EMPTY`, and an oversized index count is `ERR_OVERFLOW`.  There is no
sampling result, frame, or GPU pixel claim.

## 13. Tests

Synthetic-only:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_batch,menu_swf_atlas,menu_swf_raster,menu_swf_text,menu_swf_tess,menu_swf_shape,menu_swf_display,menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf_batch` **21 checks, 0 failures**; `menu_swf_atlas` **27
checks, 0 failures**; `menu_swf_raster` **44 checks, 0 failures**;
`menu_swf_text` **33 checks, 0 failures**; `menu_swf_tess` **47 checks, 0
failures**; `menu_swf_shape` **28 checks, 0 failures**; `menu_swf_display`
**38 checks, 0 failures**; `menu_swf` **86 checks, 0 failures**; `menu_bsa`
**277 checks, 0 failures**.

Real read-only:

```powershell
$env:SKYRIM_N3DS_REAL_DATA='F:\Skyrim\Data'
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only menu_swf_batch,menu_swf_atlas,menu_swf_raster,menu_swf_text,menu_swf_tess,menu_swf_shape,menu_swf_display,menu_swf,menu_bsa `
  -Python C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe
```

Result: `menu_swf_batch` **34 checks, 0 failures**; `menu_swf_atlas` **37
checks, 0 failures**; `menu_swf_raster` **60 checks, 0 failures**;
`menu_swf_text` **46 checks, 0 failures**; `menu_swf_tess` **55 checks, 0
failures**; `menu_swf_shape` **40 checks, 0 failures**; `menu_swf_display`
**57 checks, 0 failures**; `menu_swf` **102 checks, 0 failures**; `menu_bsa`
**286 checks, 0 failures**.

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
limit and AVM2/DefineFont4 refusals.  The tessellation test adds a 4-vertex /
2-triangle square in both contour orientations, an even-odd hole, a non-zero
star and same-direction nested contours, an adaptive curve, two fill styles, a
stroked square, an empty contour, vertex/index/edge overflow and a missing
shape; the real check tessellates all 27 shapes.  The text test adds a 3-glyph
font with an A/V kerning pair, direct glyph tessellation, placement/colour and
half-scale layout, an empty space glyph, an unknown code, and vertex/index/
glyph/scratch/length overflow; the real check lays out four StartMenu glyphs
and verifies one real kerning pair.  The raster test adds exact full-coverage
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
rotated-clip refusals, and real 33..36 atlas quads.

Full host suite with the main-tree generated slice and IR container:
**113 passed, 1 diagnostic (`tesv_slice`), 0 failed**.  `menu_swf_batch`
contributes 21 synthetic checks, `menu_swf_atlas` 27, `menu_swf_raster` 44,
`menu_swf_text` 33, `menu_swf_tess` 47, `menu_swf_shape` 28,
`menu_swf_display` 38 and `menu_swf` 86; `menu_text_draw` is an assertion-based
COM test outside that real-data matrix.  No real-data checks are run in that
suite.

## 14. What remains

1. **Renderable display list** — bounded state, shape geometry and triangle
   tessellation exist, but this is not a GFx display list: sprite/movie-clip
   materialisation, nested transforms, masks, clipping layers, button states and
   frame scripts remain.
2. **AVM/AS2** — action blocks, constants, function calls, event handlers and
   the menu's script behaviour.  `DoAction`/clip-action payloads are not
   executed.
3. **Atlas cache/real GFx fonts** — `fonts_en.swf`/`fonts_console.swf`,
   `FontConfig.txt`, texture cache/rehydration, richer shaping (ligatures,
   bidi, fallback fonts) and text metrics beyond advance+kerning.  The host
   COM demo can create/upload an A8 texture and draw one quad batch; there is
   no persistent cache, PICA A8 sampling or real GFx text object yet.
4. **NIF/DDS/shaders** — exported DDS/NIF assets, ETC1 conversion, logo/fog
   meshes and their shader closure.
5. **Draw path** — real GFx display list/masks, dynamic vertex buffer/FVF,
   TEV/PICA submission and the first real frame.  The host COM DrawPrimitiveUP
   is not a frame claim.

## 15. Integration

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

The summary/display/shape/tess/text/raster/atlas/batch structures are metadata,
CPU geometry, A8 coverage/atlas memory and CPU vertex/index data only.  They
must not be reported as a parsed movie, renderable display list or frame.

## 16. Honest claim

* StartMenu.swf is validated as one CWS/zlib stream with the declared size,
  frame header and complete top-level tag stream through End.
* The measured 1,186-tag total, 236 top-level / 950 nested split, export/font
  names and 144,607-byte top-level tag stream are reproducible.
* The bounded display model reports one top-level `MenuHolder`, 71 sprites, a
  selected sprite timeline, and the local 243-glyph DefineFont3 code/advance/
  bounds/kerning data.
* The bounded shape model reports 27 shapes, 151 paths, 1,712 shape segments,
  2,362 shape points, 243 glyphs (241 non-empty) and 5,292 glyph segments.
* The bounded tessellator turns all 27 shapes into 71,605 vertices / 35,759
  triangles / 11 line segments, with 3,118 flattened curve segments under the
  even-odd rule; shape 2's actual contour bounds are `0..522,0..520`.
* The bounded text layer lays out the four StartMenu codes 33..36 into 2,818
  vertices / 1,408 triangles with no missing glyphs, applying the 94-pair
  kerning table; the first glyph's outline bounds are `570,4460,-15440,250`.
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
* No AVM action is executed, no texture cache or real GFx draw/frame is built,
  and no frame/pixel/Azahar/hardware result is claimed.
