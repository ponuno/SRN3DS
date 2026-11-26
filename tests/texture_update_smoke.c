/* UpdateTexture / UpdateSurface / ColorFill (device slots 31/30/35).
 *
 * All three operate on CPU-backed stores owned by the existing core/COM
 * surfaces, so the test checks the bytes that would be uploaded, the partial
 * rectangles, the existing unlock/upload rules and the named refusals
 * (format/size/level mismatch, locks, GPU-only render targets, depth and cube
 * faces).  No GPU path is claimed. */
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
#define RECT (SCRATCH+0x4000)
#define POINT (SCRATCH+0x5000)
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
    if(c->r[4]!=sp)rt_unimpl(c,target,"texture update stdcall stack mismatch");
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
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));
    return r;
}
static uint32_t rd(Harness *h,uint32_t a){uint32_t v=0;assert(!guest_read32(&h->memory,a,&v));return v;}
static void wr(Harness *h,uint32_t a,const void *p,size_t n){assert(!guest_write(&h->memory,a,p,n));}
static uint32_t create_texture(Harness *h,unsigned w,unsigned ht,unsigned levels,
                               unsigned usage,unsigned format,unsigned pool){
    uint32_t a[]={w,ht,levels,usage,format,pool,OUT,0};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(call(h,h->device,23,a,8)==HR_OK);
    uint32_t t=rd(h,OUT);assert(t);return t;
}
static uint32_t create_offscreen(Harness *h,unsigned w,unsigned ht,unsigned format){
    uint32_t a[]={w,ht,format,2,OUT,0}; /* D3DPOOL_SYSTEMMEM */
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(call(h,h->device,36,a,6)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint32_t render_target_surface(Harness *h){
    uint32_t a[]={8,8,FMT_A8R8G8B8,0,0,1,OUT,0};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(call(h,h->device,28,a,8)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint32_t texture_surface(Harness *h,uint32_t tex,unsigned level){
    uint32_t a[]={level,OUT};
    assert(!guest_write32(&h->memory,OUT,0));
    assert(call(h,tex,18,a,2)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint32_t cube_surface(Harness *h,uint32_t cube,unsigned face,unsigned level){
    uint32_t a[]={face,level,OUT};
    assert(!guest_write32(&h->memory,OUT,0));
    assert(call(h,cube,18,a,3)==HR_OK);
    uint32_t s=rd(h,OUT);assert(s);return s;
}
static uint8_t *tex_lock(Harness *h,uint32_t tex,unsigned level,unsigned flags,
                         unsigned *pitch){
    uint32_t a[]={level,LOCKED,0,flags};
    assert(call(h,tex,19,a,4)==HR_OK);
    *pitch=rd(h,LOCKED);
    uint32_t bits=rd(h,LOCKED+4);
    uint8_t *p=guest_ptr(&h->memory,bits,*pitch*(8u>>level));
    assert(p);return p;
}
static void tex_unlock(Harness *h,uint32_t tex,unsigned level){
    uint32_t a[]={level};assert(call(h,tex,20,a,1)==HR_OK);
}
static uint8_t *surf_lock(Harness *h,uint32_t surf,uint32_t rect,unsigned flags,
                          unsigned *pitch){
    uint32_t a[]={LOCKED,rect,flags};
    assert(call(h,surf,13,a,3)==HR_OK);
    *pitch=rd(h,LOCKED);uint32_t bits=rd(h,LOCKED+4);
    uint8_t *p=guest_ptr(&h->memory,bits,*pitch*8u);assert(p);return p;
}
static void surf_unlock(Harness *h,uint32_t surf){assert(call(h,surf,14,NULL,0)==HR_OK);}
static void fill_texture(Harness *h,uint32_t tex,unsigned level,uint32_t seed){
    unsigned pitch;uint8_t *p=tex_lock(h,tex,level,0,&pitch);
    unsigned dim=8u>>level;if(!dim)dim=1;
    for(unsigned y=0;y<dim;++y)for(unsigned x=0;x<dim;++x)
        ((uint32_t *)(p+(size_t)y*pitch))[x]=seed+x+y*17u+level*101u;
    tex_unlock(h,tex,level);
}
static void set_rect(Harness *h,int32_t l,int32_t t,int32_t r,int32_t b){
    int32_t v[4]={l,t,r,b};wr(h,RECT,v,sizeof v);
}
static void set_point(Harness *h,int32_t x,int32_t y){
    int32_t v[2]={x,y};wr(h,POINT,v,sizeof v);
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

    /* ---- UpdateTexture: all mips copied, destination re-uploaded ---------- */
    uint32_t src=create_texture(&h,8,8,2,0,FMT_A8R8G8B8,1);
    uint32_t dst=create_texture(&h,8,8,2,0,FMT_A8R8G8B8,1);
    fill_texture(&h,src,0,0x11000000u);
    fill_texture(&h,src,1,0x22000000u);
    unsigned uploads=d9_null_stats()->texture_uploads;
    assert(call(&h,h.device,31,(uint32_t[]){src,dst},2)==HR_OK);
    assert(d9_null_stats()->texture_uploads==uploads+1);
    for(unsigned level=0;level<2;++level){
        unsigned pitch;uint8_t *p=tex_lock(&h,dst,level,0x10,&pitch);
        unsigned dim=8u>>level;if(!dim)dim=1;
        uint32_t seed=level?0x22000000u:0x11000000u;
        for(unsigned y=0;y<dim;++y)for(unsigned x=0;x<dim;++x)
            assert(((uint32_t *)(p+(size_t)y*pitch))[x]==seed+x+y*17u+level*101u);
        tex_unlock(&h,dst,level);
    }
    /* Mismatches and non-2D handles. */
    uint32_t x8=create_texture(&h,8,8,2,0,FMT_X8R8G8B8,1);
    assert(call(&h,h.device,31,(uint32_t[]){src,x8},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"share format"));
    uint32_t big=create_texture(&h,16,8,2,0,FMT_A8R8G8B8,1);
    assert(call(&h,h.device,31,(uint32_t[]){src,big},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"share format"));
    uint32_t one=create_texture(&h,8,8,1,0,FMT_A8R8G8B8,1);
    assert(call(&h,h.device,31,(uint32_t[]){src,one},2)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"share format"));
    uint32_t rtt=create_texture(&h,8,8,1,1,FMT_A8R8G8B8,0);
    assert(call(&h,h.device,31,(uint32_t[]){one,rtt},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"guest copy"));
    {unsigned pitch;tex_lock(&h,src,0,0,&pitch);
     assert(call(&h,h.device,31,(uint32_t[]){src,dst},2)==HR_INVALID);
     assert(strstr(d9_com_last_diagnostic(),"locked"));tex_unlock(&h,src,0);}
    uint32_t cube=0;
    {uint32_t a[]={8,1,0,FMT_A8R8G8B8,1,OUT,0};
     assert(!guest_write32(&h.memory,OUT,0));assert(call(&h,h.device,25,a,7)==HR_OK);
     cube=rd(&h,OUT);assert(cube);}
    assert(call(&h,h.device,31,(uint32_t[]){src,cube},2)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"cube/volume"));

    /* ---- UpdateSurface: partial rect, NULL=whole, named refusals ---------- */
    uint32_t us=create_texture(&h,8,8,1,0,FMT_A8R8G8B8,1);
    uint32_t ud=create_texture(&h,8,8,1,0,FMT_A8R8G8B8,1);
    fill_texture(&h,us,0,0x33000000u);
    {unsigned pitch;uint8_t *p=tex_lock(&h,ud,0,0,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
         ((uint32_t *)(p+(size_t)y*pitch))[x]=0xAAAAAAAAu;
     tex_unlock(&h,ud,0);}
    uint32_t ss=texture_surface(&h,us,0),ds=texture_surface(&h,ud,0);
    set_rect(&h,2,1,6,4);set_point(&h,3,5);
    assert(rd(&h,POINT)==3&&rd(&h,POINT+4)==5);
    assert(call(&h,h.device,30,(uint32_t[]){ss,RECT,ds,POINT},4)==HR_OK);
    {unsigned pitch;uint8_t *p=tex_lock(&h,ud,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
         uint32_t got=((uint32_t *)(p+(size_t)y*pitch))[x];
         if(x>=3&&x<7&&y>=5&&y<8)
             assert(got==0x33000000u+(x-1)+(y-4)*17u);
         else assert(got==0xAAAAAAAAu);
     }
     tex_unlock(&h,ud,0);}
    /* NULL rect = whole surface. */
    uint32_t ud2=create_texture(&h,8,8,1,0,FMT_A8R8G8B8,1);
    uint32_t ds2=texture_surface(&h,ud2,0);
    assert(call(&h,h.device,30,(uint32_t[]){ss,0,ds2,0},4)==HR_OK);
    {unsigned pitch;uint8_t *p=tex_lock(&h,ud2,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
         assert(((uint32_t *)(p+(size_t)y*pitch))[x]==0x33000000u+x+y*17u);
     tex_unlock(&h,ud2,0);}
    set_rect(&h,0,0,9,1);
    assert(call(&h,h.device,30,(uint32_t[]){ss,RECT,ds,POINT},4)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"inside the source"));
    set_rect(&h,0,0,2,1);set_point(&h,7,0);
    assert(call(&h,h.device,30,(uint32_t[]){ss,RECT,ds,POINT},4)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"fit the destination"));
    uint32_t xs=create_texture(&h,8,8,1,0,FMT_X8R8G8B8,1);
    uint32_t xsurf=texture_surface(&h,xs,0);set_rect(&h,0,0,1,1);set_point(&h,0,0);
    assert(call(&h,h.device,30,(uint32_t[]){ss,RECT,xsurf,POINT},4)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"formats"));
    uint32_t rts=texture_surface(&h,rtt,0);
    assert(call(&h,h.device,30,(uint32_t[]){rts,RECT,ds,POINT},4)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"GPU-only"));
    {unsigned pitch;surf_lock(&h,ss,0,0,&pitch);set_rect(&h,0,0,1,1);
     assert(call(&h,h.device,30,(uint32_t[]){ss,RECT,ds,POINT},4)==HR_INVALID);
     assert(strstr(d9_com_last_diagnostic(),"locked"));surf_unlock(&h,ss);}

    /* ---- ColorFill: offscreen, texture level and render target ------------ */
    uint32_t off=create_offscreen(&h,8,8,FMT_A8R8G8B8);
    assert(call(&h,h.device,35,(uint32_t[]){off,0xff112233u,0},3)==HR_OK);
    {unsigned pitch;uint8_t *p=surf_lock(&h,off,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
         assert(((uint32_t *)(p+(size_t)y*pitch))[x]==0xff112233u);
     surf_unlock(&h,off);}
    set_rect(&h,2,3,5,6);
    assert(call(&h,h.device,35,(uint32_t[]){off,0xff445566u,RECT},3)==HR_OK);
    {unsigned pitch;uint8_t *p=surf_lock(&h,off,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
         uint32_t got=((uint32_t *)(p+(size_t)y*pitch))[x];
         if(x>=2&&x<5&&y>=3&&y<6)assert(got==0xff445566u);
         else assert(got==0xff112233u);
     } surf_unlock(&h,off);}
    uint32_t ft=create_texture(&h,8,8,1,0,FMT_A8R8G8B8,1);
    uint32_t fs=texture_surface(&h,ft,0);
    set_rect(&h,1,1,4,3);
    assert(call(&h,h.device,35,(uint32_t[]){fs,0xff778899u,RECT},3)==HR_OK);
    {unsigned pitch;uint8_t *p=tex_lock(&h,ft,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
         uint32_t got=((uint32_t *)(p+(size_t)y*pitch))[x];
         assert(got==((x>=1&&x<4&&y>=1&&y<3)?0xff778899u:0u));
     }tex_unlock(&h,ft,0);}
    uint32_t lockrt=render_target_surface(&h);
    assert(call(&h,h.device,35,(uint32_t[]){lockrt,0xff010203u,0},3)==HR_OK);
    {unsigned pitch;uint8_t *p=surf_lock(&h,lockrt,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
         assert(((uint32_t *)(p+(size_t)y*pitch))[x]==0xff010203u);
     surf_unlock(&h,lockrt);}
    set_rect(&h,4,4,8,8);
    assert(call(&h,h.device,35,(uint32_t[]){lockrt,0xff0a0b0cu,RECT},3)==HR_OK);
    {unsigned pitch;uint8_t *p=surf_lock(&h,lockrt,0,0x10,&pitch);
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
         assert(((uint32_t *)(p+(size_t)y*pitch))[x]==((x>=4&&y>=4)?0xff0a0b0cu:0xff010203u));
     surf_unlock(&h,lockrt);}
    uint32_t cfs=cube_surface(&h,cube,0,0);
    assert(call(&h,h.device,35,(uint32_t[]){cfs,0xffabcdefu,0},3)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"cube-face"));
    set_rect(&h,0,0,9,1);
    assert(call(&h,h.device,35,(uint32_t[]){off,0xff000000u,RECT},3)==HR_INVALID);
    assert(strstr(d9_com_last_diagnostic(),"inside the surface"));
    /* Depth surface is a named refusal. */
    {uint32_t a[]={8,8,D9_D24S8,0,0,0,OUT,0};
     assert(call(&h,h.device,29,a,8)==HR_OK);uint32_t depth=rd(&h,OUT);assert(depth);
     assert(call(&h,h.device,35,(uint32_t[]){depth,0xff000000u,0},3)==HR_UNAVAILABLE);
     assert(strstr(d9_com_last_diagnostic(),"depth-stencil"));
     assert(call(&h,depth,2,NULL,0)==0);}

    /* Release all objects; surface views drop their parent reference. */
    uint32_t objects[]={ss,ds,ds2,xsurf,fs,rts,lockrt,cfs};
    for(unsigned i=0;i<sizeof objects/sizeof objects[0];++i)
        assert(call(&h,objects[i],2,NULL,0)==0);
    uint32_t texts[]={src,dst,x8,big,one,rtt,us,ud,ud2,xs,ft,off,cube};
    for(unsigned i=0;i<sizeof texts/sizeof texts[0];++i)
        assert(call(&h,texts[i],2,NULL,0)==0);
    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("UpdateTexture/UpdateSurface/ColorFill: PASS");
    return 0;
}
