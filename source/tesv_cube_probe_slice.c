#include "tesv_cube_probe.h"

#include "n3ds_lift_runtime.h"

#include <stdio.h>
#include <stdlib.h>

/* AOT/IR shadow for the guest byte patch.  The generated slice calls this
 * wrapper at the three lifted entries of the 0x00c91650 site (return address
 * 0x00c91655).  While the measurement probe is active it returns NULL in eax
 * instead of entering NiRenderedCubeMap::Create; the generated code then takes
 * its existing `test eax,eax` path to 0x00c916d1 and stores a NULL group.
 * The real function is untouched and is called for every other case. */
void f_FB5C20(CPU *c);

void tesv_cube_probe_fb5c20(CPU *c) {
    if (tesv_cube_skip_active() && c && c->r[4] &&
        n3ds_guest_ptr(c->r[4], 4) && M32(c->r[4]) == 0x00c91655u) {
        c->r[0] = 0; /* eax = NULL; the caller's checked-NULL branch runs */
        return;
    }
    f_FB5C20(c);
}

/* Conditional AOT/IR shadows for the two NiRenderTargetGroup buffer getters.
 *
 * The measurement probe points vtable slots 33/34 (0x011b0220 -> 0x00fb6070,
 * 0x011b0224 -> 0x00fb6090) at the synthetic host addresses 0x30000000 and
 * 0x30000010 while `TESV_CUBE_PROBE_VTABLE=1` is set.  Those targets land
 * here.  The stub is deliberately *not* a blanket NULL: for a valid group and
 * a readable buffer slot it runs the real container getter, so early renderer
 * queries keep their real width/height.  Only sentinel slots (-1, 0, small
 * integers) or an unreadable receiver return the checked NULL, emulating the
 * callee's `ret 4`.  Measurement only. */
int n3ds_rt_dispatch(CPU *c, uint32_t addr);
static int cube_nested;

static uint32_t cube_probe_real(CPU *c, uint32_t addr, int *ok) {
    if (!c) {
        if (ok) *ok = 0;
        return 0;
    }
    cube_nested = 1;
    int rc = n3ds_rt_dispatch(c, addr);
    cube_nested = 0;
    if (ok) *ok = rc == 0;
    return rc == 0 ? c->r[0] : 0u;
}

int tesv_cube_probe_getbuffer(CPU *c, uint32_t real_addr) {
    if (!tesv_cube_skip_active()) return 0;

    GuestMemory *m = tesv_cube_probe_memory();
    int state = 0;
    uint32_t arg = 0;

    if (m && c && !guest_read32(m, c->r[4] + 4u, &arg))
        state = tesv_cube_probe_slot_state(m, c->r[1], arg, NULL);

    if (state == 1) {
        int ok = 0;
        uint32_t value = cube_probe_real(c, real_addr, &ok);
        if (ok) {
            /* The real getter executed its own `ret 4`, so the frame is gone. */
            c->r[0] = value;
            return 1;
        }
    }

    if (getenv("TESV_CUBE_PROBE_DEBUG")) {
        uint32_t slot = 0;
        (void)tesv_cube_probe_slot_state(m, c ? c->r[1] : 0, arg, &slot);
        fprintf(stderr, "[cube-probe] getter 0x%08lx this=0x%08lx arg=%lu "
                        "state=%d slot=0x%08lx -> eax=0; measurement only\n",
                (unsigned long)real_addr, (unsigned long)(c ? c->r[1] : 0),
                (unsigned long)arg, state, (unsigned long)slot);
    }
    if (c) {
        c->r[0] = 0;
        c->r[4] += 8u; /* callee `ret 4`: return address + one argument */
    }
    return 1;
}

/* AOT/IR dispatch shadow for the direct (non-vtable-stub) path.  When the
 * guest calls 0x00fb6090/0x00fb6070 directly this runs the real container
 * getter only for a valid readable slot and otherwise returns the checked
 * NULL.  It is a no-op for every other target and while a stub-initiated real
 * call is in flight (cube_nested). */
int tesv_cube_probe_dispatch(CPU *c, uint32_t addr) {
    if (!tesv_cube_skip_active()) return 0;
    if (addr != 0x00fb6090u && addr != 0x00fb6070u) return 0;
    if (cube_nested) return 0; /* let the normal container/AOT tier execute */

    GuestMemory *m = tesv_cube_probe_memory();
    uint32_t arg = 0;
    int state = 0;
    if (m && c && !guest_read32(m, c->r[4] + 4u, &arg))
        state = tesv_cube_probe_slot_state(m, c->r[1], arg, NULL);

    if (state == 1) {
        int ok = 0;
        uint32_t value = cube_probe_real(c, addr, &ok);
        if (ok) {
            c->r[0] = value;
            return 1;
        }
    }

    if (getenv("TESV_CUBE_PROBE_DEBUG"))
        fprintf(stderr, "[cube-probe] dispatch 0x%08lx this=0x%08lx arg=%lu "
                        "state=%d -> eax=0; measurement only\n",
                (unsigned long)addr, (unsigned long)(c ? c->r[1] : 0),
                (unsigned long)arg, state);
    if (c) {
        c->r[0] = 0;
        c->r[4] += 8u;
    }
    return 1;
}
