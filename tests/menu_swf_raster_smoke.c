/* Host test: bounded A8 coverage rasterisation of tessellated glyphs. */

#include "menu_swf_raster.h"
#include "menu_swf_tess.h"
#include "menu_swf_shape.h"
#include "menu_swf_display.h"
#include "menu_swf.h"
#include "bsa_reader.h"

#include <math.h>
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
    bw_put(w, 0, 1);
    bw_put(w, 0, 1);
    bw_put(w, 0, 1);
    bw_put(w, 0, 1);
    bw_put(w, 1, 1);
    bw_put(w, 1, 1);
    bw_put(w, 10, 5);
    bw_signed(w, x, 10);
    bw_signed(w, y, 10);
    bw_put(w, 1, 1);
}

static void rec_line(BitW *w, int32_t dx, int32_t dy) {
    bw_put(w, 1, 1);
    bw_put(w, 1, 1);
    bw_put(w, 10, 4);
    bw_put(w, 1, 1);
    bw_signed(w, dx, 12);
    bw_signed(w, dy, 12);
}

static void rec_end(BitW *w) {
    bw_put(w, 0, 6);
    bw_align(w);
}

static void identity(MenuSwfRasterTransform *xf) {
    memset(xf, 0, sizeof *xf);
    xf->m00 = 1.0;
    xf->m11 = 1.0;
}

static void make_mesh(MenuSwfTessMesh *m, MenuSwfTessVertex *v,
                      uint32_t vc, uint32_t *idx, uint32_t ic) {
    memset(m, 0, sizeof *m);
    m->vertices = v;
    m->vertex_count = vc;
    m->indices = idx;
    m->index_count = ic;
}

static void target_init(MenuSwfRasterTarget *t, uint8_t *pixels, uint32_t w,
                        uint32_t h, uint32_t stride, int32_t advance) {
    memset(t, 0, sizeof *t);
    t->width = w;
    t->height = h;
    t->stride = stride;
    t->pixels = pixels;
    t->advance = advance;
}

static void test_square_triangle(void) {
    MenuSwfRasterTransform xf;
    MenuSwfRasterResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfTessVertex v[4];
    uint32_t idx[6];
    MenuSwfTessMesh m;
    uint8_t buf2[4], buf16[16 * 16];
    MenuSwfRasterTarget t;

    identity(&xf);
    v[0].x = 0; v[0].y = 0;
    v[1].x = 2; v[1].y = 0;
    v[2].x = 2; v[2].y = 2;
    v[3].x = 0; v[3].y = 2;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;
    make_mesh(&m, v, 4, idx, 6);
    target_init(&t, buf2, 2, 2, 2, 0);
    MenuSwfRasterStatus st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason,
                                                 sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "2x2 square raster");
    check(r.coverage_sum == 1020 && r.covered_pixels == 4 &&
              r.max_coverage == 255 && r.has_coverage,
          "2x2 square exact coverage");
    check(r.x0 == 0 && r.y0 == 0 && r.x1 == 2 && r.y1 == 2,
          "2x2 square used rect");
    check(buf2[0] == 255 && buf2[1] == 255 && buf2[2] == 255 &&
              buf2[3] == 255,
          "2x2 square pixels");

    v[0].x = 0; v[0].y = 0;
    v[1].x = 16; v[1].y = 0;
    v[2].x = 16; v[2].y = 16;
    v[3].x = 0; v[3].y = 16;
    target_init(&t, buf16, 16, 16, 16, 0);
    st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "16x16 square raster");
    check(r.coverage_sum == 16u * 16u * 255u &&
              r.covered_pixels == 16u * 16u && r.max_coverage == 255,
          "16x16 square exact coverage");
    check(r.x0 == 0 && r.y0 == 0 && r.x1 == 16 && r.y1 == 16,
          "16x16 square used rect");

    /* Half-square triangle: the sample grid sees 36 of 64 samples. */
    v[0].x = 0; v[0].y = 0;
    v[1].x = 2; v[1].y = 0;
    v[2].x = 0; v[2].y = 2;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    make_mesh(&m, v, 3, idx, 3);
    target_init(&t, buf2, 2, 2, 2, 0);
    st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "2x2 triangle raster");
    check(r.coverage_sum == 573 && r.covered_pixels == 3 &&
              r.max_coverage == 255,
          "2x2 triangle sample coverage");
    check(buf2[0] == 255, "triangle full corner pixel");
}

