/* Host test: bounded SWF display list + DefineFont3 metrics.
 *
 * Synthetic fixtures exercise PlaceObject2/3, RemoveObject2, ShowFrame, a
 * DefineSprite timeline and a complete DefineFont3.  With a data directory the
 * test also reads Interface/StartMenu.swf from the installed BSA (read-only).
 */

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
} BitWriter;

static void bw_put(BitWriter *w, uint32_t v, unsigned n) {
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

static void bw_signed(BitWriter *w, int32_t v, unsigned n) {
    uint32_t mask = n == 32 ? 0xffffffffu : ((1u << n) - 1u);
    bw_put(w, (uint32_t)v & mask, n);
}

static void bw_align(BitWriter *w) {
    while (w->bit & 7u) bw_put(w, 0, 1);
}

static size_t build_matrix(uint8_t *out, size_t cap, float scale_x,
                           float scale_y, int32_t tx, int32_t ty) {
    if (cap < 32) return 0;
    memset(out, 0, 24);
    BitWriter w = {out, cap, 0, 1};
    int has_scale = scale_x != 1.0f || scale_y != 1.0f;
    bw_put(&w, has_scale ? 1u : 0u, 1);
    if (has_scale) {
        bw_put(&w, 16, 5);
        bw_signed(&w, (int32_t)(scale_x * 65536.0f), 16);
        bw_signed(&w, (int32_t)(scale_y * 65536.0f), 16);
    }
    bw_put(&w, 0, 1); /* no rotate */
    bw_put(&w, 16, 5);
    bw_signed(&w, tx, 16);
    bw_signed(&w, ty, 16);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t build_color(uint8_t *out, size_t cap, int16_t r, int16_t g,
                          int16_t b, int16_t a, int16_t ar, int16_t ag,
                          int16_t ab, int16_t aa) {
    if (cap < 32) return 0;
    memset(out, 0, 24);
    BitWriter w = {out, cap, 0, 1};
    bw_put(&w, 1, 1);  /* has add */
    bw_put(&w, 1, 1);  /* has mult */
    bw_put(&w, 10, 4); /* nbits */
    bw_signed(&w, r, 10);
    bw_signed(&w, g, 10);
    bw_signed(&w, b, 10);
    bw_signed(&w, a, 10);
    bw_signed(&w, ar, 10);
    bw_signed(&w, ag, 10);
    bw_signed(&w, ab, 10);
    bw_signed(&w, aa, 10);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
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
    size_t n = strlen(text);
    memcpy(out + at, text, n);
    out[at + n] = 0;
    return at + n + 1;
}

static size_t build_place2(uint8_t *out, size_t cap, uint8_t flags,
                           uint16_t depth, uint16_t character,
                           int has_matrix, float sx, float sy, int32_t tx,
                           int32_t ty, int has_color, const char *name,
                           uint16_t ratio) {
    if (cap < 64) return 0;
    size_t at = 0;
    out[at++] = flags;
    put_u16(out + at, depth);
    at += 2;
    if (flags & 0x02) {
        put_u16(out + at, character);
        at += 2;
    }
    if (flags & 0x04) {
        if (!has_matrix) return 0;
        size_t n = build_matrix(out + at, cap - at, sx, sy, tx, ty);
        if (!n) return 0;
        at += n;
    }
    if (flags & 0x08) {
        if (!has_color) return 0;
        size_t n = build_color(out + at, cap - at, 256, 128, 64, 32,
                               10, 20, 30, 0);
        if (!n) return 0;
        at += n;
    }
    if (flags & 0x10) {
        put_u16(out + at, ratio);
        at += 2;
    }
    if (flags & 0x20) at = add_string(out, at, name);
    return at;
}

static size_t build_place3(uint8_t *out, size_t cap, uint8_t flags0,
                           uint8_t flags1, uint16_t depth, const char *class_name,
                           uint16_t character, int has_matrix, float sx,
                           float sy, int32_t tx, int32_t ty, const char *name,
                           const uint8_t *filters, size_t filter_len) {
    if (cap < 96) return 0;
    size_t at = 0;
    out[at++] = flags0;
    out[at++] = flags1;
    put_u16(out + at, depth);
    at += 2;
    if ((flags1 & 0x08) || ((flags1 & 0x10) && (flags0 & 0x02)))
        at = add_string(out, at, class_name);
    if (flags0 & 0x02) {
        put_u16(out + at, character);
        at += 2;
    }
    if (flags0 & 0x04) {
        if (!has_matrix) return 0;
        size_t n = build_matrix(out + at, cap - at, sx, sy, tx, ty);
        if (!n) return 0;
        at += n;
    }
    if (flags0 & 0x20) at = add_string(out, at, name);
    if (flags1 & 0x01) {
        if (filter_len > cap - at) return 0;
        memcpy(out + at, filters, filter_len);
        at += filter_len;
    }
    return at;
}

static size_t build_sprite(uint8_t *out, size_t cap, uint16_t sprite_id,
                           uint16_t declared_frames) {
    if (cap < 64) return 0;
    size_t at = 0;
    put_u16(out + at, sprite_id);
    at += 2;
    put_u16(out + at, declared_frames);
    at += 2;
    uint8_t p[64];
    size_t n = build_place2(p, sizeof p, 0x22, 1, 20, 0, 1, 1, 0, 0, 0,
                            "spriteobj", 0);
    at = put_tag(out, cap, at, 26, p, (uint32_t)n, 0);
    at = put_tag(out, cap, at, 1, NULL, 0, 0);
    n = build_place2(p, sizeof p, 0x05, 1, 0, 1, 1.5f, 1.5f, 100, 200, 0,
                     NULL, 0);
    at = put_tag(out, cap, at, 26, p, (uint32_t)n, 0);
    at = put_tag(out, cap, at, 1, NULL, 0, 0);
    at = put_tag(out, cap, at, 0, NULL, 0, 0);
    return at;
}

static size_t build_font3(uint8_t *out, size_t cap) {
    if (cap < 128) return 0;
    memset(out, 0, 128);
    size_t at = 0;
    put_u16(out + at, 5);
    at += 2;
    out[at++] = 0x85; /* HasLayout | WideCodes | Bold */
    out[at++] = 0;    /* language */
    out[at++] = 9;    /* name length, includes NUL */
    at = add_string(out, at, "TestFont");
    put_u16(out + at, 2);
    at += 2;
    uint32_t base = (uint32_t)at;
    /* Offset table: glyph 0 at base+0x10, glyph 1 at base+0x14. */
    put_u16(out + at, 0x10);
    at += 2;
    put_u16(out + at, 0x14);
    at += 2;
    put_u16(out + at, 0x18); /* CodeTableOffset from base */
    at += 2;
    while (at < base + 0x18) out[at++] = 0;
    put_u16(out + at, 'A');
    at += 2;
    put_u16(out + at, 'B');
    at += 2;
    put_u16(out + at, 800); /* ascent */
    at += 2;
    put_u16(out + at, 200); /* descent */
    at += 2;
    put_u16(out + at, 0);   /* leading */
    at += 2;
    put_u16(out + at, 500); /* advance A */
    at += 2;
    put_u16(out + at, 600); /* advance B */
    at += 2;
    uint8_t bits[64];
    memset(bits, 0, sizeof bits);
    BitWriter w = {bits, sizeof bits, 0, 1};
    bw_put(&w, 8, 5);
    bw_signed(&w, 0, 8); bw_signed(&w, 100, 8);
    bw_signed(&w, 0, 8); bw_signed(&w, 120, 8);
    bw_align(&w);
    bw_put(&w, 8, 5);
    bw_signed(&w, 10, 8); bw_signed(&w, 90, 8);
    bw_signed(&w, 20, 8); bw_signed(&w, 100, 8);
    bw_align(&w);
    size_t nb = (w.bit + 7) / 8;
    memcpy(out + at, bits, nb);
    at += nb;
    put_u16(out + at, 0); /* kerning count */
    at += 2;
    return at;
}

static size_t build_body(uint8_t *body, size_t cap) {
    uint8_t p[256], filters[3] = {0xaa, 0xbb, 0xcc};
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;

    size_t n = build_place2(p, sizeof p, 0x3e, 2, 10, 1, 1.0f, 1.0f,
                            20, 40, 1, "bg", 128);
    at = put_tag(body, cap, at, 26, p, (uint32_t)n, 0);
    n = build_place2(p, sizeof p, 0x22, 1, 11, 0, 1, 1, 0, 0, 0,
                     "button", 0);
    at = put_tag(body, cap, at, 26, p, (uint32_t)n, 0);
    at = put_tag(body, cap, at, 1, NULL, 0, 0);

    n = build_place2(p, sizeof p, 0x05, 1, 0, 1, 1.0f, 1.0f, 50, 60, 0,
                     NULL, 0);
    at = put_tag(body, cap, at, 26, p, (uint32_t)n, 0);
    at = put_tag(body, cap, at, 1, NULL, 0, 0);

    uint8_t rem[2];
    put_u16(rem, 2);
    at = put_tag(body, cap, at, 28, rem, 2, 0);
    at = put_tag(body, cap, at, 1, NULL, 0, 0);

    n = build_place3(p, sizeof p, 0x26, 0x09, 3, "MyButton", 12, 1,
                     2.0f, 2.0f, 70, 80, "p3", filters, sizeof filters);
    at = put_tag(body, cap, at, 70, p, (uint32_t)n, 0);
    at = put_tag(body, cap, at, 1, NULL, 0, 0);

    n = build_sprite(p, sizeof p, 100, 2);
    at = put_tag(body, cap, at, 39, p, (uint32_t)n, 0);
    n = build_font3(p, sizeof p);
    at = put_tag(body, cap, at, 75, p, (uint32_t)n, 0);
    at = put_tag(body, cap, at, 0, NULL, 0, 0);
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

static size_t zlib_store(uint8_t *dst, size_t cap, const uint8_t *src,
                         size_t n) {
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
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + src[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    uint32_t adler = (b << 16) | a;
    dst[at++] = (uint8_t)(adler >> 24);
    dst[at++] = (uint8_t)(adler >> 16);
    dst[at++] = (uint8_t)(adler >> 8);
    dst[at++] = (uint8_t)adler;
    return at;
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

static void check_frame0(const MenuSwfDisplayFrame *f) {
    check(f->stored_objects == 2, "frame0 object count");
    check(f->objects[0].depth == 1 && f->objects[0].character_id == 11 &&
              strcmp(f->objects[0].name, "button") == 0,
          "frame0 depth1 button");
    check(f->objects[1].depth == 2 && f->objects[1].character_id == 10 &&
              strcmp(f->objects[1].name, "bg") == 0,
          "frame0 depth2 bg");
    check(f->objects[1].has_matrix &&
              f->objects[1].translate_x == 20 &&
              f->objects[1].translate_y == 40,
          "frame0 bg matrix");
    check(f->objects[1].has_color_transform &&
              f->objects[1].color_mult[0] == 256 &&
              f->objects[1].color_mult[1] == 128 &&
              f->objects[1].color_add[1] == 20,
          "frame0 bg color transform");
}

static void test_synthetic(void) {
    uint8_t body[4096], fws[8192], cws[8192];
    size_t body_len = build_body(body, sizeof body);
    size_t fws_len = make_fws(fws, sizeof fws, body, body_len);
    size_t cws_len = make_cws(cws, sizeof cws, body, body_len);
    check(body_len > 0 && fws_len > 0 && cws_len > 0, "synthetic fixture builds");

    MenuSwfDisplaySummary s;
    char reason[BSA_REASON_MAX];
    check(menu_swf_display_summary(fws, fws_len, &s, reason, sizeof reason) ==
              MENU_SWF_OK,
          "FWS display summary succeeds");
    check(s.top.frame_count == 4, "top frame count");
    check(s.top.frames[0].stored_objects == 2, "top frame0 snapshot");
    check_frame0(&s.top.frames[0]);
    check(s.top.frames[1].stored_objects == 2 &&
              s.top.frames[1].objects[0].depth == 1 &&
              s.top.frames[1].objects[0].translate_x == 50 &&
              s.top.frames[1].objects[0].translate_y == 60 &&
              strcmp(s.top.frames[1].objects[0].name, "button") == 0,
          "frame1 move updates matrix and keeps name");
    check(s.top.frames[2].stored_objects == 1 &&
              s.top.frames[2].objects[0].depth == 1,
          "RemoveObject2 removes depth 2");
    check(s.top.frames[3].stored_objects == 2 &&
              s.top.frames[3].objects[1].depth == 3 &&
              s.top.frames[3].objects[1].character_id == 12 &&
              s.top.frames[3].objects[1].has_class_name &&
              strcmp(s.top.frames[3].objects[1].class_name, "MyButton") == 0 &&
              s.top.frames[3].objects[1].has_filter_list &&
              s.top.frames[3].objects[1].filter_list_bytes == 3,
          "PlaceObject3 object, class name and opaque filter list");
    check(s.sprite_total == 1 && s.stored_sprites == 1,
          "sprite summary stored");
    check(s.sprites[0].sprite_id == 100 && s.sprites[0].frame_count == 2 &&
              s.sprites[0].placements == 2 && s.sprites[0].max_objects == 1,
          "sprite frame/object counts");
    check(s.selected_sprite == 0 && s.selected.frame_count == 2,
          "selected sprite frames");
    check(s.selected.frames[0].stored_objects == 1 &&
              s.selected.frames[0].objects[0].character_id == 20 &&
              strcmp(s.selected.frames[0].objects[0].name, "spriteobj") == 0,
          "selected sprite first frame");
    check(s.selected.frames[1].objects[0].translate_x == 100 &&
              s.selected.frames[1].objects[0].translate_y == 200,
          "selected sprite second frame");

    check(s.font_total == 1 && s.stored_fonts == 1, "font stored");
    const MenuSwfFontSummary *f = &s.fonts[0];
    check(f->font_id == 5 && f->glyph_count == 2 && f->stored_glyphs == 2,
          "font glyph count");
    check(f->font_type == 3 && f->shape_scale == 20 && f->em_square == 1024 &&
              f->flags == 0x85 && f->has_layout && f->wide_codes &&
              !f->wide_offsets,
          "font DefineFont3 flags");
    check(f->ascent == 800 && f->descent == 200 && f->leading == 0,
          "font layout metrics");
    check(f->glyphs[0].code == 'A' && f->glyphs[1].code == 'B',
          "font code table");
    check(f->glyphs[0].advance == 500 && f->glyphs[1].advance == 600,
          "font advance table");
    check(f->glyphs[0].has_bounds && f->glyphs[0].bounds[0] == 0 &&
              f->glyphs[0].bounds[1] == 100 &&
              f->glyphs[0].bounds[2] == 0 &&
              f->glyphs[0].bounds[3] == 120,
          "font bounds table");
    check(f->glyphs[0].shape_offset ==
              f->offset_table_base + 0x10 &&
              f->glyphs[1].shape_offset == f->offset_table_base + 0x14,
          "font shape offsets");

    MenuSwfDisplaySummary c;
    check(menu_swf_display_summary(cws, cws_len, &c, reason, sizeof reason) ==
              MENU_SWF_OK,
          "CWS display summary succeeds");
    check(c.top.frame_count == s.top.frame_count &&
              c.selected.frame_count == s.selected.frame_count,
          "FWS/CWS display frames agree");
}

static size_t make_error_swf(uint8_t *out, size_t cap, uint8_t code,
                             const uint8_t *payload, uint32_t len) {
    uint8_t body[256];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, code, payload, len, 0);
    at = put_tag(body, sizeof body, at, 0, NULL, 0, 0);
    return make_fws(out, cap, body, at);
}

static void test_place_variants(void) {
    uint8_t body[512], file[1024], p[128];
    size_t body_at = 0;
    body[body_at++] = 0;
    put_u16(body + body_at, 0x1e00);
    body_at += 2;
    put_u16(body + body_at, 1);
    body_at += 2;

    /* PlaceObject2 with clip actions (opaque, not executed). */
    p[0] = 0x82; /* HasClipActions | HasCharacter */
    put_u16(p + 1, 1);
    put_u16(p + 3, 5);
    p[5] = 0xaa;
    p[6] = 0xbb;
    body_at = put_tag(body, sizeof body, body_at, 26, p, 7, 0);
    body_at = put_tag(body, sizeof body, body_at, 1, NULL, 0, 0);

    /* PlaceObject3 with class name, blend, cache and visibility (no filters). */
    uint8_t p3[128];
    size_t p3len = 0;
    p3[p3len++] = 0x22; /* HasName | HasCharacter */
    p3[p3len++] = 0x3e; /* HasVisible | HasImage | HasClassName | Cache | Blend */
    put_u16(p3 + p3len, 4);
    p3len += 2;
    p3len = add_string(p3, p3len, "Cls");
    put_u16(p3 + p3len, 6);
    p3len += 2;
    p3len = add_string(p3, p3len, "v3");
    p3[p3len++] = 3;  /* blend */
    p3[p3len++] = 1;  /* cache */
    p3[p3len++] = 1;  /* visible */
    p3[p3len++] = 10;
    p3[p3len++] = 20;
    p3[p3len++] = 30;
    p3[p3len++] = 40;
    body_at = put_tag(body, sizeof body, body_at, 70, p3, (uint32_t)p3len, 0);
    body_at = put_tag(body, sizeof body, body_at, 1, NULL, 0, 0);
    body_at = put_tag(body, sizeof body, body_at, 0, NULL, 0, 0);
    size_t n = make_fws(file, sizeof file, body, body_at);
    check(n > 0, "place variant fixture builds");

    MenuSwfDisplaySummary s;
    char reason[BSA_REASON_MAX];
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_OK,
          "place variant summary succeeds");
    check(s.top.frame_count == 2, "place variant frame count");
    check(s.top.frames[0].objects[0].has_clip_actions &&
              s.top.frames[0].objects[0].clip_actions_bytes == 2,
          "PlaceObject2 clip actions recorded");
    check(s.top.frames[1].stored_objects == 2 &&
              s.top.frames[1].objects[1].depth == 4 &&
              s.top.frames[1].objects[1].has_class_name &&
              strcmp(s.top.frames[1].objects[1].class_name, "Cls") == 0 &&
              s.top.frames[1].objects[1].has_blend_mode &&
              s.top.frames[1].objects[1].blend_mode == 3 &&
              s.top.frames[1].objects[1].has_cache_as_bitmap &&
              s.top.frames[1].objects[1].bitmap_cache == 1 &&
              s.top.frames[1].objects[1].has_visible &&
              s.top.frames[1].objects[1].visible == 1 &&
              s.top.frames[1].objects[1].background[0] == 10,
          "PlaceObject3 class/blend/cache/visible");
}

static void test_errors(void) {
    uint8_t file[512], p[128];
    char reason[BSA_REASON_MAX];
    MenuSwfDisplaySummary s;

    uint8_t bad_place[1] = {0x22};
    size_t n = make_error_swf(file, sizeof file, 26, bad_place, 1);
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_FORMAT,
          "short PlaceObject2 refused");

    uint8_t font4[2] = {0, 0};
    n = make_error_swf(file, sizeof file, 91, font4, 2);
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "DefineFont4 is E_NOTIMPL");

    uint8_t abc[2] = {0, 0};
    n = make_error_swf(file, sizeof file, 82, abc, 2);
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "DoABC/AVM2 is E_NOTIMPL");

    /* PlaceObject3 with filters followed by a blend mode. */
    size_t plen = build_place3(p, sizeof p, 0x22, 0x03, 1, NULL, 12, 0,
                               1, 1, 0, 0, "x", (const uint8_t *)"abc", 3);
    n = make_error_swf(file, sizeof file, 70, p, (uint32_t)plen);
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "filter list plus later fields is E_NOTIMPL");

    uint8_t body[64];
    size_t at = 0;
    body[at++] = 0;
    put_u16(body + at, 0x1e00);
    at += 2;
    put_u16(body + at, 1);
    at += 2;
    at = put_tag(body, sizeof body, at, 1, NULL, 0, 0); /* ShowFrame, no End */
    n = make_fws(file, sizeof file, body, at);
    check(menu_swf_display_summary(file, n, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_END,
          "missing End refused");

    memcpy(file, "ZWS", 3);
    file[3] = 10;
    put_u32(file + 4, 20);
    memset(file + 8, 0x5a, 12);
    check(menu_swf_display_summary(file, 20, &s, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL,
          "ZWS is E_NOTIMPL");
}

/* Optional read-only real summary. */
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

    MenuSwfDisplaySummary s;
    MenuSwfStatus st = menu_swf_display_summary(bytes, got, &s, reason,
                                                sizeof reason);
    check(st == MENU_SWF_OK, "real display summary");
    if (st == MENU_SWF_OK) {
        printf("real: %s StartMenu display: top frames=%u objects=%u "
               "max=%u sprites=%u/%u selected=%d selected_frames=%u "
               "fonts=%u/%u\n",
               used, s.top.frame_count,
               s.top.frame_count ? s.top.frames[0].stored_objects : 0,
               s.top.max_objects, s.stored_sprites, s.sprite_total,
               s.selected_sprite, s.selected.frame_count, s.stored_fonts,
               s.font_total);
        if (s.top.frame_count) {
            const MenuSwfDisplayFrame *f = &s.top.frames[0];
            printf("real: top frame0 objects=%u", f->stored_objects);
            for (uint32_t i = 0; i < f->stored_objects && i < 6; ++i) {
                const MenuSwfDisplayObject *o = &f->objects[i];
                printf(" [d=%u c=%u name='%s' tx=%d ty=%d]",
                       o->depth, o->character_id, o->name,
                       o->translate_x, o->translate_y);
            }
            printf("\n");
        }
        check(s.top.frame_count >= 1, "real top frames");
        check(s.top.frames[0].stored_objects > 0, "real first frame objects");
        check(s.sprite_total >= 71, "real sprite count");
        check(s.selected_sprite >= 0 && s.selected.frame_count > 0,
              "real selected sprite frames");
        if (s.selected_sprite >= 0) {
            const MenuSwfSpriteSummary *ss = &s.sprites[s.selected_sprite];
            printf("real: selected sprite id=%u declared_frames=%u frames=%u "
                   "placements=%u removals=%u max_objects=%u depth=%u\n",
                   ss->sprite_id, ss->declared_frames, ss->frame_count,
                   ss->placements, ss->removals, ss->max_objects, ss->depth);
            if (s.selected.frame_count && s.selected.frames[0].stored_objects) {
                const MenuSwfDisplayFrame *f = &s.selected.frames[0];
                printf("real: selected frame0 objects=%u", f->stored_objects);
                for (uint32_t i = 0; i < f->stored_objects && i < 6; ++i) {
                    const MenuSwfDisplayObject *o = &f->objects[i];
                    printf(" [d=%u c=%u name='%s' sx=%.2f sy=%.2f tx=%d ty=%d]",
                           o->depth, o->character_id, o->name, o->scale_x,
                           o->scale_y, o->translate_x, o->translate_y);
                }
                printf("\n");
            }
            check(s.selected.frames[0].stored_objects > 0,
                  "real selected sprite first frame objects");
        }
        check(s.stored_fonts >= 1, "real DefineFont3 stored");
        if (s.stored_fonts) {
            const MenuSwfFontSummary *f = &s.fonts[0];
            printf("real: font id=%u glyphs=%u stored=%u type=%u layout=%u "
                   "scale=%u em=%u ascent=%d descent=%d leading=%d "
                   "advance0=%d code0=%u shape0=%u bounds0=%d,%d,%d,%d "
                   "kerning=%u trailing=%u\n",
                   f->font_id, f->glyph_count, f->stored_glyphs, f->font_type,
                   f->has_layout, f->shape_scale, f->em_square, f->ascent,
                   f->descent, f->leading, f->glyphs[0].advance,
                   f->glyphs[0].code, f->glyphs[0].shape_offset,
                   f->glyphs[0].bounds[0], f->glyphs[0].bounds[1],
                   f->glyphs[0].bounds[2], f->glyphs[0].bounds[3],
                   f->kerning_count, f->trailing_bytes);
            check(f->glyph_count == 243, "real glyph count");
            check(f->stored_glyphs == 243, "real stored glyph count");
            check(strstr(f->name, "Futura") != NULL, "real font name");
            check(f->has_layout, "real font layout");
            check(f->glyphs[0].has_advance, "real advance table");
            check(f->glyphs[0].has_bounds, "real bounds table");
            check(f->kerning_count > 0, "real font kerning table found");
            check(f->trailing_bytes == 0, "real font tag fully consumed");
            check(f->glyphs[0].code > 0, "real first code");
            check(f->glyphs[0].shape_offset > f->offset_table_base,
                  "real first shape offset");
        }
    } else {
        printf("real: display summary failed: %s\n", reason);
    }
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF display tests\n");
    test_synthetic();
    test_place_variants();
    test_errors();

    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF display: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
