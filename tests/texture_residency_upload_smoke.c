/* Host test: direct GPU-only ETC1/SKTX registration, durable MANAGED tokens,
 * retryable release failures and exact residency accounting.
 *
 * This test is intentionally not registered in tools/run_host_tests.ps1 yet.
 * Build it with d3d9_texcache.c and d3d9_texture_data.c after runtime integration
 * has settled the core/COM ownership calls.
 */

#include "d3d9_texcache.h"
#include "d3d9_texture_data.h"

#include <stdio.h>
#include <stdlib.h>
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
    unsigned guest_releases;
    unsigned gpu_releases;
    int fail_guest;
    int fail_gpu;
} Fake;

static int release_guest(void *ctx, void *guest, size_t bytes) {
    Fake *fake = ctx;
    if (fake->fail_guest) return -1;
    (void)guest;
    (void)bytes;
    ++fake->guest_releases;
    return 0;
}

static int release_gpu_default(void *ctx, void *gpu, size_t bytes) {
    Fake *fake = ctx;
    if (fake->fail_gpu) return -1;
    (void)gpu;
    (void)bytes;
    ++fake->gpu_releases;
    return 0;
}

typedef struct {
    void *self;
    size_t bytes;
    int alive;
    int fail;
    unsigned calls;
    unsigned releases;
    unsigned errors;
    unsigned double_frees;
} NativeRelease;

typedef struct {
    unsigned char marker;
    NativeRelease release;
} Native;

static void native_init(Native *native, size_t bytes) {
    memset(native, 0, sizeof *native);
    native->marker = 0xA5;
    native->release.self = native;
    native->release.bytes = bytes;
    native->release.alive = 1;
}

static int release_native(void *ctx, void *gpu, size_t bytes) {
    NativeRelease *release = ctx;
    Native *native = gpu;
    if (!release || !native || native != release->self ||
        native->marker != 0xA5 || bytes != release->bytes) {
        if (release) ++release->errors;
        return -1;
    }
    ++release->calls;
    if (!release->alive) {
        ++release->double_frees;
        return -1;
    }
    if (release->fail) return -1;
    release->alive = 0;
    ++release->releases;
    return 0;
}

static D9TexGpuRegistration registration(Native *native, size_t bytes,
                                          int owned,
                                          D9TexRehydrateToken token) {
    D9TexGpuRegistration out;
    memset(&out, 0, sizeof out);
    out.native = native;
    out.gpu_bytes = bytes;
    out.owned = owned;
    out.release = release_native;
    out.release_ctx = &native->release;
    out.rehydrate_token = token;
    return out;
}

static void init_cache(D9TexCache *cache, D9TexEntry *entries,
                       unsigned capacity, Fake *fake, size_t guest_limit,
                       size_t gpu_limit, int default_gpu_release) {
    D9TexCacheOps ops;
    memset(&ops, 0, sizeof ops);
    memset(fake, 0, sizeof *fake);
    ops.ctx = fake;
    ops.release_guest = release_guest;
    ops.release_gpu = default_gpu_release ? release_gpu_default : NULL;
    check(d9_texcache_init(cache, entries, capacity, &ops, guest_limit,
                           gpu_limit) == D9TEXCACHE_OK, "init cache");
}

