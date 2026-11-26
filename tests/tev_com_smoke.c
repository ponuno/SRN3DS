/* End-to-end shader integration through IDirect3DDevice9::CreatePixelShader.
 * Uses the COM vtable/stdcall dispatch path used by the guest; no game bytes.
 * A mock backend records the borrowed TEV pointer carried by D9DrawState.
 */

#include "d3d9_com.h"
#include "d3d9_shader.h"
#include "d3d9_tev.h"
#include "d3d9_menu_shader_runtime.h"
#include "n3ds_lift_runtime.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define CODE    (SCRATCH+0x000u)
#define OUT     (SCRATCH+0x400u)
#define PP      (SCRATCH+0x500u)
#define DATA    (SCRATCH+0x600u)
#define STACK   (SCRATCH+0x800u)
#define PROBE   0x00401000u
#define UNAVAILABLE 0x8876086Au
#define INVALID 0x8876086Cu

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define DST_R(n)  (0x80000000u | (uint32_t)(n) | (0xFu << 16))
#define DST_C0    (0x80000000u | 0x0800u | (0xFu << 16))
#define SRC_R(n)  (0x80000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_C0    (0xA0000000u | 0xE40000u)
#define SRC_T0    (0x90000000u | 0xE40000u)
#define SAMP0     (0xA00F0800u)
#define TEXD0     (0x90030000u)
#define VER       (0xFFFF0300u)
#define END       (0xFFFFu)

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
typedef struct {int active;unsigned draws;void *texture;const D9TevProgram *shader;} Mock;
typedef struct {unsigned levels;} MockTex;

