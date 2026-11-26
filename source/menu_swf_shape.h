#pragma once
/* Bounded SWF shape and glyph geometry, without rasterisation or a frame.
 *
 * DefineShape/2/3/4 are parsed into path summaries in twips (1/20 px):
 * start/end, point/segment counts, closed flag, conservative bounds and the
 * fill0/fill1/line style indices active at the path start.  Fill/line style
 * arrays are validated and retained: solid RGBA, gradient matrix/stops, and
 * bitmap character id.  DefineFont2/3 glyph SHAPE records are linked to their
 * code table and reduced to per-glyph path summaries.
 *
 * No pixels, atlas, AVM or renderable display list are produced.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf.h"

#define MENU_SWF_SHAPE_MAX_SHAPES 256u
#define MENU_SWF_SHAPE_MAX_PATHS 512u
#define MENU_SWF_SHAPE_MAX_GLYPHS 256u
#define MENU_SWF_SHAPE_MAX_GLYPH_PATHS 1024u
#define MENU_SWF_SHAPE_MAX_SEGMENTS 4096u
#define MENU_SWF_SHAPE_MAX_STYLES 256u
#define MENU_SWF_SHAPE_MAX_FILLS 1024u
#define MENU_SWF_SHAPE_MAX_STOPS 8u
#define MENU_SWF_SHAPE_MAX_DEPTH 16u

typedef enum {
    MENU_SWF_FILL_SOLID = 0,
    MENU_SWF_FILL_LINEAR = 1,
    MENU_SWF_FILL_RADIAL = 2,
    MENU_SWF_FILL_FOCAL = 3,
    MENU_SWF_FILL_BITMAP = 4
} MenuSwfFillKind;

/* SWF MATRIX fields, kept in their on-disk fixed-point units. */
typedef struct {
    int32_t scale_x, scale_y;       /* 16.16 */
    int32_t rotate_skew0, rotate_skew1; /* 16.16 */
    int32_t translate_x, translate_y;   /* twips */
} MenuSwfShapeMatrix;

typedef struct {
    uint8_t ratio;
    uint8_t r, g, b, a;
} MenuSwfShapeGradientStop;

/* One parsed FILLSTYLE.  Solid fills carry RGBA; gradients retain their
 * matrix and up to MENU_SWF_SHAPE_MAX_STOPS color records; bitmap fills keep
 * the opaque bitmap character id.  `stop_total` is the advertised count even
 * when only the first stops were stored. */
typedef struct {
    uint32_t rgba;       /* 0xAARRGGBB for solid fills */
    uint16_t bitmap_id;
    uint8_t kind;        /* MenuSwfFillKind */
    uint8_t has_alpha;   /* shape tag stores alpha channels */
    uint8_t stop_count;  /* stored gradient stops */
    uint8_t stop_total;  /* advertised gradient stops */
    uint8_t spread;      /* gradient spread mode */
    uint8_t interpolation;
    uint8_t bitmap_repeat;
    uint8_t focal_present;
    int16_t focal;       /* 8.8 fixed point */
    MenuSwfShapeMatrix matrix;
    MenuSwfShapeGradientStop stops[MENU_SWF_SHAPE_MAX_STOPS];
} MenuSwfShapeFill;

typedef struct {
    int32_t xmin, xmax, ymin, ymax; /* twips */
} MenuSwfShapeRect;

typedef struct {
    int32_t start_x, start_y;
    int32_t end_x, end_y;
    int32_t xmin, xmax, ymin, ymax;
    uint32_t point_count;
    uint32_t segment_count;
    uint16_t fill0, fill1, line;
    uint8_t closed;
    uint8_t has_curve;
    uint8_t styles_changed;
} MenuSwfShapePath;

typedef struct {
    uint16_t shape_id;
    uint8_t tag_code;
    MenuSwfShapeRect bounds;
    MenuSwfShapeRect edge_bounds;
    uint8_t has_edge_bounds;
    uint8_t uses_fill_winding;
    uint8_t uses_non_scaling_strokes;
    uint8_t uses_scaling_strokes;
    uint16_t fill_count;
    uint16_t line_count;
    uint16_t solid_fills;
    uint16_t gradient_fills;
    uint16_t linear_fills;
    uint16_t radial_fills;
    uint16_t focal_fills;
    uint16_t bitmap_fills;
    uint16_t solid_lines;
    uint16_t gradient_lines;
    uint16_t bitmap_lines;
    uint32_t fill_first;    /* index into shape_fills[] */
    uint16_t stored_fills;  /* fills retained for the current style array */
    uint32_t path_count;
    uint32_t stored_paths;
    uint32_t path_first;
    uint32_t point_count;
    uint32_t segment_count;
    uint32_t style_changes;
    int truncated;
} MenuSwfShapeSummary;

typedef struct {
    uint16_t font_id;
    uint16_t glyph_index;
    uint16_t code;
    uint32_t shape_offset;
    MenuSwfShapeRect bounds;
    uint32_t path_count;
    uint32_t stored_paths;
    uint32_t path_first;
    uint32_t point_count;
    uint32_t segment_count;
    uint8_t has_geometry;
    int truncated;
} MenuSwfShapeGlyph;

