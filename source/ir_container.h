/* Indexed, SD-backed predecoded IR container.
 *
 * The live slice keeps only the functions selected for AOT plus a generated
 * IrFnEntry table. TESV discovery finds 90,900 functions, so a call into any
 * discovered-but-not-selected function currently ends in
 * "Guest function/import unknown". This container indexes *every* discovered
 * function, stores its decoded IR on SD, loads one function record on demand,
 * and executes that record without touching the runtime decoder.
 *
 * ---------------------------------------------------------------- layout
 * All integers are little-endian. The file is:
 *
 *   IrcFileHeader (64 bytes, 8-byte aligned)
 *   index[nfun]   (40 bytes each, sorted by addr, strictly increasing)
 *   pad to 8
 *   function records (each 8-byte aligned, CRC32 protected)
 *
 * Every index entry exists even when the function could not be decoded; such
 * entries have IRC_FN_INVALID, a decode_status, record_size == 0 and no record.
 * They fault explicitly when executed. Nothing is ever re-decoded at runtime:
 * a block that is not present in the loaded record faults instead of falling
 * back to the x86 decoder.
 *
 * ---------------------------------------------------------- resident index
 * The on-disk index is 40 bytes per function. The runtime does not keep it:
 * each function becomes a 16-byte compact entry (addr, end, record_crc32 and a
 * packed record_size/flags/decode_status word). Records are written
 * contiguously in index order, each 8-byte aligned, so a record's file offset
 * is derived from the running sum of the preceding record sizes (a checkpoint
 * every 16 entries bounds that to 15 additions, and it only happens when a
 * record is actually loaded). block_count/insn_count are read from the
 * CRC-verified record header instead of being duplicated. Measured on
 * tesv.irc (90,903 functions, 62,052,440 record bytes): the resident index
 * drops from 3,636,120 B to 1,454,448 B + 22,728 B of checkpoints.
 * A container whose records are not contiguous in index order is refused at
 * open with N3DS_IRC_ERR_INDEX_BOUNDS rather than reading the wrong bytes.
 *
 * The container is optional. A build without it keeps the current AOT + slice
 * IR behaviour. Opening a missing/corrupt container fails cleanly and leaves
 * the caller's existing tiers untouched.
 *
 * Not thread-safe: the runtime pins guest threads to one host thread, exactly
 * like ir_decode.c and ir_exec.c. One container handle may be executing at a
 * time; nested calls into the same handle are supported.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ir_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version of the on-disk format. Bump on any layout change. */
#define N3DS_IRC_VERSION 1u

#define N3DS_IRC_HEADER_BYTES   64u
#define N3DS_IRC_INDEX_ENTRY    40u
#define N3DS_IRC_RECORD_HEADER  40u
#define N3DS_IRC_BLOCK_ENTRY    20u

#define N3DS_IRC_DEFAULT_CACHE_BYTES   (2u << 20)
#define N3DS_IRC_DEFAULT_CACHE_RECORDS 64u
#define N3DS_IRC_MAX_EXEC_DEPTH        256u

/* One executor frame is {N3dsIrc *, IrcSlot *}. Cooperative guest threads keep
 * their own copy of the running frame stack: a thread that blocks (Sleep) deep
 * inside a container call must resume with its own pinned frames, not the
 * frames of whichever thread ran in between. */
#define N3DS_IRC_FRAME_SNAPSHOT_BYTES \
    (N3DS_IRC_MAX_EXEC_DEPTH * 2u * sizeof(void *))

/* File header flags (IrcFileHeader.flags). */
enum {
    N3DS_IRC_FILE_HAS_AOT    = 1u << 0,
    N3DS_IRC_FILE_HAS_FORCED = 1u << 1,
    N3DS_IRC_FILE_COMPLETE   = 1u << 2,
    N3DS_IRC_FILE_LE         = 1u << 3,
};

/* Index entry flags. */
enum {    N3DS_IRC_FN_DECODED  = 1u << 0,  /* record present and executable */
    N3DS_IRC_FN_AOT      = 1u << 1,  /* measured hot: AOT takes precedence */
    N3DS_IRC_FN_FORCED   = 1u << 2,  /* forced AOT (unsupported by v1/v2) */
    N3DS_IRC_FN_COMPLETE = 1u << 3,  /* every discovery leader decoded */
    N3DS_IRC_FN_INVALID  = 1u << 4,  /* explicit negative entry */
    N3DS_IRC_FN_PREENTRY = 1u << 5,  /* record has a cold block before entry */
};

