/* Host test: bounded DefineFont3 glyph tessellation and text layout. */

#include "menu_swf_text.h"
#include "menu_swf_display.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf.h"
#include "bsa_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_s16(uint8_t *p, int16_t v) {
    put_u16(p, (uint16_t)v);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t bit;
    int ok;
} BitW;

static void bw_put(BitW *w, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        size_t bit = w->bit + i;
        if ((bit >> 3) >= w->cap) {
            w->ok = 0;
            return;
        }
        unsigned value = (v >> (n - 1 - i)) & 1u;
        uint8_t mask = (uint8_t)(0x80u >> (bit & 7));
        if (value) w->p[bit >> 3] |= mask;
        else w->p[bit >> 3] &= (uint8_t)~mask;
    }
    w->bit += n;
}

static void bw_signed(BitW *w, int32_t v, unsigned n) {
    uint32_t mask = n == 32 ? 0xffffffffu : ((1u << n) - 1u);
    bw_put(w, (uint32_t)v & mask, n);
}

static void bw_align(BitW *w) {
    while (w->bit & 7u) bw_put(w, 0, 1);
}

static size_t rect_bytes(uint8_t *out, size_t cap, int32_t xmin, int32_t xmax,
                         int32_t ymin, int32_t ymax) {
    memset(out, 0, cap < 32 ? cap : 32);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 12, 5);
    bw_signed(&w, xmin, 12);
    bw_signed(&w, xmax, 12);
    bw_signed(&w, ymin, 12);
    bw_signed(&w, ymax, 12);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static void rec_move(BitW *w, int32_t x, int32_t y) {
    bw_put(w, 0, 1); /* non-edge */
    bw_put(w, 0, 1); /* no new styles */
    bw_put(w, 0, 1); /* no line style */
    bw_put(w, 0, 1); /* no fill1 */
    bw_put(w, 1, 1); /* fill0 */
    bw_put(w, 1, 1); /* move */
    bw_put(w, 10, 5);
    bw_signed(w, x, 10);
    bw_signed(w, y, 10);
    bw_put(w, 1, 1); /* fill style 1 */
}

static void rec_line(BitW *w, int32_t dx, int32_t dy) {
    bw_put(w, 1, 1);  /* edge */
    bw_put(w, 1, 1);  /* straight */
    bw_put(w, 10, 4);
    bw_put(w, 1, 1);  /* general */
    bw_signed(w, dx, 12);
    bw_signed(w, dy, 12);
}

static void rec_end(BitW *w) {
    bw_put(w, 0, 6);
    bw_align(w);
}

