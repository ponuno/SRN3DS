/* gfxmesh core smoke: GFx pre-transformed batch admission.
 *
 * This test is deliberately backend-honest:
 *   - POSITIONT/XYZRHW (0x104) decodes and draws through the existing
 *     XYZ+TEX1 PICA path by supplying the viewport->clip matrix;
 *   - COLOR0 (0x142/0x144 or POSITIONT+COLOR0+TEXCOORD0 declarations) is
 *     decoded into D9ColorVertex, but a backend without draw_colored gets
 *     D9_UNSUPPORTED and a named diagnostic -- the tint is never dropped;
 *   - a colour-capable mock backend proves the core plumbing is complete;
 *   - the closed SetTextureStageState subset stores and validates stage 0;
 *   - D3DUSAGE_DYNAMIC textures remain lockable/uploadable.
 */
#include "d3d9_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned draws,color_draws;size_t n;D9Vertex v[64];D9ColorVertex cv[64];
    D9Matrix clip;D9DrawState state;void *texture;int active;
} Mock;

static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;(void)levels;*native=(void *)1;return 0;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return 0;
}
static void tex_delete(void *ctx,void *native){(void)ctx;(void)native;}
static int begin(void *ctx,uint32_t argb){(void)argb;Mock *m=ctx;assert(!m->active);m->active=1;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    Mock *m=ctx;assert(m->active&&v&&clip&&n<=64);m->n=n;m->clip=*clip;m->texture=texture;m->state=*state;
    memcpy(m->v,v,n*sizeof *v);++m->draws;return 0;
}
static int draw_colored(void *ctx,const D9ColorVertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    Mock *m=ctx;assert(m->active&&v&&clip&&n<=64);m->n=n;m->clip=*clip;m->texture=texture;m->state=*state;
    memcpy(m->cv,v,n*sizeof *v);++m->color_draws;return 0;
}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;}

static const D9Backend gray_ops={
    .texture_create=tex_create,.texture_upload=tex_upload,.texture_delete=tex_delete,
    .begin=begin,.draw=draw,.end=end};
static const D9Backend color_ops={
    .texture_create=tex_create,.texture_upload=tex_upload,.texture_delete=tex_delete,
    .begin=begin,.draw=draw,.end=end,.draw_colored=draw_colored};

typedef struct {float x,y,z,w,u,v;} RhwVertex;
typedef struct {float x,y,z,w;uint32_t color;float u,v;} RhwColorVertex;
typedef struct {float x,y,z;uint32_t color;float u,v;} XyzColorVertex;

static D9Handle upload_vb(D9Device *d,const void *data,size_t bytes){
    D9Handle h;assert(!d9_create_vertex_buffer(d,bytes,&h));void *p;
    assert(!d9_buffer_lock(d,h,0,bytes,&p));memcpy(p,data,bytes);assert(!d9_buffer_unlock(d,h));return h;
}
static D9Handle upload_ib(D9Device *d,const uint16_t *idx,size_t bytes){
    D9Handle h;assert(!d9_create_index_buffer(d,bytes,D9_INDEX16,&h));void *p;
    assert(!d9_buffer_lock(d,h,0,bytes,&p));memcpy(p,idx,bytes);assert(!d9_buffer_unlock(d,h));return h;
}
static void bind_draw(D9Device *d,D9Handle vb,D9Handle ib,size_t stride,const D9Decl *decl){
    assert(!d9_set_stream_source(d,vb,0,stride));assert(!d9_set_indices(d,ib));
    assert(!d9_set_vertex_declaration(d,decl));
    assert(!d9_set_texture(d,0));
}

