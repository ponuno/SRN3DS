/* Targeted growth-style test against the real 90,900-function container.
 *
 * The AOT table contains only a dummy at 0x70000000; there is no generated
 * slice IR table. TESV 0x401860 is `mov eax,0x67; ret` and exists only in the
 * container index. Repeated invokes prove the integrated runtime enters a
 * predecoded record, caches it, and never calls the decoder/lifter.
 *
 * Usage:
 *   ir_container_growth CONTAINER.IRC [repeat] [require-missing]
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "ir_container.h"
#include "guest.h"
#include "rt_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TARGET_ADDR 0x00401860u
#define TARGET_RESULT 0x67u
#define DUMMY_ADDR 0x70000000u
#define STACK_TOP 0x60018000u
#define TEB_ADDR 0x60020000u

static GuestMemory mem;

static void dummy(CPU *c) { (void)c; }
static void target_dummy(CPU *c) { (void)c; }

const FnEntry *win32_lookup(uint32_t address) { (void)address; return NULL; }

static int run_once(uint32_t *result) {
    CPU c;
    n3ds_lift_init_cpu(&c, STACK_TOP, TEB_ADDR);
    return n3ds_lift_invoke(&c, TARGET_ADDR, NULL, 0, result);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s CONTAINER.IRC [repeat] [require-missing]\n", argv[0]);
        return 2;
    }
    int repeat = argc > 2 ? atoi(argv[2]) : 1000;
    if (repeat < 1) repeat = 1;
    int require_missing = argc > 3 && !strcmp(argv[3], "require-missing");

    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, 0x60000000u, 0x30000u)) {
        printf("guest map failed\n");
        return 2;
    }
    FnEntry aot[2] = {
        {DUMMY_ADDR, dummy},
        {DUMMY_ADDR + 0x1000u, target_dummy},
    };
    if (n3ds_lift_set_container_path(argv[1])) {
        printf("set container path failed\n");
        return 2;
    }
    if (n3ds_lift_bind(&mem, aot, 2)) {
        printf("bind failed: %s\n", n3ds_lift_fault());
        return require_missing ? 0 : 2;
    }
    if (require_missing) {
        printf("FAIL: required container bind unexpectedly succeeded\n");
        return 1;
    }
    if (n3ds_lift_container_state() != 0) {
        printf("FAIL: container opened before non-AOT dispatch\n");
        return 1;
    }
    if (n3ds_ir_has(TARGET_ADDR)) {
        printf("FAIL: target %08x unexpectedly in slice IR table\n", TARGET_ADDR);
        return 1;
    }
    if (!n3ds_lift_has_function(TARGET_ADDR)) {
        printf("FAIL: target %08x missing from container\n", TARGET_ADDR);
        return 1;
    }
    if (n3ds_lift_container_state() != 1) {
        printf("FAIL: container did not open lazily\n");
        return 1;
    }

    IrCacheStats ir_before, ir_after;
    n3ds_ir_cache_stats(&ir_before);
    clock_t t0 = clock();
    for (int i = 0; i < repeat; ++i) {
        uint32_t r = 0;
        int rc = run_once(&r);
        if (rc != N3DS_INVOKE_OK || r != TARGET_RESULT) {
            printf("FAIL: invoke %d rc=%d result=%08x fault='%s'\n",
                   i, rc, r, n3ds_lift_fault());
            return 1;
        }
    }
    clock_t t1 = clock();
    n3ds_ir_cache_stats(&ir_after);

    N3dsIrcStats st;
    memset(&st, 0, sizeof st);
    if (n3ds_lift_container_stats(&st)) {
        printf("FAIL: container stats unavailable\n");
        return 1;
    }
    double ms = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    printf("growth: target %08x result %08x repeat %d\n", TARGET_ADDR,
           TARGET_RESULT, repeat);
    printf("stats: loads %llu load_bytes %llu load_ns %llu hits %llu misses %llu "
           "evictions %llu execs %llu block_hits %llu block_misses %llu "
           "redecodes %llu\n",
           (unsigned long long)st.loads, (unsigned long long)st.load_bytes,
           (unsigned long long)st.load_ns, (unsigned long long)st.cache_hits,
           (unsigned long long)st.cache_misses, (unsigned long long)st.evictions,
           (unsigned long long)st.execs, (unsigned long long)st.block_hits,
           (unsigned long long)st.block_misses, (unsigned long long)st.redecodes);
    printf("wall: %.3f ms (host clock), %.3f us/invoke\n", ms,
           repeat ? ms * 1000.0 / repeat : 0.0);
    if (st.execs != (uint64_t)repeat || st.redecodes != 0 || st.block_misses != 0) {
        printf("FAIL: container counters (execs/redecodes/block_misses)\n");
        return 1;
    }
    if (st.loads < 1 || st.load_bytes == 0) {
        printf("FAIL: no container record load\n");
        return 1;
    }
    if (repeat > 1 && st.cache_hits == 0) {
        printf("FAIL: repeated invoke did not hit the record cache\n");
        return 1;
    }
    if (ir_after.blocks != ir_before.blocks) {
        printf("FAIL: slice decoder ran during container execution (%llu -> %llu)\n",
               (unsigned long long)ir_before.blocks,
               (unsigned long long)ir_after.blocks);
        return 1;
    }
    n3ds_lift_release();
    guest_destroy(&mem);
    printf("IR container growth PASS\n");
    return 0;
}
