#include "d3d9_null.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {unsigned w,h,levels;} NullTex;
typedef struct {unsigned w,h,format;} NullTarget;
static D9NullStats g;

static D9NullStats *stats(void *ctx){return ctx?(D9NullStats *)ctx:&g;}
static int pow2(unsigned n){return n&&n<=1024&&!(n&(n-1));}
static unsigned level_dim(unsigned n,unsigned level){n>>=level;return n?n:1;}
static void null_log(const char *op,const char *why){fprintf(stderr,"[d3d9-null] %s: %s\n",op,why);}

static int null_texture_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    D9NullStats *s=stats(ctx);
    if(!native){null_log("CreateTexture","null native handle");return -1;}
    if(!pow2(w)||!pow2(h)||w<8||h<8||!levels||levels>11){
        null_log("CreateTexture","only power-of-two 8..1024 textures with 1..11 uploaded levels are implemented");
        return -1;
    }
    NullTex *t=calloc(1,sizeof *t);if(!t){null_log("CreateTexture","out of memory");return -1;}
    t->w=w;t->h=h;t->levels=levels;*native=t;++s->texture_creates;return 0;
}
static int null_texture_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    D9NullStats *s=stats(ctx);NullTex *t=native;
    if(!t||level>=t->levels||!data||!bytes){null_log("UnlockRect","invalid texture, mip level or pixel data");return -1;}
    size_t want=(size_t)level_dim(t->w,level)*level_dim(t->h,level)*4;
    if(bytes!=want){null_log("UnlockRect","upload size does not match the RGBA8 mip level size");return -1;}
    ++s->texture_uploads;return 0;
}
static void null_texture_delete(void *ctx,void *native){
    D9NullStats *s=stats(ctx);NullTex *t=native;
    if(!t){null_log("ReleaseTexture","null native handle");return;}
    free(t);++s->texture_deletes;
}
static int null_begin(void *ctx,uint32_t argb){
    D9NullStats *s=stats(ctx);
    if(s->scene){null_log("BeginScene","a scene is already active");return -1;}
    s->scene=1;++s->begins;s->last_clear_argb=argb;s->target_active=0;return 0;
}
static void null_end(void *ctx){
    D9NullStats *s=stats(ctx);
    if(!s->scene){null_log("EndScene/Present","no scene is active");return;}
    s->scene=0;++s->ends;++s->presents;s->target_active=0;
}
static int state_ok(const D9DrawState *st){
    return st->z_enable<=1&&st->z_write<=1&&st->alpha_test<=1&&st->blend<=1&&
        st->z_func>=1&&st->z_func<=8&&st->cull>=1&&st->cull<=3&&
        st->alpha_func>=1&&st->alpha_func<=8&&st->alpha_ref<=255&&
        st->src_blend>=1&&st->src_blend<=11&&st->dst_blend>=1&&st->dst_blend<=11&&
        st->blend_op>=1&&st->blend_op<=5&&st->color_write<=15&&
        st->address_u>=1&&st->address_u<=3&&st->address_v>=1&&st->address_v<=3&&
        st->mag_filter>=1&&st->mag_filter<=2&&st->min_filter>=1&&st->min_filter<=2&&
        st->mip_filter<=2&&st->max_mip<=10&&st->resident_mips<=10;
}
static int stage_op_ok(unsigned op){
    return op==D9_TOP_DISABLE||op==D9_TOP_SELECTARG1||op==D9_TOP_SELECTARG2||op==D9_TOP_MODULATE;
}
static int stage_arg_ok(unsigned arg,int allow_current){
    return arg==D9_TA_DIFFUSE||arg==D9_TA_TEXTURE||(allow_current&&arg==D9_TA_CURRENT);
}
/* The null backend has no pixels, so it records the colour draw and its exact
 * input RGBA.  It also validates that the stage-0 tuple is inside the closed
 * SetTextureStageState subset; it does not claim the real PICA TEV can render
 * a combination (that is d9_color_tev_stage0() and the citro3d backend). */
