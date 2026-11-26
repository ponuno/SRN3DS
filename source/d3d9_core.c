#include "d3d9_core.h"
#include "d3d9_bc.h"
#include "d3d9_texcache.h"
#include "d3d9_texture_data.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

enum {MAX_BUFFERS=32,MAX_TEXTURES=32,MAX_CUBES=8,MAX_VOLUMES=8,
      MAX_TEX_ENTRIES=MAX_TEXTURES+MAX_CUBES+MAX_VOLUMES,
      MAX_TARGETS=12,MAX_DEPTHS=4,MAX_DRAW_VERTICES=4096,MAX_LEVELS=11};
/* Bounded native storage for explicit LDR render targets.  The default is 256
 * so the measured TESV set fits the N3DS 6 MiB VRAM budget:
 *   5x1024 + 4x512 logical targets -> nine 256x256 targets
 *   = 9 * (256 KiB colour + 256 KiB depth) = 4.5 MiB.
 * The retained back buffer is created through the unbounded path. */
enum {D9_TARGET_NATIVE_MIN=64,D9_TARGET_NATIVE_DEFAULT=256,D9_TARGET_NATIVE_LIMIT=512};
static unsigned target_native_max_from_env(void){
    const char *s=getenv("TESV_D3D9_TARGET_NATIVE_MAX");
    if(!s||!*s)return D9_TARGET_NATIVE_DEFAULT;
    char *end=NULL;
    unsigned long v=strtoul(s,&end,10);
    if(!end||*end||v<D9_TARGET_NATIVE_MIN||v>D9_TARGET_NATIVE_LIMIT)
        return D9_TARGET_NATIVE_DEFAULT;
    unsigned p=D9_TARGET_NATIVE_MIN;
    while((p<<1)<=v&&(p<<1)<=D9_TARGET_NATIVE_LIMIT)p<<=1;
    return p;
}
static int scaled_readback_from_env(void){
    const char *s=getenv("TESV_D3D9_SCALED_READBACK");
    return s&&strcmp(s,"1")==0;
}
typedef struct {D9Handle id;unsigned kind;uint8_t *data;size_t bytes;int locked;} Buffer;
typedef struct {
    D9Handle id;size_t offset,stride;
} Stream;
typedef struct {
    D9Handle id;unsigned w,h,native_w,native_h,format,lockable;size_t bytes;void *native;
} Target;
typedef struct {D9Handle id;unsigned w,h,format;} Depth;
typedef struct {unsigned w,h;size_t pitch,bytes,offset;int uploaded;} Level;
typedef struct {unsigned w,h;size_t pitch,bytes,offset;int uploaded;unsigned face_mask;} CubeLevel;
typedef struct {
    D9Handle id;unsigned w,h,format,levels,gpu_levels,locked,pool,usage;
    int cpu_only,gpu_only,disposable,cache_pinned;
    size_t cost,cpu_bytes,gpu_bytes;uint8_t *pixels;void *native;Level level[MAX_LEVELS];
    /* D3DUSAGE_RENDERTARGET: nonzero target handle whose colour buffer is the
     * texture storage.  The Texture owns no separate native allocation. */
    D9Handle rt_target;
} Texture;
typedef struct {
    D9Handle id;unsigned edge,levels,format,pool,usage;
    int cpu_only;
    size_t cost,cpu_bytes,gpu_bytes,face_bytes;
    uint8_t *pixels; /* face-major: 6 * face_bytes */
    void *native;
    CubeLevel level[MAX_LEVELS];
} CubeTexture;
typedef struct {unsigned w,h,depth;size_t pitch,plane_bytes,bytes,offset;int uploaded;} VolumeLevel;
typedef struct {
    D9Handle id;unsigned w,h,depth,levels,format,pool,usage;
    int cpu_only;
    size_t cost,cpu_bytes,gpu_bytes;
    uint8_t *pixels; /* level-major: level[i].offset, each level has depth slices */
    void *native;
    VolumeLevel level[MAX_LEVELS];
} VolumeTexture;
struct D9Device {
    D9Backend ops;void *ctx;size_t used,limit;D9Handle next;int teardown;
    Buffer buffers[MAX_BUFFERS];Texture textures[MAX_TEXTURES];CubeTexture cubes[MAX_CUBES];VolumeTexture volumes[MAX_VOLUMES];
    Target targets[MAX_TARGETS];D9Handle target;size_t target_used;
    /* D3DPRESENTFLAG_LOCKABLE_BACKBUFFER: a retained offscreen colour target
     * that lives as a normal Target slot.  back_tex_w/h are its native
     * (power-of-two) storage dimensions; the logical surface stays back_w x
     * back_h. */
    D9Handle back_target;unsigned back_tex_w,back_tex_h;int back_lockable;
    unsigned target_native_max; /* env-selected cap for explicit targets, 64..512 */
    int scaled_readback;       /* TESV_D3D9_SCALED_READBACK=1 */
    Depth depths[MAX_DEPTHS];D9Handle depth;
    unsigned back_w,back_h,back_format;int back_valid;int blit_error;
    D9TexEntry tex_entries[MAX_TEX_ENTRIES];D9TexCache tex_cache;
    Stream streams[D9_MAX_STREAMS];D9Handle index,texture;const D9TevProgram *pixel_shader;D9Decl declaration;int decl_active;
    float tev_constants[9][4];
    float clip_planes[D9_MAX_CLIP_PLANES][4];
    unsigned clip_plane_enable;
    float ps_constants[D9_PS_CONST_F_REGS][4];
    int32_t ps_constants_i[D9_PS_CONST_I_REGS][4];
    uint8_t ps_constants_b[D9_PS_CONST_B_REGS];
    uint32_t ps_constants_f_mask;
    int32_t vs_constants_i[D9_VS_CONST_I_REGS][4];
    uint8_t vs_constants_b[D9_VS_CONST_B_REGS];
    D9Matrix transforms[3],shader_clip;int scene,shader_active;D9DrawState state,second;
    D9TnlState tnl;int tnl_proto;
    D9Handle texture1,cube_texture,cube_texture1,volume_texture,volume_texture1;
    D9Vertex scratch[MAX_DRAW_VERTICES];unsigned scratch_count;
    D9ColorVertex color_scratch[MAX_DRAW_VERTICES];unsigned color_scratch_count;
    unsigned cube_faces[MAX_DRAW_VERTICES]; /* per-output-vertex D3D cube face */
    unsigned view_w,view_h;
    char diagnosis[224];
};
static int cache_release_guest(void *ctx,void *guest,size_t bytes){(void)ctx;(void)bytes;free(guest);return 0;}
static int cache_release_gpu(void *ctx,void *gpu,size_t bytes){
    D9Device *d=ctx;(void)bytes;
    if(d->ops.texture_release)return d->ops.texture_release(d->ctx,gpu);
    d->ops.texture_delete(d->ctx,gpu);return 0;
}
static int cache_release_cube_gpu(void *ctx,void *gpu,size_t bytes){
    D9Device *d=ctx;(void)bytes;
    if(d->ops.texture_delete_cube)return d->ops.texture_delete_cube(d->ctx,gpu);
    if(d->ops.texture_release)return d->ops.texture_release(d->ctx,gpu);
    d->ops.texture_delete(d->ctx,gpu);return 0;
}
static size_t memory_sum(size_t a,size_t b){return b>SIZE_MAX-a?SIZE_MAX:a+b;}
static int budget_has_room(const D9Device *d,size_t bytes){
    size_t used=d9_memory_used(d);return used>d->limit||bytes>d->limit-used?D9_MEMORY:D9_OK;
}
void d9_matrix_identity(D9Matrix *m){memset(m,0,sizeof *m);for(unsigned i=0;i<4;++i)m->m[i][i]=1.0f;}
void d9_matrix_multiply(D9Matrix *out,const D9Matrix *a,const D9Matrix *b){
    D9Matrix t={0};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)t.m[i][j]+=a->m[i][k]*b->m[k][j];*out=t;
}
void d9_pica_transform(D9Matrix *out,const D9Matrix *w,const D9Matrix *v,const D9Matrix *p){
    D9Matrix a,b;d9_matrix_multiply(&a,w,v);d9_matrix_multiply(&b,&a,p);
    /* D3D row-vector -> GPU column-vector, rotated top LCD, clip z [0,w] -> [-w,0]. */
    for(unsigned i=0;i<4;++i){out->m[0][i]=b.m[i][1];out->m[1][i]=-b.m[i][0];out->m[2][i]=b.m[i][2]-b.m[i][3];out->m[3][i]=b.m[i][3];}
}
static void set_diagnosis(D9Device *d,const char *why){if(!d)return;if(!why)why="";snprintf(d->diagnosis,sizeof d->diagnosis,"%s",why);}
static int pow2(unsigned n){return n&&n<=1024&&!(n&(n-1));}
static int dim(unsigned n){return n>=8&&pow2(n);}
int d9_texture_layout(unsigned w,unsigned h,unsigned f,size_t *pitch,size_t *bytes){
    if(!pitch||!bytes||!pow2(w)||!pow2(h))return D9_INVALID;
    unsigned bb=d9_bc_block_bytes(f);
    if(bb){*pitch=(size_t)((w+3)/4)*bb;*bytes=*pitch*((h+3)/4);return D9_OK;}
    if(f==D9_A8){*pitch=(size_t)w;*bytes=*pitch*h;return D9_OK;}
    if(f!=D9_A8R8G8B8&&f!=D9_X8R8G8B8)return D9_INVALID;*pitch=(size_t)w*4;*bytes=*pitch*h;return D9_OK;
}
static unsigned native_dim(unsigned n){return n<8?8:n;}
int d9_swizzle_rgba8(void *dst,size_t bytes,const void *src,size_t pitch,unsigned w,unsigned h,unsigned fmt){
    if(!dst||!src||!pow2(w)||!pow2(h)||pitch<(size_t)w*4||(h>1&&pitch>(SIZE_MAX-(size_t)w*4)/(h-1))||bytes<(size_t)native_dim(w)*native_dim(h)*4||
       (fmt!=D9_A8R8G8B8&&fmt!=D9_X8R8G8B8))return D9_INVALID;
    uint8_t *out=dst;const uint8_t *in=src;
    unsigned pw=native_dim(w),ph=native_dim(h);
    for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
        /* PICA texture origin is bottom-left; D3D surface rows are top-first. */
        unsigned ty=ph-1-y,morton=0;
        for(unsigned b=0;b<3;++b){morton|=((x>>b)&1)<<(2*b);morton|=((ty>>b)&1)<<(2*b+1);}
        size_t i=((size_t)(ty/8)*(pw/8)+x/8)*64+morton;
        const uint8_t *s=in+(size_t)(y/(ph/h))*pitch+(x/(pw/w))*4;uint8_t *d=out+i*4;
        d[0]=fmt==D9_X8R8G8B8?255:s[3];d[1]=s[0];d[2]=s[1];d[3]=s[2];
    }return D9_OK;
}
int d9_unswizzle_rgba8(void *dst,size_t dst_pitch,const void *src,
                       unsigned w,unsigned h,unsigned format,
                       unsigned x,unsigned y,unsigned rw,unsigned rh){
    if(!dst||!src||!dim(w)||!dim(h)||x>w||y>h||rw>w-x||rh>h-y||
       dst_pitch<(size_t)w*4||dst_pitch>(SIZE_MAX-(size_t)w*4)/(h-1)||
       (format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8))return D9_INVALID;
    uint8_t *out=dst;const uint8_t *in=src;
    for(unsigned yy=0;yy<rh;++yy)for(unsigned xx=0;xx<rw;++xx){
        unsigned px=x+xx,py=y+yy,ty=h-1-py,morton=0;
        for(unsigned b=0;b<3;++b){morton|=((px>>b)&1)<<(2*b);morton|=((ty>>b)&1)<<(2*b+1);}
        size_t i=((size_t)(ty/8)*(w/8)+px/8)*64+morton;
        const uint8_t *s=in+i*4;uint8_t *d=out+(size_t)py*dst_pitch+(size_t)px*4;
        d[0]=s[1];d[1]=s[2];d[2]=s[3];d[3]=format==D9_X8R8G8B8?255:s[0];
    }return D9_OK;
}
D9Device *d9_create_device(const D9Backend *ops,void *ctx,size_t limit){
    if(!ops||!ops->texture_create||!ops->texture_upload||!ops->texture_delete||!ops->begin||!ops->draw||!ops->end||!limit)return NULL;
    D9Device *d=calloc(1,sizeof *d);if(!d)return NULL;d->ops=*ops;d->ctx=ctx;d->limit=limit;d->next=1;
    d->target_native_max=target_native_max_from_env();
    d->scaled_readback=scaled_readback_from_env();
    D9TexCacheOps cache_ops={d,cache_release_guest,cache_release_gpu};
    if(d9_texcache_init(&d->tex_cache,d->tex_entries,MAX_TEX_ENTRIES,&cache_ops,0,0)){
        free(d);return NULL;
    }
    d->state=(D9DrawState){.z_enable=1,.z_write=1,.z_func=4,.cull=2,.alpha_func=2,.src_blend=2,.dst_blend=1,.blend_op=1,.color_write=15,
        .address_u=1,.address_v=1,.mag_filter=1,.min_filter=1,.mip_filter=0,
        .stage_colorop=D9_TOP_SELECTARG1,.stage_colorarg1=D9_TA_TEXTURE,.stage_colorarg2=D9_TA_CURRENT,
        .stage_alphaop=D9_TOP_SELECTARG1,.stage_alphaarg1=D9_TA_TEXTURE,.stage_alphaarg2=D9_TA_CURRENT,
        .stage_texcoordindex=0};
    d->second=d->state;
    d->second.stage_colorop=d->second.stage_alphaop=D9_TOP_DISABLE;
    d->second.stage_texcoordindex=0;
    d9_tnl_defaults(&d->tnl);
    for(unsigned i=0;i<3;++i)d9_matrix_identity(&d->transforms[i]);return d;
}
static Buffer *buffer(D9Device *d,D9Handle id){if(id)for(unsigned i=0;i<MAX_BUFFERS;++i)if(d->buffers[i].id==id)return &d->buffers[i];return NULL;}
static Texture *texture(D9Device *d,D9Handle id){if(id)for(unsigned i=0;i<MAX_TEXTURES;++i)if(d->textures[i].id==id)return &d->textures[i];return NULL;}
D9Handle d9_texture_render_target(const D9Device *d,D9Handle h){
    if(!d||!h)return 0;
    for(unsigned i=0;i<MAX_TEXTURES;++i)if(d->textures[i].id==h)return d->textures[i].rt_target;
    return 0;
}
static CubeTexture *cube_texture(D9Device *d,D9Handle id){if(id)for(unsigned i=0;i<MAX_CUBES;++i)if(d->cubes[i].id==id)return &d->cubes[i];return NULL;}
static VolumeTexture *volume_texture(D9Device *d,D9Handle id){if(id)for(unsigned i=0;i<MAX_VOLUMES;++i)if(d->volumes[i].id==id)return &d->volumes[i];return NULL;}
int d9_destroy_device_ex(D9Device *d){
    if(!d)return D9_INVALID;d->teardown=1;
    if(d->scene){d->ops.end(d->ctx);d->scene=0;}
    int rc=d9_texcache_destroy_ex(&d->tex_cache);
    if(rc!=D9TEXCACHE_OK)return D9_RELEASE_FAILED;
    for(unsigned i=0;i<MAX_BUFFERS;++i)free(d->buffers[i].data);
    /* Cube and volume guest pixels and native handles are owned by the texture
     * cache (d9_texcache_destroy_ex above); freeing the slots here would double
     * free every one of them. */
    for(unsigned i=0;i<MAX_TARGETS;++i)if(d->targets[i].id){
        if(d->ops.target_delete)d->ops.target_delete(d->ctx,d->targets[i].native);
    }
    d->target_used=0;
    free(d);return D9_OK;
}
void d9_destroy_device(D9Device *d){if(d)(void)d9_destroy_device_ex(d);}
static int create_buffer(D9Device *d,size_t bytes,unsigned kind,D9Handle *out){
    if(!d||d->scene||d->teardown||!bytes||!out||!d->next)return D9_INVALID;
    if(budget_has_room(d,bytes))return D9_MEMORY;
    for(unsigned i=0;i<MAX_BUFFERS;++i)if(!d->buffers[i].id){uint8_t *data=calloc(1,bytes);if(!data)return D9_MEMORY;
        D9Handle id=d->next++;d->buffers[i]=(Buffer){id,kind,data,bytes,0};d->used+=bytes;*out=id;return D9_OK;}
    return D9_MEMORY;
}
int d9_create_vertex_buffer(D9Device *d,size_t bytes,D9Handle *out){return create_buffer(d,bytes,0,out);}
int d9_create_index_buffer(D9Device *d,size_t bytes,unsigned fmt,D9Handle *out){if(fmt==D9_INDEX16&&!(bytes&1))return create_buffer(d,bytes,1,out);
    if(fmt==D9_INDEX32&&!(bytes&3))return create_buffer(d,bytes,2,out);return D9_INVALID;}
int d9_buffer_lock(D9Device *d,D9Handle h,size_t off,size_t bytes,void **ptr){
    if(!d||d->scene||!ptr)return D9_INVALID;Buffer *b=buffer(d,h);
    if(!b||b->locked||off>=b->bytes||bytes>b->bytes-off)return D9_INVALID;b->locked=1;*ptr=b->data+off;return D9_OK;
}
int d9_buffer_unlock(D9Device *d,D9Handle h){Buffer *b=d?buffer(d,h):NULL;if(!b||!b->locked)return D9_INVALID;b->locked=0;return D9_OK;}
int d9_release_buffer(D9Device *d,D9Handle h){Buffer *b=d?buffer(d,h):NULL;if(!b||b->locked||d->scene)return D9_INVALID;
    for(unsigned i=0;i<D9_MAX_STREAMS;++i)if(d->streams[i].id==h)d->streams[i]=(Stream){0};
    if(d->index==h)d->index=0;d->used-=b->bytes;free(b->data);memset(b,0,sizeof *b);return D9_OK;}
static void delete_unowned_native(D9Device *d,void *native){if(native)d->ops.texture_delete(d->ctx,native);}
/* Cube/volume residency.  Both kinds keep their full CPU slice store as the
 * cache guest copy and register the backend-native object as the GPU copy.
 * The guest copy is never dropped and the entry is marked explicit-release-
 * only: cube/volume native rehydration is not implemented yet, so an LRU
 * eviction could not be repaired.  d9_texcache_remove() still releases them. */
static size_t cube_gpu_bytes(unsigned edge,unsigned levels){
    uint64_t total=0,dim=edge;
    for(unsigned i=0;i<levels;++i){
        size_t side=native_dim((unsigned)dim);
        total+= (uint64_t)side*side*4u;
        if(dim>1)dim>>=1;
    }
    total*=6u;
    return total>SIZE_MAX?SIZE_MAX:(size_t)total;
}
static size_t volume_gpu_bytes(unsigned w,unsigned h,unsigned depth,unsigned levels){
    uint64_t total=0,lw=w,lh=h,ld=depth;
    for(unsigned i=0;i<levels;++i){
        size_t sw=native_dim((unsigned)lw),sh=native_dim((unsigned)lh);
        total+= (uint64_t)sw*sh*4u*ld;
        if(lw>1)lw>>=1;if(lh>1)lh>>=1;if(ld>1)ld>>=1;
    }
    return total>SIZE_MAX?SIZE_MAX:(size_t)total;
}
/* Takes ownership of pixels and native on every path.  On success both are
 * owned by the cache and the entry is non-evictable. */
