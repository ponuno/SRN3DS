#include "d3d9_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {unsigned w,h,format;uint32_t *tiled;} SwTarget;
typedef struct {
    SwTarget *active;int scene,fail_create;
    unsigned creates,deletes,binds,clears,reads;
} SwCtx;

static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;(void)levels;(void)native;return -1;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return -1;
}
static void tex_delete(void *ctx,void *native){(void)ctx;(void)native;}
static int begin(void *ctx,uint32_t argb){SwCtx *s=ctx;(void)argb;if(s->scene)return -1;s->scene=1;s->active=NULL;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *st){
    (void)ctx;(void)v;(void)n;(void)m;(void)t;(void)st;return 0;
}
static void end(void *ctx){SwCtx *s=ctx;s->scene=0;s->active=NULL;}
static int target_create(void *ctx,unsigned w,unsigned h,unsigned format,void **native){
    SwCtx *s=ctx;if(s->fail_create)return -1;
    SwTarget *t=calloc(1,sizeof *t);if(!t)return -1;
    t->w=w;t->h=h;t->format=format;t->tiled=calloc((size_t)w*h,4);if(!t->tiled){free(t);return -1;}
    *native=t;++s->creates;return 0;
}
static void target_delete(void *ctx,void *native){
    SwCtx *s=ctx;SwTarget *t=native;if(!t)return;
    if(s->active==t)s->active=NULL;free(t->tiled);free(t);++s->deletes;
}
static int target_bind(void *ctx,void *native){
    SwCtx *s=ctx;if(!s->scene)return -1;s->active=native;++s->binds;return 0;
}
static int target_clear(void *ctx,void *native,uint32_t argb){
    SwCtx *s=ctx;SwTarget *t=native;if(!s->scene||!t)return -1;
    uint32_t word=(argb<<8)|(argb>>24);
    for(size_t i=0;i<(size_t)t->w*t->h;++i)t->tiled[i]=word;
    ++s->clears;return 0;
}
static int target_read(void *ctx,void *native,void *dst,size_t pitch,
                       unsigned x,unsigned y,unsigned w,unsigned h){
    SwCtx *s=ctx;SwTarget *t=native;if(!t)return -1;++s->reads;
    return d9_unswizzle_rgba8(dst,pitch,t->tiled,t->w,t->h,t->format,x,y,w,h);
}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end,
    NULL,NULL,NULL,target_create,target_delete,target_bind,target_clear,target_read};

static void roundtrip(void){
    enum {W=16,H=16};
    uint32_t src[W*H],expected[W*H],tiled[W*H],out[W*H];
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        uint32_t p=0xff000000u|(x*13u)<<16|(y*7u)<<8|((x+y)*3u);
        src[y*W+x]=expected[y*W+x]=p;
    }
    assert(!d9_swizzle_rgba8(tiled,sizeof tiled,src,W*4,W,H,D9_A8R8G8B8));
    assert(!d9_unswizzle_rgba8(out,W*4,tiled,W,H,D9_A8R8G8B8,0,0,W,H));
    assert(!memcmp(out,expected,sizeof out));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)expected[y*W+x]=(expected[y*W+x]&0x00ffffffu)|0xff000000u;
    assert(!d9_swizzle_rgba8(tiled,sizeof tiled,src,W*4,W,H,D9_X8R8G8B8));
    assert(!d9_unswizzle_rgba8(out,W*4,tiled,W,H,D9_X8R8G8B8,0,0,W,H));
    assert(!memcmp(out,expected,sizeof out));
    memset(out,0x5a,sizeof out);
    assert(!d9_unswizzle_rgba8(out,W*4,tiled,W,H,D9_X8R8G8B8,3,4,5,6));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        if(x>=3&&x<8&&y>=4&&y<10)assert(out[y*W+x]==expected[y*W+x]);
        else assert(out[y*W+x]==0x5a5a5a5au);
    }
    assert(d9_unswizzle_rgba8(out,W*4,tiled,W,H,D9_A8R8G8B8,W,0,1,1)==D9_INVALID);
    assert(d9_unswizzle_rgba8(out,W*4,tiled,W,H,D9_A8R8G8B8,0,0,0,1)==D9_OK);
}

static void check_solid(const uint32_t *pixels,unsigned w,unsigned h,uint32_t argb){
    for(unsigned i=0;i<w*h;++i)assert(pixels[i]==argb);
}

