#include "menu_swf_shape.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t off;
} Reader;

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t bit;
} Bits;

typedef struct {
    uint16_t fill_count, line_count;
    uint16_t solid_fills, gradient_fills, bitmap_fills;
    uint16_t solid_lines, gradient_lines, bitmap_lines;
} StyleState;

typedef struct {
    uint32_t path_count;
    uint32_t point_count;
    uint32_t segment_count;
    uint32_t style_changes;
    int32_t xmin, xmax, ymin, ymax;
    int has_bounds;
} GeomStats;

typedef struct {
    MenuSwfShapePath *pool;
    uint32_t cap;
    uint32_t *used;
    uint32_t *first;
    uint32_t *stored;
    int *truncated;
} PathSink;

typedef struct {
    int active;
    int32_t sx, sy, x, y;
    int32_t minx, maxx, miny, maxy;
    uint32_t points, segments;
    uint16_t fill0, fill1, line;
    int has_curve;
    int styles_changed;
} Path;

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static int rd_u8(Reader *r, uint8_t *v) {
    if (r->off >= r->len) return -1;
    *v = r->p[r->off++];
    return 0;
}

static int rd_u16(Reader *r, uint16_t *v) {
    if (r->len - r->off < 2) return -1;
    *v = (uint16_t)(r->p[r->off] | ((uint16_t)r->p[r->off + 1] << 8));
    r->off += 2;
    return 0;
}

static int rd_u32(Reader *r, uint32_t *v) {
    if (r->len - r->off < 4) return -1;
    *v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
         ((uint32_t)r->p[r->off + 2] << 16) |
         ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return 0;
}

