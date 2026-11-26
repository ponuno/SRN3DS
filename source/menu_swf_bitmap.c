#include "menu_swf_bitmap.h"
#include "menu_swf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void bitmap_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *menu_swf_bitmap_status_name(MenuSwfBitmapStatus status) {
    switch (status) {
    case MENU_SWF_BITMAP_OK: return "ok";
    case MENU_SWF_BITMAP_ERR_ARGUMENT: return "bad bitmap argument";
    case MENU_SWF_BITMAP_ERR_FORMAT: return "bitmap format error";
    case MENU_SWF_BITMAP_ERR_ZLIB: return "bitmap zlib error";
    case MENU_SWF_BITMAP_ERR_SIZE: return "bitmap size mismatch";
    case MENU_SWF_BITMAP_ERR_OVERFLOW: return "bitmap overflow";
    case MENU_SWF_BITMAP_ERR_JPEG: return "JPEG bitmap decode is not implemented";
    default: return "unknown bitmap status";
    }
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static size_t align4(size_t n) {
    return (n + 3u) & ~(size_t)3u;
}

static uint8_t expand5(unsigned v) {
    return (uint8_t)((v << 3) | (v >> 2));
}

MenuSwfBitmapStatus menu_swf_bitmap_decode(
    uint8_t tag_code, const uint8_t *payload, size_t payload_size,
    uint8_t *rgba, size_t rgba_capacity, uint32_t stride, uint8_t *scratch,
    size_t scratch_capacity, MenuSwfBitmap *out, char *reason,
    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_BITMAP_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (tag_code == 6 || tag_code == 21 || tag_code == 35 || tag_code == 90) {
        bitmap_reason(reason, reason_cap,
                      "JPEG bitmap decode is not implemented");
        return MENU_SWF_BITMAP_ERR_JPEG;
    }
    if (tag_code != 20 && tag_code != 36) {
        bitmap_reason(reason, reason_cap,
                      "bitmap tag %u is not DefineBitsLossless/2", tag_code);
        return MENU_SWF_BITMAP_ERR_FORMAT;
    }
    if (!payload || payload_size < 7 || !rgba || !scratch) {
        bitmap_reason(reason, reason_cap,
                      "bitmap payload, output or scratch is missing");
        return MENU_SWF_BITMAP_ERR_ARGUMENT;
    }
    uint16_t id = rd16(payload);
    uint8_t format = payload[2];
    uint16_t width = rd16(payload + 3);
    uint16_t height = rd16(payload + 5);
    if (!width || !height || width > MENU_SWF_BITMAP_MAX_DIM ||
        height > MENU_SWF_BITMAP_MAX_DIM ||
        (uint64_t)width * height > MENU_SWF_BITMAP_MAX_PIXELS) {
        bitmap_reason(reason, reason_cap,
                      "bitmap dimensions %ux%u are outside the limits",
                      width, height);
        return MENU_SWF_BITMAP_ERR_OVERFLOW;
    }
    int alpha = tag_code == 36;
    if (format != 3 && format != 4 && format != 5) {
        bitmap_reason(reason, reason_cap,
                      "bitmap format %u is not 3/4/5", format);
        return MENU_SWF_BITMAP_ERR_FORMAT;
    }

    /* The zlib stream contains the palette (format 3) followed by the
     * row-padded pixel data; every row is aligned to a 32-bit boundary. */
    size_t header_at = 7;
    size_t palette_bytes = 0;
    size_t palette_entries = 0;
    uint32_t row_bytes;
    if (format == 3) {
        if (payload_size < 8) {
            bitmap_reason(reason, reason_cap, "bitmap palette is truncated");
            return MENU_SWF_BITMAP_ERR_FORMAT;
        }
        palette_entries = (size_t)payload[7] + 1u;
        palette_bytes = palette_entries * (alpha ? 4u : 3u);
        header_at = 8;
        row_bytes = (uint32_t)align4(width);
    } else if (format == 4) {
        row_bytes = (uint32_t)align4((size_t)width * 2u);
    } else {
        /* PIX24 is stored as a 4-byte 0RGB/ARGB structure, not 3 bytes. */
        row_bytes = (uint32_t)width * 4u;
    }

    if (stride < (uint32_t)width * 4u) {
        bitmap_reason(reason, reason_cap,
                      "bitmap stride %u is smaller than %u", stride,
                      (unsigned)((uint32_t)width * 4u));
        return MENU_SWF_BITMAP_ERR_SIZE;
    }
    uint64_t out_need = (uint64_t)(height - 1u) * stride +
                        (uint64_t)width * 4u;
    if (out_need > rgba_capacity) {
        bitmap_reason(reason, reason_cap,
                      "bitmap output needs %llu bytes, capacity %llu",
                      (unsigned long long)out_need,
                      (unsigned long long)rgba_capacity);
        return MENU_SWF_BITMAP_ERR_SIZE;
    }
    uint64_t inflated = (uint64_t)row_bytes * height + palette_bytes;
    if (inflated > SIZE_MAX || inflated > scratch_capacity) {
        bitmap_reason(reason, reason_cap,
                      "bitmap scratch needs %llu bytes, capacity %llu",
                      (unsigned long long)inflated,
                      (unsigned long long)scratch_capacity);
        return MENU_SWF_BITMAP_ERR_OVERFLOW;
    }
    if (header_at >= payload_size) {
        bitmap_reason(reason, reason_cap, "bitmap zlib payload is missing");
        return MENU_SWF_BITMAP_ERR_FORMAT;
    }
    size_t got = 0;
    BsaStatus bs = bsa_inflate_zlib_exact(
        payload + header_at, payload_size - header_at, scratch,
        (size_t)inflated, &got, reason, reason_cap);
    if (bs != BSA_OK || got != inflated) {
        if (reason && !reason[0])
            bitmap_reason(reason, reason_cap, "bitmap zlib decode failed");
        return MENU_SWF_BITMAP_ERR_ZLIB;
    }

    const uint8_t *pixels = scratch + palette_bytes;
    if (format == 3) {
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *src = pixels + (size_t)y * row_bytes;
            for (uint32_t x = 0; x < width; ++x)
                if (src[x] >= palette_entries) {
                    bitmap_reason(reason, reason_cap,
                                  "bitmap palette index %u >= %u", src[x],
                                  (unsigned)palette_entries);
                    return MENU_SWF_BITMAP_ERR_FORMAT;
                }
        }
    }

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *src = pixels + (size_t)y * row_bytes;
        uint8_t *dst = rgba + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t *q = dst + (size_t)x * 4u;
            if (format == 3) {
                const uint8_t *p = scratch + (size_t)src[x] * (alpha ? 4u : 3u);
                if (alpha) {
                    q[0] = p[0];
                    q[1] = p[1];
                    q[2] = p[2];
                    q[3] = p[3];
                } else {
                    q[0] = p[0];
                    q[1] = p[1];
                    q[2] = p[2];
                    q[3] = 255;
                }
            } else if (format == 4) {
                /* PIX15 is a big-endian 0RRRRRGGGGGBBBBB value. */
                unsigned v = (unsigned)src[x * 2] << 8 | src[x * 2 + 1];
                q[0] = expand5((v >> 10) & 31u);
                q[1] = expand5((v >> 5) & 31u);
                q[2] = expand5(v & 31u);
                q[3] = 255;
            } else if (alpha) {
                /* ARGB, RGB already multiplied by alpha (SWF spec). */
                const uint8_t *p = src + (size_t)x * 4u;
                q[3] = p[0];
                q[0] = p[1];
                q[1] = p[2];
                q[2] = p[3];
            } else {
                /* PIX24 is XRGB; the first byte is reserved. */
                const uint8_t *p = src + (size_t)x * 4u;
                q[0] = p[1];
                q[1] = p[2];
                q[2] = p[3];
                q[3] = 255;
            }
        }
    }

    out->character_id = id;
    out->width = width;
    out->height = height;
    out->tag_code = tag_code;
    out->format = format;
    out->has_alpha = (uint8_t)alpha;
    out->premultiplied_alpha = (uint8_t)(alpha && format == 5);
    out->stride = stride;
    return MENU_SWF_BITMAP_OK;
}

