/* Render-target cube sampling readiness.
 *
 * The cube is created as D3DUSAGE_RENDERTARGET, each face is selected and
 * cleared to a distinct colour, then bound with SetTexture and sampled by a
 * draw with +X..-Z normals.  The null backend uses the face-target pixel
 * buffers, not the zeroed CPU upload store.  A partial cube is a named
 * refusal; a backend without draw_cube (a backend without draw_cube) binds the complete RT
 * cube but refuses the draw by name and never substitutes face 0.
 */
#include "d3d9_com.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH + 0x200)
#define CAPS (SCRATCH + 0x400)
#define OUT (SCRATCH + 0x500)
#define DECLVA (SCRATCH + 0x2000)
#define VERTS (SCRATCH + 0x3000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALID 0x8876086cu
#define HR_UNAVAILABLE 0x8876086au

typedef struct { float x,y,z,nx,ny,nz,u,v; } CubeVertex;

static const uint32_t face_argb[6] = {
    0xffff0000u, /* +X red */
    0xff00ff00u, /* -X green */
    0xff0000ffu, /* +Y blue */
    0xffffff00u, /* -Y yellow */
    0xffff00ffu, /* +Z magenta */
    0xff00ffffu, /* -Z cyan */
};

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) { c->r[4] -= 4; M32(c->r[4]) = M32(sp + 8 + i * 4); }
    c->r[4] -= 4; M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c); else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "cube RT sampling stdcall stack mismatch");
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
static uint32_t rd(Harness *h, uint32_t address) { uint32_t v = 0; assert(!guest_read32(&h->memory, address, &v)); return v; }
static void wr(Harness *h, uint32_t address, const void *data, size_t bytes) { assert(!guest_write(&h->memory, address, data, bytes)); }
static uint32_t caps(Harness *h) { assert(call(h, h->device, 7, (uint32_t[]){CAPS}, 1) == HR_OK); return rd(h, CAPS + 28); }

static uint32_t create_cube(Harness *h, unsigned edge, unsigned levels, unsigned format) {
    uint32_t a[] = {edge, levels, D9_USAGE_RENDERTARGET, format, D9_POOL_DEFAULT, OUT, 0};
    uint32_t hr = call(h, h->device, 25, a, 7);
    return hr;
}
static uint32_t get_surface(Harness *h, uint32_t cube, unsigned face, unsigned level) {
    uint32_t a[] = {face, level, OUT};
    assert(call(h, cube, 18, a, 3) == HR_OK);
    uint32_t s = rd(h, OUT); assert(s); return s;
}
static uint32_t set_texture(Harness *h, uint32_t cube) {
    return call(h, h->device, 65, (uint32_t[]){0, cube}, 2);
}
static uint32_t bind_face(Harness *h, uint32_t surface) {
    return call(h, h->device, 37, (uint32_t[]){0, surface}, 2);
}
static uint32_t clear_face(Harness *h, uint32_t argb) {
    return call(h, h->device, 43, (uint32_t[]){0,0,1,argb,0,0}, 6);
}
static uint32_t begin_scene(Harness *h) { return call(h, h->device, 41, NULL, 0); }
static uint32_t end_scene(Harness *h) { return call(h, h->device, 42, NULL, 0); }
static uint32_t present(Harness *h) { return call(h, h->device, 17, (uint32_t[]){0,0,0,0}, 4); }

static uint32_t setup_decl(Harness *h) {
    D9VertexElement e[4] = {
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT3,0,D9_USAGE_NORMAL,0},
        {0,24,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
        {0xff,0,D9_DECL_UNUSED,0,0,0},
    };
    wr(h, DECLVA, e, sizeof e);
    uint32_t a[] = {DECLVA, OUT};
    assert(!guest_write32(&h->memory, OUT, 0));
    assert(call(h, h->device, 86, a, 2) == HR_OK);
    uint32_t decl = rd(h, OUT); assert(decl);
    assert(call(h, h->device, 87, (uint32_t[]){decl}, 1) == HR_OK);
    return decl;
}
static void triangle(CubeVertex v[3], float nx, float ny, float nz, float u, float v0) {
    v[0] = (CubeVertex){-1,-1,0,nx,ny,nz,u,v0};
    v[1] = (CubeVertex){ 1,-1,0,nx,ny,nz,u,v0};
    v[2] = (CubeVertex){ 0, 1,0,nx,ny,nz,u,v0};
}
static void draw_up(Harness *h, const void *verts, unsigned stride) {
    wr(h, VERTS, verts, 3 * stride);
    uint32_t a[] = {D9_TRIANGLELIST, 1, VERTS, stride};
    assert(call(h, h->device, 83, a, 4) == HR_OK);
}
static void expect_face(Harness *h, unsigned face) {
    const D9NullStats *s = d9_null_stats();
    assert(s->last_cube_count == 3);
    for (unsigned i = 0; i < 3; ++i) {
        assert(s->last_cube_faces[i] == face);
        assert(s->last_cube_samples[i] == face_argb[face]);
    }
}

