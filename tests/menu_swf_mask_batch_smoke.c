/* Host test: clip-mask culling in text and shape batches. */

#include "menu_swf_batch.h"
#include "menu_swf_clip.h"
#include "menu_swf_display_list.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"
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

static void identity_shape_style(MenuSwfBatchShapeStyle *s) {
    memset(s, 0, sizeof *s);
    s->m00 = s->m11 = 0x10000;
    s->mult[0] = s->mult[1] = s->mult[2] = s->mult[3] = 256;
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
    f->fill.rgba = 0xff112233u;
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

static MenuSwfClipRecord clip_rect(int32_t x0, int32_t y0, int32_t x1,
                                   int32_t y1) {
    MenuSwfClipRecord c;
    memset(&c, 0, sizeof c);
    c.mask_count = 1;
    c.has_rect = 1;
    c.clip_rect = (MenuSwfClipRect){x0, x1, y0, y1};
    return c;
}

static void test_shape_masks(void) {
    MenuSwfBatchShapeStyle style;
    identity_shape_style(&style);
    MenuSwfBatchVertex vertices[16];
    uint32_t indices[12];
    MenuSwfBatchResult br;
    char reason[BSA_REASON_MAX];

    /* Fully inside: unchanged. */
    MeshFix f;
    MenuSwfTessMesh m = make_mesh(&f, 1);
    set_triangle(&f, 0, 0, 0, 80, 0, 0, 80);
    MenuSwfClipRecord clip = clip_rect(0, 0, 100, 100);
    MenuSwfBatchStatus st = menu_swf_batch_shape_clip(
        &m, &style, NULL, &clip, vertices, 16, indices, 12, &br, reason,
        sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 1 &&
              br.index_count == 3 && br.vertex_count == 3,
          "mask inside keeps the triangle");
    check(indices[0] == 0 && indices[1] == 1 && indices[2] == 2 &&
              vertices[0].color == 0xff112233u,
          "mask inside preserves indices/colour");

    /* Fully outside: culled before writing. */
    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 200, 200, 280, 200, 200, 280);
    clip = clip_rect(0, 0, 100, 100);
    memset(vertices, 0x5A, sizeof vertices);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 0 &&
              br.index_count == 0 && br.vertex_count == 0 &&
              vertices[0].color == 0x5A5A5A5Au,
          "mask outside culls without writing");

    /* One inside and one outside: only the inside triangle is emitted. */
    m = make_mesh(&f, 2);
    set_triangle(&f, 0, 0, 0, 80, 0, 0, 80);
    set_triangle(&f, 1, 200, 200, 280, 200, 200, 280);
    clip = clip_rect(0, 0, 100, 100);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 1 &&
              br.index_count == 3 && br.vertex_count == 6 &&
              indices[0] == 0 && indices[1] == 1 && indices[2] == 2,
          "mask culls only the outside triangle");

    /* Partial overlap is an honest named refusal. */
    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 80, 0, 120, 0, 80, 40);
    clip = clip_rect(0, 0, 100, 100);
    memset(vertices, 0x33, sizeof vertices);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              !strcmp(reason, "partial clip-mask culling is not implemented") &&
              vertices[0].color == 0x33333333u,
          "partial shape clip is a named refusal");

    /* A rotated/partial style uses the same refusal. */
    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 80, 0, 120, 0, 80, 40);
    style.m01 = 0x1000;
    style.m10 = -0x1000;
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              strstr(reason, "partial clip-mask culling"),
          "rotated partial clip refuses by name");
    identity_shape_style(&style);

    /* Fully clipped record returns an empty batch. */
    clip = clip_rect(0, 0, 100, 100);
    clip.fully_clipped = 1;
    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 0, 0, 80, 0, 0, 80);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.triangle_count == 0 &&
              br.index_count == 0,
          "fully clipped record is empty");

    /* A clipped record without bounds is a named refusal. */
    clip = clip_rect(0, 0, 100, 100);
    clip.has_rect = 0;
    m = make_mesh(&f, 1);
    set_triangle(&f, 0, 0, 0, 80, 0, 0, 80);
    st = menu_swf_batch_shape_clip(&m, &style, NULL, &clip, vertices, 16,
                                   indices, 12, &br, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              strstr(reason, "clip-mask record bounds are missing"),
          "clip without record bounds refuses by name");
}

static void test_text_masks(void) {
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

    MenuSwfClipRecord clip = clip_rect(0, 0, 200, 200);
    MenuSwfBatchStatus st = menu_swf_batch_text_clip(
        &text, &atlas, 1, &style, NULL, &clip, vertices, 4, indices, 6, &br,
        reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.quad_count == 1,
          "text mask inside keeps the glyph");
    check(vertices[0].color == 0xff00ff00u, "text run colour survives mask");

    clip = clip_rect(200, 200, 300, 300);
    st = menu_swf_batch_text_clip(&text, &atlas, 1, &style, NULL, &clip,
                                  vertices, 4, indices, 6, &br, reason,
                                  sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.quad_count == 0,
          "text mask outside culls the glyph");

    clip = clip_rect(100, 100, 200, 200);
    memset(vertices, 0x44, sizeof vertices);
    st = menu_swf_batch_text_clip(&text, &atlas, 1, &style, NULL, &clip,
                                  vertices, 4, indices, 6, &br, reason,
                                  sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_MASK &&
              !strcmp(reason, "partial clip-mask culling is not implemented") &&
              vertices[0].color == 0x44444444u,
          "partial text clip is a named refusal");

    clip = clip_rect(0, 0, 200, 200);
    clip.fully_clipped = 1;
    st = menu_swf_batch_text_clip(&text, &atlas, 1, &style, NULL, &clip,
                                  vertices, 4, indices, 6, &br, reason,
                                  sizeof reason);
    check(st == MENU_SWF_BATCH_OK && br.quad_count == 0,
          "fully clipped text record is empty");
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
        if (clip_records[i].mask_count || clip_records[i].fully_clipped)
            all_zero = 0;
    check(all_zero, "real clip records carry no active masks");

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
                  plain.index_count == masked.index_count,
              "real zero-mask clip leaves the batch unchanged");
        free(indices);
        free(vertices);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF mask batch tests\n");
    test_shape_masks();
    test_text_masks();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF mask batch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
