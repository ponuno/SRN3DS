/* GFx/Scaleform D3D9 compatibility: the menu-blocking refusals from
 * docs/GFX_COMPAT.md section 6.
 *
 * Covered here (host COM + null/PICA-shape backend):
 *   1. CreateDepthStencilSurface(D24S8, Lockable=1) succeeds and its CPU
 *      D24S8 shadow round-trips through LockRect/UnlockRect; a non-lockable
 *      depth descriptor still refuses LockRect.
 *   2. Render states 26/28/29/52..59/136/137/152/174 return S_OK and
 *      GetRenderState reports the stored value (GFx's standard block).
 *   3. SetStreamSourceFreq accepts 1 and the indexed-data form
 *      0x80000001, stores it, and names the divide-instancing refusal.
 *   4. CreateTexture + LockRect/UnlockRect inside BeginScene succeeds; the
 *      level is published at EndScene and binds afterwards.
 *   5. A8 is non-power-of-two capable (PICA CLAMP sampling); RGBA8 NPOT and
 *      NPOT mip chains stay named refusals.
 *
 * DevCaps are unchanged (0x00082000): none of this is a capability claim.
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x100)
#define OUT2 (SCRATCH+0x108)
#define PP (SCRATCH+0x200)
#define CAPS9 (SCRATCH+0x400)
#define DESC (SCRATCH+0x600)
#define PROBE_FN 0x00401000u

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"COM stdcall stack mismatch");
    c->r[4]=sp+4;
}
typedef struct {GuestMemory memory;CPU cpu;uint32_t factory;FnModule tables[2];} Harness;
static int invoke(Harness *h,uint32_t target,const uint32_t *args,unsigned n,uint32_t *result){
    if(n>16)return -1;
    uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    return n3ds_lift_invoke(&h->cpu,PROBE_FN,frame,n+2,result);
}
static uint32_t call(Harness *h,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t vt=0,fn=0;
    if(guest_read32(&h->memory,object,&vt)||!vt){fprintf(stderr,"bad object 0x%08x slot %u\n",object,slot);assert(0);}
    if(guest_read32(&h->memory,vt+slot*4,&fn)||!fn){fprintf(stderr,"bad vtable 0x%08x slot %u\n",vt,slot);assert(0);}
    uint32_t frame[18]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;int rc=invoke(h,fn,frame,n+1,&r);
    if(rc)fprintf(stderr,"gfxcompat: invoke fault on slot %u object 0x%08x rc=%d\n",slot,object,rc);
    assert(!rc);return r;
}
static uint32_t call1(Harness *h,uint32_t object,unsigned slot,uint32_t a){return call(h,object,slot,&a,1);}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static uint32_t unref(Harness *h,uint32_t object){return call(h,object,2,NULL,0);}
static const uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};

static void harness_start(Harness *h,uint32_t *device){
    memset(h,0,sizeof *h);guest_init(&h->memory,2u<<20);
    assert(!guest_map(&h->memory,0x10000000,0x4000)&&!guest_map(&h->memory,SCRATCH,0x2000));
    assert(!d9_com_init_backend(&h->memory,d9_null_backend(),d9_null_context(),32u<<20));
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    h->tables[0]=(FnModule){"probe",probe,1};h->tables[1]=d9_com_module();
    assert(!n3ds_lift_bind_modules(&h->memory,h->tables,2));
    n3ds_lift_init_cpu(&h->cpu,0x10004000,0);
    PeImport import={.dll="d3d9.dll",.name="Direct3DCreate9"};
    uint32_t create9=d9_com_resolve(&import,NULL);assert(create9);
    uint32_t sdk=32;assert(!invoke(h,create9,&sdk,1,&h->factory)&&h->factory);
    assert(!guest_write(&h->memory,PP,pp,sizeof pp));
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(h,h->factory,16,create,6)==0);
    *device=rd(h,OUT);assert(*device);
}
static void harness_stop(Harness *h){d9_com_shutdown();n3ds_lift_release();guest_destroy(&h->memory);}
static uint32_t make_depth(Harness *h,uint32_t device,unsigned w,unsigned height,
                           unsigned format,unsigned lockable,uint32_t expect){
    uint32_t a[]={w,height,format,0,0,lockable,OUT2,0};
    uint32_t hr=call(h,device,29,a,8);
    if(hr!=expect)fprintf(stderr,"depth %ux%u fmt=%u lock=%u -> 0x%08x diag=%s\n",
                          w,height,format,lockable,hr,d9_com_last_diagnostic());
    assert(hr==expect);
    return hr==0?rd(h,OUT2):0;
}
static uint32_t make_tex(Harness *h,uint32_t device,unsigned w,unsigned height,
                         unsigned levels,unsigned format,uint32_t expect){
    uint32_t a[]={w,height,levels,0u,format,0u,OUT2,0};
    uint32_t hr=call(h,device,23,a,8);
    if(hr!=expect)fprintf(stderr,"CreateTexture %ux%u lvl=%u fmt=0x%x -> 0x%08x diag=%s\n",
                          w,height,levels,format,hr,d9_com_last_diagnostic());
    assert(hr==expect);
    return hr==0?rd(h,OUT2):0;
}
/* LockRect on a surface (3 args); returns the guest address. */
static uint32_t lock_surface(Harness *h,uint32_t obj,unsigned *pitch){
    uint32_t a[]={OUT,0,0};
    uint32_t hr=call(h,obj,13,a,3);
    if(hr)fprintf(stderr,"LockRect -> 0x%08x diag=%s\n",hr,d9_com_last_diagnostic());
    assert(hr==0);
    *pitch=rd(h,OUT);
    return rd(h,OUT+4);
}
static void set_rs(Harness *h,uint32_t device,unsigned id,unsigned value){
    uint32_t a[]={id,value};uint32_t hr=call(h,device,57,a,2);
    if(hr)fprintf(stderr,"SetRenderState(%u,%u) -> 0x%08x diag=%s\n",id,value,hr,d9_com_last_diagnostic());
    assert(hr==0);
    uint32_t out=0;uint32_t g[]={id,OUT};assert(call(h,device,58,g,2)==0);
    out=rd(h,OUT);assert(out==value);
}

