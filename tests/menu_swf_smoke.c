/* Host test: bounded SWF container + tag-stream summary.
 *
 * All fixtures are synthetic.  With a data directory argument (or
 * SKYRIM_N3DS_REAL_DATA) the test reads Interface/StartMenu.swf from the
 * installed BSA read-only and prints its tag/name summary.
 */

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

static uint32_t adler32_bytes(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static size_t zlib_store(uint8_t *dst, size_t cap, const uint8_t *src, size_t n) {
    if (cap < n + 16) return 0;
    size_t at = 0;
    dst[at++] = 0x78;
    dst[at++] = 0x01;
    size_t pos = 0;
    do {
        size_t chunk = n - pos > 65535 ? 65535 : n - pos;
        int final = pos + chunk == n;
        dst[at++] = (uint8_t)(final ? 1 : 0);
        dst[at++] = (uint8_t)(chunk & 0xff);
        dst[at++] = (uint8_t)(chunk >> 8);
        unsigned nlen = (unsigned)(~chunk) & 0xffffu;
        dst[at++] = (uint8_t)(nlen & 0xff);
        dst[at++] = (uint8_t)(nlen >> 8);
        memcpy(dst + at, src + pos, chunk);
        at += chunk;
        pos += chunk;
    } while (pos < n);
    uint32_t adler = adler32_bytes(src, n);
    dst[at++] = (uint8_t)(adler >> 24);
    dst[at++] = (uint8_t)(adler >> 16);
    dst[at++] = (uint8_t)(adler >> 8);
    dst[at++] = (uint8_t)adler;
    return at;
}

static size_t make_fws(uint8_t *out, size_t cap, const uint8_t *body,
                       size_t body_len) {
    if (cap < 8 + body_len) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    memcpy(out + 8, body, body_len);
    return 8 + body_len;
}

static size_t make_cws(uint8_t *out, size_t cap, const uint8_t *body,
                       size_t body_len) {
    if (cap < 8 + body_len + 16) return 0;
    memcpy(out, "CWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    size_t z = zlib_store(out + 8, cap - 8, body, body_len);
    return z ? 8 + z : 0;
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len, int force_long) {
    int long_form = force_long || len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at || code > 1023) return 0;
    uint16_t header = (uint16_t)((code << 6) | (long_form ? 0x3fu : len));
    put_u16(out + at, header);
    at += 2;
    if (long_form) {
        put_u32(out + at, len);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t add_string(uint8_t *out, size_t at, const char *text) {
    size_t len = strlen(text);
    memcpy(out + at, text, len);
    out[at + len] = 0;
    return at + len + 1;
}

static size_t build_export(uint8_t *out, size_t cap) {
    if (cap < 2) return 0;
    put_u16(out, 2);
    size_t at = 2;
    put_u16(out + at, 1);
    at += 2;
    at = add_string(out, at, "Logo");
    put_u16(out + at, 2);
    at += 2;
    at = add_string(out, at, "Button");
    return at;
}

static size_t build_import(uint8_t *out, size_t cap) {
    if (cap < 4) return 0;
    size_t at = add_string(out, 0, "gfxfontlib.swf");
    put_u16(out + at, 1);
    at += 2;
    put_u16(out + at, 3);
    at += 2;
    at = add_string(out, at, "$EverywhereFont");
    return at;
}

static size_t build_import2(uint8_t *out, size_t cap) {
    if (cap < 8) return 0;
    size_t at = add_string(out, 0, "gfxfontlib.swf");
    out[at++] = 1;  /* reserved */
    out[at++] = 0;
    put_u16(out + at, 1);
    at += 2;
    put_u16(out + at, 34);
    at += 2;
    at = add_string(out, at, "$EverywhereMediumFont");
    return at;
}

static size_t build_font_name(uint8_t *out, size_t cap) {
    if (cap < 4) return 0;
    put_u16(out, 1);
    return add_string(out, 2, "Futura Condensed");
}

static size_t frame_prefix(uint8_t *body) {
    body[0] = 0x00;
    put_u16(body + 1, 0x1e00);
    put_u16(body + 3, 1);
    return 5;
}

static size_t build_body(uint8_t *body, size_t cap) {
    uint8_t payload[256];
    size_t at = 0;
    if (cap < 5) return 0;
    at = frame_prefix(body);

    memset(payload, 0x11, sizeof payload);

    at = put_tag(body, cap, at, 2, payload, 4, 0);        /* DefineShape */
    at = put_tag(body, cap, at, 75, payload, 8, 0);       /* DefineFont3 */
    uint8_t sprite[8];
    put_u16(sprite, 1);
    put_u16(sprite + 2, 1);
    size_t sp = 4;
    sp = put_tag(sprite, sizeof sprite, sp, 1, NULL, 0, 0);
    sp = put_tag(sprite, sizeof sprite, sp, 0, NULL, 0, 0);
    at = put_tag(body, cap, at, 39, sprite, (uint32_t)sp, 0); /* DefineSprite */
    at = put_tag(body, cap, at, 26, payload, 4, 0);       /* PlaceObject2 */
    at = put_tag(body, cap, at, 70, payload, 4, 0);       /* PlaceObject3 */
    at = put_tag(body, cap, at, 1, NULL, 0, 0);           /* ShowFrame #0 */
    at = put_tag(body, cap, at, 12, payload, 4, 1);       /* DoAction, long */
    at = put_tag(body, cap, at, 20, payload, 4, 0);       /* DefineBitsLossless */
    at = put_tag(body, cap, at, 6, payload, 4, 0);        /* DefineBitsJpeg */
    size_t len = build_export(payload, sizeof payload);
    at = put_tag(body, cap, at, 56, payload, (uint32_t)len, 0);
    len = build_import(payload, sizeof payload);
    at = put_tag(body, cap, at, 57, payload, (uint32_t)len, 0);
    len = build_font_name(payload, sizeof payload);
    at = put_tag(body, cap, at, 88, payload, (uint32_t)len, 0);
    at = put_tag(body, cap, at, 2, payload, 70, 0);       /* long DefineShape */
    at = put_tag(body, cap, at, 1, NULL, 0, 0);           /* ShowFrame #1 */
    at = put_tag(body, cap, at, 0, NULL, 0, 0);           /* End */
    return at;
}

static void check_fixture_summary(const MenuSwfSummary *s, const char *scope) {
    char what[160];
#define CHECK_S(cond, text) \
    do { snprintf(what, sizeof what, "%s: %s", scope, text); check((cond), what); } while (0)
    CHECK_S(s->tag_count == 17, "tag count");
    CHECK_S(s->top_level_tag_count == 15, "top-level tag count");
    CHECK_S(s->nested_tag_count == 2, "nested tag count");
    CHECK_S(s->frame_header_bytes == 5, "frame header bytes");
    CHECK_S(s->define_shape == 2, "DefineShape count");
    CHECK_S(s->define_font2 == 0, "DefineFont2 count");
    CHECK_S(s->define_font3 == 1, "DefineFont3 count");
    CHECK_S(s->define_sprite == 1, "DefineSprite count");
    CHECK_S(s->place_object2 == 1, "PlaceObject2 count");
    CHECK_S(s->place_object3 == 1, "PlaceObject3 count");
    CHECK_S(s->show_frame == 3, "ShowFrame count");
    CHECK_S(s->do_action == 1, "DoAction count");
    CHECK_S(s->export_assets == 1, "ExportAssets count");
    CHECK_S(s->import_assets == 1, "ImportAssets count");
    CHECK_S(s->define_font_name == 1, "DefineFontName count");
    CHECK_S(s->define_bits_lossless == 1, "DefineBitsLossless count");
    CHECK_S(s->define_bits_jpeg == 1, "DefineBitsJpeg count");
    CHECK_S(s->first_show_frame == 5, "first ShowFrame index");
    CHECK_S(s->first_show_frame_offset > 0, "first ShowFrame offset");
    CHECK_S(s->trailing_bytes == 0, "no trailing bytes");
    CHECK_S(s->tag_stream_bytes > 0 &&
                s->tag_stream_bytes >= s->tag_payload_bytes,
            "tag byte totals");
    CHECK_S(s->name_count == 5, "stored name count");
    CHECK_S(s->name_total == 5, "total name count");
    CHECK_S(s->names_truncated == 0, "names not truncated");
    CHECK_S(s->names[0].kind == MENU_SWF_NAME_EXPORT &&
                strcmp(s->names[0].name, "Logo") == 0 &&
                s->names[0].id == 1,
            "first export name");
    CHECK_S(s->names[1].kind == MENU_SWF_NAME_EXPORT &&
                strcmp(s->names[1].name, "Button") == 0 &&
                s->names[1].id == 2,
            "second export name");
    CHECK_S(s->names[2].kind == MENU_SWF_NAME_IMPORT_URL &&
                strcmp(s->names[2].name, "gfxfontlib.swf") == 0,
            "import URL");
    CHECK_S(s->names[3].kind == MENU_SWF_NAME_IMPORT &&
                strcmp(s->names[3].name, "$EverywhereFont") == 0 &&
                s->names[3].id == 3,
            "import name");
    CHECK_S(s->names[4].kind == MENU_SWF_NAME_FONT &&
                strcmp(s->names[4].name, "Futura Condensed") == 0 &&
                s->names[4].id == 1,
            "font name");
#undef CHECK_S
}

static void test_synthetic_summary(void) {
    uint8_t body[2048], fws[4096], cws[4096];
    size_t body_len = build_body(body, sizeof body);
    check(body_len > 63, "fixture contains long tag payloads");
    size_t fws_len = make_fws(fws, sizeof fws, body, body_len);
    size_t cws_len = make_cws(cws, sizeof cws, body, body_len);
    check(fws_len > 0 && cws_len > 0, "FWS/CWS fixtures build");

    MenuSwfSummary fsum, csum;
    char reason[BSA_REASON_MAX];
    check(menu_swf_summary(fws, fws_len, &fsum, reason, sizeof reason) ==
              MENU_SWF_OK,
          "FWS summary succeeds");
    check(fsum.header.format == MENU_SWF_FWS &&
              fsum.header.version == 10 &&
              fsum.header.declared_bytes == fws_len &&
              fsum.header.file_bytes == fws_len,
          "FWS header fields");
    check_fixture_summary(&fsum, "FWS");

    check(menu_swf_summary(cws, cws_len, &csum, reason, sizeof reason) ==
              MENU_SWF_OK,
          "CWS summary succeeds");
    check(csum.header.format == MENU_SWF_CWS &&
              csum.header.version == 10 &&
              csum.header.declared_bytes == 8 + body_len &&
              csum.header.file_bytes == cws_len,
          "CWS header fields");
    check_fixture_summary(&csum, "CWS");
    check(fsum.tag_count == csum.tag_count &&
              fsum.tag_stream_bytes == csum.tag_stream_bytes &&
              fsum.tag_payload_bytes == csum.tag_payload_bytes &&
              fsum.name_total == csum.name_total,
          "FWS/CWS summaries agree");
}

static void test_import_assets2(void) {
    uint8_t body[256], file[512], payload[128];
    char reason[BSA_REASON_MAX];
    MenuSwfSummary s;
    size_t at = frame_prefix(body);
    size_t len = build_import2(payload, sizeof payload);
    check(len > 0, "ImportAssets2 payload builds");
    at = put_tag(body, sizeof body, at, 71, payload, (uint32_t)len, 0);
    at = put_tag(body, sizeof body, at, 0, NULL, 0, 0);
    size_t flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_OK,
          "ImportAssets2 summary succeeds");
    check(s.import_assets == 1 && s.name_count == 2,
          "ImportAssets2 counted");
    check(s.names[0].kind == MENU_SWF_NAME_IMPORT_URL &&
              strcmp(s.names[0].name, "gfxfontlib.swf") == 0,
          "ImportAssets2 URL");
    check(s.names[1].kind == MENU_SWF_NAME_IMPORT &&
              s.names[1].id == 34 &&
              strcmp(s.names[1].name, "$EverywhereMediumFont") == 0,
          "ImportAssets2 symbol");
}

static void test_container_errors(void) {
    uint8_t body[256], file[1024];
    char reason[BSA_REASON_MAX];
    MenuSwfSummary s;
    size_t body_len = build_body(body, sizeof body);
    size_t flen = make_fws(file, sizeof file, body, body_len);
    check(flen > 0, "error fixture builds");

    file[3] = 0;
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_VERSION,
          "version 0 refused");
    file[3] = 99;
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_VERSION,
          "version 99 refused");
    file[3] = 10;

    uint8_t bad[1024];
    memcpy(bad, file, flen);
    put_u32(bad + 4, (uint32_t)flen + 1);
    check(menu_swf_summary(bad, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_FORMAT,
          "FWS declared mismatch refused");

    memcpy(bad, "ZWS", 3);
    bad[3] = 10;
    put_u32(bad + 4, 20);
    memset(bad + 8, 0x5a, 12);
    check(menu_swf_summary(bad, 20, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL &&
              strstr(reason, "E_NOTIMPL") != NULL,
          "ZWS is E_NOTIMPL");

    memcpy(bad, "CWS", 3);
    bad[3] = 10;
    put_u32(bad + 4, 20);
    bad[8] = 0x78;
    bad[9] = 0x00;
    memset(bad + 10, 0, 10);
    check(menu_swf_summary(bad, 20, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_ZLIB,
          "bad CWS zlib header refused");

    size_t clen = make_cws(bad, sizeof bad, body, body_len);
    put_u32(bad + 4, (uint32_t)(8 + body_len + 10));
    check(menu_swf_summary(bad, clen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_ZLIB,
          "CWS declared/output mismatch refused");

    clen = make_cws(bad, sizeof bad, body, body_len);
    bad[clen++] = 0x00;
    check(menu_swf_summary(bad, clen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_ZLIB,
          "CWS trailing compressed bytes refused");

    memcpy(bad, "CWS", 3);
    bad[3] = 10;
    put_u32(bad + 4, MENU_SWF_MAX_DECLARED + 1u);
    bad[8] = 0x78;
    bad[9] = 0x01;
    check(menu_swf_summary(bad, 16, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_BUDGET,
          "oversized declared size refused before allocation");

    check(menu_swf_summary(NULL, 0, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_ARGUMENT,
          "null input refused");
}

static void test_tag_errors(void) {
    uint8_t body[256], file[512];
    char reason[BSA_REASON_MAX];
    MenuSwfSummary s;

    /* A 1-byte tag header after a valid frame header. */
    size_t at = frame_prefix(body);
    body[at++] = 0x02;
    size_t flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_TRUNCATED,
          "truncated tag header refused");

    /* Long header with only two of four length bytes. */
    at = frame_prefix(body);
    body[at++] = 0xff;
    body[at++] = 0xff;
    body[at++] = 0;
    body[at++] = 0;
    flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_TRUNCATED,
          "truncated long tag header refused");

    /* Short tag claims 10 payload bytes but only 2 exist. */
    at = frame_prefix(body);
    body[at++] = (uint8_t)((2u << 6) | 10u);
    body[at++] = 0;
    body[at++] = 0xaa;
    body[at++] = 0xbb;
    flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_TAG,
          "tag length past body refused");

    /* No End tag. */
    at = frame_prefix(body);
    at = put_tag(body, sizeof body, at, 1, NULL, 0, 0);
    flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_END,
          "missing End tag refused");

    /* End with a payload. */
    at = frame_prefix(body);
    at = put_tag(body, sizeof body, at, 0, (const uint8_t *)"xx", 2, 0);
    flen = make_fws(file, sizeof file, body, at);
    check(menu_swf_summary(file, flen, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_FORMAT,
          "non-empty End tag refused");

    check(menu_swf_status_name(MENU_SWF_ERR_END) != NULL,
          "SWF status names exist");
}

/* Optional read-only check against the installed data. */
static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    BsaArchive *archive = NULL;
    BsaMatch match;
    const char *used = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof path, "%s/%s", data_dir, archives[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (bsa_open_path(path, NULL, &archive, NULL, 0) == BSA_OK) {
            if (bsa_find(archive, "interface/startmenu.swf", &match) == BSA_OK) {
                used = archives[i];
                break;
            }
            bsa_close(archive);
            archive = NULL;
        }
    }
    if (!used) {
        printf("real: skip (StartMenu.swf not found under %s)\n", data_dir);
        return;
    }

    uint32_t unpacked = 0;
    char reason[BSA_REASON_MAX];
    check(bsa_unpacked_size(archive, &match.entry, &unpacked, reason,
                            sizeof reason) == BSA_OK,
          "real StartMenu unpacked size");
    uint8_t *bytes = (uint8_t *)malloc(unpacked ? unpacked : 1);
    check(bytes != NULL, "real StartMenu buffer");
    size_t got = 0;
    if (bytes) {
        check(bsa_read(archive, &match.entry, bytes, unpacked, &got, reason,
                       sizeof reason) == BSA_OK && got == unpacked,
              "real StartMenu read");
    }
    bsa_close(archive);

    if (!bytes) return;
    MenuSwfSummary s;
    MenuSwfStatus st = menu_swf_summary(bytes, got, &s, reason, sizeof reason);
    check(st == MENU_SWF_OK, "real StartMenu summary");
    if (st == MENU_SWF_OK) {
        printf("real: %s StartMenu.swf %u tags (%u top, %u nested), "
               "shapes=%u fonts2=%u fonts3=%u fontnames=%u sprites=%u "
               "place2=%u place3=%u show=%u doaction=%u export=%u import=%u "
               "lossless=%u jpeg=%u names=%u/%u first_show=%d "
               "frame_header=%u tag_bytes=%llu trailing=%u\n",
               used, s.tag_count, s.top_level_tag_count, s.nested_tag_count,
               s.define_shape, s.define_font2, s.define_font3,
               s.define_font_name, s.define_sprite, s.place_object2,
               s.place_object3, s.show_frame, s.do_action, s.export_assets,
               s.import_assets, s.define_bits_lossless, s.define_bits_jpeg,
               s.name_count, s.name_total, s.first_show_frame,
               s.frame_header_bytes, (unsigned long long)s.tag_stream_bytes,
               s.trailing_bytes);
        check(s.header.format == MENU_SWF_CWS && s.header.version == 10 &&
                  s.header.declared_bytes == 144628u &&
                  s.header.file_bytes == 60284u,
              "real StartMenu header matches census");
        check(s.tag_count == 1186u, "real StartMenu tag count matches census");
        check(s.top_level_tag_count == 236u,
              "real StartMenu top-level tag count");
        check(s.nested_tag_count == 950u,
              "real StartMenu nested sprite tag count");
        check(s.frame_header_bytes == 13u, "real SWF frame header size");
        check(s.export_assets >= 1u, "real ExportAssets found");
        check(s.define_font3 >= 1u || s.define_font_name >= 1u,
              "real font definition found");
        check(s.import_assets >= 1u, "real ImportAssets found");
        check(s.define_sprite >= 71u, "real DefineSprite found");
        check(s.name_total >= 47u, "real external/font names found");
        check(s.first_show_frame >= 0, "real first ShowFrame found");
        int named_font = 0;
        for (uint32_t i = 0; i < s.name_count; ++i)
            if (strstr(s.names[i].name, "Everywhere") ||
                strstr(s.names[i].name, "Futura"))
                named_font = 1;
        check(named_font, "real font name visible in the bounded name list");
    } else {
        printf("real: summary failed: %s\n", reason);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF summary tests\n");
    test_synthetic_summary();
    test_import_assets2();
    test_container_errors();
    test_tag_errors();

    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }

    printf("menu SWF summary: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
