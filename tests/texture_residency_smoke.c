/* Host test: texture residency ownership, LRU, working set and GPU failure.
 *
 * No GPU and no game bytes: the callbacks just count releases, and pointers are
 * addresses of local markers.  The point is the accounting contract:
 *   - the guest source is dropped after a successful upload when disposable;
 *   - a failed GPU allocation leaves the guest source intact;
 *   - a pinned entry is never evicted;
 *   - LRU order and byte peaks are deterministic.
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

static void check_u64(uint64_t actual, uint64_t expected, const char *what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        printf("FAIL: %s (%llu != %llu)\n", what,
               (unsigned long long)actual, (unsigned long long)expected);
    }
}

typedef struct {
    int guest_releases;
    int gpu_releases;
    int fail_release;
} Fake;

static int release_guest(void *ctx, void *guest, size_t bytes) {
    Fake *fake = ctx;
    if (fake->fail_release) return -1;
    (void)guest;
    (void)bytes;
    ++fake->guest_releases;
    return 0;
}

static int release_gpu(void *ctx, void *gpu, size_t bytes) {
    Fake *fake = ctx;
    if (fake->fail_release) return -1;
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

static void test_upload_drop_and_both(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    int g1 = 1, g2 = 2, gpu1 = 3, gpu2 = 4;
    init(&cache, entries, 8, &fake, 0, 0);

    check(d9_texcache_add_guest(&cache, 1, &g1, 100, 1) == D9TEXCACHE_OK,
          "add guest 1");
    check(d9_texcache_begin_upload(&cache, 1) == D9TEXCACHE_OK, "begin 1");
    check(d9_texcache_commit_upload(&cache, 1, &gpu1, 200, 1) == D9TEXCACHE_OK,
          "commit 1");
    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 0, "drop: guest bytes");
    check_u64(st.gpu_bytes, 200, "drop: gpu bytes");
    check_u64(st.peak_both, 300, "drop: peak both captured before drop");
    check_u64(st.both_bytes, 0, "drop: no current both");
    check_u64((uint64_t)fake.guest_releases, 1, "drop: guest released once");
    check_u64(st.guest_drops, 1, "drop: stat");
    check_u64(st.uploads, 1, "drop: upload stat");
    check(d9_texcache_find(&cache, 1) != NULL, "drop: entry stays resident");
    check(d9_texcache_find(&cache, 1)->has_gpu, "drop: gpu present");

    /* A lifetime that still needs CPU readback keeps both and says so. */
    check(d9_texcache_add_guest(&cache, 2, &g2, 100, 1) == D9TEXCACHE_OK,
          "add guest 2");
    check(d9_texcache_commit_upload(&cache, 2, &gpu2, 200, 0) == D9TEXCACHE_OK,
          "commit 2 without drop");
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 100, "keep: guest bytes");
    check_u64(st.gpu_bytes, 400, "keep: gpu bytes");
    check_u64(st.both_bytes, 300, "keep: current both");
    check_u64(st.peak_both, 300, "keep: peak both");
    check_u64((uint64_t)fake.guest_releases, 1, "keep: no extra guest release");
    d9_texcache_destroy(&cache);
    check_u64((uint64_t)fake.gpu_releases, 2, "destroy: gpu releases");
}

static void test_lru_order(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    int a = 1, b = 2, c = 3;
    init(&cache, entries, 8, &fake, 0, 0);
    check(d9_texcache_add_guest(&cache, 10, &a, 10, 1) == D9TEXCACHE_OK, "lru add 10");
    check(d9_texcache_add_guest(&cache, 11, &b, 10, 1) == D9TEXCACHE_OK, "lru add 11");
    check(d9_texcache_add_guest(&cache, 12, &c, 10, 1) == D9TEXCACHE_OK, "lru add 12");
    /* Latest use order: 10 (clock 1), 11 (2), 12 (3); touch 10. */
    check(d9_texcache_touch(&cache, 10) == D9TEXCACHE_OK, "lru touch 10");
    unsigned evicted = 0;
    check(d9_texcache_trim(&cache, 20, 0, &evicted) == D9TEXCACHE_OK,
          "lru trim guest");
    check_u64(evicted, 1, "lru one evicted");
    check(d9_texcache_find(&cache, 11) == NULL, "lru evicted oldest (11)");
    check(d9_texcache_find(&cache, 10) != NULL, "lru kept touched (10)");
    check(d9_texcache_find(&cache, 12) != NULL, "lru kept newest (12)");
    check_u64((uint64_t)fake.guest_releases, 1, "lru release count");
    check_u64(cache.guest_bytes, 20, "lru bytes");

    /* Pinning protects the oldest entry. */
    check(d9_texcache_pin(&cache, 10) == D9TEXCACHE_OK, "pin 10");
    check(d9_texcache_trim(&cache, 0, 0, &evicted) == D9TEXCACHE_GUEST_FULL,
          "trim blocked by pin reports GUEST_FULL");
    check(d9_texcache_find(&cache, 10) != NULL, "pinned survives");
    check(d9_texcache_find(&cache, 12) == NULL, "unpinned evicted");
    check(d9_texcache_remove(&cache, 10) == D9TEXCACHE_PINNED,
          "remove pinned refused");
    check(d9_texcache_unpin(&cache, 10) == D9TEXCACHE_OK, "unpin 10");
    check(d9_texcache_remove(&cache, 10) == D9TEXCACHE_OK, "remove unpinned");
    check_u64(cache.used, 0, "lru used count");
    d9_texcache_destroy(&cache);
}

