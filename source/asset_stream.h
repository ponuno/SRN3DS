#pragma once
/* Device side of the cooked streaming index (phase 1 skeleton).
 *
 * This module loads ``stream_index.bin`` (written by
 * ``tools/skyrim/stream_index.py``), resolves a canonical virtual path or a
 * resource id, and reads a bounded payload range through
 * ``bsa_read_range()``.  It does not parse JSON, does not inflate and does
 * not allocate a copy of the whole record.
 *
 * Phase 2 adds a bounded prefetch queue (256 descriptors), byte-quantum
 * executor with two 64 KiB staging buffers, a monotonic byte budget, dedup by
 * (generation, source, id, lod), cancellation/deadlines, RAM readiness and LRU
 * eviction.  Deliberately absent: GPU publication and cell-table paging (the
 * header still carries an empty cell table).
 *
 * Layout: see the comment block at the top of tools/skyrim/stream_index.py.
 * All tables are little-endian; the loader validates magic, schema, entry
 * sizes, table offsets, the trailing CRC-32, path ordering and every
 * ``physical_offset + payload_length`` against the archive size.
 */

#include <stddef.h>
#include <stdint.h>

#include "bsa_reader.h"

#define STREAM_INDEX_SCHEMA 1u
#define STREAM_INDEX_HEADER_SIZE 80u
#define STREAM_INDEX_ARCHIVE_ENTRY_SIZE 56u
#define STREAM_INDEX_RESOURCE_ENTRY_SIZE 104u
#define STREAM_INDEX_CELL_ENTRY_SIZE 64u
#define STREAM_PATH_MAX 512u
#define STREAM_REASON_MAX 192u
#define STREAM_NEGATIVE_SLOTS 64u
#define STREAM_MAX_ARCHIVES 64u
#define STREAM_MAX_RESOURCES 1000000u
#define STREAM_MAX_CELLS 1000000u
#define STREAM_CELL_KEY_BYTES 24u
#define STREAM_CELL_IDS 8u
#define STREAM_RESOURCE_FLAG_COMPRESSED 0x0001u

/* Phase 2 queue limits.  A request is a bounded prefix window, never the whole
 * record: the executor stages at most `window_max_bytes` per descriptor. */
#define STREAM_MAX_REQUESTS 256u
#define STREAM_IO_BUFFERS 2u
#define STREAM_IO_BUFFER_BYTES 65536u
#define STREAM_QUANTUM_MIN 32768u
#define STREAM_QUANTUM_MAX 65536u
#define STREAM_WINDOW_MAX (256u * 1024u)
#define STREAM_CACHE_LIMIT_DEFAULT (2u * 1024u * 1024u)
#define STREAM_RATE_DEFAULT 7000000ull
#define STREAM_MAX_ACCRUAL_QUANTA 2u

typedef enum {
    STREAM_OK = 0,
    STREAM_ERR_ARGUMENT,
    STREAM_ERR_ACCESS,
    STREAM_ERR_IO,
    STREAM_ERR_MAGIC,
    STREAM_ERR_SCHEMA,
    STREAM_ERR_BOUNDS,
    STREAM_ERR_FORMAT,
    STREAM_ERR_NOT_FOUND,
    STREAM_ERR_UNSUPPORTED,
    STREAM_ERR_NOMEM,
    STREAM_ERR_BUDGET
} StreamStatus;

const char *stream_status_name(StreamStatus status);

typedef struct {
    const uint8_t *sha256;        /* cooked payload digest (32 bytes) */
    const uint8_t *source_sha256; /* original winner digest (32 bytes) */
    const char *path;             /* points into the index blob */
    uint32_t path_length;         /* stored length; the pool adds no NUL */
    uint32_t id;
    uint32_t archive_index;
    uint32_t payload_length;
    uint32_t stored_size;
    uint32_t entry_offset;
    uint32_t flags;
    uint64_t physical_offset;     /* after the entry prefix */
} StreamResource;

typedef struct {
    uint64_t generation;
    uint32_t archive_count;
    uint32_t resource_count;
    uint32_t cell_count;
} StreamIndexInfo;

