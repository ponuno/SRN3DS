#include "d3d9_citro3d.h"
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "d9shader_shbin.h"
#include "d9shader_color_shbin.h"
#include "d9shader_cube_shbin.h"
#include "d3d9_pica_citro3d.h"
#include "d3d9_color_tev.h"
#include "d3d9_cube_coords.h"

#define FRAME_VERTICES 16384u
#define FRAME_INDICES 262144u
/* Indexed menu batches may use the full 16-bit vertex range.  Keep the older
 * 16k arena policy for ordinary/cube colour draws, but give the compact menu
 * path its own 65,536-entry allocation instead of rejecting the real 57,484
 * vertex StartMenu batch after core accepted it. */
#define FRAME_MENU_VERTICES 65536u
/* Cube draw vertex: the PICA cube shader reads a 3-component direction and
 * writes texcoord0.xy + texcoord0w, so the attribute stride is 40 bytes. */
typedef struct {float x,y,z,dx,dy,dz,r,g,b,a;} D9CubeVertex;
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
    /* Cube-map path: the core hands a per-vertex face, so the adapter converts
     * {pos3, uv2, rgba4} into {pos3, direction3, rgba4} on the CPU. */
    DVLB_s *shader_cube;
    shaderProgram_s program_cube;
    int program_cube_ready,uniform_cube;
    D9CubeVertex *arena_cube;
    size_t cursor_cube;
    /* Indexed menu draws: 16-bit indices are copied into this per-frame linear
     * arena and passed to C3D_DrawElements.  Reset in begin(), never freed
     * before the GPU consumes them. */
    uint16_t *arena_index;
    size_t cursor_index;
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
    c->arena_color=linearAlloc(FRAME_MENU_VERTICES*sizeof(D9ColorVertex));if(!c->arena_color)goto fail;
    c->shader_cube=DVLB_ParseFile((u32 *)d9shader_cube_shbin,d9shader_cube_shbin_size);if(!c->shader_cube)goto fail;
    if(R_FAILED(shaderProgramInit(&c->program_cube)))goto fail;c->program_cube_ready=1;
    if(R_FAILED(shaderProgramSetVsh(&c->program_cube,&c->shader_cube->DVLE[0])))goto fail;
    c->uniform_cube=shaderInstanceGetUniformLocation(c->program_cube.vertexShader,"clip");if(c->uniform_cube<0)goto fail;
    c->arena_cube=linearAlloc(FRAME_VERTICES*sizeof(D9CubeVertex));if(!c->arena_cube)goto fail;
    c->arena_index=linearAlloc(FRAME_INDICES*sizeof(uint16_t));if(!c->arena_index)goto fail;
    return c;
fail:d9_citro_close(c);return NULL;
}
void d9_citro_close(D9Citro *c){
    if(!c)return;if(c->scene)C3D_FrameEnd(0);C3D_FrameSync();
    if(c->arena)linearFree(c->arena);
    if(c->arena_color)linearFree(c->arena_color);
    if(c->arena_cube)linearFree(c->arena_cube);
    if(c->arena_index)linearFree(c->arena_index);
    if(c->program_cube_ready)shaderProgramFree(&c->program_cube);if(c->shader_cube)DVLB_Free(c->shader_cube);
    if(c->program_color_ready)shaderProgramFree(&c->program_color);if(c->shader_color)DVLB_Free(c->shader_color);
    if(c->program_ready)shaderProgramFree(&c->program);if(c->shader)DVLB_Free(c->shader);
    if(c->target)C3D_RenderTargetDelete(c->target);C3D_Fini();free(c);
}
typedef struct {C3D_Tex tex;unsigned levels,format;} NativeTex;
typedef struct {C3D_Tex tex;C3D_TexCube cube;unsigned edge,levels,format;} NativeCube;
typedef struct {
    C3D_RenderTarget *target;
    C3D_Tex *blit_texture; /* 2D colour storage; cube-face views cannot be blit sources. */
    unsigned w,h,format;
    int has_depth;     /* target owns a DEPTH24_STENCIL8 attachment */
    int is_cube_face;  /* native is a NativeCubeFace, not a NativeTarget */
} NativeTargetHead;
typedef struct {NativeTargetHead head;C3D_Tex tex;NativeTex sample;} NativeTarget;
typedef struct {NativeTargetHead head;NativeCube *cube;unsigned face,level;} NativeCubeFace;
static int tex_create_format(void *ctx,unsigned w,unsigned h,unsigned levels,
                             unsigned format,void **handle){
    (void)ctx;GPU_TEXCOLOR pfmt;
    if(format==D9_A8)pfmt=GPU_A8;
    else if(format==D9_A8R8G8B8)pfmt=GPU_RGBA8;
    else if(format==D9_X8R8G8B8)pfmt=GPU_RGB8;
    else return -2;
    NativeTex *n=calloc(1,sizeof *n);if(!n||!levels){free(n);return -1;}
    C3D_TexInitParams params={(u16)w,(u16)h,(u8)(levels-1),pfmt,GPU_TEX_2D,false};
    if(!C3D_TexInitWithParams(&n->tex,NULL,params)){free(n);return -1;}
    n->levels=levels;n->format=format;
    C3D_TexSetFilter(&n->tex,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&n->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    *handle=n;return 0;
}
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **handle){
    return tex_create_format(ctx,w,h,levels,D9_A8R8G8B8,handle);
}
static int tex_upload_format(void *ctx,void *handle,unsigned level,const void *data,size_t bytes){
    D9Citro *c=ctx;NativeTex *n=handle;C3D_Tex *t=&n->tex;
    if(c->scene||level>=n->levels||bytes!=C3D_TexCalcLevelSize(t->size,(int)level))return -1;
    C3D_FrameSync();t->maxLevel=(u8)(n->levels-1);t->minLevel=0;C3D_TexLoadImage(t,data,GPU_TEXFACE_2D,(int)level);C3D_TexFlush(t);return 0;
}
static int tex_upload(void *ctx,void *handle,unsigned level,const void *data,size_t bytes){
    return tex_upload_format(ctx,handle,level,data,bytes);
}
static void tex_delete(void *ctx,void *handle){(void)ctx;NativeTex *n=handle;C3D_FrameSync();C3D_TexDelete(&n->tex);free(n);}
/* PICA cube textures.  C3D_TexInitWithParams allocates the six face images;
 * C3D_TexDelete frees them (the embedded C3D_TexCube struct is ours to free).
 * Formats are limited to the two the core can upload without conversion on the
 * GPU: RGBA8 and RGB8 (X8 uploads are expanded from the core's 4-byte store). */
