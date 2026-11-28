#include "guest.h"
#include "hprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional four-byte guest watchpoint. The generated diagnostic slice calls
 * guest_watch_set_pc() before every instruction; guest_write() and the Win32
 * memcpy/memset shims call guest_watch_check() after writes. A bounded ring keeps
 * the first appearance and subsequent overlapping writes without unbounded log. */
typedef struct {
    uint32_t pc, thread, old_value, new_value;
    char source[24];
    uint32_t field_base, field_size, value_base, value_size;
    int field_slab, value_slab;
} GuestWatchRecord;
static struct {
    int active, have_old, target_seen, dumped;
    uint32_t address, target, old_value, last_pc;
    unsigned last_thread, sequence;
    GuestMemory *memory;
    uint8_t *host_address;
    GuestWatchRecord ring[128];
} guest_watch;
void guest_pc_ring_dump_thread(FILE *f, unsigned thread);

static void guest_watch_locate(GuestMemory *m, uint32_t addr,
                               uint32_t *base, uint32_t *size, int *slab) {
    *base = *size = 0; *slab = 0;
    for (size_t i = 0; i < m->count; ++i) {
        GuestRegion *r = &m->regions[i];
        if (addr >= r->base && addr - r->base < r->size) {
            *base = r->base; *size = r->size; return;
        }
    }
    for (unsigned i = 0; i < GUEST_MAX_SLABS; ++i) {
        GuestSlab *s = &m->slabs[i];
        if (s->state && addr >= s->base && addr - s->base < s->size) {
            *base = s->base; *size = s->size; *slab = 1; return;
        }
    }
}

static int guest_watch_read(GuestMemory *m, uint32_t *value) {
    uint8_t *p = (uint8_t *)guest_ptr(m, guest_watch.address, 4);
    if (!p) return -1;
    memcpy(value, p, 4);
    return 0;
}

static void guest_watch_print_record(const GuestWatchRecord *r) {
    fprintf(stderr,
            "[watch] pc=%08x thread=%u %s old=%08x new=%08x field=%08x+%08x%s value=%08x+%08x%s\n",
            r->pc, r->thread, r->source, r->old_value, r->new_value,
            r->field_base, r->field_size, r->field_slab ? " slab" : " region",
            r->value_base, r->value_size, r->value_slab ? " slab" : " region");
}

void guest_watch_dump(GuestMemory *m) {
    (void)m;
    if (!guest_watch.active || guest_watch.dumped || !guest_watch.sequence) return;
    guest_watch.dumped = 1;
    fprintf(stderr, "[watch] ring dump (%u records)\n", guest_watch.sequence);
    unsigned start = guest_watch.sequence > 128 ? guest_watch.sequence - 128 : 0;
    for (unsigned i = start; i < guest_watch.sequence; ++i)
        guest_watch_print_record(&guest_watch.ring[i & 127u]);
}

static void guest_watch_record(GuestMemory *m, const char *source) {
    if (!m) m = guest_watch.memory;
    if (!m) return;
    uint32_t value;
    if (guest_watch_read(m, &value)) {
        guest_watch.have_old = 0;
        return;
    }
    if (guest_watch.have_old && value == guest_watch.old_value) return;
    GuestWatchRecord *r = &guest_watch.ring[guest_watch.sequence++ & 127u];
    memset(r, 0, sizeof *r);
    r->pc = guest_watch.last_pc;
    r->thread = guest_watch.last_thread;
    r->old_value = guest_watch.have_old ? guest_watch.old_value : 0;
    r->new_value = value;
    snprintf(r->source, sizeof r->source, "%s", source ? source : "unknown");
    guest_watch_locate(m, guest_watch.address, &r->field_base, &r->field_size, &r->field_slab);
    if (value) guest_watch_locate(m, value, &r->value_base, &r->value_size, &r->value_slab);
    guest_watch.old_value = value;
    guest_watch.have_old = 1;
    if (value == guest_watch.target && !guest_watch.target_seen) {
        guest_watch.target_seen = 1;
        fprintf(stderr, "[watch] FIRST target %08x at field %08x\n", value, guest_watch.address);
        guest_watch_print_record(r);
        /* The writer's own block path is what places the store; the global ring
         * is interleaved, the per-thread ring is not. */
        guest_pc_ring_dump_thread(stderr, guest_watch.last_thread);
    } else if (guest_watch.sequence <= 16u) {
        guest_watch_print_record(r);
    }
    if (guest_watch.sequence == 128u) guest_watch_dump(m);
}

void guest_watch_configure(uint32_t address, uint32_t value) {
    GuestMemory *memory = guest_watch.memory;
    memset(&guest_watch, 0, sizeof guest_watch);
    guest_watch.memory = memory;
    guest_watch.active = 1;
    guest_watch.address = address;
    guest_watch.target = value;
    guest_watch.last_thread = 1;
    fprintf(stderr, "[watch] armed field=%08x target=%08x\n", address, value);
}

static uint32_t g_pc_ring[96];
static unsigned g_pc_n;
void guest_pc_ring_dump(FILE *f) {
    if (!f) return;
    unsigned cnt = g_pc_n < 96u ? g_pc_n : 96u;
    fprintf(f, "[guest] pc ring (%u, oldest first):", cnt);
    for (unsigned i = 0; i < cnt; ++i)
        fprintf(f, " %08x", (unsigned)g_pc_ring[(g_pc_n - cnt + i) % 96u]);
    fputc('\n', f);
}

/* Per-thread block ring with the registers visible at block entry.  The
 * process-wide ring above is interleaved by the cooperative scheduler, so its
 * tail is not necessarily the faulting thread; this one is indexed by
 * win32_thread_current_id() and is what the fault dump reads.  N3DS_PC_RING=1
 * arms it (the branch is one cached byte when off). */
