/* gfxmesh COM smoke: first GFx batch admission at the D3D9 COM layer.
 *
 * Proves the revised create_vb / create_ib / create_tex / SetFVF /
 * SetTextureStageState contract against the real guest bridge:
 *   - CreateVertexBuffer(usage=0x208, FVF=0) and FVF=0x144 are admitted;
 *   - CreateIndexBuffer(usage=0x208) is admitted;
 *   - CreateTexture(usage=0x200) is admitted and lockable;
 *   - SetFVF(0x144) stores the FVF and builds the declaration;
 *   - SetTextureStageState slot 67 accepts the GFx stage-0 subset and rejects
 *     anything outside it by name;
 *   - DrawIndexedPrimitive with colour returns D3DERR_NOTAVAILABLE and the
 *     diagnostic names COLOR0 when the backend has no colour shader;
 *   - the same pre-transformed geometry without colour draws, with the
 *     viewport->PICA clip matrix, so the only remaining blocker is the shader.
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "win32_shims.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const char *d9_com_last_diagnostic(void);

#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_INVALIDCALL 0x8876086cu

typedef struct {unsigned draws;size_t n;D9Matrix clip;int active;} Mock;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;(void)levels;*native=(void *)1;return 0;}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return 0;}
static void tex_delete(void *ctx,void *native){(void)ctx;(void)native;}
static int begin(void *ctx,uint32_t color){(void)color;Mock *m=ctx;assert(!m->active);m->active=1;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *tex,const D9DrawState *s){
    (void)v;(void)tex;(void)s;Mock *m=ctx;assert(m->active&&clip);m->n=n;m->clip=*clip;++m->draws;return 0;}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;}
static const D9Backend ops={
    .texture_create=tex_create,.texture_upload=tex_upload,.texture_delete=tex_delete,
    .begin=begin,.draw=draw,.end=end};

typedef struct {uint32_t hr;uint32_t r;} R;
static R call(D9ComDemo *s,uint32_t obj,unsigned slot,const uint32_t *a,unsigned n){
    R out={0,0};assert(!d9_com_demo_call(s,obj,slot,a,n,&out.hr));return out;}
static uint32_t rd(D9ComDemo *s,uint32_t addr){uint32_t v=0;assert(!guest_read32(&s->memory,addr,&v));return v;}
static void wr(D9ComDemo *s,uint32_t addr,const void *p,size_t n){assert(!guest_write(&s->memory,addr,p,n));}

int main(void){
    Mock m={0};D9Device *d=d9_create_device(&ops,&m,4u<<20);assert(d);
    D9ComDemo s;if(d9_com_demo_init(&s,d)){fprintf(stderr,"init: %s\n",s.message);return 1;}
    const uint32_t OUT=0x10010400u,DATA=0x10010500u;

    /* --- dynamic vertex buffer with GFx FVF=0 --------------------------- */
    uint32_t vbargs[]={4*28,0x208,0,0,OUT,0};
    R r=call(&s,s.device,26,vbargs,6);assert(r.hr==0);uint32_t vb=rd(&s,OUT);assert(vb);
    uint32_t desc[6]={0,0,0,0,0,0};uint32_t dargs[]={DATA};
    assert(call(&s,vb,13,dargs,1).hr==0);
    for(unsigned i=0;i<6;++i)desc[i]=rd(&s,DATA+i*4);
    assert(desc[2]==0x208&&desc[4]==4*28&&desc[5]==0); /* usage, size, FVF */

    /* CreateVertexBuffer(FVF=0x144) is legal: the FVF is a buffer hint. */
    uint32_t vb2args[]={4*28,0x208,0x144,0,OUT,0};
    assert(call(&s,s.device,26,vb2args,6).hr==0);uint32_t vb2=rd(&s,OUT);assert(vb2);
    uint32_t vb3args[]={4*28,0x208,0x1ff,0,OUT,0};
    assert(call(&s,s.device,26,vb3args,6).hr==D3DERR_NOTAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"FVF"));

    /* Fill the dynamic buffer through Lock/Unlock at real guest addresses. */
    typedef struct {float x,y,z,w;uint32_t color;float u,v;} RhwColorVertex;
    RhwColorVertex cv[4]={
        {0,0,0.5f,1.0f,0x80ff0000u,0,0},
        {400,0,0.5f,1.0f,0xff00ff00u,1,0},
        {0,240,0.5f,1.0f,0xff0000ffu,0,1},
        {400,240,0.5f,1.0f,0xffffffffu,1,1}};
    uint32_t lock[]={0,sizeof cv,OUT,0};assert(call(&s,vb,11,lock,4).hr==0);
    uint32_t p=rd(&s,OUT);assert(p);wr(&s,p,cv,sizeof cv);assert(call(&s,vb,12,NULL,0).hr==0);

    /* --- dynamic index buffer ------------------------------------------ */
    uint32_t ibrgs[]={12,0x208,D9_INDEX16,0,OUT,0};assert(call(&s,s.device,27,ibrgs,6).hr==0);
    uint32_t ib=rd(&s,OUT);assert(ib);
    uint16_t idx[6]={0,1,2,0,2,3};
    uint32_t ilock[]={0,sizeof idx,OUT,0};assert(call(&s,ib,11,ilock,4).hr==0);
    p=rd(&s,OUT);wr(&s,p,idx,sizeof idx);assert(call(&s,ib,12,NULL,0).hr==0);

    /* --- dynamic glyph-style texture ----------------------------------- */
    uint32_t texargs[]={8,8,1,0x200,D9_A8R8G8B8,0,OUT,0};
    assert(call(&s,s.device,23,texargs,8).hr==0);uint32_t tex=rd(&s,OUT);assert(tex);
    uint32_t tdesc[]={0,DATA};assert(call(&s,tex,17,tdesc,2).hr==0);
    assert(rd(&s,DATA+8)==0x200&&rd(&s,DATA+24)==8&&rd(&s,DATA+28)==8);
    uint32_t tlock[]={0,OUT,0,0};assert(call(&s,tex,19,tlock,4).hr==0);
    uint32_t tp=rd(&s,OUT+4);assert(tp);memset(guest_ptr(&s.memory,tp,256),0x7f,256);
    uint32_t level=0;assert(call(&s,tex,20,&level,1).hr==0);

    /* --- bind state and stage-0 subset --------------------------------- */
    uint32_t stream[]={0,vb,0,sizeof cv[0]};
    uint32_t setind[]={ib},settex[]={0,tex},stagefvf=0x144;
    assert(call(&s,s.device,100,stream,4).hr==0);
    assert(call(&s,s.device,104,setind,1).hr==0);
    assert(call(&s,s.device,65,settex,2).hr==0);
    assert(call(&s,s.device,89,&stagefvf,1).hr==0);
    uint32_t gfvf[]={OUT};assert(call(&s,s.device,90,gfvf,1).hr==0&&rd(&s,OUT)==0x144);

    uint32_t tss[][3]={{0,1,4},{0,2,2},{0,3,0},{0,4,4},{0,5,2},{0,6,0},{0,11,0},{1,1,1}};
    for(unsigned i=0;i<sizeof tss/sizeof tss[0];++i)assert(call(&s,s.device,67,tss[i],3).hr==0);
    uint32_t badtss[]={0,1,22};assert(call(&s,s.device,67,badtss,3).hr==D3DERR_NOTAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(),"MODULATE"));
    uint32_t badstage[]={3,1,4};assert(call(&s,s.device,67,badstage,3).hr==D3DERR_NOTAVAILABLE);

    /* --- coloured GFx draw fails by name, never silently --------------- */
    assert(call(&s,s.device,41,NULL,0).hr==0);
    unsigned draws=m.draws;
    uint32_t drawargs[]={D9_TRIANGLELIST,0,0,4,0,2};
    assert(call(&s,s.device,82,drawargs,6).hr==D3DERR_NOTAVAILABLE);
    assert(m.draws==draws);
    assert(strstr(d9_com_last_diagnostic(),"COLOR0"));
    assert(call(&s,s.device,42,NULL,0).hr==0);assert(call(&s,s.device,17,(uint32_t[]){0,0,0,0},4).hr==0);

    /* --- same geometry, no colour: the pre-transform path is live ------ */
    typedef struct {float x,y,z,w,u,v;} RhwVertex;
    RhwVertex rv[4]={{0,0,0.5f,1,0,0},{400,0,0.5f,1,1,0},{0,240,0.5f,1,0,1},{400,240,0.5f,1,1,1}};
    uint32_t vb4args[]={sizeof rv,0x208,0x104,0,OUT,0};assert(call(&s,s.device,26,vb4args,6).hr==0);
    uint32_t vb4=rd(&s,OUT);assert(vb4);
    uint32_t lock4[]={0,sizeof rv,OUT,0};assert(call(&s,vb4,11,lock4,4).hr==0);
    p=rd(&s,OUT);wr(&s,p,rv,sizeof rv);assert(call(&s,vb4,12,NULL,0).hr==0);
    assert(call(&s,s.device,41,NULL,0).hr==0);
    uint32_t stream4[]={0,vb4,0,sizeof rv[0]};assert(call(&s,s.device,100,stream4,4).hr==0);
    uint32_t fvf4=0x104;assert(call(&s,s.device,89,&fvf4,1).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==0&&m.draws==draws+1);
    assert(m.n==6);
    assert(m.clip.m[0][1]==-2.0f/240.0f&&m.clip.m[0][3]==1.0f);
    assert(m.clip.m[1][0]==-2.0f/400.0f&&m.clip.m[1][3]==1.0f);
    assert(m.clip.m[2][2]==1.0f&&m.clip.m[2][3]==-1.0f);

    /* --- the original XYZ|TEX1 path is unchanged ----------------------- */
    uint32_t fvf=0x102;assert(call(&s,s.device,89,&fvf,1).hr==0);
    uint32_t stream0[]={0,s.vb,0,sizeof(D9Vertex)};assert(call(&s,s.device,100,stream0,4).hr==0);
    assert(call(&s,s.device,104,&s.ib,1).hr==0);
    uint32_t draw0[]={D9_TRIANGLELIST,0,0,24,0,12};
    assert(call(&s,s.device,82,draw0,6).hr==0&&m.draws==draws+2);

    assert(call(&s,s.device,42,NULL,0).hr==0);assert(call(&s,s.device,17,(uint32_t[]){0,0,0,0},4).hr==0);
    d9_com_demo_destroy(&s);
    d9_destroy_device(d);
    puts("gfxmesh COM smoke: PASS");
    return 0;
}
