/* Regression for D3DUSAGE_RENDERTARGET textures.
 *
 * The SM3 gate census showed the first post-gate D3D9 failure was
 * CreateTexture(1024x1024, usage=RENDERTARGET, format=A32B32G32R32F) returning
 * D3DERR_NOTAVAILABLE, followed by A8R8G8B8 requests that also failed because
 * render-target textures were not implemented.  TESV then dereferenced the
 * missing render target and faulted at address 0x0c.
 *
 * These checks pin the honest LDR path: a pow2 8..1024 X8R8G8B8/A8R8G8B8
 * target-backed texture that GetSurfaceLevel(0) exposes as a render target and
 * SetTexture accepts as a sampler source.  Float/HDR formats, mip chains,
 * MANAGED pools and non-pow2 sizes stay named refusals.
 *
 * Core checks use the null backend, which keeps real target pixels.  COM
 * checks run through the demo device and the same lifted vtables the guest
 * uses. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 0x10010600u      /* inside the demo's mapped scratch page */
#define OUT (BUF + 0x700u)
#define ERR_INVALID 0x8876086cu
#define ERR_UNAVAILABLE 0x8876086au

/* ---------------------------------------------------------------- core --- */

static void core_render_texture(void) {
    d9_null_reset();
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);

    D9Handle tex = 0;
    assert(d9_create_render_texture(d, 256, 256, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &tex) == D9_OK);
    assert(tex);
    D9Handle tgt = d9_texture_render_target(d, tex);
    assert(tgt);

    unsigned w = 0, h = 0, format = 0, lockable = 1;
    assert(d9_render_target_info(d, tgt, &w, &h, &format, &lockable, NULL) == D9_OK);
    assert(w == 256 && h == 256 && format == D9_A8R8G8B8 && lockable == 0);

    unsigned pool = 0, usage = 0, levels = 0;
    size_t guest_bytes = 1, gpu_bytes = 0;
    int cpu_only = 1, gpu_only = 0;
    assert(d9_texture_resource_info(d, tex, &pool, &usage, &format, &levels,
                                    &guest_bytes, &gpu_bytes, &cpu_only, &gpu_only) == D9_OK);
    assert(pool == D9_POOL_DEFAULT && usage == D9_USAGE_RENDERTARGET);
    assert(format == D9_A8R8G8B8 && levels == 1);
    assert(guest_bytes == 0 && cpu_only == 0 && gpu_only == 1);
    assert(gpu_bytes == 0); /* the Target owns the allocation once */

    /* The texture can be the active render target and a bound sampler. */
    assert(d9_set_render_target(d, tgt) == D9_OK);
    assert(d9_begin_scene(d, 0xff112233u) == D9_OK);
    assert(d9_bind_texture(d, 0, tex) == D9_OK);
    assert(d9_end_scene(d) == D9_OK);

    uint8_t row[256 * 4];
    assert(d9_render_target_read(d, tgt, row, sizeof row, 0, 0, 256, 1) == D9_OK);
    /* D3DCOLOR 0xAARRGGBB reaches BGRA memory as B,G,R,A. */
    assert(row[0] == 0x33 && row[1] == 0x22 && row[2] == 0x11 && row[3] == 0xff);

    /* Retrieving the surface level is a texture property: only level 0. */
    assert(d9_texture_render_target(d, tex) == tgt);

    /* Everything the census asked for beyond LDR stays a refusal. */
    D9Handle bad = 0;
    assert(d9_create_render_texture(d, 256, 256, 0x72u /* A32B32G32R32F */,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &bad) == D9_INVALID);
    assert(d9_create_render_texture(d, 256, 256, D9_A8R8G8B8,
                                    D9_POOL_MANAGED, D9_USAGE_RENDERTARGET, &bad) == D9_INVALID);
    assert(d9_create_render_texture(d, 100, 256, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &bad) == D9_OK);
    assert(d9_release_texture(d, bad) == D9_OK); bad = 0;
    assert(d9_create_render_texture(d, 2000, 256, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &bad) == D9_INVALID);
    assert(d9_create_render_texture(d, 256, 256, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, 0, &bad) == D9_INVALID);

    /* NPOT logical surfaces round up to POT storage; the excess is padding. */
    D9Handle npot = 0;
    assert(d9_create_render_texture(d, 400, 240, D9_A8R8G8B8,
                                    D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET, &npot) == D9_OK);
    D9Handle ntgt = d9_texture_render_target(d, npot);
    assert(ntgt);
    unsigned nw = 0, nh = 0;
    assert(d9_render_target_info(d, ntgt, &nw, &nh, NULL, NULL, NULL) == D9_OK);
    assert(nw == 400 && nh == 240);
    unsigned total_w = 0, total_h = 0;
    assert(d9_render_target_storage_info(d, ntgt, &total_w, &total_h) == D9_OK);
    assert(total_w == 512 && total_h == 256);
    assert(d9_release_texture(d, npot) == D9_OK);

    /* Releasing the texture releases exactly one backing target. */
    assert(d9_null_stats()->target_creates == 3);
    assert(d9_release_texture(d, tex) == D9_OK);
    assert(d9_null_stats()->target_deletes == 3);
    assert(d9_set_render_target(d, tgt) == D9_INVALID);

    d9_destroy_device(d);
}

