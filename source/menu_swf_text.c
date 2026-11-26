#include "menu_swf_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>

static void text_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static void text_reset(MenuSwfTextMesh *m) {
    m->vertex_count = 0;
    m->index_count = 0;
    m->triangle_count = 0;
    m->glyph_count = 0;
    m->line_count = 0;
    m->run_count = 0;
    memset(&m->html, 0, sizeof m->html);
    m->missing_glyphs = 0;
    m->pen_x = 0;
    m->pen_y = 0;
    memset(&m->bounds, 0, sizeof m->bounds);
    m->has_bounds = 0;
}

static int buffers_bad(const MenuSwfTextMesh *m) {
    return (!m->vertices && m->vertex_capacity) ||
           (!m->indices && m->index_capacity) ||
           (!m->glyphs && m->glyph_capacity) ||
           (!m->lines && m->line_capacity) ||
           (!m->runs && m->run_capacity);
}

static void map_point(const MenuSwfTextStyle *s, int64_t x, int64_t y,
                      int32_t *out_x, int32_t *out_y) {
    int64_t tx = (s->m00 * x + s->m01 * y) / 65536 + s->tx;
    int64_t ty = (s->m10 * x + s->m11 * y) / 65536 + s->ty;
    *out_x = (int32_t)tx;
    *out_y = (int32_t)ty;
}

static int map_point_checked(const MenuSwfTextStyle *s, int64_t x, int64_t y,
                             int32_t *out_x, int32_t *out_y) {
    int64_t tx = (s->m00 * x + s->m01 * y) / 65536 + s->tx;
    int64_t ty = (s->m10 * x + s->m11 * y) / 65536 + s->ty;
    if (tx < INT32_MIN || tx > INT32_MAX || ty < INT32_MIN || ty > INT32_MAX)
        return -1;
    *out_x = (int32_t)tx;
    *out_y = (int32_t)ty;
    return 0;
}

static const MenuSwfGlyph *find_glyph(const MenuSwfFontSummary *font,
                                      uint16_t code) {
    uint32_t count = font->stored_glyphs;
    if (count > MENU_SWF_DISPLAY_MAX_GLYPHS)
        count = MENU_SWF_DISPLAY_MAX_GLYPHS;
    for (uint32_t i = 0; i < count; ++i)
        if (font->glyphs[i].code == code) return &font->glyphs[i];
    return NULL;
}

static int16_t find_kerning(const MenuSwfFontSummary *font, uint16_t first,
                            uint16_t second) {
    uint32_t count = font->stored_kerning;
    if (count > MENU_SWF_DISPLAY_MAX_KERNING)
        count = MENU_SWF_DISPLAY_MAX_KERNING;
    for (uint32_t i = 0; i < count; ++i)
        if (font->kerning[i].code1 == first &&
            font->kerning[i].code2 == second)
            return font->kerning[i].adjustment;
    return 0;
}

