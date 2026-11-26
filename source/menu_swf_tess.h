#pragma once
/* Bounded tessellation of SWF shape paths into triangles and line segments.
 *
 * The input is one DefineShape/2/3/4 character from a validated SWF.  The
 * output is a caller-owned vertex/index pool plus a separate list of stroked
 * line segments.  Curves are flattened adaptively, both even-odd and non-zero
 * fill rules are supported, and holes are resolved by the chosen rule.  When
 * the caller supplies a fill buffer, the shape's parsed fill styles are copied
 * there and triangle_fills carries global 1-based style indices; after a
 * StateNewStyles record later style arrays continue at the next index.
 *
 * No rasterisation, atlas, AVM or frame is produced.  The tessellator itself
 * allocates nothing: all output and scratch memory comes from the caller.  The
 * shared SWF view may decompress a CWS body into its existing temporary buffer.
 * Triangles are emitted with positive signed area in the stored coordinates
 * (x right, y down); fill style values are the shape's own 1-based indices.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf_shape.h"

#define MENU_SWF_TESS_MAX_VERTICES 65536u
#define MENU_SWF_TESS_MAX_INDICES 196608u
#define MENU_SWF_TESS_MAX_TRIANGLES 65536u
#define MENU_SWF_TESS_MAX_LINES 65536u
#define MENU_SWF_TESS_MAX_EDGES 65536u
#define MENU_SWF_TESS_MAX_BANDS 131072u
#define MENU_SWF_TESS_MAX_CONTOURS 4096u
#define MENU_SWF_TESS_MAX_CURVE_SEGMENTS 65536u
#define MENU_SWF_TESS_CURVE_DEPTH 8u
#define MENU_SWF_TESS_FLATNESS 2 /* twips, max second difference */

typedef enum {
    MENU_SWF_TESS_EVEN_ODD = 0,
    MENU_SWF_TESS_NON_ZERO = 1
} MenuSwfTessFillRule;

/* Parsed fill-style metadata copied from the shape's FILLSTYLEARRAY. */
typedef MenuSwfShapeFill MenuSwfTessFill;

typedef struct {
    int32_t x, y; /* twips */
} MenuSwfTessVertex;

typedef struct {
    int32_t x0, y0, x1, y1;
    uint16_t line_style;
} MenuSwfTessLine;

/* Scratch edge.  Fields are private to the implementation; expose the type so
 * the caller can size the array. */
typedef struct {
    int32_t x0, y0, x1, y1; /* y0 < y1 after canonicalisation */
    int32_t xtop, xbot, xmid;
    int16_t dir;            /* +1 downward, -1 upward in the source contour */
    uint16_t fill0, fill1;
    uint8_t active;
} MenuSwfTessEdge;

typedef struct {
    /* Caller-provided output buffers.  A NULL pointer is accepted only with a
     * zero capacity; a smaller capacity becomes a named budget refusal. */
    MenuSwfTessVertex *vertices;
    uint32_t vertex_capacity;
    uint32_t *indices;
    uint32_t index_capacity;       /* number of uint32_t entries, not triangles */
    uint16_t *triangle_fills;      /* one 1-based style index per triangle;
                                    * global across StateNewStyles arrays */
    uint32_t triangle_capacity;
    MenuSwfTessLine *lines;
    uint32_t line_capacity;

    /* Optional caller-owned fill-style table.  When `fills` is non-NULL and
     * `fill_capacity` is non-zero, the tessellator copies the shape's parsed
     * styles here in 1-based order so a batch can colour triangles. */
    MenuSwfTessFill *fills;
    uint32_t fill_capacity;

    /* Caller-provided scratch.  `active_capacity` must cover the peak active
     * edge count and `band_capacity` the two endpoints of every edge. */
    MenuSwfTessEdge *edges;
    uint32_t edge_capacity;
    uint32_t *active;
    uint32_t active_capacity;
    int32_t *bands;
    uint32_t band_capacity;

    /* Output, filled by menu_swf_tess_shape(). */
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t line_count;
    uint32_t contour_count;
    uint32_t edge_count;
    uint32_t curve_segments;
    uint32_t fill_count;   /* entries written to fills[] */
    MenuSwfShapeRect bounds;
    uint8_t has_bounds;
    uint8_t fill_rule;
} MenuSwfTessMesh;

/* Tessellate one shape character.  `mesh` supplies all buffers and their
 * capacities; the count/bounds fields are overwritten.  A missing character is
 * MENU_SWF_ERR_ARGUMENT, a malformed shape propagates the shape-layer status,
 * and any hard/caller limit is MENU_SWF_ERR_BUDGET with a named reason.
 * `mesh->fill_rule` reports the rule actually applied. */
MenuSwfStatus menu_swf_tess_shape(const uint8_t *data, size_t size,
                                  uint16_t shape_id,
                                  MenuSwfTessFillRule fill_rule,
                                  MenuSwfTessMesh *mesh, char *reason,
                                  size_t reason_cap);

/* Tessellate one DefineFont2/3 glyph SHAPE by font id and character code.
 * Glyph coordinates stay in raw font units (Font3 uses 20x units); the caller
 * applies layout scale/transform.  Glyph fills are tagged with style 1 and no
 * stroke segments are emitted.  An unknown font/code is E_ARGUMENT.  Output
 * and scratch buffers are the same caller-owned fields as the shape path. */
MenuSwfStatus menu_swf_tess_glyph(const uint8_t *data, size_t size,
                                  uint16_t font_id, uint16_t code,
                                  MenuSwfTessFillRule fill_rule,
                                  MenuSwfTessMesh *mesh, char *reason,
                                  size_t reason_cap);
