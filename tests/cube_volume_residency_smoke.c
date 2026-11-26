/* Cube/volume residency and unified accounting regression.
 *
 * Both kinds keep their full CPU slice store as the D9TexCache guest copy and
 * register the backend native as the GPU copy.  The guest copy is never dropped
 * and the entries are explicit-release-only (no rehydration yet), so LRU
 * eviction must skip them.  The test also covers release-failure retry, exactly
 * one native delete on device teardown, the budget refusal path, and that 2D
 * textures / render targets keep their existing accounting. */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_texcache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* D9NullStats must stay first: every null backend callback casts ctx to it. */
typedef struct {
    D9NullStats stats;
    unsigned fail_deletes;
} FlakyCtx;

static int flaky_release(void *ctx, void *native) {
    FlakyCtx *f = ctx;
    if (f->fail_deletes) {
        --f->fail_deletes;
        return -1;
    }
    d9_null_backend()->texture_delete(ctx, native);
    return 0;
}
static int flaky_cube_delete(void *ctx, void *native) {
    FlakyCtx *f = ctx;
    if (f->fail_deletes) {
        --f->fail_deletes;
        return -1;
    }
    return d9_null_backend()->texture_delete_cube(ctx, native);
}

static unsigned guest_releases, gpu_releases;
static int test_guest_release(void *ctx, void *guest, size_t bytes) {
    (void)ctx; (void)guest; (void)bytes; ++guest_releases; return 0;
}
static int test_gpu_release(void *ctx, void *gpu, size_t bytes) {
    (void)ctx; (void)gpu; (void)bytes; ++gpu_releases; return 0;
}

static void upload_cube(D9Device *dev, D9Handle cube) {
    uint8_t block[64];
    memset(block, 0, sizeof block);
    for (unsigned face = 0; face < D9_CUBE_FACES; ++face) {
        for (unsigned level = 0; level < 2; ++level) {
            unsigned edge = 8u >> level;
            size_t rows = (edge + 3u) / 4u;
            size_t row_bytes = rows * 8u;
            assert(d9_cube_upload_face_level(dev, cube, face, level, block,
                                             row_bytes, rows * row_bytes) == D9_OK);
        }
    }
}
static void upload_volume(D9Device *dev, D9Handle volume) {
    uint8_t box[4 * 4 * 2 * 4];
    memset(box, 0, sizeof box);
    assert(d9_volume_upload_level(dev, volume, 0, box, 16u, 64u, sizeof box) == D9_OK);
}
static void make_cube_volume(D9Device *dev, D9Handle *cube, D9Handle *volume) {
    *cube = *volume = 0;
    assert(d9_create_cube_texture_ex(dev, 8, 2, D9_DXT1, D9_POOL_MANAGED, 0, cube) == D9_OK);
    upload_cube(dev, *cube);
    assert(d9_create_volume_texture_ex(dev, 4, 4, 2, 2, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, volume) == D9_OK);
    upload_volume(dev, *volume);
}

