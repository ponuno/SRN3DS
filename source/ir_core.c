/* IR glue: the interpreter function-range table.
 *
 * Decoding lives in ir_decode.c and execution in ir_exec.c. Keeping the table
 * here gives the runtime one small object to link for kind resolution, and
 * makes a build that wants only the decoder (tools/ir_measure.py) link without
 * the interpreter.
 */
#include "ir_core.h"

static const IrFnEntry *g_fns;
static unsigned g_nfns;

int n3ds_ir_bind(const IrFnEntry *fns, unsigned count) {
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
    unsigned lo = 0, hi = g_nfns;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (g_fns[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo >= g_nfns || g_fns[lo].addr != addr) return 0;
    return g_fns[lo].end;
}
