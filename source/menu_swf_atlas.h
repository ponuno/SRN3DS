#pragma once
/* Bounded A8 glyph-atlas packing and composition.
 *
 * Coverage bitmaps from menu_swf_raster are packed as axis-aligned rectangles
 * with 1px gaps, using deterministic shelf/row packing: descending height,
 * descending width, then ascending glyph index and input order.  No rotation.
 * UVs are 16.16 normalized to the atlas size.  Empty glyphs occupy no space
 * and get a zero rect/UV.  The atlas is cleared and composed only after the
 * whole plan succeeds, so a refusal never overwrites a previous atlas or the
 * caller's output entries.  No upload, GPU, cache, allocation or frame.
 */

#include <stddef.h>
#include <stdint.h>

#define MENU_SWF_ATLAS_MAX_GLYPHS 512u
#define MENU_SWF_ATLAS_MAX_DIM 4096u
#define MENU_SWF_ATLAS_MAX_STRIDE (1u << 20)

typedef enum {
    MENU_SWF_ATLAS_OK = 0,
    MENU_SWF_ATLAS_ERR_ARGUMENT,
    MENU_SWF_ATLAS_ERR_TOO_MANY,
    MENU_SWF_ATLAS_ERR_TOO_LARGE,
    MENU_SWF_ATLAS_ERR_OVERFLOW
} MenuSwfAtlasStatus;

typedef struct {
    const uint8_t *pixels; /* top-left of the used coverage rect */
    uint32_t stride;       /* bytes per source row, >= width */
    uint32_t width;
    uint32_t height;
    uint16_t glyph_index;  /* identity and final packing tie-break */
} MenuSwfAtlasGlyph;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per atlas row, >= width */
    uint8_t *pixels;
} MenuSwfAtlasTarget;

typedef struct {
    uint16_t glyph_index;
    uint16_t has_rect;
    uint32_t x, y, w, h;
    uint32_t u, v, uw, vh; /* 16.16, 0..65536 */
} MenuSwfAtlasEntry;

typedef struct {
    uint32_t width, height;
    uint32_t used_height;
    uint32_t occupied_pixels; /* sum of w*h, padding excluded */
    uint32_t occupancy_16;    /* occupied / (width*used_height), 16.16 */
    uint32_t placed_glyphs;
    uint32_t empty_glyphs;
} MenuSwfAtlasResult;

const char *menu_swf_atlas_status_name(MenuSwfAtlasStatus status);

/* Pack `glyph_count` coverage bitmaps into the caller atlas.  `entries` is
 * indexed by input order and must have capacity for every input.  On success
 * the full width x height atlas is cleared (stride padding untouched), every
 * non-empty bitmap is copied at its rect, and padding remains zero.  On any
 * error the atlas, entries and result are left untouched (result is zeroed). */
MenuSwfAtlasStatus menu_swf_atlas_pack(
    const MenuSwfAtlasGlyph *glyphs, uint32_t glyph_count,
    const MenuSwfAtlasTarget *target, MenuSwfAtlasEntry *entries,
    uint32_t entry_capacity, MenuSwfAtlasResult *out, char *reason,
    size_t reason_cap);
