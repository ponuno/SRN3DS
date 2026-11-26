#pragma once

#include "guest.h"

/* Measurement-only skip for the guest water-reflection cube branch.  The probe
 * byte-verifies the exact retail sequence at 0x00c91540 and patches the
 * 0x00c91650 call so the guest takes its own checked-NULL path to 0x00c916d1
 * without calling 0x00fb5c20.  It never changes D3D9 caps, SM3 words, T&L or
 * any other renderer state, and it is not shippable. */
int tesv_cube_skip_probe(GuestMemory *m);
int tesv_cube_skip_active(void);
