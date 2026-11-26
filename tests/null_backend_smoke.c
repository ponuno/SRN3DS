#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "d3d9_capture.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x100)
#define OUT2 (SCRATCH+0x108)
#define PP (SCRATCH+0x200)
#define CAPS9 (SCRATCH+0x400)
#define RECT (SCRATCH+0x600)
#define VIEW (SCRATCH+0x700)
#define DESC (SCRATCH+0x800)
#define GUID (SCRATCH+0x900)
#define CREATION (SCRATCH+0xa00)
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
    if(guest_read32(&h->memory,object,&vt)||!vt){
        fprintf(stderr,"null_backend: bad COM object 0x%08x for slot %u (vtable 0x%08x)\n",object,slot,vt);
        assert(0);
    }
    if(guest_read32(&h->memory,vt+slot*4,&fn)||!fn){
        fprintf(stderr,"null_backend: bad COM vtable 0x%08x slot %u\n",vt,slot);
        assert(0);
    }
    uint32_t frame[18]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t call1(Harness *h,uint32_t object,unsigned slot,uint32_t a){uint32_t args[1]={a};return call(h,object,slot,args,1);}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static void wr(Harness *h,uint32_t address,const void *data,size_t bytes){assert(!guest_write(&h->memory,address,data,bytes));}
static uint32_t unref(Harness *h,uint32_t object){return call(h,object,2,NULL,0);}
static uint32_t refs(Harness *h,uint32_t object){uint32_t n=call(h,object,1,NULL,0);if(!n)return 0;uint32_t after=unref(h,object);assert(after==n-1);return after;}
static int file_contains(const char *path,const char *needle){
    FILE *f=fopen(path,"r");if(!f)return 0;char line[512];int found=0;
    while(fgets(line,sizeof line,f))if(strstr(line,needle)){found=1;break;}
    fclose(f);return found;
}
static uint32_t fbits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static void present_params(Harness *h,const uint32_t *pp){wr(h,PP,pp,14*4);}
static const uint32_t good_pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
static const uint8_t iid_unsupported[16]={0};
static const uint8_t iid_iunknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_surface[16]={0x3a,0xaf,0xfb,0x0c,0xf6,0x99,0x4a,0x29,0x99,0xb3,0xa2,0x79,0x6a,0xf8,0xb8,0x9b};
static const uint8_t iid_resource[16]={0x5d,0x05,0xec,0x5e,0x7d,0x8f,0x62,0x43,0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,4};
static const uint8_t iid_device[16]={0x96,0x3b,0x22,0xd0,0x7a,0xbf,0xfd,0x43,0x92,0xbd,0xa4,0x3b,0x0d,0x82,0xb9,0xeb};

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
static uint32_t harness_device(Harness *h){
    present_params(h,good_pp);uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(h,h->factory,16,create,6)==0);
    uint32_t device=rd(h,OUT);assert(device);return device;
}
static void harness_stop(Harness *h){d9_com_shutdown();n3ds_lift_release();guest_destroy(&h->memory);}