typedef struct {
    uint32_t shape_total;
    uint32_t stored_shapes;
    MenuSwfShapeSummary shapes[MENU_SWF_SHAPE_MAX_SHAPES];
    MenuSwfShapePath shape_paths[MENU_SWF_SHAPE_MAX_PATHS];
    uint32_t shape_path_used;
    MenuSwfShapeFill shape_fills[MENU_SWF_SHAPE_MAX_FILLS];
    uint32_t shape_fill_used;
    uint32_t font_total;
    uint32_t stored_fonts;
    uint32_t glyph_total;
    uint32_t stored_glyphs;
    MenuSwfShapeGlyph glyphs[MENU_SWF_SHAPE_MAX_GLYPHS];
    MenuSwfShapePath glyph_paths[MENU_SWF_SHAPE_MAX_GLYPH_PATHS];
    uint32_t glyph_path_used;
    int truncated;
} MenuSwfShapeResult;

/* Streaming geometry sink for menu_swf_shape_walk().  Every coordinate is in
 * twips.  A path starts at its absolute move-to position and keeps the
 * fill0/fill1/line style indices active at that point.  Curves give the
 * absolute control point and absolute anchor.  The callback must copy data it
 * wants to retain; pointers are only valid during the call. */
typedef struct {
    void *user;
    void (*begin_shape)(void *user, uint16_t shape_id, uint8_t tag_code);
    void (*end_shape)(void *user);
    /* Called before each FILLSTYLEARRAY (including StateNewStyles).  Style
     * indices restart at 1. */
    void (*begin_fill_styles)(void *user, uint16_t count);
    /* One callback per parsed fill style.  `index` is 1-based. */
    void (*fill_style)(void *user, uint16_t index,
                       const MenuSwfShapeFill *fill);
    void (*begin_path)(void *user, int32_t x, int32_t y, uint16_t fill0,
                       uint16_t fill1, uint16_t line);
    void (*line_to)(void *user, int32_t x, int32_t y);
    void (*curve_to)(void *user, int32_t cx, int32_t cy, int32_t ax,
                     int32_t ay);
    void (*end_path)(void *user);
} MenuSwfShapeSink;

/* Streaming glyph geometry sink for menu_swf_shape_walk_fonts().  Glyph
 * coordinates and path events use the same twip/20x units as the font SHAPE
 * records.  `begin_font`/`end_font` frame every DefineFont2/3 tag;
 * `begin_glyph`/`end_glyph` frame each glyph's contours. */
typedef struct {
    void *user;
    void (*begin_font)(void *user, uint16_t font_id, uint8_t tag_code,
                       uint16_t num_glyphs, uint8_t flags);
    void (*begin_glyph)(void *user, uint16_t glyph_index, uint16_t code,
                        uint32_t shape_offset);
    void (*end_glyph)(void *user);
    void (*end_font)(void *user);
    void (*begin_path)(void *user, int32_t x, int32_t y, uint16_t fill0,
                       uint16_t fill1, uint16_t line);
    void (*line_to)(void *user, int32_t x, int32_t y);
    void (*curve_to)(void *user, int32_t cx, int32_t cy, int32_t ax,
                     int32_t ay);
    void (*end_path)(void *user);
    /* Optional target filter.  When `filter` is nonzero only the glyph with
     * this font id and character code is decoded; other glyphs are framed
     * with begin/end_glyph but their contours are skipped.  This keeps a
     * single-glyph lookup from failing on unrelated complex glyphs in the
     * same font library. */
    uint8_t filter;
    uint16_t want_font_id;
    uint16_t want_code;
} MenuSwfFontSink;

/* Find a parsed shape by character id (NULL when absent). */
const MenuSwfShapeSummary *menu_swf_shape_find(const MenuSwfShapeResult *result,
                                               uint16_t shape_id);

/* Return the 1-based fill style for a shape, or NULL when the index is out of
 * range / the style array was truncated. */
const MenuSwfShapeFill *menu_swf_shape_fill(
    const MenuSwfShapeResult *result, const MenuSwfShapeSummary *shape,
    uint16_t index);

/* Stream shape geometry to `sink` without building a summary.  Only
 * DefineShape/2/3/4 tags are decoded; glyph and font tags are skipped.  The
 * same tag, style, record, segment and nesting refusals as the summary apply.
 * Callbacks are optional individually, but `sink` itself must be non-NULL. */
MenuSwfStatus menu_swf_shape_walk(const uint8_t *data, size_t size,
                                  const MenuSwfShapeSink *sink,
                                  char *reason, size_t reason_cap);

/* Stream DefineFont2/3 glyph metadata and contours.  Shape tags are skipped.
 * The same glyph/segment/limit and malformed-record refusals as the summary
 * apply; DefineFont4 and AVM2 remain named refusals. */
MenuSwfStatus menu_swf_shape_walk_fonts(const uint8_t *data, size_t size,
                                        const MenuSwfFontSink *sink,
                                        char *reason, size_t reason_cap);

/* Parse all top-level and nested DefineShape tags and all DefineFont2/3 glyph
 * shapes.  Named refusals: unsupported fill/line type, bitmap fill without id,
 * malformed/truncated style arrays or records, segment/shape/glyph limits,
 * DefineFont4 and DoABC/AVM2.  `out` is cleared on failure. */
MenuSwfStatus menu_swf_shape_summary(const uint8_t *data, size_t size,
                                     MenuSwfShapeResult *out, char *reason,
                                     size_t reason_cap);
