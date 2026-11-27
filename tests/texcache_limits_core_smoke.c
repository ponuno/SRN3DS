/* Host test: texture-cache limits + frame pinning through d3d9_core.
 *
 * N3DS_TEX_GPU_LIMIT caps the cache's GPU bytes.  A texture that is bound or
 * referenced by a submitted draw cannot be evicted to satisfy a new
 * allocation; the creation is refused with a named "pinned" diagnosis and
 * pinned_refusals++, then succeeds after Present's frame boundary (the
 * backend's frame_sync hook / synchronous ops.end) releases the pins.
 *
 * Covers the 2D, cube, volume and render-target-as-texture paths, plus the
 * env parser (bytes and MiB suffix).
 */
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMT_A8R8G8B8 21u
typedef struct { D9NullStats stats; } TestCtx;
typedef struct { float x, y, z, nx, ny, nz, u, v, w; } V3Vertex;

static void set_gpu_limit(const char *value) {
    if (value) assert(_putenv_s("N3DS_TEX_GPU_LIMIT", value) == 0);
    else assert(_putenv_s("N3DS_TEX_GPU_LIMIT", "") == 0);
}

static void fill_cube(D9Device *d, D9Handle cube, uint32_t argb) {
    uint32_t pixels[8 * 8];
    for (unsigned i = 0; i < 8 * 8; ++i) pixels[i] = argb;
    for (unsigned face = 0; face < 6; ++face)
        assert(d9_cube_upload_face_level(d, cube, face, 0, pixels, 32,
                                         sizeof pixels) == D9_OK);
}

static void fill_volume(D9Device *d, D9Handle volume) {
    uint32_t pixels[4 * 4 * 4];
    for (unsigned i = 0; i < 4 * 4 * 4; ++i) pixels[i] = 0xff000000u | i;
    assert(d9_volume_upload_level(d, volume, 0, pixels, 16, 64,
                                  sizeof pixels) == D9_OK);
}

static void setup_decl2(D9Device *d) {
    D9VertexElement e[3] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT3, 0, D9_USAGE_NORMAL, 0},
        {0, 24, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, e, 3));
    assert(!d9_set_vertex_declaration(d, &decl));
}

static void setup_decl3(D9Device *d) {
    D9VertexElement e[3] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT3, 0, D9_USAGE_NORMAL, 0},
        {0, 24, D9_DECL_FLOAT3, 0, D9_USAGE_TEXCOORD, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, e, 3));
    assert(!d9_set_vertex_declaration(d, &decl));
}

static void draw_quad2(D9Device *d) {
    float v[3][8] = {
        {-1, -1, 0, 1, 0, 0, 0.25f, 0.75f},
        { 1, -1, 0, 1, 0, 0, 0.25f, 0.75f},
        { 0,  1, 0, 1, 0, 0, 0.25f, 0.75f},
    };
    assert(!d9_begin_scene(d, 0));
    assert(!d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]));
    assert(!d9_end_scene(d));
}
static void draw_quad3(D9Device *d) {
    V3Vertex v[3] = {
        {-1, -1, 0, 1, 0, 0, 0.25f, 0.75f, 0.5f},
        { 1, -1, 0, 1, 0, 0, 0.25f, 0.75f, 0.5f},
        { 0,  1, 0, 1, 0, 0, 0.25f, 0.75f, 0.5f},
    };
    assert(!d9_begin_scene(d, 0));
    assert(!d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]));
    assert(!d9_end_scene(d));
}

static void present_frame(D9Device *d) {
    assert(!d9_begin_scene(d, 0));
    assert(!d9_present(d));
}

static size_t cube_gpu(D9Device *d, D9Handle cube) {
    size_t gpu = 0;
    assert(d9_cube_resource_info(d, cube, NULL, NULL, NULL, NULL, &gpu, NULL) ==
           D9_OK);
    return gpu;
}

static size_t volume_gpu(D9Device *d, D9Handle volume) {
    size_t gpu = 0;
    assert(d9_volume_resource_info(d, volume, NULL, NULL, NULL, NULL, NULL,
                                   NULL, &gpu, NULL) == D9_OK);
    return gpu;
}

/* The env parser: "1200" and "8M" both produce limits; a bad suffix is
 * ignored (no limit) rather than aborting device creation. */
static void test_env_limits(void) {
    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops = *d9_null_backend();

    set_gpu_limit("1200");
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);
    size_t guest = 1, gpu = 1;
    assert(d9_texture_cache_get_limits(d, &guest, &gpu) == D9_OK);
    assert(gpu == 1200 && guest == 0);
    assert(d9_destroy_device_ex(d) == D9_OK);

    set_gpu_limit("8M");
    memset(&ctx, 0, sizeof ctx);
    d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);
    assert(d9_texture_cache_get_limits(d, NULL, &gpu) == D9_OK);
    assert(gpu == 8u * 1024u * 1024u);
    assert(d9_destroy_device_ex(d) == D9_OK);

    set_gpu_limit("nonsense");
    memset(&ctx, 0, sizeof ctx);
    d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);
    assert(d9_texture_cache_get_limits(d, NULL, &gpu) == D9_OK && gpu == 0);
    assert(d9_destroy_device_ex(d) == D9_OK);
    set_gpu_limit(NULL);
}

/* Cube: a bound texture blocks the allocation, the refusal is named, and
 * Present releases the frame pin so the next create can evict it. */
