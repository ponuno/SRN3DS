#include "menu_swf_batch.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t q00, q01, q10, q11, qtx, qty;
    const MenuSwfBatchViewport *viewport;
} BatchCtx;

typedef struct {
    int32_t x[4], y[4];
    uint32_t u[4], v[4];
} BatchQuad;

static void batch_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static uint8_t batch_channel(uint32_t base, int32_t mult, int32_t add) {
    int64_t value = ((int64_t)base * mult + (int64_t)add * 256ll) / 256ll;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return (uint8_t)value;
}

static uint32_t batch_text_color(const MenuSwfBatchStyle *style,
                                 const MenuSwfTextGlyph *glyph) {
    uint32_t rgba = glyph->has_color ? glyph->color : style->color;
    if (!style->has_color_transform) return rgba;
    uint8_t r = batch_channel((rgba >> 16) & 255u, style->color_mult[0],
                              style->color_add[0]);
    uint8_t g = batch_channel((rgba >> 8) & 255u, style->color_mult[1],
                              style->color_add[1]);
    uint8_t b = batch_channel(rgba & 255u, style->color_mult[2],
                              style->color_add[2]);
    uint8_t a = batch_channel((rgba >> 24) & 255u, style->color_mult[3],
                              style->color_add[3]);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

const char *menu_swf_batch_status_name(MenuSwfBatchStatus status) {
    switch (status) {
    case MENU_SWF_BATCH_OK: return "ok";
    case MENU_SWF_BATCH_ERR_ARGUMENT: return "bad batch argument";
    case MENU_SWF_BATCH_ERR_OVERFLOW: return "batch overflow";
    case MENU_SWF_BATCH_ERR_NAN: return "non-finite batch transform";
    case MENU_SWF_BATCH_ERR_SCALE: return "degenerate/negative batch scale";
    case MENU_SWF_BATCH_ERR_CLIP: return "unsupported viewport clip";
    case MENU_SWF_BATCH_ERR_FILL: return "unsupported batch fill";
    default: return "unknown batch status";
    }
}

static MenuSwfBatchStatus quantize(double value, int32_t *out) {
    if (!isfinite(value)) return MENU_SWF_BATCH_ERR_NAN;
    double scaled = value * 65536.0;
    if (scaled < (double)INT32_MIN || scaled > (double)INT32_MAX)
        return MENU_SWF_BATCH_ERR_OVERFLOW;
    *out = (int32_t)llround(scaled);
    return MENU_SWF_BATCH_OK;
}

static void tx_point(const BatchCtx *c, double x, double y, double *ox,
                     double *oy) {
    *ox = ((double)c->q00 * x + (double)c->q01 * y + (double)c->qtx) /
          65536.0;
    *oy = ((double)c->q10 * x + (double)c->q11 * y + (double)c->qty) /
          65536.0;
}

static int32_t pixel(double value) {
    if (!isfinite(value)) return INT32_MIN;
    if (value > (double)INT32_MAX) return INT32_MAX;
    if (value < (double)INT32_MIN) return INT32_MIN;
    return (int32_t)llround(value);
}

static void uv_lerp(uint32_t base, uint32_t span, double fraction,
                    uint32_t *out) {
    double v = (double)base + (double)span * fraction;
    if (v < 0.0) v = 0.0;
    if (v > 131072.0) v = 131072.0;
    *out = (uint32_t)llround(v);
}

static int64_t area2(const BatchQuad *q) {
    return ((int64_t)q->x[1] - q->x[0]) * ((int64_t)q->y[2] - q->y[0]) -
           ((int64_t)q->y[1] - q->y[0]) * ((int64_t)q->x[2] - q->x[0]);
}

static int make_quad(const BatchCtx *c, const MenuSwfShapeRect *bounds,
                     const MenuSwfAtlasEntry *e, BatchQuad *q) {
    if (bounds->xmax <= bounds->xmin || bounds->ymax <= bounds->ymin)
        return 0;
    if (!e->has_rect || !e->w || !e->h) return 0;

    uint32_t u0 = e->u;
    uint32_t u1 = e->u + e->uw;
    uint32_t v0 = e->v;
    uint32_t v1 = e->v + e->vh;

    if (c->q01 == 0 && c->q10 == 0) {
        double xa, xb, ya, yb, dummy;
        tx_point(c, bounds->xmin, bounds->ymin, &xa, &dummy);
        tx_point(c, bounds->xmax, bounds->ymin, &xb, &dummy);
        tx_point(c, bounds->xmin, bounds->ymin, &dummy, &ya);
        tx_point(c, bounds->xmin, bounds->ymax, &dummy, &yb);
        int flip_u = xb < xa;
        int flip_v = yb < ya;
        double xlo = flip_u ? xb : xa;
        double xhi = flip_u ? xa : xb;
        double ylo = flip_v ? yb : ya;
        double yhi = flip_v ? ya : yb;
        double xc0 = xlo, xc1 = xhi, yc0 = ylo, yc1 = yhi;
        if (c->viewport) {
            if (xlo < c->viewport->x0) xc0 = c->viewport->x0;
            if (xhi > c->viewport->x1) xc1 = c->viewport->x1;
            if (ylo < c->viewport->y0) yc0 = c->viewport->y0;
            if (yhi > c->viewport->y1) yc1 = c->viewport->y1;
        }
        if (xc1 <= xc0 || yc1 <= yc0) return 0;
        int32_t px0 = pixel(xc0), px1 = pixel(xc1);
        int32_t py0 = pixel(yc0), py1 = pixel(yc1);
        if (px1 <= px0 || py1 <= py0) return 0;

        double fx0 = (xc0 - xlo) / (xhi - xlo);
        double fx1 = (xc1 - xlo) / (xhi - xlo);
        double fy0 = (yc0 - ylo) / (yhi - ylo);
        double fy1 = (yc1 - ylo) / (yhi - ylo);
        uint32_t ua, ub, va, vb;
        if (!flip_u) {
            uv_lerp(u0, e->uw, fx0, &ua);
            uv_lerp(u0, e->uw, fx1, &ub);
        } else {
            uv_lerp(u0, e->uw, 1.0 - fx0, &ua);
            uv_lerp(u0, e->uw, 1.0 - fx1, &ub);
        }
        if (!flip_v) {
            uv_lerp(v0, e->vh, fy0, &va);
            uv_lerp(v0, e->vh, fy1, &vb);
        } else {
            uv_lerp(v0, e->vh, 1.0 - fy0, &va);
            uv_lerp(v0, e->vh, 1.0 - fy1, &vb);
        }
        q->x[0] = px0; q->y[0] = py0; q->u[0] = ua; q->v[0] = va;
        q->x[1] = px1; q->y[1] = py0; q->u[1] = ub; q->v[1] = va;
        q->x[2] = px1; q->y[2] = py1; q->u[2] = ub; q->v[2] = vb;
        q->x[3] = px0; q->y[3] = py1; q->u[3] = ua; q->v[3] = vb;
        return area2(q) > 0;
    }

    if (c->viewport) return -1; /* rotated clipping is not implemented */

    double xs[4], ys[4];
    const int32_t bx[4] = {bounds->xmin, bounds->xmax, bounds->xmax,
                           bounds->xmin};
    const int32_t by[4] = {bounds->ymin, bounds->ymin, bounds->ymax,
                           bounds->ymax};
    for (unsigned i = 0; i < 4; ++i) {
        tx_point(c, bx[i], by[i], &xs[i], &ys[i]);
        if (!isfinite(xs[i]) || !isfinite(ys[i])) return -1;
        q->x[i] = pixel(xs[i]);
        q->y[i] = pixel(ys[i]);
    }
    q->u[0] = u0; q->v[0] = v0;
    q->u[1] = u1; q->v[1] = v0;
    q->u[2] = u1; q->v[2] = v1;
    q->u[3] = u0; q->v[3] = v1;
    return area2(q) > 0;
}

MenuSwfBatchStatus menu_swf_batch_text(
    const MenuSwfTextMesh *text, const MenuSwfAtlasEntry *atlas_entries,
    uint32_t atlas_count, const MenuSwfBatchStyle *style,
    const MenuSwfBatchViewport *viewport, MenuSwfBatchVertex *vertices,
    uint32_t vertex_capacity, uint32_t *indices, uint32_t index_capacity,
    MenuSwfBatchResult *out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_BATCH_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!text || !style) {
        batch_reason(reason, reason_cap, "swf batch: needs text and style");
        return MENU_SWF_BATCH_ERR_ARGUMENT;
    }
    if (text->glyph_count > MENU_SWF_TEXT_MAX_GLYPHS ||
        text->glyph_count > MENU_SWF_BATCH_MAX_QUADS) {
        batch_reason(reason, reason_cap,
                     "swf batch: %u glyphs exceed the quad limit",
                     text->glyph_count);
        return MENU_SWF_BATCH_ERR_OVERFLOW;
    }
    if (text->glyph_count && (!text->glyphs || !atlas_entries ||
                              atlas_count < text->glyph_count)) {
        batch_reason(reason, reason_cap,
                     "swf batch: atlas entries do not cover %u glyphs",
                     text->glyph_count);
        return MENU_SWF_BATCH_ERR_ARGUMENT;
    }
    if (viewport && viewport->enabled &&
        (viewport->x1 <= viewport->x0 || viewport->y1 <= viewport->y0)) {
        batch_reason(reason, reason_cap,
                     "swf batch: viewport is empty or inverted");
        return MENU_SWF_BATCH_ERR_ARGUMENT;
    }

    BatchCtx c;
    memset(&c, 0, sizeof c);
    MenuSwfBatchStatus st;
    if ((st = quantize(style->m00, &c.q00)) != MENU_SWF_BATCH_OK ||
        (st = quantize(style->m01, &c.q01)) != MENU_SWF_BATCH_OK ||
        (st = quantize(style->m10, &c.q10)) != MENU_SWF_BATCH_OK ||
        (st = quantize(style->m11, &c.q11)) != MENU_SWF_BATCH_OK ||
        (st = quantize(style->tx, &c.qtx)) != MENU_SWF_BATCH_OK ||
        (st = quantize(style->ty, &c.qty)) != MENU_SWF_BATCH_OK) {
        batch_reason(reason, reason_cap,
                     "swf batch: transform is not finite 16.16");
        return st;
    }
    int64_t det = (int64_t)c.q00 * c.q11 - (int64_t)c.q01 * c.q10;
    if (det <= 0) {
        batch_reason(reason, reason_cap,
                     "swf batch: scale determinant is zero or negative");
        return MENU_SWF_BATCH_ERR_SCALE;
    }
    c.viewport = (viewport && viewport->enabled) ? viewport : NULL;
    if (c.viewport && (c.q01 != 0 || c.q10 != 0)) {
        batch_reason(reason, reason_cap,
                     "swf batch: viewport clipping needs an axis-aligned "
                     "transform");
        return MENU_SWF_BATCH_ERR_CLIP;
    }

    uint32_t quads = 0;
    for (uint32_t i = 0; i < text->glyph_count; ++i) {
        const MenuSwfTextGlyph *g = &text->glyphs[i];
        if (!g->has_geometry || g->missing) continue;
        BatchQuad q;
        int rc = make_quad(&c, &g->bounds, &atlas_entries[i], &q);
        if (rc < 0) {
            batch_reason(reason, reason_cap,
                         "swf batch: glyph %u cannot be transformed",
                         g->code);
            return MENU_SWF_BATCH_ERR_CLIP;
        }
        if (rc) ++quads;
    }
    uint32_t need_vertices = quads * 4u;
    uint32_t need_indices = quads * 6u;
    if (need_vertices > MENU_SWF_BATCH_MAX_VERTICES ||
        need_indices > MENU_SWF_BATCH_MAX_INDICES ||
        need_vertices > vertex_capacity || need_indices > index_capacity ||
        (need_vertices && !vertices) || (need_indices && !indices)) {
        batch_reason(reason, reason_cap,
                     "swf batch: %u quads need %u vertices/%u indices "
                     "(capacity %u/%u)",
                     quads, need_vertices, need_indices, vertex_capacity,
                     index_capacity);
        return MENU_SWF_BATCH_ERR_OVERFLOW;
    }
    if (!quads) return MENU_SWF_BATCH_OK;

    uint32_t vcount = 0, icount = 0;
    int have_bounds = 0;
    int32_t bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
    for (uint32_t i = 0; i < text->glyph_count; ++i) {
        const MenuSwfTextGlyph *g = &text->glyphs[i];
        if (!g->has_geometry || g->missing) continue;
        BatchQuad q;
        if (!make_quad(&c, &g->bounds, &atlas_entries[i], &q)) continue;
        uint32_t base = vcount;
        uint32_t color = batch_text_color(style, g);
        for (unsigned k = 0; k < 4; ++k) {
            vertices[vcount].x = q.x[k];
            vertices[vcount].y = q.y[k];
            vertices[vcount].u = q.u[k];
            vertices[vcount].v = q.v[k];
            vertices[vcount].color = color;
            ++vcount;
            if (!have_bounds) {
                bxmin = bxmax = q.x[k];
                bymin = bymax = q.y[k];
                have_bounds = 1;
            } else {
                if (q.x[k] < bxmin) bxmin = q.x[k];
                if (q.x[k] > bxmax) bxmax = q.x[k];
                if (q.y[k] < bymin) bymin = q.y[k];
                if (q.y[k] > bymax) bymax = q.y[k];
            }
        }
        indices[icount++] = base + 0;
        indices[icount++] = base + 1;
        indices[icount++] = base + 2;
        indices[icount++] = base + 0;
        indices[icount++] = base + 2;
        indices[icount++] = base + 3;
    }
    out->vertex_count = vcount;
    out->index_count = icount;
    out->quad_count = vcount / 4u;
    out->bounds.xmin = bxmin;
    out->bounds.xmax = bxmax;
    out->bounds.ymin = bymin;
    out->bounds.ymax = bymax;
    out->has_bounds = (uint8_t)have_bounds;
    return MENU_SWF_BATCH_OK;
}