static int register_subtexture(D9Device *d,D9Handle key,uint8_t *pixels,size_t cpu,
                               void *native,size_t gpu,int cpu_only,
                               D9TexGpuRelease release){
    if(d9_texcache_add_guest(&d->tex_cache,key,pixels,cpu,1)!=D9TEXCACHE_OK){
        delete_unowned_native(d,native);free(pixels);return D9_BACKEND;
    }
    if(!cpu_only){
        D9TexGpuRegistration reg={native,gpu,1,release,d,D9TEXCACHE_NO_REHYDRATE_TOKEN};
        if(d9_texcache_attach_gpu(&d->tex_cache,key,&reg)!=D9TEXCACHE_OK){
            (void)d9_texcache_remove(&d->tex_cache,key);
            delete_unowned_native(d,native);
            return D9_BACKEND;
        }
    }
    (void)d9_texcache_set_no_evict(&d->tex_cache,key,1);
    return D9_OK;
}
static int subtexture_has_gpu(D9Device *d,D9Handle h){
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
    return e&&e->has_gpu;
}
static int subtexture_commit_ready(D9Device *d,D9Handle h,void *native,size_t gpu){
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
    if(!e||!e->has_gpu||e->gpu_ready)return D9_OK;
    return d9_texcache_commit_upload(&d->tex_cache,h,native,gpu,0)==D9TEXCACHE_OK?D9_OK:D9_BACKEND;
}
int d9_create_texture_ex(D9Device *d,unsigned w,unsigned h,unsigned fmt,unsigned levels,
                         unsigned pool,unsigned usage,int disposable,D9Handle *out){
    if(!d||d->scene||d->teardown||!out||!d->next||!pow2(w)||!pow2(h)||pool>D9_POOL_SYSTEMMEM)return D9_INVALID;
    int padded=w<8||h<8;
    if(padded&&fmt!=D9_A8R8G8B8&&fmt!=D9_X8R8G8B8){
        set_diagnosis(d,"Small textures require uncompressed RGBA8 or XRGB8");return D9_UNSUPPORTED;
    }
    Texture t={.w=w,.h=h,.format=fmt,.pool=pool,.usage=usage,.disposable=disposable?1:0,
               .cpu_only=pool==D9_POOL_SYSTEMMEM};size_t cpu=0,gpu=0;unsigned max=1,a=w,b=h;
    while(a>1||b>1){if(a>1)a>>=1;if(b>1)b>>=1;++max;}
    if(!levels)levels=max;if(levels>max||levels>MAX_LEVELS)return D9_INVALID;t.levels=levels;a=w;b=h;
    for(unsigned i=0;i<levels;++i){Level *l=&t.level[i];l->w=a;l->h=b;l->offset=cpu;if(d9_texture_layout(a,b,fmt,&l->pitch,&l->bytes))return D9_INVALID;
        cpu+=l->bytes;if((a>=8&&b>=8)||(padded&&i==0)){++t.gpu_levels;gpu+=(size_t)native_dim(a)*native_dim(b)*4;}if(a>1)a>>=1;if(b>1)b>>=1;}
    if(t.cpu_only)t.gpu_levels=0;
    t.cpu_bytes=cpu;t.gpu_bytes=t.cpu_only?0:gpu;t.cost=cpu+t.gpu_bytes;
    if(budget_has_room(d,t.cost))return D9_MEMORY;
    Texture *slot=NULL;for(unsigned i=0;i<MAX_TEXTURES;++i)if(!d->textures[i].id){slot=&d->textures[i];break;}
    if(!slot)return D9_MEMORY;
    t.pixels=calloc(1,cpu);if(!t.pixels)return D9_MEMORY;
    if(!t.cpu_only){
        if(d->ops.texture_create(d->ctx,native_dim(w),native_dim(h),t.gpu_levels,&t.native)||!t.native){delete_unowned_native(d,t.native);free(t.pixels);return D9_BACKEND;}
    }
    D9Handle key=d->next;
    if(d9_texcache_add_guest(&d->tex_cache,key,t.pixels,cpu,1)!=D9TEXCACHE_OK){
        delete_unowned_native(d,t.native);free(t.pixels);return D9_BACKEND;
    }
    if(!t.cpu_only){
        D9TexGpuRegistration reg={t.native,t.gpu_bytes,1,cache_release_gpu,d,D9TEXCACHE_NO_REHYDRATE_TOKEN};
        if(d9_texcache_attach_gpu(&d->tex_cache,key,&reg)!=D9TEXCACHE_OK){
            (void)d9_texcache_remove(&d->tex_cache,key);delete_unowned_native(d,t.native);return D9_BACKEND;
        }
    }
    t.id=d->next++;*slot=t;*out=t.id;return D9_OK;
}
int d9_create_texture_levels(D9Device *d,unsigned w,unsigned h,unsigned fmt,unsigned levels,D9Handle *out){
    return d9_create_texture_ex(d,w,h,fmt,levels,D9_POOL_DEFAULT,0,0,out);
}
int d9_create_texture(D9Device *d,unsigned w,unsigned h,unsigned f,D9Handle *out){return d9_create_texture_levels(d,w,h,f,1,out);}
int d9_create_texture_sktx(D9Device *d,const void *sktx_data,size_t bytes,
                           unsigned pool,unsigned usage,D9Handle *out){
    if(!d||d->scene||d->teardown||!sktx_data||!out||!d->next||pool>D9_POOL_MANAGED||
       !d->ops.texture_create_encoded||!d->ops.texture_upload_encoded)return D9_INVALID;
    D9Sktx sktx;if(d9_sktx_parse(sktx_data,bytes,&sktx))return D9_INVALID;
    if(!dim(sktx.width)||!dim(sktx.height)||sktx.levels>MAX_LEVELS)return D9_INVALID;
    unsigned pw=sktx.width,ph=sktx.height;
    for(unsigned i=0;i<sktx.levels;++i){
        if(sktx.level[i].width!=pw||sktx.level[i].height!=ph||pw<8||ph<8)return D9_INVALID;
        if(pw>1)pw>>=1;if(ph>1)ph>>=1;
    }
    if(budget_has_room(d,sktx.data_bytes))return D9_MEMORY;
    Texture *slot=NULL;for(unsigned i=0;i<MAX_TEXTURES;++i)if(!d->textures[i].id){slot=&d->textures[i];break;}
    if(!slot)return D9_MEMORY;
    Texture t={.w=sktx.width,.h=sktx.height,.format=sktx.format,.levels=sktx.levels,
               .gpu_levels=sktx.levels,.pool=pool,.usage=usage,.gpu_only=1,
               .gpu_bytes=sktx.data_bytes,.cost=sktx.data_bytes};
    if(d->ops.texture_create_encoded(d->ctx,t.w,t.h,t.levels,t.format,&t.native)||!t.native){delete_unowned_native(d,t.native);return D9_BACKEND;}
    for(unsigned i=0;i<t.levels;++i){
        if(d->ops.texture_upload_encoded(d->ctx,t.native,i,sktx.level[i].data,sktx.level[i].bytes)){
            delete_unowned_native(d,t.native);return D9_BACKEND;
        }
    }
    D9TexGpuRegistration reg={t.native,t.gpu_bytes,1,cache_release_gpu,d,D9TEXCACHE_NO_REHYDRATE_TOKEN};
    if(d9_texcache_add_gpu(&d->tex_cache,d->next,&reg)!=D9TEXCACHE_OK){
        delete_unowned_native(d,t.native);return D9_BACKEND;
    }
    size_t offset=0;
    for(unsigned i=0;i<t.levels;++i){Level *l=&t.level[i];l->w=sktx.level[i].width;l->h=sktx.level[i].height;
        l->offset=offset;l->bytes=sktx.level[i].bytes;l->uploaded=1;offset+=l->bytes;}
    t.id=d->next++;*slot=t;*out=t.id;return D9_OK;
}
unsigned d9_texture_levels(D9Device *d,D9Handle h){Texture *t=d?texture(d,h):NULL;return t?t->levels:0;}
int d9_texture_level_info(D9Device *d,D9Handle h,unsigned level,unsigned *w,unsigned *height,size_t *pitch,size_t *bytes){Texture *t=d?texture(d,h):NULL;
    if(!t||level>=t->levels)return D9_INVALID;Level *l=&t->level[level];if(w)*w=l->w;if(height)*height=l->h;if(pitch)*pitch=l->pitch;if(bytes)*bytes=l->bytes;return D9_OK;}
int d9_texture_level_view(D9Device *d,D9Handle h,unsigned level,void **pixels,
                          unsigned *w,unsigned *height,size_t *pitch,size_t *bytes){
    Texture *t=d?texture(d,h):NULL;
    if(!t||level>=t->levels)return D9_INVALID;
    if(!t->pixels||t->gpu_only)return D9_UNSUPPORTED;
    if(d9_bc_block_bytes(t->format))return D9_UNSUPPORTED;
    Level *l=&t->level[level];
    if(pixels)*pixels=t->pixels+l->offset;
    if(w)*w=l->w;if(height)*height=l->h;if(pitch)*pitch=l->pitch;if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_texture_surface_unlocked(D9Device *d,D9Handle h,unsigned level){
    Texture *t=d?texture(d,h):NULL;
    if(!t||level>=t->levels||d->scene||d->teardown)return D9_INVALID;
    Level *l=&t->level[level];
    if(!t->cpu_only&&!t->gpu_only&&level<t->gpu_levels){
        if(t->format==D9_A8){
            if(d->ops.texture_upload(d->ctx,t->native,level,t->pixels+l->offset,
                                     l->bytes))return D9_BACKEND;
        }else{
            size_t bytes=(size_t)native_dim(l->w)*native_dim(l->h)*4;uint8_t *tmp=malloc(bytes);if(!tmp)return D9_MEMORY;
            const void *src=t->pixels+l->offset;
            int rc=d9_bc_block_bytes(t->format)?d9_bc_to_pica(t->format,tmp,bytes,src,l->bytes,l->w,l->h,l->pitch):
                d9_swizzle_rgba8(tmp,bytes,src,l->pitch,l->w,l->h,t->format);
            if(!rc&&d->ops.texture_upload(d->ctx,t->native,level,tmp,bytes))rc=D9_BACKEND;
            free(tmp);if(rc)return rc;
        }
    }
    l->uploaded=1;
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,t->id);
    if(e&&e->has_gpu&&!e->gpu_ready&&
       d9_texcache_commit_upload(&d->tex_cache,t->id,t->native,t->gpu_bytes,0)!=D9TEXCACHE_OK)
        return D9_BACKEND;
    return D9_OK;
}
int d9_texture_copy_levels(D9Device *d,D9Handle dst,D9Handle src){
    Texture *a=d?texture(d,dst):NULL,*b=d?texture(d,src):NULL;
    if(!a||!b){
        set_diagnosis(d,"UpdateTexture: invalid 2D texture handle");
        return D9_INVALID;
    }
    if(d->scene||d->teardown){
        set_diagnosis(d,"UpdateTexture: cannot update while a scene is active");
        return D9_INVALID;
    }
    if(a==b)return D9_OK;
    if(a->format!=b->format||a->w!=b->w||a->h!=b->h||a->levels!=b->levels){
        set_diagnosis(d,"UpdateTexture: source and destination must share format, dimensions and level count");
        return D9_INVALID;
    }
    if(a->gpu_only||b->gpu_only||!a->pixels||!b->pixels){
        set_diagnosis(d,"UpdateTexture: both textures need a CPU guest copy; render-target/GPU-only textures are not updatable");
        return D9_UNSUPPORTED;
    }
    if(a->locked||b->locked){
        set_diagnosis(d,"UpdateTexture: source or destination texture is locked");
        return D9_INVALID;
    }
    for(unsigned i=0;i<a->levels;++i)
        memcpy(a->pixels+a->level[i].offset,b->pixels+b->level[i].offset,a->level[i].bytes);
    if(!a->cpu_only){
        for(unsigned i=0;i<a->levels&&i<a->gpu_levels;++i){
            int rc=d9_texture_surface_unlocked(d,dst,i);
            if(rc){
                snprintf(d->diagnosis,sizeof d->diagnosis,
                         "UpdateTexture: destination level %u upload failed",i);
                return rc;
            }
        }
    }
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_texture_resource_info(D9Device *d,D9Handle h,unsigned *pool,
                             unsigned *usage,unsigned *format,
                             unsigned *levels,size_t *guest_bytes,
                             size_t *gpu_bytes,int *cpu_only,int *gpu_only){
    Texture *t=d?texture(d,h):NULL;if(!t)return D9_INVALID;
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
    if(pool)*pool=t->pool;if(usage)*usage=t->usage;if(format)*format=t->format;if(levels)*levels=t->levels;
    if(guest_bytes)*guest_bytes=e&&e->has_guest?e->guest_bytes:0;
    if(gpu_bytes)*gpu_bytes=e&&e->has_gpu?e->gpu_bytes:0;
    if(cpu_only)*cpu_only=t->cpu_only;if(gpu_only)*gpu_only=t->gpu_only;return D9_OK;
}
int d9_texture_lock_level_ex(D9Device *d,D9Handle h,unsigned level,void **ptr,size_t *pitch,int readonly){
    Texture *t=d?texture(d,h):NULL;
    if(!t||d->scene||d->teardown||t->locked||!t->pixels||!ptr||!pitch||level>=t->levels)return D9_INVALID;
    if(d9_texcache_pin(&d->tex_cache,h)!=D9TEXCACHE_OK)return D9_INVALID;
    Level *l=&t->level[level];t->locked=level+1;t->cache_pinned=1;*ptr=t->pixels+l->offset;*pitch=l->pitch;
    (void)readonly;return D9_OK;
}
int d9_texture_lock_level(D9Device *d,D9Handle h,unsigned level,void **ptr,size_t *pitch){
    return d9_texture_lock_level_ex(d,h,level,ptr,pitch,0);
}
static int finish_texture_unlock(D9Device *d,Texture *t,unsigned level,int readonly){
    if(!t||level>=t->levels||t->locked!=level+1||d->scene||d->teardown)return D9_INVALID;
    Level *l=&t->level[level];
    if(!readonly&&!t->cpu_only&&level<t->gpu_levels){
        if(t->format==D9_A8){
            if(d->ops.texture_upload(d->ctx,t->native,level,t->pixels+l->offset,
                                     l->bytes))return D9_BACKEND;
        }else{
            size_t bytes=(size_t)native_dim(l->w)*native_dim(l->h)*4;uint8_t *tmp=malloc(bytes);if(!tmp)return D9_MEMORY;const void *src=t->pixels+l->offset;
            int rc=d9_bc_block_bytes(t->format)?d9_bc_to_pica(t->format,tmp,bytes,src,l->bytes,l->w,l->h,l->pitch):
                d9_swizzle_rgba8(tmp,bytes,src,l->pitch,l->w,l->h,t->format);
            if(!rc&&d->ops.texture_upload(d->ctx,t->native,level,tmp,bytes))rc=D9_BACKEND;
            free(tmp);if(rc)return rc;
        }
    }
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,t->id);
    int rc=D9TEXCACHE_OK;
    if(!readonly&&!t->cpu_only&&e&&e->has_gpu&&!e->gpu_ready)
        rc=d9_texcache_commit_upload(&d->tex_cache,t->id,t->native,t->gpu_bytes,0);
    else
        rc=d9_texcache_abort_upload(&d->tex_cache,t->id);
    if(rc!=D9TEXCACHE_OK)return D9_BACKEND;
    t->locked=0;t->cache_pinned=0;if(!readonly)l->uploaded=1;return D9_OK;
}
int d9_texture_unlock_level(D9Device *d,D9Handle h,unsigned level){Texture *t=d?texture(d,h):NULL;return finish_texture_unlock(d,t,level,0);}
int d9_texture_lock(D9Device *d,D9Handle h,void **ptr,size_t *pitch){return d9_texture_lock_level(d,h,0,ptr,pitch);}
int d9_texture_unlock(D9Device *d,D9Handle h){return d9_texture_unlock_level(d,h,0);}
int d9_texture_unlock_readonly(D9Device *d,D9Handle h,unsigned level){Texture *t=d?texture(d,h):NULL;return finish_texture_unlock(d,t,level,1);}
int d9_release_texture(D9Device *d,D9Handle h){Texture *t=d?texture(d,h):NULL;if(!t||d->scene||d->teardown||t->locked)return D9_INVALID;
    if(t->rt_target){
        D9Handle tgt=t->rt_target;
        if(d->texture==h)d->texture=0;if(d->texture1==h)d->texture1=0;
        memset(t,0,sizeof *t);
        return d9_release_render_target(d,tgt);
    }
    int rc=d9_texcache_remove(&d->tex_cache,h);
    if(rc!=D9TEXCACHE_OK){
        if(rc==D9TEXCACHE_RELEASE_FAILED){
            D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
            if(e&&!e->has_guest){t->pixels=NULL;t->cpu_bytes=0;}
            if(e&&!e->has_gpu){t->native=NULL;t->gpu_bytes=0;if(d->texture == h)d->texture=0;if(d->texture1 == h)d->texture1=0;}
            return D9_RELEASE_FAILED;
        }
        return D9_BACKEND;
    }
    if(d->texture == h)d->texture=0;if(d->texture1 == h)d->texture1=0;memset(t,0,sizeof *t);return D9_OK;}
