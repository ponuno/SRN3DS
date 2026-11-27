/* IR glue: the interpreter function-range table.
 *
 * Decoding lives in ir_decode.c and execution in ir_exec.c. Keeping the table
 * here gives the runtime one small object to link for kind resolution, and
 * makes a build that wants only the decoder (tools/ir_measure.py) link without
 * the interpreter.
 */
#include "ir_core.h"
#include "hprof.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const IrFnEntry *g_fns;
/* Host-installed cooperative time-slice hook (see ir_core.h).  Defined here
 * rather than in ir_exec.c because ir_container.c includes ir_exec.c. */
void (*n3ds_ir_yield_hook)(void);
/* Host-installed diagnostic PC ring (see ir_core.h).  Same reasoning as the
 * yield hook: both executors include this object, guest.c/win32_thread.c do not
 * have to be linked. */
void (*n3ds_ir_pc_note_hook)(uint32_t pc, unsigned thread, uint32_t esp,
                             uint32_t ecx, uint32_t eax);
uint32_t (*n3ds_ir_thread_id_hook)(void);
/* Diagnostic counters: how many IR blocks the current guest thread has run and
 * how many times the cooperative yield hook actually fired.  A hang dump can
 * then tell "the interpreter is stuck inside one block" (blocks flat) from
 * "the interpreter runs but the hook is never installed/reached" (blocks
 * climbing, yields flat).  Both increments are a single add per block. */
unsigned long long n3ds_ir_blocks_run;
unsigned long long n3ds_ir_yield_calls;
unsigned long long n3ds_ir_poll_yields;
uint32_t n3ds_ir_longest_block_ms; /* wall time inside one block, diagnostic */
unsigned n3ds_ir_yield_poll_count;   /* AOT blocks since the last poll-fired yield */
/* N3DS_SCHED_TRACE=1 additionally times each block (two clock() calls) so a
 * hang dump can prove whether one block (a `rep` or a huge unrolled body) is
 * the unit that never yields. Off by default: one cached branch per block. */
static int g_sched_trace = -1;
int n3ds_ir_sched_trace(void) {
    if (g_sched_trace < 0) {
        const char *v = getenv("N3DS_SCHED_TRACE");
        g_sched_trace = (v && *v && strcmp(v, "0")) ? 1 : 0;
    }
    return g_sched_trace;
}
static uint32_t g_block_ip;
static unsigned long long g_block_clk;
/* Only the innermost block is timed.  A block that calls a guest function runs
 * inner blocks first (they overwrite the start stamp and clear it when done),
 * so the outer measurement after a nested call is skipped rather than
 * mis-attributed.  This is diagnostic only: a block that never returns is
 * exactly the case where the stamp stays set and `block_age_ms` grows. */
uint32_t n3ds_ir_block_ip;             /* block currently executing */
uint32_t n3ds_ir_block_elapsed_ms;     /* its age at the last watchdog read */
void n3ds_ir_block_begin(uint32_t ip) {
    if (!n3ds_ir_sched_trace()) return;
    n3ds_ir_block_ip = ip;
    g_block_ip = ip;
    g_block_clk = (unsigned long long)clock();
}
void n3ds_ir_block_end(void) {
    if (!n3ds_ir_sched_trace()) return;
    if (!g_block_clk) return;   /* already consumed by an inner block */
    unsigned long long d = ((unsigned long long)clock() - g_block_clk) * 1000ull /
                           (unsigned long long)CLOCKS_PER_SEC;
    if (d > n3ds_ir_longest_block_ms) n3ds_ir_longest_block_ms = (uint32_t)d;
    g_block_clk = 0;
    n3ds_ir_block_ip = 0;
}
uint32_t n3ds_ir_current_block_ip(void) { return n3ds_ir_block_ip; }
/* CPU-time age (ms) of the block that is still running; 0 between blocks.
 * Only meaningful while n3ds_ir_sched_trace() is on. */
uint32_t n3ds_ir_block_age_ms(void) {
    if (!g_block_clk) return 0;
    return (uint32_t)(((unsigned long long)clock() - g_block_clk) * 1000ull /
                      (unsigned long long)CLOCKS_PER_SEC);
}
void n3ds_ir_note_pc(uint32_t pc, uint32_t esp, uint32_t ecx, uint32_t eax) {
    if (n3ds_ir_pc_note_hook)
        n3ds_ir_pc_note_hook(pc, n3ds_ir_thread_id_hook ? n3ds_ir_thread_id_hook() : 0u,
                             esp, ecx, eax);
}
static unsigned g_nfns;
/* Direct-mapped memo for the immutable bound table. The table is rebound only
 * at boot (or by a test), and n3ds_ir_bind clears this. */
#define IR_END_CACHE 1024u
static struct { uint32_t addr, end; } g_end_cache[IR_END_CACHE];

int n3ds_ir_bind(const IrFnEntry *fns, unsigned count) {
    memset(g_end_cache, 0, sizeof g_end_cache);
    if ((!fns && count) || !count) { g_fns = NULL; g_nfns = 0; return count ? -1 : 0; }
    for (unsigned i = 0; i < count; ++i) {
        /* Sorted, unique entry points. Extents may overlap: the discovery can
         * assign one cold block to several functions, and only exact entry
         * addresses are callable. A duplicate entry would be ambiguous. */
        if (!fns[i].end || fns[i].end <= fns[i].addr) return -1;
        if (i && fns[i - 1].addr >= fns[i].addr) return -1;
    }
    g_fns = fns; g_nfns = count;
    return 0;
}

int n3ds_ir_has(uint32_t addr) { return n3ds_ir_end(addr) != 0; }

uint32_t n3ds_ir_end(uint32_t addr) {
    if (!g_fns) return 0;
    unsigned ci = (addr >> 2) & (IR_END_CACHE - 1);
    if (g_end_cache[ci].addr == addr) return g_end_cache[ci].end;
    unsigned lo = 0, hi = g_nfns;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (g_fns[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    uint32_t end = (lo < g_nfns && g_fns[lo].addr == addr) ? g_fns[lo].end : 0;
    g_end_cache[ci].addr = addr;
    g_end_cache[ci].end = end;
    return end;
}
