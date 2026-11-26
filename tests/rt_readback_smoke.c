/* GetRenderTargetData / GetFrontBufferData device slots 32/33.
 *
 * Reads a real render target into a CPU offscreen surface through the same
 * target_read path used by LockRect, checks the BGRA pixels, and keeps the
 * front-buffer method an explicit named refusal (never a back-buffer
 * substitution or fake data). */
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH+0x200)
#define OUT (SCRATCH+0x400)
#define OUT2 (SCRATCH+0x500)
#define LOCKED (SCRATCH+0x3000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALID 0x8876086cu
#define HR_UNAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"readback stdcall stack mismatch");
    c->r[4]=sp+4;
}
typedef struct {GuestMemory memory;CPU cpu;uint32_t factory,device;} Harness;
static int invoke(Harness *h,uint32_t target,const uint32_t *args,unsigned n,uint32_t *result){
    if(n>16)return -1;
    uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    return n3ds_lift_invoke(&h->cpu,PROBE_FN,frame,n+2,result);
}
static uint32_t call(Harness *h,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t vt=0,fn=0;assert(!guest_read32(&h->memory,object,&vt)&&vt);
    assert(!guest_read32(&h->memory,vt+slot*4,&fn)&&fn);
    uint32_t frame[18]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t rd(Harness *h,uint32_t a){uint32_t v=0;assert(!guest_read32(&h->memory,a,&v));return v;}
static void wr(Harness *h,uint32_t a,const void *p,size_t n){assert(!guest_write(&h->memory,a,p,n));}
static uint32_t create_offscreen(Harness *h,unsigned w,unsigned ht,unsigned format,unsigned pool){
    uint32_t a[]={w,ht,format,pool,OUT,0};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(call(h,h->device,36,a,6)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint32_t create_rt(Harness *h,unsigned w,unsigned ht,unsigned format,unsigned lockable){
    uint32_t a[]={w,ht,format,0,0,lockable,OUT,0};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(call(h,h->device,28,a,8)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint32_t rt_texture=0;
static uint32_t rt_texture_surface(Harness *h,unsigned w,unsigned ht,unsigned format){
    uint32_t t=0;
    {uint32_t a[]={w,ht,1,1,format,0,OUT,0};
     assert(!guest_write32(&h->memory,OUT,0));
     assert(call(h,h->device,23,a,8)==HR_OK);t=rd(h,OUT);assert(t);}
    uint32_t a2[]={0,OUT2};
    assert(!guest_write32(&h->memory,OUT2,0));
    assert(call(h,t,18,a2,2)==HR_OK);
    uint32_t s=rd(h,OUT2);assert(s);
    rt_texture=t;
    return s;
}
static uint32_t depth_surface(Harness *h){
    uint32_t a[]={8,8,D9_D24S8,0,0,0,OUT,0};
    assert(call(h,h->device,29,a,8)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint8_t *surf_lock(Harness *h,uint32_t surf,unsigned flags,unsigned *pitch){
    uint32_t a[]={LOCKED,0,flags};
    assert(call(h,surf,13,a,3)==HR_OK);
    *pitch=rd(h,LOCKED);
    uint32_t bits=rd(h,LOCKED+4);
    uint8_t *p=guest_ptr(&h->memory,bits,*pitch*8);assert(p);return p;
}
static void surf_unlock(Harness *h,uint32_t surf){assert(call(h,surf,14,NULL,0)==HR_OK);}
static void fill_rt(Harness *h,uint32_t surf,uint32_t color,uint32_t rect){
    assert(call(h,h->device,35,(uint32_t[]){surf,color,rect},3)==HR_OK);
}
static void check_offscreen(Harness *h,uint32_t surf,uint32_t color){
    unsigned pitch;uint32_t *p=(uint32_t *)surf_lock(h,surf,0x10,&pitch);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
        assert(((uint32_t *)((uint8_t *)p+(size_t)y*pitch))[x]==color);
    surf_unlock(h,surf);
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x6000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[2]={{"probe",probe,1},d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,2));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    uint32_t create9=d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"},NULL);
    assert(create9);
    uint32_t sdk=32;assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    wr(&h,PP,pp,sizeof pp);
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==HR_OK);
    assert(!guest_read32(&h.memory,OUT,&h.device)&&h.device);

    uint32_t rt=create_rt(&h,8,8,FMT_A8R8G8B8,1);
    uint32_t dst=create_offscreen(&h,8,8,FMT_A8R8G8B8,2);
    fill_rt(&h,rt,0xff112233u,0);
    assert(call(&h,h.device,32,(uint32_t[]){rt,dst},2)==HR_OK);
    check_offscreen(&h,dst,0xff112233u);

    /* A second read sees the updated target, not a cached first copy. */
    fill_rt(&h,rt,0xff445566u,0);
    assert(call(&h,h.device,32,(uint32_t[]){rt,dst},2)==HR_OK);
    check_offscreen(&h,dst,0xff445566u);

    /* Non-lockable render-target texture surface is readable through target_read. */
    uint32_t rts=rt_texture_surface(&h,8,8,FMT_A8R8G8B8);
    fill_rt(&h,rts,0xff778899u,0);
    assert(call(&h,h.device,32,(uint32_t[]){rts,dst},2)==HR_OK);
    check_offscreen(&h,dst,0xff778899u);

    /* Size/format/argument refusals. */
    uint32_t big=create_offscreen(&h,16,8,FMT_A8R8G8B8,2);
    assert(call(&h,h.device,32,(uint32_t[]){rt,big},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"share size"));
    uint32_t x8=create_offscreen(&h,8,8,FMT_X8R8G8B8,2);
    assert(call(&h,h.device,32,(uint32_t[]){rt,x8},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"share size"));
    uint32_t gpu_dst=create_rt(&h,8,8,FMT_A8R8G8B8,0);
    assert(call(&h,h.device,32,(uint32_t[]){rt,gpu_dst},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"CPU offscreen"));
    uint32_t depth=depth_surface(&h);
    assert(call(&h,h.device,32,(uint32_t[]){depth,dst},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"depth"));
    uint32_t offsrc=create_offscreen(&h,8,8,FMT_A8R8G8B8,2);
    assert(call(&h,h.device,32,(uint32_t[]){offsrc,dst},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"not a render target"));
    {unsigned pitch;surf_lock(&h,dst,0,&pitch);
     assert(call(&h,h.device,32,(uint32_t[]){rt,dst},2)==HR_INVALID);
     assert(strstr(d9_com_last_diagnostic(),"locked"));surf_unlock(&h,dst);}

    /* A capped 512 target cannot be read back while scaled readback is off. */
    uint32_t scaled=create_rt(&h,512,512,FMT_A8R8G8B8,0);
    uint32_t scaled_dst=create_offscreen(&h,512,512,FMT_A8R8G8B8,2);
    assert(call(&h,h.device,32,(uint32_t[]){scaled,scaled_dst},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"scaled readback"));

    /* Front-buffer readback is a named refusal with no destination writes. */
    fill_rt(&h,rt,0xff010203u,0);
    assert(call(&h,h.device,32,(uint32_t[]){rt,dst},2)==HR_OK);
    uint32_t first=0;
    {unsigned pitch;uint32_t *p=(uint32_t *)surf_lock(&h,dst,0x10,&pitch);first=p[0];surf_unlock(&h,dst);}
    assert(call(&h,h.device,33,(uint32_t[]){0,dst},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"front buffer readback"));
    {unsigned pitch;uint32_t *p=(uint32_t *)surf_lock(&h,dst,0x10,&pitch);assert(p[0]==first);surf_unlock(&h,dst);}

    uint32_t objects[]={rt,dst,rts,big,x8,gpu_dst,depth,offsrc,scaled,scaled_dst};
    for(unsigned i=0;i<sizeof objects/sizeof objects[0];++i)
        assert(call(&h,objects[i],2,NULL,0)==0);
    assert(call(&h,rt_texture,2,NULL,0)==0);
    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("GetRenderTargetData / GetFrontBufferData: PASS");
    return 0;
}