/* ---- cube textures -------------------------------------------------------- */
static int cube_rows(unsigned edge,unsigned format,size_t *rows,size_t *row_bytes){
    unsigned bb=d9_bc_block_bytes(format);
    if(bb){
        *rows=(size_t)((edge+3u)/4u);
        *row_bytes=(size_t)((edge+3u)/4u)*bb;
    }else{
        *rows=(size_t)edge;
        *row_bytes=(size_t)edge*4u;
    }
    return 0;
}
int d9_create_cube_texture_ex(D9Device *d,unsigned edge,unsigned levels,
                              unsigned format,unsigned pool,unsigned usage,
                              D9Handle *out){
    if(out)*out=0;
    if(!d||d->scene||d->teardown||!out||!d->next||!pow2(edge)||pool>D9_POOL_SYSTEMMEM)
        return D9_INVALID;
    if(!d->ops.texture_create_cube||!d->ops.texture_upload_cube){
        set_diagnosis(d,"cube textures require backend texture_create_cube/upload_cube; "
                        "this backend has no cube-map texture API");
        return D9_UNSUPPORTED;
    }
    if(d9_bc_block_bytes(format)&&edge<8u){
        set_diagnosis(d,"compressed cube faces below 8x8 are not implemented");
        return D9_UNSUPPORTED;
    }
    unsigned max=1,a=edge;
    while(a>1){a>>=1;++max;}
    if(!levels)levels=max;
    if(levels>max||levels>MAX_LEVELS)return D9_INVALID;
    CubeTexture t={.edge=edge,.levels=levels,.format=format,.pool=pool,.usage=usage,
                   .cpu_only=pool==D9_POOL_SYSTEMMEM};
    size_t offset=0;unsigned dim=edge;
    for(unsigned i=0;i<levels;++i){
        CubeLevel *l=&t.level[i];l->w=l->h=dim;l->offset=offset;
        if(d9_texture_layout(dim,dim,format,&l->pitch,&l->bytes))return D9_INVALID;
        offset+=l->bytes;
        if(dim>1)dim>>=1;
    }
    t.face_bytes=offset;
    if(t.face_bytes>SIZE_MAX/6u)return D9_MEMORY;
    t.cpu_bytes=t.face_bytes*6u;
    t.gpu_bytes=t.cpu_only?0:cube_gpu_bytes(edge,levels);
    if(t.cpu_bytes>SIZE_MAX-t.gpu_bytes)return D9_MEMORY;
    t.cost=t.cpu_bytes+t.gpu_bytes;
    if(budget_has_room(d,t.cost))return D9_MEMORY;
    CubeTexture *slot=NULL;
    for(unsigned i=0;i<MAX_CUBES;++i)if(!d->cubes[i].id){slot=&d->cubes[i];break;}
    if(!slot)return D9_MEMORY;
    t.pixels=calloc(1,t.cpu_bytes);
    if(!t.pixels)return D9_MEMORY;
    if(!t.cpu_only){
        int rc=d->ops.texture_create_cube(d->ctx,edge,levels,format,&t.native);
        if(rc||!t.native){
            if(rc==-2){
                snprintf(d->diagnosis,sizeof d->diagnosis,
                         "cube backend does not support format 0x%lx at edge %u",
                         (unsigned long)format,edge);
                free(t.pixels);return D9_UNSUPPORTED;
            }
            if(rc==-3){
                snprintf(d->diagnosis,sizeof d->diagnosis,
                         "cube backend could not allocate edge %u levels %u (VRAM)",
                         edge,levels);
                free(t.pixels);return D9_MEMORY;
            }
            snprintf(d->diagnosis,sizeof d->diagnosis,
                     "cube backend rejected creation of format 0x%lx edge %u levels %u",
                     (unsigned long)format,edge,levels);
            free(t.pixels);return D9_BACKEND;
        }
    }
    D9Handle key=d->next;
    if(register_subtexture(d,key,t.pixels,t.cpu_bytes,t.native,t.gpu_bytes,t.cpu_only,
                           cache_release_cube_gpu))
        return D9_BACKEND;
    t.id=d->next++;*slot=t;*out=t.id;return D9_OK;
}
unsigned d9_cube_levels(D9Device *d,D9Handle h){CubeTexture *c=cube_texture(d,h);return c?c->levels:0;}
int d9_cube_level_info(D9Device *d,D9Handle h,unsigned level,
                       unsigned *edge,size_t *pitch,size_t *bytes){
    CubeTexture *c=cube_texture(d,h);
    if(!c||level>=c->levels)return D9_INVALID;
    const CubeLevel *l=&c->level[level];
    if(edge)*edge=l->w;
    if(pitch)*pitch=l->pitch;
    if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_cube_face_level_info(D9Device *d,D9Handle h,unsigned face,unsigned level,
                            unsigned *w,unsigned *height,size_t *pitch,size_t *bytes){
    CubeTexture *c=cube_texture(d,h);
    if(!c||face>=D9_CUBE_FACES||level>=c->levels)return D9_INVALID;
    const CubeLevel *l=&c->level[level];
    if(w)*w=l->w;if(height)*height=l->h;if(pitch)*pitch=l->pitch;if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_cube_face_level_view(D9Device *d,D9Handle h,unsigned face,unsigned level,
                            void **pixels,unsigned *w,unsigned *height,
                            size_t *pitch,size_t *bytes){
    CubeTexture *c=cube_texture(d,h);
    if(!c||face>=D9_CUBE_FACES||level>=c->levels||!c->pixels)return D9_INVALID;
    const CubeLevel *l=&c->level[level];
    if(pixels)*pixels=c->pixels+(size_t)face*c->face_bytes+l->offset;
    if(w)*w=l->w;if(height)*height=l->h;if(pitch)*pitch=l->pitch;if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_cube_upload_face_level(D9Device *d,D9Handle h,unsigned face,unsigned level,
                              const void *src,size_t src_pitch,size_t src_bytes){
    CubeTexture *c=cube_texture(d,h);
    if(!c||face>=D9_CUBE_FACES||level>=c->levels||!src||!src_pitch||d->scene||d->teardown)
        return D9_INVALID;
    CubeLevel *l=&c->level[level];
    size_t rows,row_bytes;cube_rows(l->w,c->format,&rows,&row_bytes);
    if(src_pitch<row_bytes)return D9_INVALID;
    if(src_bytes&&((rows-1u)>((SIZE_MAX-row_bytes)/src_pitch)||
       (rows-1u)*src_pitch+row_bytes>src_bytes))return D9_INVALID;
    if((rows-1u)>(SIZE_MAX-row_bytes)/l->pitch||
       (rows-1u)*l->pitch+row_bytes>l->bytes)return D9_INVALID;
    uint8_t *dst=c->pixels+(size_t)face*c->face_bytes+l->offset;
    for(size_t y=0;y<rows;++y)
        memcpy(dst+y*l->pitch,(const uint8_t *)src+y*src_pitch,row_bytes);
    if(!c->cpu_only){
        if(!subtexture_has_gpu(d,h)){
            set_diagnosis(d,"cube texture native copy is not resident; CPU pixels are "
                            "retained but cube rehydration is not implemented");
            return D9_UNSUPPORTED;
        }
        if(d->ops.texture_upload_cube(d->ctx,c->native,face,level,dst,l->bytes)){
            snprintf(d->diagnosis,sizeof d->diagnosis,
                     "cube backend upload failed at face %u level %u",face,level);
            return D9_BACKEND;
        }
    }
    l->uploaded=1;l->face_mask|=1u<<face;
    if(!c->cpu_only&&level==0&&
       c->level[0].face_mask==((1u<<D9_CUBE_FACES)-1u))
        return subtexture_commit_ready(d,h,c->native,c->gpu_bytes);
    return D9_OK;
}
int d9_cube_face_unlocked(D9Device *d,D9Handle h,unsigned face,unsigned level){
    CubeTexture *c=cube_texture(d,h);
    if(!c||face>=D9_CUBE_FACES||level>=c->levels)return D9_INVALID;
    CubeLevel *l=&c->level[level];
    if(!c->cpu_only){
        if(!subtexture_has_gpu(d,h)){
            set_diagnosis(d,"cube texture native copy is not resident; CPU pixels are "
                            "retained but cube rehydration is not implemented");
            return D9_UNSUPPORTED;
        }
        uint8_t *src=c->pixels+(size_t)face*c->face_bytes+l->offset;
        if(d->ops.texture_upload_cube(d->ctx,c->native,face,level,src,l->bytes)){
            snprintf(d->diagnosis,sizeof d->diagnosis,
                     "cube backend upload failed at face %u level %u",face,level);
            return D9_BACKEND;
        }
    }
    l->uploaded=1;l->face_mask|=1u<<face;
    if(!c->cpu_only&&level==0&&
       c->level[0].face_mask==((1u<<D9_CUBE_FACES)-1u))
        return subtexture_commit_ready(d,h,c->native,c->gpu_bytes);
    return D9_OK;
}
int d9_release_cube_texture(D9Device *d,D9Handle h){
    CubeTexture *c=cube_texture(d,h);
    if(!c||d->scene||d->teardown)return D9_INVALID;
    int rc=d9_texcache_remove(&d->tex_cache,h);
    if(rc!=D9TEXCACHE_OK){
        if(rc==D9TEXCACHE_RELEASE_FAILED){
            /* Mirror the 2D contract: copies the cache already released are
             * detached from the core object; the rest stay retryable. */
            D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
            if(e&&!e->has_guest){c->pixels=NULL;c->cpu_bytes=0;c->face_bytes=0;}
            if(e&&!e->has_gpu){c->native=NULL;c->gpu_bytes=0;
                if(d->cube_texture==h)d->cube_texture=0;
                if(d->cube_texture1==h)d->cube_texture1=0;}
            return D9_RELEASE_FAILED;
        }
        return D9_BACKEND;
    }
    if(d->cube_texture==h)d->cube_texture=0;
    if(d->cube_texture1==h)d->cube_texture1=0;
    memset(c,0,sizeof *c);
    return D9_OK;
}
int d9_cube_resource_info(D9Device *d,D9Handle h,unsigned *levels,
                          unsigned *edge_length,unsigned *format,
                          size_t *guest_bytes,size_t *gpu_bytes,int *cpu_only){
    CubeTexture *c=cube_texture(d,h);if(!c)return D9_INVALID;
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
    if(levels)*levels=c->levels;if(edge_length)*edge_length=c->edge;if(format)*format=c->format;
    if(guest_bytes)*guest_bytes=e&&e->has_guest?e->guest_bytes:0;
    if(gpu_bytes)*gpu_bytes=e&&e->has_gpu?e->gpu_bytes:0;
    if(cpu_only)*cpu_only=c->cpu_only;return D9_OK;
}
int d9_bind_cube_texture(D9Device *d,unsigned stage,D9Handle h){
    if(!d||stage>=d9_stage_count(d)){
        if(d)set_diagnosis(d,"SetTexture: unsupported texture unit");
        return D9_UNSUPPORTED;
    }
    if(h){
        CubeTexture *c=cube_texture(d,h);
        if(!c||c->cpu_only||!c->native||c->level[0].face_mask!=((1u<<D9_CUBE_FACES)-1u))
            return D9_INVALID;
        D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
        if(!e||!e->has_gpu||!e->gpu_ready){
            set_diagnosis(d,"cube texture is not cache-resident; cube rehydration is not implemented");
            return D9_INVALID;
        }
        d9_texcache_touch(&d->tex_cache,h);
    }
    if(stage){d->cube_texture1=h;d->texture1=0;d->volume_texture1=0;}
    else{d->cube_texture=h;d->texture=0;d->volume_texture=0;}
    return D9_OK;
}
/* ---- volume textures ------------------------------------------------------ */
static void volume_dims(unsigned w,unsigned h,unsigned depth,unsigned level,
                        unsigned *lw,unsigned *lh,unsigned *ld){
    *lw=level<32&&(w>>level)?(w>>level):1;
    *lh=level<32&&(h>>level)?(h>>level):1;
    *ld=level<32&&(depth>>level)?(depth>>level):1;
}
int d9_create_volume_texture_ex(D9Device *d,unsigned w,unsigned h,unsigned depth,
                                unsigned levels,unsigned format,unsigned pool,
                                unsigned usage,D9Handle *out){
    if(out)*out=0;
    if(!d||d->scene||d->teardown||!out||!d->next||!pow2(w)||!pow2(h)||
       !pow2(depth)||depth<2||pool>D9_POOL_SYSTEMMEM)return D9_INVALID;
    if(!d->ops.texture_create_volume||!d->ops.texture_upload_volume){
        set_diagnosis(d,"volume textures require backend texture_create_volume/upload_volume; "
                        "this backend has no volume-texture API");
        return D9_UNSUPPORTED;
    }
    unsigned max=1,a=w>h?w:h;if(depth>a)a=depth;
    while(a>1){a>>=1;++max;}
    if(!levels)levels=max;
    if(levels>max||levels>MAX_LEVELS)return D9_INVALID;
    VolumeTexture t={.w=w,.h=h,.depth=depth,.levels=levels,.format=format,.pool=pool,
                     .usage=usage,.cpu_only=pool==D9_POOL_SYSTEMMEM};
    size_t offset=0;
    for(unsigned i=0;i<levels;++i){
        VolumeLevel *l=&t.level[i];
        volume_dims(w,h,depth,i,&l->w,&l->h,&l->depth);
        l->offset=offset;
        if(d9_texture_layout(l->w,l->h,format,&l->pitch,&l->plane_bytes))return D9_INVALID;
        if(l->plane_bytes>0&&l->depth>SIZE_MAX/l->plane_bytes)return D9_MEMORY;
        l->bytes=l->plane_bytes*l->depth;
        offset+=l->bytes;
    }
    t.cpu_bytes=offset;
    t.gpu_bytes=t.cpu_only?0:volume_gpu_bytes(w,h,depth,levels);
    if(t.cpu_bytes>SIZE_MAX-t.gpu_bytes)return D9_MEMORY;
    t.cost=t.cpu_bytes+t.gpu_bytes;
    if(budget_has_room(d,t.cost))return D9_MEMORY;
    VolumeTexture *slot=NULL;
    for(unsigned i=0;i<MAX_VOLUMES;++i)if(!d->volumes[i].id){slot=&d->volumes[i];break;}
    if(!slot)return D9_MEMORY;
    t.pixels=calloc(1,t.cpu_bytes);
    if(!t.pixels)return D9_MEMORY;
    if(!t.cpu_only){
        if(d->ops.texture_create_volume(d->ctx,w,h,depth,levels,&t.native)||!t.native){
            free(t.pixels);return D9_BACKEND;
        }
    }
    D9Handle key=d->next;
    if(register_subtexture(d,key,t.pixels,t.cpu_bytes,t.native,t.gpu_bytes,t.cpu_only,
                           cache_release_gpu))
        return D9_BACKEND;
    t.id=d->next++;*slot=t;*out=t.id;return D9_OK;
}
unsigned d9_volume_levels(D9Device *d,D9Handle h){VolumeTexture *v=volume_texture(d,h);return v?v->levels:0;}
int d9_volume_level_info(D9Device *d,D9Handle h,unsigned level,
                         unsigned *w,unsigned *height,unsigned *depth,
                         size_t *pitch,size_t *plane_bytes,size_t *bytes){
    VolumeTexture *v=volume_texture(d,h);
    if(!v||level>=v->levels)return D9_INVALID;
    const VolumeLevel *l=&v->level[level];
    if(w)*w=l->w;if(height)*height=l->h;if(depth)*depth=l->depth;
    if(pitch)*pitch=l->pitch;if(plane_bytes)*plane_bytes=l->plane_bytes;if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_volume_level_view(D9Device *d,D9Handle h,unsigned level,void **pixels,
                         unsigned *w,unsigned *height,unsigned *depth,
                         size_t *pitch,size_t *plane_bytes,size_t *bytes){
    VolumeTexture *v=volume_texture(d,h);
    if(!v||level>=v->levels||!v->pixels)return D9_INVALID;
    const VolumeLevel *l=&v->level[level];
    if(pixels)*pixels=v->pixels+l->offset;
    if(w)*w=l->w;if(height)*height=l->h;if(depth)*depth=l->depth;
    if(pitch)*pitch=l->pitch;if(plane_bytes)*plane_bytes=l->plane_bytes;if(bytes)*bytes=l->bytes;
    return D9_OK;
}
int d9_volume_upload_level(D9Device *d,D9Handle h,unsigned level,
                           const void *src,size_t src_pitch,size_t src_slice_pitch,
                           size_t src_bytes){
    VolumeTexture *v=volume_texture(d,h);
    if(!v||level>=v->levels||!src||!src_pitch||!src_slice_pitch||d->scene||d->teardown)
        return D9_INVALID;
    VolumeLevel *l=&v->level[level];
    unsigned bb=d9_bc_block_bytes(v->format);
    size_t rows=bb?(size_t)((l->h+3u)/4u):(size_t)l->h;
    size_t row_bytes=bb?(size_t)((l->w+3u)/4u)*bb:(size_t)l->w*4u;
    if(src_pitch<row_bytes||src_slice_pitch<l->plane_bytes)return D9_INVALID;
    if((rows-1u)>(SIZE_MAX-row_bytes)/src_pitch||
       (rows-1u)*src_pitch+row_bytes>src_slice_pitch)return D9_INVALID;
    if((l->depth-1u)>(SIZE_MAX-src_slice_pitch)/src_slice_pitch||
       (l->depth-1u)*src_slice_pitch+src_slice_pitch>src_bytes)return D9_INVALID;
    uint8_t *dst=v->pixels+l->offset;
    for(unsigned z=0;z<l->depth;++z){
        const uint8_t *sp=(const uint8_t *)src+(size_t)z*src_slice_pitch;
        uint8_t *dp=dst+(size_t)z*l->plane_bytes;
        for(size_t y=0;y<rows;++y)
            memcpy(dp+y*l->pitch,sp+y*src_pitch,row_bytes);
    }
    if(!v->cpu_only){
        if(!subtexture_has_gpu(d,h)){
            set_diagnosis(d,"volume texture native copy is not resident; CPU pixels are "
                            "retained but volume rehydration is not implemented");
            return D9_UNSUPPORTED;
        }
        if(d->ops.texture_upload_volume(d->ctx,v->native,level,dst,l->bytes))
            return D9_BACKEND;
    }
    l->uploaded=1;
    if(!v->cpu_only&&level==0)
        return subtexture_commit_ready(d,h,v->native,v->gpu_bytes);
    return D9_OK;
}
int d9_volume_level_unlocked(D9Device *d,D9Handle h,unsigned level){
    VolumeTexture *v=volume_texture(d,h);
    if(!v||level>=v->levels)return D9_INVALID;
    VolumeLevel *l=&v->level[level];
    if(!v->cpu_only){
        if(!subtexture_has_gpu(d,h)){
            set_diagnosis(d,"volume texture native copy is not resident; CPU pixels are "
                            "retained but volume rehydration is not implemented");
            return D9_UNSUPPORTED;
        }
        if(d->ops.texture_upload_volume(d->ctx,v->native,level,v->pixels+l->offset,l->bytes))
            return D9_BACKEND;
    }
    l->uploaded=1;
    if(!v->cpu_only&&level==0)
        return subtexture_commit_ready(d,h,v->native,v->gpu_bytes);
    return D9_OK;
}
int d9_release_volume_texture(D9Device *d,D9Handle h){
    VolumeTexture *v=volume_texture(d,h);
    if(!v||d->scene||d->teardown)return D9_INVALID;
    int rc=d9_texcache_remove(&d->tex_cache,h);
    if(rc!=D9TEXCACHE_OK){
        if(rc==D9TEXCACHE_RELEASE_FAILED){
            D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
            if(e&&!e->has_guest){v->pixels=NULL;v->cpu_bytes=0;}
            if(e&&!e->has_gpu){v->native=NULL;v->gpu_bytes=0;
                if(d->volume_texture==h)d->volume_texture=0;
                if(d->volume_texture1==h)d->volume_texture1=0;}
            return D9_RELEASE_FAILED;
        }
        return D9_BACKEND;
    }
    if(d->volume_texture==h)d->volume_texture=0;
    if(d->volume_texture1==h)d->volume_texture1=0;
    memset(v,0,sizeof *v);
    return D9_OK;
}
int d9_volume_resource_info(D9Device *d,D9Handle h,unsigned *levels,
                            unsigned *w,unsigned *height,unsigned *depth,
                            unsigned *format,size_t *guest_bytes,
                            size_t *gpu_bytes,int *cpu_only){
    VolumeTexture *v=volume_texture(d,h);if(!v)return D9_INVALID;
    D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
    if(levels)*levels=v->levels;if(w)*w=v->w;if(height)*height=v->h;if(depth)*depth=v->depth;
    if(format)*format=v->format;
    if(guest_bytes)*guest_bytes=e&&e->has_guest?e->guest_bytes:0;
    if(gpu_bytes)*gpu_bytes=e&&e->has_gpu?e->gpu_bytes:0;
    if(cpu_only)*cpu_only=v->cpu_only;return D9_OK;
}
int d9_bind_volume_texture(D9Device *d,unsigned stage,D9Handle h){
    if(!d||stage>=d9_stage_count(d)){
        if(d)set_diagnosis(d,"SetTexture: unsupported texture unit");
        return D9_UNSUPPORTED;
    }
    if(h){
        VolumeTexture *v=volume_texture(d,h);
        if(!v||v->cpu_only||!v->native||!v->level[0].uploaded)return D9_INVALID;
        D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);
        if(!e||!e->has_gpu||!e->gpu_ready){
            set_diagnosis(d,"volume texture is not cache-resident; volume rehydration is not implemented");
            return D9_INVALID;
        }
        d9_texcache_touch(&d->tex_cache,h);
    }
    if(stage){d->volume_texture1=h;d->texture1=0;d->cube_texture1=0;}
    else{d->volume_texture=h;d->texture=0;d->cube_texture=0;}
    return D9_OK;
}
static Target *target(D9Device *d,D9Handle h){if(d&&h)for(unsigned i=0;i<MAX_TARGETS;++i)if(d->targets[i].id==h)return &d->targets[i];return NULL;}
static Depth *depth_slot(D9Device *d,D9Handle h);
static unsigned bounded_native_dim(unsigned v,unsigned cap){
    return v>cap?cap:v;
}
/* bounded=0 is the retained back buffer: it is already created at its native
 * power-of-two size and keeps the existing corner-viewport/present path.
 * bounded=1 is an explicit render target or render-target texture; its logical
 * size is preserved while native storage is capped per axis. */
static int create_target(D9Device *d,unsigned w,unsigned h,unsigned format,int lockable,
                         int bounded,D9Handle *out){
    if(out)*out=0;
    if(!d||d->scene||d->teardown||!out||!d->next||w<8||h<8||w>1024||h>1024||
       (w&(w-1))||(h&(h-1))||(format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8)||
       !d->ops.target_create||!d->ops.target_delete||!d->ops.target_bind||!d->ops.target_clear||
       (lockable&&!d->ops.target_read))return D9_INVALID;
    unsigned nw=bounded?bounded_native_dim(w,d->target_native_max):w;
    unsigned nh=bounded?bounded_native_dim(h,d->target_native_max):h;
    if((nw<w||nh<h)&&!d->ops.target_viewport_scaled){
        char why[160];
        snprintf(why,sizeof why,"CreateRenderTarget: backend cannot scale %ux%u logical to %ux%u native; no target_viewport_scaled",w,h,nw,nh);
        set_diagnosis(d,why);
        return D9_UNSUPPORTED;
    }
    if(lockable&&(nw<w||nh<h)&&!d->scaled_readback){
        char why[200];
        snprintf(why,sizeof why,"CreateRenderTarget: lockable %ux%u would need %ux%u native scaled readback; set TESV_D3D9_SCALED_READBACK=1",w,h,nw,nh);
        set_diagnosis(d,why);
        return D9_UNSUPPORTED;
    }
    if(lockable&&d->scaled_readback&&(nw<w||nh<h)&&!d->ops.target_write){
        set_diagnosis(d,"CreateRenderTarget: scaled readback requires backend target_write");
        return D9_UNSUPPORTED;
    }
    size_t bytes=(size_t)nw*nh*4;if(budget_has_room(d,bytes))return D9_MEMORY;
    Target *slot=NULL;for(unsigned i=0;i<MAX_TARGETS;++i)if(!d->targets[i].id){slot=&d->targets[i];break;}
    if(!slot)return D9_MEMORY;void *native=NULL;
    if(d->ops.target_create(d->ctx,nw,nh,format,&native)||!native){
        if(native)d->ops.target_delete(d->ctx,native);
        char why[160];
        snprintf(why,sizeof why,"CreateRenderTarget: backend could not allocate a %ux%u native target",nw,nh);
        set_diagnosis(d,why);
        return D9_BACKEND;
    }
    D9Handle id=d->next++;*slot=(Target){id,w,h,nw,nh,format,lockable?1:0,bytes,native};
    d->target_used+=bytes;*out=id;return D9_OK;
}
int d9_create_render_target(D9Device *d,unsigned w,unsigned h,unsigned format,int lockable,D9Handle *out){
    return create_target(d,w,h,format,lockable,1,out);
}
int d9_create_render_texture(D9Device *d,unsigned w,unsigned h,unsigned format,
                             unsigned pool,unsigned usage,D9Handle *out){
    if(out)*out=0;
    if(!d||d->scene||d->teardown||!out||!d->next||pool!=D9_POOL_DEFAULT||
       usage!=D9_USAGE_RENDERTARGET||(format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8))
        return D9_INVALID;
    if(!d->ops.target_texture){
        if(out)*out=0;
        set_diagnosis(d,"CreateTexture: backend has no sampleable render-target texture view");
        return D9_UNSUPPORTED;
    }
    D9Handle tgt=0;int rc=d9_create_render_target(d,w,h,format,0,&tgt);
    if(rc)return rc;
    Target *tg=target(d,tgt);
    Texture *slot=NULL;
    for(unsigned i=0;i<MAX_TEXTURES;++i)if(!d->textures[i].id){slot=&d->textures[i];break;}
    if(!tg||!slot){d9_release_render_target(d,tgt);return D9_MEMORY;}
    void *sample=d->ops.target_texture(d->ctx,tg->native);
    if(!sample){
        d9_release_render_target(d,tgt);
        set_diagnosis(d,"CreateTexture: backend target_texture() returned no sampleable view");
        return D9_BACKEND;
    }
    Texture t;memset(&t,0,sizeof t);
    t.w=w;t.h=h;t.format=format;t.levels=1;t.gpu_levels=1;t.pool=pool;t.usage=usage;
    t.gpu_only=1;t.native=sample;t.rt_target=tgt;
    t.level[0]=(Level){w,h,(size_t)w*4u,(size_t)w*h*4u,0,1};
    t.id=d->next++;*slot=t;*out=t.id;return D9_OK;
}
/* Program the backend viewport for a target.  The retained back buffer keeps
 * its logical 400x240 corner (Present samples that rect).  Explicit bounded
 * targets map the full logical surface through a native 512x512 viewpoint, so
 * NDC still covers the whole image instead of clipping at native_w/h. */
static int apply_target_viewport(D9Device *d,Target *t){
    if(!t)return D9_OK;
    if(t->id==d->back_target)
        return d->ops.target_viewport&&d->ops.target_viewport(d->ctx,d->back_w,d->back_h)?D9_BACKEND:D9_OK;
    if(t->native_w!=t->w||t->native_h!=t->h){
        if(!d->ops.target_viewport_scaled){
            set_diagnosis(d,"SetRenderTarget: backend cannot scale this bounded native target");
            return D9_UNSUPPORTED;
        }
        return d->ops.target_viewport_scaled(d->ctx,t->w,t->h,t->native_w,t->native_h)?D9_BACKEND:D9_OK;
    }
    return d->ops.target_viewport&&d->ops.target_viewport(d->ctx,t->w,t->h)?D9_BACKEND:D9_OK;
}
int d9_set_render_target(D9Device *d,D9Handle h){
    if(!d||d->teardown||(h&&!target(d,h))||!d->ops.target_bind)return D9_INVALID;
    /* Handle zero means the default back buffer: the retained target when
     * D3DPRESENTFLAG_LOCKABLE_BACKBUFFER created one, otherwise the screen. */
    Target *t=h?target(d,h):(d->back_target?target(d,d->back_target):NULL);
    /* A bound depth-stencil describes the single depth attachment of the
     * active target.  Switching to a target with another size would silently
     * clear/test a different attachment; refuse and require an explicit
     * SetDepthStencilSurface(NULL) or a matching surface. */
    if(d->depth){
        unsigned nw,nh;
        if(t){nw=t->id==d->back_target?d->back_w:t->w;nh=t->id==d->back_target?d->back_h:t->h;}
        else if(d->back_valid){nw=d->back_w;nh=d->back_h;}
        else return D9_INVALID;
        Depth *s=depth_slot(d,d->depth);
        if(!s||s->w!=nw||s->h!=nh){
            set_diagnosis(d,"SetRenderTarget: the bound depth-stencil surface does not match the new render target size");
            return D9_UNSUPPORTED;
        }
    }
    void *native=t?t->native:NULL;
    if(d->scene){
        if(d->ops.target_bind(d->ctx,native))return D9_BACKEND;
        int vrc=apply_target_viewport(d,t);
        if(vrc)return vrc;
    }
    d->target=h;return D9_OK;
}
D9Handle d9_get_render_target(const D9Device *d){return d?d->target:0;}
int d9_render_target_info(D9Device *d,D9Handle h,unsigned *w,unsigned *height,
                          unsigned *format,unsigned *lockable,size_t *bytes){
    Target *t=target(d,h);if(!t)return D9_INVALID;
    if(w)*w=t->w;if(height)*height=t->h;if(format)*format=t->format;
    if(lockable)*lockable=t->lockable;if(bytes)*bytes=t->bytes;return D9_OK;
}
int d9_render_target_storage_info(D9Device *d,D9Handle h,unsigned *w,unsigned *height){
    Target *t=target(d,h);if(!t)return D9_INVALID;
    if(w)*w=t->native_w;if(height)*height=t->native_h;return D9_OK;
}
unsigned d9_target_native_max(const D9Device *d){
    return d?d->target_native_max:0;
}
int d9_scaled_readback_enabled(const D9Device *d){
    return d?d->scaled_readback:0;
}
static unsigned readback_nearest_native(unsigned logical_pos,unsigned logical_dim,unsigned native_dim){
    if(!native_dim)return 0;
    uint64_t v=((uint64_t)logical_pos*2u+1u)*(uint64_t)native_dim;
    unsigned n=(unsigned)(v/((uint64_t)logical_dim*2u));
    return n<native_dim?n:native_dim-1;
}
static unsigned writeback_native_start(unsigned logical_pos,unsigned logical_dim,unsigned native_dim){
    if(!native_dim||!logical_dim)return 0;
    if(native_dim>=logical_dim)return logical_pos;
    return (unsigned)((uint64_t)logical_pos*native_dim/logical_dim);
}
static unsigned writeback_native_end(unsigned logical_end,unsigned logical_dim,unsigned native_dim){
    if(!native_dim||!logical_dim)return 0;
    if(native_dim>=logical_dim)return logical_end;
    uint64_t v=(uint64_t)logical_end*native_dim;
    return (unsigned)((v+logical_dim-1)/logical_dim);
}
static unsigned writeback_nearest_logical(unsigned native_pos,unsigned native_dim,unsigned logical_dim){
    if(!native_dim)return 0;
    uint64_t v=((uint64_t)native_pos*2u+1u)*(uint64_t)logical_dim;
    unsigned l=(unsigned)(v/((uint64_t)native_dim*2u));
    return l<logical_dim?l:logical_dim-1;
}
int d9_render_target_read(D9Device *d,D9Handle h,void *dst,size_t dst_pitch,
                          unsigned x,unsigned y,unsigned w,unsigned height){
    Target *t=target(d,h);if(!t||!d||!dst||!dst_pitch||d->scene)return D9_INVALID;
    if(t->native_w!=t->w||t->native_h!=t->h){
        if(!d->scaled_readback){
            char why[220];
            snprintf(why,sizeof why,"LockRect: bounded native target %ux%u is stored at %ux%u; scaled readback is lossy and disabled (set TESV_D3D9_SCALED_READBACK=1)",t->w,t->h,t->native_w,t->native_h);
            set_diagnosis(d,why);
            return D9_UNSUPPORTED;
        }
        if(!d->ops.target_read)return D9_BACKEND;
        if(!w||!height||x>t->w||y>t->h||w>t->w-x||height>t->h-y||dst_pitch<(size_t)w*4)
            return D9_INVALID;
        size_t npitch=(size_t)t->native_w*4,nbytes=npitch*t->native_h;
        uint8_t *native=malloc(nbytes);if(!native)return D9_MEMORY;
        if(d->ops.target_read(d->ctx,t->native,native,npitch,0,0,t->native_w,t->native_h)){
            free(native);return D9_BACKEND;
        }
        for(unsigned yy=0;yy<height;++yy){
            unsigned sy=readback_nearest_native(y+yy,t->h,t->native_h);
            for(unsigned xx=0;xx<w;++xx){
                unsigned sx=readback_nearest_native(x+xx,t->w,t->native_w);
                memcpy((uint8_t *)dst+(size_t)yy*dst_pitch+(size_t)xx*4,
                       native+(size_t)sy*npitch+(size_t)sx*4,4);
            }
        }
        free(native);
        return D9_OK;
    }
    if(!d->ops.target_read)return D9_BACKEND;
    if(x>t->w||y>t->h||w>t->w-x||height>t->h-y)return D9_INVALID;
    return d->ops.target_read(d->ctx,t->native,dst,dst_pitch,x,y,w,height)?D9_BACKEND:D9_OK;
}
int d9_render_target_write(D9Device *d,D9Handle h,const void *src,size_t src_pitch,
                           unsigned x,unsigned y,unsigned w,unsigned height){
    Target *t=target(d,h);
    if(!t||!d||!src||!src_pitch||d->scene||d->teardown)return D9_INVALID;
    if(t->native_w!=t->w||t->native_h!=t->h){
        if(!d->scaled_readback){
            char why[220];
            snprintf(why,sizeof why,"UnlockRect: bounded native target %ux%u is stored at %ux%u; scaled writeback is lossy and disabled (set TESV_D3D9_SCALED_READBACK=1)",t->w,t->h,t->native_w,t->native_h);
            set_diagnosis(d,why);
            return D9_UNSUPPORTED;
        }
        if(!d->ops.target_read||!d->ops.target_write)return D9_BACKEND;
        if(!w||!height||x>t->w||y>t->h||w>t->w-x||height>t->h-y||src_pitch<(size_t)w*4)
            return D9_INVALID;
        size_t npitch=(size_t)t->native_w*4,nbytes=npitch*t->native_h;
        uint8_t *native=malloc(nbytes);if(!native)return D9_MEMORY;
        if(d->ops.target_read(d->ctx,t->native,native,npitch,0,0,t->native_w,t->native_h)){
            free(native);return D9_BACKEND;
        }
        unsigned nx0=writeback_native_start(x,t->w,t->native_w);
        unsigned nx1=writeback_native_end(x+w,t->w,t->native_w);
        unsigned ny0=writeback_native_start(y,t->h,t->native_h);
        unsigned ny1=writeback_native_end(y+height,t->h,t->native_h);
        if(nx1>t->native_w)nx1=t->native_w;
        if(ny1>t->native_h)ny1=t->native_h;
        for(unsigned ny=ny0;ny<ny1;++ny){
            unsigned ly=writeback_nearest_logical(ny,t->native_h,t->h);
            if(ly<y||ly>=y+height)continue;
            for(unsigned nx=nx0;nx<nx1;++nx){
                unsigned lx=writeback_nearest_logical(nx,t->native_w,t->w);
                if(lx<x||lx>=x+w)continue;
                memcpy(native+(size_t)ny*npitch+(size_t)nx*4,
                       (const uint8_t *)src+(size_t)(ly-y)*src_pitch+(size_t)(lx-x)*4,4);
            }
        }
        int rc=d->ops.target_write(d->ctx,t->native,native,npitch,t->native_w,t->native_h);
        free(native);
        return rc?D9_BACKEND:D9_OK;
    }
    if(!d->ops.target_read||!d->ops.target_write)return D9_BACKEND;
    if(!w||!height||x>t->w||y>t->h||w>t->w-x||height>t->h-y||src_pitch<(size_t)w*4)return D9_INVALID;
    size_t pitch=(size_t)t->w*4,bytes=pitch*t->h;
    /* PICA targets are at most 1024x1024 RGBA8 (4 MiB); anything larger is a
     * caller error rather than a silent allocation. */
    if(bytes>4u<<20)return D9_MEMORY;
    uint8_t *full=malloc(bytes);if(!full)return D9_MEMORY;
    if(d->ops.target_read(d->ctx,t->native,full,pitch,0,0,t->w,t->h)){
        free(full);return D9_BACKEND;
    }
    for(unsigned yy=0;yy<height;++yy)
        memcpy(full+(size_t)(y+yy)*pitch+(size_t)x*4,
               (const uint8_t *)src+(size_t)yy*src_pitch,(size_t)w*4);
    int rc=d->ops.target_write(d->ctx,t->native,full,pitch,t->w,t->h);
    free(full);
    return rc?D9_BACKEND:D9_OK;
}
int d9_release_render_target(D9Device *d,D9Handle h){Target *t=target(d,h);if(!t||!d||d->scene||d->teardown||h==d->back_target)return D9_INVALID;
    if(d->target==h)d->target=0;
    if(d->ops.target_delete)d->ops.target_delete(d->ctx,t->native);
    d->target_used-=t->bytes;memset(t,0,sizeof *t);return D9_OK;}
int d9_backend_supports_lockable_back_buffer(const D9Backend *ops){
    return ops&&ops->target_create&&ops->target_delete&&ops->target_bind&&
           ops->target_clear&&ops->target_read&&ops->target_write&&ops->target_blit&&
           ops->target_viewport;
}
static unsigned pow2_up(unsigned n){
    unsigned p=8;while(p<n&&p<1024)p<<=1;return p>=n&&p<=1024?p:0;
}
int d9_set_back_buffer_info_ex(D9Device *d,unsigned w,unsigned h,unsigned format,int lockable){
    if(!d||d->teardown||!w||!h||w>16384||h>16384||(format!=D9_X8R8G8B8&&format!=D9_A8R8G8B8))return D9_INVALID;
    if(d->scene)return D9_INVALID;
    D9Handle fresh=0;unsigned tw=0,th=0;
    if(lockable){
        if(!d9_backend_supports_lockable_back_buffer(&d->ops))return D9_UNSUPPORTED;
        tw=pow2_up(w);th=pow2_up(h);
        if(!tw||!th)return D9_UNSUPPORTED;
        /* Create the replacement first so a failed allocation leaves the old
         * back buffer intact.  The retained target is already at its native
         * power-of-two size, so it uses the unbounded path. */
        int rc=create_target(d,tw,th,format,1,0,&fresh);
        if(rc)return rc;
    }
    if(d->back_target){
        (void)d9_release_render_target(d,d->back_target);
        d->back_target=0;d->back_tex_w=d->back_tex_h=0;
    }
    d->back_w=w;d->back_h=h;d->back_format=format;d->back_valid=1;
    d->back_lockable=lockable?1:0;
    if(lockable){d->back_target=fresh;(void)d9_render_target_storage_info(d,fresh,&d->back_tex_w,&d->back_tex_h);}
    return D9_OK;
}
int d9_set_back_buffer_info(D9Device *d,unsigned w,unsigned h,unsigned format){
    return d9_set_back_buffer_info_ex(d,w,h,format,0);
}
D9Handle d9_back_buffer_target(const D9Device *d){return d?d->back_target:0;}
int d9_back_buffer_info_ex(const D9Device *d,unsigned *w,unsigned *h,
                           unsigned *format,unsigned *tex_w,unsigned *tex_h,
                           int *lockable){
    if(!d||!d->back_valid)return D9_INVALID;
    if(w)*w=d->back_w;if(h)*h=d->back_h;if(format)*format=d->back_format;
    if(tex_w)*tex_w=d->back_tex_w;if(tex_h)*tex_h=d->back_tex_h;
    if(lockable)*lockable=d->back_lockable;
    return D9_OK;
}
int d9_get_back_buffer_info(const D9Device *d,unsigned *w,unsigned *h,unsigned *format){
    if(!d||!d->back_valid)return D9_INVALID;
    if(w)*w=d->back_w;if(h)*h=d->back_h;if(format)*format=d->back_format;return D9_OK;
}
static int blit_rect(const D9Rect *r,unsigned w,unsigned h,D9BlitRegion *out){
    int left=0,top=0,right=(int)w,bottom=(int)h;
    if(r){left=r->left;top=r->top;right=r->right;bottom=r->bottom;}
    if(left<0||top<0||right<=left||bottom<=top)return D9_INVALID;
    if((uint64_t)right>w||(uint64_t)bottom>h)return D9_INVALID;
    out->x=(unsigned)left;out->y=(unsigned)top;
    out->width=(unsigned)(right-left);out->height=(unsigned)(bottom-top);
    return D9_OK;
}
int d9_blit(D9Device *d,D9Handle dst,const D9Rect *dst_rect,
            D9Handle src,const D9Rect *src_rect,unsigned filter){
    if(!d){return D9_INVALID;}
    d->blit_error=D9_BLIT_ARGUMENT;
    if(d->teardown||filter>D9_FILTER_LINEAR)return D9_INVALID;
    /* Handle zero is the back buffer.  With D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
     * that is the retained offscreen target; otherwise it stays the implicit
     * screen.  Present uses an explicit retained->screen region instead. */
    Target *st=src?target(d,src):(d->back_target?target(d,d->back_target):NULL);
    Target *dt=dst?target(d,dst):(d->back_target?target(d,d->back_target):NULL);
    if((src&&!st)||(dst&&!dt))return D9_INVALID;
    if(src==dst||(st&&st==dt)){d->blit_error=D9_BLIT_SAME_SURFACE;return D9_INVALID;}
    D9BlitRegion sreg={{0}},dreg={{0}};
    if(st){
        int retained=st->id==d->back_target;
        sreg.surface=(D9BlitSurface){st->native,
            retained?d->back_w:st->w,retained?d->back_h:st->h,st->format,
            retained?d->back_tex_w:st->native_w,retained?d->back_tex_h:st->native_h};
    }else{
        if(!d->back_valid)return D9_INVALID;
        sreg.surface=(D9BlitSurface){NULL,d->back_w,d->back_h,d->back_format,0,0};
    }
    if(dt){
        int retained=dt->id==d->back_target;
        dreg.surface=(D9BlitSurface){dt->native,
            retained?d->back_w:dt->w,retained?d->back_h:dt->h,dt->format,
            retained?d->back_tex_w:dt->native_w,retained?d->back_tex_h:dt->native_h};
    }else{
        if(!d->back_valid)return D9_INVALID;
        dreg.surface=(D9BlitSurface){NULL,d->back_w,d->back_h,d->back_format,0,0};
    }
    if(sreg.surface.format!=dreg.surface.format){d->blit_error=D9_BLIT_FORMAT;return D9_INVALID;}
    if(blit_rect(src_rect,sreg.surface.width,sreg.surface.height,&sreg)||
       blit_rect(dst_rect,dreg.surface.width,dreg.surface.height,&dreg)){
        d->blit_error=D9_BLIT_RECT;return D9_INVALID;
    }
    if(!d->ops.target_blit){d->blit_error=D9_BLIT_UNSUPPORTED;return D9_BACKEND;}
    int rc=d->ops.target_blit(d->ctx,&dreg,&sreg,filter);
    d->blit_error=rc==D9_BLIT_OK?D9_BLIT_OK:rc<0?rc:D9_BLIT_BACKEND;
    /* The backend is allowed to switch its native target, and the caller's
     * selected target must survive even a rejected copy. */
    if(d->scene&&d->ops.target_bind){
        Target *cur=d->target?target(d,d->target):
            (d->back_target?target(d,d->back_target):NULL);
        if(d->ops.target_bind(d->ctx,cur?cur->native:NULL)){d->blit_error=D9_BLIT_BACKEND;return D9_BACKEND;}
        int vrc=apply_target_viewport(d,cur);
        if(vrc){d->blit_error=D9_BLIT_BACKEND;return vrc;}
    }
    return rc==D9_BLIT_OK?D9_OK:D9_BACKEND;
}
int d9_blit_last_error(const D9Device *d){return d?d->blit_error:D9_BLIT_ARGUMENT;}
int d9_set_stream(D9Device *d,unsigned stream,D9Handle h,size_t offset,size_t stride){if(!d||stream>=D9_MAX_STREAMS)return D9_INVALID;
    Buffer *b=buffer(d,h);if(!h&&!offset&&!stride){d->streams[stream]=(Stream){0};return D9_OK;}
    if(!b||b->kind||b->locked||!stride||stride>4096||offset>=b->bytes)return D9_INVALID;d->streams[stream]=(Stream){h,offset,stride};return D9_OK;}
int d9_set_stream_source(D9Device *d,D9Handle h,size_t offset,size_t stride){return h&&stride<sizeof(D9Vertex)?D9_INVALID:d9_set_stream(d,0,h,offset,stride);}
int d9_set_vertex_declaration(D9Device *d,const D9Decl *decl){if(!d)return D9_INVALID;if(!decl){d->decl_active=0;return D9_OK;}
    if(d9_decl_init(&d->declaration,decl->elements,decl->count))return D9_INVALID;d->decl_active=1;return D9_OK;}
const char *d9_last_draw_diagnostic(const D9Device *d){return d?d->diagnosis:"";}
int d9_set_viewport_size(D9Device *d,unsigned width,unsigned height){
    if(!d||!width||!height||width>4096||height>4096)return D9_INVALID;
    d->view_w=width;d->view_h=height;set_diagnosis(d,"");return D9_OK;
}
static int stage_arg_ok(unsigned arg,unsigned allow_current){
    if(arg==D9_TA_DIFFUSE||arg==D9_TA_TEXTURE)return 1;
    if(arg==D9_TA_CURRENT&&allow_current)return 1;
    return 0;
}
int d9_set_texture_stage_state(D9Device *d,unsigned stage,unsigned type,unsigned value){
    if(!d)return D9_INVALID;
    if(stage>7){set_diagnosis(d,"SetTextureStageState: only stages 0..7 exist");return D9_UNSUPPORTED;}
    if(stage>=d9_stage_count(d)){
        if(type!=D9_TSS_COLOROP&&type!=D9_TSS_ALPHAOP){
            set_diagnosis(d,"SetTextureStageState: only COLOROP/ALPHAOP are accepted on stages 1..7");return D9_UNSUPPORTED;}
        if(value!=D9_TOP_DISABLE){set_diagnosis(d,"SetTextureStageState: stage 1..7 must be DISABLE");return D9_UNSUPPORTED;}
        set_diagnosis(d,"");return D9_OK;
    }
    D9DrawState *state=stage?&d->second:&d->state;
    switch(type){
    case D9_TSS_COLOROP:
    case D9_TSS_ALPHAOP:
        if(value!=D9_TOP_DISABLE&&value!=D9_TOP_SELECTARG1&&value!=D9_TOP_SELECTARG2&&value!=D9_TOP_MODULATE){
            set_diagnosis(d,"SetTextureStageState: only DISABLE/SELECTARG1/SELECTARG2/MODULATE are implemented");return D9_UNSUPPORTED;}
        if(type==D9_TSS_COLOROP)state->stage_colorop=value;else state->stage_alphaop=value;
        break;
    case D9_TSS_COLORARG1:
    case D9_TSS_COLORARG2:
    case D9_TSS_ALPHAARG1:
    case D9_TSS_ALPHAARG2:
        if(!stage_arg_ok(value,stage==1||type==D9_TSS_COLORARG2||type==D9_TSS_ALPHAARG2)){
            set_diagnosis(d,"SetTextureStageState: only DIFFUSE/TEXTURE (and CURRENT in ARG2) are implemented");return D9_UNSUPPORTED;}
        if(type==D9_TSS_COLORARG1)state->stage_colorarg1=value;
        else if(type==D9_TSS_COLORARG2)state->stage_colorarg2=value;
        else if(type==D9_TSS_ALPHAARG1)state->stage_alphaarg1=value;
        else state->stage_alphaarg2=value;
        break;
    case D9_TSS_TEXCOORDINDEX:
        if((stage==1&&value!=0)||value>7){set_diagnosis(d,"SetTextureStageState: stage 1 supports only explicit TEXCOORDINDEX 0; stage 0 accepts indices 0..7");return D9_UNSUPPORTED;}
        state->stage_texcoordindex=value;break;
    default:
        set_diagnosis(d,"SetTextureStageState: only COLOROP/COLORARG1/COLORARG2/ALPHAOP/ALPHAARG1/ALPHAARG2/TEXCOORDINDEX are implemented");
        return D9_UNSUPPORTED;
    }
    set_diagnosis(d,"");return D9_OK;
}
int d9_get_texture_stage_state(D9Device *d,unsigned stage,unsigned type,unsigned *value){
    if(!d||!value)return D9_INVALID;
    if(stage>7){set_diagnosis(d,"GetTextureStageState: only stages 0..7 exist");return D9_UNSUPPORTED;}
    if(stage>=d9_stage_count(d)){
        if(type!=D9_TSS_COLOROP&&type!=D9_TSS_ALPHAOP){
            set_diagnosis(d,"GetTextureStageState: only COLOROP/ALPHAOP exist on stages 1..7");return D9_UNSUPPORTED;}
        *value=D9_TOP_DISABLE;set_diagnosis(d,"");return D9_OK;
    }
    const D9DrawState *state=stage?&d->second:&d->state;
    switch(type){
    case D9_TSS_COLOROP:*value=state->stage_colorop;break;
    case D9_TSS_ALPHAOP:*value=state->stage_alphaop;break;
    case D9_TSS_COLORARG1:*value=state->stage_colorarg1;break;
    case D9_TSS_COLORARG2:*value=state->stage_colorarg2;break;
    case D9_TSS_ALPHAARG1:*value=state->stage_alphaarg1;break;
    case D9_TSS_ALPHAARG2:*value=state->stage_alphaarg2;break;
    case D9_TSS_TEXCOORDINDEX:*value=state->stage_texcoordindex;break;
    default:
        set_diagnosis(d,"GetTextureStageState: only COLOROP/COLORARG1/COLORARG2/ALPHAOP/ALPHAARG1/ALPHAARG2/TEXCOORDINDEX are implemented");
        return D9_UNSUPPORTED;
    }
    set_diagnosis(d,"");return D9_OK;
}
int d9_set_indices(D9Device *d,D9Handle h){Buffer *b=d?buffer(d,h):NULL;if(d&&!h){d->index=0;return D9_OK;}if(!b||!b->kind||b->locked)return D9_INVALID;d->index=h;return D9_OK;}
unsigned d9_stage_count(const D9Device *d){return d&&d->ops.draw_two?2:1;}
int d9_set_texture(D9Device *d,D9Handle h){return d9_bind_texture(d,0,h);}
int d9_bind_texture(D9Device *d,unsigned stage,D9Handle h){
    if(stage>=d9_stage_count(d)){if(d)set_diagnosis(d,"SetTexture: unsupported texture unit");return D9_UNSUPPORTED;}
    Texture *t=d?texture(d,h):NULL;if(!d||d->teardown||(h&&(!t||t->locked||!t->native||!t->level[0].uploaded)))return D9_INVALID;
    if(stage)d->cube_texture1=0;else d->cube_texture=0;
    if(stage)d->volume_texture1=0;else d->volume_texture=0;
    if(h&&!t->rt_target){D9TexEntry *e=d9_texcache_find(&d->tex_cache,h);if(!e||!e->has_gpu||!e->gpu_ready)return D9_INVALID;d9_texcache_touch(&d->tex_cache,h);}
    if(stage)d->texture1=h;else d->texture=h;return D9_OK;}
int d9_set_pixel_shader(D9Device *d,const D9TevProgram *program){if(!d)return D9_INVALID;d->pixel_shader=program;return D9_OK;}
int d9_set_tev_constants(D9Device *d,const float values[9][4]){
    if(!d||!values)return D9_INVALID;
    for(unsigned i=0;i<9;++i)for(unsigned j=0;j<4;++j)
        if(!isfinite(values[i][j]))return D9_INVALID;
    memcpy(d->tev_constants,values,sizeof d->tev_constants);
    /* SetPixelShaderConstantF is the authoritative pixel-shader bank.  The
     * legacy VS/ambient upload must not silently undo those writes. */
    for(unsigned i=0;i<D9_PS_CONST_F_REGS;++i)
        if(d->ps_constants_f_mask&(1u<<i))
            memcpy(d->tev_constants[i],d->ps_constants[i],sizeof d->ps_constants[i]);
    return D9_OK;
}
int d9_get_tev_constants(const D9Device *d,float out[9][4]){
    if(!d||!out)return D9_INVALID;
    memcpy(out,d->tev_constants,sizeof d->tev_constants);
    return D9_OK;
}
static int const_range(const D9Device *d,const char *method,const char *label,
                       unsigned start,unsigned count,unsigned limit){
    if(start>limit||count>limit-start){
        char why[160];
        snprintf(why,sizeof why,"%s: c%u + %u is outside the %u-register %s window",
                 method,start,count,limit,label);
        set_diagnosis((D9Device *)d,why);
        return D9_INVALID;
    }
    return D9_OK;
}
static int ps_range(const D9Device *d,const char *method,unsigned start,
                    unsigned count,unsigned limit){
    return const_range(d,method,"PS",start,count,limit);
}
static int vs_range(const D9Device *d,const char *method,unsigned start,
                    unsigned count,unsigned limit){
    return const_range(d,method,"VS",start,count,limit);
}
int d9_set_ps_constants_f(D9Device *d,unsigned start,const float *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"SetPixelShaderConstantF",start,count,D9_PS_CONST_F_REGS);
    if(rc)return rc;
    if(!count)return D9_OK;
    if(!values){set_diagnosis(d,"SetPixelShaderConstantF: constant data is not readable");return D9_INVALID;}
    for(unsigned i=0;i<count*4u;++i)
        if(!isfinite(values[i])){
            set_diagnosis(d,"SetPixelShaderConstantF: non-finite constant value");
            return D9_INVALID;
        }
    memcpy(&d->ps_constants[start][0],values,(size_t)count*16);
    d->ps_constants_f_mask|=((1u<<count)-1u)<<start;
    for(unsigned i=0;i<count;++i)
        memcpy(d->tev_constants[start+i],d->ps_constants[start+i],16);
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_get_ps_constants_f(const D9Device *d,unsigned start,float *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"GetPixelShaderConstantF",start,count,D9_PS_CONST_F_REGS);
    if(rc)return rc;
    if(count&&!values)return D9_INVALID;
    if(count)memcpy(values,&d->ps_constants[start][0],(size_t)count*16);
    return D9_OK;
}
int d9_set_ps_constants_i(D9Device *d,unsigned start,const int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"SetPixelShaderConstantI",start,count,D9_PS_CONST_I_REGS);
    if(rc)return rc;
    if(!count)return D9_OK;
    if(!values){set_diagnosis(d,"SetPixelShaderConstantI: constant data is not readable");return D9_INVALID;}
    memcpy(&d->ps_constants_i[start][0],values,(size_t)count*16);
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_get_ps_constants_i(const D9Device *d,unsigned start,int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"GetPixelShaderConstantI",start,count,D9_PS_CONST_I_REGS);
    if(rc)return rc;
    if(count&&!values)return D9_INVALID;
    if(count)memcpy(values,&d->ps_constants_i[start][0],(size_t)count*16);
    return D9_OK;
}
int d9_set_ps_constants_b(D9Device *d,unsigned start,const int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"SetPixelShaderConstantB",start,count,D9_PS_CONST_B_REGS);
    if(rc)return rc;
    if(!count)return D9_OK;
    if(!values){set_diagnosis(d,"SetPixelShaderConstantB: constant data is not readable");return D9_INVALID;}
    /* D3D9 BOOL constants are normalised on write; Get returns 0/1, never the
     * original nonzero bits. */
    for(unsigned i=0;i<count;++i)d->ps_constants_b[start+i]=values[i]?1u:0u;
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_get_ps_constants_b(const D9Device *d,unsigned start,int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=ps_range(d,"GetPixelShaderConstantB",start,count,D9_PS_CONST_B_REGS);
    if(rc)return rc;
    if(count&&!values)return D9_INVALID;
    for(unsigned i=0;i<count;++i)values[i]=d->ps_constants_b[start+i];
    return D9_OK;
}
int d9_set_vs_constants_i(D9Device *d,unsigned start,const int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=vs_range(d,"SetVertexShaderConstantI",start,count,D9_VS_CONST_I_REGS);
    if(rc)return rc;
    if(!count)return D9_OK;
    if(!values){set_diagnosis(d,"SetVertexShaderConstantI: constant data is not readable");return D9_INVALID;}
    memcpy(&d->vs_constants_i[start][0],values,(size_t)count*16);
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_get_vs_constants_i(const D9Device *d,unsigned start,int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=vs_range(d,"GetVertexShaderConstantI",start,count,D9_VS_CONST_I_REGS);
    if(rc)return rc;
    if(count&&!values)return D9_INVALID;
    if(count)memcpy(values,&d->vs_constants_i[start][0],(size_t)count*16);
    return D9_OK;
}
int d9_set_vs_constants_b(D9Device *d,unsigned start,const int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=vs_range(d,"SetVertexShaderConstantB",start,count,D9_VS_CONST_B_REGS);
    if(rc)return rc;
    if(!count)return D9_OK;
    if(!values){set_diagnosis(d,"SetVertexShaderConstantB: constant data is not readable");return D9_INVALID;}
    for(unsigned i=0;i<count;++i)d->vs_constants_b[start+i]=values[i]?1u:0u;
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_get_vs_constants_b(const D9Device *d,unsigned start,int32_t *values,unsigned count){
    if(!d)return D9_INVALID;
    int rc=vs_range(d,"GetVertexShaderConstantB",start,count,D9_VS_CONST_B_REGS);
    if(rc)return rc;
    if(count&&!values)return D9_INVALID;
    for(unsigned i=0;i<count;++i)values[i]=d->vs_constants_b[start+i];
    return D9_OK;
}
unsigned d9_ps_constants_f_mask(const D9Device *d){return d?d->ps_constants_f_mask&0xFFu:0u;}
int d9_set_transform(D9Device *d,unsigned which,const D9Matrix *m){if(!d||!m||which>2)return D9_INVALID;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)if(!isfinite(m->m[i][j]))return D9_INVALID;d->transforms[which]=*m;return D9_OK;}
int d9_set_clip_plane(D9Device *d,unsigned index,const float plane[4]){
    if(!d||!plane||index>=D9_MAX_CLIP_PLANES)return D9_INVALID;
    for(unsigned i=0;i<4;++i)if(!isfinite(plane[i]))return D9_INVALID;
    memcpy(d->clip_planes[index],plane,16);return D9_OK;
}
int d9_get_clip_plane(const D9Device *d,unsigned index,float plane[4]){
    if(!d||!plane||index>=D9_MAX_CLIP_PLANES)return D9_INVALID;
    memcpy(plane,d->clip_planes[index],16);return D9_OK;
}
int d9_set_shader_clip(D9Device *d,const D9Matrix *m){if(!d)return D9_INVALID;if(!m){d->shader_active=0;return D9_OK;}
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)if(!isfinite(m->m[i][j]))return D9_INVALID;
    for(unsigned i=0;i<4;++i){d->shader_clip.m[0][i]=m->m[1][i];d->shader_clip.m[1][i]=-m->m[0][i];
        d->shader_clip.m[2][i]=m->m[2][i]-m->m[3][i];d->shader_clip.m[3][i]=m->m[3][i];}d->shader_active=1;return D9_OK;}
/* --- test-only CPU T&L prototype ----------------------------------------- */
void d9_tnl_defaults(D9TnlState *s){
    if(!s)return;
    memset(s,0,sizeof *s);
    /* D3D9 fixed-function defaults: lighting and vertex colour on, global
     * ambient black, material diffuse/ambient white, no enabled light. */
    s->lighting=1;s->color_vertex=1;
    s->diffuse_source=D9_MCS_COLOR1;
    s->ambient_source=D9_MCS_MATERIAL;
    s->specular_source=D9_MCS_COLOR2;
    s->emissive_source=D9_MCS_MATERIAL;
    for(unsigned i=0;i<4;++i){s->material.diffuse[i]=1.0f;s->material.ambient[i]=1.0f;}
    for(unsigned i=0;i<D9_TNL_MAX_LIGHTS;++i)s->lights[i].type=D9_LIGHT_POINT;
}
int d9_set_tnl_proto(D9Device *d,int enabled){
    if(!d)return D9_INVALID;
    d->tnl_proto=enabled?1:0;
    if(d->tnl_proto)d9_tnl_defaults(&d->tnl);
    return D9_OK;
}
int d9_tnl_proto(const D9Device *d){return d?d->tnl_proto:0;}
static int light_type_ok(unsigned type){
    if(type==D9_LIGHT_POINT||type==D9_LIGHT_DIRECTIONAL)return 1;
    return type==D9_LIGHT_SPOT?2:0;
}
static int light_values_finite(const D9LightState *l){
    for(unsigned i=0;i<4;++i)
        if(!isfinite(l->diffuse[i])||!isfinite(l->specular[i])||!isfinite(l->ambient[i]))return 0;
    for(unsigned i=0;i<3;++i)
        if(!isfinite(l->position[i])||!isfinite(l->direction[i]))return 0;
    return isfinite(l->range)&&isfinite(l->falloff)&&isfinite(l->attenuation0)&&
           isfinite(l->attenuation1)&&isfinite(l->attenuation2)&&isfinite(l->theta)&&isfinite(l->phi);
}
int d9_set_light(D9Device *d,unsigned index,const D9LightState *light){
    if(!d||!d->tnl_proto||!light||index>=D9_TNL_MAX_LIGHTS)return D9_INVALID;
    int ok=light_type_ok(light->type);
    if(ok!=1||!light_values_finite(light))return ok==2?D9_UNSUPPORTED:D9_INVALID;
    d->tnl.lights[index]=*light;return D9_OK;
}
int d9_get_light(const D9Device *d,unsigned index,D9LightState *out){
    if(!d||!d->tnl_proto||!out||index>=D9_TNL_MAX_LIGHTS)return D9_INVALID;
    *out=d->tnl.lights[index];return D9_OK;
}
int d9_set_light_enable(D9Device *d,unsigned index,int enable){
    if(!d||!d->tnl_proto||index>=D9_TNL_MAX_LIGHTS)return D9_INVALID;
    unsigned bit=1u<<index;if(enable)d->tnl.enabled|=bit;else d->tnl.enabled&=~bit;return D9_OK;
}
int d9_get_light_enable(const D9Device *d,unsigned index,int *enable){
    if(!d||!d->tnl_proto||!enable||index>=D9_TNL_MAX_LIGHTS)return D9_INVALID;
    *enable=(d->tnl.enabled>>index)&1u;return D9_OK;
}
int d9_set_material(D9Device *d,const D9MaterialState *m){
    if(!d||!d->tnl_proto||!m)return D9_INVALID;
    for(unsigned i=0;i<4;++i)
        if(!isfinite(m->diffuse[i])||!isfinite(m->ambient[i])||
           !isfinite(m->specular[i])||!isfinite(m->emissive[i]))return D9_INVALID;
    if(!isfinite(m->power)||m->power<0.0f)return D9_INVALID;
    d->tnl.material=*m;return D9_OK;
}
int d9_get_material(const D9Device *d,D9MaterialState *m){
    if(!d||!d->tnl_proto||!m)return D9_INVALID;
    *m=d->tnl.material;return D9_OK;
}
int d9_get_tnl_state(const D9Device *d,D9TnlState *out){
    if(!d||!out)return D9_INVALID;
    *out=d->tnl;return D9_OK;
}
int d9_set_tnl_state(D9Device *d,const D9TnlState *state){
    if(!d||!state)return D9_INVALID;
    if(state->enabled&~((1u<<D9_TNL_MAX_LIGHTS)-1u)||
       state->lighting>1u||state->color_vertex>1u||state->normalize_normals>1u||
       state->local_viewer>1u||state->specular_enable>1u||
       state->diffuse_source>D9_MCS_COLOR2||state->ambient_source>D9_MCS_COLOR2||
       state->specular_source>D9_MCS_COLOR2||state->emissive_source>D9_MCS_COLOR2)
        return D9_INVALID;
    for(unsigned i=0;i<4;++i)
        if(!isfinite(state->material.diffuse[i])||!isfinite(state->material.ambient[i])||
           !isfinite(state->material.specular[i])||!isfinite(state->material.emissive[i]))
            return D9_INVALID;
    if(!isfinite(state->material.power)||state->material.power<0.0f)return D9_INVALID;
    for(unsigned i=0;i<D9_TNL_MAX_LIGHTS;++i){
        int ok=light_type_ok(state->lights[i].type);
        if(ok!=1||!light_values_finite(&state->lights[i]))return D9_INVALID;
    }
    d->tnl=*state;return D9_OK;
}
int d9_multiply_transform(D9Device *d,unsigned which,const D9Matrix *m){
    if(!d||!d->tnl_proto||!m||which>2)return D9_INVALID;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)if(!isfinite(m->m[i][j]))return D9_INVALID;
    D9Matrix out;d9_matrix_multiply(&out,&d->transforms[which],m);d->transforms[which]=out;return D9_OK;
}
/* Row-vector normal matrix: transpose(inverse(world 3x3)).  Returns 0 on
 * success, -1 when the world matrix is singular. */
