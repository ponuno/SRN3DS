/* Compressed guest-page swap: byte-exact restore, dirty round trip,
 * incompressible pages stay resident, eviction happens only at the safe point,
 * and a full host pool refuses with a named fault instead of corrupting.
 */
#include "guest.h"
#include "guest_swap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "guest_swap_smoke: FAIL line %d: %s\n", __LINE__, #c); \
    return 1; } } while (0)

#define BASE   0x30000000u
#define PAGES  64u
#define PSIZE  GUEST_PAGE_SIZE

static uint32_t g_seed = 0x12345678u;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static void fill_pattern(uint8_t *p, unsigned page, unsigned salt) {
    for (unsigned i = 0; i < PSIZE; ++i)
        p[i] = (uint8_t)(i * 37u + page * 11u + salt * 101u + (i >> 7));
    /* Some pointer-like words so the sample resembles a real page.  The seed
     * is local: the same (page,salt) must always produce the same bytes. */
    uint32_t s = 0x9e3779b9u ^ (page * 2654435761u) ^ (salt * 40503u);
    for (unsigned i = 0; i + 4 <= PSIZE; i += 64) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        uint32_t v = 0x00400000u + s % 0x01000000u;
        p[i] = (uint8_t)v; p[i+1] = (uint8_t)(v >> 8);
        p[i+2] = (uint8_t)(v >> 16); p[i+3] = (uint8_t)(v >> 24);
    }
}

static void fill_random(uint8_t *p) {
    for (unsigned i = 0; i < PSIZE; ++i) p[i] = (uint8_t)rnd();
}

static int slab_setup(GuestMemory *m, uint32_t pages, size_t limit) {
    guest_init(m, limit);
    GuestSlab *s = guest_slab_reserve(m, BASE, pages * PSIZE);
    if (!s) return -1;
    if (guest_slab_commit(m, BASE, pages * PSIZE, 0x04)) return -1;
    return 0;
}

