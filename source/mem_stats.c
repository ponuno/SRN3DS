#include "mem_stats.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <malloc.h>
#endif

typedef struct {
    char name[16];
    uint64_t bytes;
} MemStatsArena;

typedef struct {
    int enabled;
    unsigned heap_mib, linear_mib, reserve_mib;
    size_t guest_budget;
    unsigned every;
    unsigned frame;
    unsigned snapshots;

    size_t guest_used, guest_peak, guest_limit;
    unsigned guest_regions;

    int have_newlib;
    size_t newlib_used, newlib_peak, newlib_free;
    /* newlib heap shape: mallinfo() arena/uordblks/fordblks/keepcost/ordblks and
     * the largest single allocation the heap accepts right now (probed). */
    int have_newlib_shape;
    uint64_t newlib_arena, newlib_free_chunks, newlib_keepcost;
    uint64_t newlib_largest, newlib_probe_budget;

    MemStatsSpaceProbes probes;
    MemStatsRefresh refresh;
    void *refresh_ctx;

    /* linear = 0, vram = 1 */
    int have_space[2];
    uint64_t space_free[2];
    uint64_t space_min[2];
    uint64_t space_live[2];
    uint64_t space_peak[2];
    uint64_t fail_requests[2];
    uint64_t fail_bytes[2];
    uint64_t alloc_failures;

    MemStatsArena arena[MEM_STATS_MAX_ARENAS];
    unsigned arena_count;
    uint64_t arena_bytes;

    MemStatsAlloc alloc;

    uint64_t irc_bytes, irc_records;
    uint64_t irc_index_bytes, irc_cache_bytes;
    uint64_t irdec_bytes, irdec_blocks;
    uint64_t irdec_arena, irdec_slots, irdec_retired, irdec_budget;
    uint64_t bsa_resident;
    uint64_t bsa_index, bsa_page_cache, bsa_range_cache;
    unsigned bsa_archives, bsa_mounted;
    int have_bsa;

    unsigned stream_queued;
    uint64_t stream_staged;
    uint64_t stream_reads, stream_bytes, stream_misses;
    uint64_t stream_demand_hits, stream_demand_bytes;
    uint64_t stream_prefetch_demand_hits;
    uint64_t stream_cancelled, stream_budget_denied;

    size_t tex_guest, tex_gpu, tex_guest_peak, tex_gpu_peak;
    size_t tex_guest_limit, tex_gpu_limit;
    unsigned tex_trims, tex_trim_evictions, tex_evictions_gpu;
    unsigned tex_non_evictable, tex_pinned, tex_pinned_refusals;

    int have_os_region;
    uint64_t os_size, os_used, os_free;

    MemStatsSwap swap;

    FILE *log;
    char log_path[256];
    int in_snapshot;
} MemStats;

static MemStats g;

static uint64_t sample_free(int space)
{
    if (!g.probes.ctx && !g.probes.linear_free && !g.probes.vram_free)
        return 0;
    if (space == MEM_STATS_LINEAR)
        return g.probes.linear_free ? g.probes.linear_free(g.probes.ctx) : 0;
    return g.probes.vram_free ? g.probes.vram_free(g.probes.ctx) : 0;
}

static int have_probe(int space)
{
    return space == MEM_STATS_LINEAR ? g.probes.linear_free != NULL
                                     : g.probes.vram_free != NULL;
}

static void space_sample(int space)
{
    if (!have_probe(space)) return;
    uint64_t now = sample_free(space);
    g.space_free[space] = now;
    if (!g.have_space[space] || now < g.space_min[space])
        g.space_min[space] = now;
    g.have_space[space] = 1;
}

int mem_stats_profile_valid(unsigned heap_mib, unsigned linear_mib)
{
    if (!heap_mib || !linear_mib) return 0;
    return (uint64_t)heap_mib + linear_mib + MEM_STATS_NON_HEAP_MIB <=
           MEM_STATS_REGION_MIB;
}

size_t mem_stats_guest_budget(unsigned heap_mib, unsigned reserve_mib)
{
    if (heap_mib <= reserve_mib) return 0;
    return (size_t)(heap_mib - reserve_mib) << 20;
}

