/* Single-glyph extraction from the real fonts_en.swf library.
 *
 * The shipped font library contains several faces.  A lookup of one glyph in
 * one face must not fail because an unrelated face has a complex or malformed
 * outline.  menu_swf_tess_glyph() therefore filters to the requested
 * (font_id, code) before decoding contours.  This test exercises that path on
 * the retail Futura CondensedLight face.
 */

#include "bsa_reader.h"
#include "menu_swf_display.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"

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

typedef struct {
    MenuSwfTessVertex vertices[MENU_SWF_TESS_MAX_VERTICES];
    uint32_t indices[MENU_SWF_TESS_MAX_INDICES];
    uint16_t fills[MENU_SWF_TESS_MAX_TRIANGLES];
    MenuSwfTessLine lines[MENU_SWF_TESS_MAX_LINES];
    MenuSwfTessEdge edges[MENU_SWF_TESS_MAX_EDGES];
    uint32_t active[MENU_SWF_TESS_MAX_EDGES];
    int32_t bands[MENU_SWF_TESS_MAX_BANDS];
    MenuSwfTessFill fill_styles[MENU_SWF_SHAPE_MAX_STYLES];
} TessFixture;

static TessFixture tess_fixture;

static void init(MenuSwfTessMesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = tess_fixture.vertices;
    mesh->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
    mesh->indices = tess_fixture.indices;
    mesh->index_capacity = MENU_SWF_TESS_MAX_INDICES;
    mesh->triangle_fills = tess_fixture.fills;
    mesh->triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
    mesh->lines = tess_fixture.lines;
    mesh->line_capacity = MENU_SWF_TESS_MAX_LINES;
    mesh->edges = tess_fixture.edges;
    mesh->edge_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->active = tess_fixture.active;
    mesh->active_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->bands = tess_fixture.bands;
    mesh->band_capacity = MENU_SWF_TESS_MAX_BANDS;
    mesh->fills = tess_fixture.fill_styles;
    mesh->fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
}

static uint8_t *read_bsa(const char *archive_path, const char *relative,
                         size_t *out_size) {
    BsaArchive *archive = NULL;
    if (bsa_open_path(archive_path, NULL, &archive, NULL, 0) != BSA_OK)
        return NULL;
    BsaMatch match;
    uint8_t *bytes = NULL;
    if (bsa_find(archive, relative, &match) == BSA_OK) {
        uint32_t size = 0;
        char reason[BSA_REASON_MAX];
        if (bsa_unpacked_size(archive, &match.entry, &size, reason,
                              sizeof reason) == BSA_OK) {
            bytes = malloc(size ? size : 1);
            size_t got = 0;
            if (bytes && (bsa_read(archive, &match.entry, bytes, size, &got,
                                   reason, sizeof reason) != BSA_OK ||
                          got != size)) {
                free(bytes);
                bytes = NULL;
            }
            if (bytes) *out_size = size;
        }
    }
    bsa_close(archive);
    return bytes;
}

static void test_real(const char *data_dir) {
    char path[1024];
    char reason[BSA_REASON_MAX];
    snprintf(path, sizeof path, "%s/Skyrim - Interface.bsa", data_dir);
    size_t size = 0;
    uint8_t *fonts = read_bsa(path, "interface/fonts_en.swf", &size);
    check(fonts != NULL, "retail fonts_en.swf loaded");
    if (!fonts) return;

    MenuSwfFontSummary *items = calloc(32, sizeof *items);
    check(items != NULL, "font candidate allocation");
    if (!items) {
        free(fonts);
        return;
    }
    MenuSwfFontList list = {items, 32, 0, 0, 0};
    MenuSwfStatus st = menu_swf_display_collect_fonts(fonts, size, &list,
                                                       reason, sizeof reason);
    check(st == MENU_SWF_OK && list.count == 10 && list.skipped == 2,
          "fonts_en font census");
    const MenuSwfFontSummary *face = menu_swf_display_find_font(&list, 1);
    check(face && !strcmp(face->name, "Futura CondensedLight") &&
              face->glyph_count == 187,
          "Futura CondensedLight id 1 present");
    if (!face) {
        free(items);
        free(fonts);
        return;
    }

    MenuSwfTessMesh mesh;
    init(&mesh);
    st = menu_swf_tess_glyph(fonts, size, 1, 'S', MENU_SWF_TESS_NON_ZERO,
                             &mesh, reason, sizeof reason);
    check(st == MENU_SWF_OK && mesh.vertex_count > 0 &&
              mesh.triangle_count > 0 && mesh.index_count ==
              mesh.triangle_count * 3u,
          "real 'S' outline tessellates despite unrelated complex faces");
    check(mesh.has_bounds && mesh.bounds.xmin <= mesh.bounds.xmax &&
              mesh.bounds.ymin <= mesh.bounds.ymax,
          "real glyph bounds are ordered");
    printf("real: font=1 code=S vertices=%u indices=%u triangles=%u "
           "bounds=%d,%d,%d,%d\n",
           mesh.vertex_count, mesh.index_count, mesh.triangle_count,
           mesh.bounds.xmin, mesh.bounds.xmax, mesh.bounds.ymin,
           mesh.bounds.ymax);

    MenuSwfTessMesh again;
    init(&again);
    check(menu_swf_tess_glyph(fonts, size, 1, 0x1234,
                              MENU_SWF_TESS_NON_ZERO, &again, reason,
                              sizeof reason) == MENU_SWF_ERR_ARGUMENT &&
              strstr(reason, "glyph font 1 code 4660 not found"),
          "absent glyph code is a named refusal");

    MenuSwfTessMesh other;
    init(&other);
    st = menu_swf_tess_glyph(fonts, size, 3, 'S', MENU_SWF_TESS_NON_ZERO,
                             &other, reason, sizeof reason);
    check(st == MENU_SWF_OK && other.vertex_count > 0,
          "the same code in Futura Condensed id 3 tessellates");

    free(items);
    free(fonts);
}

int main(int argc, char **argv) {
    printf("menu swf glyph tests\n");
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu swf glyph: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
