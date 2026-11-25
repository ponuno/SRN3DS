#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x100)
#define OUT2 (SCRATCH+0x108)
#define PP (SCRATCH+0x200)
#define CAPS9 (SCRATCH+0x400)
#define PROBE_FN 0x00401000u

/* This test deliberately does not link d3d9_com_demo.c or d3d9_citro3d.c: it
 * proves the COM layer creates the D9Device itself through d3d9_core. */
typedef struct {unsigned created,deleted,begins,draws,ends;int active;} Mock;
typedef struct {size_t bytes;uint8_t *data;} Tex;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    Mock *m=ctx;Tex *t=calloc(1,sizeof *t);if(!t)return -1;t->bytes=(size_t)w*h*4*levels;
    t->data=malloc(t->bytes);if(!t->data){free(t);return -1;}*native=t;++m->created;return 0;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){
    (void)ctx;(void)native;(void)level;(void)data;(void)n;return 0;
}
static void tex_delete(void *ctx,void *native){Mock *m=ctx;Tex *t=native;free(t->data);free(t);++m->deleted;}
static int begin(void *ctx,uint32_t argb){Mock *m=ctx;(void)argb;m->active=1;++m->begins;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *tex,const D9DrawState *s){
    (void)ctx;(void)v;(void)n;(void)m;(void)tex;(void)s;return 0;
}
static void end(void *ctx){Mock *m=ctx;m->active=0;++m->ends;}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end};

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
/* Same stdcall frame shape the lifted COM call sites use. */
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

typedef struct {GuestMemory memory;CPU cpu;uint32_t factory;} Harness;
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
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}

int main(void){
    Harness h;memset(&h,0,sizeof h);Mock mock={0};
    guest_init(&h.memory,2u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x1000));
    /* Headless session: no D9Device and no backend yet. Enumeration must work. */
    assert(!d9_com_init(&h.memory,NULL));
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[2]={{"probe",probe,1},d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,2));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    PeImport import={.dll="d3d9.dll",.name="Direct3DCreate9"};
    uint32_t create9=d9_com_resolve(&import,NULL);assert(create9);
    uint32_t sdk=32,r;
    assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    assert(call(&h,h.factory,4,NULL,0)==1); /* GetAdapterCount */

    uint32_t mode[]={0,OUT};assert(call(&h,h.factory,8,mode,2)==0);
    assert(rd(&h,OUT)==400&&rd(&h,OUT+4)==240&&rd(&h,OUT+8)==60&&rd(&h,OUT+12)==D9_X8R8G8B8);
    uint32_t caps[]={0,1,CAPS9};assert(call(&h,h.factory,14,caps,3)==0);
    assert(rd(&h,CAPS9+196)==0xfffe0200u&&rd(&h,CAPS9+204)==0xffff0200u);
    uint32_t rt_fmt[]={0,1,D9_X8R8G8B8,1,1,D9_A8R8G8B8};
    assert(call(&h,h.factory,10,rt_fmt,6)==0);

    /* Presentation parameters: windowed 400x240, 32-bit, discard, one back buffer. */
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    guest_write(&h.memory,PP,pp,sizeof pp);
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* no backend yet */
    assert(rd(&h,OUT)==0);

    assert(d9_com_set_backend(&ops,&mock,1u<<20));

    pp[0]=640;guest_write(&h.memory,PP,pp,sizeof pp);
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* not the 400x240 mode */
    pp[0]=400;pp[8]=0;guest_write(&h.memory,PP,pp,sizeof pp);
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* fullscreen */
    pp[8]=1;pp[6]=2;guest_write(&h.memory,PP,pp,sizeof pp);
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* non-discard swap effect */
    pp[6]=1;pp[4]=2;guest_write(&h.memory,PP,pp,sizeof pp);
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* MSAA */
    pp[4]=0;pp[2]=23;guest_write(&h.memory,PP,pp,sizeof pp);
    assert(call(&h,h.factory,16,create,6)==0x8876086au); /* R5G6B5 */
    pp[2]=D9_X8R8G8B8;guest_write(&h.memory,PP,pp,sizeof pp);

    assert(call(&h,h.factory,16,create,6)==0);
    uint32_t device=0;assert(!guest_read32(&h.memory,OUT,&device)&&device);
    /* A created device must carry the real D9Device: texture creation reaches the backend. */
    uint32_t tex[]={64,64,1,0,D9_A8R8G8B8,0,OUT2,0};
    assert(call(&h,device,23,tex,8)==0);
    uint32_t texture=0;assert(!guest_read32(&h.memory,OUT2,&texture)&&texture);
    assert(mock.created==1);
    assert(call(&h,texture,2,NULL,0)==0&&mock.deleted==1);
    uint32_t no_target[]={16,16,D9_A8R8G8B8,0,0,1,OUT2,0};
    assert(call(&h,device,28,no_target,8)==0x8876086au&&rd(&h,OUT2)==0); /* old six-callback backend */
    assert(call(&h,device,2,NULL,0)==0); /* COM-owned D9Device is destroyed here */
    /* A second CreateDevice must build a fresh D9Device from the same backend. */
    assert(call(&h,h.factory,16,create,6)==0);
    assert(!guest_read32(&h.memory,OUT,&device)&&device);
    assert(call(&h,device,2,NULL,0)==0);

    d9_com_shutdown();n3ds_lift_release();guest_destroy(&h.memory);

    /* The backend-init entry point must also work without a demo or a GPU init. */
    GuestMemory memory2;guest_init(&memory2,1u<<20);
    assert(!d9_com_init_backend(&memory2,&ops,&mock,1u<<20));
    d9_com_shutdown();guest_destroy(&memory2);
    puts("D3D9 CreateDevice: COM-owned D9Device, 400x240x32 windowed mode, no demo/Citro3D dependency passed");
    (void)r;return 0;
}
