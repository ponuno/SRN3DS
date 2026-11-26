/* Host test: bounded A8 glyph-atlas packing and composition. */

#include "menu_swf_atlas.h"
#include "menu_swf_raster.h"
#include "menu_swf_tess.h"
#include "menu_swf_shape.h"
#include "menu_swf_display.h"
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

typedef struct {
    uint8_t data[16 * 16];
} SynthBitmap;

static const uint32_t synth_w[12] = {5, 10, 3, 8, 16, 7, 12, 4, 9, 6, 14, 2};
static const uint32_t synth_h[12] = {6, 4, 12, 9, 5, 15, 3, 8, 11, 7, 2, 10};

static void fill_synth(SynthBitmap *b, uint32_t w, uint32_t h,
                       uint16_t glyph_index) {
    memset(b->data, 0, sizeof b->data);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            b->data[y * 16 + x] =
                (uint8_t)(glyph_index * 13u + x * 3u + y + 1u);
}

static void build_synth(SynthBitmap *bmp, MenuSwfAtlasGlyph *g,
                        uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        fill_synth(&bmp[i], synth_w[i], synth_h[i], (uint16_t)(100 + i));
        g[i].pixels = bmp[i].data;
        g[i].stride = 16;
        g[i].width = synth_w[i];
        g[i].height = synth_h[i];
        g[i].glyph_index = (uint16_t)(100 + i);
    }
}

