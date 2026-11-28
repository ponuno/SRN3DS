/* Depth-stencil descriptors for render-target sizes.
 *
 * A depth-stencil surface is a descriptor of the single PICA DEPTH24_STENCIL8
 * attachment that every colour target already owns; no separate VRAM is
 * allocated.  Creation accepts pow2 8..1024, D24S8/D24X8, no MSAA; a
 * Lockable=1 descriptor owns a CPU D24S8 shadow for LockRect/UnlockRect while
 * binding still resolves to the target's embedded attachment.  The binding must
 * match the active target exactly.  A depth whose size does not match the
 * active target is queued (the engine sets the depth before switching targets)
 * and applied when the matching target becomes current.  Clear(Z|STENCIL)
 * reaches the active target's attachment (the null backend records the exact
 * packed value).
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <math.h>
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
    if(rc)fprintf(stderr,"depth_rt: invoke fault on slot %u object 0x%08x rc=%d\n",slot,object,rc);
    assert(!rc);return r;
}
static uint32_t call1(Harness *h,uint32_t object,unsigned slot,uint32_t a){return call(h,object,slot,&a,1);}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static uint32_t unref(Harness *h,uint32_t object){return call(h,object,2,NULL,0);}
static uint32_t fbits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
/* Mirror the core's D24 quantization so the expected value is not a magic
 * literal (float rounding at 2^24 matters for 0.75 -> 0xC00000). */