static int tnl_normal_matrix(const D9Matrix *world,float nm[3][3]){
    float a[3][3];for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)a[i][j]=world->m[i][j];
    float det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
             -a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
             +a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    if(!isfinite(det)||fabsf(det)<1e-12f)return -1;
    float inv[3][3];
    inv[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])/det;
    inv[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])/det;
    inv[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])/det;
    inv[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])/det;
    inv[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])/det;
    inv[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])/det;
    inv[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])/det;
    inv[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])/det;
    inv[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])/det;
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)nm[i][j]=inv[j][i];
    return 0;
}
static float clamp01(float v){return v<0.0f?0.0f:v>1.0f?1.0f:v;}
static int tnl_light_vertex(const D9TnlState *s,const D9Matrix *world,
                            const float pos[3],const float normal[3],
                            const D9ColorVertex *vtx,float out[4]){
    float wp[3];
    for(unsigned j=0;j<3;++j)
        wp[j]=pos[0]*world->m[0][j]+pos[1]*world->m[1][j]+pos[2]*world->m[2][j]+world->m[3][j];
    float nm[3][3];
    if(tnl_normal_matrix(world,nm))return -1;
    float n[3]={0,0,0};
    for(unsigned j=0;j<3;++j)
        n[j]=normal[0]*nm[0][j]+normal[1]*nm[1][j]+normal[2]*nm[2][j];
    if(s->normalize_normals){
        float len=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(len>0.0f&&isfinite(len)){n[0]/=len;n[1]/=len;n[2]/=len;}
    }
    float md[4];
    if(vtx&&s->color_vertex&&s->diffuse_source==D9_MCS_COLOR1){
        md[0]=vtx->r;md[1]=vtx->g;md[2]=vtx->b;md[3]=vtx->a;
    }else for(unsigned i=0;i<4;++i)md[i]=s->material.diffuse[i];
    float ga[4];ga[0]=((s->global_ambient>>16)&0xff)/255.0f;
    ga[1]=((s->global_ambient>>8)&0xff)/255.0f;ga[2]=(s->global_ambient&0xff)/255.0f;
    float amb[3],diff[3];
    for(unsigned j=0;j<3;++j){amb[j]=s->material.ambient[j]*ga[j];diff[j]=0.0f;}
    for(unsigned i=0;i<D9_TNL_MAX_LIGHTS;++i){
        if(!(s->enabled&(1u<<i)))continue;
        const D9LightState *l=&s->lights[i];
        /* D3D fixed-function light ambient is not range- or distance-
         * attenuated, so it contributes even when the diffuse term is cut. */
        for(unsigned j=0;j<3;++j)amb[j]+=s->material.ambient[j]*l->ambient[j];
        float lv[3],attenuation=1.0f;
        if(l->type==D9_LIGHT_DIRECTIONAL){
            lv[0]=-l->direction[0];lv[1]=-l->direction[1];lv[2]=-l->direction[2];
        }else if(l->type==D9_LIGHT_POINT){
            lv[0]=l->position[0]-wp[0];lv[1]=l->position[1]-wp[1];lv[2]=l->position[2]-wp[2];
            float d=sqrtf(lv[0]*lv[0]+lv[1]*lv[1]+lv[2]*lv[2]);
            if(l->range>0.0f&&d>l->range)continue;
            float denom=l->attenuation0+l->attenuation1*d+l->attenuation2*d*d;
            if(!(denom>0.0f)||!isfinite(denom))continue;
            attenuation=1.0f/denom;
        }else continue; /* spot is rejected at SetLight */
        float len=sqrtf(lv[0]*lv[0]+lv[1]*lv[1]+lv[2]*lv[2]);
        if(!(len>0.0f)||!isfinite(len))continue;
        lv[0]/=len;lv[1]/=len;lv[2]/=len;
        float ndl=n[0]*lv[0]+n[1]*lv[1]+n[2]*lv[2];
        if(ndl>0.0f)for(unsigned j=0;j<3;++j)diff[j]+=md[j]*l->diffuse[j]*ndl*attenuation;
    }
    for(unsigned j=0;j<3;++j)out[j]=clamp01(s->material.emissive[j]+amb[j]+diff[j]);
    out[3]=clamp01(md[3]);
    return 0;
}
static unsigned *render_slot(D9DrawState *s,unsigned id,unsigned *min,unsigned *max){*min=0;*max=1;switch(id){
    case 7:return &s->z_enable;case 14:return &s->z_write;case 15:return &s->alpha_test;case 27:return &s->blend;
    case 19:*min=1;*max=11;return &s->src_blend;case 20:*min=1;*max=11;return &s->dst_blend;
    case 22:*min=1;*max=3;return &s->cull;case 23:*min=1;*max=8;return &s->z_func;
    case 24:*max=255;return &s->alpha_ref;case 25:*min=1;*max=8;return &s->alpha_func;
    case 168:*max=15;return &s->color_write;case 171:*min=1;*max=5;return &s->blend_op;default:return NULL;}}
