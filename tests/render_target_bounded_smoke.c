/* Bounded native render-target storage.
 *
 * A 1024x1024 LDR render target keeps its D3D logical size but is allocated as
 * at most 512x512 native colour storage.  The backend must map the full logical
 * surface through a native viewport (target_viewport_scaled) so the image is
 * scaled, not clipped.  The null backend and a magic-tag mock pin the contract;
 * a backend without the scaled callback must refuse by name.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_MAGIC 0x54475242u

static void set_cap_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TARGET_NATIVE_MAX", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_TARGET_NATIVE_MAX", v, 1);
    else unsetenv("TESV_D3D9_TARGET_NATIVE_MAX");
#endif
}
static void set_scaled_readback_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_SCALED_READBACK", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_SCALED_READBACK", v, 1);
    else unsetenv("TESV_D3D9_SCALED_READBACK");
#endif
}

typedef struct { uint32_t magic; unsigned w, h; size_t cost; } MockTarget;
typedef struct {
    unsigned creates, deletes, binds, clears, draws;
    unsigned create_w, create_h;
    unsigned viewport_calls, viewport_w, viewport_h;
    unsigned scaled_calls, logical_w, logical_h, native_w, native_h;
    size_t vram_used, vram_limit;
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
    (void)v; (void)n; (void)clip; (void)texture; (void)state;
    Ctx *c = ctx;
    ++c->draws;
    return 0;
}
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format,
                         void **native) {
    (void)format;
    Ctx *c = ctx;
    size_t cost = (size_t)w * h * 4u * 2u; /* colour + DEPTH24_STENCIL8 estimate */
    if (c->vram_limit && c->vram_used + cost > c->vram_limit) return -1;
    MockTarget *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->magic = TARGET_MAGIC;
    t->w = w;
    t->h = h;
    t->cost = cost;
    c->create_w = w;
    c->create_h = h;
    c->vram_used += cost;
    ++c->creates;
    *native = t;
    return 0;
}
static void target_delete(void *ctx, void *native) {
    Ctx *c = ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    t->magic = 0;
    c->vram_used -= t->cost;
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
    assert(!t || t->magic == TARGET_MAGIC);
    ++c->clears;
    return 0;
}
static int target_viewport(void *ctx, unsigned w, unsigned h) {
    Ctx *c = ctx;
    c->viewport_w = w;
    c->viewport_h = h;
    ++c->viewport_calls;
    return 0;
}
static int target_viewport_scaled(void *ctx, unsigned logical_w,
                                  unsigned logical_h, unsigned native_w,
                                  unsigned native_h) {
    Ctx *c = ctx;
    assert(native_w <= logical_w && native_h <= logical_h);
    c->logical_w = logical_w;
    c->logical_h = logical_h;
    c->native_w = native_w;
    c->native_h = native_h;
    ++c->scaled_calls;
    return 0;
}

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
    .target_viewport = target_viewport,
    .target_viewport_scaled = target_viewport_scaled,
};
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
    .target_viewport = target_viewport,
};

static void check_target(D9Device *d, unsigned logical_w, unsigned logical_h,
                         unsigned native_w, unsigned native_h) {
    D9Handle h = 0;
    assert(d9_create_render_target(d, logical_w, logical_h, D9_A8R8G8B8, 0,
                                   &h) == D9_OK);
    unsigned w = 0, height = 0, format = 0, lockable = 1, sw = 0, sh = 0;
    size_t bytes = 0;
    assert(d9_render_target_info(d, h, &w, &height, &format, &lockable,
                                 &bytes) == D9_OK);
    assert(w == logical_w && height == logical_h && format == D9_A8R8G8B8);
    assert(!lockable);
    assert(bytes == (size_t)native_w * native_h * 4);
    assert(d9_render_target_storage_info(d, h, &sw, &sh) == D9_OK);
    assert(sw == native_w && sh == native_h);
    assert(d9_release_render_target(d, h) == D9_OK);
}