void mem_stats_init(void)
{
    memset(&g, 0, sizeof g);
    g.heap_mib = MEM_STATS_DEFAULT_HEAP_MIB;
    g.linear_mib = MEM_STATS_DEFAULT_LINEAR_MIB;
    g.reserve_mib = MEM_STATS_DEFAULT_RESERVE_MIB;
    g.guest_budget = mem_stats_guest_budget(g.heap_mib, g.reserve_mib);
    g.every = MEM_STATS_DEFAULT_EVERY;
}

void mem_stats_configure(unsigned heap_mib, unsigned linear_mib,
                         unsigned reserve_mib)
{
    mem_stats_init();
    g.heap_mib = heap_mib;
    g.linear_mib = linear_mib;
    g.reserve_mib = reserve_mib;
    g.guest_budget = mem_stats_guest_budget(heap_mib, reserve_mib);
    g.enabled = 1;
}

int mem_stats_enabled(void) { return g.enabled; }

void mem_stats_set_log_path(const char *path)
{
    if (g.log) { fclose(g.log); g.log = NULL; }
    g.log_path[0] = '\0';
    if (!path || !*path) return;
    snprintf(g.log_path, sizeof g.log_path, "%s", path);
    g.log = fopen(g.log_path, "w");
}

void mem_stats_set_every(unsigned frames) { g.every = frames; }
unsigned mem_stats_every(void) { return g.every; }

void mem_stats_set_space_probes(const MemStatsSpaceProbes *probes)
{
    if (probes) g.probes = *probes;
    else memset(&g.probes, 0, sizeof g.probes);
    for (int i = 0; i < 2; ++i) {
        g.have_space[i] = 0;
        g.space_min[i] = 0;
        space_sample(i);
    }
}

void mem_stats_set_refresh(MemStatsRefresh fn, void *ctx)
{
    g.refresh = fn;
    g.refresh_ctx = ctx;
}

void mem_stats_set_guest(size_t used, size_t limit, unsigned regions)
{
    g.guest_used = used;
    g.guest_limit = limit;
    g.guest_regions = regions;
    if (used > g.guest_peak) g.guest_peak = used;
}

void mem_stats_sample_guest(size_t used, unsigned regions)
{
    g.guest_used = used;
    g.guest_regions = regions;
    if (used > g.guest_peak) g.guest_peak = used;
}

void mem_stats_set_alloc(const MemStatsAlloc *stats)
{
    if (!stats) {
        memset(&g.alloc, 0, sizeof g.alloc);
        return;
    }
    g.alloc = *stats;
}

void mem_stats_set_swap(const MemStatsSwap *stats)
{
    if (!stats) {
        memset(&g.swap, 0, sizeof g.swap);
        return;
    }
    g.swap = *stats;
}

void mem_stats_set_newlib(size_t used, size_t free_bytes)
{
    g.have_newlib = 1;
    g.newlib_used = used;
    g.newlib_free = free_bytes;
    if (used > g.newlib_peak) g.newlib_peak = used;
}

void mem_stats_set_newlib_shape(uint64_t arena, uint64_t free_chunks,
                                uint64_t keepcost, uint64_t largest,
                                uint64_t probe_budget)
{
    g.have_newlib_shape = 1;
    g.newlib_arena = arena;
    g.newlib_free_chunks = free_chunks;
    g.newlib_keepcost = keepcost;
    g.newlib_largest = largest;
    g.newlib_probe_budget = probe_budget;
}

void mem_stats_note_arena(const char *name, size_t bytes)
{
    if (!name || !*name) return;
    for (unsigned i = 0; i < g.arena_count; ++i) {
        if (!strcmp(g.arena[i].name, name)) {
            g.arena[i].bytes += bytes;
            g.arena_bytes += bytes;
            return;
        }
    }
    if (g.arena_count >= MEM_STATS_MAX_ARENAS) return;
    MemStatsArena *a = &g.arena[g.arena_count++];
    snprintf(a->name, sizeof a->name, "%s", name);
    a->bytes = bytes;
    g.arena_bytes += bytes;
}

