/* Lockable retained back buffer + explicit depth-stencil surface.
 *
 * This test drives the exact presentation set TESV was measured to request:
 * 400x240 X8R8G8B8, one back buffer, D3DSWAPEFFECT_DISCARD, no MSAA, windowed,
 * D3DPRESENT_INTERVAL_ONE and D3DPRESENTFLAG_LOCKABLE_BACKBUFFER.  The null
 * backend keeps real BGRA pixels, so the Present blit and the back-buffer
 * LockRect/UnlockRect round-trip are verified pixel by pixel, not only by
 * counting calls.  Present's destination is a private host image, which is
 * explicitly not a hardware framebuffer.
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "d3d9_capture.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x100)
#define OUT2 (SCRATCH+0x108)
#define PP (SCRATCH+0x200)
#define RECT (SCRATCH+0x600)
#define VERT (SCRATCH+0x700)
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
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t call1(Harness *h,uint32_t object,unsigned slot,uint32_t a){return call(h,object,slot,&a,1);}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static void wr(Harness *h,uint32_t address,const void *data,size_t bytes){assert(!guest_write(&h->memory,address,data,bytes));}
static void wr32(Harness *h,uint32_t address,uint32_t value){assert(!guest_write32(&h->memory,address,value));}
static uint32_t unref(Harness *h,uint32_t object){return call(h,object,2,NULL,0);}
static uint32_t fbits(float f){uint32_t u;memcpy(&u,&f,4);return u;}

/* The exact TESV request; Flags=1 is D3DPRESENTFLAG_LOCKABLE_BACKBUFFER. */
static const uint32_t tesv_pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,1,0,1};