static void test_cube_pin(void) {
    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops = *d9_null_backend();
    set_gpu_limit("2000"); /* one 8x8 cube costs 1536 GPU bytes */
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);

    D9Handle a = 0, b = 0;
    assert(d9_create_cube_texture_ex(d, 8, 1, FMT_A8R8G8B8, D9_POOL_MANAGED, 0,
                                     &a) == D9_OK);
    fill_cube(d, a, 0xffff0000u);
    assert(d9_bind_cube_texture(d, 0, a) == D9_OK);
    assert(d9_create_cube_texture_ex(d, 8, 1, FMT_A8R8G8B8, D9_POOL_MANAGED, 0,
                                     &b) == D9_BACKEND);
    assert(b == 0);
    assert(strstr(d9_last_draw_diagnostic(d), "pinned") != NULL);

    D9TexCacheStats st;
    assert(d9_texture_cache_stats(d, &st) == D9_OK);
    assert(st.pinned >= 1);
    assert(st.pinned_refusals >= 1);
    assert(cube_gpu(d, a) > 0);

    /* Still bound across Present: slot pin is renewed, draw pin is not. */
    present_frame(d);
    assert(d9_texture_cache_stats(d, &st) == D9_OK);
    assert(st.pinned >= 1);
    assert(cube_gpu(d, a) > 0);

    /* Unbinding makes the cube an LRU candidate; the next create evicts it. */
    assert(d9_bind_cube_texture(d, 0, 0) == D9_OK);
    assert(d9_create_cube_texture_ex(d, 8, 1, FMT_A8R8G8B8, D9_POOL_MANAGED, 0,
                                     &b) == D9_OK);
    fill_cube(d, b, 0xff00ff00u);
    assert(cube_gpu(d, a) == 0);
    assert(cube_gpu(d, b) > 0);

    /* Draw-use pin: submitted draw pins even after the slot is unbound. */
    setup_decl2(d);
    assert(d9_bind_cube_texture(d, 0, b) == D9_OK);
    draw_quad2(d);
    assert(d9_bind_cube_texture(d, 0, 0) == D9_OK);
    D9Handle c = 0;
    assert(d9_create_cube_texture_ex(d, 8, 1, FMT_A8R8G8B8, D9_POOL_MANAGED, 0,
                                     &c) == D9_BACKEND);
    assert(strstr(d9_last_draw_diagnostic(d), "pinned") != NULL);
    present_frame(d);
    assert(d9_create_cube_texture_ex(d, 8, 1, FMT_A8R8G8B8, D9_POOL_MANAGED, 0,
                                     &c) == D9_OK);
    fill_cube(d, c, 0xff0000ffu);
    assert(cube_gpu(d, b) == 0);

    assert(d9_release_cube_texture(d, a) == D9_OK);
    assert(d9_release_cube_texture(d, b) == D9_OK);
    assert(d9_release_cube_texture(d, c) == D9_OK);
    assert(d9_destroy_device_ex(d) == D9_OK);
    set_gpu_limit(NULL);
}

/* Volume: same policy through the 3D texture path (bind + draw). */
static void test_volume_pin(void) {
    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops = *d9_null_backend();
    set_gpu_limit("1200"); /* 4x4x4 volume = 1024 GPU bytes */
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);

    D9Handle a = 0, b = 0;
    assert(d9_create_volume_texture_ex(d, 4, 4, 4, 1, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, &a) == D9_OK);
    fill_volume(d, a);
    setup_decl3(d);
    assert(d9_bind_volume_texture(d, 0, a) == D9_OK);
    draw_quad3(d);
    assert(d9_bind_volume_texture(d, 0, 0) == D9_OK);
    assert(d9_create_volume_texture_ex(d, 4, 4, 4, 1, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, &b) == D9_BACKEND);
    assert(strstr(d9_last_draw_diagnostic(d), "pinned") != NULL);
    D9TexCacheStats st;
    assert(d9_texture_cache_stats(d, &st) == D9_OK && st.pinned >= 1);

    present_frame(d);
    assert(d9_create_volume_texture_ex(d, 4, 4, 4, 1, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, &b) == D9_OK);
    assert(volume_gpu(d, a) == 0 && volume_gpu(d, b) > 0);
    assert(ctx.stats.last_volume_count == 3);

    assert(d9_release_volume_texture(d, a) == D9_OK);
    assert(d9_release_volume_texture(d, b) == D9_OK);
    assert(d9_destroy_device_ex(d) == D9_OK);
    set_gpu_limit(NULL);
}

/* Render-target-as-texture: the Target owns the native, so the texcache never
 * evicts it; binding and drawing it must survive Present and remain sampleable. */
static void test_render_texture(void) {
    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops = *d9_null_backend();
    set_gpu_limit(NULL);
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    assert(d);

    D9Handle rt = 0;
    assert(d9_create_render_texture(d, 64, 64, D9_A8R8G8B8, D9_POOL_DEFAULT,
                                    D9_USAGE_RENDERTARGET, &rt) == D9_OK);
    setup_decl2(d);
    assert(d9_set_texture(d, rt) == D9_OK);
    draw_quad2(d);
    present_frame(d);
    /* Still bound and usable in the next frame. */
    assert(d9_set_texture(d, rt) == D9_OK);
    draw_quad2(d);
    present_frame(d);
    assert(ctx.stats.draws >= 2);
    assert(d9_release_texture(d, rt) == D9_OK);
    assert(d9_destroy_device_ex(d) == D9_OK);
}

int main(void) {
    test_env_limits();
    test_cube_pin();
    test_volume_pin();
    test_render_texture();
    puts("texcache limits: env parser, cube/volume/RT frame pins, named refusal");
    return 0;
}
