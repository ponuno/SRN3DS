/* Uses real staged upstream CPU/FPU/Flags definitions. The C functions here
 * are handwritten in the same style as lift.py outputs, not generated. */
#include "n3ds_lift_runtime.h"
#include "lift_prelude.h"
#include "win32_shims.h"
#include <stdio.h>
#include <stdlib.h>

static void f_add(CPU *c) {
    Flags f={0};
    uint32_t esp=c->r[4];
    c->r[0]=add32(M32(esp+4),M32(esp+8),&f);c->r[4]=esp+4;
}
static void f_mem(CPU *c) {
    uint32_t addr=M32(c->r[4]+4);
    M32(addr)=10;
    uint32_t old=N3DS_FETCH_ADD(N3DS_PTR(uint32_t,addr),5,__ATOMIC_SEQ_CST);
    uint32_t expected=15;
    int ok=N3DS_CAS(N3DS_PTR(uint32_t,addr),&expected,90,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    uint32_t bad=15;
    int fail=N3DS_CAS(N3DS_PTR(uint32_t,addr),&bad,100,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    c->r[0]=old==10 && ok && !fail && bad==90 ? M32(addr):0;
    c->r[4]+=8;
}
static void f_sse(CPU *c) {
    uint32_t a=M32(c->r[4]+4),b=M32(c->r[4]+8);
    Xmm x=n3ds_xmm_read(a);memcpy(n3ds_guest_ptr(b,16),&x,16);
    f80_store(b+32,3.5);c->r[0]=f80_load(b+32)==3.5?x.u[0]:0;c->r[4]+=4;
}
static void f_indirect(CPU *c) {
    static const FnEntry *ic;
    uint32_t sp=c->r[4], target=M32(sp+4);
    c->r[4]=sp;M32(sp+4)=20;M32(sp+8)=22;
    if(ic && ic->addr==target)ic->fn(c);else rt_call_ic(c,target,&ic);
}

int n3ds_lift_self_test(void) {
    static GuestMemory m;
    static const FnEntry table[]={{0x401000,f_add},{0x402000,f_mem},{0x403000,f_sse},{0x404000,f_indirect}};
    static const FnModule modules[]={{"test_a",table,2},{"test_b",table+2,2}};
    guest_init(&m,0x4000000) /* 64 MiB: VirtualAlloc commit test */;
    if(guest_map(&m,0x10000000,4096) || guest_map(&m,0x21000000,4096) || n3ds_lift_bind_modules(&m,modules,2))return -1;
    CPU c;uint32_t r;uint32_t args[]={21,7};int rc=-1;
    n3ds_lift_init_cpu(&c,0x10001000,0);
    if(n3ds_lift_invoke(&c,0x401000,args,2,&r) || r!=28)goto done;
    args[0]=0x21000001;
    if(n3ds_lift_invoke(&c,0x402000,args,1,&r) || r!=90)goto done;
    uint32_t q=0x21000101;M64(q)=UINT64_C(0x1122334455667788);
    uint64_t old64=UINT64_C(0x1122334455667788);
    if(!N3DS_CAS(N3DS_PTR(uint64_t,q),&old64,UINT64_C(0xabcdef0123456789),0,0,0) ||
        M64(q)!=UINT64_C(0xabcdef0123456789))goto done;
    if(N3DS_EXCHANGE(N3DS_PTR(uint16_t,q),0x4321,0)!=0x6789 || M16(q)!=0x4321)goto done;
    M32(q)=1;M32(q+4)=2;M32(q+8)=3;
    rt_movsd(2,q+4,q); /* x86 forward overlapping copy propagates first word */
    if(M32(q+4)!=1 || M32(q+8)!=1)goto done;
    args[0]=0x21000021;args[1]=0x21000061;M32(args[0])=0x12345678;M64(args[0]+8)=UINT64_C(0xabcdef0123456789);
    if(n3ds_lift_invoke(&c,0x403000,args,2,&r) || r!=0x12345678 || M64(args[1]+8)!=M64(args[0]+8))goto done;
    args[0]=0x401000;args[1]=0;
    if(n3ds_lift_invoke(&c,0x404000,args,2,&r) || r!=42)goto done;
    if(win32_self_test(&c,&m))goto done;
    if(n3ds_lift_invoke(&c,0x404000,args,2,&r) || r!=42)goto done;
    rc=0;
done:
    /* The test binds once per process; no further lifted code after destroying memory. */
    guest_destroy(&m);return rc;
}
#ifndef __3DS__
int main(void) {int rc=n3ds_lift_self_test();puts(rc?"Lift ABI smoke test FAILED":"Lift ABI smoke test passed");return rc?1:0;}
#endif
