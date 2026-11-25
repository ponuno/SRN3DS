#pragma once
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define D9SR_SCRATCH 0x10010000u
#define D9SR_OUT (D9SR_SCRATCH+0x100)
#define D9SR_OUT2 (D9SR_SCRATCH+0x108)
#define D9SR_PP (D9SR_SCRATCH+0x200)
#define D9SR_RECT1 (D9SR_SCRATCH+0x600)
#define D9SR_RECT2 (D9SR_SCRATCH+0x620)
#define D9SR_RECT3 (D9SR_SCRATCH+0x640)
#define D9SR_RECT4 (D9SR_SCRATCH+0x660)
#define D9SR_PROBE_FN 0x00401000u
#define D9SR_INVALIDCALL 0x8876086cu
#define D9SR_NOTAVAILABLE 0x8876086au

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void d9sr_dispatch(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=D9SR_PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"COM stdcall stack mismatch");
    c->r[4]=sp+4;
}
typedef struct {GuestMemory memory;CPU cpu;uint32_t factory;FnModule tables[2];} D9SrHarness;
static int d9sr_invoke(D9SrHarness *h,uint32_t target,const uint32_t *args,unsigned n,uint32_t *result){
    if(n>16)return -1;
    uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    return n3ds_lift_invoke(&h->cpu,D9SR_PROBE_FN,frame,n+2,result);
}
static uint32_t d9sr_call(D9SrHarness *h,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t vt=0,fn=0;
    if(guest_read32(&h->memory,object,&vt)||!vt){
        fprintf(stderr,"stretchrect_com: bad COM object 0x%08x for slot %u (vtable 0x%08x)\n",object,slot,vt);
        assert(0);
    }
    if(guest_read32(&h->memory,vt+slot*4,&fn)||!fn){
        fprintf(stderr,"stretchrect_com: bad COM vtable 0x%08x slot %u\n",vt,slot);
        assert(0);
    }
    uint32_t frame[18]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;assert(!d9sr_invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t d9sr_rd(D9SrHarness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static void d9sr_wr(D9SrHarness *h,uint32_t address,const void *data,size_t bytes){assert(!guest_write(&h->memory,address,data,bytes));}
static uint32_t d9sr_unref(D9SrHarness *h,uint32_t object){return d9sr_call(h,object,2,NULL,0);}
static void d9sr_present_params(D9SrHarness *h,const uint32_t *pp){d9sr_wr(h,D9SR_PP,pp,14*4);}
static const uint32_t d9sr_good_pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};

static int d9sr_start(D9SrHarness *h,const D9Backend *ops,void *ctx,size_t limit,int null_backend){
    memset(h,0,sizeof *h);guest_init(&h->memory,2u<<20);
    assert(!guest_map(&h->memory,0x10000000,0x4000)&&!guest_map(&h->memory,D9SR_SCRATCH,0x2000));
    if(null_backend){assert(!d9_com_init(&h->memory,NULL));d9_null_reset();assert(!d9_null_register());}
    else assert(!d9_com_init_backend(&h->memory,ops,ctx,limit));
    static const FnEntry probe[]={{D9SR_PROBE_FN,d9sr_dispatch}};
    h->tables[0]=(FnModule){"probe",probe,1};h->tables[1]=d9_com_module();
    assert(!n3ds_lift_bind_modules(&h->memory,h->tables,2));
    n3ds_lift_init_cpu(&h->cpu,0x10004000,0);
    PeImport import={.dll="d3d9.dll",.name="Direct3DCreate9"};
    uint32_t create9=d9_com_resolve(&import,NULL);assert(create9);
    uint32_t sdk=32;assert(!d9sr_invoke(h,create9,&sdk,1,&h->factory)&&h->factory);
    return 0;
}
static uint32_t d9sr_device(D9SrHarness *h){
    d9sr_present_params(h,d9sr_good_pp);uint32_t create[]={0,1,0,0x20,D9SR_PP,D9SR_OUT};
    assert(d9sr_call(h,h->factory,16,create,6)==0);
    uint32_t device=d9sr_rd(h,D9SR_OUT);assert(device);return device;
}
static void d9sr_stop(D9SrHarness *h){d9_com_shutdown();n3ds_lift_release();guest_destroy(&h->memory);}
static uint32_t d9sr_create_target(D9SrHarness *h,uint32_t device,unsigned w,unsigned hgt,unsigned format,unsigned lockable){
    uint32_t args[]={w,hgt,format,0,0,lockable,D9SR_OUT2,0};
    assert(d9sr_call(h,device,28,args,8)==0);uint32_t o=d9sr_rd(h,D9SR_OUT2);assert(o);return o;
}
static uint32_t d9sr_create_cpu_surface(D9SrHarness *h,uint32_t device,unsigned w,unsigned hgt){
    uint32_t args[]={w,hgt,D9_A8R8G8B8,2,D9SR_OUT2,0};
    assert(d9sr_call(h,device,36,args,6)==0);uint32_t o=d9sr_rd(h,D9SR_OUT2);assert(o);return o;
}
static uint32_t d9sr_stretch(D9SrHarness *h,uint32_t device,uint32_t src,uint32_t sr,uint32_t dst,uint32_t dr,uint32_t filter){
    uint32_t args[]={src,sr,dst,dr,filter};return d9sr_call(h,device,34,args,5);
}
static const char *d9sr_last_reject(void){return d9_com_last_stretch_reject();}