static int harness_start(Harness *h){
    memset(h,0,sizeof *h);guest_init(&h->memory,2u<<20);
    assert(!guest_map(&h->memory,0x10000000,0x4000)&&!guest_map(&h->memory,SCRATCH,0x2000));
    assert(!d9_com_init(&h->memory,NULL));assert(!d9_null_register());
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    h->tables[0]=(FnModule){"probe",probe,1};h->tables[1]=d9_com_module();
    assert(!n3ds_lift_bind_modules(&h->memory,h->tables,2));
    n3ds_lift_init_cpu(&h->cpu,0x10004000,0);
    PeImport import={.dll="d3d9.dll",.name="Direct3DCreate9"};
    uint32_t create9=d9_com_resolve(&import,NULL);assert(create9);
    uint32_t sdk=32;assert(!invoke(h,create9,&sdk,1,&h->factory)&&h->factory);
    return 0;
}
static uint32_t create_device(Harness *h,const uint32_t *pp){
    wr(h,PP,pp,14*4);uint32_t create[]={0,1,0,0x20,PP,OUT};
    uint32_t hr=call(h,h->factory,16,create,6);
    if(hr)return 0;
    return rd(h,OUT);
}
static void present_frame(Harness *h,uint32_t device,uint32_t color,int clear,float z,uint32_t stencil){
    if(clear){
        unsigned flags=1|(z>=0.0f?6:0);
        uint32_t c[6]={0,0,flags,color,fbits(z),stencil};
        if(z<0.0f){c[2]=1;c[4]=0;c[5]=0;}
        assert(call(h,device,43,c,6)==0);
    }
    assert(call(h,device,41,NULL,0)==0);
    assert(call(h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(call(h,device,17,present,4)==0);
}
static const uint32_t *screen_pixels(unsigned *w,unsigned *h){
    const void *p=d9_null_present_pixels(w,h);
    assert(p&&*w==400&&*h==240);
    return p;
}
static void check_screen(uint32_t argb){
    unsigned w,h;const uint32_t *p=screen_pixels(&w,&h);
    for(unsigned i=0;i<w*h;++i)assert(p[i]==argb);
}
static void check_screen_pixel(unsigned x,unsigned y,uint32_t argb){
    unsigned w,h;const uint32_t *p=screen_pixels(&w,&h);
    assert(x<w&&y<h&&p[(size_t)y*w+x]==argb);
}

/* ---------------------------------------------------------------- back buffer */
static void back_buffer_flow(Harness *h,uint32_t device){
    uint32_t back=0;
    uint32_t bb[]={0,0,0,OUT};assert(call(h,device,18,bb,4)==0&&rd(h,OUT));
    back=rd(h,OUT);
    uint32_t desc[]={OUT2};assert(call(h,back,12,desc,1)==0);
    assert(rd(h,OUT2)==D9_X8R8G8B8&&rd(h,OUT2+24)==400&&rd(h,OUT2+28)==240);
    assert(unref(h,back)==1);

    const uint32_t color=0xff204060u;
    present_frame(h,device,color,1,-1.0f,0);
    check_screen(color);
    /* The retained target is 512x256; the frame must be rasterised with the
     * logical 400x240 viewport, not the padded storage size. */
    D9NullStats stats=*d9_null_stats();
    assert(stats.viewport_calls>0&&stats.viewport_w==400&&stats.viewport_h==240);

    /* LockRect after Present reads the retained pixels (they stay valid until
     * the next BeginScene, as DISCARD allows). */
    uint32_t lock[3]={OUT2,0,0x10};
    assert(call(h,back,13,lock,3)==0);
    uint32_t pitch=rd(h,OUT2),addr=rd(h,OUT2+4);
    assert(pitch==400*4&&addr);
    assert(rd(h,addr)==color);
    assert(rd(h,addr+pitch*239+399*4)==color);
    assert(call(h,back,14,NULL,0)==0);

    /* A writable lock really changes the retained surface. */
    lock[2]=0;assert(call(h,back,13,lock,3)==0);
    addr=rd(h,OUT2+4);assert(addr);
    wr32(h,addr+pitch*9+17*4,0xff112233u);
    wr32(h,addr,0xff445566u);
    assert(call(h,back,14,NULL,0)==0);
    lock[2]=0x10;assert(call(h,back,13,lock,3)==0);
    addr=rd(h,OUT2+4);
    assert(rd(h,addr+pitch*9+17*4)==0xff112233u);
    assert(rd(h,addr)==0xff445566u);
    assert(rd(h,addr+pitch*239+399*4)==color);
    assert(call(h,back,14,NULL,0)==0);

    /* The retained surface is a texture-like target, so StretchRect can now
     * sample the back buffer (previously impossible for the implicit screen). */
    uint32_t rt_args[]={512,256,D9_X8R8G8B8,0,0,1,OUT2,0};
    assert(call(h,device,28,rt_args,8)==0);uint32_t rt=rd(h,OUT2);assert(rt);
    int32_t srect[4]={0,0,400,240};wr(h,RECT,srect,16);
    int32_t drect[4]={0,0,400,240};wr(h,RECT+16,drect,16);
    uint32_t stretch[]={back,RECT,rt,RECT+16,0};assert(call(h,device,34,stretch,5)==0);
    uint32_t rtl[]={OUT2,0,0x10};assert(call(h,rt,13,rtl,3)==0);
    uint32_t rtpitch=rd(h,OUT2),rtaddr=rd(h,OUT2+4);assert(rtpitch==512*4&&rtaddr);
    assert(rd(h,rtaddr+rtpitch*9+17*4)==0xff112233u);
    assert(rd(h,rtaddr)==0xff445566u);
    assert(call(h,rt,14,NULL,0)==0);
    assert(unref(h,rt)==0);

    /* DISCARD: the next frame does not inherit those pixels.  A BeginScene
     * without Clear starts from zero, and Present shows that. */
    present_frame(h,device,0,0,-1.0f,0);
    check_screen(0u);
}

/* -------------------------------------------------------------------- depth */
static void depth_flow(Harness *h,uint32_t device){
    uint32_t depth_args[]={400,240,D9_D24S8,0,0,0,OUT2,0};
    assert(call(h,device,29,depth_args,8)==0);
    uint32_t depth=rd(h,OUT2);assert(depth);

    uint32_t bad[8]={512,256,D9_D24S8,0,0,0,OUT2,0};
    assert(call(h,device,29,bad,8)==0); /* any pow2 8..1024 descriptor exists */
    uint32_t d512=rd(h,OUT2);assert(d512);
    assert(call(h,device,39,(uint32_t[]){d512},1)==0x8876086au); /* active back buffer is 400x240 */
    assert(unref(h,d512)==0);
    bad[0]=400;bad[1]=240;bad[2]=80; /* D16 */
    assert(call(h,device,29,bad,8)==0x8876086au&&rd(h,OUT2)==0);
    bad[2]=D9_D24S8;bad[3]=2;assert(call(h,device,29,bad,8)==0x8876086au&&rd(h,OUT2)==0);
    bad[3]=0;bad[5]=1;assert(call(h,device,29,bad,8)==0x8876086au&&rd(h,OUT2)==0);
    bad[5]=0;bad[7]=1;assert(call(h,device,29,bad,8)==0x8876086cu&&rd(h,OUT2)==0);

    uint32_t set_depth[]={depth};assert(call(h,device,39,set_depth,1)==0);
    uint32_t get_depth[]={OUT2};assert(call(h,device,40,get_depth,1)==0&&rd(h,OUT2)==depth);
    assert(unref(h,depth)==2); /* caller + device binding reference */

    /* Combined colour + depth + stencil clear reaches the depth attachment. */
    D9NullStats before=*d9_null_stats();
    uint32_t clear[]={0,0,7,0xff204060u,fbits(0.5f),9};
    assert(call(h,device,43,clear,6)==0);
    assert(call(h,device,41,NULL,0)==0);
    D9NullStats after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_flags==7&&after.last_depth_clear==0x09800000u);
    assert(call(h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(call(h,device,17,present,4)==0);
    check_screen(0xff204060u);

    /* Detach, then a depth clear is a named invalid call (not silent success). */
    uint32_t detach[]={0};assert(call(h,device,39,detach,1)==0);
    assert(call(h,device,40,get_depth,1)==0x88760866u&&rd(h,OUT2)==0);
    clear[2]=6;assert(call(h,device,43,clear,6)==0x8876086cu);

    /* Attach again; a Z-only clear is accepted and packs stencil 0. */
    assert(call(h,device,39,set_depth,1)==0);
    clear[2]=2;clear[4]=fbits(0.25f);clear[5]=0;assert(call(h,device,43,clear,6)==0);
    before=*d9_null_stats();assert(call(h,device,41,NULL,0)==0);
    after=*d9_null_stats();
    assert(after.depth_clears==before.depth_clears+1);
    assert(after.last_depth_flags==2&&after.last_depth_clear==0x00400000u);
    assert(call(h,device,42,NULL,0)==0&&call(h,device,17,present,4)==0);

    /* An out-of-range z is rejected before anything is queued. */
    clear[2]=2;clear[4]=fbits(1.5f);assert(call(h,device,43,clear,6)==0x8876086cu);
    clear[4]=0x7fc00000u;assert(call(h,device,43,clear,6)==0x8876086cu);

    /* Non-depth objects are rejected by SetDepthStencilSurface. */
    uint32_t back=0;uint32_t bb[]={0,0,0,OUT};assert(call(h,device,18,bb,4)==0);back=rd(h,OUT);
    set_depth[0]=back;assert(call(h,device,39,set_depth,1)==0x8876086cu);
    assert(unref(h,back)==1);
    set_depth[0]=depth;assert(call(h,device,39,set_depth,1)==0);

    /* Reset detaches the explicit depth surface; the COM object survives until
     * the application releases it. */
    wr(h,PP,tesv_pp,14*4);uint32_t reset[]={PP};
    assert(call(h,device,16,reset,1)==0);
    uint32_t detach2[]={0};assert(call(h,device,39,detach2,1)==0);
    assert(unref(h,depth)==0);
    assert(call(h,device,16,reset,1)==0);
    /* Reset drops the binding; the device starts without a depth surface. */
    get_depth[0]=OUT2;assert(call(h,device,40,get_depth,1)==0x88760866u&&rd(h,OUT2)==0);
}

/* -------------------------------------------------------------- capture path */
static void capture_flow(Harness *h,uint32_t device){
    D9CaptureOptions options;d9_capture_options_init(&options);
    options.directory=".";options.basename="backbuffer_lockable_capture";
    options.max_records=1;options.max_presents=1;
    assert(d9_capture_init(&options)==D9_CAPTURE_OK);
    uint32_t fvf=0x102;assert(call(h,device,89,&fvf,1)==0);
    D9Vertex verts[3]={{-1,-1,0,0,0},{1,-1,0,1,0},{0,1,0,0.5f,1}};
    wr(h,VERT,verts,sizeof verts);
    uint32_t clear[]={0,0,1,0xff336699u,0,0};assert(call(h,device,43,clear,6)==0);
    assert(call(h,device,41,NULL,0)==0);
    uint32_t up[]={D9_TRIANGLELIST,1,VERT,sizeof(D9Vertex)};
    assert(call(h,device,83,up,4)==0);
    assert(call(h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(call(h,device,17,present,4)==0);
    assert(d9_capture_count()>=1&&d9_capture_present_count()>=1);
    D9CaptureRecordInfo draw_info;D9CapturePresentInfo present_info;
    assert(!d9_capture_record(0,&draw_info));
    assert(!d9_capture_present_record(0,&present_info));
    FILE *f=fopen(draw_info.text_path,"r");assert(f);
    char text[4096];size_t n=fread(text,1,sizeof text-1,f);text[n]=0;fclose(f);
    assert(strstr(text,"target.implicit=1")&&strstr(text,"target.width=400")&&strstr(text,"target.height=240"));
    f=fopen(present_info.text_path,"r");assert(f);
    n=fread(text,1,sizeof text-1,f);text[n]=0;fclose(f);
    assert(strstr(text,"target.implicit=1")&&strstr(text,"target.width=400"));
    d9_capture_shutdown();
    check_screen(0xff336699u);
}

static void set_cap_env(const char *v){
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TARGET_NATIVE_MAX",v?v:"");
#else
    if(v&&*v)setenv("TESV_D3D9_TARGET_NATIVE_MAX",v,1);
    else unsetenv("TESV_D3D9_TARGET_NATIVE_MAX");
#endif
}

int main(void){
    set_cap_env("512"); /* this regression exercises the 512x256 readable target */
    Harness h;assert(!harness_start(&h));

    /* Depth-stencil format reporting matches what CreateDepthStencilSurface
     * really accepts; nothing else is advertised. */
    uint32_t cds[]={0,1,D9_X8R8G8B8,D9_X8R8G8B8,D9_D24S8};
    assert(call(&h,h.factory,12,cds,5)==0);
    cds[4]=D9_D24X8;assert(call(&h,h.factory,12,cds,5)==0);
    cds[4]=80;assert(call(&h,h.factory,12,cds,5)==0x8876086au);
    uint32_t cfmt[]={0,1,D9_X8R8G8B8,0,1,D9_D24S8};
    assert(call(&h,h.factory,10,cfmt,6)==0);
    cfmt[3]=1;assert(call(&h,h.factory,10,cfmt,6)==0x8876086au);

    /* Wrong presentation parameters stay rejected, including the size/other
     * flag cases, and no lockable promise without a capable backend. */
    uint32_t pp[14];memcpy(pp,tesv_pp,sizeof pp);
    pp[0]=640;wr(&h,PP,pp,14*4);uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==0x8876086au&&rd(&h,OUT)==0);
    memcpy(pp,tesv_pp,sizeof pp);pp[11]=2;wr(&h,PP,pp,14*4);
    assert(call(&h,h.factory,16,create,6)==0x8876086au&&rd(&h,OUT)==0);
    memcpy(pp,tesv_pp,sizeof pp);pp[3]=2;wr(&h,PP,pp,14*4);
    assert(call(&h,h.factory,16,create,6)==0x8876086au&&rd(&h,OUT)==0);
    memcpy(pp,tesv_pp,sizeof pp);pp[4]=2;wr(&h,PP,pp,14*4);
    assert(call(&h,h.factory,16,create,6)==0x8876086au&&rd(&h,OUT)==0);

    uint32_t device=create_device(&h,tesv_pp);assert(device);

    /* The retained buffer really exists at 512x256 and the presentation size
     * stays 400x240. */
    uint32_t bb[]={0,0,0,OUT};assert(call(&h,device,18,bb,4)==0&&rd(&h,OUT));
    uint32_t back=rd(&h,OUT);uint32_t desc[]={OUT2};assert(call(&h,back,12,desc,1)==0);
    assert(rd(&h,OUT2+24)==400&&rd(&h,OUT2+28)==240&&rd(&h,OUT2+4)==1);
    assert(unref(&h,back)==1);

    /* Without the flag the same set is accepted and the back buffer is not
     * lockable.  Recreate for the real feature tests afterwards. */
    back_buffer_flow(&h,device);
    depth_flow(&h,device);      /* ends with a successful Reset on tesv_pp */
    capture_flow(&h,device);
    back_buffer_flow(&h,device); /* Reset left a fresh retained back buffer */

    /* LockRect on a fresh non-lockable device must fail explicitly. */
    memcpy(pp,tesv_pp,sizeof pp);pp[11]=0;wr(&h,PP,pp,14*4);
    uint32_t reset[]={PP};assert(call(&h,device,16,reset,1)==0);
    uint32_t lock[3]={OUT2,0,0x10};
    assert(call(&h,device,18,bb,4)==0);back=rd(&h,OUT);
    assert(call(&h,back,13,lock,3)==0x8876086au&&rd(&h,OUT2)==0&&rd(&h,OUT2+4)==0);
    assert(unref(&h,back)==1);
    /* A non-lockable device still presents through the implicit screen; the
     * null backend has no screen pixels to inspect here. */
    present_frame(&h,device,0xff00ff00u,1,-1.0f,0);

    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    d9_com_shutdown();n3ds_lift_release();guest_destroy(&h.memory);
    puts("D3D9 lockable back buffer + depth-stencil: exact TESV presentation set, pixel Present, "
         "LockRect read/write, StretchRect sampling, depth attach/clear/detach, Reset and rejections passed");
    return 0;
}
