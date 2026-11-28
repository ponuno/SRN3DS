#include "guest_swap.h"
#include "guest.h"
#include "lz4.h"

#include <stdlib.h>
#include <string.h>

#define SWAP_PAGE GUEST_PAGE_SIZE
/* LZ4's worst case for one page; used as the per-page compression scratch. */
#define SWAP_TMP_BYTES (SWAP_PAGE + SWAP_PAGE / 255 + 32)
/* How many candidates one safe point may inspect before returning to the
 * guest.  A whole 32 MiB of anonymous pages is 8192 pages, so one pass can
 * reclaim the full target without ever holding the CPU for long. */
#define SWAP_SCAN_PER_TICK 8192u

typedef struct {
    uint32_t key;   /* slab index << 28 | page (pages are < 2^28) */
    uint32_t len;
    uint8_t *data;
    uint8_t state;  /* 0 empty, 1 used, 2 tombstone */
} SwapEntry;

struct GuestSwap {
    GuestMemory *m;
    int on;
    size_t budget;          /* compressed payload ceiling                */
    size_t stored;          /* current compressed payload bytes         */
    uint32_t target_pages;  /* anonymous resident ceiling, 0 = no ceil. */
    uint32_t resident_pages;
    SwapEntry *tab;
    uint32_t cap, used, tombs;
    uint8_t **pool;         /* recycled 4 KiB blocks                    */
    uint32_t pool_n, pool_cap;
    uint32_t cursor_slab, cursor_page;
    void *lz4;              /* LZ4 stream state (heap, no 16 KiB stack)  */
    uint8_t tmp[SWAP_TMP_BYTES];
    GuestSwapStats st;
};

/* The key stays unique across slab slots: pages per slab are < 2^28. */
#define SWAP_KEY_SLAB_BITS 28u
#define SWAP_KEY_MAX_PAGE ((uint32_t)1u << SWAP_KEY_SLAB_BITS)

static uint32_t swap_key(const GuestMemory *m, const GuestSlab *s, uint32_t p) {
    unsigned si = (unsigned)(s - m->slabs);
    return (si << SWAP_KEY_SLAB_BITS) | p;
}
static unsigned key_slab(uint32_t key) { return key >> SWAP_KEY_SLAB_BITS; }
static uint32_t key_hash(uint32_t key) {
    /* Pages and slab slots are both dense, so a 32-bit integer mix is enough:
     * the table never holds more than the compressed budget / 24 entries. */
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    key *= 0x846ca68bu;
    key ^= key >> 16;
    return key;
}

static int tab_grow(struct GuestSwap *w, uint32_t cap) {
    SwapEntry *old = w->tab;
    uint32_t oldcap = w->cap;
    SwapEntry *tab = (SwapEntry *)calloc(cap, sizeof *tab);
    if (!tab) return -1;
    w->tab = tab;
    w->cap = cap;
    w->used = w->tombs = 0;
    for (uint32_t i = 0; i < oldcap; ++i) {
        if (old[i].state != 1) continue;
        uint32_t h = key_hash(old[i].key) & (cap - 1u);
        while (w->tab[h].state == 1) h = (h + 1u) & (cap - 1u);
        w->tab[h] = old[i];
        ++w->used;
    }
    free(old);
    return 0;
}

static SwapEntry *tab_find(struct GuestSwap *w, uint32_t key, uint32_t *slot) {
    if (!w->cap) { if (slot) *slot = 0; return NULL; }
    uint32_t h = key_hash(key) & (w->cap - 1u), first_tomb = UINT32_MAX;
    for (unsigned probe = 0; probe < w->cap; ++probe) {
        SwapEntry *e = &w->tab[h];
        if (e->state == 0) {
            if (slot) *slot = first_tomb != UINT32_MAX ? first_tomb : h;
            return NULL;
        }
        if (e->state == 1 && e->key == key) { if (slot) *slot = h; return e; }
        if (e->state == 2 && first_tomb == UINT32_MAX) first_tomb = h;
        h = (h + 1u) & (w->cap - 1u);
    }
    if (slot) *slot = first_tomb;
    return NULL;
}