/* decode_status values. */
typedef enum {
    N3DS_IRC_DEC_OK = 0,
    N3DS_IRC_DEC_UNSUPPORTED = 1,
    N3DS_IRC_DEC_NO_TERMINATOR = 2,
    N3DS_IRC_DEC_EMPTY = 3,
    N3DS_IRC_DEC_BAD_RANGE = 4,
    N3DS_IRC_DEC_TOO_LARGE = 5,
    N3DS_IRC_DEC_NOT_DECODED = 6,
    N3DS_IRC_DEC_UNKNOWN = 7,
} N3dsIrcDecodeStatus;

/* Return codes. Zero is success; N3DS_IRC_TIER_CONTAINER is also zero. */
typedef enum {
    N3DS_IRC_OK = 0,
    N3DS_IRC_ERR_ARG = -1,
    N3DS_IRC_ERR_OPEN = -2,
    N3DS_IRC_ERR_HEADER = -3,
    N3DS_IRC_ERR_VERSION = -4,
    N3DS_IRC_ERR_INDEX_CRC = -5,
    N3DS_IRC_ERR_INDEX_BOUNDS = -6,
    N3DS_IRC_ERR_NOT_FOUND = -7,
    N3DS_IRC_ERR_RECORD_IO = -8,
    N3DS_IRC_ERR_RECORD_SHORT = -9,
    N3DS_IRC_ERR_RECORD_CRC = -10,
    N3DS_IRC_ERR_RECORD_BAD = -11,
    N3DS_IRC_ERR_UNDECODED = -12,
    N3DS_IRC_ERR_CACHE = -13,
    N3DS_IRC_ERR_CACHE_PINNED = -14,
    N3DS_IRC_ERR_TOO_LARGE = -15,
    N3DS_IRC_ERR_NESTING = -16,
} N3dsIrcStatus;

/* n3ds_irc_exec()/n3ds_irc_dispatch() result for a successful decision. */
enum {
    N3DS_IRC_TIER_CONTAINER = 0,
    N3DS_IRC_TIER_AOT = 1,
};

/* Public, host-endian view of one index entry. Materialized from the 16-byte
 * resident index on demand: block_count/insn_count are 0 here (they live in
 * the record header, which is validated on load) and record_offset is derived
 * for decoded entries. */
typedef struct {
    uint32_t addr;          /* exact callable entry */
    uint32_t end;           /* exclusive extent used as the frame limit */
    uint32_t flags;         /* N3DS_IRC_FN_* */
    uint16_t decode_status; /* N3dsIrcDecodeStatus */
    uint16_t block_count;
    uint32_t insn_count;
    uint32_t record_offset; /* absolute file offset, 8-byte aligned */
    uint32_t record_size;   /* zero for an entry without a record */
    uint32_t record_crc32;
    uint32_t reserved[2];
} N3dsIrcEntry;

typedef struct {
    uint64_t opens;
    uint64_t open_failures;
    uint64_t lookups;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t load_attempts;
    uint64_t loads;
    uint64_t load_bytes;
    uint64_t load_ns;
    uint64_t evictions;
    uint64_t evicted_bytes;
    uint64_t pins;
    uint64_t unpins;
    uint64_t pin_limit;
    uint64_t short_reads;
    uint64_t crc_errors;
    uint64_t record_errors;
    uint64_t io_errors;
    uint64_t invalid_entries;
    uint64_t block_hits;
    uint64_t block_misses;
    uint64_t execs;
    uint64_t aot_preempted;
    uint64_t cache_bytes;   /* current resident record bytes */
    uint64_t cache_records; /* current resident record count */
    /* Deliberately always zero in a container execution: the container
     * executor only reads predecoded streams. Differential tests assert it. */
    uint64_t redecodes;
} N3dsIrcStats;

typedef struct N3dsIrc N3dsIrc;

/* AOT overlay predicate: return nonzero when the address must stay AOT. */
typedef int (*N3dsIrcAotPredicate)(void *user, uint32_t addr);

/* ---------------------------------------------------------------- open */
N3dsIrc *n3ds_irc_open(const char *path);
/* Same, but always stores a status (including on success). */
N3dsIrc *n3ds_irc_open_ex(const char *path, int *status);
/* Closing must not race an active n3ds_irc_exec*() on the same handle. */
void n3ds_irc_close(N3dsIrc *irc);

