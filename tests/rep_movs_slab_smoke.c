/* Regression: page-windowed guest copy across a bridged slab range.
 *
 * The NavMeshInfoMap drain (0x5fc640) grows a temporary array whose buffer is
 * reallocated by CRT memcpy (0xf537f0 -> `rep movsd`). IR's rep_movsd used to
 * resolve dst and src with two cross-page guest_ptr() calls and then memmove.
 * When the two ranges share a slab page, the second guest_ptr() builds a
 * bridge, drops the allocations it covers and preserves the pages outside its
 * range -- freeing the bridge underlying the first pointer. The copy then lands
 * in freed host memory and the real array keeps stale bytes (bucket value
 * 0x012b6634 from an unrelated shader heap). n3ds_rep_movsd() resolves one page
 * window at a time; this test exercises exactly that layout.
 */
#include "guest.h"
#include "n3ds_lift_runtime.h"

#include <stdio.h>
#include <string.h>

#define BASE   0x60000000u
#define SLAB   0x00008000u
#define FILLER 0x61000000u
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "rep_movs_slab_smoke: FAIL line %d: %s\n", __LINE__, #c); \
    return 1; } } while (0)

static GuestMemory g_mem;

/* The copy helper only needs this accessor; the real slice/runtime provides
 * the same symbol over its own GuestMemory. */
void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    return guest_ptr(&g_mem, addr, bytes);
}

static int fill(GuestMemory *m, uint32_t a, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char v = (unsigned char)(seed + i * 7u);
        if (guest_write(m, a + (uint32_t)i, &v, 1)) return -1;
    }
    return 0;
}

static int same(GuestMemory *m, uint32_t a, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char v = 0;
        if (guest_read(m, a + (uint32_t)i, &v, 1)) return 0;
        if (v != (unsigned char)(seed + i * 7u)) return 0;
    }
    return 1;
}

int main(void) {
    GuestMemory *m = &g_mem;
    guest_init(m, 1u << 20);
    CHECK(guest_slab_reserve(m, BASE, SLAB) != NULL);
    CHECK(!guest_slab_commit(m, BASE, SLAB, 0x04 /* PAGE_READWRITE */));

    /* Back each page on its own, with another guest allocation in between, so
     * the host pages are not contiguous and guest_ptr() has to bridge. */
    void *page[8];
    for (unsigned i = 0; i < 8; ++i) {
        page[i] = guest_ptr(m, BASE + i * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE);
        CHECK(page[i] != NULL);
        CHECK(!guest_map(m, FILLER + i * GUEST_PAGE_SIZE, GUEST_PAGE_SIZE));
    }
    unsigned fragmented = 0;
    for (unsigned i = 0; i + 1 < 8; ++i)
        if ((const unsigned char *)page[i] + GUEST_PAGE_SIZE != page[i + 1])
            ++fragmented;
    CHECK(fragmented >= 6);

    /* src and dst cross three pages each and share the middle page, exactly
     * like the vector growth (old+0x2000 vs new at old+0x2008). */
    const uint32_t src = BASE + 0x800u;
    const uint32_t dst = BASE + 0x2808u;
    const size_t n = 0x2000u;
    fill(m, src, n, 0x40);
    CHECK(same(m, src, n, 0x40));
    for (size_t i = 0; i < n; ++i) {
        unsigned char z = 0;
        CHECK(!guest_write(m, dst + (uint32_t)i, &z, 1));
    }
    n3ds_rep_movsd((uint32_t)(n / 4), dst, src);
    CHECK(same(m, dst, n, 0x40));
    CHECK(same(m, src, n, 0x40));

    /* Overlapping forward copy keeps x86 dword propagation: reading each dword
     * before writing it means the first written dword repeats. */
    CHECK(fill(m, BASE + 0x1000u, 0x400u, 0x10) == 0);
    n3ds_rep_movsd(0x400u / 4u, BASE + 0x1004u, BASE + 0x1000u);
    {
        uint32_t first = 0, second = 0;
        CHECK(!guest_read32(m, BASE + 0x1000u, &first));
        CHECK(!guest_read32(m, BASE + 0x1004u, &second));
        CHECK(first == second);
    }

    guest_destroy(m);
    puts("HOST PASS: rep_movs_slab_smoke");
    return 0;
}
