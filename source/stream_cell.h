#pragma once
/* Host-side cell trigger for `asset_stream`.
 *
 * Reads the engine's own cell state through GuestMemory (no guest hooks, no
 * code patches): holder `[0x1b2e864]`, current cell `+0x6c`, grid `+0x5c/+0x60`,
 * worldspace `+0xbc`, the "grid moved" latch `0x12e3560` and `uGridsToLoad`
 * `0x1241000` (see docs/CELL_TRACE.md).  A tuple change or a set latch enqueues
 * the cell's P0 rows from `stream_index.bin`, marks the centre visible and
 * trims when the working set shrinks.
 *
 * The module is inert unless N3DS_STREAM=1.  The device paths default to
 * `sdmc:/Skyrim/stream_index.bin` and `sdmc:/Skyrim/Data` and can be overridden
 * with N3DS_STREAM_INDEX / N3DS_STREAM_DATA.
 */

#include <stddef.h>
#include <stdint.h>

#include "asset_stream.h"
#include "guest.h"

/* TESForm::formID.  Standard TESV form layout (flags at +0x08, formID at
 * +0x0C); a wrong value only makes index lookups miss, never reads/writes
 * guest memory outside the object. */
#define STREAM_GUEST_FORM_ID_OFFSET 0x0cu
#define STREAM_GUEST_CELL_OFFSET 0x6cu
#define STREAM_GUEST_GRID_X_OFFSET 0x5cu
#define STREAM_GUEST_GRID_Y_OFFSET 0x60u
#define STREAM_GUEST_WORLDSPACE_OFFSET 0xbcu
#define STREAM_GUEST_HOLDER_ADDR 0x1b2e864u
#define STREAM_GUEST_GRID_MOVED_ADDR 0x12e3560u
#define STREAM_GUEST_UGRIDS_ADDR 0x1241000u

typedef struct StreamCellState StreamCellState;

typedef struct {
    uint64_t ticks;
    uint64_t transitions;          /* tuple changed (cell/grid/worldspace) */
    uint64_t moved_edges;          /* 0x12e3560 == 1 */
    uint64_t exterior_ticks;
    uint64_t rows_found;
    uint64_t rows_missing;
    uint64_t prefetch_requests;    /* accepted admissions, dedup included */
    uint64_t skipped_compressed;   /* compressed row whose unpacked extent failed */
    uint64_t trim_events;
    uint64_t trim_bytes;
    uint64_t demand_hits;
    uint64_t demand_misses;
    uint64_t demand_cancelled;
    uint32_t visible_handles;      /* centre row handles marked visible */
    uint32_t last_resource_count;  /* ids in the centre row, 0 = no row */
} StreamCellMetrics;

/* Open an index and mount it.  `data_dir` holds the archive files named by the
 * index (usually the Skyrim Data directory). */
StreamStatus stream_cell_open(StreamCellState **out, const char *index_path,
                              const char *data_dir, char *reason,
                              size_t reason_cap);
void stream_cell_close(StreamCellState *state);
const StreamCellMetrics *stream_cell_metrics(const StreamCellState *state);
const StreamMount *stream_cell_mount(const StreamCellState *state);
const StreamIndex *stream_cell_index(const StreamCellState *state);
/* Centre row handles marked visible by the last tick (test/inspection). */
uint32_t stream_cell_handle_count(const StreamCellState *state);
StreamHandle stream_cell_handle_at(const StreamCellState *state, uint32_t i);

/* Poll one frame.  Returns STREAM_OK also when the guest has no holder/cell
 * yet.  `reason` is only meaningful for real failures (open/index errors). */
StreamStatus stream_cell_tick(StreamCellState *state, GuestMemory *mem,
                              char *reason, size_t reason_cap);

/* Canonical-path demand read (no archive I/O).  Returns 1 when the range was
 * copied from a READY prefetch window, 0 otherwise. */
int stream_cell_demand_read(StreamCellState *state, const char *canonical_path,
                            uint32_t rel, void *dst, uint32_t n,
                            uint32_t *out_len);

/* Per-frame hook installed in d3d9_com.c Present.  Weak at the call site. */
void stream_host_frame_tick(GuestMemory *mem);

/* s_ReadFile hook.  Converts a host path ("<root>/Data/meshes/x.nif") to the
 * canonical index path and serves a staged range when possible.  Returns 1
 * when it filled `dst`; 0 means "fall back to the overlay/file read". */
int stream_host_note_read(const char *host_path, uint32_t rel, void *dst,
                          uint32_t n, uint32_t *out_len);
