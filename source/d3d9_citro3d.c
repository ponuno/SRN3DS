#include "d3d9_citro3d.h"
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "d9shader_shbin.h"
#include "d9shader_color_shbin.h"
#include "d3d9_pica_citro3d.h"
#include "d3d9_color_tev.h"

#define FRAME_VERTICES 16384u
#define TRANSFER (GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_RAW_COPY(0)| \
 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
struct D9Citro {
    C3D_RenderTarget *target;
    void *active_target; /* NativeTarget* or NULL for the implicit screen target */
    DVLB_s *shader;
    shaderProgram_s program;
    int program_ready,uniform,scene;
    D9Vertex *arena;
    size_t cursor;
    /* Vertex-colour path: separate program, uniform and vertex arena.  The
     * arena holds D9ColorVertex verbatim because its field order
     * {pos3, uv2, rgba4} already matches the colour shader's attribute plan. */
    DVLB_s *shader_color;
    shaderProgram_s program_color;
    int program_color_ready,uniform_color;
    D9ColorVertex *arena_color;
    size_t cursor_color;
};
D9Citro *d9_citro_open(void){
    D9Citro *c=calloc(1,sizeof *c);if(!c)return NULL;
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)){free(c);return NULL;}
    c->target=C3D_RenderTargetCreate(240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    if(!c->target)goto fail;
    C3D_RenderTargetSetOutput(c->target,GFX_TOP,GFX_LEFT,TRANSFER);
    c->shader=DVLB_ParseFile((u32 *)d9shader_shbin,d9shader_shbin_size);if(!c->shader)goto fail;
    if(R_FAILED(shaderProgramInit(&c->program)))goto fail;c->program_ready=1;
    if(R_FAILED(shaderProgramSetVsh(&c->program,&c->shader->DVLE[0])))goto fail;
    c->uniform=shaderInstanceGetUniformLocation(c->program.vertexShader,"clip");if(c->uniform<0)goto fail;
    c->arena=linearAlloc(FRAME_VERTICES*sizeof(D9Vertex));if(!c->arena)goto fail;
    c->shader_color=DVLB_ParseFile((u32 *)d9shader_color_shbin,d9shader_color_shbin_size);if(!c->shader_color)goto fail;
    if(R_FAILED(shaderProgramInit(&c->program_color)))goto fail;c->program_color_ready=1;
    if(R_FAILED(shaderProgramSetVsh(&c->program_color,&c->shader_color->DVLE[0])))goto fail;
    c->uniform_color=shaderInstanceGetUniformLocation(c->program_color.vertexShader,"clip");if(c->uniform_color<0)goto fail;
    c->arena_color=linearAlloc(FRAME_VERTICES*sizeof(D9ColorVertex));if(!c->arena_color)goto fail;
    return c;
fail:d9_citro_close(c);return NULL;
}
void d9_citro_close(D9Citro *c){
    if(!c)return;if(c->scene)C3D_FrameEnd(0);C3D_FrameSync();
    if(c->arena)linearFree(c->arena);
    if(c->arena_color)linearFree(c->arena_color);
    if(c->program_color_ready)shaderProgramFree(&c->program_color);if(c->shader_color)DVLB_Free(c->shader_color);
    if(c->program_ready)shaderProgramFree(&c->program);if(c->shader)DVLB_Free(c->shader);
    if(c->target)C3D_RenderTargetDelete(c->target);C3D_Fini();free(c);
}
typedef struct {C3D_Tex tex;unsigned levels;} NativeTex;
typedef struct {C3D_Tex tex;C3D_RenderTarget *target;unsigned w,h,format;} NativeTarget;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **handle){
    (void)ctx;NativeTex *n=calloc(1,sizeof *n);if(!n||!levels){free(n);return -1;}
    C3D_TexInitParams params={(u16)w,(u16)h,(u8)(levels-1),GPU_RGBA8,GPU_TEX_2D,false};
    if(!C3D_TexInitWithParams(&n->tex,NULL,params)){free(n);return -1;}
    n->levels=levels;C3D_TexSetFilter(&n->tex,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&n->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);*handle=n;return 0;
}
static int tex_upload(void *ctx,void *handle,unsigned level,const void *data,size_t bytes){
    D9Citro *c=ctx;NativeTex *n=handle;C3D_Tex *t=&n->tex;if(c->scene||level>=n->levels||bytes!=C3D_TexCalcLevelSize(t->size,(int)level))return -1;
    C3D_FrameSync();t->maxLevel=(u8)(n->levels-1);t->minLevel=0;C3D_TexLoadImage(t,data,GPU_TEXFACE_2D,(int)level);C3D_TexFlush(t);return 0;
}
static void tex_delete(void *ctx,void *handle){(void)ctx;NativeTex *n=handle;C3D_FrameSync();C3D_TexDelete(&n->tex);free(n);}
static int tex_create_encoded(void *ctx,unsigned w,unsigned h,unsigned levels,unsigned format,void **handle){
    (void)ctx;if(!handle||(format!=D9_TEX_ETC1&&format!=D9_TEX_ETC1A4)||!levels)return -1;
    NativeTex *n=calloc(1,sizeof *n);if(!n)return -1;
    C3D_TexInitParams params={(u16)w,(u16)h,(u8)(levels-1),
        format==D9_TEX_ETC1A4?GPU_ETC1A4:GPU_ETC1,GPU_TEX_2D,false};
    if(!C3D_TexInitWithParams(&n->tex,NULL,params)){free(n);return -1;}
    n->levels=levels;C3D_TexSetFilter(&n->tex,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&n->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    *handle=n;return 0;
}
static int tex_upload_encoded(void *ctx,void *handle,unsigned level,const void *data,size_t bytes){
    D9Citro *c=ctx;NativeTex *n=handle;C3D_Tex *t=&n->tex;
    if(!c||!n||c->scene||level>=n->levels||!data||bytes!=C3D_TexCalcLevelSize(t->size,(int)level))return -1;
    C3D_FrameSync();t->maxLevel=(u8)(n->levels-1);t->minLevel=0;
    C3D_TexLoadImage(t,data,GPU_TEXFACE_2D,(int)level);C3D_TexFlush(t);return 0;
}
static void prepare_draw(D9Citro *c){
    C3D_BindProgram(&c->program);
    C3D_AttrInfo *attr=C3D_GetAttrInfo();AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr,0,GPU_FLOAT,3);AttrInfo_AddLoader(attr,1,GPU_FLOAT,2);
    C3D_BufInfo *buf=C3D_GetBufInfo();BufInfo_Init(buf);BufInfo_Add(buf,c->arena,sizeof(D9Vertex),2,0x10);
}
static int begin(void *ctx,uint32_t argb){
    D9Citro *c=ctx;if(c->scene||!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))return -1;
    c->scene=1;c->cursor=0;c->cursor_color=0;c->active_target=NULL;
    uint32_t rgba=(argb<<8)|(argb>>24);
    C3D_RenderTargetClear(c->target,C3D_CLEAR_ALL,rgba,0);
    if(!C3D_FrameDrawOn(c->target)){C3D_FrameEnd(0);c->scene=0;return -1;}
    prepare_draw(c);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);C3D_CullFace(GPU_CULL_NONE);
    d9_pica_citro3d_restore_tev();
    return 0;
}
static GPU_TESTFUNC compare(unsigned n,int reverse){
    static const GPU_TESTFUNC normal[]={GPU_ALWAYS,GPU_NEVER,GPU_LESS,GPU_EQUAL,GPU_LEQUAL,GPU_GREATER,GPU_NOTEQUAL,GPU_GEQUAL,GPU_ALWAYS};
    static const GPU_TESTFUNC depth[]={GPU_ALWAYS,GPU_NEVER,GPU_GREATER,GPU_EQUAL,GPU_GEQUAL,GPU_LESS,GPU_NOTEQUAL,GPU_LEQUAL,GPU_ALWAYS};
    return (reverse?depth:normal)[n];}
