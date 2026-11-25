/* Isolated ARM11/Azahar timing-profile fixture.
 *
 * This harness deliberately links the real IR decoder/interpreter and the real
 * generated AOT fixture, but it does not use or modify the main runtime build.
 * Each case is a deterministic guest function/workload.  AOT and IR are checked
 * for identical return value, CPU state and guest writes before any timing is
 * emitted.  The canonical selector profile is AOT warm time; IR warm and IR
 * cold (decode + execute) are emitted separately.
 *
 * All timings are Azahar emulator timings from svcGetSystemTick, not New 3DS
 * hardware measurements.  See docs/ARM_PROFILING.md for the exact boundaries.
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "ir_corpus.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const FnEntry g_fn_table_ira[];
extern const unsigned g_fn_count_ira;

/* This benchmark calls no imports. */
const FnEntry *win32_lookup(uint32_t addr) { (void)addr; return NULL; }

#define TICK_HZ      268111856u
#define CODE_ADDR    IR_CORPUS_BASE
#define DATA_ADDR    0x60000000u
#define DATA_BYTES   0x7000u
#define IN_ADDR      0x60000000u
#define OUT_ADDR     0x60001000u
#define SCR_ADDR     0x60002000u
#define SCR2_ADDR    0x60004000u
#define MAT_ADDR     0x60006000u
#define STACK_TOP    0x60018000u
#define TEB_ADDR     0x60020000u
#define SEED_A       0x12345678u
#define SEED_B       0x9e3779b9u
#define COLD_REPS    16u

typedef struct {
    uint32_t addr, end, hits;
    const char *name;
    const char *workload;
    uint64_t aot_ticks, ir_warm_ticks, ir_cold_ticks;
    uint64_t warm_cache_hits, warm_cache_misses;
    uint64_t cold_cache_misses, cold_cache_blocks;
    int verified;
    uint32_t expected_result;
} Case;

/* One case per function keeps the selector CSV unique by address.  Workload
 * labels explain the synthetic call mix; they are not a full-game profile. */
static Case cases[] = {
    {0x00501000u, 0x0050105eu, 4096u, "arith",     "integer",  0,0,0,0,0,0,0,0,0},
    {0x00501060u, 0x005010c7u, 4096u, "logic",     "integer",  0,0,0,0,0,0,0,0,0},
    {0x005010d0u, 0x0050112bu, 4096u, "shifts",    "integer",  0,0,0,0,0,0,0,0,0},
    {0x00501130u, 0x0050119au, 2048u, "muldiv",    "integer",  0,0,0,0,0,0,0,0,0},
    {0x005011a0u, 0x00501207u, 4096u, "byteops",   "integer",  0,0,0,0,0,0,0,0,0},
    {0x00501210u, 0x00501277u, 2048u, "memops",    "memory",   0,0,0,0,0,0,0,0,0},
    {0x00501280u, 0x005012f0u, 2048u, "strings",   "memory",   0,0,0,0,0,0,0,0,0},
    {0x005012f0u, 0x00501346u, 4096u, "flags",     "integer",  0,0,0,0,0,0,0,0,0},
    {0x005013a0u, 0x005013fbu, 2048u, "loopback",  "control",  0,0,0,0,0,0,0,0,0},
    {0x00501510u, 0x00501560u, 8192u, "increment", "memory",   0,0,0,0,0,0,0,0,0},
    {0x005015b0u, 0x0050161eu, 2048u, "x87ops",    "x87",      0,0,0,0,0,0,0,0,0},
    {0x00501670u, 0x0050172fu, 1024u, "sseops",    "sse",      0,0,0,0,0,0,0,0,0},
    {0x00501730u, 0x00501780u,  256u, "bench",     "sustained",0,0,0,0,0,0,0,0,0},
    {0x00501780u, 0x005017d4u,  128u, "bench87",   "x87",      0,0,0,0,0,0,0,0,0},
    {0x005017e0u, 0x0050183bu,  128u, "bench128",  "sse",      0,0,0,0,0,0,0,0,0},
};
#define CASE_COUNT (sizeof(cases) / sizeof(cases[0]))

