#include "menu_swf_raster.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RASTER_MAX_COORD 1000000.0 /* output pixels, transformed */

typedef struct {
    int32_t x[3], y[3]; /* 1/MENU_SWF_RASTER_SUBPIXEL pixel */
    int32_t px0, py0, px1, py1; /* inclusive pixel bbox */
} RasterTri;

static void raster_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *menu_swf_raster_status_name(MenuSwfRasterStatus status) {
    switch (status) {
    case MENU_SWF_RASTER_OK: return "ok";
    case MENU_SWF_RASTER_ERR_ARGUMENT: return "bad argument";
    case MENU_SWF_RASTER_ERR_TOO_SMALL: return "target too small";
    case MENU_SWF_RASTER_ERR_OVERFLOW: return "raster overflow";
    case MENU_SWF_RASTER_ERR_NAN: return "non-finite transform";
    case MENU_SWF_RASTER_ERR_SCALE: return "degenerate transform";
    case MENU_SWF_RASTER_ERR_NEGATIVE_SCALE: return "negative scale refused";
    default: return "unknown raster status";
    }
}

static MenuSwfRasterStatus quantize(double value, int32_t *out) {
    if (!isfinite(value)) return MENU_SWF_RASTER_ERR_NAN;
    double scaled = value * 65536.0;
    if (scaled < (double)INT32_MIN || scaled > (double)INT32_MAX)
        return MENU_SWF_RASTER_ERR_OVERFLOW;
    *out = (int32_t)llround(scaled);
    return MENU_SWF_RASTER_OK;
}

static int64_t cross32(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                       int32_t px, int32_t py) {
    return ((int64_t)bx - ax) * ((int64_t)py - ay) -
           ((int64_t)by - ay) * ((int64_t)px - ax);
}

static int point_inside(const RasterTri *t, int32_t px, int32_t py) {
    int64_t d1 = cross32(t->x[0], t->y[0], t->x[1], t->y[1], px, py);
    int64_t d2 = cross32(t->x[1], t->y[1], t->x[2], t->y[2], px, py);
    int64_t d3 = cross32(t->x[2], t->y[2], t->x[0], t->y[0], px, py);
    int pos = d1 >= 0 && d2 >= 0 && d3 >= 0;
    int neg = d1 <= 0 && d2 <= 0 && d3 <= 0;
    return pos || neg;
}

static MenuSwfRasterStatus raster_setup(const MenuSwfTessMesh *mesh,
                                        const MenuSwfRasterTarget *target,
                                        char *reason, size_t reason_cap) {
    if (!mesh || !target) {
        raster_reason(reason, reason_cap,
                      "swf raster: needs a mesh and a target");
        return MENU_SWF_RASTER_ERR_ARGUMENT;
    }
    if (target->width == 0 || target->height == 0 ||
        target->stride < target->width || !target->pixels) {
        raster_reason(reason, reason_cap,
                      "swf raster: target %ux%u stride %u has no usable rows",
                      target->width, target->height, target->stride);
        return MENU_SWF_RASTER_ERR_TOO_SMALL;
    }
    if (target->width > MENU_SWF_RASTER_MAX_DIM ||
        target->height > MENU_SWF_RASTER_MAX_DIM ||
        target->stride > MENU_SWF_RASTER_MAX_STRIDE) {
        raster_reason(reason, reason_cap,
                      "swf raster: target %ux%u stride %u exceeds limits",
                      target->width, target->height, target->stride);
        return MENU_SWF_RASTER_ERR_OVERFLOW;
    }
    if ((mesh->index_count % 3u) != 0) {
        raster_reason(reason, reason_cap,
                      "swf raster: index count %u is not triangles",
                      mesh->index_count);
        return MENU_SWF_RASTER_ERR_ARGUMENT;
    }
    if (mesh->index_count > MENU_SWF_TESS_MAX_INDICES ||
        mesh->vertex_count > MENU_SWF_TESS_MAX_VERTICES) {
        raster_reason(reason, reason_cap,
                      "swf raster: mesh counts exceed the tess limits");
        return MENU_SWF_RASTER_ERR_OVERFLOW;
    }
    if (mesh->index_count &&
        (!mesh->vertices || !mesh->indices)) {
        raster_reason(reason, reason_cap,
                      "swf raster: mesh has indices without buffers");
        return MENU_SWF_RASTER_ERR_ARGUMENT;
    }
    if (mesh->index_count / 3u > MENU_SWF_RASTER_MAX_TRIANGLES) {
        raster_reason(reason, reason_cap,
                      "swf raster: %u triangles exceeds %u",
                      mesh->index_count / 3u,
                      MENU_SWF_RASTER_MAX_TRIANGLES);
        return MENU_SWF_RASTER_ERR_OVERFLOW;
    }
    return MENU_SWF_RASTER_OK;
}

