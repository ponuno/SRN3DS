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

/* AOT/IR dispatch shadow for NiRenderTargetGroup::GetBufferData (0x00fb6090).
 * The skipped cube group can leave the caller holding a NULL/bad group.  If
 * the receiver has a real group vtable, fix a 0xffffffff buffer slot to NULL
 * and let the IR execute normally; otherwise return eax=0 as a checked NULL
 * and emulate the callee's `ret 4`.  Measurement only. */
int n3ds_rt_dispatch(CPU *c, uint32_t addr);

int tesv_cube_probe_dispatch(CPU *c, uint32_t addr) {
    static int nested;
    if (!tesv_cube_skip_active() || addr != 0x00fb6090u) return 0;
    if (nested) return 0; /* let the normal container/AOT tier execute */

    GuestMemory *m = tesv_cube_probe_memory();
    uint32_t this_ = c ? c->r[1] : 0;
    uint32_t vtable = 0;
    int valid = m && this_ && !guest_read32(m, this_, &vtable) &&
                (vtable == 0x011b019cu || vtable == 0x011539d8u);

    if (valid) {
        uint32_t esp = c->r[4], arg = 0;
        if (!guest_read32(m, esp + 4u, &arg)) {
            uint32_t slot = 0;
            if (!guest_read32(m, this_ + arg * 4u + 0x0cu, &slot) &&
                slot == 0xffffffffu)
                (void)guest_write32(m, this_ + arg * 4u + 0x0cu, 0u);
        }
        nested = 1;
        (void)n3ds_rt_dispatch(c, 0x00fb6090u);
        nested = 0;
        return 1;
    }

    if (getenv("TESV_CUBE_PROBE_DEBUG"))
        fprintf(stderr, "[cube-probe] dispatch guard: bad this=0x%08lx "
                        "-> eax=0; measurement only\n",
                (unsigned long)this_);
    if (c) {
        c->r[0] = 0;
        c->r[4] += 8u; /* callee `ret 4`: return address + one argument */
    }
    return 1;
}