static unsigned *sampler_slot(D9DrawState *s,unsigned id,unsigned *min,unsigned *max){*min=1;*max=2;switch(id){
    case 1:*max=3;return &s->address_u;case 2:*max=3;return &s->address_v;
    case 5:return &s->mag_filter;case 6:return &s->min_filter;case 7:*min=0;return &s->mip_filter;
    case 9:*min=0;*max=10;return &s->max_mip;default:return NULL;}}
static int tnl_set_state(D9Device *d,unsigned id,unsigned v){
    switch(id){
    case D9_RS_SPECULARENABLE:
        if(v>1)return D9_INVALID;
        if(v){set_diagnosis(d,"CPU T&L prototype does not implement per-vertex specular");return D9_UNSUPPORTED;}
        d->tnl.specular_enable=0;return D9_OK;
    case D9_RS_LIGHTING:
        if(v>1)return D9_INVALID;d->tnl.lighting=v;return D9_OK;
    case D9_RS_AMBIENT:
        d->tnl.global_ambient=v;return D9_OK;
    case D9_RS_COLORVERTEX:
        if(v>1)return D9_INVALID;d->tnl.color_vertex=v;return D9_OK;
    case D9_RS_NORMALIZENORMALS:
        if(v>1)return D9_INVALID;d->tnl.normalize_normals=v;return D9_OK;
    case D9_RS_DIFFUSEMATERIALSOURCE:
        if(v>D9_MCS_COLOR2)return D9_INVALID;
        if(v==D9_MCS_COLOR2){set_diagnosis(d,"CPU T&L prototype supports the material or COLOR1 diffuse source");return D9_UNSUPPORTED;}
        d->tnl.diffuse_source=v;return D9_OK;
    case D9_RS_AMBIENTMATERIALSOURCE:
    case D9_RS_EMISSIVEMATERIALSOURCE:
        if(v>D9_MCS_COLOR2)return D9_INVALID;
        if(v!=D9_MCS_MATERIAL){set_diagnosis(d,"CPU T&L prototype supports only the material source for ambient/emissive");return D9_UNSUPPORTED;}
        if(id==D9_RS_AMBIENTMATERIALSOURCE)d->tnl.ambient_source=v;else d->tnl.emissive_source=v;return D9_OK;
    case D9_RS_SPECULARMATERIALSOURCE:
        if(v>D9_MCS_COLOR2)return D9_INVALID;d->tnl.specular_source=v;return D9_OK;
    case D9_RS_LOCALVIEWER:
        if(v>1)return D9_INVALID;
        if(v){set_diagnosis(d,"CPU T&L prototype does not implement the local viewer");return D9_UNSUPPORTED;}
        d->tnl.local_viewer=0;return D9_OK;
    default:return D9_INVALID;
    }
}
static int tnl_get_state(const D9Device *d,unsigned id,unsigned *v){
    switch(id){
    case D9_RS_SPECULARENABLE:*v=d->tnl.specular_enable;return D9_OK;
    case D9_RS_LIGHTING:*v=d->tnl.lighting;return D9_OK;
    case D9_RS_AMBIENT:*v=d->tnl.global_ambient;return D9_OK;
    case D9_RS_COLORVERTEX:*v=d->tnl.color_vertex;return D9_OK;
    case D9_RS_NORMALIZENORMALS:*v=d->tnl.normalize_normals;return D9_OK;
    case D9_RS_DIFFUSEMATERIALSOURCE:*v=d->tnl.diffuse_source;return D9_OK;
    case D9_RS_AMBIENTMATERIALSOURCE:*v=d->tnl.ambient_source;return D9_OK;
    case D9_RS_SPECULARMATERIALSOURCE:*v=d->tnl.specular_source;return D9_OK;
    case D9_RS_EMISSIVEMATERIALSOURCE:*v=d->tnl.emissive_source;return D9_OK;
    case D9_RS_LOCALVIEWER:*v=d->tnl.local_viewer;return D9_OK;
    default:return D9_INVALID;
    }
}
int d9_set_render_state(D9Device *d,unsigned id,unsigned v){
    if(!d)return D9_INVALID;
    if(id==D9_RS_CLIPPLANEENABLE){
        if(v&~0x3Fu){set_diagnosis(d,"only clip planes 0..5 exist");return D9_INVALID;}
        d->clip_plane_enable=v;return D9_OK;
    }
    if(d->tnl_proto){
        int rc=tnl_set_state(d,id,v);
        if(rc!=D9_INVALID)return rc;
    }
    unsigned lo,hi,*p=render_slot(&d->state,id,&lo,&hi);
    if(!p||v<lo||v>hi)return D9_INVALID;*p=v;return D9_OK;
}
int d9_get_render_state(D9Device *d,unsigned id,unsigned *v){
    if(!d||!v)return D9_INVALID;
    if(id==D9_RS_CLIPPLANEENABLE){*v=d->clip_plane_enable;return D9_OK;}
    if(d->tnl_proto){
        int rc=tnl_get_state(d,id,v);
        if(rc==D9_OK)return D9_OK;
    }
    unsigned lo,hi,*p=render_slot(&d->state,id,&lo,&hi);
    if(!p)return D9_INVALID;*v=*p;return D9_OK;
}
int d9_set_sampler_state(D9Device *d,unsigned id,unsigned v){return d9_set_unit_sampler(d,0,id,v);}
int d9_set_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned v){
    if(stage>=d9_stage_count(d))return D9_UNSUPPORTED;
    unsigned lo,hi,*p=d?sampler_slot(stage?&d->second:&d->state,id,&lo,&hi):NULL;if(!p||v<lo||v>hi)return D9_INVALID;*p=v;return D9_OK;}
