/* Render-target texture sampling contract.
 *
 * A Target and a sampleable Texture are different objects.  This mock gives
 * them different magic tags and refuses to let the core pass a Target where a
 * sampler handle is required.  It also covers bind-before/after-target-render,
 * draw_two with a second RT texture, exactly-once target deletion and the
 * colour-budget/backend refusal split.
 */
#include "d3d9_core.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_MAGIC 0x54455832u

static void set_cap_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TARGET_NATIVE_MAX", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_TARGET_NATIVE_MAX", v, 1);
    else unsetenv("TESV_D3D9_TARGET_NATIVE_MAX");
#endif
}
#define TARGET_MAGIC 0x54475232u
#define SAMPLE_MAGIC 0x53414d32u

typedef struct { uint32_t magic; unsigned levels; } MockTex;
typedef struct { uint32_t magic; int owner; } MockSample;
typedef struct { uint32_t magic; MockSample sample; } MockTarget;

typedef struct {
    unsigned tex_creates, tex_deletes, target_creates, target_deletes;
    unsigned draws, draw_twos, colored_draws;
    uint32_t last0, last1;
    int active, refuse_large;
    unsigned native_w, native_h, scaled_calls;
    unsigned logical_w, logical_h;
    unsigned vp_calls, vp_w, vp_h;
    float last_u[3], last_v[3];
} Ctx;

static int tex_create(void *opaque, unsigned w, unsigned h, unsigned levels,
                      void **native) {
    (void)w; (void)h;
    Ctx *c = opaque;
    MockTex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->magic = TEX_MAGIC;
    t->levels = levels;
    ++c->tex_creates;
    *native = t;
    return 0;
}
static int tex_upload(void *opaque, void *native, unsigned level,
                      const void *data, size_t bytes) {
    (void)opaque; (void)native; (void)level; (void)data; (void)bytes;
    return 0;
}
static void tex_delete(void *opaque, void *native) {
    Ctx *c = opaque;
    MockTex *t = native;
    assert(t && t->magic == TEX_MAGIC);
    t->magic = 0;
    free(t);
    ++c->tex_deletes;
}
static int begin(void *opaque, uint32_t argb) {
    (void)argb;
    Ctx *c = opaque;
    assert(!c->active);
    c->active = 1;
    return 0;
}
static int draw(void *opaque, const D9Vertex *v, size_t n,
                const D9Matrix *clip, void *texture, const D9DrawState *state) {
    (void)clip; (void)state;
    Ctx *c = opaque;
    assert(c->active && texture && v && n <= 3);
    c->last0 = *(uint32_t *)texture;
    assert(c->last0 == TEX_MAGIC || c->last0 == SAMPLE_MAGIC);
    for (size_t i = 0; i < n; ++i) { c->last_u[i] = v[i].u; c->last_v[i] = v[i].v; }
    ++c->draws;
    return 0;
}
static int draw_colored(void *opaque, const D9ColorVertex *v, size_t n,
                        const D9Matrix *clip, void *texture,
                        const D9DrawState *state) {
    (void)v; (void)n; (void)clip; (void)state; (void)texture;
    Ctx *c = opaque;
    ++c->colored_draws;
    return 0;
}
static int draw_two(void *opaque, const D9ColorVertex *v, size_t n,
                    const D9Matrix *clip, void *tex0, const D9DrawState *s0,
                    void *tex1, const D9DrawState *s1) {
    (void)v; (void)n; (void)clip; (void)s0; (void)s1;
    Ctx *c = opaque;
    assert(c->active && tex0 && tex1 && tex0 != tex1);
    c->last0 = *(uint32_t *)tex0;
    c->last1 = *(uint32_t *)tex1;
    assert(c->last0 == SAMPLE_MAGIC && c->last1 == SAMPLE_MAGIC);
    ++c->draw_twos;
    return 0;
}
static void end(void *opaque) {
    Ctx *c = opaque;
    assert(c->active);
    c->active = 0;
}
static int target_create(void *opaque, unsigned w, unsigned h,
                         unsigned format, void **native) {
    (void)format;
    Ctx *c = opaque;
    c->native_w = w; c->native_h = h;
    if (c->refuse_large && w >= 512 && h >= 512) return -1;
    MockTarget *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->magic = TARGET_MAGIC;
    t->sample.magic = SAMPLE_MAGIC;
    t->sample.owner = 1;
    ++c->target_creates;
    *native = t;
    return 0;
}
static void target_delete(void *opaque, void *native) {
    Ctx *c = opaque;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    t->magic = 0;
    t->sample.magic = 0;
    free(t);
    ++c->target_deletes;
}
static int target_bind(void *opaque, void *native) {
    (void)opaque;
    MockTarget *t = native;
    assert(!t || t->magic == TARGET_MAGIC);
    return 0;
}
static int target_viewport(void *opaque, unsigned w, unsigned h) {
    Ctx *c = opaque;
    assert(c->active);
    c->vp_calls++; c->vp_w = w; c->vp_h = h;
    return 0;
}
static int target_clear(void *opaque, void *native, uint32_t argb) {
    (void)opaque; (void)argb;
    MockTarget *t = native;
    assert(!t || t->magic == TARGET_MAGIC);
    return 0;
}
static void *target_texture(void *opaque, void *native) {
    (void)opaque;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    return &t->sample;
}
static int target_viewport_scaled(void *opaque, unsigned logical_w,
                                  unsigned logical_h, unsigned native_w,
                                  unsigned native_h) {
    Ctx *c = opaque;
    assert(c->active);
    assert(native_w <= logical_w && native_h <= logical_h);
    c->scaled_calls++;
    c->logical_w = logical_w; c->logical_h = logical_h;
    c->native_w = native_w; c->native_h = native_h;
    return 0;
}