#define GUEST_PC_RING_SLOTS 16u
#define GUEST_PC_RING_DEPTH 96u
typedef struct {
    uint32_t pc, esp, ecx, eax;
} GuestPcRecord;
typedef struct {
    GuestPcRecord r[GUEST_PC_RING_DEPTH];
    unsigned n;
} GuestPcThread;
static GuestPcThread g_pc_threads[GUEST_PC_RING_SLOTS];
static int g_pc_thread_on = -1;
static int guest_pc_ring_enabled(void) {
    if (g_pc_thread_on < 0)
        g_pc_thread_on = getenv("N3DS_PC_RING") && *getenv("N3DS_PC_RING") ? 1 : 0;
    return g_pc_thread_on;
}
void guest_pc_ring_note(uint32_t pc, unsigned thread, uint32_t esp,
                        uint32_t ecx, uint32_t eax) {
    guest_watch.last_pc = pc;
    guest_watch.last_thread = thread;
    if (!guest_pc_ring_enabled()) return;
    unsigned slot = thread < GUEST_PC_RING_SLOTS ? thread
                                                 : GUEST_PC_RING_SLOTS - 1u;
    GuestPcThread *t = &g_pc_threads[slot];
    GuestPcRecord *r = &t->r[t->n++ % GUEST_PC_RING_DEPTH];
    r->pc = pc; r->esp = esp; r->ecx = ecx; r->eax = eax;
}
void guest_pc_ring_dump_thread(FILE *f, unsigned thread) {
    if (!f) return;
    unsigned slot = thread < GUEST_PC_RING_SLOTS ? thread
                                                 : GUEST_PC_RING_SLOTS - 1u;
    GuestPcThread *t = &g_pc_threads[slot];
    unsigned cnt = t->n < GUEST_PC_RING_DEPTH ? t->n : GUEST_PC_RING_DEPTH;
    fprintf(f, "[guest] thread %u block ring (%u, oldest first):\n", thread, cnt);
    for (unsigned i = 0; i < cnt; ++i) {
        const GuestPcRecord *r =
            &t->r[(t->n - cnt + i) % GUEST_PC_RING_DEPTH];
        fprintf(f, "[guest]   pc=%08x esp=%08x ecx=%08x eax=%08x\n",
                (unsigned)r->pc, (unsigned)r->esp, (unsigned)r->ecx,
                (unsigned)r->eax);
    }
}
uint32_t guest_watch_last_pc(void) { return guest_watch.last_pc; }

void guest_watch_set_pc(uint32_t pc, unsigned thread) {
    g_pc_ring[g_pc_n++ % (sizeof g_pc_ring / sizeof g_pc_ring[0])] = pc;
    if (!guest_watch.active) { guest_watch.last_pc = pc; guest_watch.last_thread = thread; return; }
    /* A changed value is attributed to the instruction that just finished. */
    if (guest_watch.last_pc) guest_watch_record(NULL, "post-guest");
    guest_watch.last_pc = pc;
    guest_watch.last_thread = thread;
}

void guest_watch_check(GuestMemory *m, const char *source) {
    if (guest_watch.active) guest_watch_record(m, source);
}
void guest_watch_check_active(const char *source) {
    if (guest_watch.active) guest_watch_record(guest_watch.memory, source);
}
void guest_watch_check_host(const void *p, size_t n, const char *source) {
    if (!guest_watch.active || !p || !n) return;
    if (!guest_watch.host_address && guest_watch.memory)
        guest_watch.host_address = (uint8_t *)guest_ptr(guest_watch.memory,
                                                        guest_watch.address, 4);
    if (!guest_watch.host_address) return;
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)guest_watch.host_address;
    if (a < b + 4 && b < a + n)
        guest_watch_record(guest_watch.memory, source);
}

typedef struct { uint8_t *ptr; uint32_t bytes; uint32_t first, count; } SlabAlloc;

/* Optional per-page touch map over one guest range (measurement only). The
 * hot-path cost is one subtraction and one compare at the top of guest_ptr(). */
static uint32_t g_touch_base, g_touch_span, g_touch_pages;
static uint8_t *g_touch_bits;
static uint64_t g_touch_accesses, g_touch_repeats;
void guest_touch_begin(uint32_t base, uint32_t size) {
    if (g_touch_bits || !size) return;
    uint32_t first = base & ~(GUEST_PAGE_SIZE - 1u);
    uint32_t end = base + size;
    uint32_t pages = ((end - first) + GUEST_PAGE_SIZE - 1u) / GUEST_PAGE_SIZE;
    uint8_t *bits = (uint8_t *)calloc(pages, 1);
    if (!bits) return;
    g_touch_base = first;
    g_touch_span = pages * GUEST_PAGE_SIZE;
    g_touch_pages = pages;
    g_touch_bits = bits;
}
int guest_touch_active(void) { return g_touch_bits != NULL; }
const uint8_t *guest_touch_bitmap(void) { return g_touch_bits; }
uint32_t guest_touch_page_count(void) { return g_touch_pages; }
void guest_touch_stats(uint64_t *accesses, uint64_t *repeats) {
    if (accesses) *accesses = g_touch_accesses;
    if (repeats) *repeats = g_touch_repeats;
}
static void guest_touch_note(uint32_t addr, size_t n) {
    if (!g_touch_bits) return;
    uint32_t off = addr - g_touch_base;
    if (off >= g_touch_span) return;
    size_t left = g_touch_span - off;
    if (n > left) n = left;
    uint32_t a = off, last = off + (uint32_t)(n ? n - 1u : 0u);
    ++g_touch_accesses;
    for (;;) {
        uint8_t *b = &g_touch_bits[a >> 12];
        if (*b) ++g_touch_repeats;
        if (*b < 255) ++*b;
        uint32_t next = (a | (GUEST_PAGE_SIZE - 1u)) + 1u;
        if (next > last) break;
        a = next;
    }
}

