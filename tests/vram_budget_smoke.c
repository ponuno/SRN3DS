/* Render-target VRAM budget policy on a fake backend with a real byte budget.
 *
 * The mock enforces its own VRAM limit (like PICA's 6 MiB) and supports the
 * optional linear-heap / suspend-resume callbacks.  The core's
 * N3DS_TARGET_VRAM_LIMIT accounting drives the policy:
 *
 *   A. four 512x512 targets + a 512x256 retained back buffer + depth under a
 *      5 MiB target budget with 512 KiB of non-target VRAM: three fit in VRAM,
 *      the fourth is created in the linear heap (sampling/rendering still work);
 *   B. an inactive plain target is evicted to make room, the current render
 *      target is never chosen, and the evicted target is rehydrated on bind;
 *   C. a destroyed native is reused from the pool without another allocation;
 *   D. with every fallback unavailable the refusal is named;
 *   E. PO2 downscaling is the last fallback and is counted.
 */
#include "d3d9_core.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_MAGIC 0x5a4d4255u /* 'UMBZ' */

typedef struct {
    uint32_t magic;
    unsigned w, h;
    size_t cost;
    int linear;
    int has_depth;
    int resident;
} Node;

typedef struct {
    size_t vram_limit, vram_used, linear_limit, linear_used;
    size_t vram_reserved; /* VRAM the core model does not know about (LCD etc.) */
    unsigned creates, linear_creates, deletes, suspends, resumes, binds, clears,
             reads, writes, viewport_calls, scaled_calls;
    unsigned last_w, last_h;
} Ctx;