static void test_gpu_only_add_remove_and_ownership(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    Native a, b, candidate;
    native_init(&a, 64);
    native_init(&b, 96);
    native_init(&candidate, 32);
    init_cache(&cache, entries, 4, &fake, 0, 0, 0);

    D9TexGpuRegistration missing_release = registration(&candidate, 32, 1, 0);
    missing_release.release = NULL;
    check(d9_texcache_add_gpu(&cache, 1, &missing_release) == D9TEXCACHE_INVALID,
          "owned GPU registration requires a release callback");
    check(candidate.release.alive, "invalid registration leaves candidate owned");
    check_u64(cache.gpu_bytes, 0, "invalid registration does not account");

    D9TexGpuRegistration ra = registration(&a, 64, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &ra) == D9TEXCACHE_OK,
          "add owned GPU-only texture");
    D9TexGpuRegistration rb = registration(&b, 96, 0, 0);
    check(d9_texcache_add_gpu(&cache, 2, &rb) == D9TEXCACHE_OK,
          "add borrowed GPU-only texture");
    D9TexGpuRegistration duplicate = registration(&candidate, 32, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &duplicate) == D9TEXCACHE_ALREADY_RESIDENT,
          "duplicate GPU key is rejected");
    check(candidate.release.alive, "duplicate candidate remains caller-owned");

    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check_u64(st.entries, 2, "GPU-only entries");
    check_u64(st.gpus, 2, "GPU-only gpus");
    check_u64(st.guests, 0, "GPU-only has no guests");
    check_u64(st.guest_bytes, 0, "GPU-only guest bytes");
    check_u64(st.gpu_bytes, 160, "GPU-only current bytes");
    check_u64(st.both_bytes, 0, "GPU-only current both");
    check_u64(st.peak_guest, 0, "GPU-only peak guest");
    check_u64(st.peak_gpu, 160, "GPU-only peak gpu");
    check_u64(st.peak_total, 160, "GPU-only peak total");
    check_u64(st.peak_both, 0, "GPU-only peak both");
    check_u64(st.uploads, 2, "direct registrations count as uploads");

    D9TexEntry *entry = d9_texcache_find(&cache, 1);
    check(entry && entry->gpu == &a && entry->gpu_bytes == 64,
          "explicit native pointer and byte cost");
    check(entry && entry->has_gpu && !entry->has_guest && entry->gpu_owned,
          "owned GPU-only state");
    check(entry && entry->release_gpu == release_native &&
          entry->release_gpu_ctx == &a.release, "per-entry release callback");

    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_OK, "remove borrowed GPU");
    check_u64(b.release.calls, 0, "borrowed GPU callback is never invoked");
    check(b.release.alive, "borrowed GPU remains externally owned");
    check_u64(cache.gpu_bytes, 64, "borrowed removal accounting");
    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_NOT_FOUND,
          "second borrowed removal is not found");
    check_u64(b.release.calls, 0, "second removal cannot double-free");

    check(d9_texcache_remove(&cache, 1) == D9TEXCACHE_OK, "remove owned GPU");
    check_u64(a.release.calls, 1, "owned GPU callback once");
    check_u64(a.release.releases, 1, "owned GPU released once");
    check_u64(a.release.double_frees, 0, "owned GPU no double-free");
    check(!a.release.alive, "owned GPU callback consumed pointer");
    check(d9_texcache_remove(&cache, 1) == D9TEXCACHE_NOT_FOUND,
          "second owned removal is not found");
    check_u64(a.release.calls, 1, "second removal cannot invoke callback again");
    d9_texcache_stats(&cache, &st);
    check_u64(st.entries, 0, "all GPU-only entries removed");
    check_u64(st.gpu_bytes, 0, "all GPU-only bytes removed");
    check_u64(st.peak_gpu, 160, "removal preserves exact peak");
    check_u64(st.peak_total, 160, "removal preserves exact total peak");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy clean cache");
    check_u64(a.release.calls, 1, "destroy cannot double-free removed GPU");
}