/* ------------------------------------------------------------------ */
/* Shape triangle batches                                             */
/* ------------------------------------------------------------------ */

static int shape_transform(const MenuSwfBatchShapeStyle *s, int32_t x,
                           int32_t y, int32_t *ox, int32_t *oy) {
    const int64_t divisor = 65536ll * 20ll;
    int64_t px, t;
    if (__builtin_mul_overflow((int64_t)s->m00, (int64_t)x, &px))
        return -1;
    if (__builtin_mul_overflow((int64_t)s->m01, (int64_t)y, &t) ||
        __builtin_add_overflow(px, t, &px))
        return -1;
    if (__builtin_mul_overflow((int64_t)s->tx, 65536ll, &t) ||
        __builtin_add_overflow(px, t, &px))
        return -1;
    int64_t py;
    if (__builtin_mul_overflow((int64_t)s->m10, (int64_t)x, &py))
        return -1;
    if (__builtin_mul_overflow((int64_t)s->m11, (int64_t)y, &t) ||
        __builtin_add_overflow(py, t, &py))
        return -1;
    if (__builtin_mul_overflow((int64_t)s->ty, 65536ll, &t) ||
        __builtin_add_overflow(py, t, &py))
        return -1;
    int64_t rx = px >= 0 ? (px + divisor / 2) / divisor
                         : -((-px + divisor / 2) / divisor);
    int64_t ry = py >= 0 ? (py + divisor / 2) / divisor
                         : -((-py + divisor / 2) / divisor);
    if (rx < INT32_MIN || rx > INT32_MAX || ry < INT32_MIN || ry > INT32_MAX)
        return -1;
    *ox = (int32_t)rx;
    *oy = (int32_t)ry;
    return 0;
}

