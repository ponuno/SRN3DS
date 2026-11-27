#pragma once
/* Portable memory accounting for the TESV slice.
 *
 * The 178 MiB device region has three independent heaps that fail for different
 * reasons: the newlib application heap (guest pages + host indices), the citro3d
 * linear heap (textures, render targets, frame arenas) and the 6 MiB PICA VRAM.
 * This module keeps one high-water mark per heap plus the size of every failed
 * allocation request, so a profile can be accepted or rejected on measured
 * peaks instead of an estimate.
 *
 * No dependency on libctru, citro3d or GuestMemory: callers install probe
 * callbacks.  That keeps the module linkable into the host tests, where a fake
 * allocator drives the same accounting.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_STATS_REGION_MIB 178u
/* Code, static data, stacks and every other non-heap cost in the 178 MiB
 * region.  The target profile is 120 + 24 + 34 = 178. */
#define MEM_STATS_NON_HEAP_MIB 34u
#define MEM_STATS_DEFAULT_HEAP_MIB 96u
#define MEM_STATS_DEFAULT_LINEAR_MIB 8u
#define MEM_STATS_DEFAULT_RESERVE_MIB 4u
#define MEM_STATS_DEFAULT_EVERY 300u
#define MEM_STATS_MAX_ARENAS 8u
#define MEM_STATS_PATH "sdmc:/Skyrim_N3DS_mem.log"

enum {
    MEM_STATS_LINEAR = 0,
    MEM_STATS_VRAM = 1,
};

typedef struct {
    /* Current free bytes in the linear heap / PICA VRAM.  NULL = unsupported. */
    uint64_t (*linear_free)(void *ctx);
    uint64_t (*vram_free)(void *ctx);
    void *ctx;
} MemStatsSpaceProbes;

/* Called before every formatted snapshot; the slice installs one that refreshes
 * guest, IR, texture-cache and OS-region counters.  May be NULL. */
typedef void (*MemStatsRefresh)(void *ctx);

/* ---- profile arithmetic --------------------------------------------------- */
/* 1 when heap + linear + non-heap fits the 178 MiB region. */
int mem_stats_profile_valid(unsigned heap_mib, unsigned linear_mib);
/* heap_mib - reserve_mib, floored at zero. */
size_t mem_stats_guest_budget(unsigned heap_mib, unsigned reserve_mib);

/* ---- configuration -------------------------------------------------------- */
void mem_stats_init(void);
/* Enables snapshots.  Calling it again resets every counter and peak. */
void mem_stats_configure(unsigned heap_mib, unsigned linear_mib,
                         unsigned reserve_mib);
int mem_stats_enabled(void);
void mem_stats_set_log_path(const char *path); /* NULL = stderr */
void mem_stats_set_every(unsigned frames);     /* 0 disables periodic dumps */
unsigned mem_stats_every(void);
void mem_stats_set_space_probes(const MemStatsSpaceProbes *probes);
void mem_stats_set_refresh(MemStatsRefresh fn, void *ctx);

/* ---- counters ------------------------------------------------------------- */
void mem_stats_set_guest(size_t used, size_t limit, unsigned regions);
void mem_stats_set_newlib(size_t used, size_t free_bytes);
void mem_stats_note_arena(const char *name, size_t bytes);
void mem_stats_set_irc(uint64_t bytes, uint64_t records);
void mem_stats_set_irdecode(uint64_t bytes, uint64_t blocks);
/* `resident_bytes` is measured (bsa_resident_bytes() summed over open archives),
 * never an estimate.  This compatibility overload takes `archives` as both the
 * configured and mounted count; prefer mem_stats_set_bsa_detail() when the
 * overlay reports how many sources are actually open. */
void mem_stats_set_bsa(uint64_t resident_bytes, unsigned archives);
/* Same, with the component breakdown from BsaResidentStats.  `mounted` counts
 * archives actually open; have_bsa = mounted > 0. */
void mem_stats_set_bsa_detail(uint64_t resident_bytes, uint64_t index_bytes,
                              uint64_t page_cache_bytes,
                              uint64_t range_cache_bytes, unsigned archives,
                              unsigned mounted);
/* Streaming counters (see StreamStatsSnapshot).  Flat arguments keep mem_stats
 * free of asset_stream.h; pass zeros when no mount exists. */
void mem_stats_set_stream(unsigned queued, uint64_t staged_bytes,
                          uint64_t physical_reads, uint64_t physical_bytes,
                          uint64_t misses, uint64_t demand_hits,
                          uint64_t demand_bytes, uint64_t prefetch_demand_hits,
                          uint64_t cancelled, uint64_t budget_denied);
void mem_stats_set_os_region(uint64_t size, uint64_t used, uint64_t free_bytes);
void mem_stats_set_texcache(size_t guest_bytes, size_t gpu_bytes,
                            size_t guest_peak, size_t gpu_peak,
                            unsigned budget_trims,
                            unsigned budget_trim_evictions,
                            unsigned evictions_gpu);
void mem_stats_set_tex_limits(size_t guest_limit, size_t gpu_limit);
/* Frame-scoped pin visibility: entries with pins, no_evict entries, and the
 * cumulative count of allocations refused because every candidate was pinned. */
void mem_stats_set_tex_pins(unsigned non_evictable, unsigned pinned,
                            unsigned pinned_refusals);

/* Wrap one allocation in a space.  `space_begin` returns the current free
 * counter; `space_end` takes the observed delta, updates the high-water mark and
 * records a failed request when ok == 0.  `space_released` samples a free.
 * All three are no-ops (except returning the last sample) when the probe for
 * that space is not installed. */
uint64_t mem_stats_space_begin(int space);
void mem_stats_space_end(int space, uint64_t free_before, size_t requested,
                         int ok);
void mem_stats_space_released(int space, uint64_t free_before);

/* Parses "0", "123", "512K", "4M", "1G" (case-insensitive).  *ok = 0 on an
 * empty string, sign, non-digit, unknown suffix or overflow.  Returns 0 and
 * *ok = 1 for "0" (no limit). */
size_t mem_stats_parse_limit(const char *text, int *ok);

/* ---- output --------------------------------------------------------------- */
/* Writes one key=value line into dst (always NUL-terminated).  `extra` is
 * appended verbatim after one space when non-NULL.  Returns the string length. */
size_t mem_stats_format(char *dst, size_t cap, const char *tag, const char *extra);
/* Formats and writes to the configured log (append, flushed). */
void mem_stats_snapshot(const char *tag, const char *extra);
/* Counts one rendered frame and emits tag=frame every mem_stats_every() frames. */
void mem_stats_frame(void);
/* A cell transition is a peak moment; call from the cell hook when one exists. */
void mem_stats_note_cell(const char *name);

/* Piece of the formatted line, exposed for the leak test. */
uint64_t mem_stats_live_bytes(int space);

#ifdef __cplusplus
}
#endif
