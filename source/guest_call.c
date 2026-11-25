#include "guest_call.h"
#include <string.h>

#define RETURN_MARKER 0xffffffffu
int guest_cpu_init(GuestCpu *c, GuestMemory *m, uint32_t base, uint32_t size,
                   const GuestFnEntry *table, size_t count) {
    memset(c,0,sizeof *c);
    if (!size || (base&3) || (size&3) || (uint64_t)base+size>UINT32_MAX || (!table && count)) return -1;
    for (size_t i=0;i<count;++i) if (!table[i].fn || (i && table[i].addr<=table[i-1].addr)) return -1;
    if (guest_map(m,base,size)) return -1;
    c->memory=m;c->stack_base=base;c->stack_size=size;c->r[GUEST_ESP]=base+size;
    c->table=table;c->table_count=count;return 0;
}
void guest_cpu_destroy(GuestCpu *c) {
    if(c->memory)guest_unmap(c->memory,c->stack_base);
    memset(c,0,sizeof *c);
}
static int stack_contains(GuestCpu *c, uint64_t a, uint32_t bytes) {
    return c->memory && a>=c->stack_base && a+bytes<=(uint64_t)c->stack_base+c->stack_size;
}
int guest_push32(GuestCpu *c, uint32_t v) {
    uint32_t sp=c->r[GUEST_ESP];
    if(sp<4 || !stack_contains(c,sp-4,4) || guest_write32(c->memory,sp-4,v))return -1;
    c->r[GUEST_ESP]=sp-4;return 0;
}
int guest_pop32(GuestCpu *c, uint32_t *v) {
    uint32_t sp=c->r[GUEST_ESP];
    if(!stack_contains(c,sp,4) || guest_read32(c->memory,sp,v)) return -1;
    c->r[GUEST_ESP]=sp+4;return 0;
}
int guest_arg32(GuestCpu *c, unsigned i, uint32_t *v) {
    uint64_t addr=(uint64_t)c->r[GUEST_ESP]+4+(uint64_t)i*4;
    if(!stack_contains(c,addr,4))return -1;
    return guest_read32(c->memory,(uint32_t)addr,v);
}
int guest_return(GuestCpu *c, uint32_t value, unsigned n) {
    uint64_t end=(uint64_t)c->r[GUEST_ESP]+4+(uint64_t)n*4;
    if(!stack_contains(c,c->r[GUEST_ESP],4) || !stack_contains(c,end,0))return -1;
    c->r[GUEST_EAX]=value;c->r[GUEST_ESP]=(uint32_t)end;return 0;
}
int guest_dispatch(GuestCpu *c, uint32_t addr) {
    size_t lo=0,hi=c->table_count;
    while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(c->table[mid].addr<addr)lo=mid+1;else hi=mid;}
    if(lo==c->table_count || c->table[lo].addr!=addr)return -1;
    return c->table[lo].fn(c);
}
int guest_invoke(GuestCpu *c, uint32_t addr, const uint32_t *args, size_t argc, uint32_t *result) {
    uint32_t sp=c->r[GUEST_ESP];
    int rc=-1;
    if (!result || (argc && !args) || argc>=c->stack_size/4 || !stack_contains(c,sp,0))return -1;
    for(size_t i=argc;i>0;--i) if(guest_push32(c,args[i-1]))goto done;
    if(guest_push32(c,RETURN_MARKER))goto done;
    if(guest_dispatch(c,addr))goto done;
    /* Cdecl leaves arguments; stdcall removes them. Reject other stack changes. */
    if(c->r[GUEST_ESP]!=sp && c->r[GUEST_ESP]!=sp-(uint32_t)(argc*4))goto done;
    *result=c->r[GUEST_EAX];rc=0;
done:
    c->r[GUEST_ESP]=sp;return rc;
}
/* Handwritten stand-ins for generated functions, exercising the x86 ABI. */
static int cdecl_add(GuestCpu *c) {
    uint32_t a,b;
    if(guest_arg32(c,0,&a) || guest_arg32(c,1,&b))return -1;
    return guest_return(c,a+b,0);
}
static int stdcall_double(GuestCpu *c) {
    uint32_t a;
    if(guest_arg32(c,0,&a))return -1;
    return guest_return(c,a*2,1);
}
int guest_call_self_test(void) {
    static const GuestFnEntry table[]={{0x401000,cdecl_add},{0x402000,stdcall_double}};
    GuestMemory m;GuestCpu c;int rc=-1;guest_init(&m,4096);
    if(guest_cpu_init(&c,&m,0x10000000,4096,table,2))goto done;
    uint32_t args[]={21,7},v,sp=c.r[GUEST_ESP];
    if(guest_invoke(&c,0x401000,args,2,&v) || v!=28 || c.r[GUEST_ESP]!=sp)goto done;
    if(guest_invoke(&c,0x402000,args,1,&v) || v!=42 || c.r[GUEST_ESP]!=sp)goto done;
    if(guest_invoke(&c,0x403000,args,2,&v)!=-1 || c.r[GUEST_ESP]!=sp)goto done;
    if(guest_pop32(&c,&v)!=-1)goto done;
    rc=0;
done:
    guest_cpu_destroy(&c);guest_destroy(&m);return rc;
}
