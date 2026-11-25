/* ARM11/Azahar microbenchmark: the same deterministic corpus through AOT and
 * through the IR interpreter. It is built with IRBENCH=1 (see Makefile) and
 * writes sdmc:/ir_bench.txt in addition to the console. It reports cold
 * (decode + run) and warm (run) interpreter time separately; a verification
 * failure is reported as a failure, never as a successful benchmark.
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "ir_corpus.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>

extern const FnEntry g_fn_table_ira[];
extern const unsigned g_fn_count_ira;

/* This benchmark calls no imports. */
const FnEntry *win32_lookup(uint32_t addr) { (void)addr; return NULL; }

#define SYSCLOCK_CPU 268111856.0
#define IN_ADDR   0x60000000u
#define OUT_ADDR  0x60001000u
#define SCR_ADDR  0x60002000u
#define SCR2_ADDR 0x60004000u
#define MAT_ADDR  0x60006000u
#define STACK_TOP 0x60018000u
#define TEB_ADDR  0x60020000u
#define REPS      2000
#define COLD_REPS 200

static GuestMemory mem;
static CPU cpu;
static uint32_t rnd_state = 0x12345678u;

static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void fill_state(void) {
    rnd_state = 0x12345678u;
    for (unsigned k = 0; k < 16; ++k) M32(IN_ADDR + 4 * k) = rnd();
    for (unsigned k = 0; k < 128; ++k) {
        *(uint8_t *)n3ds_guest_ptr(SCR_ADDR + k, 1) = (uint8_t)(0x11 + k);
        *(uint8_t *)n3ds_guest_ptr(SCR2_ADDR + k, 1) = (uint8_t)(0x80 + k);
        *(uint8_t *)n3ds_guest_ptr(OUT_ADDR + k, 1) = 0;
    }
    for (unsigned k = 0; k < 48; ++k) {
        float f = (float)((int32_t)(rnd() % 2001) - 1000) / 8.0f;
        memcpy(n3ds_guest_ptr(MAT_ADDR + 4 * k, 4), &f, 4);
    }
    M32(IN_ADDR + 8) = IR_CALLEE_ADDR;
    M32(IN_ADDR + 16) = SCR_ADDR;
    M32(IN_ADDR + 20) = SCR2_ADDR;
    M32(IN_ADDR + 24) = 0xdeadbe00u;
}

static uint32_t run_aot(const IrCorpusFn *fn, uint32_t target) {
    uint32_t r = 0;
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    fill_state();
    M32(IN_ADDR + 8) = target;
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    n3ds_lift_invoke(&cpu, fn->addr, args, 2, &r);
    return r;
}

static uint32_t run_ir(const IrCorpusFn *fn, uint32_t target) {
    uint32_t r = 0;
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    fill_state();
    M32(IN_ADDR + 8) = target;
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    n3ds_ir_invoke(&cpu, fn->addr, fn->end, args, 2, &r);
    return r;
}

/* Timing variants: state is filled by the caller, so the timed region is the
 * tier itself, not the shared setup code. */
static void run_aot_raw(const IrCorpusFn *fn, uint32_t target) {
    uint32_t r = 0;
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    M32(IN_ADDR + 8) = target;
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    n3ds_lift_invoke(&cpu, fn->addr, args, 2, &r);
}

static void run_ir_raw(const IrCorpusFn *fn, uint32_t target) {
    uint32_t r = 0;
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    M32(IN_ADDR + 8) = target;
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    n3ds_ir_invoke(&cpu, fn->addr, fn->end, args, 2, &r);
}

static int is_bench(const IrCorpusFn *fn) {
    return strcmp(fn->name, "pilotcall") != 0; /* pilot AOT table is not linked */
}

static uint64_t ticks(void) { return svcGetSystemTick(); }

