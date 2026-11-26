#include "menu_swf_display_list.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MATRIX_COMPONENT_LIMIT (1 << 26)
#define MATRIX_TRANSLATE_LIMIT (1 << 30)
#define COLOR_LIMIT (1 << 24)

typedef struct {
    uint16_t id;
    const uint8_t *tags;
    size_t len;
} SpriteDef;

typedef struct {
    uint16_t depth;
    uint16_t character_id;
    uint16_t clip_depth;
    uint16_t ratio;
    uint8_t has_character;
    uint8_t has_clip_depth;
    uint8_t has_name;
    uint8_t is_sprite;
    char name[MENU_SWF_DISPLAY_LIST_NAME_MAX];
    MenuSwfDisplayListMatrix matrix;
    MenuSwfDisplayListColor color;
} LiveObject;

typedef struct {
    const uint8_t *body;
    size_t body_len;
    SpriteDef sprites[MENU_SWF_DISPLAY_LIST_MAX_SPRITES];
    uint32_t sprite_count;
    MenuSwfDisplayListRecord *records;
    uint32_t capacity;
    uint32_t count;
    MenuSwfDisplayListResult *out;
    MenuSwfStatus status;
    char *reason;
    size_t reason_cap;
} Ctx;

static void dl_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static void fail(Ctx *c, MenuSwfStatus status, const char *fmt, ...) {
    if (c->status != MENU_SWF_OK) return;
    c->status = status;
    if (!c->reason || !c->reason_cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->reason, c->reason_cap, fmt, ap);
    va_end(ap);
    c->reason[c->reason_cap - 1] = 0;
}

static int tag_next(const uint8_t *p, size_t len, size_t *off,
                    unsigned *code, const uint8_t **payload,
                    uint32_t *payload_len) {
    if (*off >= len) return 0;
    if (len - *off < 2) return -1;
    uint16_t header = (uint16_t)(p[*off] | ((uint16_t)p[*off + 1] << 8));
    *off += 2;
    *code = header >> 6;
    uint32_t n = header & 0x3fu;
    if (n == 0x3fu) {
        if (len - *off < 4) return -1;
        n = (uint32_t)p[*off] | ((uint32_t)p[*off + 1] << 8) |
            ((uint32_t)p[*off + 2] << 16) |
            ((uint32_t)p[*off + 3] << 24);
        *off += 4;
    }
    if (n > len - *off) return -1;
    *payload = p + *off;
    *payload_len = n;
    *off += n;
    return 1;
}

static const SpriteDef *find_sprite(const Ctx *c, uint16_t id) {
    if (!id) return NULL;
    for (uint32_t i = 0; i < c->sprite_count; ++i)
        if (c->sprites[i].id == id) return &c->sprites[i];
    return NULL;
}

static void matrix_identity(MenuSwfDisplayListMatrix *m) {
    memset(m, 0, sizeof *m);
    m->m00 = m->m11 = 65536;
}

static void color_identity(MenuSwfDisplayListColor *c) {
    for (unsigned i = 0; i < 4; ++i) {
        c->mult[i] = 256;
        c->add[i] = 0;
    }
}

static void live_defaults(LiveObject *o, const MenuSwfDisplayObject *src) {
    memset(o, 0, sizeof *o);
    o->depth = src->depth;
    o->ratio = src->ratio;
    matrix_identity(&o->matrix);
    color_identity(&o->color);
}

