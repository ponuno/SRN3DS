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
 * configured sandbox root.  Rejects '..', empty segments, drive/absolute paths
 * outside the virtual root, and any result that escapes `root`.  Returns 0 and
 * writes a NUL-terminated host path, or -1 before win32_init()/on rejection. */
int win32_guest_path(const char *guest_name,char *out,size_t capacity);
/* Diagnostic mode: bind unknown imports to a trap that faults on call with the
 * exact dll!name. Never returns a fake success; unknown slots still fail. */
void win32_set_unresolved_trap(int enabled);
/* Diagnostics: imports bound to the fault trap (call with trap enabled). */
unsigned win32_unresolved_trap_count(void);
const char *win32_unresolved_trap_text(unsigned index);
/* Last ExitProcess seen (code and guest return address). */
uint32_t win32_last_exit_code(void);
uint32_t win32_last_exit_from(void);
/* Process last-error slot for shim modules that run without a TEB. */
void win32_set_last_error(uint32_t value);
/* Allocation accounting: what the guest asked for and how much is live. */
void win32_set_memory_probe(int on);
int win32_memory_probe(void);
void win32_memory_report(FILE *f);
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