static uint8_t shape_channel(uint32_t base, int32_t mult, int32_t add) {
    int64_t value = ((int64_t)base * mult + (int64_t)add * 256ll) / 256ll;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return (uint8_t)value;
}

#define MENU_SWF_GRADIENT_SCALE 16384.0

static int gradient_matrix(const MenuSwfShapeFill *fill, double x, double y,
                           double *gx, double *gy) {
    /* SWF MATRIX maps gradient space to shape space.  Scale/rotate are 16.16,
     * translation is twips; invert the 2x2 part to get gradient coordinates. */
    double a = fill->matrix.scale_x / 65536.0;
    double b = fill->matrix.rotate_skew0 / 65536.0;
    double c = fill->matrix.rotate_skew1 / 65536.0;
    double d = fill->matrix.scale_y / 65536.0;
    double det = a * d - b * c;
    if (!isfinite(det) || fabs(det) < 1e-9) return -1;
    double sx = x - (double)fill->matrix.translate_x;
    double sy = y - (double)fill->matrix.translate_y;
    *gx = (d * sx - c * sy) / det;
    *gy = (-b * sx + a * sy) / det;
    return 0;
}

static int gradient_spread(double t, unsigned spread, double *out) {
    if (!isfinite(t)) return -1;
    switch (spread) {
    case 0: /* pad/clamp */
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        break;
    case 1: { /* reflect */
        double period = fmod(t, 2.0);
        if (period < 0.0) period += 2.0;
        t = period > 1.0 ? 2.0 - period : period;
        break;
    }
    case 2: { /* repeat */
        t -= floor(t);
        break;
    }
    default:
        return -1;
    }
    *out = t;
    return 0;
}