/* A successful, quiet call stream for manually checking TESV_D3D9_TRACE. */
static int trace_only(int with_unsupported){
    Harness h;assert(!harness_start(&h));uint32_t device=harness_device(&h);
    assert(call(&h,h.factory,4,NULL,0)==1);
    uint32_t mode[]={0,OUT};assert(call(&h,h.factory,8,mode,2)==0&&rd(&h,OUT)==400&&rd(&h,OUT+4)==240);
    uint32_t reset[]={PP};assert(call(&h,device,16,reset,1)==0);
    present_params(&h,good_pp);
    uint32_t clear[]={0,0,1,0xff102030u,0,0};assert(call(&h,device,43,clear,6)==0);
    assert(call(&h,device,41,NULL,0)==0);assert(call(&h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(call(&h,device,17,present,4)==0);
    uint32_t rt[]={0,OUT2};assert(call(&h,device,38,rt,2)==0);assert(unref(&h,rd(&h,OUT2))==1);
    if(with_unsupported){
        uint32_t unsupported[]={16,16,75,0,0,0,OUT2,0};
        uint32_t r=call(&h,device,29,unsupported,8);assert(r==0x8876086au&&rd(&h,OUT2)==0);
    }
    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    harness_stop(&h);return 0;
}

int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--trace-only"))return trace_only(0);
    if(argc==2&&!strcmp(argv[1],"--unsupported-trace"))return trace_only(1);
    Harness h;assert(!harness_start(&h));

    /* D3DPERF exports are real stubs in the d3d9 module, not fault traps. */
    PeImport perf={.dll="d3d9.dll",.name="D3DPERF_SetOptions"};
    uint32_t fn=d9_com_resolve(&perf,NULL);assert(fn);
    uint32_t option=0,r;assert(!invoke(&h,fn,&option,1,&r)&&r==0);
    perf.name="D3DPERF_BeginEvent";fn=d9_com_resolve(&perf,NULL);assert(fn);
    uint32_t begin_args[2]={0,0};assert(!invoke(&h,fn,begin_args,2,&r)&&r==0);
    perf.name="D3DPERF_EndEvent";fn=d9_com_resolve(&perf,NULL);assert(fn);
    assert(!invoke(&h,fn,NULL,0,&r)&&r==0);
    perf.name="D3DPERF_GetStatus";fn=d9_com_resolve(&perf,NULL);assert(fn);
    assert(!invoke(&h,fn,NULL,0,&r)&&r==0);

    assert(call(&h,h.factory,4,NULL,0)==1);
    uint32_t caps[]={0,1,CAPS9};assert(call(&h,h.factory,14,caps,3)==0);
    assert(rd(&h,CAPS9+196)==0xfffe0200u&&rd(&h,CAPS9+204)==0xffff0200u);
    assert(rd(&h,CAPS9+240)==1u);
    assert(rd(&h,CAPS9+296)==0u&&rd(&h,CAPS9+300)==0u); /* no SM3 */
    uint32_t fmt[]={0,1,D9_X8R8G8B8,0,1,D9_A8R8G8B8};
    assert(call(&h,h.factory,10,fmt,6)==0); /* lockable CPU plain surface format */
    fmt[3]=1;assert(call(&h,h.factory,10,fmt,6)==0); /* one LDR render-target surface */
    fmt[4]=3;assert(call(&h,h.factory,10,fmt,6)==0x8876086au); /* no render-target texture */
    fmt[4]=1;fmt[3]=0x00020001u;assert(call(&h,h.factory,10,fmt,6)==0x8876086au);
    fmt[3]=0;fmt[5]=75;assert(call(&h,h.factory,10,fmt,6)==0); /* D24S8 depth surface */
    fmt[5]=77;assert(call(&h,h.factory,10,fmt,6)==0); /* D24X8, same attachment */
    fmt[5]=80;assert(call(&h,h.factory,10,fmt,6)==0x8876086au); /* D16 */
    fmt[5]=75;fmt[3]=1;assert(call(&h,h.factory,10,fmt,6)==0x8876086au); /* depth is not a colour RT */
    uint32_t check_type[]={0,1,D9_X8R8G8B8,D9_X8R8G8B8,1};
    assert(call(&h,h.factory,9,check_type,5)==0);

    uint32_t device=harness_device(&h);
    assert(refs(&h,device)==1);

    /* Device queries and the implicit back buffer use real D3D9 slot numbers. */
    uint32_t mode[]={0,OUT};assert(call(&h,device,8,mode,2)==0);
    assert(rd(&h,OUT)==400&&rd(&h,OUT+4)==240&&rd(&h,OUT+8)==60&&rd(&h,OUT+12)==D9_X8R8G8B8);
    mode[0]=1;assert(call(&h,device,8,mode,2)==0x8876086cu);
    mode[0]=0;mode[1]=0;assert(call(&h,device,8,mode,2)==0x8876086cu);
    assert(call1(&h,device,9,CREATION)==0);
    assert(rd(&h,CREATION)==0&&rd(&h,CREATION+4)==1&&rd(&h,CREATION+8)==0&&rd(&h,CREATION+12)==0x20);
    assert(call(&h,device,15,NULL,0)==1);
    uint32_t bb[]={0,0,0,OUT};assert(call(&h,device,18,bb,4)==0);
    uint32_t back=rd(&h,OUT);assert(back&&refs(&h,back)==2);
    bb[0]=1;assert(call(&h,device,18,bb,4)==0x8876086cu&&rd(&h,OUT)==0);
    bb[0]=0;bb[1]=1;assert(call(&h,device,18,bb,4)==0x8876086cu);
    bb[1]=0;bb[2]=1;assert(call(&h,device,18,bb,4)==0x8876086cu);
    bb[2]=0;bb[3]=0;assert(call(&h,device,18,bb,4)==0x8876086cu);

    uint32_t desc[]={DESC};assert(call(&h,back,12,desc,1)==0);
    assert(rd(&h,DESC)==D9_X8R8G8B8&&rd(&h,DESC+4)==1&&rd(&h,DESC+8)==1&&rd(&h,DESC+12)==0);
    assert(rd(&h,DESC+16)==0&&rd(&h,DESC+20)==0&&rd(&h,DESC+24)==400&&rd(&h,DESC+28)==240);
    assert(call(&h,back,10,NULL,0)==1);
    uint32_t get_device[]={OUT2};assert(call(&h,back,3,get_device,1)==0&&rd(&h,OUT2)==device&&refs(&h,device)==2);
    assert(unref(&h,device)==1);
    wr(&h,GUID,iid_surface,16);uint32_t qi[]={GUID,OUT2};assert(call(&h,back,0,qi,2)==0&&rd(&h,OUT2)==back&&refs(&h,back)==3);
    assert(unref(&h,back)==2);
    wr(&h,GUID,iid_resource,16);assert(call(&h,back,0,qi,2)==0&&rd(&h,OUT2)==back&&refs(&h,back)==3);
    assert(unref(&h,back)==2);
    wr(&h,GUID,iid_device,16);uint32_t container[]={GUID,OUT2};
    assert(call(&h,back,11,container,2)==0x8876086au&&rd(&h,OUT2)==0);
    uint32_t lock[]={OUT2,0,0};assert(call(&h,back,13,lock,3)==0x8876086au&&rd(&h,OUT2)==0&&rd(&h,OUT2+4)==0);
    uint32_t dc[]={OUT2};assert(call(&h,back,15,dc,1)==0x8876086au&&rd(&h,OUT2)==0);
    assert(unref(&h,back)==1); /* device's implicit reference */

    /* Render-target state starts at the implicit back buffer; NULL restores it. */
    uint32_t get_rt[]={0,OUT2};assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==back&&refs(&h,back)==2);
    assert(unref(&h,back)==1);
    get_rt[0]=1;assert(call(&h,device,38,get_rt,2)==0x88760866u&&rd(&h,OUT2)==0);
    uint32_t set_rt[]={0,back};assert(call(&h,device,37,set_rt,2)==0);
    set_rt[0]=1;assert(call(&h,device,37,set_rt,2)==0x8876086cu);
    set_rt[0]=0;set_rt[1]=0;assert(call(&h,device,37,set_rt,2)==0);
    get_rt[0]=0;assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==back);assert(unref(&h,back)==1);
    set_rt[1]=back;assert(call(&h,device,37,set_rt,2)==0);

    uint32_t vp[6]={0,0,400,240,0,0x3f800000u};wr(&h,VIEW,vp,24);
    assert(call1(&h,device,48,VIEW)==0);
    vp[0]=10;vp[1]=20;vp[2]=100;vp[3]=60;vp[4]=fbits(0.1f);vp[5]=fbits(0.9f);wr(&h,VIEW,vp,24);
    assert(call1(&h,device,47,VIEW)==0x8876086au); /* valid but unsupported viewport */
    assert(call1(&h,device,48,VIEW)==0);
    assert(rd(&h,VIEW)==0&&rd(&h,VIEW+4)==0&&rd(&h,VIEW+8)==400&&rd(&h,VIEW+12)==240);
    vp[2]=0;wr(&h,VIEW,vp,24);assert(call1(&h,device,47,VIEW)==0x8876086cu);
    vp[2]=391;wr(&h,VIEW,vp,24);assert(call1(&h,device,47,VIEW)==0x8876086cu);
    vp[0]=0;vp[2]=100;vp[4]=fbits(0.8f);vp[5]=fbits(0.2f);wr(&h,VIEW,vp,24);
    assert(call1(&h,device,47,VIEW)==0x8876086cu);
    vp[4]=0;vp[5]=fbits(1.1f);wr(&h,VIEW,vp,24);assert(call1(&h,device,47,VIEW)==0x8876086cu);
    assert(call(&h,device,37,set_rt,2)==0);assert(call1(&h,device,48,VIEW)==0);
    assert(rd(&h,VIEW)==0&&rd(&h,VIEW+8)==400&&rd(&h,VIEW+12)==240);

    /* D3D9 defaults: WRAP, POINT min/mag, CULL_CCW and alpha LESS. */
    uint32_t state_args[]={22,OUT};assert(call(&h,device,58,state_args,2)==0&&rd(&h,OUT)==2);
    state_args[0]=25;assert(call(&h,device,58,state_args,2)==0&&rd(&h,OUT)==2);
    uint32_t sampler_args[]={0,1,OUT};assert(call(&h,device,68,sampler_args,3)==0&&rd(&h,OUT)==1);
    sampler_args[1]=2;assert(call(&h,device,68,sampler_args,3)==0&&rd(&h,OUT)==1);
    sampler_args[1]=5;assert(call(&h,device,68,sampler_args,3)==0&&rd(&h,OUT)==1);
    sampler_args[1]=6;assert(call(&h,device,68,sampler_args,3)==0&&rd(&h,OUT)==1);

    /* The menu material source observes c0..c7; c4 is stored separately so it
     * cannot pollute the c0..c3 clip rows.  c8 remains invalid. */
    float constants[4][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{0,0,0,1}};wr(&h,VIEW,constants,sizeof constants);
    uint32_t const_args[]={8,VIEW,1};assert(call(&h,device,94,const_args,3)==0x8876086cu);
    const_args[0]=3;const_args[2]=2;assert(call(&h,device,94,const_args,3)==0);
    const_args[0]=0;const_args[2]=4;assert(call(&h,device,94,const_args,3)==0);
    const_args[0]=4;const_args[2]=1;assert(call(&h,device,94,const_args,3)==0);
    uint32_t get_const[]={0,OUT,4};assert(call(&h,device,95,get_const,3)==0);
    assert(rd(&h,OUT)==0x3f800000u&&rd(&h,OUT+12)==0x3f800000u);
    uint32_t get_high[]={4,OUT2,1};assert(call(&h,device,95,get_high,3)==0);
    assert(rd(&h,OUT2)==0x3f800000u);

    /* CPU-backed off-screen plain surfaces have real bytes, not fake pixels. */
    uint32_t surface_args[]={7,5,D9_A8R8G8B8,2,OUT2,0};assert(call(&h,device,36,surface_args,6)==0);
    uint32_t surface=rd(&h,OUT2);assert(surface&&refs(&h,surface)==1&&refs(&h,device)==2);
    surface_args[1]=0;assert(call(&h,device,36,surface_args,6)==0x8876086cu&&rd(&h,OUT2)==0);
    surface_args[1]=5;surface_args[2]=D9_DXT1;assert(call(&h,device,36,surface_args,6)==0x8876086au);
    surface_args[2]=D9_A8R8G8B8;surface_args[3]=1;assert(call(&h,device,36,surface_args,6)==0x8876086cu);
    surface_args[3]=0;assert(call(&h,device,36,surface_args,6)==0x8876086au);
    surface_args[3]=2;surface_args[5]=OUT;assert(call(&h,device,36,surface_args,6)==0x8876086cu);
    surface_args[5]=0;surface_args[4]=0;assert(call(&h,device,36,surface_args,6)==0x8876086cu);
    assert(call(&h,surface,12,desc,1)==0);
    assert(rd(&h,DESC)==D9_A8R8G8B8&&rd(&h,DESC+4)==1&&rd(&h,DESC+8)==0&&rd(&h,DESC+12)==2);
    assert(rd(&h,DESC+24)==7&&rd(&h,DESC+28)==5);
    get_device[0]=OUT;assert(call(&h,surface,3,get_device,1)==0&&rd(&h,OUT)==device&&refs(&h,device)==3);
    assert(unref(&h,device)==2);
    wr(&h,GUID,iid_unsupported,16);container[0]=GUID;container[1]=OUT;
    assert(call(&h,surface,11,container,2)==0x80004002u&&rd(&h,OUT)==0);
    wr(&h,GUID,iid_iunknown,16); /* IUnknown GUID */
    assert(call(&h,surface,11,container,2)==0&&rd(&h,OUT)==device&&refs(&h,device)==3);
    assert(unref(&h,device)==2);
    assert(refs(&h,surface)==1);

    lock[0]=OUT;assert(call(&h,surface,13,lock,3)==0&&rd(&h,OUT)==28);
    uint32_t pixels=rd(&h,OUT+4);assert(pixels&&rd(&h,pixels)==0);
    assert(!guest_write32(&h.memory,pixels,0x11223344));
    assert(call(&h,surface,14,NULL,0)==0);
    lock[2]=0x10;assert(call(&h,surface,13,lock,3)==0);pixels=rd(&h,OUT+4);
    assert(!guest_write32(&h.memory,pixels,0xdeadbeef)&&call(&h,surface,14,NULL,0)==0);
    lock[2]=0;assert(call(&h,surface,13,lock,3)==0);pixels=rd(&h,OUT+4);
    assert(rd(&h,pixels)==0x11223344);
    lock[0]=OUT;assert(call(&h,surface,13,lock,3)==0x8876086cu); /* already locked */
    assert(call(&h,surface,14,NULL,0)==0&&call(&h,surface,14,NULL,0)==0x8876086cu);
    int32_t rect[4]={1,1,5,4};wr(&h,RECT,rect,16);lock[1]=RECT;
    assert(call(&h,surface,13,lock,3)==0&&rd(&h,OUT)==28);pixels=rd(&h,OUT+4);assert(pixels);
    assert(!guest_write32(&h.memory,pixels+28,0x55667788));assert(call(&h,surface,14,NULL,0)==0);
    lock[1]=RECT;assert(call(&h,surface,13,lock,3)==0);pixels=rd(&h,OUT+4);assert(rd(&h,pixels+28)==0x55667788);
    assert(call(&h,surface,14,NULL,0)==0);
    rect[2]=rect[0];wr(&h,RECT,rect,16);assert(call(&h,surface,13,lock,3)==0x8876086cu);
    rect[0]=-1;rect[2]=5;wr(&h,RECT,rect,16);assert(call(&h,surface,13,lock,3)==0x8876086cu);
    set_rt[1]=surface;assert(call(&h,device,37,set_rt,2)==0x8876086cu);set_rt[1]=back;
    uint32_t reset[]={PP};assert(call(&h,device,16,reset,1)==0x8876086cu); /* explicit resource alive */
    assert(unref(&h,surface)==0&&refs(&h,device)==1);

    /* One explicit non-lockable LDR target has a real core/native lifetime and
     * a device-side binding reference. */
    d9_null_reset();
    uint32_t rt_args[]={16,16,D9_A8R8G8B8,0,0,0,OUT2,0};
    assert(call(&h,device,28,rt_args,8)==0);uint32_t rt=rd(&h,OUT2);assert(rt&&refs(&h,rt)==1&&refs(&h,device)==2);
    assert(call(&h,rt,12,desc,1)==0);
    assert(rd(&h,DESC)==D9_A8R8G8B8&&rd(&h,DESC+4)==1&&rd(&h,DESC+8)==1&&rd(&h,DESC+12)==0);
    assert(rd(&h,DESC+24)==16&&rd(&h,DESC+28)==16);
    get_device[0]=OUT;assert(call(&h,rt,3,get_device,1)==0&&rd(&h,OUT)==device&&refs(&h,device)==3);
    assert(unref(&h,device)==2);
    wr(&h,GUID,iid_iunknown,16);container[0]=GUID;container[1]=OUT;
    assert(call(&h,rt,11,container,2)==0&&rd(&h,OUT)==device&&refs(&h,device)==3);
    assert(unref(&h,device)==2);
    set_rt[0]=0;set_rt[1]=rt;assert(call(&h,device,37,set_rt,2)==0);
    assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==rt&&refs(&h,rt)==3);
    assert(unref(&h,rt)==2); /* GetRenderTarget reference */
    assert(refs(&h,rt)==2); /* caller + binding references */
    assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==rt&&refs(&h,rt)==3);
    assert(unref(&h,rt)==2);
    assert(call1(&h,device,48,VIEW)==0&&rd(&h,VIEW+8)==16&&rd(&h,VIEW+12)==16);
    set_rt[1]=0;assert(call(&h,device,37,set_rt,2)==0&&refs(&h,device)==2);

    uint32_t target_clear[]={0,0,1,0xff0000ffu,0,0};
    uint32_t target_present[]={0,0,0,0};
    assert(call(&h,device,43,target_clear,6)==0);
    set_rt[1]=rt;assert(call(&h,device,37,set_rt,2)==0x8876086cu); /* clear belongs to back buffer */
    get_rt[0]=0;assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==back);assert(unref(&h,back)==1);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,42,NULL,0)==0&&call(&h,device,17,target_present,4)==0);

    set_rt[1]=rt;assert(call(&h,device,37,set_rt,2)==0);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,42,NULL,0)==0&&call(&h,device,17,target_present,4)==0); /* no Clear: no target clear */
    target_clear[3]=0xff112233u;assert(call(&h,device,43,target_clear,6)==0);
    set_rt[1]=0;assert(call(&h,device,37,set_rt,2)==0x8876086cu); /* clear belongs to rt */
    get_rt[0]=0;assert(call(&h,device,38,get_rt,2)==0&&rd(&h,OUT2)==rt);assert(unref(&h,rt)==2);
    set_rt[1]=rt;assert(call(&h,device,37,set_rt,2)==0); /* same owner is allowed */
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,42,NULL,0)==0&&call(&h,device,17,target_present,4)==0);
    lock[0]=OUT2;lock[1]=0;lock[2]=0x10;
    assert(call(&h,rt,13,lock,3)==0x8876086cu&&rd(&h,OUT2)==0&&rd(&h,OUT2+4)==0); /* not lockable */
    lock[2]=0;assert(call(&h,rt,13,lock,3)==0x8876086cu);
    assert(call(&h,rt,14,NULL,0)==0x8876086cu);
    set_rt[1]=0;assert(call(&h,device,37,set_rt,2)==0);assert(unref(&h,rt)==0&&refs(&h,device)==1);
    D9NullStats targets=*d9_null_stats();
    assert(targets.target_creates==1&&targets.target_deletes==1&&targets.target_binds==3);
    assert(targets.target_clears==1&&targets.target_draws==0);
    assert(targets.last_clear_argb==0xff112233u&&!targets.scene);
    d9_null_reset();

    /* The null backend keeps real pixels, so a lockable target is admissible.
     * Lock/read/write go through the same core path the retained back buffer
     * uses. */
    rt_args[5]=1;assert(call(&h,device,28,rt_args,8)==0&&rd(&h,OUT2)!=0);
    uint32_t lockable_rt=rd(&h,OUT2);
    assert(call(&h,lockable_rt,13,lock,3)==0); /* full read */
    uint32_t lockable_pixels=rd(&h,OUT2+4);assert(lockable_pixels&&rd(&h,lockable_pixels)==0);
    assert(call(&h,lockable_rt,14,NULL,0)==0);
    lock[2]=0;assert(call(&h,lockable_rt,13,lock,3)==0); /* write */
    assert(!guest_write32(&h.memory,rd(&h,OUT2+4)+4*3,0xff556677u));
    assert(call(&h,lockable_rt,14,NULL,0)==0);
    lock[2]=0x10;assert(call(&h,lockable_rt,13,lock,3)==0); /* read back */
    assert(rd(&h,rd(&h,OUT2+4)+4*3)==0xff556677u);
    assert(call(&h,lockable_rt,14,NULL,0)==0);
    assert(unref(&h,lockable_rt)==0);
    rt_args[5]=0;

    rt_args[3]=2;assert(call(&h,device,28,rt_args,8)==0x8876086au&&rd(&h,OUT2)==0);
    rt_args[3]=0;rt_args[4]=1;assert(call(&h,device,28,rt_args,8)==0x8876086au&&rd(&h,OUT2)==0);
    rt_args[4]=0;rt_args[0]=0;assert(call(&h,device,28,rt_args,8)==0x8876086cu&&rd(&h,OUT2)==0);
    rt_args[0]=7;assert(call(&h,device,28,rt_args,8)==0x8876086cu&&rd(&h,OUT2)==0);
    rt_args[0]=16;rt_args[2]=D9_DXT1;assert(call(&h,device,28,rt_args,8)==0x8876086au&&rd(&h,OUT2)==0);
    rt_args[2]=D9_A8R8G8B8;rt_args[7]=OUT;assert(call(&h,device,28,rt_args,8)==0x8876086cu&&rd(&h,OUT2)==0);

    /* Unsupported depth paths return errors and null outputs. */
    uint32_t no_depth[]={16,16,75,0,0,0,OUT2,0};
    assert(call(&h,device,29,no_depth,8)==0x8876086au&&rd(&h,OUT2)==0);
    uint32_t no_z[]={0};assert(call(&h,device,39,no_z,1)==0);
    no_z[0]=back;assert(call(&h,device,39,no_z,1)==0x8876086cu);
    uint32_t get_z[]={OUT2};assert(call(&h,device,40,get_z,1)==0x88760866u&&rd(&h,OUT2)==0);
    uint32_t autogen[]={16,16,0,0x400,D9_A8R8G8B8,0,OUT2,0};
    assert(call(&h,device,23,autogen,8)==0x8876086au&&rd(&h,OUT2)==0);
    autogen[3]=0;autogen[5]=3;assert(call(&h,device,23,autogen,8)==0x8876086au);

    /* Clear rejects rectangles and multiple queued clears instead of discarding them. */
    uint32_t clear[]={0,0,0,0xff102030u,0x7fc00000u,0xffffffffu};
    assert(call(&h,device,43,clear,6)==0x8876086cu);
    const unsigned bad_flags[]={2,3,4,7,8};
    for(unsigned i=0;i<sizeof bad_flags/sizeof *bad_flags;++i){clear[2]=bad_flags[i];assert(call(&h,device,43,clear,6)==0x8876086cu);}
    clear[2]=1;clear[1]=1;assert(call(&h,device,43,clear,6)==0x8876086cu);
    clear[1]=RECT;assert(call(&h,device,43,clear,6)==0x8876086cu);
    clear[1]=0;assert(call(&h,device,43,clear,6)==0);
    rect[0]=0;rect[1]=0;rect[2]=400;rect[3]=240;wr(&h,RECT,rect,16);
    clear[0]=1;clear[1]=RECT;clear[2]=1;assert(call(&h,device,43,clear,6)==0x8876086au);
    clear[0]=0;clear[1]=0;assert(call(&h,device,43,clear,6)==0x8876086au); /* second queued clear */
    D9NullStats cleared=*d9_null_stats();assert(cleared.clears==1&&cleared.clear_targets==1&&cleared.last_clear_flags==1);
    uint32_t consume_clear[]={0,0,0,0};assert(call(&h,device,41,NULL,0)==0&&call(&h,device,42,NULL,0)==0&&call(&h,device,17,consume_clear,4)==0);
    d9_null_reset();

    /* Preserve the strict BeginScene/EndScene/Present state machine. */
    assert(call(&h,device,42,NULL,0)==0x8876086cu);
    uint32_t present[]={0,0,0,0};assert(call(&h,device,17,present,4)==0x8876086cu);
    uint32_t vb_args[]={120,0,0x102,0,OUT,0};
    assert(call(&h,device,26,vb_args,6)==0);uint32_t vb=rd(&h,OUT);assert(vb);
    uint32_t vb_lock[]={0,0,OUT,0};assert(call(&h,vb,11,vb_lock,4)==0);
    uint32_t vb_ptr=rd(&h,OUT);assert(vb_ptr);
    D9Vertex verts[6]={{-1,-1,0,0,0},{1,-1,0,1,0},{0,1,0,0.5f,1},
                       {-1,0,0,0,0},{1,0,0,1,0},{0,1,0,0.5f,1}};
    wr(&h,vb_ptr,verts,sizeof verts);assert(call(&h,vb,12,NULL,0)==0);
    uint32_t fvf=0x102;assert(call(&h,device,89,&fvf,1)==0);
    uint32_t stream[]={0,vb,0,sizeof(D9Vertex)};assert(call(&h,device,100,stream,4)==0);
    clear[0]=0;clear[1]=0;clear[2]=1;assert(call(&h,device,43,clear,6)==0);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,41,NULL,0)==0x8876086cu);
    assert(call(&h,device,43,clear,6)==0x8876086cu&&call(&h,device,17,present,4)==0x8876086cu);
    assert(call(&h,device,16,reset,1)==0x8876086cu);
    uint32_t draw1[]={D9_TRIANGLELIST,0,1};assert(call(&h,device,81,draw1,3)==0);
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,42,NULL,0)==0x8876086cu);
    assert(call(&h,device,16,reset,1)==0x8876086cu&&call(&h,device,17,present,4)==0);

    uint32_t tex_args[]={16,16,1,0,D9_A8R8G8B8,0,OUT2,0};assert(call(&h,device,23,tex_args,8)==0);
    uint32_t texture=rd(&h,OUT2);assert(texture);
    uint32_t lock_rect[]={0,OUT2,0,0};assert(call(&h,texture,19,lock_rect,4)==0);
    uint32_t pitch=rd(&h,OUT2),tex_ptr=rd(&h,OUT2+4);assert(pitch==64&&tex_ptr);
    memset(guest_ptr(&h.memory,tex_ptr,1024),0x7f,1024);uint32_t level=0;assert(call(&h,texture,20,&level,1)==0);
    uint32_t stage[]={0,texture};assert(call(&h,device,65,stage,2)==0);
    uint32_t draw2[]={D9_TRIANGLELIST,0,2};clear[2]=1;assert(call(&h,device,43,clear,6)==0);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,81,draw2,3)==0);
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,present,4)==0);
    stage[1]=0;assert(call(&h,device,65,stage,2)==0);assert(unref(&h,texture)==0);

    /* SYSTEMMEM is CPU-only through COM: locks work, GPU binding does not. */
    tex_args[5]=2;assert(call(&h,device,23,tex_args,8)==0);uint32_t system_texture=rd(&h,OUT2);assert(system_texture);
    lock_rect[0]=0;assert(call(&h,system_texture,19,lock_rect,4)==0);
    uint32_t system_ptr=rd(&h,OUT2+4);assert(system_ptr);memset(guest_ptr(&h.memory,system_ptr,1024),0x44,1024);
    level=0;assert(call(&h,system_texture,20,&level,1)==0);
    stage[1]=system_texture;assert(call(&h,device,65,stage,2)==0x8876086cu);stage[1]=0;
    uint32_t system_desc[]={0,DESC};assert(call(&h,system_texture,17,system_desc,2)==0&&rd(&h,DESC+12)==2);
    assert(unref(&h,system_texture)==0);

    D9NullStats frame=*d9_null_stats();
    assert(frame.begins==2&&frame.ends==2&&frame.clears==2&&frame.clear_targets==2&&frame.presents==2);
    assert(frame.draws==2&&frame.triangles==3&&frame.vertices==9);
    assert(frame.texture_creates==1&&frame.texture_uploads==1&&frame.texture_deletes==1);
    assert(frame.last_clear_argb==0xff102030u&&!frame.scene);

    /* COM integration: first draw records the explicit target; each successful
     * Present records the implicit swap-chain back buffer, even without a draw. */
    D9CaptureOptions capture_options;d9_capture_options_init(&capture_options);
    capture_options.directory=".";capture_options.basename="null_target_capture";
    capture_options.max_records=1;capture_options.max_presents=2;
    assert(d9_capture_init(&capture_options)==D9_CAPTURE_OK);
    rt_args[0]=16;rt_args[1]=16;rt_args[2]=D9_A8R8G8B8;rt_args[3]=0;rt_args[4]=0;rt_args[5]=0;rt_args[7]=0;
    assert(call(&h,device,28,rt_args,8)==0);uint32_t capture_rt=rd(&h,OUT2);assert(capture_rt);
    set_rt[0]=0;set_rt[1]=capture_rt;assert(call(&h,device,37,set_rt,2)==0);
    clear[0]=0;clear[1]=0;clear[2]=1;assert(call(&h,device,43,clear,6)==0);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,81,draw2,3)==0);
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,present,4)==0);
    set_rt[1]=0;assert(call(&h,device,37,set_rt,2)==0);
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,42,NULL,0)==0&&call(&h,device,17,present,4)==0);
    assert(d9_capture_count()==1&&d9_capture_present_count()==2);
    D9CaptureRecordInfo draw_info;D9CapturePresentInfo present_info[2];
    assert(!d9_capture_record(0,&draw_info)&&!d9_capture_present_record(0,&present_info[0])&&!d9_capture_present_record(1,&present_info[1]));
    char identity[64];snprintf(identity,sizeof identity,"target.handle=0x%08x",capture_rt);
    assert(file_contains(draw_info.text_path,identity)&&file_contains(draw_info.text_path,"target.implicit=0"));
    assert(file_contains(draw_info.text_path,"target.width=16")&&file_contains(draw_info.text_path,"target.format=21"));
    assert(file_contains(draw_info.text_path,"clear.valid=1"));
    assert(file_contains(present_info[0].text_path,"target.handle=0x00000000")&&file_contains(present_info[0].text_path,"target.implicit=1"));
    assert(file_contains(present_info[0].text_path,"target.width=400")&&file_contains(present_info[0].text_path,"target.height=240"));
    assert(file_contains(present_info[1].text_path,"target.handle=0x00000000")&&file_contains(present_info[1].text_path,"target.implicit=1"));
    d9_capture_shutdown();
    assert(unref(&h,capture_rt)==0);

    stream[1]=0;stream[2]=0;stream[3]=0;assert(call(&h,device,100,stream,4)==0);
    assert(unref(&h,vb)==0);

    /* Reset validates first, rebuilds owned core state, and writes D3D9 outputs. */
    assert(call(&h,device,41,NULL,0)==0&&call(&h,device,16,reset,1)==0x8876086cu);
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,present,4)==0);
    uint32_t bad_pp[14];memcpy(bad_pp,good_pp,sizeof bad_pp);present_params(&h,bad_pp);
    bad_pp[0]=640;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[8]=0;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    /* AutoDepthStencilFormat is ignored when EnableAutoDepthStencil is FALSE. */
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[10]=75;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0);
    present_params(&h,good_pp);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[13]=2;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[4]=2;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[6]=2;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[9]=1;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[11]=1;present_params(&h,bad_pp);
    assert(call(&h,device,16,reset,1)==0); /* LOCKABLE_BACKBUFFER is real for the null backend */
    present_params(&h,good_pp);
    memcpy(bad_pp,good_pp,sizeof bad_pp);bad_pp[3]=2;present_params(&h,bad_pp);assert(call(&h,device,16,reset,1)==0x8876086au);
    fvf=0x102;assert(call(&h,device,89,&fvf,1)==0);
    uint32_t zero_dimensions[14]={0,0,0,0,0,0,1,0,1,0,0,0,0,1};present_params(&h,zero_dimensions);
    assert(call(&h,device,16,reset,1)==0);
    assert(rd(&h,PP)==0&&rd(&h,PP+4)==0&&rd(&h,PP+8)==0&&rd(&h,PP+12)==0);
    assert(call1(&h,device,90,OUT)==0&&rd(&h,OUT)==0);
    bb[0]=bb[1]=bb[2]=0;bb[3]=OUT;assert(call(&h,device,18,bb,4)==0);back=rd(&h,OUT);
    assert(call(&h,back,12,desc,1)==0&&rd(&h,DESC)==D9_X8R8G8B8&&rd(&h,DESC+24)==400&&rd(&h,DESC+28)==240);
    assert(unref(&h,back)==1);assert(call1(&h,device,48,VIEW)==0&&rd(&h,VIEW+8)==400&&rd(&h,VIEW+12)==240);

    /* Direct backend validation: bad arguments fail with a [d3d9-null] line. */
    d9_null_reset();const D9Backend *b=d9_null_backend();void *ctx=d9_null_context();
    D9Matrix clip;d9_matrix_identity(&clip);
    D9DrawState state={.z_enable=1,.z_write=1,.z_func=4,.cull=1,.alpha_func=8,
        .src_blend=2,.dst_blend=1,.blend_op=1,.color_write=15,
        .address_u=3,.address_v=3,.mag_filter=2,.min_filter=2};
    assert(b->begin(ctx,0x11223344u)==0);d9_null_note_clear(1);
    assert(b->begin(ctx,0x11223344u)==-1);
    assert(b->draw(ctx,verts,3,&clip,NULL,&state)==0);
    assert(b->draw(ctx,NULL,3,&clip,NULL,&state)==-1);
    assert(b->draw(ctx,verts,4,&clip,NULL,&state)==-1);
    void *native=NULL;assert(b->target_create(ctx,7,8,D9_A8R8G8B8,&native)==-1);
    assert(b->target_create(ctx,8,8,D9_A8R8G8B8,&native)==0);
    assert(b->target_bind(ctx,native)==0&&b->target_clear(ctx,native,0x55667788u)==0);
    assert(b->draw(ctx,verts,3,&clip,NULL,&state)==0);
    b->end(ctx);b->end(ctx);b->target_delete(ctx,native);
    assert(b->texture_create(ctx,7,8,1,&native)==-1);
    assert(b->texture_create(ctx,8,8,1,&native)==0);
    uint8_t raw[256]={0};assert(b->texture_upload(ctx,native,0,raw,sizeof raw)==0);
    assert(b->texture_upload(ctx,native,1,raw,sizeof raw)==-1);b->texture_delete(ctx,native);
    D9NullStats direct=*d9_null_stats();
    assert(direct.begins==1&&direct.ends==1&&direct.presents==1&&direct.clears==1&&direct.clear_targets==1);
    assert(direct.draws==2&&direct.triangles==2&&direct.vertices==6);
    assert(direct.target_creates==1&&direct.target_deletes==1&&direct.target_binds==1);
    assert(direct.target_clears==1&&direct.target_draws==1);
    assert(direct.texture_creates==1&&direct.texture_uploads==1&&direct.texture_deletes==1);
    assert(direct.last_clear_argb==0x55667788u&&direct.last_clear_flags==1&&!direct.scene);

    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    harness_stop(&h);
    puts("D3D9 null backend: device queries, Reset, explicit LDR targets, CPU surfaces, viewport, Clear flags, refcounts and strict frames passed");
    return 0;
}
