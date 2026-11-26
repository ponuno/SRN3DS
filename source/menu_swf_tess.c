#include "menu_swf_tess.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MenuSwfTessMesh *mesh;
    uint16_t shape_id;
    uint16_t font_id;
    uint16_t glyph_code;
    MenuSwfTessFillRule rule;
    int glyph_mode;
    int in_shape;
    int in_font;
    int shape_found;
    int in_path;
    int32_t start_x, start_y;
    int32_t cur_x, cur_y;
    uint16_t fill0, fill1, line;
    uint16_t fill_base; /* global style offset after StateNewStyles */
    MenuSwfStatus status;
    char *reason;
    size_t reason_cap;
} TessCtx;

static void tess_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static void tess_fail(TessCtx *c, MenuSwfStatus status, const char *fmt, ...) {
    if (c->status != MENU_SWF_OK) return;
    c->status = status;
    if (!c->reason || !c->reason_cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->reason, c->reason_cap, fmt, ap);
    va_end(ap);
    c->reason[c->reason_cap - 1] = 0;
}

static void reset_output(MenuSwfTessMesh *m) {
    m->vertex_count = 0;
    m->index_count = 0;
    m->triangle_count = 0;
    m->line_count = 0;
    m->contour_count = 0;
    m->edge_count = 0;
    m->curve_segments = 0;
    m->fill_count = 0;
    memset(&m->bounds, 0, sizeof m->bounds);
    m->has_bounds = 0;
}

static void add_bounds(TessCtx *c, int32_t x, int32_t y) {
    MenuSwfTessMesh *m = c->mesh;
    if (!m->has_bounds) {
        m->bounds.xmin = m->bounds.xmax = x;
        m->bounds.ymin = m->bounds.ymax = y;
        m->has_bounds = 1;
        return;
    }
    if (x < m->bounds.xmin) m->bounds.xmin = x;
    if (x > m->bounds.xmax) m->bounds.xmax = x;
    if (y < m->bounds.ymin) m->bounds.ymin = y;
    if (y > m->bounds.ymax) m->bounds.ymax = y;
}

static void add_line(TessCtx *c, int32_t x0, int32_t y0, int32_t x1,
                     int32_t y1) {
    MenuSwfTessMesh *m = c->mesh;
    if (!c->line || c->status != MENU_SWF_OK) return;
    if (x0 == x1 && y0 == y1) return;
    add_bounds(c, x0, y0);
    add_bounds(c, x1, y1);
    if (m->line_count >= m->line_capacity ||
        m->line_count >= MENU_SWF_TESS_MAX_LINES) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: line capacity %u exceeded", m->line_capacity);
        return;
    }
    MenuSwfTessLine *line = &m->lines[m->line_count++];
    line->x0 = x0;
    line->y0 = y0;
    line->x1 = x1;
    line->y1 = y1;
    line->line_style = c->line;
}

static void add_edge(TessCtx *c, int32_t x0, int32_t y0, int32_t x1,
                     int32_t y1, int draw_line) {
    MenuSwfTessMesh *m = c->mesh;
    if (c->status != MENU_SWF_OK) return;
    if (draw_line) add_line(c, x0, y0, x1, y1);
    add_bounds(c, x0, y0);
    add_bounds(c, x1, y1);
    if (c->fill0 == 0 && c->fill1 == 0) return;
    if (y0 == y1) return; /* horizontal edges do not cross a scan line */
    if (x0 == x1 && y0 == y1) return;
    if (m->edge_count >= m->edge_capacity ||
        m->edge_count >= MENU_SWF_TESS_MAX_EDGES) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: edge capacity %u exceeded", m->edge_capacity);
        return;
    }
    MenuSwfTessEdge *e = &m->edges[m->edge_count++];
    memset(e, 0, sizeof *e);
    if (y0 < y1) {
        e->x0 = x0;
        e->y0 = y0;
        e->x1 = x1;
        e->y1 = y1;
        e->dir = 1;
    } else {
        e->x0 = x1;
        e->y0 = y1;
        e->x1 = x0;
        e->y1 = y0;
        e->dir = -1;
    }
    e->fill0 = c->fill0;
    e->fill1 = c->fill1;
}