static void test_subpixel(void) {
    MenuSwfRasterTransform xf;
    MenuSwfRasterResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfTessVertex v[4] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    MenuSwfTessMesh m;
    uint8_t buf[4];
    MenuSwfRasterTarget t;

    identity(&xf);
    xf.tx = 0.5;
    make_mesh(&m, v, 4, idx, 6);
    target_init(&t, buf, 2, 2, 2, 0);
    MenuSwfRasterStatus st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason,
                                                 sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "subpixel square raster");
    check(r.coverage_sum == 766 && r.covered_pixels == 4 &&
              r.max_coverage == 255,
          "subpixel coverage sum");
    check(buf[0] == 128 && buf[1] == 255 && buf[2] == 128 && buf[3] == 255,
          "subpixel half-covered column");
}

static void test_rotation_mirror(void) {
    MenuSwfRasterTransform xf;
    MenuSwfRasterResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfTessVertex v[4] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    MenuSwfTessMesh m;
    uint8_t buf[4];
    MenuSwfRasterTarget t;
    make_mesh(&m, v, 4, idx, 6);

    memset(&xf, 0, sizeof xf);
    xf.m01 = -1.0;
    xf.m10 = 1.0;
    xf.tx = 2.0;
    target_init(&t, buf, 2, 2, 2, 0);
    MenuSwfRasterStatus st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason,
                                                 sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "90-degree rotation raster");
    check(r.coverage_sum == 1020 && r.covered_pixels == 4,
          "90-degree rotation coverage");

    memset(&xf, 0, sizeof xf);
    xf.m00 = -1.0;
    xf.m11 = -1.0;
    xf.tx = 2.0;
    xf.ty = 2.0;
    target_init(&t, buf, 2, 2, 2, 0);
    st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "negative-scale raster");
    check(r.coverage_sum == 1020 && r.covered_pixels == 4,
          "negative-scale coverage");
}

static void test_clip_and_empty(void) {
    MenuSwfRasterTransform xf;
    MenuSwfRasterResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfTessVertex v[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    MenuSwfTessMesh m;
    uint8_t guarded[10];
    MenuSwfRasterTarget t;

    identity(&xf);
    make_mesh(&m, v, 4, idx, 6);
    memset(guarded, 0xAA, sizeof guarded);
    /* stride 4 with width 2: column 2..3 is padding and must stay 0xAA. */
    target_init(&t, guarded + 2, 2, 2, 4, 0);
    MenuSwfRasterStatus st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason,
                                                 sizeof reason);
    check(st == MENU_SWF_RASTER_OK, "border-clipped raster");
    check(r.coverage_sum == 255 && r.covered_pixels == 1,
          "border-clipped coverage");
    check(r.x0 == 0 && r.y0 == 0 && r.x1 == 1 && r.y1 == 1,
          "border-clipped used rect");
    check(guarded[0] == 0xAA && guarded[1] == 0xAA &&
              guarded[4] == 0xAA && guarded[5] == 0xAA &&
              guarded[8] == 0xAA && guarded[9] == 0xAA,
          "guards and stride padding untouched");

    memset(&m, 0, sizeof m);
    uint8_t empty[16];
    memset(empty, 0x55, sizeof empty);
    target_init(&t, empty, 4, 4, 4, 321);
    st = menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason);
    check(st == MENU_SWF_RASTER_OK && r.coverage_sum == 0 &&
              !r.has_coverage && r.advance == 321,
          "empty mesh zero coverage with advance");
    int cleared = 1;
    for (unsigned i = 0; i < sizeof empty; ++i)
        if (empty[i] != 0) cleared = 0;
    check(cleared, "empty mesh clears buffer");
}