static uint32_t packed_z(float z,uint32_t stencil){
    uint32_t q=(uint32_t)(z*16777215.0f+0.5f);
    if(q>0xffffffu)q=0xffffffu;
    return q|((stencil&0xffu)<<24);
}
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
static uint32_t make_rt(Harness *h,uint32_t device,unsigned w,unsigned height){
    uint32_t a[]={w,height,D9_X8R8G8B8,0,0,0,OUT2,0};
    assert(call(h,device,28,a,8)==0);uint32_t rt=rd(h,OUT2);assert(rt);return rt;
}
static uint32_t make_tex_rt(Harness *h,uint32_t device,unsigned w,unsigned height){
    uint32_t a[]={w,height,1,1u,D9_X8R8G8B8,0,OUT2,0}; /* Usage=1, Pool=DEFAULT */
    assert(call(h,device,23,a,8)==0);uint32_t tex=rd(h,OUT2);assert(tex);return tex;
}
static uint32_t make_depth(Harness *h,uint32_t device,unsigned w,unsigned height,
                           unsigned format,unsigned ms,unsigned quality,unsigned lockable,
                           uint32_t expect){
    uint32_t a[]={w,height,format,ms,quality,lockable,OUT2,0};
    uint32_t hr=call(h,device,29,a,8);
    if(hr!=expect)fprintf(stderr,"depth create %ux%u fmt=%u ms=%u lock=%u -> 0x%08x diag=%s\n",
                          w,height,format,ms,lockable,hr,d9_com_last_diagnostic());
    assert(hr==expect);
    return hr==0?rd(h,OUT2):0;
}
static int set_target(Harness *h,uint32_t device,uint32_t surface,uint32_t expect){
    uint32_t a[]={0,surface};
    uint32_t hr=call(h,device,37,a,2);
    if(hr!=expect)fprintf(stderr,"SetRenderTarget -> 0x%08x diag=%s\n",hr,d9_com_last_diagnostic());
    return hr==expect;
}
static int bind_depth(Harness *h,uint32_t device,uint32_t surface,uint32_t expect){
    uint32_t a[]={surface};
    uint32_t hr=call(h,device,39,a,1);
    if(hr!=expect)fprintf(stderr,"SetDepthStencilSurface -> 0x%08x diag=%s\n",hr,d9_com_last_diagnostic());
    return hr==expect;
}
static uint32_t get_depth(Harness *h,uint32_t device){
    uint32_t a[]={OUT2};assert(call(h,device,40,a,1)==0);uint32_t d=rd(h,OUT2);assert(d);return d;
}
static uint32_t get_target(Harness *h,uint32_t device){
    uint32_t a[]={0,OUT2};assert(call(h,device,38,a,2)==0);uint32_t t=rd(h,OUT2);assert(t);return t;
}
static void desc(Harness *h,uint32_t surface,uint32_t *format,uint32_t *w,uint32_t *height){
    uint32_t a[]={DESC};assert(call(h,surface,12,a,1)==0);
    *format=rd(h,DESC);*w=rd(h,DESC+24);*height=rd(h,DESC+28);
}
static void depth_frame(Harness *h,uint32_t device,float z,uint32_t stencil,uint32_t flags){
    uint32_t clear[]={0,0,flags,0xff204060u,fbits(z),stencil};
    assert(call(h,device,43,clear,6)==0);
    assert(call(h,device,41,NULL,0)==0);
    assert(call(h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(call(h,device,17,present,4)==0);
}

int main(void){
    Harness h;uint32_t device=0;harness_start(&h,&device);
    /* No caps claim is added by depth descriptors. */
    assert(call1(&h,device,7,CAPS9)==0);
    assert(rd(&h,CAPS9+28)==0x00082000u);

    /* 1024x1024 explicit target + matching D24S8 descriptor. */
    uint32_t rt1024=make_rt(&h,device,1024,1024);
    assert(set_target(&h,device,rt1024,0));
    uint32_t d1024s8=make_depth(&h,device,1024,1024,D9_D24S8,0,0,0,0);
    uint32_t fmt=0,w=0,height=0;desc(&h,d1024s8,&fmt,&w,&height);
    assert(fmt==D9_D24S8&&w==1024&&height==1024);
    assert(bind_depth(&h,device,d1024s8,0));
    uint32_t got=get_depth(&h,device);assert(got==d1024s8);assert(unref(&h,got)==2); /* binding ref remains */

    /* Clear reaches the active target's attachment; the null backend sees the
     * exact packed D24S8 value. */
    D9NullStats before=*d9_null_stats();
    depth_frame(&h,device,0.5f,9,7);
    D9NullStats after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_flags==7&&after.last_depth_clear==packed_z(0.5f,9));

    /* 512x512 descriptor for the 1024 target is queued: the engine sets a
     * depth before switching to the target that owns it.  Selecting the 512x512
     * target then applies the request. */
    uint32_t d512s8=make_depth(&h,device,512,512,D9_D24S8,0,0,0,0);
    assert(bind_depth(&h,device,d512s8,0));
    got=get_depth(&h,device);assert(got==d512s8);
    assert(unref(&h,got)==2); /* GetDepthStencilSurface retained it */

    /* Switching to a target with a different size keeps the queued depth and
     * uses the target's embedded attachment. */
    uint32_t rt512=make_rt(&h,device,512,512);
    assert(set_target(&h,device,rt512,0));
    got=get_target(&h,device);assert(got==rt512);
    assert(unref(&h,got)==2); /* GetRenderTarget retained it */
    assert(set_target(&h,device,0,0)); /* back buffer is 400x240 */

    /* Selecting the queued descriptor's 512 target applies it; the earlier
     * switch to the 400x240 back buffer used the embedded attachment. */
    assert(set_target(&h,device,rt512,0));
    before=*d9_null_stats();
    depth_frame(&h,device,0.25f,0,6);
    after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_flags==6&&after.last_depth_clear==packed_z(0.25f,0));

    /* Unbind: GetDepthStencilSurface reports NOTFOUND, then the descriptor
     * releases cleanly. */
    assert(bind_depth(&h,device,0,0));
    assert(call(&h,device,40,(uint32_t[]){OUT2},1)==0x88760866u);
    assert(unref(&h,d512s8)==0);

    /* D24X8 is the same attachment, reported as format 77. */
    uint32_t d1024x8=make_depth(&h,device,1024,1024,D9_D24X8,0,0,0,0);
    desc(&h,d1024x8,&fmt,&w,&height);
    assert(fmt==D9_D24X8&&w==1024&&height==1024);
    assert(set_target(&h,device,rt1024,0));
    assert(bind_depth(&h,device,d1024x8,0));
    depth_frame(&h,device,0.75f,5,7);
    after=*d9_null_stats();
    assert(after.last_depth_flags==7&&after.last_depth_clear==packed_z(0.75f,5));

    /* Creation matrix: MSAA/quality/lockable/shared/bad dimensions/D16. */
    assert(bind_depth(&h,device,0,0));
    make_depth(&h,device,1024,1024,D9_D24S8,2,0,0,0x8876086au);
    make_depth(&h,device,1024,1024,D9_D24S8,0,1,0,0x8876086au);
    /* Lockable=1 is a real CPU D24S8 shadow (GFx asks for it at 0xf82616). */
    {uint32_t dl=make_depth(&h,device,1024,1024,D9_D24S8,0,0,1,0);assert(dl);assert(unref(&h,dl)==0);}
    {uint32_t a[]={1024,1024,D9_D24S8,0,0,0,OUT2,1};assert(call(&h,device,29,a,8)==0x8876086cu);}
    make_depth(&h,device,1000,1000,D9_D24S8,0,0,0,0x8876086cu);
    make_depth(&h,device,2048,2048,D9_D24S8,0,0,0,0x8876086cu);
    make_depth(&h,device,400,240,80,0,0,0,0x8876086au); /* D16 */

    /* The original 400x240 back-buffer path still works. */
    assert(unref(&h,d1024x8)==0);
    assert(set_target(&h,device,0,0));
    uint32_t d400=make_depth(&h,device,400,240,D9_D24S8,0,0,0,0);
    assert(bind_depth(&h,device,d400,0));
    before=*d9_null_stats();
    depth_frame(&h,device,1.0f,1,7);
    after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_clear==packed_z(1.0f,1));
    assert(bind_depth(&h,device,0,0));
    assert(unref(&h,d400)==0);

    /* A 1024 render-target texture exposes its backing target, which accepts
     * the 1024 depth descriptor too. */
    uint32_t tex=make_tex_rt(&h,device,1024,1024);
    uint32_t level[]={0,OUT2};assert(call(&h,tex,18,level,2)==0);
    uint32_t tex_surface=rd(&h,OUT2);assert(tex_surface);
    assert(set_target(&h,device,tex_surface,0));
    assert(bind_depth(&h,device,d1024s8,0)); /* d1024s8 was unbound, still alive */
    before=*d9_null_stats();
    depth_frame(&h,device,0.5f,2,7);
    after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_clear==packed_z(0.5f,2));

    /* Cleanup: unbind, restore the back buffer, release the texture/surfaces. */
    assert(bind_depth(&h,device,0,0));
    assert(set_target(&h,device,0,0));
    assert(unref(&h,tex_surface)==0);
    assert(unref(&h,tex)==0);
    assert(unref(&h,d1024s8)==0);
    assert(unref(&h,rt512)==0);
    assert(unref(&h,rt1024)==0);
    assert(call1(&h,device,7,CAPS9)==0&&rd(&h,CAPS9+28)==0x00082000u);
    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    harness_stop(&h);
    puts("depth-stencil descriptors: 1024 D24S8/D24X8 create+bind+clear, 512/1024 queued "
         "depth applied on target switch, MSAA/lockable shadow/shared/bad sizes, 400x240 back "
         "buffer, render-target texture binding and honest caps passed");
    return 0;
}
