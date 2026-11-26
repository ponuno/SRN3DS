#include "d3d9_texcache.h"

#include <string.h>

static size_t add_size(size_t a, size_t b) {
    return b > SIZE_MAX - a ? SIZE_MAX : a + b;
}

static void update_peaks(D9TexCache *cache) {
    size_t total = add_size(cache->guest_bytes, cache->gpu_bytes);
    size_t both = 0;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        const D9TexEntry *entry = &cache->entry[i];
        if (entry->has_guest && entry->has_gpu)
            both = add_size(both, add_size(entry->guest_bytes, entry->gpu_bytes));
    }
    if (cache->guest_bytes > cache->peak_guest) cache->peak_guest = cache->guest_bytes;
    if (cache->gpu_bytes > cache->peak_gpu) cache->peak_gpu = cache->gpu_bytes;
    if (total > cache->peak_total) cache->peak_total = total;
    if (both > cache->peak_both) cache->peak_both = both;
}

/* Observe the successful backend allocation before commit_upload optionally
 * drops its guest source.  Nothing is recorded yet, so a guest-release failure
 * still leaves the caller owning gpu. */
static void observe_gpu_addition(D9TexCache *cache,
                                 const D9TexEntry *target, size_t gpu_bytes) {
    size_t gpu = add_size(cache->gpu_bytes, gpu_bytes);
    size_t total = add_size(cache->guest_bytes, gpu);
    size_t both = 0;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        const D9TexEntry *entry = &cache->entry[i];
        size_t entry_gpu = entry->has_gpu ? entry->gpu_bytes : 0;
        if (entry == target) entry_gpu = gpu_bytes;
        if (entry->has_guest && entry_gpu)
            both = add_size(both, add_size(entry->guest_bytes, entry_gpu));
    }
    if (gpu > cache->peak_gpu) cache->peak_gpu = gpu;
    if (total > cache->peak_total) cache->peak_total = total;
    if (both > cache->peak_both) cache->peak_both = both;
}

static int clear_guest(D9TexCache *cache, D9TexEntry *entry) {
    if (!entry->has_guest) return D9TEXCACHE_OK;
    if (entry->guest_owned) {
        if (!cache->ops.release_guest ||
            cache->ops.release_guest(cache->ops.ctx, entry->guest,
                                     entry->guest_bytes))
            return D9TEXCACHE_RELEASE_FAILED;
    }
    if (cache->guest_bytes >= entry->guest_bytes)
        cache->guest_bytes -= entry->guest_bytes;
    else
        cache->guest_bytes = 0;
    entry->guest = NULL;
    entry->guest_bytes = 0;
    entry->has_guest = 0;
    entry->guest_owned = 0;
    return D9TEXCACHE_OK;
}

static int clear_gpu(D9TexCache *cache, D9TexEntry *entry) {
    if (!entry->has_gpu) return D9TEXCACHE_OK;
    if (entry->gpu_owned) {
        D9TexGpuRelease release = entry->release_gpu;
        void *ctx = entry->release_gpu_ctx;
        if (!release) {
            release = cache->ops.release_gpu;
            ctx = cache->ops.ctx;
        }
        if (!release || release(ctx, entry->gpu, entry->gpu_bytes))
            return D9TEXCACHE_RELEASE_FAILED;
    }
    if (cache->gpu_bytes >= entry->gpu_bytes)
        cache->gpu_bytes -= entry->gpu_bytes;
    else
        cache->gpu_bytes = 0;
    entry->gpu = NULL;
    entry->gpu_bytes = 0;
    entry->release_gpu = NULL;
    entry->release_gpu_ctx = NULL;
    entry->has_gpu = 0;
    entry->gpu_ready = 0;
    entry->gpu_owned = 0;
    return D9TEXCACHE_OK;
}

static int entry_is_live(const D9TexEntry *entry) {
    return entry && (entry->has_guest || entry->has_gpu || entry->has_rehydrate);
}

static void discard_dead_entry(D9TexCache *cache, D9TexEntry *entry,
                               int was_live) {
    if (entry_is_live(entry)) return;
    memset(entry, 0, sizeof *entry);
    if (was_live && cache->used) --cache->used;
}

