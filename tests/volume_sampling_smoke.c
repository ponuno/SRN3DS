/* Volume-texture sampling through the optional draw_volume backend callback.
 *
 * The null backend really point-samples the core's CPU slice store, so this
 * test proves (u,v,w) addressing: a 4x4x4 volume with a distinct ARGB colour
 * per z slice returns the expected texel for each w.  It also proves the named
 * refusals for 2D texcoords, incomplete volumes/mips, stage 1, a pixel shader
 * and a backend without draw_volume (the PICA shape).  DevCaps stay
 * 0x00082000.  No hardware or frame claim is made. */
#include "d3d9_com.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH + 0x200)
#define CAPS (SCRATCH + 0x400)
#define OUT (SCRATCH + 0x500)
#define LOCKED (SCRATCH + 0x600)
#define DECLVA (SCRATCH + 0x700)
#define VERTS (SCRATCH + 0x1000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALID 0x8876086cu
#define HR_UNAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u

typedef struct {float x,y,z,u,v,w;} VolumeVertex;
typedef struct {uint32_t pitch,slice_pitch,bits;} LockedBox;

static const uint32_t slice_argb[4] = {
    0xffff0000u, /* z=0 red */
    0xff00ff00u, /* z=1 green */
    0xff0000ffu, /* z=2 blue */
    0xffffff00u, /* z=3 yellow */
};

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) { c->r[4] -= 4; M32(c->r[4]) = M32(sp + 8 + i * 4); }
    c->r[4] -= 4; M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c); else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "volume sampling stdcall stack mismatch");
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
static uint32_t device_caps(Harness *h) {
    assert(call(h, h->device, 7, (uint32_t[]){CAPS}, 1) == HR_OK);
    return rd(h, CAPS + 28);
}
static uint32_t create_volume(Harness *h, unsigned levels) {
    uint32_t args[] = {4,4,4,levels,0,FMT_A8R8G8B8,D9_POOL_MANAGED,OUT,0};
    assert(!guest_write32(&h->memory, OUT, 0));
    assert(call(h, h->device, 24, args, 9) == HR_OK);
    uint32_t v = rd(h, OUT); assert(v); return v;
}
/* Fill every voxel of level 0 with its slice colour and upload it. */
static void fill_volume(Harness *h, uint32_t volume) {
    uint32_t args[] = {0,LOCKED,0,0};
    assert(call(h, volume, 19, args, 4) == HR_OK);
    LockedBox box;
    box.pitch = rd(h, LOCKED); box.slice_pitch = rd(h, LOCKED + 4); box.bits = rd(h, LOCKED + 8);
    assert(box.pitch == 16 && box.slice_pitch == 64 && box.bits);
    for (unsigned z = 0; z < 4; ++z)
        for (unsigned i = 0; i < 16; ++i)
            assert(!guest_write32(&h->memory, box.bits + z * box.slice_pitch + i * 4, slice_argb[z]));
    assert(call(h, volume, 20, (uint32_t[]){0}, 1) == HR_OK);
}
static uint32_t setup_decl(Harness *h) {
    D9VertexElement e[3] = {
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT3,0,D9_USAGE_TEXCOORD,0},
        {0xff,0,D9_DECL_UNUSED,0,0,0},
    };
    wr(h, DECLVA, e, sizeof e);
    uint32_t args[] = {DECLVA,OUT};
    assert(!guest_write32(&h->memory, OUT, 0));
    assert(call(h, h->device, 86, args, 2) == HR_OK);
    uint32_t decl = rd(h, OUT); assert(decl);
    assert(call(h, h->device, 87, (uint32_t[]){decl}, 1) == HR_OK);
    return decl;
}
static void draw_w(Harness *h, float w) {
    VolumeVertex v[3] = {
        {-1,-1,0,0.125f,0.125f,w},
        { 1,-1,0,0.125f,0.125f,w},
        { 0, 1,0,0.125f,0.125f,w},
    };
    wr(h, VERTS, v, sizeof v);
    uint32_t args[] = {D9_TRIANGLELIST,1,VERTS,sizeof v[0]};
    assert(call(h, h->device, 83, args, 4) == HR_OK);
}
static void expect_w(Harness *h, float w, uint32_t argb) {
    draw_w(h, w);
    const D9NullStats *s = d9_null_stats();
    assert(s->last_volume_count == 3);
    for (unsigned i = 0; i < 3; ++i) {
        assert(s->last_volume_w[i] == w);
        assert(s->last_volume_samples[i] == argb);
    }
}

