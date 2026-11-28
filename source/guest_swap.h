#pragma once
/* Compressed swap for anonymous guest pages.
 *
 * Guest pages that live in an anonymous slab (VirtualAlloc / heap backing) can
 * be compressed into a host-memory pool and their 4 KiB block released.  This
 * is the only way the port can promise the engine hundreds of MiB while the
 * device region is 178 MiB: cold pages cost their compressed size instead of
 * the full page.
 *
 * Safety rule: pages are evicted ONLY from guest_swap_safe_point(), which the
 * scheduler calls between IR/AOT blocks.  A raw pointer returned by guest_ptr()
 * is never invalidated by another guest_ptr(): page-in uses a free block or
 * grows within the guest limit, and when neither is possible it fails with a
 * named GUEST_FAULT_NO_MEMORY instead of silently corrupting a held pointer.
 *
 * File-backed pages are NOT swapped here; the PE provider already pages text
 * through a bounded window and a second copy would only add work.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct GuestMemory GuestMemory;
typedef struct GuestSlab GuestSlab;

typedef struct {
    uint64_t pages_out;        /* pages compressed and released              */
    uint64_t pages_in;         /* pages restored from the compressed pool    */
    uint64_t pages_dropped;    /* blobs discarded (decommit/release)         */
    uint64_t bytes_out;        /* guest bytes compressed (pages_out * 4096)  */
    uint64_t bytes_in;         /* guest bytes decompressed (pages_in * 4096) */
    uint64_t compressed_total; /* sum of compressed sizes of all swap-outs   */
    uint64_t compressed_live;  /* compressed bytes held right now            */
    uint64_t fail_no_memory;   /* page-in refused: no host block / limit     */
    uint64_t fail_budget;      /* eviction skipped: compressed pool full     */
    uint64_t fail_incompressible; /* LZ4 did not shrink the page             */
    uint64_t fail_unsafe;      /* candidate shared a bridge / was pinned     */
    uint64_t resident_pages;   /* anonymous pages resident right now         */
    uint64_t target_pages;     /* configured resident ceiling                */
    uint64_t budget_bytes;     /* configured compressed-pool ceiling         */
    int enabled;
} GuestSwapStats;

/* Enable the store.  `compressed_budget` bounds the compressed payload;
 * `resident_target` bounds the anonymous resident bytes (0 = no ceiling, which
 * disables eviction). */
int guest_swap_enable(struct GuestMemory *m, size_t compressed_budget,
                      size_t resident_target);
/* Frees the store.  Only safe when every swapped page has been paged in:
 * an evicted page has no other copy.  Production code never calls it. */
void guest_swap_disable(struct GuestMemory *m);
/* Reads N3DS_SWAP / N3DS_SWAP_BUDGET_MB / N3DS_GUEST_TARGET_MB.  Returns 0 when
 * enabled, 1 when off, -1 on a bad value. */
int guest_swap_configure(struct GuestMemory *m);
int guest_swap_active(const struct GuestMemory *m);

/* The only eviction point: between IR/AOT blocks, no raw guest pointer live. */
void guest_swap_safe_point(struct GuestMemory *m);
/* Clock bit for the page just used; called from slab_prepare(). */
void guest_swap_touch(struct GuestMemory *m, struct GuestSlab *s, uint32_t page);

/* Block supply for page_back(): a recycled 4 KiB block (contents undefined) or
 * NULL when the host cannot provide one. */
uint8_t *guest_swap_block(struct GuestMemory *m);
void guest_swap_recycle(struct GuestMemory *m, uint8_t *block);

/* Compressed-store access, used by page_back() and slab teardown.
 * fetch: 1 restored, 0 not stored, -1 decompression error.
 * store: compressed length (> 0) on success, -2 incompressible,
 *        -3 budget full, -4 allocation/compression failed. */
int guest_swap_fetch(struct GuestMemory *m, struct GuestSlab *s, uint32_t page, uint8_t *dst);
int guest_swap_store(struct GuestMemory *m, struct GuestSlab *s, uint32_t page,
                     const uint8_t *src);
void guest_swap_discard(struct GuestMemory *m, struct GuestSlab *s, uint32_t page);
void guest_swap_forget_slab(struct GuestMemory *m, struct GuestSlab *s);

void guest_swap_stats(const struct GuestMemory *m, GuestSwapStats *out);
void guest_swap_report(FILE *f, const struct GuestMemory *m);
/* Page-level residency counter, called by page_back/page eviction. */
void guest_swap_note_resident(struct GuestMemory *m, int delta);
/* A page could not be backed because no block and no room were available. */
void guest_swap_note_nomem(struct GuestMemory *m);
/* Raw LZ4 block helpers, exposed for the compression-ratio smoke test. */
int guest_swap_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
int guest_swap_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
