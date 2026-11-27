/* Optional host-side cycle counters for the IR interpreter.
 *
 * Compiled out unless N3DS_HOST_PROF is defined. The counters are inclusive:
 * dispatch time contains container lookups, fetch time contains decode on a
 * cache miss, and so on. They exist to rank hotspots, not to be additive.
 */
#pragma once
#include <stdint.h>

#ifdef N3DS_HOST_PROF
enum {
    HP_EXEC = 0,      /* exec_block */
    HP_FETCH,         /* n3ds_ir_fetch_block / irc_fetch_block */
    HP_DISPATCH,      /* n3ds_rt_dispatch (call targets) */
    HP_LOOKUP,        /* n3ds_irc_lookup binary search */
    HP_IR_LOOKUP,     /* n3ds_ir_end (slice function table) */
    HP_GUEST_READ,    /* guest_read32 */
    HP_SYNC,          /* sync waits */
    HP_COUNT
};

extern uint64_t hprof_cycles[HP_COUNT];
extern uint64_t hprof_calls[HP_COUNT];

uint64_t hprof_rdtsc(void);
void hprof_reset(void);
void hprof_dump(void);

#define HP_START(var) uint64_t var = hprof_rdtsc()
#define HP_ADD(slot, var) do { \
        hprof_cycles[(slot)] += hprof_rdtsc() - (var); \
        hprof_calls[(slot)]++; \
    } while (0)
#define HP_CALL(slot) do { hprof_calls[(slot)]++; } while (0)
#else
#define HP_START(var) ((void)0)
#define HP_ADD(slot, var) ((void)0)
#define HP_CALL(slot) ((void)0)
#endif

#ifndef N3DS_HOST_PROF
static inline void hprof_reset(void) {}
static inline void hprof_dump(void) {}
#endif
