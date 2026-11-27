#pragma once
#include <stdio.h>
#include "rt_table.h"
#include "guest.h"
#include "pe_imports.h"

#define WIN32_THUNK_BASE UINT32_C(0xff000000)
int win32_init(GuestMemory *m,uint32_t heap_bytes,const char *game_root);
void win32_shutdown(void);
void win32_set_linker(const PeLinker *linker);
void win32_set_external_resolver(PeImportResolve resolver,void *ctx);
/* Additional resolvers are consulted after the primary external resolver and
 * before the unresolved-import trap.  Duplicate (function, context) pairs are
 * ignored.  Returns 0 when installed (or already present), -1 when the fixed
 * chain is full so the caller does not claim a shim that is not reachable. */
int win32_add_external_resolver(PeImportResolve resolver,void *ctx);
/* Map a guest relative path (or a recognised C:\Skyrim\ virtual root) to the
 * configured sandbox root.  "." components are dropped (Windows semantics;
 * TESV emits LocalAppData\Skyrim\.\Plugins.txt), while '..', empty segments,
 * drive/absolute paths outside the virtual root, and any result that escapes
 * `root` are rejected.  Returns 0 and
 * writes a NUL-terminated host path, or one of WIN32_GUEST_PATH_* below. */
enum {
    WIN32_GUEST_PATH_OK = 0,
    WIN32_GUEST_PATH_NO_ROOT = -1,   /* win32_init() has not configured a root */
    WIN32_GUEST_PATH_EMPTY = -2,
    WIN32_GUEST_PATH_TOO_LONG = -3,
    WIN32_GUEST_PATH_OUTSIDE = -4,
    WIN32_GUEST_PATH_INVALID = -5
};
int win32_guest_path(const char *guest_name,char *out,size_t capacity);
/* TESV_INI_OVERLAY test hook: merge `spec` (a key=value file path, or an inline
 * "key=value;..." string) into `buf` as the INI read path would.  Returns the
 * number of applied values, or -1 on bad arguments.  `buf` must have
 * capacity >= 1 MiB; `name` must be Skyrim.INI/Skyrim.ini for the merge to fire. */
int win32_ini_overlay_merge_text(const char *spec,const char *name,char *buf,
                                 size_t capacity,size_t *inout_len);
/* Read-only BSA overlay for Data\... files.  Sources come from Skyrim.ini
 * [Archive] plus Data\*Cooked*.bsa (and, for the host harness, every Data\*.bsa
 * when TESV_BSA_OVERLAY_ALL=1).  Loose files win unless
 * TESV_BSA_OVERLAY_FIRST=1.  The overlay never writes an archive. */
#define WIN32_BSA_OVERLAY_MAX_ARCHIVES 64u
typedef struct {
    uint64_t stat_resolves;   /* GetFileAttributes* answered from an archive */
    uint64_t open_resolves;   /* CreateFileA answered from an archive */
    uint64_t range_reads;     /* bsa_read_range() calls that succeeded */
    uint64_t full_reads;      /* compressed entries materialized via bsa_read() */
    uint64_t bytes_read;      /* bytes returned to ReadFile */
    uint64_t read_errors;
    uint32_t archives;        /* configured sources */
    uint32_t archives_failed; /* sources skipped for access/format errors */
    /* Resident memory of the archives that are actually open, measured by
     * bsa_resident_stats().  Archives are opened lazily, so `resident_archives`
     * can be smaller than `archives`. */
    uint64_t resident_bytes;
    uint64_t index_bytes;
    uint64_t page_cache_bytes;
    uint64_t range_cache_bytes;
    uint32_t resident_archives;
    /* Per configured slot, in source order; 0 for a slot that is not open. */
    uint64_t archive_resident[WIN32_BSA_OVERLAY_MAX_ARCHIVES];
} Win32BsaOverlayStats;
void win32_bsa_overlay_stats(Win32BsaOverlayStats *out);
/* Diagnostic mode: bind unknown imports to a trap that faults on call with the
 * exact dll!name. Never returns a fake success; unknown slots still fail. */
void win32_set_unresolved_trap(int enabled);
/* Diagnostics: imports bound to the fault trap (call with trap enabled). */
unsigned win32_unresolved_trap_count(void);
const char *win32_unresolved_trap_text(unsigned index);
/* Last ExitProcess seen (code and guest return address). */
uint32_t win32_last_exit_code(void);
uint32_t win32_last_exit_from(void);
/* Hang-watchdog diagnostic: tracked critical sections and their owners. */
void win32_cs_dump(FILE *f);
/* Hang-watchdog diagnostic: recent InterlockedCompareExchange/Exchange words. */
void win32_icx_dump(FILE *f);
/* IO telemetry: every completed ReadFile counts.  n3ds_io_dump lists totals,
 * throughput and the most recently read paths (largest first history of 8), so
 * a hang dump can distinguish a slow-but-moving load from a stalled reader.
 * N3DS_IO_TRACE=1 adds periodic lines (every 8 MiB or 5 s) and one line per
 * read >= 1 MiB.  Counters are always maintained; the env only controls noise. */
uint64_t n3ds_io_bytes_total(void);
uint64_t n3ds_io_reads_total(void);
void n3ds_io_dump(FILE *f);
/* Process last-error slot for shim modules that run without a TEB. */
void win32_set_last_error(uint32_t value);
/* Allocation accounting: what the guest asked for and how much is live. */
void win32_set_memory_probe(int on);
int win32_memory_probe(void);
void win32_memory_report(FILE *f);
typedef struct {
    uint64_t va_calls, heap_calls, crt_calls;
    uint64_t failures;
    uint64_t va_live, heap_live, crt_live, peak_live;
} Win32AllocStats;
void win32_alloc_stats(Win32AllocStats *out);
/* VirtualAlloc accounting (reservations never inflate committed bytes).
 * Semantics: MEM_RESERVE records address space only in the VA window
 * 0x40000000..0x70000000 (768 MiB, 64 KiB allocation granularity); MEM_COMMIT
 * maps only the committed pages (host chunks of 512 KiB, so partial decommit
 * can free pages and preserve the rest); MEM_DECOMMIT unmaps but keeps the
 * reservation; MEM_RELEASE requires the allocation base and size 0. Access to
 * reserved-but-uncommitted pages still faults via n3ds_guest_ptr. A single
 * request is capped at 1 GiB and the committed total by the guest pool; over-
 * commit fails with ERROR_NOT_ENOUGH_MEMORY and a [mem] diagnostic. */
typedef struct {
    uint64_t reserved_live, reserved_total;
    uint64_t committed_live, committed_peak; /* Win32 commit promise */
    uint64_t backed_live, backed_peak;       /* physically backed host pages */
    unsigned reservations;
    uint32_t commit_failures, last_fail_size;
} Win32VaStats;
void win32_va_stats(Win32VaStats *out);
uint32_t win32_resolve(const PeImport *entry,void *ctx);
const FnEntry *win32_lookup(uint32_t address);
int win32_self_test(CPU *cpu,GuestMemory *m);
