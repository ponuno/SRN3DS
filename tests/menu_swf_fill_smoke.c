/* Host test: SWF fill-style metadata and solid-fill vertex colours. */

#include "menu_swf_batch.h"
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

static void rec_end(BitW *w) {
    bw_put(w, 0, 6);
    bw_align(w);
}

static size_t build_records(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 4, 4); /* NumFillBits */
    bw_put(&w, 4, 4); /* NumLineBits */
    rec_move(&w, 4, 4, 0, 0, 1, 1, 0, 0);
    rec_line(&w, 100, 0);
    rec_line(&w, 0, 100);
    rec_line(&w, -100, -100);
    rec_end(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
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
    size_t n = rect_bytes(r, sizeof r, 0, 2000, 0, 2000);
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

typedef struct {
    MenuSwfTessVertex vertices[MENU_SWF_TESS_MAX_VERTICES];
    uint32_t indices[MENU_SWF_TESS_MAX_INDICES];
    uint16_t tri_fills[MENU_SWF_TESS_MAX_TRIANGLES];
    MenuSwfTessLine lines[MENU_SWF_TESS_MAX_LINES];
    MenuSwfTessEdge edges[MENU_SWF_TESS_MAX_EDGES];
    uint32_t active[MENU_SWF_TESS_MAX_EDGES];
    int32_t bands[MENU_SWF_TESS_MAX_BANDS];
    MenuSwfTessFill fills[MENU_SWF_SHAPE_MAX_STYLES];
} MeshFixture;

static MeshFixture fixture;

static void fixture_init(MenuSwfTessMesh *m) {
    memset(m, 0, sizeof *m);
    m->vertices = fixture.vertices;
    m->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
    m->indices = fixture.indices;
    m->index_capacity = MENU_SWF_TESS_MAX_INDICES;
    m->triangle_fills = fixture.tri_fills;
    m->triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
    m->lines = fixture.lines;
    m->line_capacity = MENU_SWF_TESS_MAX_LINES;
    m->edges = fixture.edges;
    m->edge_capacity = MENU_SWF_TESS_MAX_EDGES;
    m->active = fixture.active;
    m->active_capacity = MENU_SWF_TESS_MAX_EDGES;
    m->bands = fixture.bands;
    m->band_capacity = MENU_SWF_TESS_MAX_BANDS;
    m->fills = fixture.fills;
    m->fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
}

static void identity_style(MenuSwfBatchShapeStyle *s) {
    memset(s, 0, sizeof *s);
    s->m00 = s->m11 = 0x10000;
    s->mult[0] = s->mult[1] = s->mult[2] = s->mult[3] = 256;
    s->u = 0x10000;
    s->v = 0x20000;
}

static size_t build_synthetic(uint8_t *swf, size_t cap, const uint8_t *fills,
                              size_t fill_len) {
    uint8_t records[64], shape[512];
    size_t rlen = build_records(records, sizeof records);
    uint8_t lines[] = {0};
    size_t slen = build_shape(shape, sizeof shape, 42, fills, fill_len, lines,
                              sizeof lines, records, rlen);
    return make_swf(swf, cap, 32, shape, (uint32_t)slen);
}

static void test_solid_fill(void) {
    uint8_t fills[] = {1, 0x00, 0x40, 0x20, 0x10, 0x80};
    uint8_t swf[2048];
    size_t swf_len = build_synthetic(swf, sizeof swf, fills, sizeof fills);
    check(swf_len > 0, "solid SWF builds");
    char reason[BSA_REASON_MAX];

    static MenuSwfShapeResult result;
    MenuSwfStatus st = menu_swf_shape_summary(swf, swf_len, &result, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "solid shape summary");
    if (st != MENU_SWF_OK) printf("solid summary failed: %s\n", reason);
    const MenuSwfShapeSummary *shape = menu_swf_shape_find(&result, 42);
    check(shape != NULL, "solid shape found");
    if (!shape) return;
    check(shape->fill_count == 1 && shape->stored_fills == 1 &&
              shape->solid_fills == 1 && shape->gradient_fills == 0 &&
              shape->bitmap_fills == 0,
          "solid fill counted");
    const MenuSwfShapeFill *fill = menu_swf_shape_fill(&result, shape, 1);
    check(fill != NULL, "solid fill metadata present");
    if (fill) {
        check(fill->kind == MENU_SWF_FILL_SOLID && fill->rgba == 0x80402010u &&
                  fill->has_alpha,
              "solid fill RGBA 80402010");
    }

    MenuSwfTessMesh mesh;
    fixture_init(&mesh);
    st = menu_swf_tess_shape(swf, swf_len, 42, MENU_SWF_TESS_EVEN_ODD, &mesh,
                             reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.triangle_count == 1,
          "solid shape tessellates to one triangle");
    check(mesh.fill_count == 1 && mesh.fills[0].rgba == 0x80402010u,
          "tess mesh carries fill metadata");
    if (st != MENU_SWF_OK || mesh.triangle_count != 1) return;

    MenuSwfBatchShapeStyle style;
    identity_style(&style);
    MenuSwfBatchVertex vertices[3];
    uint32_t indices[3];
    MenuSwfBatchResult br;
    MenuSwfBatchStatus bs = menu_swf_batch_shape(
        &mesh, &style, NULL, vertices, 3, indices, 3, &br, reason,
        sizeof reason);
    check(bs == MENU_SWF_BATCH_OK && br.vertex_count == 3 &&
              br.index_count == 3 && br.triangle_count == 1,
          "solid batch is one triangle");
    if (bs == MENU_SWF_BATCH_OK) {
        check(vertices[0].color == 0x80402010u &&
                  vertices[1].color == 0x80402010u &&
                  vertices[2].color == 0x80402010u,
              "solid fill reaches every vertex");
        check(vertices[0].u == 0x10000 && vertices[0].v == 0x20000,
              "shape batch writes white-texel UV");
        check(indices[0] == 0 && indices[1] == 1 && indices[2] == 2,
              "shape batch indices are local");
    }

    /* CXFORM: mult 8.8 and signed add. */
    style.mult[0] = 512;
    style.mult[1] = 256;
    style.mult[2] = 128;
    style.mult[3] = 256;
    style.add[0] = 10;
    style.add[1] = 0;
    style.add[2] = -5;
    style.add[3] = 0;
    bs = menu_swf_batch_shape(&mesh, &style, NULL, vertices, 3, indices, 3,
                              &br, reason, sizeof reason);
    check(bs == MENU_SWF_BATCH_OK && vertices[0].color == 0x808a2003u,
          "solid fill applies record colour transform");

    /* A mirroring matrix is allowed and reverses index winding. */
    identity_style(&style);
    style.m00 = -0x10000;
    bs = menu_swf_batch_shape(&mesh, &style, NULL, vertices, 3, indices, 3,
                              &br, reason, sizeof reason);
    check(bs == MENU_SWF_BATCH_OK && indices[0] == 0 && indices[1] == 2 &&
              indices[2] == 1,
          "negative determinant mirrors and reverses winding");
    identity_style(&style);
    style.mult[0] = 512;
    style.mult[1] = 256;
    style.mult[2] = 128;
    style.mult[3] = 256;
    style.add[0] = 10;
    style.add[1] = 0;
    style.add[2] = -5;
    style.add[3] = 0;

    /* A short output must leave the buffers untouched. */
    memset(vertices, 0x5A, sizeof vertices);
    bs = menu_swf_batch_shape(&mesh, &style, NULL, vertices, 2, indices, 3,
                              &br, reason, sizeof reason);
    check(bs == MENU_SWF_BATCH_ERR_OVERFLOW && vertices[0].color == 0x5A5A5A5Au,
          "short shape batch refused before writing");
}

static void test_gradient_refusal(void) {
    uint8_t linear[] = {1, 0x10, 0x00, 0x02, 0, 255, 0, 0, 255,
                        255, 0, 0, 255, 255};
    uint8_t radial[] = {1, 0x12, 0x00, 0x02, 0, 10, 20, 30, 40,
                        128, 50, 60, 70, 80};
    uint8_t swf[2048];
    char reason[BSA_REASON_MAX];
    const uint8_t *cases[2] = {linear, radial};
    size_t lengths[2] = {sizeof linear, sizeof radial};
    const MenuSwfFillKind kinds[2] = {MENU_SWF_FILL_LINEAR,
                                      MENU_SWF_FILL_RADIAL};
    for (unsigned c = 0; c < 2; ++c) {
        size_t swf_len = build_synthetic(swf, sizeof swf, cases[c],
                                         lengths[c]);
        static MenuSwfShapeResult result;
        MenuSwfStatus st = menu_swf_shape_summary(swf, swf_len, &result,
                                                  reason, sizeof reason);
        check(st == MENU_SWF_OK, "gradient shape summary");
        const MenuSwfShapeSummary *shape = menu_swf_shape_find(&result, 42);
        check(shape && shape->gradient_fills == 1 &&
                  (c == 0 ? shape->linear_fills == 1 : shape->radial_fills == 1),
              "gradient fill counted by kind");
        const MenuSwfShapeFill *fill =
            shape ? menu_swf_shape_fill(&result, shape, 1) : NULL;
        check(fill && fill->kind == kinds[c] && fill->stop_total == 2 &&
                  fill->stop_count == 2,
              "gradient stops/matrix retained");
        if (!fill) continue;
        check(fill->stops[1].ratio == 255 || fill->stops[1].ratio == 128,
              "gradient stop ratio");

        MenuSwfTessMesh mesh;
        fixture_init(&mesh);
        st = menu_swf_tess_shape(swf, swf_len, 42, MENU_SWF_TESS_EVEN_ODD,
                                 &mesh, reason, sizeof reason);
        check(st == MENU_SWF_OK && mesh.triangle_count == 1 &&
                  mesh.fill_count == 1,
              "gradient shape tessellates");
        if (st != MENU_SWF_OK) continue;
        MenuSwfBatchShapeStyle style;
        identity_style(&style);
        MenuSwfBatchVertex vertices[3];
        uint32_t indices[3];
        MenuSwfBatchResult br;
        MenuSwfBatchStatus bs = menu_swf_batch_shape(
            &mesh, &style, NULL, vertices, 3, indices, 3, &br, reason,
            sizeof reason);
        check(bs == MENU_SWF_BATCH_ERR_FILL &&
                  strstr(reason, "gradient fills are not implemented"),
              "gradient batch refusal is named");
    }
}

static void test_bitmap_refusal(void) {
    uint8_t fills[] = {1, 0x40, 0x07, 0x00, 0x00};
    uint8_t swf[2048];
    size_t swf_len = build_synthetic(swf, sizeof swf, fills, sizeof fills);
    char reason[BSA_REASON_MAX];
    static MenuSwfShapeResult result;
    MenuSwfStatus st = menu_swf_shape_summary(swf, swf_len, &result, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "bitmap shape summary");
    const MenuSwfShapeSummary *shape = menu_swf_shape_find(&result, 42);
    check(shape && shape->bitmap_fills == 1 && shape->solid_fills == 0,
          "bitmap fill counted");
    const MenuSwfShapeFill *fill =
        shape ? menu_swf_shape_fill(&result, shape, 1) : NULL;
    check(fill && fill->kind == MENU_SWF_FILL_BITMAP &&
              fill->bitmap_id == 7 && fill->bitmap_repeat == 1,
          "bitmap fill keeps character id/matrix");

    MenuSwfTessMesh mesh;
    fixture_init(&mesh);
    st = menu_swf_tess_shape(swf, swf_len, 42, MENU_SWF_TESS_EVEN_ODD, &mesh,
                             reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.triangle_count == 1 &&
              mesh.fill_count == 1,
          "bitmap shape tessellates");
    if (st != MENU_SWF_OK) return;
    MenuSwfBatchShapeStyle style;
    identity_style(&style);
    MenuSwfBatchVertex vertices[3];
    uint32_t indices[3];
    MenuSwfBatchResult br;
    MenuSwfBatchStatus bs = menu_swf_batch_shape(
        &mesh, &style, NULL, vertices, 3, indices, 3, &br, reason,
        sizeof reason);
    check(bs == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "bitmap fills are not implemented"),
          "bitmap batch refusal is named");
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

    static MenuSwfShapeResult result;
    MenuSwfStatus st = menu_swf_shape_summary(bytes, got, &result, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "real fill summary");
    if (st != MENU_SWF_OK) {
        printf("real: fill summary failed: %s\n", reason);
        free(bytes);
        return;
    }
    unsigned solid = 0, linear = 0, radial = 0, focal = 0, bitmap = 0;
    unsigned mismatched = 0;
    const MenuSwfShapeSummary *top = NULL;
    for (uint32_t i = 0; i < result.stored_shapes; ++i) {
        const MenuSwfShapeSummary *s = &result.shapes[i];
        solid += s->solid_fills;
        linear += s->linear_fills;
        radial += s->radial_fills;
        focal += s->focal_fills;
        bitmap += s->bitmap_fills;
        if (s->stored_fills != s->fill_count) ++mismatched;
        if (!top || s->fill_count > top->fill_count) top = s;
    }
    printf("real: fills solid=%u linear=%u radial=%u focal=%u bitmap=%u "
           "stored_fills=%u\n",
           solid, linear, radial, focal, bitmap, result.shape_fill_used);
    check(solid == 40 && linear == 0 && radial == 0 && focal == 0 &&
              bitmap == 0,
          "real StartMenu has 40 solid fills and no gradients/bitmaps");
    check(result.shape_fill_used >= 40 && result.shape_fill_used >= solid,
          "real fill table holds all retained styles");
    check(mismatched == 0, "every real shape retains its fill array");

    const MenuSwfShapeSummary *shape2 = menu_swf_shape_find(&result, 2);
    if (shape2) {
        printf("real: shape2 fills=%u stored=%u first_colors=",
               shape2->fill_count, shape2->stored_fills);
        for (uint16_t i = 1; i <= shape2->stored_fills; ++i) {
            const MenuSwfShapeFill *f =
                menu_swf_shape_fill(&result, shape2, i);
            if (f) printf("%08x%s", f->rgba, i == shape2->stored_fills ? "" : ",");
        }
        printf("\n");
        check(shape2->solid_fills == shape2->fill_count,
              "real shape2 fills are solid");
        const MenuSwfShapeFill *f2 =
            menu_swf_shape_fill(&result, shape2, 1);
        check(shape2->stored_fills == 1 && f2 &&
                  f2->kind == MENU_SWF_FILL_SOLID && f2->rgba == 0xff000000u,
              "real shape2 is solid black");
    }
    if (top) {
        printf("real: top_fill_shape=%u fills=%u stored=%u colors=",
               top->shape_id, top->fill_count, top->stored_fills);
        for (uint16_t i = 1; i <= top->stored_fills; ++i) {
            const MenuSwfShapeFill *f =
                menu_swf_shape_fill(&result, top, i);
            if (f) printf("%08x%s", f->rgba,
                          i == top->stored_fills ? "" : ",");
        }
        printf("\n");
    }

    /* Tessellate+batch shape 2 and verify the vertices use its real colours. */
    if (shape2 && shape2->stored_fills) {
        MenuSwfTessMesh mesh;
        fixture_init(&mesh);
        st = menu_swf_tess_shape(bytes, got, 2, MENU_SWF_TESS_EVEN_ODD, &mesh,
                                 reason, sizeof reason);
        check(st == MENU_SWF_OK && mesh.triangle_count > 0,
              "real shape2 tessellates");
        if (st == MENU_SWF_OK && mesh.triangle_count > 0) {
            uint32_t n = mesh.triangle_count * 3u;
            MenuSwfBatchVertex *vertices =
                (MenuSwfBatchVertex *)malloc(n * sizeof *vertices);
            uint32_t *indices = (uint32_t *)malloc(n * sizeof *indices);
            MenuSwfBatchShapeStyle style;
            identity_style(&style);
            MenuSwfBatchResult br;
            MenuSwfBatchStatus bs =
                vertices && indices
                    ? menu_swf_batch_shape(&mesh, &style, NULL, vertices, n,
                                           indices, n, &br, reason,
                                           sizeof reason)
                    : MENU_SWF_BATCH_ERR_ARGUMENT;
            check(bs == MENU_SWF_BATCH_OK && br.triangle_count > 0,
                  "real shape2 solid batch");
            if (bs == MENU_SWF_BATCH_OK) {
                unsigned non_white = 0, matched = 0;
                for (uint32_t i = 0; i < br.vertex_count; ++i) {
                    if (vertices[i].color != 0xffffffffu) ++non_white;
                    for (uint32_t f = 0; f < mesh.fill_count; ++f)
                        if (mesh.fills[f].rgba == vertices[i].color) {
                            ++matched;
                            break;
                        }
                }
                printf("real: shape2 batch vertices=%u fills=%u non_white=%u "
                       "matched_fill=%u\n",
                       br.vertex_count, mesh.fill_count, non_white, matched);
                check(non_white > 0 && matched == br.vertex_count,
                      "real shape2 vertices carry fill colours");
            }
            free(indices);
            free(vertices);
        }
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF fill tests\n");
    test_solid_fill();
    test_gradient_refusal();
    test_bitmap_refusal();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF fill: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
