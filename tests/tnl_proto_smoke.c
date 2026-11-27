/* Test-only CPU T&L prototype.
 *
 * Two runs from one source (the lift runtime allows a single module binding per
 * process):
 *   tnl_proto_off  - production path: no flag, named refusals, honest caps.
 *   tnl_proto      - TESV_D3D9_TNL_PROTO=1: lighting math and fixed-function
 *                    state, colours checked through the null backend.
 *
 * This proves the CPU lighting math and the state/validation surface, not a
 * rendered frame and not a hardware-T&L capability: DevCaps must stay
 * 0x00082000 in both runs.
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define OUT (SCRATCH+0x100)
#define PP (SCRATCH+0x200)
#define CAPS9 (SCRATCH+0x400)
#define MAT (SCRATCH+0x600)
#define LIGHT (SCRATCH+0x700)
#define VERTS (SCRATCH+0x800)
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
    uint32_t r=0;int rc=invoke(h,fn,frame,n+1,&r);
    if(rc)fprintf(stderr,"tnl_proto: invoke fault on slot %u object 0x%08x rc=%d\n",slot,object,rc);
    assert(!rc);return r;
}
static uint32_t call1(Harness *h,uint32_t object,unsigned slot,uint32_t a){return call(h,object,slot,&a,1);}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static void wr(Harness *h,uint32_t address,const void *data,size_t bytes){assert(!guest_write(&h->memory,address,data,bytes));}
static uint32_t unref(Harness *h,uint32_t object){return call(h,object,2,NULL,0);}
static int near(float a,float b){return fabsf(a-b)<1e-3f;}

static const uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,1,0,1};

static void harness_start(Harness *h,uint32_t *device){
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
    wr(h,PP,pp,sizeof pp);uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(h,h->factory,16,create,6)==0);
    *device=rd(h,OUT);assert(*device);
}
static void harness_stop(Harness *h){d9_com_shutdown();n3ds_lift_release();guest_destroy(&h->memory);}
static uint32_t device_caps(Harness *h,uint32_t device){
    assert(call1(h,device,7,CAPS9)==0);return rd(h,CAPS9+28); /* D3DCAPS9.DevCaps */
}
static void stage_diffuse(Harness *h,uint32_t device){
    uint32_t a[3]={0,1,2};assert(call(h,device,67,a,3)==0);   /* COLOROP=SELECTARG1 */
    a[1]=2;a[2]=0;assert(call(h,device,67,a,3)==0);           /* COLORARG1=DIFFUSE */
    a[1]=4;a[2]=2;assert(call(h,device,67,a,3)==0);           /* ALPHAOP=SELECTARG1 */
    a[1]=5;a[2]=0;assert(call(h,device,67,a,3)==0);           /* ALPHAARG1=DIFFUSE */
}
static void put_material(Harness *h,const float diffuse[4],const float ambient[4],
                         const float specular[4],const float emissive[4],float power){
    uint8_t b[68];memset(b,0,sizeof b);
    memcpy(b,diffuse,16);memcpy(b+16,ambient,16);memcpy(b+32,specular,16);
    memcpy(b+48,emissive,16);memcpy(b+64,&power,4);wr(h,MAT,b,sizeof b);
}
static void put_directional(Harness *h,float dx,float dy,float dz,
                            const float diffuse[4],const float ambient[4]){
    uint8_t b[104];memset(b,0,sizeof b);
    uint32_t type=3;memcpy(b,&type,4);memcpy(b+4,diffuse,16);memcpy(b+36,ambient,16);
    float d[3]={dx,dy,dz};memcpy(b+64,d,sizeof d);wr(h,LIGHT,b,sizeof b);
}
static void put_point(Harness *h,float px,float py,float pz,float a0,float a1,float a2,
                      const float diffuse[4],const float ambient[4]){
    uint8_t b[104];memset(b,0,sizeof b);
    uint32_t type=1;memcpy(b,&type,4);memcpy(b+4,diffuse,16);memcpy(b+36,ambient,16);
    float p[3]={px,py,pz};memcpy(b+52,p,sizeof p);
    memcpy(b+84,&a0,4);memcpy(b+88,&a1,4);memcpy(b+92,&a2,4);wr(h,LIGHT,b,sizeof b);
}
typedef struct {float x,y,z,nx,ny,nz;} LitVertex; /* FVF XYZ|NORMAL (0x012) */
static void draw_lit(Harness *h,uint32_t device,const LitVertex v[3]){
    uint32_t clear[]={0,0,1,0xff000000u,0,0};
    assert(call(h,device,43,clear,6)==0);
    assert(call(h,device,41,NULL,0)==0);
    wr(h,VERTS,v,3*sizeof *v);
    uint32_t up[]={D9_TRIANGLELIST,1,VERTS,sizeof *v};
    assert(call(h,device,83,up,4)==0);
    assert(call(h,device,42,NULL,0)==0);
    uint32_t pres[4]={0,0,0,0};assert(call(h,device,17,pres,4)==0);
}
static const D9ColorVertex *last_colors(unsigned *count){
    const D9NullStats *s=d9_null_stats();*count=s->last_color_count;return s->last_color_vertices;
}

