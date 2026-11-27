#pragma once
/* Bounded read-only Bethesda BSA reader (Skyrim v104 / SE v105).
 *
 * Layout handled (verified against the shipped 2011 archives by
 * tools/skyrim/bsa.py and re-checked by tests/menu_bsa_smoke.c):
 *
 *   header (36)
 *   folder records (16 * folder_count)
 *   for each folder: u8 name length (includes NUL) + name,
 *                    file records (16 * folder_count[i])
 *   file-name block (total_file_name_length bytes of NUL-terminated base
 *                    names, one per file record in order)
 *   file data (file-record offsets are absolute)
 *
 * Archive flags acted on: 0x001/0x002 (names), 0x004 (zlib per payload),
 * 0x100 (embedded full name prefix).  The stored size includes every prefix.
 *
 * The reader never trusts a count, offset, length or name before checking it
 * against the archive size and the configured limits.  It builds a compact
 * case-insensitive locator (open-addressed u32 table + 8-bit tag, name-block
 * checkpoints, per-folder locator); names and records stay in the archive and
 * are verified on demand through a fixed read-through cache.  The archive's
 * 64-bit hash records are kept opaque (no guessed hash algorithm).
 */

#include <stddef.h>
#include <stdint.h>

#define BSA_PATH_MAX 512u
#define BSA_REASON_MAX 192u

#define BSA_DEFAULT_MAX_FOLDERS 65536u
#define BSA_DEFAULT_MAX_FILES 1000000u
#define BSA_DEFAULT_MAX_NAME_BYTES (8u * 1024u * 1024u)
#define BSA_DEFAULT_MAX_INDEX_BYTES (24u * 1024u * 1024u)
#define BSA_DEFAULT_MAX_STORED_BYTES (64u * 1024u * 1024u)
#define BSA_DEFAULT_MAX_UNPACKED_BYTES (96u * 1024u * 1024u)

typedef enum {
    BSA_OK = 0,
    BSA_ERR_ARGUMENT,
    BSA_ERR_ACCESS,
    BSA_ERR_IO,
    BSA_ERR_NOT_FOUND,
    BSA_ERR_MAGIC,
    BSA_ERR_VERSION,
    BSA_ERR_BOUNDS,
    BSA_ERR_COUNT,
    BSA_ERR_NAME,
    BSA_ERR_DUPLICATE,
    BSA_ERR_UNSUPPORTED,
    BSA_ERR_FORMAT,
    BSA_ERR_TRUNCATED,
    BSA_ERR_DEFLATE,
    BSA_ERR_BUDGET
} BsaStatus;

typedef struct {
    uint32_t max_folders;
    uint32_t max_files;
    uint32_t max_name_bytes;
    uint32_t max_index_bytes;
    uint32_t max_stored_bytes;
    uint32_t max_unpacked_bytes;
    /* Bounded LRU of fully inflated records used by compressed range reads.
     * 0 keeps the build defaults (8 MiB total, 4 MiB per record).  Records
     * larger than either limit are streamed for the requested window and never
     * cached. */
    uint32_t range_cache_bytes;
    uint32_t range_cache_entry_bytes;
    int verify_embedded_names;
} BsaOptions;

typedef int (*BsaReadAt)(void *ctx, uint64_t offset, void *dst, size_t bytes);

typedef struct {
    uint32_t version;
    uint32_t archive_flags;
    uint32_t folder_count;
    uint32_t file_count;
    uint32_t total_folder_name_length;
    uint32_t total_file_name_length;
    uint32_t file_flags;
    uint64_t data_start;
    int compressed;
    int embedded_names;
} BsaHeader;

/* One directory record.  `stored_size` counts the embedded-name prefix and the
 * 4-byte unpacked size when the archive is compressed. */
typedef struct {
    uint64_t hash;
    uint32_t stored_size;
    uint32_t offset;
    uint32_t folder_index;
    uint32_t record_index;
} BsaEntry;

typedef struct {
    BsaEntry entry;
    char path[BSA_PATH_MAX];
} BsaMatch;

typedef struct BsaArchive BsaArchive;

const char *bsa_status_name(BsaStatus status);
void bsa_options_default(BsaOptions *options);

/* Open from a caller-supplied bounded random-access callback.  `ctx` and the
 * underlying storage must outlive the archive.  The parser reads at most
 * max_index_bytes for the directory index and max_stored_bytes for one
 * compressed payload. */
BsaStatus bsa_open_reader(BsaReadAt read_at, void *ctx, uint64_t size,
                          const BsaOptions *options, BsaArchive **out,
                          char *reason, size_t reason_cap);