int main(void) {
    Harness h; memset(&h, 0, sizeof h);
    guest_init(&h.memory, 4u << 20);
    assert(!guest_map(&h.memory, 0x10000000, 0x4000) && !guest_map(&h.memory, SCRATCH, 0x4000));
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
    uint32_t pp[14] = {400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    wr(&h, PP, pp, sizeof pp);
    uint32_t create[] = {0,1,0,0x20,PP,OUT};
    assert(call(&h, h.factory, 16, create, 6) == HR_OK);
    assert(!guest_read32(&h.memory, OUT, &h.device) && h.device);
    assert(device_caps(&h) == 0x00082000u);
    d9_null_reset();

    /* Bind before upload is a named refusal. */
    uint32_t empty = create_volume(&h, 1);
    assert(call(&h, h.device, 65, (uint32_t[]){0,empty}, 2) == HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "has not been uploaded") != NULL);

    /* Complete 4x4x4 volume, one colour per z slice. */
    uint32_t volume = create_volume(&h, 1);
    fill_volume(&h, volume);
    /* A second volume with a two-level chain but only level 0 uploaded. */
    uint32_t partial = create_volume(&h, 2);
    fill_volume(&h, partial);
    assert(call(&h, h.device, 65, (uint32_t[]){0,volume}, 2) == HR_OK);
    uint32_t decl = setup_decl(&h);
    assert(call(&h, h.device, 41, NULL, 0) == HR_OK);

    expect_w(&h, 0.125f, slice_argb[0]);
    expect_w(&h, 0.375f, slice_argb[1]);
    expect_w(&h, 0.625f, slice_argb[2]);
    expect_w(&h, 0.875f, slice_argb[3]);

    /* 2D texcoords are a named refusal, never a slice-0 fallback. */
    assert(call(&h, h.device, 87, (uint32_t[]){0}, 1) == HR_OK);
    assert(call(&h, h.device, 89, (uint32_t[]){0x102u}, 1) == HR_OK);
    float flat[3][5] = {{-1,-1,0,0.125f,0.125f},{1,-1,0,0.125f,0.125f},{0,1,0,0.125f,0.125f}};
    wr(&h, VERTS, flat, sizeof flat);
    unsigned draws = d9_null_stats()->volume_draws;
    assert(call(&h, h.device, 83, (uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof flat[0]}, 4) == HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "3-component") != NULL);
    assert(d9_null_stats()->volume_draws == draws);
    assert(call(&h, h.device, 87, (uint32_t[]){decl}, 1) == HR_OK);
    expect_w(&h, 0.625f, slice_argb[2]);

    /* Incomplete mip chain: mip sampling is named, point level 0 still works. */
    assert(call(&h, h.device, 65, (uint32_t[]){0,partial}, 2) == HR_OK);
    assert(call(&h, h.device, 69, (uint32_t[]){0,7,D9_FILTER_LINEAR}, 3) == HR_OK);
    draws = d9_null_stats()->volume_draws;
    assert(call(&h, h.device, 83, (uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof(VolumeVertex)}, 4) == HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "mip levels") != NULL);
    assert(d9_null_stats()->volume_draws == draws);
    assert(call(&h, h.device, 69, (uint32_t[]){0,7,0}, 3) == HR_OK);
    expect_w(&h, 0.375f, slice_argb[1]);
    assert(call(&h, h.device, 65, (uint32_t[]){0,volume}, 2) == HR_OK);

    /* A volume bound to stage 1 is a named refusal. */
    assert(call(&h, h.device, 65, (uint32_t[]){0,0}, 2) == HR_OK);
    assert(call(&h, h.device, 65, (uint32_t[]){1,volume}, 2) == HR_OK);
    assert(call(&h, h.device, 83, (uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof(VolumeVertex)}, 4) == HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "stage 1") != NULL);
    assert(call(&h, h.device, 65, (uint32_t[]){1,0}, 2) == HR_OK);

    assert(call(&h, h.device, 42, NULL, 0) == HR_OK);
    assert(call(&h, h.device, 17, (uint32_t[]){0,0,0,0}, 4) == HR_OK);

    /* Caps/SM3 are unchanged by volume sampling. */
    assert(device_caps(&h) == 0x00082000u);
    assert((device_caps(&h) & 0x00018000u) == 0);

    assert(call(&h, h.device, 65, (uint32_t[]){0,0}, 2) == HR_OK);
    assert(call(&h, h.device, 87, (uint32_t[]){0}, 1) == HR_OK);
    assert(call(&h, decl, 2, NULL, 0) == 0);
    assert(call(&h, partial, 2, NULL, 0) == 0);
    assert(call(&h, empty, 2, NULL, 0) == 0);
    assert(call(&h, volume, 2, NULL, 0) == 0);
    assert(call(&h, h.device, 2, NULL, 0) == 0);
    assert(call(&h, h.factory, 2, NULL, 0) == 0);

    /* PICA shape: the backend has volume creation but no draw_volume.  The draw
     * must be refused by name, with no 2D-slice substitute. */
    D9NullStats direct; memset(&direct, 0, sizeof direct);
    D9Backend ops = *d9_null_backend(); ops.draw_volume = NULL;
    D9Device *dev = d9_create_device(&ops, &direct, 1u << 20); assert(dev);
    D9Handle hv = 0;
    assert(d9_create_volume_texture_ex(dev,4,4,4,1,FMT_A8R8G8B8,D9_POOL_MANAGED,0,&hv) == D9_OK);
    uint8_t voxels[4*4*4*4];
    for (unsigned z = 0; z < 4; ++z)
        for (unsigned i = 0; i < 16; ++i) {
            uint32_t c = slice_argb[z];
            memcpy(voxels + z * 64 + i * 4, &c, 4);
        }
    assert(d9_volume_upload_level(dev,hv,0,voxels,16,64,sizeof voxels) == D9_OK);
    assert(d9_bind_volume_texture(dev,0,hv) == D9_OK);
    assert(d9_begin_scene_ex(dev,0,0) == D9_OK);
    assert(d9_draw_primitive(dev,D9_TRIANGLELIST,0,1) == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(dev),"draw_volume") != NULL);
    assert(direct.draws == 0 && direct.volume_draws == 0);
    assert(d9_end_scene(dev) == D9_OK);
    assert(d9_release_volume_texture(dev,hv) == D9_OK);
    assert(d9_destroy_device_ex(dev) == D9_OK);

    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("volume sampling through draw_volume: slices, mip/incomplete/2D/stage-1/PICA refusals PASS");
    return 0;
}
