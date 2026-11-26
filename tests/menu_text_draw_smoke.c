/* Host test: menu A8 atlas/batch through the D3D9 COM demo. */

#include "d3d9_menu_text.h"
#include "menu_swf_batch.h"
#include "menu_swf_atlas.h"
#include "menu_swf_text.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned w, h, levels;
    uint8_t *data;
    size_t bytes;
} MockTex;

typedef struct {
    unsigned created, uploaded, deleted, begin, draw, colored, end;
    int active, fail_create;
    size_t count;
    void *texture;
    D9DrawState state;
    D9ColorVertex vertices[64];
    MockTex *last_tex;
} Mock;

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels,
                      void **native) {
    Mock *m = ctx;
    if (m->fail_create) return -1;
    MockTex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->bytes = (size_t)w * h * 4;
    t->data = calloc(1, t->bytes);
    if (!t->data) {
        free(t);
        return -1;
    }
    *native = t;
    m->last_tex = t;
    ++m->created;
    return 0;
}

static int tex_upload(void *ctx, void *native, unsigned level,
                      const void *data, size_t bytes) {
    Mock *m = ctx;
    MockTex *t = native;
    if (!t || level >= t->levels || !data || !bytes) return -1;
    size_t pixels = (size_t)(t->w >> level) * (t->h >> level);
    if (bytes != pixels && bytes != pixels * 4) return -1;
    if (level == 0) {
        size_t n = bytes < t->bytes ? bytes : t->bytes;
        memcpy(t->data, data, n);
    }
    ++m->uploaded;
    return 0;
}

static void tex_delete(void *ctx, void *native) {
    Mock *m = ctx;
    MockTex *t = native;
    if (!t) return;
    if (m->last_tex == t) m->last_tex = NULL;
    free(t->data);
    free(t);
    ++m->deleted;
}

static int begin(void *ctx, uint32_t color) {
    Mock *m = ctx;
    (void)color;
    if (m->active) return -1;
    m->active = 1;
    ++m->begin;
    return 0;
}

static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *mat,
                void *texture, const D9DrawState *state) {
    Mock *m = ctx;
    (void)v;
    if (!m->active || !n) return -1;
    m->count = n;
    m->texture = texture;
    if (state) m->state = *state;
    (void)mat;
    ++m->draw;
    return 0;
}

static int draw_colored(void *ctx, const D9ColorVertex *v, size_t n,
                        const D9Matrix *mat, void *texture,
                        const D9DrawState *state) {
    Mock *m = ctx;
    if (!m->active || !n || n > 64) return -1;
    m->count = n;
    m->texture = texture;
    if (state) m->state = *state;
    memcpy(m->vertices, v, n * sizeof *v);
    (void)mat;
    ++m->colored;
    return 0;
}

static void end(void *ctx) {
    Mock *m = ctx;
    assert(m->active);
    m->active = 0;
    ++m->end;
}

static const D9Backend ops = {
    .texture_create = tex_create,
    .texture_upload = tex_upload,
    .texture_delete = tex_delete,
    .begin = begin,
    .draw = draw,
    .end = end,
    .draw_colored = draw_colored,
};

static uint32_t method(D9ComDemo *s, uint32_t obj, unsigned slot,
                       const uint32_t *args, unsigned count) {
    uint32_t r = 0;
    assert(!d9_com_demo_call(s, obj, slot, args, count, &r));
    return r;
}

static uint8_t atlas_pixels[64];

static void build_batch(MenuSwfBatchVertex *vertices, uint32_t *indices,
                        MenuSwfAtlasEntry entries[2]) {
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            atlas_pixels[y * 8 + x] = (uint8_t)(x * 17 + y * 31 + 5);

    MenuSwfTextMesh text;
    MenuSwfTextGlyph glyphs[2];
    memset(&text, 0, sizeof text);
    memset(glyphs, 0, sizeof glyphs);
    glyphs[0].code = 'A';
    glyphs[0].glyph_index = 0;
    glyphs[0].has_geometry = 1;
    glyphs[0].bounds.xmin = 0;
    glyphs[0].bounds.xmax = 4;
    glyphs[0].bounds.ymin = 0;
    glyphs[0].bounds.ymax = 4;
    glyphs[1].code = 'B';
    glyphs[1].glyph_index = 1;
    glyphs[1].has_geometry = 1;
    glyphs[1].bounds.xmin = 4;
    glyphs[1].bounds.xmax = 8;
    glyphs[1].bounds.ymin = 0;
    glyphs[1].bounds.ymax = 4;
    text.glyphs = glyphs;
    text.glyph_count = 2;
    memset(entries, 0, 2 * sizeof *entries);
    for (unsigned i = 0; i < 2; ++i) {
        entries[i].glyph_index = (uint16_t)i;
        entries[i].has_rect = 1;
        entries[i].x = i * 4;
        entries[i].w = 4;
        entries[i].h = 4;
        entries[i].u = (uint32_t)((uint64_t)entries[i].x << 16) / 8;
        entries[i].v = 0;
        entries[i].uw = (uint32_t)((uint64_t)entries[i].w << 16) / 8;
        entries[i].vh = (uint32_t)((uint64_t)entries[i].h << 16) / 8;
    }
    MenuSwfBatchStyle style;
    memset(&style, 0, sizeof style);
    style.m00 = 1.0;
    style.m11 = 1.0;
    style.tx = 10.0;
    style.ty = 20.0;
    style.color = 0xFFAABBCCu;
    MenuSwfBatchResult br;
    char reason[BSA_REASON_MAX];
    MenuSwfBatchStatus bs = menu_swf_batch_text(
        &text, entries, 2, &style, NULL, vertices, 8, indices, 12, &br,
        reason, sizeof reason);
    assert(bs == MENU_SWF_BATCH_OK && br.quad_count == 2 &&
           br.vertex_count == 8 && br.index_count == 12);
}

