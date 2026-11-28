/* Measured TESV render-target set under the memory-bounded native cap.
 *
 * The measured set is five 1024x1024 targets (float fallback stored as A8R8G8B8),
 * three 512x512 A8 targets and one 512x512 float target.  With the default
 * 256-per-axis native cap all nine become 256x256, so colour + DEPTH24_STENCIL8
 * costs 9 * (256 KiB + 256 KiB) = 4.5 MiB.  The retained 512x256 back buffer
 * adds 1 MiB, for 5.5 MiB total: inside the 6 MiB N3DS VRAM budget.
 *
 * The same test also pins the scaled viewport and the NDC edge mapping, a
 * too-large cap refusing by name, and the env override.
 */
#include "d3d9_core.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_MAGIC 0x54475242u

typedef struct { uint32_t magic; unsigned w, h; size_t cost; } MockTarget;
typedef struct {
    unsigned creates, deletes, binds, clears, draws;
    unsigned create_w, create_h;
    unsigned scaled_calls, logical_w, logical_h, native_w, native_h;
    size_t vram_used, vram_limit;
    D9Matrix clip;
    int have_clip;
} Ctx;

static void set_cap_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TARGET_NATIVE_MAX", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_TARGET_NATIVE_MAX", v, 1);
    else unsetenv("TESV_D3D9_TARGET_NATIVE_MAX");
#endif
}

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
    (void)v; (void)n; (void)texture; (void)state;
    Ctx *c = ctx;
    c->clip = *clip;
    c->have_clip = 1;
    ++c->draws;
    return 0;
}
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format,
                         void **native) {
    (void)format;
    Ctx *c = ctx;
    size_t cost = (size_t)w * h * 4u * 2u; /* colour + DEPTH24_STENCIL8 */
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
    Ctx *c = ctx;
    assert(native_w <= logical_w && native_h <= logical_h);
    c->logical_w = logical_w;
    c->logical_h = logical_h;
    c->native_w = native_w;
    c->native_h = native_h;
    ++c->scaled_calls;
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
    .target_blit = target_blit,
    .target_write = target_write,
    .target_viewport = target_viewport,
    .target_viewport_scaled = target_viewport_scaled,
};

static void expect_logical(D9Device *d, D9Handle h, unsigned logical,
                           unsigned native) {
    unsigned w = 0, height = 0, format = 0, lockable = 1, sw = 0, sh = 0;
    size_t bytes = 0;
    assert(d9_render_target_info(d, h, &w, &height, &format, &lockable,
                                 &bytes) == D9_OK);
    assert(w == logical && height == logical);
    assert(bytes == (size_t)native * native * 4u);
    assert(d9_render_target_storage_info(d, h, &sw, &sh) == D9_OK);
    assert(sw == native && sh == native);
}