int main(void) {
    Harness h; memset(&h, 0, sizeof h);
    guest_init(&h.memory, 4u << 20);
    assert(!guest_map(&h.memory, 0x10000000, 0x4000) && !guest_map(&h.memory, SCRATCH, 0x6000));
    assert(!d9_com_init(&h.memory, NULL));
    assert(!d9_null_register());
    static const FnEntry probe[] = {{PROBE_FN, guest_dispatch_probe}};
    FnModule modules[2] = {{"probe", probe, 1}, d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory, modules, 2));
    n3ds_lift_init_cpu(&h.cpu, 0x10004000, 0);

    uint32_t create9 = d9_com_resolve(&(PeImport){.dll = "d3d9.dll", .name = "Direct3DCreate9"}, NULL);
    assert(create9);
    uint32_t sdk = 32;
    assert(!invoke(&h, create9, &sdk, 1, &h.factory) && h.factory);
    uint32_t pp[14] = {400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    wr(&h, PP, pp, sizeof pp);
    uint32_t create[] = {0,1,0,0x20,PP,OUT};
    assert(call(&h, h.factory, 16, create, 6) == HR_OK);
    assert(!guest_read32(&h.memory, OUT, &h.device) && h.device);
    assert(caps(&h) == 0x00082000u);

    assert(create_cube(&h, 16, 1, D9_A8R8G8B8) == HR_OK);
    uint32_t cube = rd(&h, OUT); assert(cube);

    /* No face has been rendered: SetTexture names the incomplete cube. */
    assert(set_texture(&h, cube) != HR_OK);
    assert(strstr(d9_com_last_diagnostic(), "incomplete") != NULL);

    uint32_t surf[6] = {0};
    for (unsigned face = 0; face < 6; ++face) {
        surf[face] = get_surface(&h, cube, face, 0);
        assert(bind_face(&h, surf[face]) == HR_OK);
        assert(clear_face(&h, face_argb[face]) == HR_OK);
        assert(begin_scene(&h) == HR_OK);
        assert(end_scene(&h) == HR_OK);
        assert(present(&h) == HR_OK);
        if (face < 5) {
            assert(set_texture(&h, cube) != HR_OK);
            assert(strstr(d9_com_last_diagnostic(), "incomplete") != NULL);
        }
    }
    assert(bind_face(&h, 0) == HR_OK);
    assert(set_texture(&h, cube) == HR_OK);
    assert(d9_null_stats()->cube_face_creates == 6u);

    uint32_t decl = setup_decl(&h);
    assert(begin_scene(&h) == HR_OK);
    CubeVertex v[3];
    static const float axes[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (unsigned face = 0; face < 6; ++face) {
        triangle(v, axes[face][0], axes[face][1], axes[face][2], 0.25f, 0.75f);
        draw_up(&h, v, sizeof v[0]);
        expect_face(&h, face);
    }
    assert(d9_null_stats()->cube_draws == 6u);
    assert(end_scene(&h) == HR_OK);
    assert(present(&h) == HR_OK);
    assert(caps(&h) == 0x00082000u);

    assert(set_texture(&h, 0) == HR_OK);
    assert(call(&h, h.device, 87, (uint32_t[]){0}, 1) == HR_OK);
    for (unsigned face = 0; face < 6; ++face)
        assert(call(&h, surf[face], 2, NULL, 0) == 0);
    assert(call(&h, cube, 2, NULL, 0) == 0);
    assert(call(&h, decl, 2, NULL, 0) == 0);
    assert(d9_null_stats()->cube_face_deletes == 6u);
    assert(call(&h, h.device, 2, NULL, 0) == 0);
    assert(call(&h, h.factory, 2, NULL, 0) == 0);
    d9_com_shutdown();

    /* Backend without draw_cube: RT cube binds after all six faces are rendered, but the
     * missing draw_cube callback refuses the draw by name. */
    D9NullStats direct; memset(&direct, 0, sizeof direct);
    D9Backend ops = *d9_null_backend(); ops.draw_cube = NULL;
    D9Device *dev = d9_create_device(&ops, &direct, 1u << 20); assert(dev);
    D9Handle hc = 0;
    assert(d9_create_cube_texture_ex(dev, 16, 1, D9_A8R8G8B8, D9_POOL_DEFAULT,
                                     D9_USAGE_RENDERTARGET, &hc) == D9_OK);
    for (unsigned face = 0; face < 6; ++face) {
        D9Handle tgt = 0;
        assert(d9_cube_face_target(dev, hc, face, 0, &tgt) == D9_OK);
        assert(d9_set_render_target(dev, tgt) == D9_OK);
        /* Core clear path: BeginScene applies the colour to the face target. */
        assert(d9_begin_scene_ex(dev, face_argb[face], 1) == D9_OK);
        assert(d9_end_scene(dev) == D9_OK);
    }
    assert(d9_bind_cube_texture(dev, 0, hc) == D9_OK);
    assert(d9_begin_scene_ex(dev, 0, 0) == D9_OK);
    assert(d9_draw_primitive(dev, D9_TRIANGLELIST, 0, 1) == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(dev), "draw_cube") != NULL);
    assert(direct.cube_draws == 0u);
    assert(d9_end_scene(dev) == D9_OK);
    assert(d9_release_cube_texture(dev, hc) == D9_OK);
    assert(direct.cube_face_deletes == 6u);
    assert(d9_destroy_device_ex(dev) == D9_OK);

    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("cube RT sampling: rendered faces sample correctly, partial and no-draw_cube are named refusals");
    return 0;
}