static void flatten_curve(TessCtx *c, int32_t x0, int32_t y0, int32_t cx,
                          int32_t cy, int32_t ax, int32_t ay, unsigned depth,
                          unsigned *segments) {
    if (c->status != MENU_SWF_OK) return;
    int64_t ddx = (int64_t)x0 - 2 * (int64_t)cx + ax;
    int64_t ddy = (int64_t)y0 - 2 * (int64_t)cy + ay;
    if (depth == 0 ||
        (llabs(ddx) <= MENU_SWF_TESS_FLATNESS &&
         llabs(ddy) <= MENU_SWF_TESS_FLATNESS)) {
        if (*segments >= (1u << MENU_SWF_TESS_CURVE_DEPTH)) {
            tess_fail(c, MENU_SWF_ERR_BUDGET,
                      "swf tess: curve segment limit exceeded");
            return;
        }
        ++*segments;
        if (c->mesh->curve_segments >= MENU_SWF_TESS_MAX_CURVE_SEGMENTS) {
            tess_fail(c, MENU_SWF_ERR_BUDGET,
                      "swf tess: total curve segment limit %u exceeded",
                      MENU_SWF_TESS_MAX_CURVE_SEGMENTS);
            return;
        }
        ++c->mesh->curve_segments;
        add_edge(c, x0, y0, ax, ay, 1);
        return;
    }
    int32_t x01 = (int32_t)(((int64_t)x0 + cx) / 2);
    int32_t y01 = (int32_t)(((int64_t)y0 + cy) / 2);
    int32_t x12 = (int32_t)(((int64_t)cx + ax) / 2);
    int32_t y12 = (int32_t)(((int64_t)cy + ay) / 2);
    int32_t x012 = (int32_t)(((int64_t)x01 + x12) / 2);
    int32_t y012 = (int32_t)(((int64_t)y01 + y12) / 2);
    flatten_curve(c, x0, y0, x01, y01, x012, y012, depth - 1, segments);
    flatten_curve(c, x012, y012, x12, y12, ax, ay, depth - 1, segments);
}

static void tess_begin_shape(void *user, uint16_t shape_id, uint8_t tag_code) {
    (void)tag_code;
    TessCtx *c = (TessCtx *)user;
    c->in_shape = (shape_id == c->shape_id);
    if (c->in_shape) {
        c->shape_found = 1;
        c->fill_base = 0;
    }
}

static void tess_end_shape(void *user) {
    TessCtx *c = (TessCtx *)user;
    c->in_shape = 0;
    c->in_path = 0;
}

static void tess_begin_fill_styles(void *user, uint16_t count) {
    TessCtx *c = (TessCtx *)user;
    if (!c->in_shape || c->status != MENU_SWF_OK) return;
    if (c->glyph_mode) return;
    c->fill_base = (uint16_t)c->mesh->fill_count;
    if (c->mesh->fills &&
        (uint32_t)count > c->mesh->fill_capacity - c->mesh->fill_count) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: fill capacity %u cannot hold %u more styles",
                  c->mesh->fill_capacity, count);
    }
}

static void tess_fill_style(void *user, uint16_t index,
                            const MenuSwfShapeFill *fill) {
    TessCtx *c = (TessCtx *)user;
    (void)index;
    if (!c->in_shape || c->status != MENU_SWF_OK || c->glyph_mode) return;
    if (!c->mesh->fills) return;
    if (c->mesh->fill_count >= c->mesh->fill_capacity) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: fill capacity %u exceeded",
                  c->mesh->fill_capacity);
        return;
    }
    c->mesh->fills[c->mesh->fill_count++] = *fill;
}

static void tess_begin_path(void *user, int32_t x, int32_t y, uint16_t fill0,
                            uint16_t fill1, uint16_t line) {
    TessCtx *c = (TessCtx *)user;
    if (!c->in_shape || c->status != MENU_SWF_OK) return;
    ++c->mesh->contour_count;
    if (c->mesh->contour_count > MENU_SWF_TESS_MAX_CONTOURS) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: contour limit %u exceeded",
                  MENU_SWF_TESS_MAX_CONTOURS);
        return;
    }
    c->in_path = 1;
    c->start_x = c->cur_x = x;
    c->start_y = c->cur_y = y;
    if (c->glyph_mode) {
        c->fill0 = 1;
        c->fill1 = 0;
        c->line = 0;
    } else {
        c->fill0 = fill0 ? (uint16_t)(fill0 + c->fill_base) : 0;
        c->fill1 = fill1 ? (uint16_t)(fill1 + c->fill_base) : 0;
        c->line = line;
    }
    add_bounds(c, x, y);
}