int main(void) {
    uint8_t page[PSIZE], back[PSIZE];
    GuestSwapStats st;

    /* ---- LZ4 round trip on random and compressible pages ---------------- */
    uint8_t packed[PSIZE + 64];
    for (unsigned k = 0; k < 64; ++k) {
        if (k & 1) fill_random(page); else memset(page, 0, PSIZE);
        int n = guest_swap_compress(page, PSIZE, packed, sizeof packed);
        CHECK(n > 0);
        memset(back, 0xa5, PSIZE);
        CHECK(guest_swap_decompress(packed, (size_t)n, back, PSIZE) == (int)PSIZE);
        CHECK(memcmp(page, back, PSIZE) == 0);
    }

    /* ---- eviction + exact restore --------------------------------------- */
    GuestMemory m;
    CHECK(slab_setup(&m, PAGES, 64u << 20) == 0);
    CHECK(guest_swap_enable(&m, 8u << 20, 4u * PSIZE) == 0);
    for (unsigned p = 0; p < 16; ++p) {
        fill_pattern(page, p, 0);
        CHECK(guest_write(&m, BASE + p * PSIZE, page, PSIZE) == 0);
    }
    guest_swap_safe_point(&m);
    guest_swap_stats(&m, &st);
    CHECK(st.pages_out >= 12);
    CHECK(st.resident_pages <= 4);
    CHECK(st.compressed_live > 0);
    CHECK(st.compressed_live < st.pages_out * PSIZE);
    for (unsigned p = 0; p < 16; ++p) {
        fill_pattern(page, p, 0);
        CHECK(guest_read(&m, BASE + p * PSIZE, back, PSIZE) == 0);
        CHECK(memcmp(page, back, PSIZE) == 0);
    }
    guest_swap_stats(&m, &st);
    CHECK(st.pages_in == st.pages_out);
    CHECK(st.resident_pages == 16);
    printf("guest_swap_smoke: compressed %llu pages, ratio %.2f%%\n",
           (unsigned long long)st.pages_out,
           100.0 * (double)st.compressed_total / (double)st.bytes_out);

    /* ---- write after page-in is dirty and survives another swap --------- */
    for (unsigned p = 0; p < 16; ++p) {
        fill_pattern(page, p, 7);
        CHECK(guest_write(&m, BASE + p * PSIZE, page, PSIZE) == 0);
    }
    guest_swap_safe_point(&m);
    guest_swap_stats(&m, &st);
    CHECK(st.pages_out >= 24);            /* 16 again plus the first 8 */
    for (unsigned p = 0; p < 16; ++p) {
        fill_pattern(page, p, 7);
        CHECK(guest_read(&m, BASE + p * PSIZE, back, PSIZE) == 0);
        CHECK(memcmp(page, back, PSIZE) == 0);
    }

    /* ---- incompressible pages are left resident, not corrupted ---------- */
    uint64_t out_before = st.pages_out;
    for (unsigned p = 16; p < 24; ++p) {
        fill_random(page);
        CHECK(guest_write(&m, BASE + p * PSIZE, page, PSIZE) == 0);
    }
    guest_swap_safe_point(&m);
    guest_swap_stats(&m, &st);
    CHECK(st.fail_incompressible > 0);
    CHECK(st.pages_out <= out_before + 16); /* random pages did not bloat store */
    for (int p = 16; p < 24; ++p) CHECK(guest_read(&m, BASE + p * PSIZE, back, PSIZE) == 0);

    /* ---- no eviction happens inside guest_ptr --------------------------- */
    guest_swap_disable(&m);
    CHECK(guest_swap_enable(&m, 8u << 20, 1u * PSIZE) == 0);
    for (unsigned p = 0; p < 8; ++p) {
        fill_pattern(page, p, 3);
        CHECK(guest_write(&m, BASE + p * PSIZE, page, PSIZE) == 0);
    }
    guest_swap_stats(&m, &st);
    CHECK(st.resident_pages >= 8);
    uint8_t *held = (uint8_t *)guest_ptr(&m, BASE, 4);
    CHECK(held != NULL);
    for (unsigned p = 0; p < 8; ++p) {
        uint8_t *q = (uint8_t *)guest_ptr(&m, BASE + p * PSIZE, 4);
        CHECK(q != NULL);
    }
    guest_swap_stats(&m, &st);
    CHECK(st.pages_out == 0);   /* guest_ptr never evicts: pointers stay valid */
    fill_pattern(page, 0, 3);
    CHECK(memcmp(held, page, 4) == 0);
    /* The safe point is the only eviction point. */
    guest_swap_safe_point(&m);
    guest_swap_stats(&m, &st);
    CHECK(st.pages_out > 0);
    CHECK(st.resident_pages < 24);

    /* ---- decommit discards compressed blobs ----------------------------- */
    guest_swap_safe_point(&m);
    guest_swap_stats(&m, &st);
    uint64_t dropped_before = st.pages_dropped;
    for (unsigned p = 0; p < 8; ++p)
        CHECK(guest_slab_decommit(&m, BASE + p * PSIZE, PSIZE) == 0);
    guest_swap_stats(&m, &st);
    CHECK(st.pages_dropped > dropped_before);
    CHECK(guest_slab_commit(&m, BASE, 8 * PSIZE, 0x04) == 0);

    guest_swap_report(stderr, &m);
    guest_destroy(&m);

    /* ---- compressed-pool budget refusal -------------------------------- */
    GuestMemory b;
    CHECK(slab_setup(&b, PAGES, 64u << 20) == 0);
    CHECK(guest_swap_enable(&b, 100, 1u * PSIZE) == 0);
    for (unsigned p = 0; p < 12; ++p) {
        memset(page, 0, PSIZE);
        page[0] = (uint8_t)p;
        CHECK(guest_write(&b, BASE + p * PSIZE, page, PSIZE) == 0);
    }
    guest_swap_safe_point(&b);
    guest_swap_stats(&b, &st);
    CHECK(st.fail_budget > 0);
    CHECK(st.compressed_live <= 100);
    CHECK(st.resident_pages > 1);       /* the pool is full, pages stay */
    for (unsigned p = 0; p < 12; ++p) {
        CHECK(guest_read(&b, BASE + p * PSIZE, back, PSIZE) == 0);
        CHECK(back[0] == (uint8_t)p);
    }
    guest_destroy(&b);

    /* ---- named refusal when page-in has no host block ------------------ */
    GuestMemory n;
    guest_init(&n, 0);
    GuestSlab *s = NULL;
    /* Reserve first to learn the exact metadata cost, then set the limit so
     * only four pages fit. */
    n.limit = 1u << 20;
    s = guest_slab_reserve(&n, BASE, 16u * PSIZE);
    CHECK(s != NULL);
    CHECK(guest_slab_commit(&n, BASE, 16u * PSIZE, 0x04) == 0);
    n.limit = n.used + 4u * PSIZE;
    CHECK(guest_swap_enable(&n, 1u << 20, 0) == 0); /* no eviction target */
    unsigned ok = 0;
    for (unsigned p = 0; p < 8; ++p) {
        memset(page, 0, PSIZE);
        if (guest_write(&n, BASE + p * PSIZE, page, PSIZE) == 0) { ++ok; continue; }
        CHECK(ok >= 2);                    /* the limit was reached, not earlier */
        CHECK(guest_last_fault(&n) == GUEST_FAULT_NO_MEMORY);
        CHECK(strstr(guest_fault_text(&n), "out of memory") != NULL);
        break;
    }
    CHECK(ok < 8);
    guest_swap_stats(&n, &st);
    CHECK(st.fail_no_memory > 0);
    guest_destroy(&n);

    printf("guest_swap_smoke: PASS\n");
    return 0;
}
