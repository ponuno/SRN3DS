#pragma once
/* Flattened, bounded SWF display list for the StartMenu movie (CPU only).
 *
 * The builder processes the first ShowFrame of the top-level timeline, then
 * recursively expands sprite placements at that frame.  PlaceObject2/3
 * matrices, colour transforms, clip depth and names are accumulated down the
 * sprite tree; RemoveObject/2 mutate the depth set.  The output is a caller
 * flat array of records in depth-first, depth-sorted order.
 *
 * No AVM execution, mask rendering, GPU submission or frame is produced.
 * Sprite cycles and the depth/record/live limits are named refusals.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf.h"
#include "menu_swf_display.h"

#define MENU_SWF_DISPLAY_LIST_MAX_RECORDS 8192u
#define MENU_SWF_DISPLAY_LIST_MAX_SPRITES 128u
#define MENU_SWF_DISPLAY_LIST_MAX_LIVE 128u
#define MENU_SWF_DISPLAY_LIST_MAX_DEPTH 32u
#define MENU_SWF_DISPLAY_LIST_NAME_MAX 48u

typedef struct {
    int32_t m00, m01, m10, m11; /* 16.16 affine */
    int32_t tx, ty;             /* twips */
} MenuSwfDisplayListMatrix;

typedef struct {
    int32_t mult[4]; /* 8.8, 256 = 1.0 */
    int32_t add[4];
} MenuSwfDisplayListColor;

typedef struct {
    uint16_t character_id;
    uint16_t depth;
    uint16_t clip_depth;
    uint16_t ratio;
    uint16_t parent_sprite_id;
    uint16_t sprite_depth;
    uint8_t has_character;
    uint8_t has_clip_depth;
    uint8_t has_name;
    uint8_t is_sprite;
    char name[MENU_SWF_DISPLAY_LIST_NAME_MAX];
    MenuSwfDisplayListMatrix matrix;
    MenuSwfDisplayListColor color;
} MenuSwfDisplayListRecord;

typedef struct {
    uint32_t record_count;       /* records actually written */
    uint32_t top_objects;        /* top-level live objects at frame 0 */
    uint32_t sprite_count;       /* DefineSprite definitions found */
    uint32_t sprite_placements;  /* emitted records whose character is a sprite */
    uint32_t placement_tags;
    uint32_t removals;
    uint32_t max_sprite_depth;
    int truncated;
} MenuSwfDisplayListResult;

/* Build the flattened list for the first ShowFrame.  `records` is caller-owned
 * and may be NULL only when `capacity` is zero.  On overflow, record_count is
 * the number written before the refusal and truncated is set. */
MenuSwfStatus menu_swf_display_list_build(
    const uint8_t *data, size_t size, MenuSwfDisplayListRecord *records,
    uint32_t capacity, MenuSwfDisplayListResult *out, char *reason,
    size_t reason_cap);