static GuestMemory mem;
static CPU cpu;
static IrFnEntry ir_table[CASE_COUNT];
static uint32_t rnd_state;
typedef struct {
    CPU cpu;
    uint32_t result;
    int rc;
    char fault[256];
    uint8_t memory[DATA_BYTES];
} Capture;

/* Guest snapshots are far larger than the default 3DS application stack. */
static Capture capture_a, capture_b;

static uint64_t ticks(void) { return svcGetSystemTick(); }

/* Bounded stage progress is appended outside timed regions so a timeout still
 * leaves useful evidence in the archived guest log. */
static void progressf(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    printf("%s\n", text);
    FILE *f = fopen("sdmc:/Skyrim_N3DS.log", "a");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* Shared deterministic setup is intentionally outside every timed region. */
static void fill_state(uint32_t seed) {
    rnd_state = seed;
    for (unsigned k = 0; k < 16; ++k) M32(IN_ADDR + 4 * k) = rnd();
    /* The corpus's arith LOOP uses ECX as its count.  Constrain the four words
     * that feed it to exactly one iteration; unconstrained random ECX can mean
     * billions of guest iterations and is not a useful timing fixture. */
    M32(IN_ADDR + 0) = 1;
    M32(IN_ADDR + 4) = 3;
    M32(IN_ADDR + 8) = 2;
    M32(IN_ADDR + 12) = 4;
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
    M32(IN_ADDR + 24) = MAT_ADDR;
}

static int invoke_once(const Case *fn, int ir, uint32_t seed, uint32_t *result) {
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    fill_state(seed);
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    return ir ? n3ds_ir_invoke(&cpu, fn->addr, fn->end, args, 2, result)
              : n3ds_lift_invoke(&cpu, fn->addr, args, 2, result);
}

static int capture_once(const Case *fn, int ir, uint32_t seed, Capture *cap) {
    memset(cap, 0, sizeof *cap);
    cap->rc = invoke_once(fn, ir, seed, &cap->result);
    cap->cpu = cpu;
    snprintf(cap->fault, sizeof cap->fault, "%s", n3ds_lift_fault());
    memcpy(cap->memory, n3ds_guest_ptr(DATA_ADDR, DATA_BYTES), DATA_BYTES);
    return cap->rc;
}

static int captures_match(const Capture *a, const Capture *b, const char *name) {
    if (a->rc != b->rc || a->result != b->result) {
        progressf("ARMTIME VERIFY FAIL %s: rc/result AOT=%d/%08lx IR=%d/%08lx",
                  name, a->rc, (unsigned long)a->result, b->rc, (unsigned long)b->result);
        return 0;
    }
    if (a->rc && strcmp(a->fault, b->fault)) {
        progressf("ARMTIME VERIFY FAIL %s: fault text differs", name);
        return 0;
    }
    if (memcmp(&a->cpu, &b->cpu, sizeof a->cpu)) {
        progressf("ARMTIME VERIFY FAIL %s: CPU state differs", name);
        return 0;
    }
    if (memcmp(a->memory, b->memory, DATA_BYTES)) {
        progressf("ARMTIME VERIFY FAIL %s: guest data writes differ", name);
        return 0;
    }
    return 1;
}

static int verify_case(Case *fn, uint32_t seed) {
    progressf("ARMTIME verify %s seed=%08lx tier=AOT begin", fn->name, (unsigned long)seed);
    capture_once(fn, 0, seed, &capture_a);
    progressf("ARMTIME verify %s tier=AOT done rc=%d", fn->name, capture_a.rc);
    if (seed == SEED_A && capture_a.rc == 0) fn->expected_result = capture_a.result;
    n3ds_ir_cache_reset();
    progressf("ARMTIME verify %s seed=%08lx tier=IR begin", fn->name, (unsigned long)seed);
    capture_once(fn, 1, seed, &capture_b);
    progressf("ARMTIME verify %s tier=IR done rc=%d", fn->name, capture_b.rc);
    return capture_a.rc == 0 && capture_b.rc == 0 &&
           captures_match(&capture_a, &capture_b, fn->name);
}

/* Time only CPU initialization plus the tier invocation.  fill_state() and the
 * two svcGetSystemTick calls are outside the measured interval; the timer reads
 * themselves naturally bound the interval. */
static int timed_once(const Case *fn, int ir, uint32_t seed,
                      uint64_t *elapsed, uint32_t *result) {
    uint32_t args[2] = {IN_ADDR, OUT_ADDR};
    int rc;
    fill_state(seed);
    uint64_t t0 = ticks();
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    rc = ir ? n3ds_ir_invoke(&cpu, fn->addr, fn->end, args, 2, result)
            : n3ds_lift_invoke(&cpu, fn->addr, args, 2, result);
    *elapsed = ticks() - t0;
    return rc;
}

static uint64_t measure_aot(const Case *fn) {
    uint64_t total = 0, elapsed;
    uint32_t result = 0;
    for (uint32_t i = 0; i < fn->hits; ++i) {
        if (timed_once(fn, 0, SEED_A, &elapsed, &result) ||
            result != fn->expected_result) return UINT64_MAX;
        total += elapsed;
    }
    return total;
}

static uint64_t measure_ir_warm(Case *fn) {
    uint64_t total = 0, elapsed, ignored;
    uint32_t result = 0;
    IrCacheStats before, after;
    /* Decode once outside the timed region. */
    n3ds_ir_cache_reset();
    if (timed_once(fn, 1, SEED_A, &ignored, &result) ||
        result != fn->expected_result) return UINT64_MAX;
    n3ds_ir_cache_stats(&before);
    for (uint32_t i = 0; i < fn->hits; ++i) {
        if (timed_once(fn, 1, SEED_A, &elapsed, &result) ||
            result != fn->expected_result) return UINT64_MAX;
        total += elapsed;
    }
    n3ds_ir_cache_stats(&after);
    fn->warm_cache_hits = after.hits - before.hits;
    fn->warm_cache_misses = after.misses - before.misses;
    return total;
}

static uint64_t measure_ir_cold(Case *fn) {
    uint64_t total = 0, elapsed;
    uint32_t result = 0;
    IrCacheStats before, after;
    n3ds_ir_cache_stats(&before);
    for (uint32_t i = 0; i < COLD_REPS; ++i) {
        n3ds_ir_cache_reset();            /* reset cost is not timed */
        if (timed_once(fn, 1, SEED_A, &elapsed, &result) ||
            result != fn->expected_result) return UINT64_MAX;
        total += elapsed;
    }
    n3ds_ir_cache_stats(&after);
    fn->cold_cache_misses = after.misses - before.misses;
    fn->cold_cache_blocks = after.blocks - before.blocks;
    return total;
}

static uint64_t decode_estimate(const Case *fn) {
    uint64_t warm_per_call = fn->ir_warm_ticks / fn->hits;
    uint64_t warm_equivalent = warm_per_call * COLD_REPS;
    return fn->ir_cold_ticks > warm_equivalent ? fn->ir_cold_ticks - warm_equivalent : 0;
}

static int close_checked(FILE *f, const char *path) {
    int bad = ferror(f);
    if (fclose(f)) bad = 1;
    if (bad) progressf("ARMTIME FILE FAIL %s", path);
    return !bad;
}

static int write_csvs(void) {
    FILE *f = fopen("sdmc:/armtime_profile.csv", "w");
    FILE *a = fopen("sdmc:/armtime_aot.csv", "w");
    FILE *w = fopen("sdmc:/armtime_ir_warm.csv", "w");
    FILE *c = fopen("sdmc:/armtime_ir_cold.csv", "w");
    int ok = f && a && w && c;
    if (!ok) goto done;
    fprintf(f, "addr,hits,ticks\n");
    fprintf(a, "addr,hits,ticks\n");
    fprintf(w, "addr,hits,ticks\n");
    fprintf(c, "addr,hits,ticks\n");
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        const Case *fn = &cases[i];
        fprintf(f, "%08lx,%lu,%llu\n", (unsigned long)fn->addr,
                (unsigned long)fn->hits, (unsigned long long)fn->aot_ticks);
        fprintf(a, "%08lx,%lu,%llu\n", (unsigned long)fn->addr,
                (unsigned long)fn->hits, (unsigned long long)fn->aot_ticks);
        fprintf(w, "%08lx,%lu,%llu\n", (unsigned long)fn->addr,
                (unsigned long)fn->hits, (unsigned long long)fn->ir_warm_ticks);
        fprintf(c, "%08lx,%lu,%llu\n", (unsigned long)fn->addr,
                (unsigned long)COLD_REPS, (unsigned long long)fn->ir_cold_ticks);
    }
done:
    if (f) ok &= close_checked(f, "armtime_profile.csv");
    if (a) ok &= close_checked(a, "armtime_aot.csv");
    if (w) ok &= close_checked(w, "armtime_ir_warm.csv");
    if (c) ok &= close_checked(c, "armtime_ir_cold.csv");
    return ok;
}

