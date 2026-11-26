/* CreatePixelShader / CreateVertexShader exact-hash plan wiring.
 *
 * Uses the real COM/stdcall path (no game bytes).  The generated plan table is
 * keyed by retail SHA-256, so this test routes lookup at a caller-owned table
 * through the D9_SHADER_PLAN_TEST_HOOKS seam: the same local descriptor is
 * selected by the SHA-256 of self-authored bytecode.  The seam does not exist
 * in production builds.
 *
 * Covers: own hash -> S_OK and a TEV object; plan draw precondition (missing
 * texture is a named refusal while the bound case draws); VS descriptor is a
 * marked, non-executable object; foreign bytecode keeps the named generic
 * refusal; GetDeviceCaps is untouched.
 */

#include "d3d9_adapter.h"
#include "d3d9_com.h"
#include "d3d9_menu_closure.h"
#include "d3d9_shader.h"
#include "d3d9_shader_plan.h"
#include "d3d9_tev.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define CODE    (SCRATCH+0x000u)
#define OUT     (SCRATCH+0x400u)
#define PP      (SCRATCH+0x500u)
#define PROBE   0x00401000u
#define UNAVAILABLE 0x8876086Au

#define COPY_SHA "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de"

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define DR(type_, reg, mask, mod) (0x80000000u | (((type_) & 7u) << 28) | \
    (((type_) & 0x18u) << 8) | ((reg) & 0x7FFu) | (((mask) & 0xFu) << 16) | \
    (((mod) & 0xFu) << 20))
#define SR(type_, reg, sw, mod) (0x80000000u | (((type_) & 7u) << 28) | \
    (((type_) & 0x18u) << 8) | ((reg) & 0x7FFu) | (((sw) & 0xFFu) << 16) | \
    (((mod) & 0xFu) << 24))
#define SW_ID 0xE4u

extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"COM stdcall stack mismatch");
    c->r[4]=sp+4;
}

typedef struct {GuestMemory memory;CPU cpu;uint32_t factory,device;} Harness;
typedef struct {int active;unsigned draws;} Mock;
typedef struct {unsigned levels;} MockTex;

static int mock_tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;MockTex *t=calloc(1,sizeof *t);if(!t)return -1;
    t->levels=levels;*native=t;return 0;
}
static int mock_tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return 0;
}
static void mock_tex_delete(void *ctx,void *native){(void)ctx;free(native);}
static int mock_begin(void *ctx,uint32_t argb){Mock *m=ctx;(void)argb;assert(!m->active);m->active=1;return 0;}
static int mock_draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    Mock *m=ctx;assert(m->active&&v&&n==3&&clip&&state);(void)texture;++m->draws;return 0;
}
static void mock_end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;}
static const D9Backend mock_backend={mock_tex_create,mock_tex_upload,mock_tex_delete,mock_begin,mock_draw,mock_end};

