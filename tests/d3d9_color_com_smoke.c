/* Vertex-colour draw through the real D3D9 COM bridge.
 *
 * Proves the end-to-end chain the PICA backend adds:
 *   guest DrawIndexedPrimitive(FVF 0x144 + stage-0 GFx tuple)
 *     -> d3d9_com.c -> d3d9_core.c -> D9Backend.draw_colored
 *     -> d9_color_tev_stage0() (single TEV stage) -> backend success.
 *
 * A stage tuple that cannot be expressed in one TEV stage (TEXTURE selected
 * while no texture is bound, TEXCOORDINDEX != 0, DISABLE) makes the mapping
 * return D9_UNSUPPORTED, the backend refuses, and COM reports
 * D3DERR_NOTAVAILABLE with no backend draw issued.  This does not prove a
 * PICA pixel; it proves the refusal/acceptance contract. */
#include "d3d9_com_demo.h"
#include "d3d9_color_tev.h"
#include "d3d9_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3DERR_NOTAVAILABLE 0x8876086au

extern const char *d9_com_last_diagnostic(void);

typedef struct {size_t base;unsigned levels;uint8_t *data;} Tex;
typedef struct {
    unsigned created,uploaded,draws,color_draws;size_t n;D9Matrix clip;
    D9DrawState state;D9ColorVertex cv[8];D9ColorTevStage plan;int active;
} Mock;

static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    Mock *m=ctx;Tex *t=calloc(1,sizeof *t);if(!t)return -1;
    t->base=(size_t)w*h*4;t->levels=levels;size_t total=0;
    for(unsigned i=0;i<levels;++i)total+=t->base>>(2*i);
    t->data=malloc(total);if(!t->data){free(t);return -1;}*native=t;++m->created;return 0;}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){
    Mock *m=ctx;Tex *t=native;assert(level<t->levels&&n==(t->base>>(2*level)));
    size_t off=0;for(unsigned i=0;i<level;++i)off+=t->base>>(2*i);
    memcpy(t->data+off,data,n);++m->uploaded;return 0;}
static void tex_delete(void *ctx,void *native){Mock *m=ctx;(void)m;free(((Tex *)native)->data);free(native);}
static int begin(void *ctx,uint32_t color){(void)color;Mock *m=ctx;assert(!m->active);m->active=1;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *t,const D9DrawState *s){
    (void)v;(void)t;Mock *m=ctx;assert(m->active&&clip&&s);m->n=n;m->clip=*clip;m->state=*s;++m->draws;return 0;}
static int draw_colored(void *ctx,const D9ColorVertex *v,size_t n,const D9Matrix *clip,void *t,const D9DrawState *s){
    Mock *m=ctx;const char *why=NULL;assert(m->active&&v&&clip&&s&&n<=8);
    if(d9_color_tev_stage0(s,t!=NULL,&m->plan,&why))return -1;
    m->n=n;m->clip=*clip;m->state=*s;memcpy(m->cv,v,n*sizeof *v);++m->color_draws;return 0;}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;}
static const D9Backend ops={
    .texture_create=tex_create,.texture_upload=tex_upload,.texture_delete=tex_delete,
    .begin=begin,.draw=draw,.end=end,.draw_colored=draw_colored};

typedef struct {uint32_t hr;} R;
static R call(D9ComDemo *s,uint32_t obj,unsigned slot,const uint32_t *a,unsigned n){
    R out={0};assert(!d9_com_demo_call(s,obj,slot,a,n,&out.hr));return out;}
static uint32_t rd(D9ComDemo *s,uint32_t addr){uint32_t v=0;assert(!guest_read32(&s->memory,addr,&v));return v;}
static void wr(D9ComDemo *s,uint32_t addr,const void *p,size_t n){assert(!guest_write(&s->memory,addr,p,n));}

