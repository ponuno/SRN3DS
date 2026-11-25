/* Focused host test for d9_draw_primitive_up / d9_draw_indexed_primitive_up.
 *
 * Covers the D3D9 stream-zero contract at the core layer:
 *   - primitive list/strip/fan and 16/32-bit indices decode through the same
 *     FVF/declaration fetch path as the buffer-backed draws;
 *   - VertexStreamZeroStride is honoured (including padding and rejection of a
 *     stride shorter than the active FVF/declaration needs);
 *   - malformed counts, pointers, index formats and index ranges fail with
 *     D9_INVALID and do not reach the backend or replace the last-draw record;
 *   - the bound stream 0, index buffer, transforms, render state, texture and
 *     memory budget are unchanged by UP draws; a later normal draw still reads
 *     the bound vertex buffer.
 */
#include "d3d9_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned draws;size_t n;D9Vertex v[64];D9Matrix clip;void *texture;
    D9DrawState state;int active;
} Mock;

typedef struct {size_t base;unsigned levels;uint8_t *data;} Tex;

static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    Mock *m=ctx;(void)m;Tex *t=calloc(1,sizeof *t);if(!t)return -1;
    t->base=(size_t)w*h*4;t->levels=levels;size_t total=0;
    for(unsigned i=0;i<levels;++i)total+=t->base>>(2*i);
    t->data=malloc(total);if(!t->data){free(t);return -1;}*native=t;return 0;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){
    (void)ctx;Tex *t=native;assert(level<t->levels&&n==(t->base>>(2*level)));
    size_t off=0;for(unsigned i=0;i<level;++i)off+=t->base>>(2*i);
    memcpy(t->data+off,data,n);return 0;
}
static void tex_delete(void *ctx,void *native){(void)ctx;Tex *t=native;free(t->data);free(t);}
static int begin(void *ctx,uint32_t color){(void)color;Mock *m=ctx;assert(!m->active);m->active=1;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *texture,const D9DrawState *state){
    Mock *m=ctx;assert(m->active&&v&&clip&&n<=64);
    m->n=n;m->clip=*clip;m->texture=texture;m->state=*state;
    memcpy(m->v,v,n*sizeof *v);++m->draws;return 0;
}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end};

/* A stride-padded vertex: 24-byte stride, XYZ+UV in the first 20 bytes. */
typedef struct {D9Vertex v;float pad;} Padded;

