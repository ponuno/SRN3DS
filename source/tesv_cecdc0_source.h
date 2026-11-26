#pragma once

#include <stdint.h>

#include "guest.h"

/* Measurement-only fill for the uninitialised f_CECDC0 base local
 * [esp+0x18].  The retail block at 0x00ced041 does:
 *
 *     index = [[esp+0x54]+0x624]
 *     base  = [esp+0x18]              ; never written by f_CECDC0
 *     desc  = [base + index*4]
 *     [esp+0x14] = desc
 *
 * The real source object is [[esp+0x54]+0x604]; its +0x10 holds the same
 * index and [source+index*4] is the missing descriptor.  When the pending
 * slot is NULL and the source is valid, the hook points the base local at
 * that existing engine object; the original 68-byte copy is unchanged.
 * Host-slice only, not shippable. */
int tesv_cecdc0_source_probe(GuestMemory *m);
int tesv_cecdc0_source_active(void);

/* 1: fill with *new_base (the real source object); 0: slot already valid;
 * -1: no fill (bad frame/source/index or no real descriptor). */
int tesv_cecdc0_source_fill_slot(GuestMemory *m, uint32_t frame,
                                 uint32_t index, uint32_t *new_base);

/* 0: run the original base[index] lookup (optionally after a fill);
 * 1: skip the lookup and use *new_desc (the real ebx+0x14 buffer or the
 *    existing f_CE9E00 descriptor) in [esp+0x14]; -1: no safe change. */
int tesv_cecdc0_source_decide(GuestMemory *m, uint32_t frame, uint32_t index,
                              uint32_t ebx, uint32_t *new_base,
                              uint32_t *new_desc);

/* Test-only latch reset. */
void tesv_cecdc0_source_reset_for_test(void);