static void tess_line_to(void *user, int32_t x, int32_t y) {
    TessCtx *c = (TessCtx *)user;
    if (!c->in_path || c->status != MENU_SWF_OK) return;
    add_edge(c, c->cur_x, c->cur_y, x, y, 1);
    c->cur_x = x;
    c->cur_y = y;
}

static void tess_curve_to(void *user, int32_t cx, int32_t cy, int32_t ax,
                          int32_t ay) {
    TessCtx *c = (TessCtx *)user;
    if (!c->in_path || c->status != MENU_SWF_OK) return;
    unsigned segments = 0;
    flatten_curve(c, c->cur_x, c->cur_y, cx, cy, ax, ay,
                  MENU_SWF_TESS_CURVE_DEPTH, &segments);
    c->cur_x = ax;
    c->cur_y = ay;
}

static void tess_end_path(void *user) {
    TessCtx *c = (TessCtx *)user;
    if (!c->in_path) return;
    if (c->status == MENU_SWF_OK &&
        (c->cur_x != c->start_x || c->cur_y != c->start_y))
        add_edge(c, c->cur_x, c->cur_y, c->start_x, c->start_y, 0);
    c->in_path = 0;
}

static void tess_begin_font(void *user, uint16_t font_id, uint8_t tag_code,
                            uint16_t num_glyphs, uint8_t flags) {
    (void)tag_code;
    (void)num_glyphs;
    (void)flags;
    TessCtx *c = (TessCtx *)user;
    c->in_font = (font_id == c->font_id);
}

static void tess_begin_glyph(void *user, uint16_t glyph_index, uint16_t code,
                             uint32_t shape_offset) {
    (void)glyph_index;
    (void)shape_offset;
    TessCtx *c = (TessCtx *)user;
    c->in_shape = c->in_font && code == c->glyph_code && !c->shape_found;
    if (c->in_shape) c->shape_found = 1;
    c->in_path = 0;
}

static void tess_end_glyph(void *user) {
    TessCtx *c = (TessCtx *)user;
    c->in_shape = 0;
    c->in_path = 0;
}

static void tess_end_font(void *user) {
    TessCtx *c = (TessCtx *)user;
    c->in_font = 0;
}

static int32_t edge_x_at(const MenuSwfTessEdge *e, int32_t y) {
    int64_t dy = (int64_t)e->y1 - e->y0;
    if (dy <= 0) return e->x0;
    int64_t num = (int64_t)e->x0 * dy +
                  ((int64_t)e->x1 - e->x0) * ((int64_t)y - e->y0);
    return (int32_t)(num / dy);
}

static int cmp_i32(const void *pa, const void *pb) {
    int32_t a = *(const int32_t *)pa;
    int32_t b = *(const int32_t *)pb;
    return (a > b) - (a < b);
}

static int push_vertex(TessCtx *c, int32_t x, int32_t y, uint32_t *index) {
    MenuSwfTessMesh *m = c->mesh;
    if (m->vertex_count >= m->vertex_capacity ||
        m->vertex_count >= MENU_SWF_TESS_MAX_VERTICES) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: vertex capacity %u exceeded",
                  m->vertex_capacity);
        return -1;
    }
    *index = m->vertex_count;
    m->vertices[m->vertex_count].x = x;
    m->vertices[m->vertex_count].y = y;
    ++m->vertex_count;
    add_bounds(c, x, y);
    return 0;
}

static int push_triangle(TessCtx *c, uint32_t a, uint32_t b, uint32_t d,
                         uint16_t style) {
    MenuSwfTessMesh *m = c->mesh;
    if (m->index_count + 3 > m->index_capacity ||
        m->triangle_count >= m->triangle_capacity ||
        m->index_count + 3 > MENU_SWF_TESS_MAX_INDICES ||
        m->triangle_count >= MENU_SWF_TESS_MAX_TRIANGLES) {
        tess_fail(c, MENU_SWF_ERR_BUDGET,
                  "swf tess: index/triangle capacity exceeded");
        return -1;
    }
    m->indices[m->index_count++] = a;
    m->indices[m->index_count++] = b;
    m->indices[m->index_count++] = d;
    m->triangle_fills[m->triangle_count++] = style;
    return 0;
}

