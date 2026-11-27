/* Host test: frame-scoped texture pinning and the pinned-refusal policy.
 *
 * A texture referenced by a submitted GPU draw must not be evicted until the
 * frame's commands are known to have completed.  The cache models that with
 * two frame-scoped pins per entry (slot bind and draw use) that are released
 * only by d9_texcache_frame_clear(), called by the core after Present's GPU
 * barrier.  Purely bound textures are released on rebind, so a bind without a
 * draw never blocks the budget trim forever.
 *
 * Also checks the LRU limit paths: when every candidate is pinned the
 * allocation is refused with a pinned marker, not a silent corruption, and a
 * no_evict entry keeps the pool full even after unpinning.
 */

#include "d3d9_texcache.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

typedef struct {
    int guest_releases;
    int gpu_releases;
} Fake;

static int release_guest(void *ctx, void *guest, size_t bytes) {
    Fake *fake = ctx;
    (void)guest;
    (void)bytes;
    ++fake->guest_releases;
    return 0;
}

static int release_gpu(void *ctx, void *gpu, size_t bytes) {
    Fake *fake = ctx;
    (void)gpu;
    (void)bytes;
    ++fake->gpu_releases;
    return 0;
}

static void init(D9TexCache *cache, D9TexEntry *entries, unsigned capacity,
                 Fake *fake, size_t guest_limit, size_t gpu_limit) {
    D9TexCacheOps ops = {fake, release_guest, release_gpu};
    memset(fake, 0, sizeof *fake);
    check(d9_texcache_init(cache, entries, capacity, &ops, guest_limit,
                           gpu_limit) == D9TEXCACHE_OK, "init");
}

static int add_gpu(D9TexCache *cache, uint64_t key, uint64_t *native,
                   size_t bytes) {
    D9TexGpuRegistration reg = {native, bytes, 0, NULL, NULL,
                                D9TEXCACHE_NO_REHYDRATE_TOKEN};
    return d9_texcache_add_gpu(cache, key, &reg);
}

/* A draw-pinned texture survives allocations that would otherwise evict it;
 * the refusal is flagged as pin-caused.  Unpinning at the frame boundary makes
 * it evictable again. */
static void test_draw_pin_protects_lru(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    uint64_t a = 1, b = 2, c = 3, d = 4;
    init(&cache, entries, 8, &fake, 0, 200);

    check(add_gpu(&cache, 10, &a, 100) == D9TEXCACHE_OK, "add A");
    check(d9_texcache_frame_pin(&cache, 10, 1) == D9TEXCACHE_OK, "draw-pin A");
    check(add_gpu(&cache, 11, &b, 100) == D9TEXCACHE_OK, "add B");
    check(d9_texcache_frame_pin(&cache, 11, 1) == D9TEXCACHE_OK, "draw-pin B");

    D9TexCacheStats st;
    check(add_gpu(&cache, 12, &c, 100) == D9TEXCACHE_GPU_FULL,
          "pinned pool refuses C");
    check(d9_texcache_find(&cache, 10) != NULL, "A not evicted while pinned");
    check(d9_texcache_find(&cache, 11) != NULL, "B not evicted while pinned");
    check(d9_texcache_last_refusal_pinned(&cache) == 1,
          "refusal attributed to pins");
    d9_texcache_stats(&cache, &st);
    check(st.pinned_refusals >= 1, "pinned_refusals counted");
    check(st.pinned == 2, "two pinned entries reported");
    check(st.evictions_gpu == 0, "nothing evicted before unpin");

    /* A second pin for the same frame must not accumulate. */
    check(d9_texcache_frame_pin(&cache, 10, 1) == D9TEXCACHE_OK, "idempotent pin");
    check(d9_texcache_frame_clear(&cache, 1) == 2, "clear releases both entries");
    d9_texcache_stats(&cache, &st);
    check(st.pinned == 0, "no pinned entries after clear");

    check(add_gpu(&cache, 12, &c, 100) == D9TEXCACHE_OK, "C fits after unpin");
    check(d9_texcache_find(&cache, 10) == NULL, "LRU A evicted after unpin");
    check(d9_texcache_find(&cache, 12) != NULL, "C resident");
    check(d9_texcache_last_refusal_pinned(&cache) == 0,
          "successful allocation clears the pin flag");

    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy");
}

