/* Host test: bounded SWF shape tessellation into triangles and lines. */

#include "menu_swf_tess.h"
#include "menu_swf_shape.h"
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

static void rec_move(BitW *w, unsigned fill_bits, unsigned line_bits,
                     int32_t x, int32_t y, int has_fill0, uint16_t fill0,
                     int has_line, uint16_t line) {
    bw_put(w, 0, 1);          /* non-edge */
    bw_put(w, 0, 1);          /* no new styles */
    bw_put(w, has_line, 1);   /* line style */
    bw_put(w, 0, 1);          /* fill1 */
    bw_put(w, has_fill0, 1);  /* fill0 */
    bw_put(w, 1, 1);          /* move */
    bw_put(w, 10, 5);
    bw_signed(w, x, 10);
    bw_signed(w, y, 10);
    if (has_fill0) bw_put(w, fill0, fill_bits);
    if (has_line) bw_put(w, line, line_bits);
}

static void rec_line(BitW *w, int32_t dx, int32_t dy) {
    bw_put(w, 1, 1);  /* edge */
    bw_put(w, 1, 1);  /* straight */
    bw_put(w, 10, 4);
    bw_put(w, 1, 1);  /* general */
    bw_signed(w, dx, 12);
    bw_signed(w, dy, 12);
}

static void rec_curve(BitW *w, int32_t cdx, int32_t cdy, int32_t adx,
                      int32_t ady) {
    bw_put(w, 1, 1); /* edge */
    bw_put(w, 0, 1); /* curved */
    bw_put(w, 10, 4);
    bw_signed(w, cdx, 12);
    bw_signed(w, cdy, 12);
    bw_signed(w, adx, 12);
    bw_signed(w, ady, 12);
}

static void rec_end(BitW *w) {
    bw_put(w, 0, 6);
    bw_align(w);
}

static size_t finish_records(BitW *w) {
    return w->ok ? (w->bit + 7) / 8 : 0;
}

static size_t build_shape(uint8_t *out, size_t cap, uint16_t shape_id,
                          const uint8_t *fills, size_t fill_len,
                          const uint8_t *lines, size_t line_len,
                          const uint8_t *records, size_t records_len) {
    if (cap < 64) return 0;
    size_t at = 0;
    put_u16(out + at, shape_id);
    at += 2;
    uint8_t r[32];
    size_t n = rect_bytes(r, sizeof r, 0, 1000, 0, 1000);
    memcpy(out + at, r, n);
    at += n;
    memcpy(out + at, fills, fill_len);
    at += fill_len;
    memcpy(out + at, lines, line_len);
    at += line_len;
    memcpy(out + at, records, records_len);
    at += records_len;
    return at;
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at || code > 1023) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        put_u32(out + at, len);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t make_swf(uint8_t *out, size_t cap, unsigned code,
                       const uint8_t *payload, uint32_t len) {
    uint8_t body[8192];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, code, payload, len);
    at = put_tag(body, sizeof body, at, 0, NULL, 0);
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

#define FIX_VERTICES 65536u
#define FIX_INDICES 196608u
#define FIX_TRIANGLES 65536u
#define FIX_LINES 65536u
#define FIX_EDGES 65536u
#define FIX_ACTIVE 65536u
#define FIX_BANDS 131072u

typedef struct {
    MenuSwfTessVertex vertices[FIX_VERTICES];
    uint32_t indices[FIX_INDICES];
    uint16_t fills[FIX_TRIANGLES];
    MenuSwfTessLine lines[FIX_LINES];
    MenuSwfTessEdge edges[FIX_EDGES];
    uint32_t active[FIX_ACTIVE];
    int32_t bands[FIX_BANDS];
} MeshFixture;

static MeshFixture fixture;