static int summary_tag_next(const uint8_t *p, size_t len, size_t *off,
                            unsigned *code, const uint8_t **payload,
                            uint32_t *payload_len) {
    if (*off >= len) return 0;
    if (len - *off < 2) return -1;
    uint16_t h = (uint16_t)(p[*off] | ((uint16_t)p[*off + 1] << 8));
    *off += 2;
    *code = h >> 6;
    uint32_t n = h & 0x3fu;
    if (n == 0x3f) {
        if (len - *off < 4) return -1;
        n = (uint32_t)p[*off] | ((uint32_t)p[*off + 1] << 8) |
            ((uint32_t)p[*off + 2] << 16) |
            ((uint32_t)p[*off + 3] << 24);
        *off += 4;
    }
    if (n > len - *off) return -1;
    *payload = p + *off;
    *payload_len = n;
    *off += n;
    return 1;
}

static void summary_item(MenuSwfBitmapSummary *s, uint16_t id, uint16_t w,
                         uint16_t h, uint8_t tag, uint8_t format,
                         uint8_t alpha) {
    ++s->total_tags;
    if (s->stored >= MENU_SWF_BITMAP_MAX_ITEMS) {
        s->truncated = 1;
        return;
    }
    MenuSwfBitmapInfo *it = &s->items[s->stored++];
    it->character_id = id;
    it->width = w;
    it->height = h;
    it->tag_code = tag;
    it->format = format;
    it->has_alpha = alpha;
}

