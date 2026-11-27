#pragma once
/* Texture residency and guest/GPU copy ownership.
 *
 * Why this exists
 * ---------------
 * d3d9_core.c currently keeps the guest (CPU) copy of every texture for the
 * lifetime of the D3D texture, even after every GPU level has been uploaded.
 * For a 4 MiB texture budget that doubles the real footprint and makes an
 * allocation failure look like a GPU problem when it is actually redundant
 * CPU memory.  This module is the explicit policy layer:
 *
 *   guest copy  -> decoded source bytes, needed to upload or to lock/read back
 *   GPU copy    -> PICA texture, needed to draw
 *   both        -> only during an upload or while the caller pins/reads back
 *
 * A successful upload calls release_guest when the caller says the CPU copy is
 * disposable.  A failed GPU allocation keeps the guest copy intact so the
 * caller can evict an LRU texture and retry deterministically.  Pinning
 * protects an entry from eviction while it is being uploaded or read back.
 *
 * ETC1/SKTX asset uploads can register an already-created native texture with
 * d9_texcache_add_gpu().  That path never retains the SKTX container or any
 * other guest source.  MANAGED assets may also carry an opaque rehydrate token;
 * the token outlives GPU eviction so the asset loader can reproduce the native
 * texture later without keeping the temporary container alive.
 *
 * The module is deliberately independent of d3d9_core.c so the host tests can
 * drive every failure path without a GPU.  Integration points are documented
 * in docs/TEXTURES.md and docs/TEXTURE_RESIDENCY_INTEGRATION.md; the cache owns
 * bytes accounting, not D3D handle creation.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D9TEXCACHE_MAX_ENTRIES 256u
#define D9TEXCACHE_NO_REHYDRATE_TOKEN 0u

/* Opaque, asset-manager-defined handle.  The cache only stores and returns it;
 * zero means that no durable rehydrate source is attached to an entry. */
typedef uint64_t D9TexRehydrateToken;

enum {
    D9TEXCACHE_OK = 0,
    D9TEXCACHE_INVALID = -1,
    D9TEXCACHE_NOT_FOUND = -2,
    D9TEXCACHE_GUEST_FULL = -3,
    D9TEXCACHE_GPU_FULL = -4,
    D9TEXCACHE_PINNED = -5,
    D9TEXCACHE_ALREADY_RESIDENT = -6,
    D9TEXCACHE_RELEASE_FAILED = -7,
    D9TEXCACHE_NO_ENTRIES = -8,
};

/* Callbacks are invoked when the cache gives up ownership.  Return 0 on
 * success.  A non-zero return aborts that release and leaves the copy and its
 * accounting unchanged, so a backend bug cannot silently lose a texture. */
typedef int (*D9TexGpuRelease)(void *ctx, void *gpu, size_t bytes);

typedef struct {
    void *ctx;
    int (*release_guest)(void *ctx, void *guest, size_t bytes);
    D9TexGpuRelease release_gpu;
} D9TexCacheOps;

/* Registration for a native texture that has already been uploaded.  No guest
 * pointer is accepted or retained.  owned=1 requires release to be non-NULL;
 * borrowed registrations (owned=0) never invoke it.  rehydrate_token is optional
 * and zero means "absent" for a new key (or "keep the existing token" when
 * reloading a token-only entry). */
typedef struct {
    void *native;
    size_t gpu_bytes;
    int owned;
    D9TexGpuRelease release;
    void *release_ctx;
    D9TexRehydrateToken rehydrate_token;
} D9TexGpuRegistration;

