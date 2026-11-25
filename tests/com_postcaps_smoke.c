/* Host regression for the post-caps IDirect3DDevice9 state methods that TESV
 * reaches: GetViewport (slot 48), Set/GetScissorRect (75/76),
 * Set/GetGammaRamp (21/22) and Set/GetNPatchMode (79/80).
 *
 * Runs against the real COM bridge and the demo device, through the same
 * lifted indirect-call path the guest uses.  NPatch is the only method here
 * that returns a float: the guest pops it from ST(0), not EAX, so the check
 * reads the emulated x87 stack directly. */
#include "d3d9_com_demo.h"
#include "d3d9_com.h"
#include "d3d9_core.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 0x10010600u /* inside the demo's mapped SCRATCH page */
#define OUT (BUF + 0x700u) /* outside the 3 x 256 WORD gamma ramp at BUF */
#define ERR_INVALID 0x8876086cu

/* Minimal backend: device creation, texture upload, the demo's single indexed
 * draw, and one mock render-target surface. */
typedef struct {
    unsigned created, uploaded, deleted, begin, draw, end;
    size_t n;
    int active;
    D9Matrix clip;
    D9DrawState state;
    D9Vertex vertices[36];
    unsigned targets_created, targets_deleted, targets_bound;
} Mock;
typedef struct { size_t base; unsigned levels; uint8_t *data; } Tex;

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels, void **native) {
    Mock *m = ctx;
    Tex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->base = (size_t)w * h * 4;
    t->levels = levels;
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
static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *mat, void *t, const D9DrawState *state) {
    Mock *s = ctx;
    assert(s->active && v && mat && t && n <= 36);
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
    *native = (void *)(uintptr_t)(++m->targets_created);
    return 0;
}
static void target_delete(void *ctx, void *native) {
    Mock *m = ctx;
    (void)native;
    ++m->targets_deleted;
}
static int target_bind(void *ctx, void *native) {
    Mock *m = ctx;
    (void)native;
    ++m->targets_bound;
    return 0;
}
static int target_clear(void *ctx, void *native, uint32_t argb) {
    (void)ctx; (void)native; (void)argb;
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
};

static uint32_t call(D9ComDemo *s, unsigned slot, const uint32_t *a, unsigned n) {
    uint32_t r = 0;
    assert(!d9_com_demo_call(s, s->device, slot, a, n, &r));
    return r;
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t v = 0;
    assert(!guest_read32(&s->memory, address, &v));
    return v;
}
static void read_rect(D9ComDemo *s, uint32_t address, uint32_t out[4]) {
    assert(!guest_read(&s->memory, address, out, 4 * sizeof *out));
}
static void write_rect(D9ComDemo *s, uint32_t address, const uint32_t in[4]) {
    assert(!guest_write(&s->memory, address, in, 4 * sizeof *in));
}
static const uint32_t FULL_400x240[4] = {0, 0, 400, 240};

/* Float-return path: one value is pushed on the emulated x87 stack. */
static double get_npatch(D9ComDemo *s) {
    uint32_t before = s->cpu.top, result = 0xdeadbeefu;
    assert(!d9_com_demo_call(s, s->device, 80, NULL, 0, &result));
    assert(result == 0);
    assert(s->cpu.top == ((before + 7u) & 7u));
    return s->cpu.st[s->cpu.top];
}