void guest_init(GuestMemory *m, size_t limit) {
    memset(m, 0, sizeof *m);
    m->limit = limit;
    memset(&guest_watch, 0, sizeof guest_watch);
    guest_watch.memory = m;
    const char *address = getenv("N3DS_WATCH_ADDR");
    if (address && *address) {
        const char *target = getenv("N3DS_WATCH_VALUE");
        guest_watch_configure((uint32_t)strtoul(address, NULL, 0),
                              target && *target ? (uint32_t)strtoul(target, NULL, 0)
                                                : UINT32_MAX);
    }
}

/* ---- ordinary guest regions -------------------------------------------------- */
void guest_destroy(GuestMemory *m) {
    guest_watch_dump(m);
    free(g_touch_bits);
    g_touch_bits = NULL; g_touch_span = 0; g_touch_pages = 0;
    for (size_t i=0; i<m->count; ++i) free(m->regions[i].data);
    for (unsigned i=0; i<GUEST_MAX_SLABS; ++i)
        if (m->slabs[i].state) guest_slab_release(m, m->slabs[i].base);
    memset(m, 0, sizeof *m);
}
int guest_map(GuestMemory *m, uint32_t base, uint32_t size) {
    uint64_t end = (uint64_t)base + size;
    if (!size || end > (UINT64_C(1)<<32) || m->count >= GUEST_MAX_REGIONS ||
        size > m->limit - m->used) return -1;
    for (size_t i=0; i<m->count; ++i) {
        GuestRegion *r = &m->regions[i];
        if (base < (uint64_t)r->base + r->size && r->base < end) return -1;
    }
    uint8_t *data = calloc(1, size);
    if (!data) return -1;
    m->regions[m->count++] = (GuestRegion){base,size,data};
    m->used += size;
    return 0;
}
int guest_unmap(GuestMemory *m, uint32_t base) {
    for (size_t i=0; i<m->count; ++i) if (m->regions[i].base == base) {
        if (guest_watch.active && base <= guest_watch.address &&
            guest_watch.address - base < m->regions[i].size)
            guest_watch.host_address = NULL;
        m->used -= m->regions[i].size;
        free(m->regions[i].data);
        m->regions[i] = m->regions[--m->count];
        return 0;
    }
    return -1;
}

/* ---- lazy page-backed slabs ---------------------------------------------------
 * Every committed page is a separate 4 KiB host allocation. A range that
 * crosses a page boundary and whose pages are not host-contiguous is bridged
 * by one allocation covering the whole range; the bridge replaces the page
 * allocations it covers, so physical accounting stays exact. No 256 MiB host
 * reservation is required, which is what makes this path usable on the 3DS
 * (svcControlMemory MEMOP_RESERVE is not implemented there). */
GuestSlab *guest_slab_find(GuestMemory *m, uint32_t addr, uint32_t n) {
    if (!m || !n) return NULL;
    uint64_t end = (uint64_t)addr + n;
    for (unsigned i=0; i<GUEST_MAX_SLABS; ++i) {
        GuestSlab *s = &m->slabs[i];
        if (!s->state) continue;
        if (addr >= s->base && end <= (uint64_t)s->base + s->size) return s;
    }
    return NULL;
}

/* The allocation registry is a small array stored immediately after the page
 * arrays. It is sized for one allocation per page plus bridge headroom. */
typedef struct {
    GuestSlab *slab;
    SlabAlloc *allocs;
    uint32_t count, cap;
} SlabRegistry;
static SlabRegistry regs[GUEST_MAX_SLABS];

static int reg_init(unsigned slot, GuestSlab *s, uint32_t cap) {
    regs[slot].allocs = (SlabAlloc *)calloc(cap, sizeof(SlabAlloc));
    if (!regs[slot].allocs) return -1;
    regs[slot].slab = s;
    regs[slot].count = 0;
    regs[slot].cap = cap;
    return 0;
}
static unsigned reg_slot(GuestSlab *s) {
    for (unsigned i=0; i<GUEST_MAX_SLABS; ++i) if (regs[i].slab == s) return i;
    return GUEST_MAX_SLABS;
}
static int reg_add(GuestSlab *s, uint8_t *ptr, uint32_t bytes, uint32_t first, uint32_t count) {
    unsigned i = reg_slot(s);
    if (i == GUEST_MAX_SLABS || regs[i].count >= regs[i].cap) return -1;
    regs[i].allocs[regs[i].count++] = (SlabAlloc){ptr, bytes, first, count};
    return 0;
}
static int reg_find_page(GuestSlab *s, uint32_t page) {
    unsigned i = reg_slot(s);
    if (i == GUEST_MAX_SLABS) return -1;
    for (unsigned k=0; k<regs[i].count; ++k) {
        SlabAlloc *a = &regs[i].allocs[k];
        if (page >= a->first && page - a->first < a->count) return (int)k;
    }
    return -1;
}
static void reg_remove(GuestSlab *s, unsigned k) {
    unsigned i = reg_slot(s);
    if (i == GUEST_MAX_SLABS) return;
    SlabRegistry *r = &regs[i];
    r->allocs[k] = r->allocs[--r->count];
}