static MenuSwfBitmapStatus summary_walk(const uint8_t *p, size_t len,
                                        unsigned depth,
                                        MenuSwfBitmapSummary *s,
                                        char *reason, size_t reason_cap) {
    size_t off = 0;
    while (off < len) {
        unsigned code;
        const uint8_t *payload;
        uint32_t plen;
        int tr = summary_tag_next(p, len, &off, &code, &payload, &plen);
        if (tr < 0) {
            bitmap_reason(reason, reason_cap, "bitmap summary: truncated tag");
            return MENU_SWF_BITMAP_ERR_FORMAT;
        }
        if (tr == 0) break;
        if (code == 0) return MENU_SWF_BITMAP_OK;
        if (code == 20 || code == 36) {
            if (plen < 7) {
                bitmap_reason(reason, reason_cap,
                              "bitmap summary: lossless header is truncated");
                return MENU_SWF_BITMAP_ERR_FORMAT;
            }
            uint8_t alpha = code == 36;
            if (alpha) ++s->lossless2_tags;
            else ++s->lossless_tags;
            summary_item(s, rd16(payload), rd16(payload + 3),
                         rd16(payload + 5), (uint8_t)code, payload[2], alpha);
        } else if (code == 6) {
            ++s->define_bits_tags;
            summary_item(s, plen >= 2 ? rd16(payload) : 0, 0, 0,
                         (uint8_t)code, 0, 0);
        } else if (code == 21 || code == 35 || code == 90) {
            ++s->jpeg_tags;
            summary_item(s, plen >= 2 ? rd16(payload) : 0, 0, 0,
                         (uint8_t)code, 0, 0);
        } else if (code == 39) {
            if (depth >= 16 || plen < 4) {
                bitmap_reason(reason, reason_cap,
                              "bitmap summary: bad DefineSprite nesting");
                return MENU_SWF_BITMAP_ERR_FORMAT;
            }
            MenuSwfBitmapStatus st = summary_walk(
                payload + 4, plen - 4, depth + 1, s, reason, reason_cap);
            if (st != MENU_SWF_BITMAP_OK) return st;
        }
    }
    return MENU_SWF_BITMAP_OK;
}

MenuSwfBitmapStatus menu_swf_bitmap_summary(
    const uint8_t *data, size_t size, MenuSwfBitmapSummary *out,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_BITMAP_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!data) return MENU_SWF_BITMAP_ERR_ARGUMENT;
    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason,
                                           reason_cap);
    if (st != MENU_SWF_OK) return MENU_SWF_BITMAP_ERR_FORMAT;
    MenuSwfBitmapStatus bs = summary_walk(
        view.body + view.tag_offset, view.body_len - view.tag_offset, 0, out,
        reason, reason_cap);
    menu_swf_view_close(&view);
    return bs;
}
