#pragma once

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

/* Measurement-only skip for the guest water-reflection cube branch.  The probe
 * byte-verifies the exact retail sequence at 0x00c91540 and patches the
 * 0x00c91650 call so the guest takes its own checked-NULL path to 0x00c916d1
 * without calling 0x00fb5c20.  It never changes D3D9 caps, SM3 words, T&L or
 * any other renderer state, and it is not shippable. */
int tesv_cube_skip_probe(GuestMemory *m);
int tesv_cube_skip_active(void);
GuestMemory *tesv_cube_probe_memory(void);

/* Guest-memory shadow for the renderer default group's 0xffffffff buffer
 * sentinel.  The probe rewrites that one slot to a checked NULL and returns a
 * zero word for the same four-byte read.  NULL when the probe is off. */
void *tesv_cube_probe_ptr(uint32_t address, size_t bytes);

/* Classify one NiRenderTargetGroup buffer slot for the conditional getter
 * stubs.  `this_` is the group (vtables 0x011b019c/0x011539d8), `arg` is the
 * 0-based buffer index.  1: valid readable buffer pointer (the caller must run
 * the real container getter); -1: sentinel/absent slot (0, -1 or a small
 * integer); 0: bad `this`, out-of-range index or unreadable slot. */
int tesv_cube_probe_slot_state(GuestMemory *m, uint32_t this_, uint32_t arg,
                               uint32_t *slot_out);

/* Read one 32-bit field from a buffer pointer that the classifier accepted.
 * Mirrors the real getter's `[buffer+field]` read and returns 0 if the field
 * is unreadable.  Used by the host test and by debug logging. */
uint32_t tesv_cube_probe_slot_field(GuestMemory *m, uint32_t slot,
                                    uint32_t field, int *ok);