static int mock_tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;MockTex *t=calloc(1,sizeof *t);if(!t)return -1;
    t->levels=levels;*native=t;return 0;
}
static int mock_tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return 0;
}
static void mock_tex_delete(void *ctx,void *native){
    Mock *m=ctx;assert(!m->active);free(native);
}
static int mock_begin(void *ctx,uint32_t argb){Mock *m=ctx;(void)argb;assert(!m->active);m->active=1;return 0;}
static int mock_draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    Mock *m=ctx;assert(m->active&&v&&n==3&&clip&&state);
    m->texture=texture;m->shader=state->pixel_shader;++m->draws;return 0;
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
static uint32_t create_pixel_shader(Harness *h,uint32_t address,uint32_t out){
    uint32_t args[2]={address,out};return call(h,h->device,106,args,2);
}
static uint32_t create_vertex_shader(Harness *h,uint32_t address,uint32_t out){
    uint32_t args[2]={address,out};return call(h,h->device,91,args,2);
}
static uint32_t set_vertex_shader(Harness *h,uint32_t shader){
    uint32_t args[1]={shader};return call(h,h->device,92,args,1);
}
static uint32_t vs_dst(unsigned type,unsigned reg,unsigned mask){
    return 0x80000000u|((type&7u)<<28)|((type&0x18u)<<8)|(reg&0x7FFu)|((mask&0xFu)<<16);
}
static uint32_t vs_src(unsigned type,unsigned reg,unsigned sw){
    return 0x80000000u|((type&7u)<<28)|((type&0x18u)<<8)|(reg&0x7FFu)|((sw&0xFFu)<<16);
}
static size_t build_menu_vs(uint32_t *code,int input_color,int output_color1){
    size_t n=0;
    code[n++]=0xFFFE0300u;
    code[n++]=DI(0x1F,2);code[n++]=0x80000000u;code[n++]=vs_dst(1,0,0xF); /* position v0 */
    code[n++]=DI(0x1F,2);code[n++]=0x80000005u;code[n++]=vs_dst(1,1,0xF); /* texcoord0 v1 */
    if(input_color){
        code[n++]=DI(0x1F,2);code[n++]=0x8000000Au;code[n++]=vs_dst(1,2,0xF); /* color0 v2 */
    }
    code[n++]=DI(0x1F,2);code[n++]=0x80000000u;code[n++]=vs_dst(6,0,0xF); /* position o0 */
    code[n++]=DI(0x1F,2);code[n++]=0x80000005u;code[n++]=vs_dst(6,1,0xF); /* texcoord0 o1 */
    code[n++]=DI(0x1F,2);code[n++]=0x8000000Au;code[n++]=vs_dst(6,2,0xF); /* color0 o2 */
    if(output_color1){
        code[n++]=DI(0x1F,2);code[n++]=0x8001000Au;code[n++]=vs_dst(6,3,0xF); /* color1 o3 */
    }
    code[n++]=DI(0x01,2);code[n++]=vs_dst(6,0,0xF);code[n++]=vs_src(1,0,0xE4);
    code[n++]=DI(0x01,2);code[n++]=vs_dst(6,1,0xF);code[n++]=vs_src(1,1,0xE4);
    code[n++]=DI(0x01,2);code[n++]=vs_dst(6,2,0xF);code[n++]=vs_src(1,input_color?2:1,0xE4);
    if(output_color1){
        code[n++]=DI(0x01,2);code[n++]=vs_dst(6,3,0xF);code[n++]=vs_src(1,1,0xE4);
    }
    code[n++]=END;
    return n;
}
static void prefix(uint32_t *code,size_t *n){
    code[(*n)++]=VER;
    code[(*n)++]=DI(0x1F,2);code[(*n)++]=0x80000005u;code[(*n)++]=TEXD0;
    code[(*n)++]=DI(0x1F,2);code[(*n)++]=0x90000000u;code[(*n)++]=0xA00F0800u;
}
static void set_pixel_shader(Harness *h,uint32_t shader){uint32_t args[1]={shader};assert(call(h,h->device,107,args,1)==0);}
static void set_texture(Harness *h,uint32_t texture){uint32_t args[2]={0,texture};assert(call(h,h->device,65,args,2)==0);}
static void draw_frame(Harness *h,Mock *m){
    uint32_t draw[]={D9_TRIANGLELIST,0,1},present[]={0,0,0,0};
    unsigned before=m->draws;
    assert(call(h,h->device,41,NULL,0)==0);
    assert(call(h,h->device,81,draw,3)==0);
    assert(call(h,h->device,42,NULL,0)==0);
    assert(call(h,h->device,17,present,4)==0);
    assert(m->draws==before+1&&!m->active);
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
static void expect_shader_rejection(Harness *h,const uint32_t *code,size_t words,uint32_t hresult,const char *needle){
    assert(!guest_write(&h->memory,CODE,code,words*4));
    assert(!guest_write32(&h->memory,OUT,0xDEADBEEFu));
    assert(create_pixel_shader(h,CODE,OUT)==hresult&&rd(h,OUT)==0);
    assert(strstr(d9_com_last_shader_rejection(),needle)!=NULL);
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
    make_vertices(&h);uint32_t texture=make_texture(&h);set_texture(&h,texture);

    /* Observable material evidence: bound LDR texture, declaration colour and
     * constants c0..c7.  Normal/tangent/ambient stay absent by design. */
    uint32_t observed=d9_com_menu_observed_preconditions();
    assert((observed&(D9_MENU_SHADER_PRE_LDR_QUALITY|
                      D9_MENU_SHADER_PRE_TEXTURE0_2D))==
           (D9_MENU_SHADER_PRE_LDR_QUALITY|D9_MENU_SHADER_PRE_TEXTURE0_2D));
    assert(!(observed&D9_MENU_SHADER_PRE_VERTEX_COLOR));
    float rows[8][4]={
        {1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1},
        {0.5f,0.25f,0.75f,0.5f},{0,0,0,0},{0,0,0,0},{0.25f,0,0,1}
    };
    assert(!guest_write(&h.memory,DATA,rows,sizeof rows));
    uint32_t const_args[]={0,DATA,8};assert(call(&h,h.device,94,const_args,3)==0);
    observed=d9_com_menu_observed_preconditions();
    assert((observed&D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7)!=0);
    assert((observed&D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4)!=0);
    assert(!(observed&D9_MENU_SHADER_PRE_NORMAL_PREBAKED));
    assert(!(observed&D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED));
    float ambient_emissive[4]={0.1f,0.2f,0.3f,1.0f};
    assert(d9_com_menu_set_ambient_emissive(ambient_emissive)==0);
    observed=d9_com_menu_observed_preconditions();
    assert((observed&D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING)!=0);
    float bad_ambient[4]={NAN,0,0,1};
    assert(d9_com_menu_set_ambient_emissive(bad_ambient)==INVALID);

    uint32_t code[128],shader=0;size_t n=0;prefix(code,&n);
    code[n++]=DI(0x42,3);code[n++]=DST_C0;code[n++]=SRC_T0;code[n++]=SAMP0;code[n++]=END;
    assert(!guest_write(&h.memory,CODE,code,n*4));assert(create_pixel_shader(&h,CODE,OUT)==0);
    assert((shader=rd(&h,OUT))!=0);
    D9TevProgram tev;assert(d9_com_pixel_shader_tev(shader,&tev));
    assert(tev.stages==1&&tev.textures==1&&tev.stage[0].func_rgb==D9TEV_FUNC_REPLACE);
    set_pixel_shader(&h,shader);draw_frame(&h,&mock);
    assert(mock.shader&&mock.shader->stages==1&&mock.shader->textures==1&&mock.texture);
    const D9TevProgram *bound=mock.shader;
    set_pixel_shader(&h,shader);draw_frame(&h,&mock);assert(mock.shader==bound);

    /* Literal tint reaches draw through the borrowed pointer. */
    n=0;prefix(code,&n);
    code[n++]=DI(0x51,5);code[n++]=0xA00F0000u;
    code[n++]=0x3F000000u;code[n++]=0x3E800000u;code[n++]=0x3F800000u;code[n++]=0x3F800000u;
    code[n++]=DI(0x42,3);code[n++]=DST_R(0);code[n++]=SRC_T0;code[n++]=SAMP0;
    code[n++]=DI(0x05,3);code[n++]=DST_C0;code[n++]=SRC_R(0);code[n++]=SRC_C0;code[n++]=END;
    assert(!guest_write(&h.memory,CODE,code,n*4));assert(create_pixel_shader(&h,CODE,OUT)==0);
    shader=rd(&h,OUT);assert(shader);set_pixel_shader(&h,shader);draw_frame(&h,&mock);
    assert(mock.shader&&mock.shader->stage[0].func_rgb==D9TEV_FUNC_MODULATE);
    assert(mock.shader->stage[0].const_is_literal&&mock.texture);

    /* Zero-texture literal programs draw without a bound texture. */
    n=0;code[n++]=VER;
    code[n++]=DI(0x51,5);code[n++]=0xA00F0000u;
    code[n++]=0x3F000000u;code[n++]=0x3E800000u;code[n++]=0x3F800000u;code[n++]=0x3F800000u;
    code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=SRC_C0;code[n++]=END;
    assert(!guest_write(&h.memory,CODE,code,n*4));assert(create_pixel_shader(&h,CODE,OUT)==0);
    shader=rd(&h,OUT);assert(shader);set_texture(&h,0);set_pixel_shader(&h,shader);draw_frame(&h,&mock);
    assert(mock.shader&&mock.shader->textures==0&&!mock.texture);

    /* SM2 and NULL both restore the fixed-function null-program path. */
    assert(!guest_write(&h.memory,CODE,d9_demo_ps,d9_demo_ps_words*4));
    assert(create_pixel_shader(&h,CODE,OUT)==0);shader=rd(&h,OUT);assert(shader);
    set_texture(&h,texture);set_pixel_shader(&h,shader);draw_frame(&h,&mock);
    assert(!mock.shader&&mock.texture);
    set_pixel_shader(&h,0);draw_frame(&h,&mock);assert(!mock.shader&&mock.texture);
    set_texture(&h,0);draw_frame(&h,&mock);assert(!mock.shader&&!mock.texture);

    /* Backend/translator rejections are named and never reach the fixed path. */
    n=0;code[n++]=VER;code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=SRC_C0;code[n++]=END;
    expect_shader_rejection(&h,code,n,UNAVAILABLE,"external constant c0");
    n=0;code[n++]=VER;code[n++]=DI(0x1F,2);code[n++]=0x80000000u;code[n++]=0x900F1000u;
    code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=0x90E41000u;code[n++]=END;
    expect_shader_rejection(&h,code,n,UNAVAILABLE,"VPOS");
    n=0;code[n++]=VER;code[n++]=DI(0x1F,2);code[n++]=0x8000000Au;code[n++]=0x900F0000u;
    code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=0x90E40000u;code[n++]=END;
    expect_shader_rejection(&h,code,n,UNAVAILABLE,"vertex colour");
    n=0;prefix(code,&n);code[n++]=DI(0x1F,2);code[n++]=0x90000000u;code[n++]=0xA00F0801u;
    code[n++]=DI(0x42,3);code[n++]=DST_R(0);code[n++]=SRC_T0;code[n++]=SAMP0;
    code[n++]=DI(0x42,3);code[n++]=DST_R(1);code[n++]=SRC_T0;code[n++]=0xA00F0801u;
    code[n++]=DI(0x02,3);code[n++]=DST_C0;code[n++]=SRC_R(0);code[n++]=SRC_R(1);code[n++]=END;
    expect_shader_rejection(&h,code,n,UNAVAILABLE,"sampler s1");
    n=0;prefix(code,&n);
    for(unsigned i=0;i<6;++i){
        code[n++]=DI(0x51,5);code[n++]=0xA00F0000u|i;
        code[n++]=0x3F000000u;code[n++]=0x3F000000u;code[n++]=0x3F000000u;code[n++]=0x3F800000u;
    }
    code[n++]=DI(0x42,3);code[n++]=DST_R(0);code[n++]=SRC_T0;code[n++]=SAMP0;
    for(unsigned i=0;i<6;++i){
        code[n++]=DI(0x05,3);code[n++]=DST_R(0);code[n++]=SRC_R(0);code[n++]=(0xA0E40000u|i);
    }
    code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=SRC_R(0);code[n++]=END;
    expect_shader_rejection(&h,code,n,UNAVAILABLE,"TEV stages");

    /* Menu VS family: the declaration/FVF gate is part of selection. */
    uint32_t vscode[64],vshader=0,fvf=0x102;
    size_t vn=build_menu_vs(vscode,1,1);
    assert(!guest_write(&h.memory,CODE,vscode,vn*4));
    assert(!guest_write32(&h.memory,OUT,0xDEADBEEFu));
    assert(create_vertex_shader(&h,CODE,OUT)==0&&(vshader=rd(&h,OUT))!=0);
    assert(d9_com_last_shader_rejection()[0]==0);

    /* FVF 0x102 has position+texcoord only: SetVertexShader must refuse. */
    assert(call(&h,h.device,89,&fvf,1)==0);
    assert(set_vertex_shader(&h,vshader)==UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"color0")!=NULL);

    /* FVF 0x142 supplies position+diffuse+tex1: bind succeeds, draw does not. */
    fvf=0x142;assert(call(&h,h.device,89,&fvf,1)==0);
    observed=d9_com_menu_observed_preconditions();
    assert((observed&D9_MENU_SHADER_PRE_VERTEX_COLOR)!=0);
    assert(set_vertex_shader(&h,vshader)==0);
    unsigned before=mock.draws;
    uint32_t draw[]={D9_TRIANGLELIST,0,1},present[]={0,0,0,0};
    assert(call(&h,h.device,41,NULL,0)==0);
    assert(call(&h,h.device,81,draw,3)==UNAVAILABLE);
    assert(mock.draws==before);
    assert(strstr(d9_com_last_diagnostic(),"PICA")!=NULL||
           strstr(d9_com_last_diagnostic(),"vertex shader")!=NULL);
    assert(call(&h,h.device,42,NULL,0)==0);
    assert(call(&h,h.device,17,present,4)==0);
    assert(set_vertex_shader(&h,0)==0);
    fvf=0x102;assert(call(&h,h.device,89,&fvf,1)==0);

    /* Missing color1 output is a named refusal with no partial object. */
    vn=build_menu_vs(vscode,1,0);
    assert(!guest_write(&h.memory,CODE,vscode,vn*4));
    assert(!guest_write32(&h.memory,OUT,0xDEADBEEFu));
    assert(create_vertex_shader(&h,CODE,OUT)==UNAVAILABLE);
    assert(rd(&h,OUT)==0&&strstr(d9_com_last_shader_rejection(),"color1")!=NULL);

    /* Leave a TEV shader bound across COM shutdown of an adopted device. */
    n=0;code[n++]=VER;
    code[n++]=DI(0x51,5);code[n++]=0xA00F0000u;
    code[n++]=0x3F000000u;code[n++]=0x3E800000u;code[n++]=0x3F800000u;code[n++]=0x3F800000u;
    code[n++]=DI(0x01,2);code[n++]=DST_C0;code[n++]=SRC_C0;code[n++]=END;
    assert(!guest_write(&h.memory,CODE,code,n*4));assert(create_pixel_shader(&h,CODE,OUT)==0);
    shader=rd(&h,OUT);assert(shader);set_texture(&h,0);set_pixel_shader(&h,shader);

    d9_com_shutdown();n3ds_lift_release();guest_destroy(&h.memory);
    assert(d9_begin_scene(core,0)==0);
    assert(d9_draw_primitive(core,D9_TRIANGLELIST,0,1)==0&&!mock.shader);
    assert(d9_present(core)==0);
    d9_destroy_device(core);
    puts("D3D9 TEV COM: borrowed draw pointer, literal tint, zero-texture, SM2/NULL reset, shutdown clear and named rejections passed");
    return 0;
}