static uint32_t gradient_stop_color(const MenuSwfShapeFill *fill, double t) {
    const MenuSwfShapeGradientStop *s = fill->stops;
    unsigned count = fill->stop_count;
    double position = t * 255.0;
    if (position <= (double)s[0].ratio)
        return ((uint32_t)s[0].a << 24) | ((uint32_t)s[0].r << 16) |
               ((uint32_t)s[0].g << 8) | s[0].b;
    for (unsigned i = 1; i < count; ++i) {
        if (position <= (double)s[i].ratio) {
            double span = (double)s[i].ratio - (double)s[i - 1].ratio;
            double k = span > 0.0 ? (position - (double)s[i - 1].ratio) / span
                                  : 0.0;
            double r = s[i - 1].r + ((double)s[i].r - s[i - 1].r) * k;
            double g = s[i - 1].g + ((double)s[i].g - s[i - 1].g) * k;
            double b = s[i - 1].b + ((double)s[i].b - s[i - 1].b) * k;
            double a = s[i - 1].a + ((double)s[i].a - s[i - 1].a) * k;
            return ((uint32_t)(a + 0.5) << 24) |
                   ((uint32_t)(r + 0.5) << 16) |
                   ((uint32_t)(g + 0.5) << 8) | (uint32_t)(b + 0.5);
        }
    }
    const MenuSwfShapeGradientStop *last = &s[count - 1];
    return ((uint32_t)last->a << 24) | ((uint32_t)last->r << 16) |
           ((uint32_t)last->g << 8) | last->b;
}

