/* D3DX9_42 plain surface/shader shims through the real D3D9 COM bridge.
 *
 * Covers:
 *   - import resolution (d3dx9_42.dll -> host thunk, unrelated names untouched);
 *   - D3DXLoadSurfaceFromMemory whole and sub-rectangle copies;
 *   - D3DXLoadSurfaceFromSurface same-format copies;
 *   - named failures for format conversion, bad pitch, scaling and filters;
 *   - D3DXCompileShader E_NOTIMPL with cleared outputs and no trap.
 *
 * The surfaces are CPU-backed IDirect3DSurface9 objects created by the null
 * backend, so every pixel assertion reads bytes the shim actually wrote. */
#include "d3dx_surface_shims.h"
#include "d3dx_texture_shims.h"
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH + 0x100)
#define OUT2 (SCRATCH + 0x108)
#define PP (SCRATCH + 0x200)
#define DESC (SCRATCH + 0x300)
#define RECT (SCRATCH + 0x400)
#define LOCKED (SCRATCH + 0x500)
#define DATA (SCRATCH + 0x1000)

#define HR_INVALIDCALL 0x8876086cu
#define HR_NOTAVAILABLE 0x8876086au
#define HR_E_NOTIMPL 0x80004001u

/* Both D3DX modules are bound in the same slice.  Their thunk windows must
 * not overlap: d3dx_texture_shims owns 0xfd000000, this module 0xfd010000.
 * The texture base comes from d3dx_texture_shims.h. */

typedef struct {
    GuestMemory memory;
    CPU cpu;
    uint32_t factory, device;
    FnModule tables[3];
} H;

static uint32_t rd(H *h, uint32_t address) {
    uint32_t v = 0;
    assert(!guest_read32(&h->memory, address, &v));
    return v;
}
static void wr(H *h, uint32_t address, const void *data, size_t bytes) {
    assert(!guest_write(&h->memory, address, data, bytes));
}

/* A COM method is a normal stdcall thunk, so it can be invoked directly with
 * `this` as argument zero. */
static int invoke(H *h, uint32_t target, const uint32_t *args, unsigned n, uint32_t *result) {
    return n3ds_lift_invoke(&h->cpu, target, args, n, result);
}
static uint32_t call(H *h, uint32_t object, unsigned slot, const uint32_t *args, unsigned n) {
    uint32_t vt = 0, fn = 0, frame[20];
    assert(!guest_read32(&h->memory, object, &vt) && vt);
    assert(!guest_read32(&h->memory, vt + slot * 4, &fn) && fn);
    assert(n < 19);
    frame[0] = object;
    for (unsigned i = 0; i < n; ++i) frame[i + 1] = args[i];
    uint32_t r = 0;
    assert(!invoke(h, fn, frame, n + 1, &r));
    return r;
}
static uint32_t d3dx_call(H *h, const char *name, const uint32_t *args, unsigned n,
                          uint32_t *result) {
    PeImport entry = {.dll = "d3dx9_42.dll", .name = name};
    uint32_t fn = win32_resolve(&entry, NULL);
    assert(fn == D3DX_SURFACE_THUNK_BASE +
                    4u * (unsigned)(!strcmp(name, "D3DXLoadSurfaceFromMemory") ? 0 :
                                    !strcmp(name, "D3DXLoadSurfaceFromSurface") ? 1 : 2));
    assert(!n3ds_lift_invoke(&h->cpu, fn, args, n, result));
    return *result;
}

static int harness_start(H *h) {
    memset(h, 0, sizeof *h);
    guest_init(&h->memory, 2u << 20);
    assert(!guest_map(&h->memory, 0x10000000, 0x4000));
    assert(!guest_map(&h->memory, SCRATCH, 0x4000));
    assert(!d9_com_init(&h->memory, NULL));
    assert(!d9_null_register());
    assert(!d3dx_surface_init(&h->memory));
    assert(!d3dx_texture_shims_install(&h->memory));
    h->tables[0] = d9_com_module();
    h->tables[1] = d3dx_surface_module();
    h->tables[2] = d3dx_texture_shims_module();
    /* Binding both D3DX tables in one runtime is itself the overlap check:
     * n3ds_lift_bind_modules() rejects intersecting address ranges. */
    assert(!n3ds_lift_bind_modules(&h->memory, h->tables, 3));
    n3ds_lift_init_cpu(&h->cpu, 0x10004000, 0);

    PeImport create9 = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    uint32_t fn = win32_resolve(&create9, NULL);
    assert(fn);
    uint32_t sdk = 32;
    assert(!invoke(h, fn, &sdk, 1, &h->factory) && h->factory);

    const uint32_t pp[14] = {400, 240, D9_X8R8G8B8, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1};
    wr(h, PP, pp, sizeof pp);
    uint32_t create[] = {0, 1, 0, 0x20, PP, OUT};
    assert(call(h, h->factory, 16, create, 6) == 0);
    h->device = rd(h, OUT);
    assert(h->device);
    return 0;
}
static void harness_stop(H *h) {
    d3dx_texture_shims_uninstall();
    d3dx_surface_shutdown();
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h->memory);
}

