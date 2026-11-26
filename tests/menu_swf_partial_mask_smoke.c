/* Host test: partial axis-aligned clip-mask clipping and rotated refusals.
 *
 * Covers shape triangles crossing one mask edge (positions, area, colour and
 * winding), fully inside/outside triangles, source-vertex dedup, partially
 * clipped text quads with UV interpolation, rotated text and rotated masks as
 * named refusals, and the real StartMenu zero-mask equivalence.
 */

#include "bsa_reader.h"
#include "menu_swf_batch.h"
#include "menu_swf_clip.h"
#include "menu_swf_display_list.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"
#include "menu_swf.h"

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

static void identity_shape_style(MenuSwfBatchShapeStyle *s) {
    memset(s, 0, sizeof *s);
    s->m00 = s->m11 = 0x10000;
    s->mult[0] = s->mult[1] = s->mult[2] = s->mult[3] = 256;
}

static MenuSwfClipRecord clip_rect(int32_t xmin, int32_t ymin, int32_t xmax,
                                   int32_t ymax) {
    MenuSwfClipRecord c;
    memset(&c, 0, sizeof c);
    c.mask_count = 1;
    c.has_rect = 1;
    c.clip_rect = (MenuSwfClipRect){xmin, xmax, ymin, ymax};
    return c;
}

typedef struct {
    MenuSwfTessVertex v[8];
    uint32_t idx[6];
    uint16_t fills[2];
    MenuSwfTessFill fill;
} MeshFix;

static MenuSwfTessMesh make_mesh(MeshFix *f, unsigned triangles) {
    MenuSwfTessMesh m;
    memset(&m, 0, sizeof m);
    memset(f, 0, sizeof *f);
    f->fill.kind = MENU_SWF_FILL_SOLID;
    f->fill.rgba = 0xff336699u;
    m.vertices = f->v;
    m.vertex_count = triangles * 3u;
    m.vertex_capacity = 8;
    m.indices = f->idx;
    m.index_count = triangles * 3u;
    m.index_capacity = 6;
    m.triangle_fills = f->fills;
    m.triangle_count = triangles;
    m.triangle_capacity = 2;
    m.fills = &f->fill;
    m.fill_count = 1;
    for (unsigned t = 0; t < triangles; ++t) {
        f->idx[t * 3 + 0] = t * 3 + 0;
        f->idx[t * 3 + 1] = t * 3 + 1;
        f->idx[t * 3 + 2] = t * 3 + 2;
        f->fills[t] = 1;
    }
    return m;
}

static void set_triangle(MeshFix *f, unsigned t, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    f->v[t * 3 + 0] = (MenuSwfTessVertex){x0, y0};
    f->v[t * 3 + 1] = (MenuSwfTessVertex){x1, y1};
    f->v[t * 3 + 2] = (MenuSwfTessVertex){x2, y2};
}

static double signed_area(const MenuSwfBatchVertex *v, uint32_t a, uint32_t b,
                          uint32_t c) {
    return ((double)v[b].x - v[a].x) * ((double)v[c].y - v[a].y) -
           ((double)v[b].y - v[a].y) * ((double)v[c].x - v[a].x);
}

static double output_area(const MenuSwfBatchVertex *v, const uint32_t *indices,
                          uint32_t index_count) {
    double sum = 0.0;
    for (uint32_t i = 0; i + 2 < index_count; i += 3)
        sum += signed_area(v, indices[i], indices[i + 1], indices[i + 2]) /
               2.0;
    return sum;
}

static int all_winding_positive(const MenuSwfBatchVertex *v,
                                const uint32_t *indices,
                                uint32_t index_count) {
    for (uint32_t i = 0; i + 2 < index_count; i += 3)
        if (!(signed_area(v, indices[i], indices[i + 1], indices[i + 2]) >
              0.0))
            return 0;
    return 1;
}

