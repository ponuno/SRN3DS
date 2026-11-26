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
           (!m->lines && m->line_capacity);
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
} TextSpan;

static int plain_push(char *dst, size_t cap, size_t *len, char value) {
    if (*len + 1 >= cap) return -1;
    dst[(*len)++] = value;
    dst[*len] = 0;
    return 0;
}

static int html_entity(const char *src, size_t n, char *out) {
    if (n == 2 && !memcmp(src, "lt", 2)) { *out = '<'; return 0; }
    if (n == 2 && !memcmp(src, "gt", 2)) { *out = '>'; return 0; }
    if (n == 3 && !memcmp(src, "amp", 3)) { *out = '&'; return 0; }
    if (n == 4 && !memcmp(src, "quot", 4)) { *out = '"'; return 0; }
    if (n == 4 && !memcmp(src, "apos", 4)) { *out = '\''; return 0; }
    return -1;
}

static MenuSwfStatus edit_plain_text(const MenuSwfEditText *edit, char *dst,
                                     size_t cap, size_t *out_len,
                                     char *reason, size_t reason_cap) {
    const char *src = edit->initial_text;
    size_t len = edit->initial_text_length;
    if (!len && edit->html) {
        src = edit->variable_name;
        len = strlen(src);
    }
    if (len >= cap) {
        text_reason(reason, reason_cap,
                    "swf edit text: source length %u exceeds %u",
                    (unsigned)len, (unsigned)cap);
        return MENU_SWF_ERR_BUDGET;
    }
    size_t used = 0;
    dst[0] = 0;
    if (!edit->html) {
        memcpy(dst, src, len);
        dst[len] = 0;
        *out_len = len;
        return MENU_SWF_OK;
    }

    char normalized[MENU_SWF_TEXT_MAX_CHARS + 2];
    if (len && src[0] != '<') {
        normalized[0] = '<';
        memcpy(normalized + 1, src, len);
        src = normalized;
        ++len;
    }

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
            size_t name = i + 1 + (closing ? 1 : 0);
            int breaks = !closing && name + 1 < close &&
                         ((src[name] == 'b' && src[name + 1] == 'r') ||
                          (src[name] == 'p' && (name + 1 == close ||
                                                src[name + 1] == ' ')) ||
                          (src[name] == 'l' && src[name + 1] == 'i'));
            if (breaks && used && dst[used - 1] != '\n' &&
                plain_push(dst, cap, &used, '\n')) {
                text_reason(reason, reason_cap,
                            "swf edit text: plain text overflow");
                return MENU_SWF_ERR_BUDGET;
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
            if (html_entity(src + i + 1, close - i - 1, &decoded) ||
                plain_push(dst, cap, &used, decoded)) {
                text_reason(reason, reason_cap,
                            "swf edit text: unsupported HTML entity or overflow");
                return MENU_SWF_ERR_NOTIMPL;
            }
            i = close + 1;
        } else {
            if (plain_push(dst, cap, &used, src[i])) {
                text_reason(reason, reason_cap,
                            "swf edit text: plain text overflow");
                return MENU_SWF_ERR_BUDGET;
            }
            ++i;
        }
    }
    while (used && dst[used - 1] == '\n') dst[--used] = 0;
    *out_len = used;
    return MENU_SWF_OK;
}

static int64_t measure_text_run(const MenuSwfFontSummary *font,
                                const char *text, size_t start, size_t len) {
    int64_t width = 0;
    uint16_t previous = 0;
    int have_previous = 0;
    for (size_t i = 0; i < len; ++i) {
        uint16_t code = (uint8_t)text[start + i];
        if (have_previous) width += find_kerning(font, previous, code);
        const MenuSwfGlyph *glyph = find_glyph(font, code);
        if (glyph && glyph->has_advance) width += glyph->advance;
        previous = code;
        have_previous = 1;
    }
    return width;
}

