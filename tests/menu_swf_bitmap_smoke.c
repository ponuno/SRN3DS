/* Host test: bounded SWF lossless bitmap decode. */

#include "menu_swf_bitmap.h"
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

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* zlib envelope with one final stored deflate block. */
static size_t zlib_store(uint8_t *out, size_t cap, const uint8_t *src,
                         size_t len) {
    if (len > 65535u || cap < 2u + 5u + len + 4u) return 0;
    size_t at = 0;
    out[at++] = 0x78;
    out[at++] = 0x01;
    out[at++] = 0x01; /* BFINAL, stored */
    out[at++] = (uint8_t)len;
    out[at++] = (uint8_t)(len >> 8);
    uint16_t nlen = (uint16_t)~len;
    out[at++] = (uint8_t)nlen;
    out[at++] = (uint8_t)(nlen >> 8);
    memcpy(out + at, src, len);
    at += len;
    uint32_t ad = adler32(src, len);
    out[at++] = (uint8_t)(ad >> 24);
    out[at++] = (uint8_t)(ad >> 16);
    out[at++] = (uint8_t)(ad >> 8);
    out[at++] = (uint8_t)ad;
    return at;
}

static size_t make_payload(uint8_t *out, size_t cap, uint16_t id,
                           uint8_t format, uint16_t w, uint16_t h,
                           const uint8_t *raw, size_t raw_len) {
    uint8_t z[2048];
    size_t zn = zlib_store(z, sizeof z, raw, raw_len);
    if (!zn || cap < 7 + zn) return 0;
    put_u16(out, id);
    out[2] = format;
    put_u16(out + 3, w);
    put_u16(out + 5, h);
    memcpy(out + 7, z, zn);
    return 7 + zn;
}

static size_t make_palette(uint8_t *out, size_t cap, uint16_t id,
                           uint8_t tag, uint16_t w, uint16_t h,
                           const uint8_t *palette, size_t palette_bytes,
                           const uint8_t *indices, size_t row_bytes) {
    uint8_t raw[1024];
    size_t entries = palette_bytes / (tag == 36 ? 4u : 3u);
    size_t raw_len = palette_bytes + row_bytes * h;
    if (raw_len > sizeof raw || !entries) return 0;
    memcpy(raw, palette, palette_bytes);
    memcpy(raw + palette_bytes, indices, row_bytes * h);
    uint8_t z[2048];
    size_t zn = zlib_store(z, sizeof z, raw, raw_len);
    if (!zn || cap < 8 + zn) return 0;
    put_u16(out, id);
    out[2] = 3;
    put_u16(out + 3, w);
    put_u16(out + 5, h);
    out[7] = (uint8_t)(entries - 1u);
    memcpy(out + 8, z, zn);
    return 8 + zn;
}

static void test_lossless2_argb(void) {
    uint8_t raw[] = {128, 64, 32, 16, 255, 10, 20, 30};
    uint8_t payload[256], rgba[8], scratch[64];
    char reason[BSA_REASON_MAX];
    size_t n = make_payload(payload, sizeof payload, 0x1234, 5, 2, 1, raw,
                            sizeof raw);
    check(n > 0, "ARGB fixture builds");
    memset(rgba, 0xAA, sizeof rgba);
    MenuSwfBitmap b;
    MenuSwfBitmapStatus st = menu_swf_bitmap_decode(
        36, payload, n, rgba, sizeof rgba, 8, scratch, sizeof scratch, &b,
        reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK, "Lossless2 ARGB decode");
    check(b.character_id == 0x1234 && b.width == 2 && b.height == 1 &&
              b.format == 5 && b.has_alpha && b.premultiplied_alpha,
          "Lossless2 ARGB metadata (premultiplied)");
    check(rgba[0] == 64 && rgba[1] == 32 && rgba[2] == 16 && rgba[3] == 128 &&
              rgba[4] == 10 && rgba[5] == 20 && rgba[6] == 30 &&
              rgba[7] == 255,
          "Lossless2 ARGB exact pixels");

    /* Bad Adler/corruption: output must stay untouched. */
    payload[n - 1] ^= 0xFF;
    memset(rgba, 0x5A, sizeof rgba);
    st = menu_swf_bitmap_decode(36, payload, n, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_ZLIB, "bad zlib refused");
    int unchanged = 1;
    for (unsigned i = 0; i < sizeof rgba; ++i)
        if (rgba[i] != 0x5A) unchanged = 0;
    check(unchanged, "bad zlib leaves output untouched");
}