/* Bind-only pins are released when the slot is rebound; draw pins are not. */
static void test_bind_pin_scope(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    uint64_t a = 1, b = 2, c = 3;
    init(&cache, entries, 8, &fake, 0, 100);

    check(add_gpu(&cache, 20, &a, 100) == D9TEXCACHE_OK, "add A");
    check(d9_texcache_frame_bind(&cache, 20, 1) == D9TEXCACHE_OK, "bind A");
    check(add_gpu(&cache, 21, &b, 100) == D9TEXCACHE_GPU_FULL,
          "bound A refuses B");
    check(d9_texcache_frame_unbind(&cache, 20, 1) == D9TEXCACHE_OK, "unbind A");
    check(add_gpu(&cache, 21, &b, 100) == D9TEXCACHE_OK,
          "B fits after slot rebind");

    /* Draw-pinned B must survive its own unbind. */
    check(d9_texcache_frame_pin(&cache, 21, 1) == D9TEXCACHE_OK, "draw-pin B");
    check(d9_texcache_frame_unbind(&cache, 21, 1) == D9TEXCACHE_OK,
          "unbind B does not touch the draw pin");
    check(add_gpu(&cache, 22, &c, 100) == D9TEXCACHE_GPU_FULL,
          "draw-pinned B still refuses C");
    check(d9_texcache_frame_clear(&cache, 1) >= 1, "clear draw pin");
    check(add_gpu(&cache, 22, &c, 100) == D9TEXCACHE_OK, "C fits after clear");

    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy");
}

/* no_evict entries are not pin-caused refusals, but they still block the pool. */
static void test_no_evict_refusal(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    uint64_t a = 1, b = 2, c = 3;
    init(&cache, entries, 8, &fake, 0, 200);
    check(add_gpu(&cache, 30, &a, 100) == D9TEXCACHE_OK, "add A");
    check(d9_texcache_set_no_evict(&cache, 30, 1) == D9TEXCACHE_OK, "no_evict A");
    check(add_gpu(&cache, 31, &b, 100) == D9TEXCACHE_OK, "add B");
    check(d9_texcache_frame_pin(&cache, 31, 1) == D9TEXCACHE_OK, "draw-pin B");
    check(add_gpu(&cache, 32, &c, 100) == D9TEXCACHE_GPU_FULL,
          "no_evict + pinned refuses C");
    check(d9_texcache_last_refusal_pinned(&cache) == 1,
          "pinned candidate identifies the refusal");
    check(d9_texcache_frame_clear(&cache, 1) == 1, "clear B");
    /* A remains no_evict, so C evicts B. */
    check(add_gpu(&cache, 32, &c, 100) == D9TEXCACHE_OK, "C evicts B");
    check(d9_texcache_find(&cache, 30) != NULL, "no_evict A kept");
    check(d9_texcache_find(&cache, 31) == NULL, "B evicted");

    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check(st.non_evictable == 1, "non_evictable counted");
    check(st.evictions_gpu == 1, "one GPU eviction");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy");
}

/* A stale pin from an older generation is replaced, never accumulated. */
static void test_generation_replacement(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    uint64_t a = 1;
    init(&cache, entries, 4, &fake, 0, 100);
    check(add_gpu(&cache, 40, &a, 100) == D9TEXCACHE_OK, "add A");
    check(d9_texcache_frame_pin(&cache, 40, 1) == D9TEXCACHE_OK, "pin gen 1");
    check(d9_texcache_frame_pin(&cache, 40, 2) == D9TEXCACHE_OK, "pin gen 2");
    D9TexEntry *e = d9_texcache_find(&cache, 40);
    check(e && e->pins == 1, "one pin after generation move");
    check(d9_texcache_frame_clear(&cache, 2) == 1, "clear gen 2");
    e = d9_texcache_find(&cache, 40);
    check(e && e->pins == 0, "unpinned by current generation");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy");
}

/* Explicit trim reports its calls and names a pinned refusal. */
static void test_trim_and_guest_refusal(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    uint64_t a = 1, b = 2;
    init(&cache, entries, 4, &fake, 100, 0);
    check(d9_texcache_add_guest(&cache, 50, &a, 60, 1) == D9TEXCACHE_OK,
          "guest A");
    check(d9_texcache_frame_pin(&cache, 50, 7) == D9TEXCACHE_OK, "pin A");
    check(d9_texcache_add_guest(&cache, 51, &b, 60, 1) == D9TEXCACHE_GUEST_FULL,
          "pinned guest pool refuses B");
    check(d9_texcache_last_refusal_pinned(&cache) == 1,
          "guest refusal attributed to pins");
    unsigned evicted = 99;
    check(d9_texcache_trim(&cache, 0, 0, &evicted) == D9TEXCACHE_GUEST_FULL,
          "trim stops at the pinned entry");
    check(evicted == 0, "trim evicted nothing");
    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check(st.trims == 1, "trim call counted");
    check(st.pinned_refusals >= 1, "trim refusal counted");
    check(d9_texcache_frame_clear(&cache, 7) == 1, "clear pin");
    check(d9_texcache_trim(&cache, 0, 0, &evicted) == D9TEXCACHE_OK,
          "trim evicts after unpin");
    check(evicted == 1 && d9_texcache_find(&cache, 50) == NULL,
          "evicted A after unpin");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy");
}

int main(void) {
    test_draw_pin_protects_lru();
    test_bind_pin_scope();
    test_no_evict_refusal();
    test_generation_replacement();
    test_trim_and_guest_refusal();
    if (failures) {
        printf("texcache pinning: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("texcache pinning: %d checks passed\n", checks);
    return 0;
}
