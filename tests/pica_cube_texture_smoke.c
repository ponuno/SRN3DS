/* Core-path regression for the PICA cube callbacks.
 *
 * citro3d cannot be linked into a host test, so this records the exact core ->
 * backend calls a PICA backend receives: create(edge, levels, format),
 * upload(face, level, bytes) for all six faces and mips, and delete(native)
 * exactly once.  It also covers missing callbacks, create format/VRAM refusals
 * and an upload failure followed by a clean release. */
#include "d3d9_core.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC 0x43554245u /* "CUBE" */

typedef struct {uint32_t magic;} FakeCube;
typedef struct {
    unsigned create_calls,edge,levels,format;int create_rc;
    unsigned upload_calls,delete_calls,alive;
    unsigned last_face,last_level;
    size_t last_bytes,last_sum,last_first,last_last;
    unsigned seq_face[64],seq_level[64];
    size_t seq_bytes[64];
    unsigned fail_face,fail_level;
} Fake;

static int stub_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;(void)w;(void)h;(void)levels;(void)native;return -1;
}
static int stub_upload(void *ctx,void *native,unsigned level,const void *data,size_t bytes){
    (void)ctx;(void)native;(void)level;(void)data;(void)bytes;return -1;
}
static void stub_delete(void *ctx,void *native){(void)ctx;(void)native;}
static int stub_begin(void *ctx,uint32_t argb){(void)ctx;(void)argb;return 0;}
static int stub_draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,
                     void *tex,const D9DrawState *s){
    (void)ctx;(void)v;(void)n;(void)m;(void)tex;(void)s;return 0;
}
static void stub_end(void *ctx){(void)ctx;}

static int fake_create(void *ctx,unsigned edge,unsigned levels,unsigned format,void **native){
    Fake *f=ctx;assert(native);*native=NULL;
    ++f->create_calls;f->edge=edge;f->levels=levels;f->format=format;
    if(f->create_rc)return f->create_rc;
    FakeCube *c=calloc(1,sizeof *c);if(!c)return -3;
    c->magic=MAGIC;++f->alive;*native=c;return 0;
}
static int fake_upload(void *ctx,void *native,unsigned face,unsigned level,
                       const void *data,size_t bytes){
    Fake *f=ctx;FakeCube *c=native;
    assert(c&&c->magic==MAGIC);
    if(f->fail_face&&face==f->fail_face&&level==f->fail_level)return -1;
    if(f->upload_calls<64){
        f->seq_face[f->upload_calls]=face;f->seq_level[f->upload_calls]=level;
        f->seq_bytes[f->upload_calls]=bytes;
    }
    ++f->upload_calls;f->last_face=face;f->last_level=level;f->last_bytes=bytes;
    const uint8_t *p=data;size_t sum=0;
    for(size_t i=0;i<bytes;++i)sum+=p[i];
    f->last_sum=sum;f->last_first=p[0];f->last_last=p[bytes?bytes-1:0];
    return 0;
}
static int fake_delete(void *ctx,void *native){
    Fake *f=ctx;FakeCube *c=native;
    assert(c&&c->magic==MAGIC);
    c->magic=0;++f->delete_calls;--f->alive;free(c);return 0;
}
static void fake_backend(D9Backend *ops){
    memset(ops,0,sizeof *ops);
    ops->texture_create=stub_create;ops->texture_upload=stub_upload;
    ops->texture_delete=stub_delete;ops->begin=stub_begin;
    ops->draw=stub_draw;ops->end=stub_end;
    ops->texture_create_cube=fake_create;ops->texture_upload_cube=fake_upload;
    ops->texture_delete_cube=fake_delete;
}
static void upload_face(D9Device *d,D9Handle cube,unsigned face,unsigned level,
                        uint8_t *buf,size_t bytes,unsigned seed){
    for(size_t i=0;i<bytes;++i)buf[i]=(uint8_t)(seed+i*7u);
    assert(d9_cube_upload_face_level(d,cube,face,level,buf,level?16u:32u,bytes)==D9_OK);
}