static int null_draw_colored(void *ctx,const D9ColorVertex *v,size_t n,
                             const D9Matrix *pica_clip,void *texture,
                             const D9DrawState *st){
    D9NullStats *s=stats(ctx);(void)texture;
    if(!s->scene){null_log("DrawPrimitive","no scene is active");return -1;}
    if(!v||!n||n%3){null_log("DrawPrimitive","colour vertex list is empty, null or not a multiple of three");return -1;}
    if(!pica_clip||!st){null_log("DrawPrimitive","null clip matrix or draw state");return -1;}
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        if(!isfinite(pica_clip->m[i][j])){null_log("DrawPrimitive","non-finite clip matrix");return -1;}
    for(size_t i=0;i<n;++i){
        const D9ColorVertex *p=&v[i];
        if(!isfinite(p->x)||!isfinite(p->y)||!isfinite(p->z)||
           !isfinite(p->u)||!isfinite(p->v)||!isfinite(p->r)||
           !isfinite(p->g)||!isfinite(p->b)||!isfinite(p->a)){
            null_log("DrawPrimitive","non-finite colour vertex");return -1;
        }
    }
    if(!state_ok(st)){null_log("DrawPrimitive","draw state is outside the supported D3D9 range");return -1;}
    if(!stage_op_ok(st->stage_colorop)||!stage_op_ok(st->stage_alphaop)||
       !stage_arg_ok(st->stage_colorarg1,0)||!stage_arg_ok(st->stage_colorarg2,1)||
       !stage_arg_ok(st->stage_alphaarg1,0)||!stage_arg_ok(st->stage_alphaarg2,1)||
       st->stage_texcoordindex>7){
        null_log("DrawPrimitive","colour draw stage state is outside the closed stage-0 subset");return -1;
    }
    ++s->draws;++s->color_draws;s->triangles+=(unsigned)(n/3);s->vertices+=n;s->color_vertices+=n;
    if(s->target_active)++s->target_draws;
    s->last_color_count=n<3?n:3;
    memcpy(s->last_color_vertices,v,s->last_color_count*sizeof *v);
    s->last_color_state=*st;
    return 0;
}
static int null_draw(void *ctx,const D9Vertex *vertices,size_t count,const D9Matrix *pica_clip,void *texture,const D9DrawState *st){
    D9NullStats *s=stats(ctx);(void)texture;
    if(!s->scene){null_log("DrawPrimitive","no scene is active");return -1;}
    if(!vertices||!count||count%3){null_log("DrawPrimitive","vertex list is empty, null or not a multiple of three");return -1;}
    if(!pica_clip||!st){null_log("DrawPrimitive","null clip matrix or draw state");return -1;}
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        if(!isfinite(pica_clip->m[i][j])){null_log("DrawPrimitive","non-finite clip matrix");return -1;}
    if(!state_ok(st)){null_log("DrawPrimitive","draw state is outside the supported D3D9 range");return -1;}
    ++s->draws;s->triangles+=(unsigned)(count/3);s->vertices+=count;if(s->target_active)++s->target_draws;return 0;
}
static int null_target_create(void *ctx,unsigned w,unsigned h,unsigned format,void **native){
    D9NullStats *s=stats(ctx);
    if(!native||w<8||h<8||w>1024||h>1024||(w&(w-1))||(h&(h-1))||
       (format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8)){
        null_log("CreateRenderTarget","only power-of-two 8..1024 X8R8G8B8/A8R8G8B8 targets are implemented");return -1;
    }
    NullTarget *t=calloc(1,sizeof *t);if(!t)return -1;
    t->w=w;t->h=h;t->format=format;*native=t;++s->target_creates;return 0;
}
static void null_target_delete(void *ctx,void *native){
    D9NullStats *s=stats(ctx);free(native);++s->target_deletes;
}
static int null_target_bind(void *ctx,void *native){
    D9NullStats *s=stats(ctx);if(!s->scene){null_log("SetRenderTarget","target bind requires an active scene");return -1;}
    s->target_active=native!=NULL;++s->target_binds;return 0;
}
static int null_target_clear(void *ctx,void *native,uint32_t argb){
    D9NullStats *s=stats(ctx);if(!s->scene||!native)return -1;
    s->last_clear_argb=argb;++s->target_clears;return 0;
}
/* The null backend validates and counts a StretchRect but owns no pixels.  A
 * successful result means "the call stream was accepted", never "the image was
 * copied". */
static int null_target_blit(void *ctx,const D9BlitRegion *dst,const D9BlitRegion *src,unsigned filter){
    D9NullStats *s=stats(ctx);
    if(!dst||!src||!dst->width||!dst->height||!src->width||!src->height||filter>D9_FILTER_LINEAR){
        null_log("StretchRect","invalid region, surface or filter");return D9_BLIT_ARGUMENT;
    }
    if(dst->surface.native&&src->surface.native&&dst->surface.native==src->surface.native){
        null_log("StretchRect","source and destination are the same explicit target");return D9_BLIT_SAME_SURFACE;
    }
    if(src->surface.format!=dst->surface.format){
        null_log("StretchRect","source and destination formats differ");return D9_BLIT_FORMAT;
    }
    ++s->blits;s->blit_pixels+=(unsigned long long)dst->width*dst->height;
    if(dst->width!=src->width||dst->height!=src->height)++s->blit_stretched;
    if(filter==D9_FILTER_LINEAR)++s->blit_linear;
    s->last_blit_filter=filter;
    s->last_blit_src_back=src->surface.native==NULL;
    s->last_blit_dst_back=dst->surface.native==NULL;
    return D9_BLIT_OK;
}
static int null_draw_two(void *ctx,const D9ColorVertex *vertices,size_t count,
                         const D9Matrix *clip,void *tex0,const D9DrawState *stage0,
                         void *tex1,const D9DrawState *stage1){
    if(!tex0||!tex1||!stage1||stage1->stage_texcoordindex||
       !stage_op_ok(stage1->stage_colorop)||!stage_op_ok(stage1->stage_alphaop))
        return -1;
    return null_draw_colored(ctx,vertices,count,clip,tex0,stage0);
}
static const D9Backend backend={null_texture_create,null_texture_upload,null_texture_delete,null_begin,null_draw,null_end,
    NULL,NULL,NULL,null_target_create,null_target_delete,null_target_bind,null_target_clear,NULL,null_target_blit,null_draw_colored,null_draw_two};
const D9Backend *d9_null_backend(void){return &backend;}
void *d9_null_context(void){return &g;}
const D9NullStats *d9_null_stats(void){return &g;}
void d9_null_note_clear(unsigned flags){++g.clears;g.last_clear_flags=flags;if(flags&1u)++g.clear_targets;}
void d9_null_reset(void){memset(&g,0,sizeof g);}