static int find_index(const D9TexCache *cache, uint64_t key) {
    for (unsigned i = 0; i < cache->capacity; ++i)
        if (cache->entry[i].key == key && entry_is_live(&cache->entry[i]))
            return (int)i;
    return -1;
}

static int find_free_index(const D9TexCache *cache) {
    for (unsigned i = 0; i < cache->capacity; ++i)
        if (!entry_is_live(&cache->entry[i])) return (int)i;
    return -1;
}

D9TexEntry *d9_texcache_find(D9TexCache *cache, uint64_t key) {
    if (!cache) return NULL;
    int i = find_index(cache, key);
    return i < 0 ? NULL : &cache->entry[i];
}

const D9TexEntry *d9_texcache_find_const(const D9TexCache *cache,
                                          uint64_t key) {
    if (!cache) return NULL;
    int i = find_index(cache, key);
    return i < 0 ? NULL : &cache->entry[i];
}

int d9_texcache_init(D9TexCache *cache, D9TexEntry *entries,
                     unsigned capacity, const D9TexCacheOps *ops,
                     size_t guest_limit, size_t gpu_limit) {
    if (!cache || !entries || !ops ||
        !capacity || capacity > D9TEXCACHE_MAX_ENTRIES)
        return D9TEXCACHE_INVALID;
    memset(cache, 0, sizeof *cache);
    memset(entries, 0, capacity * sizeof *entries);
    cache->ops = *ops;
    cache->entry = entries;
    cache->capacity = capacity;
    cache->guest_limit = guest_limit;
    cache->gpu_limit = gpu_limit;
    return D9TEXCACHE_OK;
}

int d9_texcache_destroy_ex(D9TexCache *cache) {
    if (!cache) return D9TEXCACHE_INVALID;
    int first = D9TEXCACHE_OK;
    if (!cache->entry) return first;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        D9TexEntry *entry = &cache->entry[i];
        int was_live = entry_is_live(entry);
        int rc = clear_guest(cache, entry);
        if (first == D9TEXCACHE_OK && rc != D9TEXCACHE_OK) first = rc;
        rc = clear_gpu(cache, entry);
        if (first == D9TEXCACHE_OK && rc != D9TEXCACHE_OK) first = rc;
        discard_dead_entry(cache, entry, was_live);
    }
    update_peaks(cache);
    return first;
}

void d9_texcache_destroy(D9TexCache *cache) {
    if (cache) (void)d9_texcache_destroy_ex(cache);
}

int d9_texcache_set_limits(D9TexCache *cache, size_t guest_limit,
                           size_t gpu_limit) {
    if (!cache) return D9TEXCACHE_INVALID;
    cache->guest_limit = guest_limit;
    cache->gpu_limit = gpu_limit;
    return D9TEXCACHE_OK;
}

int d9_texcache_set_no_evict(D9TexCache *cache, uint64_t key, int no_evict) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    entry->no_evict = no_evict ? 1 : 0;
    return D9TEXCACHE_OK;
}

static int evict_one(D9TexCache *cache, int want_gpu, int want_guest,
                     uint64_t exclude_key, int exclude, unsigned *evicted) {
    int best = -1;
    uint64_t best_clock = UINT64_MAX;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        D9TexEntry *entry = &cache->entry[i];
        if (exclude && entry->key == exclude_key) continue;
        if (!entry->pins && !entry->no_evict &&
            ((want_gpu && entry->has_gpu) ||
             (want_guest && entry->has_guest))) {
            if (entry->last_use <= best_clock) {
                best_clock = entry->last_use;
                best = (int)i;
            }
        }
    }
    if (best < 0) return D9TEXCACHE_PINNED;
    D9TexEntry *entry = &cache->entry[best];
    int rc;
    if (want_gpu && entry->has_gpu)
        rc = clear_gpu(cache, entry);
    else
        rc = clear_guest(cache, entry);
    if (rc != D9TEXCACHE_OK) return rc;
    if (want_gpu && !want_guest) ++cache->evictions_gpu;
    else ++cache->evictions_guest;
    if (evicted) ++*evicted;
    /* A MANAGED token keeps the key reserved after its GPU copy is evicted. */
    discard_dead_entry(cache, entry, 1);
    return D9TEXCACHE_OK;
}