/* ------------------------------------------------------------ metadata */
unsigned n3ds_irc_function_count(const N3dsIrc *irc);
unsigned n3ds_irc_decoded_count(const N3dsIrc *irc);
uint32_t n3ds_irc_file_flags(const N3dsIrc *irc);
uint64_t n3ds_irc_file_size(const N3dsIrc *irc);
uint64_t n3ds_irc_index_size(const N3dsIrc *irc);
uint32_t n3ds_irc_index_crc32(const N3dsIrc *irc);
uint32_t n3ds_irc_data_crc32(const N3dsIrc *irc);

/* Exact-entry lookup. Returns N3DS_IRC_OK and fills `out`, or
 * N3DS_IRC_ERR_NOT_FOUND / N3DS_IRC_ERR_ARG. */
int n3ds_irc_lookup(const N3dsIrc *irc, uint32_t addr, N3dsIrcEntry *out);
int n3ds_irc_has_preentry(const N3dsIrc *irc, uint32_t addr);
int n3ds_irc_entry_at(const N3dsIrc *irc, unsigned ordinal, N3dsIrcEntry *out);

/* --------------------------------------------------------------- cache */
/* Bounded LRU over whole function records. `max_bytes` 0 selects the default;
 * `max_records` 0 selects the default. Re-initialising clears the cache.
 * Must not be called while a container execution is active (n3ds_irc_exec*). */
int n3ds_irc_cache_init(N3dsIrc *irc, size_t max_bytes, unsigned max_records);
void n3ds_irc_cache_clear(N3dsIrc *irc);
/* Load without pinning, pin (loading if needed), release one pin. */
int n3ds_irc_load(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_pin(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_unpin(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_is_loaded(const N3dsIrc *irc, uint32_t addr);
/* Drop pins held by an interrupted execution (after a guest fault longjmp). */
void n3ds_irc_recover(N3dsIrc *irc);
/* Cooperative threads: current container frame depth, and recovery down to the
 * depth a guest thread had at its entry. A fault in thread B must not release
 * the pins of thread A, which may be suspended deep inside a container call. */
uint32_t n3ds_irc_depth(const N3dsIrc *irc);
void n3ds_irc_recover_to(N3dsIrc *irc, uint32_t depth);
/* Save/restore the running thread's executor frame stack across a cooperative
 * host context switch. `dst` holds at least N3DS_IRC_FRAME_SNAPSHOT_BYTES.
 * n3ds_irc_frames_save() returns the saved depth; n3ds_irc_frames_load()
 * installs a depth of zero when count is 0 (a fresh thread). */
uint32_t n3ds_irc_frames_save(void *dst);
void n3ds_irc_frames_load(const void *src, uint32_t count);
/* Diagnostic: the container function/block currently executing. Updated on
 * every prefetch; useful when a host-side guest-memory access faults. */
extern uint32_t n3ds_irc_last_function;
extern uint32_t n3ds_irc_last_block;
extern CPU *n3ds_irc_current_cpu;

void n3ds_irc_stats(const N3dsIrc *irc, N3dsIrcStats *out);
void n3ds_irc_stats_reset(N3dsIrc *irc);

/* ------------------------------------------------------------ dispatch */
void n3ds_irc_set_aot_predicate(N3dsIrc *irc, N3dsIrcAotPredicate pred, void *user);

/* Execute one predecoded function. The entry must be decoded; an invalid /
 * unsupported entry raises rt_unimpl() with an explicit message. A record I/O
 * or CRC failure raises rt_unimpl() too, unless the caller uses the status
 * form below and handles the error itself.
 *
 * n3ds_irc_exec()    : asks the AOT predicate first. Returns TIER_AOT without
 *                      touching the cache when the address must stay AOT.
 * n3ds_irc_exec_forced(): container only (differential tests).
 *
 * Neither function ever calls n3ds_ir_encode_block()/n3ds_ir_fetch_block(). */
int n3ds_irc_exec(N3dsIrc *irc, CPU *c, uint32_t addr);
int n3ds_irc_exec_forced(N3dsIrc *irc, CPU *c, uint32_t addr);
/* Status variant: returns a negative N3dsIrcStatus instead of rt_unimpl for
 * lookup / invalid-entry / record-load failures. Guest execution itself can
 * still fault (unsupported instruction stream, block miss, guest memory).
 * On success returns N3DS_IRC_TIER_CONTAINER/AOT. */
int n3ds_irc_exec_status(N3dsIrc *irc, CPU *c, uint32_t addr, int allow_aot);

/* ---------------------------------------------------------- diagnostics */
const char *n3ds_irc_strstatus(int status);
const char *n3ds_irc_decode_status_name(unsigned status);
/* CRC32 used by the format, exposed for tools/tests. */
uint32_t n3ds_irc_crc32(const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
