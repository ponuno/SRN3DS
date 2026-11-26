#pragma once
/* Bounded DefineFont3/2 glyph tessellation, Latin-1 text layout and
 * DefineEditText field layout.
 *
 * A string is laid out with per-glyph advances and kerning from the caller's
 * MenuSwfFontSummary.  Each glyph's SHAPE is tessellated through
 * menu_swf_tess_glyph() in raw font units, then transformed into the caller's
 * output buffers.  DefineEditText adds WordWrap/Multiline/alignment/Justify,
 * margins/indent/leading, Password masking and bounded HTML plain extraction.
 * There is no rasterisation, atlas, rich-text spans, shaping beyond
 * advance+kerning, bidi, or frame.
 *
 * Font3 coordinates and advances are in 20x font units; Font2 uses 1x units.
 * The 16.16 matrix in MenuSwfTextStyle is the caller's unit conversion (for
 * example 1/20 for Font3 pixels); translation is in the same output units.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf_display.h"
#include "menu_swf_tess.h"

#define MENU_SWF_TEXT_MAX_CHARS 4096u
#define MENU_SWF_TEXT_MAX_GLYPHS 4096u
#define MENU_SWF_TEXT_MAX_VERTICES 262144u
#define MENU_SWF_TEXT_MAX_INDICES 786432u
#define MENU_SWF_TEXT_MAX_TRIANGLES 262144u
#define MENU_SWF_TEXT_MAX_LINES 256u
#define MENU_SWF_TEXT_MISSING_GLYPH 0xffffu

typedef struct {
    /* 16.16 affine matrix followed by twip translation. */
    int32_t m00, m01, m10, m11;
    int32_t tx, ty;
    uint32_t color; /* ARGB copied to each output vertex */
} MenuSwfTextStyle;

typedef struct {
    int32_t x, y;       /* transformed placement */
    int32_t pen_x, pen_y; /* raw font-unit pen before this glyph */
    int32_t advance;    /* raw font units */
    int16_t kerning;    /* raw font units applied before this glyph */
    uint16_t code;
    uint16_t glyph_index; /* MENU_SWF_TEXT_MISSING_GLYPH when absent */
    uint32_t vertex_first, vertex_count;
    uint32_t triangle_first, triangle_count;
    MenuSwfShapeRect bounds;
    uint8_t has_geometry;
    uint8_t missing;
    uint16_t line_index;
} MenuSwfTextGlyph;

typedef struct {
    int32_t x, y; /* transformed output units */
    uint32_t color;
} MenuSwfTextVertex;

typedef struct {
    int32_t x, y, width, height;
    uint32_t glyph_first, glyph_count;
    uint8_t has_content;
} MenuSwfTextLine;

typedef struct {
    /* Caller output buffers.  A NULL pointer is allowed only with zero
     * capacity; a too-small capacity is a named budget refusal. */
    MenuSwfTextVertex *vertices;
    uint32_t vertex_capacity;
    uint32_t *indices;
    uint32_t index_capacity; /* uint32_t entries */
    MenuSwfTextGlyph *glyphs;
    uint32_t glyph_capacity;

    /* Optional caller-owned line records for edit-text layout. */
    MenuSwfTextLine *lines;
    uint32_t line_capacity;

    /* Caller scratch for one glyph.  The caller fills the buffers/capacities
     * exactly as for menu_swf_tess_shape(); its output counts are reset. */
    MenuSwfTessMesh glyph_tess;

    /* Output. */
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t glyph_count;
    uint32_t line_count;
    uint32_t missing_glyphs;
    int32_t pen_x, pen_y; /* raw font-unit pen after the last advance */
    MenuSwfShapeRect bounds;
    uint8_t has_bounds;
    uint16_t font_id;
    uint16_t em_square;
    uint8_t shape_scale;
    MenuSwfTextStyle style;
} MenuSwfTextMesh;

/* Lay out and tessellate `text_len` Latin-1 bytes using `font` (usually from
 * menu_swf_display_summary).  Known glyphs with empty outlines still produce a
 * placement and advance; unknown codes produce a placement with
 * glyph_index=MENU_SWF_TEXT_MISSING_GLYPH, zero advance and no triangles.
 * Glyph fills use the non-zero rule and style 1.  Overflow of any hard or
 * caller limit is MENU_SWF_ERR_BUDGET; malformed font geometry propagates the
 * shape/tess status. */
MenuSwfStatus menu_swf_text_layout(const uint8_t *data, size_t size,
                                   const MenuSwfFontSummary *font,
                                   const char *text, size_t text_len,
                                   const MenuSwfTextStyle *style,
                                   MenuSwfTextMesh *out, char *reason,
                                   size_t reason_cap);

/* Lay out one DefineEditText in local pixel coordinates.  `style` maps raw
 * font units to pixels and must be axis-aligned/positive.  WordWrap uses the
 * edit bounds/margins, explicit newlines require Multiline, alignment and
 * first-line indent/leading are applied, and Justify distributes extra space
 * after spaces on non-final lines.  Password fields display '*' when the font
 * provides it, otherwise NOTIMPL.  Retail GFx HTML payloads stored in the
 * variable-name slot are stripped to bounded plain text.  Empty text keeps the
 * transformed edit bounds and emits one empty line/zero glyphs. */
MenuSwfStatus menu_swf_text_layout_edit(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfEditText *edit, const MenuSwfTextStyle *style,
    MenuSwfTextMesh *out, char *reason, size_t reason_cap);

/* Extract the bounded plain source used by edit layout (including the measured
 * GFx HTML-in-variable-name convention).  No font is required. */
MenuSwfStatus menu_swf_text_edit_plain(
    const MenuSwfEditText *edit, char *dst, size_t capacity, size_t *out_len,
    char *reason, size_t reason_cap);
