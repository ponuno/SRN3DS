/* Cube-face render targets (D3DUSAGE_RENDERTARGET cubes).
 *
 * Host contract:
 *   - an LDR cube face is a named render target exposed by GetCubeMapSurface
 *     and accepted by SetRenderTarget/GetRenderTarget;
 *   - face/level views are lazily created per cube face and level, reused, and
 *     released with the cube (never independently);
 *   - a float/HDR cube target is refused unless the measurement-only LDR
 *     fallback is enabled, in which case only the storage format is downgraded
 *     (recorded by the backend) and the logical format stays visible through
 *     GetLevelDesc;
 *   - a backend without target_create_cube_face/target_delete_cube_face is a
 *     named refusal, never a substituted face 0;
 *   - caps/SM3/T&L stay unchanged.
 */
#include "d3d9_com.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH + 0x200)
#define CAPS (SCRATCH + 0x400)
#define CAPS_VSVER (CAPS + 196)
#define CAPS_PSVER (CAPS + 204)
#define OUT (SCRATCH + 0x500)
#define DESC (SCRATCH + 0x600)
#define VP (SCRATCH + 0x700)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALID 0x8876086cu
#define HR_UNAVAILABLE 0x8876086au

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) { c->r[4] -= 4; M32(c->r[4]) = M32(sp + 8 + i * 4); }
    c->r[4] -= 4; M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c); else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "cube RT stdcall stack mismatch");
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
static uint32_t caps(Harness *h) { assert(call(h, h->device, 7, (uint32_t[]){CAPS}, 1) == HR_OK); return rd(h, CAPS + 28); }

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void start_case(Harness *h, const D9Backend *ops, void *ctx, int ldr) {
    set_env("TESV_D3D9_HDR_LDR_FALLBACK", ldr ? "1" : "0");
    h->factory = h->device = 0;
    d9_null_reset();
    assert(!d9_com_init(&h->memory, NULL));
    assert(d9_com_set_backend(ops, ctx, 64u << 20));
    uint32_t create9 = d9_com_resolve(&(PeImport){.dll = "d3d9.dll", .name = "Direct3DCreate9"}, NULL);
    assert(create9);
    uint32_t sdk = 32;
    assert(!invoke(h, create9, &sdk, 1, &h->factory) && h->factory);
    uint32_t pp[14] = {400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    wr(h, PP, pp, sizeof pp);
    uint32_t create[] = {0,1,0,0x20,PP,OUT};
    assert(call(h, h->factory, 16, create, 6) == HR_OK);
    assert(!guest_read32(&h->memory, OUT, &h->device) && h->device);
    /* Caps/SM3/T&L are not part of this feature. */
    assert(caps(h) == 0x00082000u);
    assert(rd(h, CAPS_VSVER) == 0xFFFE0200u);
    assert(rd(h, CAPS_PSVER) == 0xFFFF0200u);
    assert(call(h, h->device, 89, (uint32_t[]){0x012u}, 1) == HR_UNAVAILABLE);
    assert(call(h, h->device, 89, (uint32_t[]){0x102u}, 1) == HR_OK);
}
static void stop_case(Harness *h) {
    assert(call(h, h->device, 2, NULL, 0) == 0);
    assert(call(h, h->factory, 2, NULL, 0) == 0);
    d9_com_shutdown();
}

static uint32_t create_cube(Harness *h, unsigned edge, unsigned levels, unsigned usage,
                            unsigned format, unsigned pool) {
    uint32_t a[] = {edge, levels, usage, format, pool, OUT, 0};
    uint32_t hr = call(h, h->device, 25, a, 7);
    return hr;
}
static uint32_t get_surface(Harness *h, uint32_t cube, unsigned face, unsigned level) {
    uint32_t a[] = {face, level, OUT};
    assert(call(h, cube, 18, a, 3) == HR_OK);
    uint32_t s = rd(h, OUT); assert(s); return s;
}
static uint32_t level_count(Harness *h, uint32_t cube) { return call(h, cube, 13, NULL, 0); }

static void expect_cube_level(Harness *h, uint32_t cube, unsigned level,
                              unsigned edge, unsigned format) {
    uint32_t a[] = {level, DESC};
    assert(call(h, cube, 17, a, 2) == HR_OK);
    assert(rd(h, DESC) == format);
    assert(rd(h, DESC + 4) == 1u);          /* D3DRTYPE_CUBETEXTURE */
    assert(rd(h, DESC + 8) == 1u);          /* D3DUSAGE_RENDERTARGET is preserved */
    assert(rd(h, DESC + 12) == 0u);         /* D3DPOOL_DEFAULT */
    assert(rd(h, DESC + 24) == edge);
    assert(rd(h, DESC + 28) == edge);
}
static void expect_surface(Harness *h, uint32_t surface, unsigned edge, unsigned format,
                           unsigned usage) {
    uint32_t a[] = {DESC};
    assert(call(h, surface, 12, a, 1) == HR_OK);
    assert(rd(h, DESC) == format);
    assert(rd(h, DESC + 4) == 1u);          /* D3DRTYPE_SURFACE */
    assert(rd(h, DESC + 8) == usage);
    assert(rd(h, DESC + 12) == 0u);
    assert(rd(h, DESC + 24) == edge);
    assert(rd(h, DESC + 28) == edge);
}

static void case_ldr(Harness *h) {
    start_case(h, d9_null_backend(), d9_null_context(), 1);

    assert(create_cube(h, 32, 2, D9_USAGE_RENDERTARGET, 0x71u, D9_POOL_DEFAULT) == HR_OK);
    uint32_t cube = rd(h, OUT); assert(cube);
    assert(level_count(h, cube) == 2u);
    expect_cube_level(h, cube, 0, 32, 0x71u); /* logical HDR format survives */
    expect_cube_level(h, cube, 1, 16, 0x71u);
    /* The recording backend saw the downgraded storage format. */
    assert(d9_null_stats()->last_cube_format == D9_A8R8G8B8);

    uint32_t s[6][2];
    for (unsigned f = 0; f < 6; ++f)
        for (unsigned l = 0; l < 2; ++l) {
            s[f][l] = get_surface(h, cube, f, l);
            expect_surface(h, s[f][l], l ? 16u : 32u, 0x71u, D9_USAGE_RENDERTARGET);
        }
    assert(d9_null_stats()->cube_face_creates == 12u);
    assert(d9_null_stats()->last_cube_face == 5u);
    assert(d9_null_stats()->last_cube_level == 1u);
    assert(d9_null_stats()->last_cube_depth_flags == D9_CUBE_FACE_DEPTH24_STENCIL8);

    /* Same face/level reuses the same target; a new surface view is allowed. */
    uint32_t again = get_surface(h, cube, 0, 0);
    assert(d9_null_stats()->cube_face_creates == 12u);

    /* Out-of-range face/level are named invalid calls. */
    uint32_t bad[] = {6, 0, OUT};
    assert(call(h, cube, 18, bad, 3) == HR_INVALID);
    bad[0] = 0; bad[1] = 2;
    assert(call(h, cube, 18, bad, 3) == HR_INVALID);

    /* Set/GetRenderTarget round-trip and the full-target viewport. */
    assert(call(h, h->device, 37, (uint32_t[]){0, s[0][0]}, 2) == HR_OK);
    assert(call(h, h->device, 38, (uint32_t[]){0, OUT}, 2) == HR_OK);
    uint32_t got = rd(h, OUT); assert(got == s[0][0]);
    assert(call(h, got, 2, NULL, 0) >= 1); /* release the GetRenderTarget reference */
    assert(call(h, h->device, 48, (uint32_t[]){VP}, 1) == HR_OK);
    assert(rd(h, VP + 8) == 32u && rd(h, VP + 12) == 32u);
    assert(rd(h, VP + 16) == 0u && rd(h, VP + 20) == 0x3f800000u);
    assert(call(h, h->device, 47, (uint32_t[]){VP}, 1) == HR_OK);
    wr(h, VP + 8, (uint32_t[]){16u}, 4);
    wr(h, VP + 12, (uint32_t[]){16u}, 4);
    assert(call(h, h->device, 47, (uint32_t[]){VP}, 1) == HR_UNAVAILABLE);

    /* Shared depth policy: the D3D depth surface matches the face size and
     * reaches the backend clear; switching to the 400x240 back buffer without
     * unbinding it is a named refusal. */
    assert(call(h, h->device, 29, (uint32_t[]){32,32,D9_D24S8,0,0,0,OUT,0}, 8) == HR_OK);
    uint32_t depth = rd(h, OUT); assert(depth);
    assert(call(h, h->device, 39, (uint32_t[]){depth}, 1) == HR_OK);
    assert(call(h, h->device, 43, (uint32_t[]){0,0,3,0xff112233u,0x3f800000u,0}, 6) == HR_OK);
    assert(call(h, h->device, 41, NULL, 0) == HR_OK);
    assert(call(h, h->device, 42, NULL, 0) == HR_OK);
    assert(d9_null_stats()->target_binds >= 1u);
    assert(d9_null_stats()->cube_face_binds >= 1u);
    assert(d9_null_stats()->target_clears >= 1u);
    assert(d9_null_stats()->cube_face_clears >= 1u);
    assert(d9_null_stats()->depth_clears >= 1u);
    assert(d9_null_stats()->last_clear_argb == 0xff112233u);

    assert(call(h, h->device, 37, (uint32_t[]){0, s[1][0]}, 2) == HR_OK);
    assert(call(h, h->device, 38, (uint32_t[]){0, OUT}, 2) == HR_OK);
    got = rd(h, OUT); assert(got == s[1][0]);
    assert(call(h, got, 2, NULL, 0) >= 1);
    assert(call(h, h->device, 37, (uint32_t[]){0, 0}, 2) == HR_UNAVAILABLE);
    assert(call(h, h->device, 39, (uint32_t[]){0}, 1) == HR_OK);
    assert(call(h, h->device, 37, (uint32_t[]){0, 0}, 2) == HR_OK);
    /* EndScene leaves the device in phase 2; Present returns it to phase 0 so
     * deferred surface releases destroy their views immediately. */
    assert(call(h, h->device, 17, (uint32_t[]){0,0,0,0}, 4) == HR_OK);

    /* Surface views own no target; releasing them returns the cube to one
     * reference, and the cube releases all twelve face targets. */
    assert(call(h, again, 2, NULL, 0) == 0);
    for (unsigned f = 0; f < 6; ++f)
        for (unsigned l = 0; l < 2; ++l)
            assert(call(h, s[f][l], 2, NULL, 0) == 0);
    assert(call(h, depth, 2, NULL, 0) == 0);
    assert(call(h, cube, 2, NULL, 0) == 0);
    assert(d9_null_stats()->cube_face_deletes == 12u);
    assert(d9_null_stats()->cube_face_deletes == 12u);

    /* A depth surface is not a cube-face render target. */
    assert(call(h, h->device, 29, (uint32_t[]){32,32,D9_D24S8,0,0,0,OUT,0}, 8) == HR_OK);
    depth = rd(h, OUT); assert(depth);
    assert(call(h, h->device, 37, (uint32_t[]){0, depth}, 2) == HR_INVALID);
    assert(call(h, depth, 2, NULL, 0) == 0);

    stop_case(h);
}

static void case_float_off(Harness *h) {
    start_case(h, d9_null_backend(), d9_null_context(), 0);
    assert(create_cube(h, 32, 2, D9_USAGE_RENDERTARGET, 0x71u, D9_POOL_DEFAULT) == HR_UNAVAILABLE);
    assert(rd(h, OUT) == 0u);
    assert(d9_null_stats()->cube_creates == 0u);
    assert(strstr(d9_com_last_diagnostic(), "float/HDR cube render targets") != NULL);

    /* LDR cube targets still work without the fallback. */
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET, D9_A8R8G8B8, D9_POOL_DEFAULT) == HR_OK);
    uint32_t cube = rd(h, OUT); assert(cube);
    uint32_t s = get_surface(h, cube, 0, 0);
    expect_surface(h, s, 16, D9_A8R8G8B8, D9_USAGE_RENDERTARGET);
    assert(call(h, h->device, 37, (uint32_t[]){0, s}, 2) == HR_OK);
    assert(call(h, h->device, 37, (uint32_t[]){0, 0}, 2) == HR_OK);
    assert(call(h, s, 2, NULL, 0) == 0);
    assert(call(h, cube, 2, NULL, 0) == 0);

    /* Formats with no backend representation, a non-default pool and combined
     * usage bits are named refusals before any resource is created. */
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET, D9_DXT1, D9_POOL_DEFAULT) == HR_UNAVAILABLE);
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET, 0x17u, D9_POOL_DEFAULT) == HR_UNAVAILABLE);
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET, D9_A8R8G8B8, D9_POOL_MANAGED) == HR_UNAVAILABLE);
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET | 0x200u, D9_A8R8G8B8, D9_POOL_DEFAULT) == HR_UNAVAILABLE);
    assert(rd(h, OUT) == 0u);
    stop_case(h);
}