static void test_gpu_full_eviction_and_managed_reload(void) {
    D9TexCache cache;
    D9TexEntry entries[6];
    Fake fake;
    Native a, b, c, managed, reload, other, trigger, oversized;
    native_init(&a, 60);
    native_init(&b, 60);
    native_init(&c, 60);
    native_init(&managed, 60);
    native_init(&reload, 60);
    native_init(&other, 60);
    native_init(&trigger, 60);
    native_init(&oversized, 101);
    init_cache(&cache, entries, 6, &fake, 0, 100, 0);

    D9TexGpuRegistration ra = registration(&a, 60, 1, 0);
    D9TexGpuRegistration rb = registration(&b, 60, 1, 0);
    D9TexGpuRegistration rc = registration(&c, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &ra) == D9TEXCACHE_OK, "limit add 1");
    check(d9_texcache_add_gpu(&cache, 2, &rb) == D9TEXCACHE_OK,
          "limit add 2 evicts LRU 1");
    check(d9_texcache_find(&cache, 1) == NULL, "ephemeral LRU 1 removed");
    check_u64(a.release.releases, 1, "LRU 1 released once");
    check_u64(cache.gpu_bytes, 60, "eviction keeps exact current bytes");
    check_u64(cache.peak_gpu, 60, "evict-before-insert peak stays exact");

    check(d9_texcache_pin(&cache, 2) == D9TEXCACHE_OK, "pin limit blocker");
    check(d9_texcache_add_gpu(&cache, 3, &rc) == D9TEXCACHE_GPU_FULL,
          "pinned GPU blocker returns GPU_FULL");
    check(c.release.alive, "GPU_FULL candidate remains caller-owned");
    check(d9_texcache_find(&cache, 3) == NULL, "GPU_FULL inserts nothing");
    check_u64(cache.gpu_bytes, 60, "GPU_FULL current bytes unchanged");
    check_u64(cache.evictions_gpu, 1, "GPU_FULL does not evict pinned entry");
    check_u64(cache.upload_failures, 1, "GPU_FULL failure stat");
    check(d9_texcache_unpin(&cache, 2) == D9TEXCACHE_OK, "unpin blocker");
    check(d9_texcache_add_gpu(&cache, 3, &rc) == D9TEXCACHE_OK,
          "retry after unpin succeeds");
    check_u64(b.release.releases, 1, "retry evicts previous blocker once");

    D9TexGpuRegistration too_big = registration(&oversized, 101, 1, 0);
    check(d9_texcache_add_gpu(&cache, 4, &too_big) == D9TEXCACHE_GPU_FULL,
          "oversized GPU returns GPU_FULL before eviction");
    check(oversized.release.alive, "oversized candidate remains caller-owned");
    check_u64(c.release.releases, 0, "oversized request evicts nothing");
    check_u64(cache.gpu_bytes, 60, "oversized request bytes unchanged");
    check_u64(cache.upload_failures, 2, "oversized failure stat");

    check(d9_texcache_remove(&cache, 3) == D9TEXCACHE_OK, "remove transient 3");
    check(d9_texcache_set_limits(&cache, 0, 120) == D9TEXCACHE_OK,
          "raise GPU limit for managed fixture");
    D9TexGpuRegistration rm = registration(&managed, 60, 1, 0x1234u);
    check(d9_texcache_add_gpu(&cache, 10, &rm) == D9TEXCACHE_OK,
          "register MANAGED GPU with token");
    D9TexRehydrateToken token = 0;
    check(d9_texcache_get_rehydrate_token(&cache, 10, &token) == D9TEXCACHE_OK &&
          token == 0x1234u, "read MANAGED token");

    D9TexGpuRegistration ro = registration(&other, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 11, &ro) == D9TEXCACHE_OK,
          "second resident fits raised limit");
    check_u64(cache.gpu_bytes, 120, "two resident GPU byte total");
    check_u64(cache.peak_gpu, 120, "two resident GPU peak");
    check_u64(cache.peak_total, 120, "two resident total peak");
    check(d9_texcache_set_limits(&cache, 0, 60) == D9TEXCACHE_OK,
          "lower GPU limit to force MANAGED eviction");

    D9TexGpuRegistration rt = registration(&trigger, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 12, &rt) == D9TEXCACHE_OK,
          "new GPU evicts LRU MANAGED copy");
    check_u64(managed.release.releases, 1, "evicted MANAGED GPU released once");
    check_u64(other.release.releases, 1, "second eviction releases other GPU once");
    D9TexEntry *slot = d9_texcache_find(&cache, 10);
    check(slot && !slot->has_gpu && !slot->has_guest && slot->has_rehydrate,
          "MANAGED token survives GPU eviction");
    check_u64(cache.gpu_bytes, 60, "MANAGED eviction current bytes");
    check_u64(cache.peak_gpu, 120, "MANAGED eviction preserves peak");
    check(d9_texcache_get_rehydrate_token(&cache, 10, &token) == D9TEXCACHE_OK &&
          token == 0x1234u, "evicted MANAGED token remains durable");
    check(d9_texcache_find(&cache, 11) == NULL, "older ephemeral entry evicted");

    check(d9_texcache_set_limits(&cache, 0, 120) == D9TEXCACHE_OK,
          "raise limit for MANAGED reload");
    D9TexGpuRegistration rr = registration(&reload, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 10, &rr) == D9TEXCACHE_OK,
          "reload token-only MANAGED slot");
    check(slot && slot->has_gpu && slot->gpu == &reload,
          "reload records replacement native pointer");
    check_u64(reload.release.releases, 0, "reload native remains resident");
    check_u64(cache.gpu_bytes, 120, "reload exact current bytes");
    check_u64(cache.peak_gpu, 120, "reload exact peak bytes");
    check_u64(cache.peak_total, 120, "reload exact peak total");
    check(d9_texcache_get_rehydrate_token(&cache, 10, &token) == D9TEXCACHE_OK &&
          token == 0x1234u, "zero registration token preserves durable token");
    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check_u64(st.entries, 2, "reload entries");
    check_u64(st.gpus, 2, "reload gpus");
    check_u64(st.rehydratable, 1, "reload rehydratable count");
    check_u64(st.guest_bytes, 0, "reload guest bytes");
    check_u64(st.both_bytes, 0, "reload both bytes");
    check_u64(st.peak_both, 0, "reload peak both");
    check_u64(st.evictions_gpu, 4, "deterministic GPU eviction count");
    check_u64(st.uploads, 7, "direct upload count across retry and reload");
    check_u64(st.upload_failures, 2, "direct upload failure count");

    check(d9_texcache_remove(&cache, 10) == D9TEXCACHE_OK, "remove reloaded MANAGED");
    check_u64(reload.release.releases, 1, "reloaded native released once");
    check(d9_texcache_get_rehydrate_token(&cache, 10, &token) ==
          D9TEXCACHE_NOT_FOUND && token == 0, "remove drops durable token");
    check(d9_texcache_remove(&cache, 12) == D9TEXCACHE_OK, "remove evictor 12");
    check_u64(trigger.release.releases, 1, "evictor native released once");
    check_u64(cache.gpu_bytes, 0, "managed fixture final bytes");
    check_u64(cache.used, 0, "managed fixture final entries");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy managed fixture");
    check_u64(managed.release.double_frees, 0, "managed native no double-free");
    check_u64(reload.release.double_frees, 0, "reload native no double-free");
}