static void test_shape_edge(void) {
    MenuSwfBatchShapeStyle style;
    identity_shape_style(&style);
    MenuSwfBatchVertex vertices[16];
    uint32_t indices[16];
    MenuSwfBatchResult br;
    char reason[BSA_REASON_MAX];

    /* (4,0)-(6,0)-(4,2) pixels, clipped at x=5. */
    MeshFix f;
    MenuSwfTessMesh m = make_mesh(&f, 1);
    set_triangle(&f, 0, 80, 0, 120, 0, 80, 40);
    MenuSwfClipRecord clip = clip_rect(0, 0, 100, 100);
    MenuSwfBatchStatus st = menu_swf_batch_shape_clip(
        &m, &style, NULL, &clip, vertices, 16, indices, 16, &br, reason,
        sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 2 &&
              br.vertex_count == 5 && br.index_count == 6 && br.has_bounds,
          "edge-crossing triangle yields a clipped fan");
    check(st == MENU_SWF_BATCH_OK && vertices[0].x == 4 &&
              vertices[0].y == 0 && vertices[1].x == 6 &&
              vertices[1].y == 0 && vertices[2].x == 4 &&
              vertices[2].y == 2,
          "source vertices keep their transformed positions");
    check(st == MENU_SWF_BATCH_OK && vertices[3].x == 5 &&
              vertices[3].y == 0 && vertices[4].x == 5 &&
              vertices[4].y == 1,
          "generated vertices land on the mask edge");
    check(st == MENU_SWF_BATCH_OK &&
              fabs(output_area(vertices, indices, br.index_count) - 1.5) <
                  1e-9,
          "clipped area is 1.5 square pixels");
    check(st == MENU_SWF_BATCH_OK &&
              all_winding_positive(vertices, indices, br.index_count),
          "clipped fan keeps positive winding");
    int colour_ok = 1;
    for (uint32_t i = 0; i < br.index_count; ++i)
        if (vertices[indices[i]].color != 0xff336699u) colour_ok = 0;
    check(colour_ok, "every clipped vertex keeps the solid fill colour");
    check(br.bounds.xmin == 4 && br.bounds.xmax == 5 && br.bounds.ymin == 0 &&
              br.bounds.ymax == 2,
          "clipped bounds cover only emitted geometry");
}

static void test_inside_outside(void) {
    MenuSwfBatchShapeStyle style;
    identity_shape_style(&style);
    MenuSwfBatchVertex vertices[16];
    uint32_t indices[16];
    MenuSwfBatchResult br;
    char reason[BSA_REASON_MAX];

    MeshFix f;
    MenuSwfTessMesh m = make_mesh(&f, 1);
    set_triangle(&f, 0, 0, 0, 80, 0, 0, 80);
    MenuSwfClipRecord clip = clip_rect(0, 0, 200, 200);
    MenuSwfBatchStatus st = menu_swf_batch_shape_clip(
        &m, &style, NULL, &clip, vertices, 16, indices, 16, &br, reason,
        sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 1 &&
              br.vertex_count == 3 && br.index_count == 3 &&
              indices[0] == 0 && indices[1] == 1 && indices[2] == 2,
          "fully inside triangle is byte-for-byte unchanged");

    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 200, 200, 280, 200, 200, 280);
    clip = clip_rect(0, 0, 100, 100);
    memset(vertices, 0x5A, sizeof vertices);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 16, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 0 &&
              br.index_count == 0 && br.vertex_count == 0 &&
              vertices[0].color == 0x5A5A5A5Au,
          "fully outside triangle culls without writing");

    /* Two triangles share vertex (4,0); the inside one keeps indices 0,1,2. */
    MeshFix g;
    memset(&g, 0, sizeof g);
    g.fill.kind = MENU_SWF_FILL_SOLID;
    g.fill.rgba = 0xff336699u;
    g.v[0] = (MenuSwfTessVertex){0, 0};
    g.v[1] = (MenuSwfTessVertex){80, 0};
    g.v[2] = (MenuSwfTessVertex){0, 80};
    g.v[3] = (MenuSwfTessVertex){160, 0};
    g.v[4] = (MenuSwfTessVertex){80, 80};
    g.idx[0] = 0; g.idx[1] = 1; g.idx[2] = 2;
    g.idx[3] = 1; g.idx[4] = 3; g.idx[5] = 4;
    g.fills[0] = g.fills[1] = 1;
    MenuSwfTessMesh shared;
    memset(&shared, 0, sizeof shared);
    shared.vertices = g.v;
    shared.vertex_count = 5;
    shared.vertex_capacity = 8;
    shared.indices = g.idx;
    shared.index_count = 6;
    shared.index_capacity = 6;
    shared.triangle_fills = g.fills;
    shared.triangle_count = 2;
    shared.triangle_capacity = 2;
    shared.fills = &g.fill;
    shared.fill_count = 1;
    clip = clip_rect(0, 0, 100, 200);
    st = menu_swf_batch_shape_clip(&shared, &style, NULL, &clip, vertices, 16,
                                   indices, 16, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 3 &&
              br.vertex_count == 7 && br.index_count == 9 &&
              indices[0] == 0 && indices[1] == 1 && indices[2] == 2,
          "inside triangle keeps source sharing beside a clipped one");
    check(st == MENU_SWF_BATCH_OK &&
              fabs(output_area(vertices, indices, br.index_count) - 11.5) <
                  1e-9 &&
              all_winding_positive(vertices, indices, br.index_count),
          "mixed dedup fan keeps total area and winding");
}