int main(void){
    Harness h;uint32_t device=0;harness_start(&h,&device);
    assert(call1(&h,device,7,CAPS9)==0);
    assert(rd(&h,CAPS9+28)==0x00082000u);

    /* 1. Lockable D24S8 at the 400x240 presentation size. */
    uint32_t depth=make_depth(&h,device,400,240,D9_D24S8,1,0);
    unsigned pitch=0;
    uint32_t addr=lock_surface(&h,depth,&pitch);
    assert(pitch==400*4&&addr);
    assert(!guest_write32(&h.memory,addr,0x00abcdefu));
    assert(call(&h,depth,14,NULL,0)==0);
    addr=lock_surface(&h,depth,&pitch);
    assert(rd(&h,addr)==0x00abcdefu);
    assert(call(&h,depth,14,NULL,0)==0);
    assert(unref(&h,depth)==0);

    /* Non-lockable depth still refuses LockRect. */
    uint32_t depth_nl=make_depth(&h,device,512,512,D9_D24S8,0,0);
    {uint32_t a[]={OUT,0,0};uint32_t hr=call(&h,depth_nl,13,a,3);
     assert(hr==0x8876086cu&&rd(&h,OUT)==0);}
    assert(unref(&h,depth_nl)==0);

    /* 2. The GFx state block: every id must be accepted and read back. */
    set_rs(&h,device,26,0);            /* DITHERENABLE */
    set_rs(&h,device,28,0);            /* FOGENABLE */
    set_rs(&h,device,29,0);            /* SPECULARENABLE */
    set_rs(&h,device,52,0);            /* STENCILENABLE */
    set_rs(&h,device,53,1);            /* STENCILFAIL=KEEP */
    set_rs(&h,device,54,1);            /* STENCILZFAIL */
    set_rs(&h,device,55,1);            /* STENCILPASS */
    set_rs(&h,device,56,8);            /* STENCILFUNC=ALWAYS */
    set_rs(&h,device,57,0);            /* STENCILREF */
    set_rs(&h,device,58,0xffu);        /* STENCILMASK */
    set_rs(&h,device,59,0xffu);        /* STENCILWRITEMASK */
    set_rs(&h,device,136,0);           /* CLIPPING */
    set_rs(&h,device,137,0);           /* LIGHTING */
    set_rs(&h,device,152,0);           /* CLIPPLANEENABLE */
    set_rs(&h,device,174,0);           /* SCISSORTESTENABLE */
    /* Value validation is still real, not a blanket accept. */
    {uint32_t a[]={53,0};assert(call(&h,device,57,a,2)==0x8876086cu);}
    {uint32_t a[]={58,0u};assert(call(&h,device,57,a,2)==0);}

    /* 3. SetStreamSourceFreq: 1 and the indexed-data form. */
    {uint32_t a[]={0,0x80000001u};assert(call(&h,device,102,a,2)==0);}
    assert(call(&h,device,103,(uint32_t[]){0,OUT},2)==0);
    assert(rd(&h,OUT)==0x80000001u);
    {uint32_t a[]={0,0x80000000u};assert(call(&h,device,102,a,2)==0x8876086cu);} /* count 0 */
    {uint32_t a[]={0,1u};assert(call(&h,device,102,a,2)==0);}
    assert(call(&h,device,103,(uint32_t[]){0,OUT},2)==0);
    assert(rd(&h,OUT)==1u);
    {uint32_t a[]={0,0x40000002u};assert(call(&h,device,102,a,2)==0x8876086au);}
    {uint32_t a[]={0,0u};assert(call(&h,device,102,a,2)==0x8876086cu);}

    /* 4/5. NPOT A8 is legal; RGBA8 NPOT and NPOT mip chains are not. */
    uint32_t tex96=make_tex(&h,device,96,37,1,D9_A8,0);
    assert(tex96);
    (void)make_tex(&h,device,100,100,1,D9_A8R8G8B8,0x8876086cu);
    (void)make_tex(&h,device,96,96,2,D9_A8,0x8876086cu);
    (void)make_tex(&h,device,96,96,0,D9_A8,0x8876086cu); /* default mip chain */

    /* CreateTexture + LockRect/UnlockRect inside BeginScene. */
    assert(call(&h,device,41,NULL,0)==0);
    uint32_t tex=make_tex(&h,device,128,128,1,D9_A8,0);
    assert(tex);
    uint32_t ta[]={0,OUT,0,0};uint32_t thr=call(&h,tex,19,ta,4);
    assert(thr==0);
    pitch=rd(&h,OUT);uint32_t taddr=rd(&h,OUT+4);
    assert(pitch==128&&taddr);
    assert(!guest_write32(&h.memory,taddr,0xdeadbeefu));
    assert(call1(&h,tex,20,0)==0);
    assert(call(&h,device,42,NULL,0)==0);
    /* Present is the real COM frame boundary: it publishes the deferred level,
     * and the texture now binds. */
    {uint32_t a[]={0,0,0,0};assert(call(&h,device,17,a,4)==0);}
    {uint32_t a[]={0,tex};assert(call(&h,device,65,a,2)==0);}

    assert(call1(&h,device,7,CAPS9)==0);
    assert(rd(&h,CAPS9+28)==0x00082000u);
    harness_stop(&h);
    printf("gfx_d3d9_menu_compat_smoke: PASS\n");
    return 0;
}
