/* SWF linear/radial gradient fills evaluated per vertex.
 *
 * The gradient matrix maps gradient space to shape space; the batch inverts it,
 * normalises the linear x axis from the -16384..16384 gradient square (radial
 * uses the distance from the center and the same 16384 radius), applies the SWF
 * spread mode (pad/reflect/repeat), interpolates up to eight stops and then the
 * record CXFORM.  Focal and bitmap fills are named refusals.  Colours are
 * per-vertex; large triangles are an explicit approximation.
 *
 * Optional real data: the retail StartMenu has no gradient/bitmap fills, so the
 * layer must not change its solid-fill census. */
#include "menu_swf_batch.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
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

static void identity_style(MenuSwfBatchShapeStyle *style) {
    memset(style, 0, sizeof *style);
    style->m00 = style->m11 = 0x10000;
    style->mult[0] = style->mult[1] = style->mult[2] = style->mult[3] = 256;
    style->u = 0x10000;
    style->v = 0x20000;
}

static void linear_fill(MenuSwfShapeFill *fill, unsigned spread) {
    memset(fill, 0, sizeof *fill);
    fill->kind = MENU_SWF_FILL_LINEAR;
    fill->spread = (uint8_t)spread;
    fill->interpolation = 0;
    fill->stop_count = fill->stop_total = 2;
    fill->stops[0] = (MenuSwfShapeGradientStop){0, 255, 0, 0, 255};
    fill->stops[1] = (MenuSwfShapeGradientStop){255, 0, 255, 0, 255};
    /* Map gradient -16384..16384 to shape x=0..100. */
    fill->matrix.scale_x = 200;
    fill->matrix.scale_y = 0x10000;
    fill->matrix.translate_x = 50;
}

static void radial_fill(MenuSwfShapeFill *fill) {
    memset(fill, 0, sizeof *fill);
    fill->kind = MENU_SWF_FILL_RADIAL;
    fill->interpolation = 0;
    fill->stop_count = fill->stop_total = 2;
    fill->stops[0] = (MenuSwfShapeGradientStop){0, 255, 0, 0, 255};
    fill->stops[1] = (MenuSwfShapeGradientStop){255, 0, 255, 0, 255};
    fill->matrix.scale_x = fill->matrix.scale_y = 200; /* 50px radius / 16384 */
    fill->matrix.translate_x = fill->matrix.translate_y = 50;
}

static int batch_one(const MenuSwfTessVertex *verts, uint32_t vertex_count,
                     const MenuSwfShapeFill *fill, const MenuSwfBatchShapeStyle *style,
                     MenuSwfBatchVertex *out, uint32_t out_capacity,
                     char *reason, size_t reason_cap) {
    uint32_t indices[8], out_indices[8];
    uint16_t tri_fill[1] = {1};
    for (uint32_t i = 0; i < 3; ++i)
        indices[i] = i < vertex_count ? i : 0;
    MenuSwfTessMesh mesh;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = (MenuSwfTessVertex *)verts;
    mesh.vertex_count = vertex_count;
    mesh.indices = indices;
    mesh.index_count = 3;
    mesh.triangle_fills = tri_fill;
    mesh.triangle_count = 1;
    mesh.fills = (MenuSwfTessFill *)fill;
    mesh.fill_count = 1;
    MenuSwfBatchResult result;
    MenuSwfBatchStatus st = menu_swf_batch_shape(
        &mesh, style, NULL, out, out_capacity, out_indices, 3, &result,
        reason, reason_cap);
    return st;
}

static void test_linear(void) {
    MenuSwfShapeFill fill;
    linear_fill(&fill, 0);
    MenuSwfBatchShapeStyle style;
    identity_style(&style);
    MenuSwfTessVertex v[3] = {{0, 0}, {50, 0}, {100, 0}};
    MenuSwfBatchVertex out[8];
    char reason[BSA_REASON_MAX];
    int st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK, "linear gradient batch");
    if (st == MENU_SWF_BATCH_OK) {
        check(out[0].color == 0xffff0000u, "linear stop 0 is red");
        check(out[1].color == 0xff808000u, "linear midpoint interpolates");
        check(out[2].color == 0xff00ff00u, "linear stop 1 is green");
    }

    /* Three stops: red -> green (128) -> blue (255). */
    fill.stop_count = fill.stop_total = 3;
    fill.stops[1] = (MenuSwfShapeGradientStop){128, 0, 255, 0, 255};
    fill.stops[2] = (MenuSwfShapeGradientStop){255, 0, 0, 255, 255};
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK, "three-stop linear batch");
    if (st == MENU_SWF_BATCH_OK) {
        check(out[0].color == 0xffff0000u, "three-stop start red");
        check(((out[1].color >> 16) & 255u) < 3 &&
                  ((out[1].color >> 8) & 255u) > 252 &&
                  (out[1].color & 255u) < 3,
              "three-stop midpoint is green");
        check(out[2].color == 0xff0000ffu, "three-stop end blue");
    }

    /* Spread modes at t=1.25: pad clamps, reflect 0.75, repeat 0.25. */
    linear_fill(&fill, 0);
    MenuSwfTessVertex far = {125, 0};
    MenuSwfBatchVertex one[8];
    st = batch_one(&far, 1, &fill, &style, one, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && one[0].color == 0xff00ff00u,
          "linear pad clamps to the last stop");
    fill.spread = 1;
    st = batch_one(&far, 1, &fill, &style, one, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && one[0].color == 0xff40bf00u,
          "linear reflect maps 1.25 to 0.75");
    fill.spread = 2;
    st = batch_one(&far, 1, &fill, &style, one, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK && one[0].color == 0xffbf4000u,
          "linear repeat maps 1.25 to 0.25");
}