typedef struct {
    uint64_t key;
    void *guest;
    size_t guest_bytes;
    void *gpu;
    size_t gpu_bytes;
    D9TexGpuRelease release_gpu;
    void *release_gpu_ctx;
    D9TexRehydrateToken rehydrate_token;
    uint64_t last_use;
    uint16_t pins;
    /* Frame-scoped pins.  bound_frame is set while the texture occupies a
     * sampler slot; draw_frame is set once a draw referencing it has been
     * submitted.  Both contribute exactly one to pins and are released by
     * d9_texcache_frame_clear() after the GPU finished the frame. */
    uint32_t bound_frame;
    uint32_t draw_frame;
    uint8_t has_guest;
    uint8_t has_gpu;
    uint8_t gpu_ready;       /* direct add=1; core-allocated native starts at 0 */
    uint8_t guest_owned;     /* cache may call release_guest */
    uint8_t gpu_owned;       /* cache may call the per-entry/default release */
    uint8_t has_rehydrate;   /* token-only entries remain findable after eviction */
    uint8_t gpu_failed;      /* last upload attempt failed; guest retained */
    uint8_t no_evict;        /* explicit-release-only; LRU eviction skips it */
} D9TexEntry;

typedef struct {
    unsigned entries;
    unsigned guests;
    unsigned gpus;
    unsigned pending_gpus;
    unsigned rehydratable;
    unsigned pinned;
    unsigned uploads;
    unsigned upload_failures;
    unsigned rehydrations;
    unsigned rehydrate_failures;
    unsigned budget_trims;
    unsigned budget_trim_candidates;
    unsigned budget_trim_evictions;
    size_t budget_trim_evicted_bytes;
    unsigned budget_trim_failures;
    unsigned trims;            /* explicit d9_texcache_trim() calls */
    unsigned pinned_refusals;  /* allocations refused because candidates were pinned */
    unsigned frame_pins;       /* frame-scoped pins taken (bound or draw) */
    unsigned evictions_gpu;
    unsigned evictions_guest;
    unsigned guest_drops;
    unsigned non_evictable;
    size_t guest_bytes;
    size_t gpu_bytes;
    size_t both_bytes;       /* entries currently holding both copies */
    size_t peak_guest;
    size_t peak_gpu;
    size_t peak_total;
    size_t peak_both;
    uint64_t clock;
} D9TexCacheStats;

typedef struct {
    D9TexCacheOps ops;
    D9TexEntry *entry;
    unsigned capacity;
    unsigned used;
    size_t guest_limit;
    size_t gpu_limit;
    size_t guest_bytes;
    size_t gpu_bytes;
    size_t peak_guest;
    size_t peak_gpu;
    size_t peak_total;
    size_t peak_both;
    uint64_t clock;
    unsigned uploads;
    unsigned upload_failures;
    unsigned rehydrations;
    unsigned rehydrate_failures;
    unsigned budget_trims;
    unsigned budget_trim_candidates;
    unsigned budget_trim_evictions;
    size_t budget_trim_evicted_bytes;
    unsigned budget_trim_failures;
    unsigned trims;
    unsigned pinned_refusals;
    unsigned frame_pins;
    unsigned last_refusal_pinned;
    unsigned evictions_gpu;
    unsigned evictions_guest;
    unsigned guest_drops;
} D9TexCache;

/* entries must remain valid for the cache lifetime; capacity <=
 * D9TEXCACHE_MAX_ENTRIES.  limits are byte caps; 0 means "no limit".
 * ops.release_gpu is the default for commit_upload; add_gpu can instead supply
 * an explicit per-registration callback. */
int d9_texcache_init(D9TexCache *cache, D9TexEntry *entries,
                     unsigned capacity, const D9TexCacheOps *ops,
                     size_t guest_limit, size_t gpu_limit);

/* Releases every guest/GPU copy the cache still owns.  Safe on NULL.  Use
 * d9_texcache_destroy_ex when release failures must be observed and retried. */
void d9_texcache_destroy(D9TexCache *cache);

/* Like destroy, but returns the first release failure.  Independent copies are
 * still attempted; failed copies remain live and accounted so a later destroy
 * can retry without double-freeing anything. */
int d9_texcache_destroy_ex(D9TexCache *cache);

int d9_texcache_set_limits(D9TexCache *cache, size_t guest_limit,
                           size_t gpu_limit);