void mem_stats_set_irc(uint64_t bytes, uint64_t records)
{
    g.irc_bytes = bytes;
    g.irc_records = records;
    g.irc_index_bytes = 0;
    g.irc_cache_bytes = bytes;
}

void mem_stats_set_irc_detail(uint64_t index_bytes, uint64_t cache_bytes,
                              uint64_t cache_records)
{
    g.irc_index_bytes = index_bytes;
    g.irc_cache_bytes = cache_bytes;
    g.irc_bytes = index_bytes + cache_bytes;
    g.irc_records = cache_records;
}

void mem_stats_set_irdecode(uint64_t bytes, uint64_t blocks)
{
    g.irdec_bytes = bytes;
    g.irdec_blocks = blocks;
}

void mem_stats_set_irdecode_detail(uint64_t live_bytes, uint64_t blocks,
                                   uint64_t arena_bytes, uint64_t slot_bytes,
                                   uint64_t retired_bytes, uint64_t budget_bytes)
{
    g.irdec_bytes = live_bytes;
    g.irdec_blocks = blocks;
    g.irdec_arena = arena_bytes;
    g.irdec_slots = slot_bytes;
    g.irdec_retired = retired_bytes;
    g.irdec_budget = budget_bytes;
}

void mem_stats_set_bsa(uint64_t resident_bytes, unsigned archives) {
    mem_stats_set_bsa_detail(resident_bytes, 0, 0, 0, archives,
                              archives ? 1u : 0u);
}

void mem_stats_set_bsa_detail(uint64_t resident_bytes, uint64_t index_bytes,
                              uint64_t page_cache_bytes,
                              uint64_t range_cache_bytes, unsigned archives,
                              unsigned mounted) {
    g.bsa_resident = resident_bytes;
    g.bsa_index = index_bytes;
    g.bsa_page_cache = page_cache_bytes;
    g.bsa_range_cache = range_cache_bytes;
    g.bsa_archives = archives;
    g.bsa_mounted = mounted;
    g.have_bsa = mounted > 0;
}

void mem_stats_set_stream(unsigned queued, uint64_t staged_bytes,
                          uint64_t physical_reads, uint64_t physical_bytes,
                          uint64_t misses, uint64_t demand_hits,
                          uint64_t demand_bytes, uint64_t prefetch_demand_hits,
                          uint64_t cancelled, uint64_t budget_denied) {
    g.stream_queued = queued;
    g.stream_staged = staged_bytes;
    g.stream_reads = physical_reads;
    g.stream_bytes = physical_bytes;
    g.stream_misses = misses;
    g.stream_demand_hits = demand_hits;
    g.stream_demand_bytes = demand_bytes;
    g.stream_prefetch_demand_hits = prefetch_demand_hits;
    g.stream_cancelled = cancelled;
    g.stream_budget_denied = budget_denied;
}

void mem_stats_set_os_region(uint64_t size, uint64_t used, uint64_t free_bytes)
{
    g.have_os_region = 1;
    g.os_size = size;
    g.os_used = used;
    g.os_free = free_bytes;
}

/* Replace the VRAM space counters with the backend's authoritative global
 * accounting.  The incremental space_end/released path double-counts a
 * suspend/resume or a release/re-create pair, so snapshots must not use it. */
void mem_stats_set_vram(size_t live, size_t peak, size_t limit)
{
    if (peak < live) peak = live;
    g.space_live[MEM_STATS_VRAM] = live;
    g.space_peak[MEM_STATS_VRAM] = peak;
    g.space_free[MEM_STATS_VRAM] = limit > live ? limit - live : 0;
    g.space_min[MEM_STATS_VRAM] = g.space_free[MEM_STATS_VRAM];
    g.have_space[MEM_STATS_VRAM] = 1;
}

void mem_stats_set_texcache(size_t guest_bytes, size_t gpu_bytes,
                            size_t guest_peak, size_t gpu_peak,
                            unsigned budget_trims,
                            unsigned budget_trim_evictions,
                            unsigned evictions_gpu)
{
    g.tex_guest = guest_bytes;
    g.tex_gpu = gpu_bytes;
    g.tex_guest_peak = guest_peak;
    g.tex_gpu_peak = gpu_peak;
    g.tex_trims = budget_trims;
    g.tex_trim_evictions = budget_trim_evictions;
    g.tex_evictions_gpu = evictions_gpu;
}