/* Inserts (or replaces) one blob.  Takes ownership of `data`. */
static int tab_insert(struct GuestSwap *w, uint32_t key, uint8_t *data,
                      uint32_t len) {
    if (w->cap == 0 && tab_grow(w, 1024)) return -1;
    if ((w->used + w->tombs + 1u) * 10u >= w->cap * 7u) {
        uint32_t cap = w->cap * 2u;
        if (tab_grow(w, cap)) return -1;
    }
    uint32_t slot = 0;
    SwapEntry *e = tab_find(w, key, &slot);
    if (e) {
        free(e->data);
        e->data = data;
        e->len = len;
        return 0;
    }
    if (slot >= w->cap) return -1;
    if (w->tab[slot].state == 2) { if (w->tombs) --w->tombs; }
    w->tab[slot].key = key;
    w->tab[slot].len = len;
    w->tab[slot].data = data;
    w->tab[slot].state = 1;
    ++w->used;
    return 0;
}

static void tab_remove(struct GuestSwap *w, uint32_t key, int free_data) {
    SwapEntry *e = tab_find(w, key, NULL);
    if (!e) return;
    if (free_data) free(e->data);
    e->data = NULL;
    e->len = 0;
    e->state = 2;
    --w->used;
    ++w->tombs;
}

int guest_swap_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (!src || !n || n > (size_t)INT32_MAX) return -1;
    int bound = LZ4_compressBound((int)n);
    if (bound <= 0 || (size_t)bound > cap) return -1;
    return LZ4_compress_default((const char *)src, (char *)dst, (int)n, bound);
}

int guest_swap_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (!src || !dst || !n || cap > (size_t)INT32_MAX || n > (size_t)INT32_MAX)
        return -1;
    return LZ4_decompress_safe((const char *)src, (char *)dst, (int)n,
                               (int)cap);
}

int guest_swap_active(const GuestMemory *m) {
    return m && m->swap && ((const struct GuestSwap *)m->swap)->on;
}

int guest_swap_enable(GuestMemory *m, size_t budget, size_t resident_target) {
    if (!m) return -1;
    if (m->swap) guest_swap_disable(m);
    struct GuestSwap *w = (struct GuestSwap *)calloc(1, sizeof *w);
    if (!w) return -1;
    w->m = m;
    w->on = 1;
    w->budget = budget;
    w->target_pages = (uint32_t)(resident_target / SWAP_PAGE);
    w->pool_cap = 64; /* 256 KiB recycled; the rest returns to malloc */
    w->pool = (uint8_t **)calloc(w->pool_cap, sizeof(uint8_t *));
    w->lz4 = calloc(1, (size_t)LZ4_sizeofState());
    if (!w->pool || !w->lz4) {
        free(w->pool);
        free(w->lz4);
        free(w);
        return -1;
    }
    /* Count already-resident anonymous pages so the first safe point starts
     * from an honest number instead of assuming a cold heap. */
    for (unsigned i = 0; i < GUEST_MAX_SLABS; ++i) {
        GuestSlab *s = &m->slabs[i];
        if (!s->state || s->fill) continue;
        for (uint32_t p = 0; p < s->pages; ++p)
            if (s->page && s->page[p]) ++w->resident_pages;
    }
    w->st.enabled = 1;
    w->st.budget_bytes = budget;
    w->st.target_pages = w->target_pages;
    m->swap = w;
    return 0;
}

void guest_swap_disable(GuestMemory *m) {
    if (!m || !m->swap) return;
    struct GuestSwap *w = (struct GuestSwap *)m->swap;
    if (w->tab) {
        for (uint32_t i = 0; i < w->cap; ++i)
            if (w->tab[i].state == 1) free(w->tab[i].data);
        free(w->tab);
    }
    if (w->pool) {
        for (uint32_t i = 0; i < w->pool_n; ++i) free(w->pool[i]);
        free(w->pool);
    }
    free(w->lz4);
    free(w);
    m->swap = NULL;
}

int guest_swap_configure(GuestMemory *m) {
    const char *on = getenv("N3DS_SWAP");
    if (!on || !*on) return 1;
    if (on[0] == '0' && on[1] == '\0') return 1;
    size_t budget = (size_t)32u << 20;
    size_t target = (size_t)32u << 20;
    const char *b = getenv("N3DS_SWAP_BUDGET_MB");
    if (b && *b) {
        char *end = NULL;
        unsigned long v = strtoul(b, &end, 10);
        if (!end || *end || v == 0 || v > 4096) return -1;
        budget = (size_t)v << 20;
    }
    const char *t = getenv("N3DS_GUEST_TARGET_MB");
    if (t && *t) {
        char *end = NULL;
        unsigned long v = strtoul(t, &end, 10);
        if (!end || *end || v > 4096) return -1;
        target = (size_t)v << 20;
    }
    return guest_swap_enable(m, budget, target) == 0 ? 0 : -1;
}