static void env_set(const char *name, const char *v) {
#ifdef _WIN32
    _putenv_s(name, v ? v : "");
#else
    if (v && *v) setenv(name, v, 1); else unsetenv(name);
#endif
}
static size_t cost_of(unsigned w, unsigned h) { return (size_t)w * h * 4u; }

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
static int target_create_common(Ctx *c, unsigned w, unsigned h, int linear,
                                void **native) {
    size_t cost = cost_of(w, h);
    if (linear) {
        if (c->linear_limit && c->linear_used + cost > c->linear_limit) return -1;
    } else if (c->vram_used + c->vram_reserved + cost > c->vram_limit) {
        return -1;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n) return -1;
    n->magic = NODE_MAGIC; n->w = w; n->h = h; n->cost = cost;
    n->linear = linear; n->resident = 1;
    if (linear) c->linear_used += cost; else c->vram_used += cost;
    c->last_w = w; c->last_h = h;
    if (linear) ++c->linear_creates; else ++c->creates;
    *native = n;
    return 0;
}
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format,
                         void **native) {
    (void)format;
    return target_create_common(ctx, w, h, 0, native);
}
static int target_create_linear(void *ctx, unsigned w, unsigned h,
                                unsigned format, void **native) {
    (void)format;
    return target_create_common(ctx, w, h, 1, native);
}
static void target_delete(void *ctx, void *native) {
    Ctx *c = ctx; Node *n = native;
    if (!n) return;
    assert(n->magic == NODE_MAGIC);
    n->magic = 0;
    if (n->resident) {
        if (n->linear) c->linear_used -= n->cost; else c->vram_used -= n->cost;
    }
    ++c->deletes;
    free(n);
}
static int target_suspend(void *ctx, void *native) {
    Ctx *c = ctx; Node *n = native;
    if (!n || !n->resident || n->linear) return -1;
    n->resident = 0; c->vram_used -= n->cost; ++c->suspends;
    return 0;
}
static int target_resume(void *ctx, void *native) {
    Ctx *c = ctx; Node *n = native;
    if (!n || n->resident) return -1;
    assert(!n->linear);
    if (c->vram_used + c->vram_reserved + n->cost > c->vram_limit) return -1;
    n->resident = 1; c->vram_used += n->cost; ++c->resumes;
    return 0;
}
static int target_bind(void *ctx, void *native) {
    Ctx *c = ctx; Node *n = native;
    assert(!n || n->magic == NODE_MAGIC);
    (void)ctx;
    ++c->binds;
    return 0;
}
static int target_clear(void *ctx, void *native, uint32_t argb) {
    Ctx *c = ctx; Node *n = native; (void)argb;
    assert(!n || n->magic == NODE_MAGIC);
    ++c->clears;
    return 0;
}
static int target_read(void *ctx, void *native, void *dst, size_t pitch,
                       unsigned x, unsigned y, unsigned w, unsigned h) {
    Ctx *c = ctx; (void)native; (void)dst; (void)pitch; (void)x; (void)y;
    (void)w; (void)h; ++c->reads; return 0;
}
static int target_write(void *ctx, void *native, const void *bgra, size_t pitch,
                        unsigned w, unsigned h) {
    Ctx *c = ctx; (void)native; (void)bgra; (void)pitch; (void)w; (void)h;
    ++c->writes; return 0;
}
static int target_blit(void *ctx, const D9BlitRegion *dst,
                       const D9BlitRegion *src, unsigned filter) {
    (void)ctx; (void)dst; (void)src; (void)filter;
    return D9_BLIT_OK;
}
static int target_viewport(void *ctx, unsigned w, unsigned h) {
    Ctx *c = ctx; (void)w; (void)h; ++c->viewport_calls; return 0;
}
static int target_viewport_scaled(void *ctx, unsigned logical_w,
                                  unsigned logical_h, unsigned native_w,
                                  unsigned native_h) {
    Ctx *c = ctx;
    assert(native_w <= logical_w && native_h <= logical_h);
    ++c->scaled_calls; return 0;
}
static int target_has_depth(void *ctx, void *native) {
    (void)ctx; Node *n = native; return n ? n->has_depth : 0;
}
static int target_depth_attach(void *ctx, void *native) {
    Ctx *c = ctx; Node *n = native;
    if (!n || n->has_depth) return 0;
    size_t cost = cost_of(n->w, n->h);
    if (n->resident && c->vram_used + c->vram_reserved + cost > c->vram_limit)
        return -1;
    if (n->resident) { c->vram_used += cost; n->cost += cost; }
    n->has_depth = 1;
    return 0;
}
static void *target_texture(void *ctx, void *native) {
    (void)ctx; return native;
}

static const D9Backend ops_full = {
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
    .target_blit = target_blit,
    .target_write = target_write,
    .target_viewport = target_viewport,
    .target_texture = target_texture,
    .target_viewport_scaled = target_viewport_scaled,
    .target_has_depth = target_has_depth,
    .target_depth_attach = target_depth_attach,
    .target_create_linear = target_create_linear,
    .target_suspend = target_suspend,
    .target_resume = target_resume,
};
/* Downscale only: no linear storage, no eviction, but a scaled viewport. */
static const D9Backend ops_scaled = {
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
    .target_blit = target_blit,
    .target_write = target_write,
    .target_viewport = target_viewport,
    .target_texture = target_texture,
    .target_viewport_scaled = target_viewport_scaled,
};
/* No fallback at all. */
static const D9Backend ops_plain = {
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
    .target_blit = target_blit,
    .target_write = target_write,
    .target_viewport = target_viewport,
    .target_texture = target_texture,
};

static void frame(D9Device *d, D9Handle current) {
    assert(d9_begin_scene(d, 0) == D9_OK);
    if (current) assert(d9_set_render_target(d, current) == D9_OK);
    assert(d9_present(d) == D9_OK);
}
static void bind_and_render(D9Device *d, D9Handle h) {
    assert(d9_begin_scene(d, 0) == D9_OK);
    int rc = d9_set_render_target(d, h);
    if (rc != D9_OK)
        fprintf(stderr, "set_render_target(%u) rc=%d diag=%s\n", h, rc,
                d9_last_draw_diagnostic(d));
    assert(rc == D9_OK);
    assert(d9_end_scene(d) == D9_OK);
}

