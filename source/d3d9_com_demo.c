#include "d3d9_com_demo.h"
#include "d3d9_demo_scene.h"
#include "d3d9_shader.h"
#include "win32_shims.h"
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x400)
#define DATA (SCRATCH+0x500)
#define IAT (SCRATCH+0x2a0)
#define PROBE_FN 0x00401000u
extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
/* Equivalent to the indirect-call sequence produced by the upstream lifter. */
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
static int error(D9ComDemo *s,const char *msg){snprintf(s->message,sizeof s->message,"%s",msg);return -1;}
static int invoke(D9ComDemo *s,uint32_t target,const uint32_t *args,unsigned n,uint32_t *r){
    if(n>16)return error(s,"too many arguments");uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    int rc=n3ds_lift_invoke(&s->cpu,PROBE_FN,frame,n+2,r);if(rc){snprintf(s->message,sizeof s->message,"invoke %08lx: %s",(unsigned long)target,n3ds_lift_fault());return rc;}return 0;}
int d9_com_demo_call(D9ComDemo *s,uint32_t o,unsigned slot,const uint32_t *args,unsigned n,uint32_t *r){
    uint32_t vt,fn,frame[16];if(!o||slot>=119||n>=16||guest_read32(&s->memory,o,&vt)||!vt||
       guest_read32(&s->memory,vt+slot*4,&fn))return error(s,"bad COM vtable");
    frame[0]=o;for(unsigned i=0;i<n;++i)frame[i+1]=args[i];return invoke(s,fn,frame,n+1,r);
}
static int call(D9ComDemo *s,uint32_t o,unsigned slot,const uint32_t *args,unsigned n){uint32_t r=0;
    if(d9_com_demo_call(s,o,slot,args,n,&r))return -1;if(r){snprintf(s->message,sizeof s->message,"slot %u HRESULT %08lx",slot,(unsigned long)r);return -1;}return 0;}
static int read_out(D9ComDemo *s,uint32_t p,uint32_t *out){return guest_read32(&s->memory,p,out)||!*out?error(s,"missing guest output pointer"):0;}
static int matrix(D9ComDemo *s,unsigned type,const D9Matrix *m){uint32_t a[]={type,DATA};
    if(guest_write(&s->memory,DATA,m,sizeof *m))return -1;return call(s,s->device,44,a,2);}
static int release(D9ComDemo *s,uint32_t object,uint32_t *refs){return d9_com_demo_call(s,object,2,NULL,0,refs);}
static int make_import(D9ComDemo *s){GuestMemory *m=&s->memory;
    guest_write32(m,SCRATCH+0x100,0x280);guest_write32(m,SCRATCH+0x10c,0x200);guest_write32(m,SCRATCH+0x110,0x2a0);
    guest_write32(m,SCRATCH+0x280,0x240);guest_write32(m,IAT,0x240);
    guest_write(m,SCRATCH+0x200,"d3d9.dll",9);guest_write(m,SCRATCH+0x242,"Direct3DCreate9",16);
    GuestImage image={.base=SCRATCH,.size=4096,.import_rva=0x100,.import_size=40};size_t count=0;
    return pe_import_bind(m,&image,win32_resolve,NULL,&count)||count!=1?error(s,"D3D9 import bind failed"):0;}