static int64_t tri_area2(const MenuSwfTessVertex *a,
                         const MenuSwfTessVertex *b,
                         const MenuSwfTessVertex *d) {
    return ((int64_t)b->x - a->x) * ((int64_t)d->y - a->y) -
           ((int64_t)b->y - a->y) * ((int64_t)d->x - a->x);
}

static int emit_span(TessCtx *c, const MenuSwfTessEdge *left,
                     const MenuSwfTessEdge *right, int32_t y0, int32_t y1,
                     uint16_t style) {
    int32_t ltx = left->xtop, rtx = right->xtop;
    int32_t lbx = left->xbot, rbx = right->xbot;
    int64_t top_width = (int64_t)rtx - ltx;
    int64_t bottom_width = (int64_t)rbx - lbx;
    if (top_width <= 0 && bottom_width <= 0) return 0;

    uint32_t base = c->mesh->vertex_count;
    if (top_width > 0 && bottom_width > 0) {
        uint32_t v0, v1, v2, v3;
        if (push_vertex(c, ltx, y0, &v0) ||
            push_vertex(c, rtx, y0, &v1) ||
            push_vertex(c, rbx, y1, &v2) ||
            push_vertex(c, lbx, y1, &v3))
            return -1;
        if (tri_area2(&c->mesh->vertices[v0], &c->mesh->vertices[v1],
                      &c->mesh->vertices[v2]) <= 0 ||
            tri_area2(&c->mesh->vertices[v0], &c->mesh->vertices[v2],
                      &c->mesh->vertices[v3]) <= 0) {
            c->mesh->vertex_count = base;
            return 0;
        }
        return push_triangle(c, v0, v1, v2, style) ||
               push_triangle(c, v0, v2, v3, style);
    }
    if (top_width <= 0 && bottom_width > 0) {
        uint32_t v0, v1, v2;
        if (push_vertex(c, ltx, y0, &v0) ||
            push_vertex(c, lbx, y1, &v1) ||
            push_vertex(c, rbx, y1, &v2))
            return -1;
        if (tri_area2(&c->mesh->vertices[v0], &c->mesh->vertices[v1],
                      &c->mesh->vertices[v2]) <= 0) {
            c->mesh->vertex_count = base;
            return 0;
        }
        return push_triangle(c, v0, v1, v2, style);
    }
    if (top_width > 0 && bottom_width <= 0) {
        uint32_t v0, v1, v2;
        if (push_vertex(c, ltx, y0, &v0) ||
            push_vertex(c, rtx, y0, &v1) ||
            push_vertex(c, lbx, y1, &v2))
            return -1;
        if (tri_area2(&c->mesh->vertices[v0], &c->mesh->vertices[v1],
                      &c->mesh->vertices[v2]) <= 0) {
            c->mesh->vertex_count = base;
            return 0;
        }
        return push_triangle(c, v0, v1, v2, style);
    }
    return 0;
}

