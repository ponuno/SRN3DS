/* Host test: flattened SWF display list / sprite nesting. */

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

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
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

static size_t matrix_bytes(uint8_t *out, size_t cap, int32_t sx, int32_t sy,
                           int32_t tx, int32_t ty) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 1);
    bw_put(&w, 20, 5);
    bw_signed(&w, sx, 20);
    bw_signed(&w, sy, 20);
    bw_put(&w, 0, 1); /* no rotate/skew */
    bw_put(&w, 20, 5);
    bw_signed(&w, tx, 20);
    bw_signed(&w, ty, 20);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t cxform_bytes(uint8_t *out, size_t cap, int32_t mult0,
                           int32_t add0) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 1); /* has add */
    bw_put(&w, 1, 1); /* has mult */
    bw_put(&w, 10, 4);
    for (unsigned i = 0; i < 4; ++i) bw_signed(&w, i ? 256 : mult0, 10);
    for (unsigned i = 0; i < 4; ++i) bw_signed(&w, i ? 0 : add0, 10);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t place2(uint8_t *out, size_t cap, uint16_t depth,
                     uint16_t character, int32_t sx, int32_t sy, int32_t tx,
                     int32_t ty, int has_color, int32_t mult0, int32_t add0,
                     int has_clip, uint16_t clip, const char *name) {
    size_t at = 0;
    uint8_t flags = 0x02 | 0x04; /* character + matrix */
    if (has_color) flags |= 0x08;
    if (name) flags |= 0x20;
    if (has_clip) flags |= 0x40;
    if (at >= cap) return 0;
    out[at++] = flags;
    if (at + 2 > cap) return 0;
    put_u16(out + at, depth);
    at += 2;
    put_u16(out + at, character);
    at += 2;
    size_t n = matrix_bytes(out + at, cap - at, sx, sy, tx, ty);
    at += n;
    if (has_color) {
        n = cxform_bytes(out + at, cap - at, mult0, add0);
        at += n;
    }
    if (name) {
        n = strlen(name) + 1;
        if (at + n > cap) return 0;
        memcpy(out + at, name, n);
        at += n;
    }
    if (has_clip) {
        if (at + 2 > cap) return 0;
        put_u16(out + at, clip);
        at += 2;
    }
    return at;
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        out[at] = (uint8_t)len;
        out[at + 1] = (uint8_t)(len >> 8);
        out[at + 2] = (uint8_t)(len >> 16);
        out[at + 3] = (uint8_t)(len >> 24);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t make_sprite(uint8_t *out, size_t cap, uint16_t id,
                          const uint8_t *body, size_t body_len) {
    if (cap < 4 + body_len) return 0;
    put_u16(out, id);
    put_u16(out + 2, 1);
    memcpy(out + 4, body, body_len);
    return 4 + body_len;
}

static size_t make_fws(uint8_t *out, size_t cap, const uint8_t *body,
                       size_t body_len) {
    if (cap < 8 + body_len) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    out[4] = (uint8_t)(8 + body_len);
    out[5] = (uint8_t)((8 + body_len) >> 8);
    out[6] = (uint8_t)((8 + body_len) >> 16);
    out[7] = (uint8_t)((8 + body_len) >> 24);
    memcpy(out + 8, body, body_len);
    return 8 + body_len;
}

static size_t add_frame_header(uint8_t *body, size_t cap) {
    if (cap < 5) return 0;
    body[0] = 0;
    put_u16(body + 1, 0x1e00);
    put_u16(body + 3, 1);
    return 5;
}

static size_t build_nested(uint8_t *file, size_t cap) {
    uint8_t file_body[8192], sprite10[2048], sprite11[2048], top[4096];
    uint8_t tag[512], payload[256];
    size_t s10 = 0, s11 = 0, t = 0;

    size_t plen = place2(payload, sizeof payload, 1, 100, 65536, 65536, 10,
                         20, 1, 128, 5, 0, 0, NULL);
    s10 = put_tag(sprite10, sizeof sprite10, s10, 26, payload,
                  (uint32_t)plen);
    s10 = put_tag(sprite10, sizeof sprite10, s10, 1, NULL, 0);
    s10 = put_tag(sprite10, sizeof sprite10, s10, 0, NULL, 0);

    plen = place2(payload, sizeof payload, 1, 10, 65536, 65536, 5, 0, 0, 0,
                  0, 0, 0, NULL);
    s11 = put_tag(sprite11, sizeof sprite11, s11, 26, payload,
                  (uint32_t)plen);
    s11 = put_tag(sprite11, sizeof sprite11, s11, 1, NULL, 0);
    s11 = put_tag(sprite11, sizeof sprite11, s11, 0, NULL, 0);

    size_t n = make_sprite(tag, sizeof tag, 10, sprite10, s10);
    t = put_tag(top, sizeof top, t, 39, tag, (uint32_t)n);
    n = make_sprite(tag, sizeof tag, 11, sprite11, s11);
    t = put_tag(top, sizeof top, t, 39, tag, (uint32_t)n);

    plen = place2(payload, sizeof payload, 1, 100, 65536, 65536, 0, 0, 0, 0,
                  0, 0, 0, "leaf");
    t = put_tag(top, sizeof top, t, 26, payload, (uint32_t)plen);
    plen = place2(payload, sizeof payload, 2, 11, 131072, 131072, 100, 0,
                  1, 128, 10, 0, 0, NULL);
    t = put_tag(top, sizeof top, t, 26, payload, (uint32_t)plen);
    plen = place2(payload, sizeof payload, 3, 100, 65536, 65536, 0, 0, 0, 0,
                  0, 1, 5, NULL);
    t = put_tag(top, sizeof top, t, 26, payload, (uint32_t)plen);
    plen = place2(payload, sizeof payload, 4, 100, 65536, 65536, 0, 0, 0, 0,
                  0, 0, 0, NULL);
    t = put_tag(top, sizeof top, t, 26, payload, (uint32_t)plen);
    uint8_t rm[2];
    put_u16(rm, 4);
    t = put_tag(top, sizeof top, t, 28, rm, 2);
    t = put_tag(top, sizeof top, t, 1, NULL, 0);
    t = put_tag(top, sizeof top, t, 0, NULL, 0);

    size_t b = add_frame_header(file_body, sizeof file_body);
    memcpy(file_body + b, top, t);
    b += t;
    return make_fws(file, cap, file_body, b);
}

static void test_nested(void) {
    uint8_t file[16384];
    size_t flen = build_nested(file, sizeof file);
    check(flen > 0, "nested fixture builds");
    MenuSwfDisplayListRecord rec[16];
    MenuSwfDisplayListResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_display_list_build(
        file, flen, rec, 16, &r, reason, sizeof reason);
    check(st == MENU_SWF_OK, "nested display list");
    check(r.record_count == 5 && r.top_objects == 3 &&
              r.sprite_count == 2 && r.sprite_placements == 2,
          "nested counts");
    check(r.max_sprite_depth == 2, "nested sprite depth");
    check(rec[0].depth == 1 && rec[0].character_id == 100 &&
              rec[0].has_name && !strcmp(rec[0].name, "leaf"),
          "top depth1 leaf");
    check(rec[1].depth == 2 && rec[1].character_id == 11 &&
              rec[1].is_sprite && rec[1].matrix.m00 == 131072 &&
              rec[1].matrix.tx == 100,
          "top depth2 sprite transform");
    check(rec[2].character_id == 10 && rec[2].is_sprite &&
              rec[2].sprite_depth == 1 && rec[2].parent_sprite_id == 11,
          "nested sprite record");
    check(rec[3].character_id == 100 && rec[3].sprite_depth == 2 &&
              rec[3].matrix.m00 == 131072 && rec[3].matrix.tx == 130 &&
              rec[3].matrix.ty == 40,
          "nested accumulated matrix");
    check(rec[3].color.mult[0] == 64 && rec[3].color.add[0] == 12,
          "nested accumulated color");
    check(rec[4].depth == 3 && rec[4].has_clip_depth &&
              rec[4].clip_depth == 5,
          "clip depth record");
    check(r.removals == 1, "RemoveObject2 counted");
    int removed = 1;
    for (uint32_t i = 0; i < r.record_count; ++i)
        if (rec[i].depth == 4) removed = 0;
    check(removed, "RemoveObject2 removed depth4");
}

static void test_cycle_and_errors(void) {
    uint8_t file[8192], body[4096], sprite[1024], tag[256], payload[128];
    size_t s = 0, b = add_frame_header(body, sizeof body);
    size_t plen = place2(payload, sizeof payload, 1, 12, 65536, 65536, 0, 0,
                         0, 0, 0, 0, 0, NULL);
    s = put_tag(sprite, sizeof sprite, s, 26, payload, (uint32_t)plen);
    s = put_tag(sprite, sizeof sprite, s, 1, NULL, 0);
    s = put_tag(sprite, sizeof sprite, s, 0, NULL, 0);
    size_t n = make_sprite(tag, sizeof tag, 12, sprite, s);
    b = put_tag(body, sizeof body, b, 39, tag, (uint32_t)n);
    plen = place2(payload, sizeof payload, 1, 12, 65536, 65536, 0, 0, 0, 0,
                  0, 0, 0, NULL);
    b = put_tag(body, sizeof body, b, 26, payload, (uint32_t)plen);
    b = put_tag(body, sizeof body, b, 1, NULL, 0);
    b = put_tag(body, sizeof body, b, 0, NULL, 0);
    size_t flen = make_fws(file, sizeof file, body, b);
    MenuSwfDisplayListRecord rec[8];
    MenuSwfDisplayListResult r;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_display_list_build(
        file, flen, rec, 8, &r, reason, sizeof reason);
    check(st == MENU_SWF_ERR_TAG && strstr(reason, "cycle"),
          "sprite cycle refused");

    uint8_t badbody[64], bad[128];
    size_t bb = add_frame_header(badbody, sizeof badbody);
    uint8_t short_place[] = {0};
    bb = put_tag(badbody, sizeof badbody, bb, 26, short_place, 1);
    bb = put_tag(badbody, sizeof badbody, bb, 0, NULL, 0);
    size_t blen = make_fws(bad, sizeof bad, badbody, bb);
    st = menu_swf_display_list_build(bad, blen, rec, 8, &r, reason,
                                     sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT, "broken PlaceObject2 refused");

    st = menu_swf_display_list_build(file, flen, rec, 1, &r, reason,
                                     sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && r.truncated,
          "record capacity overflow refused");
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
    MenuSwfDisplayListRecord *rec =
        (MenuSwfDisplayListRecord *)malloc(cap * sizeof *rec);
    MenuSwfDisplayListResult r;
    check(rec != NULL, "real record allocation");
    if (rec) {
        MenuSwfStatus st = menu_swf_display_list_build(
            bytes, got, rec, cap, &r, reason, sizeof reason);
        check(st == MENU_SWF_OK, "real display list");
        if (st == MENU_SWF_OK) {
            printf("real: records=%u sprites=%u sprite_records=%u top=%u "
                   "placements=%u removals=%u max_depth=%u\n",
                   r.record_count, r.sprite_count, r.sprite_placements,
                   r.top_objects, r.placement_tags, r.removals,
                   r.max_sprite_depth);
            check(r.sprite_count == 71, "real 71 sprite definitions");
            check(r.top_objects == 1, "real one top object");
            check(r.record_count > 0 && r.sprite_placements > 0,
                  "real flattened records");
            check(r.max_sprite_depth <= MENU_SWF_DISPLAY_LIST_MAX_DEPTH,
                  "real sprite depth bounded");
        } else {
            printf("real: display list failed: %s\n", reason);
        }
        free(rec);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF display list tests\n");
    test_nested();
    test_cycle_and_errors();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF display list: %d checks, %d failures\n", checks,
           failures);
    return failures ? 1 : 0;
}
