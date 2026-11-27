/* `rep movs*` host-pointer lifetime regression.
 *
 * guest_ptr() backs a page lazily.  While backing it, guest.c can:
 *   - bridge a multi-page slab range and free the individual page blocks it
 *     replaces (slab_prepare -> page_drop_preserve), or
 *   - evict another file-backed page when the backing limit is reached
 *     (page_back -> slab_evict_one -> page_drop).
 * Both release host memory, so a pointer obtained before the commit is dead
 * afterwards.  The old rep-movsd helper fetched src/dst pointers and copied
 * through them; this test forces eviction on every page commit and would read
 * freed memory (ASan) or reused page contents (plain build).
 *
 * All data is synthetic; no game files are involved. */
#include "n3ds_lift_runtime.h"
#include "lift_prelude.h"
#include <stdio.h>
#include <string.h>

#define PAGE  4096u
#define BASE  0x50000000u
#define PAGES 4u

static uint8_t store[PAGES * PAGE];

static uint8_t pattern(uint64_t off) {
    return (uint8_t)(((off >> 12) * 37u + (off & 0xffu)) ^ 0x5au);
}

static int provider_fill(void *ctx, uint64_t off, uint8_t *dst, size_t n,
                         size_t *got) {
    (void)ctx;
    for (size_t i = 0; i < n; ++i) dst[i] = pattern(off + i);
    if (got) *got = n;
    return 0;
}

static int provider_flush(void *ctx, uint64_t off, const uint8_t *src,
                          size_t n) {
    (void)ctx;
    if (off > sizeof store || n > sizeof store - (size_t)off) return -1;
    memcpy(store + off, src, n);
    return 0;
}

static void dummy_fn(CPU *c) { (void)c; }

static int fail(const char *what) {
    printf("rep movs smoke: FAIL: %s\n", what);
    return 1;
}

int main(void) {
    GuestMemory m;
    static const FnEntry table[] = {{0x401000u, dummy_fn}};
    guest_init(&m, 64u << 20);
    if (!guest_slab_reserve(&m, BASE, PAGES * PAGE))
        return fail("slab reserve");
    if (guest_slab_commit(&m, BASE, PAGES * PAGE, 4))
        return fail("slab commit");
    if (guest_slab_set_provider(&m, BASE, 4, 1, provider_fill,
                                provider_flush, NULL, 0))
        return fail("slab provider");
    if (n3ds_lift_bind(&m, table, 1)) return fail("bind");

    /* --- 1. eviction while copying: src pages 0..1 -> dst pages 2..3 --- */
    size_t meta = m.used;
    m.limit = meta + 2u * PAGE;      /* at most two resident pages */
    rt_movsd(2u * PAGE / 4u, BASE + 2u * PAGE, BASE);
    m.limit = 64u << 20;

    for (uint32_t p = 0; p < 2; ++p) {
        uint8_t got[PAGE];
        if (guest_read(&m, BASE + (2u + p) * PAGE, got, PAGE))
            return fail("read copied page");
        for (uint32_t i = 0; i < PAGE; ++i)
            if (got[i] != pattern((uint64_t)p * PAGE + i))
                return fail("eviction copy content");
    }
    for (uint32_t p = 0; p < 2; ++p) {
        uint8_t got[PAGE];
        if (guest_read(&m, BASE + p * PAGE, got, PAGE))
            return fail("read source page");
        for (uint32_t i = 0; i < PAGE; ++i)
            if (got[i] != pattern((uint64_t)p * PAGE + i))
                return fail("source page was modified");
    }

    /* --- 2. x86 forward overlap semantics (dst > src) --- */
    uint32_t q = 0x60000000u;
    if (guest_map(&m, q, 0x2000)) return fail("region map");
    for (uint32_t i = 0; i < 16; ++i) M32(q + 4u * i) = i + 1u;
    rt_movsd(4, q + 4u, q);          /* dword 0 propagates forward */
    for (uint32_t i = 1; i <= 4; ++i)
        if (M32(q + 4u * i) != 1u) return fail("forward overlap");
    if (M32(q) != 1u) return fail("forward overlap source");

    /* dst < src is a plain forward copy */
    for (uint32_t i = 0; i < 5; ++i) M32(q + 4u * i) = i + 1u;
    rt_movsd(4, q, q + 4u);
    for (uint32_t i = 0; i < 4; ++i)
        if (M32(q + 4u * i) != i + 2u) return fail("forward safe overlap");
    if (M32(q + 16u) != 5u) return fail("forward safe overlap tail");

    /* --- 3. non-overlapping copy across a page boundary --- */
    for (uint32_t i = 0; i < 0x400u; ++i)
        *(uint8_t *)n3ds_guest_ptr(q + 0x100u + i, 1) = (uint8_t)(i * 3u + 1u);
    for (uint32_t i = 0; i < 0x40u; ++i)
        *(uint8_t *)n3ds_guest_ptr(q + PAGE - 0x10u + i, 1) =
            (uint8_t)(0xc0u + i);
    rt_movsd(8, q + 0x800u, q + PAGE - 0x10u);
    for (uint32_t i = 0; i < 0x20u; ++i)
        if (*(uint8_t *)n3ds_guest_ptr(q + 0x800u + i, 1) !=
            (uint8_t)(0xc0u + i))
            return fail("page-crossing copy");
    rt_movsd(0, 0, 0);               /* count zero: no access at all */

#ifndef REP_MOVS_BASELINE
    /* --- 4. the shared `movs` element helper (rep movsb/movsw/DF=1 path) --- */
    M32(q + 0x200u) = 0x11223344u;
    M32(q + 0x204u) = 0;
    n3ds_movs32(q + 0x204u, q + 0x200u);
    if (M32(q + 0x204u) != 0x11223344u) return fail("n3ds_movs32");
    n3ds_movs8(q + 0x204u, q + 0x208u);
    n3ds_movs16(q + 0x206u, q + 0x20au);
#endif

    printf("rep movs smoke: PASS (eviction copy, overlap, page crossing)\n");
    guest_destroy(&m);
    return 0;
}
