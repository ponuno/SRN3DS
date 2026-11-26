#pragma once
/* Bounded clip-depth/mask rectangles derived from a flattened SWF display list.
 *
 * A record with ClipDepth creates a mask active for display depths
 * (mask.depth, mask.clip_depth].  Masks compose by intersection; up to eight
 * may be active on one record.  Mask rectangles are axis-aligned screen-space
 * AABBs from the record matrix and caller-provided local shape bounds.  A mask
 * whose shape is not a rectangle/quad is refused by name; shape tessellation is
 * not implemented here.  No AVM, GPU or frame is produced.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf_display_list.h"
#include "menu_swf_shape.h"

#define MENU_SWF_CLIP_MAX_ACTIVE 8u
#define MENU_SWF_CLIP_MAX_MASKS 256u
#define MENU_SWF_CLIP_MAX_RECORDS 8192u

typedef enum {
    MENU_SWF_CLIP_OK = 0,
    MENU_SWF_CLIP_ERR_ARGUMENT,
    MENU_SWF_CLIP_ERR_PAIR,
    MENU_SWF_CLIP_ERR_MASK,
    MENU_SWF_CLIP_ERR_NESTING,
    MENU_SWF_CLIP_ERR_OVERFLOW,
    MENU_SWF_CLIP_ERR_CYCLE
} MenuSwfClipStatus;

typedef struct {
    int32_t xmin, xmax, ymin, ymax; /* screen twips */
} MenuSwfClipRect;

typedef struct {
    MenuSwfShapeRect bounds; /* local shape/glyph bounds */
    uint8_t has_bounds;
    uint8_t is_rect; /* 1 = rectangle/quad, 0 = unsupported complex shape */
} MenuSwfClipShape;

typedef struct {
    int32_t x, y;
} MenuSwfClipPoint;

typedef struct {
    uint32_t mask_record_index;
    uint16_t mask_depth;
    uint16_t clip_depth;
    MenuSwfClipRect rect;
    MenuSwfClipPoint quad[4];
} MenuSwfClipMask;

typedef struct {
    uint32_t record_index;
    MenuSwfClipRect rect;      /* record AABB; valid only when has_rect */
    MenuSwfClipRect clip_rect; /* intersection with active masks */
    uint8_t has_rect;
    uint8_t fully_clipped;
    uint8_t mask_count;
    uint32_t mask_indices[MENU_SWF_CLIP_MAX_ACTIVE];
} MenuSwfClipRecord;

typedef struct {
    uint32_t record_count;
    uint32_t mask_count;
    uint32_t clipped_records;
    uint32_t max_active_masks;
} MenuSwfClipResult;

const char *menu_swf_clip_status_name(MenuSwfClipStatus status);

/* Axis-aligned half-open intersection; returns 1 when non-empty, 0 otherwise. */
int menu_swf_clip_rect_intersect(const MenuSwfClipRect *a,
                                 const MenuSwfClipRect *b,
                                 MenuSwfClipRect *out);

/* Prepare the four transformed mask corners (TL, TR, BR, BL) in twips.
 * `shape` must have bounds and is_rect; complex masks are the named
 * "mask shape tessellation is not implemented" refusal. */
MenuSwfClipStatus menu_swf_clip_mask_quad(
    const MenuSwfDisplayListRecord *record, const MenuSwfClipShape *shape,
    MenuSwfClipPoint out[4], char *reason, size_t reason_cap);

/* Build one clip record and one mask entry per ClipDepth record.  `shapes` may
 * be NULL when no mask bounds are needed; otherwise it is indexed like
 * `records` and must cover `record_count`.  Masks may be NULL only when
 * `mask_capacity` is zero.  All validation and counting precedes any write. */
MenuSwfClipStatus menu_swf_clip_build(
    const MenuSwfDisplayListRecord *records, uint32_t record_count,
    const MenuSwfClipShape *shapes, uint32_t shape_count,
    MenuSwfClipMask *masks, uint32_t mask_capacity,
    MenuSwfClipRecord *out_records, uint32_t record_capacity,
    MenuSwfClipResult *out, char *reason, size_t reason_cap);