static void fixture_init(MenuSwfTessMesh *m) {
    memset(m, 0, sizeof *m);
    m->vertices = fixture.vertices;
    m->vertex_capacity = FIX_VERTICES;
    m->indices = fixture.indices;
    m->index_capacity = FIX_INDICES;
    m->triangle_fills = fixture.fills;
    m->triangle_capacity = FIX_TRIANGLES;
    m->lines = fixture.lines;
    m->line_capacity = FIX_LINES;
    m->edges = fixture.edges;
    m->edge_capacity = FIX_EDGES;
    m->active = fixture.active;
    m->active_capacity = FIX_ACTIVE;
    m->bands = fixture.bands;
    m->band_capacity = FIX_BANDS;
}

static int64_t mesh_area2(const MenuSwfTessMesh *m) {
    int64_t sum = 0;
    for (uint32_t t = 0; t < m->triangle_count; ++t) {
        const MenuSwfTessVertex *a =
            &m->vertices[m->indices[t * 3 + 0]];
        const MenuSwfTessVertex *b =
            &m->vertices[m->indices[t * 3 + 1]];
        const MenuSwfTessVertex *c =
            &m->vertices[m->indices[t * 3 + 2]];
        sum += ((int64_t)b->x - a->x) * ((int64_t)c->y - a->y) -
               ((int64_t)b->y - a->y) * ((int64_t)c->x - a->x);
    }
    return sum;
}

static size_t square_records(uint8_t *out, size_t cap, int ccw,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint16_t fill0, uint16_t fill1, uint16_t line,
                             unsigned fill_bits, unsigned line_bits) {
    memset(out, 0, cap);
    BitW bw = {out, cap, 0, 1};
    bw_put(&bw, fill_bits, 4);
    bw_put(&bw, line_bits, 4);
    rec_move(&bw, fill_bits, line_bits, x, y, fill0 != 0, fill0,
             line != 0, line);
    if (!ccw) {
        rec_line(&bw, w, 0);
        rec_line(&bw, 0, h);
        rec_line(&bw, -w, 0);
        rec_line(&bw, 0, -h);
    } else {
        rec_line(&bw, 0, h);
        rec_line(&bw, w, 0);
        rec_line(&bw, 0, -h);
        rec_line(&bw, -w, 0);
    }
    rec_end(&bw);
    (void)fill1;
    return finish_records(&bw);
}

static void expect_status(MenuSwfStatus got, MenuSwfStatus want,
                          const char *what) {
    check(got == want, what);
}