/* Convenience wrappers.  The memory blob must outlive the archive. */
BsaStatus bsa_open_memory(const void *data, size_t size,
                          const BsaOptions *options, BsaArchive **out,
                          char *reason, size_t reason_cap);
BsaStatus bsa_open_path(const char *path, const BsaOptions *options,
                        BsaArchive **out, char *reason, size_t reason_cap);

void bsa_close(BsaArchive *archive);
const BsaHeader *bsa_header(const BsaArchive *archive);
uint32_t bsa_file_count(const BsaArchive *archive);
/* Diagnostic: bytes currently charged to the archive's index budget (locator
 * table + tags + checkpoints + folder array).  The fixed per-archive page
 * cache is separate and deliberately not included. */
size_t bsa_index_bytes(const BsaArchive *archive);

/* Case- and separator-insensitive lookup ("interface/StartMenu.swf" works).
 * Returns BSA_ERR_NOT_FOUND without touching `out` when absent. */
BsaStatus bsa_find(const BsaArchive *archive, const char *path, BsaMatch *out);

/* Unpacked size without decompressing a compressed payload. */
BsaStatus bsa_unpacked_size(const BsaArchive *archive, const BsaEntry *entry,
                            uint32_t *out, char *reason, size_t reason_cap);

/* Read at most `cap` unpacked bytes.  For a compressed entry the stored
 * payload is bounded by max_stored_bytes; `cap` bounds the output. */
BsaStatus bsa_read_prefix(const BsaArchive *archive, const BsaEntry *entry,
                          uint8_t *dst, size_t cap, size_t *out_len,
                          char *reason, size_t reason_cap);

/* Read the complete unpacked payload.  Fails with BSA_ERR_BUDGET when the
 * payload is larger than `cap`, and BSA_ERR_FORMAT/CORRUPT when the declared
 * size, zlib stream or Adler-32 disagree. */
BsaStatus bsa_read(const BsaArchive *archive, const BsaEntry *entry,
                   uint8_t *dst, size_t cap, size_t *out_len,
                   char *reason, size_t reason_cap);

/* Read `n` unpacked bytes starting at `rel` directly into `dst`.
 *
 * Uncompressed entries read only the requested range; the physical offset is
 * computed in 64 bits after entry_prefix() removed the embedded-name prefix.
 * `rel == payload_length` with `n == 0` is a valid empty read at the end.
 *
 * Compressed (zlib) entries can be read arbitrarily as well.  A record is
 * inflated once and kept in the archive's bounded LRU (see BsaOptions), so the
 * next window of the same record is a memory copy; a record larger than the
 * per-record or total cache limit is streamed for the requested window and
 * discarded.  `rel`/`n` address the *unpacked* payload.
 *
 * For both layouts `rel > unpacked` or `n > unpacked - rel` is BSA_ERR_BOUNDS
 * (the range is never silently truncated).  On any error *out_len is zero and
 * nothing after `dst` was written.
 *
 * The cache shares the archive's existing single-thread contract: one archive
 * is used from one cooperative guest thread at a time, exactly like the
 * read-through page cache and FILE* seek state. */
BsaStatus bsa_read_range(const BsaArchive *archive, const BsaEntry *entry,
                         uint32_t rel, void *dst, uint32_t n,
                         uint32_t *out_len, char *reason, size_t reason_cap);

/* Metrics for the compressed range cache.  Counters are monotonic; `bytes` is
 * the number of unreferenced inflated bytes currently resident. */
typedef struct {
    uint64_t hits;       /* served from a resident inflated record */
    uint64_t misses;     /* record not resident (inflated or streamed) */
    uint64_t evictions;  /* records dropped by the LRU */
    uint64_t bytes;      /* resident inflated bytes right now */
} BsaRangeCacheStats;
void bsa_range_cache_stats(const BsaArchive *archive,
                           BsaRangeCacheStats *out);

/* Exposed for tests and for the runtime's content identity line. */
uint32_t bsa_crc32(const void *data, size_t size);

/* Root-level inflate entry point used by bsa_read*(); exposed so the synthetic
 * host test can exercise stored/fixed/dynamic deflate blocks directly.
 * `allow_partial` stops once `cap` bytes were produced, which is what a header
 * prefix read needs.  Otherwise the whole stream must end exactly at `cap`. */
BsaStatus bsa_inflate_zlib(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t cap, size_t *out_len,
                           int allow_partial, char *reason, size_t reason_cap);

/* Strict single-stream variant: the deflate stream must end exactly at the
 * payload boundary, produce exactly `cap` bytes and pass Adler-32.  Used by the
 * SWF container reader, where a concatenated stream or trailing garbage is a
 * format error rather than a partial success. */
BsaStatus bsa_inflate_zlib_exact(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t cap, size_t *out_len,
                                 char *reason, size_t reason_cap);
