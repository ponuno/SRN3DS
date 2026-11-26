#pragma once
/* Bounded SWF display-list, DefineFont3 metrics and DefineEditText definitions.
 *
 * This module does not execute AVM/AS2, rasterise glyphs, build a renderable
 * display list or draw.  It models PlaceObject/RemoveObject state at each
 * ShowFrame, records bounded matrices/colours/names, summarizes sprites,
 * reports DefineFont2/3 glyph metrics and collects DefineEditText records.
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
#define MENU_SWF_DISPLAY_MAX_EDIT_TEXTS 64u
#define MENU_SWF_DISPLAY_EDIT_TEXT_MAX 4096u
#define MENU_SWF_DISPLAY_EDIT_VARIABLE_MAX 256u

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
    uint8_t flags;           /* raw FontFlags (Bold=0x01, Italic=0x02) */
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
    MenuSwfFontSummary *items; /* caller-owned */
    uint32_t capacity;
    uint32_t count;
    uint32_t total;
    uint32_t skipped; /* oversized definitions recorded in skip_reason */
    char skip_reason[BSA_REASON_MAX];
    int truncated;
} MenuSwfFontList;

typedef enum {
    MENU_SWF_EDIT_TEXT_ALIGN_LEFT = 0,
    MENU_SWF_EDIT_TEXT_ALIGN_RIGHT = 1,
    MENU_SWF_EDIT_TEXT_ALIGN_CENTER = 2,
    MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY = 3
} MenuSwfEditTextAlign;

typedef struct {
    uint16_t character_id;
    int32_t bounds[4]; /* xmin, xmax, ymin, ymax in twips */
    uint16_t flags;
    uint8_t has_text;
    uint8_t word_wrap;
    uint8_t multiline;
    uint8_t password;
    uint8_t read_only;
    uint8_t has_text_color;
    uint8_t has_max_length;
    uint8_t has_font;
    uint8_t has_layout;
    uint8_t no_select;
    uint8_t has_border;
    uint8_t was_static;
    uint8_t html;
    uint8_t use_outlines;
    uint16_t font_id;
    uint16_t font_height; /* twips */
    uint16_t max_length;
    uint8_t color[4]; /* RGBA */
    uint8_t alignment;
    int32_t left_margin;  /* twips */
    int32_t right_margin; /* twips */
    int32_t indent;       /* twips, first line */
    int32_t leading;      /* signed twips */
    char variable_name[MENU_SWF_DISPLAY_EDIT_VARIABLE_MAX];
    char initial_text[MENU_SWF_DISPLAY_EDIT_TEXT_MAX];
    uint32_t initial_text_length;
    uint32_t trailing_bytes;
} MenuSwfEditText;

typedef struct {
    MenuSwfEditText *items; /* caller-owned */
    uint32_t capacity;
    uint32_t count;
    uint32_t total;         /* definitions encountered, including overflow */
    int truncated;
} MenuSwfEditTextList;

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

/* Parse one PlaceObject2 (26) or PlaceObject3 (70) payload into the bounded
 * display-object fields.  Filters followed by undecodable fields are
 * E_NOTIMPL; malformed tags are E_FORMAT.  Used by the flattened display-list
 * builder. */
MenuSwfStatus menu_swf_display_parse_place(const uint8_t *p, size_t len,
                                           uint8_t tag_code,
                                           MenuSwfDisplayObject *out,
                                           char *reason,
                                           size_t reason_cap);

/* Parse the container, build the top-level display list, summarize every
 * DefineSprite (bounded), re-walk the first sprite with placements for its
 * frames, and report DefineFont2/3 metrics.  ZWS, DefineFont4, DoABC/AVM2 and
 * structural corruption are named refusals.  `out` is cleared on failure. */
MenuSwfStatus menu_swf_display_summary(const uint8_t *data, size_t size,
                                       MenuSwfDisplaySummary *out,
                                       char *reason, size_t reason_cap);

/* Parse one DefineEditText (37) payload.  Text/variable overflow is a named
 * BUDGET refusal, malformed fields are FORMAT, and an unsupported alignment is
 * NOTIMPL.  `out` is cleared on failure. */
MenuSwfStatus menu_swf_display_parse_edit_text(
    const uint8_t *payload, size_t len, MenuSwfEditText *out,
    char *reason, size_t reason_cap);

/* Walk top-level and nested DefineSprite tags and collect DefineEditText
 * definitions into caller-owned `list->items`.  Capacity overflow is a named
 * BUDGET refusal with total/truncated recorded. */
MenuSwfStatus menu_swf_display_collect_edit_texts(
    const uint8_t *data, size_t size, MenuSwfEditTextList *list,
    char *reason, size_t reason_cap);

const MenuSwfEditText *menu_swf_display_find_edit_text(
    const MenuSwfEditTextList *list, uint16_t character_id);

/* Parse one DefineFont2/3 tag payload into a caller-owned summary. */
MenuSwfStatus menu_swf_display_parse_font(
    const uint8_t *payload, size_t len, uint8_t font_type,
    MenuSwfFontSummary *out, char *reason, size_t reason_cap);

/* Collect all top-level/nested DefineFont2/3 definitions from one SWF into a
 * caller-owned list.  Duplicate ids and malformed tags are FORMAT; caller
 * capacity is a named BUDGET refusal.  Oversized unrelated definitions are
 * skipped and recorded in skipped/skip_reason so later exact ids remain
 * searchable. */
MenuSwfStatus menu_swf_display_collect_fonts(
    const uint8_t *data, size_t size, MenuSwfFontList *list,
    char *reason, size_t reason_cap);

const MenuSwfFontSummary *menu_swf_display_find_font(
    const MenuSwfFontList *list, uint16_t font_id);

/* Resolve an imported symbol by exact DefineFont id.  Names are diagnostic
 * only.  A missing id is NOTIMPL with
 * "imported font id N is not present in <candidate_source>". */
MenuSwfStatus menu_swf_display_resolve_import_font(
    const MenuSwfImportList *imports, const MenuSwfFontList *candidates,
    uint16_t imported_id, const char *candidate_source,
    const MenuSwfFontSummary **out, char *reason, size_t reason_cap);
