#include "tesv_cube_probe.h"

#include "n3ds_lift_runtime.h"

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