static int rect_overlap(const MenuSwfAtlasEntry *a,
                        const MenuSwfAtlasEntry *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static void test_synthetic(void) {
    SynthBitmap bmp[12];
    MenuSwfAtlasGlyph glyphs[12];
    MenuSwfAtlasEntry entries[12], entries2[12], entries_rev[12];
    MenuSwfAtlasResult r, r2;
    uint8_t storage1[2 + 66 * 64];
    uint8_t storage2[2 + 66 * 64];
    uint8_t rev_storage[2 + 66 * 64];
    char reason[BSA_REASON_MAX];

    build_synth(bmp, glyphs, 12);
    memset(storage1, 0xAA, sizeof storage1);
    memset(storage2, 0xAA, sizeof storage2);
    MenuSwfAtlasTarget t;
    t.width = 64;
    t.height = 64;
    t.stride = 66;
    t.pixels = storage1 + 1;
    MenuSwfAtlasStatus st = menu_swf_atlas_pack(glyphs, 12, &t, entries, 12,
                                                &r, reason, sizeof reason);
    check(st == MENU_SWF_ATLAS_OK, "synthetic atlas pack");
    check(r.placed_glyphs == 12 && r.empty_glyphs == 0,
          "synthetic glyph counts");

    uint32_t expected_occupied = 0;
    for (unsigned i = 0; i < 12; ++i)
        expected_occupied += synth_w[i] * synth_h[i];
    check(r.occupied_pixels == expected_occupied,
          "synthetic occupied pixels");
    check(r.used_height > 0 && r.used_height <= 64, "synthetic used height");
    check(r.occupancy_16 ==
              (uint32_t)((uint64_t)expected_occupied * 65536u /
                         (64ull * r.used_height)),
          "synthetic occupancy");

    int overlap = 0, inside = 1, uv_ok = 1;
    for (unsigned i = 0; i < 12; ++i) {
        MenuSwfAtlasEntry *e = &entries[i];
        if (!e->has_rect || e->w != synth_w[i] || e->h != synth_h[i])
            inside = 0;
        if (e->x + e->w > 64 || e->y + e->h > 64) inside = 0;
        uint32_t u = (uint32_t)(((uint64_t)e->x << 16) / 64);
        uint32_t v = (uint32_t)(((uint64_t)e->y << 16) / 64);
        uint32_t u2 = (uint32_t)(((uint64_t)(e->x + e->w) << 16) / 64);
        uint32_t v2 = (uint32_t)(((uint64_t)(e->y + e->h) << 16) / 64);
        if (e->u != u || e->v != v || e->uw != u2 - u ||
            e->vh != v2 - v)
            uv_ok = 0;
        for (unsigned j = i + 1; j < 12; ++j)
            if (rect_overlap(e, &entries[j])) overlap = 1;
    }
    check(!overlap, "synthetic rects do not overlap");
    check(inside, "synthetic rects inside atlas");
    check(uv_ok, "synthetic exact UVs");

    /* Composition: every glyph byte lands in its rect, padding stays zero. */
    int copied = 1, padding_zero = 1;
    uint8_t *atlas = storage1 + 1;
    for (unsigned i = 0; i < 12; ++i) {
        MenuSwfAtlasEntry *e = &entries[i];
        for (uint32_t y = 0; y < e->h; ++y)
            for (uint32_t x = 0; x < e->w; ++x)
                if (atlas[(e->y + y) * 66 + e->x + x] !=
                    bmp[i].data[y * 16 + x])
                    copied = 0;
    }
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            int in_rect = 0;
            for (unsigned i = 0; i < 12; ++i) {
                MenuSwfAtlasEntry *e = &entries[i];
                if (x >= e->x && x < e->x + e->w && y >= e->y &&
                    y < e->y + e->h)
                    in_rect = 1;
            }
            if (!in_rect && atlas[y * 66 + x] != 0) padding_zero = 0;
        }
    }
    check(copied, "synthetic composition copies coverage");
    check(padding_zero, "synthetic padding is zero");
    check(storage1[0] == 0xAA && storage1[sizeof storage1 - 1] == 0xAA &&
              atlas[64] == 0xAA && atlas[65] == 0xAA &&
              atlas[63 * 66 + 64] == 0xAA &&
              atlas[63 * 66 + 65] == 0xAA,
          "synthetic guards and stride padding untouched");

    /* Same inputs into a second atlas: byte-identical and same entries. */
    t.pixels = storage2 + 1;
    st = menu_swf_atlas_pack(glyphs, 12, &t, entries2, 12, &r2, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_OK, "synthetic repack");
    check(memcmp(&r, &r2, sizeof r) == 0 &&
              memcmp(entries, entries2, sizeof entries) == 0 &&
              memcmp(storage1, storage2, sizeof storage1) == 0,
          "synthetic deterministic repack");

    /* Reversed input order must produce the same physical atlas. */
    MenuSwfAtlasGlyph rev[12];
    for (unsigned i = 0; i < 12; ++i) rev[i] = glyphs[11 - i];
    memset(rev_storage, 0xAA, sizeof rev_storage);
    t.pixels = rev_storage + 1;
    st = menu_swf_atlas_pack(rev, 12, &t, entries_rev, 12, &r2, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_OK, "synthetic reversed pack");
    check(memcmp(storage1, rev_storage, sizeof storage1) == 0,
          "synthetic order-independent atlas");
    int rev_ok = 1;
    for (unsigned i = 0; i < 12; ++i) {
        unsigned j = 11 - i;
        if (entries[i].x != entries_rev[j].x ||
            entries[i].y != entries_rev[j].y ||
            entries[i].w != entries_rev[j].w ||
            entries[i].h != entries_rev[j].h)
            rev_ok = 0;
    }
    check(rev_ok, "synthetic order-independent rects");
}

