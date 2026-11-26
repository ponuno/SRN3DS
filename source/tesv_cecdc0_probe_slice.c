#include "tesv_cecdc0_probe.h"

#include "n3ds_lift_runtime.h"

#include <stdio.h>

/* AOT/IR shadow for the guest byte patch above.  The generated f_CD4E40 body
 * calls f_CECDC0 directly, so the patched guest bytes are never re-fetched on
 * the lifted path.  The measurement build renames that one call site in the
 * generated slice to this wrapper.  It returns NULL and pops only the return
 * address; f_CD4E40's existing `add esp,8` removes the two arguments, and its
 * `test edi,edi` / `setne al` path returns false to f_AE7CD0.  Every other
 * f_CECDC0 caller runs the real function. */
void f_CECDC0(CPU *c);

void tesv_cecdc0_probe_shadow(CPU *c) {
    if (tesv_cecdc0_probe_active() && c && c->r[4] &&
        n3ds_guest_ptr(c->r[4], 4) && M32(c->r[4]) == 0x00cd4e68u) {
        c->r[0] = 0;   /* eax = NULL */
        c->r[4] += 4u; /* callee `ret`; caller's add esp,8 drops the arguments */
        return;
    }
    f_CECDC0(c);
}