static void test_release_failures_are_atomic_and_retryable(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    Native a, b, c;
    native_init(&a, 60);
    native_init(&b, 60);
    native_init(&c, 60);
    init_cache(&cache, entries, 4, &fake, 0, 100, 0);

    D9TexGpuRegistration ra = registration(&a, 60, 1, 0);
    D9TexGpuRegistration rb = registration(&b, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &ra) == D9TEXCACHE_OK, "release add a");
    a.release.fail = 1;
    check(d9_texcache_add_gpu(&cache, 2, &rb) == D9TEXCACHE_RELEASE_FAILED,
          "eviction release failure is reported");
    check(a.release.alive && b.release.alive, "failed eviction keeps both owned");
    check(d9_texcache_find(&cache, 1) != NULL, "failed eviction keeps victim");
    check(d9_texcache_find(&cache, 2) == NULL, "failed eviction inserts no candidate");
    check_u64(cache.gpu_bytes, 60, "failed eviction bytes unchanged");
    check_u64(cache.used, 1, "failed eviction entries unchanged");
    check_u64(cache.evictions_gpu, 0, "failed eviction stat unchanged");
    check_u64(a.release.calls, 1, "failed victim callback attempted once");
    check_u64(a.release.releases, 0, "failed victim not released");

    a.release.fail = 0;
    check(d9_texcache_add_gpu(&cache, 2, &rb) == D9TEXCACHE_OK,
          "eviction retry succeeds");
    check_u64(a.release.calls, 2, "victim callback retried once");
    check_u64(a.release.releases, 1, "victim released exactly once");
    check_u64(a.release.double_frees, 0, "victim never double-freed");
    check_u64(cache.gpu_bytes, 60, "eviction retry exact bytes");

    b.release.fail = 1;
    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_RELEASE_FAILED,
          "remove release failure is reported");
    check(d9_texcache_find(&cache, 2) != NULL, "failed remove keeps entry");
    check_u64(cache.gpu_bytes, 60, "failed remove keeps accounting");
    check_u64(b.release.calls, 1, "failed remove callback attempted once");
    b.release.fail = 0;
    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_OK, "remove retry succeeds");
    check_u64(b.release.calls, 2, "remove callback retried once");
    check_u64(b.release.releases, 1, "remove releases exactly once");
    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_NOT_FOUND,
          "removed entry cannot be released again");
    check_u64(b.release.calls, 2, "no callback after successful remove");

    D9TexGpuRegistration rc = registration(&c, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 3, &rc) == D9TEXCACHE_OK, "release add c");
    c.release.fail = 1;
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_RELEASE_FAILED,
          "destroy reports release failure");
    check(c.release.alive, "destroy failure keeps native alive");
    check_u64(cache.gpu_bytes, 60, "destroy failure keeps byte accounting");
    check_u64(cache.used, 1, "destroy failure keeps entry accounting");
    c.release.fail = 0;
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy retry succeeds");
    check_u64(c.release.calls, 2, "destroy callback retried once");
    check_u64(c.release.releases, 1, "destroy releases exactly once");
    check_u64(c.release.double_frees, 0, "destroy never double-frees");
    check_u64(cache.gpu_bytes, 0, "destroy retry final bytes");
    check_u64(cache.used, 0, "destroy retry final entries");
    d9_texcache_destroy(&cache);
    check_u64(c.release.calls, 2, "second destroy cannot double-free");
}

