#include "d3d9_null.h"
#include "d3d9_bc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {unsigned w,h,levels;} NullTex;
typedef struct {unsigned w,h,format;uint8_t *pixels;} NullTarget;
static D9NullStats g;
/* The null backend owns no LCD, but Present's screen blit has a destination.
 * Keeping it as real BGRA bytes lets host tests verify the retained-back-buffer
 * presentation path pixel by pixel instead of only counting calls.  The buffer
 * lives outside D9NullStats so copying the stats struct stays cheap. */
enum {NULL_PRESENT_W=400,NULL_PRESENT_H=240};
static uint8_t g_present[NULL_PRESENT_W*NULL_PRESENT_H*4];

static D9NullStats *stats(void *ctx){return ctx?(D9NullStats *)ctx:&g;}
static int pow2(unsigned n){return n&&n<=1024&&!(n&(n-1));}
static unsigned level_dim(unsigned n,unsigned level){n>>=level;return n?n:1;}
static void null_log(const char *op,const char *why){fprintf(stderr,"[d3d9-null] %s: %s\n",op,why);}
/* A D3DCOLOR 0xAARRGGBB has the little-endian byte order B,G,R,A, which is
 * exactly the top-first BGRA layout target_read returns. */

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
typedef struct {unsigned edge,levels,face_uploads;} NullCube;
static int null_cube_create(void *ctx,unsigned edge,unsigned levels,void **native){
    D9NullStats *s=stats(ctx);
    if(!native||!pow2(edge)||edge<8||edge>1024||!levels||levels>11){
        null_log("CreateCubeTexture","only power-of-two 8..1024 cube edges with 1..11 levels are implemented");
        return -1;
    }
    NullCube *c=calloc(1,sizeof *c);if(!c)return -1;
    c->edge=edge;c->levels=levels;*native=c;++s->cube_creates;return 0;
}
static int null_cube_upload(void *ctx,void *native,unsigned face,unsigned level,
                            const void *data,size_t bytes){
    D9NullStats *s=stats(ctx);NullCube *c=native;
    if(!c||face>=D9_CUBE_FACES||level>=c->levels||!data||!bytes){
        null_log("UnlockRect","invalid cube, face, mip level or pixel data");
        return -1;
    }
    ++c->face_uploads;++s->cube_uploads;return 0;
}
typedef struct {unsigned w,h,depth,levels,uploads;} NullVolume;
static int null_volume_create(void *ctx,unsigned w,unsigned h,unsigned depth,
                              unsigned levels,void **native){
    D9NullStats *s=stats(ctx);
    if(!native||!pow2(w)||!pow2(h)||!pow2(depth)||depth<2||
       w>1024||h>1024||depth>1024||!levels||levels>11){
        null_log("CreateVolumeTexture","only power-of-two 1..1024 dimensions, depth > 1 "
                 "and 1..11 levels are implemented");
        return -1;
    }
    NullVolume *v=calloc(1,sizeof *v);if(!v)return -1;
    v->w=w;v->h=h;v->depth=depth;v->levels=levels;*native=v;++s->volume_creates;return 0;
}
static int null_volume_upload(void *ctx,void *native,unsigned level,
                              const void *data,size_t bytes){
    D9NullStats *s=stats(ctx);NullVolume *v=native;
    if(!v||level>=v->levels||!data||!bytes){
        null_log("UnlockBox","invalid volume, mip level or slice data");
        return -1;
    }
    ++v->uploads;++s->volume_uploads;return 0;
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
/* Cube sampling for the null backend: point-sample level 0 from the core's
 * CPU face store.  Address modes follow D3DTADDRESS_WRAP/MIRROR/CLAMP. */
static int cube_coord(int c,unsigned edge,unsigned address){
    if(address==1)return c&(int)(edge-1u); /* WRAP; edge is a power of two */
    if(address==3)return c<0?0:c>=(int)edge?(int)edge-1:c; /* CLAMP */
    int period=(int)edge*2;c%=period;if(c<0)c+=period; /* MIRROR */
    return c>=(int)edge?period-1-c:c;
}
static int null_cube_texel(const D9CubeView *cube,unsigned face,float u,float v,
                           unsigned address_u,unsigned address_v,uint32_t *out){
    if(!cube||!cube->pixels||face>=6u||!cube->edge||!cube->levels||!out)return -1;
    int x=cube_coord((int)floorf(u*(float)cube->edge),(unsigned)cube->edge,address_u);
    int y=cube_coord((int)floorf(v*(float)cube->edge),(unsigned)cube->edge,address_v);
    const uint8_t *base=cube->pixels+(size_t)face*cube->face_bytes;
    if(cube->format==D9_A8R8G8B8||cube->format==D9_X8R8G8B8){
        const uint8_t *p=base+(size_t)y*cube->level_pitch[0]+(size_t)x*4u;
        *out=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|
             ((uint32_t)(cube->format==D9_X8R8G8B8?255u:p[3])<<24);
        return 0;
    }
    unsigned block=cube->format==D9_DXT1?8u:
                   (cube->format==D9_DXT3||cube->format==D9_DXT5)?16u:0u;
    if(!block)return -1;
    const uint8_t *bp=base+(size_t)(y/4)*cube->level_pitch[0]+(size_t)(x/4)*block;
    uint8_t rgba[64];
    if(d9_bc_decode_block(cube->format,bp,block,rgba))return -1;
    *out=(uint32_t)rgba[0]|((uint32_t)rgba[1]<<8)|((uint32_t)rgba[2]<<16)|
         ((uint32_t)rgba[3]<<24);
    return 0;
}
static int null_draw_cube(void *ctx,const D9ColorVertex *v,size_t n,
                          const unsigned *faces,const D9Matrix *clip,
                          const D9CubeView *cube,const D9DrawState *st){
    D9NullStats *s=stats(ctx);
    if(!s->scene){null_log("DrawPrimitive","no scene is active");return -1;}
    if(!v||!n||n%3||!faces||!cube||!clip||!st){
        null_log("DrawPrimitive","cube draw is missing vertices, faces, view, clip or state");return -1;
    }
    if(!cube->pixels||cube->levels<1||cube->edge<8||cube->edge>1024){
        null_log("DrawPrimitive","cube view has no CPU pixels or an invalid edge");return -1;
    }
    if(cube->format!=D9_A8R8G8B8&&cube->format!=D9_X8R8G8B8&&
       cube->format!=D9_DXT1&&cube->format!=D9_DXT3&&cube->format!=D9_DXT5){
        null_log("DrawPrimitive","null cube sampling supports A8/X8/DXT1/3/5 level 0");return -1;
    }
    uint32_t samples[3]={0};unsigned last_faces[3]={0};
    for(size_t i=0;i<n;++i){
        if(faces[i]>=6u){null_log("DrawPrimitive","cube face index is outside 0..5");return -1;}
        uint32_t texel=0;
        if(null_cube_texel(cube,faces[i],v[i].u,v[i].v,st->address_u,st->address_v,&texel)){
            null_log("DrawPrimitive","cube face/texel sample failed");return -1;
        }
        if(i<3){samples[i]=texel;last_faces[i]=faces[i];}
    }
    if(null_draw_colored(ctx,v,n,clip,NULL,st))return -1;
    ++s->cube_draws;s->cube_vertices+=n;
    s->last_cube_count=n<3?n:3;
    for(unsigned i=0;i<s->last_cube_count;++i){
        s->last_cube_faces[i]=last_faces[i];
        s->last_cube_samples[i]=samples[i];
    }
    for(size_t i=0;i<n;++i)++s->cube_face_counts[faces[i]];
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
    t->pixels=calloc((size_t)w*h,4);if(!t->pixels){free(t);null_log("CreateRenderTarget","out of memory");return -1;}
    t->w=w;t->h=h;t->format=format;*native=t;++s->target_creates;return 0;
}
static void null_target_delete(void *ctx,void *native){
    D9NullStats *s=stats(ctx);NullTarget *t=native;if(!t)return;free(t->pixels);free(t);++s->target_deletes;
}
static int null_target_bind(void *ctx,void *native){
    D9NullStats *s=stats(ctx);if(!s->scene){null_log("SetRenderTarget","target bind requires an active scene");return -1;}
    s->target_active=native!=NULL;++s->target_binds;return 0;
}
static int null_target_clear(void *ctx,void *native,uint32_t argb){
    D9NullStats *s=stats(ctx);NullTarget *t=native;if(!s->scene||!t)return -1;
    for(size_t i=0;i<(size_t)t->w*t->h;++i)((uint32_t *)t->pixels)[i]=argb;
    s->last_clear_argb=argb;++s->target_clears;return 0;
}
static int null_target_read(void *ctx,void *native,void *dst,size_t pitch,
                            unsigned x,unsigned y,unsigned w,unsigned h){
    D9NullStats *s=stats(ctx);NullTarget *t=native;
    if(!t||!dst||!pitch||!w||!h||x>t->w||y>t->h||w>t->w-x||h>t->h-y||pitch<(size_t)w*4)return -1;
    for(unsigned yy=0;yy<h;++yy)
        memcpy((uint8_t *)dst+(size_t)yy*pitch,t->pixels+((size_t)(y+yy)*t->w+x)*4,(size_t)w*4);
    ++s->target_reads;return 0;
}
static int null_target_write(void *ctx,void *native,const void *bgra,size_t pitch,
                             unsigned w,unsigned h){
    D9NullStats *s=stats(ctx);NullTarget *t=native;
    if(!t||!bgra||w!=t->w||h!=t->h||pitch<(size_t)w*4||s->scene)return -1;
    for(unsigned yy=0;yy<h;++yy)
        memcpy(t->pixels+(size_t)yy*w*4,(const uint8_t *)bgra+(size_t)yy*pitch,(size_t)w*4);
    ++s->target_writes;return 0;
}
typedef struct {uint8_t *pixels;unsigned lw,lh,sw,sh;} BlitView;

static int blit_view(const D9BlitSurface *surface,BlitView *view){
    if(surface->native){
        NullTarget *t=surface->native;
        if(!t||!t->pixels||!surface->width||!surface->height)return D9_BLIT_ARGUMENT;
        view->pixels=t->pixels;
        view->lw=surface->width;
        view->lh=surface->height;
        view->sw=surface->storage_width?surface->storage_width:t->w;
        view->sh=surface->storage_height?surface->storage_height:t->h;
        if(!view->sw||!view->sh||view->sw>t->w||view->sh>t->h)return D9_BLIT_UNSUPPORTED;
    }else{
        view->pixels=g_present;
        view->lw=surface->width;
        view->lh=surface->height;
        view->sw=view->lw;
        view->sh=view->lh;
    }
    return D9_BLIT_OK;
}
static unsigned native_start(unsigned logical_pos,unsigned logical,unsigned native){
    if(!logical||!native)return 0;
    if(native>=logical)return logical_pos;
    return (unsigned)((uint64_t)logical_pos*native/logical);
}
static unsigned native_end(unsigned logical_end,unsigned logical,unsigned native){
    if(!logical||!native)return 0;
    if(native>=logical)return logical_end;
    uint64_t v=(uint64_t)logical_end*native;
    return (unsigned)((v+logical-1)/logical);
}
static const uint8_t *blit_texel(const BlitView *view,unsigned x,unsigned y){
    if(x>=view->sw)x=view->sw-1;
    if(y>=view->sh)y=view->sh-1;
    return view->pixels+((size_t)y*view->sw+x)*4;
}
static void blit_nearest(const BlitView *src,float x,float y,uint8_t out[4]){
    int ix=(int)floorf(x),iy=(int)floorf(y);
    if(ix<0)ix=0;if(iy<0)iy=0;
    if(ix>=(int)src->sw)ix=(int)src->sw-1;
    if(iy>=(int)src->sh)iy=(int)src->sh-1;
    memcpy(out,blit_texel(src,(unsigned)ix,(unsigned)iy),4);
}
static void blit_bilinear(const BlitView *src,float x,float y,uint8_t out[4]){
    float fx=x-0.5f,fy=y-0.5f;
    int x0=(int)floorf(fx),y0=(int)floorf(fy);
    float tx=fx-(float)x0,ty=fy-(float)y0;
    int x1=x0+1,y1=y0+1;
    if(x0<0)x0=0;if(y0<0)y0=0;
    if(x1>=(int)src->sw)x1=(int)src->sw-1;
    if(y1>=(int)src->sh)y1=(int)src->sh-1;
    if(x0>=(int)src->sw)x0=(int)src->sw-1;
    if(y0>=(int)src->sh)y0=(int)src->sh-1;
    const uint8_t *p00=blit_texel(src,(unsigned)x0,(unsigned)y0);
    const uint8_t *p10=blit_texel(src,(unsigned)x1,(unsigned)y0);
    const uint8_t *p01=blit_texel(src,(unsigned)x0,(unsigned)y1);
    const uint8_t *p11=blit_texel(src,(unsigned)x1,(unsigned)y1);
    for(unsigned c=0;c<4;++c){
        float top=(float)p00[c]*(1.0f-tx)+(float)p10[c]*tx;
        float bot=(float)p01[c]*(1.0f-tx)+(float)p11[c]*tx;
        float v=top*(1.0f-ty)+bot*ty;
        if(v<0.0f)v=0.0f;if(v>255.0f)v=255.0f;
        out[c]=(uint8_t)(v+0.5f);
    }
}
/* The null backend validates, counts and really copies a StretchRect.  Logical
 * rectangles are mapped through the native storage of bounded targets, so a
 * 1024-logical/256-native target samples the same full-surface image that the
 * viewport path renders.  The implicit destination is a private 400x240 BGRA
 * image, which is what the host pixel tests read back through
 * d9_null_present_pixels(). */
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
    BlitView sv,dv;
    int rc=blit_view(&src->surface,&sv);if(rc)return rc;
    rc=blit_view(&dst->surface,&dv);if(rc)return rc;
    if(src->x>sv.lw||src->y>sv.lh||src->width>sv.lw-src->x||src->height>sv.lh-src->y||
       dst->x>dv.lw||dst->y>dv.lh||dst->width>dv.lw-dst->x||dst->height>dv.lh-dst->y)
        return D9_BLIT_RECT;
    unsigned dx0=native_start(dst->x,dv.lw,dv.sw);
    unsigned dx1=native_end(dst->x+dst->width,dv.lw,dv.sw);
    unsigned dy0=native_start(dst->y,dv.lh,dv.sh);
    unsigned dy1=native_end(dst->y+dst->height,dv.lh,dv.sh);
    if(dx1>dv.sw)dx1=dv.sw;
    if(dy1>dv.sh)dy1=dv.sh;
    if(dx0>=dx1||dy0>=dy1)return D9_BLIT_RECT;
    float dlx=dv.sw<dv.lw?(float)dv.lw/(float)dv.sw:1.0f;
    float dly=dv.sh<dv.lh?(float)dv.lh/(float)dv.sh:1.0f;
    float slx=sv.sw<sv.lw?(float)sv.lw/(float)sv.sw:1.0f;
    float sly=sv.sh<sv.lh?(float)sv.lh/(float)sv.sh:1.0f;
    for(unsigned ny=dy0;ny<dy1;++ny){
        float logical_y=((float)ny+0.5f)*dly;
        float vrel=(logical_y-(float)dst->y)/(float)dst->height;
        if(vrel<0.0f)vrel=0.0f;if(vrel>1.0f)vrel=1.0f;
        float src_y=((float)src->y+vrel*(float)src->height)/sly;
        for(unsigned nx=dx0;nx<dx1;++nx){
            float logical_x=((float)nx+0.5f)*dlx;
            float urel=(logical_x-(float)dst->x)/(float)dst->width;
            if(urel<0.0f)urel=0.0f;if(urel>1.0f)urel=1.0f;
            float src_x=((float)src->x+urel*(float)src->width)/slx;
            uint8_t out[4];
            if(filter==D9_FILTER_LINEAR)blit_bilinear(&sv,src_x,src_y,out);
            else blit_nearest(&sv,src_x,src_y,out);
            memcpy(dv.pixels+((size_t)ny*dv.sw+nx)*4,out,4);
        }
    }
    ++s->blits;s->blit_pixels+=(unsigned long long)dst->width*dst->height;
    if(dst->width!=src->width||dst->height!=src->height)++s->blit_stretched;
    if(filter==D9_FILTER_LINEAR)++s->blit_linear;
    s->last_blit_filter=filter;
    s->last_blit_src_back=src->surface.native==NULL;
    s->last_blit_dst_back=dst->surface.native==NULL;
    if(!dst->surface.native){s->present_valid=1;s->present_w=dv.lw;s->present_h=dv.lh;}
    return D9_BLIT_OK;
}
static int null_depth_clear(void *ctx,unsigned flags,uint32_t depth_stencil){
    D9NullStats *s=stats(ctx);
    if(!s->scene||!(flags&6u)||(flags&~7u)){null_log("Clear","depth clear requires an active scene and Z/STENCIL flags");return -1;}
    ++s->depth_clears;s->last_depth_flags=flags;s->last_depth_clear=depth_stencil;
    return 0;
}
static int null_target_viewport(void *ctx,unsigned w,unsigned h){
    D9NullStats *s=stats(ctx);
    if(!s->scene||!w||!h||w>4096||h>4096){null_log("SetViewport","target viewport requires an active scene and plausible dimensions");return -1;}
    s->viewport_w=w;s->viewport_h=h;++s->viewport_calls;return 0;
}
static int null_target_viewport_scaled(void *ctx,unsigned logical_w,unsigned logical_h,
                                       unsigned native_w,unsigned native_h){
    if(!logical_w||!logical_h||!native_w||!native_h||native_w>logical_w||native_h>logical_h)
        return -1;
    return null_target_viewport(ctx,native_w,native_h);
}
static int null_draw_two(void *ctx,const D9ColorVertex *vertices,size_t count,
                         const D9Matrix *clip,void *tex0,const D9DrawState *stage0,
                         void *tex1,const D9DrawState *stage1){
    if(!tex0||!tex1||!stage1||stage1->stage_texcoordindex||
       !stage_op_ok(stage1->stage_colorop)||!stage_op_ok(stage1->stage_alphaop))
        return -1;
    return null_draw_colored(ctx,vertices,count,clip,tex0,stage0);
}
static void *null_target_texture(void *ctx,void *native){
    (void)ctx;return native;
}
static int null_draw_menu(void *ctx,const D9ColorVertex *vertices,size_t count,
                          const D9Matrix *clip,void *tex0,void *tex1,
                          const D9DrawState *state,const float (*constants)[4]){
    D9NullStats *s=stats(ctx);
    if(!s->scene||!vertices||!count||count%3||!clip||!state||!state->pixel_shader)
        return -1;
    for(size_t i=0;i<count;++i){
        const D9ColorVertex *v=&vertices[i];
        if(!isfinite(v->x)||!isfinite(v->y)||!isfinite(v->z)||
           !isfinite(v->u)||!isfinite(v->v)||!isfinite(v->r)||
           !isfinite(v->g)||!isfinite(v->b)||!isfinite(v->a)) return -1;
    }
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        if(!isfinite(clip->m[i][j]))return -1;
    if(state->pixel_shader->textures>=1&&!tex0)return -1;
    if(state->pixel_shader->textures>=2&&!tex1)return -1;
    if(!constants)return -1;
    memcpy(s->last_menu_constants,constants,sizeof s->last_menu_constants);
    s->menu_textures=state->pixel_shader->textures;
    ++s->menu_draws;++s->draws;++s->color_draws;
    s->triangles+=(unsigned)(count/3);s->vertices+=count;s->color_vertices+=(unsigned)count;
    return 0;
}
static const D9Backend backend={null_texture_create,null_texture_upload,null_texture_delete,null_begin,null_draw,null_end,
    NULL,NULL,NULL,null_target_create,null_target_delete,null_target_bind,null_target_clear,null_target_read,null_target_blit,null_draw_colored,null_draw_two,
    null_target_write,null_depth_clear,null_target_viewport,null_draw_menu,null_target_texture,
    null_cube_create,null_cube_upload,null_target_viewport_scaled,
    null_volume_create,null_volume_upload,null_draw_cube};
const D9Backend *d9_null_backend(void){return &backend;}
void *d9_null_context(void){return &g;}
const D9NullStats *d9_null_stats(void){return &g;}
const void *d9_null_present_pixels(unsigned *w,unsigned *h){
    if(w)*w=g.present_valid?g.present_w:0;
    if(h)*h=g.present_valid?g.present_h:0;
    return g.present_valid?g_present:NULL;
}
void d9_null_note_clear(unsigned flags){++g.clears;g.last_clear_flags=flags;if(flags&1u)++g.clear_targets;}
void d9_null_reset(void){memset(&g,0,sizeof g);memset(g_present,0,sizeof g_present);}