static size_t glyph_square(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 4);
    bw_put(&w, 0, 4);
    rec_move(&w, 0, 0);
    rec_line(&w, 100, 0);
    rec_line(&w, 0, 100);
    rec_line(&w, -100, 0);
    rec_line(&w, 0, -100);
    rec_end(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t glyph_triangle(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 4);
    bw_put(&w, 0, 4);
    rec_move(&w, 0, 0);
    rec_line(&w, 100, 0);
    rec_line(&w, -50, 100);
    rec_line(&w, -50, -100);
    rec_end(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t glyph_empty(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 4);
    bw_put(&w, 0, 4);
    rec_move(&w, 0, 0);
    rec_end(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t build_font_tag(uint8_t *out, size_t cap) {
    uint8_t g0[256], g1[256], g2[256], rb[32];
    size_t n0 = glyph_square(g0, sizeof g0);
    size_t n1 = glyph_triangle(g1, sizeof g1);
    size_t n2 = glyph_empty(g2, sizeof g2);
    memset(out, 0, cap);
    size_t at = 0;
    put_u16(out + at, 1);
    at += 2;
    out[at++] = 0x85; /* HasLayout | WideCodes | Bold */
    out[at++] = 0;
    const char *name = "TestFont";
    out[at++] = 8;
    memcpy(out + at, name, 8);
    at += 8;
    put_u16(out + at, 3);
    at += 2;
    size_t base = at;
    size_t off0 = 8;
    size_t off1 = off0 + n0;
    size_t off2 = off1 + n1;
    size_t code_off = off2 + n2;
    put_u16(out + at, (uint16_t)off0);
    at += 2;
    put_u16(out + at, (uint16_t)off1);
    at += 2;
    put_u16(out + at, (uint16_t)off2);
    at += 2;
    put_u16(out + at, (uint16_t)code_off);
    at += 2;
    memcpy(out + at, g0, n0);
    at += n0;
    memcpy(out + at, g1, n1);
    at += n1;
    memcpy(out + at, g2, n2);
    at += n2;
    put_u16(out + at, 'A');
    at += 2;
    put_u16(out + at, 'V');
    at += 2;
    put_u16(out + at, ' ');
    at += 2;
    /* HasLayout: ascent, descent, leading, advances, bounds, kerning. */
    put_u16(out + at, 800);
    at += 2;
    put_u16(out + at, 200);
    at += 2;
    put_s16(out + at, 0);
    at += 2;
    put_s16(out + at, 600);
    at += 2;
    put_s16(out + at, 600);
    at += 2;
    put_s16(out + at, 300);
    at += 2;
    size_t r0 = rect_bytes(rb, sizeof rb, 0, 100, 0, 100);
    memcpy(out + at, rb, r0);
    at += r0;
    r0 = rect_bytes(rb, sizeof rb, 0, 100, 0, 100);
    memcpy(out + at, rb, r0);
    at += r0;
    r0 = rect_bytes(rb, sizeof rb, 0, 0, 0, 0);
    memcpy(out + at, rb, r0);
    at += r0;
    put_u16(out + at, 1); /* kerning count */
    at += 2;
    put_u16(out + at, 'A');
    at += 2;
    put_u16(out + at, 'V');
    at += 2;
    put_s16(out + at, -100);
    at += 2;
    (void)base;
    return at;
}

static size_t make_swf(uint8_t *out, size_t cap, const uint8_t *payload,
                       uint32_t len) {
    uint8_t body[8192];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    int long_form = len >= 0x3f;
    put_u16(body + at, (uint16_t)((75u << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        put_u32(body + at, len);
        at += 4;
    }
    memcpy(body + at, payload, len);
    at += len;
    put_u16(body + at, 0);
    at += 2;
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        put_u32(out + at, len);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t make_two_font_swf(uint8_t *out, size_t cap,
                                const uint8_t *font0, uint32_t font0_len,
                                const uint8_t *font1, uint32_t font1_len) {
    uint8_t body[8192];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, 75, font0, font0_len);
    at = put_tag(body, sizeof body, at, 75, font1, font1_len);
    at = put_tag(body, sizeof body, at, 0, NULL, 0);
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

static size_t make_nested_font_swf(uint8_t *out, size_t cap,
                                   const uint8_t *font, uint32_t font_len) {
    uint8_t body[8192], sprite[2048];
    size_t sprite_len = 4;
    put_u16(sprite, 1);
    sprite_len = put_tag(sprite, sizeof sprite, sprite_len, 75, font, font_len);
    sprite_len = put_tag(sprite, sizeof sprite, sprite_len, 0, NULL, 0);
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, 39, sprite, (uint32_t)sprite_len);
    at = put_tag(body, sizeof body, at, 0, NULL, 0);
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

static size_t make_edit_swf(uint8_t *out, size_t cap,
                            const uint8_t *font, uint32_t font_len,
                            const uint8_t *edit, uint32_t edit_len) {
    uint8_t body[8192];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, 75, font, font_len);
    at = put_tag(body, sizeof body, at, 37, edit, edit_len);
    at = put_tag(body, sizeof body, at, 0, NULL, 0);
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

static size_t build_edit_payload(uint8_t *out, size_t cap, uint16_t id,
                                 uint16_t flags, uint16_t font_id,
                                 uint16_t font_height, uint8_t align,
                                 uint16_t left, uint16_t right,
                                 int16_t indent, int16_t leading,
                                 int32_t xmin, int32_t xmax,
                                 int32_t ymin, int32_t ymax,
                                 const char *variable, const char *text) {
    uint8_t rect[32];
    size_t n = rect_bytes(rect, sizeof rect, xmin, xmax, ymin, ymax);
    if (!n || cap < 2 + n + 2 + 32 + strlen(variable) + 1 +
                      (text ? strlen(text) + 1 : 0))
        return 0;
    size_t at = 0;
    put_u16(out + at, id);
    at += 2;
    memcpy(out + at, rect, n);
    at += n;
    put_u16(out + at, flags);
    at += 2;
    if (flags & 0x0080u) {
        put_u16(out + at, font_id);
        at += 2;
        put_u16(out + at, font_height);
        at += 2;
    }
    if (flags & 0x0020u) {
        out[at++] = 0x11;
        out[at++] = 0x22;
        out[at++] = 0x33;
        out[at++] = 0x44;
    }
    if (flags & 0x0040u) {
        put_u16(out + at, 8);
        at += 2;
    }
    if (flags & 0x0100u) {
        out[at++] = align;
        put_u16(out + at, left);
        at += 2;
        put_u16(out + at, right);
        at += 2;
        put_s16(out + at, indent);
        at += 2;
        put_s16(out + at, leading);
        at += 2;
    }
    size_t var_len = strlen(variable) + 1;
    memcpy(out + at, variable, var_len);
    at += var_len;
    if (flags & 0x0001u) {
        size_t text_len = strlen(text) + 1;
        memcpy(out + at, text, text_len);
        at += text_len;
    }
    return at;
}

typedef struct {
    MenuSwfTextVertex vertices[8192];
    uint32_t indices[24576];
    MenuSwfTextGlyph glyphs[32];
    MenuSwfTextLine lines[32];
    MenuSwfTessVertex tvert[8192];
    uint32_t tindices[24576];
    uint16_t tfills[8192];
    MenuSwfTessLine tlines[2048];
    MenuSwfTessEdge tedges[4096];
    uint32_t tactive[4096];
    int32_t tbands[8192];
} TextFixture;

static void fixture_init(TextFixture *f, MenuSwfTextMesh *m) {
    memset(m, 0, sizeof *m);
    m->vertices = f->vertices;
    m->vertex_capacity = 8192;
    m->indices = f->indices;
    m->index_capacity = 24576;
    m->glyphs = f->glyphs;
    m->glyph_capacity = 32;
    m->lines = f->lines;
    m->line_capacity = 32;
    m->glyph_tess.vertices = f->tvert;
    m->glyph_tess.vertex_capacity = 8192;
    m->glyph_tess.indices = f->tindices;
    m->glyph_tess.index_capacity = 24576;
    m->glyph_tess.triangle_fills = f->tfills;
    m->glyph_tess.triangle_capacity = 8192;
    m->glyph_tess.lines = f->tlines;
    m->glyph_tess.line_capacity = 2048;
    m->glyph_tess.edges = f->tedges;
    m->glyph_tess.edge_capacity = 4096;
    m->glyph_tess.active = f->tactive;
    m->glyph_tess.active_capacity = 4096;
    m->glyph_tess.bands = f->tbands;
    m->glyph_tess.band_capacity = 8192;
}

static void style_identity(MenuSwfTextStyle *s) {
    memset(s, 0, sizeof *s);
    s->m00 = 65536;
    s->m11 = 65536;
    s->tx = 1000;
    s->ty = 2000;
    s->color = 0xFF112233u;
}

static void test_synthetic(void) {
    uint8_t tag[2048], file[4096];
    static TextFixture f;
    MenuSwfTextMesh m;
    MenuSwfTextStyle style;
    char reason[BSA_REASON_MAX];

    size_t tlen = build_font_tag(tag, sizeof tag);
    size_t flen = make_swf(file, sizeof file, tag, (uint32_t)tlen);
    check(flen > 0, "synthetic font fixture builds");

    MenuSwfDisplaySummary *display =
        (MenuSwfDisplaySummary *)malloc(sizeof *display);
    check(display != NULL, "display summary allocation");
    if (!display) return;
    MenuSwfStatus st = menu_swf_display_summary(file, flen, display, reason,
                                                sizeof reason);
    check(st == MENU_SWF_OK, "synthetic display summary");
    check(display->stored_fonts == 1 && display->fonts[0].glyph_count == 3,
          "synthetic font glyph count");
    check(display->fonts[0].stored_kerning == 1 &&
              display->fonts[0].kerning[0].code1 == 'A' &&
              display->fonts[0].kerning[0].code2 == 'V' &&
              display->fonts[0].kerning[0].adjustment == -100,
          "synthetic kerning table");
    const MenuSwfFontSummary *font = &display->fonts[0];

    fixture_init(&f, &m);
    MenuSwfTessMesh glyph_mesh = m.glyph_tess;
    st = menu_swf_tess_glyph(file, flen, font->font_id, 'A',
                             MENU_SWF_TESS_NON_ZERO, &glyph_mesh, reason,
                             sizeof reason);
    check(st == MENU_SWF_OK, "synthetic glyph tess");
    check(glyph_mesh.vertex_count == 4 && glyph_mesh.triangle_count == 2 &&
              glyph_mesh.line_count == 0,
          "synthetic glyph tess counts");
    check(glyph_mesh.has_bounds && glyph_mesh.bounds.xmin == 0 &&
              glyph_mesh.bounds.xmax == 100 &&
              glyph_mesh.bounds.ymin == 0 && glyph_mesh.bounds.ymax == 100,
          "synthetic glyph tess bounds");
    check(glyph_mesh.triangle_fills[0] == 1, "synthetic glyph fill style");
    st = menu_swf_tess_glyph(file, flen, font->font_id, ' ',
                             MENU_SWF_TESS_NON_ZERO, &glyph_mesh, reason,
                             sizeof reason);
    check(st == MENU_SWF_OK && glyph_mesh.triangle_count == 0,
          "synthetic empty glyph tess");
    st = menu_swf_tess_glyph(file, flen, font->font_id, 'Z',
                             MENU_SWF_TESS_NON_ZERO, &glyph_mesh, reason,
                             sizeof reason);
    check(st == MENU_SWF_ERR_ARGUMENT, "synthetic missing glyph tess");

    fixture_init(&f, &m);
    style_identity(&style);
    const char *text = "AV A";
    st = menu_swf_text_layout(file, flen, font, text, strlen(text), &style, &m,
                              reason, sizeof reason);
    check(st == MENU_SWF_OK, "synthetic text layout");
    check(m.glyph_count == 4 && m.missing_glyphs == 0, "synthetic glyph count");
    check(m.vertex_count == 11 && m.triangle_count == 5 &&
              m.index_count == 15,
          "synthetic vertex/triangle counts");
    check(m.pen_x == 2000, "synthetic final pen");
    check(m.has_bounds && m.bounds.xmin == 1000 && m.bounds.xmax == 2500 &&
              m.bounds.ymin == 2000 && m.bounds.ymax == 2100,
          "synthetic text bounds");
    check(m.glyphs[0].code == 'A' && m.glyphs[0].x == 1000 &&
              m.glyphs[0].pen_x == 0 && m.glyphs[0].advance == 600,
          "synthetic first glyph placement");
    check(m.glyphs[1].code == 'V' && m.glyphs[1].kerning == -100 &&
              m.glyphs[1].x == 1500 && m.glyphs[1].pen_x == 500,
          "synthetic kerning placement");
    check(m.glyphs[2].code == ' ' && !m.glyphs[2].has_geometry &&
              m.glyphs[2].vertex_count == 0 &&
              m.glyphs[2].triangle_count == 0 && m.glyphs[2].advance == 300,
          "synthetic empty glyph");
    check(m.glyphs[0].triangle_first == 0 &&
              m.glyphs[0].triangle_count == 2 &&
              m.glyphs[1].triangle_first == 2 &&
              m.glyphs[1].triangle_count == 1 &&
              m.glyphs[3].triangle_first == 3 &&
              m.glyphs[3].triangle_count == 2,
          "synthetic glyph triangle ranges");
    check(m.vertices[0].color == style.color, "synthetic vertex color");
    int indices_ok = 1;
    for (uint32_t i = 0; i < m.index_count; ++i)
        if (m.indices[i] >= m.vertex_count) indices_ok = 0;
    check(indices_ok, "synthetic indices in range");

    /* Half scale plus translation: geometry and placements scale together. */
    MenuSwfTextStyle half = style;
    half.m00 = 32768;
    half.m11 = 32768;
    fixture_init(&f, &m);
    st = menu_swf_text_layout(file, flen, font, "A", 1, &half, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_OK, "scaled text layout");
    check(m.has_bounds && m.bounds.xmax == 1050 && m.bounds.ymax == 2050,
          "scaled text bounds");

    /* 90-degree rotation through the same 16.16 matrix. */
    MenuSwfTextStyle rot;
    memset(&rot, 0, sizeof rot);
    rot.m01 = -65536;
    rot.m10 = 65536;
    rot.color = style.color;
    fixture_init(&f, &m);
    st = menu_swf_text_layout(file, flen, font, "A", 1, &rot, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_OK, "rotated text layout");
    check(m.has_bounds && m.bounds.xmin == -100 && m.bounds.xmax == 0 &&
              m.bounds.ymin == 0 && m.bounds.ymax == 100,
          "rotated text bounds");

    /* Unknown and empty glyphs keep their placements but no triangles. */
    fixture_init(&f, &m);
    st = menu_swf_text_layout(file, flen, font, "Z", 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_OK && m.glyph_count == 1 && m.missing_glyphs == 1 &&
              m.glyphs[0].missing &&
              m.glyphs[0].glyph_index == MENU_SWF_TEXT_MISSING_GLYPH &&
              m.vertex_count == 0 && m.pen_x == 0,
          "unknown glyph placement");

    fixture_init(&f, &m);
    st = menu_swf_text_layout(file, flen, font, " ", 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_OK && m.glyph_count == 1 && m.missing_glyphs == 0 &&
              !m.glyphs[0].has_geometry && m.pen_x == 300,
          "empty glyph placement");

    /* Named overflow refusals. */
    fixture_init(&f, &m);
    m.vertex_capacity = 2;
    st = menu_swf_text_layout(file, flen, font, "A", 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && reason[0], "text vertex overflow");

    fixture_init(&f, &m);
    m.index_capacity = 5;
    st = menu_swf_text_layout(file, flen, font, "A", 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET, "text index overflow");

    fixture_init(&f, &m);
    m.glyph_capacity = 1;
    st = menu_swf_text_layout(file, flen, font, "AV", 2, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET, "text glyph overflow");

    fixture_init(&f, &m);
    m.glyph_tess.vertex_capacity = 3;
    st = menu_swf_text_layout(file, flen, font, "A", 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET, "glyph tess scratch overflow");

    fixture_init(&f, &m);
    st = menu_swf_text_layout(file, flen, font, "x",
                              MENU_SWF_TEXT_MAX_CHARS + 1, &style, &m, reason,
                              sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET, "text length overflow");

    free(display);
}

static void test_edit_text(void) {
    uint8_t font_tag[2048], edit_tag[2048], file[8192];
    uint8_t two_file[10000], nested_file[4096], oversized[4096], malformed[2048];
    size_t font_len = build_font_tag(font_tag, sizeof font_tag);
    uint16_t flags = 0x0001u | 0x0004u | 0x0010u | 0x0020u |
                     0x0040u | 0x0080u | 0x0100u;
    size_t edit_len = build_edit_payload(
        edit_tag, sizeof edit_tag, 7, flags, 1, 400,
        MENU_SWF_EDIT_TEXT_ALIGN_CENTER, 20, 30, 10, 100,
        -2000, 2000, -200, 800, "label", "AV A");
    size_t file_len = make_edit_swf(file, sizeof file, font_tag,
                                    (uint32_t)font_len, edit_tag,
                                    (uint32_t)edit_len);
    check(font_len && edit_len && file_len, "edit-text fixture builds");

    MenuSwfEditText parsed;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_display_parse_edit_text(
        edit_tag, edit_len, &parsed, reason, sizeof reason);
    check(st == MENU_SWF_OK, "DefineEditText parse");
    check(parsed.character_id == 7 && parsed.flags == flags &&
              parsed.word_wrap == 0 && parsed.multiline && parsed.read_only &&
              parsed.has_text_color && parsed.has_max_length && parsed.has_font &&
              parsed.has_layout && parsed.font_id == 1 &&
              parsed.font_height == 400 && parsed.max_length == 8,
          "DefineEditText flags/font/max fields");
    check(parsed.bounds[0] == -2000 && parsed.bounds[1] == 2000 &&
              parsed.bounds[2] == -200 && parsed.bounds[3] == 800 &&
              parsed.alignment == MENU_SWF_EDIT_TEXT_ALIGN_CENTER &&
              parsed.left_margin == 20 && parsed.right_margin == 30 &&
              parsed.indent == 10 && parsed.leading == 100 &&
              !strcmp(parsed.variable_name, "label") &&
              !strcmp(parsed.initial_text, "AV A"),
          "DefineEditText bounds/layout/strings");

    MenuSwfEditText items[4];
    MenuSwfEditTextList list = {items, 4, 0, 0, 0};
    st = menu_swf_display_collect_edit_texts(file, file_len, &list, reason,
                                             sizeof reason);
    check(st == MENU_SWF_OK && list.count == 1 && list.total == 1 &&
              menu_swf_display_find_edit_text(&list, 7) == &items[0],
          "DefineEditText caller-owned collector");

    MenuSwfDisplaySummary *display = malloc(sizeof *display);
    check(display != NULL, "edit display allocation");
    if (!display) return;
    st = menu_swf_display_summary(file, file_len, display, reason, sizeof reason);
    check(st == MENU_SWF_OK && display->stored_fonts == 1,
          "edit fixture display/font summary");
    if (st != MENU_SWF_OK) {
        free(display);
        return;
    }
    const MenuSwfFontSummary *font = &display->fonts[0];

    MenuSwfFontSummary font_items[4];
    MenuSwfFontList font_list = {.items = font_items, .capacity = 4};
    st = menu_swf_display_collect_fonts(file, file_len, &font_list, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK && font_list.count == 1 &&
              font_items[0].font_id == 1,
          "caller-owned DefineFont collection");

    size_t nested_len = make_nested_font_swf(nested_file, sizeof nested_file,
                                             font_tag, (uint32_t)font_len);
    st = menu_swf_display_collect_fonts(nested_file, nested_len, &font_list,
                                        reason, sizeof reason);
    check(st == MENU_SWF_OK && font_list.count == 1 && font_list.total == 1,
          "DefineFont collector recurses into DefineSprite");
    st = menu_swf_display_collect_fonts(file, file_len, &font_list, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK && font_list.count == 1,
          "top-level font collection restored after nested fixture");

    MenuSwfFontSummary parsed_font;
    st = menu_swf_display_parse_font(font_tag, font_len, 3, &parsed_font,
                                     reason, sizeof reason);
    check(st == MENU_SWF_OK && parsed_font.font_id == 1 &&
              parsed_font.glyph_count == 3,
          "single DefineFont payload parse");
    st = menu_swf_display_parse_font(font_tag, font_len - 1, 3, &parsed_font,
                                     reason, sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && reason[0],
          "malformed DefineFont payload is named FORMAT");

    MenuSwfFontList no_font_capacity = {0};
    st = menu_swf_display_collect_fonts(file, file_len, &no_font_capacity, reason,
                                        sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && no_font_capacity.total == 1 &&
              no_font_capacity.truncated,
          "DefineFont collector capacity refusal");

    size_t two_len = make_two_font_swf(two_file, sizeof two_file,
                                       font_tag, (uint32_t)font_len,
                                       font_tag, (uint32_t)font_len);
    MenuSwfFontSummary duplicate_items[2];
    MenuSwfFontList duplicates = {
        .items = duplicate_items, .capacity = 2};
    st = menu_swf_display_collect_fonts(two_file, two_len, &duplicates, reason,
                                        sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "duplicate DefineFont id"),
          "duplicate DefineFont id is fatal");

    memcpy(malformed, font_tag, font_len);
    malformed[2] &= (uint8_t)~0x04u; /* Font3 requires WideCodes */
    size_t malformed_len = make_swf(two_file, sizeof two_file, malformed,
                                    (uint32_t)font_len);
    st = menu_swf_display_collect_fonts(two_file, malformed_len, &font_list,
                                        reason, sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "WideCodes"),
          "malformed DefineFont is fatal to collection");
    st = menu_swf_display_collect_fonts(file, file_len, &font_list, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK && font_list.count == 1,
          "font collector resets cleanly after refusal");

    memcpy(oversized, font_tag, font_len);
    put_u16(oversized, 17);
    put_u16(oversized + font_len - 8, MENU_SWF_DISPLAY_MAX_KERNING + 1u);
    size_t oversized_len = font_len;
    for (uint32_t i = 1; i <= MENU_SWF_DISPLAY_MAX_KERNING; ++i) {
        put_u16(oversized + oversized_len, 'A');
        put_u16(oversized + oversized_len + 2, 'V');
        put_s16(oversized + oversized_len + 4, 0);
        oversized_len += 6;
    }
    two_len = make_two_font_swf(two_file, sizeof two_file,
                                font_tag, (uint32_t)font_len,
                                oversized, (uint32_t)oversized_len);
    MenuSwfFontSummary skip_items[4];
    MenuSwfFontList skipped = {.items = skip_items, .capacity = 4};
    st = menu_swf_display_collect_fonts(two_file, two_len, &skipped, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK && skipped.count == 1 && skipped.total == 2 &&
              skipped.skipped == 1 && skipped.truncated &&
              strstr(skipped.skip_reason, "id 17"),
          "oversized unrelated DefineFont is skipped, not fatal");

    MenuSwfImportSymbol import_symbol = {1, "SameName"};
    MenuSwfImportAssets import_asset;
    memset(&import_asset, 0, sizeof import_asset);
    strcpy(import_asset.url, "gfxfontlib.swf");
    import_asset.symbols[0] = import_symbol;
    import_asset.symbols[1] = (MenuSwfImportSymbol){34, "OtherName"};
    import_asset.symbol_count = import_asset.symbol_total = 2;
    MenuSwfImportAssets import_items[1] = {import_asset};
    MenuSwfImportList import_list = {import_items, 1, 1, 1, 0};
    const MenuSwfFontSummary *resolved = NULL;
    st = menu_swf_display_resolve_import_font(
        &import_list, &font_list, 1, "Interface/fonts_en.swf", &resolved,
        reason, sizeof reason);
    check(st == MENU_SWF_OK && resolved == &font_items[0],
          "import font resolves by exact id");
    resolved = NULL;
    st = menu_swf_display_resolve_import_font(
        &import_list, &font_list, 34, "Interface/fonts_en.swf", &resolved,
        reason, sizeof reason);
    check(st == MENU_SWF_ERR_NOTIMPL && !resolved &&
              strstr(reason, "imported font id 34 is not present in Interface/fonts_en.swf"),
          "missing import font id is named, not name-substituted");

    MenuSwfFontSummary wrong_items[1] = {font_items[0]};
    wrong_items[0].font_id = 34; /* same name, wrong id */
    MenuSwfFontList wrong_fonts = {
        .items = wrong_items, .capacity = 1, .count = 1, .total = 1};
    st = menu_swf_display_resolve_import_font(
        &import_list, &wrong_fonts, 1, "Interface/fonts_en.swf", &resolved,
        reason, sizeof reason);
    check(st == MENU_SWF_ERR_NOTIMPL &&
              strstr(reason, "imported font id 1 is not present in Interface/fonts_en.swf"),
          "import resolver never substitutes by font name");

    static TextFixture fixture;
    MenuSwfTextMesh mesh;
    MenuSwfTextStyle style;
    memset(&style, 0, sizeof style);
    style.m00 = style.m11 = 8192; /* 0.125 pixels per raw font unit */
    style.color = 0xff112233u;

    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &parsed, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.glyph_count == 4 &&
              mesh.missing_glyphs == 0 && mesh.line_count == 1 &&
              mesh.vertex_count == 11 && mesh.triangle_count == 5,
          "DefineEditText basic layout");
    check(mesh.lines && mesh.lines[0].glyph_count == 4 &&
              mesh.lines[0].has_content,
          "DefineEditText line record");

    MenuSwfEditText wrapped = parsed;
    wrapped.word_wrap = 1;
    wrapped.bounds[0] = -1000;
    wrapped.bounds[1] = 1000;
    memcpy(wrapped.initial_text, "AV", 3);
    wrapped.initial_text_length = 2;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &wrapped, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.glyph_count == 2 &&
              mesh.line_count == 2 && mesh.lines[0].glyph_count == 1 &&
              mesh.lines[1].glyph_count == 1,
          "DefineEditText WordWrap");

    MenuSwfEditText multiline = parsed;
    multiline.leading = 100;
    memcpy(multiline.initial_text, "A\nV", 4);
    multiline.initial_text_length = 3;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &multiline, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.line_count == 2 &&
              mesh.glyphs[1].y - mesh.glyphs[0].y == 25,
          "DefineEditText Multiline/leading");

    for (unsigned align = 0; align < 4; ++align) {
        MenuSwfEditText aligned = parsed;
        aligned.alignment = (uint8_t)align;
        aligned.left_margin = aligned.right_margin = aligned.indent = 0;
        memcpy(aligned.initial_text, "A", 2);
        aligned.initial_text_length = 1;
        if (align == MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY) {
            memcpy(aligned.initial_text, "A V\nA", 6);
            aligned.initial_text_length = 5;
        }
        fixture_init(&fixture, &mesh);
        st = menu_swf_text_layout_edit(file, file_len, font, &aligned, &style,
                                       &mesh, reason, sizeof reason);
        check(st == MENU_SWF_OK, "DefineEditText alignment layout");
        if (st == MENU_SWF_OK) {
            if (align == MENU_SWF_EDIT_TEXT_ALIGN_LEFT)
                check(mesh.glyphs[0].x == -100, "edit align left");
            else if (align == MENU_SWF_EDIT_TEXT_ALIGN_RIGHT)
                check(mesh.glyphs[0].x == 25, "edit align right");
            else if (align == MENU_SWF_EDIT_TEXT_ALIGN_CENTER)
                check(mesh.glyphs[0].x == -38, "edit align center");
            else
                check(mesh.glyphs[2].x == 25 && mesh.glyphs[3].x == -100,
                      "edit justify distributes non-final line");
        }
    }

    MenuSwfEditText html = parsed;
    html.html = 1;
    html.initial_text[0] = 0;
    html.initial_text_length = 0;
    strcpy(html.variable_name, "<p>AV</p>");
    char plain[64];
    size_t plain_len = 0;
    st = menu_swf_text_edit_plain(&html, plain, sizeof plain, &plain_len,
                                  reason, sizeof reason);
    check(st == MENU_SWF_OK && plain_len == 2 && !memcmp(plain, "AV", 2),
          "DefineEditText HTML plain extraction");
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &html, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.glyph_count == 2 &&
              mesh.glyphs[0].code == 'A' && mesh.glyphs[1].code == 'V',
          "DefineEditText GFx HTML variable payload");

    MenuSwfEditText empty = parsed;
    empty.initial_text[0] = 0;
    empty.initial_text_length = 0;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &empty, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.glyph_count == 0 &&
              mesh.vertex_count == 0 && mesh.line_count == 1 &&
              mesh.has_bounds && mesh.bounds.xmin == -100 &&
              mesh.bounds.xmax == 100 && mesh.bounds.ymin == -10 &&
              mesh.bounds.ymax == 40,
          "empty DefineEditText keeps bounds/empty rect");

    MenuSwfEditText password = parsed;
    password.password = 1;
    memcpy(password.initial_text, "AV", 3);
    password.initial_text_length = 2;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &password, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_ERR_NOTIMPL && strstr(reason, "password mask"),
          "Password without '*' is named NOTIMPL");

    MenuSwfEditText unknown = parsed;
    unknown.font_id = 99;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, font, &unknown, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_ERR_ARGUMENT && strstr(reason, "font id"),
          "unknown DefineEditText font id is named");

    fixture_init(&fixture, &mesh);
    mesh.glyph_capacity = 0;
    st = menu_swf_text_layout_edit(file, file_len, font, &parsed, &style,
                                   &mesh, reason, sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && strstr(reason, "glyph capacity"),
          "DefineEditText glyph overflow is named");

    size_t sentinel_len = build_edit_payload(
        edit_tag, sizeof edit_tag, 8, 0x0080u | 0x0100u, 1, 400, 0xff,
        0xffff, 0xffff, 0, 0, -2000, 2000, -200, 800, "x", "");
    st = menu_swf_display_parse_edit_text(edit_tag, sentinel_len, &parsed,
                                          reason, sizeof reason);
    check(st == MENU_SWF_OK &&
              parsed.alignment == MENU_SWF_EDIT_TEXT_ALIGN_LEFT &&
              parsed.left_margin == 0 && parsed.right_margin == 0,
          "retail GFx alignment/margin sentinels normalize");

    size_t bad_align_len = build_edit_payload(
        edit_tag, sizeof edit_tag, 8, 0x0100u, 1, 400, 4,
        0, 0, 0, 0, -2000, 2000, -200, 800, "x", "");
    st = menu_swf_display_parse_edit_text(edit_tag, bad_align_len, &parsed,
                                          reason, sizeof reason);
    check(st == MENU_SWF_ERR_NOTIMPL && strstr(reason, "alignment"),
          "unknown DefineEditText alignment is named");

    char long_variable[320];
    memset(long_variable, 'v', sizeof long_variable - 1);
    long_variable[sizeof long_variable - 1] = 0;
    size_t long_len = build_edit_payload(
        edit_tag, sizeof edit_tag, 8, 0, 1, 400, 0,
        0, 0, 0, 0, -2000, 2000, -200, 800, long_variable, "");
    st = menu_swf_display_parse_edit_text(edit_tag, long_len, &parsed,
                                          reason, sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && strstr(reason, "variable name length"),
          "DefineEditText variable overflow is named");

    st = menu_swf_display_parse_edit_text(edit_tag, sentinel_len - 1, &parsed,
                                          reason, sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT, "truncated DefineEditText is named");

    MenuSwfEditTextList no_capacity = {NULL, 0, 0, 0, 0};
    st = menu_swf_display_collect_edit_texts(file, file_len, &no_capacity,
                                             reason, sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && no_capacity.total == 1 &&
              no_capacity.truncated,
          "DefineEditText collector capacity overflow is named");

    free(display);
}