static int cube_create(void *ctx,unsigned edge,unsigned levels,unsigned format,void **handle){
    (void)ctx;if(!handle||!levels||edge<8||edge>1024||(edge&(edge-1)))return -2;
    GPU_TEXCOLOR pfmt;
    if(format==D9_A8R8G8B8)pfmt=GPU_RGBA8;
    else if(format==D9_X8R8G8B8)pfmt=GPU_RGB8;
    else return -2;
    NativeCube *n=calloc(1,sizeof *n);if(!n)return -3;
    C3D_TexInitParams params={(u16)edge,(u16)edge,(u8)(levels-1),pfmt,
                              GPU_TEX_CUBE_MAP,false};
    if(!C3D_TexInitWithParams(&n->tex,&n->cube,params)){
        if(n->tex.cube)C3D_TexDelete(&n->tex);
        free(n);return -3;
    }
    n->edge=edge;n->levels=levels;n->format=format;
    C3D_TexSetFilter(&n->tex,GPU_LINEAR,GPU_LINEAR);
    C3D_TexSetWrap(&n->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    *handle=n;return 0;
}
static int cube_upload(void *ctx,void *handle,unsigned face,unsigned level,
                       const void *data,size_t bytes){
    D9Citro *c=ctx;NativeCube *n=handle;
    if(!c||!n||c->scene||face>=6u||level>=n->levels||!data)return -1;
    unsigned dim=n->edge>>level;if(!dim)dim=1;
    C3D_Tex *t=&n->tex;
    u32 want=C3D_TexCalcLevelSize(t->size,(int)level);
    size_t src=(size_t)dim*dim*4u;
    C3D_FrameSync();t->maxLevel=(u8)(n->levels-1);t->minLevel=0;
    if(n->format==D9_X8R8G8B8){
        if(want!=(u32)((size_t)dim*dim*3u)||bytes!=src)return -1;
        uint8_t *rgb=malloc(want);if(!rgb)return -1;
        const uint8_t *in=data;
        for(size_t i=0;i<(size_t)dim*dim;++i){
            rgb[i*3]=in[i*4];rgb[i*3+1]=in[i*4+1];rgb[i*3+2]=in[i*4+2];
        }
        C3D_TexLoadImage(t,rgb,(GPU_TEXFACE)face,(int)level);
        free(rgb);
    }else{
        if(bytes!=want)return -1;
        C3D_TexLoadImage(t,data,(GPU_TEXFACE)face,(int)level);
    }
    C3D_TexFlush(t);return 0;
}
static int cube_delete(void *ctx,void *handle){
    (void)ctx;NativeCube *n=handle;if(!n)return 0;
    C3D_FrameSync();C3D_TexDelete(&n->tex);free(n);return 0;
}
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
    c->scene=1;c->cursor=0;c->cursor_color=0;c->cursor_cube=0;c->cursor_index=0;c->active_target=NULL;
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
    /* D3DSAMP_MIPMAPLODBIAS: exact hardware lod bias, clamped to [-16,16). */
    C3D_TexSetLodBias(&t->tex,config.lod_bias);
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
static int apply_shader_alpha_test(const D9DrawState *s) {
    if (!s->pixel_shader || !s->pixel_shader->requires_alpha_test) return 0;
    if (s->alpha_test && (s->alpha_func != 5 || s->alpha_ref != 0)) {
        fprintf(stderr, "[d3d9-citro] texkill alpha test conflicts with draw-state alpha test\n");
        return -1;
    }
    C3D_AlphaTest(true, GPU_GREATER, 0);
    return 0;
}

static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    D9Citro *c=ctx;if(!c->scene||n>FRAME_VERTICES-c->cursor)return -1;
    memcpy(c->arena+c->cursor,v,n*sizeof *v);GSPGPU_FlushDataCache(c->arena+c->cursor,n*sizeof *v);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform,&m);
    apply_state(state,1);
    if(apply_shader_alpha_test(state))return -1;
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
    if(apply_shader_alpha_test(state))return -1;
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
/* --- cube-map path -------------------------------------------------------
 * The core selects a D3D face per vertex from the major normal axis (or the
 * u-0.5/v-0.5 fallback) and keeps face-local (u,v).  PICA has no per-vertex
 * face attribute: a cube map is sampled from a 3-component direction, whose
 * third component travels in texcoord0w.  The adapter therefore converts each
 * vertex into {pos3, direction3, rgba4}; d9_cube_direction() builds the
 * standard face direction with the same vertical flip the 2D colour shader
 * uses.  Level 0 is always the base level; when the sampler's MIP filter is
 * not NONE the core has already required every mip level to be resident and
 * the same C3D_Tex mip chain is configured here.  Sampler state is
 * point/clamp only for the wrap mode; filters follow the D3D state.
 */