int d9_get_sampler_state(D9Device *d,unsigned id,unsigned *v){return d9_get_unit_sampler(d,0,id,v);}
int d9_get_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned *v){
    if(stage>=d9_stage_count(d))return D9_UNSUPPORTED;
    unsigned lo,hi,*p=d?sampler_slot(stage?&d->second:&d->state,id,&lo,&hi):NULL;if(!p||!v)return D9_INVALID;*v=*p;return D9_OK;}
int d9_begin_scene_ex(D9Device *d,uint32_t argb,int clear_target){
    if(!d||d->teardown||d->scene)return D9_INVALID;
    if(d->ops.begin(d->ctx,argb))return D9_BACKEND;
    d->scene=1;
    /* The default back buffer is the retained target when lockable. */
    int on_back=!d->target&&d->back_target;
    Target *t=d->target?target(d,d->target):
        (d->back_target?target(d,d->back_target):NULL);
    if(d->target&&!t){d->ops.end(d->ctx);d->scene=0;return D9_INVALID;}
    /* D3DSWAPEFFECT_DISCARD: a new frame may not inherit the previous one.
     * A retained back buffer is therefore cleared at every BeginScene (with the
     * queued Clear colour when the guest asked for one, zero otherwise); an
     * explicit render target keeps the caller's exact clear intent. */
    int want_clear=clear_target||on_back;
    uint32_t color=clear_target?argb:0u;
    if(d->ops.target_bind&&d->ops.target_bind(d->ctx,t?t->native:NULL)){
        d->ops.end(d->ctx);d->scene=0;return D9_BACKEND;
    }
    if(t){
        int vrc=apply_target_viewport(d,t);
        if(vrc){d->ops.end(d->ctx);d->scene=0;return vrc;}
    }
    if(t&&want_clear&&d->ops.target_clear(d->ctx,t->native,color)){
        d->ops.end(d->ctx);d->scene=0;return D9_BACKEND;
    }
    return D9_OK;
}
int d9_begin_scene(D9Device *d,uint32_t argb){return d9_begin_scene_ex(d,argb,1);}
typedef struct {const uint8_t *data;size_t stride,max_index;} DrawStream;
/* Optional stream-zero/index override for D3D9 UP draws.  vertices/indices are
 * borrowed guest buffers; count is the number of valid vertices in the UP
 * stream (not bytes). */
