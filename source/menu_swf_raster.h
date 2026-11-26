#pragma once
/* Bounded A8 coverage rasterisation of tessellated glyph triangles.
 *
 * The input is a MenuSwfTessMesh (normally from menu_swf_tess_glyph()).
 * Triangles are already filled by the tessellator's non-zero (or even-odd)
 * rule, so they are non-overlapping; this module unions them into a caller
 * A8 buffer.  A 4x4 sample grid per pixel (16 samples) gives sub-pixel
 * coverage.  The matrix is quantised to signed 16.16 before use.  Negative
 * scales/mirrors are valid; a zero-determinant matrix is refused.
 *
 * No allocation is performed.  The buffer is cleared (width bytes per row,
 * honouring stride) and every write stays inside width x height.  There is no
 * atlas, upload, cache, GPU call or frame.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf_tess.h"

#define MENU_SWF_RASTER_MAX_DIM 4096u
#define MENU_SWF_RASTER_MAX_STRIDE (1u << 20)
#define MENU_SWF_RASTER_MAX_TRIANGLES 1024u
#define MENU_SWF_RASTER_SAMPLES 16u
#define MENU_SWF_RASTER_SUBPIXEL 256 /* fixed-point units per pixel */

typedef enum {
    MENU_SWF_RASTER_OK = 0,
    MENU_SWF_RASTER_ERR_ARGUMENT,
    MENU_SWF_RASTER_ERR_TOO_SMALL,
    MENU_SWF_RASTER_ERR_OVERFLOW,
    MENU_SWF_RASTER_ERR_NAN,
    MENU_SWF_RASTER_ERR_SCALE,
    MENU_SWF_RASTER_ERR_NEGATIVE_SCALE
} MenuSwfRasterStatus;

typedef struct {
    /* Matrix terms are quantised to signed 16.16; translation is in output
     * pixels and also quantised to 16.16.  Pen is in raw font units and is
     * added to each vertex before the matrix. */
    double m00, m01, m10, m11;
    double tx, ty;
    int32_t pen_x, pen_y;
} MenuSwfRasterTransform;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per row, >= width */
    uint8_t *pixels;
    int32_t advance; /* declared glyph advance, echoed into the result */
    /* Mirrors are valid by default.  Set this to refuse a negative
     * determinant with MENU_SWF_RASTER_ERR_NEGATIVE_SCALE. */
    uint8_t reject_negative_scale;
} MenuSwfRasterTarget;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t x0, y0, x1, y1; /* used rectangle, x1/y1 exclusive */
    uint64_t coverage_sum;
    uint32_t covered_pixels;
    uint8_t max_coverage;
    uint8_t has_coverage;
    int32_t advance;
} MenuSwfRasterResult;

const char *menu_swf_raster_status_name(MenuSwfRasterStatus status);

/* Rasterise every indexed triangle in `mesh` into target->pixels.  The target
 * is cleared first; rows use target->stride and only target->width bytes are
 * touched.  `out` receives the used rectangle, coverage sum, covered-pixel
 * count, max coverage and the echoed advance.  Empty meshes are valid and
 * produce zero coverage. */
MenuSwfRasterStatus menu_swf_raster_mesh(
    const MenuSwfTessMesh *mesh, const MenuSwfRasterTransform *transform,
    const MenuSwfRasterTarget *target, MenuSwfRasterResult *out,
    char *reason, size_t reason_cap);
