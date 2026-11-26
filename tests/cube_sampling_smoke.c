/* Cube-map sampling through the optional draw_cube backend callback.
 *
 * The null backend really samples the six CPU face stores, so this test proves
 * face order (+X,-X,+Y,-Y,+Z,-Z), normal-major-axis selection, the documented
 * UV fallback and the exact texel returned for a solid-colour A8 cube.  A
 * backend without draw_cube (the PICA shape) must refuse the draw by name and
 * never substitute face 0.  DevCaps stay 0x00082000. */
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "d3dx_texture_shims.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH+0x200)
#define CAPS (SCRATCH+0x400)
#define INFO (SCRATCH+0x500)
#define OUT (SCRATCH+0x600)
#define DATA (SCRATCH+0x800)
#define VERTS (SCRATCH+0x3000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_INVALIDDATA 0x88760b59u
#define HR_UNAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u
#define CUBE_CAPS 0x0000fe00u
#define DECLVA (SCRATCH+0x2000)

typedef struct {float x,y,z,nx,ny,nz,u,v;} CubeVertex;

static const uint32_t face_argb[6]={
    0xffff0000u, /* +X red */
    0xff00ff00u, /* -X green */
    0xff0000ffu, /* +Y blue */
    0xffffff00u, /* -Y yellow */
    0xffff00ffu, /* +Z magenta */
    0xff00ffffu, /* -Z cyan */
};

static void wr32(uint8_t *p,uint32_t v){
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"cube sampling stdcall stack mismatch");
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
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static uint32_t resolve(const char *name){PeImport e={.dll="d3dx9_42.dll",.name=name};return win32_resolve(&e,NULL);}
static uint32_t shim_call(Harness *h,const char *name,const uint32_t *args,unsigned n){
    uint32_t fn=resolve(name);assert(fn);
    uint32_t r=0;assert(!invoke(h,fn,args,n,&r));return r;
}
static uint32_t device_caps(Harness *h){assert(call(h,h->device,7,(uint32_t[]){CAPS},1)==HR_OK);return rd(h,CAPS+28);}