int main(void) {
    Mock m = {0};
    D9Device *d = d9_create_device(&ops, &m, 4u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }

    /* 1. GetViewport is slot 48 and returns the default full-target viewport.
     *    Slot 46 (MultiplyTransform) must still be a different thunk. */
    memset(guest_ptr(&demo.memory, BUF, 24), 0xcd, 24);
    uint32_t vp[] = {BUF};
    assert(call(&demo, 48, vp, 1) == 0);
    uint32_t got[6];
    assert(!guest_read(&demo.memory, BUF, got, sizeof got));
    assert(got[0] == 0 && got[1] == 0 && got[2] == 400 && got[3] == 240);
    assert(got[4] == 0 && got[5] == 0x3f800000u);
    uint32_t vt = read32(&demo, demo.device);
    uint32_t fn46 = read32(&demo, vt + 46 * 4), fn48 = read32(&demo, vt + 48 * 4);
    assert(fn46 && fn48 && fn46 != fn48);
    printf("slot48 viewport=%ux%u fn[46]=0x%08x fn[48]=0x%08x\n", got[2], got[3], fn46, fn48);

    /* 2. Fresh device reports the full-target scissor without a prior set. */
    uint32_t sp[] = {BUF};
    assert(call(&demo, 76, sp, 1) == 0);
    uint32_t rect[4];
    read_rect(&demo, BUF, rect);
    assert(!memcmp(rect, FULL_400x240, sizeof rect));

    /* 3. Valid scissor round-trip, then bounds/order/NULL rejection. */
    const uint32_t partial[4] = {10, 20, 100, 120};
    write_rect(&demo, BUF, partial);
    assert(call(&demo, 75, sp, 1) == 0);
    memset(guest_ptr(&demo.memory, BUF, 16), 0, 16);
    assert(call(&demo, 76, sp, 1) == 0);
    read_rect(&demo, BUF, rect);
    assert(!memcmp(rect, partial, sizeof rect));

    const uint32_t bad[][4] = {
        {100, 0, 50, 240},   /* left >= right */
        {0, 100, 400, 50},   /* top >= bottom */
        {0, 0, 401, 240},    /* right > width */
        {0, 0, 400, 241},    /* bottom > height */
        {0xffffffffu, 0, 400, 240}, /* negative left */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        write_rect(&demo, BUF, bad[i]);
        assert(call(&demo, 75, sp, 1) == ERR_INVALID);
    }
    uint32_t nullp = 0;
    assert(call(&demo, 75, &nullp, 1) == ERR_INVALID);
    assert(call(&demo, 76, &nullp, 1) == ERR_INVALID);
    assert(call(&demo, 76, sp, 1) == 0);
    read_rect(&demo, BUF, rect); /* rejected calls keep the last valid rect */
    assert(!memcmp(rect, partial, sizeof rect));

    /* 4. Selecting another render target resets the scissor to that target,
     *    and selecting the back buffer resets it to 400x240. */
    uint32_t crt[] = {64, 64, D9_X8R8G8B8, 0, 0, 0, OUT, 0};
    assert(call(&demo, 28, crt, 8) == 0);
    uint32_t rt = read32(&demo, OUT);
    assert(rt && m.targets_created == 1);
    uint32_t bind[] = {0, rt};
    assert(call(&demo, 37, bind, 2) == 0);
    assert(call(&demo, 76, sp, 1) == 0);
    read_rect(&demo, BUF, rect);
    assert(rect[0] == 0 && rect[1] == 0 && rect[2] == 64 && rect[3] == 64);
    const uint32_t too_wide[4] = {0, 0, 400, 64}; /* valid on the back buffer only */
    write_rect(&demo, BUF, too_wide);
    assert(call(&demo, 75, sp, 1) == ERR_INVALID);
    const uint32_t half[4] = {0, 0, 32, 32};
    write_rect(&demo, BUF, half);
    assert(call(&demo, 75, sp, 1) == 0);
    bind[1] = 0;
    assert(call(&demo, 37, bind, 2) == 0); /* implicit back buffer */
    assert(call(&demo, 76, sp, 1) == 0);
    read_rect(&demo, BUF, rect);
    assert(!memcmp(rect, FULL_400x240, sizeof rect));
    uint32_t refs = 0xdeadbeefu;
    assert(!d9_com_demo_call(&demo, rt, 2, NULL, 0, &refs)); /* Release the render target */
    assert(refs == 0);
    assert(m.targets_deleted == 1);

    /* 5. Gamma ramp: identity after CreateDevice, round-trip, validation. */
    assert(call(&demo, 22, (uint32_t[]){0, BUF}, 2) == 0);
    uint16_t ramp[3][256];
    assert(!guest_read(&demo.memory, BUF, ramp, sizeof ramp));
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned i = 0; i < 256; ++i) assert(ramp[c][i] == i * 0x0101u);

    uint16_t mine[3][256];
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned i = 0; i < 256; ++i) mine[c][i] = (uint16_t)(0xffffu - i * 17u - c);
    assert(!guest_write(&demo.memory, BUF, mine, sizeof mine));
    assert(call(&demo, 21, (uint32_t[]){0, 0, BUF}, 3) == 0);
    memset(ramp, 0, sizeof ramp);
    assert(call(&demo, 22, (uint32_t[]){0, BUF}, 2) == 0);
    assert(!guest_read(&demo.memory, BUF, ramp, sizeof ramp));
    assert(!memcmp(ramp, mine, sizeof ramp));
    assert(call(&demo, 21, (uint32_t[]){0, 2, BUF}, 3) == ERR_INVALID); /* bad flags */
    assert(call(&demo, 21, (uint32_t[]){1, 0, BUF}, 3) == ERR_INVALID); /* swap chain 1 */
    assert(call(&demo, 21, (uint32_t[]){0, 0, 0}, 3) == ERR_INVALID);   /* NULL ramp */
    assert(call(&demo, 22, (uint32_t[]){0, 0}, 2) == ERR_INVALID);      /* NULL ramp */
    assert(call(&demo, 22, (uint32_t[]){1, BUF}, 2) == ERR_INVALID);    /* swap chain 1 */

    /* 6. NPatch mode: default 0.0, [0,64] accepted, invalid input rejected.
     *    GetNPatchMode returns its float in ST(0). */
    assert(get_npatch(&demo) == 0.0);
    uint32_t bits;
    float f = 3.5f;
    memcpy(&bits, &f, 4);
    assert(call(&demo, 79, &bits, 1) == 0);
    assert(get_npatch(&demo) == 3.5);
    f = 0.0f;
    memcpy(&bits, &f, 4);
    assert(call(&demo, 79, &bits, 1) == 0);
    assert(get_npatch(&demo) == 0.0);
    const float rejected[] = {-1.0f, 64.5f, 65.0f, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
        memcpy(&bits, &rejected[i], 4);
        assert(call(&demo, 79, &bits, 1) == ERR_INVALID);
    }
    assert(get_npatch(&demo) == 0.0); /* rejected calls leave the state alone */

    /* 7. The normal first-frame path still works after the new state calls. */
    assert(!d9_com_demo_frame(&demo, 0.0f));
    assert(!d9_com_demo_frame(&demo, 0.7f));
    assert(m.draw == 2 && m.end == 2);

    assert(!d9_com_demo_destroy(&demo));
    d9_destroy_device(d);
    puts("post-caps COM state: GetViewport, scissor, gamma ramp and NPatch passed");
    return 0;
}
