/* Host test: clip-depth masks over a flattened SWF display list. */

#include "menu_swf_clip.h"
#include "menu_swf_display_list.h"
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

static void make_record(MenuSwfDisplayListRecord *r, uint16_t depth,
                        int clip, uint16_t clip_depth, int32_t tx,
                        int32_t ty) {
    memset(r, 0, sizeof *r);
    r->depth = depth;
    r->character_id = (uint16_t)(100 + depth);
    r->has_character = 1;
    r->matrix.m00 = 65536;
    r->matrix.m11 = 65536;
    r->matrix.tx = tx;
    r->matrix.ty = ty;
    if (clip) {
        r->has_clip_depth = 1;
        r->clip_depth = clip_depth;
    }
}

static void make_shape(MenuSwfClipShape *s, int32_t x0, int32_t x1,
                       int32_t y0, int32_t y1, int rect) {
    memset(s, 0, sizeof *s);
    s->has_bounds = 1;
    s->is_rect = (uint8_t)rect;
    s->bounds.xmin = x0;
    s->bounds.xmax = x1;
    s->bounds.ymin = y0;
    s->bounds.ymax = y1;
}

static void test_masks(void) {
    MenuSwfDisplayListRecord rec[7];
    MenuSwfClipShape shape[7];
    MenuSwfClipMask masks[16];
    MenuSwfClipRecord out[7];
    MenuSwfClipResult r;
    char reason[BSA_REASON_MAX];

    make_record(&rec[0], 1, 1, 6, 0, 0);
    make_record(&rec[1], 2, 1, 5, 0, 0);
    make_record(&rec[2], 3, 1, 4, 0, 0);
    make_record(&rec[3], 4, 0, 0, 0, 0);
    make_record(&rec[4], 5, 0, 0, 0, 0);
    make_record(&rec[5], 5, 0, 0, 0, 0);
    make_record(&rec[6], 7, 0, 0, 100, 200);
    make_shape(&shape[0], 0, 100, 0, 100, 1);
    make_shape(&shape[1], 20, 80, 20, 80, 1);
    make_shape(&shape[2], 40, 60, 40, 60, 1);
    make_shape(&shape[3], 30, 70, 30, 70, 1);
    make_shape(&shape[4], 30, 70, 30, 70, 1);
    make_shape(&shape[5], 200, 300, 200, 300, 1);
    make_shape(&shape[6], 0, 10, 0, 10, 1);

    MenuSwfClipStatus st = menu_swf_clip_build(
        rec, 7, shape, 7, masks, 16, out, 7, &r, reason, sizeof reason);
    check(st == MENU_SWF_CLIP_OK, "synthetic clip build");
    check(r.record_count == 7 && r.mask_count == 3 &&
              r.max_active_masks == 3 && r.clipped_records == 5,
          "synthetic clip counts");
    check(out[0].mask_count == 0 && out[6].mask_count == 0,
          "maskless records");
    check(out[3].mask_count == 3 && out[3].clip_rect.xmin == 40 &&
              out[3].clip_rect.xmax == 60 && out[3].clip_rect.ymin == 40 &&
              out[3].clip_rect.ymax == 60 && !out[3].fully_clipped,
          "nested mask intersection");
    check(out[4].mask_count == 2 && out[4].clip_rect.xmin == 30 &&
              out[4].clip_rect.xmax == 70 && !out[4].fully_clipped,
          "two-mask intersection");
    check(out[5].mask_count == 2 && out[5].fully_clipped,
          "fully clipped record");
    check(out[6].has_rect && out[6].rect.xmin == 100 &&
              out[6].rect.ymin == 200 && out[6].rect.xmax == 110,
          "record matrix screen rect");
    check(masks[0].mask_record_index == 0 && masks[0].rect.xmin == 0 &&
              masks[0].rect.xmax == 100 && masks[2].rect.xmin == 40,
          "mask rects in record order");

    MenuSwfClipPoint q[4];
    st = menu_swf_clip_mask_quad(&rec[0], &shape[0], q, reason,
                                 sizeof reason);
    check(st == MENU_SWF_CLIP_OK && q[0].x == 0 && q[1].x == 100 &&
              q[2].y == 100,
          "mask quad preparation");

    MenuSwfClipRect a = {0, 10, 0, 10}, b = {5, 20, 5, 20}, c;
    check(menu_swf_clip_rect_intersect(&a, &b, &c) &&
              c.xmin == 5 && c.xmax == 10 && c.ymin == 5 && c.ymax == 10,
          "rect intersect");
    MenuSwfClipRect d = {20, 30, 0, 10};
    check(!menu_swf_clip_rect_intersect(&a, &d, &c), "rect disjoint");
}