static int write_metadata(void) {
    FILE *f = fopen("sdmc:/armtime_meta.json", "w");
    if (!f) return 0;
    uint64_t aot = 0, warm = 0, cold = 0, decode = 0, total_hits = 0;
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        aot += cases[i].aot_ticks;
        warm += cases[i].ir_warm_ticks;
        cold += cases[i].ir_cold_ticks;
        decode += decode_estimate(&cases[i]);
        total_hits += cases[i].hits;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"format_version\": 1,\n");
    fprintf(f, "  \"benchmark\": \"armtime-fixture-v1\",\n");
    fprintf(f, "  \"timing_scope\": \"azahar_emulator\",\n");
    fprintf(f, "  \"hardware_time_claim\": false,\n");
    fprintf(f, "  \"clock_source\": \"svcGetSystemTick\",\n");
    fprintf(f, "  \"tick_hz\": %u,\n", TICK_HZ);
    fprintf(f, "  \"time_unit\": \"ticks\",\n");
    fprintf(f, "  \"canonical_tier\": \"aot_warm\",\n");
    fprintf(f, "  \"canonical_profile\": \"armtime_profile.csv\",\n");
    fprintf(f, "  \"profile_semantics\": \"AOT warm total ticks; excludes state fill and cache reset; includes CPU init and invoke\",\n");
    fprintf(f, "  \"cold_semantics\": \"IR cache reset before each single invocation; includes decode plus execute\",\n");
    fprintf(f, "  \"verification\": \"pass\",\n");
    fprintf(f, "  \"verification_inputs\": 2,\n");
    fprintf(f, "  \"postflight_verification\": \"pass\",\n");
    fprintf(f, "  \"fixture\": \"tests/ir_corpus.h + tests/ir_aot_fixture.c\",\n");
    fprintf(f, "  \"notes\": \"Azahar emulator timings only; not New 3DS hardware measurements\",\n");
    fprintf(f, "  \"functions\": [\n");
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        const Case *fn = &cases[i];
        fprintf(f, "    {\"addr\": \"0x%08lx\", \"end\": \"0x%08lx\", "
                   "\"name\": \"%s\", \"workload\": \"%s\", \"hits\": %lu, "
                   "\"aot_ticks\": %llu, \"ir_warm_ticks\": %llu, "
                   "\"ir_cold_ticks\": %llu, \"cold_reps\": %u, "
                   "\"decode_est_ticks\": %llu, \"warm_cache_hits\": %llu, "
                   "\"warm_cache_misses\": %llu, \"cold_cache_misses\": %llu, "
                   "\"cold_cache_blocks\": %llu, \"verified\": true}%s\n",
                (unsigned long)fn->addr, (unsigned long)fn->end, fn->name, fn->workload,
                (unsigned long)fn->hits, (unsigned long long)fn->aot_ticks,
                (unsigned long long)fn->ir_warm_ticks, (unsigned long long)fn->ir_cold_ticks,
                COLD_REPS, (unsigned long long)decode_estimate(fn),
                (unsigned long long)fn->warm_cache_hits,
                (unsigned long long)fn->warm_cache_misses,
                (unsigned long long)fn->cold_cache_misses,
                (unsigned long long)fn->cold_cache_blocks,
                i + 1 == CASE_COUNT ? "" : ",");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"totals\": {\"hits\": %llu, \"aot_ticks\": %llu, "
               "\"ir_warm_ticks\": %llu, \"ir_cold_ticks\": %llu, "
               "\"decode_est_ticks\": %llu}\n",
            (unsigned long long)total_hits,
            (unsigned long long)aot, (unsigned long long)warm,
            (unsigned long long)cold, (unsigned long long)decode);
    fprintf(f, "}\n");
    return close_checked(f, "armtime_meta.json");
}