/* Mark an entry as explicit-release-only.  LRU eviction (reserve/trim) skips
 * it, but d9_texcache_remove() still releases it.  Used for resources whose
 * native cannot be recreated from the guest source (render-target cube faces
 * have no CPU copy).  Returns NOT_FOUND for an absent key. */
int d9_texcache_set_no_evict(D9TexCache *cache, uint64_t key, int no_evict);

/* Find an entry; NULL when absent.  Does not touch LRU order. */
D9TexEntry *d9_texcache_find(D9TexCache *cache, uint64_t key);
const D9TexEntry *d9_texcache_find_const(const D9TexCache *cache,
                                         uint64_t key);

/* Add/replace the guest copy.  Evicts unpinned guest-only entries oldest
 * first until it fits.  A pinned or GPU-resident entry is never dropped just
 * to make room for a guest copy; that returns D9TEXCACHE_GUEST_FULL.  owned=1
 * requires ops.release_guest. */
int d9_texcache_add_guest(D9TexCache *cache, uint64_t key, void *guest,
                          size_t bytes, int owned);

/* Register an already-uploaded native ETC1/SKTX texture without retaining a
 * guest source.  Takes ownership only when registration.owned is non-zero.
 * On failure the caller still owns native.  GPU-budget failures are reported as
 * D9TEXCACHE_GPU_FULL; release-callback failures are retryable and leave the
 * candidate unregistered.  A token-only entry for key is the reload slot. */
int d9_texcache_add_gpu(D9TexCache *cache, uint64_t key,
                        const D9TexGpuRegistration *registration);

/* Attach an allocated but not-yet-uploaded native texture.  Unlike add_gpu this
 * may coexist with a guest source.  begin_upload/commit_upload use the pair to
 * pin the guest and mark the native drawable without transferring ownership or
 * changing byte accounting a second time. */
int d9_texcache_attach_gpu(D9TexCache *cache, uint64_t key,
                           const D9TexGpuRegistration *registration);

/* Attach, replace or clear (token == 0) an opaque durable rehydrate handle.
 * Clearing the last token from a token-only entry frees its slot.  The getter
 * returns D9TEXCACHE_NOT_FOUND when the key or token is absent. */
int d9_texcache_set_rehydrate_token(D9TexCache *cache, uint64_t key,
                                    D9TexRehydrateToken token);
int d9_texcache_get_rehydrate_token(const D9TexCache *cache, uint64_t key,
                                    D9TexRehydrateToken *token);

/* Upload lifecycle.  begin_upload pins the guest source; it must be followed
 * by commit_upload or abort_upload.  A pending native attached with
 * d9_texcache_attach_gpu is allowed; commit_upload then only marks it ready. */
int d9_texcache_begin_upload(D9TexCache *cache, uint64_t key);

/* Record a failed GPU allocation.  The guest copy stays; returns
 * D9TEXCACHE_GPU_FULL so callers cannot mistake it for success. */
int d9_texcache_gpu_alloc_failed(D9TexCache *cache, uint64_t key);

/* Commit a successful upload.  Takes ownership of gpu only on return
 * D9TEXCACHE_OK.  For a native previously registered with attach_gpu, gpu must
 * be the same pointer/cost, drop_guest must be zero, and the call only marks the
 * native ready (ownership and accounting are unchanged).  Otherwise, if
 * drop_guest is non-zero and the guest is owned and unpinned, release_guest is
 * called before gpu is recorded and the guest bytes are removed.  If the GPU
 * budget cannot fit the new copy after evicting unpinned LRU entries, or the
 * guest release fails, nothing about the candidate is recorded: the caller
 * still owns gpu and the guest source is retained for a later retry.
 * Already-committed LRU evictions are not rolled back. */
int d9_texcache_commit_upload(D9TexCache *cache, uint64_t key, void *gpu,
                              size_t gpu_bytes, int drop_guest);

/* Abort an upload begun with begin_upload (unpin, no accounting change). */
int d9_texcache_abort_upload(D9TexCache *cache, uint64_t key);

int d9_texcache_touch(D9TexCache *cache, uint64_t key);
int d9_texcache_pin(D9TexCache *cache, uint64_t key);
int d9_texcache_unpin(D9TexCache *cache, uint64_t key);