static MenuSwfBatchStatus shape_fill_validate(const MenuSwfTessFill *fill,
                                               char *reason,
                                               size_t reason_cap) {
    switch (fill->kind) {
    case MENU_SWF_FILL_SOLID: return MENU_SWF_BATCH_OK;
    case MENU_SWF_FILL_BITMAP:
        batch_reason(reason, reason_cap, "bitmap fills are not implemented");
        return MENU_SWF_BATCH_ERR_FILL;
    case MENU_SWF_FILL_FOCAL:
        batch_reason(reason, reason_cap,
                     "focal gradient fills are not implemented");
        return MENU_SWF_BATCH_ERR_FILL;
    case MENU_SWF_FILL_LINEAR:
    case MENU_SWF_FILL_RADIAL:
        break;
    default:
        batch_reason(reason, reason_cap, "shape batch: unknown fill kind %u",
                     fill->kind);
        return MENU_SWF_BATCH_ERR_FILL;
    }
    if (fill->focal_present) {
        batch_reason(reason, reason_cap,
                     "focal gradient fills are not implemented");
        return MENU_SWF_BATCH_ERR_FILL;
    }
    if (!fill->stop_count || fill->stop_count != fill->stop_total) {
        batch_reason(reason, reason_cap,
                     "gradient fills require 1..8 stored colour stops "
                     "(have %u of %u)", fill->stop_count, fill->stop_total);
        return MENU_SWF_BATCH_ERR_FILL;
    }
    if (fill->interpolation != 0) {
        batch_reason(reason, reason_cap,
                     "gradient interpolation mode %u is not implemented",
                     fill->interpolation);
        return MENU_SWF_BATCH_ERR_FILL;
    }
    if (fill->spread > 2) {
        batch_reason(reason, reason_cap,
                     "gradient spread mode %u is not implemented",
                     fill->spread);
        return MENU_SWF_BATCH_ERR_FILL;
    }
    for (unsigned i = 1; i < fill->stop_count; ++i)
        if (fill->stops[i].ratio < fill->stops[i - 1].ratio) {
            batch_reason(reason, reason_cap,
                         "gradient stops are not ascending at %u", i);
            return MENU_SWF_BATCH_ERR_FILL;
        }
    double gx, gy;
    if (gradient_matrix(fill, 0.0, 0.0, &gx, &gy)) {
        batch_reason(reason, reason_cap, "gradient matrix is singular");
        return MENU_SWF_BATCH_ERR_FILL;
    }
    return MENU_SWF_BATCH_OK;
}