int main(void) {
    /* ---- 2D / render-target coexistence and exact accounting ------------ */
    D9NullStats stats;
    memset(&stats, 0, sizeof stats);
    D9Backend ops = *d9_null_backend();
    D9Device *dev = d9_create_device(&ops, &stats, 1u << 20);
    assert(dev);
    assert(d9_memory_used(dev) == 0);

    D9Handle tex = 0;
    assert(d9_create_texture(dev, 8, 8, D9_A8R8G8B8, &tex) == D9_OK);
    {void *pixels = NULL; size_t pitch = 0;
     assert(d9_texture_lock_level(dev, tex, 0, &pixels, &pitch) == D9_OK && pixels);
     assert(d9_texture_unlock_level(dev, tex, 0) == D9_OK);}
    size_t guest = 0, gpu = 0;
    assert(d9_texture_resource_info(dev, tex, NULL, NULL, NULL, NULL,
                                    &guest, &gpu, NULL, NULL) == D9_OK);
    assert(guest == 256 && gpu == 256);
    assert(d9_memory_used(dev) == 512);

    D9Handle rt = 0;
    assert(d9_create_render_texture(dev, 8, 8, D9_A8R8G8B8, D9_POOL_DEFAULT,
                                    D9_USAGE_RENDERTARGET, &rt) == D9_OK);
    assert(d9_memory_used(dev) == 768); /* 512 texture cache + 256 target */
    D9TexCacheStats base_stats;
    assert(d9_texture_cache_stats(dev, &base_stats) == D9_OK);
    assert(base_stats.entries == 1 && base_stats.guests == 1 &&
           base_stats.gpus == 1 && base_stats.pending_gpus == 0);
    assert(base_stats.non_evictable == 0); /* RT texture is not cached at all */

    D9Handle cube = 0, volume = 0;
    make_cube_volume(dev, &cube, &volume);
    unsigned levels = 0, edge = 0, format = 0;
    assert(d9_cube_resource_info(dev, cube, &levels, &edge, &format,
                                 &guest, &gpu, NULL) == D9_OK);
    assert(levels == 2 && edge == 8 && format == D9_DXT1);
    assert(guest == 240 && gpu == 3072); /* six faces * (32 + 8) / estimate */
    unsigned vlevels = 0, vw = 0, vh = 0, vd = 0;
    assert(d9_volume_resource_info(dev, volume, &vlevels, &vw, &vh, &vd,
                                   &format, &guest, &gpu, NULL) == D9_OK);
    assert(vlevels == 2 && vw == 4 && vh == 4 && vd == 2);
    assert(guest == 144 && gpu == 768); /* two slices + mips / estimate */

    D9TexCacheStats s;
    assert(d9_texture_cache_stats(dev, &s) == D9_OK);
    assert(s.entries == base_stats.entries + 2);
    assert(s.guests == base_stats.guests + 2 && s.gpus == base_stats.gpus + 2);
    assert(s.pending_gpus == base_stats.pending_gpus);
    assert(s.non_evictable == 2); /* cube + volume only */
    assert(s.uploads == base_stats.uploads + 2);
    assert(d9_memory_used(dev) == 768 + 240 + 3072 + 144 + 768);
    assert(s.guest_bytes == 640 && s.gpu_bytes == 4096);

    /* 2D and render-target paths are untouched by the new entries. */
    assert(d9_bind_cube_texture(dev, 0, cube) == D9_OK);
    assert(d9_bind_volume_texture(dev, 0, volume) == D9_OK);
    assert(d9_bind_texture(dev, 0, tex) == D9_OK);
    unsigned rw = 0, rh = 0;
    assert(d9_render_target_info(dev, d9_texture_render_target(dev, rt), &rw, &rh,
                                 NULL, NULL, NULL) == D9_OK);
    assert(rw == 8 && rh == 8);
    assert(d9_texture_resource_info(dev, tex, NULL, NULL, NULL, NULL,
                                    &guest, &gpu, NULL, NULL) == D9_OK);
    assert(guest == 256 && gpu == 256);

    /* Explicit release removes exactly one native per resource. */
    unsigned deletes = stats.texture_deletes;
    assert(d9_release_cube_texture(dev, cube) == D9_OK);
    assert(stats.texture_deletes == deletes + 1);
    assert(d9_memory_used(dev) == 768 + 144 + 768);
    assert(d9_release_volume_texture(dev, volume) == D9_OK);
    assert(stats.texture_deletes == deletes + 2);
    assert(d9_memory_used(dev) == 768);
    assert(d9_texture_cache_stats(dev, &s) == D9_OK);
    assert(s.entries == base_stats.entries && s.non_evictable == 0);

    assert(d9_release_texture(dev, tex) == D9_OK);
    assert(d9_release_texture(dev, rt) == D9_OK);
    assert(d9_memory_used(dev) == 0);
    assert(d9_texture_cache_stats(dev, &s) == D9_OK && s.entries == 0);
    assert(d9_destroy_device_ex(dev) == D9_OK);

    /* ---- teardown owns exactly one delete per native -------------------- */
    memset(&stats, 0, sizeof stats);
    dev = d9_create_device(&ops, &stats, 1u << 20);
    assert(dev);
    make_cube_volume(dev, &cube, &volume);
    assert(stats.texture_deletes == 0);
    assert(d9_destroy_device_ex(dev) == D9_OK);
    assert(stats.texture_deletes == 2 && stats.target_deletes == 0);

    /* ---- budget refusal leaves no native allocated or deleted ----------- */
    memset(&stats, 0, sizeof stats);
    dev = d9_create_device(&ops, &stats, 512);
    assert(dev);
    D9Handle refused = 1;
    assert(d9_create_cube_texture_ex(dev, 8, 2, D9_DXT1, D9_POOL_MANAGED, 0,
                                     &refused) == D9_MEMORY);
    assert(refused == 0);
    assert(d9_create_volume_texture_ex(dev, 4, 4, 2, 2, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, &refused) == D9_MEMORY);
    assert(refused == 0);
    assert(d9_memory_used(dev) == 0);
    assert(stats.texture_creates == 0 && stats.texture_deletes == 0);
    assert(d9_destroy_device_ex(dev) == D9_OK);
    assert(stats.texture_deletes == 0);

    /* ---- release/teardown failures are retryable, never double-free ----- */
    FlakyCtx flaky;
    memset(&flaky, 0, sizeof flaky);
    D9Backend flaky_ops = ops;
    flaky_ops.texture_release = flaky_release;
    flaky_ops.texture_delete_cube = flaky_cube_delete;
    dev = d9_create_device(&flaky_ops, &flaky, 1u << 20);
    assert(dev);
    make_cube_volume(dev, &cube, &volume);
    flaky.fail_deletes = 1;
    assert(d9_release_cube_texture(dev, cube) == D9_RELEASE_FAILED);
    assert(flaky.stats.texture_deletes == 0);
    assert(d9_cube_levels(dev, cube) == 2); /* slot survives for the retry */
    assert(d9_release_cube_texture(dev, cube) == D9_OK);
    assert(flaky.stats.texture_deletes == 1);
    assert(d9_cube_levels(dev, cube) == 0);
    assert(d9_release_volume_texture(dev, volume) == D9_OK);

    make_cube_volume(dev, &cube, &volume);
    flaky.fail_deletes = 1;
    assert(d9_destroy_device_ex(dev) == D9_RELEASE_FAILED);
    /* The volume native is released in the same pass; only the cube native
     * remains for the retry. */
    assert(flaky.stats.texture_deletes == 3);
    assert(d9_destroy_device_ex(dev) == D9_OK);
    assert(flaky.stats.texture_deletes == 4);

    /* ---- non-evictable cache entries keep both copies intact ------------ */
    guest_releases = gpu_releases = 0;
    D9TexEntry entries[4];
    D9TexCache cache;
    D9TexCacheOps cache_ops = {NULL, test_guest_release, test_gpu_release};
    assert(d9_texcache_init(&cache, entries, 4, &cache_ops, 0, 0) == D9TEXCACHE_OK);
    int guest_token = 1, gpu_token = 2;
    assert(d9_texcache_add_guest(&cache, 7, &guest_token, 16, 1) == D9TEXCACHE_OK);
    D9TexGpuRegistration reg = {&gpu_token, 64, 1, test_gpu_release, NULL,
                                D9TEXCACHE_NO_REHYDRATE_TOKEN};
    assert(d9_texcache_attach_gpu(&cache, 7, &reg) == D9TEXCACHE_OK);
    assert(d9_texcache_commit_upload(&cache, 7, &gpu_token, 64, 0) == D9TEXCACHE_OK);
    assert(d9_texcache_set_no_evict(&cache, 7, 1) == D9TEXCACHE_OK);
    assert(d9_texcache_set_limits(&cache, 0, 64) == D9TEXCACHE_OK);
    assert(d9_texcache_reserve_gpu(&cache, 64, NULL) != D9TEXCACHE_OK);
    assert(gpu_releases == 0 && guest_releases == 0);
    D9TexEntry *entry = d9_texcache_find(&cache, 7);
    assert(entry && entry->has_guest && entry->has_gpu && entry->gpu_ready);
    D9TexCacheStats cs;
    d9_texcache_stats(&cache, &cs);
    assert(cs.non_evictable == 1);
    assert(d9_texcache_set_no_evict(&cache, 7, 0) == D9TEXCACHE_OK);
    assert(d9_texcache_reserve_gpu(&cache, 64, NULL) == D9TEXCACHE_OK);
    assert(gpu_releases == 1 && guest_releases == 0);
    entry = d9_texcache_find(&cache, 7);
    assert(entry && entry->has_guest && !entry->has_gpu);
    assert(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK);
    assert(guest_releases == 1);

    puts("cube/volume residency: PASS");
    return 0;
}
