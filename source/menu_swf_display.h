#pragma once
/* Bounded SWF display-list and DefineFont3 metrics for the main menu.
 *
 * This module does not execute AVM/AS2, rasterise glyphs, build a renderable
 * display list or draw.  It models PlaceObject/RemoveObject state at each
 * ShowFrame, records bounded matrices/colours/names, summarizes sprites and
 * reports DefineFont2/3 glyph metrics (code -> shape offset, advances, bounds).
 * All limits are compile-time constants; overflow is visible via `truncated`
 * fields or a named error.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf.h"

#define MENU_SWF_DISPLAY_MAX_FRAMES 8u
#define MENU_SWF_DISPLAY_MAX_OBJECTS 12u
#define MENU_SWF_DISPLAY_MAX_LIVE 32u
#define MENU_SWF_DISPLAY_MAX_SPRITES 128u
#define MENU_SWF_DISPLAY_MAX_GLYPHS 256u
#define MENU_SWF_DISPLAY_MAX_FONTS 2u
#define MENU_SWF_DISPLAY_MAX_KERNING 256u
#define MENU_SWF_DISPLAY_NAME_MAX 48u
#define MENU_SWF_DISPLAY_SPRITE_DEPTH 16u

typedef struct {
    uint16_t depth;
    uint16_t character_id;
    uint16_t clip_depth;
    uint16_t ratio;

    uint8_t has_character;
    uint8_t move;
    uint8_t has_matrix;
    uint8_t has_color_transform;
    uint8_t has_name;
    uint8_t has_ratio;
    uint8_t has_clip_depth;
    uint8_t has_clip_actions;
    uint8_t has_image;
    uint8_t has_filter_list;
    uint8_t has_blend_mode;
    uint8_t has_cache_as_bitmap;
    uint8_t has_visible;
    uint8_t has_opaque_background;
    uint8_t has_class_name;
    uint8_t visible;
    uint8_t blend_mode;
    uint8_t bitmap_cache;
    uint8_t background[4];
    uint32_t clip_actions_bytes;

    /* MATRIX: scale/rotation are 16.16 fixed point; translation is twips. */
    float scale_x;
    float scale_y;
    float rotate_skew0;
    float rotate_skew1;
    int32_t translate_x;
    int32_t translate_y;

    /* CXFORMWITHALPHA, RGBA order. */
    int16_t color_mult[4];
    int16_t color_add[4];

    char name[MENU_SWF_DISPLAY_NAME_MAX];
    char class_name[MENU_SWF_DISPLAY_NAME_MAX];
    uint32_t filter_list_bytes; /* opaque when HasFilterList is set */
} MenuSwfDisplayObject;

typedef struct {
    uint32_t frame_index;
    uint32_t live_objects;   /* all live objects at this ShowFrame */
    uint32_t stored_objects; /* copied into objects[] */
    int truncated;
    MenuSwfDisplayObject objects[MENU_SWF_DISPLAY_MAX_OBJECTS];
} MenuSwfDisplayFrame;

typedef struct {
    uint32_t frame_count;    /* total ShowFrames; frames[] stores the first N */
    uint32_t placements;
    uint32_t removals;
    uint32_t max_objects;
    int truncated;
    MenuSwfDisplayFrame frames[MENU_SWF_DISPLAY_MAX_FRAMES];
} MenuSwfDisplayList;

typedef struct {
    uint16_t sprite_id;
    uint16_t declared_frames;
    uint32_t frame_count;
    uint32_t placements;
    uint32_t removals;
    uint32_t max_objects;
    uint16_t depth;
    int truncated;
} MenuSwfSpriteSummary;

typedef struct {
    uint16_t code;
    uint16_t glyph_index;
    uint32_t shape_offset; /* relative to the DefineFont tag payload */
    int16_t advance;
    int32_t bounds[4];     /* raw font units (Font3 shape scale is 20) */
    uint8_t has_advance;
    uint8_t has_bounds;
} MenuSwfGlyph;

typedef struct {
    uint16_t code1;
    uint16_t code2;
    int16_t adjustment;
} MenuSwfKerning;

typedef struct {
    uint16_t font_id;
    uint16_t glyph_count;
    uint16_t stored_glyphs;
    uint8_t font_type;       /* 2 = DefineFont2, 3 = DefineFont3 */
    uint8_t has_layout;
    uint8_t wide_offsets;
    uint8_t wide_codes;
    uint8_t shape_scale;     /* 1 for Font2, 20 for Font3 */
    uint16_t em_square;      /* SWF glyphs are defined on 1024x1024 */
    int32_t ascent;
    int32_t descent;
    int32_t leading;
    uint16_t kerning_count;
    uint16_t stored_kerning;
    uint32_t offset_table_base;
    uint32_t code_table_offset;
    uint32_t trailing_bytes;
    char name[64];
    int truncated;
    MenuSwfGlyph glyphs[MENU_SWF_DISPLAY_MAX_GLYPHS];
    MenuSwfKerning kerning[MENU_SWF_DISPLAY_MAX_KERNING];
} MenuSwfFontSummary;

typedef struct {
    MenuSwfDisplayList top;
    uint32_t sprite_total;
    uint32_t stored_sprites;
    MenuSwfSpriteSummary sprites[MENU_SWF_DISPLAY_MAX_SPRITES];
    int32_t selected_sprite; /* index into sprites, -1 when none */
    MenuSwfDisplayList selected; /* frames for the selected sprite */
    uint32_t font_total;
    uint32_t stored_fonts;
    MenuSwfFontSummary fonts[MENU_SWF_DISPLAY_MAX_FONTS];
    int truncated;
} MenuSwfDisplaySummary;

/* Parse the container, build the top-level display list, summarize every
 * DefineSprite (bounded), re-walk the first sprite with placements for its
 * frames, and report DefineFont2/3 metrics.  ZWS, DefineFont4, DoABC/AVM2 and
 * structural corruption are named refusals.  `out` is cleared on failure. */
MenuSwfStatus menu_swf_display_summary(const uint8_t *data, size_t size,
                                       MenuSwfDisplaySummary *out,
                                       char *reason, size_t reason_cap);