static size_t build_hole_swf(uint8_t *file, size_t cap) {
    uint8_t body[4096], tag[2048], records[512];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    memset(records, 0, sizeof records);
    BitW w = {records, sizeof records, 0, 1};
    bw_put(&w, 1, 4);
    bw_put(&w, 0, 4);
    rec_move(&w, 0, 0);
    rec_line(&w, 100, 0);
    rec_line(&w, 0, 100);
    rec_line(&w, -100, 0);
    rec_line(&w, 0, -100);
    rec_move(&w, 25, 25);
    rec_line(&w, 50, 0);
    rec_line(&w, 0, 50);
    rec_line(&w, -50, 0);
    rec_line(&w, 0, -50);
    rec_end(&w);
    size_t rlen = w.ok ? (w.bit + 7) / 8 : 0;
    size_t at = 0;
    put_u16(tag + at, 4);
    at += 2;
    uint8_t rb[32];
    size_t rn = rect_bytes(rb, sizeof rb, 0, 100, 0, 100);
    memcpy(tag + at, rb, rn);
    at += rn;
    memcpy(tag + at, fills, sizeof fills);
    at += sizeof fills;
    memcpy(tag + at, lines, sizeof lines);
    at += sizeof lines;
    memcpy(tag + at, records, rlen);
    at += rlen;

    size_t bat = 0;
    body[bat++] = 0;
    put_u16(body + bat, 0x1e00);
    bat += 2;
    put_u16(body + bat, 1);
    bat += 2;
    put_u16(body + bat, (uint16_t)((22u << 6) | at));
    bat += 2;
    memcpy(body + bat, tag, at);
    bat += at;
    put_u16(body + bat, 0);
    bat += 2;
    if (cap < 8 + bat) return 0;
    memcpy(file, "FWS", 3);
    file[3] = 10;
    put_u32(file + 4, (uint32_t)(8 + bat));
    memcpy(file + 8, body, bat);
    return 8 + bat;
}

typedef struct {
    MenuSwfTessVertex vertices[4096];
    uint32_t indices[12288];
    uint16_t fills[4096];
    MenuSwfTessLine lines[1024];
    MenuSwfTessEdge edges[4096];
    uint32_t active[4096];
    int32_t bands[8192];
} TessFixture;

static void tess_fixture_init(TessFixture *f, MenuSwfTessMesh *m) {
    memset(m, 0, sizeof *m);
    m->vertices = f->vertices;
    m->vertex_capacity = 4096;
    m->indices = f->indices;
    m->index_capacity = 12288;
    m->triangle_fills = f->fills;
    m->triangle_capacity = 4096;
    m->lines = f->lines;
    m->line_capacity = 1024;
    m->edges = f->edges;
    m->edge_capacity = 4096;
    m->active = f->active;
    m->active_capacity = 4096;
    m->bands = f->bands;
    m->band_capacity = 8192;
}

