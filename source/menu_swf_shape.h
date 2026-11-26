#pragma once
/* Bounded SWF shape and glyph geometry, without rasterisation or a frame.
 *
 * DefineShape/2/3/4 are parsed into path summaries in twips (1/20 px):
 * start/end, point/segment counts, closed flag, conservative bounds and the
 * fill0/fill1/line style indices active at the path start.  Fill/line style
 * arrays are validated and counted by kind (solid/gradient/bitmap); bitmap
 * fills keep their opaque bitmap id.  DefineFont2/3 glyph SHAPE records are
 * linked to their code table and reduced to per-glyph path summaries.
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
#define MENU_SWF_SHAPE_MAX_DEPTH 16u

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
    uint16_t bitmap_fills;
    uint16_t solid_lines;
    uint16_t gradient_lines;
    uint16_t bitmap_lines;
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
    uint32_t font_total;
    uint32_t stored_fonts;
    uint32_t glyph_total;
    uint32_t stored_glyphs;
    MenuSwfShapeGlyph glyphs[MENU_SWF_SHAPE_MAX_GLYPHS];
    MenuSwfShapePath glyph_paths[MENU_SWF_SHAPE_MAX_GLYPH_PATHS];
    uint32_t glyph_path_used;
    int truncated;
} MenuSwfShapeResult;

/* Find a parsed shape by character id (NULL when absent). */
const MenuSwfShapeSummary *menu_swf_shape_find(const MenuSwfShapeResult *result,
                                               uint16_t shape_id);

/* Parse all top-level and nested DefineShape tags and all DefineFont2/3 glyph
 * shapes.  Named refusals: unsupported fill/line type, bitmap fill without id,
 * malformed/truncated style arrays or records, segment/shape/glyph limits,
 * DefineFont4 and DoABC/AVM2.  `out` is cleared on failure. */
MenuSwfStatus menu_swf_shape_summary(const uint8_t *data, size_t size,
                                     MenuSwfShapeResult *out, char *reason,
                                     size_t reason_cap);
