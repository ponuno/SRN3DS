/* Measurement-only shader-skip probe.
 *
 * Without TESV_D3D9_SHADER_SKIP_PROBE an unknown shader is rejected exactly
 * as before.  With it, Create*Shader returns a named, non-executable object
 * (no menu match, no TEV program and no draw plan), Set* accepts it, and every
 * draw using it is refused by name.  Caps/SM3/T&L stay unchanged. */
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH+0x200)
#define CAPS (SCRATCH+0x400)
#define CAPS_VSVER (CAPS+196)
#define CAPS_PSVER (CAPS+204)
#define OUT (SCRATCH+0x500)
#define OUT2 (SCRATCH+0x600)
#define SIZE (SCRATCH+0x700)
#define FUNC (SCRATCH+0x800)
#define CODE (SCRATCH+0x3000)
#define GUID (SCRATCH+0x4000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_UNAVAILABLE 0x8876086au

static const uint8_t iid_iunknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"shader probe stdcall stack mismatch");
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
static uint32_t caps(Harness *h){assert(call(h,h->device,7,(uint32_t[]){CAPS},1)==HR_OK);return rd(h,CAPS+28);}
static uint32_t available(Harness *h){return call(h,h->device,4,NULL,0);}

static void run_case(Harness *h,int probe){
#ifdef _WIN32
    _putenv_s("TESV_D3D9_SHADER_SKIP_PROBE",probe?"1":"0");
#else
    setenv("TESV_D3D9_SHADER_SKIP_PROBE",probe?"1":"0",1);
#endif
    h->factory=h->device=0;
    assert(!d9_com_init(&h->memory,NULL));
    assert(!d9_null_register());
    uint32_t create9=d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"},NULL);
    assert(create9);
    uint32_t sdk=32;assert(!invoke(h,create9,&sdk,1,&h->factory)&&h->factory);
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    wr(h,PP,pp,sizeof pp);
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(h,h->factory,16,create,6)==HR_OK);
    assert(!guest_read32(&h->memory,OUT,&h->device)&&h->device);
    assert(caps(h)==0x00082000u);
    assert((caps(h)&0x00018000u)==0);
    /* SM3 stays absent: caps keep VS 2.0 / PS 2.0 even when a probe object was
     * created from a 3.0 token stream. */
    assert(rd(h,CAPS_VSVER)==0xFFFE0200u);
    assert(rd(h,CAPS_PSVER)==0xFFFF0200u);
    /* NORMAL FVF remains owned by the T&L prototype. */
    assert(call(h,h->device,89,(uint32_t[]){0x012u},1)==HR_UNAVAILABLE);
    assert(call(h,h->device,89,(uint32_t[]){0x102u},1)==HR_OK);
    uint32_t before=available(h);

    uint32_t ps_words[]={0xFFFF0300u,0x0000FFFFu};
    uint32_t vs_words[]={0xFFFE0300u,0x0000FFFFu};
    wr(h,CODE,ps_words,sizeof ps_words);
    uint32_t cp[]={CODE,OUT};assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    uint32_t pc=call(h,h->device,106,cp,2);
    uint32_t ps=rd(h,OUT);
    if(!probe){
        assert(pc!=HR_OK&&ps==0);
    }else{
        assert(pc==HR_OK&&ps);
        /* GetFunction reports the original token stream (2 words), but the
         * object carries no menu match, TEV program or draw plan. */
        uint32_t size=0;wr(h,SIZE,&size,4);
        assert(call(h,ps,4,(uint32_t[]){0,SIZE},2)==HR_OK);
        assert(rd(h,SIZE)==8u);
        assert(call(h,ps,4,(uint32_t[]){FUNC,SIZE},2)==HR_OK);
        assert(rd(h,FUNC)==0xFFFF0300u);
        /* QI IUnknown, AddRef/Release and a second create/release cycle. */
        wr(h,GUID,iid_iunknown,16);
        uint32_t qi[]={GUID,OUT2};assert(!guest_write32(&h->memory,OUT2,0));
        assert(call(h,ps,0,qi,2)==HR_OK&&rd(h,OUT2)==ps);
        assert(call(h,ps,2,NULL,0)>=1);
        assert(call(h,ps,1,NULL,0)>=2);
        assert(call(h,ps,2,NULL,0)>=1);
        uint32_t ps2=0;
        wr(h,CODE,ps_words,sizeof ps_words);assert(!guest_write32(&h->memory,OUT,0));
        assert(call(h,h->device,106,cp,2)==HR_OK);ps2=rd(h,OUT);assert(ps2);
        assert(call(h,ps2,2,NULL,0)==0);
        wr(h,CODE,vs_words,sizeof vs_words);assert(!guest_write32(&h->memory,OUT,0));
        assert(call(h,h->device,91,cp,2)==HR_OK);
        uint32_t vs=rd(h,OUT);assert(vs);
        assert(call(h,h->device,107,(uint32_t[]){ps},1)==HR_OK);
        assert(call(h,h->device,92,(uint32_t[]){vs},1)==HR_OK);
        assert(call(h,h->device,41,NULL,0)==HR_OK);
        assert(call(h,h->device,81,(uint32_t[]){D9_TRIANGLELIST,0,1},3)==HR_UNAVAILABLE);
        assert(strstr(d9_com_last_diagnostic(),"shader-skip probe"));
        assert(call(h,h->device,42,NULL,0)==HR_OK);
        assert(call(h,h->device,17,(uint32_t[]){0,0,0,0},4)==HR_OK);
        assert(call(h,h->device,107,(uint32_t[]){0},1)==HR_OK);
        assert(call(h,h->device,92,(uint32_t[]){0},1)==HR_OK);
        assert(call(h,ps,2,NULL,0)==0);
        assert(call(h,vs,2,NULL,0)==0);
        assert(available(h)==before);
    }
    assert(caps(h)==0x00082000u);
    assert(call(h,h->device,2,NULL,0)==0);
    assert(call(h,h->factory,2,NULL,0)==0);
    d9_com_shutdown();
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x6000));
    static const FnEntry probe_fn[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[2]={{"probe",probe_fn,1},d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,2));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    run_case(&h,0);
    run_case(&h,1);
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("shader-skip probe: off rejects, on accepts non-executable objects and refuses draws; caps unchanged");
    return 0;
}