static void test_partial_eviction_failure_is_accounted(void) {
    D9TexCache cache;
    D9TexEntry entries[5];
    Fake fake;
    Native a, b, c, d;
    native_init(&a, 60);
    native_init(&b, 60);
    native_init(&c, 60);
    native_init(&d, 60);
    init_cache(&cache, entries, 5, &fake, 0, 180, 0);

    D9TexGpuRegistration ra = registration(&a, 60, 1, 0);
    D9TexGpuRegistration rb = registration(&b, 60, 1, 0);
    D9TexGpuRegistration rc = registration(&c, 60, 1, 0);
    D9TexGpuRegistration rd = registration(&d, 60, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &ra) == D9TEXCACHE_OK, "partial add a");
    check(d9_texcache_add_gpu(&cache, 2, &rb) == D9TEXCACHE_OK, "partial add b");
    check(d9_texcache_add_gpu(&cache, 3, &rc) == D9TEXCACHE_OK, "partial add c");
    check_u64(cache.gpu_bytes, 180, "partial fixture starting bytes");
    check_u64(cache.peak_gpu, 180, "partial fixture starting peak");
    check(d9_texcache_set_limits(&cache, 0, 100) == D9TEXCACHE_OK,
          "lower limit for partial eviction");

    b.release.fail = 1;
    check(d9_texcache_add_gpu(&cache, 4, &rd) == D9TEXCACHE_RELEASE_FAILED,
          "second eviction release failure is reported");
    check_u64(a.release.releases, 1, "earlier eviction remains committed");
    check_u64(b.release.releases, 0, "failed victim remains live");
    check(b.release.alive && c.release.alive && d.release.alive,
          "partial failure leaves failed victim and candidate alive");
    check(d9_texcache_find(&cache, 1) == NULL, "earlier evicted key is absent");
    check(d9_texcache_find(&cache, 2) != NULL, "failed victim key remains");
    check(d9_texcache_find(&cache, 4) == NULL, "partial failure inserts no candidate");
    check_u64(cache.gpu_bytes, 120, "partial failure exact remaining bytes");
    check_u64(cache.used, 2, "partial failure exact remaining entries");
    check_u64(cache.evictions_gpu, 1, "partial failure counts only successful eviction");
    check_u64(cache.peak_gpu, 180, "partial failure preserves prior peak");

    b.release.fail = 0;
    check(d9_texcache_add_gpu(&cache, 4, &rd) == D9TEXCACHE_OK,
          "partial eviction retry succeeds");
    check_u64(b.release.releases, 1, "retry releases failed victim once");
    check_u64(c.release.releases, 1, "retry releases next LRU once");
    check_u64(cache.gpu_bytes, 60, "partial retry exact bytes");
    check_u64(cache.used, 1, "partial retry exact entries");
    check_u64(cache.evictions_gpu, 3, "partial retry exact eviction count");
    check(d9_texcache_find(&cache, 4) != NULL, "partial retry candidate resident");
    check(d9_texcache_remove(&cache, 4) == D9TEXCACHE_OK, "remove partial candidate");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy partial fixture");
    check_u64(a.release.releases, 1, "partial a released once");
    check_u64(b.release.releases, 1, "partial b released once");
    check_u64(c.release.releases, 1, "partial c released once");
    check_u64(d.release.releases, 1, "partial d released once");
    check_u64(a.release.double_frees + b.release.double_frees +
              c.release.double_frees + d.release.double_frees, 0,
          "partial eviction never double-frees");
}