int main(void) {
    set_cap_env("512"); /* this regression pins the 512-cap path */
    set_scaled_readback_env(NULL);
    Ctx c;
    memset(&c, 0, sizeof c);
    D9Device *d = d9_create_device(&ops_scaled, &c, 8u << 20);
    assert(d);

    /* 1024 logical -> 512 native; desc and allocation stay consistent. */
    D9Handle t1024 = 0;
    assert(d9_create_render_target(d, 1024, 1024, D9_X8R8G8B8, 0, &t1024) == D9_OK);
    assert(c.create_w == 512 && c.create_h == 512);

    unsigned w = 0, h = 0, format = 0, lockable = 1, sw = 0, sh = 0;
    size_t bytes = 0;
    assert(d9_render_target_info(d, t1024, &w, &h, &format, &lockable,
                                 &bytes) == D9_OK);
    assert(w == 1024 && h == 1024 && format == D9_X8R8G8B8);
    assert(bytes == 512u * 512u * 4u);
    assert(d9_render_target_storage_info(d, t1024, &sw, &sh) == D9_OK);
    assert(sw == 512 && sh == 512);
    assert(d9_memory_used(d) == 512u * 512u * 4u);

    assert(d9_set_render_target(d, t1024) == D9_OK);
    assert(d9_begin_scene(d, 0xff102030u) == D9_OK);
    assert(c.scaled_calls == 1);
    assert(c.logical_w == 1024 && c.logical_h == 1024);
    assert(c.native_w == 512 && c.native_h == 512);
    assert(c.viewport_calls == 0); /* the scaled path replaces the 1:1 viewport */
    assert(c.binds == 1 && c.clears == 1);

    /* A draw submitted to the bounded target is accepted; the native viewport
     * maps the 1024x1024 D3D surface into 512x512 storage instead of clipping. */
    assert(d9_set_viewport_size(d, 1024, 1024) == D9_OK);
    D9Vertex vertices[3] = {{0,0,0,0,0},{1024,0,0,1,0},{0,1024,0,0,1}};
    assert(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, vertices,
                                sizeof vertices[0]) == D9_OK);
    assert(c.draws == 1);
    assert(d9_end_scene(d) == D9_OK);

    /* Scaled LockRect is a named refusal, not a 1024-pixel read from 512. */
    uint8_t row[1024 * 4];
    assert(d9_render_target_read(d, t1024, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "bounded native") != NULL);

    /* 512x512 stays native 512x512 and uses the legacy viewport callback. */
    check_target(d, 512, 512, 512, 512);
    unsigned before_legacy = c.viewport_calls;
    D9Handle t512 = 0;
    assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0, &t512) == D9_OK);
    assert(d9_begin_scene(d, 0) == D9_OK); /* back buffer, not t512 */
    assert(d9_end_scene(d) == D9_OK);
    assert(c.viewport_calls == before_legacy);
    assert(d9_set_render_target(d, t512) == D9_OK);
    assert(d9_begin_scene(d, 0) == D9_OK);
    assert(c.viewport_calls == before_legacy + 1);
    assert(c.viewport_w == 512 && c.viewport_h == 512);
    assert(d9_end_scene(d) == D9_OK);
    assert(d9_release_render_target(d, t512) == D9_OK);

    /* 256 is unchanged. */
    check_target(d, 256, 256, 256, 256);
    check_target(d, 1024, 512, 512, 512);
    check_target(d, 512, 1024, 512, 512);

    assert(d9_release_render_target(d, t1024) == D9_OK);
    /* The last target is current ("retired"); the device delete happens when
     * a later SetRenderTarget replaces it. */
    assert(c.deletes == 5);
    assert(d9_destroy_device_ex(d) == 0);

    /* No scaled callback: a >512 target is refused by name; 512 still works. */
    Ctx plain;
    memset(&plain, 0, sizeof plain);
    D9Device *dp = d9_create_device(&ops_plain, &plain, 8u << 20);
    assert(dp);
    D9Handle refused = 0;
    assert(d9_create_render_target(dp, 1024, 1024, D9_A8R8G8B8, 0, &refused)
           == D9_UNSUPPORTED);
    assert(refused == 0 && plain.creates == 0);
    assert(strstr(d9_last_draw_diagnostic(dp), "target_viewport_scaled") != NULL);
    assert(d9_create_render_target(dp, 512, 512, D9_A8R8G8B8, 0, &refused)
           == D9_OK);
    assert(plain.creates == 1 && plain.create_w == 512 && plain.create_h == 512);
    assert(d9_release_render_target(dp, refused) == D9_OK);
    assert(d9_destroy_device_ex(dp) == 0);

    /* Structural VRAM estimate: one 1024 logical target is 512x512 colour +
     * 512x512 depth = 2 MiB, so a single target fits 6 MiB.  Three fit; the
     * fourth fails in target_create and is a named D9_BACKEND. */
    Ctx vram;
    memset(&vram, 0, sizeof vram);
    vram.vram_limit = 6u << 20;
    D9Device *dv = d9_create_device(&ops_scaled, &vram, 16u << 20);
    assert(dv);
    D9Handle vram_targets[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        assert(d9_create_render_target(dv, 1024, 1024, D9_A8R8G8B8, 0,
                                       &vram_targets[i]) == D9_OK);
        assert(vram.create_w == 512 && vram.create_h == 512);
        assert(vram.vram_used == (size_t)(i + 1) * 2u * 1024u * 1024u);
    }
    D9Handle nofit = 0;
    assert(d9_create_render_target(dv, 1024, 1024, D9_A8R8G8B8, 0, &nofit)
           == D9_BACKEND);
    assert(nofit == 0);
    assert(strstr(d9_last_draw_diagnostic(dv), "native target") != NULL);
    for (unsigned i = 0; i < 3; ++i)
        assert(d9_release_render_target(dv, vram_targets[i]) == D9_OK);
    assert(vram.vram_used == 0);
    assert(d9_destroy_device_ex(dv) == 0);

    /* Null integration: a 1024x1024 render-target texture is 512x512 native,
     * reports the logical desc, draws through the null backend and refuses a
     * scaled LockRect. */
    d9_null_reset();
    D9Device *dn = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(dn);
    D9Handle tex = 0;
    assert(d9_create_render_texture(dn, 1024, 1024, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET,
                                    &tex) == D9_OK);
    D9Handle tgt = d9_texture_render_target(dn, tex);
    assert(tgt);
    assert(d9_render_target_info(dn, tgt, &w, &h, &format, &lockable,
                                 &bytes) == D9_OK);
    assert(w == 1024 && h == 1024 && bytes == 512u * 512u * 4u);
    assert(d9_render_target_storage_info(dn, tgt, &sw, &sh) == D9_OK);
    assert(sw == 512 && sh == 512);
    assert(d9_memory_used(dn) == 512u * 512u * 4u);

    assert(d9_set_render_target(dn, tgt) == D9_OK);
    assert(d9_begin_scene(dn, 0xff000000u) == D9_OK);
    assert(d9_null_stats()->viewport_w == 512 && d9_null_stats()->viewport_h == 512);
    assert(d9_bind_texture(dn, 0, tex) == D9_OK);
    assert(d9_draw_primitive_up(dn, D9_TRIANGLELIST, 1, vertices,
                                sizeof vertices[0]) == D9_OK);
    assert(d9_null_stats()->target_draws == 1);
    assert(d9_end_scene(dn) == D9_OK);
    assert(d9_render_target_read(dn, tgt, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(d9_release_texture(dn, tex) == D9_OK);
    /* tgt is the current target: release retires it; the device destroy below
     * performs the actual backend delete. */
    assert(d9_null_stats()->target_deletes == 0);
    assert(d9_destroy_device_ex(dn) == 0);

    /* Budget: five 1024 targets fit in a 5 MiB device by native accounting
     * (5 * 1 MiB), and the sixth is refused.  The old 4 MiB-per-target cost
     * would have failed at the second. */
    d9_null_reset();
    D9Device *db = d9_create_device(d9_null_backend(), d9_null_context(), 5u << 20);
    assert(db);
    D9Handle many[5] = {0};
    for (unsigned i = 0; i < 5; ++i)
        assert(d9_create_render_texture(db, 1024, 1024, D9_A8R8G8B8,
                                        D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET,
                                        &many[i]) == D9_OK);
    assert(d9_memory_used(db) == 5u * 1024u * 1024u);
    D9Handle extra = 0;
    assert(d9_create_render_texture(db, 1024, 1024, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET,
                                    &extra) == D9_MEMORY);
    for (unsigned i = 0; i < 5; ++i)
        assert(d9_release_texture(db, many[i]) == D9_OK);
    assert(d9_destroy_device_ex(db) == 0);

    printf("bounded render targets: 1024->512 native, scaled viewport, budget PASS\n");
    return 0;
}