static GPU_BLENDFACTOR factor(unsigned n){static const GPU_BLENDFACTOR f[]={GPU_ZERO,GPU_ZERO,GPU_ONE,GPU_SRC_COLOR,GPU_ONE_MINUS_SRC_COLOR,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
    GPU_DST_ALPHA,GPU_ONE_MINUS_DST_ALPHA,GPU_DST_COLOR,GPU_ONE_MINUS_DST_COLOR,GPU_SRC_ALPHA_SATURATE};return f[n];}
static GPU_TEXTURE_WRAP_PARAM wrap(unsigned n){return n==1?GPU_REPEAT:n==2?GPU_MIRRORED_REPEAT:GPU_CLAMP_TO_EDGE;}
static void configure_native_texture(void *texture,const D9DrawState *state){
    NativeTex *t=texture;D9CitroTextureConfig config=d9_citro_texture_config(t->levels,state);
    t->tex.minLevel=(u8)config.min_level;t->tex.maxLevel=(u8)config.max_level;
    C3D_TexSetFilter(&t->tex,config.mag_filter==2?GPU_LINEAR:GPU_NEAREST,config.min_filter==2?GPU_LINEAR:GPU_NEAREST);
    /* NONE disables interpolation between mip levels, not mip selection itself. */
    C3D_TexSetFilterMipmap(&t->tex,config.mip_filter==2?GPU_LINEAR:GPU_NEAREST);C3D_TexSetWrap(&t->tex,wrap(state->address_u),wrap(state->address_v));
}
/* The LCD framebuffer is stored rotated 240x400, so every clip matrix the core
 * builds carries a screen rotation.  An offscreen colour target is a normal D3D
 * surface: LockRect must return a D3D top-left image and Present samples it with
 * direct UVs.  Post-multiplying the clip output by this permutation turns the
 * screen conversion into the D3D/texture conversion:
 *     clip'_x = -clip_y,  clip'_y = -clip_x
 * (z/w untouched).  This is applied only while an explicit or retained target is
 * bound; the screen path is unchanged. */