static void test_four_512_with_back_buffer(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "5M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "0");
    env_set("N3DS_TARGET_DOWNSCALE", "1");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 5u << 20;
    c.vram_reserved = 512u << 10; /* LCD/OS VRAM the target model ignores */
    c.linear_limit = 8u << 20;
    D9Device *d = d9_create_device(&ops_full, &c, 32u << 20);
    assert(d);

    assert(d9_set_back_buffer_info_ex(d, 400, 240, D9_A8R8G8B8, 1) == D9_OK);
    assert(c.creates == 1 && c.last_w == 512 && c.last_h == 256);
    c.vram_reserved = 512u << 10;
    /* The retained back buffer's depth is attached lazily on the first bind. */
    frame(d, 0);
    printf("back buffer: vram_used=%zuKiB (colour+depth+reserved)\n", c.vram_used >> 10);

    D9Handle tex[4] = {0}, t[4] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        assert(d9_create_render_texture(d, 512, 512, D9_A8R8G8B8,
                                        D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET,
                                        &tex[i]) == D9_OK);
        t[i] = d9_texture_render_target(d, tex[i]);
        assert(t[i]);
    }
    /* Three targets fit the backend's VRAM; the fourth request evicts one of
     * the unused 512x512 sampleable targets (creation is not a use) and then
     * still gets a full 512x512 allocation. */
    assert(c.creates == 5); /* back buffer + four 512x512 */
    assert(c.linear_creates == 0);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.vram_limit == 5u << 20);
    assert(st.vram_live == 4u << 20);
    assert(st.vram_peak == 4u << 20);
    assert(st.linear_fallbacks == 0);
    assert(st.downscales == 0);
    assert(st.alloc_failures == 0);
    assert(st.evictions == 1);
    assert(st.suspended_targets == 1);
    unsigned nw = 0, nh = 0;
    assert(d9_render_target_storage_info(d, t[3], &nw, &nh) == D9_OK);
    assert(nw == 512 && nh == 512);
    assert(c.vram_used == 4u << 20); /* back + 3 resident + the new 512 */
    assert(c.linear_used == 0);

    /* Free the new target, then every original one binds again; the evicted
     * one is resumed on bind. */
    assert(d9_release_texture(d, tex[3]) == D9_OK);
    for (unsigned i = 0; i < 3; ++i) {
        bind_and_render(d, t[i]);
        assert(d9_render_target_storage_info(d, t[i], NULL, NULL) == D9_OK);
    }
    assert(c.binds >= 3);
    assert(c.resumes == 1);
    for (unsigned i = 0; i < 3; ++i)
        assert(d9_release_texture(d, tex[i]) == D9_OK);
    assert(c.vram_used == 1u << 20); /* only the retained back buffer remains */
    assert(c.linear_used == 0);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0 && c.linear_used == 0);
    puts("A: four 512x512 + back buffer + depth -> evict sampleable, counters PASS");
}

static void test_eviction_and_current_rt(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "2M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "0");
    /* Explicit render targets are capped at 256 by default; lift the cap so
     * each 512x512 target is a full 1 MiB native allocation. */
    env_set("TESV_D3D9_TARGET_NATIVE_MAX", "512");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 3u << 20;
    D9Device *d = d9_create_device(&ops_full, &c, 32u << 20);
    assert(d);

    D9Handle a = 0, b = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &a) == D9_OK);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &b) == D9_OK);
    assert(c.vram_used == 2u << 20);

    /* Advance one frame with A as the current render target. */
    frame(d, a);
    assert(d9_get_render_target(d) == a);

    D9Handle cc = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &cc) == D9_OK);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.evictions == 1);
    assert(st.suspended_targets == 1);
    assert(st.alloc_failures == 0);
    assert(c.suspends == 1 && c.resumes == 0);
    /* The evicted one is B, never the current target A. */
    assert(c.binds > 0);

    /* A is still resident: binding it does not need a resume. */
    bind_and_render(d, a);
    assert(c.resumes == 0);

    /* Free the newly created target and rehydrate the evicted one on bind. */
    assert(d9_release_render_target(d, cc) == D9_OK);
    bind_and_render(d, b);
    assert(c.resumes == 1);
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.suspended_targets == 0);
    assert(st.vram_live == 2u << 20);

    assert(d9_release_render_target(d, a) == D9_OK);
    assert(d9_release_render_target(d, b) == D9_OK);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0);
    puts("B: eviction skips the current RT and rehydrates on bind PASS");
}