int main(void){
    const uint16_t idx[6]={0,1,2,0,2,3};
    D9Matrix m;
    const char *why=NULL;
    D9Decl rhw,rhwc,xyzc,plain,unsupported;

    /* --- FVF decode: exact required byte sizes and semantics ------------- */
    assert(!d9_decl_from_fvf(0x102,0,&plain,&why));
    assert(!d9_decl_from_fvf(0x104,0,&rhw,&why));
    assert(rhw.position_t&&rhw.color==UINT32_MAX&&rhw.uv!=UINT32_MAX&&rhw.required[0]==24);
    assert(!d9_decl_from_fvf(0x142,0,&xyzc,&why));
    assert(!xyzc.position_t&&xyzc.color!=UINT32_MAX&&xyzc.required[0]==24);
    assert(!d9_decl_from_fvf(0x144,0,&rhwc,&why));
    assert(rhwc.position_t&&rhwc.color!=UINT32_MAX&&rhwc.uv!=UINT32_MAX&&rhwc.required[0]==28);
    assert(d9_decl_from_fvf(0x112,0,&unsupported,&why)==D9_UNSUPPORTED&&why&&strstr(why,"unsupported"));
    assert(d9_decl_from_fvf(0x1ff,0,&unsupported,&why)==D9_UNSUPPORTED);
    /* The prototype widens exactly one bit: NORMAL (production stays closed). */
    D9Decl lit;
    assert(!d9_decl_from_fvf(0x112,1,&lit,&why));
    assert(lit.normal!=UINT32_MAX&&lit.uv!=UINT32_MAX&&lit.required[0]==32);
    assert(!d9_decl_from_fvf(0x012,1,&lit,&why));
    assert(lit.normal!=UINT32_MAX&&lit.uv==UINT32_MAX&&lit.required[0]==24);
    assert(d9_decl_from_fvf(0x112,0,&lit,&why)==D9_UNSUPPORTED);

    /* --- POSITIONT without colour draws today --------------------------- */
    Mock gray={0};
    D9Device *d=d9_create_device(&gray_ops,&gray,4u<<20);assert(d);
    assert(!d9_set_viewport_size(d,400,240));
    RhwVertex rv[4]={
        {0.0f,0.0f,0.5f,1.0f,0.0f,0.0f},
        {400.0f,0.0f,0.5f,1.0f,1.0f,0.0f},
        {0.0f,240.0f,0.5f,1.0f,0.0f,1.0f},
        {400.0f,240.0f,0.5f,1.0f,1.0f,1.0f}};
    D9Handle vb=upload_vb(d,rv,sizeof rv),ib=upload_ib(d,idx,sizeof idx);
    bind_draw(d,vb,ib,sizeof rv[0],&rhw);
    assert(!d9_begin_scene(d,0xff000000));
    assert(!d9_draw_indexed_range(d,D9_TRIANGLELIST,0,0,4,0,2));
    assert(gray.draws==1&&gray.n==6);
    /* pica_x = 1 - 2y/240, pica_y = 1 - 2x/400, pica_z = z - 1 */
    assert(gray.clip.m[0][1]==-2.0f/240.0f&&gray.clip.m[0][3]==1.0f);
    assert(gray.clip.m[1][0]==-2.0f/400.0f&&gray.clip.m[1][3]==1.0f);
    assert(gray.clip.m[2][2]==1.0f&&gray.clip.m[2][3]==-1.0f);
    assert(gray.v[0].x==0.0f&&gray.v[0].y==0.0f&&gray.v[0].z==0.5f);
    assert(!d9_present(d));

    /* --- POSITIONT + COLOR0 must fail explicitly, not silently ---------- */
    RhwColorVertex cv[4]={
        {0,0,0.5f,1.0f,0x80ff0000u,0,0},
        {400,0,0.5f,1.0f,0xff00ff00u,1,0},
        {0,240,0.5f,1.0f,0xff0000ffu,0,1},
        {400,240,0.5f,1.0f,0xffffffffu,1,1}};
    D9Handle cvb=upload_vb(d,cv,sizeof cv);
    assert(!d9_set_stream_source(d,cvb,0,sizeof cv[0]));
    assert(!d9_set_vertex_declaration(d,&rhwc));
    assert(!d9_begin_scene(d,0xff000000));
    unsigned before=gray.draws;
    assert(d9_draw_indexed_range(d,D9_TRIANGLELIST,0,0,4,0,2)==D9_UNSUPPORTED);
    assert(gray.draws==before&&gray.color_draws==0);
    assert(strstr(d9_last_draw_diagnostic(d),"COLOR0"));
    assert(!d9_present(d));

    /* w != 1 is not representable by the current fixed-w shader. */
    RhwVertex bad[4];memcpy(bad,rv,sizeof bad);bad[0].w=0.5f;
    D9Handle bvb=upload_vb(d,bad,sizeof bad);
    assert(!d9_set_stream_source(d,bvb,0,sizeof bad[0]));
    assert(!d9_set_vertex_declaration(d,&rhw));
    assert(!d9_begin_scene(d,0xff000000));
    assert(d9_draw_indexed_range(d,D9_TRIANGLELIST,0,0,4,0,2)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d),"w != 1"));
    assert(!d9_present(d));

    /* No viewport size: refuse with a named reason. */
    D9Device *nv=d9_create_device(&gray_ops,&gray,4u<<20);assert(nv);
    D9Handle nvb=upload_vb(nv,rv,sizeof rv),nib=upload_ib(nv,idx,sizeof idx);
    bind_draw(nv,nvb,nib,sizeof rv[0],&rhw);
    assert(!d9_begin_scene(nv,0xff000000));
    assert(d9_draw_indexed_range(nv,D9_TRIANGLELIST,0,0,4,0,2)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(nv),"viewport"));
    assert(!d9_present(nv));d9_destroy_device(nv);

    /* --- Colour-capable backend gets the decoded tint ------------------- */
    Mock color={0};
    D9Device *cd=d9_create_device(&color_ops,&color,4u<<20);assert(cd);
    assert(!d9_set_viewport_size(cd,400,240));
    D9Handle cb=upload_vb(cd,cv,sizeof cv),cib=upload_ib(cd,idx,sizeof idx);
    bind_draw(cd,cb,cib,sizeof cv[0],&rhwc);
    assert(!d9_begin_scene(cd,0xff000000));
    assert(!d9_draw_indexed_range(cd,D9_TRIANGLELIST,0,0,4,0,2));
    assert(color.color_draws==1&&color.n==6);
    assert(color.cv[0].r==1.0f&&color.cv[0].g==0.0f&&color.cv[0].b==0.0f&&color.cv[0].a==128.0f/255.0f);
    assert(color.cv[1].g==1.0f&&color.cv[2].b==1.0f);
    assert(!d9_present(cd));

    /* --- POSITION+COLOR0+TEXCOORD0 declaration (non-RHW) ---------------- */
    D9Device *xd=d9_create_device(&color_ops,&color,4u<<20);assert(xd);
    XyzColorVertex xv[4]={{0,0,0,0xffffffffu,0,0},{1,0,0,0xffffffffu,1,0},
                          {0,1,0,0xffffffffu,0,1},{1,1,0,0xffffffffu,1,1}};
    D9Handle xvb=upload_vb(xd,xv,sizeof xv),xib=upload_ib(xd,idx,sizeof idx);
    D9VertexElement elems[3]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
                               {0,12,D9_DECL_COLOR,0,D9_USAGE_COLOR,0},
                               {0,16,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0}};
    D9Decl xdcl;assert(!d9_decl_init(&xdcl,elems,3));
    assert(xdcl.position_t==0&&xdcl.color!=UINT32_MAX&&xdcl.required[0]==24);
    bind_draw(xd,xvb,xib,sizeof xv[0],&xdcl);
    assert(!d9_begin_scene(xd,0xff000000));
    unsigned cbefore=color.color_draws;
    assert(!d9_draw_indexed_range(xd,D9_TRIANGLELIST,0,0,4,0,2)&&color.color_draws==cbefore+1);
    assert(!d9_present(xd));d9_destroy_device(xd);

    /* --- Closed texture stage state ------------------------------------ */
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_COLOROP,D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_COLORARG1,D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_COLORARG2,D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_ALPHAOP,D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_ALPHAARG1,D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_ALPHAARG2,D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d,0,D9_TSS_TEXCOORDINDEX,0));
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_COLOROP,D9_TOP_DISABLE));
    D9DrawState st;assert(!d9_get_draw_state(d,&st));
    assert(st.stage_colorop==D9_TOP_MODULATE&&st.stage_colorarg1==D9_TA_TEXTURE&&
           st.stage_colorarg2==D9_TA_DIFFUSE&&st.stage_alphaop==D9_TOP_MODULATE);
    assert(d9_set_texture_stage_state(d,0,D9_TSS_COLOROP,22u)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d),"MODULATE"));
    assert(d9_set_texture_stage_state(d,1,D9_TSS_COLOROP,D9_TOP_MODULATE)==D9_UNSUPPORTED);
    assert(d9_set_texture_stage_state(d,0,7,0)==D9_UNSUPPORTED);
    assert(d9_set_texture_stage_state(d,0,D9_TSS_TEXCOORDINDEX,0x100u)==D9_UNSUPPORTED);

    /* --- DYNAMIC texture usage stays lockable --------------------------- */
    D9Handle dyn;assert(!d9_create_texture_ex(d,8,8,D9_A8R8G8B8,1,D9_POOL_DEFAULT,0x200,0,&dyn));
    unsigned pool,usage,format,levels;size_t guest,gpu;int cpu_only,gpu_only;
    assert(!d9_texture_resource_info(d,dyn,&pool,&usage,&format,&levels,&guest,&gpu,&cpu_only,&gpu_only));
    assert(usage==0x200&&!cpu_only);
    void *px;size_t pitch;assert(!d9_texture_lock_level(d,dyn,0,&px,&pitch)&&pitch==32);
    memset(px,0x5a,8*8*4);assert(!d9_texture_unlock_level(d,dyn,0));

    /* Plain XYZ path still draws through the old D9Vertex stream. */
    D9Vertex pv[3]={{0,0,0,0,0},{1,0,0,1,0},{0,1,0,0,1}};
    D9Handle pvb=upload_vb(d,pv,sizeof pv);
    D9Handle pib=upload_ib(d,(const uint16_t[]){0,1,2},6);
    bind_draw(d,pvb,pib,sizeof pv[0],&plain);
    assert(!d9_begin_scene(d,0xff000000));
    unsigned gbefore=gray.draws;
    assert(!d9_draw_indexed_range(d,D9_TRIANGLELIST,0,0,3,0,1)&&gray.draws==gbefore+1);
    assert(!d9_present(d));
    d9_destroy_device(d);d9_destroy_device(cd);

    puts("gfxmesh core smoke: PASS");
    return 0;
}
