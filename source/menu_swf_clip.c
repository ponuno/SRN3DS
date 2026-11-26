#include "menu_swf_clip.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void clip_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *menu_swf_clip_status_name(MenuSwfClipStatus status) {
    switch (status) {
    case MENU_SWF_CLIP_OK: return "ok";
    case MENU_SWF_CLIP_ERR_ARGUMENT: return "bad clip argument";
    case MENU_SWF_CLIP_ERR_PAIR: return "bad clip-depth pair";
    case MENU_SWF_CLIP_ERR_MASK: return "mask shape not supported";
    case MENU_SWF_CLIP_ERR_NESTING: return "too many nested masks";
    case MENU_SWF_CLIP_ERR_OVERFLOW: return "clip capacity overflow";
    case MENU_SWF_CLIP_ERR_CYCLE: return "clip cycle";
    default: return "unknown clip status";
    }
}

int menu_swf_clip_rect_intersect(const MenuSwfClipRect *a,
                                 const MenuSwfClipRect *b,
                                 MenuSwfClipRect *out) {
    if (!a || !b || !out) return 0;
    MenuSwfClipRect r;
    r.xmin = a->xmin > b->xmin ? a->xmin : b->xmin;
    r.ymin = a->ymin > b->ymin ? a->ymin : b->ymin;
    r.xmax = a->xmax < b->xmax ? a->xmax : b->xmax;
    r.ymax = a->ymax < b->ymax ? a->ymax : b->ymax;
    *out = r;
    return r.xmin < r.xmax && r.ymin < r.ymax;
}

static int transform_point(const MenuSwfDisplayListMatrix *m, int32_t x,
                           int32_t y, MenuSwfClipPoint *out,
                           char *reason, size_t reason_cap) {
    int64_t tx = (int64_t)m->m00 * x + (int64_t)m->m01 * y;
    int64_t ty = (int64_t)m->m10 * x + (int64_t)m->m11 * y;
    int64_t ox = tx / 65536 + m->tx;
    int64_t oy = ty / 65536 + m->ty;
    if (ox < INT32_MIN || ox > INT32_MAX ||
        oy < INT32_MIN || oy > INT32_MAX) {
        clip_reason(reason, reason_cap, "swf clip: transformed point overflow");
        return -1;
    }
    out->x = (int32_t)ox;
    out->y = (int32_t)oy;
    return 0;
}

static MenuSwfClipStatus make_quad(const MenuSwfDisplayListRecord *record,
                                   const MenuSwfClipShape *shape,
                                   MenuSwfClipPoint q[4], char *reason,
                                   size_t reason_cap) {
    if (!shape || !shape->has_bounds) {
        clip_reason(reason, reason_cap, "swf clip: mask bounds are missing");
        return MENU_SWF_CLIP_ERR_MASK;
    }
    if (!shape->is_rect) {
        clip_reason(reason, reason_cap,
                    "mask shape tessellation is not implemented");
        return MENU_SWF_CLIP_ERR_MASK;
    }
    const int32_t x0 = shape->bounds.xmin, x1 = shape->bounds.xmax;
    const int32_t y0 = shape->bounds.ymin, y1 = shape->bounds.ymax;
    const int32_t xs[4] = {x0, x1, x1, x0};
    const int32_t ys[4] = {y0, y0, y1, y1};
    for (unsigned i = 0; i < 4; ++i)
        if (transform_point(&record->matrix, xs[i], ys[i], &q[i], reason,
                            reason_cap))
            return MENU_SWF_CLIP_ERR_OVERFLOW;
    return MENU_SWF_CLIP_OK;
}

MenuSwfClipStatus menu_swf_clip_mask_quad(
    const MenuSwfDisplayListRecord *record, const MenuSwfClipShape *shape,
    MenuSwfClipPoint out[4], char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!record || !shape || !out) return MENU_SWF_CLIP_ERR_ARGUMENT;
    return make_quad(record, shape, out, reason, reason_cap);
}