static int tess_group(TessCtx *c, uint16_t style) {
    MenuSwfTessMesh *m = c->mesh;
    uint32_t nbands = 0;
    for (uint32_t i = 0; i < m->edge_count; ++i) {
        MenuSwfTessEdge *e = &m->edges[i];
        if (e->fill0 != style && e->fill1 != style) continue;
        if (nbands + 2 > m->band_capacity ||
            nbands + 2 > MENU_SWF_TESS_MAX_BANDS) {
            tess_fail(c, MENU_SWF_ERR_BUDGET,
                      "swf tess: band capacity %u exceeded",
                      m->band_capacity);
            return -1;
        }
        m->bands[nbands++] = e->y0;
        m->bands[nbands++] = e->y1;
        e->active = 0;
    }
    if (nbands == 0) return 0;
    qsort(m->bands, nbands, sizeof m->bands[0], cmp_i32);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < nbands; ++i) {
        if (unique == 0 || m->bands[i] != m->bands[unique - 1])
            m->bands[unique++] = m->bands[i];
    }
    nbands = unique;

    uint32_t active_count = 0;
    for (uint32_t bi = 0; bi + 1 < nbands; ++bi) {
        int32_t y0 = m->bands[bi];
        int32_t y1 = m->bands[bi + 1];
        if (y0 == y1) continue;

        uint32_t kept = 0;
        for (uint32_t k = 0; k < active_count; ++k) {
            uint32_t ei = m->active[k];
            MenuSwfTessEdge *e = &m->edges[ei];
            if (e->y0 <= y0 && e->y1 >= y1) {
                m->active[kept++] = ei;
            } else {
                e->active = 0;
            }
        }
        active_count = kept;

        for (uint32_t i = 0; i < m->edge_count; ++i) {
            MenuSwfTessEdge *e = &m->edges[i];
            if (e->fill0 != style && e->fill1 != style) continue;
            if (e->active) continue;
            if (e->y0 <= y0 && e->y1 >= y1) {
                if (active_count >= m->active_capacity ||
                    active_count >= MENU_SWF_TESS_MAX_EDGES) {
                    tess_fail(c, MENU_SWF_ERR_BUDGET,
                              "swf tess: active edge capacity %u exceeded",
                              m->active_capacity);
                    return -1;
                }
                e->active = 1;
                m->active[active_count++] = i;
            }
        }
        if (active_count < 2) continue;

        for (uint32_t k = 0; k < active_count; ++k) {
            MenuSwfTessEdge *e = &m->edges[m->active[k]];
            e->xtop = edge_x_at(e, y0);
            e->xbot = edge_x_at(e, y1);
            e->xmid = edge_x_at(e, y0 + (y1 - y0) / 2);
        }
        for (uint32_t k = 1; k < active_count; ++k) {
            uint32_t key = m->active[k];
            int32_t kx = m->edges[key].xmid;
            uint32_t j = k;
            while (j > 0 && m->edges[m->active[j - 1]].xmid > kx) {
                m->active[j] = m->active[j - 1];
                --j;
            }
            m->active[j] = key;
        }

        int winding = 0;
        int inside = 0;
        uint32_t left = 0;
        for (uint32_t k = 0; k < active_count; ++k) {
            MenuSwfTessEdge *e = &m->edges[m->active[k]];
            int prev = inside;
            if (c->rule == MENU_SWF_TESS_EVEN_ODD) {
                inside = !inside;
            } else {
                winding += e->dir;
                inside = winding != 0;
            }
            if (!prev && inside) {
                left = k;
            } else if (prev && !inside) {
                if (emit_span(c, &m->edges[m->active[left]], e, y0, y1,
                              style))
                    return -1;
            }
        }
    }
    return 0;
}

static int check_buffers(const MenuSwfTessMesh *m, char *reason,
                         size_t reason_cap) {
    if ((!m->vertices && m->vertex_capacity) ||
        (!m->indices && m->index_capacity) ||
        (!m->triangle_fills && m->triangle_capacity) ||
        (!m->lines && m->line_capacity) ||
        (!m->edges && m->edge_capacity) ||
        (!m->active && m->active_capacity) ||
        (!m->bands && m->band_capacity) ||
        (!m->fills && m->fill_capacity)) {
        tess_reason(reason, reason_cap,
                    "swf tess: NULL buffer with a nonzero capacity");
        return -1;
    }
    return 0;
}