static D9Backend no_face_ops;
static void init_no_face_ops(void) {
    no_face_ops = *d9_null_backend();
    no_face_ops.target_create_cube_face = NULL;
    no_face_ops.target_delete_cube_face = NULL;
}

static void case_no_callbacks(Harness *h) {
    start_case(h, &no_face_ops, d9_null_context(), 1);
    assert(create_cube(h, 16, 1, D9_USAGE_RENDERTARGET, D9_A8R8G8B8, D9_POOL_DEFAULT) == HR_OK);
    uint32_t cube = rd(h, OUT); assert(cube);
    uint32_t a[] = {0, 0, OUT};
    assert(call(h, cube, 18, a, 3) == HR_UNAVAILABLE);
    assert(rd(h, OUT) == 0u);
    assert(d9_null_stats()->cube_face_creates == 0u);
    assert(strstr(d9_com_last_diagnostic(), "cube-face render-target callbacks") != NULL);
    assert(call(h, cube, 2, NULL, 0) == 0);
    assert(d9_null_stats()->cube_face_deletes == 0u);
    stop_case(h);
}

static void case_non_rt(Harness *h) {
    start_case(h, d9_null_backend(), d9_null_context(), 1);
    assert(create_cube(h, 32, 1, 0, D9_A8R8G8B8, D9_POOL_DEFAULT) == HR_OK);
    uint32_t cube = rd(h, OUT); assert(cube);
    uint32_t s = get_surface(h, cube, 0, 0);
    uint32_t d[] = {DESC};
    assert(call(h, s, 12, d, 1) == HR_OK);
    assert(rd(h, DESC + 8) == 0u); /* no render-target usage */
    /* A CPU cube view has no target handle and cannot be bound. */
    assert(call(h, h->device, 37, (uint32_t[]){0, s}, 2) == HR_INVALID);
    assert(d9_null_stats()->cube_face_creates == 0u);
    assert(call(h, s, 2, NULL, 0) == 0);
    assert(call(h, cube, 2, NULL, 0) == 0);
    stop_case(h);
}

int main(void) {
    Harness h; memset(&h, 0, sizeof h);
    guest_init(&h.memory, 4u << 20);
    assert(!guest_map(&h.memory, 0x10000000, 0x4000) && !guest_map(&h.memory, SCRATCH, 0x2000));
    static const FnEntry probe_fn[] = {{PROBE_FN, guest_dispatch_probe}};
    FnModule modules[2] = {{"probe", probe_fn, 1}, d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory, modules, 2));
    n3ds_lift_init_cpu(&h.cpu, 0x10004000, 0);

    init_no_face_ops();
    case_ldr(&h);
    case_float_off(&h);
    case_no_callbacks(&h);
    case_non_rt(&h);

    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("cube RT smoke: face targets, LDR downgrade, visible RGB/float refusals, no-callback refusal; caps unchanged");
    return 0;
}