static int quad_axis_aligned(const MenuSwfClipPoint q[4]) {
    return q[0].y == q[1].y && q[1].x == q[2].x &&
           q[2].y == q[3].y && q[3].x == q[0].x;
}

static void quad_rect(const MenuSwfClipPoint q[4], MenuSwfClipRect *r) {
    r->xmin = r->xmax = q[0].x;
    r->ymin = r->ymax = q[0].y;
    for (unsigned i = 1; i < 4; ++i) {
        if (q[i].x < r->xmin) r->xmin = q[i].x;
        if (q[i].x > r->xmax) r->xmax = q[i].x;
        if (q[i].y < r->ymin) r->ymin = q[i].y;
        if (q[i].y > r->ymax) r->ymax = q[i].y;
    }
}

static int record_rect(const MenuSwfDisplayListRecord *record,
                       const MenuSwfClipShape *shape, MenuSwfClipRect *out,
                       char *reason, size_t reason_cap) {
    if (!shape || !shape->has_bounds) return 1; /* unknown bounds */
    const int32_t x0 = shape->bounds.xmin, x1 = shape->bounds.xmax;
    const int32_t y0 = shape->bounds.ymin, y1 = shape->bounds.ymax;
    const int32_t xs[4] = {x0, x1, x1, x0};
    const int32_t ys[4] = {y0, y0, y1, y1};
    MenuSwfClipPoint q[4];
    for (unsigned i = 0; i < 4; ++i)
        if (transform_point(&record->matrix, xs[i], ys[i], &q[i], reason,
                            reason_cap))
            return -1;
    quad_rect(q, out);
    return 0;
}

static int is_mask(const MenuSwfDisplayListRecord *r) {
    return r->has_clip_depth && r->clip_depth > r->depth;
}