static MenuSwfBatchStatus shape_vertex_color(const MenuSwfTessFill *fill,
                                             const MenuSwfBatchShapeStyle *style,
                                             const MenuSwfTessVertex *v,
                                             uint32_t *out, char *reason,
                                             size_t reason_cap) {
    uint32_t rgba;
    if (fill->kind == MENU_SWF_FILL_SOLID) {
        rgba = fill->rgba;
    } else {
        double gx, gy;
        if (gradient_matrix(fill, v->x, v->y, &gx, &gy)) {
            batch_reason(reason, reason_cap, "gradient matrix is singular");
            return MENU_SWF_BATCH_ERR_FILL;
        }
        double t;
        if (fill->kind == MENU_SWF_FILL_LINEAR)
            t = (gx + MENU_SWF_GRADIENT_SCALE) / (2.0 * MENU_SWF_GRADIENT_SCALE);
        else
            t = sqrt(gx * gx + gy * gy) / MENU_SWF_GRADIENT_SCALE;
        if (gradient_spread(t, fill->spread, &t)) {
            batch_reason(reason, reason_cap, "gradient spread is invalid");
            return MENU_SWF_BATCH_ERR_FILL;
        }
        rgba = gradient_stop_color(fill, t);
    }
    uint8_t r = shape_channel((rgba >> 16) & 255u, style->mult[0],
                              style->add[0]);
    uint8_t g = shape_channel((rgba >> 8) & 255u, style->mult[1],
                              style->add[1]);
    uint8_t b = shape_channel(rgba & 255u, style->mult[2], style->add[2]);
    uint8_t a = shape_channel((rgba >> 24) & 255u, style->mult[3], style->add[3]);
    *out = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return MENU_SWF_BATCH_OK;
}