static int matrix_from_place(Ctx *c, const MenuSwfDisplayObject *src,
                             MenuSwfDisplayListMatrix *m) {
    matrix_identity(m);
    if (!src->has_matrix) return 0;
    double vals[4] = {src->scale_x, src->rotate_skew1, src->rotate_skew0,
                      src->scale_y};
    int32_t *dst[4] = {&m->m00, &m->m01, &m->m10, &m->m11};
    for (unsigned i = 0; i < 4; ++i) {
        double q = vals[i] * 65536.0;
        if (!isfinite(q) || q < -MATRIX_COMPONENT_LIMIT ||
            q > MATRIX_COMPONENT_LIMIT) {
            fail(c, MENU_SWF_ERR_BUDGET, "swf display list: matrix overflow");
            return -1;
        }
        *dst[i] = (int32_t)llround(q);
    }
    if (src->translate_x < -MATRIX_TRANSLATE_LIMIT ||
        src->translate_x > MATRIX_TRANSLATE_LIMIT ||
        src->translate_y < -MATRIX_TRANSLATE_LIMIT ||
        src->translate_y > MATRIX_TRANSLATE_LIMIT) {
        fail(c, MENU_SWF_ERR_BUDGET,
             "swf display list: matrix translation overflow");
        return -1;
    }
    m->tx = src->translate_x;
    m->ty = src->translate_y;
    return 0;
}

static void color_from_place(const MenuSwfDisplayObject *src,
                             MenuSwfDisplayListColor *col) {
    color_identity(col);
    if (!src->has_color_transform) return;
    for (unsigned i = 0; i < 4; ++i) {
        col->mult[i] = src->color_mult[i];
        col->add[i] = src->color_add[i];
    }
}

static int live_find(const LiveObject *live, uint32_t count, uint16_t depth) {
    for (uint32_t i = 0; i < count; ++i)
        if (live[i].depth == depth) return (int)i;
    return -1;
}

static int live_insert(Ctx *c, LiveObject *live, uint32_t *count,
                       const LiveObject *obj) {
    if (*count >= MENU_SWF_DISPLAY_LIST_MAX_LIVE) {
        fail(c, MENU_SWF_ERR_BUDGET,
             "swf display list: live object limit %u exceeded",
             MENU_SWF_DISPLAY_LIST_MAX_LIVE);
        return -1;
    }
    uint32_t at = 0;
    while (at < *count && live[at].depth < obj->depth) ++at;
    memmove(&live[at + 1], &live[at],
            (*count - at) * sizeof live[0]);
    live[at] = *obj;
    ++*count;
    return 0;
}

static void live_remove(LiveObject *live, uint32_t *count, uint16_t depth) {
    int idx = live_find(live, *count, depth);
    if (idx < 0) return;
    memmove(&live[idx], &live[idx + 1],
            (*count - (uint32_t)idx - 1u) * sizeof live[0]);
    --*count;
}

static int apply_place(Ctx *c, LiveObject *live, uint32_t *count,
                       const MenuSwfDisplayObject *obj) {
    MenuSwfDisplayListMatrix m;
    MenuSwfDisplayListColor col;
    if (matrix_from_place(c, obj, &m)) return -1;
    color_from_place(obj, &col);

    int idx = live_find(live, *count, obj->depth);
    if (obj->move) {
        if (idx < 0) return 0;
        LiveObject *dst = &live[idx];
        if (obj->has_character) {
            dst->character_id = obj->character_id;
            dst->has_character = 1;
        }
        if (obj->has_matrix) dst->matrix = m;
        if (obj->has_color_transform) dst->color = col;
        if (obj->has_ratio) dst->ratio = obj->ratio;
        if (obj->has_name) {
            memcpy(dst->name, obj->name, sizeof dst->name);
            dst->has_name = 1;
        }
        if (obj->has_clip_depth) {
            dst->clip_depth = obj->clip_depth;
            dst->has_clip_depth = 1;
        }
        dst->is_sprite = find_sprite(c, dst->character_id) != NULL;
        return 0;
    }

    LiveObject next;
    live_defaults(&next, obj);
    next.has_character = obj->has_character;
    next.character_id = obj->character_id;
    next.matrix = m;
    next.color = col;
    next.has_clip_depth = obj->has_clip_depth;
    next.clip_depth = obj->clip_depth;
    next.has_name = obj->has_name;
    if (obj->has_name) memcpy(next.name, obj->name, sizeof next.name);
    next.is_sprite = find_sprite(c, next.character_id) != NULL;
    if (!obj->has_character && !obj->has_class_name && !obj->has_image) {
        fail(c, MENU_SWF_ERR_FORMAT,
             "swf display list: PlaceObject has no character");
        return -1;
    }
    if (idx >= 0) {
        live[idx] = next;
        return 0;
    }
    return live_insert(c, live, count, &next);
}