GuestSlab *guest_slab_reserve(GuestMemory *m, uint32_t base, uint32_t size) {
    if (!m || !size || (base & (GUEST_PAGE_SIZE-1)) || (size & (GUEST_PAGE_SIZE-1))) return NULL;
    if ((uint64_t)base + size > (UINT64_C(1)<<32)) return NULL;
    for (unsigned i=0; i<GUEST_MAX_SLABS; ++i) {
        GuestSlab *s = &m->slabs[i];
        if (s->state) {
            if (base < (uint64_t)s->base + s->size && s->base < (uint64_t)base + size)
                return NULL;
            continue;
        }
        uint32_t pages = size / GUEST_PAGE_SIZE;
        size_t meta = (size_t)pages * (4 + sizeof(uint8_t *)) + (size_t)pages * sizeof(SlabAlloc);
        if (meta > m->limit - m->used) return NULL;
        uint8_t *state = (uint8_t *)calloc(pages, 1);
        uint8_t *protect = (uint8_t *)calloc(pages, 1);
        uint8_t *ever = (uint8_t *)calloc(pages, 1);
        uint8_t *pinned = (uint8_t *)calloc(pages, 1);
        uint8_t **page = (uint8_t **)calloc(pages, sizeof(uint8_t *));
        if (!state || !protect || !ever || !pinned || !page || reg_init(i, s, pages)) {
            free(state); free(protect); free(ever); free(pinned); free(page);
            return NULL;
        }
        memset(s, 0, sizeof *s);
        s->base = base; s->size = size; s->pages = pages;
        s->state = state; s->protect = protect; s->ever = ever; s->page = page;
        s->pinned = pinned;
        s->type = 0x20000; /* MEM_PRIVATE */
        m->slab_meta += meta;
        m->used += meta;
        return s;
    }
    return NULL;
}

static int page_flush(GuestMemory *m, GuestSlab *s, uint32_t p) {    if (!s->mapped || !s->writable || !s->dirty || !s->dirty[p] || !s->flush) return 0;
    if (!s->page[p]) return 0;
    if (s->flush(s->provider_ctx, s->provider_base + (uint64_t)p * GUEST_PAGE_SIZE,
                 s->page[p], GUEST_PAGE_SIZE)) {
        m->last_fault = GUEST_FAULT_IO;
        return -1;
    }
    s->dirty[p] = 0;
    ++m->pages_dirty_written;
    return 0;
}
/* Evict one clean (or write-back) file-backed page, never the page we are
 * about to back. Anonymous pages have no backing store and are not evicted. */
static void page_drop(GuestMemory *m, GuestSlab *s, uint32_t p);
/* A read-only file page can be dropped only if it still matches its provider.
 * Without write faults the comparison is the COW trigger: a page the guest
 * modified is pinned as an anonymous copy (its block already holds the data),
 * so the file is never written and a later read still sees the modification. */
static int page_can_drop(GuestMemory *m, GuestSlab *s, uint32_t p) {
    if (!s->fill || s->writable || s->flush) return 1;
    uint8_t buf[GUEST_PAGE_SIZE];
    size_t got = 0;
    if (s->fill(s->provider_ctx, s->provider_base + (uint64_t)p * GUEST_PAGE_SIZE,
                buf, GUEST_PAGE_SIZE, &got))
        return 0;
    if (got < GUEST_PAGE_SIZE) memset(buf + got, 0, GUEST_PAGE_SIZE - got);
    if (memcmp(buf, s->page[p], GUEST_PAGE_SIZE) == 0) return 1;
    if (s->pinned && !s->pinned[p]) {
        s->pinned[p] = 1;
        if (s->resident_pages) --s->resident_pages;
        ++m->pages_pinned;
    }
    return 0;
}
static int slab_evict_one(GuestMemory *m, GuestSlab *skip, uint32_t skip_page) {
    for (unsigned n=0; n<GUEST_MAX_SLABS; ++n) {
        unsigned si = (m->evict_slab + n) % GUEST_MAX_SLABS;
        GuestSlab *s = &m->slabs[si];
        if (!s->state || !s->mapped || !s->backed_pages) continue;
        for (uint32_t k=0; k<s->pages; ++k) {
            uint32_t p = (m->evict_page + k) % s->pages;
            if (s == skip && p == skip_page) continue;
            if (!s->page[p]) continue;
            if (s->pinned && s->pinned[p]) continue;
            int was_dirty = s->dirty && s->dirty[p];
            if (page_flush(m, s, p)) return 0;
            if (!page_can_drop(m, s, p)) continue;
            unsigned slot = reg_find_page(s, p);
            if ((int)slot < 0) continue;
            page_drop(m, s, p);
            ++m->pages_evicted;
            if (!was_dirty) ++m->pages_evicted_clean;
            m->evict_slab = si;
            m->evict_page = (p + 1) % s->pages;
            return 1;
        }
    }
    return 0;
}
/* Keep a provider slab inside its window: drop evictable pages outside the
 * range the caller is about to use. An allocation that overlaps the live range
 * is skipped as a whole, because dropping it would free the returned pointer.
 * Runs only when a page was actually backed, so cache hits never scan here. */