MenuSwfStatus menu_swf_text_layout(const uint8_t *data, size_t size,
                                   const MenuSwfFontSummary *font,
                                   const char *text, size_t text_len,
                                   const MenuSwfTextStyle *style,
                                   MenuSwfTextMesh *out, char *reason,
                                   size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    if (!data || !font || !style || (!text && text_len)) {
        text_reason(reason, reason_cap,
                    "swf text: needs SWF bytes, font, style and text");
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (font->font_type != 2 && font->font_type != 3) {
        text_reason(reason, reason_cap,
                    "swf text: font %u has no usable glyph table",
                    font->font_id);
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (text_len > MENU_SWF_TEXT_MAX_CHARS) {
        text_reason(reason, reason_cap,
                    "swf text: %u characters exceeds %u", (unsigned)text_len,
                    MENU_SWF_TEXT_MAX_CHARS);
        return MENU_SWF_ERR_BUDGET;
    }
    if (buffers_bad(out)) {
        text_reason(reason, reason_cap,
                    "swf text: NULL output buffer with a nonzero capacity");
        return MENU_SWF_ERR_ARGUMENT;
    }

    text_reset(out);
    out->style = *style;
    out->font_id = font->font_id;
    out->em_square = font->em_square;
    out->shape_scale = font->shape_scale;

    uint16_t previous = 0;
    int have_previous = 0;
    for (size_t i = 0; i < text_len; ++i) {
        uint16_t code = (uint16_t)(uint8_t)text[i];
        const MenuSwfGlyph *src = find_glyph(font, code);
        int16_t kerning = have_previous
                              ? find_kerning(font, previous, code)
                              : 0;
        out->pen_x += kerning;
        int32_t pen_x = out->pen_x;
        int32_t pen_y = out->pen_y;
        int32_t place_x, place_y;
        map_point(style, pen_x, pen_y, &place_x, &place_y);

        MenuSwfTextGlyph glyph;
        memset(&glyph, 0, sizeof glyph);
        glyph.x = place_x;
        glyph.y = place_y;
        glyph.pen_x = pen_x;
        glyph.pen_y = pen_y;
        glyph.code = code;
        glyph.kerning = kerning;
        glyph.glyph_index = MENU_SWF_TEXT_MISSING_GLYPH;
        glyph.vertex_first = out->vertex_count;
        glyph.triangle_first = out->triangle_count;

        if (src) {
            glyph.glyph_index = src->glyph_index;
            glyph.advance = src->has_advance ? src->advance : 0;
            MenuSwfStatus st = menu_swf_tess_glyph(
                data, size, font->font_id, code, MENU_SWF_TESS_NON_ZERO,
                &out->glyph_tess, reason, reason_cap);
            if (st != MENU_SWF_OK) {
                text_reset(out);
                return st;
            }
            uint32_t add_vertices = out->glyph_tess.vertex_count;
            uint32_t add_indices = out->glyph_tess.index_count;
            uint32_t add_triangles = out->glyph_tess.triangle_count;
            if (out->vertex_count + add_vertices > out->vertex_capacity ||
                out->vertex_count + add_vertices >
                    MENU_SWF_TEXT_MAX_VERTICES ||
                out->index_count + add_indices > out->index_capacity ||
                out->index_count + add_indices >
                    MENU_SWF_TEXT_MAX_INDICES ||
                out->triangle_count + add_triangles >
                    MENU_SWF_TEXT_MAX_TRIANGLES) {
                text_reason(reason, reason_cap,
                            "swf text: vertex/index capacity exceeded");
                text_reset(out);
                return MENU_SWF_ERR_BUDGET;
            }
            uint32_t base = out->vertex_count;
            int32_t bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
            int have_bounds = 0;
            for (uint32_t v = 0; v < add_vertices; ++v) {
                int32_t rx = out->glyph_tess.vertices[v].x + pen_x;
                int32_t ry = out->glyph_tess.vertices[v].y + pen_y;
                int32_t tx, ty;
                map_point(style, rx, ry, &tx, &ty);
                out->vertices[out->vertex_count].x = tx;
                out->vertices[out->vertex_count].y = ty;
                out->vertices[out->vertex_count].color = style->color;
                ++out->vertex_count;
                if (!have_bounds) {
                    bxmin = bxmax = tx;
                    bymin = bymax = ty;
                    have_bounds = 1;
                } else {
                    if (tx < bxmin) bxmin = tx;
                    if (tx > bxmax) bxmax = tx;
                    if (ty < bymin) bymin = ty;
                    if (ty > bymax) bymax = ty;
                }
            }
            for (uint32_t k = 0; k < add_indices; ++k)
                out->indices[out->index_count++] =
                    base + out->glyph_tess.indices[k];
            glyph.vertex_count = add_vertices;
            glyph.triangle_count = add_triangles;
            out->triangle_count += add_triangles;
            if (add_vertices) {
                glyph.has_geometry = 1;
                glyph.bounds.xmin = bxmin;
                glyph.bounds.xmax = bxmax;
                glyph.bounds.ymin = bymin;
                glyph.bounds.ymax = bymax;
                if (!out->has_bounds) {
                    out->bounds = glyph.bounds;
                    out->has_bounds = 1;
                } else {
                    if (bxmin < out->bounds.xmin) out->bounds.xmin = bxmin;
                    if (bxmax > out->bounds.xmax) out->bounds.xmax = bxmax;
                    if (bymin < out->bounds.ymin) out->bounds.ymin = bymin;
                    if (bymax > out->bounds.ymax) out->bounds.ymax = bymax;
                }
            }
        } else {
            glyph.missing = 1;
            ++out->missing_glyphs;
        }

        if (out->glyph_count >= out->glyph_capacity ||
            out->glyph_count >= MENU_SWF_TEXT_MAX_GLYPHS) {
            text_reason(reason, reason_cap,
                        "swf text: glyph capacity %u exceeded",
                        out->glyph_capacity);
            text_reset(out);
            return MENU_SWF_ERR_BUDGET;
        }
        out->glyphs[out->glyph_count++] = glyph;
        out->pen_x += glyph.advance;
        previous = code;
        have_previous = 1;
    }
    return MENU_SWF_OK;
}

/* ------------------------------------------------------------------ */
/* DefineEditText layout                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t start, len;
    uint8_t align;
} TextSpan;

static int html_entity(const char *src, size_t n, char *out) {
    if (n == 2 && !memcmp(src, "lt", 2)) { *out = '<'; return 0; }
    if (n == 2 && !memcmp(src, "gt", 2)) { *out = '>'; return 0; }
    if (n == 3 && !memcmp(src, "amp", 3)) { *out = '&'; return 0; }
    if (n == 4 && !memcmp(src, "quot", 4)) { *out = '"'; return 0; }
    if (n == 4 && !memcmp(src, "apos", 4)) { *out = '\''; return 0; }
    if (n == 4 && !memcmp(src, "nbsp", 4)) { *out = ' '; return 0; }
    return -1;
}

typedef struct {
    uint32_t color;
    uint16_t size;
    char face[MENU_SWF_TEXT_FACE_MAX];
    uint8_t has_color, has_size, has_face;
    uint8_t bold, italic, underline;
    uint8_t align;
} HtmlStyle;

typedef struct {
    HtmlStyle style;
    uint8_t tag;
} HtmlFrame;

typedef struct {
    char *dst;
    size_t cap;
    size_t used;
    MenuSwfTextRun *runs;
    uint32_t run_cap;
    uint32_t run_count;
    uint32_t run_start;
    HtmlStyle style;
    MenuSwfTextHtmlSummary *summary;
    char *reason;
    size_t reason_cap;
} HtmlCtx;

static int ascii_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ci_equal(const char *a, size_t n, const char *b) {
    size_t m = strlen(b);
    if (n != m) return 0;
    for (size_t i = 0; i < n; ++i)
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_html_color(const char *v, size_t n, uint32_t *out) {
    if (n != 7 && n != 4) return -1;
    if (n == 7) {
        if (v[0] != '#') return -1;
        uint32_t rgb = 0;
        for (size_t i = 1; i < 7; ++i) {
            int d = hex_digit((unsigned char)v[i]);
            if (d < 0) return -1;
            rgb = (rgb << 4) | (uint32_t)d;
        }
        *out = 0xff000000u | rgb;
        return 0;
    }
    if (v[0] != '#') return -1;
    uint32_t rgb = 0;
    for (size_t i = 1; i < 4; ++i) {
        int d = hex_digit((unsigned char)v[i]);
        if (d < 0) return -1;
        rgb = (rgb << 8) | (uint32_t)(d | (d << 4));
    }
    *out = 0xff000000u | rgb;
    return 0;
}

static int parse_html_uint(const char *v, size_t n, uint16_t *out) {
    if (!n) return -1;
    uint32_t value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (v[i] < '0' || v[i] > '9') return -1;
        value = value * 10u + (uint32_t)(v[i] - '0');
        if (value > 4096u) return -1;
    }
    if (!value) return -1;
    *out = (uint16_t)value;
    return 0;
}

static int html_flush(HtmlCtx *c) {
    if (c->used <= c->run_start) {
        c->run_start = (uint32_t)c->used;
        return 0;
    }
    if (c->runs) {
        if (c->run_count >= c->run_cap) {
            text_reason(c->reason, c->reason_cap,
                        "swf edit text: %u HTML runs exceed %u",
                        c->run_count + 1, c->run_cap);
            return -1;
        }
        MenuSwfTextRun *run = &c->runs[c->run_count++];
        memset(run, 0, sizeof *run);
        run->text_first = c->run_start;
        run->text_length = (uint32_t)(c->used - c->run_start);
        run->color = c->style.color;
        run->has_color = c->style.has_color;
        run->size = c->style.size;
        run->has_size = c->style.has_size;
        run->has_face = c->style.has_face;
        run->bold = c->style.bold;
        run->italic = c->style.italic;
        run->underline = c->style.underline;
        run->align = c->style.align;
        memcpy(run->face, c->style.face, MENU_SWF_TEXT_FACE_MAX);
    }
    c->run_start = (uint32_t)c->used;
    if (c->summary) ++c->summary->runs;
    return 0;
}

static int html_push_char(HtmlCtx *c, char value) {
    if (c->used + 1 >= c->cap) {
        text_reason(c->reason, c->reason_cap,
                    "swf edit text: plain text overflow");
        return -1;
    }
    c->dst[c->used++] = value;
    c->dst[c->used] = 0;
    return 0;
}

static int html_newline(HtmlCtx *c) {
    if (c->used && c->dst[c->used - 1] != '\n' && html_push_char(c, '\n'))
        return -1;
    return html_flush(c);
}

static int html_push_style(HtmlCtx *c, HtmlFrame *stack, unsigned *depth,
                           uint8_t tag) {
    if (*depth >= MENU_SWF_TEXT_HTML_STACK) {
        text_reason(c->reason, c->reason_cap,
                    "swf edit text: HTML nesting exceeds %u",
                    MENU_SWF_TEXT_HTML_STACK);
        return -1;
    }
    stack[*depth].style = c->style;
    stack[*depth].tag = tag;
    ++*depth;
    return 0;
}

static void html_pop_style(HtmlCtx *c, const HtmlFrame *stack,
                           unsigned *depth, uint8_t tag) {
    for (unsigned i = *depth; i > 0; --i) {
        if (stack[i - 1].tag == tag) {
            c->style = stack[i - 1].style;
            *depth = i - 1;
            return;
        }
    }
}

static int html_set_attr(HtmlCtx *c, const char *name, size_t name_len,
                         const char *value, size_t value_len) {
    uint32_t color;
    uint16_t number;
    if (ci_equal(name, name_len, "color")) {
        if (!value || parse_html_color(value, value_len, &color)) {
            text_reason(c->reason, c->reason_cap,
                        "swf edit text: bad HTML colour attribute");
            return -1;
        }
        c->style.color = color;
        c->style.has_color = 1;
        if (c->summary) ++c->summary->color_attrs;
        return 0;
    }
    if (ci_equal(name, name_len, "size")) {
        if (!value || parse_html_uint(value, value_len, &number)) {
            text_reason(c->reason, c->reason_cap,
                        "swf edit text: bad HTML size attribute");
            return -1;
        }
        c->style.size = number;
        c->style.has_size = 1;
        if (c->summary) ++c->summary->size_attrs;
        return 0;
    }
    if (ci_equal(name, name_len, "face")) {
        if (!value || value_len >= MENU_SWF_TEXT_FACE_MAX) {
            text_reason(c->reason, c->reason_cap,
                        "swf edit text: HTML face attribute exceeds %u bytes",
                        MENU_SWF_TEXT_FACE_MAX - 1);
            return -1;
        }
        memcpy(c->style.face, value, value_len);
        c->style.face[value_len] = 0;
        c->style.has_face = value_len != 0;
        if (c->summary) ++c->summary->face_attrs;
        return 0;
    }
    if (ci_equal(name, name_len, "align")) {
        if (!value) return 0;
        if (ci_equal(value, value_len, "left"))
            c->style.align = MENU_SWF_EDIT_TEXT_ALIGN_LEFT;
        else if (ci_equal(value, value_len, "right"))
            c->style.align = MENU_SWF_EDIT_TEXT_ALIGN_RIGHT;
        else if (ci_equal(value, value_len, "center"))
            c->style.align = MENU_SWF_EDIT_TEXT_ALIGN_CENTER;
        else if (ci_equal(value, value_len, "justify"))
            c->style.align = MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY;
        else {
            text_reason(c->reason, c->reason_cap,
                        "swf edit text: unknown HTML alignment");
            return -1;
        }
        if (c->summary) ++c->summary->align_attrs;
        return 0;
    }
    if (c->summary) ++c->summary->unknown_attrs;
    return 0;
}

static int html_parse_attrs(HtmlCtx *c, const char *src, size_t start,
                            size_t end) {
    size_t i = start;
    while (i < end) {
        while (i < end && (src[i] == ' ' || src[i] == '\t' || src[i] == '/' ||
                           src[i] == '\r' || src[i] == '\n'))
            ++i;
        if (i >= end) break;
        size_t name = i;
        while (i < end && src[i] != '=' && src[i] != ' ' && src[i] != '\t' &&
               src[i] != '/' && src[i] != '\r' && src[i] != '\n')
            ++i;
        size_t name_len = i - name;
        if (!name_len) { ++i; continue; }
        while (i < end && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' ||
                           src[i] == '\n'))
            ++i;
        const char *value = NULL;
        size_t value_len = 0;
        if (i < end && src[i] == '=') {
            ++i;
            while (i < end && (src[i] == ' ' || src[i] == '\t' ||
                               src[i] == '\r' || src[i] == '\n'))
                ++i;
            if (i < end && (src[i] == '"' || src[i] == '\'')) {
                char quote = src[i++];
                size_t vstart = i;
                while (i < end && src[i] != quote) ++i;
                if (i >= end) {
                    text_reason(c->reason, c->reason_cap,
                                "swf edit text: unterminated HTML attribute");
                    return -1;
                }
                value = src + vstart;
                value_len = i - vstart;
                ++i;
            } else {
                size_t vstart = i;
                while (i < end && src[i] != ' ' && src[i] != '\t' &&
                       src[i] != '/' && src[i] != '\r' && src[i] != '\n')
                    ++i;
                value = src + vstart;
                value_len = i - vstart;
            }
        }
        if (html_set_attr(c, src + name, name_len, value, value_len))
            return -1;
    }
    return 0;
}

static MenuSwfStatus edit_html_runs(const MenuSwfEditText *edit, char *dst,
                                    size_t cap, size_t *out_len,
                                    MenuSwfTextRun *runs, uint32_t run_cap,
                                    uint32_t *run_count,
                                    MenuSwfTextHtmlSummary *summary,
                                    char *reason, size_t reason_cap) {
    const char *src = edit->initial_text;
    size_t len = edit->initial_text_length;
    if (!len && edit->html) {
        src = edit->variable_name;
        len = strlen(src);
    }
    if (summary) memset(summary, 0, sizeof *summary);
    if (run_count) *run_count = 0;
    if (len >= cap) {
        text_reason(reason, reason_cap,
                    "swf edit text: source length %u exceeds %u",
                    (unsigned)len, (unsigned)cap);
        return MENU_SWF_ERR_BUDGET;
    }
    if (!edit->html) {
        memcpy(dst, src, len);
        dst[len] = 0;
        *out_len = len;
        if (runs && run_cap && len) {
            MenuSwfTextRun *run = &runs[0];
            memset(run, 0, sizeof *run);
            run->text_first = 0;
            run->text_length = (uint32_t)len;
            run->align = edit->alignment;
            if (edit->has_text_color) {
                run->color = ((uint32_t)edit->color[3] << 24) |
                             ((uint32_t)edit->color[0] << 16) |
                             ((uint32_t)edit->color[1] << 8) | edit->color[2];
                run->has_color = 1;
            }
            if (run_count) *run_count = 1;
            if (summary) summary->runs = 1;
        }
        return MENU_SWF_OK;
    }

    char normalized[MENU_SWF_TEXT_MAX_CHARS + 2];
    if (len && src[0] != '<') {
        normalized[0] = '<';
        memcpy(normalized + 1, src, len);
        src = normalized;
        ++len;
    }

    HtmlCtx c;
    memset(&c, 0, sizeof c);
    c.dst = dst;
    c.cap = cap;
    c.runs = runs;
    c.run_cap = run_cap;
    c.run_start = 0;
    c.style.align = edit->alignment;
    if (edit->has_text_color) {
        c.style.color = ((uint32_t)edit->color[3] << 24) |
                        ((uint32_t)edit->color[0] << 16) |
                        ((uint32_t)edit->color[1] << 8) | edit->color[2];
        c.style.has_color = 1;
    }
    c.summary = summary;
    c.reason = reason;
    c.reason_cap = reason_cap;
    dst[0] = 0;

    HtmlFrame stack[MENU_SWF_TEXT_HTML_STACK];
    unsigned depth = 0;
    for (size_t i = 0; i < len;) {
        if (src[i] == '<') {
            size_t close = i + 1;
            while (close < len && src[close] != '>') ++close;
            if (close == len) {
                text_reason(reason, reason_cap,
                            "swf edit text: unterminated HTML tag");
                return MENU_SWF_ERR_FORMAT;
            }
            int closing = i + 1 < close && src[i + 1] == '/';
            int self_closing = close > i + 1 && src[close - 1] == '/';
            size_t name = i + 1 + (closing ? 1 : 0);
            size_t name_end = name;
            while (name_end < close && src[name_end] != ' ' &&
                   src[name_end] != '\t' && src[name_end] != '\r' &&
                   src[name_end] != '\n' && src[name_end] != '/')
                ++name_end;
            size_t name_len = name_end - name;
            if (summary) {
                ++summary->tags;
                if (closing) ++summary->end_tags;
                else ++summary->start_tags;
            }
            if (name_len && ci_equal(src + name, name_len, "br")) {
                if (summary) ++summary->br_tags;
                if (!closing && html_newline(&c)) return MENU_SWF_ERR_BUDGET;
            } else if (name_len && ci_equal(src + name, name_len, "p")) {
                if (summary) ++summary->p_tags;
                if (!closing) {
                    if (html_newline(&c)) return MENU_SWF_ERR_BUDGET;
                    if (!self_closing) {
                        if (html_push_style(&c, stack, &depth, 5))
                            return MENU_SWF_ERR_BUDGET;
                        if (html_parse_attrs(&c, src, name_end, close))
                            return MENU_SWF_ERR_FORMAT;
                    }
                } else {
                    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;
                    html_pop_style(&c, stack, &depth, 5);
                    if (html_newline(&c)) return MENU_SWF_ERR_BUDGET;
                }
            } else if (name_len && ci_equal(src + name, name_len, "li")) {
                if (summary) ++summary->li_tags;
                if (!closing && html_newline(&c)) return MENU_SWF_ERR_BUDGET;
            } else if (name_len && ci_equal(src + name, name_len, "font")) {
                if (summary) ++summary->font_tags;
                if (!closing) {
                    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;
                    if (!self_closing) {
                        if (html_push_style(&c, stack, &depth, 1))
                            return MENU_SWF_ERR_BUDGET;
                        if (html_parse_attrs(&c, src, name_end, close))
                            return MENU_SWF_ERR_FORMAT;
                    }
                } else {
                    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;
                    html_pop_style(&c, stack, &depth, 1);
                }
            } else if (name_len && (ci_equal(src + name, name_len, "b") ||
                                    ci_equal(src + name, name_len, "i") ||
                                    ci_equal(src + name, name_len, "u"))) {
                uint8_t tag = ci_equal(src + name, name_len, "b")
                                  ? 2
                                  : ci_equal(src + name, name_len, "i") ? 3 : 4;
                if (summary) {
                    if (tag == 2) ++summary->b_tags;
                    else if (tag == 3) ++summary->i_tags;
                    else ++summary->u_tags;
                }
                if (!closing) {
                    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;
                    if (!self_closing) {
                        if (html_push_style(&c, stack, &depth, tag))
                            return MENU_SWF_ERR_BUDGET;
                        if (tag == 2) c.style.bold = 1;
                        else if (tag == 3) c.style.italic = 1;
                        else c.style.underline = 1;
                    }
                } else {
                    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;
                    html_pop_style(&c, stack, &depth, tag);
                }
            } else {
                if (summary) ++summary->unknown_tags;
            }
            i = close + 1;
        } else if (src[i] == '&') {
            size_t close = i + 1;
            while (close < len && src[close] != ';') ++close;
            if (close == len) {
                text_reason(reason, reason_cap,
                            "swf edit text: unterminated HTML entity");
                return MENU_SWF_ERR_FORMAT;
            }
            char decoded;
            if (html_entity(src + i + 1, close - i - 1, &decoded)) {
                text_reason(reason, reason_cap,
                            "swf edit text: unsupported HTML entity");
                return MENU_SWF_ERR_NOTIMPL;
            }
            if (summary) ++summary->entities;
            if (html_push_char(&c, decoded)) return MENU_SWF_ERR_BUDGET;
            i = close + 1;
        } else {
            if (html_push_char(&c, src[i])) return MENU_SWF_ERR_BUDGET;
            ++i;
        }
    }
    if (html_flush(&c)) return MENU_SWF_ERR_BUDGET;

    while (c.used && dst[c.used - 1] == '\n') dst[--c.used] = 0;
    if (run_count) *run_count = c.run_count;
    if (c.runs) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < c.run_count; ++i) {
            MenuSwfTextRun *run = &c.runs[i];
            if (run->text_first >= c.used) continue;
            if (run->text_first + run->text_length > c.used)
                run->text_length = (uint32_t)(c.used - run->text_first);
            if (!run->text_length) continue;
            if (kept != i) c.runs[kept] = *run;
            ++kept;
        }
        c.run_count = kept;
        if (run_count) *run_count = kept;
        if (summary) summary->runs = kept;
    }
    *out_len = c.used;
    return MENU_SWF_OK;
}

static MenuSwfStatus edit_plain_text(const MenuSwfEditText *edit, char *dst,
                                     size_t cap, size_t *out_len,
                                     char *reason, size_t reason_cap) {
    return edit_html_runs(edit, dst, cap, out_len, NULL, 0, NULL, NULL,
                          reason, reason_cap);
}
static double html_run_scale(const MenuSwfTextRun *run, double default_px) {
    if (!run->has_size || !run->size || !(default_px > 0.0)) return 1.0;
    return (double)run->size / default_px;
}

static double measure_text_run(const MenuSwfFontSummary *font,
                               const MenuSwfFontSummary *const *run_fonts,
                               const char *text, size_t start, size_t len,
                               const uint16_t *run_of_char,
                               const double *run_scales) {
    double width = 0.0;
    uint16_t previous = 0;
    int have_previous = 0;
    for (size_t i = 0; i < len; ++i) {
        uint16_t code = (uint8_t)text[start + i];
        const MenuSwfFontSummary *rf =
            (run_fonts && run_of_char) ? run_fonts[run_of_char[start + i]]
                                       : font;
        if (!rf) rf = font;
        uint32_t run = run_of_char ? run_of_char[start + i] : 0;
        double scale = run_scales ? run_scales[run] : 1.0;
        if (have_previous)
            width += (double)find_kerning(rf, previous, code) * scale;
        const MenuSwfGlyph *glyph = find_glyph(rf, code);
        if (glyph && glyph->has_advance)
            width += (double)glyph->advance * scale;
        previous = code;
        have_previous = 1;
    }
    return width;
}

static uint8_t line_align_at(const MenuSwfEditText *edit,
                             const MenuSwfTextRun *runs, uint32_t run_count,
                             const uint16_t *run_of_char, size_t pos,
                             size_t len) {
    if (pos < len && run_of_char) return runs[run_of_char[pos]].align;
    if (run_count) return runs[run_count - 1].align;
    return edit->alignment;
}

static MenuSwfStatus split_text_lines(
    const MenuSwfEditText *edit, const MenuSwfFontSummary *font,
    const MenuSwfFontSummary *const *run_fonts, const char *text, size_t len,
    int64_t available_raw, const uint16_t *run_of_char,
    const double *run_scales, const MenuSwfTextRun *runs, uint32_t run_count,
    TextSpan *spans, uint32_t span_capacity, uint32_t *span_count,
    char *reason, size_t reason_cap) {
    uint32_t count = 0;
    if (!len) {
        spans[count++] = (TextSpan){0, 0, edit->alignment};
        *span_count = count;
        return MENU_SWF_OK;
    }
    size_t pos = 0;
    for (;;) {
        size_t end = pos;
        while (end < len && text[end] != '\n') ++end;
        if (end < len && !edit->multiline) {
            text_reason(reason, reason_cap,
                        "swf edit text: newline in a single-line field");
            return MENU_SWF_ERR_NOTIMPL;
        }
        uint8_t align = line_align_at(edit, runs, run_count, run_of_char, pos,
                                      len);
        if (!edit->word_wrap || !edit->multiline || pos == end) {
            if (count >= span_capacity) goto too_many;
            spans[count++] = (TextSpan){pos, end - pos, align};
        } else {
            if (available_raw <= 0) {
                text_reason(reason, reason_cap,
                            "swf edit text: bounds/margins leave no text width");
                return MENU_SWF_ERR_BUDGET;
            }
            size_t cur = pos;
            while (cur < end) {
                size_t take = 0, last_space = SIZE_MAX;
                double width = 0.0;
                uint16_t previous = 0;
                int have_previous = 0;
                for (size_t i = cur; i < end; ++i) {
                    uint16_t code = (uint8_t)text[i];
                    uint32_t run = run_of_char[i];
                    const MenuSwfFontSummary *rf =
                        (run_fonts && run < run_count) ? run_fonts[run] : font;
                    if (!rf) rf = font;
                    double scale = run_scales[run];
                    double kerning = have_previous
                                         ? (double)find_kerning(rf, previous, code) * scale
                                         : 0.0;
                    const MenuSwfGlyph *glyph = find_glyph(rf, code);
                    double advance = glyph && glyph->has_advance
                                         ? (double)glyph->advance * scale
                                         : 0.0;
                    double next = width + kerning + advance;
                    if (next > (double)available_raw && i > cur) {
                        take = last_space != SIZE_MAX
                                   ? last_space - cur
                                   : i - cur;
                        break;
                    }
                    width = next;
                    if (code == ' ') last_space = i;
                    previous = code;
                    have_previous = 1;
                }
                if (!take) take = end - cur;
                if (!take) {
                    text_reason(reason, reason_cap,
                                "swf edit text: one glyph exceeds the wrap width");
                    return MENU_SWF_ERR_BUDGET;
                }
                if (count >= span_capacity) goto too_many;
                uint8_t line_align = line_align_at(edit, runs, run_count,
                                                   run_of_char, cur, len);
                spans[count++] = (TextSpan){cur, take, line_align};
                cur += take;
                while (cur < end && text[cur] == ' ') ++cur;
            }
        }
        if (end == len) break;
        pos = end + 1;
        if (pos == len) {
            if (count >= span_capacity) goto too_many;
            spans[count++] = (TextSpan){len, 0,
                                        line_align_at(edit, runs, run_count,
                                                      run_of_char, len, len)};
            break;
        }
    }
    *span_count = count;
    return MENU_SWF_OK;

too_many:
    text_reason(reason, reason_cap,
                "swf edit text: %u lines exceed %u", count + 1,
                span_capacity);
    return MENU_SWF_ERR_BUDGET;
}

static MenuSwfStatus append_edit_glyph(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    uint16_t code, int32_t pen_x, int32_t pen_y,
    const MenuSwfTextStyle *style, uint16_t line_index, uint16_t run_index,
    MenuSwfTextMesh *out, MenuSwfTextGlyph *glyph,
    char *reason, size_t reason_cap) {
    memset(glyph, 0, sizeof *glyph);
    glyph->code = code;
    glyph->line_index = line_index;
    glyph->run_index = run_index;
    glyph->has_color = 1;
    glyph->color = style->color;
    glyph->pen_x = pen_x;
    glyph->pen_y = pen_y;
    glyph->vertex_first = out->vertex_count;
    glyph->triangle_first = out->triangle_count;
    glyph->glyph_index = MENU_SWF_TEXT_MISSING_GLYPH;
    if (map_point_checked(style, pen_x, pen_y, &glyph->x, &glyph->y)) {
        text_reason(reason, reason_cap, "swf edit text: placement overflow");
        return MENU_SWF_ERR_BUDGET;
    }

    const MenuSwfGlyph *src = find_glyph(font, code);
    if (src) {
        glyph->glyph_index = src->glyph_index;
        glyph->advance = src->has_advance ? src->advance : 0;
        MenuSwfStatus st = menu_swf_tess_glyph(
            data, size, font->font_id, code, MENU_SWF_TESS_NON_ZERO,
            &out->glyph_tess, reason, reason_cap);
        if (st != MENU_SWF_OK) return st;
        uint32_t add_vertices = out->glyph_tess.vertex_count;
        uint32_t add_indices = out->glyph_tess.index_count;
        uint32_t add_triangles = out->glyph_tess.triangle_count;
        if (out->vertex_count + add_vertices > out->vertex_capacity ||
            out->vertex_count + add_vertices > MENU_SWF_TEXT_MAX_VERTICES ||
            out->index_count + add_indices > out->index_capacity ||
            out->index_count + add_indices > MENU_SWF_TEXT_MAX_INDICES ||
            out->triangle_count + add_triangles > MENU_SWF_TEXT_MAX_TRIANGLES) {
            text_reason(reason, reason_cap,
                        "swf edit text: vertex/index capacity exceeded");
            return MENU_SWF_ERR_BUDGET;
        }
        uint32_t base = out->vertex_count;
        int32_t bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
        int have_bounds = 0;
        for (uint32_t v = 0; v < add_vertices; ++v) {
            int32_t rx = out->glyph_tess.vertices[v].x + pen_x;
            int32_t ry = out->glyph_tess.vertices[v].y + pen_y;
            int32_t tx, ty;
            if (map_point_checked(style, rx, ry, &tx, &ty)) {
                text_reason(reason, reason_cap,
                            "swf edit text: glyph point overflow");
                return MENU_SWF_ERR_BUDGET;
            }
            out->vertices[out->vertex_count].x = tx;
            out->vertices[out->vertex_count].y = ty;
            out->vertices[out->vertex_count].color = style->color;
            ++out->vertex_count;
            if (!have_bounds) {
                bxmin = bxmax = tx;
                bymin = bymax = ty;
                have_bounds = 1;
            } else {
                if (tx < bxmin) bxmin = tx;
                if (tx > bxmax) bxmax = tx;
                if (ty < bymin) bymin = ty;
                if (ty > bymax) bymax = ty;
            }
        }
        for (uint32_t k = 0; k < add_indices; ++k)
            out->indices[out->index_count++] = base + out->glyph_tess.indices[k];
        glyph->vertex_count = add_vertices;
        glyph->triangle_count = add_triangles;
        out->triangle_count += add_triangles;
        if (add_vertices) {
            glyph->has_geometry = 1;
            glyph->bounds = (MenuSwfShapeRect){bxmin, bxmax, bymin, bymax};
            if (!out->has_bounds) {
                out->bounds = glyph->bounds;
                out->has_bounds = 1;
            } else {
                if (bxmin < out->bounds.xmin) out->bounds.xmin = bxmin;
                if (bxmax > out->bounds.xmax) out->bounds.xmax = bxmax;
                if (bymin < out->bounds.ymin) out->bounds.ymin = bymin;
                if (bymax > out->bounds.ymax) out->bounds.ymax = bymax;
            }
        }
    } else {
        glyph->missing = 1;
        ++out->missing_glyphs;
    }
    if (out->glyph_count >= out->glyph_capacity ||
        out->glyph_count >= MENU_SWF_TEXT_MAX_GLYPHS) {
        text_reason(reason, reason_cap,
                    "swf edit text: glyph capacity %u exceeded",
                    out->glyph_capacity);
        return MENU_SWF_ERR_BUDGET;
    }
    out->glyphs[out->glyph_count++] = *glyph;
    out->pen_x = pen_x + glyph->advance;
    out->pen_y = pen_y;
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_text_edit_plain(
    const MenuSwfEditText *edit, char *dst, size_t capacity, size_t *out_len,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!edit || !dst || !capacity || !out_len)
        return MENU_SWF_ERR_ARGUMENT;
    return edit_plain_text(edit, dst, capacity, out_len, reason, reason_cap);
}

MenuSwfStatus menu_swf_text_edit_runs(
    const MenuSwfEditText *edit, char *text, size_t text_capacity,
    size_t *text_len, MenuSwfTextRun *runs, uint32_t run_capacity,
    uint32_t *run_count, MenuSwfTextHtmlSummary *summary, char *reason,
    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!edit || !text || !text_capacity || !text_len)
        return MENU_SWF_ERR_ARGUMENT;
    if (!runs && run_capacity) {
        text_reason(reason, reason_cap,
                    "swf edit text: NULL run buffer with nonzero capacity");
        return MENU_SWF_ERR_ARGUMENT;
    }
    return edit_html_runs(edit, text, text_capacity, text_len, runs,
                          run_capacity, run_count, summary, reason,
                          reason_cap);
}

static MenuSwfStatus layout_edit_impl(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfFontSummary *const *run_fonts,
    const MenuSwfEditText *edit, const MenuSwfTextStyle *style,
    MenuSwfTextMesh *out, char *reason, size_t reason_cap,
    int allow_resolved_id) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    if (!data || !font || !edit || !style) {
        text_reason(reason, reason_cap,
                    "swf edit text: needs SWF bytes, font, edit and style");
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (font->font_type != 2 && font->font_type != 3) {
        text_reason(reason, reason_cap,
                    "swf edit text: font %u has no usable glyph table",
                    font->font_id);
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (!allow_resolved_id && edit->has_font &&
        edit->font_id != font->font_id) {
        text_reason(reason, reason_cap,
                    "swf edit text: unknown/mismatched font id %u (have %u)",
                    edit->font_id, font->font_id);
        return MENU_SWF_ERR_ARGUMENT;
    }
    if (style->m00 <= 0 || style->m11 <= 0 || style->m01 || style->m10) {
        text_reason(reason, reason_cap,
                    "swf edit text: rotated/skewed/negative layout is not implemented");
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (edit->alignment > MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY) {
        text_reason(reason, reason_cap,
                    "swf edit text: alignment %u is not implemented",
                    edit->alignment);
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (buffers_bad(out)) {
        text_reason(reason, reason_cap,
                    "swf edit text: NULL output buffer with nonzero capacity");
        return MENU_SWF_ERR_ARGUMENT;
    }

    char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
    size_t plain_len = 0;
    MenuSwfTextRun runs[MENU_SWF_TEXT_MAX_RUNS];
    MenuSwfTextHtmlSummary html;
    uint32_t run_count = 0;
    MenuSwfStatus st = edit_html_runs(
        edit, plain, sizeof plain, &plain_len, runs, MENU_SWF_TEXT_MAX_RUNS,
        &run_count, &html, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    /* MaxLength governs user input, not authored initial text.  Retail
     * StartMenu fields carry HasMaxLength with small sentinel values while
     * their HTML initial text is longer, so refusing the authored text here
     * would suppress every real label. */
    if (edit->password && plain_len) {
        if (!find_glyph(font, '*')) {
            text_reason(reason, reason_cap,
                        "swf edit text: password mask '*' is absent from font %u",
                        font->font_id);
            return MENU_SWF_ERR_NOTIMPL;
        }
        for (size_t i = 0; i < plain_len; ++i)
            if (plain[i] != '\n') plain[i] = '*';
    }

    text_reset(out);
    out->style = *style;
    out->font_id = font->font_id;
    out->em_square = font->em_square;
    out->shape_scale = font->shape_scale;
    out->html = html;
    if (out->runs) {
        if (run_count > out->run_capacity) {
            text_reason(reason, reason_cap,
                        "swf edit text: %u runs exceed caller capacity %u",
                        run_count, out->run_capacity);
            return MENU_SWF_ERR_BUDGET;
        }
        memcpy(out->runs, runs, run_count * sizeof *runs);
        out->run_count = run_count;
    }

    double scale_x = (double)style->m00 / 65536.0;
    double scale_y = (double)style->m11 / 65536.0;
    double origin_x = (double)edit->bounds[0] / 20.0;
    double origin_y = (double)edit->bounds[2] / 20.0;
    double width_px = (double)(edit->bounds[1] - edit->bounds[0]) / 20.0;
    double height_px = (double)(edit->bounds[3] - edit->bounds[2]) / 20.0;
    double content_width = width_px -
                           ((double)edit->left_margin +
                            edit->right_margin) / 20.0;
    if (width_px <= 0.0 || height_px <= 0.0 || content_width <= 0.0) {
        text_reason(reason, reason_cap,
                    "swf edit text: bounds/margins produce an empty field");
        return MENU_SWF_ERR_BUDGET;
    }
    int64_t available_raw = (int64_t)(content_width / scale_x);

    double font_height = edit->has_font && edit->font_height
                             ? (double)edit->font_height / 20.0
                             : ((double)font->ascent + font->descent) * scale_y;
    double ascent = font->has_layout ? (double)font->ascent * scale_y
                                     : font_height * 0.8;
    double leading_px = (double)edit->leading / 20.0;
    if (!(font_height > 0.0) || !(ascent >= 0.0) ||
        !(font_height + leading_px > 0.0)) {
        text_reason(reason, reason_cap,
                    "swf edit text: font height/leading is not positive");
        return MENU_SWF_ERR_BUDGET;
    }

    uint16_t run_of_char[MENU_SWF_TEXT_MAX_CHARS + 1];
    memset(run_of_char, 0, sizeof run_of_char);
    for (uint32_t r = 0; r < run_count && r < MENU_SWF_TEXT_MAX_RUNS; ++r) {
        size_t first = runs[r].text_first;
        size_t last = first + runs[r].text_length;
        if (last > plain_len) last = plain_len;
        for (size_t p = first; p < last && p < MENU_SWF_TEXT_MAX_CHARS; ++p)
            run_of_char[p] = (uint16_t)r;
    }
    const MenuSwfFontSummary *run_fonts_used[MENU_SWF_TEXT_MAX_RUNS];
    for (uint32_t r = 0; r < run_count; ++r) {
        const MenuSwfFontSummary *rf = run_fonts ? run_fonts[r] : NULL;
        if (!rf) rf = font;
        if (!rf || (rf->font_type != 2 && rf->font_type != 3)) {
            text_reason(reason, reason_cap,
                        "swf edit text: run %u has no usable resolved font", r);
            return MENU_SWF_ERR_ARGUMENT;
        }
        run_fonts_used[r] = rf;
    }
    double run_scales[MENU_SWF_TEXT_MAX_RUNS];
    for (uint32_t r = 0; r < run_count; ++r) {
        double scale = html_run_scale(&runs[r], font_height);
        if (scale < 0.05 || scale > 64.0) {
            text_reason(reason, reason_cap,
                        "swf edit text: HTML run scale %.3f is out of range",
                        scale);
            return MENU_SWF_ERR_NOTIMPL;
        }
        run_scales[r] = scale;
    }

    TextSpan spans[MENU_SWF_TEXT_MAX_LINES];
    uint32_t span_count = 0;
    st = split_text_lines(edit, font, run_fonts_used, plain, plain_len,
                          available_raw, run_of_char, run_scales, runs,
                          run_count, spans, MENU_SWF_TEXT_MAX_LINES,
                          &span_count, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    if (out->lines && span_count > out->line_capacity) {
        text_reason(reason, reason_cap,
                    "swf edit text: %u lines exceed caller capacity %u",
                    span_count, out->line_capacity);
        return MENU_SWF_ERR_BUDGET;
    }

    if (!plain_len) {
        double x0 = style->tx + origin_x;
        double y0 = style->ty + origin_y;
        double x1 = x0 + width_px;
        double y1 = y0 + height_px;
        if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
            x0 < INT32_MIN || x0 > INT32_MAX || y0 < INT32_MIN ||
            y0 > INT32_MAX || x1 < INT32_MIN || x1 > INT32_MAX ||
            y1 < INT32_MIN || y1 > INT32_MAX) {
            text_reason(reason, reason_cap,
                        "swf edit text: empty bounds overflow");
            return MENU_SWF_ERR_BUDGET;
        }
        out->bounds = (MenuSwfShapeRect){(int32_t)llround(x0),
                                         (int32_t)llround(x1),
                                         (int32_t)llround(y0),
                                         (int32_t)llround(y1)};
        out->has_bounds = 1;
    }

    double line_top = 0.0;
    for (uint32_t line_index = 0; line_index < span_count; ++line_index) {
        const TextSpan *span = &spans[line_index];
        uint32_t glyph_first = out->glyph_count;
        uint32_t vertex_first = out->vertex_count;
        double line_ascent = ascent;
        double line_height = font_height;
        for (size_t i = 0; i < span->len; ++i) {
            uint32_t r = run_count ? run_of_char[span->start + i] : 0;
            double scale = run_count ? run_scales[r] : 1.0;
            double run_ascent = ascent * scale;
            double run_height = font_height * scale;
            if (run_ascent > line_ascent) line_ascent = run_ascent;
            if (run_height > line_height) line_height = run_height;
        }
        double line_width =
            measure_text_run(font, run_fonts_used, plain, span->start, span->len,
                             run_of_char, run_scales) *
            scale_x;
        double x_offset = (double)edit->left_margin / 20.0;
        if (!line_index) x_offset += (double)edit->indent / 20.0;
        uint32_t spaces = 0;
        for (size_t i = 0; i < span->len; ++i)
            if (plain[span->start + i] == ' ') ++spaces;
        if (span->align == MENU_SWF_EDIT_TEXT_ALIGN_RIGHT)
            x_offset += content_width - line_width;
        else if (span->align == MENU_SWF_EDIT_TEXT_ALIGN_CENTER)
            x_offset += (content_width - line_width) / 2.0;
        double justify_raw = 0.0;
        if (span->align == MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY &&
            line_index + 1 < span_count && spaces && line_width < content_width)
            justify_raw = (content_width - line_width) / (spaces * scale_x);

        double layout_tx = style->tx + origin_x + x_offset;
        double baseline = style->ty + origin_y + line_top + line_ascent;
        if (!isfinite(layout_tx) || !isfinite(baseline) ||
            layout_tx < INT32_MIN || layout_tx > INT32_MAX ||
            baseline < INT32_MIN || baseline > INT32_MAX) {
            text_reason(reason, reason_cap,
                        "swf edit text: line transform overflow");
            text_reset(out);
            return MENU_SWF_ERR_BUDGET;
        }
        MenuSwfTextStyle line_style = *style;
        line_style.tx = (int32_t)llround(layout_tx);
        line_style.ty = (int32_t)llround(baseline);
        double pen_base = 0.0;
        uint16_t previous = 0;
        int have_previous = 0;
        int has_content = 0;
        for (size_t i = 0; i < span->len; ++i) {
            uint16_t code = (uint8_t)plain[span->start + i];
            uint32_t r = run_count ? run_of_char[span->start + i] : 0;
            double scale = run_count ? run_scales[r] : 1.0;
            MenuSwfTextStyle glyph_style = line_style;
            double sm00 = (double)style->m00 * scale;
            double sm11 = (double)style->m11 * scale;
            if (sm00 < 1.0 || sm11 < 1.0 || sm00 > (double)INT32_MAX ||
                sm11 > (double)INT32_MAX) {
                text_reason(reason, reason_cap,
                            "swf edit text: HTML run transform overflow");
                text_reset(out);
                return MENU_SWF_ERR_BUDGET;
            }
            glyph_style.m00 = (int32_t)llround(sm00);
            glyph_style.m11 = (int32_t)llround(sm11);
            glyph_style.color = (run_count && runs[r].has_color)
                                    ? runs[r].color
                                    : style->color;
            if (have_previous)
                pen_base +=
                    (double)find_kerning(font, previous, code) * scale;
            double pen_local = pen_base / scale;
            if (!isfinite(pen_local) || pen_local < INT32_MIN ||
                pen_local > INT32_MAX) {
                text_reason(reason, reason_cap,
                            "swf edit text: HTML run pen overflow");
                text_reset(out);
                return MENU_SWF_ERR_BUDGET;
            }
            const MenuSwfFontSummary *glyph_font =
                (run_count && r < run_count) ? run_fonts_used[r] : font;
            uint32_t before = out->glyph_count;
            MenuSwfTextGlyph glyph;
            st = append_edit_glyph(
                data, size, glyph_font, code, (int32_t)llround(pen_local), 0,
                &glyph_style, (uint16_t)line_index, (uint16_t)r, out, &glyph,
                reason, reason_cap);
            if (st != MENU_SWF_OK) {
                text_reset(out);
                return st;
            }
            if (out->runs && r < out->run_count) {
                MenuSwfTextRun *run = &out->runs[r];
                if (!run->glyph_count) run->glyph_first = before;
                ++run->glyph_count;
            }
            pen_base += (double)glyph.advance * scale;
            if (justify_raw > 0.0 && code == ' ')
                pen_base += justify_raw;
            if (code != ' ' && code != '\t') has_content = 1;
            previous = code;
            have_previous = 1;
        }
        out->pen_x = (int32_t)llround(pen_base);
        out->pen_y = 0;

        MenuSwfTextLine line;
        memset(&line, 0, sizeof line);
        line.x = (int32_t)llround(layout_tx);
        line.y = (int32_t)llround(baseline - line_ascent);
        line.width = (int32_t)llround(line_width > 0.0 ? line_width : 0.0);
        line.height = (int32_t)llround(line_height);
        line.glyph_first = glyph_first;
        line.glyph_count = out->glyph_count - glyph_first;
        line.has_content = (uint8_t)has_content;
        if (out->vertex_count > vertex_first) {
            int32_t xmin = out->vertices[vertex_first].x;
            int32_t xmax = xmin, ymin = out->vertices[vertex_first].y;
            int32_t ymax = ymin;
            for (uint32_t v = vertex_first + 1; v < out->vertex_count; ++v) {
                int32_t x = out->vertices[v].x, y = out->vertices[v].y;
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
            }
            line.x = xmin;
            line.y = ymin;
            line.width = xmax - xmin;
            line.height = ymax - ymin;
        }
        if (out->lines) out->lines[line_index] = line;
        line_top += line_height + leading_px;
    }
    out->line_count = span_count;
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_text_layout_edit(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfEditText *edit, const MenuSwfTextStyle *style,
    MenuSwfTextMesh *out, char *reason, size_t reason_cap) {
    return layout_edit_impl(data, size, font, NULL, edit, style, out, reason,
                            reason_cap, 0);
}

MenuSwfStatus menu_swf_text_layout_edit_resolved(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfFontSummary *const *run_fonts, const MenuSwfEditText *edit,
    const MenuSwfTextStyle *style, MenuSwfTextMesh *out, char *reason,
    size_t reason_cap) {
    return layout_edit_impl(data, size, font, run_fonts, edit, style, out,
                            reason, reason_cap, 1);
}