MenuSwfRasterStatus menu_swf_raster_mesh(
    const MenuSwfTessMesh *mesh, const MenuSwfRasterTransform *transform,
    const MenuSwfRasterTarget *target, MenuSwfRasterResult *out,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_RASTER_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!transform) {
        raster_reason(reason, reason_cap,
                      "swf raster: needs a transform");
        return MENU_SWF_RASTER_ERR_ARGUMENT;
    }
    MenuSwfRasterStatus st = raster_setup(mesh, target, reason, reason_cap);
    if (st != MENU_SWF_RASTER_OK) return st;
    out->width = target->width;
    out->height = target->height;
    out->advance = target->advance;

    int32_t q00, q01, q10, q11, qtx, qty;
    if ((st = quantize(transform->m00, &q00)) != MENU_SWF_RASTER_OK ||
        (st = quantize(transform->m01, &q01)) != MENU_SWF_RASTER_OK ||
        (st = quantize(transform->m10, &q10)) != MENU_SWF_RASTER_OK ||
        (st = quantize(transform->m11, &q11)) != MENU_SWF_RASTER_OK ||
        (st = quantize(transform->tx, &qtx)) != MENU_SWF_RASTER_OK ||
        (st = quantize(transform->ty, &qty)) != MENU_SWF_RASTER_OK) {
        raster_reason(reason, reason_cap,
                      "swf raster: transform term is not a finite 16.16 value");
        return st;
    }
    int64_t det = (int64_t)q00 * q11 - (int64_t)q01 * q10;
    if (det == 0) {
        raster_reason(reason, reason_cap,
                      "swf raster: transform has zero determinant");
        return MENU_SWF_RASTER_ERR_SCALE;
    }
    if (det < 0 && target->reject_negative_scale) {
        raster_reason(reason, reason_cap,
                      "swf raster: negative scale is disabled");
        return MENU_SWF_RASTER_ERR_NEGATIVE_SCALE;
    }

    uint32_t tri_count = mesh->index_count / 3u;
    RasterTri tris[MENU_SWF_RASTER_MAX_TRIANGLES];
    uint32_t stored = 0;
    for (uint32_t i = 0; i < tri_count; ++i) {
        uint32_t ia = mesh->indices[i * 3 + 0];
        uint32_t ib = mesh->indices[i * 3 + 1];
        uint32_t ic = mesh->indices[i * 3 + 2];
        if (ia >= mesh->vertex_count || ib >= mesh->vertex_count ||
            ic >= mesh->vertex_count) {
            raster_reason(reason, reason_cap,
                          "swf raster: triangle %u index out of range", i);
            return MENU_SWF_RASTER_ERR_ARGUMENT;
        }
        int32_t xs[3], ys[3];
        const uint32_t idx[3] = {ia, ib, ic};
        for (unsigned k = 0; k < 3; ++k) {
            const MenuSwfTessVertex *v = &mesh->vertices[idx[k]];
            double raw_x = (double)v->x + transform->pen_x;
            double raw_y = (double)v->y + transform->pen_y;
            double x = ((double)q00 * raw_x + (double)q01 * raw_y +
                        (double)qtx) / 65536.0;
            double y = ((double)q10 * raw_x + (double)q11 * raw_y +
                        (double)qty) / 65536.0;
            if (!isfinite(x) || !isfinite(y) ||
                fabs(x) > RASTER_MAX_COORD || fabs(y) > RASTER_MAX_COORD) {
                raster_reason(reason, reason_cap,
                              "swf raster: transformed vertex is non-finite "
                              "or too large");
                return MENU_SWF_RASTER_ERR_OVERFLOW;
            }
            double sx = x * MENU_SWF_RASTER_SUBPIXEL;
            double sy = y * MENU_SWF_RASTER_SUBPIXEL;
            if (sx < (double)INT32_MIN || sx > (double)INT32_MAX ||
                sy < (double)INT32_MIN || sy > (double)INT32_MAX) {
                raster_reason(reason, reason_cap,
                              "swf raster: subpixel coordinate overflow");
                return MENU_SWF_RASTER_ERR_OVERFLOW;
            }
            xs[k] = (int32_t)llround(sx);
            ys[k] = (int32_t)llround(sy);
        }
        int64_t area = cross32(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2]);
        if (area == 0) continue; /* degenerate triangle */

        int32_t xmin = xs[0], xmax = xs[0];
        int32_t ymin = ys[0], ymax = ys[0];
        for (unsigned k = 1; k < 3; ++k) {
            if (xs[k] < xmin) xmin = xs[k];
            if (xs[k] > xmax) xmax = xs[k];
            if (ys[k] < ymin) ymin = ys[k];
            if (ys[k] > ymax) ymax = ys[k];
        }
        int32_t bx0 = (int32_t)floor((double)xmin /
                                     MENU_SWF_RASTER_SUBPIXEL);
        int32_t bx1 = (int32_t)ceil((double)xmax /
                                    MENU_SWF_RASTER_SUBPIXEL);
        int32_t by0 = (int32_t)floor((double)ymin /
                                     MENU_SWF_RASTER_SUBPIXEL);
        int32_t by1 = (int32_t)ceil((double)ymax /
                                    MENU_SWF_RASTER_SUBPIXEL);
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > (int32_t)target->width) bx1 = (int32_t)target->width;
        if (by1 > (int32_t)target->height) by1 = (int32_t)target->height;
        if (bx0 >= bx1 || by0 >= by1) continue; /* fully clipped */

        RasterTri *t = &tris[stored++];
        memcpy(t->x, xs, sizeof xs);
        memcpy(t->y, ys, sizeof ys);
        t->px0 = bx0;
        t->py0 = by0;
        t->px1 = bx1 - 1;
        t->py1 = by1 - 1;
    }

    for (uint32_t y = 0; y < target->height; ++y)
        memset(target->pixels + (size_t)y * target->stride, 0,
               target->width);

    uint32_t covered = 0;
    uint64_t sum = 0;
    uint8_t max_cov = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int have = 0;
    for (uint32_t py = 0; py < target->height; ++py) {
        for (uint32_t px = 0; px < target->width; ++px) {
            unsigned inside = 0;
            for (unsigned sy = 0; sy < 4; ++sy) {
                int32_t sample_y =
                    (int32_t)(py * MENU_SWF_RASTER_SUBPIXEL +
                              (2 * sy + 1) * (MENU_SWF_RASTER_SUBPIXEL / 8));
                for (unsigned sx = 0; sx < 4; ++sx) {
                    int32_t sample_x =
                        (int32_t)(px * MENU_SWF_RASTER_SUBPIXEL +
                                  (2 * sx + 1) *
                                      (MENU_SWF_RASTER_SUBPIXEL / 8));
                    for (uint32_t k = 0; k < stored; ++k) {
                        const RasterTri *t = &tris[k];
                        if ((int32_t)px < t->px0 || (int32_t)px > t->px1 ||
                            (int32_t)py < t->py0 || (int32_t)py > t->py1)
                            continue;
                        if (point_inside(t, sample_x, sample_y)) {
                            ++inside;
                            break;
                        }
                    }
                }
            }
            if (!inside) continue;
            uint8_t cov = (uint8_t)((inside * 255u + 8u) / 16u);
            target->pixels[(size_t)py * target->stride + px] = cov;
            ++covered;
            sum += cov;
            if (cov > max_cov) max_cov = cov;
            if (!have) {
                x0 = px;
                y0 = py;
                x1 = px + 1;
                y1 = py + 1;
                have = 1;
            } else {
                if (px < x0) x0 = px;
                if (py < y0) y0 = py;
                if (px + 1 > x1) x1 = px + 1;
                if (py + 1 > y1) y1 = py + 1;
            }
        }
    }

    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;
    out->coverage_sum = sum;
    out->covered_pixels = covered;
    out->max_coverage = max_cov;
    out->has_coverage = (uint8_t)have;
    return MENU_SWF_RASTER_OK;
}