int main(void){
    D9Backend ops;fake_backend(&ops);
    Fake f;memset(&f,0,sizeof f);
    D9Device *d=d9_create_device(&ops,&f,1u<<20);assert(d);
    D9Handle cube=0;
    assert(d9_create_cube_texture_ex(d,8,2,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_OK);
    assert(f.create_calls==1&&f.edge==8&&f.levels==2&&f.format==D9_A8R8G8B8);
    assert(f.alive==1);

    unsigned edge=0;size_t pitch=0,bytes=0;
    assert(d9_cube_level_info(d,cube,0,&edge,&pitch,&bytes)==D9_OK);
    assert(edge==8&&pitch==32&&bytes==256);
    assert(d9_cube_level_info(d,cube,1,&edge,&pitch,&bytes)==D9_OK);
    assert(edge==4&&pitch==16&&bytes==64);

    uint8_t buf[256];
    unsigned calls=0;
    for(unsigned face=0;face<D9_CUBE_FACES;++face){
        for(unsigned level=0;level<2;++level){
            size_t n=(level==0)?256u:64u;
            uint8_t *b=buf;
            for(size_t i=0;i<n;++i)b[i]=(uint8_t)(face*31u+level*17u+i*3u);
            /* The fake records the exact contiguous level image. */
            assert(d9_cube_upload_face_level(d,cube,face,level,b,level?16u:32u,n)==D9_OK);
            assert(f.seq_face[calls]==face&&f.seq_level[calls]==level);
            assert(f.seq_bytes[calls]==n);
            size_t sum=0;for(size_t i=0;i<n;++i)sum+=b[i];
            assert(f.last_sum==sum&&f.last_first==b[0]&&f.last_last==b[n-1]);
            ++calls;
        }
    }
    assert(f.upload_calls==12&&calls==12);
    assert(d9_release_cube_texture(d,cube)==D9_OK);
    assert(f.delete_calls==1&&f.alive==0);
    assert(d9_destroy_device_ex(d)==D9_OK);
    assert(f.delete_calls==1); /* no double free on teardown */

    /* X8 format reaches the backend unchanged. */
    memset(&f,0,sizeof f);
    d=d9_create_device(&ops,&f,1u<<20);assert(d);
    assert(d9_create_cube_texture_ex(d,8,1,D9_X8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_OK);
    assert(f.create_calls==1&&f.format==D9_X8R8G8B8);
    upload_face(d,cube,0,0,buf,256,9);
    assert(f.last_face==0&&f.last_level==0&&f.last_bytes==256);
    assert(f.alive==1);
    /* Device teardown owns exactly one delete. */
    assert(d9_destroy_device_ex(d)==D9_OK);
    assert(f.delete_calls==1&&f.alive==0);

    /* Missing callbacks: named refusal, no native. */
    D9Backend no_cube=ops;no_cube.texture_create_cube=NULL;no_cube.texture_upload_cube=NULL;
    memset(&f,0,sizeof f);
    d=d9_create_device(&no_cube,&f,1u<<20);assert(d);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d),"no cube-map texture API"));
    assert(f.create_calls==0&&f.delete_calls==0);
    assert(d9_destroy_device_ex(d)==D9_OK);

    /* Unsupported format (-2) and VRAM (-3) are distinct named refusals. */
    memset(&f,0,sizeof f);f.create_rc=-2;
    d=d9_create_device(&ops,&f,1u<<20);assert(d);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d),"does not support format"));
    assert(f.alive==0);
    assert(d9_destroy_device_ex(d)==D9_OK);
    memset(&f,0,sizeof f);f.create_rc=-3;
    d=d9_create_device(&ops,&f,1u<<20);assert(d);
    assert(d9_create_cube_texture_ex(d,8,1,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_MEMORY);
    assert(strstr(d9_last_draw_diagnostic(d),"VRAM"));
    assert(f.alive==0);
    assert(d9_destroy_device_ex(d)==D9_OK);

    /* Upload failure is named, leaves the cube releasable and deletes once. */
    memset(&f,0,sizeof f);f.fail_face=2;f.fail_level=1;
    d=d9_create_device(&ops,&f,1u<<20);assert(d);
    assert(d9_create_cube_texture_ex(d,8,2,D9_A8R8G8B8,D9_POOL_MANAGED,0,&cube)==D9_OK);
    uint8_t b0[256],b1[64];
    upload_face(d,cube,0,0,b0,256,1);
    upload_face(d,cube,0,1,b1,64,2);
    upload_face(d,cube,1,0,b0,256,3);
    upload_face(d,cube,1,1,b1,64,4);
    upload_face(d,cube,2,0,b0,256,5);
    assert(d9_cube_upload_face_level(d,cube,2,1,b1,16u,64u)==D9_BACKEND);
    assert(strstr(d9_last_draw_diagnostic(d),"upload failed at face 2 level 1"));
    assert(f.alive==1&&f.delete_calls==0);
    assert(d9_release_cube_texture(d,cube)==D9_OK);
    assert(f.delete_calls==1&&f.alive==0);
    assert(d9_destroy_device_ex(d)==D9_OK);
    assert(f.delete_calls==1);

    puts("PICA cube core-path recording: PASS");
    return 0;
}
