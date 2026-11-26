#pragma once
/* Bounded text and shape quad-batch construction (no upload, GPU or frame).
 *
 * Text input is a MenuSwfTextMesh (one layout placement per glyph), an aligned
 * array of MenuSwfAtlasEntry (one per text glyph), a caller style and optional
 * axis-aligned viewport.  Glyphs are emitted in the text mesh's pen order.
 * Each non-empty glyph becomes four vertices / two triangles with the atlas UV
 * rect and one ARGB colour.  Empty, missing or fully clipped glyphs are omitted
 * without leaving index holes.
 *
 * Shape input is a tessellated MenuSwfTessMesh with its parsed fill table.
 * Each triangle uses its 1-based style; solid RGBA is transformed by the
 * record CXFORM and written per vertex.  Linear/radial gradients evaluate the
 * gradient matrix and up to eight stops per vertex (a documented approximation
 * for large triangles); bitmap and focal fills are honest named refusals.
 *
 * The *_clip entry points apply a MenuSwfClipRecord: fully outside glyphs or
 * triangles are culled, fully inside ones are unchanged, and partial overlap
 * is the named "partial clip-mask culling is not implemented" refusal.  No
 * vertex clamping, re-winding or vertex expansion is performed.
 *
 * Glyph bounds are taken from the text mesh's layout space and the batch style
 * is applied on top; for one final transform, lay out text with an identity
 * style.  Matrix/translation are quantised to signed 16.16 before use.  Negative
 * determinant (mirroring), NaN and degenerate scales are refused; 180-degree
 * rotations have a positive determinant and are allowed with viewport clipping
 * when both off-diagonal terms are zero.  Positions are rounded to integer pixel
 * edges with no +0.5 bias; UVs map atlas texel edges to those edges, which is
 * the half-pixel rule used by the raster/atlas path.  Winding is positive in
 * x-right/y-down coordinates (TL, TR, BR, BL).
 *
 * The required counts are computed before any write.  A capacity overflow
 * returns MENU_SWF_BATCH_ERR_OVERFLOW and leaves both buffers untouched.
 * No allocation, texture, cache or GPU call is performed.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf_atlas.h"
#include "menu_swf_clip.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"

#define MENU_SWF_BATCH_MAX_QUADS 4096u
#define MENU_SWF_BATCH_MAX_VERTICES 16384u
#define MENU_SWF_BATCH_MAX_INDICES 24576u
#define MENU_SWF_BATCH_MAX_SHAPE_VERTICES 262144u
#define MENU_SWF_BATCH_MAX_SHAPE_INDICES 786432u

typedef enum {
    MENU_SWF_BATCH_OK = 0,
    MENU_SWF_BATCH_ERR_ARGUMENT,
    MENU_SWF_BATCH_ERR_OVERFLOW,
    MENU_SWF_BATCH_ERR_NAN,
    MENU_SWF_BATCH_ERR_SCALE,
    MENU_SWF_BATCH_ERR_CLIP,
    MENU_SWF_BATCH_ERR_FILL,
    MENU_SWF_BATCH_ERR_MASK
} MenuSwfBatchStatus;

typedef struct {
    /* Matrix is quantised to signed 16.16; translation is in output pixels. */
    double m00, m01, m10, m11;
    double tx, ty;
    uint32_t color; /* default ARGB for glyphs without a per-run colour */
    /* Optional record CXFORM applied to the chosen per-glyph/run colour. */
    uint8_t has_color_transform;
    int32_t color_mult[4]; /* 8.8, 256 = 1.0 */
    int32_t color_add[4];
} MenuSwfBatchStyle;

/* Shape batch inputs: the record matrix is 16.16 with twip translation, and
 * the CXFORM values use the display-list 8.8 mult / add convention.  `u`/`v`
 * are the white-texel UVs written to every vertex. */
typedef struct {
    int32_t m00, m01, m10, m11; /* 16.16 */
    int32_t tx, ty;             /* twips */
    int32_t mult[4];            /* 8.8, 256 = 1.0 */
    int32_t add[4];
    uint32_t u, v;              /* 16.16 white-texel UV */
} MenuSwfBatchShapeStyle;

typedef struct {
    uint8_t enabled;
    int32_t x0, y0, x1, y1; /* half-open output pixel rect */
} MenuSwfBatchViewport;

typedef struct {
    int32_t x, y;
    uint32_t u, v; /* 16.16 atlas UV */
    uint32_t color;
} MenuSwfBatchVertex;