int main(void) {
    FILE *f = NULL;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, IR_CORPUS_BASE, 0x1000) ||
        guest_map(&mem, IN_ADDR, 0x1000) || guest_map(&mem, OUT_ADDR, 0x1000) ||
        guest_map(&mem, SCR_ADDR, 0x2000) || guest_map(&mem, SCR2_ADDR, 0x2000) ||
        guest_map(&mem, MAT_ADDR, 0x1000) || guest_map(&mem, 0x60010000u, 0x10000) ||
        guest_map(&mem, TEB_ADDR, 0x1000)) {
        printf("IRBENCH FAIL: guest map\n");
        goto out;
    }
    memcpy(guest_ptr(&mem, IR_CORPUS_BASE, IR_CORPUS_BYTES), ir_corpus_image, IR_CORPUS_BYTES);
    int br = n3ds_lift_bind(&mem, g_fn_table_ira, g_fn_count_ira);
    if (br) {
        printf("IRBENCH FAIL: AOT bind %d\n", br);
        goto out;
    }
    static const IrFnEntry irtab[] = {{IR_HELPER_ADDR, IR_HELPER_END}};
    if (n3ds_ir_bind(irtab, 1) || n3ds_ir_cache_init(1u << 20)) {
        printf("IRBENCH FAIL: IR bind/cache\n");
        goto out;
    }

    int bad = 0;
    unsigned n = 0;
    for (unsigned f = 0; f < IR_CORPUS_FN_COUNT; ++f) {
        const IrCorpusFn *fn = &ir_corpus_fns[f];
        if (!is_bench(fn)) continue;
        uint32_t ta = run_aot(fn, IR_CALLEE_ADDR);
        uint32_t tr = run_ir(fn, IR_CALLEE_ADDR);
        if (ta != tr) {
            printf("IRBENCH VERIFY FAIL %s: %08x != %08x\n", fn->name, (unsigned)ta, (unsigned)tr);
            bad = 1;
        }
        n++;
    }

    /* AOT: warm code, no decode. */
    fill_state();
    uint64_t t0 = ticks();
    for (unsigned r = 0; r < REPS; ++r)
        for (unsigned f = 0; f < IR_CORPUS_FN_COUNT; ++f)
            if (is_bench(&ir_corpus_fns[f])) run_aot_raw(&ir_corpus_fns[f], IR_CALLEE_ADDR);
    uint64_t t_aot = ticks() - t0;

    /* IR warm: decode once, then execute. */
    fill_state();
    n3ds_ir_cache_reset();
    for (unsigned f = 0; f < IR_CORPUS_FN_COUNT; ++f)
        if (is_bench(&ir_corpus_fns[f])) run_ir_raw(&ir_corpus_fns[f], IR_CALLEE_ADDR);
    t0 = ticks();
    for (unsigned r = 0; r < REPS; ++r)
        for (unsigned f = 0; f < IR_CORPUS_FN_COUNT; ++f)
            if (is_bench(&ir_corpus_fns[f])) run_ir_raw(&ir_corpus_fns[f], IR_CALLEE_ADDR);
    uint64_t t_warm = ticks() - t0;

    /* IR cold: cache reset before every pass. */
    fill_state();
    t0 = ticks();
    for (unsigned r = 0; r < COLD_REPS; ++r) {
        n3ds_ir_cache_reset();
        for (unsigned f = 0; f < IR_CORPUS_FN_COUNT; ++f)
            if (is_bench(&ir_corpus_fns[f])) run_ir_raw(&ir_corpus_fns[f], IR_CALLEE_ADDR);
    }
    uint64_t t_cold = ticks() - t0;

    double us = 1e6 / SYSCLOCK_CPU;
    double aot_pass = t_aot * us / REPS;
    double warm_pass = t_warm * us / REPS;
    double cold_pass = t_cold * us / COLD_REPS;
    double per_aot = aot_pass / n, per_warm = warm_pass / n, per_cold = cold_pass / n;

    f = fopen("sdmc:/ir_bench.txt", "w");
    FILE *outs[2] = {stdout, f};
    for (unsigned k = 0; k < 2; ++k) {
        FILE *o = outs[k];
        if (!o) continue;
        fprintf(o, "IRBENCH functions=%u reps=%u cold_reps=%u verify=%s\n",
                n, REPS, COLD_REPS, bad ? "FAIL" : "ok");
        fprintf(o, "AOT       total_ticks=%llu us_per_pass=%.1f us_per_fn=%.2f\n",
                (unsigned long long)t_aot, aot_pass, per_aot);
        fprintf(o, "IR warm   total_ticks=%llu us_per_pass=%.1f us_per_fn=%.2f\n",
                (unsigned long long)t_warm, warm_pass, per_warm);
        fprintf(o, "IR cold   total_ticks=%llu us_per_pass=%.1f us_per_fn=%.2f\n",
                (unsigned long long)t_cold, cold_pass, per_cold);
        fprintf(o, "decode    us_per_pass=%.1f (cold - warm)\n", cold_pass - warm_pass);
        fprintf(o, "ratio     IR warm / AOT = %.2fx\n", warm_pass / (aot_pass ? aot_pass : 1));
    }
    if (f) fclose(f);

    printf("IRBENCH done; see sdmc:/ir_bench.txt\n");
    svcSleepThread(5000000000ULL);
    return bad ? 1 : 0;
out:
    if ((f = fopen("sdmc:/ir_bench.txt", "w"))) {
        fprintf(f, "IRBENCH FAIL\n");
        fclose(f);
    }
    return 1;
}