int d9_com_demo_init(D9ComDemo *s,D9Device *d){
    memset(s,0,sizeof *s);guest_init(&s->memory,2u<<20);
    if(guest_map(&s->memory,0x10000000,0x4000)||guest_map(&s->memory,SCRATCH,4096))return error(s,"guest allocation failed");
    if(d9_com_init(&s->memory,d))return error(s,"COM init failed");s->bridge=1;
    if(win32_init(&s->memory,0x8000,"."))return error(s,"Win32 init failed");s->shims=1;
    static const FnEntry probe_table[]={{PROBE_FN,guest_dispatch_probe}};
    s->tables[0]=(FnModule){"demo-probe",probe_table,1};s->tables[1]=d9_com_module();
    if(n3ds_lift_bind_modules(&s->memory,s->tables,2))return error(s,"runtime bind failed");s->bound=1;
    n3ds_lift_init_cpu(&s->cpu,0x10004000,0);
    if(make_import(s))return -1;uint32_t import_fn,r,refs;
    uint32_t sdk=32;if(guest_read32(&s->memory,IAT,&import_fn)||invoke(s,import_fn,&sdk,1,&s->factory)||!s->factory)return error(s,"Direct3DCreate9 failed");
    if(d9_com_demo_call(s,s->factory,4,NULL,0,&r)||r!=1)return error(s,"adapter count failed");
    /* Exercise Win32 dynamic module lookup as well as the static PE IAT. */
    PeImport find_module={.dll="kernel32.dll",.name="GetModuleHandleA"},find_symbol={.dll="kernel32.dll",.name="GetProcAddress"};
    guest_write(&s->memory,DATA,"D3D9.DLL",9);uint32_t name=DATA,handle;
    if(invoke(s,win32_resolve(&find_module,NULL),&name,1,&handle)||!handle)return error(s,"GetModuleHandleA(d3d9) failed");
    guest_write(&s->memory,DATA,"Direct3DCreate9",16);uint32_t proc[]={handle,DATA};
    if(invoke(s,win32_resolve(&find_symbol,NULL),proc,2,&r)||r!=import_fn)return error(s,"GetProcAddress(d3d9) failed");
    /* D3DPRESENT_PARAMETERS x86: 56 bytes, single 400x240 backbuffer. */
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,75,0,0,1};guest_write(&s->memory,DATA,pp,sizeof pp);
    uint32_t create[]={0,1,0,0x20,DATA,OUT};
    if(call(s,s->factory,16,create,6)||read_out(s,OUT,&s->device))return -1;
    /* Query IUnknown and release its extra reference. */
    const uint8_t iid[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};guest_write(&s->memory,DATA,iid,16);
    uint32_t qi[]={DATA,OUT};if(call(s,s->device,0,qi,2)||guest_read32(&s->memory,OUT,&r)||r!=s->device||release(s,r,&refs)||refs!=1)return error(s,"QueryInterface/refcount failed");
    D9Vertex vertices[24];uint16_t idx[36];d9_demo_mesh(vertices,idx);
    uint32_t vb[]={sizeof vertices,0,0x102,1,OUT,0};
    uint32_t ib[]={sizeof idx,0,D9_INDEX16,1,OUT,0};
    uint32_t tex[]={64,64,1,0,D9_A8R8G8B8,1,OUT,0};
    if(call(s,s->device,26,vb,6)||read_out(s,OUT,&s->vb)||call(s,s->device,27,ib,6)||read_out(s,OUT,&s->ib)||
       call(s,s->device,23,tex,8)||read_out(s,OUT,&s->texture))return -1;
    /* All writes pass through real guest addresses from COM Lock/LockRect. */
    uint32_t lock[]={0,0,OUT,0};
    if(call(s,s->vb,11,lock,4)||read_out(s,OUT,&r)||guest_write(&s->memory,r,vertices,sizeof vertices)||call(s,s->vb,12,NULL,0))return error(s,"vertex buffer upload failed");
    if(call(s,s->ib,11,lock,4)||read_out(s,OUT,&r)||guest_write(&s->memory,r,idx,sizeof idx)||call(s,s->ib,12,NULL,0))return error(s,"index buffer upload failed");
    uint32_t lockrect[]={0,OUT,0,0},pitch;
    if(call(s,s->texture,19,lockrect,4)||guest_read32(&s->memory,OUT,&pitch)||pitch!=256||read_out(s,OUT+4,&r))return error(s,"texture lock failed");
    void *pixels=guest_ptr(&s->memory,r,pitch*64);if(!pixels)return error(s,"invalid locked texture");d9_demo_fill_texture(pixels,pitch);
    uint32_t level=0;if(call(s,s->texture,20,&level,1))return -1;
    uint32_t stream[]={0,s->vb,0,sizeof(D9Vertex)},stage[]={0,s->texture},fvf=0x102;
    if(call(s,s->device,100,stream,4)||call(s,s->device,104,&s->ib,1)||call(s,s->device,65,stage,2)||call(s,s->device,89,&fvf,1))return -1;
    D9Matrix view,proj;d9_demo_camera(&view,&proj);if(matrix(s,2,&view)||matrix(s,3,&proj))return -1;
    s->ready=1;snprintf(s->message,sizeof s->message,"Guest IAT, vtables and resource upload OK");return 0;
}
int d9_com_demo_frame(D9ComDemo *s,float angle){if(!s->ready)return -1;
    D9Matrix world;d9_demo_world(&world,angle);uint32_t clear[]={0,0,1,0xff14243c,0x3f800000,0};
    uint32_t draw[]={D9_TRIANGLELIST,0,0,24,0,12},present[]={0,0,0,0};
    if(s->shaders){D9Matrix view,proj,wv,wvp,rows;d9_demo_camera(&view,&proj);d9_matrix_multiply(&wv,&world,&view);d9_matrix_multiply(&wvp,&wv,&proj);
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)rows.m[i][j]=wvp.m[j][i];
        uint32_t constants[]={0,DATA,4};if(guest_write(&s->memory,DATA,&rows,sizeof rows)||call(s,s->device,94,constants,3))return -1;
    }
    if(call(s,s->device,43,clear,6)||call(s,s->device,41,NULL,0)||(!s->shaders&&matrix(s,256,&world))||call(s,s->device,82,draw,6)||
       call(s,s->device,42,NULL,0)||call(s,s->device,17,present,4))return -1;return 0;
}
int d9_com_demo_enable_bc(D9ComDemo *s){if(!s->ready)return -1;
    static const unsigned formats[]={D9_DXT1,D9_DXT3,D9_DXT5};
    for(unsigned i=0;i<3;++i){if(s->bc[i])continue;uint32_t f=formats[i];
        uint32_t check[]={0,1,D9_X8R8G8B8,0,3,f},tex[]={64,64,0,0,f,1,OUT,0};
        if(call(s,s->factory,10,check,6)||call(s,s->device,23,tex,8)||read_out(s,OUT,&s->bc[i]))return -1;
        uint32_t count;if(d9_com_demo_call(s,s->bc[i],13,NULL,0,&count)||count!=7)return error(s,"BC mip count mismatch");
        for(uint32_t level=0;level<count;++level){uint32_t w=64>>level,h=w,lock[]={level,OUT,0,0},p,pitch;size_t expected,bytes;
            if(d9_texture_layout(w,h,f,&expected,&bytes)||call(s,s->bc[i],19,lock,4)||guest_read32(&s->memory,OUT,&pitch)||
               pitch!=expected||read_out(s,OUT+4,&p))return error(s,"BC mip LockRect layout mismatch");
            if(d9_demo_fill_bc_level(guest_ptr(&s->memory,p,bytes),pitch,bytes,w,h,f,level)||call(s,s->bc[i],20,&level,1))return error(s,"BC mip upload failed");}
    }return 0;
}
int d9_com_demo_select_texture(D9ComDemo *s,unsigned mode){if(!s->ready||mode>3)return -1;
    uint32_t t=mode?s->bc[mode-1]:s->texture;if(!t)return -1;uint32_t stage[]={0,t};return call(s,s->device,65,stage,2);}