static void test_exact_peaks_with_guest_and_direct_gpu(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    int guest1 = 1, guest2 = 2, committed_gpu = 3;
    Native direct;
    native_init(&direct, 70);
    init_cache(&cache, entries, 4, &fake, 0, 0, 1);

    check(d9_texcache_add_guest(&cache, 1, &guest1, 40, 1) == D9TEXCACHE_OK,
          "peak add guest 1");
    D9TexGpuRegistration rd = registration(&direct, 70, 1, 0);
    check(d9_texcache_add_gpu(&cache, 2, &rd) == D9TEXCACHE_OK,
          "peak add direct GPU");
    D9TexCacheStats st;
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 40, "peak stage guest bytes");
    check_u64(st.gpu_bytes, 70, "peak stage gpu bytes");
    check_u64(st.peak_guest, 40, "peak stage peak guest");
    check_u64(st.peak_gpu, 70, "peak stage peak gpu");
    check_u64(st.peak_total, 110, "peak stage peak total");
    check_u64(st.peak_both, 0, "separate entries are not both copies");
    check_u64(st.both_bytes, 0, "peak stage current both");

    check(d9_texcache_begin_upload(&cache, 1) == D9TEXCACHE_OK, "peak begin upload");
    check(d9_texcache_commit_upload(&cache, 1, &committed_gpu, 60, 1) ==
          D9TEXCACHE_OK, "peak commit and drop guest");
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 0, "drop current guest bytes");
    check_u64(st.gpu_bytes, 130, "drop current gpu bytes");
    check_u64(st.both_bytes, 0, "drop current both bytes");
    check_u64(st.peak_guest, 40, "drop exact peak guest");
    check_u64(st.peak_gpu, 130, "drop exact peak gpu");
    check_u64(st.peak_total, 170, "drop exact peak total");
    check_u64(st.peak_both, 100, "drop captures coexistence before release");
    check_u64(st.guest_drops, 1, "drop stat");
    check_u64(st.uploads, 2, "direct plus committed uploads");
    check_u64(fake.guest_releases, 1, "guest released once");

    check(d9_texcache_add_guest(&cache, 3, &guest2, 25, 1) == D9TEXCACHE_OK,
          "peak add later guest");
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 25, "later current guest bytes");
    check_u64(st.gpu_bytes, 130, "later current gpu bytes");
    check_u64(st.peak_guest, 40, "later peak guest remains exact");
    check_u64(st.peak_gpu, 130, "later peak gpu remains exact");
    check_u64(st.peak_total, 170, "later peak total remains exact");
    check_u64(st.peak_both, 100, "later peak both remains exact");

    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_OK, "peak remove direct");
    check_u64(direct.release.releases, 1, "direct native released once");
    check(d9_texcache_remove(&cache, 1) == D9TEXCACHE_OK, "peak remove committed");
    check_u64(fake.gpu_releases, 1, "committed GPU released once");
    check(d9_texcache_remove(&cache, 3) == D9TEXCACHE_OK, "peak remove later guest");
    check_u64(fake.guest_releases, 2, "both guests released once");
    d9_texcache_stats(&cache, &st);
    check_u64(st.guest_bytes, 0, "peak fixture final guest bytes");
    check_u64(st.gpu_bytes, 0, "peak fixture final gpu bytes");
    check_u64(st.peak_total, 170, "peak fixture final total peak");
    check_u64(st.peak_both, 100, "peak fixture final both peak");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy peak fixture");
    check_u64(direct.release.calls, 1, "peak fixture no direct double-free");
    check_u64(fake.gpu_releases, 1, "peak fixture no committed double-free");
}

static void test_commit_release_failure_keeps_caller_gpu(void) {
    D9TexCache cache;
    D9TexEntry entries[2];
    Fake fake;
    int guest = 1, gpu = 2;
    init_cache(&cache, entries, 2, &fake, 0, 0, 1);
    check(d9_texcache_add_guest(&cache, 5, &guest, 40, 1) == D9TEXCACHE_OK,
          "commit failure add guest");
    check(d9_texcache_begin_upload(&cache, 5) == D9TEXCACHE_OK,
          "commit failure begin");
    fake.fail_guest = 1;
    check(d9_texcache_commit_upload(&cache, 5, &gpu, 60, 1) ==
          D9TEXCACHE_RELEASE_FAILED, "guest release failure aborts commit");
    D9TexEntry *entry = d9_texcache_find(&cache, 5);
    check(entry && entry->has_guest && !entry->has_gpu,
          "failed commit retains guest and does not record GPU");
    check_u64(cache.guest_bytes, 40, "failed commit guest accounting");
    check_u64(cache.gpu_bytes, 0, "failed commit GPU accounting");
    check_u64(cache.uploads, 0, "failed commit upload stat");
    check_u64(cache.upload_failures, 1, "failed commit failure stat");
    check_u64(fake.gpu_releases, 0, "failed commit never owns caller GPU");
    check_u64(cache.peak_gpu, 60, "failed commit observed allocated GPU peak");
    check_u64(cache.peak_total, 100, "failed commit observed total peak");
    check_u64(cache.peak_both, 100, "failed commit observed coexistence peak");

    fake.fail_guest = 0;
    check(d9_texcache_commit_upload(&cache, 5, &gpu, 60, 1) == D9TEXCACHE_OK,
          "same GPU pointer can be retried after release failure");
    check_u64(fake.guest_releases, 1, "commit retry releases guest once");
    check_u64(cache.gpu_bytes, 60, "commit retry exact GPU bytes");
    check_u64(cache.uploads, 1, "commit retry upload stat");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy commit fixture");
    check_u64(fake.gpu_releases, 1, "successful retry GPU released once");
}

