/* PICA lazy depth attachment on explicit render targets.
 *
 * The N3DS has 6 MiB of VRAM, and the engine's measured shadow/reflection set
 * would need colour + DEPTH24_STENCIL8 for every explicit target.  PICA now
 * creates explicit colour targets without depth and attaches DEPTH24_STENCIL8
 * only when a matching D3D depth-stencil surface is actually bound; the
 * retained back buffer still gets depth eagerly.  This test models that VRAM
 * budget (colour-only creation, attach charges the depth bytes, allocation
 * failure is a named D9_MEMORY refusal and keeps the old binding) without
 * touching hardware.
 */
#include "d3d9_core.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_MAGIC 0x4C5A4454u /* "LZDT" */

typedef struct {
    uint32_t magic;
    unsigned w, h;
    size_t color_cost, depth_cost;
} MockTarget;

typedef struct {
    size_t vram_used, vram_limit;
    unsigned creates, deletes, attaches, binds, clears, depth_clears;
    unsigned create_w, create_h;
    unsigned last_clear_flags;
} Ctx;

static int begin(void *ctx, uint32_t argb) { (void)ctx; (void)argb; return 0; }
static void end(void *ctx) { (void)ctx; }
static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels,
                      void **native) {
    (void)ctx; (void)w; (void)h; (void)levels;
    *native = (void *)(uintptr_t)1;
    return 0;
}
static int tex_upload(void *ctx, void *native, unsigned level, const void *data,
                      size_t bytes) {
    (void)ctx; (void)native; (void)level; (void)data; (void)bytes;
    return 0;
}
static void tex_delete(void *ctx, void *native) { (void)ctx; (void)native; }
static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *clip,
                void *texture, const D9DrawState *state) {
    (void)ctx; (void)v; (void)n; (void)clip; (void)texture; (void)state;
    return 0;
}

/* Colour-only allocation, like PICA target_create after the fix. */
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format,
                         void **native) {
    (void)format;
    Ctx *c = ctx;
    size_t cost = (size_t)w * h * 4u;
    if (c->vram_limit && c->vram_used + cost > c->vram_limit) return -1;
    MockTarget *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->magic = TARGET_MAGIC;
    t->w = w;
    t->h = h;
    t->color_cost = cost;
    c->vram_used += cost;
    c->create_w = w;
    c->create_h = h;
    ++c->creates;
    *native = t;
    return 0;
}
static void target_delete(void *ctx, void *native) {
    Ctx *c = ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    t->magic = 0;
    c->vram_used -= t->color_cost + t->depth_cost;
    free(t);
    ++c->deletes;
}
static int target_bind(void *ctx, void *native) {
    Ctx *c = ctx;
    MockTarget *t = native;
    assert(!t || t->magic == TARGET_MAGIC);
    ++c->binds;
    return 0;
}
static int target_clear(void *ctx, void *native, uint32_t argb) {
    (void)argb;
    Ctx *c = ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    ++c->clears;
    return 0;
}
static int target_read(void *ctx, void *native, void *dst, size_t pitch,
                       unsigned x, unsigned y, unsigned w, unsigned h) {
    (void)ctx; (void)native; (void)dst; (void)pitch; (void)x; (void)y;
    (void)w; (void)h;
    return 0;
}
static int target_write(void *ctx, void *native, const void *bgra, size_t pitch,
                        unsigned w, unsigned h) {
    (void)ctx; (void)native; (void)bgra; (void)pitch; (void)w; (void)h;
    return 0;
}
static int target_blit(void *ctx, const D9BlitRegion *dst,
                       const D9BlitRegion *src, unsigned filter) {
    (void)ctx; (void)dst; (void)src; (void)filter;
    return D9_BLIT_OK;
}
static int target_viewport(void *ctx, unsigned w, unsigned h) {
    (void)ctx; (void)w; (void)h;
    return 0;
}
static int target_viewport_scaled(void *ctx, unsigned logical_w,
                                  unsigned logical_h, unsigned native_w,
                                  unsigned native_h) {
    (void)ctx;
    assert(native_w <= logical_w && native_h <= logical_h);
    return 0;
}
static int target_has_depth(void *ctx, void *native) {
    (void)ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    return t->depth_cost != 0;
}
/* Lazy DEPTH24_STENCIL8 allocation: one depth buffer is w*h*4 bytes. */
static int target_depth_attach(void *ctx, void *native) {
    Ctx *c = ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    if (t->depth_cost) return 0;
    size_t cost = (size_t)t->w * t->h * 4u;
    if (c->vram_limit && c->vram_used + cost > c->vram_limit) return -1;
    c->vram_used += cost;
    t->depth_cost = cost;
    ++c->attaches;
    return 0;
}
static int depth_clear(void *ctx, unsigned flags, uint32_t value) {
    (void)value;
    Ctx *c = ctx;
    c->last_clear_flags = flags;
    ++c->depth_clears;
    return 0;
}