static void slab_trim_window(GuestMemory *m, GuestSlab *s,
                             uint32_t keep_first, uint32_t keep_last) {
    if (!s->resident_cap) return;
    unsigned slot = reg_slot(s), skips = 0;
    while (s->resident_pages > s->resident_cap && skips < 8) {
        uint32_t q = s->trim_cursor;
        s->trim_cursor = (q + 1) % s->pages;
        if (!s->page[q]) { ++skips; continue; }
        if (q >= keep_first && q <= keep_last) { ++skips; continue; }
        if (s->pinned && s->pinned[q]) { ++skips; continue; }
        if (slot != GUEST_MAX_SLABS) {
            int ai = reg_find_page(s, q);
            if (ai >= 0) {
                SlabAlloc *a = &regs[slot].allocs[ai];
                if (a->first <= keep_last && keep_first <= a->first + a->count - 1) {
                    ++skips; continue;
                }
            }
        }
        if (!page_can_drop(m, s, q)) { ++skips; continue; }
        page_drop(m, s, q);
        ++m->pages_evicted;
        ++m->pages_evicted_clean;
        skips = 0;
    }
}
static int page_back(GuestMemory *m, GuestSlab *s, uint32_t p) {
    if (s->page[p]) return 0;
    if (m->used + GUEST_PAGE_SIZE > m->limit) {
        if (!slab_evict_one(m, s, p) || m->used + GUEST_PAGE_SIZE > m->limit) {
            m->last_fault = GUEST_FAULT_NO_MEMORY;
            ++m->backing_failures;
            return -1;
        }
    }
    uint8_t *q = (uint8_t *)calloc(1, GUEST_PAGE_SIZE);
    if (!q) {
        m->last_fault = GUEST_FAULT_NO_MEMORY;
        ++m->backing_failures;
        return -1;
    }
    if (s->fill) {
        size_t got = 0;
        if (s->fill(s->provider_ctx, s->provider_base + (uint64_t)p * GUEST_PAGE_SIZE,
                    q, GUEST_PAGE_SIZE, &got)) {
            free(q);
            m->last_fault = GUEST_FAULT_IO;
            return -1;
        }
        /* calloc() zeroed the tail past a short read. */
    }
    if (reg_add(s, q, GUEST_PAGE_SIZE, p, 1)) {
        free(q);
        m->last_fault = GUEST_FAULT_NO_MEMORY;
        ++m->backing_failures;
        return -1;
    }
    s->page[p] = q;
    if (s->ever && s->ever[p]) {
        if (s->mapped) ++m->pages_reloaded;
    } else {
        ++m->pages_first_touched;
        if (s->mapped) ++m->pages_file; else ++m->pages_anon;
    }
    if (s->ever) s->ever[p] = 1;
    ++s->backed_pages;
    if (!s->pinned || !s->pinned[p]) ++s->resident_pages;
    m->slab_backed += GUEST_PAGE_SIZE;
    m->used += GUEST_PAGE_SIZE;
    if (m->slab_backed > m->slab_backed_peak) m->slab_backed_peak = m->slab_backed;
    return 0;
}
/* Free the allocation containing page p and clear every page it covers. */
static void page_drop(GuestMemory *m, GuestSlab *s, uint32_t p) {
    unsigned slot = reg_slot(s);
    if (slot == GUEST_MAX_SLABS) return;
    int ai = reg_find_page(s, p);
    if (ai < 0) { s->page[p] = NULL; return; }
    SlabAlloc a = regs[slot].allocs[ai];
    for (uint32_t q=a.first; q<a.first+a.count; ++q) {
        if (q < s->pages && s->page[q]) {
            s->page[q] = NULL;
            if (s->dirty) s->dirty[q] = 0;
            if (!s->pinned || !s->pinned[q]) {
                if (s->resident_pages) --s->resident_pages;
            }
            if (s->backed_pages) --s->backed_pages;
        }
    }
    m->slab_backed = a.bytes <= m->slab_backed ? m->slab_backed - a.bytes : 0;
    m->used = a.bytes <= m->used ? m->used - a.bytes : 0;
    free(a.ptr);
    reg_remove(s, ai);
}
/* Drop the allocation containing p while preserving backed pages outside
 * [keep_first,keep_last]. Used when a new cross-page bridge replaces part of
 * an older bridge. */
static int page_drop_preserve(GuestMemory *m, GuestSlab *s, uint32_t p,
                              uint32_t keep_first, uint32_t keep_last) {
    unsigned slot = reg_slot(s);
    if (slot == GUEST_MAX_SLABS) { page_drop(m,s,p); return 0; }
    int ai = reg_find_page(s,p);
    if (ai < 0) { page_drop(m,s,p); return 0; }
    SlabAlloc a = regs[slot].allocs[ai];
    int inside = a.first >= keep_first && a.first + a.count - 1 <= keep_last;
    if (inside) { page_drop(m,s,p); return 0; }
    uint8_t **tmp = (uint8_t **)calloc(a.count,sizeof(uint8_t *));
    if (!tmp) { m->last_fault=GUEST_FAULT_NO_MEMORY; ++m->backing_failures; return -1; }
    int bad=0;
    for (uint32_t q=a.first;q<a.first+a.count;++q) {
        if ((q>=keep_first && q<=keep_last) || q>=s->pages || !s->state[q] || !s->page[q])
            continue;
        tmp[q-a.first]=(uint8_t *)calloc(1,GUEST_PAGE_SIZE);
        if (!tmp[q-a.first]) { bad=1; break; }
        memcpy(tmp[q-a.first],s->page[q],GUEST_PAGE_SIZE);
    }
    if (bad) {
        for (uint32_t k=0;k<a.count;++k) free(tmp[k]);
        free(tmp); m->last_fault=GUEST_FAULT_NO_MEMORY; ++m->backing_failures;
        return -1;
    }
    page_drop(m,s,p);
    for (uint32_t q=a.first;q<a.first+a.count;++q) {
        uint8_t *d=tmp[q-a.first];
        if (!d) continue;
        if (reg_add(s,d,GUEST_PAGE_SIZE,q,1)) {
            free(d); tmp[q-a.first]=NULL; m->last_fault=GUEST_FAULT_NO_MEMORY;
            ++m->backing_failures;
            for (uint32_t k=0;k<a.count;++k) free(tmp[k]);
            free(tmp); return -1;
        }
        s->page[q]=d; ++s->backed_pages;
        if (!s->pinned || !s->pinned[q]) ++s->resident_pages;
        m->slab_backed+=GUEST_PAGE_SIZE; m->used+=GUEST_PAGE_SIZE;
    }
    free(tmp);
    return 0;
}
/* Prepare [addr,addr+n) inside one slab and return a host pointer. Ranges that
 * cross a page boundary are bridged into one contiguous allocation. */