static void run_off(void){
    _putenv_s("TESV_D3D9_TNL_PROTO","0");
    Harness h;uint32_t device=0;harness_start(&h,&device);
    assert(device_caps(&h,device)==0x00082000u);
    assert(call1(&h,device,89,0x012)==0x8876086au); /* NORMAL FVF needs the prototype */
    assert(strstr(d9_com_last_diagnostic(),"NORMAL"));
    float white[4]={1,1,1,1},black[4]={0,0,0,1};
    put_directional(&h,0,0,-1,white,black);
    assert(call(&h,device,51,(uint32_t[]){0,LIGHT},2)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"TESV_D3D9_TNL_PROTO"));
    put_material(&h,white,white,black,black,0.0f);
    assert(call(&h,device,49,(uint32_t[]){MAT},1)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"TESV_D3D9_TNL_PROTO"));
    assert(call(&h,device,46,(uint32_t[]){256,MAT},2)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"TESV_D3D9_TNL_PROTO"));
    assert(call(&h,device,45,(uint32_t[]){256,MAT},2)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"TESV_D3D9_TNL_PROTO"));
    /* The production render-state surface keeps the real T&L switches closed.
     * D3DRS_LIGHTING is the deliberate exception: GFx state blocks set it, and
     * rejecting it stalled the menu, so it is stored-but-inert on the shader
     * path (see d9_set_render_state / render_slot). */
    assert(call(&h,device,57,(uint32_t[]){D9_RS_LOCALVIEWER,1},2)==0x8876086cu);
    assert(call(&h,device,57,(uint32_t[]){D9_RS_NORMALIZENORMALS,1},2)==0x8876086cu);
    assert(call(&h,device,57,(uint32_t[]){D9_RS_LIGHTING,1},2)==0);
    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    harness_stop(&h);
    puts("CPU T&L off: normal FVF/light/material/transform calls are named refusals, DevCaps 0x00082000 passed");
}