typedef struct {
    const uint8_t *vertices;size_t stride,count;
    const uint8_t *indices;unsigned index_size;
} UpSource;
static int prepare_streams(D9Device *d,DrawStream out[D9_MAX_STREAMS],size_t *max_index,
                           const UpSource *up){
    unsigned mask=d->decl_active?d->declaration.stream_mask:1;
    *max_index=SIZE_MAX;
    for(unsigned i=0;i<D9_MAX_STREAMS;++i)if(mask&(1u<<i)){
        size_t need=d->decl_active?d->declaration.required[i]:sizeof(D9Vertex);
        if(i==0&&up&&up->vertices){
            if(up->stride<need||up->stride>4096||!up->count)return D9_INVALID;
            out[0]=(DrawStream){up->vertices,up->stride,up->count-1};
        }else{
            Stream *s=&d->streams[i];Buffer *b=buffer(d,s->id);
            if(!b||b->kind||b->locked||s->stride<need||s->offset>b->bytes||need>b->bytes-s->offset)return D9_INVALID;
            out[i]=(DrawStream){b->data+s->offset,s->stride,(b->bytes-s->offset-need)/s->stride};
        }
        if(out[i].max_index<*max_index)*max_index=out[i].max_index;
    }return D9_OK;
}
/* POSITIONT (XYZRHW) maps D3D9 screen pixels to the rotated PICA clip space.
 * For a WxH viewport, D3D9 NDC is x'=2x/W-1, y'=1-2y/H, z'=z_d3d (w=1).
 * d9_pica_transform's convention is pica_x=y', pica_y=-x', pica_z=z_d3d-w. */
static void positiont_matrix(D9Matrix *m,unsigned w,unsigned h){
    d9_matrix_identity(m);
    m->m[0][1]=-2.0f/(float)h;m->m[0][3]=1.0f;   /* out x = 1 - 2y/h */
    m->m[1][0]=-2.0f/(float)w;m->m[1][3]=1.0f;   /* out y = 1 - 2x/w */
    m->m[2][2]=1.0f;m->m[2][3]=-1.0f;            /* out z = z - 1 */
    m->m[3][0]=m->m[3][1]=m->m[3][2]=0.0f;m->m[3][3]=1.0f;
}
static int fetch(D9Device *d,const DrawStream *s,uint64_t index,D9Vertex *out,D9ColorVertex *col,float normal[3]){
    if(!d->decl_active){
        memcpy(out,s[0].data+(size_t)index*s[0].stride,sizeof *out);
        *col=(D9ColorVertex){out->x,out->y,out->z,out->u,out->v,1.0f,1.0f,1.0f,1.0f};
        normal[0]=normal[1]=normal[2]=0.0f;
    }else{const D9Decl *decl=&d->declaration;float pos[4],uv[4]={0,0,0,1},rgba[4]={1,1,1,1},nrm[4]={0,0,0,1};
        const D9VertexElement *p=&decl->elements[decl->position];
        if(d9_decl_decode(p->type,s[p->stream].data+(size_t)index*s[p->stream].stride+p->offset,d9_decl_type_size(p->type),pos))return D9_INVALID;
        if(decl->position_t){
            if(pos[3]!=1.0f)return D9_UNSUPPORTED; /* shader has fixed w=1 today */
        }else if(pos[3]!=1.0f)return D9_INVALID;
        if(decl->uv!=UINT32_MAX){p=&decl->elements[decl->uv];if(d9_decl_decode(p->type,s[p->stream].data+(size_t)index*s[p->stream].stride+p->offset,d9_decl_type_size(p->type),uv))return D9_INVALID;}
        if(decl->color!=UINT32_MAX){p=&decl->elements[decl->color];if(d9_decl_decode(p->type,s[p->stream].data+(size_t)index*s[p->stream].stride+p->offset,d9_decl_type_size(p->type),rgba))return D9_INVALID;}
        if(decl->normal!=UINT32_MAX){p=&decl->elements[decl->normal];if(d9_decl_decode(p->type,s[p->stream].data+(size_t)index*s[p->stream].stride+p->offset,d9_decl_type_size(p->type),nrm))return D9_INVALID;}
        *out=(D9Vertex){pos[0],pos[1],pos[2],uv[0],uv[1]};
        *col=(D9ColorVertex){pos[0],pos[1],pos[2],uv[0],uv[1],rgba[0],rgba[1],rgba[2],rgba[3]};
        normal[0]=nrm[0];normal[1]=nrm[1];normal[2]=nrm[2];
    }
    return isfinite(out->x)&&isfinite(out->y)&&isfinite(out->z)&&isfinite(out->u)&&isfinite(out->v)&&
           isfinite(col->r)&&isfinite(col->g)&&isfinite(col->b)&&isfinite(col->a)&&
           isfinite(normal[0])&&isfinite(normal[1])&&isfinite(normal[2])?D9_OK:D9_INVALID;
}
static size_t triangle_index(unsigned type,size_t i){size_t t=i/3,c=i%3;
    if(type==D9_TRIANGLELIST)return i;
    if(type==D9_TRIANGLEFAN)return c?t+c:0;
    return t+((t&1)&&c<2?1-c:c);}
/* Replicated texels preserve normalized point sampling, including wrap/clamp.
 * A linear sample or mip transition would use a different physical footprint. */
static int padded_filter_supported(const Texture *t,const D9DrawState *s){
    return !t||(t->w>=8&&t->h>=8)||(s->min_filter==1&&s->mag_filter==1&&!s->mip_filter&&!s->max_mip);
}
/* D3D9 stage 0 CURRENT is the diffuse (vertex) colour.  A lit draw must keep
 * DIFFUSE/CURRENT in the stage output, otherwise the CPU lighting would be
 * silently replaced by the texture. */
static int stage_uses_diffuse(const D9DrawState *s){
    if(s->stage_colorop==D9_TOP_DISABLE)return 0;
    if(s->stage_colorop==D9_TOP_SELECTARG1)
        return s->stage_colorarg1==D9_TA_DIFFUSE||s->stage_colorarg1==D9_TA_CURRENT;
    if(s->stage_colorop==D9_TOP_SELECTARG2)
        return s->stage_colorarg2==D9_TA_DIFFUSE||s->stage_colorarg2==D9_TA_CURRENT;
    return s->stage_colorarg1==D9_TA_DIFFUSE||s->stage_colorarg1==D9_TA_CURRENT||
           s->stage_colorarg2==D9_TA_DIFFUSE||s->stage_colorarg2==D9_TA_CURRENT;
}
/* D3D cube faces are selected by the major axis of the sampling direction.  A
 * NORMAL element is transformed by the world normal matrix; without one the
 * documented 2D-emulation direction is (u-0.5, v-0.5, 0), with +Z at UV centre.
 * Returns 0 and writes face 0..5, or -1 when a non-zero normal exists but the
 * world matrix is singular. */
