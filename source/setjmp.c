/* setjmp/longjmp for recompiled guest code, ported from
 * Skyrim_Android/runtime/setjmp.c and adapted to this project's IR runtime.
 *
 * Upstream's lifter inlines a host setjmp in the guest function that calls
 * _setjmp3.  Our IR interpreter cannot rewrite a caller frame, so the same
 * contract is provided at the guest level: _setjmp3 writes the MSVC jmp_buf
 * (ebp/ebx/edi/esi/esp/eip/SEH/cookie), associates the guest buffer with a host
 * jmp_buf slot, and returns 0.  longjmp restores the saved GPRs and asks the
 * runtime to unwind to the innermost invoke frame and resume guest execution at
 * the saved EIP/ESP.  No host frame that already returned is ever jumped into.
 *
 * The slot also remembers the IR function end active when _setjmp3 ran, so the
 * interpreter can resume mid-function without re-decoding it as an entry point.
 */
#include "n3ds_lift_runtime.h"
#include <setjmp.h>
#include <string.h>

#define SLOT_COUNT 64
typedef struct {
    uint32_t guest;
    uint32_t func_end;
    jmp_buf jb;
} Slot;
static __thread Slot slots[SLOT_COUNT];
static __thread unsigned next_slot;

jmp_buf *n3ds_setjmp_host_slot(uint32_t guest_jb) {
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        if (slots[i].guest == guest_jb) return &slots[i].jb;
    unsigned i = next_slot++ % SLOT_COUNT;
    slots[i].guest = guest_jb;
    slots[i].func_end = 0;
    return &slots[i].jb;
}

jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb) {
    (void)c;
    return n3ds_setjmp_host_slot(guest_jb);
}

void rt_setjmp3(CPU *c) {
    uint32_t esp = c->r[4];
    uint32_t jb = M32(esp + 4);
    if (!jb) rt_unimpl(c, esp, "setjmp3 with NULL jmp_buf");
    M32(jb + 0) = c->r[5];           /* ebp */
    M32(jb + 4) = c->r[3];           /* ebx */
    M32(jb + 8) = c->r[7];           /* edi */
    M32(jb + 12) = c->r[6];          /* esi */
    M32(jb + 16) = esp + 4;          /* esp after the return */
    M32(jb + 20) = M32(esp);         /* eip */
    M32(jb + 24) = c->fs_base ? M32(c->fs_base) : 0; /* SEH registration */
    M32(jb + 28) = 0xFFFFFFFFu;
    /* Remember the function range for the interpreter resume. */
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        if (slots[i].guest == jb) { slots[i].func_end = n3ds_lift_current_function_end(); break; }
    (void)n3ds_setjmp_host_slot(jb);
    c->r[4] = esp + 4;
    c->r[0] = 0;
}

void rt_longjmp(CPU *c, uint32_t jb, uint32_t val) {
    if (!jb) rt_unimpl(c, 0, "longjmp with NULL jmp_buf");
    Slot *slot = NULL;
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        if (slots[i].guest == jb) { slot = &slots[i]; break; }
    if (!slot) rt_unimpl(c, jb, "longjmp to an unknown jmp_buf (no _setjmp3)");
    c->r[5] = M32(jb + 0);
    c->r[3] = M32(jb + 4);
    c->r[7] = M32(jb + 8);
    c->r[6] = M32(jb + 12);
    c->r[4] = M32(jb + 16);
    c->r[0] = val ? val : 1u;
    uint32_t eip = M32(jb + 20);
    /* Unwind host frames and resume the guest at the saved EIP.  This never
     * jumps into a dead host C frame; the runtime restarts the IR loop. */
    n3ds_lift_guest_resume(c, eip, slot->func_end);
}