/* Cell table row key, 24 bytes on disk:
 *   kind u32 (0 = interior, 1 = exterior) | form_id u32 |
 *   grid_x i32 | grid_y i32 | worldspace_form_id u32 | reserved u32
 * Interior rows use form_id and leave the grid/worldspace zero; exterior rows
 * use the grid and the worldspace form id.  The writer sorts rows by
 * (kind, form_id, grid_x, grid_y, worldspace_form_id). */
typedef struct {
    uint32_t kind;
    uint32_t form_id;
    int32_t grid_x;
    int32_t grid_y;
    uint32_t worldspace_form_id;
    uint32_t reserved;
} StreamCellKey;

/* Cell table row, 64 bytes on disk:
 *   key[24] | resource_count u32 | flags u32 | resource_id[8] u32
 * `resource_count` is 0..8; only the first `resource_count` ids are valid.
 * Schema 1 has no room for more than 8 ids per cell. */
typedef struct {
    StreamCellKey key;
    uint32_t resource_count;
    uint32_t flags;
    uint32_t resource_id[STREAM_CELL_IDS];
} StreamCellRow;

typedef struct StreamIndex StreamIndex;

/* Parse an index held in caller memory.  The blob must outlive the index. */
StreamStatus stream_index_parse(const void *blob, size_t size,
                                StreamIndex **out, char *reason,
                                size_t reason_cap);
/* Read the whole file into an owned buffer and parse it. */
StreamStatus stream_index_load(const char *path, StreamIndex **out,
                               char *reason, size_t reason_cap);
void stream_index_free(StreamIndex *index);
const StreamIndexInfo *stream_index_info(const StreamIndex *index);

StreamStatus stream_index_archive(const StreamIndex *index, uint32_t slot,
                                  const char **name, uint64_t *bytes,
                                  const uint8_t **sha256);
StreamStatus stream_index_find_path(const StreamIndex *index, const char *path,
                                    StreamResource *out, char *reason,
                                    size_t reason_cap);
StreamStatus stream_index_find_id(const StreamIndex *index, uint32_t id,
                                  StreamResource *out, char *reason,
                                  size_t reason_cap);
/* Binary search over the cell table.  A missing key returns
 * STREAM_ERR_NOT_FOUND; malformed rows are rejected at parse. */
StreamStatus stream_index_find_cell(const StreamIndex *index,
                                    const StreamCellKey *key,
                                    StreamCellRow *out, char *reason,
                                    size_t reason_cap);

/* Miss cache valid for exactly one index generation.  Resets clear every
 * entry; a reloaded index is a new StreamIndex with a new generation. */
void stream_index_negative_reset(StreamIndex *index);

typedef struct {
    uint64_t requests;
    uint64_t bytes_requested;
    uint64_t bytes_returned;
    uint64_t misses;
    uint64_t errors;
    uint64_t negative_hits;
    uint64_t physical_reads;   /* successful bsa_read_range calls */
    uint64_t archive_opens;
    /* Phase 2.  `physical_reads`/`errors` continue to count executor reads. */
    uint64_t prefetch_requests; /* accepted admissions, dedup included */
    uint64_t dedup_hits;
    uint64_t quanta;
    uint64_t quantum_bytes;     /* actually read and charged to the budget */
    uint64_t seeks;             /* archive change or non-adjacent offset */
    uint64_t budget_denied;     /* pump found work but no byte tokens */
    uint64_t cancelled;         /* explicit cancel and unreferenced P2 drops */
    uint64_t expired;           /* deadline passed before the first quantum */
    uint64_t evicted;
    uint64_t evicted_bytes;
    uint64_t verified;
    uint64_t ready;
    uint64_t request_errors;
    uint64_t cache_hits;
    uint64_t cache_hit_bytes;
    uint64_t allocs;
    uint64_t frees;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
    /* Demand integration (`stream_mount_demand_path`). */
    uint64_t demand_hits;          /* reads served from a READY window */
    uint64_t demand_hit_bytes;
    uint64_t prefetch_demand_hits; /* demand hits whose window was prefetched */
    uint64_t demand_cancelled_prefetch; /* staged work dropped for a demand read */
    uint32_t descriptors;       /* live descriptors, including terminal ones */
    uint32_t cache_bytes;       /* live READY/QUEUED window bytes */
    uint32_t peak_cache_bytes;
} StreamMetrics;

