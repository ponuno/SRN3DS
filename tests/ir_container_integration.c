/* Integrated runtime growth-style test.
 *
 * The bound AOT table deliberately omits `direct` and `indirect` from the
 * self-authored corpus image and there is no generated slice IR table. The
 * target is present only in the 90,900-function container (or the small corpus
 * container in standalone mode), so a successful invoke proves the integrated
 * runtime entered a predecoded container function without lifting/rebuilding.
 *
 * `direct` then calls `x_callee` through rt_call, which is bound AOT, proving a
 * container record calls back into AOT (not a second container record and not
 * a decoder fallback).
 *
 * Modes:
 *   ir_container_integration CONTAINER.IRC [require-missing]
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "ir_container.h"
#include "ir_corpus.h"
#include "guest.h"
#include "rt_table.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

extern const FnEntry g_fn_table_ira[];
extern const unsigned g_fn_count_ira;

#define IN_ADDR   0x60000000u
#define OUT_ADDR  0x60001000u
#define SCR_ADDR  0x60002000u
#define SCR2_ADDR 0x60004000u
#define MAT_ADDR  0x60006000u
#define STACK_TOP 0x60018000u
#define TEB_ADDR  0x60020000u

static GuestMemory mem;
static FnEntry aot[128];
static unsigned naot;
static const FnEntry *callee_entry;
static int callee_calls;
static int expect_container_callee;
static const IrCorpusFn *direct_fn, *indirect_fn;

const FnEntry *win32_lookup(uint32_t address) { (void)address; return NULL; }

static void callee_wrap(CPU *c) {
    callee_calls++;
    callee_entry->fn(c);
}

static const IrCorpusFn *find_fn(const char *name) {
    for (unsigned i = 0; i < IR_CORPUS_FN_COUNT; ++i)
        if (!strcmp(ir_corpus_fns[i].name, name)) return &ir_corpus_fns[i];
    return NULL;
}

static const FnEntry *fixture_entry(uint32_t addr) {
    for (unsigned i = 0; i < g_fn_count_ira; ++i)
        if (g_fn_table_ira[i].addr == addr) return &g_fn_table_ira[i];
    return NULL;
}

static void fill_state(CPU *c, uint32_t call_target) {
    static const uint32_t vals[16] = {
        0x11223344u, 0, 0x80000000u, 0xffffffffu, 7, 0x55aa55aau,
        0, 0x12345678u, 0x9abcdef0u, 0x10203040u, 0, 0xdeadbeefu,
        0x77777777u, 0x88888888u, 0x01020304u, 0xf0e0d0c0u,
    };
    for (unsigned i = 0; i < 16; ++i) M32(IN_ADDR + 4 * i) = vals[i];
    for (unsigned k = 0; k < 128; ++k) {
        M8(SCR_ADDR + k) = (uint8_t)(0x11 + k);
        M8(SCR2_ADDR + k) = (uint8_t)(0x80 + k);
        M8(OUT_ADDR + k) = 0;
    }
    M32(IN_ADDR + 8) = call_target;
    M32(IN_ADDR + 16) = SCR_ADDR;
    M32(IN_ADDR + 20) = SCR2_ADDR;
    M32(IN_ADDR + 24) = 0xdeadbe00u;
    n3ds_lift_init_cpu(c, STACK_TOP, TEB_ADDR);
}

static int setup_stack(CPU *c, const uint32_t *args, size_t argc, uint32_t *base_out,
                       uint32_t *sp_out) {
    uint32_t sp = c->r[4];
    uint32_t frame = (uint32_t)((argc + 1) * 4);
    if (sp < frame) return -1;
    uint32_t base = sp - frame;
    if (!guest_ptr(&mem, base, frame)) return -1;
    M32(base) = 0xffffffffu;
    for (size_t i = 0; i < argc; ++i) M32(base + 4 + (uint32_t)i * 4) = args[i];
    c->r[4] = base;
    *base_out = base;
    *sp_out = sp;
    return 0;
}

static int invoke_aot_direct(const IrCorpusFn *fn, uint32_t call_target, CPU *c,
                             uint32_t *res) {
    const FnEntry *e = fixture_entry(fn->addr);
    if (!e) return -1;
    fill_state(c, call_target);
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    uint32_t base, sp;
    if (setup_stack(c, args, 2, &base, &sp)) return -1;
    e->fn(c);
    int ok = c->r[4] == base + 4 || c->r[4] == sp;
    c->r[4] = sp;
    *res = c->r[0];
    return ok ? 0 : -1;
}

static int invoke_container(const IrCorpusFn *fn, uint32_t call_target, CPU *c,
                            uint32_t *res, int *invoke_rc) {
    fill_state(c, call_target);
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    *invoke_rc = n3ds_lift_invoke(c, fn->addr, args, 2, res);
    return *invoke_rc;
}

static int compare_run(const IrCorpusFn *fn) {
    uint32_t call_target = IR_CALLEE_ADDR;
    CPU ca, cc;
    uint32_t ra = 0, rcres = 0;
    int rc;
    callee_calls = 0;
    if (invoke_aot_direct(fn, call_target, &ca, &ra)) {
        printf("FAIL %s: AOT direct run failed\n", fn->name);
        return 1;
    }
    int aot_callee_calls = callee_calls;
    uint8_t out_a[128], scr_a[256], scr2_a[256];
    memcpy(out_a, n3ds_guest_ptr(OUT_ADDR, sizeof out_a), sizeof out_a);
    memcpy(scr_a, n3ds_guest_ptr(SCR_ADDR, sizeof scr_a), sizeof scr_a);
    memcpy(scr2_a, n3ds_guest_ptr(SCR2_ADDR, sizeof scr2_a), sizeof scr2_a);

    callee_calls = 0;
    rc = invoke_container(fn, call_target, &cc, &rcres, &rc);
    if (rc != N3DS_INVOKE_OK) {
        printf("FAIL %s: container invoke rc=%d fault='%s'\n", fn->name, rc,
               n3ds_lift_fault());
        return 1;
    }
    if (ra != rcres || memcmp(&ca, &cc, sizeof ca)) {
        printf("FAIL %s: result/CPU differs AOT=%08x container=%08x\n",
               fn->name, ra, rcres);
        return 1;
    }
    if (memcmp(out_a, n3ds_guest_ptr(OUT_ADDR, sizeof out_a), sizeof out_a) ||
        memcmp(scr_a, n3ds_guest_ptr(SCR_ADDR, sizeof scr_a), sizeof scr_a) ||
        memcmp(scr2_a, n3ds_guest_ptr(SCR2_ADDR, sizeof scr2_a), sizeof scr2_a)) {
        printf("FAIL %s: guest memory differs\n", fn->name);
        return 1;
    }
    if (!expect_container_callee && callee_calls == 0) {
        printf("FAIL %s: container did not call back into AOT (AOT run used %d "
               "direct calls)\n", fn->name, aot_callee_calls);
        return 1;
    }
    printf("  ok %-8s result %08x, %s callee calls %d "
           "(AOT fixture direct calls %d)\n",
           fn->name, rcres,
           expect_container_callee ? "container->container" : "container->AOT",
           callee_calls, aot_callee_calls);
    return 0;
}

static int setup_guest(void) {
    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, IR_CORPUS_BASE, 0x1000) ||
        guest_map(&mem, IN_ADDR, 0x1000) || guest_map(&mem, OUT_ADDR, 0x1000) ||
        guest_map(&mem, SCR_ADDR, 0x2000) || guest_map(&mem, SCR2_ADDR, 0x2000) ||
        guest_map(&mem, MAT_ADDR, 0x1000) || guest_map(&mem, 0x60010000u, 0x10000) ||
        guest_map(&mem, TEB_ADDR, 0x1000)) {
        printf("guest map failed\n");
        return 1;
    }
    void *code = guest_ptr(&mem, IR_CORPUS_BASE, IR_CORPUS_BYTES);
    if (!code) { printf("code map failed\n"); return 1; }
    memcpy(code, ir_corpus_image, IR_CORPUS_BYTES);
    return 0;
}

static int build_aot_table(int exclude_callee) {
    naot = 0;
    for (unsigned i = 0; i < g_fn_count_ira; ++i) {
        const FnEntry *e = &g_fn_table_ira[i];
        if (e->addr == direct_fn->addr || e->addr == indirect_fn->addr) continue;
        if (exclude_callee && e->addr == IR_CALLEE_ADDR) continue;
        aot[naot] = *e;
        if (e->addr == IR_CALLEE_ADDR) {
            callee_entry = e;
            aot[naot].fn = callee_wrap;
        }
        naot++;
    }
    if (!exclude_callee && !callee_entry) {
        printf("callee missing from fixture\n");
        return -1;
    }
    for (unsigned i = 1; i < naot; ++i) {
        FnEntry e = aot[i];
        unsigned j = i;
        while (j && aot[j - 1].addr > e.addr) { aot[j] = aot[j - 1]; --j; }
        aot[j] = e;
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s CONTAINER.IRC [require-missing]\n", argv[0]);
        return 2;
    }
    if (setup_guest()) return 2;
    direct_fn = find_fn("direct");
    indirect_fn = find_fn("indirect");
    if (!direct_fn || !indirect_fn) { printf("corpus targets missing\n"); return 2; }
    expect_container_callee = argc > 2 && !strcmp(argv[2], "container-callee");
    if (build_aot_table(expect_container_callee)) return 2;

    if (n3ds_lift_set_container_path(argv[1])) {
        printf("set container path failed\n");
        return 2;
    }
    if (n3ds_lift_bind(&mem, aot, naot)) {
        printf("bind failed: %s\n", n3ds_lift_fault());
        return (argc > 2 && !strcmp(argv[2], "require-missing")) ? 0 : 2;
    }
    if (argc > 2 && !strcmp(argv[2], "require-missing")) {
        printf("FAIL: required container bind unexpectedly succeeded\n");
        return 1;
    }
    if (n3ds_lift_container_state() != 0) {
        printf("FAIL: container opened before a non-AOT lookup\n");
        return 1;
    }

    IrCacheStats ir_before, ir_after;
    n3ds_ir_cache_stats(&ir_before);

    /* The first call must open the container and route to the predecoded
     * record. `direct`/`indirect` are absent from the bound AOT table. */
    if (!n3ds_lift_has_function(direct_fn->addr)) {
        printf("FAIL: has_function missed the container target\n");
        return 1;
    }
    if (n3ds_lift_container_state() != 1) {
        printf("FAIL: container did not open lazily\n");
        return 1;
    }

    int fail = 0;
    clock_t t0 = clock();
    if (compare_run(direct_fn)) fail = 1;
    if (compare_run(indirect_fn)) fail = 1;
    clock_t t1 = clock();

    n3ds_ir_cache_stats(&ir_after);
    N3dsIrcStats st;
    memset(&st, 0, sizeof st);
    if (n3ds_lift_container_stats(&st)) {
        printf("FAIL: container stats unavailable\n");
        fail = 1;
    } else {
        printf("container stats: loads %llu bytes %llu load_ns %llu hits %llu "
               "misses %llu evictions %llu execs %llu block_hits %llu "
               "redecodes %llu\n",
               (unsigned long long)st.loads, (unsigned long long)st.load_bytes,
               (unsigned long long)st.load_ns, (unsigned long long)st.cache_hits,
               (unsigned long long)st.cache_misses,
               (unsigned long long)st.evictions, (unsigned long long)st.execs,
               (unsigned long long)st.block_hits, (unsigned long long)st.redecodes);
        uint64_t min_execs = expect_container_callee ? 4 : 2;
        if (st.redecodes != 0 || st.execs < min_execs) {
            printf("FAIL: container did not execute predecoded records\n");
            fail = 1;
        }
    }
    if (ir_after.blocks != ir_before.blocks) {
        printf("FAIL: slice decoder ran during container execution (%llu -> %llu)\n",
               (unsigned long long)ir_before.blocks,
               (unsigned long long)ir_after.blocks);
        fail = 1;
    }
    printf("target wall: %.3f ms (host clock), container execs %llu\n",
           1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC,
           (unsigned long long)st.execs);
    n3ds_lift_release();
    guest_destroy(&mem);
    if (!fail) printf("IR container integration PASS (%s)\n",
                      expect_container_callee ? "container->container" : "container->AOT");
    return fail ? 1 : 0;
}