static void orient_clip(D9Citro *c,D9Matrix *m){
    if(!c->active_target)return;
    float r0[4],r1[4];
    for(unsigned i=0;i<4;++i){r0[i]=-m->m[1][i];r1[i]=-m->m[0][i];}
    for(unsigned i=0;i<4;++i){m->m[0][i]=r0[i];m->m[1][i]=r1[i];}
}
static void apply_state(const D9DrawState *s,int has_depth){
    /* Every colour target (screen and retained offscreen) carries the single
     * PICA DEPTH24_STENCIL8 attachment, so depth test/write state always has a
     * real attachment behind it. */
    GPU_WRITEMASK mask=(GPU_WRITEMASK)(s->color_write|((has_depth&&s->z_enable&&s->z_write)?GPU_WRITE_DEPTH:0));
    C3D_DepthTest(has_depth&&s->z_enable,compare(s->z_func,1),mask);
    C3D_CullFace(s->cull==2?GPU_CULL_BACK_CCW:s->cull==3?GPU_CULL_FRONT_CCW:GPU_CULL_NONE);
    C3D_AlphaTest(s->alpha_test,compare(s->alpha_func,0),s->alpha_ref);
    GPU_BLENDFACTOR src=s->blend?factor(s->src_blend):GPU_ONE,dst=s->blend?factor(s->dst_blend):GPU_ZERO;
    GPU_BLENDEQUATION eq=s->blend?(GPU_BLENDEQUATION)(s->blend_op-1):GPU_BLEND_ADD;
    C3D_AlphaBlend(eq,eq,src,dst,src,dst);
}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    D9Citro *c=ctx;if(!c->scene||n>FRAME_VERTICES-c->cursor)return -1;
    memcpy(c->arena+c->cursor,v,n*sizeof *v);GSPGPU_FlushDataCache(c->arena+c->cursor,n*sizeof *v);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform,&m);
    apply_state(state,1);
    if(texture)configure_native_texture(texture,state);
    if(state->pixel_shader){
        NativeTex *native=texture;
        if(d9_pica_citro3d_apply_tev(state->pixel_shader,native?&native->tex:NULL,NULL,0))return -1;
    }else{
        d9_pica_citro3d_restore_tev();
        C3D_TexEnv *env=C3D_GetTexEnv(0);if(!env)return -1;
        if(texture){NativeTex *native=texture;C3D_TexBind(0,&native->tex);C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,0,0);}
        else C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    }
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor,(int)n);
    d9_pica_citro3d_restore_tev();
    c->cursor+=n;return 0;
}
/* --- per-vertex colour path -------------------------------------------
 * D3D9 stage-0 state is translated by d9_color_tev_stage0() (host-tested)
 * into one TEV stage whose inputs are texture0 and primary (vertex) colour.
 * No lighting is enabled by this path, so PICA primary colour is exactly the
 * shader's outclr = incolor.  Anything the stage-0 subset cannot express is
 * rejected before a draw is issued. */