static int compose_matrix(Ctx *c, const MenuSwfDisplayListMatrix *a,
                          const MenuSwfDisplayListMatrix *b,
                          MenuSwfDisplayListMatrix *out) {
    int64_t m00 = (int64_t)a->m00 * b->m00 + (int64_t)a->m01 * b->m10;
    int64_t m01 = (int64_t)a->m00 * b->m01 + (int64_t)a->m01 * b->m11;
    int64_t m10 = (int64_t)a->m10 * b->m00 + (int64_t)a->m11 * b->m10;
    int64_t m11 = (int64_t)a->m10 * b->m01 + (int64_t)a->m11 * b->m11;
    int64_t tx = (int64_t)a->m00 * b->tx + (int64_t)a->m01 * b->ty;
    int64_t ty = (int64_t)a->m10 * b->tx + (int64_t)a->m11 * b->ty;
    int64_t vals[4] = {m00 / 65536, m01 / 65536, m10 / 65536, m11 / 65536};
    int64_t tr[2] = {tx / 65536 + a->tx, ty / 65536 + a->ty};
    for (unsigned i = 0; i < 4; ++i) {
        if (vals[i] < -MATRIX_COMPONENT_LIMIT ||
            vals[i] > MATRIX_COMPONENT_LIMIT) {
            fail(c, MENU_SWF_ERR_BUDGET,
                 "swf display list: composed matrix overflow");
            return -1;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (tr[i] < -MATRIX_TRANSLATE_LIMIT ||
            tr[i] > MATRIX_TRANSLATE_LIMIT) {
            fail(c, MENU_SWF_ERR_BUDGET,
                 "swf display list: composed translation overflow");
            return -1;
        }
    }
    out->m00 = (int32_t)vals[0];
    out->m01 = (int32_t)vals[1];
    out->m10 = (int32_t)vals[2];
    out->m11 = (int32_t)vals[3];
    out->tx = (int32_t)tr[0];
    out->ty = (int32_t)tr[1];
    return 0;
}

static int compose_color(Ctx *c, const MenuSwfDisplayListColor *a,
                         const MenuSwfDisplayListColor *b,
                         MenuSwfDisplayListColor *out) {
    for (unsigned i = 0; i < 4; ++i) {
        int64_t mult = (int64_t)a->mult[i] * b->mult[i] / 256;
        int64_t add = (int64_t)a->mult[i] * b->add[i] / 256 + a->add[i];
        if (mult < -COLOR_LIMIT || mult > COLOR_LIMIT ||
            add < -COLOR_LIMIT || add > COLOR_LIMIT) {
            fail(c, MENU_SWF_ERR_BUDGET,
                 "swf display list: composed colour overflow");
            return -1;
        }
        out->mult[i] = (int32_t)mult;
        out->add[i] = (int32_t)add;
    }
    return 0;
}

static int append_record(Ctx *c, const LiveObject *live,
                         const MenuSwfDisplayListMatrix *matrix,
                         const MenuSwfDisplayListColor *color,
                         uint16_t parent_sprite, uint32_t sprite_depth) {
    if (c->count >= c->capacity ||
        c->count >= MENU_SWF_DISPLAY_LIST_MAX_RECORDS) {
        c->out->truncated = 1;
        fail(c, MENU_SWF_ERR_BUDGET,
             "swf display list: record capacity %u exceeded", c->capacity);
        return -1;
    }
    MenuSwfDisplayListRecord *r = &c->records[c->count++];
    memset(r, 0, sizeof *r);
    r->character_id = live->character_id;
    r->depth = live->depth;
    r->clip_depth = live->clip_depth;
    r->ratio = live->ratio;
    r->parent_sprite_id = parent_sprite;
    r->sprite_depth = (uint16_t)sprite_depth;
    r->has_character = live->has_character;
    r->has_clip_depth = live->has_clip_depth;
    r->has_name = live->has_name;
    r->is_sprite = live->is_sprite;
    memcpy(r->name, live->name, sizeof r->name);
    r->matrix = *matrix;
    r->color = *color;
    if (live->is_sprite) ++c->out->sprite_placements;
    if (sprite_depth > c->out->max_sprite_depth)
        c->out->max_sprite_depth = sprite_depth;
    return 0;
}

static MenuSwfStatus flatten_timeline(Ctx *c, const uint8_t *tags, size_t len,
                                      const MenuSwfDisplayListMatrix *parent_m,
                                      const MenuSwfDisplayListColor *parent_c,
                                      uint16_t parent_sprite,
                                      uint32_t sprite_depth,
                                      uint16_t *stack, uint32_t stack_n,
                                      int top) {
    LiveObject live[MENU_SWF_DISPLAY_LIST_MAX_LIVE];
    uint32_t live_count = 0;
    int saw_frame = 0;
    size_t off = 0;
    while (off < len) {
        if (c->status != MENU_SWF_OK) return c->status;
        unsigned code;
        const uint8_t *payload;
        uint32_t plen;
        int tr = tag_next(tags, len, &off, &code, &payload, &plen);
        if (tr < 0) {
            fail(c, MENU_SWF_ERR_TRUNCATED,
                 "swf display list: truncated tag header/body");
            return c->status;
        }
        if (tr == 0) break;
        if (code == 0) break;
        if (code == 1) { /* ShowFrame */
            saw_frame = 1;
            if (top) c->out->top_objects = live_count;
            for (uint32_t i = 0; i < live_count; ++i) {
                MenuSwfDisplayListMatrix m;
                MenuSwfDisplayListColor col;
                if (compose_matrix(c, parent_m, &live[i].matrix, &m) ||
                    compose_color(c, parent_c, &live[i].color, &col))
                    return c->status;
                if (append_record(c, &live[i], &m, &col, parent_sprite,
                                  sprite_depth))
                    return c->status;
                if (live[i].is_sprite) {
                    const SpriteDef *def = find_sprite(c, live[i].character_id);
                    if (!def) continue;
                    if (sprite_depth + 1 >= MENU_SWF_DISPLAY_LIST_MAX_DEPTH) {
                        fail(c, MENU_SWF_ERR_BUDGET,
                             "swf display list: sprite depth %u exceeded",
                             MENU_SWF_DISPLAY_LIST_MAX_DEPTH);
                        return c->status;
                    }
                    int cycle = 0;
                    for (uint32_t k = 0; k < stack_n; ++k)
                        if (stack[k] == def->id) cycle = 1;
                    if (def->id == parent_sprite) cycle = 1;
                    if (cycle) {
                        fail(c, MENU_SWF_ERR_TAG,
                             "swf display list: sprite cycle at id %u",
                             def->id);
                        return c->status;
                    }
                    stack[stack_n] = def->id;
                    MenuSwfStatus st = flatten_timeline(
                        c, def->tags, def->len, &m, &col, def->id,
                        sprite_depth + 1, stack, stack_n + 1, 0);
                    if (st != MENU_SWF_OK) return st;
                }
            }
            return MENU_SWF_OK;
        }
        if (code == 26 || code == 70) {
            MenuSwfDisplayObject obj;
            MenuSwfStatus st = menu_swf_display_parse_place(
                payload, plen, (uint8_t)code, &obj, c->reason, c->reason_cap);
            ++c->out->placement_tags;
            if (st != MENU_SWF_OK) {
                if (c->status == MENU_SWF_OK) c->status = st;
                return st;
            }
            if (apply_place(c, live, &live_count, &obj)) return c->status;
        } else if (code == 28) {
            if (plen < 2) {
                fail(c, MENU_SWF_ERR_FORMAT,
                     "swf display list: RemoveObject2 is truncated");
                return c->status;
            }
            uint16_t depth = (uint16_t)(payload[0] | (payload[1] << 8));
            live_remove(live, &live_count, depth);
            ++c->out->removals;
        } else if (code == 5) {
            if (plen < 4) {
                fail(c, MENU_SWF_ERR_FORMAT,
                     "swf display list: RemoveObject is truncated");
                return c->status;
            }
            uint16_t depth = (uint16_t)(payload[2] | (payload[3] << 8));
            live_remove(live, &live_count, depth);
            ++c->out->removals;
        }
    }
    if (top) c->out->top_objects = live_count;
    if (!saw_frame) {
        /* A sprite/timeline without ShowFrame still has a useful end state. */
        for (uint32_t i = 0; i < live_count; ++i) {
            MenuSwfDisplayListMatrix m;
            MenuSwfDisplayListColor col;
            if (compose_matrix(c, parent_m, &live[i].matrix, &m) ||
                compose_color(c, parent_c, &live[i].color, &col))
                return c->status;
            if (append_record(c, &live[i], &m, &col, parent_sprite,
                              sprite_depth))
                return c->status;
        }
    }
    return MENU_SWF_OK;
}

static int collect_sprites(Ctx *c, const uint8_t *tags, size_t len) {
    size_t off = 0;
    while (off < len) {
        unsigned code;
        const uint8_t *payload;
        uint32_t plen;
        int tr = tag_next(tags, len, &off, &code, &payload, &plen);
        if (tr <= 0) break;
        if (code == 0) break;
        if (code != 39) continue;
        if (plen < 4) {
            fail(c, MENU_SWF_ERR_FORMAT,
                 "swf display list: DefineSprite is too short");
            return -1;
        }
        uint16_t id = (uint16_t)(payload[0] | (payload[1] << 8));
        if (find_sprite(c, id)) continue;
        if (c->sprite_count >= MENU_SWF_DISPLAY_LIST_MAX_SPRITES) {
            fail(c, MENU_SWF_ERR_BUDGET,
                 "swf display list: sprite definition limit %u exceeded",
                 MENU_SWF_DISPLAY_LIST_MAX_SPRITES);
            return -1;
        }
        SpriteDef *d = &c->sprites[c->sprite_count++];
        d->id = id;
        d->tags = payload + 4;
        d->len = plen - 4;
    }
    return 0;
}

MenuSwfStatus menu_swf_display_list_build(
    const uint8_t *data, size_t size, MenuSwfDisplayListRecord *records,
    uint32_t capacity, MenuSwfDisplayListResult *out, char *reason,
    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!data || (capacity && !records)) {
        dl_reason(reason, reason_cap,
                  "swf display list: needs bytes and a record buffer");
        return MENU_SWF_ERR_ARGUMENT;
    }

    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason,
                                           reason_cap);
    if (st != MENU_SWF_OK) return st;

    Ctx c;
    memset(&c, 0, sizeof c);
    c.body = view.body + view.tag_offset;
    c.body_len = view.body_len - view.tag_offset;
    c.records = records;
    c.capacity = capacity;
    c.out = out;
    c.status = MENU_SWF_OK;
    c.reason = reason;
    c.reason_cap = reason_cap;

    if (collect_sprites(&c, c.body, c.body_len)) {
        MenuSwfStatus failed = c.status;
        menu_swf_view_close(&view);
        return failed;
    }
    out->sprite_count = c.sprite_count;

    MenuSwfDisplayListMatrix identity_m;
    MenuSwfDisplayListColor identity_c;
    matrix_identity(&identity_m);
    color_identity(&identity_c);
    uint16_t stack[MENU_SWF_DISPLAY_LIST_MAX_DEPTH];
    st = flatten_timeline(&c, c.body, c.body_len, &identity_m, &identity_c, 0,
                          0, stack, 0, 1);
    out->record_count = c.count;
    menu_swf_view_close(&view);
    if (st != MENU_SWF_OK) {
        if (reason && !reason[0])
            dl_reason(reason, reason_cap, "swf display list: build failed");
        return st;
    }
    if (c.status != MENU_SWF_OK) return c.status;
    return MENU_SWF_OK;
}