static void test_rotated_mask(void) {
    MenuSwfDisplayListRecord rec[2];
    MenuSwfClipShape shape[2];
    MenuSwfClipMask masks[2];
    MenuSwfClipRecord out[2];
    MenuSwfClipResult cr;
    char reason[BSA_REASON_MAX];
    memset(rec, 0, sizeof rec);
    memset(shape, 0, sizeof shape);

    /* 45-degree mask at depth 1, clipped record at depth 2. */
    rec[0].depth = 1;
    rec[0].character_id = 100;
    rec[0].has_character = 1;
    rec[0].has_clip_depth = 1;
    rec[0].clip_depth = 2;
    rec[0].matrix.m00 = 46341;
    rec[0].matrix.m01 = 46341;
    rec[0].matrix.m10 = -46341;
    rec[0].matrix.m11 = 46341;
    rec[1].depth = 2;
    rec[1].character_id = 101;
    rec[1].has_character = 1;
    rec[1].matrix.m00 = 65536;
    rec[1].matrix.m11 = 65536;
    for (unsigned i = 0; i < 2; ++i) {
        shape[i].has_bounds = 1;
        shape[i].is_rect = 1;
        shape[i].bounds = (MenuSwfShapeRect){0, 100, 0, 100};
    }
    MenuSwfClipStatus cs = menu_swf_clip_build(
        rec, 2, shape, 2, masks, 2, out, 2, &cr, reason, sizeof reason);
    check(cs == MENU_SWF_CLIP_OK && cr.mask_count == 1 &&
              out[1].mask_count == 1 && out[1].has_rotated_mask,
          "rotated mask is marked on the record");
    check(cs == MENU_SWF_CLIP_OK && !masks[0].axis_aligned,
          "rotated mask quad is not axis aligned");

    MenuSwfBatchShapeStyle style;
    identity_shape_style(&style);
    MeshFix f;
    MenuSwfTessMesh m = make_mesh(&f, 1);
    set_triangle(&f, 0, 0, 0, 200, 0, 0, 200);
    MenuSwfBatchVertex vertices[16];
    uint32_t indices[16];
    MenuSwfBatchResult br;
    MenuSwfBatchStatus st = menu_swf_batch_shape_clip(
        &m, &style, NULL, &out[1], vertices, 16, indices, 16, &br, reason,
        sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              strstr(reason, "partial clip-mask culling of rotated masks") &&
              strstr(reason, "not implemented"),
          "partial rotated mask is a named refusal");
}

static void test_text_partial(void) {
    MenuSwfTextMesh text;
    MenuSwfTextGlyph glyph;
    MenuSwfAtlasEntry atlas;
    MenuSwfBatchStyle style;
    MenuSwfBatchVertex vertices[4];
    uint32_t indices[6];
    MenuSwfBatchResult br;
    char reason[BSA_REASON_MAX];

    memset(&text, 0, sizeof text);
    memset(&glyph, 0, sizeof glyph);
    memset(&atlas, 0, sizeof atlas);
    glyph.glyph_index = 0;
    glyph.code = 'A';
    glyph.has_geometry = 1;
    glyph.has_color = 1;
    glyph.color = 0xff00ff00u;
    glyph.bounds = (MenuSwfShapeRect){0, 10, 0, 10};
    text.glyphs = &glyph;
    text.glyph_count = 1;
    atlas.has_rect = 1;
    atlas.w = 10;
    atlas.h = 10;
    atlas.uw = 0x10000;
    atlas.vh = 0x10000;
    memset(&style, 0, sizeof style);
    style.m00 = style.m11 = 1.0;
    style.color = 0xffffffffu;

    MenuSwfClipRecord clip = clip_rect(100, 100, 200, 200);
    MenuSwfBatchStatus st = menu_swf_batch_text_clip(
        &text, &atlas, 1, &style, NULL, &clip, vertices, 4, indices, 6, &br,
        reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.quad_count == 1 &&
              br.vertex_count == 4 && br.index_count == 6,
          "partial text glyph becomes one clipped sub-quad");
    check(st == MENU_SWF_BATCH_OK && vertices[0].x == 5 &&
              vertices[0].y == 5 && vertices[1].x == 10 &&
              vertices[1].y == 5 && vertices[2].x == 10 &&
              vertices[2].y == 10 && vertices[3].x == 5 &&
              vertices[3].y == 10,
          "clipped text quad meets the mask rectangle");
    check(st == MENU_SWF_BATCH_OK && vertices[0].u == 32768 &&
              vertices[0].v == 32768 && vertices[1].u == 65536 &&
              vertices[1].v == 32768 && vertices[2].u == 65536 &&
              vertices[2].v == 65536 && vertices[3].u == 32768 &&
              vertices[3].v == 65536,
          "clipped text UVs interpolate over the kept fraction");
    check(br.has_bounds && br.bounds.xmin == 5 && br.bounds.xmax == 10 &&
              br.bounds.ymin == 5 && br.bounds.ymax == 10,
          "clipped text bounds follow the mask");
    int colour_ok = 1;
    for (unsigned i = 0; i < 4; ++i)
        if (vertices[i].color != 0xff00ff00u) colour_ok = 0;
    check(colour_ok, "clipped text keeps its run colour");

    /* A rotated text transform cannot be represented as a sub-rectangle. */
    style.m01 = 0.1;
    style.m10 = -0.1;
    memset(vertices, 0x44, sizeof vertices);
    st = menu_swf_batch_text_clip(&text, &atlas, 1, &style, NULL, &clip,
                                  vertices, 4, indices, 6, &br, reason,
                                  sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              strstr(reason, "partial clip-mask culling of rotated text") &&
              vertices[0].color == 0x44444444u,
          "partial rotated text is a named refusal");
}

