/* Differential regression for the SSE/x87 forms Havok needs.
 *
 * The corpus in tests/ir_corpus.h is self-authored i386 and its AOT twin is
 * generated from staged/lift.py.  Here the two functions that carry the new
 * forms are executed through both tiers from identical state:
 *
 *   x87ops: D9 D8..DF (fstp st(i), capstone's "fstpnce") and DF /6 (fbstp)
 *   sseops: cvtdq2ps, punpckl/hwd, punpckl/hdq, packssdw, paddusw, movntps
 *
 * A decoder gap raises rt_unimpl (non-zero exit); a semantic mismatch prints
 * the differing field.  This test does not need the D3DX pilot fixture.
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "ir_corpus.h"
#include "guest.h"

#include <stdio.h>
#include <string.h>

extern const FnEntry g_fn_table_ira[];
extern const unsigned g_fn_count_ira;

#define IN_ADDR   0x60000000u
#define OUT_ADDR  0x60001000u
#define SCR_ADDR  0x60002000u
#define STACK_TOP 0x60018000u
#define TEB_ADDR  0x60020000u

static GuestMemory mem;

static const IrCorpusFn *find_fn(const char *name) {
    for (unsigned i = 0; i < IR_CORPUS_FN_COUNT; ++i)
        if (!strcmp(ir_corpus_fns[i].name, name)) return &ir_corpus_fns[i];
    return NULL;
}

static const FnEntry *find_aot(uint32_t addr) {
    for (unsigned i = 0; i < g_fn_count_ira; ++i)
        if (g_fn_table_ira[i].addr == addr) return &g_fn_table_ira[i];
    return NULL;
}

static void init_state(CPU *c) {
    n3ds_lift_init_cpu(c, STACK_TOP, TEB_ADDR);
    for (uint32_t k = 0; k < 256; ++k)
        M8(SCR_ADDR + k) = (uint8_t)(0x31 + k);
    for (uint32_t k = 0; k < 128; ++k)
        M8(OUT_ADDR + k) = 0;
    for (uint32_t k = 0; k < 16; ++k)
        M32(IN_ADDR + 4 * k) = 0x11110000u + 0x01010101u * k;
    M32(IN_ADDR + 16) = SCR_ADDR;
    M32(IN_ADDR + 20) = SCR_ADDR + 0x100;
    /* Push (arg0, arg1) and a dummy return address. */
    uint32_t sp = STACK_TOP - 12;
    M32(sp) = 0xffffffffu;
    M32(sp + 4) = IN_ADDR;
    M32(sp + 8) = OUT_ADDR;
    c->r[4] = sp;
}

static int same_state(const CPU *a, const CPU *b) {
    if (a->r[0] != b->r[0]) { printf("  eax %08x != %08x\n", a->r[0], b->r[0]); return 0; }
    if (memcmp(a->xmm, b->xmm, sizeof a->xmm)) { printf("  xmm differs\n"); return 0; }
    if (memcmp(a->st, b->st, sizeof a->st)) { printf("  x87 stack differs\n"); return 0; }
    if (a->top != b->top) { printf("  x87 top %u != %u\n", a->top, b->top); return 0; }
    if (a->fpu_cw != b->fpu_cw || a->mxcsr != b->mxcsr) { printf("  control words differ\n"); return 0; }
    if (a->c0 != b->c0 || a->c1 != b->c1 || a->c2 != b->c2 || a->c3 != b->c3) {
        printf("  x87 condition codes differ\n"); return 0;
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, IR_CORPUS_BASE, 0x1000) ||
        guest_map(&mem, IN_ADDR, 0x1000) || guest_map(&mem, OUT_ADDR, 0x1000) ||
        guest_map(&mem, SCR_ADDR, 0x2000) || guest_map(&mem, 0x60010000u, 0x10000) ||
        guest_map(&mem, TEB_ADDR, 0x1000)) {
        printf("map failed\n");
        return 1;
    }
    memcpy(guest_ptr(&mem, IR_CORPUS_BASE, IR_CORPUS_BYTES), ir_corpus_image, IR_CORPUS_BYTES);

    if (n3ds_lift_bind(&mem, g_fn_table_ira, g_fn_count_ira)) { printf("bind AOT failed\n"); return 1; }
    if (n3ds_ir_cache_init(1u << 20)) { printf("cache init failed\n"); return 1; }

    int rc = 0;
    static const char *names[] = {"x87ops", "sseops"};
    for (unsigned n = 0; n < sizeof names / sizeof names[0]; ++n) {
        const IrCorpusFn *fn = find_fn(names[n]);
        const FnEntry *e = fn ? find_aot(fn->addr) : NULL;
        if (!fn || !e) { printf("FAIL %s: fixture entry missing\n", names[n]); rc = 1; continue; }

        static uint8_t scr0[256], out0[128];
        memcpy(scr0, n3ds_guest_ptr(SCR_ADDR, 256), 256);
        memcpy(out0, n3ds_guest_ptr(OUT_ADDR, 128), 128);
        static CPU ca, ci;
        init_state(&ca);
        e->fn(&ca);
        static uint8_t scr_a[256], out_a[128];
        memcpy(scr_a, n3ds_guest_ptr(SCR_ADDR, 256), 256);
        memcpy(out_a, n3ds_guest_ptr(OUT_ADDR, 128), 128);

        memcpy(n3ds_guest_ptr(SCR_ADDR, 256), scr0, 256);
        memcpy(n3ds_guest_ptr(OUT_ADDR, 128), out0, 128);
        init_state(&ci);
        n3ds_ir_run(&ci, fn->addr, fn->end);

        int ok = !memcmp(scr_a, n3ds_guest_ptr(SCR_ADDR, 256), 256) &&
                 !memcmp(out_a, n3ds_guest_ptr(OUT_ADDR, 128), 128) &&
                 same_state(&ca, &ci);
        if (!ok) { printf("FAIL %s: AOT and IR differ\n", names[n]); rc = 1; }
        else printf("ok %s\n", names[n]);
    }
    return rc;
}