int main(void){
    Mock m={0};
    D9Device *d=d9_create_device(&ops,&m,4u<<20);assert(d);

    /* Buffer-backed baseline: stream 0, a 16-bit index buffer and a texture. */
    D9Vertex src[4]={{0,0,0,0,0},{1,0,0,1,0},{0,1,0,0,1},{1,1,0,1,1}};
    D9Handle vb;assert(!d9_create_vertex_buffer(d,sizeof src,&vb));
    void *p;assert(!d9_buffer_lock(d,vb,0,sizeof src,&p));memcpy(p,src,sizeof src);assert(!d9_buffer_unlock(d,vb));
    assert(!d9_set_stream_source(d,vb,0,sizeof(D9Vertex)));
    uint16_t idx16[3]={0,2,3};D9Handle ib16;
    assert(!d9_create_index_buffer(d,sizeof idx16,D9_INDEX16,&ib16));
    assert(!d9_buffer_lock(d,ib16,0,sizeof idx16,&p));memcpy(p,idx16,sizeof idx16);assert(!d9_buffer_unlock(d,ib16));
    assert(!d9_set_indices(d,ib16));
    D9Handle tex;assert(!d9_create_texture(d,8,8,D9_A8R8G8B8,&tex));
    size_t pitch;assert(!d9_texture_lock(d,tex,&p,&pitch));memset(p,0x80,8*8*4);assert(!d9_texture_unlock(d,tex));
    assert(!d9_set_texture(d,tex));
    assert(!d9_set_render_state(d,27,1)&&!d9_set_render_state(d,23,5)&&!d9_set_render_state(d,24,7));

    D9Matrix world,view,proj,expected;
    d9_matrix_identity(&world);world.m[3][0]=2.0f;
    d9_matrix_identity(&view);d9_matrix_identity(&proj);
    assert(!d9_set_transform(d,D9_WORLD,&world));
    d9_pica_transform(&expected,&world,&view,&proj);

    /* Declaration resources are created before BeginScene (the core forbids
     * resource creation inside a scene).  Positions arrive through UP stream 0;
     * UVs stay in a bound stream 1. */
    static const float uv[4][2]={{0.0f,0.0f},{1.0f,0.0f},{0.0f,1.0f},{1.0f,1.0f}};
    D9Handle vbu;assert(!d9_create_vertex_buffer(d,sizeof uv,&vbu));
    assert(!d9_buffer_lock(d,vbu,0,sizeof uv,&p));memcpy(p,uv,sizeof uv);assert(!d9_buffer_unlock(d,vbu));
    assert(!d9_set_stream(d,1,vbu,0,sizeof uv[0]));
    typedef struct {float x,y,z;} Position;
    Position pw[4]={{1,2,3},{4,5,6},{7,8,9},{10,11,12}};
    D9VertexElement elements[3]={
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {1,0,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
        {0xff,0,D9_DECL_UNUSED,0,0,0}};
    D9Decl decl;assert(!d9_decl_init(&decl,elements,2));

    /* UP draws are valid only inside BeginScene/EndScene. */
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,src,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,src,sizeof(D9Vertex))==D9_INVALID);
    assert(!d9_begin_scene(d,0xff000000));
    size_t used=d9_memory_used(d);

    D9Vertex up[4]={{10,11,12,0.1f,0.2f},{13,14,15,0.3f,0.4f},{16,17,18,0.5f,0.6f},{19,20,21,0.7f,0.8f}};
    unsigned base=m.draws;

    /* Triangle list through stream zero. */
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,up,sizeof(D9Vertex)));
    assert(m.draws==base+1&&m.n==3&&!memcmp(m.v,up,3*sizeof(D9Vertex)));
    assert(!memcmp(&m.clip,&expected,sizeof expected)&&m.texture!=NULL);
    assert(m.state.blend==1&&m.state.z_func==5&&m.state.alpha_ref==7);

    /* Triangle strip and fan expand through the same index walk. */
    assert(!d9_draw_primitive_up(d,D9_TRIANGLESTRIP,2,up,sizeof(D9Vertex)));
    assert(m.n==6&&!memcmp(&m.v[5],&up[3],sizeof up[3]));
    assert(!d9_draw_primitive_up(d,D9_TRIANGLEFAN,2,up,sizeof(D9Vertex)));
    assert(m.n==6&&!memcmp(&m.v[5],&up[3],sizeof up[3]));

    /* A larger VertexStreamZeroStride is honoured and padding is skipped. */
    Padded padded[3];memset(padded,0xee,sizeof padded);
    for(unsigned i=0;i<3;++i)padded[i].v=up[i];
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,padded,sizeof(Padded)));
    assert(m.n==3&&!memcmp(m.v,up,3*sizeof(D9Vertex)));

    /* Malformed UP draws: explicit D9_INVALID, no backend call, no clobber. */
    const D9Vertex *last;size_t last_n;assert(!d9_get_last_draw_vertices(d,&last,&last_n));
    unsigned rejected=m.draws;
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,NULL,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,up,0)==D9_INVALID);
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,up,sizeof(D9Vertex)-4)==D9_INVALID);
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,up,4097)==D9_INVALID);
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,0,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,4096u/3u+1,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_primitive_up(d,7,1,up,sizeof(D9Vertex))==D9_INVALID);
    assert(m.draws==rejected);
    const D9Vertex *still;size_t still_n;assert(!d9_get_last_draw_vertices(d,&still,&still_n)&&still_n==last_n);

    /* 16-bit indexed stream zero: {0,2,3}. */
    assert(!d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,up,sizeof(D9Vertex)));
    assert(m.n==3&&!memcmp(&m.v[0],&up[0],sizeof up[0])&&
           !memcmp(&m.v[1],&up[2],sizeof up[2])&&!memcmp(&m.v[2],&up[3],sizeof up[3]));

    /* 32-bit indexed stream zero with a non-zero MinVertexIndex. */
    uint32_t idx32[3]={1,2,3};
    assert(!d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,1,3,1,idx32,D9_INDEX32,up,sizeof(D9Vertex)));
    assert(m.n==3&&!memcmp(&m.v[0],&up[1],sizeof up[1])&&!memcmp(&m.v[2],&up[3],sizeof up[3]));

    /* Malformed indexed arguments and out-of-range indices. */
    rejected=m.draws;
    uint16_t below[3]={0,1,2},over[3]={0,1,4};
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,1,3,1,below,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,over,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,0,1,idx16,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,UINT32_MAX,2,1,idx16,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,0,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,NULL,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,NULL,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,up,0)==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,0,idx16,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);
    assert(m.draws==rejected);

    /* Bound state is untouched by every UP call above. */
    D9Handle sb;size_t soff,sstride;assert(!d9_get_stream_info(d,0,&sb,&soff,&sstride));
    assert(sb==vb&&soff==0&&sstride==sizeof(D9Vertex));
    D9Handle ib;unsigned ifmt;assert(!d9_get_index_info(d,&ib,&ifmt)&&ib==ib16&&ifmt==D9_INDEX16);
    D9Matrix world2;assert(!d9_get_transform(d,D9_WORLD,&world2)&&!memcmp(&world2,&world,sizeof world));
    unsigned rs;assert(!d9_get_render_state(d,27,&rs)&&rs==1);
    assert(d9_memory_used(d)==used); /* no temporary allocations */
    assert(!d9_draw_primitive(d,D9_TRIANGLELIST,0,1)&&m.n==3&&!memcmp(m.v,src,3*sizeof(D9Vertex)));

    /* Declaration path: positions from UP stream 0, UVs from the bound stream 1. */
    assert(!d9_set_vertex_declaration(d,&decl));
    D9Handle lsb;size_t lss;assert(!d9_get_stream_info(d,1,&lsb,&soff,&lss)&&lsb==vbu);
    assert(!d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,pw,sizeof pw[0]));
    assert(m.n==3);
    assert(m.v[0].x==pw[0].x&&m.v[0].y==pw[0].y&&m.v[0].z==pw[0].z);
    assert(m.v[1].x==pw[2].x&&m.v[2].x==pw[3].x);
    assert(m.v[0].u==uv[0][0]&&m.v[0].v==uv[0][1]);
    assert(m.v[1].u==uv[2][0]&&m.v[1].v==uv[2][1]);
    assert(m.v[2].u==uv[3][0]&&m.v[2].v==uv[3][1]);
    /* A declaration needing 12 bytes accepts a 12-byte stream-zero stride. */
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,pw,sizeof pw[0]));
    assert(m.n==3&&m.v[0].u==uv[0][0]);
    /* The bound stream 1 is preserved after both UP draws. */
    assert(!d9_get_stream_info(d,1,&lsb,&soff,&lss)&&lsb==vbu&&soff==0&&lss==sizeof uv[0]);
    assert(!d9_set_vertex_declaration(d,NULL));
    assert(!d9_set_stream_source(d,vb,0,sizeof(D9Vertex)));

    /* Outside the scene the same entry points stay invalid. */
    assert(!d9_present(d));
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,up,sizeof(D9Vertex))==D9_INVALID);
    assert(d9_draw_indexed_primitive_up(d,D9_TRIANGLELIST,0,4,1,idx16,D9_INDEX16,up,sizeof(D9Vertex))==D9_INVALID);

    assert(!d9_destroy_device_ex(d));
    puts("D3D9 UP draws: list/strip/fan, 16/32-bit indices, padded and declaration strides, malformed rejection, state/refcount/budget preservation passed");
    return 0;
}
