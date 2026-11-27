#pragma once
/* Device side of the cooked streaming index (phase 1 skeleton).
 *
 * This module loads ``stream_index.bin`` (written by
 * ``tools/skyrim/stream_index.py``), resolves a canonical virtual path or a
 * resource id, and reads a bounded payload range through
 * ``bsa_read_range()``.  It does not parse JSON, does not inflate and does
 * not allocate a copy of the whole record.
 *
 * Deliberately absent in phase 1 (see docs/STREAMING_IMPL.md):
 *   - prefetch queue, byte quanta and request owners;
 *   - range cache / eviction / GPU publication;
 *   - cell table paging (the header carries an empty table).
 * ``stream_mount_prefetch()`` refuses by name instead of pretending.
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
} StreamMetrics;

typedef struct StreamMount StreamMount;

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

void stream_mount_metrics(const StreamMount *mount, StreamMetrics *out);

/* Phase 2: queue/prefetch/eviction.  Refused by name for now. */
StreamStatus stream_mount_prefetch(StreamMount *mount, const StreamResource *res,
                                   uint32_t n, char *reason,
                                   size_t reason_cap);