static void test_lossless_rgb(void) {
    /* PIX24 is XRGB: a reserved byte, then R, G and B. */
    uint8_t raw[8] = {0, 1, 2, 3, 0, 4, 5, 6};
    uint8_t payload[256], rgba[8], scratch[64];
    char reason[BSA_REASON_MAX];
    size_t n = make_payload(payload, sizeof payload, 7, 5, 2, 1, raw,
                            sizeof raw);
    MenuSwfBitmap b;
    memset(rgba, 0xAA, sizeof rgba);
    MenuSwfBitmapStatus st = menu_swf_bitmap_decode(
        20, payload, n, rgba, sizeof rgba, 8, scratch, sizeof scratch, &b,
        reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK, "Lossless 24-bit decode");
    check(!b.has_alpha && !b.premultiplied_alpha && b.format == 5,
          "Lossless 24-bit metadata");
    check(rgba[0] == 1 && rgba[1] == 2 && rgba[2] == 3 && rgba[3] == 255 &&
              rgba[4] == 4 && rgba[5] == 5 && rgba[6] == 6 && rgba[7] == 255,
          "Lossless 24-bit exact pixels");

    /* PIX15 is big-endian; one red pixel, row padded to four bytes. */
    uint8_t raw4[4] = {0x7C, 0x00, 0, 0};
    n = make_payload(payload, sizeof payload, 8, 4, 1, 1, raw4, sizeof raw4);
    st = menu_swf_bitmap_decode(20, payload, n, rgba, sizeof rgba, 4, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK && rgba[0] == 255 && rgba[1] == 0 &&
              rgba[2] == 0 && rgba[3] == 255,
          "Lossless 15-bit big-endian RGB555 decode");

    /* Format 4 in DefineBitsLossless2 is undocumented in SWF19 but Flash
     * Player displays it; accept it as opaque RGB555. */
    uint8_t raw4l2[4] = {0x7C, 0x00, 0, 0};
    n = make_payload(payload, sizeof payload, 12, 4, 1, 1, raw4l2,
                     sizeof raw4l2);
    st = menu_swf_bitmap_decode(36, payload, n, rgba, sizeof rgba, 4, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK && rgba[0] == 255 && rgba[1] == 0 &&
              rgba[2] == 0 && rgba[3] == 255 && b.has_alpha &&
              !b.premultiplied_alpha,
          "Lossless2 RGB555 compatibility decode");
}