static int slab_prepare(GuestMemory *m, GuestSlab *s, uint32_t addr, size_t n, uint8_t **out) {
    if (!n) { m->last_fault = GUEST_FAULT_UNMAPPED; return -1; }
    uint64_t off = (uint64_t)addr - s->base;
    uint64_t end = off + n;
    if (end > s->size) { m->last_fault = GUEST_FAULT_UNMAPPED; return -1; }
    uint32_t first = (uint32_t)(off / GUEST_PAGE_SIZE);
    uint32_t last = (uint32_t)((end - 1) / GUEST_PAGE_SIZE);
    for (uint32_t p=first; p<=last; ++p)
        if (!s->state[p]) { m->last_fault = GUEST_FAULT_UNMAPPED; return -1; }
    int backed = 0;
    for (uint32_t p=first; p<=last; ++p)
        if (!s->page[p]) { if (page_back(m, s, p)) return -1; backed = 1; }
    if (first != last) {
        int contiguous = 1;
        for (uint32_t p=first; p<last; ++p)
            if (s->page[p+1] != s->page[p] + GUEST_PAGE_SIZE) { contiguous = 0; break; }
        if (!contiguous) {
            uint32_t count = last - first + 1;
            uint8_t *bridge = (uint8_t *)calloc(count, GUEST_PAGE_SIZE);
            if (!bridge) { m->last_fault = GUEST_FAULT_NO_MEMORY; ++m->backing_failures; return -1; }
            for (uint32_t p=first; p<=last; ++p)
                memcpy(bridge + (size_t)(p-first) * GUEST_PAGE_SIZE, s->page[p], GUEST_PAGE_SIZE);
            /* Release the replaced page allocations and account the bridge.
             * Preserve pages outside this range if an older bridge covered
             * them as well. */
            for (uint32_t p=first; p<=last; ++p)
                if (s->page[p] && page_drop_preserve(m,s,p,first,last)) {
                    free(bridge);
                    return -1;
                }
            if (reg_add(s, bridge, count * GUEST_PAGE_SIZE, first, count)) {
                free(bridge); m->last_fault = GUEST_FAULT_NO_MEMORY; return -1;
            }
            for (uint32_t p=first; p<=last; ++p) {
                s->page[p] = bridge + (size_t)(p-first) * GUEST_PAGE_SIZE;
                ++s->backed_pages;
                if (!s->pinned || !s->pinned[p]) ++s->resident_pages;
                m->slab_backed += GUEST_PAGE_SIZE;
                m->used += GUEST_PAGE_SIZE;
                if (m->slab_backed > m->slab_backed_peak) m->slab_backed_peak = m->slab_backed;
            }
        }
    }
    if (s->writable && s->dirty)
        for (uint32_t p=first; p<=last; ++p) s->dirty[p] = 1;
    *out = s->page[first] + (size_t)(off % GUEST_PAGE_SIZE);
    if (backed) slab_trim_window(m, s, first, last);
    return 0;
}

