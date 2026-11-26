/* IDirect3DDevice9::Reset (slot 16).
 *
 * A successful Reset keeps the same presentation mode, releases DEFAULT-pool
 * GPU resources and explicit targets/depth, preserves MANAGED CPU stores for
 * rehydration, restores the D3D default state, keeps COM refcounts and leaves
 * the device usable for BeginScene/EndScene/Present.  Invalid presentation
 * parameters fail before any state change.
 */
#include "d3d9_com.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH + 0x200)
#define OUT (SCRATCH + 0x400)
#define OUT2 (SCRATCH + 0x500)
#define MAT (SCRATCH + 0x600)
#define LOCKRECT (SCRATCH + 0x700)
#define RECT (SCRATCH + 0x800)
#define VIEW (SCRATCH + 0x900)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALID 0x8876086cu
#define HR_UNAVAILABLE 0x8876086au
#define HR_NOTFOUND 0x88760866u

#define D3DRS_ZENABLE 7u
#define D3DSAMP_ADDRESSU 1u
#define D3DTSS_COLOROP 1u
#define D3DTOP_SELECTARG1 2u

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) { c->r[4] -= 4; M32(c->r[4]) = M32(sp + 8 + i * 4); }
    c->r[4] -= 4; M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c); else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "Reset stdcall stack mismatch");
    c->r[4] = sp + 4;
}
typedef struct { GuestMemory memory; CPU cpu; uint32_t factory, device; } Harness;

static int invoke(Harness *h, uint32_t target, const uint32_t *args, unsigned n, uint32_t *result) {
    if (n > 16) return -1;
    uint32_t frame[18] = {target, n};
    for (unsigned i = 0; i < n; ++i) frame[i + 2] = args[i];
    return n3ds_lift_invoke(&h->cpu, PROBE_FN, frame, n + 2, result);
}
static uint32_t call(Harness *h, uint32_t object, unsigned slot, const uint32_t *args, unsigned n) {
    uint32_t vt = 0, fn = 0;
    assert(!guest_read32(&h->memory, object, &vt) && vt);
    assert(!guest_read32(&h->memory, vt + slot * 4, &fn) && fn);
    uint32_t frame[18] = {object};
    for (unsigned i = 0; i < n; ++i) frame[i + 1] = args[i];
    uint32_t r = 0; assert(!invoke(h, fn, frame, n + 1, &r)); return r;
}
static uint32_t rd(Harness *h, uint32_t a) { uint32_t v = 0; assert(!guest_read32(&h->memory, a, &v)); return v; }
static void wr(Harness *h, uint32_t a, const void *p, size_t n) { assert(!guest_write(&h->memory, a, p, n)); }
static float rdf(Harness *h, uint32_t a) { float f = 0; assert(!guest_read(&h->memory, a, &f, sizeof f)); return f; }

static void write_pp_flags(Harness *h, unsigned w, unsigned format, unsigned swap, unsigned flags) {
    uint32_t pp[14] = {w,240,format,1,0,0,swap,0,1,0,0,flags,0,1};
    wr(h, PP, pp, sizeof pp);
}
static void write_pp(Harness *h, unsigned w, unsigned format, unsigned swap) {
    write_pp_flags(h, w, format, swap, 0);
}
static uint32_t reset(Harness *h) { return call(h, h->device, 16, (uint32_t[]){PP}, 1); }
static uint32_t get_texture(Harness *h, unsigned stage) {
    assert(call(h, h->device, 64, (uint32_t[]){stage,OUT}, 2) == HR_OK);
    return rd(h, OUT);
}
static uint32_t get_stream(Harness *h, unsigned stream) {
    assert(call(h, h->device, 101, (uint32_t[]){stream,OUT,OUT2,OUT2+4}, 4) == HR_OK);
    return rd(h, OUT);
}
static uint32_t get_indices(Harness *h) {
    assert(call(h, h->device, 105, (uint32_t[]){OUT}, 1) == HR_OK);
    return rd(h, OUT);
}
static uint32_t get_decl(Harness *h) {
    assert(call(h, h->device, 88, (uint32_t[]){OUT}, 1) == HR_OK);
    return rd(h, OUT);
}
static uint32_t get_fvf(Harness *h) {
    assert(call(h, h->device, 90, (uint32_t[]){OUT}, 1) == HR_OK);
    return rd(h, OUT);
}

