/* Measurement-only HDR->LDR render-target fallback.
 *
 * With TESV_D3D9_HDR_LDR_FALLBACK=1 the COM layer accepts the measured
 * A16B16G16R16F (0x71) and A32B32G32R32F (0x72) RENDERTARGET tuples, backs
 * them with an A8R8G8B8 target and still reports the requested format through
 * GetLevelDesc/GetDesc.  This is a lossy measurement path, not a shipping
 * capability: it exists to let the TESV census pass renderer initialisation and
 * to observe whether any menu draw samples these targets.
 *
 * The default-off refusal is covered by tests/render_texture_smoke.c. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 0x10010600u
#define OUT (BUF + 0x700u)

typedef struct {
    unsigned created, uploaded, deleted, begin, draw, end;
    unsigned targets_created, targets_deleted, targets_bound;
} Mock;
typedef struct { size_t base; unsigned levels; uint8_t *data; } Tex;

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels, void **native) {
    Mock *m = ctx;
    Tex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->base = (size_t)w * h * 4;
    t->levels = levels;
    t->data = malloc(t->base);
    if (!t->data) { free(t); return -1; }
    *native = t;
    ++m->created;
    return 0;
}
static int tex_upload(void *ctx, void *native, unsigned level, const void *data, size_t n) {
    Mock *m = ctx;
    Tex *t = native;
    assert(level < t->levels && n == (t->base >> (2 * level)));
    memcpy(t->data, data, n);
    ++m->uploaded;
    return 0;
}
static void tex_delete(void *ctx, void *native) {
    Mock *m = ctx;
    Tex *t = native;
    free(t->data);
    free(t);
    ++m->deleted;
}
static int begin(void *ctx, uint32_t color) { (void)ctx; (void)color; return 0; }
static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *mat, void *t,
                const D9DrawState *state) {
    (void)ctx; (void)v; (void)n; (void)mat; (void)t; (void)state;
    return 0;
}
static void end(void *ctx) { (void)ctx; }
static int target_create(void *ctx, unsigned w, unsigned h, unsigned format, void **native) {
    Mock *m = ctx;
    (void)w; (void)h; (void)format;
    *native = (void *)(uintptr_t)(++m->targets_created);
    return 0;
}
static void target_delete(void *ctx, void *native) { Mock *m = ctx; (void)native; ++m->targets_deleted; }
static int target_bind(void *ctx, void *native) { Mock *m = ctx; (void)native; ++m->targets_bound; return 0; }
static int target_clear(void *ctx, void *native, uint32_t argb) { (void)ctx; (void)native; (void)argb; return 0; }
static void *target_texture(void *ctx, void *native) {
    (void)ctx; (void)native;
    return (void *)(uintptr_t)0x5a5a5a5au; /* sampleable view (draw ignores it) */
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
        fprintf(stderr, "demo call trap object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
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
static uint32_t create_rt(D9ComDemo *s, unsigned w, unsigned h, unsigned format) {
    uint32_t a[] = {w, h, 1, 1u, format, 0, OUT, 0};
    return device_call(s, 23, a, 8);
}

int main(void) {
#if defined(_WIN32)
    _putenv_s("TESV_D3D9_HDR_LDR_FALLBACK", "1");
#else
    setenv("TESV_D3D9_HDR_LDR_FALLBACK", "1", 1);
#endif
    Mock m = {0};
    D9Device *d = d9_create_device(&ops, &m, 8u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }

    /* 512x512 A16B16G16R16F: real target, requested format preserved. */
    uint32_t rc = create_rt(&demo, 512, 512, 0x71u);
    if (rc) fprintf(stderr, "hdr create rc=0x%08x env=%s diag=%s\n", rc,
                    getenv("TESV_D3D9_HDR_LDR_FALLBACK"), d9_com_last_diagnostic());
    assert(rc == 0);
    uint32_t tex = read32(&demo, OUT);
    assert(tex && m.targets_created == 1);

    uint32_t desc[] = {0, OUT};
    assert(call(&demo, tex, 17, desc, 2) == 0);
    uint32_t got[8];
    assert(!guest_read(&demo.memory, OUT, got, sizeof got));
    assert(got[0] == 0x71u && got[1] == 1 && got[2] == 1 && got[3] == 0);
    assert(got[6] == 512 && got[7] == 512);

    uint32_t gl[] = {0, OUT};
    assert(call(&demo, tex, 18, gl, 2) == 0);
    uint32_t surface = read32(&demo, OUT);
    assert(surface);
    uint32_t bind[] = {0, surface};
    assert(device_call(&demo, 37, bind, 2) == 0);
    uint32_t stage[] = {0, tex};
    assert(device_call(&demo, 65, stage, 2) == 0);

    /* 1024x1024 A32B32G32R32F is accepted the same way. */
    assert(create_rt(&demo, 1024, 1024, 0x72u) == 0);
    uint32_t tex32 = read32(&demo, OUT);
    assert(tex32 && m.targets_created == 2);

    /* Refcounted release frees exactly one target per texture. */
    uint32_t back[] = {0, 0};
    assert(device_call(&demo, 37, back, 2) == 0);
    uint32_t unbind[] = {0, 0};
    assert(device_call(&demo, 65, unbind, 2) == 0);
    assert(call(&demo, surface, 2, NULL, 0) == 0);
    assert(call(&demo, tex, 2, NULL, 0) == 0);
    assert(call(&demo, tex32, 2, NULL, 0) == 0);
    assert(m.targets_deleted == 2);

    d9_com_demo_destroy(&demo);
    assert(!d9_destroy_device_ex(d));
    printf("HDR->LDR render-target fallback (measurement only) PASS\n");
    return 0;
}