static void test_radial(void) {
    MenuSwfShapeFill fill;
    radial_fill(&fill);
    MenuSwfBatchShapeStyle style;
    identity_style(&style);
    MenuSwfTessVertex v[3] = {{50, 50}, {75, 50}, {100, 50}};
    MenuSwfBatchVertex out[8];
    char reason[BSA_REASON_MAX];
    int st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_OK, "radial gradient batch");
    if (st == MENU_SWF_BATCH_OK) {
        check(out[0].color == 0xffff0000u, "radial center is stop 0");
        check(out[1].color == 0xff808000u, "radial midpoint interpolates");
        check(out[2].color == 0xff00ff00u, "radial radius is stop 1");
    }
}

static void test_refusals(void) {
    MenuSwfShapeFill fill;
    MenuSwfBatchShapeStyle style;
    identity_style(&style);
    MenuSwfTessVertex v[3] = {{0, 0}, {50, 0}, {100, 0}};
    MenuSwfBatchVertex out[8];
    char reason[BSA_REASON_MAX];

    linear_fill(&fill, 0);
    fill.kind = MENU_SWF_FILL_FOCAL;
    int st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "focal gradient") != NULL,
          "focal gradient is a named refusal");

    linear_fill(&fill, 0);
    fill.focal_present = 1;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "focal gradient") != NULL,
          "focal point on a linear style is refused");

    linear_fill(&fill, 0);
    fill.kind = MENU_SWF_FILL_BITMAP;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "bitmap fills") != NULL,
          "bitmap fill remains a named refusal");

    linear_fill(&fill, 0);
    fill.stops[0].ratio = 200;
    fill.stops[1].ratio = 1;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "not ascending") != NULL,
          "descending stops are named");

    linear_fill(&fill, 0);
    fill.stop_total = 9;
    fill.stop_count = 8;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "1..8") != NULL,
          "truncated stop list is named");

    linear_fill(&fill, 0);
    fill.interpolation = 1;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "interpolation") != NULL,
          "unsupported interpolation is named");

    linear_fill(&fill, 3);
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "spread") != NULL,
          "unsupported spread is named");

    linear_fill(&fill, 0);
    fill.matrix.scale_x = fill.matrix.scale_y = 0;
    st = batch_one(v, 3, &fill, &style, out, 8, reason, sizeof reason);
    check(st == MENU_SWF_BATCH_ERR_FILL &&
              strstr(reason, "singular") != NULL,
          "singular gradient matrix is named");
}

static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024], reason[BSA_REASON_MAX];
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
    if (bsa_unpacked_size(archive, &match.entry, &size, reason, sizeof reason)
        != BSA_OK) {
        bsa_close(archive);
        return;
    }
    uint8_t *bytes = malloc(size ? size : 1);
    size_t got = 0;
    int ok = bytes && bsa_read(archive, &match.entry, bytes, size, &got,
                               reason, sizeof reason) == BSA_OK && got == size;
    bsa_close(archive);
    if (!ok) {
        free(bytes);
        return;
    }
    static MenuSwfShapeResult result;
    MenuSwfStatus st = menu_swf_shape_summary(bytes, got, &result, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "real gradient summary");
    if (st == MENU_SWF_OK) {
        unsigned solid = 0, gradients = 0, bitmaps = 0;
        for (uint32_t i = 0; i < result.stored_shapes; ++i) {
            solid += result.shapes[i].solid_fills;
            gradients += result.shapes[i].gradient_fills;
            bitmaps += result.shapes[i].bitmap_fills;
        }
        printf("real: fills solid=%u gradient=%u bitmap=%u\n",
               solid, gradients, bitmaps);
        check(solid == 40 && gradients == 0 && bitmaps == 0,
              "real StartMenu still has 40 solid fills and no gradients");
    }
    free(bytes);
}

int main(int argc, char **argv) {
    test_linear();
    test_radial();
    test_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF gradients: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