static void test_hole_rules(void) {
    static TessFixture fx;
    uint8_t file[8192];
    size_t flen = build_hole_swf(file, sizeof file);
    char reason[BSA_REASON_MAX];
    MenuSwfTessMesh mesh;
    uint8_t even_buf[32 * 32], non_buf[32 * 32];
    MenuSwfRasterTarget t;
    MenuSwfRasterResult re, rn;
    check(flen > 0, "hole fixture builds");

    MenuSwfRasterTransform xf;
    identity(&xf);
    xf.m00 = 0.32;
    xf.m11 = 0.32;

    tess_fixture_init(&fx, &mesh);
    MenuSwfStatus st = menu_swf_tess_shape(file, flen, 4,
                                           MENU_SWF_TESS_EVEN_ODD, &mesh,
                                           reason, sizeof reason);
    check(st == MENU_SWF_OK, "hole even-odd tess");
    target_init(&t, even_buf, 32, 32, 32, 0);
    MenuSwfRasterStatus rs = menu_swf_raster_mesh(&mesh, &xf, &t, &re,
                                                  reason, sizeof reason);
    check(rs == MENU_SWF_RASTER_OK, "hole even-odd raster");

    tess_fixture_init(&fx, &mesh);
    st = menu_swf_tess_shape(file, flen, 4, MENU_SWF_TESS_NON_ZERO, &mesh,
                             reason, sizeof reason);
    check(st == MENU_SWF_OK, "hole non-zero tess");
    target_init(&t, non_buf, 32, 32, 32, 0);
    rs = menu_swf_raster_mesh(&mesh, &xf, &t, &rn, reason, sizeof reason);
    check(rs == MENU_SWF_RASTER_OK, "hole non-zero raster");

    check(rn.coverage_sum > re.coverage_sum,
          "non-zero hole fills more than even-odd");
    check(re.coverage_sum > 0 && rn.coverage_sum > 0,
          "both hole rules produce coverage");
    check(even_buf[16 * 32 + 16] == 0,
          "even-odd hole center is empty");
    check(non_buf[16 * 32 + 16] != 0,
          "non-zero hole center is filled");
    check(re.x0 >= 0 && re.y0 >= 0 && re.x1 <= 32 && re.y1 <= 32 &&
              rn.x0 >= 0 && rn.y0 >= 0 && rn.x1 <= 32 && rn.y1 <= 32,
          "hole bounds stay in buffer");
}

