#include "menu_swf_display.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded readers                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t off;
} Reader;

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t bit;
} Bits;

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static int rd_u8(Reader *r, uint8_t *v) {
    if (r->off >= r->len) return -1;
    *v = r->p[r->off++];
    return 0;
}

static int rd_u16(Reader *r, uint16_t *v) {
    if (r->len - r->off < 2) return -1;
    *v = (uint16_t)(r->p[r->off] | ((uint16_t)r->p[r->off + 1] << 8));
    r->off += 2;
    return 0;
}

static int rd_s16(Reader *r, int16_t *v) {
    uint16_t u;
    if (rd_u16(r, &u)) return -1;
    *v = (int16_t)u;
    return 0;
}

static int rd_u32(Reader *r, uint32_t *v) {
    if (r->len - r->off < 4) return -1;
    *v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
         ((uint32_t)r->p[r->off + 2] << 16) |
         ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return 0;
}

static int rd_string_measured(Reader *r, char *dst, size_t cap, int *truncated,
                              uint32_t *actual) {
    size_t start = r->off;
    while (r->off < r->len && r->p[r->off]) ++r->off;
    if (r->off >= r->len) return -1;
    size_t n = r->off - start;
    if (actual) *actual = (uint32_t)n;
    ++r->off;
    if (n >= cap) {
        n = cap - 1;
        if (truncated) *truncated = 1;
    }
    if (n) memcpy(dst, r->p + start, n);
    dst[n] = 0;
    return 0;
}

static int rd_string(Reader *r, char *dst, size_t cap, int *truncated) {
    return rd_string_measured(r, dst, cap, truncated, NULL);
}