MenuSwfBatchStatus menu_swf_batch_shape(
    const MenuSwfTessMesh *mesh, const MenuSwfBatchShapeStyle *style,
    const MenuSwfBatchViewport *viewport, MenuSwfBatchVertex *vertices,
    uint32_t vertex_capacity, uint32_t *indices, uint32_t index_capacity,
    MenuSwfBatchResult *out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_BATCH_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!mesh || !style) {
        batch_reason(reason, reason_cap,
                     "shape batch: needs a mesh and a style");
        return MENU_SWF_BATCH_ERR_ARGUMENT;
    }
    if (mesh->index_count != mesh->triangle_count * 3u ||
        (mesh->triangle_count && (!mesh->indices || !mesh->triangle_fills ||
                                  !mesh->fills))) {
        batch_reason(reason, reason_cap,
                     "shape batch: mesh indices/fills are incomplete");
        return MENU_SWF_BATCH_ERR_ARGUMENT;
    }
    if (viewport && viewport->enabled) {
        batch_reason(reason, reason_cap,
                     "shape batch viewport clipping is not implemented");
        return MENU_SWF_BATCH_ERR_CLIP;
    }
    int64_t det = (int64_t)style->m00 * style->m11 -
                  (int64_t)style->m01 * style->m10;
    if (det == 0) {
        batch_reason(reason, reason_cap,
                     "shape batch: scale determinant is zero");
        return MENU_SWF_BATCH_ERR_SCALE;
    }
    int flip = det < 0;
    uint32_t triangles = mesh->triangle_count;
    if (!triangles) return MENU_SWF_BATCH_OK;

    /* Validate every fill and transformed source point before writing. */
    for (uint32_t s = 0; s < mesh->vertex_count; ++s) {
        int32_t x, y;
        if (shape_transform(style, mesh->vertices[s].x, mesh->vertices[s].y,
                            &x, &y)) {
            batch_reason(reason, reason_cap,
                         "shape batch: transformed point overflow");
            return MENU_SWF_BATCH_ERR_OVERFLOW;
        }
    }
    for (uint32_t t = 0; t < triangles; ++t) {
        uint16_t style_index = mesh->triangle_fills[t];
        if (!style_index || style_index > mesh->fill_count) {
            batch_reason(reason, reason_cap,
                         "shape batch: triangle references fill %u of %u",
                         style_index, mesh->fill_count);
            return MENU_SWF_BATCH_ERR_FILL;
        }
        MenuSwfBatchStatus st = shape_fill_validate(
            &mesh->fills[style_index - 1], reason, reason_cap);
        if (st != MENU_SWF_BATCH_OK) return st;
        for (unsigned k = 0; k < 3; ++k) {
            uint32_t index = mesh->indices[t * 3u + k];
            if (index >= mesh->vertex_count) {
                batch_reason(reason, reason_cap,
                             "shape batch: index %u is outside %u vertices",
                             index, mesh->vertex_count);
                return MENU_SWF_BATCH_ERR_ARGUMENT;
            }
        }
    }

    uint64_t worst64 = (uint64_t)mesh->vertex_count + mesh->index_count;
    uint32_t worst_vertices = worst64 > UINT32_MAX
                                  ? UINT32_MAX
                                  : (uint32_t)worst64;
    if (mesh->vertex_count > MENU_SWF_BATCH_MAX_SHAPE_VERTICES ||
        mesh->index_count > MENU_SWF_BATCH_MAX_SHAPE_INDICES ||
        worst_vertices > MENU_SWF_BATCH_MAX_SHAPE_VERTICES ||
        worst_vertices > vertex_capacity ||
        mesh->index_count > index_capacity ||
        (worst_vertices && (!vertices || !indices))) {
        batch_reason(reason, reason_cap,
                     "shape batch: %u source vertices / %u indices need up to "
                     "%u vertices (capacity %u/%u)",
                     mesh->vertex_count, mesh->index_count, worst_vertices,
                     vertex_capacity, index_capacity);
        return MENU_SWF_BATCH_ERR_OVERFLOW;
    }


    /* Keep the source mesh's vertex sharing.  Each source vertex starts with
     * the colour of its first triangle; a vertex shared by triangles with
     * different fills is duplicated only when that conflict is observed. */
    uint8_t seen[(MENU_SWF_BATCH_MAX_SHAPE_VERTICES + 7u) / 8u];
    memset(seen, 0, sizeof seen);
    uint32_t vcount = 0;
    int have_bounds = 0;
    int32_t bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
    for (uint32_t s = 0; s < mesh->vertex_count; ++s) {
        MenuSwfBatchVertex *v = &vertices[vcount];
        if (shape_transform(style, mesh->vertices[s].x, mesh->vertices[s].y,
                            &v->x, &v->y)) {
            batch_reason(reason, reason_cap,
                         "shape batch: transformed point overflow");
            return MENU_SWF_BATCH_ERR_OVERFLOW;
        }
        v->u = style->u;
        v->v = style->v;
        v->color = 0;
        ++vcount;
        if (!have_bounds) {
            bxmin = bxmax = v->x;
            bymin = bymax = v->y;
            have_bounds = 1;
        } else {
            if (v->x < bxmin) bxmin = v->x;
            if (v->x > bxmax) bxmax = v->x;
            if (v->y < bymin) bymin = v->y;
            if (v->y > bymax) bymax = v->y;
        }
    }

    uint32_t icount = 0;
    for (uint32_t t = 0; t < triangles; ++t) {
        uint16_t style_index = mesh->triangle_fills[t];
        const MenuSwfTessFill *fill = &mesh->fills[style_index - 1];
        uint32_t tri[3];
        for (unsigned k = 0; k < 3; ++k) {
            uint32_t s = mesh->indices[t * 3u + k];
            uint32_t color;
            MenuSwfBatchStatus st = shape_vertex_color(
                fill, style, &mesh->vertices[s], &color, reason, reason_cap);
            if (st != MENU_SWF_BATCH_OK) return st;
            uint32_t index;
            uint8_t mask = (uint8_t)(1u << (s & 7u));
            if (!(seen[s >> 3] & mask)) {
                seen[s >> 3] |= mask;
                vertices[s].color = color;
                index = s;
            } else if (vertices[s].color == color) {
                index = s;
            } else {
                /* A source vertex shared across fills needs one copy. */
                if (vcount >= vertex_capacity ||
                    vcount >= MENU_SWF_BATCH_MAX_SHAPE_VERTICES) {
                    batch_reason(reason, reason_cap,
                                 "shape batch: %u vertex copies exceed %u",
                                 vcount + 1, vertex_capacity);
                    return MENU_SWF_BATCH_ERR_OVERFLOW;
                }
                vertices[vcount] = vertices[s];
                vertices[vcount].color = color;
                index = vcount++;
            }
            tri[k] = index;
        }
        if (flip) {
            uint32_t swap = tri[1];
            tri[1] = tri[2];
            tri[2] = swap;
        }
        indices[icount++] = tri[0];
        indices[icount++] = tri[1];
        indices[icount++] = tri[2];
    }
    out->vertex_count = vcount;
    out->index_count = icount;
    out->triangle_count = triangles;
    out->bounds.xmin = bxmin;
    out->bounds.xmax = bxmax;
    out->bounds.ymin = bymin;
    out->bounds.ymax = bymax;
    out->has_bounds = (uint8_t)have_bounds;
    return MENU_SWF_BATCH_OK;
}