static int pool_has_room(size_t used, size_t bytes, size_t limit, int gpu) {
    if (!bytes) return D9TEXCACHE_OK;
    if ((limit && bytes > limit) || bytes > SIZE_MAX - used)
        return gpu ? D9TEXCACHE_GPU_FULL : D9TEXCACHE_GUEST_FULL;
    return D9TEXCACHE_OK;
}

static int reserve_bytes(D9TexCache *cache, int want_gpu, int want_guest,
                         size_t bytes, size_t limit, uint64_t exclude_key,
                         int exclude, unsigned *evicted) {
    size_t used = want_gpu ? cache->gpu_bytes : cache->guest_bytes;
    int rc = pool_has_room(used, bytes, limit, want_gpu);
    if (rc != D9TEXCACHE_OK || !bytes) return rc;
    while (limit && used > limit - bytes) {
        rc = evict_one(cache, want_gpu, want_guest, exclude_key, exclude, evicted);
        if (rc == D9TEXCACHE_PINNED)
            return want_gpu ? D9TEXCACHE_GPU_FULL : D9TEXCACHE_GUEST_FULL;
        if (rc != D9TEXCACHE_OK) return rc;
        used = want_gpu ? cache->gpu_bytes : cache->guest_bytes;
    }
    return D9TEXCACHE_OK;
}

int d9_texcache_reserve_gpu(D9TexCache *cache, size_t need,
                            unsigned *evicted) {
    if (!cache) return D9TEXCACHE_INVALID;
    unsigned count = 0;
    int rc = reserve_bytes(cache, 1, 0, need, cache->gpu_limit, 0, 0, &count);
    if (evicted) *evicted = count;
    update_peaks(cache);
    return rc;
}

int d9_texcache_reserve_guest(D9TexCache *cache, size_t need,
                              unsigned *evicted) {
    if (!cache) return D9TEXCACHE_INVALID;
    unsigned count = 0;
    int rc = reserve_bytes(cache, 0, 1, need, cache->guest_limit, 0, 0, &count);
    if (evicted) *evicted = count;
    update_peaks(cache);
    return rc;
}