int guest_slab_commit(GuestMemory *m, uint32_t a, uint32_t size, uint32_t protect) {
    GuestSlab *s = guest_slab_find(m, a, size);
    if (!s) return -1;
    uint32_t first = (a - s->base) / GUEST_PAGE_SIZE;
    uint32_t last = (a - s->base + size - 1) / GUEST_PAGE_SIZE;
    for (uint32_t p=first; p<=last; ++p) {
        s->state[p] = 1;
        s->protect[p] = (uint8_t)protect;
        /* Re-commit is a no-op: existing backing is left untouched. */
    }
    return 0;
}
int guest_slab_decommit(GuestMemory *m, uint32_t a, uint32_t size) {
    GuestSlab *s = guest_slab_find(m, a, size);
    if (!s) return -1;
    uint32_t first = (a - s->base) / GUEST_PAGE_SIZE;
    uint32_t last = (a - s->base + size - 1) / GUEST_PAGE_SIZE;
    unsigned slot = reg_slot(s);
    for (uint32_t p=first; p<=last; ++p) {
        int was_committed = s->state[p];
        if (s->page[p] && slot != GUEST_MAX_SLABS) {
            int ai = reg_find_page(s, p);
            if (ai >= 0) {
                SlabAlloc al = regs[slot].allocs[ai];
                int inside = al.first >= first && al.first + al.count - 1 <= last;
                for (uint32_t q=al.first; q<al.first+al.count; ++q)
                    if (q < s->pages && s->page[q] && page_flush(m, s, q)) {
                        if (m->fault_handler)
                            m->fault_handler(m, s->base + (uint64_t)q * GUEST_PAGE_SIZE,
                                             guest_fault_text(m));
                        return -1;
                    }
                if (inside) {
                    page_drop(m, s, p);
                } else {
                    /* The allocation is a bridge that also covers pages outside
                     * [first,last]. Save those pages, free the bridge, then
                     * restore them as individual allocations. */
                    uint8_t **tmp = (uint8_t **)calloc(al.count, sizeof(uint8_t *));
                    if (!tmp) { m->last_fault = GUEST_FAULT_NO_MEMORY; ++m->backing_failures; return -1; }
                    int bad = 0;
                    for (uint32_t q=al.first; q<al.first+al.count; ++q) {
                        if ((q >= first && q <= last) || q >= s->pages ||
                            !s->state[q] || !s->page[q]) continue;
                        tmp[q-al.first] = (uint8_t *)calloc(1, GUEST_PAGE_SIZE);
                        if (!tmp[q-al.first]) { bad = 1; break; }
                        memcpy(tmp[q-al.first], s->page[q], GUEST_PAGE_SIZE);
                    }
                    if (bad) {
                        for (uint32_t k=0;k<al.count;++k) free(tmp[k]);
                        free(tmp);
                        m->last_fault = GUEST_FAULT_NO_MEMORY;
                        ++m->backing_failures;
                        return -1;
                    }
                    page_drop(m, s, p);
                    for (uint32_t q=al.first; q<al.first+al.count; ++q) {
                        uint8_t *d = tmp[q-al.first];
                        if (!d) continue;
                        if (reg_add(s, d, GUEST_PAGE_SIZE, q, 1)) {
                            free(d); tmp[q-al.first]=NULL;
                            m->last_fault = GUEST_FAULT_NO_MEMORY;
                            ++m->backing_failures;
                            for (uint32_t k=0;k<al.count;++k) free(tmp[k]);
                            free(tmp);
                            return -1;
                        }
                        s->page[q] = d;
                        s->ever[q] = 1;
                        ++s->backed_pages;
                        if (!s->pinned || !s->pinned[q]) ++s->resident_pages;
                        m->slab_backed += GUEST_PAGE_SIZE;
                        m->used += GUEST_PAGE_SIZE;
                    }
                    free(tmp);
                }
            } else {
                s->page[p] = NULL;
            }
        }
        if (was_committed) ++m->pages_decommitted;
        s->state[p] = 0;
        s->protect[p] = 0;
    }
    return 0;
}
int guest_slab_release(GuestMemory *m, uint32_t base) {
    for (unsigned i=0; i<GUEST_MAX_SLABS; ++i) {
        GuestSlab *s = &m->slabs[i];
        if (!s->state || s->base != base) continue;
        for (uint32_t p=0; p<s->pages; ++p)
            if (s->page[p] && page_flush(m, s, p)) {
                if (m->fault_handler)
                    m->fault_handler(m, s->base + (uint64_t)p * GUEST_PAGE_SIZE,
                                     guest_fault_text(m));
                /* Keep going: release must not leak host memory. The caller
                 * still sees GUEST_FAULT_IO through guest_fault_text(). */
            }
        unsigned slot = reg_slot(s);
        if (slot != GUEST_MAX_SLABS) {
            SlabRegistry *r = &regs[slot];
            for (unsigned k=0; k<r->count; ++k) {
                uint32_t bytes = r->allocs[k].bytes;
                uint32_t covered = r->allocs[k].count;
                free(r->allocs[k].ptr);
                m->slab_backed = bytes <= m->slab_backed ? m->slab_backed - bytes : 0;
                m->used = bytes <= m->used ? m->used - bytes : 0;
                (void)covered;
            }
            free(r->allocs);
            memset(r, 0, sizeof *r);
        }
        size_t meta = (size_t)s->pages * (4 + sizeof(uint8_t *)) + (size_t)s->pages * sizeof(SlabAlloc);
        if (s->dirty) meta += s->pages;
        m->slab_meta = meta <= m->slab_meta ? m->slab_meta - meta : 0;
        m->used = meta <= m->used ? m->used - meta : 0;
        if (s->provider_close) s->provider_close(s->provider_ctx);
        free(s->state); free(s->protect); free(s->ever); free(s->pinned);
        free(s->page); free(s->dirty);
        memset(s, 0, sizeof *s);
        return 0;
    }
    return -1;
}
int guest_slab_state(GuestMemory *m, uint32_t addr, uint32_t *protect) {
    GuestSlab *s = guest_slab_find(m, addr, 1);
    if (!s) return -1;
    uint32_t p = (addr - s->base) / GUEST_PAGE_SIZE;
    if (protect) *protect = s->state[p] ? s->protect[p] : 0;
    return s->state[p] ? 1 : 0;
}
int guest_slab_set_provider(GuestMemory *m, uint32_t base, uint32_t protect,
                            int writable, GuestPageFill fill, GuestPageFlush flush,
                            void *ctx, uint64_t file_base) {
    GuestSlab *s = guest_slab_find(m, base, 1);
    if (!s) return -1;
    size_t extra = writable ? s->pages : 0;
    if (extra > m->limit - m->used) return -1;
    uint8_t *dirty = writable ? (uint8_t *)calloc(s->pages, 1) : NULL;
    if (writable && !dirty) return -1;
    s->mapped = 1;
    s->writable = writable;
    s->type = 0x40000; /* MEM_MAPPED */
    s->fill = fill;
    s->flush = flush;
    s->provider_ctx = ctx;
    s->provider_base = file_base;
    s->dirty = dirty;
    if (dirty) { m->slab_meta += extra; m->used += extra; }
    for (uint32_t p=0; p<s->pages; ++p) {
        s->state[p] = 1;
        s->protect[p] = (uint8_t)protect;
    }
    return 0;
}
int guest_slab_set_provider_close(GuestMemory *m, uint32_t base,
                                  void (*fn)(void *)) {
    GuestSlab *s = guest_slab_find(m, base, 1);
    if (!s) return -1;
    s->provider_close = fn;
    return 0;
}
int guest_slab_set_window(GuestMemory *m, uint32_t base, uint32_t pages) {
    GuestSlab *s = guest_slab_find(m, base, 1);
    if (!s) return -1;
    s->resident_cap = pages;
    return 0;
}
int guest_slab_set_type(GuestMemory *m, uint32_t base, int type) {
    GuestSlab *s = guest_slab_find(m, base, 1);
    if (!s) return -1;
    s->type = type;
    return 0;
}
int guest_slab_pin_range(GuestMemory *m, uint32_t base, uint32_t addr, uint32_t size) {
    GuestSlab *s = guest_slab_find(m, base, 1);
    if (!s || !s->pinned || !size || addr < s->base || addr - s->base >= s->size) return -1;
    uint32_t first = (addr - s->base) / GUEST_PAGE_SIZE;
    uint64_t end = (uint64_t)addr + size;
    uint32_t last = (uint32_t)((end - 1 - s->base) / GUEST_PAGE_SIZE);
    if (last >= s->pages) last = s->pages - 1;
    for (uint32_t p = first; p <= last; ++p) {
        if (s->pinned[p]) continue;
        s->pinned[p] = 1;
        if (s->page[p] && s->resident_pages) --s->resident_pages;
    }
    return 0;
}
uint64_t guest_slab_backed(const GuestMemory *m) { return m ? m->slab_backed : 0; }uint64_t guest_slab_backed_peak(const GuestMemory *m) { return m ? m->slab_backed_peak : 0; }
GuestFault guest_last_fault(const GuestMemory *m) { return m ? m->last_fault : GUEST_FAULT_UNMAPPED; }
const char *guest_fault_text(const GuestMemory *m) {
    switch (guest_last_fault(m)) {
    case GUEST_FAULT_NO_MEMORY: return "guest memory backing failed: out of memory";
    case GUEST_FAULT_IO: return "guest file-backed page I/O error";
    default: return "unmapped guest memory";
    }
}
void guest_set_fault_handler(GuestMemory *m,
                             void (*fn)(GuestMemory *, uint32_t, const char *)) {
    if (m) m->fault_handler = fn;
}
void guest_page_stats(const GuestMemory *m, GuestPageStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m) return;
    out->first_touched = m->pages_first_touched;
    out->anon = m->pages_anon;
    out->file = m->pages_file;
    out->dirty_written = m->pages_dirty_written;
    out->decommitted = m->pages_decommitted;
    out->evicted = m->pages_evicted;
    out->evicted_clean = m->pages_evicted_clean;
    out->reloaded = m->pages_reloaded;
    out->failures = m->backing_failures;
}