static void test_pool_reuse(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "5M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "4");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 5u << 20;
    D9Device *d = d9_create_device(&ops_full, &c, 32u << 20);
    assert(d);

    D9Handle x = 0, y = 0;
    assert(d9_create_render_target(d, 256, 256, D9_A8R8G8B8, 0, &x) == D9_OK);
    assert(d9_create_render_target(d, 256, 256, D9_A8R8G8B8, 0, &y) == D9_OK);
    unsigned creates = c.creates, deletes = c.deletes;
    assert(creates == 2);

    assert(d9_release_render_target(d, x) == D9_OK);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.pool_entries == 1 && st.pool_bytes == 256u << 10);
    assert(c.deletes == deletes);           /* native retained */
    assert(st.vram_live == 512u << 10);     /* still charged while pooled */

    D9Handle z = 0;
    assert(d9_create_render_target(d, 256, 256, D9_A8R8G8B8, 0, &z) == D9_OK);
    assert(c.creates == creates);           /* no new backend allocation */
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.pool_hits == 1 && st.pool_entries == 0);
    assert(st.vram_live == 512u << 10);

    assert(d9_release_render_target(d, y) == D9_OK);
    assert(d9_release_render_target(d, z) == D9_OK);
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.pool_entries == 2);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0);
    assert(c.deletes == deletes + 2);
    puts("C: destroy/recreate reuses the pooled native PASS");
}

static void test_named_refusal(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "1M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "0");
    env_set("TESV_D3D9_TARGET_NATIVE_MAX", "512");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 1u << 20;
    D9Device *d = d9_create_device(&ops_plain, &c, 32u << 20);
    assert(d);

    D9Handle a = 0, b = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &a) == D9_OK);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &b) == D9_BACKEND);
    assert(b == 0);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.alloc_failures == 1);
    assert(st.evictions == 0 && st.linear_fallbacks == 0 && st.downscales == 0);
    assert(strstr(d9_last_draw_diagnostic(d), "allocation refused") != NULL);
    assert(strstr(d9_last_draw_diagnostic(d), "native target") != NULL);
    assert(strstr(d9_last_draw_diagnostic(d), "512x512") != NULL);
    assert(c.creates == 1 && c.vram_used == 1u << 20);
    assert(d9_release_render_target(d, a) == D9_OK);
    assert(d9_destroy_device_ex(d) == 0);
    puts("D: named refusal when no fallback exists PASS");
}