int main(void) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TNL_PROTO", "1");
#else
    setenv("TESV_D3D9_TNL_PROTO", "1", 1);
#endif
    Harness h; memset(&h, 0, sizeof h);
    guest_init(&h.memory, 4u << 20);
    assert(!guest_map(&h.memory, 0x10000000, 0x4000) && !guest_map(&h.memory, SCRATCH, 0x2000));
    assert(!d9_com_init(&h.memory, NULL));
    assert(!d9_null_register());
    static const FnEntry probe[] = {{PROBE_FN, guest_dispatch_probe}};
    FnModule modules[2] = {{"probe", probe, 1}, d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory, modules, 2));
    n3ds_lift_init_cpu(&h.cpu, 0x10004000, 0);

    uint32_t create9 = d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"}, NULL);
    assert(create9);
    uint32_t sdk = 32;
    assert(!invoke(&h, create9, &sdk, 1, &h.factory) && h.factory);
    write_pp(&h, 400, D9_X8R8G8B8, 1);
    uint32_t create[] = {0,1,0,0x20,PP,OUT};
    assert(call(&h, h.factory, 16, create, 6) == HR_OK);
    assert(!guest_read32(&h.memory, OUT, &h.device) && h.device);

    /* MANAGED texture with a known pixel. */
    uint32_t tex = 0;
    assert(call(&h, h.device, 23, (uint32_t[]){8,8,1,0,D9_A8R8G8B8,D9_POOL_MANAGED,OUT,0}, 8) == HR_OK);
    tex = rd(&h, OUT); assert(tex);
    assert(call(&h, tex, 19, (uint32_t[]){0,LOCKRECT,0,0}, 4) == HR_OK);
    uint32_t bits = rd(&h, LOCKRECT + 4), tpitch = rd(&h, LOCKRECT);
    assert(bits && tpitch >= 32);
    for (unsigned i = 0; i < 8 * 8; ++i) assert(!guest_write32(&h.memory, bits + i * 4, 0xff123456u));
    assert(call(&h, tex, 20, (uint32_t[]){0}, 1) == HR_OK);

    /* DEFAULT render target, bound so Reset must release it. */
    uint32_t rt = 0;
    assert(call(&h, h.device, 28, (uint32_t[]){8,8,D9_A8R8G8B8,0,0,0,OUT,0}, 8) == HR_OK);
    rt = rd(&h, OUT); assert(rt);
    unsigned target_creates = d9_null_stats()->target_creates;
    assert(call(&h, h.device, 37, (uint32_t[]){0,rt}, 2) == HR_OK);

    /* Non-default state that Reset must clear. */
    assert(call(&h, h.device, 65, (uint32_t[]){0,tex}, 2) == HR_OK);
    assert(call(&h, h.device, 89, (uint32_t[]){0x102u}, 1) == HR_OK);
    assert(call(&h, h.device, 57, (uint32_t[]){D3DRS_ZENABLE,0}, 2) == HR_OK);
    assert(call(&h, h.device, 69, (uint32_t[]){0,D3DSAMP_ADDRESSU,2}, 3) == HR_OK);
    assert(call(&h, h.device, 67, (uint32_t[]){0,D3DTSS_COLOROP,4}, 3) == HR_OK);
    float world[16]; for (unsigned i = 0; i < 16; ++i) world[i] = (i % 5 == 0) ? 2.0f : 0.0f;
    wr(&h, MAT, world, sizeof world);
    assert(call(&h, h.device, 44, (uint32_t[]){256,MAT}, 2) == HR_OK);
    uint32_t rect[4] = {2,2,4,4}; wr(&h, RECT, rect, sizeof rect);
    assert(call(&h, h.device, 75, (uint32_t[]){RECT}, 1) == HR_OK);

    uint32_t vb = 0, ib = 0;
    assert(call(&h, h.device, 26, (uint32_t[]){64,0,0x102,1,OUT,0}, 6) == HR_OK);
    vb = rd(&h, OUT); assert(vb);
    assert(call(&h, h.device, 27, (uint32_t[]){12,0,D9_INDEX16,1,OUT,0}, 6) == HR_OK);
    ib = rd(&h, OUT); assert(ib);
    assert(call(&h, h.device, 100, (uint32_t[]){0,vb,0,12}, 4) == HR_OK);
    assert(call(&h, h.device, 104, (uint32_t[]){ib}, 1) == HR_OK);

    uint32_t vbref = get_texture(&h, 0);
    assert(vbref == tex);
    assert(call(&h, vbref, 2, NULL, 0) >= 1); /* drop GetTexture's extra ref */

    /* Invalid presentation parameters: named refusal, no state change. */
    uint32_t before_mem = call(&h, h.device, 4, NULL, 0);
    write_pp(&h, 320, D9_X8R8G8B8, 1);
    assert(reset(&h) == HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "400x240") != NULL);
    assert(get_fvf(&h) == 0x102u); /* FVF unchanged */
    uint32_t tget = get_texture(&h, 0);
    assert(tget == tex);
    assert(call(&h, tget, 2, NULL, 0) >= 1);
    assert(call(&h, h.device, 4, NULL, 0) == before_mem);
    write_pp(&h, 400, D9_X8R8G8B8, 2); /* non-DISCARD swap effect */
    assert(reset(&h) == HR_UNAVAILABLE);
    write_pp(&h, 400, D9_X8R8G8B8, 1);

    /* A locked resource refuses Reset before touching state. */
    assert(call(&h, tex, 19, (uint32_t[]){0,LOCKRECT,0,0}, 4) == HR_OK);
    assert(reset(&h) == HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "locked") != NULL);
    assert(get_fvf(&h) == 0x102u);
    assert(call(&h, h.device, 4, NULL, 0) == before_mem);
    assert(call(&h, tex, 20, (uint32_t[]){0}, 1) == HR_OK);

    /* Successful Reset. */
    unsigned tex_creates = d9_null_stats()->texture_creates;
    unsigned tex_uploads = d9_null_stats()->texture_uploads;
    assert(reset(&h) == HR_OK);
    assert(d9_null_stats()->target_creates == target_creates);
    assert(d9_null_stats()->target_deletes > 0); /* DEFAULT RT released */
    assert(get_fvf(&h) == 0); /* FVF default */
    assert(call(&h, h.device, 88, (uint32_t[]){OUT}, 1) == HR_OK && rd(&h, OUT) == 0);
    assert(get_texture(&h, 0) == 0);
    assert(get_stream(&h, 0) == 0);
    assert(get_indices(&h) == 0);
    assert(call(&h, h.device, 58, (uint32_t[]){D3DRS_ZENABLE,OUT}, 2) == HR_OK && rd(&h, OUT) == 1);
    assert(call(&h, h.device, 68, (uint32_t[]){0,D3DSAMP_ADDRESSU,OUT}, 3) == HR_OK && rd(&h, OUT) == 1);
    assert(call(&h, h.device, 66, (uint32_t[]){0,D3DTSS_COLOROP,OUT}, 3) == HR_OK && rd(&h, OUT) == D3DTOP_SELECTARG1);
    assert(call(&h, h.device, 76, (uint32_t[]){RECT}, 1) == HR_OK);
    assert(rd(&h, RECT) == 0 && rd(&h, RECT + 4) == 0 && rd(&h, RECT + 8) == 400 && rd(&h, RECT + 12) == 240);
    assert(call(&h, h.device, 48, (uint32_t[]){VIEW}, 1) == HR_OK);
    assert(rd(&h, VIEW + 8) == 400 && rd(&h, VIEW + 12) == 240);
    assert(rdf(&h, VIEW + 16) == 0.0f && rdf(&h, VIEW + 20) == 1.0f);
    assert(call(&h, h.device, 45, (uint32_t[]){256,MAT}, 2) == HR_OK);
    assert(rdf(&h, MAT) == 1.0f && rdf(&h, MAT + 4) == 0.0f && rdf(&h, MAT + 20) == 1.0f);
    assert(call(&h, h.device, 40, (uint32_t[]){OUT}, 1) == HR_NOTFOUND);
    assert(call(&h, h.device, 38, (uint32_t[]){0,OUT}, 2) == HR_OK);
    uint32_t current_rt = rd(&h, OUT);
    assert(call(&h, h.device, 18, (uint32_t[]){0,0,0,OUT2}, 4) == HR_OK);
    assert(rd(&h, OUT2) == current_rt);
    /* The release DEFAULT target no longer binds. */
    assert(call(&h, h.device, 37, (uint32_t[]){0,rt}, 2) == HR_INVALID);
    assert(call(&h, rt, 2, NULL, 0) == 0);

    /* MANAGED CPU store survived and rehydrates on SetTexture/Lock. */
    assert(call(&h, h.device, 65, (uint32_t[]){0,tex}, 2) == HR_OK);
    assert(d9_null_stats()->texture_creates >= tex_creates + 1);
    assert(d9_null_stats()->texture_uploads >= tex_uploads + 1);
    assert(call(&h, tex, 19, (uint32_t[]){0,LOCKRECT,0,0}, 4) == HR_OK);
    bits = rd(&h, LOCKRECT + 4);
    assert(rd(&h, bits) == 0xff123456u);
    assert(call(&h, tex, 20, (uint32_t[]){0}, 1) == HR_OK);
    assert(call(&h, h.device, 65, (uint32_t[]){0,0}, 2) == HR_OK);

    /* Repeated Reset and Present after Reset. */
    assert(reset(&h) == HR_OK);
    assert(call(&h, h.device, 41, NULL, 0) == HR_OK);
    assert(call(&h, h.device, 42, NULL, 0) == HR_OK);
    assert(call(&h, h.device, 17, (uint32_t[]){0,0,0,0}, 4) == HR_OK);
    assert(get_fvf(&h) == 0);

    /* A supported present-flag change recreates the retained back buffer. */
    unsigned bb_creates = d9_null_stats()->target_creates;
    write_pp_flags(&h, 400, D9_X8R8G8B8, 1, 1);
    assert(reset(&h) == HR_OK);
    assert(d9_null_stats()->target_creates > bb_creates);
    assert(call(&h, h.device, 18, (uint32_t[]){0,0,0,OUT2}, 4) == HR_OK);
    uint32_t lb = rd(&h, OUT2);
    assert(lb);
    assert(call(&h, lb, 13, (uint32_t[]){LOCKRECT,0,0}, 3) == HR_OK);
    assert(rd(&h, LOCKRECT + 4));
    assert(call(&h, lb, 14, NULL, 0) == HR_OK);
    assert(call(&h, lb, 2, NULL, 0) >= 1);
    write_pp(&h, 400, D9_X8R8G8B8, 1);
    assert(reset(&h) == HR_OK);

    /* Refcounts: the device and MANAGED texture survived Reset. */
    assert(call(&h, h.device, 1, NULL, 0) >= 2);
    assert(call(&h, h.device, 2, NULL, 0) >= 1);
    assert(call(&h, tex, 2, NULL, 0) == 0);
    assert(call(&h, vb, 2, NULL, 0) == 0);
    assert(call(&h, ib, 2, NULL, 0) == 0);
    assert(call(&h, h.device, 2, NULL, 0) == 0);
    assert(call(&h, h.factory, 2, NULL, 0) == 0);
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("device reset: state defaults, MANAGED rehydrate, DEFAULT RT released, invalid pp no-op, repeated Reset/Present");
    return 0;
}