static int rd_fixed_name(Reader *r, size_t n, char *dst, size_t cap,
                         int *truncated) {
    if (r->len - r->off < n) return -1;
    const uint8_t *src = r->p + r->off;
    r->off += n;
    if (n && src[n - 1] == 0) --n;
    if (n >= cap) {
        n = cap - 1;
        if (truncated) *truncated = 1;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

static int bt_bits(Bits *b, unsigned n, uint32_t *out) {
    if (n > 31 || b->bit + n > b->len * 8) return -1;
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        size_t bit = b->bit + i;
        v = (v << 1) | ((b->p[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    b->bit += n;
    *out = v;
    return 0;
}

static int bt_signed(Bits *b, unsigned n, int32_t *out) {
    uint32_t v;
    if (bt_bits(b, n, &v)) return -1;
    if (n && (v & (1u << (n - 1)))) v |= ~((1u << n) - 1u);
    *out = (int32_t)v;
    return 0;
}

static void bt_align(Bits *b) {
    b->bit = (b->bit + 7u) & ~(size_t)7u;
}

/* ------------------------------------------------------------------ */
/* MATRIX / CXFORM / RECT                                              */
/* ------------------------------------------------------------------ */

static int parse_matrix(Reader *r, MenuSwfDisplayObject *o) {
    Bits b = {r->p, r->len, r->off * 8};
    uint32_t flag, nbits;
    int32_t v;
    o->scale_x = o->scale_y = 1.0f;
    o->rotate_skew0 = o->rotate_skew1 = 0.0f;
    o->translate_x = o->translate_y = 0;
    if (bt_bits(&b, 1, &flag)) return -1;
    if (flag) {
        if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
        if (bt_signed(&b, nbits, &v)) return -1;
        o->scale_x = (float)v / 65536.0f;
        if (bt_signed(&b, nbits, &v)) return -1;
        o->scale_y = (float)v / 65536.0f;
    }
    if (bt_bits(&b, 1, &flag)) return -1;
    if (flag) {
        if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
        if (bt_signed(&b, nbits, &v)) return -1;
        o->rotate_skew0 = (float)v / 65536.0f;
        if (bt_signed(&b, nbits, &v)) return -1;
        o->rotate_skew1 = (float)v / 65536.0f;
    }
    if (bt_bits(&b, 5, &nbits) || nbits > 31) return -1;
    if (bt_signed(&b, nbits, &v)) return -1;
    o->translate_x = v;
    if (bt_signed(&b, nbits, &v)) return -1;
    o->translate_y = v;
    bt_align(&b);
    if (b.bit > r->len * 8) return -1;
    r->off = b.bit / 8;
    o->has_matrix = 1;
    return 0;
}

static int parse_color_transform(Reader *r, MenuSwfDisplayObject *o) {
    Bits b = {r->p, r->len, r->off * 8};
    uint32_t has_add, has_mult, nbits;
    for (unsigned i = 0; i < 4; ++i) {
        o->color_mult[i] = 256;
        o->color_add[i] = 0;
    }
    if (bt_bits(&b, 1, &has_add) || bt_bits(&b, 1, &has_mult) ||
        bt_bits(&b, 4, &nbits) || nbits > 15) return -1;
    if (has_mult) {
        for (unsigned i = 0; i < 4; ++i) {
            int32_t v;
            if (bt_signed(&b, nbits, &v)) return -1;
            o->color_mult[i] = (int16_t)v;
        }
    }
    if (has_add) {
        for (unsigned i = 0; i < 4; ++i) {
            int32_t v;
            if (bt_signed(&b, nbits, &v)) return -1;
            o->color_add[i] = (int16_t)v;
        }
    }
    bt_align(&b);
    if (b.bit > r->len * 8) return -1;
    r->off = b.bit / 8;
    o->has_color_transform = 1;
    return 0;
}

static int read_rect(Bits *b, int32_t out[4]) {
    uint32_t nbits;
    if (bt_bits(b, 5, &nbits) || nbits > 31) return -1;
    for (unsigned i = 0; i < 4; ++i)
        if (bt_signed(b, nbits, &out[i])) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PlaceObject / RemoveObject                                          */
/* ------------------------------------------------------------------ */

static int parse_place2(const uint8_t *p, size_t len,
                        MenuSwfDisplayObject *o) {
    memset(o, 0, sizeof *o);
    Reader r = {p, len, 0};
    uint8_t flags;
    if (rd_u8(&r, &flags)) return -1;
    o->has_clip_actions = (flags & 0x80) != 0;
    o->has_clip_depth = (flags & 0x40) != 0;
    o->has_name = (flags & 0x20) != 0;
    o->has_ratio = (flags & 0x10) != 0;
    o->has_color_transform = (flags & 0x08) != 0;
    o->has_matrix = (flags & 0x04) != 0;
    o->has_character = (flags & 0x02) != 0;
    o->move = (flags & 0x01) != 0;
    if (rd_u16(&r, &o->depth)) return -1;
    if (o->has_character && rd_u16(&r, &o->character_id)) return -1;
    if (o->has_matrix && parse_matrix(&r, o)) return -1;
    if (o->has_color_transform && parse_color_transform(&r, o)) return -1;
    if (o->has_ratio && rd_u16(&r, &o->ratio)) return -1;
    if (o->has_name && rd_string(&r, o->name, sizeof o->name, NULL)) return -1;
    if (o->has_clip_depth && rd_u16(&r, &o->clip_depth)) return -1;
    if (o->has_clip_actions) {
        o->clip_actions_bytes = (uint32_t)(r.len - r.off);
        r.off = r.len; /* AVM/AS2 clip actions are not executed. */
    }
    return r.off <= r.len ? 0 : -1;
}

static int parse_place3(const uint8_t *p, size_t len,
                        MenuSwfDisplayObject *o) {
    memset(o, 0, sizeof *o);
    Reader r = {p, len, 0};
    uint8_t flags0, flags1;
    if (rd_u8(&r, &flags0) || rd_u8(&r, &flags1)) return -1;
    if (flags1 & 0x80) return -1; /* reserved */
    o->has_clip_actions = (flags0 & 0x80) != 0;
    o->has_clip_depth = (flags0 & 0x40) != 0;
    o->has_name = (flags0 & 0x20) != 0;
    o->has_ratio = (flags0 & 0x10) != 0;
    o->has_color_transform = (flags0 & 0x08) != 0;
    o->has_matrix = (flags0 & 0x04) != 0;
    o->has_character = (flags0 & 0x02) != 0;
    o->move = (flags0 & 0x01) != 0;
    o->has_opaque_background = (flags1 & 0x40) != 0;
    o->has_visible = (flags1 & 0x20) != 0;
    o->has_image = (flags1 & 0x10) != 0;
    o->has_class_name = (flags1 & 0x08) != 0;
    o->has_cache_as_bitmap = (flags1 & 0x04) != 0;
    o->has_blend_mode = (flags1 & 0x02) != 0;
    o->has_filter_list = (flags1 & 0x01) != 0;
    if (rd_u16(&r, &o->depth)) return -1;
    if ((o->has_class_name || (o->has_image && o->has_character)) &&
        rd_string(&r, o->class_name, sizeof o->class_name, NULL)) return -1;
    if (o->has_character && rd_u16(&r, &o->character_id)) return -1;
    if (o->has_matrix && parse_matrix(&r, o)) return -1;
    if (o->has_color_transform && parse_color_transform(&r, o)) return -1;
    if (o->has_ratio && rd_u16(&r, &o->ratio)) return -1;
    if (o->has_name && rd_string(&r, o->name, sizeof o->name, NULL)) return -1;
    if (o->has_clip_depth && rd_u16(&r, &o->clip_depth)) return -1;
    if (o->has_filter_list) {
        o->filter_list_bytes = (uint32_t)(r.len - r.off);
        if (o->has_blend_mode || o->has_cache_as_bitmap || o->has_visible ||
            o->has_clip_actions)
            return -2; /* filters followed by fields we do not decode */
        r.off = r.len;
    } else {
        if (o->has_blend_mode && rd_u8(&r, &o->blend_mode)) return -1;
        if (o->has_cache_as_bitmap && rd_u8(&r, &o->bitmap_cache)) return -1;
        if (o->has_visible) {
            if (rd_u8(&r, &o->visible)) return -1;
            for (unsigned i = 0; i < 4; ++i)
                if (rd_u8(&r, &o->background[i])) return -1;
        }
        if (o->has_clip_actions) {
            o->clip_actions_bytes = (uint32_t)(r.len - r.off);
            r.off = r.len;
        }
    }
    return r.off <= r.len ? 0 : -1;
}

static int parse_remove2(const uint8_t *p, size_t len, uint16_t *depth) {
    Reader r = {p, len, 0};
    return rd_u16(&r, depth) ? -1 : 0;
}

static int parse_remove(const uint8_t *p, size_t len, uint16_t *depth) {
    Reader r = {p, len, 0};
    uint16_t character;
    if (rd_u16(&r, &character) || rd_u16(&r, depth)) return -1;
    (void)character;
    return 0;
}

MenuSwfStatus menu_swf_display_parse_place(const uint8_t *p, size_t len,
                                           uint8_t tag_code,
                                           MenuSwfDisplayObject *out,
                                           char *reason,
                                           size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!p || !out) return MENU_SWF_ERR_ARGUMENT;
    int rc = tag_code == 26 ? parse_place2(p, len, out)
                            : tag_code == 70 ? parse_place3(p, len, out)
                                             : -1;
    if (rc == -2) {
        set_reason(reason, reason_cap,
                   "swf: PlaceObject3 filters hide later fields");
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (rc) {
        set_reason(reason, reason_cap,
                   "swf: malformed PlaceObject%d tag",
                   tag_code == 70 ? 3 : 2);
        return MENU_SWF_ERR_FORMAT;
    }
    return MENU_SWF_OK;
}

/* ------------------------------------------------------------------ */
/* DefineEditText                                                      */
/* ------------------------------------------------------------------ */

/* 0 = parsed, -1 = malformed, -2 = named NOTIMPL, -3 = named BUDGET. */
static int parse_edit_text(const uint8_t *p, size_t len, MenuSwfEditText *out,
                           char *reason, size_t reason_cap) {
    memset(out, 0, sizeof *out);
    Reader r = {p, len, 0};
    uint16_t flags;
    if (rd_u16(&r, &out->character_id)) return -1;
    Bits b = {p, len, r.off * 8};
    if (read_rect(&b, out->bounds)) return -1;
    bt_align(&b);
    r.off = b.bit / 8;
    if (rd_u16(&r, &flags)) return -1;
    if (flags & 0xc000u) return -1; /* reserved DefineEditText flag bits */
    out->flags = flags;
    out->has_text = (flags & 0x0001u) != 0;
    out->word_wrap = (flags & 0x0002u) != 0;
    out->multiline = (flags & 0x0004u) != 0;
    out->password = (flags & 0x0008u) != 0;
    out->read_only = (flags & 0x0010u) != 0;
    out->has_text_color = (flags & 0x0020u) != 0;
    out->has_max_length = (flags & 0x0040u) != 0;
    out->has_font = (flags & 0x0080u) != 0;
    out->has_layout = (flags & 0x0100u) != 0;
    out->no_select = (flags & 0x0200u) != 0;
    out->has_border = (flags & 0x0400u) != 0;
    out->was_static = (flags & 0x0800u) != 0;
    out->html = (flags & 0x1000u) != 0;
    out->use_outlines = (flags & 0x2000u) != 0;

    if (out->has_font &&
        (rd_u16(&r, &out->font_id) || rd_u16(&r, &out->font_height)))
        return -1;
    if (out->has_text_color) {
        for (unsigned i = 0; i < 4; ++i)
            if (rd_u8(&r, &out->color[i])) return -1;
    } else {
        out->color[0] = out->color[1] = out->color[2] = out->color[3] = 255;
    }
    if (out->has_max_length && rd_u16(&r, &out->max_length)) return -1;
    if (out->has_layout) {
        uint16_t left, right;
        int16_t indent, leading;
        if (rd_u8(&r, &out->alignment) || rd_u16(&r, &left) ||
            rd_u16(&r, &right) || rd_s16(&r, &indent) ||
            rd_s16(&r, &leading))
            return -1;
        /* Retail GFx writes 0xff/0xffff for default alignment/margins. */
        if (out->alignment == 0xffu) out->alignment = MENU_SWF_EDIT_TEXT_ALIGN_LEFT;
        if (left == 0xffffu) left = 0;
        if (right == 0xffffu) right = 0;
        out->left_margin = left;
        out->right_margin = right;
        out->indent = indent;
        out->leading = leading;
        if (out->alignment > MENU_SWF_EDIT_TEXT_ALIGN_JUSTIFY) return -2;
    }

    int truncated = 0;
    uint32_t actual = 0;
    if (rd_string_measured(&r, out->variable_name,
                           sizeof out->variable_name, &truncated, &actual)) {
        set_reason(reason, reason_cap,
                   "swf: DefineEditText id %u variable string at %u has no NUL",
                   out->character_id, (unsigned)r.off);
        return -1;
    }
    if (truncated) {
        set_reason(reason, reason_cap,
                   "swf: DefineEditText id %u variable name length %u exceeds %u",
                   out->character_id, actual,
                   (unsigned)sizeof out->variable_name);
        return -3;
    }
    if (out->has_text) {
        if (r.off == r.len) {
            out->initial_text_length = 0;
            out->initial_text[0] = 0;
        } else {
            size_t initial_start = r.off;
            if (rd_string_measured(&r, out->initial_text,
                                   sizeof out->initial_text, &truncated,
                                   &actual)) {
                set_reason(reason, reason_cap,
                           "swf: DefineEditText id %u initial string at %u/%u "
                           "has no NUL (bytes %02x %02x %02x %02x %02x %02x %02x %02x)",
                           out->character_id, (unsigned)initial_start,
                           (unsigned)r.len,
                           initial_start < r.len ? p[initial_start] : 0,
                           initial_start + 1 < r.len ? p[initial_start + 1] : 0,
                           initial_start + 2 < r.len ? p[initial_start + 2] : 0,
                           initial_start + 3 < r.len ? p[initial_start + 3] : 0,
                           initial_start + 4 < r.len ? p[initial_start + 4] : 0,
                           initial_start + 5 < r.len ? p[initial_start + 5] : 0,
                           initial_start + 6 < r.len ? p[initial_start + 6] : 0,
                           initial_start + 7 < r.len ? p[initial_start + 7] : 0);
                return -1;
            }
            if (truncated) {
                set_reason(reason, reason_cap,
                           "swf: DefineEditText id %u initial text length %u exceeds %u",
                           out->character_id, actual,
                           (unsigned)sizeof out->initial_text);
                return -3;
            }
            out->initial_text_length = actual;
        }
    }
    if (r.off != r.len) out->trailing_bytes = (uint32_t)(r.len - r.off);
    return 0;
}

MenuSwfStatus menu_swf_display_parse_edit_text(
    const uint8_t *payload, size_t len, MenuSwfEditText *out,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!payload || !out) return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    int rc = parse_edit_text(payload, len, out, reason, reason_cap);
    if (rc == -2) {
        set_reason(reason, reason_cap,
                   "swf: DefineEditText id %u flags=%04x font=%u height=%u "
                   "alignment=%u margins=%d/%d indent=%d leading=%d",
                   out->character_id, out->flags, out->font_id,
                   out->font_height, out->alignment, out->left_margin,
                   out->right_margin, out->indent, out->leading);
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (rc == -3) {
        if (!reason || !reason[0])
            set_reason(reason, reason_cap,
                       "swf: DefineEditText variable/text exceeds bounded storage");
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_BUDGET;
    }
    if (rc) {
        if (!reason || !reason[0])
            set_reason(reason, reason_cap,
                       "swf: malformed DefineEditText id %u tag",
                       out->character_id);
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_FORMAT;
    }
    return MENU_SWF_OK;
}

/* ------------------------------------------------------------------ */
/* Display builder                                                     */
/* ------------------------------------------------------------------ */

/* DisplayBuilder is created per DefineSprite nesting level.  Keeping the whole
 * 32-object live array inside every recursive frame overflowed the console's
 * main-thread stack, so each depth gets a static slice instead. */
#define MENU_SWF_DISPLAY_BUILDER_DEPTH 32u
static MenuSwfDisplayObject
    g_builder_live[MENU_SWF_DISPLAY_BUILDER_DEPTH][MENU_SWF_DISPLAY_MAX_LIVE];

typedef struct {
    MenuSwfDisplayObject *live;
    uint32_t live_count;
    uint32_t frame_count;
    uint32_t placements;
    uint32_t removals;
    uint32_t max_objects;
    int truncated;
    MenuSwfDisplayList *dest;
} DisplayBuilder;

static int find_depth(const DisplayBuilder *b, uint16_t depth) {
    for (uint32_t i = 0; i < b->live_count; ++i)
        if (b->live[i].depth == depth) return (int)i;
    return -1;
}

static void merge_object(MenuSwfDisplayObject *dst,
                         const MenuSwfDisplayObject *src) {
    if (src->has_character) {
        dst->character_id = src->character_id;
        dst->has_character = 1;
    }
    if (src->has_matrix) {
        dst->has_matrix = 1;
        dst->scale_x = src->scale_x;
        dst->scale_y = src->scale_y;
        dst->rotate_skew0 = src->rotate_skew0;
        dst->rotate_skew1 = src->rotate_skew1;
        dst->translate_x = src->translate_x;
        dst->translate_y = src->translate_y;
    }
    if (src->has_color_transform) {
        dst->has_color_transform = 1;
        memcpy(dst->color_mult, src->color_mult, sizeof dst->color_mult);
        memcpy(dst->color_add, src->color_add, sizeof dst->color_add);
    }
    if (src->has_ratio) dst->ratio = src->ratio;
    if (src->has_name) {
        memcpy(dst->name, src->name, sizeof dst->name);
        dst->has_name = 1;
    }
    if (src->has_clip_depth) {
        dst->clip_depth = src->clip_depth;
        dst->has_clip_depth = 1;
    }
    if (src->has_image) dst->has_image = 1;
    if (src->has_opaque_background) dst->has_opaque_background = 1;
    if (src->has_class_name) {
        memcpy(dst->class_name, src->class_name, sizeof dst->class_name);
        dst->has_class_name = 1;
    }
    if (src->has_filter_list) {
        dst->has_filter_list = 1;
        dst->filter_list_bytes = src->filter_list_bytes;
    }
    if (src->has_blend_mode) {
        dst->has_blend_mode = 1;
        dst->blend_mode = src->blend_mode;
    }
    if (src->has_cache_as_bitmap) {
        dst->has_cache_as_bitmap = 1;
        dst->bitmap_cache = src->bitmap_cache;
    }
    if (src->has_visible) {
        dst->has_visible = 1;
        dst->visible = src->visible;
        memcpy(dst->background, src->background, sizeof dst->background);
    }
    if (src->has_clip_actions) {
        dst->has_clip_actions = 1;
        dst->clip_actions_bytes = src->clip_actions_bytes;
    }
    dst->move = 1;
}

static int builder_place(DisplayBuilder *b, const MenuSwfDisplayObject *obj) {
    ++b->placements;
    int idx = find_depth(b, obj->depth);
    if (idx >= 0) {
        if (obj->move) merge_object(&b->live[idx], obj);
        else b->live[idx] = *obj;
        return 0;
    }
    if (obj->move) {
        /* Moving a depth that is not present changes nothing in the model. */
        return 0;
    }
    if (!obj->has_character && !obj->has_class_name && !obj->has_image)
        return -1;
    if (b->live_count >= MENU_SWF_DISPLAY_MAX_LIVE) {
        b->truncated = 1;
        return 0;
    }
    uint32_t at = 0;
    while (at < b->live_count && b->live[at].depth < obj->depth) ++at;
    memmove(&b->live[at + 1], &b->live[at],
            (b->live_count - at) * sizeof b->live[0]);
    b->live[at] = *obj;
    ++b->live_count;
    return 0;
}

static void builder_remove(DisplayBuilder *b, uint16_t depth) {
    ++b->removals;
    int idx = find_depth(b, depth);
    if (idx < 0) return;
    memmove(&b->live[idx], &b->live[idx + 1],
            (b->live_count - (uint32_t)idx - 1u) * sizeof b->live[0]);
    --b->live_count;
}

static void builder_show(DisplayBuilder *b) {
    if (b->live_count > b->max_objects) b->max_objects = b->live_count;
    if (b->dest && b->frame_count < MENU_SWF_DISPLAY_MAX_FRAMES) {
        MenuSwfDisplayFrame *f = &b->dest->frames[b->frame_count];
        memset(f, 0, sizeof *f);
        f->frame_index = b->frame_count;
        f->live_objects = b->live_count;
        f->stored_objects = b->live_count < MENU_SWF_DISPLAY_MAX_OBJECTS
                                ? b->live_count
                                : MENU_SWF_DISPLAY_MAX_OBJECTS;
        if (f->stored_objects < b->live_count) {
            f->truncated = 1;
            b->truncated = 1;
        }
        for (uint32_t i = 0; i < f->stored_objects; ++i)
            f->objects[i] = b->live[i];
        b->dest->truncated |= f->truncated;
    } else if (b->dest) {
        b->truncated = 1;
    }
    ++b->frame_count;
}

static void builder_finish(DisplayBuilder *b) {
    if (!b->dest) return;
    b->dest->frame_count = b->frame_count;
    b->dest->placements = b->placements;
    b->dest->removals = b->removals;
    b->dest->max_objects = b->max_objects;
    if (b->frame_count > MENU_SWF_DISPLAY_MAX_FRAMES) b->truncated = 1;
    b->dest->truncated |= b->truncated;
}

/* ------------------------------------------------------------------ */
/* DefineFont2/3                                                       */
/* ------------------------------------------------------------------ */

static int parse_font(const uint8_t *p, size_t len, uint8_t font_type,
                      MenuSwfFontSummary *f, char *reason, size_t reason_cap) {
    memset(f, 0, sizeof *f);
    Reader r = {p, len, 0};
    uint8_t flags, lang, name_len;
    uint16_t u16;
    if (rd_u16(&r, &f->font_id) || rd_u8(&r, &flags) || rd_u8(&r, &lang)) {
        set_reason(reason, reason_cap, "swf: DefineFont%d header is truncated",
                   font_type);
        return -1;
    }
    (void)lang;
    f->font_type = font_type;
    f->flags = flags;
    f->has_layout = (flags & 0x80) != 0;
    f->wide_offsets = (flags & 0x08) != 0;
    f->wide_codes = (flags & 0x04) != 0;
    f->shape_scale = font_type == 3 ? 20 : 1;
    f->em_square = 1024;
    if (font_type == 3 && !f->wide_codes) {
        set_reason(reason, reason_cap,
                   "swf: DefineFont3 requires FontFlagsWideCodes");
        return -1;
    }
    if (rd_u8(&r, &name_len) ||
        rd_fixed_name(&r, name_len, f->name, sizeof f->name, &f->truncated)) {
        set_reason(reason, reason_cap, "swf: DefineFont%d name is truncated",
                   font_type);
        return -1;
    }
    if (rd_u16(&r, &f->glyph_count)) {
        set_reason(reason, reason_cap, "swf: DefineFont%d glyph count missing",
                   font_type);
        return -1;
    }
    f->offset_table_base = (uint32_t)r.off;
    size_t offset_size = f->wide_offsets ? 4u : 2u;
    if ((uint64_t)f->glyph_count * offset_size > r.len - r.off) {
        set_reason(reason, reason_cap,
                   "swf: DefineFont%d offset table exceeds the tag", font_type);
        return -1;
    }
    for (uint32_t i = 0; i < f->glyph_count; ++i) {
        uint32_t off;
        if (offset_size == 4) {
            if (rd_u32(&r, &off)) return -1;
        } else {
            uint16_t o16;
            if (rd_u16(&r, &o16)) return -1;
            off = o16;
        }
        if (i < MENU_SWF_DISPLAY_MAX_GLYPHS)
            f->glyphs[i].shape_offset = f->offset_table_base + off;
        else
            f->truncated = 1;
    }
    uint32_t code_off;
    if (f->wide_offsets) {
        if (rd_u32(&r, &code_off)) return -1;
    } else {
        uint16_t o16;
        if (rd_u16(&r, &o16)) return -1;
        code_off = o16;
    }
    f->code_table_offset = f->offset_table_base + code_off;
    size_t code_size = f->wide_codes ? 2u : 1u;
    if (f->code_table_offset > r.len ||
        (uint64_t)f->glyph_count * code_size > r.len - f->code_table_offset) {
        set_reason(reason, reason_cap,
                   "swf: DefineFont%d code table exceeds the tag", font_type);
        return -1;
    }
    for (uint32_t i = 0; i < f->glyph_count; ++i) {
        uint32_t code;
        size_t at = f->code_table_offset + (size_t)i * code_size;
        if (code_size == 2)
            code = (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8);
        else
            code = p[at];
        if (i < MENU_SWF_DISPLAY_MAX_GLYPHS) {
            f->glyphs[i].code = (uint16_t)code;
            f->glyphs[i].glyph_index = (uint16_t)i;
        }
    }
    r.off = f->code_table_offset + (size_t)f->glyph_count * code_size;
    if (f->has_layout) {
        if (rd_u16(&r, &u16)) return -1;
        f->ascent = u16;
        if (rd_u16(&r, &u16)) return -1;
        f->descent = u16;
        int16_t s16;
        if (rd_s16(&r, &s16)) return -1;
        f->leading = s16;
        if ((uint64_t)f->glyph_count * 2u > r.len - r.off) {
            set_reason(reason, reason_cap,
                       "swf: DefineFont%d advance table exceeds the tag",
                       font_type);
            return -1;
        }
        for (uint32_t i = 0; i < f->glyph_count; ++i) {
            if (rd_s16(&r, &s16)) return -1;
            if (i < MENU_SWF_DISPLAY_MAX_GLYPHS) {
                f->glyphs[i].advance = s16;
                f->glyphs[i].has_advance = 1;
            }
        }
        Bits b = {p, len, r.off * 8};
        for (uint32_t i = 0; i < f->glyph_count; ++i) {
            int32_t rect[4];
            if (read_rect(&b, rect)) {
                set_reason(reason, reason_cap,
                           "swf: DefineFont%d bounds table is truncated",
                           font_type);
                return -1;
            }
            /* Retail FontBoundsTable RECTs are byte-aligned; the next RECT
             * starts on a fresh byte. */
            bt_align(&b);
            if (i < MENU_SWF_DISPLAY_MAX_GLYPHS) {
                memcpy(f->glyphs[i].bounds, rect, sizeof rect);
                f->glyphs[i].has_bounds = 1;
            }
        }
        if (b.bit > len * 8) {
            set_reason(reason, reason_cap,
                       "swf: DefineFont%d bounds table overruns", font_type);
            return -1;
        }
        r.off = b.bit / 8;
        if (rd_u16(&r, &f->kerning_count)) return -1;
        if ((uint64_t)f->kerning_count * 6u > r.len - r.off) {
            set_reason(reason, reason_cap,
                       "swf: DefineFont%d kerning table exceeds the tag",
                       font_type);
            return -1;
        }
        for (uint32_t i = 0; i < f->kerning_count; ++i) {
            uint16_t code1, code2;
            int16_t adjustment;
            if (rd_u16(&r, &code1) || rd_u16(&r, &code2) ||
                rd_s16(&r, &adjustment))
                return -1;
            if (i < MENU_SWF_DISPLAY_MAX_KERNING) {
                f->kerning[i].code1 = code1;
                f->kerning[i].code2 = code2;
                f->kerning[i].adjustment = adjustment;
                ++f->stored_kerning;
            } else {
                f->truncated = 1;
            }
        }
    }
    f->stored_glyphs = f->glyph_count < MENU_SWF_DISPLAY_MAX_GLYPHS
                           ? f->glyph_count
                           : MENU_SWF_DISPLAY_MAX_GLYPHS;
    f->trailing_bytes = (uint32_t)(r.len - r.off);
    return 0;
}

MenuSwfStatus menu_swf_display_parse_font(
    const uint8_t *payload, size_t len, uint8_t font_type,
    MenuSwfFontSummary *out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!payload || !out || (font_type != 2 && font_type != 3))
        return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (parse_font(payload, len, font_type, out, reason, reason_cap)) {
        if (!reason || !reason[0])
            set_reason(reason, reason_cap,
                       "swf: malformed DefineFont%u tag", font_type);
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_FORMAT;
    }
    if (out->truncated) {
        set_reason(reason, reason_cap,
                   "swf: DefineFont%u id %u exceeds bounded glyph/kerning tables",
                   font_type, out->font_id);
        memset(out, 0, sizeof *out);
        return MENU_SWF_ERR_BUDGET;
    }
    return MENU_SWF_OK;
}

typedef struct {
    MenuSwfFontList *list;
    uint32_t tags_seen;
} FontCollectContext;

static MenuSwfStatus font_collect_stream(FontCollectContext *ctx,
                                         const uint8_t *p, size_t len,
                                         unsigned depth,
                                         char *reason, size_t reason_cap) {
    if (depth > MENU_SWF_DISPLAY_SPRITE_DEPTH) {
        set_reason(reason, reason_cap,
                   "swf fonts: sprite depth exceeds %u",
                   MENU_SWF_DISPLAY_SPRITE_DEPTH);
        return MENU_SWF_ERR_TAG;
    }
    size_t off = 0;
    for (;;) {
        if (off >= len) {
            set_reason(reason, reason_cap, "swf fonts: missing End tag");
            return MENU_SWF_ERR_END;
        }
        if (len - off < 2) {
            set_reason(reason, reason_cap,
                       "swf fonts: truncated tag header");
            return MENU_SWF_ERR_TRUNCATED;
        }
        uint16_t header = (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
        off += 2;
        unsigned code = header >> 6;
        uint32_t tlen = header & 0x3fu;
        if (tlen == 0x3fu) {
            if (len - off < 4) {
                set_reason(reason, reason_cap,
                           "swf fonts: truncated long tag header");
                return MENU_SWF_ERR_TRUNCATED;
            }
            tlen = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
                   ((uint32_t)p[off + 2] << 16) |
                   ((uint32_t)p[off + 3] << 24);
            off += 4;
        }
        if (tlen > len - off) {
            set_reason(reason, reason_cap,
                       "swf fonts: tag %u length exceeds stream", code);
            return MENU_SWF_ERR_TAG;
        }
        const uint8_t *payload = p + off;
        off += tlen;
        if (++ctx->tags_seen > MENU_SWF_MAX_TAGS) {
            set_reason(reason, reason_cap,
                       "swf fonts: tag limit %u exceeded",
                       MENU_SWF_MAX_TAGS);
            return MENU_SWF_ERR_BUDGET;
        }
        if (code == 0) {
            if (tlen) {
                set_reason(reason, reason_cap,
                           "swf fonts: End tag has payload bytes");
                return MENU_SWF_ERR_FORMAT;
            }
            return MENU_SWF_OK;
        }
        if (code == 48 || code == 75) {
            MenuSwfFontList *list = ctx->list;
            ++list->total;
            if (list->count >= list->capacity) {
                list->truncated = 1;
                set_reason(reason, reason_cap,
                           "swf fonts: capacity %u exceeded", list->capacity);
                return MENU_SWF_ERR_BUDGET;
            }
            MenuSwfFontSummary font;
            MenuSwfStatus st = menu_swf_display_parse_font(
                payload, tlen, code == 75 ? 3 : 2, &font, reason, reason_cap);
            if (st == MENU_SWF_ERR_BUDGET) {
                ++list->skipped;
                list->truncated = 1;
                if (!list->skip_reason[0])
                    snprintf(list->skip_reason, sizeof list->skip_reason,
                             "%s", reason ? reason : "oversized DefineFont");
                continue;
            }
            if (st != MENU_SWF_OK) return st;
            for (uint32_t i = 0; i < list->count; ++i) {
                if (list->items[i].font_id == font.font_id) {
                    set_reason(reason, reason_cap,
                               "swf fonts: duplicate DefineFont id %u",
                               font.font_id);
                    return MENU_SWF_ERR_FORMAT;
                }
            }
            list->items[list->count++] = font;
        } else if (code == 39) {
            if (tlen < 4) {
                set_reason(reason, reason_cap,
                           "swf fonts: short DefineSprite");
                return MENU_SWF_ERR_FORMAT;
            }
            MenuSwfStatus st = font_collect_stream(
                ctx, payload + 4, tlen - 4, depth + 1, reason, reason_cap);
            if (st != MENU_SWF_OK) return st;
        }
    }
}

MenuSwfStatus menu_swf_display_collect_fonts(
    const uint8_t *data, size_t size, MenuSwfFontList *list,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!list || (list->capacity && !list->items))
        return MENU_SWF_ERR_ARGUMENT;
    list->count = 0;
    list->total = 0;
    list->skipped = 0;
    list->skip_reason[0] = 0;
    list->truncated = 0;
    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    FontCollectContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.list = list;
    st = font_collect_stream(&ctx, view.body + view.tag_offset,
                             view.body_len - view.tag_offset, 0, reason,
                             reason_cap);
    menu_swf_view_close(&view);
    return st;
}

const MenuSwfFontSummary *menu_swf_display_find_font(
    const MenuSwfFontList *list, uint16_t font_id) {
    if (!list || !list->items) return NULL;
    uint32_t count = list->count < list->capacity ? list->count : list->capacity;
    for (uint32_t i = 0; i < count; ++i)
        if (list->items[i].font_id == font_id) return &list->items[i];
    return NULL;
}

MenuSwfStatus menu_swf_display_resolve_import_font(
    const MenuSwfImportList *imports, const MenuSwfFontList *candidates,
    uint16_t imported_id, const char *candidate_source,
    const MenuSwfFontSummary **out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (out) *out = NULL;
    if (!imports || !candidates || !candidate_source)
        return MENU_SWF_ERR_ARGUMENT;
    const MenuSwfImportSymbol *symbol = NULL;
    const MenuSwfImportAssets *asset = menu_swf_find_import(
        imports, imported_id, &symbol);
    if (!asset || !symbol) {
        set_reason(reason, reason_cap,
                   "swf: imported font id %u is not declared by this movie",
                   imported_id);
        return MENU_SWF_ERR_NOTIMPL;
    }
    const MenuSwfFontSummary *font = menu_swf_display_find_font(
        candidates, imported_id);
    if (!font) {
        set_reason(reason, reason_cap,
                   "swf: imported font id %u is not present in %s "
                   "(symbol %s, URL %s)",
                   imported_id, candidate_source, symbol->name, asset->url);
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (out) *out = font;
    return MENU_SWF_OK;
}

/* ------------------------------------------------------------------ */
/* Tag walk                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *payload;
    size_t len;
} SpritePayload;

typedef struct {
    MenuSwfDisplaySummary *out;
    MenuSwfEditTextList *edit_texts;
    uint32_t tags_seen;
    SpritePayload sprite_payloads[MENU_SWF_DISPLAY_MAX_SPRITES];
} ParseContext;

static MenuSwfStatus parse_sprite_body(ParseContext *ctx, const uint8_t *p,
                                       size_t len, unsigned depth,
                                       int collect_sprites, int collect_fonts,
                                       MenuSwfSpriteSummary *summary,
                                       MenuSwfDisplayList *dest,
                                       char *reason, size_t reason_cap);

static MenuSwfStatus parse_display_stream(ParseContext *ctx, const uint8_t *p,
                                          size_t len, unsigned depth,
                                          DisplayBuilder *b,
                                          int collect_sprites,
                                          int collect_fonts,
                                          char *reason, size_t reason_cap) {
    size_t off = 0;
    while (off < len) {
        if (ctx->tags_seen++ >= MENU_SWF_MAX_TAGS) {
            set_reason(reason, reason_cap,
                       "swf display: more than %u tags", MENU_SWF_MAX_TAGS);
            return MENU_SWF_ERR_TAG;
        }
        if (len - off < 2) {
            set_reason(reason, reason_cap,
                       "swf display: truncated tag header at %llu",
                       (unsigned long long)off);
            return MENU_SWF_ERR_TRUNCATED;
        }
        uint16_t header = (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
        off += 2;
        uint32_t code = header >> 6;
        uint32_t tlen = header & 0x3fu;
        if (tlen == 0x3fu) {
            if (len - off < 4) {
                set_reason(reason, reason_cap,
                           "swf display: truncated long tag header at %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_TRUNCATED;
            }
            tlen = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
                   ((uint32_t)p[off + 2] << 16) |
                   ((uint32_t)p[off + 3] << 24);
            off += 4;
        }
        if (tlen > len - off) {
            set_reason(reason, reason_cap,
                       "swf display: tag %u length %u runs past the body",
                       code, tlen);
            return MENU_SWF_ERR_TAG;
        }
        const uint8_t *payload = p + off;
        if (code == 0) {
            if (tlen != 0) {
                set_reason(reason, reason_cap,
                           "swf display: End tag has %u payload bytes", tlen);
                return MENU_SWF_ERR_FORMAT;
            }
            return MENU_SWF_OK;
        }
        if (code == 1) {
            builder_show(b);
        } else if (code == 37) {
            if (ctx->edit_texts) {
                MenuSwfEditTextList *list = ctx->edit_texts;
                ++list->total;
                if (list->count >= list->capacity) {
                    list->truncated = 1;
                    set_reason(reason, reason_cap,
                               "swf display: DefineEditText capacity %u exceeded",
                               list->capacity);
                    return MENU_SWF_ERR_BUDGET;
                }
                MenuSwfEditText item;
                MenuSwfStatus est = menu_swf_display_parse_edit_text(
                    payload, tlen, &item, reason, reason_cap);
                if (est != MENU_SWF_OK) return est;
                for (uint32_t i = 0; i < list->count; ++i) {
                    if (list->items[i].character_id == item.character_id) {
                        set_reason(reason, reason_cap,
                                   "swf display: duplicate DefineEditText id %u",
                                   item.character_id);
                        return MENU_SWF_ERR_FORMAT;
                    }
                }
                list->items[list->count++] = item;
            }
        } else if (code == 26) {
            MenuSwfDisplayObject obj;
            if (parse_place2(payload, tlen, &obj) ||
                builder_place(b, &obj)) {
                set_reason(reason, reason_cap,
                           "swf display: malformed PlaceObject2 at %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
        } else if (code == 70) {
            MenuSwfDisplayObject obj;
            int pr = parse_place3(payload, tlen, &obj);
            if (pr == -2) {
                set_reason(reason, reason_cap,
                           "swf display: PlaceObject3 filter list followed by "
                           "unsupported fields is E_NOTIMPL");
                return MENU_SWF_ERR_NOTIMPL;
            }
            if (pr || builder_place(b, &obj)) {
                set_reason(reason, reason_cap,
                           "swf display: malformed PlaceObject3 at %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
        } else if (code == 28) {
            uint16_t d;
            if (parse_remove2(payload, tlen, &d)) {
                set_reason(reason, reason_cap,
                           "swf display: malformed RemoveObject2 at %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            builder_remove(b, d);
        } else if (code == 5) {
            uint16_t d;
            if (parse_remove(payload, tlen, &d)) {
                set_reason(reason, reason_cap,
                           "swf display: malformed RemoveObject at %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            builder_remove(b, d);
        } else if (code == 39) {
            if (collect_sprites) {
                if (depth >= MENU_SWF_DISPLAY_SPRITE_DEPTH) {
                    set_reason(reason, reason_cap,
                               "swf display: DefineSprite nesting exceeds %u",
                               MENU_SWF_DISPLAY_SPRITE_DEPTH);
                    return MENU_SWF_ERR_TAG;
                }
                MenuSwfDisplaySummary *out = ctx->out;
                ++out->sprite_total;
                uint32_t idx = out->stored_sprites;
                MenuSwfSpriteSummary local;
                MenuSwfSpriteSummary *summary = idx < MENU_SWF_DISPLAY_MAX_SPRITES
                                                     ? &out->sprites[idx]
                                                     : &local;
                MenuSwfStatus st = parse_sprite_body(
                    ctx, payload, tlen, depth, 1, collect_fonts, summary,
                    NULL, reason, reason_cap);
                if (st != MENU_SWF_OK) return st;
                if (idx < MENU_SWF_DISPLAY_MAX_SPRITES) {
                    ctx->sprite_payloads[idx].payload = payload;
                    ctx->sprite_payloads[idx].len = tlen;
                    ++out->stored_sprites;
                    if (out->selected_sprite < 0 &&
                        (summary->placements || summary->max_objects))
                        out->selected_sprite = (int32_t)idx;
                } else {
                    out->truncated = 1;
                }
            } else if (ctx->edit_texts) {
                if (depth >= MENU_SWF_DISPLAY_SPRITE_DEPTH) {
                    set_reason(reason, reason_cap,
                               "swf display: DefineSprite nesting exceeds %u",
                               MENU_SWF_DISPLAY_SPRITE_DEPTH);
                    return MENU_SWF_ERR_TAG;
                }
                MenuSwfStatus st = parse_sprite_body(
                    ctx, payload, tlen, depth, 0, collect_fonts, NULL, NULL,
                    reason, reason_cap);
                if (st != MENU_SWF_OK) return st;
            }
        } else if (code == 75 || code == 48) {
            if (collect_fonts) {
                MenuSwfDisplaySummary *out = ctx->out;
                ++out->font_total;
                if (out->stored_fonts < MENU_SWF_DISPLAY_MAX_FONTS) {
                    MenuSwfFontSummary *f = &out->fonts[out->stored_fonts];
                    if (parse_font(payload, tlen, code == 75 ? 3 : 2, f,
                                   reason, reason_cap)) {
                        if (!reason || !reason[0])
                            set_reason(reason, reason_cap,
                                       "swf display: malformed DefineFont%d metrics",
                                       code == 75 ? 3 : 2);
                        return MENU_SWF_ERR_FORMAT;
                    }
                    ++out->stored_fonts;
                } else {
                    out->truncated = 1;
                }
            }
        } else if (code == 91) {
            set_reason(reason, reason_cap,
                       "swf display: DefineFont4 (CFF) is E_NOTIMPL");
            return MENU_SWF_ERR_NOTIMPL;
        } else if (code == 82) {
            set_reason(reason, reason_cap,
                       "swf display: DoABC/AVM2 is E_NOTIMPL");
            return MENU_SWF_ERR_NOTIMPL;
        }
        off += tlen;
    }
    set_reason(reason, reason_cap, "swf display: tag stream has no End tag");
    return MENU_SWF_ERR_END;
}

static MenuSwfStatus parse_sprite_body(ParseContext *ctx, const uint8_t *p,
                                       size_t len, unsigned depth,
                                       int collect_sprites, int collect_fonts,
                                       MenuSwfSpriteSummary *summary,
                                       MenuSwfDisplayList *dest,
                                       char *reason, size_t reason_cap) {
    Reader r = {p, len, 0};
    uint16_t sprite_id, declared_frames;
    if (rd_u16(&r, &sprite_id) || rd_u16(&r, &declared_frames)) {
        set_reason(reason, reason_cap,
                   "swf display: DefineSprite header is truncated");
        return MENU_SWF_ERR_FORMAT;
    }
    DisplayBuilder b;
    memset(&b, 0, sizeof b);
    b.live = g_builder_live[(depth + 1u) < MENU_SWF_DISPLAY_BUILDER_DEPTH
                                ? (depth + 1u)
                                : (MENU_SWF_DISPLAY_BUILDER_DEPTH - 1u)];
    b.dest = dest;
    MenuSwfStatus st = parse_display_stream(
        ctx, p + 4, len - 4, depth + 1, &b, collect_sprites, collect_fonts,
        reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    builder_finish(&b);
    if (summary) {
        memset(summary, 0, sizeof *summary);
        summary->sprite_id = sprite_id;
        summary->declared_frames = declared_frames;
        summary->frame_count = b.frame_count;
        summary->placements = b.placements;
        summary->removals = b.removals;
        summary->max_objects = b.max_objects;
        summary->depth = (uint16_t)depth;
        summary->truncated = b.truncated;
    }
    return MENU_SWF_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

MenuSwfStatus menu_swf_display_summary(const uint8_t *data, size_t size,
                                       MenuSwfDisplaySummary *out,
                                       char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->selected_sprite = -1;

    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    ParseContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;

    DisplayBuilder top;
    memset(&top, 0, sizeof top);
    top.live = g_builder_live[0];
    top.dest = &out->top;
    st = parse_display_stream(&ctx, view.body + view.tag_offset,
                              view.body_len - view.tag_offset, 0, &top, 1, 1,
                              reason, reason_cap);
    if (st != MENU_SWF_OK) {
        menu_swf_view_close(&view);
        memset(out, 0, sizeof *out);
        return st;
    }
    builder_finish(&top);

    if (out->selected_sprite >= 0 &&
        out->selected_sprite < (int32_t)out->stored_sprites) {
        uint32_t idx = (uint32_t)out->selected_sprite;
        MenuSwfSpriteSummary *s = &out->sprites[idx];
        st = parse_sprite_body(&ctx, ctx.sprite_payloads[idx].payload,
                               ctx.sprite_payloads[idx].len, s->depth, 0, 0,
                               NULL, &out->selected, reason, reason_cap);
        if (st != MENU_SWF_OK) {
            menu_swf_view_close(&view);
            memset(out, 0, sizeof *out);
            return st;
        }
    }

    if (out->top.truncated) out->truncated = 1;
    if (out->selected.truncated) out->truncated = 1;
    for (uint32_t i = 0; i < out->stored_sprites; ++i)
        if (out->sprites[i].truncated) out->truncated = 1;
    for (uint32_t i = 0; i < out->stored_fonts; ++i)
        if (out->fonts[i].truncated) out->truncated = 1;

    menu_swf_view_close(&view);
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_display_collect_edit_texts(
    const uint8_t *data, size_t size, MenuSwfEditTextList *list,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!list || (list->capacity && !list->items))
        return MENU_SWF_ERR_ARGUMENT;
    list->count = 0;
    list->total = 0;
    list->truncated = 0;

    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    ParseContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.edit_texts = list;
    DisplayBuilder top;
    memset(&top, 0, sizeof top);
    top.live = g_builder_live[0];
    st = parse_display_stream(&ctx, view.body + view.tag_offset,
                              view.body_len - view.tag_offset, 0, &top, 0, 0,
                              reason, reason_cap);
    menu_swf_view_close(&view);
    return st;
}

const MenuSwfEditText *menu_swf_display_find_edit_text(
    const MenuSwfEditTextList *list, uint16_t character_id) {
    if (!list || !list->items) return NULL;
    uint32_t count = list->count < list->capacity ? list->count : list->capacity;
    for (uint32_t i = 0; i < count; ++i)
        if (list->items[i].character_id == character_id) return &list->items[i];
    return NULL;
}
