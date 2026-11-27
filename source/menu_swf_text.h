#pragma once
/* Bounded DefineFont3/2 glyph tessellation, Latin-1 text layout and
 * DefineEditText field layout.
 *
 * A string is laid out with per-glyph advances and kerning from the caller's
 * MenuSwfFontSummary.  Each glyph's SHAPE is tessellated through
 * menu_swf_tess_glyph() in raw font units, then transformed into the caller's
 * output buffers.  DefineEditText adds WordWrap/Multiline/alignment/Justify,
 * margins/indent/leading, Password masking and a bounded GFx HTML run model.
 * There is no rasterisation, atlas, shaping beyond advance+kerning, bidi, or
 * frame.
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
#define MENU_SWF_TEXT_MAX_RUNS 128u
#define MENU_SWF_TEXT_FACE_MAX 32u
#define MENU_SWF_TEXT_HTML_STACK 16u
#define MENU_SWF_TEXT_MISSING_GLYPH 0xffffu

typedef struct {
    /* 16.16 affine matrix followed by twip translation. */
    int32_t m00, m01, m10, m11;
    int32_t tx, ty;
    uint32_t color; /* default ARGB (record/style tint) */
    uint8_t has_color_transform; /* apply color_mult/add to run colours */
    int32_t color_mult[4];       /* 8.8, 256 = 1.0 */
    int32_t color_add[4];
} MenuSwfTextStyle;

typedef struct {
    uint32_t text_first, text_length; /* range in the plain text buffer */
    uint32_t glyph_first, glyph_count; /* filled by edit layout */
    uint32_t color;                    /* explicit run ARGB */
    uint16_t size;                     /* explicit HTML size, 0 = default */
    uint8_t has_color, has_size, has_face;
    int32_t letter_spacing; /* 16.16 output pixels, applied after each glyph */
    uint8_t has_letter_spacing;
    uint8_t kerning;        /* default 1; HTML kerning="0" disables pairs */
    uint8_t has_kerning;
    uint8_t bold, italic, underline;
    uint8_t align;                     /* MenuSwfEditTextAlign */
    char face[MENU_SWF_TEXT_FACE_MAX];
} MenuSwfTextRun;

typedef struct {
    uint32_t runs;
    uint32_t tags;
    uint32_t start_tags, end_tags;
    uint32_t font_tags, b_tags, i_tags, u_tags, br_tags, p_tags, li_tags;
    uint32_t unknown_tags;
    uint32_t entities;
    uint32_t color_attrs, size_attrs, face_attrs, align_attrs;
    uint32_t letter_spacing_attrs, kerning_attrs;
    uint32_t unknown_attrs;
} MenuSwfTextHtmlSummary;

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
    uint16_t run_index;
    uint8_t has_color;
    uint32_t color; /* per-run ARGB, used by the text batch */
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

    /* Optional caller-owned runs for edit-text layout. */
    MenuSwfTextRun *runs;
    uint32_t run_capacity;

    /* Caller scratch for one glyph.  The caller fills the buffers/capacities
     * exactly as for menu_swf_tess_shape(); its output counts are reset. */
    MenuSwfTessMesh glyph_tess;

    /* Output. */
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t glyph_count;
    uint32_t line_count;
    uint32_t run_count;
    uint32_t missing_glyphs;
    int32_t pen_x, pen_y; /* raw font-unit pen after the last advance */
    MenuSwfShapeRect bounds;
    uint8_t has_bounds;
    uint16_t font_id;
    uint16_t em_square;
    uint8_t shape_scale;
    MenuSwfTextStyle style;
    MenuSwfTextHtmlSummary html;
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
 * provides it, otherwise NOTIMPL.  GFx HTML labels in the variable-name slot
 * are authored rich text and are not password-masked.  A missing glyph is a
 * named NOTIMPL rather than a silently collapsed field.  Retail GFx HTML
 * payloads stored in the variable-name slot are parsed into runs; explicit run
 * colours/sizes/flags/letter-spacing/kerning affect layout and per-vertex colour,
 * while face names are metadata-only and unknown tags preserve their text.
 * Empty text keeps the transformed edit bounds and emits one empty line/zero
 * glyphs. */
MenuSwfStatus menu_swf_text_layout_edit(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfEditText *edit, const MenuSwfTextStyle *style,
    MenuSwfTextMesh *out, char *reason, size_t reason_cap);

/* Resolved-field variant: `font` is the DefineFont that actually contains the
 * glyphs for an imported field font (for example fonts_en.swf id 3 for an
 * imported id 34), so the edit's stored `font_id` is allowed to differ from
 * `font->font_id`.  `run_fonts` is optional; when non-NULL it is indexed by
 * MenuSwfTextRun index and supplies the real face for each HTML run (NULL
 * entries fall back to `font`).  Line metrics still come from `font`.  The
 * caller is responsible for proving the substitution through
 * menu_swf_font_resolve()/menu_swf_font_resolve_face() and logging it. */
MenuSwfStatus menu_swf_text_layout_edit_resolved(
    const uint8_t *data, size_t size, const MenuSwfFontSummary *font,
    const MenuSwfFontSummary *const *run_fonts, const MenuSwfEditText *edit,
    const MenuSwfTextStyle *style, MenuSwfTextMesh *out, char *reason,
    size_t reason_cap);

/* Extract the bounded plain source used by edit layout (including the measured
 * GFx HTML-in-variable-name convention).  No font is required. */
MenuSwfStatus menu_swf_text_edit_plain(
    const MenuSwfEditText *edit, char *dst, size_t capacity, size_t *out_len,
    char *reason, size_t reason_cap);

/* Parse the same source into plain text plus bounded style runs.  Supported
 * HTML: <font color="#RRGGBB" size="N" face="..." letterSpacing="N"
 * kerning="0|1">, <b>, <i>, <u>, <br>, <p align=...> and <li> (paragraph
 * break).  Entities: amp/lt/gt/quot/apos/nbsp.  Unknown tags are skipped while
 * their inner text is preserved and counted in `summary->unknown_tags`;
 * malformed tags and unknown entities are named refusals.  Face names are retained but this layer has no face-to-font
 * matching: layout uses the supplied font for every run.  `runs` may be NULL
 * only with `run_capacity` zero; `summary` is optional. */
MenuSwfStatus menu_swf_text_edit_runs(
    const MenuSwfEditText *edit, char *text, size_t text_capacity,
    size_t *text_len, MenuSwfTextRun *runs, uint32_t run_capacity,
    uint32_t *run_count, MenuSwfTextHtmlSummary *summary, char *reason,
    size_t reason_cap);
