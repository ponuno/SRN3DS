#include "d3d9_core.h"
#include "d3d9_demo_scene.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {size_t bytes;uint8_t *data;} MockTex;
typedef struct {unsigned created,deleted,uploads,begins,draws,ends;size_t vertices;int active;D9Matrix clip;D9Vertex first,last;} Mock;
static int create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){Mock *m=ctx;assert(levels==1);MockTex *t=malloc(sizeof *t);if(!t)return -1;
    t->bytes=(size_t)w*h*4;t->data=malloc(t->bytes);if(!t->data){free(t);return -1;}*native=t;++m->created;return 0;}
static int upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){Mock *m=ctx;MockTex *t=native;assert(level==0);
    assert(!m->active&&bytes==t->bytes);memcpy(t->data,data,bytes);++m->uploads;return 0;}
static void del(void *ctx,void *native){Mock *m=ctx;MockTex *t=native;free(t->data);free(t);++m->deleted;}
static int begin(void *ctx,uint32_t color){Mock *m=ctx;assert(!m->active);(void)color;m->active=1;++m->begins;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *clip,void *native,const D9DrawState *state){Mock *m=ctx;assert(m->active&&n&&n%3==0);(void)state;
    if(native)assert(((MockTex *)native)->data);m->vertices=n;m->clip=*clip;m->first=v[0];m->last=v[n-1];++m->draws;return 0;}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;++m->ends;}
static const D9Backend ops={create,upload,del,begin,draw,end};
static void swizzle(void){
    uint8_t in[8*8*4]={0},out[8*8*4]={0};
    in[0]=2;in[1]=3;in[2]=4;in[3]=5; /* top-left maps to Morton(0,7)=42 */
    in[7*8*4]=7;in[7*8*4+1]=8;in[7*8*4+2]=9;in[7*8*4+3]=10; /* bottom-left maps to 0 */
    assert(!d9_swizzle_rgba8(out,sizeof out,in,32,8,8,D9_A8R8G8B8));
    assert(out[42*4]==5&&out[42*4+1]==2&&out[42*4+2]==3&&out[42*4+3]==4);
    assert(out[0]==10&&out[1]==7&&out[2]==8&&out[3]==9);
    assert(!d9_swizzle_rgba8(out,sizeof out,in,32,8,8,D9_X8R8G8B8)&&out[0]==255);
    assert(d9_swizzle_rgba8(out,sizeof out-1,in,32,8,8,D9_A8R8G8B8)==D9_INVALID);
    /* Independent 16x8 test checks the horizontal tile offset. */
    uint8_t wide[16*8*4]={0},tiled[sizeof wide]={0};wide[(7*16+8)*4+2]=123;
    assert(!d9_swizzle_rgba8(tiled,sizeof tiled,wide,64,16,8,D9_A8R8G8B8)&&tiled[64*4+3]==123);
}
static void matrix(void){D9Matrix id,p,out;d9_matrix_identity(&id);d9_matrix_identity(&p);
    d9_pica_transform(&out,&id,&id,&p);
    assert(out.m[0][1]==1&&out.m[1][0]==-1&&out.m[2][2]==1&&out.m[2][3]==-1&&out.m[3][3]==1);
    id.m[3][0]=2;id.m[3][2]=0.25f;D9Matrix v;d9_matrix_identity(&v);d9_pica_transform(&out,&id,&v,&p);
    assert(out.m[1][3]==-2&&out.m[2][3]==-0.75f);
}
int main(void){swizzle();matrix();Mock m={0};D9Device *d=d9_create_device(&ops,&m,1<<20);assert(d);
    D9Demo scene;assert(!d9_demo_init(d,&scene)&&m.created==1&&m.uploads==1);
    assert(!d9_demo_frame(d,&scene,0)&&m.vertices==36&&m.draws==1&&m.begins==1&&m.ends==1);
    D9Matrix first=m.clip;assert(!d9_demo_frame(d,&scene,0.7f)&&memcmp(&first,&m.clip,sizeof first)!=0&&m.draws==2);
    assert(d9_draw_primitive(d,D9_TRIANGLELIST,0,1)==D9_INVALID);
    assert(!d9_begin_scene(d,0)&&d9_begin_scene(d,0)==D9_INVALID);
    assert(d9_draw_indexed_primitive(d,D9_TRIANGLELIST,-100,0,1)==D9_INVALID);
    assert(d9_draw_indexed_primitive(d,D9_TRIANGLELIST,0,35,1)==D9_INVALID);
    assert(d9_draw_primitive(d,D9_TRIANGLELIST,24,1)==D9_INVALID);
    assert(!d9_draw_primitive(d,D9_TRIANGLELIST,0,1)&&m.vertices==3);
    assert(!d9_present(d)&&d9_present(d)==D9_INVALID);
    assert(!d9_release_texture(d,scene.texture)&&!d9_release_buffer(d,scene.vb)&&!d9_release_buffer(d,scene.ib));
    assert(d9_memory_used(d)==0&&d9_release_buffer(d,scene.vb)==D9_INVALID);
    D9Handle h;assert(d9_create_texture(d,1024,1024,D9_A8R8G8B8,&h)==D9_MEMORY);
    assert(d9_create_texture(d,7,8,D9_A8R8G8B8,&h)==D9_INVALID);
    /* Non-square BGRA chain continues down to 1x1, even when PICA can use only level 0. */
    unsigned w,hgt;size_t pitch,bytes;void *p;
    assert(!d9_create_texture_levels(d,16,8,D9_A8R8G8B8,0,&h)&&d9_texture_levels(d,h)==5&&d9_memory_used(d)==1196);
    assert(!d9_texture_level_info(d,h,4,&w,&hgt,&pitch,&bytes)&&w==1&&hgt==1&&pitch==4&&bytes==4);
    unsigned uploads=m.uploads;assert(!d9_texture_lock_level(d,h,4,&p,&pitch));memset(p,0xab,4);
    assert(!d9_texture_unlock_level(d,h,4)&&m.uploads==uploads);
    assert(!d9_texture_lock_level(d,h,4,&p,&pitch)&&((uint8_t *)p)[0]==0xab);
    assert(!d9_texture_unlock_readonly(d,h,4)&&m.uploads==uploads);
    assert(d9_set_texture(d,h)==D9_INVALID); /* no level 0 upload */
    assert(!d9_release_texture(d,h)&&d9_memory_used(d)==0);
    assert(d9_set_render_state(d,23,9)==D9_INVALID&&d9_set_sampler_state(d,7,3)==D9_INVALID);
    d9_destroy_device(d);assert(m.deleted==m.created);
    puts("Graphics core: textures, swizzle, transforms, indexed/nonindexed draws, resource limits passed");return 0;
}
