/* CreateVertexShader / CreateVertexDeclaration contract.
 *
 * D3D9 distinguishes three cases that the shim must not conflate:
 *   1. a pointer that is not a D3D9 token stream -> D3DERR_INVALIDCALL;
 *   2. a valid stream this device cannot execute -> D3DERR_NOTAVAILABLE
 *      (named refusal) in a normal run;
 *   3. the same stream under the measurement-only skip probe -> S_OK with a
 *      non-executable object (GetFunction still reports the real tokens).
 *
 * CreateVertexDeclaration names the first rejected element (stream, type,
 * usage, offset) instead of returning a bare INVALIDCALL.
 *
 * DevCaps stay 0x00082000 and VS/PS caps stay 2.0.
 */
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
#define SIZE (SCRATCH+0x700)
#define FUNC (SCRATCH+0x800)
#define CODE (SCRATCH+0x3000)
#define ELEMS (SCRATCH+0x6000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALIDCALL 0x8876086cu
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
static void put_elem(Harness *h,unsigned i,unsigned stream,unsigned offset,unsigned type,
                     unsigned method,unsigned usage,unsigned index){
    uint8_t e[8]={(uint8_t)stream,(uint8_t)(stream>>8),(uint8_t)offset,(uint8_t)(offset>>8),
                  (uint8_t)type,(uint8_t)method,(uint8_t)usage,(uint8_t)index};
    wr(h,ELEMS+i*8,e,sizeof e);
}
static void end_elem(Harness *h,unsigned i){put_elem(h,i,0xFF,0,17,0,0,0);}

static uint32_t create_decl(Harness *h){
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    uint32_t rc=call(h,h->device,86,(uint32_t[]){ELEMS,OUT},2);
    return rc;
}
static void expect_decl_reason(Harness *h,const char *reason){
    uint32_t rc=create_decl(h);
    if(rc!=HR_INVALIDCALL){
        fprintf(stderr,"declaration should be INVALIDCALL, got 0x%08x (%s)\n",
                rc,d9_com_last_diagnostic());
        assert(0);
    }
    if(!strstr(d9_com_last_diagnostic(),reason)){
        fprintf(stderr,"diagnostic %s does not name '%s'\n",d9_com_last_diagnostic(),reason);
        assert(0);
    }
    assert(rd(h,OUT)==0);
}

static void declarations(Harness *h){
    /* Position + texcoord, the admitted shape. */
    put_elem(h,0,0,0,2,0,0,0);       /* POSITION0 FLOAT3 @0 */
    put_elem(h,1,0,12,1,0,5,0);      /* TEXCOORD0 FLOAT2 @12 */
    end_elem(h,2);
    uint32_t rc=create_decl(h);
    assert(rc==HR_OK);uint32_t decl=rd(h,OUT);assert(decl);
    assert(call(h,h->device,87,(uint32_t[]){decl},1)==HR_OK);
    assert(call(h,h->device,88,(uint32_t[]){OUT},1)==HR_OK);
    assert(rd(h,OUT)==decl);
    assert(call(h,decl,2,NULL,0)>=1);           /* GetVertexDeclaration retained it */
    assert(call(h,h->device,87,(uint32_t[]){0},1)==HR_OK);
    assert(call(h,decl,2,NULL,0)==0);

    /* COLOR0 with FLOAT16_4: a half-float RGBA colour, the exact type of the
     * eight measured retail refusals (4x element 1, 4x element 3).  The old
     * whitelist omitted it and returned a false D3DERR_INVALIDCALL. */
    put_elem(h,0,0,0,2,0,0,0);       /* POSITION0 FLOAT3 @0 */
    put_elem(h,1,0,12,16,0,10,0);    /* COLOR0 FLOAT16_4 @12 */
    end_elem(h,2);
    rc=create_decl(h);
    assert(rc==HR_OK);
    uint32_t cdecl=rd(h,OUT);assert(cdecl);
    assert(call(h,cdecl,2,NULL,0)==0);

    /* Duplicate semantic. */
    put_elem(h,0,0,0,2,0,0,0);
    put_elem(h,1,0,12,1,0,5,0);
    put_elem(h,2,0,20,1,0,5,0);
    end_elem(h,3);
    expect_decl_reason(h,"duplicates texcoord0");

    /* Overlapping offsets in the same stream. */
    put_elem(h,0,0,0,2,0,0,0);
    put_elem(h,1,0,8,1,0,5,0);
    end_elem(h,2);
    expect_decl_reason(h,"overlaps the previous element");

    /* XYZRHW with the wrong width: only FLOAT4 carries w. */
    put_elem(h,0,0,0,2,0,9,0);
    end_elem(h,1);
    expect_decl_reason(h,"positiont0 is FLOAT3");

    /* Stream index outside 0..15. */
    put_elem(h,0,16,0,2,0,0,0);
    end_elem(h,1);
    expect_decl_reason(h,"stream 16 is outside");

    /* No position at all. */
    put_elem(h,0,0,0,1,0,5,0);
    end_elem(h,1);
    expect_decl_reason(h,"no POSITION or POSITIONT");

    /* Terminator with non-zero fields. */
    put_elem(h,0,0,0,2,0,0,0);
    put_elem(h,1,0xFF,0,0xFF,0,1,0);
    expect_decl_reason(h,"terminator carries non-zero fields");
}

