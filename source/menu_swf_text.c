#include "menu_swf_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
    m->missing_glyphs = 0;
    m->pen_x = 0;
    m->pen_y = 0;
    memset(&m->bounds, 0, sizeof m->bounds);
    m->has_bounds = 0;
}

static int buffers_bad(const MenuSwfTextMesh *m) {
    return (!m->vertices && m->vertex_capacity) ||
           (!m->indices && m->index_capacity) ||
           (!m->glyphs && m->glyph_capacity);
}

static void map_point(const MenuSwfTextStyle *s, int64_t x, int64_t y,
                      int32_t *out_x, int32_t *out_y) {
    int64_t tx = (s->m00 * x + s->m01 * y) / 65536 + s->tx;
    int64_t ty = (s->m10 * x + s->m11 * y) / 65536 + s->ty;
    *out_x = (int32_t)tx;
    *out_y = (int32_t)ty;
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
