/* Link against fixture_*.c emitted by the staged lift.py. */
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"
#include <stdio.h>

extern const FnEntry g_fn_table_fixture[];
extern const unsigned g_fn_count_fixture;
int n3ds_lift_self_test(void) {
    static GuestMemory m;
    guest_init(&m,0x40000);
    if(guest_map(&m,0x10000000,4096) || guest_map(&m,0x21000000,4096) ||
       n3ds_lift_bind(&m,g_fn_table_fixture,g_fn_count_fixture))return -1;
    CPU c;uint32_t r;uint32_t args[]={21,7};int rc=-1;
    n3ds_lift_init_cpu(&c,0x10001000,0);
    if(n3ds_lift_invoke(&c,0x401000,args,2,&r) || r!=28)goto done;
    args[0]=0x21000001;M32(args[0])=10;
    if(n3ds_lift_invoke(&c,0x401010,args,1,&r) || r!=15 || M32(args[0])!=15)goto done;
    if(win32_self_test(&c,&m))goto done;
    rc=0;
done:
    guest_destroy(&m);return rc;
}
#ifndef __3DS__
int main(void) {int rc=n3ds_lift_self_test();puts(rc?"Generated lift test FAILED":"Generated lift test passed");return rc?1:0;}
#endif