static uint32_t create_surface(H *h, unsigned w, unsigned height, unsigned format) {
    uint32_t args[] = {w, height, format, 2, OUT2, 0};
    assert(call(h, h->device, 36, args, 6) == 0);
    uint32_t surface = rd(h, OUT2);
    assert(surface);
    return surface;
}
static void read_surface(H *h, uint32_t surface, unsigned w, unsigned height, uint32_t *out) {
    uint32_t args[] = {LOCKED, 0, 0x10};
    assert(call(h, surface, 13, args, 3) == 0);
    uint32_t pitch = rd(h, LOCKED), address = rd(h, LOCKED + 4);
    assert(pitch >= w * 4u && address);
    const uint8_t *p = guest_ptr(&h->memory, address,
                                (size_t)(height - 1) * pitch + (size_t)w * 4);
    assert(p);
    for (unsigned y = 0; y < height; ++y)
        memcpy(out + (size_t)y * w, p + (size_t)y * pitch, (size_t)w * 4);
    assert(call(h, surface, 14, NULL, 0) == 0);
}

static void test_resolution(H *h) {
    PeImport unknown = {.dll = "d3dx9_42.dll", .name = "D3DXMatrixMultiply"};
    PeImport other_dll = {.dll = "d3d9.dll", .name = "D3DXLoadSurfaceFromMemory"};
    PeImport known = {.dll = "d3dx9_42.dll", .name = "D3DXLoadSurfaceFromSurface"};
    PeImport d3d9 = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    assert(!d3dx_surface_resolve(&unknown, NULL));
    assert(!d3dx_surface_resolve(&other_dll, NULL));
    assert(!win32_resolve(&unknown, NULL)); /* no masking of lifted/PE names */
    assert(!win32_resolve(&other_dll, NULL));
    assert(win32_resolve(&known, NULL) == D3DX_SURFACE_THUNK_BASE + 4u);
    /* The d3d9 bridge resolver stays a d3d9.dll resolver; the D3DX names are
     * owned by the win32 extra-resolver chain, so no chain is consulted twice. */
    assert(!d9_com_resolve(&known, NULL));
    assert(d9_com_resolve(&d3d9, NULL) == win32_resolve(&d3d9, NULL));
    assert(win32_resolve(&d3d9, NULL));
    printf("d3dx surface resolution: PASS\n");
}

/* Regression for the merged tree: both D3DX modules live in one slice, so each
 * of the nine plain exports must reach its own thunk.  The surface test binds
 * both FnModules and both resolvers above; this checks exact addresses,
 * table non-overlap and dispatch identity. */