static void test_guest_full_and_token_only_slot(void) {
    D9TexCache cache;
    D9TexEntry entries[4];
    Fake fake;
    int guest1 = 1, guest2 = 2, too_big = 3;
    Native native;
    native_init(&native, 30);
    init_cache(&cache, entries, 4, &fake, 100, 0, 0);

    check(d9_texcache_add_guest(&cache, 1, &guest1, 60, 1) == D9TEXCACHE_OK,
          "token guest add 1");
    check(d9_texcache_set_rehydrate_token(&cache, 1, 77) == D9TEXCACHE_OK,
          "attach token to guest entry");
    check(d9_texcache_add_guest(&cache, 2, &guest2, 60, 1) == D9TEXCACHE_OK,
          "token guest add 2 evicts source 1");
    D9TexEntry *slot = d9_texcache_find(&cache, 1);
    check(slot && slot->has_rehydrate && !slot->has_guest && !slot->has_gpu,
          "guest eviction leaves token-only slot");
    check_u64(cache.guest_bytes, 60, "token guest exact bytes");
    check_u64(cache.evictions_guest, 1, "token guest eviction stat");
    check(d9_texcache_add_guest(&cache, 3, &too_big, 101, 1) ==
          D9TEXCACHE_GUEST_FULL, "oversized guest returns GUEST_FULL");
    check(d9_texcache_find(&cache, 3) == NULL, "GUEST_FULL inserts nothing");
    check_u64(cache.guest_bytes, 60, "GUEST_FULL bytes unchanged");

    D9TexGpuRegistration rn = registration(&native, 30, 1, 0);
    check(d9_texcache_add_gpu(&cache, 1, &rn) == D9TEXCACHE_OK,
          "token-only slot accepts direct GPU reload");
    D9TexRehydrateToken token = 0;
    check(d9_texcache_get_rehydrate_token(&cache, 1, &token) == D9TEXCACHE_OK &&
          token == 77, "guest-backed token survives direct reload");
    D9TexGpuRegistration duplicate = registration(&native, 30, 1, 0);
    check(d9_texcache_add_gpu(&cache, 2, &duplicate) == D9TEXCACHE_ALREADY_RESIDENT,
          "direct GPU cannot replace an existing guest source");
    check_u64(cache.guest_bytes, 60, "rejected direct add keeps guest bytes");
    check_u64(cache.gpu_bytes, 30, "token reload exact GPU bytes");
    check_u64(cache.peak_total, 90, "token reload exact total peak");
    check_u64(cache.peak_both, 0, "token reload has no both copies");

    check(d9_texcache_set_rehydrate_token(&cache, 1, 0) == D9TEXCACHE_OK,
          "clear token while GPU remains");
    check(d9_texcache_get_rehydrate_token(&cache, 1, &token) ==
          D9TEXCACHE_NOT_FOUND && token == 0, "cleared token is absent");
    check(d9_texcache_find(&cache, 1) != NULL, "GPU remains after token clear");
    check(d9_texcache_remove(&cache, 1) == D9TEXCACHE_OK, "remove token reload");
    check_u64(native.release.releases, 1, "token reload native released once");
    check(d9_texcache_remove(&cache, 2) == D9TEXCACHE_OK, "remove guest 2");
    check_u64(cache.guest_bytes, 0, "guest fixture final guest bytes");
    check_u64(cache.gpu_bytes, 0, "guest fixture final GPU bytes");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy guest fixture");
}