static MenuSwfStatus split_text_lines(const MenuSwfEditText *edit,
                                      const MenuSwfFontSummary *font,
                                      const char *text, size_t len,
                                      int64_t available_raw, TextSpan *spans,
                                      uint32_t span_capacity, uint32_t *span_count,
                                      char *reason, size_t reason_cap) {
    uint32_t count = 0;
    if (!len) {
        spans[count++] = (TextSpan){0, 0};
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
        if (!edit->word_wrap || !edit->multiline || pos == end) {
            if (count >= span_capacity) goto too_many;
            spans[count++] = (TextSpan){pos, end - pos};
        } else {
            if (available_raw <= 0) {
                text_reason(reason, reason_cap,
                            "swf edit text: bounds/margins leave no text width");
                return MENU_SWF_ERR_BUDGET;
            }
            size_t cur = pos;
            while (cur < end) {
                size_t take = 0, last_space = SIZE_MAX;
                int64_t width = 0;
                uint16_t previous = 0;
                int have_previous = 0;
                for (size_t i = cur; i < end; ++i) {
                    uint16_t code = (uint8_t)text[i];
                    int64_t kerning = have_previous
                                          ? find_kerning(font, previous, code)
                                          : 0;
                    const MenuSwfGlyph *glyph = find_glyph(font, code);
                    int64_t advance = glyph && glyph->has_advance
                                          ? glyph->advance
                                          : 0;
                    int64_t next = width + kerning + advance;
                    if (next > available_raw && i > cur) {
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
                spans[count++] = (TextSpan){cur, take};
                cur += take;
                while (cur < end && text[cur] == ' ') ++cur;
            }
        }
        if (end == len) break;
        pos = end + 1;
        if (pos == len) {
            if (count >= span_capacity) goto too_many;
            spans[count++] = (TextSpan){len, 0};
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
    const MenuSwfTextStyle *style, uint16_t line_index,
    MenuSwfTextMesh *out, MenuSwfTextGlyph *glyph,
    char *reason, size_t reason_cap) {
    memset(glyph, 0, sizeof *glyph);
    glyph->code = code;
    glyph->line_index = line_index;
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

MenuSwfStatus menu_swf_text_layout_edit(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfEditText *edit, const MenuSwfTextStyle *style,
    MenuSwfTextMesh *out, char *reason, size_t reason_cap) {
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
    if (edit->has_font && edit->font_id != font->font_id) {
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
    MenuSwfStatus st = edit_plain_text(edit, plain, sizeof plain, &plain_len,
                                       reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    if (edit->has_max_length && plain_len > edit->max_length) {
        text_reason(reason, reason_cap,
                    "swf edit text: %u characters exceed max length %u",
                    (unsigned)plain_len, edit->max_length);
        return MENU_SWF_ERR_BUDGET;
    }
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

    double scale_x = (double)style->m00 / 65536.0;
    double scale_y = (double)style->m11 / 65536.0;
    double origin_x = (double)edit->bounds[0] / 20.0;
    double origin_y = (double)edit->bounds[2] / 20.0;
    double width_px = (double)(edit->bounds[1] - edit->bounds[0]) / 20.0;
    double height_px = (double)(edit->bounds[3] - edit->bounds[2]) / 20.0;
    double content_width = width_px -
                           ((double)edit->left_margin + edit->right_margin) / 20.0;
    if (width_px <= 0.0 || height_px <= 0.0 || content_width <= 0.0) {
        text_reason(reason, reason_cap,
                    "swf edit text: bounds/margins produce an empty field");
        return MENU_SWF_ERR_BUDGET;
    }
    int64_t available_raw = (int64_t)(content_width / scale_x);

    TextSpan spans[MENU_SWF_TEXT_MAX_LINES];
    uint32_t span_count = 0;
    st = split_text_lines(edit, font, plain, plain_len, available_raw, spans,
                          MENU_SWF_TEXT_MAX_LINES, &span_count, reason,
                          reason_cap);
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

    double font_height = edit->has_font && edit->font_height
                             ? (double)edit->font_height / 20.0
                             : ((double)font->ascent + font->descent) * scale_y;
    double ascent = font->has_layout ? (double)font->ascent * scale_y
                                     : font_height * 0.8;
    double line_height = font_height + (double)edit->leading / 20.0;
    if (!(font_height > 0.0) || !(ascent >= 0.0) || !(line_height > 0.0)) {
        text_reason(reason, reason_cap,
                    "swf edit text: font height/leading is not positive");
        return MENU_SWF_ERR_BUDGET;
    }

    for (uint32_t line_index = 0; line_index < span_count; ++line_index) {
        const TextSpan *span = &spans[line_index];
        uint32_t glyph_first = out->glyph_count;
        uint32_t vertex_first = out->vertex_count;
        int64_t raw_width = measure_text_run(font, plain, span->start, span->len);
        double line_width = (double)raw_width * scale_x;
        double x_offset = (double)edit->left_margin / 20.0;
        if (!line_index) x_offset += (double)edit->indent / 20.0;
        uint32_t spaces = 0;
        for (size_t i = 0; i < span->len; ++i)
            if (plain[span->start + i] == ' ') ++spaces;
        if (edit->alignment == MENU_SWF_EDIT_TEXT_ALIGN_RIGHT)
            x_offset += content_width - line_width;
        else if (edit->alignment == MENU_SWF_EDIT_TEXT_ALIGN_CENTER)
            x_offset += (content_width - line_width) / 2.0;
        double justify_raw = 0.0;
        if (edit->alignment == MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY &&
            line_index + 1 < span_count && spaces && line_width < content_width)
            justify_raw = (content_width - line_width) / (spaces * scale_x);

        double layout_tx = style->tx + origin_x + x_offset;
        double baseline = style->ty + origin_y + ascent +
                          (double)line_index * line_height;
        if (!isfinite(layout_tx) || !isfinite(baseline) ||
            layout_tx < INT32_MIN || layout_tx > INT32_MAX ||
            baseline < INT32_MIN || baseline > INT32_MAX) {
            text_reason(reason, reason_cap,
                        "swf edit text: line transform overflow");
            text_reset(out);
            return MENU_SWF_ERR_BUDGET;
        }
        MenuSwfTextStyle glyph_style = *style;
        glyph_style.tx = (int32_t)llround(layout_tx);
        glyph_style.ty = (int32_t)llround(baseline);
        int32_t pen_x = 0;
        int32_t pen_y = 0; /* line baseline is carried by glyph_style.ty */
        uint16_t previous = 0;
        int have_previous = 0;
        int has_content = 0;
        for (size_t i = 0; i < span->len; ++i) {
            uint16_t code = (uint8_t)plain[span->start + i];
            if (have_previous) pen_x += find_kerning(font, previous, code);
            MenuSwfTextGlyph glyph;
            st = append_edit_glyph(data, size, font, code, pen_x, pen_y,
                                   &glyph_style, (uint16_t)line_index, out,
                                   &glyph, reason, reason_cap);
            if (st != MENU_SWF_OK) {
                text_reset(out);
                return st;
            }
            pen_x = out->pen_x;
            if (justify_raw > 0.0 && code == ' ')
                pen_x += (int32_t)llround(justify_raw);
            if (code != ' ' && code != '\t') has_content = 1;
            previous = code;
            have_previous = 1;
        }

        MenuSwfTextLine line;
        memset(&line, 0, sizeof line);
        line.x = (int32_t)llround(layout_tx);
        line.y = (int32_t)llround(baseline - ascent);
        line.width = (int32_t)llround(line_width > 0.0 ? line_width : 0.0);
        line.height = (int32_t)llround(font_height);
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
    }
    out->line_count = span_count;
    return MENU_SWF_OK;
}