static void test_refusals(void) {
    MenuSwfRasterTransform xf;
    MenuSwfRasterResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfTessVertex v[4] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    MenuSwfTessMesh m;
    uint8_t buf[4];
    MenuSwfRasterTarget t;

    identity(&xf);
    make_mesh(&m, v, 4, idx, 6);
    target_init(&t, buf, 2, 2, 2, 0);

    xf.m00 = NAN;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_NAN,
          "NaN transform refused");

    identity(&xf);
    xf.m00 = 1e9;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_OVERFLOW,
          "transform overflow refused");

    identity(&xf);
    xf.m00 = 0;
    xf.m11 = 0;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_SCALE,
          "zero determinant refused");

    identity(&xf);
    xf.m00 = -1.0;
    xf.tx = 2.0;
    target_init(&t, buf, 2, 2, 2, 0);
    t.reject_negative_scale = 1;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_NEGATIVE_SCALE,
          "negative scale refusal honored");

    identity(&xf);
    target_init(&t, buf, 0, 2, 2, 0);
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_TOO_SMALL,
          "zero width refused");

    target_init(&t, buf, 2, 2, 1, 0);
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_TOO_SMALL,
          "short stride refused");

    target_init(&t, NULL, 2, 2, 2, 0);
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_TOO_SMALL,
          "NULL pixels refused");

    target_init(&t, buf, 2, 2, 2, 0);
    make_mesh(&m, v, 4, idx, 4);
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_ARGUMENT,
          "non-triangle index count refused");

    make_mesh(&m, v, 4, idx, 6);
    idx[0] = 9;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_ARGUMENT,
          "out-of-range index refused");
    idx[0] = 0;

    target_init(&t, buf, MENU_SWF_RASTER_MAX_DIM + 1, 2,
                MENU_SWF_RASTER_MAX_DIM + 1, 0);
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_OVERFLOW,
          "oversize target refused");

    target_init(&t, buf, 2, 2, 2, 0);
    m.index_count = (MENU_SWF_RASTER_MAX_TRIANGLES + 1u) * 3u;
    check(menu_swf_raster_mesh(&m, &xf, &t, &r, reason, sizeof reason) ==
              MENU_SWF_RASTER_ERR_OVERFLOW,
          "triangle limit refused");
    m.index_count = 6;
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
    bsa_close(archive);
    if (!bytes) return;

    MenuSwfDisplaySummary *display =
        (MenuSwfDisplaySummary *)malloc(sizeof *display);
    check(display != NULL, "real display allocation");
    if (!display) {
        free(bytes);
        return;
    }
    MenuSwfStatus st = menu_swf_display_summary(bytes, got, display, reason,
                                                sizeof reason);
    check(st == MENU_SWF_OK, "real display summary");
    if (st == MENU_SWF_OK) {
        const MenuSwfFontSummary *font = NULL;
        for (uint32_t i = 0; i < display->stored_fonts; ++i)
            if (display->fonts[i].glyph_count == 243) {
                font = &display->fonts[i];
                break;
            }
        check(font != NULL, "real 243-glyph font found");
        if (font) {
            int32_t advance = 0;
            for (uint32_t i = 0; i < font->stored_glyphs; ++i)
                if (font->glyphs[i].code == 33 && font->glyphs[i].has_advance)
                    advance = font->glyphs[i].advance;
            static TessFixture fx;
            MenuSwfTessMesh mesh;
            tess_fixture_init(&fx, &mesh);
            st = menu_swf_tess_glyph(bytes, got, font->font_id, 33,
                                     MENU_SWF_TESS_NON_ZERO, &mesh, reason,
                                     sizeof reason);
            check(st == MENU_SWF_OK, "real code 33 glyph tess");
            if (st == MENU_SWF_OK) {
                double bw = mesh.bounds.xmax - mesh.bounds.xmin;
                double bh = mesh.bounds.ymax - mesh.bounds.ymin;
                const uint32_t sizes[2] = {32, 64};
                for (unsigned s = 0; s < 2; ++s) {
                    uint32_t dim = sizes[s];
                    uint8_t *pixels = (uint8_t *)malloc((size_t)dim * dim);
                    if (!pixels) continue;
                    double scale = (dim - 2.0) / bw;
                    double sy = (dim - 2.0) / bh;
                    if (sy < scale) scale = sy;
                    MenuSwfRasterTransform xf;
                    memset(&xf, 0, sizeof xf);
                    xf.m00 = scale;
                    xf.m11 = scale;
                    xf.tx = (dim - bw * scale) / 2.0 -
                            mesh.bounds.xmin * scale;
                    xf.ty = (dim - bh * scale) / 2.0 -
                            mesh.bounds.ymin * scale;
                    MenuSwfRasterTarget t;
                    target_init(&t, pixels, dim, dim, dim, advance);
                    MenuSwfRasterResult r;
                    MenuSwfRasterStatus rs = menu_swf_raster_mesh(
                        &mesh, &xf, &t, &r, reason, sizeof reason);
                    check(rs == MENU_SWF_RASTER_OK, "real glyph raster");
                    if (rs == MENU_SWF_RASTER_OK) {
                        printf("real: glyph 33 %ux%u sum=%llu covered=%u "
                               "bounds=%u,%u,%u,%u max=%u advance=%d "
                               "scale=%.6f\n",
                               dim, dim,
                               (unsigned long long)r.coverage_sum,
                               r.covered_pixels, r.x0, r.y0, r.x1, r.y1,
                               r.max_coverage, r.advance, scale);
                        check(r.coverage_sum > 0 && r.has_coverage,
                              "real glyph non-zero coverage");
                        check(r.coverage_sum <=
                                  (uint64_t)dim * dim * 255u,
                              "real glyph coverage in range");
                        check(r.x0 < r.x1 && r.y0 < r.y1 && r.x1 <= dim &&
                                  r.y1 <= dim,
                              "real glyph bounds in buffer");
                        check(r.advance == advance && advance > 0,
                              "real glyph advance echoed");
                    } else {
                        printf("real: raster failed: %s\n", reason);
                    }
                    free(pixels);
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
    printf("menu SWF raster tests\n");
    test_square_triangle();
    test_subpixel();
    test_rotation_mirror();
    test_clip_and_empty();
    test_hole_rules();
    test_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF raster: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