int d9_com_demo_mip(D9ComDemo *s,unsigned level){if(!s->ready||level>3)return -1;
    uint32_t filter[]={0,7,2},max[]={0,9,level};return call(s,s->device,69,filter,3)||call(s,s->device,69,max,3)?-1:0;}
int d9_com_demo_blend(D9ComDemo *s,int enabled){if(!s->ready)return -1;
    uint32_t src[]={19,5},dst[]={20,6},blend[]={27,!!enabled};return call(s,s->device,57,src,2)||call(s,s->device,57,dst,2)||call(s,s->device,57,blend,2)?-1:0;}
int d9_com_demo_enable_shaders(D9ComDemo *s){if(!s->ready)return -1;uint32_t args[]={DATA,OUT};
    if(!s->vs&&(guest_write(&s->memory,DATA,d9_demo_vs,d9_demo_vs_words*4)||call(s,s->device,91,args,2)||read_out(s,OUT,&s->vs)))return -1;
    if(!s->ps&&(guest_write(&s->memory,DATA,d9_demo_ps,d9_demo_ps_words*4)||call(s,s->device,106,args,2)||read_out(s,OUT,&s->ps)))return -1;
    return 0;}
int d9_com_demo_select_shaders(D9ComDemo *s,int enabled){if(!s->ready||(enabled&&(!s->vs||!s->ps)))return -1;
    uint32_t vs=enabled?s->vs:0,ps=enabled?s->ps:0;if(call(s,s->device,92,&vs,1)||call(s,s->device,107,&ps,1))return -1;s->shaders=!!enabled;return 0;}
