/* Regression: a decoded block must survive a decode-cache flush when another
 * cooperative guest thread is still executing it.
 *
 * The interpreter keeps decoded blocks in one flat arena and used to reset that
 * arena in place when it filled up. A guest thread can be suspended *inside* an
 * IR block (a shim it called switched host stacks); if another thread then
 * flushed the arena, the suspended executor resumed from overwritten decoded
 * instructions and corrupted guest state (observed as intermittent unmapped
 * reads in TESV's zlib inflate after the first menu draws).
 *
 * This test pins the runtime contract directly: hold a block live, force a
 * flush, and verify its instruction words are unchanged.  Synthetic code only.
 */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "guest.h"
#include <stdio.h>
#include <string.h>

#define BASE 0x50000000u
#define STACK_TOP 0x50080000u
#define STRIDE 8u
#define BLOCKS 20000u

static GuestMemory mem;

static void dummy_fn(CPU *c) { (void)c; }

static int fail(const char *what) {
    printf("ir cache flush smoke: FAIL: %s\n", what);
    return 1;
}

int main(void) {
    static uint8_t img[BLOCKS * STRIDE];
    for (unsigned i = 0; i < BLOCKS; ++i) {
        uint8_t *p = img + i * STRIDE;
        p[0] = 0xb8;                      /* mov eax, i */
        p[1] = (uint8_t)i;
        p[2] = (uint8_t)(i >> 8);
        p[3] = (uint8_t)(i >> 16);
        p[4] = (uint8_t)(i >> 24);
        p[5] = 0xc3;                      /* ret */
    }

    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, BASE, sizeof img)) return fail("map");
    if (guest_write(&mem, BASE, img, sizeof img)) return fail("write image");

    static const FnEntry aot[] = {{0x50008000u, dummy_fn}};
    if (n3ds_lift_bind(&mem, aot, 1)) return fail("bind");
    static const IrFnEntry irtab[] = {{BASE, BASE + sizeof img}};
    if (n3ds_ir_bind(irtab, 1)) return fail("ir bind");
    /* Small arena so a handful of blocks forces a flush. */
    if (n3ds_ir_cache_init(1u << 16)) return fail("cache init");

    CPU c;
    n3ds_lift_init_cpu(&c, STACK_TOP, 0);

    IrExecBlock first;
    if (n3ds_ir_fetch_block(&c, BASE, BASE + sizeof img, &first))
        return fail("fetch first block");
    if (!first.stream_words || first.stream_words > 32) {
        printf("ir cache flush smoke: first block words=%u\n", first.stream_words);
        return fail("first block size");
    }
    uint32_t snap[32];
    memcpy(snap, first.stream, first.stream_words * sizeof snap[0]);

    /* A guest thread is now executing this block (possibly suspended inside a
     * shim). Decode enough other blocks to wrap the whole arena at least once. */
    n3ds_ir_block_enter();
    IrCacheStats st;
    for (unsigned i = 1; i < BLOCKS; ++i) {
        IrExecBlock b;
        if (n3ds_ir_fetch_block(&c, BASE + i * STRIDE, BASE + sizeof img, &b))
            return fail("fetch fill block");
        n3ds_ir_cache_stats(&st);
        if (st.flushes >= 2) break;
    }
    n3ds_ir_cache_stats(&st);
    if (!st.flushes) {
        n3ds_ir_block_leave();
        return fail("no flush happened; test did not exercise the case");
    }
    if (memcmp(snap, first.stream, first.stream_words * sizeof snap[0])) {
        n3ds_ir_block_leave();
        return fail("live block was overwritten by the flush");
    }

    /* Quiescent: the retired arena may now be released and decoding continues. */
    n3ds_ir_block_leave();
    IrExecBlock again;
    if (n3ds_ir_fetch_block(&c, BASE, BASE + sizeof img, &again))
        return fail("fetch after leave");

    /* Resident accounting: live_bytes is what newlib holds, and it must be
     * exactly arena + slots + retired (the old counter accumulated decoded
     * bytes and reported more than the arena could ever hold). */
    n3ds_ir_cache_stats(&st);
    if (st.live_bytes != st.arena_bytes + st.slot_bytes + st.retired_bytes)
        return fail("live bytes are not arena+slots+retired");
    if (st.arena_bytes != (1u << 16))
        return fail("arena bytes do not match the requested cap");
    if (!st.slot_bytes)
        return fail("slot bytes not reported");
    if (st.bytes > st.arena_bytes)
        return fail("live decoded bytes exceed the arena");

    printf("ir cache flush smoke: PASS (%llu flushes, live block preserved, "
           "live=%llu arena=%llu slots=%llu retired=%llu)\n",
           (unsigned long long)st.flushes, (unsigned long long)st.live_bytes,
           (unsigned long long)st.arena_bytes, (unsigned long long)st.slot_bytes,
           (unsigned long long)st.retired_bytes);
    return 0;
}