static void test_refusals(void) {
    MenuSwfDisplayListRecord rec[12];
    MenuSwfClipShape shape[12];
    MenuSwfClipMask masks[16];
    MenuSwfClipRecord out[12];
    MenuSwfClipResult r;
    char reason[BSA_REASON_MAX];

    for (unsigned i = 0; i < 12; ++i) {
        make_record(&rec[i], (uint16_t)(i + 1), 0, 0, 0, 0);
        make_shape(&shape[i], 0, 10, 0, 10, 1);
    }
    rec[0].has_clip_depth = 1;
    rec[0].clip_depth = 1; /* equal mask/clip depth */
    memset(masks, 0xAA, sizeof masks);
    memset(out, 0xAA, sizeof out);
    MenuSwfClipStatus st = menu_swf_clip_build(
        rec, 2, shape, 2, masks, 16, out, 12, &r, reason, sizeof reason);
    check(st == MENU_SWF_CLIP_ERR_PAIR && reason[0],
          "bad clip pair refused");
    check(masks[0].mask_record_index == 0xAAAAAAAAu &&
              out[0].record_index == 0xAAAAAAAAu,
          "bad pair leaves outputs untouched");

    /* Nine nested masks exceed the limit of eight. */
    for (unsigned i = 0; i < 9; ++i) {
        make_record(&rec[i], (uint16_t)(i + 1), 1, 10, 0, 0);
        make_shape(&shape[i], 0, 100, 0, 100, 1);
    }
    make_record(&rec[9], 10, 0, 0, 0, 0);
    make_shape(&shape[9], 10, 20, 10, 20, 1);
    st = menu_swf_clip_build(rec, 10, shape, 10, masks, 16, out, 12, &r,
                             reason, sizeof reason);
    check(st == MENU_SWF_CLIP_ERR_NESTING, "nested mask limit refused");

    make_shape(&shape[0], 0, 10, 0, 10, 0);
    make_record(&rec[0], 1, 1, 3, 0, 0);
    st = menu_swf_clip_build(rec, 2, shape, 2, masks, 16, out, 12, &r,
                             reason, sizeof reason);
    check(st == MENU_SWF_CLIP_ERR_MASK &&
              strstr(reason, "mask shape tessellation is not implemented"),
          "complex mask refused");

    shape[0].is_rect = 1;
    st = menu_swf_clip_build(rec, 2, shape, 2, masks, 1, out, 12, &r,
                             reason, sizeof reason);
    check(st == MENU_SWF_CLIP_ERR_OVERFLOW, "mask capacity refused");

    st = menu_swf_clip_build(rec, 2, shape, 2, masks, 16, out, 1, &r,
                             reason, sizeof reason);
    check(st == MENU_SWF_CLIP_ERR_OVERFLOW, "record capacity refused");
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

    uint32_t cap = 8192;
    MenuSwfDisplayListRecord *records =
        (MenuSwfDisplayListRecord *)malloc(cap * sizeof *records);
    MenuSwfClipRecord *clips =
        (MenuSwfClipRecord *)malloc(cap * sizeof *clips);
    MenuSwfClipMask *masks = (MenuSwfClipMask *)malloc(256 * sizeof *masks);
    MenuSwfDisplayListResult dr;
    MenuSwfClipResult cr;
    check(records && clips && masks, "real clip allocations");
    if (records && clips && masks) {
        MenuSwfStatus st = menu_swf_display_list_build(
            bytes, got, records, cap, &dr, reason, sizeof reason);
        check(st == MENU_SWF_OK, "real display list for clip");
        if (st == MENU_SWF_OK) {
            MenuSwfClipStatus cs = menu_swf_clip_build(
                records, dr.record_count, NULL, 0, masks, 256, clips, cap,
                &cr, reason, sizeof reason);
            check(cs == MENU_SWF_CLIP_OK, "real clip build");
            if (cs == MENU_SWF_CLIP_OK) {
                printf("real: clip records=%u masks=%u clipped=%u max=%u\n",
                       cr.record_count, cr.mask_count, cr.clipped_records,
                       cr.max_active_masks);
                check(cr.record_count == dr.record_count,
                      "real clip record count");
                check(cr.mask_count == 0, "real StartMenu has no clip pairs");
            } else {
                printf("real: clip failed: %s\n", reason);
            }
        } else {
            printf("real: display list failed: %s\n", reason);
        }
    }
    free(masks);
    free(clips);
    free(records);
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF clip tests\n");
    test_masks();
    test_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF clip: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
