#include "menu_swf_atlas.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t glyph_index;
    uint32_t input_index;
    uint32_t w, h, x, y;
} AtlasPlan;

static void atlas_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *menu_swf_atlas_status_name(MenuSwfAtlasStatus status) {
    switch (status) {
    case MENU_SWF_ATLAS_OK: return "ok";
    case MENU_SWF_ATLAS_ERR_ARGUMENT: return "bad atlas argument";
    case MENU_SWF_ATLAS_ERR_TOO_MANY: return "too many glyphs";
    case MENU_SWF_ATLAS_ERR_TOO_LARGE: return "glyph larger than atlas";
    case MENU_SWF_ATLAS_ERR_OVERFLOW: return "atlas overflow";
    default: return "unknown atlas status";
    }
}

static int plan_before(const AtlasPlan *a, const AtlasPlan *b) {
    if (a->h != b->h) return a->h > b->h;
    if (a->w != b->w) return a->w > b->w;
    if (a->glyph_index != b->glyph_index)
        return a->glyph_index < b->glyph_index;
    return a->input_index < b->input_index;
}

static void sort_plan(AtlasPlan *plan, uint32_t count) {
    for (uint32_t i = 1; i < count; ++i) {
        AtlasPlan key = plan[i];
        uint32_t j = i;
        while (j > 0 && plan_before(&key, &plan[j - 1])) {
            plan[j] = plan[j - 1];
            --j;
        }
        plan[j] = key;
    }
}

static uint32_t uv16(uint32_t coord, uint32_t size) {
    return (uint32_t)(((uint64_t)coord << 16) / size);
}

MenuSwfAtlasStatus menu_swf_atlas_pack(
    const MenuSwfAtlasGlyph *glyphs, uint32_t glyph_count,
    const MenuSwfAtlasTarget *target, MenuSwfAtlasEntry *entries,
    uint32_t entry_capacity, MenuSwfAtlasResult *out, char *reason,
    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ATLAS_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!target) {
        atlas_reason(reason, reason_cap, "swf atlas: needs a target");
        return MENU_SWF_ATLAS_ERR_ARGUMENT;
    }
    if (target->width == 0 || target->height == 0 ||
        target->stride < target->width || !target->pixels) {
        atlas_reason(reason, reason_cap,
                     "swf atlas: target %ux%u stride %u has no usable rows",
                     target->width, target->height, target->stride);
        return MENU_SWF_ATLAS_ERR_ARGUMENT;
    }
    if (target->width > MENU_SWF_ATLAS_MAX_DIM ||
        target->height > MENU_SWF_ATLAS_MAX_DIM ||
        target->stride > MENU_SWF_ATLAS_MAX_STRIDE) {
        atlas_reason(reason, reason_cap,
                     "swf atlas: target %ux%u stride %u exceeds limits",
                     target->width, target->height, target->stride);
        return MENU_SWF_ATLAS_ERR_TOO_LARGE;
    }
    if (glyph_count > MENU_SWF_ATLAS_MAX_GLYPHS ||
        glyph_count > entry_capacity || (glyph_count && (!glyphs || !entries))) {
        atlas_reason(reason, reason_cap,
                     "swf atlas: %u glyphs exceed %u capacity",
                     glyph_count, entry_capacity);
        return MENU_SWF_ATLAS_ERR_TOO_MANY;
    }

    AtlasPlan plan[MENU_SWF_ATLAS_MAX_GLYPHS];
    uint32_t plan_count = 0;
    uint32_t empty = 0;
    for (uint32_t i = 0; i < glyph_count; ++i) {
        const MenuSwfAtlasGlyph *g = &glyphs[i];
        if (g->width == 0 || g->height == 0) {
            ++empty;
            continue;
        }
        if (g->width > target->width || g->height > target->height) {
            atlas_reason(reason, reason_cap,
                         "swf atlas: glyph %u is %ux%u, atlas is %ux%u",
                         g->glyph_index, g->width, g->height, target->width,
                         target->height);
            return MENU_SWF_ATLAS_ERR_TOO_LARGE;
        }
        if (!g->pixels || g->stride < g->width) {
            atlas_reason(reason, reason_cap,
                         "swf atlas: glyph %u has no usable source rows",
                         g->glyph_index);
            return MENU_SWF_ATLAS_ERR_ARGUMENT;
        }
        AtlasPlan *p = &plan[plan_count++];
        p->glyph_index = g->glyph_index;
        p->input_index = i;
        p->w = g->width;
        p->h = g->height;
    }
    sort_plan(plan, plan_count);

    uint32_t shelf_y = 0, shelf_h = 0, cursor_x = 0;
    uint32_t used_height = 0, occupied = 0;
    for (uint32_t i = 0; i < plan_count; ++i) {
        AtlasPlan *p = &plan[i];
        if (cursor_x && cursor_x + p->w > target->width) {
            shelf_y += shelf_h + 1;
            cursor_x = 0;
            shelf_h = 0;
        }
        if (shelf_y + p->h > target->height) {
            atlas_reason(reason, reason_cap,
                         "swf atlas: no shelf room for glyph %u at %u+%u",
                         p->glyph_index, shelf_y, p->h);
            return MENU_SWF_ATLAS_ERR_OVERFLOW;
        }
        p->x = cursor_x;
        p->y = shelf_y;
        cursor_x += p->w + 1; /* 1px padding between rects */
        if (p->h > shelf_h) shelf_h = p->h;
        if (p->y + p->h > used_height) used_height = p->y + p->h;
        occupied += p->w * p->h;
    }

    /* Plan succeeded: only now touch the caller's atlas and entries. */
    for (uint32_t y = 0; y < target->height; ++y)
        memset(target->pixels + (size_t)y * target->stride, 0,
               target->width);
    if (glyph_count)
        memset(entries, 0, (size_t)glyph_count * sizeof *entries);
    for (uint32_t i = 0; i < glyph_count; ++i)
        entries[i].glyph_index = glyphs[i].glyph_index;
    for (uint32_t i = 0; i < plan_count; ++i) {
        const AtlasPlan *p = &plan[i];
        const MenuSwfAtlasGlyph *g = &glyphs[p->input_index];
        MenuSwfAtlasEntry *e = &entries[p->input_index];
        e->has_rect = 1;
        e->x = p->x;
        e->y = p->y;
        e->w = p->w;
        e->h = p->h;
        e->u = uv16(p->x, target->width);
        e->v = uv16(p->y, target->height);
        e->uw = uv16(p->x + p->w, target->width) - e->u;
        e->vh = uv16(p->y + p->h, target->height) - e->v;
        for (uint32_t r = 0; r < p->h; ++r)
            memcpy(target->pixels + (size_t)(p->y + r) * target->stride +
                       p->x,
                   g->pixels + (size_t)r * g->stride, p->w);
    }

    out->width = target->width;
    out->height = target->height;
    out->used_height = used_height;
    out->occupied_pixels = occupied;
    out->placed_glyphs = plan_count;
    out->empty_glyphs = empty;
    if (used_height)
        out->occupancy_16 =
            (uint32_t)((uint64_t)occupied * 65536u /
                       ((uint64_t)target->width * used_height));
    return MENU_SWF_ATLAS_OK;
}
