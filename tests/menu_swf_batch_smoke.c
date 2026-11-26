/* Host test: bounded text quad-batch construction. */

#include "menu_swf_batch.h"
#include "menu_swf_text.h"
#include "menu_swf_atlas.h"
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

static uint32_t uv(uint32_t coord, uint32_t size) {
    return (uint32_t)(((uint64_t)coord << 16) / size);
}

static void make_entry(MenuSwfAtlasEntry *e, uint16_t index, uint32_t x,
                       uint32_t y, uint32_t w, uint32_t h) {
    memset(e, 0, sizeof *e);
    e->glyph_index = index;
    e->has_rect = 1;
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    e->u = uv(x, 64);
    e->v = uv(y, 64);
    e->uw = uv(x + w, 64) - e->u;
    e->vh = uv(y + h, 64) - e->v;
}

static void test_synthetic(void) {
    MenuSwfTextMesh text;
    MenuSwfTextGlyph glyphs[3];
    MenuSwfAtlasEntry atlas[3];
    MenuSwfBatchStyle style;
    MenuSwfBatchResult r;
    MenuSwfBatchVertex vertices[8];
    uint32_t indices[12];
    char reason[BSA_REASON_MAX];

    memset(&text, 0, sizeof text);
    memset(glyphs, 0, sizeof glyphs);
    glyphs[0].code = 1;
    glyphs[0].glyph_index = 0;
    glyphs[0].has_geometry = 1;
    glyphs[0].bounds.xmin = 0;
    glyphs[0].bounds.xmax = 10;
    glyphs[0].bounds.ymin = 0;
    glyphs[0].bounds.ymax = 20;
    glyphs[1].code = 2;
    glyphs[1].glyph_index = 1;
    glyphs[1].has_geometry = 0; /* empty glyph */
    glyphs[2].code = 3;
    glyphs[2].glyph_index = 2;
    glyphs[2].has_geometry = 1;
    glyphs[2].bounds.xmin = 20;
    glyphs[2].bounds.xmax = 30;
    glyphs[2].bounds.ymin = 0;
    glyphs[2].bounds.ymax = 20;
    text.glyphs = glyphs;
    text.glyph_count = 3;

    make_entry(&atlas[0], 1, 2, 4, 10, 20);
    memset(&atlas[1], 0, sizeof atlas[1]);
    make_entry(&atlas[2], 3, 12, 4, 10, 20);

    memset(&style, 0, sizeof style);
    style.m00 = 1.0;
    style.m11 = 1.0;
    style.tx = 100.0;
    style.ty = 50.0;
    style.color = 0xFF112233u;

    MenuSwfBatchStatus st = menu_swf_batch_text(
        &text, atlas, 3, &style, NULL, vertices, 8, indices, 12, &r, reason,
        sizeof reason);
    check(st == MENU_SWF_BATCH_OK, "synthetic batch");
    check(r.quad_count == 2 && r.vertex_count == 8 && r.index_count == 12,
          "synthetic counts");
    check(r.has_bounds && r.bounds.xmin == 100 && r.bounds.xmax == 130 &&
              r.bounds.ymin == 50 && r.bounds.ymax == 70,
          "synthetic bounds");
    check(vertices[0].x == 100 && vertices[0].y == 50 &&
              vertices[1].x == 110 && vertices[1].y == 50 &&
              vertices[2].x == 110 && vertices[2].y == 70 &&
              vertices[3].x == 100 && vertices[3].y == 70,
          "synthetic first quad positions");
    check(vertices[4].x == 120 && vertices[6].x == 130,
          "synthetic second quad skips empty glyph");
    check(vertices[0].u == atlas[0].u && vertices[0].v == atlas[0].v &&
              vertices[1].u == atlas[0].u + atlas[0].uw &&
              vertices[2].v == atlas[0].v + atlas[0].vh &&
              vertices[3].u == atlas[0].u,
          "synthetic first quad UVs");
    check(vertices[4].u == atlas[2].u && vertices[6].u == atlas[2].u +
              atlas[2].uw,
          "synthetic second quad UVs");
    check(vertices[0].color == style.color && vertices[3].color == style.color &&
              vertices[7].color == style.color,
          "synthetic vertex colors");
    const uint32_t expected[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    check(memcmp(indices, expected, sizeof expected) == 0,
          "synthetic indices without holes");
    int winding = 1;
    for (unsigned t = 0; t < 4; ++t) {
        const MenuSwfBatchVertex *a = &vertices[indices[t * 3 + 0]];
        const MenuSwfBatchVertex *b = &vertices[indices[t * 3 + 1]];
        const MenuSwfBatchVertex *c = &vertices[indices[t * 3 + 2]];
        int64_t area = ((int64_t)b->x - a->x) * ((int64_t)c->y - a->y) -
                       ((int64_t)b->y - a->y) * ((int64_t)c->x - a->x);
        if (area <= 0) winding = 0;
    }
    check(winding, "synthetic positive winding");

    /* Partial viewport: clip both axes and interpolate UVs. */
    MenuSwfBatchViewport vp = {1, 105, 55, 125, 75};
    memset(vertices, 0, sizeof vertices);
    memset(indices, 0, sizeof indices);
    st = menu_swf_batch_text(&text, atlas, 3, &style, &vp, vertices, 8,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && r.quad_count == 2,
          "partial viewport batch");
    check(vertices[0].x == 105 && vertices[0].y == 55 &&
              vertices[1].x == 110 && vertices[2].y == 70,
          "partial viewport positions");
    check(vertices[0].u == atlas[0].u + atlas[0].uw / 2 &&
              vertices[1].u == atlas[0].u + atlas[0].uw &&
              vertices[0].v == atlas[0].v + atlas[0].vh / 4 &&
              vertices[2].v == atlas[0].v + atlas[0].vh,
          "partial viewport proportional UVs");
    check(vertices[4].x == 120 && vertices[5].x == 125,
          "partial viewport second quad");

    /* Fully clipped viewport. */
    MenuSwfBatchViewport outside = {1, 200, 0, 210, 10};
    st = menu_swf_batch_text(&text, atlas, 3, &style, &outside, vertices, 8,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && r.quad_count == 0 &&
              r.vertex_count == 0 && !r.has_bounds,
          "fully clipped batch empty");

    /* Overflow must not touch either buffer. */
    for (unsigned i = 0; i < 8; ++i) {
        vertices[i].x = 0x5A5A5A5A;
        vertices[i].u = 0x5A5A5A5A;
    }
    for (unsigned i = 0; i < 12; ++i) indices[i] = 0x5A5A5A5A;
    st = menu_swf_batch_text(&text, atlas, 3, &style, NULL, vertices, 7,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_OVERFLOW, "vertex overflow refused");
    int untouched = 1;
    for (unsigned i = 0; i < 8; ++i)
        if (vertices[i].x != 0x5A5A5A5A || vertices[i].u != 0x5A5A5A5A)
            untouched = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (indices[i] != 0x5A5A5A5A) untouched = 0;
    check(untouched, "overflow leaves buffers untouched");

    st = menu_swf_batch_text(&text, atlas, 3, &style, NULL, vertices, 8,
                             indices, 11, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_OVERFLOW, "index overflow refused");

    style.m00 = NAN;
    st = menu_swf_batch_text(&text, atlas, 3, &style, NULL, vertices, 8,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_NAN, "NaN transform refused");

    style.m00 = -1.0;
    style.m11 = 1.0;
    st = menu_swf_batch_text(&text, atlas, 3, &style, NULL, vertices, 8,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_SCALE, "negative scale refused");

    style.m00 = 0.0;
    style.m01 = 1.0;
    style.m10 = -1.0;
    style.m11 = 0.0;
    st = menu_swf_batch_text(&text, atlas, 3, &style, &vp, vertices, 8,
                             indices, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_CLIP, "rotated viewport clip refused");
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
        check(font != NULL, "real font found");
        if (font) {
            static TessFixture fx;
            MenuSwfTextMesh text;
            memset(&text, 0, sizeof text);
            MenuSwfTextVertex text_vertices[4096];
            uint32_t text_indices[12288];
            MenuSwfTextGlyph text_glyphs[4];
            MenuSwfTessMesh glyph_mesh;
            tess_fixture_init(&fx, &glyph_mesh);
            text.vertices = text_vertices;
            text.vertex_capacity = 4096;
            text.indices = text_indices;
            text.index_capacity = 12288;
            text.glyphs = text_glyphs;
            text.glyph_capacity = 4;
            text.glyph_tess = glyph_mesh;
            MenuSwfTextStyle tstyle;
            memset(&tstyle, 0, sizeof tstyle);
            tstyle.m00 = 65536;
            tstyle.m11 = 65536;
            const char *str = "\x21\x22\x23\x24"; /* codes 33..36 */
            st = menu_swf_text_layout(bytes, got, font, str, 4, &tstyle,
                                      &text, reason, sizeof reason);
            check(st == MENU_SWF_OK && text.glyph_count == 4,
                  "real text layout for batch");

            uint8_t rasters[4][64 * 64];
            MenuSwfAtlasGlyph atlas_glyphs[4];
            MenuSwfAtlasEntry packed[4];
            int ready = (st == MENU_SWF_OK);
            for (unsigned i = 0; ready && i < 4; ++i) {
                uint16_t code = (uint16_t)(33 + i);
                tess_fixture_init(&fx, &glyph_mesh);
                st = menu_swf_tess_glyph(bytes, got, font->font_id, code,
                                         MENU_SWF_TESS_NON_ZERO, &glyph_mesh,
                                         reason, sizeof reason);
                if (st != MENU_SWF_OK) {
                    ready = 0;
                    break;
                }
                double bw = glyph_mesh.bounds.xmax - glyph_mesh.bounds.xmin;
                double bh = glyph_mesh.bounds.ymax - glyph_mesh.bounds.ymin;
                double scale = (64 - 2.0) / bw;
                double sy = (64 - 2.0) / bh;
                if (sy < scale) scale = sy;
                MenuSwfRasterTransform xf;
                memset(&xf, 0, sizeof xf);
                xf.m00 = scale;
                xf.m11 = scale;
                xf.tx = (64 - bw * scale) / 2.0 -
                        glyph_mesh.bounds.xmin * scale;
                xf.ty = (64 - bh * scale) / 2.0 -
                        glyph_mesh.bounds.ymin * scale;
                MenuSwfRasterTarget rt = {64, 64, 64, rasters[i], 0, 0};
                MenuSwfRasterResult rr;
                MenuSwfRasterStatus rs = menu_swf_raster_mesh(
                    &glyph_mesh, &xf, &rt, &rr, reason, sizeof reason);
                if (rs != MENU_SWF_RASTER_OK) {
                    ready = 0;
                    break;
                }
                atlas_glyphs[i].pixels = rasters[i] + rr.y0 * 64 + rr.x0;
                atlas_glyphs[i].stride = 64;
                atlas_glyphs[i].width = rr.x1 - rr.x0;
                atlas_glyphs[i].height = rr.y1 - rr.y0;
                atlas_glyphs[i].glyph_index = code;
            }
            check(ready, "real batch atlas inputs");
            if (ready) {
                static uint8_t atlas_pixels[256 * 256];
                MenuSwfAtlasTarget at = {256, 256, 256, atlas_pixels};
                MenuSwfAtlasResult ar;
                MenuSwfAtlasStatus as = menu_swf_atlas_pack(
                    atlas_glyphs, 4, &at, packed, 4, &ar, reason,
                    sizeof reason);
                check(as == MENU_SWF_ATLAS_OK, "real batch atlas pack");

                MenuSwfAtlasEntry aligned[4];
                for (unsigned i = 0; i < 4; ++i) {
                    aligned[i] = packed[i];
                    for (unsigned j = 0; j < 4; ++j)
                        if (packed[j].glyph_index == (uint16_t)(33 + i))
                            aligned[i] = packed[j];
                }

                int32_t xmin = text_glyphs[0].bounds.xmin;
                int32_t xmax = text_glyphs[0].bounds.xmax;
                int32_t ymin = text_glyphs[0].bounds.ymin;
                int32_t ymax = text_glyphs[0].bounds.ymax;
                for (unsigned i = 1; i < 4; ++i) {
                    if (text_glyphs[i].bounds.xmin < xmin)
                        xmin = text_glyphs[i].bounds.xmin;
                    if (text_glyphs[i].bounds.xmax > xmax)
                        xmax = text_glyphs[i].bounds.xmax;
                    if (text_glyphs[i].bounds.ymin < ymin)
                        ymin = text_glyphs[i].bounds.ymin;
                    if (text_glyphs[i].bounds.ymax > ymax)
                        ymax = text_glyphs[i].bounds.ymax;
                }
                double scale = 126.0 / (xmax - xmin);
                double sy = 62.0 / (ymax - ymin);
                if (sy < scale) scale = sy;
                MenuSwfBatchStyle bs;
                memset(&bs, 0, sizeof bs);
                bs.m00 = scale;
                bs.m11 = scale;
                bs.tx = 1.0 - xmin * scale;
                bs.ty = 1.0 - ymin * scale;
                bs.color = 0xFFCC8844u;
                MenuSwfBatchVertex verts[16];
                uint32_t idx[24];
                MenuSwfBatchResult br;
                MenuSwfBatchViewport vp = {1, 0, 0, 128, 64};
                MenuSwfBatchStatus bst = menu_swf_batch_text(
                    &text, aligned, 4, &bs, &vp, verts, 16, idx, 24, &br,
                    reason, sizeof reason);
                check(bst == MENU_SWF_BATCH_OK, "real batch build");
                if (bst == MENU_SWF_BATCH_OK) {
                    printf("real: batch quads=%u vertices=%u indices=%u "
                           "bounds=%d,%d,%d,%d\n",
                           br.quad_count, br.vertex_count, br.index_count,
                           br.bounds.xmin, br.bounds.xmax, br.bounds.ymin,
                           br.bounds.ymax);
                    printf("real: first quad x=%d..%d y=%d..%d "
                           "uv=%u,%u..%u,%u\n",
                           verts[0].x, verts[1].x, verts[0].y, verts[2].y,
                           verts[0].u, verts[0].v, verts[2].u, verts[2].v);
                    check(br.quad_count == 4 && br.vertex_count == 16 &&
                              br.index_count == 24,
                          "real batch counts");
                    int uv_ok = 1, overlap = 0, inside = 1;
                    for (unsigned i = 0; i < 4; ++i) {
                        const MenuSwfBatchVertex *v = &verts[i * 4];
                        if (v[0].u != aligned[i].u ||
                            v[0].v != aligned[i].v ||
                            v[1].u != aligned[i].u + aligned[i].uw ||
                            v[2].v != aligned[i].v + aligned[i].vh)
                            uv_ok = 0;
                        int32_t qx0 = v[0].x, qx1 = v[1].x;
                        int32_t qy0 = v[0].y, qy1 = v[2].y;
                        if (qx0 < 0 || qy0 < 0 || qx1 > 128 || qy1 > 64)
                            inside = 0;
                        for (unsigned j = i + 1; j < 4; ++j) {
                            const MenuSwfBatchVertex *w = &verts[j * 4];
                            if (qx0 < w[1].x && w[0].x < qx1 &&
                                qy0 < w[2].y && w[0].y < qy1)
                                overlap = 1;
                        }
                    }
                    check(uv_ok, "real batch UV corners");
                    check(!overlap, "real batch quads do not overlap");
                    check(inside, "real batch inside viewport");
                } else {
                    printf("real: batch failed: %s\n", reason);
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
    printf("menu SWF batch tests\n");
    test_synthetic();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF batch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