int d9_com_demo_enable_decl(D9ComDemo *s){if(!s->ready)return -1;if(s->decl&&s->decl_pos&&s->decl_uv)return 0;
    D9Vertex v[24];uint16_t indices[36];d9_demo_mesh(v,indices);
    struct Position{float x,y,z;uint32_t normal;} pos[24];uint16_t uv[24][2];
    for(unsigned i=0;i<24;++i){pos[i]=(struct Position){v[i].x,v[i].y,v[i].z,511u<<20};uv[i][0]=v[i].u?0x3c00:0;uv[i][1]=v[i].v?0x3c00:0;}
    D9VertexElement decl[]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},{0,12,D9_DECL_DEC3N,0,D9_USAGE_NORMAL,0},
        {1,0,D9_DECL_FLOAT16_2,0,D9_USAGE_TEXCOORD,0},{0xff,0,D9_DECL_UNUSED,0,0,0}};
    uint32_t cd[]={DATA,OUT};if(!s->decl&&(guest_write(&s->memory,DATA,decl,sizeof decl)||call(s,s->device,86,cd,2)||read_out(s,OUT,&s->decl)))return -1;
    uint32_t vb[]={8+sizeof pos,0,0,1,OUT,0},lock[]={0,0,OUT,0},p;
    if(!s->decl_pos){if(call(s,s->device,26,vb,6)||read_out(s,OUT,&s->decl_pos)||call(s,s->decl_pos,11,lock,4)||read_out(s,OUT,&p)||
        guest_write(&s->memory,p+8,pos,sizeof pos)||call(s,s->decl_pos,12,NULL,0))return -1;}
    vb[0]=4+sizeof uv;if(!s->decl_uv){if(call(s,s->device,26,vb,6)||read_out(s,OUT,&s->decl_uv)||call(s,s->decl_uv,11,lock,4)||read_out(s,OUT,&p)||
        guest_write(&s->memory,p+4,uv,sizeof uv)||call(s,s->decl_uv,12,NULL,0))return -1;}
    return 0;
}
int d9_com_demo_select_decl(D9ComDemo *s,int enabled){if(!s->ready||(enabled&&(!s->decl||!s->decl_pos||!s->decl_uv)))return -1;
    uint32_t v0[]={0,enabled?s->decl_pos:s->vb,enabled?8:0,enabled?16:sizeof(D9Vertex)};
    uint32_t v1[]={1,enabled?s->decl_uv:0,enabled?4:0,enabled?4:0};
    if(call(s,s->device,100,v0,4)||call(s,s->device,100,v1,4))return -1;
    uint32_t fvf=0x102;if(enabled?call(s,s->device,87,&s->decl,1):call(s,s->device,89,&fvf,1))return -1;s->declaration=!!enabled;return 0;
}
int d9_com_demo_destroy(D9ComDemo *s){int rc=0;
    if(s->ready){uint32_t stream[]={0,0,0,0},stage[]={0,0},zero=0,refs;
        if(call(s,s->device,100,stream,4)||call(s,s->device,104,&zero,1)||call(s,s->device,65,stage,2))rc=-1;
        stream[0]=1;if(!rc&&(call(s,s->device,100,stream,4)||call(s,s->device,87,&zero,1)))rc=-1;
        if(!rc&&s->decl&&(release(s,s->decl,&refs)||refs))rc=-1;
        if(!rc&&s->decl_pos&&(release(s,s->decl_pos,&refs)||refs))rc=-1;
        if(!rc&&s->decl_uv&&(release(s,s->decl_uv,&refs)||refs))rc=-1;
        if(!rc)for(unsigned i=0;i<3;++i)if(s->bc[i]&&(release(s,s->bc[i],&refs)||refs)){rc=-1;break;}
        if(!rc&&d9_com_demo_select_shaders(s,0))rc=-1;
        if(!rc&&s->vs&&(release(s,s->vs,&refs)||refs))rc=-1;
        if(!rc&&s->ps&&(release(s,s->ps,&refs)||refs))rc=-1;
        if(!rc&&(release(s,s->vb,&refs)||refs||release(s,s->ib,&refs)||refs||release(s,s->texture,&refs)||refs||
           release(s,s->device,&refs)||refs||release(s,s->factory,&refs)||refs))rc=-1;
    }
    if(s->bound)n3ds_lift_release();if(s->bridge)d9_com_shutdown();if(s->shims)win32_shutdown();guest_destroy(&s->memory);
    s->ready=s->bridge=s->bound=s->shims=0;return rc;
}