/* ---- block supply --------------------------------------------------------- */
/* A block is popped from the pool and pushed back exactly once.  Keeping the
 * pool itself duplicate-free is the cheap, accurate ownership check: a second
 * push of the same pointer means two slabs believe they own it, which is the
 * aliasing that corrupts guest data.  (A 128-entry "live" ring was wrong here:
 * resident pages are thousands, so it wrapped and reported false positives.) */
static int pool_contains(const struct GuestSwap *w, const uint8_t *p) {
    for (uint32_t i = 0; i < w->pool_n; ++i)
        if (w->pool[i] == p) return 1;
    return 0;
}
uint8_t *guest_swap_block(GuestMemory *m) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (w && w->pool_n) return w->pool[--w->pool_n];
    return (uint8_t *)calloc(1, SWAP_PAGE);
}

void guest_swap_recycle(GuestMemory *m, uint8_t *block) {
    if (!block) return;
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (w && pool_contains(w, block)) {
        ++w->st.fail_reissued;
        fprintf(stderr, "[swap] block %p recycled twice; refusing duplicate\n",
                (void *)block);
        return;
    }
    if (w && w->pool_n < w->pool_cap) { w->pool[w->pool_n++] = block; return; }
    free(block);
}

/* ---- compressed store ----------------------------------------------------- */
/* Inserts one blob.  Returns its compressed length (> 0), or a negative
 * SWAP_ERR_*: LZ4 did not shrink the page, the budget is full, or the host
 * allocation failed.  On failure the page stays resident. */
int guest_swap_store(GuestMemory *m, GuestSlab *s, uint32_t page,
                     const uint8_t *src) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on || !src || page >= s->pages) return -4;
    int len = LZ4_compress_fast_extState(w->lz4, (const char *)src,
                                         (char *)w->tmp, (int)SWAP_PAGE,
                                         (int)sizeof w->tmp, 1);
    if (len <= 0) return -4;
    if ((uint32_t)len >= SWAP_PAGE) return -2; /* no saving */
    if (w->stored + (size_t)len > w->budget) return -3;
    uint8_t *blob = (uint8_t *)malloc((size_t)len);
    if (!blob) return -4;
    memcpy(blob, w->tmp, (size_t)len);
    if (tab_insert(w, swap_key(m, s, page), blob, (uint32_t)len)) {
        free(blob);
        return -4;
    }
    w->stored += (size_t)len;
    return len;
}

int guest_swap_fetch(GuestMemory *m, GuestSlab *s, uint32_t page, uint8_t *dst) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on || !dst) return 0;
    SwapEntry *e = tab_find(w, swap_key(m, s, page), NULL);
    if (!e) return 0;
    uint32_t key = e->key, len = e->len;
    int n = LZ4_decompress_safe((const char *)e->data, (char *)dst, (int)len,
                                (int)SWAP_PAGE);
    w->stored = len <= w->stored ? w->stored - len : 0;
    tab_remove(w, key, 1);
    if (n != (int)SWAP_PAGE) return -1;
    ++w->st.pages_in;
    w->st.bytes_in += SWAP_PAGE;
    return 1;
}

void guest_swap_discard(GuestMemory *m, GuestSlab *s, uint32_t page) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on || page >= s->pages) return;
    SwapEntry *e = tab_find(w, swap_key(m, s, page), NULL);
    if (!e) return;
    w->stored = e->len <= w->stored ? w->stored - e->len : 0;
    tab_remove(w, e->key, 1);
    ++w->st.pages_dropped;
}

void guest_swap_forget_slab(GuestMemory *m, GuestSlab *s) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on || !w->cap) return;
    unsigned si = (unsigned)(s - m->slabs);
    for (uint32_t i = 0; i < w->cap; ++i) {
        SwapEntry *e = &w->tab[i];
        if (e->state != 1 || key_slab(e->key) != si) continue;
        w->stored = e->len <= w->stored ? w->stored - e->len : 0;
        tab_remove(w, e->key, 1);
    }
}

/* ---- clock / eviction ----------------------------------------------------- */
static int age_ready(GuestMemory *m, GuestSlab *s) {
    if (s->age) return 0;
    if (s->pages > m->limit - m->used) return -1;
    s->age = (uint8_t *)calloc(s->pages, 1);
    if (!s->age) return -1;
    m->slab_meta += s->pages;
    m->used += s->pages;
    return 0;
}

