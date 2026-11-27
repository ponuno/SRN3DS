/* Cube/volume texcache rehydration.
 *
 * Non-render-target cube/volume entries are evictable and carry a rehydrate
 * token.  After a trim, binding or locking the resource recreates its native
 * from the CPU store and reloads every face/level or slice.  Render-target
 * cubes have no CPU face copy and stay explicit-release-only.  A failed
 * rehydrate leaves the entry evicted with no partial native.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FMT_A8R8G8B8 21u

static const uint32_t face_argb[6] = {
    0xffff0000u, /* +X */
    0xff00ff00u, /* -X */
    0xff0000ffu, /* +Y */
    0xffffff00u, /* -Y */
    0xffff00ffu, /* +Z */
    0xff00ffffu, /* -Z */
};
typedef struct { float x,y,z,nx,ny,nz,u,v; } CubeVertex;

/* D9NullStats must stay first: the null backend casts ctx to it. */
typedef struct {
    D9NullStats stats;
    unsigned fail_cube_create;
    unsigned fail_volume_create;
    unsigned cube_create_calls;
    unsigned volume_create_calls;
} TestCtx;

static int test_cube_create(void *ctx,unsigned edge,unsigned levels,
                            unsigned format,void **native){
    TestCtx *t=ctx;++t->cube_create_calls;
    if(t->fail_cube_create)return -3;
    return d9_null_backend()->texture_create_cube(ctx,edge,levels,format,native);
}
static int test_volume_create(void *ctx,unsigned w,unsigned h,unsigned depth,
                              unsigned levels,void **native){
    TestCtx *t=ctx;++t->volume_create_calls;
    if(t->fail_volume_create)return -3;
    return d9_null_backend()->texture_create_volume(ctx,w,h,depth,levels,native);
}

static void fill_cube(D9Device *d,D9Handle cube){
    for(unsigned face=0;face<6;++face)
        for(unsigned level=0;level<2;++level){
            unsigned edge=8u>>level;
            uint32_t pixels[8*8];
            for(unsigned i=0;i<edge*edge;++i)pixels[i]=face_argb[face];
            assert(d9_cube_upload_face_level(d,cube,face,level,pixels,
                                             edge*4u,sizeof(uint32_t)*edge*edge)==D9_OK);
        }
}
static void fill_volume(D9Device *d,D9Handle volume){
    uint32_t pixels[4*4*4];
    for(unsigned z=0;z<4;++z)
        for(unsigned y=0;y<4;++y)
            for(unsigned x=0;x<4;++x)
                pixels[(z*4+y)*4+x]=0xff000000u|(z<<16)|(y<<8)|x;
    assert(d9_volume_upload_level(d,volume,0,pixels,16,64,sizeof pixels)==D9_OK);
}
static void setup_decl(D9Device *d){
    D9VertexElement e[3]={
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT3,0,D9_USAGE_NORMAL,0},
        {0,24,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl,e,3));
    assert(!d9_set_vertex_declaration(d,&decl));
}
static void draw_face(D9Device *d,unsigned face,const D9NullStats *s){
    static const float axes[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    CubeVertex v[3]={
        {-1,-1,0,axes[face][0],axes[face][1],axes[face][2],0.25f,0.75f},
        { 1,-1,0,axes[face][0],axes[face][1],axes[face][2],0.25f,0.75f},
        { 0, 1,0,axes[face][0],axes[face][1],axes[face][2],0.25f,0.75f},
    };
    assert(!d9_begin_scene(d,0));
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0]));
    assert(!d9_end_scene(d));
    assert(s->last_cube_count==3);
    for(unsigned i=0;i<3;++i){
        assert(s->last_cube_faces[i]==face);
        assert(s->last_cube_samples[i]==face_argb[face]);
    }
}
static void sample_all_faces(D9Device *d,D9Handle cube,const D9NullStats *s){
    int rc=d9_bind_cube_texture(d,0,cube);
    if(rc)fprintf(stderr,"set cube rc=%d: %s\n",rc,d9_last_draw_diagnostic(d));
    assert(!rc);
    for(unsigned face=0;face<6;++face)draw_face(d,face,s);
    assert(!d9_bind_cube_texture(d,0,0));
}