static int write_log(int pass) {
    FILE *f = fopen("sdmc:/Skyrim_N3DS.log", "a");
    if (!f) return 0;
    fprintf(f, "ARMTIME scope=Azahar emulator (NOT New 3DS hardware)\n");
    fprintf(f, "ARMTIME clock=svcGetSystemTick tick_hz=%u unit=ticks\n", TICK_HZ);
    fprintf(f, "ARMTIME fixture=armtime-fixture-v1 functions=%lu cold_reps=%u\n",
            (unsigned long)CASE_COUNT, COLD_REPS);
    if (pass) {
        for (unsigned i = 0; i < CASE_COUNT; ++i) {
            const Case *fn = &cases[i];
            fprintf(f, "CASE %08lx %-10s workload=%-9s hits=%lu aot=%llu "
                       "ir_warm=%llu ir_cold=%llu decode_est=%llu\n",
                    (unsigned long)fn->addr, fn->name, fn->workload,
                    (unsigned long)fn->hits, (unsigned long long)fn->aot_ticks,
                    (unsigned long long)fn->ir_warm_ticks,
                    (unsigned long long)fn->ir_cold_ticks,
                    (unsigned long long)decode_estimate(fn));
        }
        fprintf(f, "ARMTIME RESULT: PASS\n");
    } else {
        fprintf(f, "ARMTIME RESULT: FAIL\n");
    }
    return close_checked(f, "Skyrim_N3DS.log");
}