void mem_stats_set_tex_limits(size_t guest_limit, size_t gpu_limit)
{
    g.tex_guest_limit = guest_limit;
    g.tex_gpu_limit = gpu_limit;
}

void mem_stats_set_tex_pins(unsigned non_evictable, unsigned pinned,
                            unsigned pinned_refusals)
{
    g.tex_non_evictable = non_evictable;
    g.tex_pinned = pinned;
    g.tex_pinned_refusals = pinned_refusals;
}

uint64_t mem_stats_space_begin(int space)
{
    if (space != MEM_STATS_LINEAR && space != MEM_STATS_VRAM) return 0;
    space_sample(space);
    return g.have_space[space] ? g.space_free[space] : 0;
}

void mem_stats_space_end(int space, uint64_t free_before, size_t requested,
                         int ok)
{
    if (space != MEM_STATS_LINEAR && space != MEM_STATS_VRAM) return;
    uint64_t after = free_before;
    if (have_probe(space)) {
        after = sample_free(space);
        g.space_free[space] = after;
        if (!g.have_space[space] || after < g.space_min[space])
            g.space_min[space] = after;
        g.have_space[space] = 1;
    }
    if (ok) {
        uint64_t observed = free_before > after ? free_before - after : 0;
        uint64_t add = observed ? observed : (uint64_t)requested;
        g.space_live[space] += add;
        if (g.space_live[space] > g.space_peak[space])
            g.space_peak[space] = g.space_live[space];
        return;
    }
    uint64_t bytes = requested ? (uint64_t)requested
                               : (free_before > after ? free_before - after : 0);
    ++g.fail_requests[space];
    g.fail_bytes[space] += bytes;
    ++g.alloc_failures;
    if (g.enabled && !g.in_snapshot) {
        char extra[96];
        snprintf(extra, sizeof extra, "fail_kind=%s fail_bytes=%llu",
                 space == MEM_STATS_LINEAR ? "linear" : "vram",
                 (unsigned long long)bytes);
        mem_stats_snapshot("alloc_fail", extra);
    }
}

void mem_stats_space_released(int space, uint64_t free_before)
{
    if (space != MEM_STATS_LINEAR && space != MEM_STATS_VRAM) return;
    uint64_t after = free_before;
    if (have_probe(space)) {
        after = sample_free(space);
        g.space_free[space] = after;
        if (!g.have_space[space] || after < g.space_min[space])
            g.space_min[space] = after;
        g.have_space[space] = 1;
    }
    uint64_t freed = after > free_before ? after - free_before : 0;
    g.space_live[space] = freed > g.space_live[space] ? 0
                                                      : g.space_live[space] - freed;
}

uint64_t mem_stats_live_bytes(int space)
{
    if (space != MEM_STATS_LINEAR && space != MEM_STATS_VRAM) return 0;
    return g.space_live[space];
}

size_t mem_stats_parse_limit(const char *text, int *ok)
{
    if (ok) *ok = 0;
    if (!text || !*text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text) return 0;
    if (*text == '-' || *text == '+') return 0;
    uint64_t value = 0;
    const char *p = text;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p - '0');
        if (value > (UINT64_MAX - d) / 10u) return 0;
        value = value * 10u + d;
        ++p;
        ++digits;
    }
    if (!digits) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    uint64_t scale = 1;
    if (*p) {
        char suffix = *p++;
        if (suffix == 'k' || suffix == 'K') scale = 1024ull;
        else if (suffix == 'm' || suffix == 'M') scale = 1024ull * 1024ull;
        else if (suffix == 'g' || suffix == 'G') scale = 1024ull * 1024ull * 1024ull;
        else return 0;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p) return 0;
    }
    if (value > UINT64_MAX / scale) return 0;
    value *= scale;
    if (value > (uint64_t)SIZE_MAX) return 0;
    if (ok) *ok = 1;
    return (size_t)value;
}