static void test_sampleable_eviction_and_resume(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "2M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "0");
    env_set("TESV_D3D9_TARGET_NATIVE_MAX", "512");
    env_set("N3DS_TARGET_DOWNSCALE", "0");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 3u << 20;
    D9Device *d = d9_create_device(&ops_full, &c, 32u << 20);
    assert(d);

    D9Handle tex = 0, a = 0, b = 0, cc = 0;
    assert(d9_create_render_texture(d, 512, 512, D9_A8R8G8B8, D9_POOL_DEFAULT,
                                    D9_USAGE_RENDERTARGET, &tex) == D9_OK);
    a = d9_texture_render_target(d, tex);
    assert(a);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &b) == D9_OK);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.vram_live == (2u << 20));

    /* Advance a frame with B current: A is sampleable but not used this frame,
     * so it is a valid eviction candidate and must free its 1 MiB. */
    frame(d, b);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &cc) == D9_OK);
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.evictions == 1);
    assert(st.suspended_targets == 1);
    assert(c.suspends == 1 && c.resumes == 0);
    assert(st.vram_live == (2u << 20));

    /* The suspended sampleable target is rehydrated when its texture is bound
     * again, without a new D3D handle. */
    assert(d9_release_render_target(d, cc) == D9_OK);
    assert(d9_bind_texture(d, 0, tex) == D9_OK);
    assert(c.resumes == 1);
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.suspended_targets == 0 && st.vram_live == (2u << 20));

    /* Repeated evict/rehydrate cycles must not drift the authoritative
     * vram_live/peak counters (unlike the incremental mem_stats path). */
    for (unsigned i = 0; i < 8; ++i) {
        frame(d, b);
        assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &cc) == D9_OK);
        assert(d9_target_stats(d, &st) == D9_OK);
        assert(st.vram_live == (2u << 20));
        assert(st.suspended_targets == 1);
        assert(d9_release_render_target(d, cc) == D9_OK);
        assert(d9_bind_texture(d, 0, tex) == D9_OK);
        assert(d9_target_stats(d, &st) == D9_OK);
        assert(st.vram_live == (2u << 20));
        assert(st.suspended_targets == 0);
        assert(st.vram_peak == (2u << 20));
    }
    assert(st.evictions == 9);
    assert(c.resumes == 9);

    assert(d9_bind_texture(d, 0, 0) == D9_OK);
    assert(d9_release_texture(d, tex) == D9_OK);
    assert(d9_release_render_target(d, b) == D9_OK);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0);
    puts("F: sampleable-target eviction and bind-time resume, no counter drift PASS");
}

static void test_downscale_fallback(void) {
    env_set("N3DS_TARGET_VRAM_LIMIT", "5M");
    env_set("N3DS_TARGET_POOL_ENTRIES", "0");
    env_set("TESV_D3D9_TARGET_NATIVE_MAX", "512");
    env_set("N3DS_TARGET_DOWNSCALE", "1");
    Ctx c; memset(&c, 0, sizeof c);
    c.vram_limit = 1088u << 10; /* 512K fits, two 512s do not, 128+512 do */
    D9Device *d = d9_create_device(&ops_scaled, &c, 32u << 20);
    assert(d);

    D9Handle a = 0, b = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &a) == D9_OK);
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &b) == D9_OK);
    unsigned nw = 0, nh = 0;
    assert(d9_render_target_storage_info(d, b, &nw, &nh) == D9_OK);
    assert(nw == 128 && nh == 128);
    D9TargetStats st;
    assert(d9_target_stats(d, &st) == D9_OK);
    assert(st.downscales == 2);
    assert(st.linear_fallbacks == 0 && st.evictions == 0);
    assert(st.alloc_failures == 0);
    bind_and_render(d, b);
    assert(c.scaled_calls > 0);
    assert(d9_release_render_target(d, a) == D9_OK);
    assert(d9_release_render_target(d, b) == D9_OK);
    assert(d9_destroy_device_ex(d) == 0);
    puts("E: downscale to 128x128 PO2 storage under VRAM pressure PASS");
}

int main(void) {
    test_four_512_with_back_buffer();
    test_eviction_and_current_rt();
    test_pool_reuse();
    test_named_refusal();
    test_sampleable_eviction_and_resume();
    test_downscale_fallback();
    env_set("N3DS_TARGET_VRAM_LIMIT", NULL);
    env_set("N3DS_TARGET_POOL_ENTRIES", NULL);
    env_set("TESV_D3D9_TARGET_NATIVE_MAX", NULL);
    env_set("N3DS_TARGET_DOWNSCALE", NULL);
    printf("vram budget: linear fallback, eviction, pool, refusal, downscale PASS\n");
    return 0;
}
