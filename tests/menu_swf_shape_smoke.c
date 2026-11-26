/* Host test: bounded SWF shape and glyph geometry. */

#include "menu_swf_shape.h"
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

static size_t rect_bytes(uint8_t *out, size_t cap, int32_t xmin, int32_t xmax,
                         int32_t ymin, int32_t ymax) {
    memset(out, 0, cap < 32 ? cap : 32);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 12, 5);
    bw_signed(&w, xmin, 12);
    bw_signed(&w, xmax, 12);
    bw_signed(&w, ymin, 12);
    bw_signed(&w, ymax, 12);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static void add_line(BitW *w, int32_t dx, int32_t dy) {
    bw_put(w, 1, 1);  /* edge */
    bw_put(w, 1, 1);  /* straight */
    bw_put(w, 10, 4); /* NumBits (12-bit values) */
    bw_put(w, 1, 1);  /* general line */
    bw_signed(w, dx, 12);
    bw_signed(w, dy, 12);
}

static void add_curve(BitW *w, int32_t cdx, int32_t cdy, int32_t adx,
                      int32_t ady) {
    bw_put(w, 1, 1);  /* edge */
    bw_put(w, 0, 1);  /* curved */
    bw_put(w, 10, 4);
    bw_signed(w, cdx, 12);
    bw_signed(w, cdy, 12);
    bw_signed(w, adx, 12);
    bw_signed(w, ady, 12);
}

static void style_move(BitW *w, int32_t x, int32_t y, uint16_t fill0,
                       uint16_t line) {
    bw_put(w, 0, 1); /* non-edge */
    bw_put(w, 0, 1); /* no new styles */
    bw_put(w, 1, 1); /* line style */
    bw_put(w, 0, 1); /* fill1 */
    bw_put(w, 1, 1); /* fill0 */
    bw_put(w, 1, 1); /* move */
    bw_put(w, 10, 5);
    bw_signed(w, x, 10);
    bw_signed(w, y, 10);
    bw_put(w, fill0, 2);
    bw_put(w, line, 1);
}

static void move_only(BitW *w, int32_t x, int32_t y) {
    bw_put(w, 0, 1); /* non-edge */
    bw_put(w, 0, 1); /* no new styles */
    bw_put(w, 0, 1); /* no line style */
    bw_put(w, 0, 1); /* no fill1 */
    bw_put(w, 0, 1); /* no fill0 */
    bw_put(w, 1, 1); /* move */
    bw_put(w, 10, 5);
    bw_signed(w, x, 10);
    bw_signed(w, y, 10);
}

static size_t build_rect_records(uint8_t *out, size_t cap, int two_paths,
                                 int curve) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 2, 4); /* fill bits */
    bw_put(&w, 1, 4); /* line bits */
    style_move(&w, 100, 100, 1, 1);
    if (curve) {
        add_curve(&w, 200, 200, 200, -200);
        add_line(&w, 300, 300);
        add_line(&w, -700, 0);
        add_line(&w, 0, -300);
    } else {
        add_line(&w, 800, 0);
        add_line(&w, 0, 400);
        add_line(&w, -800, 0);
        add_line(&w, 0, -400);
    }
    if (two_paths) {
        style_move(&w, 150, 150, 2, 1);
        add_line(&w, 200, 0);
        add_line(&w, 0, 200);
        add_line(&w, -200, 0);
        add_line(&w, 0, -200);
    }
    bw_put(&w, 0, 6); /* EndShapeRecord */
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t build_many_records(uint8_t *out, size_t cap, unsigned count) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 0, 4);
    bw_put(&w, 0, 4);
    move_only(&w, 0, 0);
    for (unsigned i = 0; i < count; ++i) add_line(&w, 1, 0);
    bw_put(&w, 0, 6);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t build_shape_tag(uint8_t *out, size_t cap, uint16_t shape_id,
                              int shape4, const uint8_t *fills, size_t fill_len,
                              const uint8_t *lines, size_t line_len,
                              const uint8_t *records, size_t records_len) {
    size_t at = 0;
    put_u16(out + at, shape_id);
    at += 2;
    if (cap < 64) return 0;
    uint8_t r[32];
    size_t n = rect_bytes(r, sizeof r, 0, 1000, 0, 1000);
    memcpy(out + at, r, n);
    at += n;
    if (shape4) {
        n = rect_bytes(r, sizeof r, 0, 1000, 0, 1000);
        memcpy(out + at, r, n);
        at += n;
        out[at++] = 0; /* reserved + flags */
    }
    memcpy(out + at, fills, fill_len);
    at += fill_len;
    memcpy(out + at, lines, line_len);
    at += line_len;
    memcpy(out + at, records, records_len);
    at += records_len;
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

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at || code > 1023) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        put_u32(out + at, len);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t add_string(uint8_t *out, size_t at, const char *text) {
    size_t n = strlen(text);
    memcpy(out + at, text, n);
    out[at + n] = 0;
    return at + n + 1;
}