typedef struct StreamMount StreamMount;

/* Ordering is strict: demand before P0 before P1 before P2. */
typedef enum {
    STREAM_PRIO_DEMAND = 0,
    STREAM_PRIO_P0,
    STREAM_PRIO_P1,
    STREAM_PRIO_P2
} StreamPriority;

typedef enum {
    STREAM_REQ_QUEUED = 0,
    STREAM_REQ_READING,
    STREAM_REQ_VERIFIED,
    STREAM_REQ_READY,
    STREAM_REQ_ERROR,
    STREAM_REQ_CANCELLED
} StreamRequestState;

const char *stream_priority_name(StreamPriority priority);
const char *stream_request_state_name(StreamRequestState state);

/* Dedup key: mounting generation, winning source (archive), resource id, LOD. */
typedef struct {
    uint64_t generation;
    uint32_t source;
    uint32_t id;
    uint32_t lod;
} StreamRequestKey;

typedef uint32_t StreamHandle;   /* 0 is never valid */

typedef struct {
    StreamRequestKey key;
    StreamPriority priority;      /* current, possibly boosted */
    StreamPriority requested_priority;
    StreamRequestState state;
    uint32_t target_bytes;        /* window staged for this request */
    uint32_t fetched_bytes;
    uint32_t payload_length;
    uint32_t consumers;           /* demand subscribers; prefetch is zero */
    int pinned;
    int visible;
    uint64_t deadline_ms;         /* 0 = none, else mount-monotonic ms */
    uint64_t enqueued_ms;
    uint64_t last_touch_ms;
    uint32_t quanta;
} StreamRequestInfo;

typedef struct {
    uint64_t rate_bytes_per_sec;  /* default 7,000,000 */
    uint32_t quantum_bytes;       /* 32-64 KiB, default 64 KiB */
    uint32_t window_max_bytes;    /* per descriptor, default 256 KiB */
    uint32_t cache_limit_bytes;   /* staged bytes, default 2 MiB */
    uint32_t max_accrual_quanta;  /* 1-2, default 2 */
} StreamConfig;

void stream_mount_config_default(StreamConfig *out);
StreamStatus stream_mount_configure(StreamMount *mount,
                                    const StreamConfig *config, char *reason,
                                    size_t reason_cap);

/* Monotonic clock in nanoseconds.  The default uses clock() so the module
 * stays dependency-free; the device should install a tick-based clock and
 * tests install an artificial one. */
typedef uint64_t (*StreamClockFn)(void *ctx);
void stream_mount_set_clock(StreamMount *mount, StreamClockFn clock, void *ctx);
uint64_t stream_mount_now_ms(const StreamMount *mount);

/* Optional digest/identity check once the full window is staged.  Returning
 * non-zero turns the request into STREAM_REQ_ERROR and frees its bytes.  With
 * no callback, `VERIFIED` means "the requested window was read completely";
 * it does not claim a SHA-256 check. */
typedef int (*StreamVerifyFn)(void *ctx, const StreamResource *res,
                              const uint8_t *data, uint32_t len);
void stream_mount_set_verify(StreamMount *mount, StreamVerifyFn verify,
                             void *ctx);

/* Enqueue a prefix window [0, min(n, payload_length, window_max)).  Prefetch
 * priorities stage bytes without a subscriber; STREAM_PRIO_DEMAND adds a
 * subscriber that must be released.  An identical live key returns the same
 * handle and boosts priority/extent, so one resource is read once. */
StreamStatus stream_mount_prefetch_ex(StreamMount *mount,
                                      const StreamResource *res, uint32_t n,
                                      uint32_t lod, StreamPriority priority,
                                      uint64_t deadline_ms, StreamHandle *out,
                                      char *reason, size_t reason_cap);