/* Frame-scoped pinning, for asynchronous GPU backends.
 *
 * d9_texcache_frame_bind() pins a texture that currently occupies a sampler
 * slot; d9_texcache_frame_unbind() releases only that slot pin (used when the
 * slot is rebound, so a texture that was never drawn is evictable again).
 * d9_texcache_frame_pin() records that a submitted draw references the
 * texture; it survives an unbind because the GPU still needs the native.
 * Both are idempotent within one frame value and contribute one pin each.
 * frame values are opaque and non-zero; the core uses a per-present
 * generation counter.  d9_texcache_frame_clear(frame) releases every pin for
 * that generation and returns the number of entries unpinned; call it only
 * after the backend proves the frame's GPU commands completed (FrameSync). */
int d9_texcache_frame_bind(D9TexCache *cache, uint64_t key, uint32_t frame);
int d9_texcache_frame_unbind(D9TexCache *cache, uint64_t key, uint32_t frame);
int d9_texcache_frame_pin(D9TexCache *cache, uint64_t key, uint32_t frame);
unsigned d9_texcache_frame_clear(D9TexCache *cache, uint32_t frame);
/* Drop every frame-scoped pin regardless of generation.  Only for paths that
 * invalidate all GPU state (device Reset) or tests that simulate the barrier. */
unsigned d9_texcache_frame_clear_all(D9TexCache *cache);
/* Drop both frame-scoped pins of one entry without touching plain pins.  Used
 * by explicit ReleaseTexture, whose backend deletion already waits for the GPU
 * (citro3d C3D_FrameSync) before freeing the native. */
int d9_texcache_frame_forget(D9TexCache *cache, uint64_t key);

/* Non-zero when the most recent allocation refusal happened because every
 * eviction candidate in the exhausted pool was pinned (rather than no_evict,
 * or simply larger than the whole limit).  Pairs with stats.pinned_refusals. */
int d9_texcache_last_refusal_pinned(const D9TexCache *cache);

/* Evict the oldest unpinned GPU/guest copies until need more bytes fit.
 * Returns D9TEXCACHE_OK when the requested space is available, otherwise the
 * corresponding *_FULL code.  Eviction stops at pinned entries. */
int d9_texcache_reserve_gpu(D9TexCache *cache, size_t need,
                            unsigned *evicted);
int d9_texcache_reserve_guest(D9TexCache *cache, size_t need,
                              unsigned *evicted);

/* Working-set trim: evict oldest unpinned copies until both byte counts are
 * at or below the targets.  Returns the number evicted in *evicted. */
int d9_texcache_trim(D9TexCache *cache, size_t guest_target,
                     size_t gpu_target, unsigned *evicted);

/* Remove one entry entirely (both copies). */
int d9_texcache_remove(D9TexCache *cache, uint64_t key);

/* Evict only the GPU copy of one entry, leaving the guest copy and rehydrate
 * token intact.  Pinned and no_evict entries are refused.  *freed receives the
 * released GPU bytes on success.  This is the primitive used by the core's
 * budget-pressure trim. */
int d9_texcache_evict_gpu(D9TexCache *cache, uint64_t key, size_t *freed);

/* Record an automatic budget-pressure trim attempt.  candidates is the number
 * of eligible entries considered, evicted the number actually released. */
void d9_texcache_note_budget_trim(D9TexCache *cache, unsigned candidates,
                                  unsigned evicted, size_t bytes,
                                  int success);

/* Number of entries used within max_age ticks of the current clock. */
unsigned d9_texcache_working_set(const D9TexCache *cache, uint64_t max_age);

void d9_texcache_stats(const D9TexCache *cache, D9TexCacheStats *out);
/* Record one core-driven rehydration attempt in the eviction/rehydration
 * counters.  The cache itself never allocates cube/volume natives. */
void d9_texcache_note_rehydrate(D9TexCache *cache,int success);

#ifdef __cplusplus
}
#endif
