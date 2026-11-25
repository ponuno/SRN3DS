#pragma once
#include <stddef.h>
#include <stdint.h>

/* Per-function execution profiling for lifted code.
 *
 * The instrumenter inserts RT_PROF(0xADDR) at each lifted function entry.
 * Counters live in a fixed open-addressed table: no allocation, no ordering
 * assumptions, and a disabled profiler costs one predictable branch. This is a
 * diagnostic measurement, not part of a playable build. */

void rt_prof_enable(int on);
int rt_prof_enabled(void);
void rt_prof_hit(uint32_t addr);
void rt_prof_reset(void);

/* Number of distinct functions seen. */
size_t rt_prof_distinct(void);
/* Total calls across all functions. */
uint64_t rt_prof_total(void);
/* Iterate occupied slots; returns 0 at the end. */
int rt_prof_slot(size_t index, uint32_t *addr, uint32_t *hits);
/* Find the hit count for one address (0 when never entered). */
uint32_t rt_prof_hits(uint32_t addr);
/* Write "addr,hits" sorted by hits descending. Returns rows written, or -1. */
int rt_prof_write_csv(const char *path);
/* Print the busiest functions to stdout. */
void rt_prof_print_top(unsigned n);

#define RT_PROF(addr) do { if (rt_prof_on_hint) rt_prof_hit((uint32_t)(addr)); } while (0)
extern int rt_prof_on_hint;