/* Unpacked bytes a prefetch would stage for `res`: the archive's unpacked size
 * for a compressed record, otherwise `payload_length`.  Callers that account a
 * working set use this instead of the stored length. */
StreamStatus stream_mount_stage_extent(StreamMount *mount,
                                       const StreamResource *res,
                                       uint32_t *out, char *reason,
                                       size_t reason_cap);

/* Execute up to `max_quanta` 32-64 KiB reads under the byte budget.  One read
 * is active at a time; the two 64 KiB buffers are rotated and copied into the
 * request's bounded window.  `*out_quanta == 0` with live queued work means
 * the budget is empty or memory/backpressure refused admission. */
StreamStatus stream_mount_pump(StreamMount *mount, uint32_t max_quanta,
                               uint32_t *out_quanta, char *reason,
                               size_t reason_cap);

StreamStatus stream_mount_request_info(const StreamMount *mount,
                                       StreamHandle handle,
                                       StreamRequestInfo *out);
/* Copy from a READY window.  No archive I/O and no allocation. */
StreamStatus stream_mount_request_copy(StreamMount *mount, StreamHandle handle,
                                       uint32_t rel, void *dst, uint32_t n,
                                       uint32_t *out_len, char *reason,
                                       size_t reason_cap);

StreamStatus stream_mount_pin(StreamMount *mount, StreamHandle handle,
                              int pinned, char *reason, size_t reason_cap);
StreamStatus stream_mount_mark_visible(StreamMount *mount, StreamHandle handle,
                                       int visible, char *reason,
                                       size_t reason_cap);
/* Drop one demand subscriber.  The last one makes a READY window an eviction
 * candidate; it is not freed until pressure or trim. */
StreamStatus stream_mount_release(StreamMount *mount, StreamHandle handle,
                                  char *reason, size_t reason_cap);
/* Cancel a live request (buffer freed) or reclaim a terminal descriptor. */
StreamStatus stream_mount_cancel(StreamMount *mount, StreamHandle handle,
                                 char *reason, size_t reason_cap);
/* Evict READY windows with no consumers that are not pinned/visible, oldest
 * touch first, until cache_bytes <= target_bytes (or nothing is evictable). */
StreamStatus stream_mount_trim(StreamMount *mount, uint32_t target_bytes,
                               uint32_t *out_freed, char *reason,
                               size_t reason_cap);

/* `data_dir` is the directory holding the cooked archives named by the
 * index (usually ``.../Data``).  Archives are opened lazily on first use and
 * kept for the session; size and directory identity are checked then. */
StreamStatus stream_mount_open(const StreamIndex *index, const char *data_dir,
                               StreamMount **out, char *reason,
                               size_t reason_cap);
void stream_mount_close(StreamMount *mount);

/* Read at most `n` payload bytes at `rel` through bsa_read_range().  On
 * success *out_len is what the range API returned (0 is valid at EOF). */
StreamStatus stream_mount_read(StreamMount *mount, const StreamResource *res,
                               uint32_t rel, void *dst, uint32_t n,
                               uint32_t *out_len, char *reason,
                               size_t reason_cap);

/* Demand-read a canonical path from the staged queue without archive I/O.
 *   *served = 1  -> [rel, rel+n) was copied from a READY prefetch window;
 *   *served = 0  -> nothing was copied (miss, or the window is too small).
 * A live prefetch request for the same resource is cancelled when it cannot
 * serve the read, so the caller's own I/O is not duplicated.  This never
 * enqueues or allocates a request. */
StreamStatus stream_mount_demand_path(StreamMount *mount, const char *path,
                                      uint32_t rel, void *dst, uint32_t n,
                                      uint32_t *out_len, int *served,
                                      char *reason, size_t reason_cap);

void stream_mount_metrics(const StreamMount *mount, StreamMetrics *out);

/* Phase 2: queue/prefetch/eviction.  Refused by name for now. */
StreamStatus stream_mount_prefetch(StreamMount *mount, const StreamResource *res,
                                   uint32_t n, char *reason,
                                   size_t reason_cap);