static size_t build_font_tag(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    size_t at = 0;
    put_u16(out + at, 5);
    at += 2;
    out[at++] = 0x85; /* HasLayout | WideCodes | Bold */
    out[at++] = 0;
    out[at++] = 9;
    at = add_string(out, at, "TestFont");
    put_u16(out + at, 1);
    at += 2;
    put_u16(out + at, 4); /* glyph shape at base+4 */
    at += 2;
    uint8_t glyph[128];
    memset(glyph, 0, sizeof glyph);
    size_t g = 0;
    {
        BitW w = {glyph, sizeof glyph, 0, 1};
        bw_put(&w, 0, 4);
        bw_put(&w, 0, 4);
        move_only(&w, 0, 0);
        add_line(&w, 100, 0);
        add_line(&w, 0, 100);
        add_line(&w, -100, 0);
        add_line(&w, 0, -100);
        bw_put(&w, 0, 6);
        bw_align(&w);
        g = w.ok ? (w.bit + 7) / 8 : 0;
    }
    put_u16(out + at, (uint16_t)(4 + g)); /* CodeTableOffset from base */
    at += 2;
    memcpy(out + at, glyph, g);
    at += g;
    put_u16(out + at, 'A');
    at += 2;
    return at;
}

static void test_synthetic(void) {
    uint8_t body[8192], file[16384], records[1024], tag[2048];
    uint8_t fills2[] = {2, 0, 0xFF, 0, 0, 0, 0, 0, 0xFF};
    uint8_t lines2[] = {1, 0x14, 0, 0, 0, 0};
    uint8_t fills4[] = {1, 0, 0xFF, 0, 0, 0xFF};
    uint8_t lines4[] = {1, 0x14, 0, 0, 0, 0xFF, 0, 0, 0xFF};
    size_t rlen = build_rect_records(records, sizeof records, 1, 0);
    size_t tlen = build_shape_tag(tag, sizeof tag, 2, 0, fills2, sizeof fills2,
                                  lines2, sizeof lines2, records, rlen);
    body[0] = 0;
    put_u16(body + 1, 0x1e00);
    put_u16(body + 3, 1);
    size_t body_at = 5;
    body_at = put_tag(body, sizeof body, body_at, 22, tag, (uint32_t)tlen);
    rlen = build_rect_records(records, sizeof records, 0, 1);
    tlen = build_shape_tag(tag, sizeof tag, 83, 1, fills4, sizeof fills4,
                           lines4, sizeof lines4, records, rlen);
    body_at = put_tag(body, sizeof body, body_at, 83, tag, (uint32_t)tlen);
    tlen = build_font_tag(tag, sizeof tag);
    body_at = put_tag(body, sizeof body, body_at, 75, tag, (uint32_t)tlen);
    body_at = put_tag(body, sizeof body, body_at, 0, NULL, 0);
    size_t flen = make_fws(file, sizeof file, body, body_at);
    check(flen > 0, "shape fixture builds");

    MenuSwfShapeResult s;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_shape_summary(file, flen, &s, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "shape summary succeeds");
    check(s.shape_total == 2 && s.stored_shapes == 2, "shape count");
    check(s.font_total == 1 && s.stored_fonts == 1, "font count");
    check(s.glyph_total == 1 && s.stored_glyphs == 1, "glyph count");

    const MenuSwfShapeSummary *a = menu_swf_shape_find(&s, 2);
    check(a != NULL, "find shape 2");
    if (a) {
        check(a->path_count == 2 && a->stored_paths == 2, "shape paths");
        check(a->segment_count == 8 && a->point_count == 10,
              "shape segments/points");
        check(s.shape_paths[a->path_first + 0].closed && s.shape_paths[a->path_first + 0].segment_count == 4,
              "rectangle closed");
        check(s.shape_paths[a->path_first + 0].fill0 == 1 && s.shape_paths[a->path_first + 0].line == 1,
              "rectangle styles");
        check(s.shape_paths[a->path_first + 1].fill0 == 2 && s.shape_paths[a->path_first + 1].closed,
              "second style path");
        check(a->solid_fills == 2 && a->solid_lines == 1,
              "style array counts");
        check(a->bounds.xmin == 0 && a->bounds.xmax == 1000,
              "shape bounds");
    }
    const MenuSwfShapeSummary *b = menu_swf_shape_find(&s, 83);
    check(b != NULL, "find shape 83");
    if (b) {
        check(b->tag_code == 83 && b->has_edge_bounds, "Shape4 edge bounds");
        check(b->path_count == 1 && s.shape_paths[b->path_first + 0].has_curve &&
                  s.shape_paths[b->path_first + 0].segment_count == 4,
              "curve path");
        check(s.shape_paths[b->path_first + 0].closed, "curve path closes");
    }
    const MenuSwfShapeGlyph *g = &s.glyphs[0];
    check(g->font_id == 5 && g->code == 'A' && g->glyph_index == 0,
          "glyph code/id");
    check(g->has_geometry && g->path_count == 1 && g->segment_count == 4,
          "glyph geometry");
    check(g->bounds.xmin == 0 && g->bounds.xmax == 100 &&
              g->bounds.ymin == 0 && g->bounds.ymax == 100,
          "glyph bounds");
    check(g->shape_offset > 0, "glyph shape offset");
}