static void remove_outputs(void) {
    const char *paths[] = {
        "sdmc:/Skyrim_N3DS.log", "sdmc:/armtime_profile.csv", "sdmc:/armtime_aot.csv",
        "sdmc:/armtime_ir_warm.csv", "sdmc:/armtime_ir_cold.csv", "sdmc:/armtime_meta.json"
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) remove(paths[i]);
}

int main(void) {
    int bad = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    remove_outputs();
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    osSetSpeedupEnable(true);
    printf("armtime: isolated AOT/IR profile fixture\n");
    printf("scope: Azahar emulator timing, NOT New 3DS hardware\n");
    progressf("ARMTIME stage=start scope=Azahar emulator (NOT New 3DS hardware)");

    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, CODE_ADDR, 0x1000) || guest_map(&mem, DATA_ADDR, DATA_BYTES) ||
        guest_map(&mem, 0x60010000u, 0x10000) || guest_map(&mem, TEB_ADDR, 0x1000)) {
        progressf("ARMTIME FAIL: guest map");
        bad = 1;
        goto out;
    }
    memcpy(guest_ptr(&mem, CODE_ADDR, IR_CORPUS_BYTES), ir_corpus_image, IR_CORPUS_BYTES);
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        ir_table[i].addr = cases[i].addr;
        ir_table[i].end = cases[i].end;
    }
    if (n3ds_lift_bind(&mem, g_fn_table_ira, g_fn_count_ira) ||
        n3ds_ir_bind(ir_table, CASE_COUNT) || n3ds_ir_cache_init(1u << 20)) {
        progressf("ARMTIME FAIL: AOT/IR bind or cache init");
        bad = 1;
        goto out;
    }

    progressf("ARMTIME stage=verify functions=%lu", (unsigned long)CASE_COUNT);
    /* Verification gate: no timing is written unless every case matches on two
     * deterministic inputs, including the complete CPU and all guest data. */
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        cases[i].verified = verify_case(&cases[i], SEED_A) && verify_case(&cases[i], SEED_B);
        if (!cases[i].verified) {
            progressf("ARMTIME VERIFY FAIL %s", cases[i].name);
            bad = 1;
            goto out;
        }
        progressf("ARMTIME verified %08lx %s", (unsigned long)cases[i].addr, cases[i].name);
    }
    progressf("ARMTIME verification: PASS (%lu functions, 2 inputs each)",
              (unsigned long)CASE_COUNT);

    progressf("ARMTIME stage=measure");
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        Case *fn = &cases[i];
        fn->aot_ticks = measure_aot(fn);
        fn->ir_warm_ticks = measure_ir_warm(fn);
        fn->ir_cold_ticks = measure_ir_cold(fn);
        if (fn->aot_ticks == UINT64_MAX || fn->ir_warm_ticks == UINT64_MAX ||
            fn->ir_cold_ticks == UINT64_MAX || !fn->warm_cache_hits ||
            fn->warm_cache_misses || !fn->cold_cache_misses) {
            progressf("ARMTIME MEASURE FAIL %s (warm hits=%llu misses=%llu cold misses=%llu)",
                      fn->name, (unsigned long long)fn->warm_cache_hits,
                      (unsigned long long)fn->warm_cache_misses,
                      (unsigned long long)fn->cold_cache_misses);
            bad = 1;
            goto out;
        }
        progressf("CASE %08lx %-10s hits=%lu aot=%llu ir_warm=%llu ir_cold=%llu ticks",
                  (unsigned long)fn->addr, fn->name, (unsigned long)fn->hits,
                  (unsigned long long)fn->aot_ticks,
                  (unsigned long long)fn->ir_warm_ticks,
                  (unsigned long long)fn->ir_cold_ticks);
    }

    progressf("ARMTIME stage=postflight");
    /* Postflight catches state/cache contamination after the timed loops. */
    for (unsigned i = 0; i < CASE_COUNT; ++i) {
        if (!verify_case(&cases[i], SEED_B)) {
            progressf("ARMTIME POSTFLIGHT FAIL %s", cases[i].name);
            bad = 1;
            goto out;
        }
    }
    progressf("ARMTIME stage=write_outputs");
    if (!write_csvs() || !write_metadata() || !write_log(1)) bad = 1;
    if (!bad) progressf("ARMTIME outputs=written");

out:
    if (bad) {
        const char *results[] = {
            "sdmc:/armtime_profile.csv", "sdmc:/armtime_aot.csv",
            "sdmc:/armtime_ir_warm.csv", "sdmc:/armtime_ir_cold.csv",
            "sdmc:/armtime_meta.json"
        };
        for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i) remove(results[i]);
        progressf("ARMTIME RESULT: FAIL");
    }
    guest_destroy(&mem);
    svcSleepThread(5000000000ULL);
    gfxExit();
    return bad ? 1 : 0;
}