/* ------------------------------------------------------------ NPOT core --- */

static void core_npot_render_target(void) {
    d9_null_reset();
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);

    struct { unsigned w, h, tw, th; } sizes[] = {
        {400, 240, 512, 256}, {320, 180, 512, 256}, {100, 100, 128, 128},
        {2, 2, 8, 8}, {1, 1, 8, 8},
    };
    D9Handle tex[5] = {0}, tgt[5] = {0};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        assert(d9_create_render_texture(d, sizes[i].w, sizes[i].h, D9_A8R8G8B8,
                                        D9_POOL_DEFAULT, D9_USAGE_RENDERTARGET,
                                        &tex[i]) == D9_OK);
        tgt[i] = d9_texture_render_target(d, tex[i]);
        assert(tgt[i]);
        unsigned sw = 0, sh = 0;
        assert(d9_render_target_storage_info(d, tgt[i], &sw, &sh) == D9_OK);
        assert(sw == sizes[i].tw && sh == sizes[i].th);
        /* Clear covers the logical surface; readback is an exact sub-rect. */
        assert(d9_set_render_target(d, tgt[i]) == D9_OK);
        assert(d9_begin_scene(d, 0xff112233u) == D9_OK);
        assert(d9_end_scene(d) == D9_OK);
        uint8_t *row = malloc((size_t)sizes[i].w * sizes[i].h * 4);
        assert(row);
        assert(d9_render_target_read(d, tgt[i], row, (size_t)sizes[i].w * 4,
                                     0, 0, sizes[i].w, sizes[i].h) == D9_OK);
        for (unsigned p = 0; p < sizes[i].w * sizes[i].h; ++p)
            assert(((uint32_t *)row)[p] == 0xff112233u);
        free(row);
    }

    /* Partial write/read of the logical sub-rect leaves the rest intact. */
    uint32_t patch[20 * 10];
    for (unsigned i = 0; i < 20 * 10; ++i) patch[i] = 0xff445566u;
    assert(d9_render_target_write(d, tgt[0], patch, 20 * 4, 10, 5, 20, 10) == D9_OK);
    static uint32_t full[400 * 240];
    assert(d9_render_target_read(d, tgt[0], full, 400 * 4, 0, 0, 400, 240) == D9_OK);
    for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 400; ++x) {
            int inside = x >= 10 && x < 30 && y >= 5 && y < 15;
            assert(full[y * 400 + x] == (inside ? 0xff445566u : 0xff112233u));
        }

    /* StretchRect NPOT -> NPOT (full logical rects). */
    assert(d9_blit(d, tgt[1], NULL, tgt[2], NULL, D9_FILTER_POINT) == D9_OK);
    static uint32_t np[320 * 180];
    assert(d9_render_target_read(d, tgt[1], np, 320 * 4, 0, 0, 320, 180) == D9_OK);
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 0; x < 320; ++x)
            assert(np[y * 320 + x] == 0xff112233u);

    /* StretchRect NPOT -> POT target. */
    D9Handle pot = 0;
    assert(d9_create_render_target(d, 256, 256, D9_A8R8G8B8, 0, &pot) == D9_OK);
    assert(d9_blit(d, pot, NULL, tgt[2], NULL, D9_FILTER_POINT) == D9_OK);
    static uint32_t pp[256 * 256];
    assert(d9_render_target_read(d, pot, pp, 256 * 4, 0, 0, 256, 256) == D9_OK);
    for (unsigned i = 0; i < 256 * 256; ++i) assert(pp[i] == 0xff112233u);

    assert(d9_release_render_target(d, pot) == D9_OK);
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
        assert(d9_release_texture(d, tex[i]) == D9_OK);
    d9_destroy_device(d);
}