static void test_empty_and_refusals(void) {
    SynthBitmap bmp[3];
    MenuSwfAtlasGlyph glyphs[3];
    MenuSwfAtlasEntry entries[3];
    MenuSwfAtlasResult r;
    uint8_t atlas[64];
    char reason[BSA_REASON_MAX];
    MenuSwfAtlasTarget t = {8, 8, 8, atlas};

    fill_synth(&bmp[0], 2, 2, 1);
    glyphs[0] = (MenuSwfAtlasGlyph){bmp[0].data, 16, 2, 2, 1};
    glyphs[1] = (MenuSwfAtlasGlyph){NULL, 0, 0, 0, 2};
    glyphs[2] = (MenuSwfAtlasGlyph){NULL, 0, 0, 0, 3};
    MenuSwfAtlasStatus st = menu_swf_atlas_pack(glyphs, 3, &t, entries, 3,
                                                &r, reason, sizeof reason);
    check(st == MENU_SWF_ATLAS_OK, "empty glyphs accepted");
    check(r.placed_glyphs == 1 && r.empty_glyphs == 2,
          "empty glyph counts");
    check(entries[1].w == 0 && entries[1].h == 0 &&
              entries[1].u == 0 && entries[1].v == 0 &&
              entries[1].uw == 0 && entries[1].vh == 0 &&
              !entries[1].has_rect && entries[1].glyph_index == 2,
          "empty glyph zero rect/UV");
    check(entries[2].w == 0 && !entries[2].has_rect,
          "second empty glyph");

    /* Too-large glyph: no atlas or entry mutation. */
    memset(atlas, 0x5A, sizeof atlas);
    memset(entries, 0xCC, sizeof entries);
    glyphs[0] = (MenuSwfAtlasGlyph){bmp[0].data, 16, 9, 1, 7};
    st = menu_swf_atlas_pack(glyphs, 1, &t, entries, 3, &r, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_ERR_TOO_LARGE, "too-large glyph refused");
    int unchanged = 1;
    for (unsigned i = 0; i < sizeof atlas; ++i)
        if (atlas[i] != 0x5A) unchanged = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (entries[i].x != 0xCCCCCCCCu) unchanged = 0;
    check(unchanged, "too-large leaves atlas and entries unchanged");

    /* Shelf overflow: two 6x6 glyphs do not fit in 8x8. */
    SynthBitmap big[2];
    fill_synth(&big[0], 6, 6, 8);
    fill_synth(&big[1], 6, 6, 9);
    glyphs[0] = (MenuSwfAtlasGlyph){big[0].data, 16, 6, 6, 8};
    glyphs[1] = (MenuSwfAtlasGlyph){big[1].data, 16, 6, 6, 9};
    memset(atlas, 0x5A, sizeof atlas);
    memset(entries, 0xCC, sizeof entries);
    st = menu_swf_atlas_pack(glyphs, 2, &t, entries, 3, &r, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_ERR_OVERFLOW, "shelf overflow refused");
    unchanged = 1;
    for (unsigned i = 0; i < sizeof atlas; ++i)
        if (atlas[i] != 0x5A) unchanged = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (entries[i].x != 0xCCCCCCCCu) unchanged = 0;
    check(unchanged, "overflow leaves atlas and entries unchanged");

    glyphs[0] = (MenuSwfAtlasGlyph){big[0].data, 16, 2, 2, 1};
    st = menu_swf_atlas_pack(glyphs, MENU_SWF_ATLAS_MAX_GLYPHS + 1, &t,
                             entries, 3, &r, reason, sizeof reason);
    check(st == MENU_SWF_ATLAS_ERR_TOO_MANY, "too many glyphs refused");

    st = menu_swf_atlas_pack(glyphs, 2, &t, entries, 1, &r, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_ERR_TOO_MANY, "entry capacity refused");

    glyphs[0] = (MenuSwfAtlasGlyph){big[0].data, 1, 2, 2, 1};
    st = menu_swf_atlas_pack(glyphs, 1, &t, entries, 3, &r, reason,
                             sizeof reason);
    check(st == MENU_SWF_ATLAS_ERR_ARGUMENT, "short source stride refused");
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

static uint64_t rect_sum(const uint8_t *atlas, uint32_t stride,
                         const MenuSwfAtlasEntry *e) {
    uint64_t sum = 0;
    for (uint32_t y = 0; y < e->h; ++y)
        for (uint32_t x = 0; x < e->w; ++x)
            sum += atlas[(e->y + y) * stride + e->x + x];
    return sum;
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
    MenuSwfAtlasResult ar;
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
            MenuSwfTessMesh mesh;
            uint8_t rasters[4][64 * 64];
            uint64_t raster_sums[4] = {0, 0, 0, 0};
            MenuSwfAtlasGlyph glyphs[4];
            MenuSwfAtlasEntry entries[4];
            int ready = 1;
            for (unsigned i = 0; i < 4; ++i) {
                uint16_t code = (uint16_t)(33 + i);
                tess_fixture_init(&fx, &mesh);
                st = menu_swf_tess_glyph(bytes, got, font->font_id, code,
                                         MENU_SWF_TESS_NON_ZERO, &mesh,
                                         reason, sizeof reason);
                if (st != MENU_SWF_OK) {
                    printf("real: glyph %u tess failed: %s (%s)\n", code,
                           menu_swf_status_name(st), reason);
                    ready = 0;
                    break;
                }
                double bw = mesh.bounds.xmax - mesh.bounds.xmin;
                double bh = mesh.bounds.ymax - mesh.bounds.ymin;
                double scale = (64 - 2.0) / bw;
                double sy = (64 - 2.0) / bh;
                if (sy < scale) scale = sy;
                MenuSwfRasterTransform xf;
                memset(&xf, 0, sizeof xf);
                xf.m00 = scale;
                xf.m11 = scale;
                xf.tx = (64 - bw * scale) / 2.0 -
                        mesh.bounds.xmin * scale;
                xf.ty = (64 - bh * scale) / 2.0 -
                        mesh.bounds.ymin * scale;
                MenuSwfRasterTarget rt = {64, 64, 64, rasters[i], 0, 0};
                MenuSwfRasterResult rr;
                MenuSwfRasterStatus rs = menu_swf_raster_mesh(
                    &mesh, &xf, &rt, &rr, reason, sizeof reason);
                if (rs != MENU_SWF_RASTER_OK) {
                    printf("real: glyph %u raster failed: %s (%s)\n", code,
                           menu_swf_raster_status_name(rs), reason);
                    ready = 0;
                    break;
                }
                raster_sums[i] = rr.coverage_sum;
                glyphs[i].pixels = rasters[i] + rr.y0 * 64 + rr.x0;
                glyphs[i].stride = 64;
                glyphs[i].width = rr.x1 - rr.x0;
                glyphs[i].height = rr.y1 - rr.y0;
                glyphs[i].glyph_index = code;
            }
            check(ready, "real glyph raster inputs");
            if (ready) {
                static uint8_t atlas[256 * 256];
                MenuSwfAtlasTarget at = {256, 256, 256, atlas};
                MenuSwfAtlasStatus as = menu_swf_atlas_pack(
                    glyphs, 4, &at, entries, 4, &ar, reason, sizeof reason);
                check(as == MENU_SWF_ATLAS_OK, "real atlas pack");
                if (as == MENU_SWF_ATLAS_OK) {
                    printf("real: atlas used_height=%u occupied=%u "
                           "occupancy16=%u placed=%u empty=%u\n",
                           ar.used_height, ar.occupied_pixels,
                           ar.occupancy_16, ar.placed_glyphs,
                           ar.empty_glyphs);
                    int sums_ok = 1;
                    for (unsigned i = 0; i < 4; ++i) {
                        uint64_t s = rect_sum(atlas, 256, &entries[i]);
                        printf("real: glyph %u rect=%u,%u,%u,%u "
                               "sum=%llu raster=%llu\n",
                               entries[i].glyph_index, entries[i].x,
                               entries[i].y, entries[i].w, entries[i].h,
                               (unsigned long long)s,
                               (unsigned long long)raster_sums[i]);
                        if (s != raster_sums[i]) sums_ok = 0;
                    }
                    check(sums_ok, "real atlas sums match raster");
                    check(ar.placed_glyphs == 4 && ar.empty_glyphs == 0,
                          "real atlas glyph counts");
                    check(ar.used_height > 0 && ar.used_height <= 256,
                          "real atlas used height");
                } else {
                    printf("real: atlas failed: %s\n", reason);
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
    printf("menu SWF atlas tests\n");
    test_synthetic();
    test_empty_and_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF atlas: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