static void test_gpu_failure_keeps_guest(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    int g1 = 1, g2 = 2, gpu1 = 3, gpu2 = 4;
    init(&cache, entries, 8, &fake, 0, 100);
    check(d9_texcache_add_guest(&cache, 20, &g1, 50, 1) == D9TEXCACHE_OK,
          "fail add 20");
    check(d9_texcache_commit_upload(&cache, 20, &gpu1, 60, 1) == D9TEXCACHE_OK,
          "fail commit 20");
    check(d9_texcache_pin(&cache, 20) == D9TEXCACHE_OK, "fail pin 20");
    check(d9_texcache_add_guest(&cache, 21, &g2, 50, 1) == D9TEXCACHE_OK,
          "fail add 21");
    check(d9_texcache_begin_upload(&cache, 21) == D9TEXCACHE_OK,
          "fail begin 21");
    check(d9_texcache_commit_upload(&cache, 21, &gpu2, 60, 1) ==
          D9TEXCACHE_GPU_FULL, "fail commit 21 returns GPU_FULL");
    D9TexEntry *e = d9_texcache_find(&cache, 21);
    check(e && e->has_guest, "fail: guest retained");
    check(e && !e->has_gpu, "fail: no GPU recorded");
    check(e && e->gpu_failed, "fail: diagnostic flag");
    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check_u64(st.upload_failures, 1, "fail: failure stat");
    check_u64(st.gpu_bytes, 60, "fail: gpu bytes unchanged");
    /* The caller still owns gpu2 on failure; a real backend must free it. */
    check(d9_texcache_unpin(&cache, 20) == D9TEXCACHE_OK, "fail unpin 20");
    check(d9_texcache_commit_upload(&cache, 21, &gpu2, 60, 1) == D9TEXCACHE_OK,
          "fail retry after eviction succeeds");
    check(d9_texcache_find(&cache, 20) == NULL, "fail retry evicted LRU 20");
    check(d9_texcache_find(&cache, 21) && d9_texcache_find(&cache, 21)->has_gpu,
          "fail retry has gpu");
    check(d9_texcache_find(&cache, 21)->has_guest == 0, "fail retry dropped guest");
    d9_texcache_destroy(&cache);
}

static void test_guest_limit_and_working_set(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    int a = 1, b = 2, c = 3;
    init(&cache, entries, 8, &fake, 100, 0);
    check(d9_texcache_add_guest(&cache, 30, &a, 60, 1) == D9TEXCACHE_OK,
          "guest limit add 30");
    check(d9_texcache_add_guest(&cache, 31, &b, 60, 1) == D9TEXCACHE_OK,
          "guest limit add evicts 30");
    check(d9_texcache_find(&cache, 30) == NULL, "guest limit 30 evicted");
    check(d9_texcache_find(&cache, 31) != NULL, "guest limit 31 retained");
    check(d9_texcache_add_guest(&cache, 32, &c, 200, 1) ==
          D9TEXCACHE_GUEST_FULL, "guest limit too large fails");
    check(d9_texcache_find(&cache, 32) == NULL, "guest limit no partial insert");
    check_u64(cache.guest_bytes, 60, "guest limit bytes");
    /* Working set: 31 is fresh, a (clock age) distinction. */
    check_u64(d9_texcache_working_set(&cache, 0), 1, "working set immediate");
    check_u64(d9_texcache_working_set(&cache, 100), 1, "working set window");
    d9_texcache_destroy(&cache);
}

static void test_release_failure_is_atomic(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    int g = 1;
    init(&cache, entries, 4, &fake, 0, 0);
    check(d9_texcache_add_guest(&cache, 50, &g, 10, 1) == D9TEXCACHE_OK,
          "release fail add");
    fake.fail_release = 1;
    check(d9_texcache_remove(&cache, 50) == D9TEXCACHE_RELEASE_FAILED,
          "release fail reported");
    check(d9_texcache_find(&cache, 50) != NULL, "release fail keeps entry");
    check(cache.guest_bytes == 10, "release fail keeps accounting");
    fake.fail_release = 0;
    check(d9_texcache_remove(&cache, 50) == D9TEXCACHE_OK, "release retry");
    check_u64(cache.used, 0, "release fail used");
    d9_texcache_destroy(&cache);
}

static void test_deterministic_repeat(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake;
    int a = 1, b = 2;
    init(&cache, entries, 8, &fake, 0, 0);
    for (int round = 0; round < 2; ++round) {
        check(d9_texcache_add_guest(&cache, 40, &a, 10, 1) == D9TEXCACHE_OK,
              "repeat add 40");
        check(d9_texcache_add_guest(&cache, 41, &b, 10, 1) == D9TEXCACHE_OK,
              "repeat add 41");
        check(d9_texcache_touch(&cache, 40) == D9TEXCACHE_OK, "repeat touch");
        unsigned evicted = 0;
        check(d9_texcache_trim(&cache, 10, 0, &evicted) == D9TEXCACHE_OK,
              "repeat trim");
        check_u64(evicted, 1, "repeat evicted count");
        check(d9_texcache_find(&cache, 41) == NULL, "repeat evicted 41");
        check(d9_texcache_remove(&cache, 40) == D9TEXCACHE_OK, "repeat remove");
    }
    check_u64(cache.used, 0, "repeat used");
    d9_texcache_destroy(&cache);
}

int main(void) {
    test_upload_drop_and_both();
    test_lru_order();
    test_gpu_failure_keeps_guest();
    test_guest_limit_and_working_set();
    test_release_failure_is_atomic();
    test_deterministic_repeat();
    printf("texture residency: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
