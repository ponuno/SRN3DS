#include "d3d9_core.h"
#include "d3d9_citro3d.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void decode(void){float f[4];uint16_t half[]={1,0x3c00,0xbc00,0x7c00};
    assert(D9_USAGE_COLOR==10);
    assert(!d9_decl_decode(D9_DECL_FLOAT16_4,half,8,f)&&f[0]==ldexpf(1,-24)&&f[1]==1&&f[2]==-1&&isinf(f[3]));
    uint8_t color[]={0,128,255,255};assert(!d9_decl_decode(D9_DECL_COLOR,color,4,f)&&f[0]==1&&f[1]==128/255.0f&&f[2]==0&&f[3]==1);
    uint16_t sn[]={0x8000,0x7fff};assert(!d9_decl_decode(D9_DECL_SHORT2N,sn,4,f)&&f[0]==-1&&f[1]==1&&f[2]==0&&f[3]==1);
    uint32_t packed=512u|(511u<<10);assert(!d9_decl_decode(D9_DECL_DEC3N,&packed,4,f)&&f[0]==-1&&f[1]==1&&f[2]==0);
    assert(!d9_decl_decode(D9_DECL_UDEC3,&packed,4,f)&&f[0]==512&&f[1]==511);
    assert(d9_decl_decode(D9_DECL_FLOAT3,half,8,f)<0);
    D9Decl d;D9VertexElement e[]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},{0,12,D9_DECL_DEC3N,0,D9_USAGE_NORMAL,0},{1,0,D9_DECL_FLOAT16_2,0,D9_USAGE_TEXCOORD,0}};
    assert(!d9_decl_init(&d,e,3)&&d.stream_mask==3&&d.required[0]==16&&d.required[1]==4);
    e[1].offset=8;assert(d9_decl_init(&d,e,3)<0);e[1].offset=12;e[2].stream=16;assert(d9_decl_init(&d,e,3)<0);
    e[2].stream=1;e[0].usage=D9_USAGE_POSITIONT;assert(d9_decl_init(&d,e,3)<0);
    D9VertexElement supported[]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},{1,0,D9_DECL_FLOAT16_2,0,D9_USAGE_TEXCOORD,0}};
    assert(!d9_decl_init(&d,supported,2)&&d.position==0&&d.uv==1&&d.stream_mask==3);
    D9VertexElement color_elements[]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},{0,12,D9_DECL_COLOR,0,D9_USAGE_COLOR,0}};
    assert(!d9_decl_init(&d,color_elements,2)&&d.color!=UINT32_MAX); /* COLOR0 is admitted as data */
    color_elements[1].type=D9_DECL_FLOAT4;assert(!d9_decl_init(&d,color_elements,2)); /* float colour too */
    color_elements[1].type=D9_DECL_FLOAT16_4;assert(!d9_decl_init(&d,color_elements,2)&&d.color!=UINT32_MAX); /* retail half-float RGBA */
    color_elements[1].type=D9_DECL_FLOAT3;assert(d9_decl_init(&d,color_elements,2)<0); /* three components are not */
    color_elements[1].type=D9_DECL_COLOR;color_elements[1].usage_index=1;
    assert(!d9_decl_init(&d,color_elements,2)&&d.color==UINT32_MAX); /* COLOR1 is unused, not the draw tint */
}
static void sampler_state(void){
    D9DrawState state={.resident_mips=4,.max_mip=1,.mip_filter=0,.mag_filter=2,.min_filter=2};
    D9CitroTextureConfig config=d9_citro_texture_config(4,&state);
    assert(config.min_level==1&&config.max_level==3&&config.mip_filter==0);
    assert(config.mag_filter==2&&config.min_filter==2); /* NONE changes only mip filtering */
    state.mip_filter=1;config=d9_citro_texture_config(4,&state);
    assert(config.min_level==1&&config.max_level==3&&config.mip_filter==1);
    state.mip_filter=2;config=d9_citro_texture_config(4,&state);
    assert(config.min_level==1&&config.max_level==3&&config.mip_filter==2);
    state.max_mip=10;config=d9_citro_texture_config(4,&state);
    assert(config.min_level==3&&config.max_level==3); /* clamp MAXMIPLEVEL to resident range */
    state.resident_mips=2;config=d9_citro_texture_config(4,&state);
    assert(config.min_level==1&&config.max_level==1); /* a resident hole caps the range */
}
typedef struct {D9Vertex v[6];size_t n;unsigned calls;} Mock;
static int tex_new(void *ctx,unsigned w,unsigned h,unsigned lv,void **out){(void)ctx;(void)w;(void)h;(void)lv;(void)out;return -1;}
static int tex_upload(void *ctx,void *tex,unsigned lv,const void *p,size_t n){(void)ctx;(void)tex;(void)lv;(void)p;(void)n;return -1;}
static void tex_del(void *ctx,void *tex){(void)ctx;(void)tex;}
static int begin(void *ctx,uint32_t color){(void)ctx;(void)color;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *s){Mock *a=ctx;(void)m;(void)t;(void)s;assert(n<=6);memcpy(a->v,v,n*sizeof *v);a->n=n;++a->calls;return 0;}
static void end(void *ctx){(void)ctx;}
static const D9Backend ops={tex_new,tex_upload,tex_del,begin,draw,end};
int main(void){decode();sampler_state();Mock m={0};D9Device *d=d9_create_device(&ops,&m,1<<20);assert(d);
    D9Handle vb,uv,ib;void *p=NULL;float pos[4][3]={{0,0,0},{1,0,0},{2,0,0},{3,0,0}};uint16_t tex[4][2]={{0,0},{0x3c00,0},{0,0x3c00},{0x3c00,0x3c00}};
    uint32_t indices[]={1,2,3,4};assert(!d9_create_vertex_buffer(d,sizeof pos,&vb)&&!d9_buffer_lock(d,vb,0,0,&p));memcpy(p,pos,sizeof pos);assert(!d9_buffer_unlock(d,vb));
    assert(!d9_create_vertex_buffer(d,sizeof tex,&uv)&&!d9_buffer_lock(d,uv,0,0,&p));memcpy(p,tex,sizeof tex);assert(!d9_buffer_unlock(d,uv));
    assert(!d9_create_index_buffer(d,sizeof indices,D9_INDEX32,&ib)&&!d9_buffer_lock(d,ib,0,0,&p));memcpy(p,indices,sizeof indices);assert(!d9_buffer_unlock(d,ib));
    D9VertexElement e[]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},{1,0,D9_DECL_FLOAT16_2,0,D9_USAGE_TEXCOORD,0}};D9Decl decl;
    assert(!d9_decl_init(&decl,e,2)&&!d9_set_vertex_declaration(d,&decl)&&!d9_set_stream(d,0,vb,0,12)&&!d9_set_indices(d,ib));
    D9Decl rejected=decl;rejected.elements[1].usage=D9_USAGE_COLOR;
    assert(d9_set_vertex_declaration(d,&rejected)==D9_INVALID);
    assert(!d9_set_vertex_declaration(d,&decl));
    assert(!d9_begin_scene(d,0));assert(d9_draw_primitive(d,D9_TRIANGLESTRIP,0,2)==D9_INVALID);assert(!d9_set_stream(d,1,uv,0,4));
    assert(!d9_draw_primitive(d,D9_TRIANGLESTRIP,0,2)&&m.n==6);assert(m.v[0].x==0&&m.v[1].x==1&&m.v[2].x==2&&m.v[3].x==2&&m.v[4].x==1&&m.v[5].x==3);
    assert(m.v[5].u==1&&m.v[5].v==1);
    assert(!d9_draw_indexed_range(d,D9_TRIANGLEFAN,-1,1,4,0,2)&&m.v[3].x==0&&m.v[4].x==2&&m.v[5].x==3);
    assert(d9_draw_indexed_range(d,D9_TRIANGLEFAN,-1,2,3,0,2)==D9_INVALID);
    assert(d9_draw_indexed_range(d,D9_TRIANGLELIST,0,1,4,1,1)==D9_INVALID); /* last index 4 overruns streams */
    assert(!d9_present(d));assert(!d9_release_buffer(d,uv)&&!d9_release_buffer(d,vb)&&!d9_release_buffer(d,ib)&&d9_memory_used(d)==0);
    d9_destroy_device(d);puts("Vertex declarations, colour rejection, mip state, multiple streams, INDEX32, strip/fan winding and bounds passed");return 0;
}
