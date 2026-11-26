#pragma once

#include "guest.h"

/* Measurement-only bypass for the exact retail TESV SM3 gate.  This never
 * changes D3D9 caps, shader version words, or the computed level global. */
int tesv_sm3_gate_probe(GuestMemory *m);

/* Predecoded AOT/IR does not re-fetch patched instruction bytes.  While the
 * measurement patch is active, the first computed-level store is absorbed by a
 * private sink and subsequent predecoded reads see invariant level 7.  The
 * guest data word itself is never changed.  NULL when the probe is off. */
void *tesv_sm3_probe_level_ptr(uint32_t address, size_t bytes);