int guest_range_mapped(GuestMemory *m, uint32_t base, uint32_t size) {
    if (!m || !size) return 0;
    uint64_t end = (uint64_t)base + size;
    for (size_t i = 0; i < m->count; ++i) {
        GuestRegion *r = &m->regions[i];
        if (base >= r->base && end <= (uint64_t)r->base + r->size) return 1;
    }
    GuestSlab *s = guest_slab_find(m, base, size);
    if (!s || !s->state) return 0;
    uint32_t first = (base - s->base) / GUEST_PAGE_SIZE;
    uint32_t last = (uint32_t)((end - 1 - s->base) / GUEST_PAGE_SIZE);
    if (last >= s->pages) return 0;
    for (uint32_t p = first; p <= last; ++p) if (!s->state[p]) return 0;
    return 1;
}

void *guest_ptr(GuestMemory *m, uint32_t addr, size_t n) {
    m->last_fault = GUEST_FAULT_NONE;
    if (!n) return NULL;
    guest_touch_note(addr, n);
    for (size_t i=0; i<m->count; ++i) {
        GuestRegion *r = &m->regions[i];
        if (addr >= r->base && n <= r->size && addr-r->base <= r->size-n)
            return r->data + (addr-r->base);
    }
    if (n > UINT32_MAX) { m->last_fault = GUEST_FAULT_UNMAPPED; return NULL; }
    GuestSlab *s = guest_slab_find(m, addr, (uint32_t)n);
    if (!s) { m->last_fault = GUEST_FAULT_UNMAPPED; return NULL; }
    uint8_t *p = NULL;
    if (slab_prepare(m, s, addr, n, &p)) {
        if ((m->last_fault == GUEST_FAULT_NO_MEMORY || m->last_fault == GUEST_FAULT_IO) &&
            m->fault_handler)
            m->fault_handler(m, addr, guest_fault_text(m));
        return NULL;
    }
    return p;
}
/* Page-aware copies: validate and back every page before copying anything, so
 * an unmapped page later in the range cannot produce a partial write. */
static int copy_range(GuestMemory *m, uint32_t addr, void *buf, size_t n, int to_guest) {
    uint8_t *b = (uint8_t *)buf;
    uint32_t a = addr;
    size_t left = n;
    while (left) {
        size_t room = GUEST_PAGE_SIZE - (a & (GUEST_PAGE_SIZE-1));
        size_t chunk = left < room ? left : room;
        if (!guest_ptr(m, a, chunk)) return -1;
        a += (uint32_t)chunk; left -= chunk;
    }
    a = addr; left = n;
    while (left) {
        size_t room = GUEST_PAGE_SIZE - (a & (GUEST_PAGE_SIZE-1));
        size_t chunk = left < room ? left : room;
        void *p = guest_ptr(m, a, chunk);
        if (!p) return -1; /* cannot happen after the validation pass */
        if (to_guest) memcpy(p, b, chunk); else memcpy(b, p, chunk);
        if (to_guest) guest_watch_check(m, "guest_write");
        a += (uint32_t)chunk; b += chunk; left -= chunk;
    }
    return 0;
}
int guest_read(GuestMemory *m, uint32_t addr, void *dst, size_t n) {
    if (!n) return 0;
    if (!dst) { m->last_fault = GUEST_FAULT_UNMAPPED; return -1; }
    return copy_range(m, addr, dst, n, 0);
}
int guest_write(GuestMemory *m, uint32_t addr, const void *src, size_t n) {
    if (!n) return 0;
    if (!src) { m->last_fault = GUEST_FAULT_UNMAPPED; return -1; }
    return copy_range(m, addr, (void *)src, n, 1);
}
int guest_read32(GuestMemory *m, uint32_t addr, uint32_t *v) {
    HP_START(hp_t0);
    uint8_t b[4];
    if (!v || guest_read(m,addr,b,4)) { HP_ADD(HP_GUEST_READ, hp_t0); return -1; }
    *v = (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
    HP_ADD(HP_GUEST_READ, hp_t0);
    return 0;
}
int guest_write32(GuestMemory *m, uint32_t addr, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    return guest_write(m,addr,b,4);
}
const char *guest_cstr(GuestMemory *m,uint32_t addr,size_t max) {
    if(!addr || !max)return NULL;
    for(size_t i=0;i<m->count;++i) {
        GuestRegion *r=&m->regions[i];
        if(addr>=r->base && addr-r->base<r->size) {
            size_t n=r->size-(addr-r->base);if(n>max)n=max;
            const char *p=(const char *)r->data+(addr-r->base);
            return memchr(p,0,n)?p:NULL;
        }
    }
    if (guest_slab_find(m,addr,1)) {
        for (size_t i=0;i<max;++i) {
            const char *p=(const char *)guest_ptr(m,addr+(uint32_t)i,1);
            if (!p) return NULL;
            if (!*p) return (const char *)guest_ptr(m,addr,(uint32_t)i+1);
        }
    }
    return NULL;
}