static void test_shared_binding(H *h) {
    static const struct { const char *name; uint32_t expected; } exports[] = {
        {"D3DXGetImageInfoFromFileInMemory", D3DX_TEXTURE_THUNK_BASE + 0u},
        {"D3DXCreateTextureFromFileInMemory", D3DX_TEXTURE_THUNK_BASE + 4u},
        {"D3DXGetImageInfoFromFileA", D3DX_TEXTURE_THUNK_BASE + 8u},
        {"D3DXCreateTextureFromFileExA", D3DX_TEXTURE_THUNK_BASE + 12u},
        {"D3DXCreateCubeTextureFromFileInMemory", D3DX_TEXTURE_THUNK_BASE + 16u},
        {"D3DXCreateVolumeTextureFromFileInMemory", D3DX_TEXTURE_THUNK_BASE + 20u},
        {"D3DXLoadSurfaceFromMemory", D3DX_SURFACE_THUNK_BASE + 0u},
        {"D3DXLoadSurfaceFromSurface", D3DX_SURFACE_THUNK_BASE + 4u},
        {"D3DXCompileShader", D3DX_SURFACE_THUNK_BASE + 8u},
    };
    const unsigned count = sizeof exports / sizeof exports[0];

    for (unsigned i = 0; i < count; ++i) {
        PeImport entry = {.dll = "d3dx9_42.dll", .name = exports[i].name};
        uint32_t fn = win32_resolve(&entry, NULL);
        assert(fn == exports[i].expected);
        assert(n3ds_lift_has_function(fn));
        for (unsigned j = 0; j < i; ++j)
            assert(fn != exports[j].expected); /* one name, one thunk */
    }

    /* The two FnEntry tables are ordered and disjoint. */
    FnModule texture = d3dx_texture_shims_module();
    FnModule surface = d3dx_surface_module();
    for (unsigned i = 0; i < texture.count; ++i) {
        assert(texture.table[i].addr >= D3DX_TEXTURE_THUNK_BASE &&
               texture.table[i].addr < D3DX_SURFACE_THUNK_BASE);
        for (unsigned j = 0; j < surface.count; ++j) {
            assert(surface.table[j].addr >= D3DX_SURFACE_THUNK_BASE);
            assert(texture.table[i].addr != surface.table[j].addr);
        }
    }
    assert(texture.table[texture.count - 1].addr < D3DX_SURFACE_THUNK_BASE);
    /* Base separation is at least 0x10000, leaving room for future exports. */
    assert(D3DX_SURFACE_THUNK_BASE - D3DX_TEXTURE_THUNK_BASE >= 0x10000u);

    /* Dispatch identity through the real runtime: a texture-only export must
     * not land in the surface shim and vice versa. */
    uint8_t path[16]; memcpy(path, "nonexistent.dds", 16);
    wr(h, DATA, path, sizeof path);
    uint32_t gi[] = {DATA, OUT};
    uint32_t r = 0;
    PeImport info = {.dll = "d3dx9_42.dll", .name = "D3DXGetImageInfoFromFileA"};
    uint32_t fn = win32_resolve(&info, NULL);
    assert(fn == D3DX_TEXTURE_THUNK_BASE + 8u);
    assert(!n3ds_lift_invoke(&h->cpu, fn, gi, 2, &r));
    assert(r == HR_E_NOTIMPL);
    assert(strstr(d3dx_texture_shims_last_error(), "D3DXGetImageInfoFromFileA"));

    uint32_t compile[10] = {0};
    PeImport shader = {.dll = "d3dx9_42.dll", .name = "D3DXCompileShader"};
    fn = win32_resolve(&shader, NULL);
    assert(fn == D3DX_SURFACE_THUNK_BASE + 8u);
    assert(!n3ds_lift_invoke(&h->cpu, fn, compile, 10, &r));
    assert(r == HR_E_NOTIMPL);
    assert(strstr(d3dx_surface_last_error(), "architectural blocker"));
    assert(strstr(d3dx_texture_shims_last_error(), "D3DXGetImageInfoFromFileA"));
    printf("d3dx texture+surface shared dispatch: PASS\n");
}