static size_t cube_dds(uint8_t *b,unsigned edge){
    memset(b,0,128);memcpy(b,"DDS ",4);wr32(b+4,124);
    wr32(b+8,0x1u|0x2u|0x4u|0x1000u|0x80000u);
    wr32(b+12,edge);wr32(b+16,edge);wr32(b+76,32);
    wr32(b+80,0x41u);wr32(b+88,32);wr32(b+92,0x00ff0000u);
    wr32(b+96,0x0000ff00u);wr32(b+100,0x000000ffu);wr32(b+104,0xff000000u);
    wr32(b+108,0x1000u);wr32(b+112,CUBE_CAPS);
    size_t off=128;
    for(unsigned face=0;face<6;++face)
        for(unsigned i=0;i<edge*edge;++i){wr32(b+off,face_argb[face]);off+=4;}
    return off;
}
static uint32_t create_cube(Harness *h,unsigned edge){
    uint8_t dds[4096];size_t bytes=cube_dds(dds,edge);
    assert(!guest_write(&h->memory,DATA,dds,bytes));
    uint32_t args[]={h->device,DATA,(uint32_t)bytes,OUT};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(shim_call(h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_OK);
    uint32_t cube=rd(h,OUT);assert(cube);return cube;
}
static void triangle(CubeVertex v[3],float nx,float ny,float nz,float u,float v0){
    v[0]=(CubeVertex){-1,-1,0,nx,ny,nz,u,v0};
    v[1]=(CubeVertex){ 1,-1,0,nx,ny,nz,u,v0};
    v[2]=(CubeVertex){ 0, 1,0,nx,ny,nz,u,v0};
}
static void draw_up(Harness *h,const void *verts,unsigned stride){
    assert(!guest_write(&h->memory,VERTS,verts,3*stride));
    uint32_t args[]={D9_TRIANGLELIST,1,VERTS,stride};
    assert(call(h,h->device,83,args,4)==HR_OK);
}
/* NORMAL is admitted here through an explicit vertex declaration; the FVF
 * NORMAL gate stays owned by the test-only T&L prototype. */
static uint32_t setup_decl(Harness *h){
    D9VertexElement e[4]={
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT3,0,D9_USAGE_NORMAL,0},
        {0,24,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
        {0xff,0,D9_DECL_UNUSED,0,0,0},
    };
    assert(!guest_write(&h->memory,DECLVA,e,sizeof e));
    uint32_t args[]={DECLVA,OUT};
    assert(!guest_write32(&h->memory,OUT,0));
    assert(call(h,h->device,86,args,2)==HR_OK);
    uint32_t decl=rd(h,OUT);assert(decl);
    assert(call(h,h->device,87,(uint32_t[]){decl},1)==HR_OK);
    return decl;
}
static void expect_face(Harness *h,unsigned face,unsigned increment){
    const D9NullStats *s=d9_null_stats();
    assert(s->last_cube_count==3);
    for(unsigned i=0;i<3;++i){
        assert(s->last_cube_faces[i]==face);
        assert(s->last_cube_samples[i]==face_argb[face]);
    }
    assert(s->cube_face_counts[face]>=increment);
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x6000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());
    assert(!d3dx_texture_shims_install(&h.memory));
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[3]={{"probe",probe,1},d3dx_texture_shims_module(),d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,3));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);
    uint32_t create9=d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"},NULL);
    assert(create9);
    uint32_t sdk=32;assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    assert(!guest_write(&h.memory,PP,pp,sizeof pp));
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==HR_OK);
    assert(!guest_read32(&h.memory,OUT,&h.device)&&h.device);
    assert(device_caps(&h)==0x00082000u);
    d9_null_reset();

    uint32_t cube=create_cube(&h,8);
    assert(call(&h,h.device,65,(uint32_t[]){0,cube},2)==HR_OK);
    /* The FVF NORMAL gate is still the T&L prototype's; cube sampling uses an
     * explicit declaration with NORMAL so DevCaps/T&L behaviour is unchanged. */
    assert(call(&h,h.device,89,(uint32_t[]){0x112u},1)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"NORMAL"));
    uint32_t decl=setup_decl(&h);
    assert(call(&h,h.device,41,NULL,0)==HR_OK);

    CubeVertex v[3];
    static const float axes[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(unsigned face=0;face<6;++face){
        triangle(v,axes[face][0],axes[face][1],axes[face][2],0.25f,0.75f);
        draw_up(&h,v,sizeof v[0]);
        expect_face(&h,face,3);
    }
    /* A non-axis normal still picks the major axis. */
    triangle(v,0.6f,-0.2f,0.3f,0.5f,0.5f);
    draw_up(&h,v,sizeof v[0]);
    expect_face(&h,D9_CUBE_FACE_POSITIVE_X,6);

    /* No NORMAL: documented UV fallback direction (u-0.5, v-0.5, 0). */
    static const struct {float u,v;unsigned face;} uv_cases[]={
        {0.9f,0.5f,D9_CUBE_FACE_POSITIVE_X},
        {0.1f,0.5f,D9_CUBE_FACE_NEGATIVE_X},
        {0.5f,0.1f,D9_CUBE_FACE_NEGATIVE_Y},
        {0.5f,0.9f,D9_CUBE_FACE_POSITIVE_Y},
        {0.5f,0.5f,D9_CUBE_FACE_POSITIVE_Z},
    };
    for(unsigned i=0;i<sizeof uv_cases/sizeof uv_cases[0];++i){
        triangle(v,0,0,0,uv_cases[i].u,uv_cases[i].v);
        draw_up(&h,v,sizeof v[0]);
        expect_face(&h,uv_cases[i].face,3);
    }

    /* Cube on stage 1 and cube plus a second stage are named refusals. */
    assert(call(&h,h.device,65,(uint32_t[]){0,0},2)==HR_OK);
    assert(call(&h,h.device,65,(uint32_t[]){1,cube},2)==HR_OK);
    triangle(v,1,0,0,0.5f,0.5f);
    assert(!guest_write(&h.memory,VERTS,v,sizeof v));
    assert(call(&h,h.device,83,(uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof v[0]},4)==HR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"stage 1"));
    assert(call(&h,h.device,65,(uint32_t[]){1,0},2)==HR_OK);

    /* Caps/SM3 are unchanged by cube sampling. */
    assert(device_caps(&h)==0x00082000u);
    assert((device_caps(&h)&0x00018000u)==0);

    assert(call(&h,h.device,42,NULL,0)==HR_OK);
    assert(call(&h,h.device,17,(uint32_t[]){0,0,0,0},4)==HR_OK);
    assert(call(&h,h.device,87,(uint32_t[]){0},1)==HR_OK);
    assert(call(&h,cube,2,NULL,0)==0);
    assert(call(&h,decl,2,NULL,0)==0);
    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);

    /* PICA shape: no draw_cube -> named refusal, no face-0 substitution. */
    D9NullStats direct;memset(&direct,0,sizeof direct);
    D9Backend ops=*d9_null_backend();ops.draw_cube=NULL;
    D9Device *dev=d9_create_device(&ops,&direct,1u<<20);assert(dev);
    D9Handle hc=0;
    assert(d9_create_cube_texture_ex(dev,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&hc)==D9_OK);
    uint8_t face_rgba[8*8*4];memset(face_rgba,0x7f,sizeof face_rgba);
    for(unsigned face=0;face<D9_CUBE_FACES;++face)
        assert(d9_cube_upload_face_level(dev,hc,face,0,face_rgba,32,sizeof face_rgba)==D9_OK);
    assert(d9_bind_cube_texture(dev,0,hc)==D9_OK);
    assert(d9_begin_scene_ex(dev,0,0)==D9_OK);
    assert(d9_draw_primitive(dev,D9_TRIANGLELIST,0,1)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(dev),"draw_cube"));
    assert(direct.draws==0&&direct.cube_draws==0);
    assert(d9_end_scene(dev)==D9_OK);
    assert(d9_release_cube_texture(dev,hc)==D9_OK);
    assert(d9_destroy_device_ex(dev)==D9_OK);

    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("cube sampling through draw_cube: PASS");
    return 0;
}