void guest_swap_touch(GuestMemory *m, GuestSlab *s, uint32_t page) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on || !s || page >= s->pages) return;
    if (age_ready(m, s)) return;
    s->age[page] = 1;
}

/* Called by page_back() when an anonymous page is backed or evicted again. */
void guest_swap_note_resident(GuestMemory *m, int delta) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on) return;
    if ((delta < 0 && w->resident_pages == 0)) return;
    w->resident_pages = (uint32_t)((int)w->resident_pages + delta);
}

void guest_swap_note_nomem(GuestMemory *m) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (w && w->on) ++w->st.fail_no_memory;
}

void guest_swap_safe_point(GuestMemory *m) {
    struct GuestSwap *w = m && m->swap ? (struct GuestSwap *)m->swap : NULL;
    if (!w || !w->on) return;
    if (!w->target_pages || w->resident_pages <= w->target_pages) return;
    uint32_t scanned = 0, incompressible = 0;
    while (w->resident_pages > w->target_pages && scanned < SWAP_SCAN_PER_TICK) {
        unsigned di;
        GuestSlab *s = NULL;
        uint32_t p = 0;
        /* Round-robin over anonymous slabs.  File-backed slabs are excluded:
         * their PE provider already owns eviction and a copy would double the
         * work without saving anything. */
        for (di = 0; di < GUEST_MAX_SLABS; ++di) {
            unsigned i = (w->cursor_slab + di) % GUEST_MAX_SLABS;
            GuestSlab *c = &m->slabs[i];
            if (!c->state || c->fill || !c->pages) continue;
            s = c;
            p = w->cursor_page % c->pages;
            break;
        }
        if (!s) break;
        w->cursor_page = (p + 1u) % s->pages;
        if (w->cursor_page == 0) w->cursor_slab = (w->cursor_slab + 1u) % GUEST_MAX_SLABS;
        ++scanned;
        if (!s->state[p] || !s->page || !s->page[p]) continue;
        if (s->pinned && s->pinned[p]) { ++w->st.fail_unsafe; continue; }
        if (age_ready(m, s) == 0 && s->age) {
            if (s->age[p]) { s->age[p] = 0; continue; } /* second chance */
        }
        int rc = guest_swap_store(m, s, p, s->page[p]);
        if (rc == -2) {
            if (++incompressible > 64u) break;
            ++w->st.fail_incompressible;
            if (s->age) s->age[p] = 0;
            continue;
        }
        if (rc == -3) { ++w->st.fail_budget; break; }
        if (rc < 0) { ++w->st.fail_no_memory; break; }
        if (guest_slab_evict_page(m, s, p)) {
            guest_swap_discard(m, s, p);
            ++w->st.fail_unsafe;
            continue;
        }
        ++w->st.pages_out;
        w->st.bytes_out += SWAP_PAGE;
        w->st.compressed_total += (uint32_t)rc;
    }
}

void guest_swap_stats(const GuestMemory *m, GuestSwapStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m || !m->swap) return;
    const struct GuestSwap *w = (const struct GuestSwap *)m->swap;
    *out = w->st;
    out->compressed_live = w->stored;
    out->resident_pages = w->resident_pages;
}

void guest_swap_report(FILE *f, const GuestMemory *m) {
    if (!f) return;
    GuestSwapStats st;
    guest_swap_stats(m, &st);
    unsigned long long ratio = st.compressed_total
        ? (unsigned long long)((st.bytes_out * 100ull) / st.compressed_total) : 0;
    fprintf(f, "[swap] enabled=%d out=%llu in=%llu live=%llu/%llu ratio=%llu.%02llu "
               "resident=%llu target=%llu dropped=%llu fail(nomem=%llu budget=%llu "
               "incompressible=%llu unsafe=%llu reissued=%llu)\n",
            st.enabled, (unsigned long long)st.pages_out,
            (unsigned long long)st.pages_in,
            (unsigned long long)st.compressed_live,
            (unsigned long long)st.budget_bytes, ratio / 100ull, ratio % 100ull,
            (unsigned long long)st.resident_pages,
            (unsigned long long)st.target_pages,
            (unsigned long long)st.pages_dropped,
            (unsigned long long)st.fail_no_memory,
            (unsigned long long)st.fail_budget,
            (unsigned long long)st.fail_incompressible,
            (unsigned long long)st.fail_unsafe,
            (unsigned long long)st.fail_reissued);
}