static GPU_TEVSRC color_tev_source(unsigned unit,unsigned src){
    switch(src){
    case D9_CTEV_SRC_TEXTURE:return unit?GPU_TEXTURE1:GPU_TEXTURE0;
    case D9_CTEV_SRC_CURRENT:return unit?GPU_PREVIOUS:GPU_PRIMARY_COLOR;
    default:return GPU_PRIMARY_COLOR;
    }
}
static void apply_color_tev(unsigned unit,const D9ColorTevStage *plan){
    C3D_TexEnv *env=C3D_GetTexEnv(unit);if(!env)return;
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_RGB,color_tev_source(unit,plan->color_src[0]),
                  color_tev_source(unit,plan->color_src[1]),color_tev_source(unit,plan->color_src[2]));
    C3D_TexEnvSrc(env,C3D_Alpha,color_tev_source(unit,plan->alpha_src[0]),
                  color_tev_source(unit,plan->alpha_src[1]),color_tev_source(unit,plan->alpha_src[2]));
    C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvOpAlpha(env,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env,C3D_RGB,plan->color_func==D9_CTEV_MODULATE?GPU_MODULATE:GPU_REPLACE);
    C3D_TexEnvFunc(env,C3D_Alpha,plan->alpha_func==D9_CTEV_MODULATE?GPU_MODULATE:GPU_REPLACE);
    C3D_TexEnvScale(env,C3D_Both,GPU_TEVSCALE_1);
    C3D_DirtyTexEnv(env);
}
static void prepare_draw_color(D9Citro *c){
    C3D_BindProgram(&c->program_color);
    C3D_AttrInfo *attr=C3D_GetAttrInfo();AttrInfo_Init(attr);
    /* D9ColorVertex is {pos3, uv2, rgba4}; the colour shader reads
     * v0=position, v1=texcoord, v2=colour in that order. */
    AttrInfo_AddLoader(attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(attr,1,GPU_FLOAT,2);
    AttrInfo_AddLoader(attr,2,GPU_FLOAT,4);
    C3D_BufInfo *buf=C3D_GetBufInfo();BufInfo_Init(buf);
    BufInfo_Add(buf,c->arena_color,sizeof(D9ColorVertex),3,0x210);
}
static int draw_color_stages(void *ctx,const D9ColorVertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state,void *texture1,const D9DrawState *second){
    D9Citro *c=ctx;D9ColorTevStage plan,second_plan;const char *why=NULL;
    if(!c||!c->scene||!c->program_color_ready||!v||!clip||!state||!n||n%3)return -1;
    if(n>FRAME_VERTICES-c->cursor_color)return -1;
    /* The colour path is fixed-function stage-0 state only; a ps_3_0 program
     * would own the TEV stages and the two would silently disagree. */
    if(state->pixel_shader)return -1;
    if(d9_color_tev_stage0(state,texture!=NULL,&plan,&why))return -1;
    if(second&&d9_color_tev_stage0(second,texture1!=NULL,&second_plan,&why))return -1;
    memcpy(c->arena_color+c->cursor_color,v,n*sizeof *v);
    GSPGPU_FlushDataCache(c->arena_color+c->cursor_color,n*sizeof *v);
    prepare_draw_color(c);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform_color,&m);
    apply_state(state,1);
    if(texture)configure_native_texture(texture,state);
    d9_pica_citro3d_restore_tev();
    if(texture){NativeTex *native=texture;C3D_TexBind(0,&native->tex);}
    apply_color_tev(0,&plan);
    if(second){
        NativeTex *native=texture1;
        configure_native_texture(native,second);
        C3D_TexBind(1,&native->tex);
        apply_color_tev(1,&second_plan);
    }
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor_color,(int)n);
    d9_pica_citro3d_restore_tev();
    /* citro3d 2023-10-28 has no public texture unbind: C3D_TexBind(1,NULL)
     * dereferences tex->param through a NULL pointer (fault at offset 0x0C).
     * restore_tev() has already removed every TEXTURE1 source, so the stale
     * unit-1 binding is unreferenced and deterministic.  Keep it instead of
     * crashing.  (Unit 0 is left bound the same way.) */
    c->cursor_color+=n;
    /* Leave the shared program/attribute state as begin() left it so a
     * following non-colour draw is unchanged. */
    prepare_draw(c);
    return 0;
}
static int draw_colored(void *ctx,const D9ColorVertex *vertices,size_t count,
                        const D9Matrix *clip,void *texture,const D9DrawState *state){
    return draw_color_stages(ctx,vertices,count,clip,texture,state,NULL,NULL);
}
/* Menu TEV plans: the colour vertex program carries color0, the plan can bind
 * two texture units, and external stage constants come from the c0..c8 bank.
 * c8 is the internal ambient+emissive fold, not a D3D9 cap. */