static void test_from_memory(H *h) {
    uint32_t dest = create_surface(h, 16, 16, D9_A8R8G8B8);
    uint32_t src[16 * 16];
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            src[y * 16 + x] = 0xff000000u | (y << 8) | (x << 16) | (x ^ y);
    wr(h, DATA, src, sizeof src);

    uint32_t args[10] = {dest, 0, 0, DATA, D9_A8R8G8B8, 16 * 4, 0, 0, 0, 0};
    uint32_t r = 0;
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", args, 10, &r) == 0);
    assert(!d3dx_surface_last_error()[0]);

    uint32_t got[16 * 16];
    read_surface(h, dest, 16, 16, got);
    assert(!memcmp(got, src, sizeof src));

    /* Sub-rectangle: 8x8 source at the origin into (4,4)..(12,12). */
    uint32_t dest2 = create_surface(h, 16, 16, D9_A8R8G8B8);
    int32_t dr[4] = {4, 4, 12, 12};
    int32_t sr[4] = {0, 0, 8, 8};
    wr(h, RECT, dr, sizeof dr);
    wr(h, RECT + 0x20, sr, sizeof sr);
    uint32_t sub[10] = {dest2, 0, RECT, DATA, D9_A8R8G8B8, 16 * 4, 0, RECT + 0x20, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", sub, 10, &r) == 0);
    read_surface(h, dest2, 16, 16, got);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            uint32_t want = (x >= 4 && x < 12 && y >= 4 && y < 12)
                                ? src[(y - 4) * 16 + (x - 4)] : 0;
            assert(got[y * 16 + x] == want);
        }

    /* Non-zero source origin with a padded pitch: the source span starts at
     * src + y0*pitch + x0*4 and each row advances by src_pitch, not row_bytes.
     * This is the only non-trivial pointer arithmetic in the shim. */
    uint32_t dest3 = create_surface(h, 16, 16, D9_A8R8G8B8);
    uint32_t padded[11 * 20];
    for (unsigned y = 0; y < 11; ++y)
        for (unsigned x = 0; x < 20; ++x)
            padded[y * 20 + x] = 0xff000000u | (y << 8) | x;
    wr(h, DATA + 0x2000, padded, sizeof padded);
    int32_t pdr[4] = {2, 2, 10, 10};
    int32_t psr[4] = {2, 3, 10, 11};
    wr(h, RECT + 0x80, pdr, sizeof pdr);
    wr(h, RECT + 0xa0, psr, sizeof psr);
    uint32_t padded_args[10] = {dest3, 0, RECT + 0x80, DATA + 0x2000,
                                D9_A8R8G8B8, 20 * 4, 0, RECT + 0xa0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", padded_args, 10, &r) == 0);
    read_surface(h, dest3, 16, 16, got);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            uint32_t want = (x >= 2 && x < 10 && y >= 2 && y < 10)
                                ? padded[(y - 2 + 3) * 20 + (x - 2 + 2)] : 0;
            assert(got[y * 16 + x] == want);
        }

    /* A render-target destination is not CPU-writable.  It must be a named
     * NOTAVAILABLE (the old path reported INVALIDCALL from LockRect). */
    uint32_t rt_args[] = {16, 16, D9_X8R8G8B8, 0, 0, 0, OUT2, 0};
    assert(call(h, h->device, 28, rt_args, 8) == 0);
    uint32_t rt = rd(h, OUT2);
    assert(rt);
    uint32_t rt_fill[10] = {rt, 0, 0, DATA, D9_X8R8G8B8, 16 * 4, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", rt_fill, 10, &r) == HR_NOTAVAILABLE);
    assert(strstr(d3dx_surface_last_error(), "render target"));

    /* Format conversion is a named failure and must not write pixels. */
    uint32_t before = got[0];
    uint32_t conv[10] = {dest2, 0, 0, DATA, D9_X8R8G8B8, 16 * 4, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", conv, 10, &r) == HR_NOTAVAILABLE);
    assert(strstr(d3dx_surface_last_error(), "format conversion"));
    read_surface(h, dest2, 16, 16, got);
    assert(got[0] == before);

    /* Pitch smaller than the source row. */
    uint32_t badpitch[10] = {dest, 0, 0, DATA, D9_A8R8G8B8, 8 * 4, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", badpitch, 10, &r) == HR_INVALIDCALL);
    assert(strstr(d3dx_surface_last_error(), "pitch"));

    /* Scaling is refused instead of resampled. */
    int32_t small[4] = {0, 0, 8, 8};
    wr(h, RECT + 0x40, small, sizeof small);
    uint32_t scale[10] = {dest, 0, 0, DATA, D9_A8R8G8B8, 16 * 4, 0, RECT + 0x40, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", scale, 10, &r) == HR_NOTAVAILABLE);
    assert(strstr(d3dx_surface_last_error(), "scaling"));

    /* Destination rectangle outside the surface. */
    int32_t outside[4] = {0, 0, 17, 17};
    wr(h, RECT + 0x60, outside, sizeof outside);
    uint32_t oob[10] = {dest, 0, RECT + 0x60, DATA, D9_A8R8G8B8, 16 * 4, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", oob, 10, &r) == HR_INVALIDCALL);

    printf("d3dx surface from memory: PASS\n");
}

static void test_from_surface(H *h) {
    uint32_t src_surface = create_surface(h, 8, 8, D9_A8R8G8B8);
    uint32_t dst_surface = create_surface(h, 8, 8, D9_A8R8G8B8);
    uint32_t x8_surface = create_surface(h, 8, 8, D9_X8R8G8B8);
    uint32_t src[8 * 8];
    for (unsigned i = 0; i < 8 * 8; ++i) src[i] = 0xff224400u + i * 0x00010101u;
    wr(h, DATA, src, sizeof src);

    uint32_t fill[10] = {src_surface, 0, 0, DATA, D9_A8R8G8B8, 8 * 4, 0, 0, 0, 0};
    uint32_t r = 0;
    assert(d3dx_call(h, "D3DXLoadSurfaceFromMemory", fill, 10, &r) == 0);

    uint32_t copy[8] = {dst_surface, 0, 0, src_surface, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromSurface", copy, 8, &r) == 0);
    uint32_t got[8 * 8];
    read_surface(h, dst_surface, 8, 8, got);
    assert(!memcmp(got, src, sizeof src));

    /* Format conversion between live surfaces. */
    uint32_t conv[8] = {x8_surface, 0, 0, src_surface, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromSurface", conv, 8, &r) == HR_NOTAVAILABLE);
    assert(strstr(d3dx_surface_last_error(), "format conversion"));

    /* Same surface, bad filter and out-of-bounds source rectangle. */
    uint32_t same[8] = {src_surface, 0, 0, src_surface, 0, 0, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromSurface", same, 8, &r) == HR_INVALIDCALL);
    assert(strstr(d3dx_surface_last_error(), "same surface"));

    uint32_t filter[8] = {dst_surface, 0, 0, src_surface, 0, 0, 0x77, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromSurface", filter, 8, &r) == HR_NOTAVAILABLE);
    assert(strstr(d3dx_surface_last_error(), "filter"));

    int32_t sr[4] = {0, 0, 9, 8};
    wr(h, RECT, sr, sizeof sr);
    uint32_t oob[8] = {dst_surface, 0, 0, src_surface, 0, RECT, 0, 0};
    assert(d3dx_call(h, "D3DXLoadSurfaceFromSurface", oob, 8, &r) == HR_INVALIDCALL);

    printf("d3dx surface from surface: PASS\n");
}

static void test_compile_shader(H *h) {
    uint32_t out_shader = DATA + 0x800, out_errors = DATA + 0x804, out_constants = DATA + 0x808;
    wr(h, out_shader, "\xef\xbe\xad\xde", 4);
    wr(h, out_errors, "\xef\xbe\xad\xde", 4);
    wr(h, out_constants, "\xef\xbe\xad\xde", 4);
    wr(h, DATA + 0x100, "vs_3_0", 7);
    wr(h, DATA + 0x120, "void main() { }", 16);

    uint32_t args[10] = {DATA + 0x120, 15, 0, 0, DATA + 0x130, DATA + 0x100,
                         0, out_shader, out_errors, out_constants};
    uint32_t r = 0;
    assert(d3dx_call(h, "D3DXCompileShader", args, 10, &r) == HR_E_NOTIMPL);
    assert(rd(h, out_shader) == 0 && rd(h, out_errors) == 0 && rd(h, out_constants) == 0);
    assert(strstr(d3dx_surface_last_error(), "SM3") ||
           strstr(d3dx_surface_last_error(), "architectural blocker"));
    assert(strstr(d3dx_surface_last_error(), "no shader"));

    /* All-NULL and unmapped-output calls still return E_NOTIMPL without a trap
     * or a guest fault. */
    uint32_t zero[10] = {0};
    assert(d3dx_call(h, "D3DXCompileShader", zero, 10, &r) == HR_E_NOTIMPL);
    assert(strstr(d3dx_surface_last_error(), "architectural blocker"));

    uint32_t bad[10] = {0, 0, 0, 0, 0, 0, 0, 0x20000000u, 0x20000004u, 0x20000008u};
    assert(d3dx_call(h, "D3DXCompileShader", bad, 10, &r) == HR_E_NOTIMPL);

    printf("d3dx compile shader named unsupported: PASS\n");
}

/* The shader refusal must not disappear when the D3D9 session is absent.  The
 * resolver install is memory-only; surface copies then fail by name, but
 * D3DXCompileShader still reaches its host thunk. */
static void test_no_d3d9_install(void) {
    GuestMemory m;
    memset(&m, 0, sizeof m);
    assert(!d3dx_surface_init(&m));
    PeImport shader = {.dll = "d3dx9_42.dll", .name = "D3DXCompileShader"};
    assert(win32_resolve(&shader, NULL) == D3DX_SURFACE_THUNK_BASE + 8u);
    d3dx_surface_shutdown();
}

int main(void) {
    H h;
    test_no_d3d9_install();
    assert(!harness_start(&h));
    test_resolution(&h);
    test_shared_binding(&h);
    test_from_memory(&h);
    test_from_surface(&h);
    test_compile_shader(&h);
    harness_stop(&h);
    printf("d3dx surface shims: PASS\n");
    return 0;
}