typedef struct {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t quad_count;
    uint32_t triangle_count;
    MenuSwfShapeRect bounds; /* output pixel bounds, x1/y1 exclusive */
    uint8_t has_bounds;
} MenuSwfBatchResult;

const char *menu_swf_batch_status_name(MenuSwfBatchStatus status);

/* `atlas_entries[i]` belongs to `text->glyphs[i]`; `atlas_count` must cover
 * `text->glyph_count`.  `viewport` may be NULL (no clipping).  Buffers must be
 * non-NULL only when their capacity is non-zero. */
MenuSwfBatchStatus menu_swf_batch_text(
    const MenuSwfTextMesh *text, const MenuSwfAtlasEntry *atlas_entries,
    uint32_t atlas_count, const MenuSwfBatchStyle *style,
    const MenuSwfBatchViewport *viewport, MenuSwfBatchVertex *vertices,
    uint32_t vertex_capacity, uint32_t *indices, uint32_t index_capacity,
    MenuSwfBatchResult *out, char *reason, size_t reason_cap);

/* Clip-mask variants.  `clip` is the MenuSwfClipRecord for this display-list
 * record (NULL or mask_count==0 means no clip).  A fully clipped record emits
 * nothing; a glyph/triangle wholly inside the intersected clip rectangle is
 * kept; a wholly outside one is culled.  A partially overlapping glyph/triangle
 * is the named refusal "partial clip-mask culling is not implemented"; this
 * layer never clamps or re-winds geometry.  `clip->clip_rect` is in screen
 * twips and is compared against the integer output AABB (*20). */
MenuSwfBatchStatus menu_swf_batch_text_clip(
    const MenuSwfTextMesh *text, const MenuSwfAtlasEntry *atlas_entries,
    uint32_t atlas_count, const MenuSwfBatchStyle *style,
    const MenuSwfBatchViewport *viewport, const MenuSwfClipRecord *clip,
    MenuSwfBatchVertex *vertices, uint32_t vertex_capacity, uint32_t *indices,
    uint32_t index_capacity, MenuSwfBatchResult *out, char *reason,
    size_t reason_cap);

/* Build a triangle batch from a tessellated shape plus its fill table.
 * Every triangle uses the style named by mesh->triangle_fills[] (1-based).
 * Solid fills are transformed by style->mult/add and written as per-vertex
 * ARGB.  Linear gradients project the source vertex through the inverse fill
 * matrix, normalize x from the gradient square (-16384..16384), apply the
 * SWF spread mode (pad/reflect/repeat) and interpolate the stored stops;
 * radial gradients use the distance from the gradient center divided by
 * 16384.  Colours are per-vertex, so large triangles are an approximation.
 * Focal gradients and bitmap fills return MENU_SWF_BATCH_ERR_FILL with named
 * reasons.  A negative determinant mirrors and
 * reverses each triangle's winding; zero is ERR_SCALE.  The batch reuses
 * mesh->vertices[]/indices[]: a source vertex shared by triangles with
 * different fills is copied once and its index remapped.  The caller capacity
 * must cover vertex_count + index_count output vertices; that worst case is
 * checked before any write.  Indices are local to this batch (add the base
 * when appending).  A single 16-bit indexed draw additionally requires at most
 * 65,536 output vertices. */
MenuSwfBatchStatus menu_swf_batch_shape(
    const MenuSwfTessMesh *mesh, const MenuSwfBatchShapeStyle *style,
    const MenuSwfBatchViewport *viewport, MenuSwfBatchVertex *vertices,
    uint32_t vertex_capacity, uint32_t *indices, uint32_t index_capacity,
    MenuSwfBatchResult *out, char *reason, size_t reason_cap);

/* Shape clip-mask variant.  Fully clipped triangles are culled; fully inside
 * triangles keep the existing winding/colour/dedup; a partially overlapping
 * triangle is the named "partial clip-mask culling is not implemented"
 * refusal.  A fully clipped record returns an empty result without writing. */
MenuSwfBatchStatus menu_swf_batch_shape_clip(
    const MenuSwfTessMesh *mesh, const MenuSwfBatchShapeStyle *style,
    const MenuSwfBatchViewport *viewport, const MenuSwfClipRecord *clip,
    MenuSwfBatchVertex *vertices, uint32_t vertex_capacity, uint32_t *indices,
    uint32_t index_capacity, MenuSwfBatchResult *out, char *reason,
    size_t reason_cap);