int main(void){
    TestCtx ctx;memset(&ctx,0,sizeof ctx);
    D9Backend ops=*d9_null_backend();
    ops.texture_create_cube=test_cube_create;
    ops.texture_create_volume=test_volume_create;
    D9Device *d=d9_create_device(&ops,&ctx,1u<<20);assert(d);

    D9Handle cube=0,volume=0,rt=0;
    assert(d9_create_cube_texture_ex(d,8,2,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_OK);
    assert(d9_create_volume_texture_ex(d,4,4,4,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&volume)==D9_OK);
    fill_cube(d,cube);
    fill_volume(d,volume);
    setup_decl(d);
    sample_all_faces(d,cube,&ctx.stats);

    D9TexCacheStats before;assert(d9_texture_cache_stats(d,&before)==D9_OK);
    assert(before.gpus==2&&before.rehydratable>=2&&before.non_evictable==0);

    /* Evict both GPU copies; their CPU stores and tokens survive.  A submitted
     * draw pins both natives until the backend frame barrier, so simulate that
     * barrier first: this test is about rehydration, not pin lifetime. */
    assert(d9_texture_cache_unpin_all(d)==D9_OK);
    unsigned evicted=0;
    assert(d9_texture_cache_trim(d,SIZE_MAX,0,&evicted)==D9_OK&&evicted>=2);
    D9TexCacheStats stats;assert(d9_texture_cache_stats(d,&stats)==D9_OK);
    assert(stats.evictions_gpu>=2&&stats.gpus==0);
    size_t guest=0,gpu=1;
    assert(d9_cube_resource_info(d,cube,NULL,NULL,NULL,&guest,&gpu,NULL)==D9_OK);
    assert(guest>0&&gpu==0);
    assert(d9_volume_resource_info(d,volume,NULL,NULL,NULL,NULL,NULL,&guest,&gpu,NULL)==D9_OK);
    assert(guest>0&&gpu==0);

    /* Bind rehydrates the cube and the sampled colours are reproduced. */
    unsigned rehydrated=stats.rehydrations;
    sample_all_faces(d,cube,&ctx.stats);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrations==rehydrated+1);
    assert(d9_cube_resource_info(d,cube,NULL,NULL,NULL,NULL,&gpu,NULL)==D9_OK&&gpu>0);

    /* Bind rehydrates the volume and its CPU contents are unchanged. */
    unsigned volume_uploads=ctx.stats.volume_uploads;
    assert(d9_bind_volume_texture(d,0,volume)==D9_OK);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrations==rehydrated+2);
    assert(ctx.stats.volume_uploads==volume_uploads+1);
    void *slice=NULL;unsigned sw=0,sh=0,sd=0;size_t spitch=0,splane=0,sbytes=0;
    assert(d9_volume_level_view(d,volume,0,&slice,&sw,&sh,&sd,&spitch,&splane,&sbytes)==D9_OK);
    assert(sw==4&&sh==4&&sd==4&&((const uint32_t *)slice)[0]==0xff000000u);
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);

    /* Evict again and inject a rehydrate failure.  The entry must stay
       evicted and no partial native may remain. */
    assert(d9_texture_cache_unpin_all(d)==D9_OK);
    assert(d9_texture_cache_trim(d,SIZE_MAX,0,&evicted)==D9_OK&&evicted>=2);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK);
    unsigned failures=stats.rehydrate_failures;
    unsigned creates=ctx.stats.cube_creates;
    ctx.fail_cube_create=1;
    int rc=d9_bind_cube_texture(d,0,cube);
    assert(rc!=D9_OK);
    assert(strstr(d9_last_draw_diagnostic(d),"cube rehydration")!=NULL);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrate_failures==failures+1);
    assert(ctx.stats.cube_creates==creates); /* no native was created */
    assert(d9_cube_resource_info(d,cube,NULL,NULL,NULL,NULL,&gpu,NULL)==D9_OK&&gpu==0);
    ctx.fail_cube_create=0;
    unsigned cube_rehydrated=stats.rehydrations;
    assert(d9_bind_cube_texture(d,0,cube)==D9_OK);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrations==cube_rehydrated+1);
    draw_face(d,D9_CUBE_FACE_POSITIVE_X,&ctx.stats);
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);

    /* Same failure contract for volume. */
    assert(d9_texture_cache_unpin_all(d)==D9_OK);
    assert(d9_texture_cache_trim(d,SIZE_MAX,0,&evicted)==D9_OK&&evicted>=1);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK);
    failures=stats.rehydrate_failures;
    ctx.fail_volume_create=1;
    assert(d9_bind_volume_texture(d,0,volume)!=D9_OK);
    assert(strstr(d9_last_draw_diagnostic(d),"volume rehydration")!=NULL);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrate_failures==failures+1);
    assert(d9_volume_resource_info(d,volume,NULL,NULL,NULL,NULL,NULL,NULL,&gpu,NULL)==D9_OK&&gpu==0);
    ctx.fail_volume_create=0;
    unsigned volume_rehydrated=stats.rehydrations;
    assert(d9_bind_volume_texture(d,0,volume)==D9_OK);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.rehydrations==volume_rehydrated+1);
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);

    /* Render-target cube: no CPU face copy, so trim must not evict it. */
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_DEFAULT,
                                     D9_USAGE_RENDERTARGET,&rt)==D9_OK);
    assert(d9_texture_cache_stats(d,&stats)==D9_OK&&stats.non_evictable>=1);
    unsigned evictions=stats.evictions_gpu;
    assert(d9_texture_cache_unpin_all(d)==D9_OK);
    int trc=d9_texture_cache_trim(d,SIZE_MAX,0,&evicted);
    assert(trc==D9_OK||trc==D9_UNSUPPORTED); /* RT cube keeps the target unreachable */
    D9TexCacheStats after;assert(d9_texture_cache_stats(d,&after)==D9_OK);
    assert(after.evictions_gpu>=evictions); /* may evict other entries */
    assert(d9_cube_resource_info(d,rt,NULL,NULL,NULL,NULL,&gpu,NULL)==D9_OK&&gpu>0);
    assert(after.non_evictable>=1);

    assert(d9_release_cube_texture(d,cube)==D9_OK);
    assert(d9_release_volume_texture(d,volume)==D9_OK);
    assert(d9_release_cube_texture(d,rt)==D9_OK);
    assert(d9_memory_used(d)==0);
    assert(d9_destroy_device_ex(d)==D9_OK);
    puts("cube/volume rehydration: pixels reproduced, partial native refused, RT cube non-evictable");
    return 0;
}
