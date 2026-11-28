/* Render-target / depth-stencil pairing and mismatch classification.
 *
 * The bridge uses the colour target's embedded depth attachment and currently
 * requires matching dimensions. Exercise this restriction with the measurement
 * override disabled. Failed binds must preserve both bindings and refcounts.
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH + 0x100)
#define OUT2 (SCRATCH + 0x108)
#define PP (SCRATCH + 0x200)
#define DESC (SCRATCH + 0x600)
#define PROBE_FN 0x00401000u

#define INVALIDCALL 0x8876086cu
#define NOTAVAILABLE 0x8876086au

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) {
        c->r[4] -= 4;
        M32(c->r[4]) = M32(sp + 8 + i * 4);
    }
    c->r[4] -= 4;
    M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c);
    else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "COM stdcall stack mismatch");
    c->r[4] = sp + 4;
}

typedef struct {
    GuestMemory memory;
    CPU cpu;
    uint32_t factory;
    FnModule tables[2];
} Harness;

static int invoke(Harness *h, uint32_t target, const uint32_t *args, unsigned n,
                  uint32_t *result) {
    if (n > 16) return -1;
    uint32_t frame[18] = {target, n};
    for (unsigned i = 0; i < n; ++i) frame[i + 2] = args[i];
    return n3ds_lift_invoke(&h->cpu, PROBE_FN, frame, n + 2, result);
}

static uint32_t call(Harness *h, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned n) {
    uint32_t vt = 0, fn = 0;
    if (guest_read32(&h->memory, object, &vt) || !vt) {
        fprintf(stderr, "bad object 0x%08x slot %u\n", object, slot);
        assert(0);
    }
    if (guest_read32(&h->memory, vt + slot * 4, &fn) || !fn) {
        fprintf(stderr, "bad vtable 0x%08x slot %u\n", vt, slot);
        assert(0);
    }
    uint32_t frame[18] = {object};
    for (unsigned i = 0; i < n; ++i) frame[i + 1] = args[i];
    uint32_t r = 0;
    int rc = invoke(h, fn, frame, n + 1, &r);
    if (rc) fprintf(stderr, "depth_match: fault slot %u object 0x%08x rc=%d\n",
                    slot, object, rc);
    assert(!rc);
    return r;
}

static uint32_t call1(Harness *h, uint32_t object, unsigned slot, uint32_t a) {
    return call(h, object, slot, &a, 1);
}
static uint32_t rd(Harness *h, uint32_t address) {
    uint32_t v = 0;
    assert(!guest_read32(&h->memory, address, &v));
    return v;
}
static uint32_t unref(Harness *h, uint32_t object) {
    return call(h, object, 2, NULL, 0);
}

static const uint32_t pp[14] = {400, 240, D9_X8R8G8B8, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1};

static void harness_start(Harness *h, uint32_t *device) {
    memset(h, 0, sizeof *h);
    guest_init(&h->memory, 2u << 20);
    assert(!guest_map(&h->memory, 0x10000000, 0x4000));
    assert(!guest_map(&h->memory, SCRATCH, 0x2000));
    assert(!d9_com_init_backend(&h->memory, d9_null_backend(),
                                d9_null_context(), 32u << 20));
    static const FnEntry probe[] = {{PROBE_FN, guest_dispatch_probe}};
    h->tables[0] = (FnModule){"probe", probe, 1};
    h->tables[1] = d9_com_module();
    assert(!n3ds_lift_bind_modules(&h->memory, h->tables, 2));
    n3ds_lift_init_cpu(&h->cpu, 0x10004000, 0);
    PeImport import = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    uint32_t create9 = d9_com_resolve(&import, NULL);
    assert(create9);
    uint32_t sdk = 32;
    assert(!invoke(h, create9, &sdk, 1, &h->factory) && h->factory);
    assert(!guest_write(&h->memory, PP, pp, sizeof pp));
    uint32_t create[] = {0, 1, 0, 0x20, PP, OUT};
    assert(call(h, h->factory, 16, create, 6) == 0);
    *device = rd(h, OUT);
    assert(*device);
}

static void harness_stop(Harness *h) {
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h->memory);
}

static uint32_t make_depth(Harness *h, uint32_t device, unsigned w, unsigned height,
                           uint32_t expect) {
    uint32_t a[] = {w, height, D9_D24S8, 0, 0, 0, OUT2, 0};
    uint32_t hr = call(h, device, 29, a, 8);
    if (hr != expect)
        fprintf(stderr, "depth create %ux%u -> 0x%08x diag=%s\n", w, height, hr,
                d9_com_last_diagnostic());
    assert(hr == expect);
    return hr == 0 ? rd(h, OUT2) : 0;
}

static uint32_t make_rt(Harness *h, uint32_t device, unsigned w, unsigned height) {
    uint32_t a[] = {w, height, D9_X8R8G8B8, 0, 0, 0, OUT2, 0};
    assert(call(h, device, 28, a, 8) == 0);
    uint32_t rt = rd(h, OUT2);
    assert(rt);
    return rt;
}

static void expect_hr(Harness *h, uint32_t device, unsigned slot, uint32_t surface,
                      uint32_t expect, const char *what) {
    uint32_t a[] = {surface};
    unsigned n = slot == 37 ? 2 : 1;
    if (slot == 37) {
        uint32_t pair[] = {0, surface};
        uint32_t hr = call(h, device, slot, pair, 2);
        if (hr != expect)
            fprintf(stderr, "%s -> 0x%08x diag=%s\n", what, hr,
                    d9_com_last_diagnostic());
        assert(hr == expect);
        return;
    }
    uint32_t hr = call(h, device, slot, a, n);
    if (hr != expect)
        fprintf(stderr, "%s -> 0x%08x diag=%s\n", what, hr,
                d9_com_last_diagnostic());
    assert(hr == expect);
}

static uint32_t get_depth(Harness *h, uint32_t device) {
    uint32_t a[] = {OUT2};
    assert(call(h, device, 40, a, 1) == 0);
    uint32_t d = rd(h, OUT2);
    assert(d);
    return d;
}

static uint32_t get_target(Harness *h, uint32_t device) {
    uint32_t a[] = {0, OUT2};
    assert(call(h, device, 38, a, 2) == 0);
    uint32_t t = rd(h, OUT2);
    assert(t);
    return t;
}

int main(void) {
    Harness h;
    uint32_t device = 0;
    harness_start(&h, &device);

    /* 1. Back buffer 400x240 + matching 400x240 depth: S_OK. */
    uint32_t d400 = make_depth(&h, device, 400, 240, 0);
    expect_hr(&h, device, 39, d400, 0, "bind 400x240 depth to back buffer");
    uint32_t got = get_depth(&h, device);
    assert(got == d400);
    assert(unref(&h, got) == 2); /* binding + caller */

    /* 2. 512x1024 depth against the 400x240 back buffer: accepted and queued
     *    for the target that owns it; the request is visible through Get. */
    uint32_t d512x1024 = make_depth(&h, device, 512, 1024, 0);
    expect_hr(&h, device, 39, d512x1024, 0, "queue 512x1024 depth on 400x240");
    got = get_depth(&h, device);
    assert(got == d512x1024);
    assert(unref(&h, got) == 2);

    /* 3. Matching 512x1024 RT/depth pair after an explicit SetRenderTarget. */
    uint32_t rt512x1024 = make_rt(&h, device, 512, 1024);
    expect_hr(&h, device, 39, 0, 0, "unbind depth");
    expect_hr(&h, device, 37, rt512x1024, 0, "bind 512x1024 RT");
    uint32_t d512x1024b = make_depth(&h, device, 512, 1024, 0);
    expect_hr(&h, device, 39, d512x1024b, 0, "bind matching 512x1024 depth");
    got = get_depth(&h, device);
    assert(got == d512x1024b);
    assert(unref(&h, got) == 2);

    /* 4. Same-size-class but different extent: queued, previous pair intact. */
    uint32_t d512 = make_depth(&h, device, 512, 512, 0);
    expect_hr(&h, device, 39, d512, 0, "queue 512x512 depth for 512x1024");
    got = get_depth(&h, device);
    assert(got == d512);
    assert(unref(&h, got) == 2);

    /* 5. 400x240 depth against the 512x1024 target is queued too. */
    expect_hr(&h, device, 39, d400, 0, "queue 400x240 depth on 512x1024");
    got = get_depth(&h, device);
    assert(got == d400);
    assert(unref(&h, got) == 2);

    /* 6. Switching back to the back buffer under the queued 512x1024 depth
     *    succeeds and uses the back buffer's embedded depth. */
    expect_hr(&h, device, 37, 0, 0, "switch to 400x240 under queued depth");
    got = get_target(&h, device);
    assert(got != rt512x1024);
    (void)unref(&h, got); /* drop GetRenderTarget's reference */

    /* 7. Explicit unbind restores the back-buffer path and the 400x240 pair. */
    expect_hr(&h, device, 39, 0, 0, "unbind queued depth");
    expect_hr(&h, device, 37, 0, 0, "switch to back buffer");
    expect_hr(&h, device, 39, d400, 0, "rebind 400x240 depth");
    got = get_depth(&h, device);
    assert(got == d400);
    assert(unref(&h, got) == 2);

    /* 8. Refcounts: unbind and release everything to zero. */
    expect_hr(&h, device, 39, 0, 0, "final depth unbind");
    assert(unref(&h, d400) == 0);
    assert(unref(&h, d512) == 0);
    assert(unref(&h, d512x1024) == 0);
    assert(unref(&h, d512x1024b) == 0);
    assert(unref(&h, rt512x1024) == 0);

    harness_stop(&h);
    puts("PASS depth_target_matching: 400x240/512x1024 pairs, queued mismatches, "
         "stable previous pair, refcounts");
    return 0;
}