static const D9Backend ops = {
    .texture_create = tex_create,
    .texture_upload = tex_upload,
    .texture_delete = tex_delete,
    .begin = begin,
    .draw = draw,
    .end = end,
    .draw_colored = draw_colored,
    .draw_two = draw_two,
    .target_create = target_create,
    .target_delete = target_delete,
    .target_bind = target_bind,
    .target_clear = target_clear,
    .target_texture = target_texture,
    .target_viewport = target_viewport,
    .target_viewport_scaled = target_viewport_scaled,
};

static void stage1(D9Device *d) {
    assert(d9_set_texture_stage_state(d, 1, D9_TSS_COLOROP, D9_TOP_MODULATE) == D9_OK);
    assert(d9_set_texture_stage_state(d, 1, D9_TSS_COLORARG1, D9_TA_CURRENT) == D9_OK);
    assert(d9_set_texture_stage_state(d, 1, D9_TSS_COLORARG2, D9_TA_TEXTURE) == D9_OK);
    assert(d9_set_texture_stage_state(d, 1, D9_TSS_ALPHAOP, D9_TOP_SELECTARG2) == D9_OK);
    assert(d9_set_texture_stage_state(d, 1, D9_TSS_TEXCOORDINDEX, 0) == D9_OK);
}

int main(void) {
    set_cap_env("512"); /* this regression pins the 512-cap sample path */
    Ctx c;
    memset(&c, 0, sizeof c);
    D9Device *d = d9_create_device(&ops, &c, 16u << 20);
    assert(d);

    D9Handle t0 = 0, t1 = 0;
    assert(d9_create_render_texture(d, 8, 8, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &t0) == D9_OK);
    assert(d9_create_render_texture(d, 8, 8, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &t1) == D9_OK);
    assert(t0 && t1 && c.target_creates == 2 && c.tex_creates == 0);
    D9Handle tgt0 = d9_texture_render_target(d, t0);
    assert(tgt0 && d9_texture_render_target(d, t1));

    D9Vertex vertices[3] = {{0,0,0,0,0},{1,0,0,1,0},{0,1,0,0,1}};

    /* Bind the RT texture before rendering into its target. */
    assert(d9_set_render_target(d, tgt0) == D9_OK);
    assert(d9_begin_scene(d, 0xff102030u) == D9_OK);
    assert(d9_bind_texture(d, 0, t0) == D9_OK);
    assert(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, vertices, sizeof vertices[0]) == D9_OK);
    assert(c.draws == 1 && c.last0 == SAMPLE_MAGIC);
    assert(d9_end_scene(d) == D9_OK);

    /* Bind/bind again after the render target has been written and restored. */
    assert(d9_set_render_target(d, 0) == D9_OK);
    assert(d9_begin_scene(d, 0) == D9_OK);
    assert(d9_bind_texture(d, 0, t0) == D9_OK);
    assert(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, vertices, sizeof vertices[0]) == D9_OK);
    assert(c.draws == 2 && c.last0 == SAMPLE_MAGIC);
    assert(d9_end_scene(d) == D9_OK);

    /* Two RT textures: stage 1 must receive the second sampleable handle. */
    stage1(d);
    assert(d9_begin_scene(d, 0) == D9_OK);
    assert(d9_bind_texture(d, 0, t0) == D9_OK);
    assert(d9_bind_texture(d, 1, t1) == D9_OK);
    assert(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, vertices, sizeof vertices[0]) == D9_OK);
    assert(c.draw_twos == 1 && c.last0 == SAMPLE_MAGIC && c.last1 == SAMPLE_MAGIC);
    assert(d9_end_scene(d) == D9_OK);

    /* Exactly one target delete per RT texture, no native texture delete. */
    assert(d9_release_texture(d, t0) == D9_OK);
    assert(d9_release_texture(d, t1) == D9_OK);
    assert(c.target_deletes == 2 && c.tex_deletes == 0 && c.target_creates == 2);
    assert(d9_destroy_device_ex(d) == 0);

    /* A 1024x1024 LDR target keeps its logical size but uses 512x512 native
     * storage (1 MiB).  Core admits it under a 5 MiB device budget; a 512 KiB
     * device refuses before the backend. */
    Ctx big;
    memset(&big, 0, sizeof big);
    D9Device *d5 = d9_create_device(&ops, &big, 5u << 20);
    assert(d5);
    D9Handle r = 0;
    assert(d9_create_render_texture(d5, 1024, 1024, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &r) == D9_OK);
    assert(r && big.target_creates == 1);
    assert(big.native_w == 512 && big.native_h == 512);
    assert(d9_release_texture(d5, r) == D9_OK && big.target_deletes == 1);
    assert(d9_destroy_device_ex(d5) == 0);

    /* NPOT render-target textures: logical size stays, storage rounds up to
     * POT, UVs scale by logical/storage and the viewport is the logical rect. */
    Ctx np; memset(&np, 0, sizeof np);
    D9Device *dnp = d9_create_device(&ops, &np, 8u << 20);
    assert(dnp);
    struct { unsigned w, h, tw, th; } cases[] = {
        {400, 240, 512, 256}, {320, 180, 512, 256}, {100, 100, 128, 128},
        {2, 2, 8, 8}, {1, 1, 8, 8},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        D9Handle rt = 0;
        assert(d9_create_render_texture(dnp, cases[i].w, cases[i].h,
                                        D9_A8R8G8B8, D9_POOL_DEFAULT,
                                        D9_USAGE_RENDERTARGET, &rt) == D9_OK);
        assert(rt && np.target_creates == i + 1);
        assert(np.native_w == cases[i].tw && np.native_h == cases[i].th);
        D9Handle tgt = d9_texture_render_target(dnp, rt);
        unsigned sw = 0, sh = 0;
        assert(d9_render_target_storage_info(dnp, tgt, &sw, &sh) == D9_OK);
        assert(sw == cases[i].tw && sh == cases[i].th);

        assert(d9_set_render_target(dnp, tgt) == D9_OK);
        assert(d9_begin_scene(dnp, 0xff010203u) == D9_OK);
        assert(np.vp_calls >= 1 && np.vp_w == cases[i].w && np.vp_h == cases[i].h);
        assert(d9_bind_texture(dnp, 0, rt) == D9_OK);
        assert(d9_draw_primitive_up(dnp, D9_TRIANGLELIST, 1, vertices,
                                    sizeof vertices[0]) == D9_OK);
        float eu = (float)cases[i].w / (float)cases[i].tw;
        float ev = (float)cases[i].h / (float)cases[i].th;
        for (unsigned k = 0; k < 3; ++k) {
            assert(np.last_u[k] >= 0.0f && np.last_u[k] <= eu);
            assert(np.last_v[k] >= 0.0f && np.last_v[k] <= ev);
        }
        assert(np.last_u[1] > eu - 1e-5f && np.last_v[2] > ev - 1e-5f);
        assert(d9_end_scene(dnp) == D9_OK);
        assert(d9_release_texture(dnp, rt) == D9_OK);
    }
    assert(np.target_deletes == 5);
    assert(d9_destroy_device_ex(dnp) == 0);

    Ctx small;
    memset(&small, 0, sizeof small);
    D9Device *d3 = d9_create_device(&ops, &small, 512u << 10);
    assert(d3);
    r = 0;
    assert(d9_create_render_texture(d3, 1024, 1024, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &r) == D9_MEMORY);
    assert(r == 0 && small.target_creates == 0);
    assert(d9_destroy_device_ex(d3) == 0);

    Ctx backend;
    memset(&backend, 0, sizeof backend);
    backend.refuse_large = 1;
    D9Device *dback = d9_create_device(&ops, &backend, 16u << 20);
    assert(dback);
    r = 0;
    assert(d9_create_render_texture(dback, 1024, 1024, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &r) == D9_BACKEND);
    assert(r == 0 && backend.target_creates == 0);
    assert(strstr(d9_last_draw_diagnostic(dback), "native target") != NULL);
    assert(d9_destroy_device_ex(dback) == 0);

    printf("render-target texture sampling: distinct target/sample handles, draw_two, budget PASS\n");
    return 0;
}