int d9_texcache_add_guest(D9TexCache *cache, uint64_t key, void *guest,
                          size_t bytes, int owned) {
    if (!cache || !guest || !bytes) return D9TEXCACHE_INVALID;
    if (owned && !cache->ops.release_guest) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (entry && entry->pins) return D9TEXCACHE_PINNED;
    size_t old = entry && entry->has_guest ? entry->guest_bytes : 0;
    if (bytes > old) {
        int rc = pool_has_room(cache->guest_bytes, bytes - old,
                               cache->guest_limit, 0);
        if (rc != D9TEXCACHE_OK) return rc;
    }
    int slot = -1;
    if (!entry) {
        slot = find_free_index(cache);
        if (slot < 0) return D9TEXCACHE_NO_ENTRIES;
    }
    if (bytes > old) {
        int rc = reserve_bytes(cache, 0, 1, bytes - old, cache->guest_limit,
                               key, 1, NULL);
        if (rc != D9TEXCACHE_OK) return rc;
    }
    if (entry && entry->has_guest) {
        int rc = clear_guest(cache, entry);
        if (rc != D9TEXCACHE_OK) return rc;
    }
    if (!entry) {
        entry = &cache->entry[slot];
        memset(entry, 0, sizeof *entry);
        ++cache->used;
    }
    entry->key = key;
    entry->guest = guest;
    entry->guest_bytes = bytes;
    entry->has_guest = 1;
    entry->guest_owned = owned ? 1 : 0;
    entry->last_use = ++cache->clock;
    cache->guest_bytes += bytes;
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

static int add_gpu_internal(D9TexCache *cache, uint64_t key,
                            const D9TexGpuRegistration *registration,
                            int ready, int allow_guest) {
    if (!cache || !registration || !registration->native || !registration->gpu_bytes)
        return D9TEXCACHE_INVALID;
    int owned = registration->owned ? 1 : 0;
    if (owned && !registration->release) return D9TEXCACHE_INVALID;

    /* Copy the descriptor before any callback can run. */
    void *native = registration->native;
    size_t bytes = registration->gpu_bytes;
    D9TexGpuRelease release = registration->release;
    void *release_ctx = registration->release_ctx;
    D9TexRehydrateToken token = registration->rehydrate_token;

    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (entry && entry->has_gpu) return D9TEXCACHE_ALREADY_RESIDENT;
    if (entry && entry->has_guest && !allow_guest)
        return D9TEXCACHE_ALREADY_RESIDENT;
    if (entry && entry->pins) return D9TEXCACHE_PINNED;
    int rc = pool_has_room(cache->gpu_bytes, bytes, cache->gpu_limit, 1);
    if (rc != D9TEXCACHE_OK) {
        if (rc == D9TEXCACHE_GPU_FULL) {
            if (entry) entry->gpu_failed = 1;
            ++cache->upload_failures;
        }
        return rc;
    }
    int slot = -1;
    if (!entry) {
        slot = find_free_index(cache);
        if (slot < 0) return D9TEXCACHE_NO_ENTRIES;
    }
    rc = reserve_bytes(cache, 1, 0, bytes, cache->gpu_limit, key, 1, NULL);
    if (rc != D9TEXCACHE_OK) {
        if (rc == D9TEXCACHE_GPU_FULL) {
            if (entry) entry->gpu_failed = 1;
            ++cache->upload_failures;
        }
        update_peaks(cache);
        return rc;
    }
    if (!entry) {
        entry = &cache->entry[slot];
        memset(entry, 0, sizeof *entry);
        ++cache->used;
    }
    if (token) {
        entry->rehydrate_token = token;
        entry->has_rehydrate = 1;
    }
    entry->key = key;
    entry->gpu = native;
    entry->gpu_bytes = bytes;
    entry->release_gpu = owned ? release : NULL;
    entry->release_gpu_ctx = owned ? release_ctx : NULL;
    entry->has_gpu = 1;
    entry->gpu_ready = ready ? 1 : 0;
    entry->gpu_owned = (uint8_t)owned;
    entry->gpu_failed = 0;
    entry->last_use = ++cache->clock;
    cache->gpu_bytes += bytes;
    if (ready) ++cache->uploads;
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

int d9_texcache_add_gpu(D9TexCache *cache, uint64_t key,
                        const D9TexGpuRegistration *registration) {
    return add_gpu_internal(cache, key, registration, 1, 0);
}

int d9_texcache_attach_gpu(D9TexCache *cache, uint64_t key,
                           const D9TexGpuRegistration *registration) {
    return add_gpu_internal(cache, key, registration, 0, 1);
}

int d9_texcache_set_rehydrate_token(D9TexCache *cache, uint64_t key,
                                    D9TexRehydrateToken token) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (!token && entry->pins && !entry->has_guest && !entry->has_gpu)
        return D9TEXCACHE_PINNED;
    entry->rehydrate_token = token;
    entry->has_rehydrate = token ? 1 : 0;
    if (!entry_is_live(entry)) {
        discard_dead_entry(cache, entry, 1);
    } else {
        entry->last_use = ++cache->clock;
    }
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

int d9_texcache_get_rehydrate_token(const D9TexCache *cache, uint64_t key,
                                    D9TexRehydrateToken *token) {
    if (!cache || !token) return D9TEXCACHE_INVALID;
    *token = D9TEXCACHE_NO_REHYDRATE_TOKEN;
    const D9TexEntry *entry = d9_texcache_find_const(cache, key);
    if (!entry || !entry->has_rehydrate) return D9TEXCACHE_NOT_FOUND;
    *token = entry->rehydrate_token;
    return D9TEXCACHE_OK;
}

int d9_texcache_begin_upload(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry || !entry->has_guest) return D9TEXCACHE_NOT_FOUND;
    if (entry->gpu_ready) return D9TEXCACHE_ALREADY_RESIDENT;
    if (entry->pins == UINT16_MAX) return D9TEXCACHE_PINNED;
    ++entry->pins;
    return D9TEXCACHE_OK;
}

int d9_texcache_gpu_alloc_failed(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (entry->pins) --entry->pins;
    entry->gpu_failed = 1;
    ++cache->upload_failures;
    update_peaks(cache);
    return D9TEXCACHE_GPU_FULL;
}

int d9_texcache_commit_upload(D9TexCache *cache, uint64_t key, void *gpu,
                              size_t gpu_bytes, int drop_guest) {
    if (!cache || !gpu || !gpu_bytes || !cache->ops.release_gpu)
        return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry || !entry->has_guest) return D9TEXCACHE_NOT_FOUND;
    int pending = entry->has_gpu && !entry->gpu_ready;
    if (entry->has_gpu && !pending) return D9TEXCACHE_ALREADY_RESIDENT;
    if (pending && (entry->gpu != gpu || entry->gpu_bytes != gpu_bytes))
        return D9TEXCACHE_ALREADY_RESIDENT;
    if (pending && drop_guest) return D9TEXCACHE_INVALID;
    if (entry->pins) --entry->pins;
    if (pending) {
        /* Ownership and GPU bytes were recorded by attach_gpu. */
        entry->gpu_ready = 1;
        entry->gpu_failed = 0;
        entry->last_use = ++cache->clock;
        ++cache->uploads;
        update_peaks(cache);
        return D9TEXCACHE_OK;
    }
    int rc = reserve_bytes(cache, 1, 0, gpu_bytes, cache->gpu_limit, key, 1, NULL);
    if (rc != D9TEXCACHE_OK) {
        if (rc == D9TEXCACHE_GPU_FULL) entry->gpu_failed = 1;
        ++cache->upload_failures;
        update_peaks(cache);
        return rc;
    }

    /* Do every fallible guest release before recording gpu.  If it fails, the
     * guest and all current accounting remain intact and the caller still owns
     * gpu, so there is no ambiguous ownership or double-free path. */
    if (drop_guest && entry->has_guest && entry->guest_owned && !entry->pins) {
        observe_gpu_addition(cache, entry, gpu_bytes);
        rc = clear_guest(cache, entry);
        if (rc != D9TEXCACHE_OK) {
            ++cache->upload_failures;
            update_peaks(cache);
            return rc;
        }
        ++cache->guest_drops;
    }
    entry->gpu = gpu;
    entry->gpu_bytes = gpu_bytes;
    entry->release_gpu = NULL;
    entry->release_gpu_ctx = NULL;
    entry->has_gpu = 1;
    entry->gpu_owned = 1;
    entry->gpu_failed = 0;
    entry->last_use = ++cache->clock;
    cache->gpu_bytes += gpu_bytes;
    ++cache->uploads;
    /* Capture coexistence when the guest was deliberately retained. */
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

int d9_texcache_abort_upload(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (entry->pins) --entry->pins;
    return D9TEXCACHE_OK;
}

int d9_texcache_touch(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    entry->last_use = ++cache->clock;
    return D9TEXCACHE_OK;
}

int d9_texcache_pin(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (entry->pins == UINT16_MAX) return D9TEXCACHE_PINNED;
    ++entry->pins;
    return D9TEXCACHE_OK;
}

int d9_texcache_unpin(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (!entry->pins) return D9TEXCACHE_PINNED;
    --entry->pins;
    return D9TEXCACHE_OK;
}

int d9_texcache_trim(D9TexCache *cache, size_t guest_target,
                     size_t gpu_target, unsigned *evicted) {
    if (!cache) return D9TEXCACHE_INVALID;
    unsigned count = 0;
    while (cache->guest_bytes > guest_target) {
        int rc = evict_one(cache, 0, 1, 0, 0, &count);
        if (rc != D9TEXCACHE_OK) {
            if (evicted) *evicted = count;
            update_peaks(cache);
            return rc == D9TEXCACHE_PINNED ? D9TEXCACHE_GUEST_FULL : rc;
        }
    }
    while (cache->gpu_bytes > gpu_target) {
        int rc = evict_one(cache, 1, 0, 0, 0, &count);
        if (rc != D9TEXCACHE_OK) {
            if (evicted) *evicted = count;
            update_peaks(cache);
            return rc == D9TEXCACHE_PINNED ? D9TEXCACHE_GPU_FULL : rc;
        }
    }
    if (evicted) *evicted = count;
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

int d9_texcache_remove(D9TexCache *cache, uint64_t key) {
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry) return D9TEXCACHE_NOT_FOUND;
    if (entry->pins) return D9TEXCACHE_PINNED;
    int rc = clear_guest(cache, entry);
    if (rc != D9TEXCACHE_OK) return rc;
    rc = clear_gpu(cache, entry);
    if (rc != D9TEXCACHE_OK) return rc;
    entry->rehydrate_token = D9TEXCACHE_NO_REHYDRATE_TOKEN;
    entry->has_rehydrate = 0;
    discard_dead_entry(cache, entry, 1);
    update_peaks(cache);
    return D9TEXCACHE_OK;
}

int d9_texcache_evict_gpu(D9TexCache *cache, uint64_t key, size_t *freed) {
    if (freed) *freed = 0;
    if (!cache) return D9TEXCACHE_INVALID;
    D9TexEntry *entry = d9_texcache_find(cache, key);
    if (!entry || !entry->has_gpu) return D9TEXCACHE_NOT_FOUND;
    if (entry->pins || entry->no_evict) return D9TEXCACHE_PINNED;
    size_t bytes = entry->gpu_bytes;
    int rc = clear_gpu(cache, entry);
    if (rc != D9TEXCACHE_OK) return rc;
    ++cache->evictions_gpu;
    discard_dead_entry(cache, entry, 1);
    update_peaks(cache);
    if (freed) *freed = bytes;
    return D9TEXCACHE_OK;
}

unsigned d9_texcache_working_set(const D9TexCache *cache, uint64_t max_age) {
    if (!cache) return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        const D9TexEntry *entry = &cache->entry[i];
        if (!entry_is_live(entry)) continue;
        if (cache->clock - entry->last_use <= max_age) ++count;
    }
    return count;
}