static const D9Backend ops = {
    .texture_create = tex_create,
    .texture_upload = tex_upload,
    .texture_delete = tex_delete,
    .begin = begin,
    .draw = draw,
    .end = end,
    .target_create = target_create,
    .target_delete = target_delete,
    .target_bind = target_bind,
    .target_clear = target_clear,
    .target_read = target_read,
    .target_write = target_write,
    .target_blit = target_blit,
    .target_viewport = target_viewport,
    .target_viewport_scaled = target_viewport_scaled,
    .depth_clear = depth_clear,
    .target_has_depth = target_has_depth,
    .target_depth_attach = target_depth_attach,
};

static void expect_diag(D9Device *d, const char *needle) {
    const char *why = d9_last_draw_diagnostic(d);
    if (!why || !strstr(why, needle)) {
        fprintf(stderr, "expected diagnostic containing '%s', got '%s'\n",
                needle, why ? why : "(null)");
        assert(0);
    }
}

int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.vram_limit = 1536u * 1024u; /* 1.5 MiB: back buffer + two 256 targets */
    D9Device *d = d9_create_device(&ops, &c, 32u << 20);
    assert(d);

    /* The retained 512x256 back buffer is the implicit depth target: it gets
     * its attachment eagerly (colour 512 KiB + depth 512 KiB = 1 MiB). */
    assert(d9_set_back_buffer_info_ex(d, 400, 240, D9_A8R8G8B8, 1) == D9_OK);
    assert(c.creates == 1 && c.attaches == 1);
    assert(c.vram_used == (size_t)512 * 256 * 4u * 2u);
    D9Handle back = d9_back_buffer_target(d);
    assert(back);

    /* Two 256x256 explicit targets fit colour-only (2 * 256 KiB = 512 KiB);
     * with the old always-embedded policy the second would have needed
     * another 512 KiB and failed. */
    D9Handle t1 = 0, t2 = 0, t3 = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &t1) == D9_OK);
    assert(c.create_w == 256 && c.create_h == 256);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &t2) == D9_OK);
    assert(c.creates == 3 && c.attaches == 1); /* no depth yet */
    assert(c.vram_used == (size_t)512 * 256 * 4u * 2u +
                          (size_t)2 * 256 * 256 * 4u);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &t3)
           == D9_BACKEND);
    assert(t3 == 0);
    expect_diag(d, "256x256 native target");

    /* Attaching depth to t1 would need 256 KiB but VRAM is full: a named
     * memory refusal, and the depth binding is unchanged.  The descriptor is
     * the logical 512x512 surface; PICA's native storage is 256x256. */
    D9Handle depth512 = 0;
    assert(d9_create_depth_surface(d, 512, 512, D9_D24S8, 0, 0, 0,
                                   &depth512) == D9_OK);
    assert(d9_set_render_target(d, t1) == D9_OK);
    assert(d9_set_depth_surface(d, depth512) == D9_MEMORY);
    expect_diag(d, "depth-stencil attachment allocation failed");
    assert(d9_get_depth_surface(d) == 0);
    assert(c.attaches == 1);

    /* Dropping t2 makes room; the same bind now attaches once and succeeds. */
    assert(d9_release_render_target(d, t2) == D9_OK);
    assert(d9_set_depth_surface(d, depth512) == D9_OK);
    assert(c.attaches == 2);
    assert(d9_get_depth_surface(d) == depth512);
    assert(c.vram_used == (size_t)512 * 256 * 4u * 2u +
                          (size_t)2 * 256 * 256 * 4u);

    /* Re-binding does not allocate a second depth attachment. */
    assert(d9_set_depth_surface(d, 0) == D9_OK);
    assert(d9_set_depth_surface(d, depth512) == D9_OK);
    assert(c.attaches == 2);

    /* Clear(Z) reaches the attachment of the active target. */
    assert(d9_begin_scene(d, 0) == D9_OK);
    assert(d9_clear_depth(d, 2, 0.5f, 0) == D9_OK);
    assert(c.depth_clears == 1 && c.last_clear_flags == 2);
    assert(d9_end_scene(d) == D9_OK);

    /* A size mismatch is queued for the target that owns it; the active target
     * keeps its embedded attachment and Get returns the request. */
    D9Handle depth128 = 0;
    assert(d9_create_depth_surface(d, 128, 128, D9_D24S8, 0, 0, 0,
                                   &depth128) == D9_OK);
    assert(d9_set_depth_surface(d, depth128) == D9_OK);
    assert(strstr(d9_last_draw_diagnostic(d), "queued") != NULL);
    assert(d9_get_depth_surface(d) == depth128);
    assert(d9_set_depth_surface(d, 0) == D9_OK);
    assert(d9_release_depth_surface(d, depth128) == D9_OK);

    /* Releasing a target frees both its colour and attached depth bytes. */
    assert(d9_release_render_target(d, t1) == D9_OK);
    /* t1 is current: release retires it, so its storage lives until destroy. */
    assert(c.vram_used == (size_t)512 * 256 * 4u * 3u);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0);

    puts("PICA lazy depth: colour-only explicit targets, one on-demand "
         "DEPTH24_STENCIL8 attach, named VRAM refusal, no re-attach, "
         "Clear(Z) and teardown PASS");
    return 0;
}