int main(void) {
    /* Default: nine measured targets plus the retained back buffer fit. */
    set_cap_env(NULL);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.vram_limit = 6u << 20;
    D9Device *d = d9_create_device(&ops, &c, 32u << 20);
    assert(d);
    assert(d9_target_native_max(d) == 256);

    assert(d9_set_back_buffer_info_ex(d, 400, 240, D9_A8R8G8B8, 1) == D9_OK);
    unsigned bw = 0, bh = 0, btw = 0, bth = 0, bf = 0;
    int blockable = 0;
    assert(d9_back_buffer_info_ex(d, &bw, &bh, &bf, &btw, &bth,
                                 &blockable) == D9_OK);
    assert(bw == 400 && bh == 240 && btw == 512 && bth == 256 && blockable);
    D9Handle back = d9_back_buffer_target(d);
    assert(back);
    unsigned bsw = 0, bsh = 0;
    assert(d9_render_target_storage_info(d, back, &bsw, &bsh) == D9_OK);
    assert(bsw == 512 && bsh == 256);
    assert(c.vram_used == (size_t)512 * 256 * 4u * 2u);
    unsigned after_back = c.creates;

    D9Handle set[9] = {0};
    for (unsigned i = 0; i < 5; ++i) {
        assert(d9_create_render_target(d, 1024, 1024, D9_A8R8G8B8, 0,
                                       &set[i]) == D9_OK);
        assert(c.create_w == 256 && c.create_h == 256);
        expect_logical(d, set[i], 1024, 256);
    }
    for (unsigned i = 5; i < 9; ++i) {
        assert(d9_create_render_target(d, 512, 512, D9_A8R8G8B8, 0,
                                       &set[i]) == D9_OK);
        assert(c.create_w == 256 && c.create_h == 256);
        expect_logical(d, set[i], 512, 256);
    }
    assert(c.creates == after_back + 9);
    assert(c.vram_used == (size_t)512 * 256 * 4u * 2u +
                          (size_t)9 * 256 * 256 * 4u * 2u);
    assert(c.vram_used == (size_t)11 * 512u * 1024u); /* 5.5 MiB */
    assert(c.vram_used <= (size_t)6u << 20);

    /* Every target binds with the scaled native viewport. */
    for (unsigned i = 0; i < 9; ++i) {
        assert(d9_set_render_target(d, set[i]) == D9_OK);
        assert(d9_begin_scene(d, 0) == D9_OK);
        assert(c.scaled_calls == i + 1);
        assert(c.logical_w == (i < 5 ? 1024u : 512u));
        assert(c.native_w == 256 && c.native_h == 256);
        assert(d9_end_scene(d) == D9_OK);
    }

    /* POSITIONT edge maps to NDC +-1 and the native viewport is 256, so the
     * 1024 logical edge lands on the native edge instead of being clipped. */
    assert(d9_set_render_target(d, set[0]) == D9_OK);
    assert(d9_begin_scene(d, 0) == D9_OK);
    assert(d9_set_viewport_size(d, 1024, 1024) == D9_OK);
    D9VertexElement pe[] = {{0, 0, D9_DECL_FLOAT4, 0, D9_USAGE_POSITIONT, 0}};
    D9Decl pdecl;
    assert(d9_decl_init(&pdecl, pe, 1) == 0);
    assert(d9_set_vertex_declaration(d, &pdecl) == D9_OK);
    float tri[3][4] = {{0, 0, 0, 1}, {1024, 0, 0, 1}, {0, 1024, 0, 1}};
    assert(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, tri,
                                sizeof tri[0]) == D9_OK);
    assert(c.draws == 1 && c.have_clip);
    assert(c.native_w == 256 && c.native_h == 256);
    float nx = c.clip.m[1][0] * 1024.0f + c.clip.m[1][3];
    float ny = c.clip.m[0][1] * 1024.0f + c.clip.m[0][3];
    assert(fabsf(nx + 1.0f) < 1e-5f);
    assert(fabsf(ny + 1.0f) < 1e-5f);
    assert(d9_end_scene(d) == D9_OK);

    for (unsigned i = 0; i < 9; ++i)
        assert(d9_release_render_target(d, set[i]) == D9_OK);
    /* The current (last) target is retired, not deleted, until the device is
     * destroyed: its 512x256 storage still counts. */
    assert(c.vram_used == (size_t)512 * 256 * 4u * 3u);
    assert(d9_destroy_device_ex(d) == 0);
    assert(c.vram_used == 0);

    /* A too-large cap must fail by name at the first target that cannot fit:
     * 512 cap -> 2 MiB per 1024 target, so the fourth exceeds 6 MiB. */
    set_cap_env("512");
    Ctx n;
    memset(&n, 0, sizeof n);
    n.vram_limit = 6u << 20;
    D9Device *dn = d9_create_device(&ops, &n, 32u << 20);
    assert(dn);
    assert(d9_target_native_max(dn) == 512);
    D9Handle first3[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        assert(d9_create_render_target(dn, 1024, 1024, D9_A8R8G8B8, 0,
                                       &first3[i]) == D9_OK);
        assert(n.vram_used == (size_t)(i + 1) * 2u * 1024u * 1024u);
    }
    D9Handle nofit = 0;
    assert(d9_create_render_target(dn, 1024, 1024, D9_A8R8G8B8, 0, &nofit)
           == D9_BACKEND);
    assert(nofit == 0 && n.creates == 3);
    assert(n.vram_used == (size_t)6u << 20);
    assert(strstr(d9_last_draw_diagnostic(dn), "512x512 native target") != NULL);
    for (unsigned i = 0; i < 3; ++i)
        assert(d9_release_render_target(dn, first3[i]) == D9_OK);
    assert(d9_destroy_device_ex(dn) == 0);

    /* Env override: 128 is accepted, 300 rounds down to 256, invalid falls back
     * to the default 256. */
    set_cap_env("128");
    Ctx q;
    memset(&q, 0, sizeof q);
    D9Device *dq = d9_create_device(&ops, &q, 8u << 20);
    assert(dq && d9_target_native_max(dq) == 128);
    D9Handle one = 0;
    assert(d9_create_render_target(dq, 1024, 1024, D9_A8R8G8B8, 0, &one) == D9_OK);
    expect_logical(dq, one, 1024, 128);
    assert(q.vram_used == (size_t)128 * 128 * 4u * 2u);
    assert(d9_release_render_target(dq, one) == D9_OK);
    assert(d9_destroy_device_ex(dq) == 0);

    set_cap_env("300");
    Ctx r;
    memset(&r, 0, sizeof r);
    D9Device *dr = d9_create_device(&ops, &r, 8u << 20);
    assert(dr && d9_target_native_max(dr) == 256);
    assert(d9_destroy_device_ex(dr) == 0);

    set_cap_env("32");
    Ctx s;
    memset(&s, 0, sizeof s);
    D9Device *ds = d9_create_device(&ops, &s, 8u << 20);
    assert(ds && d9_target_native_max(ds) == 256);
    assert(d9_destroy_device_ex(ds) == 0);

    set_cap_env(NULL);
    printf("measured render-target set: default 256 cap, 4.5 MiB + 1 MiB back buffer, 6 MiB budget PASS\n");
    return 0;
}
