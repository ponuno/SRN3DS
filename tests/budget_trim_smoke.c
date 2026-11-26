/* Automatic texture-cache trim under device-budget pressure.
 *
 * A create that would exceed the device budget evicts the LRU rehydratable,
 * non-render-target cube/volume GPU copies until the request fits plus the
 * documented headroom.  Bound, locked, pinned, no_evict and render-target
 * entries are skipped.  If no candidate remains the create is a named
 * D3DERR_OUTOFVIDEOMEMORY and no native is allocated.  Rebinding an evicted
 * resource rehydrates it from its CPU store.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FMT_A8R8G8B8 21u
typedef struct { D9NullStats stats; } TestCtx;
typedef struct { float x,y,z,nx,ny,nz,u,v; } CubeVertex;

static void fill_cube(D9Device *d,D9Handle cube,uint32_t argb){
    uint32_t pixels[8*8];
    for(unsigned i=0;i<8*8;++i)pixels[i]=argb;
    for(unsigned face=0;face<6;++face)
        assert(d9_cube_upload_face_level(d,cube,face,0,pixels,32,sizeof pixels)==D9_OK);
}
static void fill_volume(D9Device *d,D9Handle volume){
    uint32_t pixels[4*4*4];
    for(unsigned i=0;i<4*4*4;++i)pixels[i]=0xff000000u|i;
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
static void draw_px(D9Device *d,const D9NullStats *s,uint32_t argb){
    CubeVertex v[3]={
        {-1,-1,0,1,0,0,0.25f,0.75f},
        { 1,-1,0,1,0,0,0.25f,0.75f},
        { 0, 1,0,1,0,0,0.25f,0.75f},
    };
    assert(!d9_begin_scene(d,0));
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0]));
    assert(!d9_end_scene(d));
    assert(s->last_cube_count==3);
    assert(s->last_cube_faces[0]==D9_CUBE_FACE_POSITIVE_X);
    assert(s->last_cube_samples[0]==argb);
}
static size_t cube_gpu(D9Device *d,D9Handle cube){
    size_t gpu=0;
    assert(d9_cube_resource_info(d,cube,NULL,NULL,NULL,NULL,&gpu,NULL)==D9_OK);
    return gpu;
}

int main(void){
    TestCtx ctx;memset(&ctx,0,sizeof ctx);
    D9Backend ops=*d9_null_backend();
    D9Device *d=d9_create_device(&ops,&ctx,11264);assert(d);

    D9Handle volume=0,A=0,B=0,C=0,D=0;
    assert(d9_create_volume_texture_ex(d,4,4,4,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&volume)==D9_OK);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&A)==D9_OK);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&B)==D9_OK);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&C)==D9_OK);
    fill_volume(d,volume);
    fill_cube(d,A,0xffff0000u);
    fill_cube(d,B,0xff00ff00u);
    fill_cube(d,C,0xff0000ffu);
    assert(d9_memory_used(d)==10496u);

    /* Touch A and B so the untouched C is the cube LRU; the volume was created
     * first and is the overall LRU. */
    assert(d9_bind_cube_texture(d,0,A)==D9_OK);
    assert(d9_bind_cube_texture(d,0,B)==D9_OK);
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);

    /* D does not fit: evict volume, then cube C, then cube A.  B stays. */
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&D)==D9_OK);
    assert(d9_memory_used(d)<=11264u);
    D9TexCacheStats st;assert(d9_texture_cache_stats(d,&st)==D9_OK);
    assert(st.budget_trims>=1);
    assert(st.budget_trim_evictions==3);
    assert(st.budget_trim_evicted_bytes>0);
    assert(st.budget_trim_failures==0);
    size_t vgpu=1;assert(d9_volume_resource_info(d,volume,NULL,NULL,NULL,NULL,NULL,NULL,&vgpu,NULL)==D9_OK);
    assert(vgpu==0);
    assert(cube_gpu(d,C)==0u);
    assert(cube_gpu(d,A)==0u);
    assert(cube_gpu(d,B)>0u);

    /* Rebinding the evicted C and volume rehydrates from the CPU stores. */
    assert(d9_release_cube_texture(d,D)==D9_OK);
    assert(d9_bind_cube_texture(d,0,C)==D9_OK);
    assert(cube_gpu(d,C)>0u);
    setup_decl(d);
    assert(d9_memory_used(d)<=11264u);
    draw_px(d,&ctx.stats,0xff0000ffu);
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);
    assert(d9_bind_volume_texture(d,0,volume)==D9_OK);
    assert(d9_volume_resource_info(d,volume,NULL,NULL,NULL,NULL,NULL,NULL,&vgpu,NULL)==D9_OK);
    assert(vgpu>0);
    assert(d9_texture_cache_stats(d,&st)==D9_OK&&st.rehydrations>=2);

    assert(d9_release_cube_texture(d,A)==D9_OK);
    assert(d9_release_cube_texture(d,B)==D9_OK);
    assert(d9_release_cube_texture(d,C)==D9_OK);
    assert(d9_release_volume_texture(d,volume)==D9_OK);
    assert(d9_memory_used(d)==0);
    assert(d9_destroy_device_ex(d)==D9_OK);

    /* Bound and locked resources are not candidates.  The failed create leaves
     * no partial native. */
    memset(&ctx,0,sizeof ctx);
    d=d9_create_device(&ops,&ctx,11264);assert(d);
    D9Handle rt=0;assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,
                                                   D9_POOL_DEFAULT,D9_USAGE_RENDERTARGET,&rt)==D9_OK);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&A)==D9_OK);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&B)==D9_OK);
    fill_cube(d,A,0xffff0000u);fill_cube(d,B,0xff00ff00u);
    assert(d9_memory_used(d)==9216u);
    assert(d9_bind_cube_texture(d,0,A)==D9_OK); /* bound */
    void *pixels=NULL;unsigned w=0,h=0;size_t pitch=0,bytes=0;
    assert(d9_cube_face_level_view(d,B,0,0,&pixels,&w,&h,&pitch,&bytes)==D9_OK); /* locked */
    d9_cube_face_view_locked(d,B,1);
    unsigned creates=ctx.stats.cube_creates;
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&C)==D9_MEMORY);
    assert(C==0);
    assert(ctx.stats.cube_creates==creates);
    assert(strstr(d9_last_draw_diagnostic(d),"budget trim")!=NULL);
    assert(d9_texture_cache_stats(d,&st)==D9_OK&&st.budget_trim_failures>=1);
    assert(cube_gpu(d,A)>0u&&cube_gpu(d,B)>0u);
    size_t rtgpu=0;assert(d9_cube_resource_info(d,rt,NULL,NULL,NULL,NULL,&rtgpu,NULL)==D9_OK);
    assert(rtgpu>0u);

    /* Release the guard and retry: B is the LRU candidate and is evicted. */
    assert(d9_bind_cube_texture(d,0,0)==D9_OK);
    d9_cube_face_view_locked(d,B,0);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&C)==D9_OK);
    assert(ctx.stats.cube_creates==creates+1);
    assert(cube_gpu(d,B)==0u);
    assert(cube_gpu(d,A)>0u);

    assert(d9_release_cube_texture(d,A)==D9_OK);
    assert(d9_release_cube_texture(d,B)==D9_OK);
    assert(d9_release_cube_texture(d,C)==D9_OK);
    assert(d9_release_cube_texture(d,rt)==D9_OK);
    assert(d9_memory_used(d)==0);
    assert(d9_destroy_device_ex(d)==D9_OK);
    puts("budget trim: LRU eviction, bound/locked/RT skip, named OOM, rebind rehydrate");
    return 0;
}