static void configure_native_cube(NativeCube *t,const D9DrawState *state){
    /* PICA LOD clamp: base level 0, top level = last resident mip. */
    unsigned max=state&&state->resident_mips?state->resident_mips-1u:0u;
    if(max>=t->levels)max=t->levels-1u;
    t->tex.minLevel=0;t->tex.maxLevel=(u8)max;
    C3D_TexSetFilter(&t->tex,state&&state->mag_filter==2?GPU_LINEAR:GPU_NEAREST,
                     state&&state->min_filter==2?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetFilterMipmap(&t->tex,state&&state->mip_filter==2?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetWrap(&t->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    C3D_TexSetLodBias(&t->tex,state?d9_sampler_lod_bias(state->lod_bias):0.0f);
}
static void prepare_draw_cube(D9Citro *c){
    C3D_BindProgram(&c->program_cube);
    C3D_AttrInfo *attr=C3D_GetAttrInfo();AttrInfo_Init(attr);
    /* D9CubeVertex is {pos3, direction3, rgba4}. */
    AttrInfo_AddLoader(attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(attr,1,GPU_FLOAT,3);
    AttrInfo_AddLoader(attr,2,GPU_FLOAT,4);
    C3D_BufInfo *buf=C3D_GetBufInfo();BufInfo_Init(buf);
    BufInfo_Add(buf,c->arena_cube,sizeof(D9CubeVertex),3,0x210);
}
static int draw_cube(void *ctx,const D9ColorVertex *vertices,size_t count,
                     const unsigned *faces,const D9Matrix *clip,
                     const D9CubeView *cube,const D9DrawState *state){
    D9Citro *c=ctx;
    if(!c||!c->scene||!c->program_cube_ready||!vertices||!faces||!clip||
       !cube||!state||!count||count%3)return -1;
    NativeCube *native=cube->native;
    if(!native||cube->levels<1||cube->edge<8||cube->edge>1024){
        fprintf(stderr,"[d3d9-citro] draw_cube: cube view has no native or an invalid edge/level\n");
        return -1;
    }
    if(cube->format!=D9_A8R8G8B8&&cube->format!=D9_X8R8G8B8){
        fprintf(stderr,"[d3d9-citro] draw_cube: only A8R8G8B8/X8R8G8B8 cube sampling is implemented\n");
        return -1;
    }
    if(state->pixel_shader){
        fprintf(stderr,"[d3d9-citro] draw_cube: a pixel shader owns the TEV stages; fixed-function cube sampling only\n");
        return -1;
    }
    if(state->mip_filter&&state->resident_mips<cube->levels){
        fprintf(stderr,"[d3d9-citro] draw_cube: cube mip sampling requires all mip levels resident\n");
        return -1;
    }
    D9ColorTevStage plan;const char *why=NULL;
    if(d9_color_tev_stage0(state,1,&plan,&why)){
        fprintf(stderr,"[d3d9-citro] draw_cube: %s\n",why?why:"stage-0 state is not expressible");
        return -1;
    }
    if(count>FRAME_VERTICES-c->cursor_cube)return -1;
    for(size_t i=0;i<count;++i){
        if(faces[i]>=D9_CUBE_FACES){
            fprintf(stderr,"[d3d9-citro] draw_cube: face index %u is outside 0..5\n",faces[i]);
            return -1;
        }
        float dir[3];d9_cube_direction(faces[i],vertices[i].u,vertices[i].v,dir);
        D9CubeVertex *o=&c->arena_cube[c->cursor_cube+i];
        o->x=vertices[i].x;o->y=vertices[i].y;o->z=vertices[i].z;
        o->dx=dir[0];o->dy=dir[1];o->dz=dir[2];
        o->r=vertices[i].r;o->g=vertices[i].g;o->b=vertices[i].b;o->a=vertices[i].a;
    }
    GSPGPU_FlushDataCache(c->arena_cube+c->cursor_cube,count*sizeof(D9CubeVertex));
    prepare_draw_cube(c);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform_cube,&m);
    apply_state(state,1);
    configure_native_cube(native,state);
    d9_pica_citro3d_restore_tev();
    C3D_TexBind(0,&native->tex);
    apply_color_tev(0,&plan);
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor_cube,(int)count);
    d9_pica_citro3d_restore_tev();
    c->cursor_cube+=count;
    prepare_draw(c);
    return 0;
}
/* Menu TEV plans: the colour vertex program carries color0, the plan can bind
 * two texture units, and external stage constants come from the c0..c8 bank.
 * c8 is the internal ambient+emissive fold, not a D3D9 cap. */
static int draw_menu_ex(void *ctx,const D9ColorVertex *vertices,size_t count,
                        const D9Matrix *clip,void *texture0,void *texture1,
                        const D9DrawState *state,const float (*constants)[4],
                        const uint16_t *indices,size_t index_count){
    D9Citro *c=ctx;
    if(!c||!c->scene||!c->program_color_ready||!vertices||!clip||!state||
       !state->pixel_shader||!count)return -1;
    if(indices?(!index_count||index_count%3):(!count||count%3))return -1;
    if(indices)for(size_t i=0;i<index_count;++i)if(indices[i]>=count)return -1;
    int text_plan=(state->pixel_shader->backend_flags&D9TEV_PLAN_MENU_TEXT)!=0;
    NativeTex *native0=texture0,*native1=texture1;
    if(text_plan&&
       (state->pixel_shader->stages!=1||state->pixel_shader->textures!=1||
        !native0||native1||native0->format!=D9_A8)){
        fprintf(stderr,"[d3d9-citro] draw_menu: text TEV plan requires an A8 texture unit\n");
        return -1;
    }
    if(count>FRAME_MENU_VERTICES-c->cursor_color)return -1;
    if(indices&&index_count>FRAME_INDICES-c->cursor_index)return -1;
    static int menu_clip_logged;
    if(!menu_clip_logged){
        menu_clip_logged=1;
        fprintf(stderr,"MENU_CLIP m00=%.6f m03=%.6f m11=%.6f m13=%.6f "
                        "m22=%.6f m33=%.6f\n",
                clip->m[0][0],clip->m[0][3],clip->m[1][1],clip->m[1][3],
                clip->m[2][2],clip->m[3][3]);
    }
    D9DrawState draw_state=*state;
    if(text_plan){
        /* The menu-text contract is point sampling with clamped UVs, regardless
         * of any stale sampler state from an earlier draw. */
        draw_state.address_u=3;draw_state.address_v=3; /* D3DTADDRESS_CLAMP */
        draw_state.mag_filter=1;draw_state.min_filter=1; /* D3DTEXF_POINT */
        draw_state.mip_filter=0; /* D3DTEXF_NONE */
    }
    memcpy(c->arena_color+c->cursor_color,vertices,count*sizeof *vertices);
    GSPGPU_FlushDataCache(c->arena_color+c->cursor_color,count*sizeof *vertices);
    if(indices){
        memcpy(c->arena_index+c->cursor_index,indices,index_count*sizeof *indices);
        GSPGPU_FlushDataCache(c->arena_index+c->cursor_index,index_count*sizeof *indices);
    }
    prepare_draw_color(c);
    D9Matrix mtx=*clip;orient_clip(c,&mtx);
    C3D_Mtx m;for(unsigned i=0;i<4;++i)m.r[i]=FVec4_New(mtx.m[i][0],mtx.m[i][1],mtx.m[i][2],mtx.m[i][3]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,c->uniform_color,&m);
    apply_state(&draw_state,1);
    if(apply_shader_alpha_test(&draw_state))return -1;
    if(texture0)configure_native_texture(texture0,&draw_state);
    if(texture1)configure_native_texture(texture1,&draw_state);
    /* The generic draw path binds its unit explicitly; the menu path used to
     * rely on the TEV apply doing it, so the A8 atlas never reached unit 0. */
    if(native0)C3D_TexBind(0,&native0->tex);
    if(native1)C3D_TexBind(1,&native1->tex);
    d9_pica_citro3d_restore_tev();
    if(d9_pica_citro3d_apply_tev_ex(state->pixel_shader,
                                    native0?&native0->tex:NULL,
                                    native1?&native1->tex:NULL,
                                    constants,9,NULL,0)){
        d9_pica_citro3d_restore_tev();
        prepare_draw(c);
        return -1;
    }
    if(indices)
        C3D_DrawElements(GPU_TRIANGLES,(int)index_count,C3D_UNSIGNED_SHORT,
                         c->arena_index+c->cursor_index);
    else
        C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor_color,(int)count);
    d9_pica_citro3d_restore_tev();
    c->cursor_color+=count;
    if(indices)c->cursor_index+=index_count;
    prepare_draw(c);
    return 0;
}
static int draw_menu(void *ctx,const D9ColorVertex *vertices,size_t count,
                     const D9Matrix *clip,void *texture0,void *texture1,
                     const D9DrawState *state,const float (*constants)[4]){
    return draw_menu_ex(ctx,vertices,count,clip,texture0,texture1,state,constants,NULL,0);
}
static int draw_menu_indexed(void *ctx,const D9ColorVertex *vertices,size_t count,
                             const uint16_t *indices,size_t index_count,
                             const D9Matrix *clip,void *texture0,void *texture1,
                             const D9DrawState *state,const float (*constants)[4]){
    if(!indices||!clip)return -1;
    return draw_menu_ex(ctx,vertices,count,clip,texture0,texture1,state,constants,indices,index_count);
}
static void end(void *ctx){D9Citro *c=ctx;if(c->scene){C3D_FrameEnd(0);c->scene=0;}c->active_target=NULL;}
static int target_create(void *ctx,unsigned w,unsigned h,unsigned format,void **native){
    (void)ctx;if(!native||w<8||h<8||w>1024||h>1024||(w&(w-1))||(h&(h-1))||
       (format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8))return -1;
    NativeTarget *t=calloc(1,sizeof *t);if(!t)return -1;
    C3D_TexInitParams params={(u16)w,(u16)h,0,GPU_RGBA8,GPU_TEX_2D,true};
    if(!C3D_TexInitWithParams(&t->tex,NULL,params)){
        fprintf(stderr,"[d3d9-citro] CreateRenderTarget: %ux%u VRAM texture allocation failed "
                "(vram free %u KiB)\n",w,h,(unsigned)(vramSpaceFree()>>10));
        free(t);return -1;
    }
    C3D_TexSetFilter(&t->tex,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&t->tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    /* Colour-only target.  PICA VRAM is 6 MiB and every DEPTH24_STENCIL8
     * attachment doubles an offscreen target's cost; the core attaches depth
     * lazily when a matching D3D depth-stencil surface is actually bound.
     * Render-target textures used only for sampling therefore spend no VRAM on
     * depth they never touch. */
    t->head.target=C3D_RenderTargetCreateFromTex(&t->tex,GPU_TEXFACE_2D,0,-1);
    if(!t->head.target){
        fprintf(stderr,"[d3d9-citro] CreateRenderTarget: %ux%u render-target allocation failed "
                "(vram free %u KiB)\n",w,h,(unsigned)(vramSpaceFree()>>10));
        C3D_TexDelete(&t->tex);free(t);return -1;
    }
    t->head.has_depth=0;
    /* VRAM starts with arbitrary bytes.  PICA depth 0 is "far" and the draws
     * use GPU_GEQUAL, so a prime clear avoids sampling a fresh colour target's
     * garbage.  A depth-less target has no depth to prime.  This is a private
     * frame: the target is not linked to either LCD, so FrameEnd performs no
     * display transfer. */
    if(C3D_FrameBegin(C3D_FRAME_SYNCDRAW)){
        if(C3D_FrameDrawOn(t->head.target))
            C3D_RenderTargetClear(t->head.target,C3D_CLEAR_COLOR,0,0);
        C3D_FrameEnd(0);
    }
    fprintf(stderr,"[d3d9-citro] target %ux%u colour-only; vram free %u KiB\n",
            w,h,(unsigned)(vramSpaceFree()>>10));
    /* Sampling view: same C3D_Tex data, separate filter/wrap state.  The
     * target's own C3D_Tex owns the storage; the view is never deleted. */
    t->sample.tex=t->tex;t->sample.levels=1;
    t->head.blit_texture=&t->tex;
    t->head.w=w;t->head.h=h;t->head.format=format;*native=t;return 0;
}
static int target_has_depth(void *ctx,void *native){
    (void)ctx;NativeTargetHead *h=native;return h?h->has_depth:0;
}
/* Attach a DEPTH24_STENCIL8 attachment to an existing colour target.  Called
 * by the core outside BeginScene/EndScene when a matching D3D depth surface is
 * bound.  The colour storage (C3D_Tex) is reused; only the render-target view
 * and its depth buffer are recreated.  Failure leaves the colour-only view
 * live and is reported as a named VRAM refusal by the core. */
static int target_depth_attach(void *ctx,void *native){
    D9Citro *c=ctx;NativeTargetHead *h=native;
    if(!c||!h)return -1;
    if(h->has_depth)return 0;
    if(h->is_cube_face){fprintf(stderr,"[d3d9-citro] target depth attach: cube face view is not a 2D target\n");return -1;}
    if(c->scene){fprintf(stderr,"[d3d9-citro] target depth attach: inside a scene\n");return -1;}
    NativeTarget *t=(NativeTarget *)native;
    if(!t->tex.data)return -1;
    C3D_FrameSync();
    C3D_RenderTarget *rt=C3D_RenderTargetCreateFromTex(&t->tex,GPU_TEXFACE_2D,0,
                                                       GPU_RB_DEPTH24_STENCIL8);
    if(!rt){
        fprintf(stderr,"[d3d9-citro] target depth attach: %ux%u DEPTH24_STENCIL8 allocation failed "
                "(vram free %u KiB)\n",h->w,h->h,(unsigned)(vramSpaceFree()>>10));
        return -1;
    }
    C3D_RenderTarget *old=h->target;
    h->target=rt;h->has_depth=1;
    if(old)C3D_RenderTargetDelete(old);
    if(C3D_FrameBegin(C3D_FRAME_SYNCDRAW)){
        if(C3D_FrameDrawOn(rt))
            C3D_RenderTargetClear(rt,C3D_CLEAR_DEPTH,0,0);
        C3D_FrameEnd(0);
    }
    fprintf(stderr,"[d3d9-citro] target %ux%u depth attached; vram free %u KiB\n",
            h->w,h->h,(unsigned)(vramSpaceFree()>>10));
    return 0;
}
static void *target_texture(void *ctx,void *native){
    (void)ctx;NativeTarget *t=native;if(!t)return NULL;
    return &t->sample;
}
static int target_bind(void *ctx,void *native){
    D9Citro *c=ctx;if(!c||!c->scene)return -1;
    NativeTargetHead *h=native;
    C3D_RenderTarget *target=h?h->target:c->target;
    if(!target||!C3D_FrameDrawOn(target))return -1;
    c->active_target=native;return 0;
}
static int target_clear(void *ctx,void *native,uint32_t argb){
    D9Citro *c=ctx;NativeTargetHead *t=native;if(!c||!c->scene||!t)return -1;
    C3D_RenderTargetClear(t->target,C3D_CLEAR_COLOR,(argb<<8)|(argb>>24),0);return 0;
}
static int target_read(void *ctx,void *native,void *dst,size_t pitch,
                       unsigned x,unsigned y,unsigned w,unsigned h){
    D9Citro *c=ctx;NativeTarget *t=native;if(!c||!t||!dst||c->scene||x>t->head.w||y>t->head.h||w>t->head.w-x||h>t->head.h-y)return -1;
    size_t bytes=(size_t)t->head.w*t->head.h*4;if(bytes!=t->tex.size)return -1;
    C3D_FrameSync();uint8_t *tmp=linearAlloc(bytes);if(!tmp)return -1;
    C3D_SyncTextureCopy((u32 *)t->tex.data,0,(u32 *)tmp,0,(u32)bytes,8);
    GSPGPU_InvalidateDataCache(tmp,bytes);
    int rc=d9_unswizzle_rgba8(dst,pitch,tmp,t->head.w,t->head.h,t->head.format,x,y,w,h);
    linearFree(tmp);return rc;
}
static void target_delete(void *ctx,void *native){
    D9Citro *c=ctx;NativeTarget *t=native;if(!t)return;
    if(c->active_target==native)c->active_target=NULL;
    C3D_FrameSync();if(t->head.target)C3D_RenderTargetDelete(t->head.target);
    C3D_TexDelete(&t->tex);free(t);
}
/* Cube-face render target: a C3D_RenderTarget view over one face/level of an
 * existing NativeCube.  It owns the render-target and depth attachment only;
 * the cube texture owns the colour storage and stays alive until cube delete.
 * PICA's API always has a depth attachment, so the D3D depth policy is D24S8. */
static int cube_face_create(void *ctx,void *native_cube,unsigned face,
                            unsigned level,unsigned depth_flags,void **native){
    (void)ctx;(void)depth_flags;NativeCube *cube=native_cube;
    if(!native||!cube||face>=D9_CUBE_FACES||level>=cube->levels)return -2;
    NativeCubeFace *f=calloc(1,sizeof *f);if(!f)return -3;
    f->head.target=C3D_RenderTargetCreateFromTex(&cube->tex,(GPU_TEXFACE)face,
                                                 (int)level,GPU_RB_DEPTH24_STENCIL8);
    if(!f->head.target){
        fprintf(stderr,"[d3d9-citro] GetCubeMapSurface: face %u level %u render-target allocation failed\n",
                face,level);
        free(f);return -3;
    }
    unsigned edge=cube->edge>>level;if(!edge)edge=1;
    f->head.w=f->head.h=edge;f->head.format=cube->format;
    f->head.has_depth=1;f->head.is_cube_face=1;
    f->cube=cube;f->face=face;f->level=level;
    /* Prime the private depth/colour view, matching the 2D target_create path.
     * The cube face is not linked to either LCD, so FrameEnd displays nothing. */
    if(C3D_FrameBegin(C3D_FRAME_SYNCDRAW)){
        if(C3D_FrameDrawOn(f->head.target))
            C3D_RenderTargetClear(f->head.target,C3D_CLEAR_DEPTH|C3D_CLEAR_COLOR,0,0);
        C3D_FrameEnd(0);
    }
    *native=f;return 0;
}
static void cube_face_delete(void *ctx,void *native){
    D9Citro *c=ctx;NativeCubeFace *f=native;if(!f)return;
    if(c&&c->active_target==native)c->active_target=NULL;
    C3D_FrameSync();if(f->head.target)C3D_RenderTargetDelete(f->head.target);
    free(f);
}
/* Whole-surface write-back for the lockable back buffer.  The source is
 * top-first BGRA; PICA RGBA8 storage is tiled with a bottom-left origin, so the
 * existing swizzle produces the exact texel layout and C3D_TexLoadImage replaces
 * level 0.  This is a real upload, not a shadow copy. */
static int target_write(void *ctx,void *native,const void *bgra,size_t pitch,
                        unsigned w,unsigned h){
    D9Citro *c=ctx;NativeTarget *t=native;
    if(!c||!t||!bgra||c->scene||w!=t->head.w||h!=t->head.h||pitch<(size_t)w*4)return -1;
    size_t bytes=(size_t)t->head.w*t->head.h*4;if(bytes!=t->tex.size)return -1;
    C3D_FrameSync();
    uint8_t *tmp=linearAlloc(bytes);if(!tmp)return -1;
    if(d9_swizzle_rgba8(tmp,bytes,bgra,pitch,w,h,t->head.format)){linearFree(tmp);return -1;}
    t->tex.maxLevel=0;t->tex.minLevel=0;
    C3D_TexLoadImage(&t->tex,tmp,GPU_TEXFACE_2D,0);
    C3D_TexFlush(&t->tex);
    linearFree(tmp);return 0;
}
/* PICA target_create/render-target views allocate depth lazily; the
 * d9_set_depth_surface path validates the descriptor size and calls
 * target_depth_attach.  The core's per-target callbacks replace the old
 * always-embedded depth policy. */
/* Clear the depth attachment of the currently bound colour target.  PICA stores
 * depth and stencil in one DEPTH24_STENCIL8 buffer, so a depth clear writes the
 * packed word (and, with it, the stencil byte). */
static int depth_clear(void *ctx,unsigned flags,uint32_t depth_stencil){
    D9Citro *c=ctx;
    if(!c||!c->scene||!(flags&6u)||(flags&~7u))return -1;
    C3D_RenderTarget *rt=c->active_target?((NativeTargetHead *)c->active_target)->target:c->target;
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
/* Bounded native target: the D3D surface stays 1024x1024, but the colour
 * storage and depth attachment are 512x512.  Mapping NDC through the native
 * viewport scales the whole image into that storage instead of clipping it at
 * 512.  The retained back buffer never takes this path. */
static int target_viewport_scaled(void *ctx,unsigned logical_w,unsigned logical_h,
                                  unsigned native_w,unsigned native_h){
    D9Citro *c=ctx;
    if(!c||!c->scene||!logical_w||!logical_h||!native_w||!native_h||
       native_w>logical_w||native_h>logical_h)return -1;
    NativeTargetHead *t=c->active_target;
    if(!t||native_w>t->w||native_h>t->h)return -1;
    C3D_SetViewport(0,0,native_w,native_h);
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
    if(src->x+src->width>src->surface.width||src->y+src->height>src->surface.height||
       dst->x+dst->width>dst->surface.width||dst->y+dst->height>dst->surface.height)
        return D9_BLIT_RECT;
    NativeTargetHead *native=dst->surface.native;
    C3D_RenderTarget *rt=native?native->target:c->target;
    NativeTargetHead *source=src->surface.native;
    C3D_Tex *stex=source->blit_texture;
    if(!stex)return D9_BLIT_UNSUPPORTED;
    if(!rt)return D9_BLIT_BACKEND;
    unsigned sw=src->surface.storage_width?src->surface.storage_width:src->surface.width;
    unsigned shgt=src->surface.storage_height?src->surface.storage_height:src->surface.height;
    unsigned dw=dst->surface.storage_width?dst->surface.storage_width:dst->surface.width;
    unsigned dh=dst->surface.storage_height?dst->surface.storage_height:dst->surface.height;
    /* The retained back buffer keeps its logical 400x240 content in a 512x256
     * storage corner, so its UV denominator is the storage size.  An explicit
     * bounded target fills the whole native storage from its logical surface,
     * so its UV denominator is the larger logical size. */
    unsigned uvw=src->surface.width>sw?src->surface.width:sw;
    unsigned uvh=src->surface.height>shgt?src->surface.height:shgt;
    if(stex->width!=sw||stex->height!=shgt)return D9_BLIT_UNSUPPORTED;
    if(dst->surface.native&&(native->w!=dw||native->h!=dh))return D9_BLIT_UNSUPPORTED;
    if(c->cursor+6>FRAME_VERTICES)return D9_BLIT_BACKEND;
    int own_frame=!c->scene;
    if(own_frame){
        if(!dst->surface.native)return D9_BLIT_NO_SCENE;
        if(!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))return D9_BLIT_BACKEND;
    }
    if(!C3D_FrameDrawOn(rt)){if(own_frame)C3D_FrameEnd(0);return D9_BLIT_BACKEND;}
    if(dst->surface.native){
        /* Explicit bounded target: draw into the whole native storage.  The
         * retained back buffer is the opposite case (storage larger than the
         * logical surface) and keeps its logical corner viewport. */
        unsigned vw=dw,vh=dh;
        if(vw>=dst->surface.width&&vh>=dst->surface.height){
            vw=dst->surface.width;vh=dst->surface.height;
        }
        C3D_SetViewport(0,0,vw,vh);
    }
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
    float u0=(float)src->x/(float)uvw;
    float v0=(float)src->y/(float)uvh;
    float u1=(float)(src->x+src->width)/(float)uvw;
    float v1=(float)(src->y+src->height)/(float)uvh;
    D9Vertex *v=c->arena+c->cursor;
    v[0]=(D9Vertex){x0,y0,0.0f,u0,v0};v[1]=(D9Vertex){x1,y0,0.0f,u1,v0};v[2]=(D9Vertex){x1,y1,0.0f,u1,v1};
    v[3]=v[0];v[4]=v[2];v[5]=(D9Vertex){x0,y1,0.0f,u0,v1};
    GSPGPU_FlushDataCache(v,6*sizeof *v);
    C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor,6);
    c->cursor+=6;
    stex->param=saved_param;
    d9_pica_citro3d_restore_tev();
    if(own_frame){C3D_FrameEnd(0);return D9_BLIT_OK;}
    C3D_RenderTarget *prev=c->active_target?((NativeTargetHead *)c->active_target)->target:c->target;
    if(prev&&prev!=rt&&!C3D_FrameDrawOn(prev))return D9_BLIT_BACKEND;
    return D9_BLIT_OK;
}
const D9Backend *d9_citro_backend(void){static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end,NULL,tex_create_encoded,tex_upload_encoded,
    target_create,target_delete,target_bind,target_clear,target_read,target_blit,draw_colored,draw_color_stages,
    target_write,depth_clear,target_viewport,draw_menu,target_texture,
    cube_create,cube_upload,target_viewport_scaled,NULL,NULL,draw_cube,cube_delete,
    cube_face_create,cube_face_delete,tex_create_format,tex_upload_format,draw_menu_indexed,
    /* draw_volume: citro3d/libctru exposes GPU_TEX_2D, CUBE_MAP, SHADOW_2D,
     * PROJECTION and SHADOW_CUBE only; there is no 3D texture mode, so volume
     * sampling is refused by name and the PICA backend also has no
     * texture_create_volume callback. */
    NULL,NULL,target_has_depth,target_depth_attach};return &ops;}