int main(void){
    roundtrip();
    SwCtx sw={0};D9Device *d=d9_create_device(&ops,&sw,1u<<20);assert(d);
    D9Handle a=0,b=0,info=0;unsigned w=0,h=0,format=0,lockable=0;size_t bytes=0;
    assert(d9_create_render_target(NULL,8,8,D9_A8R8G8B8,1,&a)==D9_INVALID);
    assert(d9_create_render_target(d,7,8,D9_A8R8G8B8,1,&a)==D9_INVALID&&!a);
    assert(d9_create_render_target(d,8,12,D9_A8R8G8B8,1,&a)==D9_INVALID&&!a);
    assert(d9_create_render_target(d,8,8,D9_DXT1,1,&a)==D9_INVALID&&!a);
    assert(d9_create_render_target(d,8,8,D9_A8R8G8B8,1,NULL)==D9_INVALID);
    assert(!d9_create_render_target(d,16,16,D9_A8R8G8B8,1,&a)&&a);
    assert(!d9_create_render_target(d,16,16,D9_X8R8G8B8,0,&b)&&b&&a!=b);
    assert(d9_memory_used(d)==2*16*16*4);
    assert(!d9_render_target_info(d,a,&w,&h,&format,&lockable,&bytes));
    assert(w==16&&h==16&&format==D9_A8R8G8B8&&lockable==1&&bytes==1024);
    assert(!d9_set_render_target(d,a)&&d9_get_render_target(d)==a);
    uint32_t pixels[16*16];
    assert(!d9_render_target_read(d,a,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0u); /* native storage is zero-initialised */
    assert(!d9_begin_scene(d,0xffff0000u));
    assert(d9_render_target_read(d,a,pixels,16*4,0,0,16,16)==D9_INVALID); /* no readback inside a scene */
    assert(!d9_present(d));
    assert(!d9_render_target_read(d,a,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0xffff0000u);
    assert(!d9_begin_scene_ex(d,0xff00ff00u,0)&&!d9_present(d));
    assert(!d9_render_target_read(d,a,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0xffff0000u); /* BeginScene without clear intent preserves A */
    assert(!d9_set_render_target(d,b)&&!d9_begin_scene(d,0xff00ff00u)&&!d9_present(d));
    assert(!d9_render_target_read(d,b,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0xff00ff00u);
    assert(!d9_render_target_read(d,a,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0xffff0000u); /* B did not alias A */
    assert(!d9_set_render_target(d,0)&&d9_get_render_target(d)==0);
    assert(!d9_begin_scene(d,0xff0000ffu)&&!d9_present(d));
    assert(!d9_render_target_read(d,a,pixels,16*4,0,0,16,16));
    check_solid(pixels,16,16,0xffff0000u); /* back-buffer clear did not touch A */
    assert(d9_render_target_read(d,a,pixels,16*4,17,0,1,1)==D9_INVALID);
    assert(d9_set_render_target(d,999)==D9_INVALID);
    assert(!d9_begin_scene(d,0));
    assert(d9_set_render_target(d,b)==D9_OK); /* software bind succeeds mid-scene */
    assert(d9_get_render_target(d)==b);
    assert(d9_release_render_target(d,a)==D9_INVALID);
    assert(!d9_present(d));
    assert(!d9_release_render_target(d,a)&&d9_memory_used(d)==16*16*4);
    assert(!d9_release_render_target(d,b)&&d9_memory_used(d)==0);
    unsigned binds_before=sw.binds;
    assert(!d9_begin_scene(d,0xff0000ffu)&&!d9_present(d));
    assert(sw.binds==binds_before+1&&sw.active==NULL); /* selected-target release rebinds back buffer */
    assert(sw.creates==2&&sw.deletes==2&&sw.clears==2&&sw.reads==6);
    d9_destroy_device(d);

    D9Device *small=d9_create_device(&ops,&sw,1024);assert(small);
    assert(!d9_create_render_target(small,16,16,D9_A8R8G8B8,1,&a));
    assert(d9_create_render_target(small,16,16,D9_A8R8G8B8,1,&b)==D9_MEMORY&&!b);
    assert(!d9_release_render_target(small,a));d9_destroy_device(small);

    D9Device *slots=d9_create_device(&ops,&sw,4096);assert(slots);
    D9Handle hs[4]={0};
    for(unsigned i=0;i<4;++i)assert(!d9_create_render_target(slots,8,8,D9_A8R8G8B8,1,&hs[i]));
    assert(d9_create_render_target(slots,8,8,D9_A8R8G8B8,1,&info)==D9_MEMORY&&!info);
    for(unsigned i=0;i<4;++i)assert(!d9_release_render_target(slots,hs[i]));
    d9_destroy_device(slots);

    sw.fail_create=1;d=d9_create_device(&ops,&sw,4096);assert(d);
    assert(d9_create_render_target(d,8,8,D9_A8R8G8B8,1,&a)==D9_BACKEND&&!a&&d9_memory_used(d)==0);
    sw.fail_create=0;assert(!d9_create_render_target(d,8,8,D9_A8R8G8B8,1,&a));
    d9_destroy_device(d); /* live target is freed by device teardown */
    assert(sw.creates==sw.deletes);

    const D9Backend old={tex_create,tex_upload,tex_delete,begin,draw,end};
    d=d9_create_device(&old,&sw,4096);assert(d);
    assert(d9_create_render_target(d,8,8,D9_A8R8G8B8,1,&a)==D9_INVALID&&!a);
    d9_destroy_device(d);

    D9Backend no_read=ops;no_read.target_read=NULL;
    d=d9_create_device(&no_read,&sw,4096);assert(d);
    assert(d9_create_render_target(d,8,8,D9_A8R8G8B8,1,&a)==D9_INVALID&&!a);
    assert(!d9_create_render_target(d,8,8,D9_A8R8G8B8,0,&a));
    assert(d9_render_target_read(d,a,pixels,8*4,0,0,8,8)==D9_BACKEND);
    assert(!d9_release_render_target(d,a));d9_destroy_device(d);
    puts("D3D9 render target: swizzle round-trip, independent clears/readback, restore, budget, slots and lifetime passed");
    return 0;
}
