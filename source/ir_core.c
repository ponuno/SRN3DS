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