/* ----------------------------------------------------------------- COM --- */

#define TEX_MAGIC 0x54455831u
#define TARGET_MAGIC 0x54475231u
#define SAMPLE_MAGIC 0x53414d31u
typedef struct {
    unsigned created, uploaded, deleted, begin, draw, end;
    size_t n;
    int active;
    D9Matrix clip;
    D9DrawState state;
    D9Vertex vertices[36];
    unsigned targets_created, targets_deleted, targets_bound;
    unsigned sample_draws, texture_draws;
} Mock;
typedef struct { size_t base; unsigned levels; uint8_t *data; uint32_t magic; } Tex;
typedef struct { uint32_t magic; } MockSample;
typedef struct { uint32_t magic; MockSample sample; } MockTarget;

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels, void **native) {
    Mock *m = ctx;
    Tex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->base = (size_t)w * h * 4;
    t->levels = levels;
    t->magic = TEX_MAGIC;
    size_t total = 0;
    for (unsigned i = 0; i < levels; ++i) total += t->base >> (2 * i);
    t->data = malloc(total);
    if (!t->data) { free(t); return -1; }
    *native = t;
    ++m->created;
    return 0;
}
static int tex_upload(void *ctx, void *native, unsigned level, const void *data, size_t n) {
    Mock *m = ctx;
    Tex *t = native;
    assert(level < t->levels && n == (t->base >> (2 * level)));
    size_t off = 0;
    for (unsigned i = 0; i < level; ++i) off += t->base >> (2 * i);
    memcpy(t->data + off, data, n);
    ++m->uploaded;
    return 0;
}
static void tex_delete(void *ctx, void *native) {
    Mock *m = ctx;
    Tex *t = native;
    assert(!m->active);
    free(t->data);
    free(t);
    ++m->deleted;
}
static int begin(void *ctx, uint32_t color) {
    Mock *m = ctx;
    (void)color;
    assert(!m->active);
    m->active = 1;
    ++m->begin;
    return 0;
}
static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *mat, void *t,
                const D9DrawState *state) {
    Mock *s = ctx;
    assert(s->active && v && mat && t && n <= 36);
    uint32_t magic = *(uint32_t *)t;
    assert(magic == TEX_MAGIC || magic == SAMPLE_MAGIC);
    if (magic == SAMPLE_MAGIC) ++s->sample_draws; else ++s->texture_draws;
    s->n = n;
    s->clip = *mat;
    s->state = *state;
    memcpy(s->vertices, v, n * sizeof *v);
    ++s->draw;
    return 0;
}
static void end(void *ctx) {
    Mock *m = ctx;
    assert(m->active);
    m->active = 0;
    ++m->end;
}
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format, void **native) {
    Mock *m = ctx;
    (void)w; (void)h; (void)format;
    MockTarget *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->magic = TARGET_MAGIC;
    t->sample.magic = SAMPLE_MAGIC;
    *native = t;
    ++m->targets_created;
    return 0;
}
static void target_delete(void *ctx, void *native) {
    Mock *m = ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    t->magic = 0;
    free(t);
    ++m->targets_deleted;
}
static int target_bind(void *ctx, void *native) {
    Mock *m = ctx;
    MockTarget *t = native;
    assert(!t || t->magic == TARGET_MAGIC);
    ++m->targets_bound;
    return 0;
}
static int target_clear(void *ctx, void *native, uint32_t argb) {
    (void)ctx; (void)argb;
    MockTarget *t = native;
    assert(!t || t->magic == TARGET_MAGIC);
    return 0;
}
static void *target_texture(void *ctx, void *native) {
    (void)ctx;
    MockTarget *t = native;
    assert(t && t->magic == TARGET_MAGIC);
    return &t->sample;
}
static int target_viewport_scaled(void *ctx, unsigned logical_w, unsigned logical_h,
                                  unsigned native_w, unsigned native_h) {
    Mock *m = ctx;
    (void)logical_w; (void)logical_h; (void)native_w; (void)native_h;
    ++m->targets_bound;
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
    .target_texture = target_texture,
    .target_viewport_scaled = target_viewport_scaled,
};

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *a, unsigned n) {
    uint32_t r = 0;
    int rc = d9_com_demo_call(s, object, slot, a, n, &r);
    if (rc) {
        fprintf(stderr, "demo call trap object=0x%08x slot=%u rc=%d args=%u: %s\n",
                object, slot, rc, n, s->message);
        abort();
    }
    return r;
}
static uint32_t device_call(D9ComDemo *s, unsigned slot, const uint32_t *a, unsigned n) {
    return call(s, s->device, slot, a, n);
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t v = 0;
    assert(!guest_read32(&s->memory, address, &v));
    return v;
}

