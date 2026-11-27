#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GUEST_MAX_REGIONS 64
#define GUEST_PAGE_SIZE 4096u
/* A guest VirtualAlloc arena is one reservation plus per-page state. Pages are
 * backed independently (non-contiguous), so the port never needs a 256 MiB
 * contiguous host reservation. This matters on the 3DS: MEMOP_RESERVE is not
 * implemented (measured: svcControlMemory returns 0xE0E01BEE for every size). */
#define GUEST_MAX_SLABS 32

typedef int (*GuestPageFill)(void *ctx, uint64_t offset, uint8_t *dst, size_t n,
                             size_t *got);
typedef int (*GuestPageFlush)(void *ctx, uint64_t offset, const uint8_t *src,
                              size_t n);

typedef struct {
    uint32_t base, size;      /* guest reservation */
    uint32_t pages;
    uint32_t backed_pages;
    uint8_t *state;           /* per page: 0 = reserved, 1 = committed promise */
    uint8_t *protect;         /* per page: Win32 PAGE_* value */
    uint8_t *dirty;           /* per page: writable file mapping was touched */
    uint8_t *ever;            /* per page: backed at least once (reload count) */
    uint8_t **page;           /* per page: host block, NULL = not backed yet */
    /* Optional backing store (file mappings); NULL means anonymous zero pages. */
    GuestPageFill fill;
    GuestPageFlush flush;
    void *provider_ctx;
    uint64_t provider_base;
    int mapped;
    int writable;
    int type;                 /* Win32 MEM_* type reported by VirtualQuery */
} GuestSlab;

typedef struct { uint32_t base, size; uint8_t *data; } GuestRegion;
typedef enum {
    GUEST_FAULT_NONE = 0,
    GUEST_FAULT_UNMAPPED,     /* reserved/uncommitted or outside any region */
    GUEST_FAULT_NO_MEMORY,    /* committed, but backing could not be allocated */
    GUEST_FAULT_IO            /* file-backed page read/write failed */
} GuestFault;
typedef struct GuestMemory {
    GuestRegion regions[GUEST_MAX_REGIONS];
    GuestSlab slabs[GUEST_MAX_SLABS];
    size_t count, used, limit;
    size_t slab_meta;          /* page tables, counted in `used` */
    uint64_t slab_backed;      /* physical backing currently present */
    uint64_t slab_backed_peak;
    uint64_t backing_failures;
    GuestFault last_fault;
    uint64_t last_io_error;
    unsigned evict_slab, evict_page;
    /* Page-level measurements, printed by the [mem] report. */
    uint64_t pages_first_touched, pages_anon, pages_file, pages_dirty_written;
    uint64_t pages_decommitted, pages_evicted, pages_evicted_clean;
    uint64_t pages_reloaded;
    /* Installed by guest_process_init(): report backing failure distinctly from
     * a reserved/unmapped access. May not return. */
    void (*fault_handler)(struct GuestMemory *, uint32_t, const char *);
} GuestMemory;

void guest_init(GuestMemory *m, size_t limit);
void guest_destroy(GuestMemory *m);
int guest_map(GuestMemory *m, uint32_t base, uint32_t size);
int guest_unmap(GuestMemory *m, uint32_t base);
void *guest_ptr(GuestMemory *m, uint32_t addr, size_t n);
int guest_read(GuestMemory *m, uint32_t addr, void *dst, size_t n);
int guest_write(GuestMemory *m, uint32_t addr, const void *src, size_t n);
int guest_read32(GuestMemory *m, uint32_t addr, uint32_t *value);
int guest_write32(GuestMemory *m, uint32_t addr, uint32_t value);
/* Optional diagnostic watchpoint. Disabled unless N3DS_WATCH_ADDR is set. */
void guest_watch_configure(uint32_t addr, uint32_t value);
void guest_watch_set_pc(uint32_t pc, unsigned thread);
uint32_t guest_watch_last_pc(void);
void guest_pc_ring_dump(FILE *f);
/* Per-thread block ring with registers at block entry (N3DS_PC_RING=1). */
void guest_pc_ring_note(uint32_t pc, unsigned thread, uint32_t esp,
                        uint32_t ecx, uint32_t eax);
void guest_pc_ring_dump_thread(FILE *f, unsigned thread);
void guest_watch_check(GuestMemory *m, const char *source);
void guest_watch_check_active(const char *source);
void guest_watch_check_host(const void *p, size_t n, const char *source);
void guest_watch_dump(GuestMemory *m);
/* Return a NUL-terminated string entirely inside one region, bounded by max. */
const char *guest_cstr(GuestMemory *m,uint32_t addr,size_t max);

/* ---- lazy VirtualAlloc/file-view slabs ---------------------------------------
 * Reserving costs only page tables. Marking pages committed records the Win32
 * promise; guest_ptr() backs a committed page on its first access. An access
 * to a reserved (or uncommitted) page returns NULL with GUEST_FAULT_UNMAPPED. */
GuestSlab *guest_slab_find(GuestMemory *m, uint32_t addr, uint32_t n);
GuestSlab *guest_slab_reserve(GuestMemory *m, uint32_t base, uint32_t size);
int guest_slab_commit(GuestMemory *m, uint32_t a, uint32_t size, uint32_t protect);
int guest_slab_decommit(GuestMemory *m, uint32_t a, uint32_t size);
int guest_slab_release(GuestMemory *m, uint32_t base);
int guest_slab_state(GuestMemory *m, uint32_t addr, uint32_t *protect);
/* Attach a file provider. `window` pages are read through fill() on first
 * touch; writable dirty pages are written back by flush() on eviction/release.
 * All pages become committed with `protect` and type MEM_MAPPED. */
int guest_slab_set_provider(GuestMemory *m, uint32_t base, uint32_t protect,
                            int writable, GuestPageFill fill,
                            GuestPageFlush flush, void *ctx, uint64_t file_base);
uint64_t guest_slab_backed(const GuestMemory *m);
uint64_t guest_slab_backed_peak(const GuestMemory *m);
GuestFault guest_last_fault(const GuestMemory *m);
const char *guest_fault_text(const GuestMemory *m);
typedef struct {
    uint64_t first_touched, anon, file, dirty_written;
    uint64_t decommitted, evicted, evicted_clean, reloaded, failures;
} GuestPageStats;
void guest_page_stats(const GuestMemory *m, GuestPageStats *out);
void guest_set_fault_handler(GuestMemory *m,
                             void (*fn)(GuestMemory *, uint32_t, const char *));