static void shaders(Harness *h,int probe){
    /* A valid vs_3_0 with dcl_position and a 7-word mad whose two constant
     * operands are relative-addressed (the retail shape measured in
     * shaders001.fxp meta 8582 word 132).  Not executable on this backend. */
    static const uint32_t relative_vs[]={
        0xFFFE0300u,
        0x0200001Fu,0x80000000u,0x900F0000u,     /* dcl_position v0 */
        0x06000004u,0x80030001u,0xA0ED2000u,0xB0000000u,
        0x90E40001u,0xA0E82000u,0xB0000000u,     /* mad r1.xyz, c0[a0.x], v1, c0[a0.x] */
        0x0000FFFFu};

    /* Garbage bytecode is INVALIDCALL in both modes: this is not a shader. */
    static const uint32_t garbage[]={0xFFFE0300u,0x00001234u,0x0000FFFFu};
    wr(h,CODE,garbage,sizeof garbage);
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    uint32_t cp[]={CODE,OUT};
    uint32_t rc=call(h,h->device,91,cp,2);
    assert(rc==HR_INVALIDCALL);
    assert(rd(h,OUT)==0);
    assert(strstr(d9_com_last_shader_rejection(),"unknown opcode"));

    wr(h,CODE,relative_vs,sizeof relative_vs);
    assert(!guest_write32(&h->memory,OUT,0));
    rc=call(h,h->device,91,cp,2);
    if(!probe){
        /* Valid for D3D9, but outside the executable closure: named refusal,
         * never a fake success. */
        assert(rc==HR_UNAVAILABLE);
        assert(rd(h,OUT)==0);
        assert(d9_com_last_diagnostic()[0]);
    }else{
        assert(rc==HR_OK);
        uint32_t vs=rd(h,OUT);assert(vs);
        uint32_t size=0;wr(h,SIZE,&size,4);
        assert(call(h,vs,4,(uint32_t[]){0,SIZE},2)==HR_OK);
        assert(rd(h,SIZE)==sizeof relative_vs);
        assert(call(h,vs,4,(uint32_t[]){FUNC,SIZE},2)==HR_OK);
        assert(rd(h,FUNC)==0xFFFE0300u);
        assert(rd(h,FUNC+4)==relative_vs[1]);
        assert(call(h,h->device,92,(uint32_t[]){vs},1)==HR_OK);
        assert(call(h,h->device,92,(uint32_t[]){0},1)==HR_OK);
        assert(call(h,vs,2,NULL,0)==0);
    }
    assert(caps(h)==0x00082000u);
    assert(rd(h,CAPS_VSVER)==0xFFFE0200u);
    assert(rd(h,CAPS_PSVER)==0xFFFF0200u);
}

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
    shaders(h,probe);
    declarations(h);
    assert(caps(h)==0x00082000u);
    assert(call(h,h->device,2,NULL,0)==0);
    assert(call(h,h->factory,2,NULL,0)==0);
    d9_com_shutdown();
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x8000));
    static const FnEntry probe_fn[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[2]={{"probe",probe_fn,1},d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,2));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    run_case(&h,0);
    run_case(&h,1);
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("shader/declaration contract: garbage is INVALIDCALL, valid SM3 is a "
         "named refusal or a non-executable probe object, declarations name the "
         "rejected element; caps unchanged");
    return 0;
}