/* Largest single allocation the heap accepts right now.
 *
 * mallinfo() has no "largest free chunk" field, so this probes: a bounded
 * binary search on malloc(size)/free(size). Each probe splits the best-fitting
 * chunk and returns it immediately, so the free list is unchanged afterwards
 * (the two halves coalesce again). Only run when a snapshot is being written
 * (300 frames by default) or when N3DS_MEM_PROBE_ALLOC=0 disables it. */
#ifndef MEM_STATS_LARGEST_STEP
#define MEM_STATS_LARGEST_STEP 4096u
#endif

static size_t largest_free_probe(size_t upper)
{
    if (!upper) return 0;
    size_t lo = 0, hi = upper;
    while (hi - lo > MEM_STATS_LARGEST_STEP) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = malloc(mid);
        if (p) { lo = mid; free(p); }
        else hi = mid;
    }
    return lo;
}

static int probe_alloc_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("N3DS_MEM_PROBE_ALLOC");
        cached = !(s && *s == '0' && !s[1]);
    }
    return cached;
}

static void refresh_newlib(void)
{
#ifdef __3DS__
    struct mallinfo mi = mallinfo();
    mem_stats_set_newlib((size_t)mi.uordblks, (size_t)mi.fordblks);
    if (probe_alloc_enabled())
        mem_stats_set_newlib_shape((uint64_t)mi.arena, (uint64_t)mi.ordblks,
                                   (uint64_t)mi.keepcost,
                                   (uint64_t)largest_free_probe((size_t)mi.fordblks),
                                   (uint64_t)mi.fordblks);
    else
        mem_stats_set_newlib_shape((uint64_t)mi.arena, (uint64_t)mi.ordblks,
                                   (uint64_t)mi.keepcost, 0, (uint64_t)mi.fordblks);
#endif
}