void d9_texcache_stats(const D9TexCache *cache, D9TexCacheStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!cache) return;
    out->guest_bytes = cache->guest_bytes;
    out->gpu_bytes = cache->gpu_bytes;
    out->peak_guest = cache->peak_guest;
    out->peak_gpu = cache->peak_gpu;
    out->peak_total = cache->peak_total;
    out->peak_both = cache->peak_both;
    out->uploads = cache->uploads;
    out->upload_failures = cache->upload_failures;
    out->rehydrations = cache->rehydrations;
    out->rehydrate_failures = cache->rehydrate_failures;
    out->budget_trims = cache->budget_trims;
    out->budget_trim_candidates = cache->budget_trim_candidates;
    out->budget_trim_evictions = cache->budget_trim_evictions;
    out->budget_trim_evicted_bytes = cache->budget_trim_evicted_bytes;
    out->budget_trim_failures = cache->budget_trim_failures;
    out->evictions_gpu = cache->evictions_gpu;
    out->evictions_guest = cache->evictions_guest;
    out->guest_drops = cache->guest_drops;
    out->clock = cache->clock;
    for (unsigned i = 0; i < cache->capacity; ++i) {
        const D9TexEntry *entry = &cache->entry[i];
        if (!entry_is_live(entry)) continue;
        ++out->entries;
        if (entry->has_guest) ++out->guests;
        if (entry->has_gpu) {
            ++out->gpus;
            if (!entry->gpu_ready) ++out->pending_gpus;
        }
        if (entry->has_rehydrate) ++out->rehydratable;
        if (entry->pins) ++out->pinned;
        if (entry->no_evict) ++out->non_evictable;
        if (entry->has_guest && entry->has_gpu)
            out->both_bytes = add_size(out->both_bytes,
                                       add_size(entry->guest_bytes, entry->gpu_bytes));
    }
}

void d9_texcache_note_rehydrate(D9TexCache *cache, int success) {
    if (!cache) return;
    if (success) ++cache->rehydrations;
    else ++cache->rehydrate_failures;
}

void d9_texcache_note_budget_trim(D9TexCache *cache, unsigned candidates,
                                  unsigned evicted, size_t bytes,
                                  int success) {
    if (!cache) return;
    ++cache->budget_trims;
    cache->budget_trim_candidates += candidates;
    cache->budget_trim_evictions += evicted;
    cache->budget_trim_evicted_bytes += bytes;
    if (!success) ++cache->budget_trim_failures;
}