static int cube_face_from_direction(float x,float y,float z,unsigned *face){
    float ax=fabsf(x),ay=fabsf(y),az=fabsf(z);
    if(ax==0.0f&&ay==0.0f&&az==0.0f){*face=D9_CUBE_FACE_POSITIVE_Z;return 0;}
    if(ax>=ay&&ax>=az)*face=x>=0.0f?D9_CUBE_FACE_POSITIVE_X:D9_CUBE_FACE_NEGATIVE_X;
    else if(ay>=az)*face=y>=0.0f?D9_CUBE_FACE_POSITIVE_Y:D9_CUBE_FACE_NEGATIVE_Y;
    else *face=z>=0.0f?D9_CUBE_FACE_POSITIVE_Z:D9_CUBE_FACE_NEGATIVE_Z;
    return 0;
}
static int cube_select_face(const float normal[3],int has_normal,
                            int normal_matrix_ok,const float nm[3][3],
                            const D9Vertex *v,unsigned *face){
    if(has_normal){
        float len2=normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2];
        if(len2>1e-20f){
            if(!normal_matrix_ok)return -1;
            float n[3];
            for(unsigned j=0;j<3;++j)
                n[j]=normal[0]*nm[0][j]+normal[1]*nm[1][j]+normal[2]*nm[2][j];
            float n2=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
            if(n2>1e-20f){float inv=1.0f/sqrtf(n2);
                return cube_face_from_direction(n[0]*inv,n[1]*inv,n[2]*inv,face);}
        }
    }
    return cube_face_from_direction(v->u-0.5f,v->v-0.5f,0.0f,face);
}
static void fill_cube_view(D9CubeView *view,const CubeTexture *c){
    memset(view,0,sizeof *view);
    view->native=c->native;view->edge=c->edge;view->levels=c->levels;
    view->format=c->format;view->pixels=c->pixels;view->face_bytes=c->face_bytes;
    for(unsigned i=0;i<c->levels&&i<D9_CUBE_MAX_LEVELS;++i){
        view->level_edge[i]=c->level[i].w;
        view->level_pitch[i]=c->level[i].pitch;
        view->level_bytes[i]=c->level[i].bytes;
    }
}
static int draw(D9Device *d,unsigned type,int32_t base,unsigned start,unsigned primitives,int indexed,int limited,unsigned min,unsigned count,const UpSource *up){
    if(!d||d->teardown||!d->scene||(type!=D9_TRIANGLELIST&&type!=D9_TRIANGLESTRIP&&type!=D9_TRIANGLEFAN)||!primitives||primitives>MAX_DRAW_VERTICES/3||(limited&&!count))return D9_INVALID;
    if(d->clip_plane_enable){
        set_diagnosis(d,"user clip planes are enabled; PICA has no user clip-plane stage");
        return D9_UNSUPPORTED;
    }
    CubeTexture *ct=NULL;int cube_mode=0;
    if(d->cube_texture1){
        set_diagnosis(d,"cube-map sampling on texture stage 1 is not implemented; bind the cube to stage 0");
        return D9_UNSUPPORTED;
    }
    if(d->volume_texture||d->volume_texture1){
        set_diagnosis(d,"volume sampling is not implemented; bind a 2D texture for this draw");
        return D9_UNSUPPORTED;
    }
    if(d->cube_texture){
        ct=cube_texture(d,d->cube_texture);
        if(!ct||ct->cpu_only||!ct->native||!ct->pixels||
           ct->level[0].face_mask!=((1u<<D9_CUBE_FACES)-1u)){
            set_diagnosis(d,"cube-map draw requires a complete six-face cube texture");
            return D9_INVALID;
        }
        if(!d->ops.draw_cube){
            set_diagnosis(d,"cube-map sampling requires a backend draw_cube callback; "
                            "this backend cannot sample cube maps");
            return D9_UNSUPPORTED;
        }
        if(d->pixel_shader){
            set_diagnosis(d,"cube-map sampling with a pixel shader is not implemented");
            return D9_UNSUPPORTED;
        }
        if(d->texture1||d->second.stage_colorop!=D9_TOP_DISABLE){
            set_diagnosis(d,"cube-map sampling supports only texture stage 0");
            return D9_UNSUPPORTED;
        }
        if(d->state.stage_colorop==D9_TOP_DISABLE){
            set_diagnosis(d,"cube-map sampling requires an active stage-0 colour operation");
            return D9_UNSUPPORTED;
        }
        if(d->tnl_proto&&d->tnl.lighting&&!(d->decl_active&&d->declaration.position_t)){
            set_diagnosis(d,"cube-map sampling with CPU T&L lighting is not implemented");
            return D9_UNSUPPORTED;
        }
        cube_mode=1;
    }
    if(up&&(!up->vertices||!up->stride||(up->indices&&up->index_size!=2&&up->index_size!=4)))return D9_INVALID;
    const uint8_t *ixdata=NULL;size_t ixbytes=0;unsigned step=2;
    if(indexed){
        if(up&&up->indices){ixdata=up->indices;step=up->index_size;}
        else{Buffer *ix=buffer(d,d->index);if(!ix||!ix->kind||ix->locked)return D9_INVALID;
            ixdata=ix->data;ixbytes=ix->bytes;step=ix->kind==2?4:2;}
    }
    Texture *t=texture(d,d->texture);
    if(!padded_filter_supported(t,&d->state)){
        set_diagnosis(d,"Padded textures support point sampling at level zero");return D9_UNSUPPORTED;
    }
    if(t&&(t->locked||!t->level[0].uploaded))return D9_INVALID;
    if(t&&d->decl_active&&d->declaration.uv==UINT32_MAX)return D9_INVALID;
    DrawStream streams[D9_MAX_STREAMS]={0};size_t max_index;if(prepare_streams(d,streams,&max_index,up))return D9_INVALID;
    /* Test-only CPU T&L: light object-space vertices before the existing draw
     * path.  POSITIONT bypasses the transform, so it is never lit (D3D does the
     * same for pre-transformed vertices). */
    int lit=0;
    if(d->tnl_proto&&d->tnl.lighting&&!(d->decl_active&&d->declaration.position_t)){
        if(!d->decl_active||d->declaration.normal==UINT32_MAX){
            set_diagnosis(d,"CPU T&L lighting requires a NORMAL element in the active FVF/declaration");
            return D9_UNSUPPORTED;
        }
        if(!d->ops.draw_colored){
            set_diagnosis(d,"CPU T&L lighting requires a colour-capable backend (draw_colored)");
            return D9_UNSUPPORTED;
        }
        if(d->second.stage_colorop!=D9_TOP_DISABLE){
            set_diagnosis(d,"CPU T&L prototype supports only one texture stage");
            return D9_UNSUPPORTED;
        }
        if(!stage_uses_diffuse(&d->state)){
            set_diagnosis(d,"CPU T&L lighting requires stage 0 to source DIFFUSE/CURRENT; the current stage would drop the lit colour");
            return D9_UNSUPPORTED;
        }
        lit=1;
    }
    /* Repeated indexed vertices often share UV/position. Cache decoded values per draw only. */
    struct Cached {uint64_t index;D9Vertex vertex;D9ColorVertex color;float normal[3];int valid;} cache[64]={0};
    int cube_has_normal=cube_mode&&d->decl_active&&d->declaration.normal!=UINT32_MAX;
    float cube_nm[3][3];int cube_nm_ok=0;
    if(cube_has_normal)cube_nm_ok=!tnl_normal_matrix(&d->transforms[D9_WORLD],cube_nm);
    size_t n=(size_t)primitives*3,in=type==D9_TRIANGLELIST?n:(size_t)primitives+2;
    if(indexed&&!(up&&up->indices)&&((size_t)start>ixbytes/step||in>ixbytes/step-(size_t)start))return D9_INVALID;
    for(size_t i=0;i<n;++i){size_t rel=triangle_index(type,i);int64_t index=(int64_t)start+(int64_t)rel;
        if(indexed){const uint8_t *p=ixdata+((size_t)start+rel)*step;uint32_t raw=(uint32_t)p[0]|(uint32_t)p[1]<<8;
            if(step==4)raw|=(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
            if(limited&&(raw<min||(uint64_t)raw>=((uint64_t)min+count)))return D9_INVALID;index=(int64_t)base+raw;}
        if(index<0||(uint64_t)index>max_index)return D9_INVALID;
        struct Cached *entry=&cache[(unsigned)index&63];if(!entry->valid||entry->index!=(uint64_t)index){
            int rc=fetch(d,streams,(uint64_t)index,&entry->vertex,&entry->color,entry->normal);
            if(rc){set_diagnosis(d,rc==D9_UNSUPPORTED?
                "POSITIONT vertex has w != 1; the current fixed-w PICA shader cannot represent it":
                "vertex decode failed");return rc;}
            if(lit){
                float out[4];
                if(tnl_light_vertex(&d->tnl,&d->transforms[D9_WORLD],&entry->vertex.x,entry->normal,
                                    d->declaration.color!=UINT32_MAX?&entry->color:NULL,out)){
                    set_diagnosis(d,"CPU T&L: the world matrix is singular; the normal cannot be transformed");
                    return D9_INVALID;
                }
                entry->color.r=out[0];entry->color.g=out[1];entry->color.b=out[2];entry->color.a=out[3];
            }
            entry->index=(uint64_t)index;entry->valid=1;}
        d->scratch[i]=entry->vertex;d->color_scratch[i]=entry->color;
        if(cube_mode){
            unsigned face=0;
            if(cube_select_face(entry->normal,cube_has_normal,cube_nm_ok,cube_nm,
                                &entry->vertex,&face)){
                set_diagnosis(d,"cube sampling: the world matrix is singular and the "
                                "normal cannot be transformed");
                return D9_INVALID;
            }
            d->cube_faces[i]=face;
        }
    }
    d->scratch_count=(unsigned)n;d->color_scratch_count=(unsigned)n;
    D9Matrix clip;
    if(d->decl_active&&d->declaration.position_t){
        if(d->shader_active)
            clip=d->shader_clip; /* an explicit shader owns its own transform */
        else if(!d->view_w||!d->view_h){
            set_diagnosis(d,"POSITIONT/XYZRHW draw needs a viewport size; call d9_set_viewport_size first");
            return D9_UNSUPPORTED;
        }else positiont_matrix(&clip,d->view_w,d->view_h);
    }else if(d->shader_active)clip=d->shader_clip;
    else d9_pica_transform(&clip,&d->transforms[0],&d->transforms[1],&d->transforms[2]);
    D9DrawState state=d->state;state.pixel_shader=d->pixel_shader;state.resident_mips=0;if(t)while(state.resident_mips<t->gpu_levels&&t->level[state.resident_mips].uploaded)++state.resident_mips;
    if(cube_mode){
        D9CubeView view;fill_cube_view(&view,ct);
        state.resident_mips=1;
        if(d->ops.draw_cube(d->ctx,d->color_scratch,n,d->cube_faces,&clip,&view,&state)){
            set_diagnosis(d,"cube-map draw rejected by the backend");
            return D9_BACKEND;
        }
        set_diagnosis(d,"");
        return D9_OK;
    }
    int has_color=lit||(d->decl_active&&d->declaration.color!=UINT32_MAX);
    /* Menu TEV plans can need a second texture unit, color0 and an external
     * constant bank.  Route those before the fixed-function two-stage path. */
    if(state.pixel_shader&&d->ops.draw_menu&&
       (state.pixel_shader->textures>1||state.pixel_shader->constants>0||
        state.pixel_shader->uses_vertex_color)){
        if(!has_color){
            set_diagnosis(d,"menu TEV plan requires per-vertex COLOR0; the declaration has no color");
            return D9_UNSUPPORTED;
        }
        Texture *second=NULL;
        if(state.pixel_shader->textures>1){
            second=texture(d,d->texture1);
            if(!second||!second->native||second->locked||!second->level[0].uploaded){
                set_diagnosis(d,"menu TEV plan requires a ready texture unit 1");
                return D9_UNSUPPORTED;
            }
            if(!padded_filter_supported(second,&d->second)){
                set_diagnosis(d,"menu TEV texture unit 1 supports point sampling at level zero");
                return D9_UNSUPPORTED;
            }
        }
        if(state.pixel_shader->textures>0&&(!t||!t->native)) {
            set_diagnosis(d,"menu TEV plan requires a ready texture unit 0");
            return D9_UNSUPPORTED;
        }
        if(d->ops.draw_menu(d->ctx,d->color_scratch,n,&clip,
                            t?t->native:NULL,second?second->native:NULL,
                            &state,(const float (*)[4])d->tev_constants)){
            set_diagnosis(d,"menu TEV draw rejected by the backend");
            return D9_BACKEND;
        }
        set_diagnosis(d,"");return D9_OK;
    }
    if(!state.pixel_shader && state.stage_colorop!=D9_TOP_DISABLE && d->second.stage_colorop!=D9_TOP_DISABLE){
        Texture *second=texture(d,d->texture1);
        if(!t||!second||!second->native||second->locked||!second->level[0].uploaded||
           !d->ops.draw_two||d->second.stage_texcoordindex||state.stage_texcoordindex||
           state.stage_alphaop==D9_TOP_DISABLE||d->second.stage_alphaop==D9_TOP_DISABLE){
            set_diagnosis(d,"Two-stage FFP requires two ready textures, UV0 and active RGB/alpha operations");
            return D9_UNSUPPORTED;
        }
        if(!padded_filter_supported(second,&d->second)){
            set_diagnosis(d,"Padded stage-1 texture requires point sampling at level zero");return D9_UNSUPPORTED;
        }
        D9DrawState unit1=d->second;
        while(unit1.resident_mips<second->gpu_levels&&second->level[unit1.resident_mips].uploaded)++unit1.resident_mips;
        if(t==second && (state.address_u!=unit1.address_u||state.address_v!=unit1.address_v||
            state.min_filter!=unit1.min_filter||state.mag_filter!=unit1.mag_filter||
            state.mip_filter!=unit1.mip_filter||state.max_mip!=unit1.max_mip)){
            set_diagnosis(d,"Shared texture with conflicting sampler settings is unsupported");return D9_UNSUPPORTED;
        }
        if(d->ops.draw_two(d->ctx,d->color_scratch,n,&clip,t->native,&state,second->native,&unit1)){
            set_diagnosis(d,"Backend rejected two-stage fixed-function draw");return D9_BACKEND;
        }
        set_diagnosis(d,"");return D9_OK;
    }
    if(has_color){
        if(!d->ops.draw_colored){
            set_diagnosis(d,"draw requires per-vertex COLOR0 (DIFFUSE/COLOR0); backend has no color-capable PICA vertex shader, refusing draw instead of dropping the tint");
            return D9_UNSUPPORTED;
        }
        set_diagnosis(d,"");
        if(d->ops.draw_colored(d->ctx,d->color_scratch,n,&clip,t?t->native:NULL,&state)){
            set_diagnosis(d,"colour draw rejected by the backend (unsupported stage-0 TEV combination, vertex-colour program unavailable, or arena exhausted)");
            return D9_BACKEND;
        }
        return D9_OK;
    }
    set_diagnosis(d,"");
    return d->ops.draw(d->ctx,d->scratch,n,&clip,t?t->native:NULL,&state)?D9_BACKEND:D9_OK;
}
int d9_draw_primitive(D9Device *d,unsigned type,unsigned start,unsigned primitives){return draw(d,type,0,start,primitives,0,0,0,0,NULL);}
int d9_draw_indexed_primitive(D9Device *d,unsigned type,int32_t base,unsigned start,unsigned primitives){return draw(d,type,base,start,primitives,1,0,0,0,NULL);}
int d9_draw_indexed_range(D9Device *d,unsigned type,int32_t base,unsigned min,unsigned count,unsigned start,unsigned primitives){return draw(d,type,base,start,primitives,1,1,min,count,NULL);}
int d9_draw_primitive_up(D9Device *d,unsigned type,unsigned primitives,const void *vertex_data,unsigned vertex_stride){
    if(!vertex_data||!vertex_stride||!primitives||primitives>MAX_DRAW_VERTICES/3)return D9_INVALID;
    size_t n=type==D9_TRIANGLELIST?(size_t)primitives*3:(size_t)primitives+2;
    UpSource up={vertex_data,vertex_stride,n,NULL,0};
    return draw(d,type,0,0,primitives,0,0,0,0,&up);
}
int d9_draw_indexed_primitive_up(D9Device *d,unsigned type,unsigned min_vertex,unsigned vertex_count,unsigned primitives,const void *index_data,unsigned index_format,const void *vertex_data,unsigned vertex_stride){
    if(!vertex_data||!vertex_stride||!index_data||(index_format!=D9_INDEX16&&index_format!=D9_INDEX32)||
       !primitives||primitives>MAX_DRAW_VERTICES/3||!vertex_count||min_vertex>UINT32_MAX-vertex_count)return D9_INVALID;
    UpSource up={vertex_data,vertex_stride,(size_t)min_vertex+vertex_count,index_data,index_format==D9_INDEX32?4u:2u};
    return draw(d,type,0,0,primitives,1,1,min_vertex,vertex_count,&up);
}
int d9_end_scene(D9Device *d){if(!d||d->teardown||!d->scene)return D9_INVALID;d->ops.end(d->ctx);d->scene=0;return D9_OK;}
int d9_present(D9Device *d){
    if(!d||d->teardown||!d->scene)return D9_INVALID;
    if(d->back_target){
        Target *bt=target(d,d->back_target);
        if(!bt||!d->ops.target_blit||!d->ops.target_bind){d->ops.end(d->ctx);d->scene=0;return D9_BACKEND;}
        D9BlitRegion src,dst;
        memset(&src,0,sizeof src);memset(&dst,0,sizeof dst);
        src.surface=(D9BlitSurface){bt->native,d->back_w,d->back_h,bt->format,d->back_tex_w,d->back_tex_h};
        src.width=d->back_w;src.height=d->back_h;
        dst.surface=(D9BlitSurface){NULL,d->back_w,d->back_h,d->back_format,0,0};
        dst.width=d->back_w;dst.height=d->back_h;
        /* Present is the only operation that targets the LCD screen. */
        if(d->ops.target_bind(d->ctx,NULL)){
            d->ops.end(d->ctx);d->scene=0;return D9_BACKEND;
        }
        int rc=d->ops.target_blit(d->ctx,&dst,&src,D9_FILTER_POINT);
        if(rc!=D9_BLIT_OK){
            d->blit_error=rc<0?rc:D9_BLIT_BACKEND;
            /* Leave the frame with the retained target selected and finish it,
             * so a rejected Present cannot wedge the BeginScene state machine. */
            (void)d->ops.target_bind(d->ctx,bt->native);
            d->ops.end(d->ctx);d->scene=0;return D9_BACKEND;
        }
        /* D3DSWAPEFFECT_DISCARD: the retained pixels are not carried into the
         * next frame.  The next BeginScene clears them (see begin_scene_ex),
         * so the contents are guaranteed only from the last BeginScene/Clear
         * or lock/unlock until this Present. */
    }
    d->ops.end(d->ctx);d->scene=0;return D9_OK;
}
/* --- depth-stencil surfaces ------------------------------------------------ */
static Depth *depth_slot(D9Device *d,D9Handle h){
    if(d&&h)for(unsigned i=0;i<MAX_DEPTHS;++i)if(d->depths[i].id==h)return &d->depths[i];
    return NULL;
}
int d9_create_depth_surface(D9Device *d,unsigned w,unsigned h,unsigned format,
                            unsigned multisample,unsigned quality,int lockable,
                            D9Handle *out){
    if(out)*out=0;
    if(!d||d->teardown||d->scene||!out||!d->next||!d->back_valid||
       (format!=D9_D24S8&&format!=D9_D24X8)||multisample||quality||lockable)return D9_INVALID;
    if(!d->ops.depth_clear)return D9_UNSUPPORTED;
    /* A depth-stencil surface is a descriptor of the single PICA
     * DEPTH24_STENCIL8 attachment of the active target: no VRAM is allocated
     * here.  Creation accepts any power-of-two 8..1024 surface (headroom for
     * render-target textures) plus the exact non-power-of-two presentation back
     * buffer size; binding requires a size match with the active target. */
    unsigned back_size=w==d->back_w&&h==d->back_h;
    unsigned pow2=(w>=8&&h>=8&&w<=1024&&h<=1024&&!(w&(w-1))&&!(h&(h-1)));
    if(!back_size&&!pow2)return D9_INVALID;
    Depth *slot=NULL;for(unsigned i=0;i<MAX_DEPTHS;++i)if(!d->depths[i].id){slot=&d->depths[i];break;}
    if(!slot)return D9_MEMORY;
    D9Handle id=d->next++;*slot=(Depth){id,w,h,format};*out=id;return D9_OK;
}
int d9_release_depth_surface(D9Device *d,D9Handle h){
    Depth *s=depth_slot(d,h);
    if(!s||!d||d->scene||d->teardown)return D9_INVALID;
    if(d->depth==h)d->depth=0;
    memset(s,0,sizeof *s);return D9_OK;
}
static int logical_dims(const D9Device *d,unsigned *w,unsigned *h){
    D9Device *mutable_d=(D9Device *)d;
    if(d->back_target&&(!d->target||d->target==d->back_target)){
        *w=d->back_w;*h=d->back_h;return D9_OK;
    }
    Target *t=d->target?target(mutable_d,d->target):NULL;
    if(t){*w=t->w;*h=t->h;return D9_OK;}
    if(!d->back_valid)return D9_INVALID;
    *w=d->back_w;*h=d->back_h;return D9_OK;
}
int d9_set_depth_surface(D9Device *d,D9Handle h){
    if(!d||d->teardown)return D9_INVALID;
    Depth *s=h?depth_slot(d,h):NULL;
    if(h&&!s)return D9_INVALID;
    if(s){
        unsigned w,height;
        if(logical_dims(d,&w,&height)||s->w!=w||s->h!=height){
            set_diagnosis(d,"SetDepthStencilSurface: the depth-stencil size does not match the active render target");
            return D9_UNSUPPORTED;
        }
    }
    d->depth=h;return D9_OK;
}
D9Handle d9_get_depth_surface(const D9Device *d){return d?d->depth:0;}
int d9_depth_surface_info(D9Device *d,D9Handle h,unsigned *width,unsigned *height,unsigned *format){
    Depth *s=depth_slot(d,h);if(!s)return D9_INVALID;
    if(width)*width=s->w;if(height)*height=s->h;if(format)*format=s->format;return D9_OK;
}
int d9_clear_depth(D9Device *d,unsigned flags,float z,uint32_t stencil){
    if(!d||d->teardown||!d->scene||!d->depth||!d->ops.depth_clear)return D9_INVALID;
    if(!(flags&6u)||(flags&~7u))return D9_INVALID;
    if(!isfinite(z)||z<0.0f||z>1.0f)return D9_INVALID;
    Depth *s=depth_slot(d,d->depth);
    if(!s)return D9_INVALID;
    unsigned w,h;if(logical_dims(d,&w,&h)||w!=s->w||h!=s->h)return D9_UNSUPPORTED;
    uint32_t quant=(uint32_t)(z*16777215.0f+0.5f);
    if(quant>0xffffffu)quant=0xffffffu;
    uint32_t packed=quant|((stencil&0xffu)<<24);
    return d->ops.depth_clear(d->ctx,flags,packed)?D9_BACKEND:D9_OK;
}
size_t d9_memory_used(const D9Device *d){if(!d)return 0;D9TexCacheStats st;d9_texcache_stats(&d->tex_cache,&st);
    return memory_sum(d->used,memory_sum(st.guest_bytes,memory_sum(st.gpu_bytes,d->target_used)));}
size_t d9_available_texture_mem(const D9Device *d){
    if(!d)return 0;
    size_t used=d9_memory_used(d);
    return used<d->limit?d->limit-used:0;
}
int d9_validate_device(D9Device *d,unsigned *passes){
    if(!d||!passes)return D9_INVALID;
    *passes=0;
    if(d->teardown){set_diagnosis(d,"ValidateDevice: device is shutting down");return D9_INVALID;}
    if(d->pixel_shader){
        const D9TevProgram *p=d->pixel_shader;
        if(p->stage_limit<1||p->stage_limit>D9TEV_MAX_STAGES){
            char why[160];
            snprintf(why,sizeof why,"ValidateDevice: pixel shader stage limit %u is outside 1..%u",p->stage_limit,D9TEV_MAX_STAGES);
            set_diagnosis(d,why);return D9_UNSUPPORTED;
        }
        if(p->textures>D9TEV_MAX_TEXTURES){
            char why[160];
            snprintf(why,sizeof why,"ValidateDevice: pixel shader needs %u textures; PICA supports at most %u",p->textures,D9TEV_MAX_TEXTURES);
            set_diagnosis(d,why);return D9_UNSUPPORTED;
        }
        if(p->textures>d9_stage_count(d)){
            char why[180];
            snprintf(why,sizeof why,"ValidateDevice: pixel shader needs %u texture units; backend binds %u",p->textures,d9_stage_count(d));
            set_diagnosis(d,why);return D9_UNSUPPORTED;
        }
        if(p->uses_vertex_color&&!d->ops.draw_colored){
            set_diagnosis(d,"ValidateDevice: pixel shader uses vertex colour; backend has no colour draw path");
            return D9_UNSUPPORTED;
        }
    }else{
        if(d->state.stage_colorop<D9_TOP_DISABLE||d->state.stage_colorop>D9_TOP_MODULATE||
           d->state.stage_alphaop<D9_TOP_DISABLE||d->state.stage_alphaop>D9_TOP_MODULATE||
           d->second.stage_colorop<D9_TOP_DISABLE||d->second.stage_colorop>D9_TOP_MODULATE||
           d->second.stage_alphaop<D9_TOP_DISABLE||d->second.stage_alphaop>D9_TOP_MODULATE){
            set_diagnosis(d,"ValidateDevice: texture stage operator is outside the supported DISABLE/SELECTARG1/SELECTARG2/MODULATE set");
            return D9_UNSUPPORTED;
        }
        if(d->state.stage_texcoordindex>7){
            set_diagnosis(d,"ValidateDevice: texture coordinate index is outside the supported range");
            return D9_UNSUPPORTED;
        }
        if(d9_stage_count(d)>1&&
           (d->second.stage_colorop!=D9_TOP_DISABLE||d->second.stage_alphaop!=D9_TOP_DISABLE)&&
           d->second.stage_texcoordindex!=0){
            set_diagnosis(d,"ValidateDevice: stage 1 texture coordinate index must be 0");
            return D9_UNSUPPORTED;
        }
    }
    if(d->cube_texture||d->cube_texture1||d->volume_texture||d->volume_texture1){
        set_diagnosis(d,"ValidateDevice: cube/volume sampling has no draw path in this bridge");
        return D9_UNSUPPORTED;
    }
    *passes=1;
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_evict_managed_resources(D9Device *d){
    if(!d)return D9_INVALID;
    if(d->teardown){set_diagnosis(d,"EvictManagedResources: device is shutting down");return D9_INVALID;}
    for(unsigned i=0;i<MAX_TEXTURES;++i){
        Texture *t=&d->textures[i];
        if(!t->id||t->pool!=D9_POOL_MANAGED)continue;
        D9TexEntry *e=d9_texcache_find(&d->tex_cache,t->id);
        if(t->gpu_only||!t->pixels||!e||!e->has_guest){
            set_diagnosis(d,"EvictManagedResources: a MANAGED texture has no guest-backed re-upload source");
            return D9_UNSUPPORTED;
        }
    }
    for(unsigned i=0;i<MAX_CUBES;++i){
        CubeTexture *c=&d->cubes[i];
        if(!c->id||c->pool!=D9_POOL_MANAGED)continue;
        D9TexEntry *e=d9_texcache_find(&d->tex_cache,c->id);
        if(!c->pixels||!e||!e->has_guest){
            set_diagnosis(d,"EvictManagedResources: a MANAGED cube has no guest-backed re-upload source");
            return D9_UNSUPPORTED;
        }
    }
    for(unsigned i=0;i<MAX_VOLUMES;++i){
        VolumeTexture *v=&d->volumes[i];
        if(!v->id||v->pool!=D9_POOL_MANAGED)continue;
        D9TexEntry *e=d9_texcache_find(&d->tex_cache,v->id);
        if(!v->pixels||!e||!e->has_guest){
            set_diagnosis(d,"EvictManagedResources: a MANAGED volume has no guest-backed re-upload source");
            return D9_UNSUPPORTED;
        }
    }
    set_diagnosis(d,"");
    return D9_OK;
}
int d9_texture_cache_stats(D9Device *d,D9TexCacheStats *out){
    if(!d||!out)return D9_INVALID;
    d9_texcache_stats(&d->tex_cache,out);
    return D9_OK;
}
int d9_get_draw_state(const D9Device *d,D9DrawState *out){
    if(!d||!out)return D9_INVALID;
    *out=d->state;
    return D9_OK;
}
int d9_get_transform(const D9Device *d,unsigned which,D9Matrix *out){
    if(!d||!out||which>2)return D9_INVALID;
    *out=d->transforms[which];
    return D9_OK;
}
int d9_get_stream_info(const D9Device *d,unsigned stream,D9Handle *buffer,
                       size_t *offset,size_t *stride){
    if(!d||stream>=D9_MAX_STREAMS||!buffer||!offset||!stride)return D9_INVALID;
    const Stream *s=&d->streams[stream];
    *buffer=s->id;
    *offset=s->offset;
    *stride=s->stride;
    return D9_OK;
}
int d9_get_index_info(const D9Device *d,D9Handle *buffer,unsigned *format){
    if(!d||!buffer||!format)return D9_INVALID;
    const Buffer *b=NULL;
    if(d->index)for(unsigned i=0;i<MAX_BUFFERS;++i)if(d->buffers[i].id==d->index){b=&d->buffers[i];break;}
    *buffer=d->index;
    *format=b?(b->kind==2?D9_INDEX32:D9_INDEX16):0u;
    return D9_OK;
}
int d9_get_buffer_data(const D9Device *d,D9Handle h,const void **data,size_t *bytes){
    if(!d||!data||!bytes)return D9_INVALID;
    const Buffer *b=NULL;
    if(h)for(unsigned i=0;i<MAX_BUFFERS;++i)if(d->buffers[i].id==h){b=&d->buffers[i];break;}
    if(!b)return D9_INVALID;
    *data=b->data;
    *bytes=b->bytes;
    return D9_OK;
}
int d9_get_texture_capture(const D9Device *d,D9Handle h,unsigned level,
                           const void **pixels,size_t *pitch,size_t *bytes,
                           unsigned *width,unsigned *height,unsigned *format){
    if(!d||!pixels||!pitch||!bytes)return D9_INVALID;
    const Texture *t=NULL;
    if(h)for(unsigned i=0;i<MAX_TEXTURES;++i)if(d->textures[i].id==h){t=&d->textures[i];break;}
    if(!t||level>=t->levels||!t->pixels)return D9_INVALID;
    const Level *l=&t->level[level];
    *pixels=t->pixels+l->offset;
    *pitch=l->pitch;
    *bytes=l->bytes;
    if(width)*width=l->w;
    if(height)*height=l->h;
    if(format)*format=t->format;
    return D9_OK;
}
int d9_get_last_draw_vertices(const D9Device *d,const D9Vertex **vertices,size_t *count){
    if(!d||!vertices||!count)return D9_INVALID;
    *vertices=d->scratch;
    *count=d->scratch_count;
    return D9_OK;
}
int d9_get_last_draw_color_vertices(const D9Device *d,const D9ColorVertex **vertices,size_t *count){
    if(!d||!vertices||!count)return D9_INVALID;
    *vertices=d->color_scratch;
    *count=d->color_scratch_count;
    return D9_OK;
}