int main(void) {
    Mock mock = {0};
    D9Device *dev = d9_create_device(&ops, &mock, 4u << 20);
    assert(dev);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, dev)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }
    unsigned created_before = mock.created;
    unsigned uploaded_before = mock.uploaded;

    MenuSwfAtlasTarget atlas;
    atlas.width = 8;
    atlas.height = 8;
    atlas.stride = 8;
    atlas.pixels = atlas_pixels;
    MenuSwfAtlasEntry entries[2];
    MenuSwfBatchVertex vertices[8];
    uint32_t indices[12];
    build_batch(vertices, indices, entries);

    MenuTextDraw text;
    MenuTextDrawStatus st = menu_text_draw_init(
        &text, &demo, &atlas, 0x10100000u, 0x20000u);
    if (st != MENU_TEXT_DRAW_OK) {
        fprintf(stderr, "menu text init failed: %s (%s)\n",
                menu_text_draw_status_name(st), text.reason);
        return 1;
    }
    assert(mock.created == created_before + 1);
    assert(mock.uploaded == uploaded_before + 1);

    uint32_t desc_args[2] = {0, 0x10010400};
    assert(!method(&demo, text.texture, 17, desc_args, 2));
    uint32_t fmt = 0, width = 0, height = 0;
    assert(!guest_read32(&demo.memory, 0x10010400, &fmt));
    assert(!guest_read32(&demo.memory, 0x10010418, &width));
    assert(!guest_read32(&demo.memory, 0x1001041c, &height));
    assert(fmt == D9_A8 && width == 8 && height == 8);

    uint32_t lock_args[4] = {0, 0x10010400, 0, 0};
    assert(!method(&demo, text.texture, 19, lock_args, 4));
    uint32_t pitch = 0, pixels = 0;
    assert(!guest_read32(&demo.memory, 0x10010400, &pitch));
    assert(!guest_read32(&demo.memory, 0x10010404, &pixels));
    assert(pitch == 8);
    uint8_t *locked = (uint8_t *)guest_ptr(&demo.memory, pixels, 64);
    assert(locked && memcmp(locked, atlas_pixels, 64) == 0);
    uint32_t level = 0;
    assert(!method(&demo, text.texture, 20, &level, 1));

    assert(method(&demo, demo.device, 41, NULL, 0) == 0); /* BeginScene */
    unsigned draws = mock.colored;
    st = menu_text_draw_batch(&text, vertices, 8, indices, 12);
    assert(st == MENU_TEXT_DRAW_OK);
    assert(mock.colored == draws + 1);
    assert(mock.count == 12);
    assert(mock.texture != NULL);
    assert(mock.state.address_u == 3 && mock.state.address_v == 3);
    assert(mock.state.min_filter == 1 && mock.state.mag_filter == 1 &&
           mock.state.mip_filter == 0);
    assert(mock.state.stage_colorop == 4 && mock.state.stage_colorarg1 == 2 &&
           mock.state.stage_colorarg2 == 1);
    assert(mock.state.stage_alphaop == 4 && mock.state.stage_alphaarg1 == 2 &&
           mock.state.stage_alphaarg2 == 1);

    uint32_t fvf_out = 0;
    uint32_t fvf_args[1] = {0x10010400};
    assert(!method(&demo, demo.device, 90, fvf_args, 1));
    assert(!guest_read32(&demo.memory, 0x10010400, &fvf_out));
    assert(fvf_out == 0x142);

    const MenuSwfBatchVertex *b0 = &vertices[indices[0]];
    assert(mock.vertices[0].x == (float)b0->x);
    assert(mock.vertices[0].y == (float)b0->y);
    assert(mock.vertices[0].z == 0.0f);
    assert(mock.vertices[0].u == (float)b0->u / 65536.0f);
    assert(mock.vertices[0].v == (float)b0->v / 65536.0f);
    assert(mock.vertices[0].a > 0.0f);

    assert(method(&demo, demo.device, 42, NULL, 0) == 0); /* EndScene */
    uint32_t present[4] = {0, 0, 0, 0};
    assert(method(&demo, demo.device, 17, present, 4) == 0); /* Present */

    st = menu_text_draw_batch(&text, vertices, 8, indices, 0);
    assert(st == MENU_TEXT_DRAW_ERR_EMPTY);
    st = menu_text_draw_batch(&text, vertices, 8, indices, 6000);
    assert(st == MENU_TEXT_DRAW_ERR_OVERFLOW);

    assert(menu_text_draw_destroy(&text) == MENU_TEXT_DRAW_OK);
    assert(mock.deleted >= 1);

    /* A device/backend that refuses texture creation must be a named A8
     * refusal, not an A8R8G8B8 substitution. */
    unsigned created2 = mock.created;
    mock.fail_create = 1;
    MenuTextDraw refused;
    st = menu_text_draw_init(&refused, &demo, &atlas, 0x10100000u, 0x20000u);
    assert(st == MENU_TEXT_DRAW_ERR_A8);
    assert(mock.created == created2);
    mock.fail_create = 0;

    assert(!d9_com_demo_destroy(&demo));
    d9_destroy_device(dev);
    puts("menu text D3D9 COM draw: A8 texture/upload/sampler/TSS/FVF/UP passed");
    return 0;
}