static int invoke(Harness *h,uint32_t target,const uint32_t *args,unsigned n,uint32_t *result){
    if(n>16)return -1;
    uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    return n3ds_lift_invoke(&h->cpu,PROBE,frame,n+2,result);
}
static uint32_t call(Harness *h,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t vt=0,fn=0;assert(!guest_read32(&h->memory,object,&vt)&&vt);
    assert(!guest_read32(&h->memory,vt+slot*4,&fn)&&fn);
    uint32_t frame[16]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static uint32_t create_ps(Harness *h,uint32_t address,uint32_t out){
    uint32_t args[2]={address,out};return call(h,h->device,106,args,2);
}
static uint32_t set_ps(Harness *h,uint32_t shader){
    uint32_t args[1]={shader};return call(h,h->device,107,args,1);
}
static uint32_t create_vs(Harness *h,uint32_t address,uint32_t out){
    uint32_t args[2]={address,out};return call(h,h->device,91,args,2);
}
static uint32_t set_vs(Harness *h,uint32_t shader){
    uint32_t args[1]={shader};return call(h,h->device,92,args,1);
}
static uint32_t set_texture(Harness *h,uint32_t texture){
    uint32_t args[2]={0,texture};return call(h,h->device,65,args,2);
}
static uint32_t draw_one(Harness *h){
    uint32_t draw[]={D9_TRIANGLELIST,0,1},present[]={0,0,0,0};
    assert(call(h,h->device,41,NULL,0)==0);          /* BeginScene */
    uint32_t rc=call(h,h->device,81,draw,3);
    assert(call(h,h->device,42,NULL,0)==0);          /* EndScene */
    assert(call(h,h->device,17,present,4)==0);       /* Present resets the phase */
    return rc;
}
static uint32_t make_vertices(Harness *h){
    D9Vertex vertices[3]={{0,0,0,0,0},{1,0,0,1,0},{0,1,0,0,1}};
    uint32_t create[]={sizeof vertices,0,0,1,OUT,0},lock[]={0,0,OUT,0},p;
    assert(call(h,h->device,26,create,6)==0);uint32_t vb=rd(h,OUT);assert(vb);
    assert(call(h,vb,11,lock,4)==0);assert(!guest_read32(&h->memory,OUT,&p)&&p);
    assert(!guest_write(&h->memory,p,vertices,sizeof vertices));assert(call(h,vb,12,NULL,0)==0);
    uint32_t stream[]={0,vb,0,sizeof(D9Vertex)},fvf=0x102;
    assert(call(h,h->device,100,stream,4)==0&&call(h,h->device,89,&fvf,1)==0);return vb;
}
static uint32_t make_texture(Harness *h){
    uint32_t create[]={8,8,1,0,D9_A8R8G8B8,1,OUT,0},lock[]={0,OUT,0,0},level=0,p,pitch;
    assert(call(h,h->device,23,create,8)==0);uint32_t texture=rd(h,OUT);assert(texture);
    assert(call(h,texture,19,lock,4)==0);assert(!guest_read32(&h->memory,OUT,&pitch)&&pitch==32);
    assert(!guest_read32(&h->memory,OUT+4,&p)&&p);memset(guest_ptr(&h->memory,p,256),0x7f,256);
    assert(call(h,texture,20,&level,1)==0);return texture;
}
static size_t build_copy_ps(uint32_t *code){
    size_t n=0;
    code[n++]=0xFFFF0300u;
    code[n++]=DI(0x1F,2);code[n++]=5u;code[n++]=DR(1,0,0xF,0);
    code[n++]=DI(0x1F,2);code[n++]=2u<<27;code[n++]=DR(10,0,0xF,0);
    code[n++]=DI(0x42,3);code[n++]=DR(8,0,0xF,0);code[n++]=SR(1,0,SW_ID,0);
    code[n++]=SR(10,0,SW_ID,0);code[n++]=0x0000FFFFu;
    return n;
}
static size_t build_vs(uint32_t *code){
    size_t n=0;
    code[n++]=0xFFFE0300u;
    code[n++]=DI(0x1F,2);code[n++]=0x80000000u;code[n++]=DR(1,0,0xF,0);
    code[n++]=DI(0x01,2);code[n++]=DR(6,0,0xF,0);code[n++]=SR(1,0,SW_ID,0);
    code[n++]=0x0000FFFFu;
    return n;
}
static void sha_hex(const uint32_t *code,size_t words,char out[65]){
    uint8_t digest[D9_MENU_CLOSURE_SHA256_BYTES];
    static const char h[]="0123456789abcdef";
    d9_menu_closure_sha256(code,words*4,digest);
    for(unsigned i=0;i<D9_MENU_CLOSURE_SHA256_BYTES;++i){
        out[i*2]=h[digest[i]>>4];out[i*2+1]=h[digest[i]&15];
    }
    out[64]=0;
}

int main(void){
    Harness h;memset(&h,0,sizeof h);Mock mock={0};
    guest_init(&h.memory,2u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x1000));
    D9Device *core=d9_create_device(&mock_backend,&mock,4u<<20);assert(core);
    assert(!d9_com_init(&h.memory,core));
    static const FnEntry probe[]={{PROBE,guest_dispatch_probe}};
    FnModule modules[2]={{"probe",probe,1},d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,2));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    PeImport import={.dll="d3d9.dll",.name="Direct3DCreate9"};
    uint32_t create9=d9_com_resolve(&import,NULL);assert(create9);
    uint32_t sdk=32;assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    uint32_t pp_values[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    assert(!guest_write(&h.memory,PP,pp_values,sizeof pp_values));
    uint32_t create[]={0,1,0,0x20,PP,OUT};assert(call(&h,h.factory,16,create,6)==0);
    assert((h.device=rd(&h,OUT))!=0);
    make_vertices(&h);uint32_t texture=make_texture(&h);assert(set_texture(&h,texture)==0);

    /* Caps are unchanged by plan selection: no SM3, DevCaps 0x00082000. */
    D9Caps9 caps;memset(&caps,0xAA,sizeof caps);
    assert(d9_adapter_caps(0,1,&caps)==0);
    if(caps.dev_caps!=0x00082000u||caps.vertex_shader_version!=0xFFFE0200u||
       caps.pixel_shader_version!=0xFFFF0200u||
       caps.max_vertex_shader_30_instruction_slots!=0||
       caps.max_pixel_shader_30_instruction_slots!=0){
        printf("FAIL: caps changed by plan selection (DevCaps 0x%08x VS 0x%08x PS 0x%08x)\n",
               (unsigned)caps.dev_caps,(unsigned)caps.vertex_shader_version,
               (unsigned)caps.pixel_shader_version);
        return 1;
    }

    /* Exact-hash PS plan: self-authored copy stream, local descriptor. */
    uint32_t ps_code[16];size_t ps_words=build_copy_ps(ps_code);
    char ps_sha[65];sha_hex(ps_code,ps_words,ps_sha);
    const D9ShaderPlan *copy=NULL;char why[192];
    assert(d9_shader_plan_lookup(D9_SHADER_PLAN_STAGE_PS,COPY_SHA,&copy,why,sizeof why)==1);
    D9ShaderPlan ps_plan=*copy;
    ps_plan.id="test.plan.copy";ps_plan.sha256=ps_sha;ps_plan.draw_enabled=1;

    uint32_t vs_code[8];size_t vs_words=build_vs(vs_code);
    char vs_sha[65];sha_hex(vs_code,vs_words,vs_sha);
    D9ShaderPlan vs_plan;memset(&vs_plan,0,sizeof vs_plan);
    vs_plan.id="test.plan.vs";vs_plan.sha256=vs_sha;
    vs_plan.stage=D9_SHADER_PLAN_STAGE_VS;vs_plan.kind=D9_SHADER_PLAN_PICA_VS;
    vs_plan.draw_enabled=0;vs_plan.note="PICA VS descriptor; native code emitter is absent";

    D9ShaderPlan entries[2]={ps_plan,vs_plan};
    d9_shader_plan_test_set(entries,2);

    assert(!guest_write(&h.memory,CODE,ps_code,ps_words*4));
    assert(create_ps(&h,CODE,OUT)==0);
    uint32_t ps=rd(&h,OUT);assert(ps);
    assert(set_ps(&h,ps)==0);            /* descriptor really became the TEV object */

    /* The descriptor's texture is bound: the same plan draws. */
    unsigned draws=mock.draws;
    uint32_t rc=draw_one(&h);
    if(rc!=0||mock.draws!=draws+1){
        printf("FAIL: plan draw with texture rc=%08x draws=%u diag=%s\n",
               (unsigned)rc,mock.draws,d9_com_last_diagnostic());
        return 1;
    }

    /* Draw precondition: no texture bound is a named refusal, not a draw. */
    assert(set_texture(&h,0)==0);
    draws=mock.draws;
    rc=draw_one(&h);
    if(rc!=UNAVAILABLE||mock.draws!=draws||
       !strstr(d9_com_last_diagnostic(),"texture stage 0")){
        printf("FAIL: plan draw precondition rc=%08x draws=%u diag=%s\n",
               (unsigned)rc,mock.draws,d9_com_last_diagnostic());
        return 1;
    }

    /* VS descriptor: create and bind succeed, execution is refused by name. */
    assert(!guest_write(&h.memory,CODE,vs_code,vs_words*4));
    assert(create_vs(&h,CODE,OUT)==0);
    uint32_t vs=rd(&h,OUT);assert(vs);
    assert(set_vs(&h,vs)==0);
    draws=mock.draws;
    rc=draw_one(&h);
    if(rc!=UNAVAILABLE||mock.draws!=draws){
        printf("FAIL: VS plan draw rc=%08x draws=%u diag=%s\n",
               (unsigned)rc,mock.draws,d9_com_last_diagnostic());
        return 1;
    }
    if(!strstr(d9_com_last_diagnostic(),"emitter")&&
       !strstr(d9_com_last_diagnostic(),"not draw-enabled")){
        printf("FAIL: VS plan refusal is not named: %s\n",d9_com_last_diagnostic());
        return 1;
    }

    /* Foreign bytecode keeps the named generic refusal (changed sampler). */
    set_ps(&h,0);set_vs(&h,0);
    uint32_t foreign[32]={0};memcpy(foreign,d9_demo_ps,d9_demo_ps_words*4);
    foreign[10]=0xa0e40801u;
    assert(!guest_write(&h.memory,CODE,foreign,d9_demo_ps_words*4));
    assert(!guest_write32(&h.memory,OUT,0xDEADBEEFu));
    rc=create_ps(&h,CODE,OUT);
    if(rc!=UNAVAILABLE||rd(&h,OUT)!=0||!d9_com_last_shader_rejection()[0]){
        printf("FAIL: foreign PS rc=%08x out=%08x rejection=%s\n",
               (unsigned)rc,rd(&h,OUT),d9_com_last_shader_rejection());
        return 1;
    }

    d9_shader_plan_test_set(NULL,0);
    puts("shader_plan_com: exact-hash PS/Vs plans, draw preconditions, named refusal, caps unchanged");
    return 0;
}