static uint32_t create_rt(D9ComDemo *s, unsigned w, unsigned h, unsigned levels,
                          unsigned format, unsigned pool) {
    uint32_t a[] = {w, h, levels, 1u, format, pool, OUT, 0};
    return device_call(s, 23, a, 8);
}

static void com_render_texture(void) {
    Mock m = {0};
    D9Device *d = d9_create_device(&ops, &m, 8u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return;
    }

    /* 1. CheckDeviceFormat now tells the truth about LDR render-target
     *    textures and keeps float/HDR and autogen unsupported. */
    uint32_t ok_a8[] = {0, 1, D9_X8R8G8B8, 1, 3, D9_A8R8G8B8};
    uint32_t ok_x8[] = {0, 1, D9_X8R8G8B8, 1, 3, D9_X8R8G8B8};
    uint32_t hdr[] = {0, 1, D9_X8R8G8B8, 1, 3, 0x72u};
    uint32_t autogen[] = {0, 1, D9_X8R8G8B8, 0x400u, 3, D9_A8R8G8B8};
    uint32_t plain[] = {0, 1, D9_X8R8G8B8, 0, 3, D9_A8R8G8B8};
    assert(call(&demo, demo.factory, 10, ok_a8, 6) == 0);
    assert(call(&demo, demo.factory, 10, ok_x8, 6) == 0);
    assert(call(&demo, demo.factory, 10, hdr, 6) == ERR_UNAVAILABLE);
    assert(call(&demo, demo.factory, 10, autogen, 6) == ERR_UNAVAILABLE);
    assert(call(&demo, demo.factory, 10, plain, 6) == 0);

    /* 2. The 1024x1024 A8R8G8B8 target texture the census asked for. */
    assert(create_rt(&demo, 1024, 1024, 1, D9_A8R8G8B8, D9_POOL_DEFAULT) == 0);
    uint32_t tex = read32(&demo, OUT);
    assert(tex);

    uint32_t desc[] = {0, OUT};
    memset(guest_ptr(&demo.memory, OUT, 32), 0xcd, 32);
    assert(call(&demo, tex, 17, desc, 2) == 0);
    uint32_t got[8];
    assert(!guest_read(&demo.memory, OUT, got, sizeof got));
    assert(got[0] == D9_A8R8G8B8 && got[1] == 1 && got[2] == 1 && got[3] == D9_POOL_DEFAULT);
    assert(got[6] == 1024 && got[7] == 1024);

    /* 3. GetSurfaceLevel(0) yields a real render target; level 1 does not. */
    uint32_t gl[] = {0, OUT};
    assert(call(&demo, tex, 18, gl, 2) == 0);
    uint32_t surface = read32(&demo, OUT);
    assert(surface && m.targets_created == 1);
    uint32_t gl1[] = {1, OUT};
    assert(call(&demo, tex, 18, gl1, 2) == ERR_INVALID);

    /* 4. Bind it, bind the texture as a sampler, and draw a frame into it. */
    uint32_t bind[] = {0, surface};
    assert(device_call(&demo, 37, bind, 2) == 0);
    uint32_t stage[] = {0, tex};
    assert(device_call(&demo, 65, stage, 2) == 0);
    unsigned draws_before = m.draw;
    uint32_t clear[] = {0, 0, 1, 0xff14243c, 0x3f800000, 0};
    uint32_t di[] = {D9_TRIANGLELIST, 0, 0, 24, 0, 12};
    assert(device_call(&demo, 43, clear, 6) == 0);
    assert(device_call(&demo, 41, NULL, 0) == 0);
    assert(device_call(&demo, 82, di, 6) == 0);
    assert(device_call(&demo, 42, NULL, 0) == 0);
    uint32_t present[] = {0, 0, 0, 0};
    assert(device_call(&demo, 17, present, 4) == 0);
    assert(m.draw == draws_before + 1);
    assert(m.targets_bound >= 1);
    assert(m.n == 36);
    assert(m.sample_draws == 1 && m.texture_draws == 0);

    /* 5. Restoring the implicit back buffer and the demo texture still works. */
    uint32_t back[] = {0, 0};
    assert(device_call(&demo, 37, back, 2) == 0);
    uint32_t demo_tex[] = {0, demo.texture};
    assert(device_call(&demo, 65, demo_tex, 2) == 0);

    /* 6. 512/256 bloom sizes are independent targets. */
    assert(create_rt(&demo, 512, 512, 1, D9_A8R8G8B8, D9_POOL_DEFAULT) == 0);
    uint32_t tex512 = read32(&demo, OUT);
    assert(create_rt(&demo, 256, 256, 1, D9_X8R8G8B8, D9_POOL_DEFAULT) == 0);
    uint32_t tex256 = read32(&demo, OUT);
    assert(tex512 && tex256 && m.targets_created == 3);

    /* 7. NPOT surfaces keep their logical size and use POT storage. */
    assert(create_rt(&demo, 400, 240, 1, D9_A8R8G8B8, D9_POOL_DEFAULT) == 0);
    uint32_t tex400 = read32(&demo, OUT);
    assert(tex400 && m.targets_created == 4);
    uint32_t nsv[] = {0, OUT};
    assert(call(&demo, tex400, 18, nsv, 2) == 0);
    uint32_t nsurf = read32(&demo, OUT);
    assert(nsurf && m.targets_created == 4);
    /* Scaleform also creates 2x2 helper targets; storage is the 8x8 floor. */
    assert(create_rt(&demo, 2, 2, 1, D9_A8R8G8B8, D9_POOL_DEFAULT) == 0);
    uint32_t tex2 = read32(&demo, OUT);
    assert(tex2 && m.targets_created == 5);

    /* 8. Refusals are named and do not allocate a target. */
    unsigned created_before = m.targets_created;
    assert(create_rt(&demo, 1024, 1024, 1, 0x72u, D9_POOL_DEFAULT) == ERR_UNAVAILABLE);
    assert(create_rt(&demo, 1024, 1024, 1, 0x71u, D9_POOL_DEFAULT) == ERR_UNAVAILABLE);
    assert(create_rt(&demo, 1024, 1024, 2, D9_A8R8G8B8, D9_POOL_DEFAULT) == ERR_UNAVAILABLE);
    assert(create_rt(&demo, 1024, 1024, 1, D9_A8R8G8B8, D9_POOL_MANAGED) == ERR_UNAVAILABLE);
    assert(create_rt(&demo, 2048, 128, 1, D9_A8R8G8B8, D9_POOL_DEFAULT) == ERR_UNAVAILABLE);
    assert(m.targets_created == created_before);

    /* 9. Releasing surface + texture releases exactly the backing target. */
    assert(call(&demo, surface, 2, NULL, 0) == 0);
    assert(call(&demo, tex, 2, NULL, 0) == 0);
    assert(m.targets_deleted == 1);
    assert(call(&demo, tex512, 2, NULL, 0) == 0);
    assert(call(&demo, tex256, 2, NULL, 0) == 0);
    assert(m.targets_deleted == 3);
    assert(call(&demo, nsurf, 2, NULL, 0) == 0);
    assert(call(&demo, tex400, 2, NULL, 0) == 0);
    assert(m.targets_deleted == 4);
    assert(call(&demo, tex2, 2, NULL, 0) == 0);
    assert(m.targets_deleted == 5);

    d9_com_demo_destroy(&demo);
    assert(!d9_destroy_device_ex(d));

    printf("render texture: core target-backed sampling, COM surface/bind and named refusals PASS\n");
}

int main(void) {
    core_render_texture();
    core_npot_render_target();
    com_render_texture();
    return 0;
}