static MenuSwfStatus tess_finish(TessCtx *c, char *reason,
                                 size_t reason_cap) {
    MenuSwfTessMesh *mesh = c->mesh;
    if (c->status != MENU_SWF_OK) {
        MenuSwfStatus failed = c->status;
        reset_output(mesh);
        return failed;
    }
    if (!c->shape_found) {
        reset_output(mesh);
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (mesh->edge_count == 0) return MENU_SWF_OK;

    uint16_t styles[MENU_SWF_SHAPE_MAX_STYLES];
    uint32_t style_count = 0;
    for (uint32_t i = 0; i < mesh->edge_count; ++i) {
        uint16_t values[2] = {mesh->edges[i].fill0, mesh->edges[i].fill1};
        for (unsigned k = 0; k < 2; ++k) {
            uint16_t style = values[k];
            if (!style) continue;
            uint32_t j = 0;
            for (; j < style_count; ++j)
                if (styles[j] == style) break;
            if (j == style_count) {
                if (style_count >= MENU_SWF_SHAPE_MAX_STYLES) {
                    tess_reason(reason, reason_cap,
                                "swf tess: style limit exceeded");
                    reset_output(mesh);
                    return MENU_SWF_ERR_BUDGET;
                }
                styles[style_count++] = style;
            }
        }
    }

    for (uint32_t i = 0; i < style_count; ++i) {
        if (tess_group(c, styles[i])) {
            MenuSwfStatus failed = c->status;
            reset_output(mesh);
            return failed;
        }
    }
    return MENU_SWF_OK;
}

static MenuSwfStatus tess_setup(const uint8_t *data, MenuSwfTessMesh *mesh,
                                MenuSwfTessFillRule fill_rule, char *reason,
                                size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!data || !mesh) {
        tess_reason(reason, reason_cap,
                    "swf tess: needs SWF bytes and a mesh");
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (fill_rule != MENU_SWF_TESS_EVEN_ODD &&
        fill_rule != MENU_SWF_TESS_NON_ZERO) {
        tess_reason(reason, reason_cap, "swf tess: unknown fill rule %d",
                    (int)fill_rule);
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (check_buffers(mesh, reason, reason_cap)) return MENU_SWF_ERR_ARGUMENT;
    reset_output(mesh);
    mesh->fill_rule = (uint8_t)fill_rule;
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_tess_shape(const uint8_t *data, size_t size,
                                  uint16_t shape_id,
                                  MenuSwfTessFillRule fill_rule,
                                  MenuSwfTessMesh *mesh, char *reason,
                                  size_t reason_cap) {
    MenuSwfStatus st = tess_setup(data, mesh, fill_rule, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    TessCtx c;
    memset(&c, 0, sizeof c);
    c.mesh = mesh;
    c.shape_id = shape_id;
    c.rule = fill_rule;
    c.status = MENU_SWF_OK;
    c.reason = reason;
    c.reason_cap = reason_cap;

    MenuSwfShapeSink sink;
    memset(&sink, 0, sizeof sink);
    sink.user = &c;
    sink.begin_shape = tess_begin_shape;
    sink.end_shape = tess_end_shape;
    sink.begin_fill_styles = tess_begin_fill_styles;
    sink.fill_style = tess_fill_style;
    sink.begin_path = tess_begin_path;
    sink.line_to = tess_line_to;
    sink.curve_to = tess_curve_to;
    sink.end_path = tess_end_path;

    st = menu_swf_shape_walk(data, size, &sink, reason, reason_cap);
    if (st != MENU_SWF_OK) {
        reset_output(mesh);
        return st;
    }
    if (!c.shape_found) {
        tess_reason(reason, reason_cap, "swf tess: shape %u not found",
                    shape_id);
        reset_output(mesh);
        return MENU_SWF_ERR_ARGUMENT;
    }
    return tess_finish(&c, reason, reason_cap);
}

MenuSwfStatus menu_swf_tess_glyph(const uint8_t *data, size_t size,
                                  uint16_t font_id, uint16_t code,
                                  MenuSwfTessFillRule fill_rule,
                                  MenuSwfTessMesh *mesh, char *reason,
                                  size_t reason_cap) {
    MenuSwfStatus st = tess_setup(data, mesh, fill_rule, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    TessCtx c;
    memset(&c, 0, sizeof c);
    c.mesh = mesh;
    c.font_id = font_id;
    c.glyph_code = code;
    c.rule = fill_rule;
    c.glyph_mode = 1;
    c.status = MENU_SWF_OK;
    c.reason = reason;
    c.reason_cap = reason_cap;

    MenuSwfFontSink sink;
    memset(&sink, 0, sizeof sink);
    sink.user = &c;
    sink.begin_font = tess_begin_font;
    sink.begin_glyph = tess_begin_glyph;
    sink.end_glyph = tess_end_glyph;
    sink.end_font = tess_end_font;
    sink.begin_path = tess_begin_path;
    sink.line_to = tess_line_to;
    sink.curve_to = tess_curve_to;
    sink.end_path = tess_end_path;

    st = menu_swf_shape_walk_fonts(data, size, &sink, reason, reason_cap);
    if (st != MENU_SWF_OK) {
        reset_output(mesh);
        return st;
    }
    if (!c.shape_found) {
        tess_reason(reason, reason_cap,
                    "swf tess: glyph font %u code %u not found", font_id,
                    code);
        reset_output(mesh);
        return MENU_SWF_ERR_ARGUMENT;
    }
    return tess_finish(&c, reason, reason_cap);
}