int main(void){
    Mock m={0};D9Device *d=d9_create_device(&ops,&m,4u<<20);assert(d);
    D9ComDemo s;
    if(d9_com_demo_init(&s,d)){fprintf(stderr,"init: %s\n",s.message);return 1;}
    const uint32_t OUT=0x10010400u;

    typedef struct {float x,y,z,w;uint32_t color;float u,v;} RhwColorVertex;
    RhwColorVertex cv[4]={
        {0,0,0.5f,1.0f,0x80ff0000u,0,0},
        {400,0,0.5f,1.0f,0xff00ff00u,1,0},
        {0,240,0.5f,1.0f,0xff0000ffu,0,1},
        {400,240,0.5f,1.0f,0xffffffffu,1,1}};
    uint32_t vbargs[]={sizeof cv,0x208,0,0,OUT,0};
    assert(call(&s,s.device,26,vbargs,6).hr==0);uint32_t vb=rd(&s,OUT);assert(vb);
    uint32_t lock[]={0,sizeof cv,OUT,0};assert(call(&s,vb,11,lock,4).hr==0);
    uint32_t p=rd(&s,OUT);assert(p);wr(&s,p,cv,sizeof cv);assert(call(&s,vb,12,NULL,0).hr==0);

    uint16_t idx[6]={0,1,2,0,2,3};
    uint32_t ibrgs[]={sizeof idx,0x208,D9_INDEX16,0,OUT,0};assert(call(&s,s.device,27,ibrgs,6).hr==0);
    uint32_t ib=rd(&s,OUT);assert(ib);
    uint32_t ilock[]={0,sizeof idx,OUT,0};assert(call(&s,ib,11,ilock,4).hr==0);
    p=rd(&s,OUT);assert(p);wr(&s,p,idx,sizeof idx);assert(call(&s,ib,12,NULL,0).hr==0);

    uint32_t stream[]={0,vb,0,sizeof cv[0]};
    uint32_t setind[]={ib},settex[]={0,s.texture},fvf=0x144;
    assert(call(&s,s.device,100,stream,4).hr==0);
    assert(call(&s,s.device,104,setind,1).hr==0);
    assert(call(&s,s.device,65,settex,2).hr==0);
    assert(call(&s,s.device,89,&fvf,1).hr==0);

    uint32_t gfx[][3]={{0,1,4},{0,2,2},{0,3,0},{0,4,4},{0,5,2},{0,6,0},{0,11,0}};
    for(unsigned i=0;i<sizeof gfx/sizeof gfx[0];++i)assert(call(&s,s.device,67,gfx[i],3).hr==0);

    uint32_t drawargs[]={D9_TRIANGLELIST,0,0,4,0,2};

    /* --- supported GFx tuple draws through draw_colored ----------------- */
    assert(call(&s,s.device,41,NULL,0).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==0);
    assert(m.color_draws==1&&m.n==6&&m.draws==0);
    assert(m.plan.color_func==D9_CTEV_MODULATE&&m.plan.color_src[0]==D9_CTEV_SRC_TEXTURE&&
           m.plan.color_src[1]==D9_CTEV_SRC_PRIMARY);
    assert(m.plan.alpha_func==D9_CTEV_MODULATE&&m.plan.alpha_src[1]==D9_CTEV_SRC_PRIMARY);
    /* The exact RGBA decoded by the core reaches the backend. */
    assert(m.cv[0].r==1.0f&&m.cv[0].g==0.0f&&m.cv[0].b==0.0f&&m.cv[0].a==128.0f/255.0f);
    assert(m.cv[1].g==1.0f&&m.cv[2].b==1.0f&&m.cv[5].a==1.0f);
    assert(m.state.stage_colorop==D9_TOP_MODULATE);
    assert(m.clip.m[0][1]==-2.0f/240.0f&&m.clip.m[0][3]==1.0f);

    /* --- TEXTURE selected with no bound texture: refuse, do not draw ---- */
    unsigned before=m.color_draws;
    assert(call(&s,s.device,65,(uint32_t[]){0,0},2).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==D3DERR_NOTAVAILABLE);
    assert(m.color_draws==before);
    assert(strstr(d9_com_last_diagnostic(),"colour draw rejected"));

    /* --- TEXCOORDINDEX 1 has no texcoord1 in the colour shader ---------- */
    assert(call(&s,s.device,65,settex,2).hr==0);
    assert(call(&s,s.device,67,(uint32_t[]){0,11,1},3).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==D3DERR_NOTAVAILABLE);
    assert(m.color_draws==before);

    /* --- DISABLE on stage 0 is not a single TEV stage ------------------- */
    assert(call(&s,s.device,67,(uint32_t[]){0,11,0},3).hr==0);
    assert(call(&s,s.device,67,(uint32_t[]){0,1,D9_TOP_DISABLE},3).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==D3DERR_NOTAVAILABLE);
    assert(m.color_draws==before);

    /* --- SELECTARG2(CURRENT) is expressible and maps to PRIMARY --------- */
    assert(call(&s,s.device,67,(uint32_t[]){0,1,D9_TOP_SELECTARG2},3).hr==0);
    assert(call(&s,s.device,67,(uint32_t[]){0,3,D9_TA_CURRENT},3).hr==0);
    assert(call(&s,s.device,82,drawargs,6).hr==0);
    assert(m.color_draws==before+1);
    assert(m.plan.color_func==D9_CTEV_REPLACE&&m.plan.color_src[0]==D9_CTEV_SRC_CURRENT);

    assert(call(&s,s.device,42,NULL,0).hr==0);
    assert(call(&s,s.device,17,(uint32_t[]){0,0,0,0},4).hr==0);

    call(&s,s.device,67,(uint32_t[]){0,1,D9_TOP_SELECTARG1},3);
    d9_com_demo_destroy(&s);
    d9_destroy_device(d);
    puts("d3d9 colour COM smoke: PASS");
    return 0;
}