static void test_palette(void) {
    /* Palette entries are R,G,B,A; alpha is the last byte. */
    uint8_t palette[] = {1, 2, 3, 255, 4, 5, 6, 128};
    uint8_t indices[4] = {0, 1, 0, 0};
    uint8_t payload[512], rgba[8], scratch[128];
    char reason[BSA_REASON_MAX];
    size_t n = make_palette(payload, sizeof payload, 9, 36, 2, 1, palette,
                            sizeof palette, indices, sizeof indices);
    check(n > 0, "palette fixture builds");
    memset(rgba, 0xAA, sizeof rgba);
    MenuSwfBitmap b;
    MenuSwfBitmapStatus st = menu_swf_bitmap_decode(
        36, payload, n, rgba, sizeof rgba, 8, scratch, sizeof scratch, &b,
        reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK, "palette decode");
    check(rgba[0] == 1 && rgba[1] == 2 && rgba[2] == 3 && rgba[3] == 255 &&
              rgba[4] == 4 && rgba[5] == 5 && rgba[6] == 6 &&
              rgba[7] == 128 && !b.premultiplied_alpha,
          "palette exact pixels");

    /* Opaque palette (Lossless) pads rows and forces alpha 255. */
    uint8_t rgb_palette[] = {1, 2, 3, 4, 5, 6};
    n = make_palette(payload, sizeof payload, 10, 20, 2, 1, rgb_palette,
                     sizeof rgb_palette, indices, sizeof indices);
    st = menu_swf_bitmap_decode(20, payload, n, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK && rgba[3] == 255 && rgba[7] == 255 &&
              !b.has_alpha,
          "opaque palette decode");

    /* Index beyond the one-entry table is a named refusal before writing. */
    uint8_t one[4] = {9, 8, 7, 255};
    uint8_t bad_idx[4] = {1, 0, 0, 0};
    n = make_palette(payload, sizeof payload, 11, 36, 2, 1, one, sizeof one,
                     bad_idx, sizeof bad_idx);
    memset(rgba, 0x33, sizeof rgba);
    st = menu_swf_bitmap_decode(36, payload, n, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    int untouched = 1;
    for (unsigned i = 0; i < sizeof rgba; ++i)
        if (rgba[i] != 0x33) untouched = 0;
    check(st == MENU_SWF_BITMAP_ERR_FORMAT && untouched,
          "palette index overflow refused before writing");
}

static void test_refusals(void) {
    uint8_t raw[] = {128, 1, 2, 3, 255, 4, 5, 6};
    uint8_t payload[256], rgba[8], scratch[64];
    char reason[BSA_REASON_MAX];
    size_t n = make_payload(payload, sizeof payload, 1, 5, 2, 1, raw,
                            sizeof raw);
    MenuSwfBitmap b;

    MenuSwfBitmapStatus st = menu_swf_bitmap_decode(
        21, payload, n, rgba, sizeof rgba, 8, scratch, sizeof scratch, &b,
        reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_JPEG &&
              strstr(reason, "JPEG bitmap decode is not implemented"),
          "JPEG2 named refusal");
    st = menu_swf_bitmap_decode(35, payload, n, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_JPEG, "JPEG3 named refusal");

    payload[2] = 2;
    st = menu_swf_bitmap_decode(36, payload, n, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_FORMAT, "bad bitmap format refused");
    payload[2] = 5;

    st = menu_swf_bitmap_decode(36, payload, n, rgba, 4, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_SIZE, "short RGBA refused");
    st = menu_swf_bitmap_decode(36, payload, n, rgba, sizeof rgba, 8, scratch,
                                2, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_OVERFLOW, "short scratch refused");
    st = menu_swf_bitmap_decode(36, payload, 5, rgba, sizeof rgba, 8, scratch,
                                sizeof scratch, &b, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_ERR_ARGUMENT ||
              st == MENU_SWF_BITMAP_ERR_FORMAT,
          "truncated header refused");

    uint8_t badz[] = {0x78, 0x01, 0, 0};
    size_t got = 0;
    BsaStatus bs = bsa_inflate_zlib_exact(badz, sizeof badz, scratch,
                                           sizeof scratch, &got, reason,
                                           sizeof reason);
    check(bs != BSA_OK, "short zlib stream refused");
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

    MenuSwfBitmapSummary s;
    MenuSwfBitmapStatus st = menu_swf_bitmap_summary(
        bytes, got, &s, reason, sizeof reason);
    check(st == MENU_SWF_BITMAP_OK, "real bitmap summary");
    if (st == MENU_SWF_BITMAP_OK) {
        printf("real: bitmaps total=%u lossless=%u lossless2=%u jpeg=%u "
               "definebits=%u\n",
               s.total_tags, s.lossless_tags, s.lossless2_tags, s.jpeg_tags,
               s.define_bits_tags);
        check(s.total_tags == 0 && s.lossless_tags == 0 &&
                  s.lossless2_tags == 0 && s.jpeg_tags == 0 &&
                  s.define_bits_tags == 0,
              "real StartMenu has no bitmap tags");
    } else {
        printf("real: bitmap summary failed: %s\n", reason);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF bitmap tests\n");
    test_lossless2_argb();
    test_lossless_rgb();
    test_palette();
    test_refusals();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF bitmap: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