static int draw_menu(void *ctx,const D9ColorVertex *vertices,size_t count,
                     const D9Matrix *clip,void *texture0,void *texture1,
                     const D9DrawState *state,const float (*constants)[4]){
    D9Citro *c=ctx;
    if(!c||!c->scene||!c->program_color_ready||!vertices||!clip||!state||
       !state->pixel_shader||!count||count%3)return -1;
    if(count>FRAME_VERTICES-c->cursor_color)return -1;
    memcpy(c->arena_color+c->cursor_color,vertices,count*sizeof *vertices);
    GSPGPU_FlushDataCache(c->arena_color+c->cursor_color,count*sizeof *vertices);
    prepare_draw_color(c);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform_color,&m);
    apply_state(state,1);
    if(texture0)configure_native_texture(texture0,state);
    if(texture1)configure_native_texture(texture1,state);
    d9_pica_citro3d_restore_tev();
    NativeTex *native0=texture0,*native1=texture1;
    if(d9_pica_citro3d_apply_tev_ex(state->pixel_shader,
                                    native0?&native0->tex:NULL,
                                    native1?&native1->tex:NULL,
                                    constants,9,NULL,0)){
        d9_pica_citro3d_restore_tev();
        prepare_draw(c);
        return -1;
    }
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor_color,(int)count);
    d9_pica_citro3d_restore_tev();
    c->cursor_color+=count;
    prepare_draw(c);
    return 0;
}
static void end(void *ctx){D9Citro *c=ctx;if(c->scene){C3D_FrameEnd(0);c->scene=0;}c->active_target=NULL;}
static int target_create(void *ctx,unsigned w,unsigned h,unsigned format,void **native){
    (void)ctx;if(!native||w<8||h<8||w>1024||h>1024||(w&(w-1))||(h&(h-1))||
       (format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8))return -1;
    NativeTarget *t=calloc(1,sizeof *t);if(!t)return -1;
    C3D_TexInitParams params={(u16)w,(u16)h,0,GPU_RGBA8,GPU_TEX_2D,true};
    if(!C3D_TexInitWithParams(&t->tex,NULL,params)){
        fprintf(stderr,"[d3d9-citro] CreateRenderTarget: %ux%u VRAM texture allocation failed\n",w,h);
        free(t);return -1;
    }
    C3D_TexSetFilter(&t->tex,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&t->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    /* The retained back buffer and every explicit LDR target need a real depth
     * attachment: SetDepthStencilSurface/Clear(Z) and depth-tested draws must
     * reach hardware, not a bookkeeping flag. */
    t->target=C3D_RenderTargetCreateFromTex(&t->tex,GPU_TEXFACE_2D,0,GPU_RB_DEPTH24_STENCIL8);
    if(!t->target){
        fprintf(stderr,"[d3d9-citro] CreateRenderTarget: %ux%u render-target allocation failed\n",w,h);
        C3D_TexDelete(&t->tex);free(t);return -1;
    }
    /* VRAM starts with arbitrary bytes.  PICA depth 0 is "far" and the draws
     * use GPU_GEQUAL, so a prime clear avoids depth-testing a fresh target
     * against garbage.  This is a private frame: the target is not linked to
     * either LCD, so FrameEnd performs no display transfer. */
    if(C3D_FrameBegin(C3D_FRAME_SYNCDRAW)){
        if(C3D_FrameDrawOn(t->target))
            C3D_RenderTargetClear(t->target,C3D_CLEAR_DEPTH|C3D_CLEAR_COLOR,0,0);
        C3D_FrameEnd(0);
    }
    t->w=w;t->h=h;t->format=format;*native=t;return 0;
}
static int target_bind(void *ctx,void *native){
    D9Citro *c=ctx;if(!c||!c->scene)return -1;
    C3D_RenderTarget *target=native?((NativeTarget *)native)->target:c->target;
    if(!target||!C3D_FrameDrawOn(target))return -1;
    c->active_target=native;return 0;
}
static int target_clear(void *ctx,void *native,uint32_t argb){
    D9Citro *c=ctx;NativeTarget *t=native;if(!c||!c->scene||!t)return -1;
    C3D_RenderTargetClear(t->target,C3D_CLEAR_COLOR,(argb<<8)|(argb>>24),0);return 0;
}
static int target_read(void *ctx,void *native,void *dst,size_t pitch,
                       unsigned x,unsigned y,unsigned w,unsigned h){
    D9Citro *c=ctx;NativeTarget *t=native;if(!c||!t||!dst||c->scene||x>t->w||y>t->h||w>t->w-x||h>t->h-y)return -1;
    size_t bytes=(size_t)t->w*t->h*4;if(bytes!=t->tex.size)return -1;
    C3D_FrameSync();uint8_t *tmp=linearAlloc(bytes);if(!tmp)return -1;
    C3D_SyncTextureCopy((u32 *)t->tex.data,0,(u32 *)tmp,0,(u32)bytes,8);
    GSPGPU_InvalidateDataCache(tmp,bytes);
    int rc=d9_unswizzle_rgba8(dst,pitch,tmp,t->w,t->h,t->format,x,y,w,h);
    linearFree(tmp);return rc;
}
static void target_delete(void *ctx,void *native){
    D9Citro *c=ctx;NativeTarget *t=native;if(!t)return;
    if(c->active_target==native)c->active_target=NULL;
    C3D_FrameSync();if(t->target)C3D_RenderTargetDelete(t->target);
    C3D_TexDelete(&t->tex);free(t);
}
/* Whole-surface write-back for the lockable back buffer.  The source is
 * top-first BGRA; PICA RGBA8 storage is tiled with a bottom-left origin, so the
 * existing swizzle produces the exact texel layout and C3D_TexLoadImage replaces
 * level 0.  This is a real upload, not a shadow copy. */
static int target_write(void *ctx,void *native,const void *bgra,size_t pitch,
                        unsigned w,unsigned h){
    D9Citro *c=ctx;NativeTarget *t=native;
    if(!c||!t||!bgra||c->scene||w!=t->w||h!=t->h||pitch<(size_t)w*4)return -1;
    size_t bytes=(size_t)t->w*t->h*4;if(bytes!=t->tex.size)return -1;
    C3D_FrameSync();
    uint8_t *tmp=linearAlloc(bytes);if(!tmp)return -1;
    if(d9_swizzle_rgba8(tmp,bytes,bgra,pitch,w,h,t->format)){linearFree(tmp);return -1;}
    t->tex.maxLevel=0;t->tex.minLevel=0;
    C3D_TexLoadImage(&t->tex,tmp,GPU_TEXFACE_2D,0);
    C3D_TexFlush(&t->tex);
    linearFree(tmp);return 0;
}
/* Clear the depth attachment of the currently bound colour target.  PICA stores
 * depth and stencil in one DEPTH24_STENCIL8 buffer, so a depth clear writes the
 * packed word (and, with it, the stencil byte). */
static int depth_clear(void *ctx,unsigned flags,uint32_t depth_stencil){
    D9Citro *c=ctx;
    if(!c||!c->scene||!(flags&6u)||(flags&~7u))return -1;
    C3D_RenderTarget *rt=c->active_target?((NativeTarget *)c->active_target)->target:c->target;
    if(!rt)return -1;
    C3D_RenderTargetClear(rt,C3D_CLEAR_DEPTH,0,depth_stencil);
    return 0;
}
/* Logical viewport for the current offscreen target.  C3D_FrameDrawOn sets the
 * viewport to the target storage size, but the retained back buffer is 512x256
 * while the D3D surface is 400x240; rasterising into the padded storage would
 * stretch the frame and make the present blit sample the wrong region. */
static int target_viewport(void *ctx,unsigned w,unsigned h){
    D9Citro *c=ctx;
    if(!c||!c->scene||!w||!h||w>4096||h>4096)return -1;
    C3D_SetViewport(0,0,w,h);
    return 0;
}
/* StretchRect-class blit.  Only explicit render-target textures can be
 * sampled: the implicit 240x400 screen framebuffer is not a PICA texture, so a
 * back-buffer source is rejected explicitly (D9_BLIT_BACK_BUFFER_SRC) instead
 * of silently showing garbage.  A back-buffer destination draws into the
 * normal screen target and therefore needs the guest's BeginScene/EndScene.
 * Explicit-to-explicit copies can open a short private frame when no guest
 * scene is active; the destination is not linked to either LCD, so FrameEnd
 * performs no visible transfer. */
static int target_blit(void *ctx,const D9BlitRegion *dst,const D9BlitRegion *src,unsigned filter){
    D9Citro *c=ctx;
    if(!c||!dst||!src||!dst->width||!dst->height||!src->width||!src->height||filter>D9_FILTER_LINEAR)
        return D9_BLIT_ARGUMENT;
    if(!src->surface.native)return D9_BLIT_BACK_BUFFER_SRC;
    if(dst->surface.format!=src->surface.format)return D9_BLIT_FORMAT;
    NativeTarget *native=dst->surface.native;
    C3D_RenderTarget *rt=native?native->target:c->target;
    C3D_Tex *stex=(C3D_Tex *)src->surface.native;
    if(!rt||!stex)return D9_BLIT_BACKEND;
    unsigned sw=src->surface.storage_width?src->surface.storage_width:src->surface.width;
    unsigned shgt=src->surface.storage_height?src->surface.storage_height:src->surface.height;
    unsigned dw=dst->surface.storage_width?dst->surface.storage_width:dst->surface.width;
    unsigned dh=dst->surface.storage_height?dst->surface.storage_height:dst->surface.height;
    if(stex->width!=sw||stex->height!=shgt)return D9_BLIT_UNSUPPORTED;
    if(dst->surface.native&&(native->w!=dw||native->h!=dh))return D9_BLIT_UNSUPPORTED;
    if(c->cursor+6>FRAME_VERTICES)return D9_BLIT_BACKEND;
    int own_frame=!c->scene;
    if(own_frame){
        if(!dst->surface.native)return D9_BLIT_NO_SCENE;
        if(!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))return D9_BLIT_BACKEND;
    }
    if(!C3D_FrameDrawOn(rt)){if(own_frame)C3D_FrameEnd(0);return D9_BLIT_BACKEND;}
    prepare_draw(c);
    float w=(float)dst->surface.width,h=(float)dst->surface.height;
    /* Same D3D-top-left -> rotated PICA clip mapping as d9_pica_transform()
     * uses for a screen-aligned orthographic quad. */
    C3D_Mtx m;
    m.r[0]=FVec4_New(0.0f,-2.0f/h,0.0f,1.0f);
    m.r[1]=FVec4_New(-2.0f/w,0.0f,0.0f,1.0f);
    m.r[2]=FVec4_New(0.0f,0.0f,0.0f,0.0f);
    m.r[3]=FVec4_New(0.0f,0.0f,0.0f,1.0f);
    if(dst->surface.native){
        /* Offscreen destination: the same orient_clip() permutation, written
         * out explicitly (row0 = -screen row1, row1 = -screen row0), so a
         * texture-to-texture copy is not rotated. */
        m.r[0]=FVec4_New(2.0f/w,0.0f,0.0f,-1.0f);
        m.r[1]=FVec4_New(0.0f,2.0f/h,0.0f,-1.0f);
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform,&m);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    d9_pica_citro3d_restore_tev();
    u32 saved_param=stex->param;
    GPU_TEXTURE_FILTER_PARAM tex_filter=filter==D9_FILTER_LINEAR?GPU_LINEAR:GPU_NEAREST;
    C3D_TexSetFilter(stex,tex_filter,tex_filter);
    C3D_TexSetWrap(stex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0,stex);
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    if(!env){stex->param=saved_param;if(own_frame)C3D_FrameEnd(0);return D9_BLIT_BACKEND;}
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,0,0);
    C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    float x0=(float)dst->x,y0=(float)dst->y;
    float x1=(float)(dst->x+dst->width),y1=(float)(dst->y+dst->height);
    float u0=(float)src->x/(float)sw;
    float v0=(float)src->y/(float)shgt;
    float u1=(float)(src->x+src->width)/(float)sw;
    float v1=(float)(src->y+src->height)/(float)shgt;
    D9Vertex *v=c->arena+c->cursor;
    v[0]=(D9Vertex){x0,y0,0.0f,u0,v0};v[1]=(D9Vertex){x1,y0,0.0f,u1,v0};v[2]=(D9Vertex){x1,y1,0.0f,u1,v1};
    v[3]=v[0];v[4]=v[2];v[5]=(D9Vertex){x0,y1,0.0f,u0,v1};
    GSPGPU_FlushDataCache(v,6*sizeof *v);
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor,6);
    c->cursor+=6;
    stex->param=saved_param;
    d9_pica_citro3d_restore_tev();
    if(own_frame){C3D_FrameEnd(0);return D9_BLIT_OK;}
    C3D_RenderTarget *prev=c->active_target?((NativeTarget *)c->active_target)->target:c->target;
    if(prev&&prev!=rt&&!C3D_FrameDrawOn(prev))return D9_BLIT_BACKEND;
    return D9_BLIT_OK;
}
const D9Backend *d9_citro_backend(void){static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end,NULL,tex_create_encoded,tex_upload_encoded,
    target_create,target_delete,target_bind,target_clear,target_read,target_blit,draw_colored,draw_color_stages,
    target_write,depth_clear,target_viewport,draw_menu};return &ops;}