static void run_on(void){
    _putenv_s("TESV_D3D9_TNL_PROTO","1");
    Harness h;uint32_t device=0;harness_start(&h,&device);
    /* The prototype never changes the caps claim. */
    assert(device_caps(&h,device)==0x00082000u);
    assert((device_caps(&h,device)&0x00018000u)==0);
    stage_diffuse(&h,device);

    float red[4]={1,0,0,1},zero[4]={0,0,0,1},white[4]={1,1,1,1},half[4]={0.5f,0.5f,0.5f,1};
    /* Material round-trip. */
    put_material(&h,red,zero,zero,zero,0.0f);
    assert(call(&h,device,49,(uint32_t[]){MAT},1)==0);
    wr(&h,MAT,zero,64);
    assert(call(&h,device,50,(uint32_t[]){MAT},1)==0);
    float md[4];memcpy(md,guest_ptr(&h.memory,MAT,16),16);
    assert(near(md[0],1.0f)&&near(md[1],0.0f)&&near(md[2],0.0f)&&near(md[3],1.0f));

    /* One directional light: normals facing/opposing it. */
    put_directional(&h,0,0,-1,white,zero);
    assert(call(&h,device,51,(uint32_t[]){0,LIGHT},2)==0);
    assert(call(&h,device,53,(uint32_t[]){0,1},2)==0);
    assert(call(&h,device,54,(uint32_t[]){0,OUT},2)==0&&rd(&h,OUT)==1);
    assert(call1(&h,device,89,0x012)==0);
    LitVertex tri[3]={{0,0,0, 0,0,1},{1,-1,0, 0,0,-1},{0,1,0, 1,0,0}};
    draw_lit(&h,device,tri);
    unsigned count=0;const D9ColorVertex *c=last_colors(&count);
    assert(count==3);
    assert(near(c[0].r,1.0f)&&near(c[0].g,0.0f)&&near(c[0].b,0.0f)&&near(c[0].a,1.0f));
    assert(near(c[1].r,0.0f)&&near(c[1].g,0.0f)&&near(c[1].b,0.0f));
    assert(near(c[2].r,0.0f)&&near(c[2].g,0.0f)&&near(c[2].b,0.0f));
    assert(call(&h,device,52,(uint32_t[]){0,OUT},2)==0&&rd(&h,OUT)==3); /* GetLight type */

    /* Point light with linear attenuation: d=2, attenuation=0.5. */
    assert(call(&h,device,53,(uint32_t[]){0,0},2)==0);
    put_point(&h,0,0,2,0,1,0,white,zero);
    assert(call(&h,device,51,(uint32_t[]){1,LIGHT},2)==0);
    assert(call(&h,device,53,(uint32_t[]){1,1},2)==0);
    draw_lit(&h,device,tri);
    c=last_colors(&count);assert(count==3);
    assert(near(c[0].r,0.5f)&&near(c[0].g,0.0f)&&near(c[0].b,0.0f));

    /* Ambient: material ambient * global ambient, no lights. */
    assert(call(&h,device,53,(uint32_t[]){1,0},2)==0);
    assert(call(&h,device,57,(uint32_t[]){139,0x00404040u},2)==0);
    put_material(&h,red,half,zero,zero,0.0f);
    assert(call(&h,device,49,(uint32_t[]){MAT},1)==0);
    draw_lit(&h,device,tri);
    c=last_colors(&count);
    assert(near(c[0].r,0.125f)&&near(c[0].g,0.125f)&&near(c[0].b,0.125f));

    /* Out-of-range point light: its ambient still applies, its diffuse is cut. */
    assert(call(&h,device,57,(uint32_t[]){139,0},2)==0);
    float quarter[4]={0.25f,0.25f,0.25f,1};
    put_point(&h,0,0,100,1,0,0,white,quarter);
    float range=1.0f;wr(&h,LIGHT+76,&range,4);
    assert(call(&h,device,51,(uint32_t[]){0,LIGHT},2)==0);
    assert(call(&h,device,53,(uint32_t[]){0,1},2)==0);
    put_material(&h,red,quarter,zero,zero,0.0f);
    assert(call(&h,device,49,(uint32_t[]){MAT},1)==0);
    draw_lit(&h,device,tri);
    c=last_colors(&count);
    assert(near(c[0].r,0.0625f)&&near(c[0].g,0.0625f)&&near(c[0].b,0.0625f));
    assert(call(&h,device,53,(uint32_t[]){0,0},2)==0);
    put_material(&h,red,zero,zero,zero,0.0f);
    assert(call(&h,device,49,(uint32_t[]){MAT},1)==0);

    /* Transforms: Get, Set, MultiplyTransform (World = World * m). */
    D9Matrix id;d9_matrix_identity(&id);
    assert(call(&h,device,45,(uint32_t[]){256,MAT},2)==0);
    D9Matrix got;memcpy(&got,guest_ptr(&h.memory,MAT,sizeof got),sizeof got);
    assert(near(got.m[0][0],1.0f)&&near(got.m[3][3],1.0f));
    D9Matrix t1;d9_matrix_identity(&t1);t1.m[3][0]=10;t1.m[3][1]=20;t1.m[3][2]=30;
    wr(&h,MAT,&t1,sizeof t1);assert(call(&h,device,44,(uint32_t[]){256,MAT},2)==0);
    D9Matrix t2;d9_matrix_identity(&t2);t2.m[3][0]=1;t2.m[3][1]=2;t2.m[3][2]=3;
    wr(&h,MAT,&t2,sizeof t2);assert(call(&h,device,46,(uint32_t[]){256,MAT},2)==0);
    assert(call(&h,device,45,(uint32_t[]){256,MAT},2)==0);
    memcpy(&got,guest_ptr(&h.memory,MAT,sizeof got),sizeof got);
    assert(near(got.m[3][0],11.0f)&&near(got.m[3][1],22.0f)&&near(got.m[3][2],33.0f));
    wr(&h,MAT,&id,sizeof id);assert(call(&h,device,44,(uint32_t[]){256,MAT},2)==0);

    /* Negative: a stage that only sources TEXTURE would drop the lit colour. */
    assert(call(&h,device,67,(uint32_t[]){0,1,2},3)==0); /* COLOROP=SELECTARG1 */
    assert(call(&h,device,67,(uint32_t[]){0,2,2},3)==0); /* COLORARG1=TEXTURE */
    {uint32_t c2[]={0,0,1,0xff000000u,0,0};
     assert(call(&h,device,43,c2,6)==0&&call(&h,device,41,NULL,0)==0);
     wr(&h,VERTS,tri,sizeof tri);
     uint32_t hr=call(&h,device,83,(uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof tri[0]},4);
     assert(hr==0x8876086au&&strstr(d9_com_last_diagnostic(),"stage 0"));
     assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,(uint32_t[]){0,0,0,0},4)==0);}
    stage_diffuse(&h,device);

    /* Negative: lighting without a normal is a named refusal. */
    assert(call1(&h,device,89,0x102)==0);
    uint32_t clear[]={0,0,1,0xff000000u,0,0};
    assert(call(&h,device,43,clear,6)==0&&call(&h,device,41,NULL,0)==0);
    D9Vertex plain[3]={{-1,-1,0,0,0},{1,-1,0,1,0},{0,1,0,0.5f,1}};
    wr(&h,VERTS,plain,sizeof plain);
    assert(call(&h,device,83,(uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof(D9Vertex)},4)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"NORMAL"));
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,(uint32_t[]){0,0,0,0},4)==0);

    /* Negative: spot, too many lights, non-finite values, specular/local viewer. */
    uint8_t light[104];memset(light,0,sizeof light);uint32_t spot=2;memcpy(light,&spot,4);
    wr(&h,LIGHT,light,sizeof light);
    {uint32_t hr=call(&h,device,51,(uint32_t[]){0,LIGHT},2);
     if(hr!=0x8876086au)fprintf(stderr,"spot hr=0x%08x diag=%s\n",hr,d9_com_last_diagnostic());
     assert(hr==0x8876086au);}
    assert(strstr(d9_com_last_diagnostic(),"point and directional"));
    assert(call(&h,device,51,(uint32_t[]){8,LIGHT},2)==0x8876086cu);
    put_directional(&h,0,0,-1,white,zero);
    ((float *)guest_ptr(&h.memory,LIGHT+4,16))[0]=NAN;
    assert(call(&h,device,51,(uint32_t[]){0,LIGHT},2)==0x8876086cu);
    assert(call(&h,device,57,(uint32_t[]){29,1},2)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"specular"));
    assert(call(&h,device,57,(uint32_t[]){146,1},2)==0x8876086au);
    assert(strstr(d9_com_last_diagnostic(),"local viewer"));

    /* Negative: singular world cannot transform the normal. */
    assert(call1(&h,device,89,0x012)==0);
    D9Matrix singular;memset(&singular,0,sizeof singular);singular.m[3][3]=1;
    wr(&h,MAT,&singular,sizeof singular);assert(call(&h,device,44,(uint32_t[]){256,MAT},2)==0);
    assert(call(&h,device,43,clear,6)==0&&call(&h,device,41,NULL,0)==0);
    wr(&h,VERTS,tri,sizeof tri);
    assert(call(&h,device,83,(uint32_t[]){D9_TRIANGLELIST,1,VERTS,sizeof tri[0]},4)==0x8876086cu);
    assert(strstr(d9_com_last_diagnostic(),"singular"));
    assert(call(&h,device,42,NULL,0)==0&&call(&h,device,17,(uint32_t[]){0,0,0,0},4)==0);
    wr(&h,MAT,&id,sizeof id);assert(call(&h,device,44,(uint32_t[]){256,MAT},2)==0);

    assert(device_caps(&h,device)==0x00082000u);
    assert(unref(&h,device)==0&&unref(&h,h.factory)==0);
    harness_stop(&h);
    puts("CPU T&L prototype: directional/point/ambient lighting, material/light state, "
         "MultiplyTransform, named refusals, upload-through-null colours and honest caps passed");
}

int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"off")){run_off();return 0;}
    run_on();return 0;
}
