#pragma once
/* Bounded SWF lossless bitmap decoding (CPU RGBA8, no upload/GPU/frame).
 *
 * DefineBitsLossless (20) formats 3/4/5: 8-bit palette, 15-bit RGB555 and
 * 24-bit RGB stored as 32-bit 0RGB.  DefineBitsLossless2 (36) formats 3 and 5
 * are palette RGBA and 32-bit ARGB; format 4 is also accepted as opaque RGB555
 * for Flash Player compatibility even though SWF19 documents only 3/5.  Per
 * the SWF specification the zlib stream contains the palette followed by
 * row-padded pixel data, every row is padded to a 32-bit boundary, palette
 * entries are R,G,B,A, and direct Lossless2 ARGB data is premultiplied ("the
 * RGB data must already be multiplied by the alpha channel value").  The
 * decoder preserves those bytes and reports `premultiplied_alpha=1` instead of
 * silently unpremultiplying.  Palette RGBA is returned as stored (straight)
 * with `premultiplied_alpha=0`.  JPEG tags are named refusals; no JPEG decoder
 * exists in this repository.
 */

#include <stddef.h>
#include <stdint.h>

#include "bsa_reader.h"

#define MENU_SWF_BITMAP_MAX_ITEMS 64u
#define MENU_SWF_BITMAP_MAX_DIM 4096u
#define MENU_SWF_BITMAP_MAX_PIXELS 16777216u

typedef enum {
    MENU_SWF_BITMAP_OK = 0,
    MENU_SWF_BITMAP_ERR_ARGUMENT,
    MENU_SWF_BITMAP_ERR_FORMAT,
    MENU_SWF_BITMAP_ERR_ZLIB,
    MENU_SWF_BITMAP_ERR_SIZE,
    MENU_SWF_BITMAP_ERR_OVERFLOW,
    MENU_SWF_BITMAP_ERR_JPEG
} MenuSwfBitmapStatus;

typedef struct {
    uint16_t character_id;
    uint16_t width, height;
    uint8_t tag_code;
    uint8_t format;
    uint8_t has_alpha;
    uint8_t premultiplied_alpha;
    uint32_t stride; /* output row bytes supplied by caller */
} MenuSwfBitmap;

const char *menu_swf_bitmap_status_name(MenuSwfBitmapStatus status);

/* Decode one lossless payload into caller RGBA8.  `scratch` holds the exact
 * inflated palette+pixel bytes; it is never smaller than needed.  The output
 * is written only after successful inflate and validation.  `payload` starts
 * at CharacterID (the tag header is not included). */
MenuSwfBitmapStatus menu_swf_bitmap_decode(
    uint8_t tag_code, const uint8_t *payload, size_t payload_size,
    uint8_t *rgba, size_t rgba_capacity, uint32_t stride, uint8_t *scratch,
    size_t scratch_capacity, MenuSwfBitmap *out, char *reason,
    size_t reason_cap);

typedef struct {
    uint16_t character_id;
    uint16_t width, height;
    uint8_t tag_code;
    uint8_t format;
    uint8_t has_alpha;
} MenuSwfBitmapInfo;

typedef struct {
    uint32_t lossless_tags;
    uint32_t lossless2_tags;
    uint32_t jpeg_tags;
    uint32_t define_bits_tags;
    uint32_t total_tags;
    uint32_t stored;
    int truncated;
    MenuSwfBitmapInfo items[MENU_SWF_BITMAP_MAX_ITEMS];
} MenuSwfBitmapSummary;

/* Walk top-level and nested tags and report bitmap character ids/sizes.  It
 * does not decode pixels or reject JPEG payloads (the decode API does). */
MenuSwfBitmapStatus menu_swf_bitmap_summary(
    const uint8_t *data, size_t size, MenuSwfBitmapSummary *out,
    char *reason, size_t reason_cap);