static int bt_bits(Bits *b, unsigned n, uint32_t *out) {
    if (n > 31 || b->bit + n > b->len * 8) return -1;
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        size_t bit = b->bit + i;
        v = (v << 1) | ((b->p[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    b->bit += n;
    *out = v;
    return 0;
}

static int bt_signed(Bits *b, unsigned n, int32_t *out) {
    uint32_t v;
    if (bt_bits(b, n, &v)) return -1;
    if (n && (v & (1u << (n - 1)))) v |= ~((1u << n) - 1u);
    *out = (int32_t)v;
    return 0;
}

static void bt_align(Bits *b) {
    b->bit = (b->bit + 7u) & ~(size_t)7u;
}

static int read_rect(Bits *b, MenuSwfShapeRect *r) {
    uint32_t nbits;
    int32_t v;
    if (bt_bits(b, 5, &nbits) || nbits > 31) return -1;
    if (bt_signed(b, nbits, &v)) return -1;
    r->xmin = v;
    if (bt_signed(b, nbits, &v)) return -1;
    r->xmax = v;
    if (bt_signed(b, nbits, &v)) return -1;
    r->ymin = v;
    if (bt_signed(b, nbits, &v)) return -1;
    r->ymax = v;
    return 0;
}

static int skip_matrix(Reader *r) {
    Bits b = {r->p, r->len, r->off * 8};
    uint32_t flag, nbits;
    int32_t v;
    if (bt_bits(&b, 1, &flag)) return -1;
    if (flag) {
        if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
        if (bt_signed(&b, nbits, &v) || bt_signed(&b, nbits, &v)) return -1;
    }
    if (bt_bits(&b, 1, &flag)) return -1;
    if (flag) {
        if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
        if (bt_signed(&b, nbits, &v) || bt_signed(&b, nbits, &v)) return -1;
    }
    if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
    if (bt_signed(&b, nbits, &v) || bt_signed(&b, nbits, &v)) return -1;
    bt_align(&b);
    if (b.bit > r->len * 8) return -1;
    r->off = b.bit / 8;
    return 0;
}

static int parse_fill_style(Reader *r, int shape_type, StyleState *st,
                            char *reason, size_t reason_cap) {
    uint8_t type;
    if (rd_u8(r, &type)) return -1;
    if (type == 0x00) {
        size_t n = shape_type >= 3 ? 4u : 3u;
        if (r->len - r->off < n) return -1;
        r->off += n;
        ++st->solid_fills;
        return 0;
    }
    if (type == 0x10 || type == 0x12 || type == 0x13) {
        if (skip_matrix(r)) return -1;
        uint8_t header;
        if (rd_u8(r, &header)) return -1;
        unsigned colors = header & 0x0fu;
        if (!colors) {
            set_reason(reason, reason_cap,
                       "swf shape: gradient has zero color records");
            return -1;
        }
        size_t color_size = shape_type >= 3 ? 4u : 3u;
        if ((uint64_t)colors * (1u + color_size) > r->len - r->off) return -1;
        r->off += colors * (1u + color_size);
        if (type == 0x13) {
            if (r->len - r->off < 4) return -1;
            r->off += 4;
        }
        ++st->gradient_fills;
        return 0;
    }
    if (type == 0x40 || type == 0x41 || type == 0x42 || type == 0x43) {
        uint16_t bitmap_id;
        if (rd_u16(r, &bitmap_id)) return -1;
        if (!bitmap_id) {
            set_reason(reason, reason_cap,
                       "swf shape: bitmap fill has no bitmap id");
            return -1;
        }
        if (skip_matrix(r)) return -1;
        ++st->bitmap_fills;
        return 0;
    }
    set_reason(reason, reason_cap,
               "swf shape: fill style 0x%02x is unsupported", type);
    return -2;
}

static int parse_fill_array(Reader *r, int shape_type, StyleState *st,
                            char *reason, size_t reason_cap) {
    uint8_t count8;
    uint32_t count;
    if (rd_u8(r, &count8)) return -1;
    count = count8;
    if (count8 == 0xff && shape_type >= 2) {
        uint16_t c16;
        if (rd_u16(r, &c16)) return -1;
        count = c16;
    }
    if (count > MENU_SWF_SHAPE_MAX_STYLES) {
        set_reason(reason, reason_cap,
                   "swf shape: %u fill styles exceeds %u",
                   count, MENU_SWF_SHAPE_MAX_STYLES);
        return -3;
    }
    st->fill_count = (uint16_t)count;
    st->solid_fills = st->gradient_fills = st->bitmap_fills = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int rc = parse_fill_style(r, shape_type, st, reason, reason_cap);
        if (rc) return rc;
    }
    return 0;
}

static int parse_line_array(Reader *r, int shape_type, StyleState *st,
                            char *reason, size_t reason_cap) {
    uint8_t count8;
    uint32_t count;
    if (rd_u8(r, &count8)) return -1;
    count = count8;
    if (count8 == 0xff && shape_type >= 2) {
        uint16_t c16;
        if (rd_u16(r, &c16)) return -1;
        count = c16;
    }
    if (count > MENU_SWF_SHAPE_MAX_STYLES) {
        set_reason(reason, reason_cap,
                   "swf shape: %u line styles exceeds %u",
                   count, MENU_SWF_SHAPE_MAX_STYLES);
        return -3;
    }
    st->line_count = (uint16_t)count;
    st->solid_lines = st->gradient_lines = st->bitmap_lines = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t width;
        if (rd_u16(r, &width)) return -1;
        if (shape_type == 4) {
            Bits b = {r->p, r->len, r->off * 8};
            uint32_t start_cap, join, has_fill, noh, nov, pixel, reserved,
                     noclose, end_cap;
            if (bt_bits(&b, 2, &start_cap) || bt_bits(&b, 2, &join) ||
                bt_bits(&b, 1, &has_fill) || bt_bits(&b, 1, &noh) ||
                bt_bits(&b, 1, &nov) || bt_bits(&b, 1, &pixel) ||
                bt_bits(&b, 5, &reserved) || bt_bits(&b, 1, &noclose) ||
                bt_bits(&b, 2, &end_cap))
                return -1;
            bt_align(&b);
            if (b.bit > r->len * 8) return -1;
            r->off = b.bit / 8;
            if (join == 2) {
                uint16_t miter;
                if (rd_u16(r, &miter)) return -1;
            }
            if (has_fill) {
                StyleState dummy = {0};
                int rc = parse_fill_style(r, 4, &dummy, reason, reason_cap);
                if (rc) return rc;
                ++st->gradient_lines;
            } else {
                if (r->len - r->off < 4) return -1;
                r->off += 4;
                ++st->solid_lines;
            }
        } else {
            size_t n = shape_type >= 3 ? 4u : 3u;
            if (r->len - r->off < n) return -1;
            r->off += n;
            ++st->solid_lines;
        }
    }
    return 0;
}

static void path_start(Path *p, int32_t x, int32_t y, uint16_t fill0,
                       uint16_t fill1, uint16_t line) {
    memset(p, 0, sizeof *p);
    p->active = 1;
    p->sx = p->x = x;
    p->sy = p->y = y;
    p->minx = p->maxx = x;
    p->miny = p->maxy = y;
    p->points = 1;
    p->fill0 = fill0;
    p->fill1 = fill1;
    p->line = line;
}

static void path_point(Path *p, int32_t x, int32_t y) {
    if (x < p->minx) p->minx = x;
    if (x > p->maxx) p->maxx = x;
    if (y < p->miny) p->miny = y;
    if (y > p->maxy) p->maxy = y;
    ++p->points;
    p->x = x;
    p->y = y;
}

static void stats_bounds(GeomStats *g, const Path *p) {
    if (!g->has_bounds) {
        g->xmin = p->minx; g->xmax = p->maxx;
        g->ymin = p->miny; g->ymax = p->maxy;
        g->has_bounds = 1;
    } else {
        if (p->minx < g->xmin) g->xmin = p->minx;
        if (p->maxx > g->xmax) g->xmax = p->maxx;
        if (p->miny < g->ymin) g->ymin = p->miny;
        if (p->maxy > g->ymax) g->ymax = p->maxy;
    }
}

static void path_finish(PathSink *sink, GeomStats *g, Path *p) {
    if (!p->active || p->segments == 0) {
        p->active = 0;
        return;
    }
    MenuSwfShapePath out;
    memset(&out, 0, sizeof out);
    out.start_x = p->sx;
    out.start_y = p->sy;
    out.end_x = p->x;
    out.end_y = p->y;
    out.xmin = p->minx;
    out.xmax = p->maxx;
    out.ymin = p->miny;
    out.ymax = p->maxy;
    out.point_count = p->points;
    out.segment_count = p->segments;
    out.fill0 = p->fill0;
    out.fill1 = p->fill1;
    out.line = p->line;
    out.closed = (p->x == p->sx && p->y == p->sy && p->segments > 1) ? 1 : 0;
    out.has_curve = (uint8_t)p->has_curve;
    out.styles_changed = (uint8_t)p->styles_changed;

    ++g->path_count;
    g->point_count += p->points;
    g->segment_count += p->segments;
    stats_bounds(g, p);
    if (*sink->stored == 0) *sink->first = *sink->used;
    if (*sink->used < sink->cap) {
        sink->pool[*sink->used] = out;
        ++*sink->used;
        ++*sink->stored;
    } else {
        *sink->truncated = 1;
    }
    p->active = 0;
}

/* Parse SHAPE records.  `glyph_mode` skips style-index validation because a
 * glyph SHAPE has no FILLSTYLEARRAY/LINESTYLEARRAY. */
static int parse_records(const uint8_t *p, size_t len, size_t start_byte,
                         int shape_type, int glyph_mode, StyleState *styles,
                         GeomStats *g, PathSink *sink, uint32_t max_segments,
                         char *reason, size_t reason_cap) {
    Bits b = {p, len, start_byte * 8};
    uint32_t fill_bits, line_bits;
    if (bt_bits(&b, 4, &fill_bits) || bt_bits(&b, 4, &line_bits)) return -1;
    uint16_t fill0 = 0, fill1 = 0, line = 0;
    int32_t x = 0, y = 0;
    Path path;
    memset(&path, 0, sizeof path);

    for (;;) {
        uint32_t tf;
        if (bt_bits(&b, 1, &tf)) return -1;
        if (tf == 0) {
            uint32_t newstyles, ls, f1, f0, mv;
            if (bt_bits(&b, 1, &newstyles) || bt_bits(&b, 1, &ls) ||
                bt_bits(&b, 1, &f1) || bt_bits(&b, 1, &f0) ||
                bt_bits(&b, 1, &mv)) return -1;
            if (!newstyles && !ls && !f1 && !f0 && !mv) {
                path_finish(sink, g, &path);
                return 0;
            }
            int32_t dx = 0, dy = 0;
            int split = 0;
            if (mv) {
                uint32_t mb;
                if (bt_bits(&b, 5, &mb) || bt_signed(&b, mb, &dx) ||
                    bt_signed(&b, mb, &dy)) return -1;
                split = 1;
            }
            uint16_t nf0 = fill0, nf1 = fill1, nl = line;
            if (f0) {
                uint32_t v;
                if (bt_bits(&b, fill_bits, &v)) return -1;
                nf0 = (uint16_t)v;
            }
            if (f1) {
                uint32_t v;
                if (bt_bits(&b, fill_bits, &v)) return -1;
                nf1 = (uint16_t)v;
            }
            if (ls) {
                uint32_t v;
                if (bt_bits(&b, line_bits, &v)) return -1;
                nl = (uint16_t)v;
            }
            if (newstyles) {
                if (glyph_mode) {
                    set_reason(reason, reason_cap,
                               "swf glyph shape: StateNewStyles is unsupported");
                    return -2;
                }
                bt_align(&b);
                Reader r = {p, len, b.bit / 8};
                int rc = parse_fill_array(&r, shape_type, styles, reason,
                                          reason_cap);
                if (!rc) rc = parse_line_array(&r, shape_type, styles, reason,
                                               reason_cap);
                if (rc) return rc;
                b.bit = r.off * 8;
                Bits nb = {p, len, b.bit};
                if (bt_bits(&nb, 4, &fill_bits) ||
                    bt_bits(&nb, 4, &line_bits)) return -1;
                b = nb;
            }
            if (!glyph_mode) {
                if (nf0 > styles->fill_count || nf1 > styles->fill_count ||
                    nl > styles->line_count) {
                    set_reason(reason, reason_cap,
                               "swf shape: style index out of range");
                    return -1;
                }
            }
            int styles_changed = (f0 || f1 || ls || newstyles);
            if (styles_changed) ++g->style_changes;
            fill0 = nf0; fill1 = nf1; line = nl;
            if (split) {
                path_finish(sink, g, &path);
                path_start(&path, dx, dy, fill0, fill1, line);
                x = dx; y = dy;
            } else if (path.active) {
                if (styles_changed) {
                    if (path.segments) path.styles_changed = 1;
                    else {
                        path.fill0 = fill0;
                        path.fill1 = fill1;
                        path.line = line;
                    }
                }
            }
        } else {
            uint32_t straight, nbits;
            if (bt_bits(&b, 1, &straight) || bt_bits(&b, 4, &nbits)) return -1;
            if (!path.active) path_start(&path, x, y, fill0, fill1, line);
            if (straight) {
                uint32_t general;
                int32_t dx = 0, dy = 0;
                if (bt_bits(&b, 1, &general)) return -1;
                if (general) {
                    if (bt_signed(&b, nbits + 2, &dx) ||
                        bt_signed(&b, nbits + 2, &dy)) return -1;
                } else {
                    uint32_t vert;
                    if (bt_bits(&b, 1, &vert)) return -1;
                    if (vert) {
                        if (bt_signed(&b, nbits + 2, &dy)) return -1;
                    } else {
                        if (bt_signed(&b, nbits + 2, &dx)) return -1;
                    }
                }
                x += dx;
                y += dy;
                path_point(&path, x, y);
                ++path.segments;
            } else {
                int32_t cdx, cdy, adx, ady;
                if (bt_signed(&b, nbits + 2, &cdx) ||
                    bt_signed(&b, nbits + 2, &cdy) ||
                    bt_signed(&b, nbits + 2, &adx) ||
                    bt_signed(&b, nbits + 2, &ady)) return -1;
                int32_t cx = x + cdx, cy = y + cdy;
                int32_t ex = cx + adx, ey = cy + ady;
                path_point(&path, cx, cy);
                path_point(&path, ex, ey);
                x = ex; y = ey;
                path.has_curve = 1;
                ++path.segments;
            }
            if (path.segments > max_segments ||
                g->segment_count + path.segments > max_segments) {
                set_reason(reason, reason_cap,
                           "swf shape: segment limit %u exceeded", max_segments);
                return -3;
            }
        }
    }
}

static int parse_shape_tag(const uint8_t *p, size_t len, uint8_t tag_code,
                           MenuSwfShapeResult *out, char *reason,
                           size_t reason_cap) {
    int shape_type = tag_code == 2 ? 1 : tag_code == 22 ? 2 : tag_code == 32 ? 3 : 4;
    Reader r = {p, len, 0};
    MenuSwfShapeSummary shape;
    memset(&shape, 0, sizeof shape);
    if (rd_u16(&r, &shape.shape_id)) return -1;
    shape.tag_code = tag_code;

    Bits b = {p, len, r.off * 8};
    if (read_rect(&b, &shape.bounds)) return -1;
    bt_align(&b);
    if (shape_type == 4) {
        if (read_rect(&b, &shape.edge_bounds)) return -1;
        bt_align(&b);
        uint32_t reserved, fw, ns, ss;
        if (bt_bits(&b, 5, &reserved) || bt_bits(&b, 1, &fw) ||
            bt_bits(&b, 1, &ns) || bt_bits(&b, 1, &ss)) return -1;
        shape.has_edge_bounds = 1;
        shape.uses_fill_winding = (uint8_t)fw;
        shape.uses_non_scaling_strokes = (uint8_t)ns;
        shape.uses_scaling_strokes = (uint8_t)ss;
    }
    bt_align(&b);
    if (b.bit > len * 8) return -1;
    r.off = b.bit / 8;

    StyleState styles;
    memset(&styles, 0, sizeof styles);
    int rc = parse_fill_array(&r, shape_type, &styles, reason, reason_cap);
    if (!rc) rc = parse_line_array(&r, shape_type, &styles, reason, reason_cap);
    if (rc) return rc;

    GeomStats g;
    memset(&g, 0, sizeof g);
    PathSink sink = {out->shape_paths, MENU_SWF_SHAPE_MAX_PATHS,
                     &out->shape_path_used, &shape.path_first,
                     &shape.stored_paths, &shape.truncated};
    rc = parse_records(p, len, r.off, shape_type, 0, &styles, &g, &sink,
                       MENU_SWF_SHAPE_MAX_SEGMENTS, reason, reason_cap);
    if (rc) return rc;

    shape.fill_count = styles.fill_count;
    shape.line_count = styles.line_count;
    shape.solid_fills = styles.solid_fills;
    shape.gradient_fills = styles.gradient_fills;
    shape.bitmap_fills = styles.bitmap_fills;
    shape.solid_lines = styles.solid_lines;
    shape.gradient_lines = styles.gradient_lines;
    shape.bitmap_lines = styles.bitmap_lines;
    shape.path_count = g.path_count;
    shape.point_count = g.point_count;
    shape.segment_count = g.segment_count;
    shape.style_changes = g.style_changes;

    if (out->stored_shapes >= MENU_SWF_SHAPE_MAX_SHAPES) {
        set_reason(reason, reason_cap, "swf shape: shape limit %u exceeded",
                   MENU_SWF_SHAPE_MAX_SHAPES);
        return -3;
    }
    out->shapes[out->stored_shapes++] = shape;
    return 0;
}

/* Parse DefineFont2/3 glyph SHAPE records. */
static int parse_font_tag(const uint8_t *p, size_t len, uint8_t tag_code,
                          MenuSwfShapeResult *out, char *reason,
                          size_t reason_cap) {
    Reader r = {p, len, 0};
    uint16_t font_id, num_glyphs;
    uint8_t flags, lang, name_len;
    if (rd_u16(&r, &font_id) || rd_u8(&r, &flags) || rd_u8(&r, &lang) ||
        rd_u8(&r, &name_len)) return -1;
    if (r.len - r.off < name_len) return -1;
    r.off += name_len;
    if (rd_u16(&r, &num_glyphs)) return -1;
    if (num_glyphs > MENU_SWF_SHAPE_MAX_GLYPHS ||
        out->glyph_total > MENU_SWF_SHAPE_MAX_GLYPHS - num_glyphs) {
        set_reason(reason, reason_cap, "swf shape: glyph limit %u exceeded",
                   MENU_SWF_SHAPE_MAX_GLYPHS);
        return -3;
    }
    int wide_offsets = (flags & 0x08) != 0;
    int wide_codes = (flags & 0x04) != 0;
    if (tag_code == 75 && !wide_codes) {
        set_reason(reason, reason_cap,
                   "swf shape: DefineFont3 requires wide codes");
        return -1;
    }
    uint32_t offset_base = (uint32_t)r.off;
    size_t offset_size = wide_offsets ? 4u : 2u;
    if ((uint64_t)num_glyphs * offset_size > r.len - r.off) return -1;
    uint32_t offsets[MENU_SWF_SHAPE_MAX_GLYPHS];
    for (uint32_t i = 0; i < num_glyphs; ++i) {
        if (offset_size == 4) {
            if (rd_u32(&r, &offsets[i])) return -1;
        } else {
            uint16_t o;
            if (rd_u16(&r, &o)) return -1;
            offsets[i] = o;
        }
    }
    uint32_t code_off;
    if (wide_offsets) {
        if (rd_u32(&r, &code_off)) return -1;
    } else {
        uint16_t o;
        if (rd_u16(&r, &o)) return -1;
        code_off = o;
    }
    uint64_t code_table64 = (uint64_t)offset_base + code_off;
    if (code_table64 > len) return -1;
    uint32_t code_table = (uint32_t)code_table64;
    size_t code_size = wide_codes ? 2u : 1u;
    if ((uint64_t)num_glyphs * code_size > len - code_table) return -1;

    ++out->font_total;
    ++out->stored_fonts;
    for (uint32_t i = 0; i < num_glyphs; ++i) {
        uint64_t start64 = (uint64_t)offset_base + offsets[i];
        uint64_t end64 = i + 1 < num_glyphs
                             ? (uint64_t)offset_base + offsets[i + 1]
                             : code_table64;
        if (start64 > end64 || end64 > len) {
            set_reason(reason, reason_cap,
                       "swf shape: glyph %u shape range is invalid", i);
            return -1;
        }
        uint32_t start = (uint32_t)start64;
        uint32_t end = (uint32_t)end64;
        uint32_t code = code_size == 2
                            ? (uint32_t)p[code_table + i * 2] |
                                  ((uint32_t)p[code_table + i * 2 + 1] << 8)
                            : p[code_table + i];
        MenuSwfShapeGlyph glyph;
        memset(&glyph, 0, sizeof glyph);
        glyph.font_id = font_id;
        glyph.glyph_index = (uint16_t)i;
        glyph.code = (uint16_t)code;
        glyph.shape_offset = start;
        if (start < end) {
            GeomStats g;
            memset(&g, 0, sizeof g);
            StyleState styles;
            memset(&styles, 0, sizeof styles);
            PathSink sink = {out->glyph_paths, MENU_SWF_SHAPE_MAX_GLYPH_PATHS,
                             &out->glyph_path_used, &glyph.path_first,
                             &glyph.stored_paths, &glyph.truncated};
            int rc = parse_records(p, end, start, 3, 1, &styles, &g, &sink,
                                   MENU_SWF_SHAPE_MAX_SEGMENTS, reason,
                                   reason_cap);
            if (rc) return rc;
            glyph.path_count = g.path_count;
            glyph.point_count = g.point_count;
            glyph.segment_count = g.segment_count;
            glyph.has_geometry = g.segment_count > 0;
            if (g.has_bounds) {
                glyph.bounds.xmin = g.xmin;
                glyph.bounds.xmax = g.xmax;
                glyph.bounds.ymin = g.ymin;
                glyph.bounds.ymax = g.ymax;
            }
        }
        out->glyphs[out->stored_glyphs++] = glyph;
    }
    out->glyph_total += num_glyphs;
    return 0;
}

typedef struct {
    MenuSwfShapeResult *out;
    uint32_t tags_seen;
} ShapeContext;

static MenuSwfStatus walk_tags(ShapeContext *ctx, const uint8_t *p, size_t len,
                               unsigned depth, char *reason,
                               size_t reason_cap) {
    size_t off = 0;
    while (off < len) {
        if (ctx->tags_seen++ >= MENU_SWF_MAX_TAGS) {
            set_reason(reason, reason_cap, "swf shape: tag limit exceeded");
            return MENU_SWF_ERR_TAG;
        }
        if (len - off < 2) {
            set_reason(reason, reason_cap, "swf shape: truncated tag header");
            return MENU_SWF_ERR_TRUNCATED;
        }
        uint16_t header = (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
        off += 2;
        uint32_t code = header >> 6;
        uint32_t tlen = header & 0x3fu;
        if (tlen == 0x3fu) {
            if (len - off < 4) {
                set_reason(reason, reason_cap,
                           "swf shape: truncated long tag header");
                return MENU_SWF_ERR_TRUNCATED;
            }
            tlen = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
                   ((uint32_t)p[off + 2] << 16) |
                   ((uint32_t)p[off + 3] << 24);
            off += 4;
        }
        if (tlen > len - off) {
            set_reason(reason, reason_cap,
                       "swf shape: tag %u length runs past the body", code);
            return MENU_SWF_ERR_TAG;
        }
        const uint8_t *payload = p + off;
        if (code == 0) {
            if (tlen != 0) {
                set_reason(reason, reason_cap, "swf shape: End has payload");
                return MENU_SWF_ERR_FORMAT;
            }
            return MENU_SWF_OK;
        }
        if (code == 2 || code == 22 || code == 32 || code == 83) {
            ++ctx->out->shape_total;
            int rc = parse_shape_tag(payload, tlen, (uint8_t)code, ctx->out,
                                     reason, reason_cap);
            if (rc == -2) return MENU_SWF_ERR_NOTIMPL;
            if (rc == -3) return MENU_SWF_ERR_BUDGET;
            if (rc) {
                if (!reason || !reason[0])
                    set_reason(reason, reason_cap,
                               "swf shape: malformed shape tag %u", code);
                return MENU_SWF_ERR_FORMAT;
            }
        } else if (code == 39) {
            if (depth >= MENU_SWF_SHAPE_MAX_DEPTH) {
                set_reason(reason, reason_cap,
                           "swf shape: sprite nesting exceeds %u",
                           MENU_SWF_SHAPE_MAX_DEPTH);
                return MENU_SWF_ERR_TAG;
            }
            if (tlen < 4) {
                set_reason(reason, reason_cap,
                           "swf shape: DefineSprite is shorter than 4 bytes");
                return MENU_SWF_ERR_FORMAT;
            }
            MenuSwfStatus st = walk_tags(ctx, payload + 4, tlen - 4, depth + 1,
                                         reason, reason_cap);
            if (st != MENU_SWF_OK) return st;
        } else if (code == 48 || code == 75) {
            int rc = parse_font_tag(payload, tlen, (uint8_t)code, ctx->out,
                                    reason, reason_cap);
            if (rc == -3) return MENU_SWF_ERR_BUDGET;
            if (rc == -2) return MENU_SWF_ERR_NOTIMPL;
            if (rc) {
                if (!reason || !reason[0])
                    set_reason(reason, reason_cap,
                               "swf shape: malformed font tag %u", code);
                return MENU_SWF_ERR_FORMAT;
            }
        } else if (code == 91) {
            set_reason(reason, reason_cap,
                       "swf shape: DefineFont4 is E_NOTIMPL");
            return MENU_SWF_ERR_NOTIMPL;
        } else if (code == 82) {
            set_reason(reason, reason_cap,
                       "swf shape: DoABC/AVM2 is E_NOTIMPL");
            return MENU_SWF_ERR_NOTIMPL;
        }
        off += tlen;
    }
    set_reason(reason, reason_cap, "swf shape: tag stream has no End");
    return MENU_SWF_ERR_END;
}

const MenuSwfShapeSummary *menu_swf_shape_find(const MenuSwfShapeResult *result,
                                               uint16_t shape_id) {
    if (!result) return NULL;
    for (uint32_t i = 0; i < result->stored_shapes; ++i)
        if (result->shapes[i].shape_id == shape_id) return &result->shapes[i];
    return NULL;
}

MenuSwfStatus menu_swf_shape_summary(const uint8_t *data, size_t size,
                                     MenuSwfShapeResult *out, char *reason,
                                     size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);

    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    ShapeContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;
    st = walk_tags(&ctx, view.body + view.tag_offset,
                   view.body_len - view.tag_offset, 0, reason, reason_cap);
    menu_swf_view_close(&view);
    if (st != MENU_SWF_OK) {
        memset(out, 0, sizeof *out);
        return st;
    }
    if (out->shape_total > MENU_SWF_SHAPE_MAX_SHAPES ||
        out->glyph_total > MENU_SWF_SHAPE_MAX_GLYPHS) {
        set_reason(reason, reason_cap,
                   "swf shape: collection limit exceeded");
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_BUDGET;
    }
    return MENU_SWF_OK;
}