size_t mem_stats_format(char *dst, size_t cap, const char *tag, const char *extra)
{
    if (!dst || !cap) return 0;
    size_t n = 0;
#define APPEND(...) do { \
        if (n < cap) { \
            int w = snprintf(dst + n, cap - n, __VA_ARGS__); \
            if (w > 0) n += (size_t)w; \
        } \
    } while (0)

    if (g.refresh && !g.in_snapshot) {
        g.in_snapshot = 1;
        g.refresh(g.refresh_ctx);
        g.in_snapshot = 0;
    }
    refresh_newlib();
    space_sample(MEM_STATS_LINEAR);
    space_sample(MEM_STATS_VRAM);

    APPEND("mem tag=%s frame=%u heap_mib=%u linear_mib=%u reserve_mib=%u "
           "guest_budget=%llu guest_used=%llu guest_peak=%llu guest_limit=%llu "
           "guest_regions=%u",
           tag ? tag : "snap", g.frame, g.heap_mib, g.linear_mib,
           g.reserve_mib, (unsigned long long)g.guest_budget,
           (unsigned long long)g.guest_used, (unsigned long long)g.guest_peak,
           (unsigned long long)g.guest_limit, g.guest_regions);
    APPEND(" newlib_used=%llu newlib_peak=%llu newlib_free=%llu have_newlib=%d",
           (unsigned long long)g.newlib_used, (unsigned long long)g.newlib_peak,
           (unsigned long long)g.newlib_free, g.have_newlib);
    APPEND(" newlib_arena=%llu newlib_free_chunks=%llu newlib_keepcost=%llu "
           "newlib_largest=%llu newlib_probe_budget=%llu have_newlib_shape=%d",
           (unsigned long long)g.newlib_arena,
           (unsigned long long)g.newlib_free_chunks,
           (unsigned long long)g.newlib_keepcost,
           (unsigned long long)g.newlib_largest,
           (unsigned long long)g.newlib_probe_budget,
           g.have_newlib_shape);
    APPEND(" linear_live=%llu linear_peak=%llu linear_free=%llu "
           "linear_min_free=%llu linear_fail_n=%llu linear_fail_bytes=%llu "
           "linear_probe=%d vram_probe=%d",
           (unsigned long long)g.space_live[MEM_STATS_LINEAR],
           (unsigned long long)g.space_peak[MEM_STATS_LINEAR],
           (unsigned long long)g.space_free[MEM_STATS_LINEAR],
           (unsigned long long)(g.have_space[MEM_STATS_LINEAR]
                                    ? g.space_min[MEM_STATS_LINEAR] : 0),
           (unsigned long long)g.fail_requests[MEM_STATS_LINEAR],
           (unsigned long long)g.fail_bytes[MEM_STATS_LINEAR],
           g.probes.linear_free != NULL, g.probes.vram_free != NULL);
    APPEND(" vram_live=%llu vram_peak=%llu vram_free=%llu vram_min_free=%llu "
           "vram_fail_n=%llu vram_fail_bytes=%llu",
           (unsigned long long)g.space_live[MEM_STATS_VRAM],
           (unsigned long long)g.space_peak[MEM_STATS_VRAM],
           (unsigned long long)g.space_free[MEM_STATS_VRAM],
           (unsigned long long)(g.have_space[MEM_STATS_VRAM]
                                    ? g.space_min[MEM_STATS_VRAM] : 0),
           (unsigned long long)g.fail_requests[MEM_STATS_VRAM],
           (unsigned long long)g.fail_bytes[MEM_STATS_VRAM]);
    APPEND(" arenas_bytes=%llu", (unsigned long long)g.arena_bytes);
    for (unsigned i = 0; i < g.arena_count; ++i)
        APPEND(" arena_%s=%llu", g.arena[i].name,
               (unsigned long long)g.arena[i].bytes);
    APPEND(" irc_bytes=%llu irc_index_bytes=%llu irc_cache_bytes=%llu "
           "irc_records=%llu irdec_bytes=%llu irdec_blocks=%llu "
           "irdec_arena_bytes=%llu irdec_slot_bytes=%llu "
           "irdec_retired_bytes=%llu irdec_budget=%llu "
           "bsa_resident=%llu bsa_index=%llu bsa_page_cache=%llu "
           "bsa_range_cache=%llu bsa_archives=%u bsa_mounted=%u have_bsa=%d",
           (unsigned long long)g.irc_bytes,
           (unsigned long long)g.irc_index_bytes,
           (unsigned long long)g.irc_cache_bytes,
           (unsigned long long)g.irc_records,
           (unsigned long long)g.irdec_bytes, (unsigned long long)g.irdec_blocks,
           (unsigned long long)g.irdec_arena,
           (unsigned long long)g.irdec_slots,
           (unsigned long long)g.irdec_retired,
           (unsigned long long)g.irdec_budget,
           (unsigned long long)g.bsa_resident,
           (unsigned long long)g.bsa_index,
           (unsigned long long)g.bsa_page_cache,
           (unsigned long long)g.bsa_range_cache, g.bsa_archives, g.bsa_mounted,
           g.have_bsa);
    APPEND(" stream_queued=%u stream_staged=%llu stream_reads=%llu "
           "stream_bytes=%llu stream_misses=%llu stream_demand_hits=%llu "
           "stream_demand_bytes=%llu stream_prefetch_demand_hits=%llu "
           "stream_cancelled=%llu stream_budget_denied=%llu",
           g.stream_queued, (unsigned long long)g.stream_staged,
           (unsigned long long)g.stream_reads,
           (unsigned long long)g.stream_bytes,
           (unsigned long long)g.stream_misses,
           (unsigned long long)g.stream_demand_hits,
           (unsigned long long)g.stream_demand_bytes,
           (unsigned long long)g.stream_prefetch_demand_hits,
           (unsigned long long)g.stream_cancelled,
           (unsigned long long)g.stream_budget_denied);
    APPEND(" tex_guest=%llu tex_gpu=%llu tex_guest_peak=%llu tex_gpu_peak=%llu "
           "tex_guest_limit=%llu tex_gpu_limit=%llu "
           "tex_trims=%u tex_trim_evictions=%u tex_evictions_gpu=%u "
           "tex_non_evictable=%u tex_pinned=%u tex_pinned_refusals=%u",
           (unsigned long long)g.tex_guest, (unsigned long long)g.tex_gpu,
           (unsigned long long)g.tex_guest_peak,
           (unsigned long long)g.tex_gpu_peak,
           (unsigned long long)g.tex_guest_limit,
           (unsigned long long)g.tex_gpu_limit, g.tex_trims,
           g.tex_trim_evictions, g.tex_evictions_gpu,
           g.tex_non_evictable, g.tex_pinned, g.tex_pinned_refusals);
    APPEND(" os_region_used=%llu os_region_free=%llu os_region_size=%llu "
           "have_os_region=%d alloc_failures=%llu",
           (unsigned long long)g.os_used, (unsigned long long)g.os_free,
           (unsigned long long)g.os_size, g.have_os_region,
           (unsigned long long)g.alloc_failures);
    APPEND(" alloc_calls=%llu alloc_failures_host=%llu alloc_peak_live=%llu "
           "heap_live=%llu crt_live=%llu va_live=%llu "
           "va_reserved_live=%llu va_committed_live=%llu va_committed_peak=%llu "
           "va_backed_live=%llu va_backed_peak=%llu va_reservations=%llu "
           "va_commit_failures=%llu guest_backing_failures=%llu",
           (unsigned long long)g.alloc.calls,
           (unsigned long long)g.alloc.failures,
           (unsigned long long)g.alloc.peak_live,
           (unsigned long long)g.alloc.heap_live,
           (unsigned long long)g.alloc.crt_live,
           (unsigned long long)g.alloc.va_live,
           (unsigned long long)g.alloc.va_reserved_live,
           (unsigned long long)g.alloc.va_committed_live,
           (unsigned long long)g.alloc.va_committed_peak,
           (unsigned long long)g.alloc.va_backed_live,
           (unsigned long long)g.alloc.va_backed_peak,
           (unsigned long long)g.alloc.va_reservations,
           (unsigned long long)g.alloc.va_commit_failures,
           (unsigned long long)g.alloc.backing_failures);
    APPEND(" swap=%d swap_out=%llu swap_in=%llu swap_resident=%llu "
           "swap_target=%llu swap_compressed=%llu swap_ratio_pct=%llu "
           "swap_fail_nomem=%llu swap_fail_budget=%llu "
           "swap_fail_incompressible=%llu",
           g.swap.enabled, (unsigned long long)g.swap.pages_out,
           (unsigned long long)g.swap.pages_in,
           (unsigned long long)g.swap.resident_pages,
           (unsigned long long)g.swap.target_pages,
           (unsigned long long)g.swap.compressed_live,
           (unsigned long long)(g.swap.compressed_total
               ? (g.swap.bytes_out * 100ull) / g.swap.compressed_total : 0),
           (unsigned long long)g.swap.fail_no_memory,
           (unsigned long long)g.swap.fail_budget,
           (unsigned long long)g.swap.fail_incompressible);
    if (extra && *extra) APPEND(" %s", extra);
    APPEND("\n");
#undef APPEND
    if (n >= cap) n = cap - 1; /* snprintf truncation: report what fits */
    return n;
}

void mem_stats_snapshot(const char *tag, const char *extra)
{
    if (!g.enabled) return;
    char line[3072];
    mem_stats_format(line, sizeof line, tag, extra);
    FILE *f = g.log ? g.log : stderr;
    fputs(line, f);
    fflush(f);
    ++g.snapshots;
}

void mem_stats_frame(void)
{
    if (!g.enabled) return;
    ++g.frame;
    if (g.every && g.frame % g.every == 0) {
        char extra[32];
        snprintf(extra, sizeof extra, "frame=%u", g.frame);
        mem_stats_snapshot("frame", extra);
    }
}

void mem_stats_note_cell(const char *name)
{
    if (!g.enabled) return;
    char safe[64];
    unsigned j = 0;
    for (unsigned i = 0; name && name[i] && j + 1 < sizeof safe; ++i) {
        char c = name[i];
        safe[j++] = (c == ' ' || c == '=') ? '_' : c;
    }
    safe[j] = '\0';
    char extra[96];
    snprintf(extra, sizeof extra, "cell=%s", j ? safe : "unknown");
    mem_stats_snapshot("cell", extra);
}