MenuSwfClipStatus menu_swf_clip_build(
    const MenuSwfDisplayListRecord *records, uint32_t record_count,
    const MenuSwfClipShape *shapes, uint32_t shape_count,
    MenuSwfClipMask *masks, uint32_t mask_capacity,
    MenuSwfClipRecord *out_records, uint32_t record_capacity,
    MenuSwfClipResult *out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_CLIP_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!records || !out_records) {
        clip_reason(reason, reason_cap, "swf clip: needs records and output");
        return MENU_SWF_CLIP_ERR_ARGUMENT;
    }
    if (record_count > MENU_SWF_CLIP_MAX_RECORDS ||
        record_count > record_capacity) {
        clip_reason(reason, reason_cap,
                    "swf clip: %u records exceed capacity %u", record_count,
                    record_capacity);
        return MENU_SWF_CLIP_ERR_OVERFLOW;
    }
    if (shapes && shape_count < record_count) {
        clip_reason(reason, reason_cap,
                    "swf clip: shape entries do not cover all records");
        return MENU_SWF_CLIP_ERR_ARGUMENT;
    }

    /* Validate pairs and masks, count masks and peak nesting. */
    uint32_t mask_count = 0;
    uint32_t max_active = 0;
    for (uint32_t i = 0; i < record_count; ++i) {
        const MenuSwfDisplayListRecord *r = &records[i];
        if (!r->has_clip_depth) continue;
        if (r->clip_depth <= r->depth) {
            clip_reason(reason, reason_cap,
                        "swf clip: mask depth %u >= clip depth %u",
                        r->depth, r->clip_depth);
            return MENU_SWF_CLIP_ERR_PAIR;
        }
        ++mask_count;
        const MenuSwfClipShape *shape = shapes ? &shapes[i] : NULL;
        if (!shape || !shape->has_bounds) {
            clip_reason(reason, reason_cap,
                        "swf clip: mask record %u has no bounds", i);
            return MENU_SWF_CLIP_ERR_MASK;
        }
        if (!shape->is_rect) {
            clip_reason(reason, reason_cap,
                        "mask shape tessellation is not implemented");
            return MENU_SWF_CLIP_ERR_MASK;
        }
        MenuSwfClipPoint q[4];
        if (make_quad(r, shape, q, reason, reason_cap) != MENU_SWF_CLIP_OK)
            return MENU_SWF_CLIP_ERR_MASK;
    }
    if (mask_count > mask_capacity ||
        mask_count > MENU_SWF_CLIP_MAX_MASKS) {
        clip_reason(reason, reason_cap,
                    "swf clip: %u masks exceed capacity %u", mask_count,
                    mask_capacity);
        return MENU_SWF_CLIP_ERR_OVERFLOW;
    }

    for (uint32_t i = 0; i < record_count; ++i) {
        uint32_t active = 0;
        for (uint32_t j = 0; j < record_count; ++j) {
            const MenuSwfDisplayListRecord *m = &records[j];
            if (!is_mask(m)) continue;
            if (records[i].depth > m->depth &&
                records[i].depth <= m->clip_depth)
                ++active;
        }
        if (active > MENU_SWF_CLIP_MAX_ACTIVE) {
            clip_reason(reason, reason_cap,
                        "swf clip: record %u has %u nested masks, max %u",
                        i, active, MENU_SWF_CLIP_MAX_ACTIVE);
            return MENU_SWF_CLIP_ERR_NESTING;
        }
        if (active > max_active) max_active = active;
        MenuSwfClipRect dummy;
        const MenuSwfClipShape *shape = shapes ? &shapes[i] : NULL;
        if (record_rect(&records[i], shape, &dummy, reason, reason_cap) < 0)
            return MENU_SWF_CLIP_ERR_OVERFLOW;
    }

    /* Validation complete: write masks and records. */
    uint32_t mi = 0;
    for (uint32_t i = 0; i < record_count; ++i) {
        const MenuSwfDisplayListRecord *r = &records[i];
        if (!is_mask(r)) continue;
        MenuSwfClipMask *m = &masks[mi];
        m->mask_record_index = i;
        m->mask_depth = r->depth;
        m->clip_depth = r->clip_depth;
        MenuSwfClipStatus st = make_quad(r, &shapes[i], m->quad, reason,
                                         reason_cap);
        if (st != MENU_SWF_CLIP_OK) return st;
        quad_rect(m->quad, &m->rect);
        m->axis_aligned = (uint8_t)quad_axis_aligned(m->quad);
        ++mi;
    }
    out->mask_count = mask_count;

    uint32_t clipped = 0;
    for (uint32_t i = 0; i < record_count; ++i) {
        MenuSwfClipRecord *o = &out_records[i];
        memset(o, 0, sizeof *o);
        o->record_index = i;
        const MenuSwfClipShape *shape = shapes ? &shapes[i] : NULL;
        int rr = record_rect(&records[i], shape, &o->rect, reason,
                             reason_cap);
        if (rr < 0) return MENU_SWF_CLIP_ERR_OVERFLOW;
        o->has_rect = rr == 0;
        if (o->has_rect) o->clip_rect = o->rect;
        for (uint32_t j = 0; j < mask_count; ++j) {
            const MenuSwfClipMask *m = &masks[j];
            if (!(records[i].depth > m->mask_depth &&
                  records[i].depth <= m->clip_depth))
                continue;
            for (uint32_t k = 0; k < o->mask_count; ++k)
                if (o->mask_indices[k] == j) {
                    clip_reason(reason, reason_cap,
                                "swf clip: duplicate active mask %u", j);
                    return MENU_SWF_CLIP_ERR_CYCLE;
                }
            o->mask_indices[o->mask_count++] = j;
            if (!m->axis_aligned) o->has_rotated_mask = 1;
            if (o->has_rect &&
                !menu_swf_clip_rect_intersect(&o->clip_rect, &m->rect,
                                              &o->clip_rect)) {
                o->fully_clipped = 1;
            }
        }
        if (o->mask_count) ++clipped;
    }
    out->record_count = record_count;
    out->clipped_records = clipped;
    out->max_active_masks = max_active;
    return MENU_SWF_CLIP_OK;
}
