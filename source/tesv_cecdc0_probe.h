#pragma once

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

/* Measurement-only skip for the NiDX9TextureBufferData creation at
 * 0x00cecdc0, applied only at the f_CD4E40 call site 0x00cd4e63.  That caller
 * copies EAX to EDI, tests EDI and returns `al = (eax != 0)`, so returning
 * NULL is a valid failure path.  The other direct caller at 0x00ce77f8
 * discards EAX and is deliberately not patched.  Host-slice only, not
 * shippable. */
int tesv_cecdc0_skip_probe(GuestMemory *m);
int tesv_cecdc0_probe_active(void);

/* True only for the return address of the f_CD4E40 call site; the IR/AOT
 * shadow uses this to leave every other f_CECDC0 caller untouched. */
int tesv_cecdc0_is_primary_return(uint32_t ret);

/* Test-only latch reset so the host test can exercise several independent
 * GuestMemory images in one process.  The probe itself never calls this. */
void tesv_cecdc0_probe_reset_for_test(void);