static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    BsaArchive *archive = NULL;
    BsaMatch match;
    for (unsigned i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof path, "%s/%s", data_dir, archives[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (bsa_open_path(path, NULL, &archive, NULL, 0) == BSA_OK) {
            if (bsa_find(archive, "interface/startmenu.swf", &match) == BSA_OK)
                break;
            bsa_close(archive);
            archive = NULL;
        }
    }
    if (!archive) {
        printf("real: skip (StartMenu.swf not found under %s)\n", data_dir);
        return;
    }
    uint32_t size = 0;
    char reason[BSA_REASON_MAX];
    check(bsa_unpacked_size(archive, &match.entry, &size, reason,
                            sizeof reason) == BSA_OK,
          "real StartMenu size");
    uint8_t *bytes = (uint8_t *)malloc(size ? size : 1);
    size_t got = 0;
    if (bytes)
        check(bsa_read(archive, &match.entry, bytes, size, &got, reason,
                       sizeof reason) == BSA_OK && got == size,
              "real StartMenu read");
    BsaMatch font_match;
    int has_fontlib = bsa_find(archive, "interface/gfxfontlib.swf",
                               &font_match) == BSA_OK;
    printf("real: gfxfontlib.swf present=%d\n", has_fontlib);
    bsa_close(archive);
    BsaArchive *base_archive = NULL;
    char base_path[1024];
    snprintf(base_path, sizeof base_path, "%s/Skyrim - Interface.bsa", data_dir);
    if (bsa_open_path(base_path, NULL, &base_archive, NULL, 0) == BSA_OK) {
        int base_has_font = bsa_find(base_archive, "interface/gfxfontlib.swf",
                                     &font_match) == BSA_OK;
        printf("real: base gfxfontlib.swf present=%d\n", base_has_font);
        bsa_close(base_archive);
    }
    if (!bytes) return;

    MenuSwfDisplaySummary *display =
        (MenuSwfDisplaySummary *)malloc(sizeof *display);
    static TextFixture f;
    MenuSwfTextMesh m;
    MenuSwfTextStyle style;
    check(display != NULL, "real display allocation");
    if (!display) {
        free(bytes);
        return;
    }
    MenuSwfStatus st = menu_swf_display_summary(bytes, got, display, reason,
                                                sizeof reason);
    check(st == MENU_SWF_OK, "real display summary");
    if (st == MENU_SWF_OK) {
        MenuSwfSummary import_summary;
        st = menu_swf_summary(bytes, got, &import_summary, reason, sizeof reason);
        check(st == MENU_SWF_OK, "real import summary");
        if (st == MENU_SWF_OK) {
            for (uint32_t i = 0; i < import_summary.name_count; ++i) {
                const MenuSwfName *name = &import_summary.names[i];
                if (name->kind == MENU_SWF_NAME_IMPORT ||
                    name->kind == MENU_SWF_NAME_IMPORT_URL)
                    printf("real: import id=%u kind=%u name='%s'\n", name->id,
                           name->kind, name->name);
            }
        }
        MenuSwfEditText *edit_items = calloc(MENU_SWF_DISPLAY_MAX_EDIT_TEXTS,
                                             sizeof *edit_items);
        MenuSwfEditTextList edit_list = {edit_items,
                                         MENU_SWF_DISPLAY_MAX_EDIT_TEXTS,
                                         0, 0, 0};
        check(edit_items != NULL, "real edit-text allocation");
        if (edit_items) {
            st = menu_swf_display_collect_edit_texts(
                bytes, got, &edit_list, reason, sizeof reason);
            check(st == MENU_SWF_OK && edit_list.count == 22,
                  "real 22 DefineEditText definitions");
            if (st == MENU_SWF_OK) {
                uint32_t nonempty = 0, chars = 0;
                int plain_ok = 1;
                char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
                for (uint32_t i = 0; i < edit_list.count; ++i) {
                    const MenuSwfEditText *e = &edit_items[i];
                    size_t plain_len = 0;
                    st = menu_swf_text_edit_plain(e, plain, sizeof plain,
                                                  &plain_len, reason,
                                                  sizeof reason);
                    if (st != MENU_SWF_OK) plain_ok = 0;
                    if (plain_len) ++nonempty;
                    chars += (uint32_t)plain_len;
                    printf("real: edit id=%u flags=%04x font=%u height=%u "
                           "bounds=%d,%d,%d,%d align=%u var='%s' len=%u "
                           "text='%.*s' plain_len=%u\n",
                           e->character_id, e->flags, e->font_id,
                           e->font_height, e->bounds[0], e->bounds[1],
                           e->bounds[2], e->bounds[3], e->alignment,
                           e->variable_name, e->initial_text_length,
                           (int)e->initial_text_length, e->initial_text,
                           (unsigned)plain_len);
                }
                printf("real: edit_text definitions=%u nonempty=%u chars=%u\n",
                       edit_list.count, nonempty, chars);
                check(plain_ok && nonempty == 3 && chars == 72,
                      "real three plain edit-text sources total 72 chars");
            } else {
                printf("real: edit-text collect failed: %s (%s)\n",
                       menu_swf_status_name(st), reason);
            }
        }
        free(edit_items);

        const MenuSwfFontSummary *font = NULL;
        for (uint32_t i = 0; i < display->stored_fonts; ++i)
            if (display->fonts[i].glyph_count == 243) {
                font = &display->fonts[i];
                break;
            }
        check(font != NULL, "real 243-glyph font found");
        if (font) {
            printf("real: local font id=%u name='%s' glyphs=%u\n",
                   font->font_id, font->name, font->glyph_count);
            check(font->stored_kerning == 94, "real 94 kerning records");
            char text[8];
            size_t text_len = 0;
            const uint16_t codes[4] = {33, 34, 35, 36};
            for (unsigned i = 0; i < 4; ++i) text[text_len++] = (char)codes[i];

            fixture_init(&f, &m);
            style_identity(&style);
            style.tx = 0;
            style.ty = 0;
            st = menu_swf_text_layout(bytes, got, font, text, text_len, &style,
                                      &m, reason, sizeof reason);
            check(st == MENU_SWF_OK, "real text layout");
            if (st == MENU_SWF_OK) {
                printf("real: text glyphs=%u vertices=%u triangles=%u "
                       "missing=%u pen=%d kerning=%u\n",
                       m.glyph_count, m.vertex_count, m.triangle_count,
                       m.missing_glyphs, m.pen_x, font->stored_kerning);
                printf("real: first glyph code=%u index=%u x=%d y=%d "
                       "advance=%d tris=%u bounds=%d,%d,%d,%d\n",
                       m.glyphs[0].code, m.glyphs[0].glyph_index,
                       m.glyphs[0].x, m.glyphs[0].y, m.glyphs[0].advance,
                       m.glyphs[0].triangle_count, m.glyphs[0].bounds.xmin,
                       m.glyphs[0].bounds.xmax, m.glyphs[0].bounds.ymin,
                       m.glyphs[0].bounds.ymax);
                check(m.glyph_count == 4 && m.missing_glyphs == 0,
                      "real four glyph placements");
                check(m.vertex_count > 0 && m.triangle_count > 0,
                      "real glyph triangles");
                check(m.glyphs[0].has_geometry, "real first glyph geometry");
                check(m.font_id == font->font_id && m.em_square == 1024 &&
                          m.shape_scale == 20,
                      "real font metrics");
            } else {
                printf("real: text failed: %s\n", reason);
            }

            if (font->stored_kerning) {
                uint16_t c1 = font->kerning[0].code1;
                uint16_t c2 = font->kerning[0].code2;
                if (c1 < 256 && c2 < 256) {
                    char pair[2] = {(char)c1, (char)c2};
                    fixture_init(&f, &m);
                    st = menu_swf_text_layout(bytes, got, font, pair, 2,
                                              &style, &m, reason,
                                              sizeof reason);
                    check(st == MENU_SWF_OK && m.glyph_count == 2,
                          "real kerning pair layout");
                    if (st == MENU_SWF_OK)
                        check(m.glyphs[1].kerning ==
                                  font->kerning[0].adjustment,
                              "real kerning applied");
                }
            }
        }
    } else {
        printf("real: display failed: %s\n", reason);
    }
    free(display);
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF text tests\n");
    test_synthetic();
    test_edit_text();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF text: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