static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    char reason[BSA_REASON_MAX];
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

    MenuSwfDisplayListRecord records[216];
    MenuSwfDisplayListResult flat;
    MenuSwfStatus st = menu_swf_display_list_build(
        bytes, got, records, 216, &flat, reason, sizeof reason);
    check(st == MENU_SWF_OK && flat.record_count == 216,
          "real flattened display list");
    MenuSwfClipMask masks[16];
    MenuSwfClipRecord clip_records[216];
    MenuSwfClipResult clip_result;
    MenuSwfClipStatus cs = menu_swf_clip_build(
        records, flat.record_count, NULL, 0, masks, 16, clip_records, 216,
        &clip_result, reason, sizeof reason);
    check(cs == MENU_SWF_CLIP_OK && clip_result.mask_count == 0 &&
              clip_result.clipped_records == 0,
          "real StartMenu has no clip masks");
    int all_zero = 1;
    for (uint32_t i = 0; i < flat.record_count; ++i)
        if (clip_records[i].mask_count || clip_records[i].fully_clipped ||
            clip_records[i].has_rotated_mask)
            all_zero = 0;
    check(all_zero, "real clip records carry no active or rotated masks");

    MenuSwfTessMesh mesh;
    static MenuSwfTessVertex tv[65536];
    static uint32_t ti[196608];
    static uint16_t tf[65536];
    static MenuSwfTessLine tl[65536];
    static MenuSwfTessEdge te[65536];
    static uint32_t ta[65536];
    static int32_t tb[131072];
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = tv; mesh.vertex_capacity = 65536;
    mesh.indices = ti; mesh.index_capacity = 196608;
    mesh.triangle_fills = tf; mesh.triangle_capacity = 65536;
    mesh.lines = tl; mesh.line_capacity = 65536;
    mesh.edges = te; mesh.edge_capacity = 65536;
    mesh.active = ta; mesh.active_capacity = 65536;
    mesh.bands = tb; mesh.band_capacity = 131072;
    static MenuSwfTessFill fills[256];
    mesh.fills = fills; mesh.fill_capacity = 256;
    st = menu_swf_tess_shape(bytes, got, 2, MENU_SWF_TESS_EVEN_ODD, &mesh,
                             reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.triangle_count > 0,
          "real shape 2 tessellates");
    if (st == MENU_SWF_OK && mesh.triangle_count > 0) {
        uint32_t cap = mesh.vertex_count + mesh.index_count;
        MenuSwfBatchVertex *vertices =
            (MenuSwfBatchVertex *)malloc(cap * sizeof *vertices);
        uint32_t *indices = (uint32_t *)malloc(cap * sizeof *indices);
        MenuSwfBatchShapeStyle style;
        identity_shape_style(&style);
        MenuSwfBatchResult plain, masked;
        MenuSwfBatchStatus bs = MENU_SWF_BATCH_ERR_ARGUMENT;
        if (vertices && indices)
            bs = menu_swf_batch_shape(&mesh, &style, NULL, vertices, cap,
                                      indices, cap, &plain, reason,
                                      sizeof reason);
        MenuSwfBatchStatus ms = MENU_SWF_BATCH_ERR_ARGUMENT;
        if (vertices && indices)
            ms = menu_swf_batch_shape_clip(
                &mesh, &style, NULL, &clip_records[0], vertices, cap, indices,
                cap, &masked, reason, sizeof reason);
        check(bs == MENU_SWF_BATCH_OK && ms == MENU_SWF_BATCH_OK &&
                  plain.vertex_count == masked.vertex_count &&
                  plain.index_count == masked.index_count &&
                  plain.triangle_count == masked.triangle_count,
              "real zero-mask clip leaves the batch unchanged");
        printf("real: shape 2 tess triangles=%u batch vertices=%u indices=%u\n",
               mesh.triangle_count, plain.vertex_count, plain.index_count);
        free(indices);
        free(vertices);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF partial clip-mask tests\n");
    test_shape_edge();
    test_inside_outside();
    test_rotated_mask();
    test_text_partial();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF partial clip-mask: %d checks, %d failures\n", checks,
           failures);
    return failures ? 1 : 0;
}