static size_t make_tag_swf(uint8_t *out, size_t cap, unsigned code,
                           const uint8_t *payload, uint32_t len, int end) {
    uint8_t body[32768];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, code, payload, len);
    if (end) at = put_tag(body, sizeof body, at, 0, NULL, 0);
    return make_fws(out, cap, body, at);
}

static void test_errors(void) {
    uint8_t file[65536], tag[32768];
    char reason[BSA_REASON_MAX];
    MenuSwfShapeResult s;

    uint8_t bitmap_zero[] = {2, 0, 0, 0, 0x40, 0, 0, 0, 0, 0};
    size_t n = make_tag_swf(file, sizeof file, 22, bitmap_zero,
                            sizeof bitmap_zero, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_FORMAT,
          "bitmap fill without id refused");

    uint8_t unsupported[] = {2, 0, 0, 1, 0x99, 0};
    n = make_tag_swf(file, sizeof file, 22, unsupported, sizeof unsupported, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "unsupported fill type refused");

    uint8_t many[32768];
    size_t mlen = build_many_records(many, sizeof many, 4097);
    uint8_t fill1[] = {0};
    uint8_t line1[] = {0};
    size_t tlen = build_shape_tag(tag, sizeof tag, 9, 0, fill1, 1, line1, 1,
                                  many, mlen);
    n = make_tag_swf(file, sizeof file, 2, tag, (uint32_t)tlen, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_BUDGET,
          "shape segment limit refused");

    uint8_t short_tag[] = {2};
    n = make_tag_swf(file, sizeof file, 2, short_tag, 1, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_FORMAT,
          "truncated shape refused");

    uint8_t font4[] = {0, 0};
    n = make_tag_swf(file, sizeof file, 91, font4, 2, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "DefineFont4 is E_NOTIMPL");

    uint8_t abc[] = {0, 0};
    n = make_tag_swf(file, sizeof file, 82, abc, 2, 1);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "DoABC/AVM2 is E_NOTIMPL");

    uint8_t end_records[256];
    size_t end_rlen = build_rect_records(end_records, sizeof end_records, 0, 0);
    uint8_t end_fill[] = {1, 0, 0, 0, 0};
    uint8_t end_line[] = {1, 0x14, 0, 0, 0, 0};
    size_t end_tlen = build_shape_tag(tag, sizeof tag, 11, 0, end_fill,
                                      sizeof end_fill, end_line,
                                      sizeof end_line, end_records, end_rlen);
    n = make_tag_swf(file, sizeof file, 2, tag, (uint32_t)end_tlen, 0);
    check(menu_swf_shape_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_END,
          "missing End refused");
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

    MenuSwfShapeResult s;
    MenuSwfStatus st = menu_swf_shape_summary(bytes, got, &s, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK, "real shape summary");
    if (st == MENU_SWF_OK) {
        const MenuSwfShapeSummary *shape2 = menu_swf_shape_find(&s, 2);
        const MenuSwfShapeSummary *shape101 = menu_swf_shape_find(&s, 101);
        unsigned shape_segments = 0, shape_points = 0;
        for (uint32_t i = 0; i < s.stored_shapes; ++i) {
            shape_segments += s.shapes[i].segment_count;
            shape_points += s.shapes[i].point_count;
        }
        printf("real: shapes=%u/%u fonts=%u/%u glyphs=%u/%u shape_paths=%u "
               "shape_segments=%u shape_points=%u glyph_paths=%u\n",
               s.stored_shapes, s.shape_total, s.stored_fonts, s.font_total,
               s.stored_glyphs, s.glyph_total, s.shape_path_used,
               shape_segments, shape_points, s.glyph_path_used);
        if (shape2) {
            printf("real: shape2 paths=%u segs=%u pts=%u bounds=%d,%d,%d,%d\n",
                   shape2->path_count, shape2->segment_count,
                   shape2->point_count, shape2->bounds.xmin,
                   shape2->bounds.xmax, shape2->bounds.ymin,
                   shape2->bounds.ymax);
        }
        printf("real: MenuHolder char101 is shape=%s\n",
               shape101 ? "yes" : "no (sprite id)");
        check(s.shape_total == 27 && s.stored_shapes == 27,
              "real shape count 27");
        check(shape2 != NULL, "real shape 2 found");
        if (shape2) {
            check(shape2->path_count > 0 && shape2->segment_count > 0,
                  "real shape2 geometry");
            check(shape2->bounds.xmax > shape2->bounds.xmin &&
                      shape2->bounds.ymax > shape2->bounds.ymin,
                  "real shape2 bounds");
        }
        check(shape101 == NULL, "MenuHolder is a sprite, not a shape");
        check(s.glyph_total == 243 && s.stored_glyphs == 243,
              "real glyph count 243");
        check(s.font_total >= 1, "real font found");
        unsigned nonempty = 0, glyph_segments = 0;
        for (uint32_t i = 0; i < s.stored_glyphs; ++i) {
            if (s.glyphs[i].has_geometry) ++nonempty;
            glyph_segments += s.glyphs[i].segment_count;
        }
        printf("real: glyphs_with_geometry=%u glyph_segments=%u\n",
               nonempty, glyph_segments);
        check(nonempty > 0, "real glyphs have geometry");
        const MenuSwfShapeGlyph *first = NULL;
        for (uint32_t i = 0; i < s.stored_glyphs; ++i)
            if (s.glyphs[i].has_geometry) { first = &s.glyphs[i]; break; }
        if (first) {
            printf("real: first glyph code=%u paths=%u segs=%u pts=%u "
                   "bounds=%d,%d,%d,%d offset=%u\n",
                   first->code, first->path_count, first->segment_count,
                   first->point_count, first->bounds.xmin, first->bounds.xmax,
                   first->bounds.ymin, first->bounds.ymax, first->shape_offset);
            check(first->path_count > 0, "real first glyph paths");
        }
    } else {
        printf("real: shape summary failed: %s\n", reason);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF shape tests\n");
    test_synthetic();
    test_errors();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF shape: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
