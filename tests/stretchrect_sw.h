#pragma once
#include "d3d9_core.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Shared host-side LDR backend for the StretchRect tests.  Targets are linear
 * top-first BGRA, so a readback word is exactly the D3D ARGB value written by
 * Clear and the expected copy results stay readable. */
typedef struct {unsigned w,h,format;uint32_t *pixels;} D9SwTarget;
typedef struct {
    D9SwTarget *active,*last_created;
    D9SwTarget back;
    int scene,bind_on_blit,fail_restore,reject_code;
    unsigned creates,deletes,binds,clears,reads,blits,restores;
    unsigned last_filter,last_dx,last_dy,last_dw,last_dh,last_sx,last_sy,last_sw,last_sh;
} D9SwCtx;

static int d9sw_tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;(void)levels;(void)native;return -1;
}
static int d9sw_tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return -1;
}
static void d9sw_tex_delete(void *ctx,void *native){(void)ctx;(void)native;}
static int d9sw_begin(void *ctx,uint32_t argb){D9SwCtx *s=ctx;(void)argb;if(s->scene)return -1;s->scene=1;s->active=NULL;return 0;}
static int d9sw_draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *st){
    (void)ctx;(void)v;(void)n;(void)m;(void)t;(void)st;return 0;
}
static void d9sw_end(void *ctx){D9SwCtx *s=ctx;s->scene=0;s->active=NULL;}
static int d9sw_target_create(void *ctx,unsigned w,unsigned h,unsigned format,void **native){
    D9SwCtx *s=ctx;D9SwTarget *t=calloc(1,sizeof *t);if(!t)return -1;
    t->w=w;t->h=h;t->format=format;t->pixels=calloc((size_t)w*h,4);
    if(!t->pixels){free(t);return -1;}*native=t;s->last_created=t;++s->creates;return 0;
}
static void d9sw_target_delete(void *ctx,void *native){
    D9SwCtx *s=ctx;D9SwTarget *t=native;if(!t)return;
    if(s->active==t)s->active=NULL;if(s->last_created==t)s->last_created=NULL;
    free(t->pixels);free(t);++s->deletes;
}
static int d9sw_target_bind(void *ctx,void *native){
    D9SwCtx *s=ctx;if(!s->scene)return -1;
    if(s->fail_restore)return -1;
    s->active=native;++s->binds;return 0;
}
static int d9sw_target_clear(void *ctx,void *native,uint32_t argb){
    D9SwCtx *s=ctx;D9SwTarget *t=native;if(!s->scene||!t)return -1;
    for(size_t i=0;i<(size_t)t->w*t->h;++i)t->pixels[i]=argb;
    ++s->clears;return 0;
}
static int d9sw_target_read(void *ctx,void *native,void *dst,size_t pitch,
                            unsigned x,unsigned y,unsigned w,unsigned h){
    D9SwCtx *s=ctx;D9SwTarget *t=native;if(!t||!dst)return -1;++s->reads;
    for(unsigned row=0;row<h;++row)
        memcpy((uint8_t*)dst+(size_t)row*pitch,t->pixels+(size_t)(y+row)*t->w+x,(size_t)w*4);
    return 0;
}
static int d9sw_target_blit(void *ctx,const D9BlitRegion *dst,const D9BlitRegion *src,unsigned filter){
    D9SwCtx *s=ctx;++s->blits;s->last_filter=filter;
    s->last_dx=dst->x;s->last_dy=dst->y;s->last_dw=dst->width;s->last_dh=dst->height;
    s->last_sx=src->x;s->last_sy=src->y;s->last_sw=src->width;s->last_sh=src->height;
    if(s->reject_code)return s->reject_code;
    D9SwTarget *dt=dst->surface.native?dst->surface.native:&s->back;
    D9SwTarget *st=src->surface.native?src->surface.native:&s->back;
    assert(dst->x+dst->width<=dt->w&&dst->y+dst->height<=dt->h);
    assert(src->x+src->width<=st->w&&src->y+src->height<=st->h);
    if(s->bind_on_blit)s->active=dt;
    for(unsigned y=0;y<dst->height;++y)for(unsigned x=0;x<dst->width;++x){
        unsigned sx=src->x+(unsigned)((uint64_t)x*src->width/dst->width);
        unsigned sy=src->y+(unsigned)((uint64_t)y*src->height/dst->height);
        dt->pixels[(size_t)(dst->y+y)*dt->w+dst->x+x]=st->pixels[(size_t)sy*st->w+sx];
    }
    return 0;
}
static const D9Backend d9sw_ops={d9sw_tex_create,d9sw_tex_upload,d9sw_tex_delete,d9sw_begin,d9sw_draw,d9sw_end,
    NULL,NULL,NULL,d9sw_target_create,d9sw_target_delete,d9sw_target_bind,d9sw_target_clear,d9sw_target_read,d9sw_target_blit};

static uint32_t d9sw_pattern(unsigned x,unsigned y){return 0xff000000u|(x<<16)|(y<<8)|((x^y)&0xffu);}
static void d9sw_fill(D9SwTarget *t){for(unsigned y=0;y<t->h;++y)for(unsigned x=0;x<t->w;++x)t->pixels[(size_t)y*t->w+x]=d9sw_pattern(x,y);}
static void d9sw_check_pixel(const uint32_t *p,unsigned pitch,unsigned x,unsigned y,uint32_t want){
    uint32_t got=p[(size_t)y*pitch+x];assert(got==want);
}
static int d9sw_start(D9SwCtx *s,unsigned back_w,unsigned back_h,unsigned format){
    memset(s,0,sizeof *s);
    size_t bytes=(size_t)back_w*back_h*4;
    uint32_t *pixels=calloc(1,bytes);if(!pixels)return -1;
    s->back=(D9SwTarget){back_w,back_h,format,pixels};return 0;
}
static void d9sw_stop(D9SwCtx *s){free(s->back.pixels);s->back.pixels=NULL;}