static void wr16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t build_etc1(uint8_t *out) {
    const size_t table = 24u + 12u;
    const size_t data = 32u;
    memset(out, 0, table + data);
    memcpy(out, "SKTX", 4);
    wr32(out + 4, 1);
    wr16(out + 8, D9SKTX_FORMAT_ETC1);
    wr16(out + 10, 1);
    wr16(out + 12, 8);
    wr16(out + 14, 8);
    wr32(out + 16, (uint32_t)data);
    wr16(out + 24, 8);
    wr16(out + 26, 8);
    wr32(out + 28, (uint32_t)table);
    wr32(out + 32, (uint32_t)data);
    for (size_t i = 0; i < data; ++i) out[table + i] = (uint8_t)(i * 3u);
    wr32(out + 20, d9_sktx_crc32(out + table, data));
    return table + data;
}

static void test_temporary_sktx_is_not_retained(void) {
    D9TexCache cache;
    D9TexEntry entries[3];
    Fake fake;
    Native first, second, reload;
    native_init(&first, 32);
    native_init(&second, 32);
    native_init(&reload, 32);
    init_cache(&cache, entries, 3, &fake, 0, 32, 0);

    uint8_t fixture[68];
    size_t bytes = build_etc1(fixture);
    uint8_t *blob = malloc(bytes);
    check(blob != NULL, "allocate temporary SKTX");
    if (!blob) return;
    memcpy(blob, fixture, bytes);
    D9Sktx sktx;
    check(d9_sktx_parse(blob, bytes, &sktx) == 0, "parse temporary SKTX");
    check(sktx.level[0].bytes == 32, "temporary SKTX GPU byte cost");

    D9TexGpuRegistration r1 = registration(&first, sktx.level[0].bytes, 1, 99);
    check(d9_texcache_add_gpu(&cache, 77, &r1) == D9TEXCACHE_OK,
          "register native uploaded from temporary SKTX");
    free(blob);
    D9TexEntry *entry = d9_texcache_find(&cache, 77);
    check(entry && !entry->has_guest && entry->guest == NULL,
          "cache retains no temporary SKTX pointer");
    check(entry && entry->gpu == &first && entry->gpu_bytes == 32,
          "cache retains only native pointer and GPU cost");

    D9TexGpuRegistration r2 = registration(&second, 32, 1, 0);
    check(d9_texcache_add_gpu(&cache, 88, &r2) == D9TEXCACHE_OK,
          "evict SKTX-backed native");
    check_u64(first.release.releases, 1, "SKTX-backed native evicted once");
    D9TexRehydrateToken token = 0;
    check(d9_texcache_get_rehydrate_token(&cache, 77, &token) == D9TEXCACHE_OK &&
          token == 99, "durable token remains after SKTX buffer is freed");
    check(d9_texcache_find(&cache, 77) && !d9_texcache_find(&cache, 77)->has_gpu,
          "SKTX-backed key is token-only after eviction");

    D9TexGpuRegistration rr = registration(&reload, 32, 1, 0);
    check(d9_texcache_add_gpu(&cache, 77, &rr) == D9TEXCACHE_OK,
          "rehydrate SKTX-backed key without old container");
    check_u64(second.release.releases, 1, "reload evicts previous native once");
    check_u64(cache.gpu_bytes, 32, "SKTX reload exact current bytes");
    check_u64(cache.peak_gpu, 32, "SKTX reload exact peak bytes");
    check_u64(cache.peak_total, 32, "SKTX reload exact total peak");
    check_u64(cache.evictions_gpu, 2, "SKTX deterministic eviction count");
    check(d9_texcache_remove(&cache, 77) == D9TEXCACHE_OK, "remove rehydrated SKTX");
    check_u64(reload.release.releases, 1, "rehydrated native released once");
    check(d9_texcache_destroy_ex(&cache) == D9TEXCACHE_OK, "destroy SKTX fixture");
    check_u64(first.release.double_frees, 0, "SKTX first native no double-free");
    check_u64(second.release.double_frees, 0, "SKTX second native no double-free");
    check_u64(reload.release.double_frees, 0, "SKTX reload native no double-free");
}

int main(void) {
    test_gpu_only_add_remove_and_ownership();
    test_gpu_full_eviction_and_managed_reload();
    test_release_failures_are_atomic_and_retryable();
    test_partial_eviction_failure_is_accounted();
    test_exact_peaks_with_guest_and_direct_gpu();
    test_commit_release_failure_keeps_caller_gpu();
    test_guest_full_and_token_only_slot();
    test_temporary_sktx_is_not_retained();
    printf("texture residency upload: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