static void test_square(void) {
    uint8_t file[8192], tag[4096], records[512];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    for (int ccw = 0; ccw < 2; ++ccw) {
        size_t rlen = square_records(records, sizeof records, ccw, 0, 0, 100,
                                     100, 1, 0, 0, 1, 0);
        size_t tlen = build_shape(tag, sizeof tag, 2, fills, sizeof fills,
                                  lines, sizeof lines, records, rlen);
        size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
        fixture_init(&m);
        MenuSwfStatus st = menu_swf_tess_shape(
            file, n, 2, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
        expect_status(st, MENU_SWF_OK, ccw ? "square ccw tess" : "square cw tess");
        check(m.vertex_count == 4 && m.triangle_count == 2 &&
                  m.index_count == 6,
              ccw ? "square ccw counts" : "square cw counts");
        check(m.line_count == 0, "square has no stroke lines");
        check(m.has_bounds && m.bounds.xmin == 0 && m.bounds.xmax == 100 &&
                  m.bounds.ymin == 0 && m.bounds.ymax == 100,
              "square bounds");
        check(m.triangle_fills[0] == 1 && m.triangle_fills[1] == 1,
              "square fill style");
        check(mesh_area2(&m) == 20000, "square area");
    }

    /* A stroked square keeps the same fill and emits four separate lines. */
    uint8_t lines2[] = {1, 0x14, 0, 0, 0, 0};
    size_t rlen = square_records(records, sizeof records, 0, 0, 0, 100, 100,
                                 1, 0, 1, 1, 1);
    size_t tlen = build_shape(tag, sizeof tag, 3, fills, sizeof fills, lines2,
                              sizeof lines2, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 3, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "stroked square tess");
    check(m.vertex_count == 4 && m.triangle_count == 2, "stroked square fill");
    check(m.line_count == 4, "stroked square line count");
    check(m.lines[0].line_style == 1, "stroked square line style");
    check(m.lines[0].x0 == 0 && m.lines[0].y0 == 0 &&
              m.lines[0].x1 == 100 && m.lines[0].y1 == 0,
          "stroked square first line");
}

static void test_hole(void) {
    uint8_t file[8192], tag[4096], records[1024];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    memset(records, 0, sizeof records);
    BitW bw = {records, sizeof records, 0, 1};
    bw_put(&bw, 1, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 1, 0, 0, 0, 1, 1, 0, 0);
    rec_line(&bw, 100, 0);
    rec_line(&bw, 0, 100);
    rec_line(&bw, -100, 0);
    rec_line(&bw, 0, -100);
    rec_move(&bw, 1, 0, 25, 25, 1, 1, 0, 0);
    rec_line(&bw, 50, 0);
    rec_line(&bw, 0, 50);
    rec_line(&bw, -50, 0);
    rec_line(&bw, 0, -50);
    rec_end(&bw);
    size_t rlen = finish_records(&bw);
    size_t tlen = build_shape(tag, sizeof tag, 4, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 4, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "hole tess");
    check(m.contour_count == 2, "hole contour count");
    check(m.vertex_count == 16 && m.triangle_count == 8, "hole counts");
    check(mesh_area2(&m) == 15000, "hole even-odd area");
    check(m.bounds.xmin == 0 && m.bounds.xmax == 100 &&
              m.bounds.ymin == 0 && m.bounds.ymax == 100,
          "hole bounds");
}

static void test_nonzero(void) {
    uint8_t file[8192], tag[4096], records[1024];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    static const int32_t star[10][2] = {
        {0, -100},  {23, -32},  {95, -31}, {37, 12},  {59, 81},
        {0, 32},    {-59, 81},  {-37, 12}, {-95, -31}, {-23, -32}};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    memset(records, 0, sizeof records);
    BitW bw = {records, sizeof records, 0, 1};
    bw_put(&bw, 1, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 1, 0, star[0][0], star[0][1], 1, 1, 0, 0);
    for (unsigned i = 1; i < 10; ++i)
        rec_line(&bw, star[i][0] - star[i - 1][0],
                 star[i][1] - star[i - 1][1]);
    rec_line(&bw, -star[9][0], -star[9][1]);
    rec_end(&bw);
    size_t rlen = finish_records(&bw);
    size_t tlen = build_shape(tag, sizeof tag, 5, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 5, MENU_SWF_TESS_NON_ZERO, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "star non-zero tess");
    check(m.contour_count == 1, "star contour count");
    check(m.vertex_count > 0 && m.triangle_count > 0, "star triangles");
    check(mesh_area2(&m) > 0, "star positive area");
    check(m.bounds.xmin <= -95 && m.bounds.xmax >= 95 &&
              m.bounds.ymin <= -100 && m.bounds.ymax >= 81,
          "star bounds");

    /* Same-direction concentric contours: non-zero fills the inner region. */
    memset(records, 0, sizeof records);
    bw = (BitW){records, sizeof records, 0, 1};
    bw_put(&bw, 1, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 1, 0, 0, 0, 1, 1, 0, 0);
    rec_line(&bw, 100, 0);
    rec_line(&bw, 0, 100);
    rec_line(&bw, -100, 0);
    rec_line(&bw, 0, -100);
    rec_move(&bw, 1, 0, 25, 25, 1, 1, 0, 0);
    rec_line(&bw, 50, 0);
    rec_line(&bw, 0, 50);
    rec_line(&bw, -50, 0);
    rec_line(&bw, 0, -50);
    rec_end(&bw);
    rlen = finish_records(&bw);
    tlen = build_shape(tag, sizeof tag, 6, fills, sizeof fills, lines,
                       sizeof lines, records, rlen);
    n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    st = menu_swf_tess_shape(file, n, 6, MENU_SWF_TESS_NON_ZERO, &m, reason,
                             sizeof reason);
    expect_status(st, MENU_SWF_OK, "non-zero nested tess");
    check(mesh_area2(&m) == 20000, "non-zero fills same-direction hole");
}

static void test_curve(void) {
    uint8_t file[8192], tag[4096], records[1024];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    memset(records, 0, sizeof records);
    BitW bw = {records, sizeof records, 0, 1};
    bw_put(&bw, 1, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 1, 0, 0, 0, 1, 1, 0, 0);
    rec_line(&bw, 100, 0);
    rec_curve(&bw, 100, 50, -100, 100);
    rec_line(&bw, -100, 0);
    rec_end(&bw);
    size_t rlen = finish_records(&bw);
    size_t tlen = build_shape(tag, sizeof tag, 7, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 7, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "curve tess");
    check(m.curve_segments > 1, "curve flattened segments");
    check(m.vertex_count > 4 && m.triangle_count > 2, "curve triangle growth");
    check(m.bounds.xmax > 100, "curve control point widens bounds");
    check(mesh_area2(&m) > 0, "curve positive area");
}

static void test_styles(void) {
    uint8_t file[8192], tag[4096], records[1024];
    uint8_t fills[] = {2, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    memset(records, 0, sizeof records);
    BitW bw = {records, sizeof records, 0, 1};
    bw_put(&bw, 2, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 2, 0, 0, 0, 1, 1, 0, 0);
    rec_line(&bw, 50, 0);
    rec_line(&bw, 0, 50);
    rec_line(&bw, -50, 0);
    rec_line(&bw, 0, -50);
    rec_move(&bw, 2, 0, 100, 0, 1, 2, 0, 0);
    rec_line(&bw, 50, 0);
    rec_line(&bw, 0, 50);
    rec_line(&bw, -50, 0);
    rec_line(&bw, 0, -50);
    rec_end(&bw);
    size_t rlen = finish_records(&bw);
    size_t tlen = build_shape(tag, sizeof tag, 8, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 8, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "multi-style tess");
    check(m.contour_count == 2, "multi-style contours");
    check(m.triangle_count == 4, "multi-style triangles");
    int seen1 = 0, seen2 = 0;
    for (uint32_t i = 0; i < m.triangle_count; ++i) {
        if (m.triangle_fills[i] == 1) ++seen1;
        if (m.triangle_fills[i] == 2) ++seen2;
    }
    check(seen1 == 2 && seen2 == 2, "multi-style fill indices");
}

static void test_empty(void) {
    uint8_t file[8192], tag[4096], records[128];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    memset(records, 0, sizeof records);
    BitW bw = {records, sizeof records, 0, 1};
    bw_put(&bw, 1, 4);
    bw_put(&bw, 0, 4);
    rec_move(&bw, 1, 0, 10, 10, 1, 1, 0, 0);
    rec_end(&bw);
    size_t rlen = finish_records(&bw);
    size_t tlen = build_shape(tag, sizeof tag, 9, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);
    fixture_init(&m);
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 9, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_OK, "empty contour accepted");
    check(m.contour_count == 1, "empty contour count");
    check(m.vertex_count == 0 && m.triangle_count == 0, "empty contour output");
    check(m.has_bounds && m.bounds.xmin == 10 && m.bounds.ymin == 10,
          "empty contour bounds");
}

static void test_refusals(void) {
    uint8_t file[8192], tag[4096], records[512];
    uint8_t fills[] = {1, 0, 0, 0, 0};
    uint8_t lines[] = {0};
    MenuSwfTessMesh m;
    char reason[BSA_REASON_MAX];

    size_t rlen = square_records(records, sizeof records, 0, 0, 0, 100, 100,
                                 1, 0, 0, 1, 0);
    size_t tlen = build_shape(tag, sizeof tag, 10, fills, sizeof fills, lines,
                              sizeof lines, records, rlen);
    size_t n = make_swf(file, sizeof file, 22, tag, (uint32_t)tlen);

    fixture_init(&m);
    m.vertex_capacity = 3;
    MenuSwfStatus st = menu_swf_tess_shape(
        file, n, 10, MENU_SWF_TESS_EVEN_ODD, &m, reason, sizeof reason);
    expect_status(st, MENU_SWF_ERR_BUDGET, "vertex overflow refused");
    check(reason[0] != 0, "vertex overflow reason");

    fixture_init(&m);
    m.index_capacity = 5;
    st = menu_swf_tess_shape(file, n, 10, MENU_SWF_TESS_EVEN_ODD, &m, reason,
                             sizeof reason);
    expect_status(st, MENU_SWF_ERR_BUDGET, "index overflow refused");

    fixture_init(&m);
    m.edge_capacity = 1;
    st = menu_swf_tess_shape(file, n, 10, MENU_SWF_TESS_EVEN_ODD, &m, reason,
                             sizeof reason);
    expect_status(st, MENU_SWF_ERR_BUDGET, "edge overflow refused");

    fixture_init(&m);
    st = menu_swf_tess_shape(file, n, 99, MENU_SWF_TESS_EVEN_ODD, &m, reason,
                             sizeof reason);
    expect_status(st, MENU_SWF_ERR_ARGUMENT, "missing shape refused");
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

    MenuSwfShapeResult *summary =
        (MenuSwfShapeResult *)malloc(sizeof *summary);
    MenuSwfTessMesh m;
    char why[BSA_REASON_MAX];
    check(summary != NULL, "real summary allocation");
    if (!summary) {
        free(bytes);
        return;
    }
    MenuSwfStatus st = menu_swf_shape_summary(bytes, got, summary, why,
                                              sizeof why);
    check(st == MENU_SWF_OK, "real shape summary for tess");
    if (st == MENU_SWF_OK) {
        uint32_t tessellated = 0, total_vertices = 0, total_triangles = 0;
        uint32_t total_lines = 0, total_curves = 0;
        MenuSwfShapeRect shape2_bounds = {0, 0, 0, 0};
        int have_shape2 = 0;
        for (uint32_t i = 0; i < summary->stored_shapes; ++i) {
            fixture_init(&m);
            st = menu_swf_tess_shape(bytes, got, summary->shapes[i].shape_id,
                                     MENU_SWF_TESS_EVEN_ODD, &m, why,
                                     sizeof why);
            if (st != MENU_SWF_OK) {
                printf("real: shape %u tess failed: %s (%s)\n",
                       summary->shapes[i].shape_id,
                       menu_swf_status_name(st), why);
                break;
            }
            ++tessellated;
            total_vertices += m.vertex_count;
            total_triangles += m.triangle_count;
            total_lines += m.line_count;
            total_curves += m.curve_segments;
            if (summary->shapes[i].shape_id == 2) {
                shape2_bounds = m.bounds;
                have_shape2 = m.has_bounds;
            }
        }
        printf("real: tess shapes=%u/%u vertices=%u triangles=%u lines=%u "
               "curve_segments=%u\n",
               tessellated, summary->stored_shapes, total_vertices,
               total_triangles, total_lines, total_curves);
        if (have_shape2)
            printf("real: shape2 tess bounds=%d,%d,%d,%d\n",
                   shape2_bounds.xmin, shape2_bounds.xmax, shape2_bounds.ymin,
                   shape2_bounds.ymax);
        check(tessellated == 27, "real 27 shapes tessellated");
        check(total_vertices > 0 && total_triangles > 0,
              "real tessellation non-empty");
        check(have_shape2, "real shape2 tess bounds present");
        if (have_shape2) {
            check(shape2_bounds.xmin == 0 && shape2_bounds.xmax == 522 &&
                      shape2_bounds.ymin == 0 && shape2_bounds.ymax == 520,
                  "real shape2 tess bounds 0,522,0,520");
        }
    } else {
        printf("real: shape summary failed: %s\n", why);
    }
    free(summary);
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF tess tests\n");
    test_square();
    test_hole();
    test_nonzero();
    test_curve();
    test_styles();
    test_empty();
    test_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF tess: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
