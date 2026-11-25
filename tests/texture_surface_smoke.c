/* IDirect3DTexture9 level surfaces and AddDirtyRect through real guest COM.
 *
 * The engine asks for a level surface and writes it through LockRect, so the
 * view must share the parent texture's CPU store and re-upload on unlock. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "win32_shims.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {size_t base;unsigned levels;uint8_t *data;} Tex;
static unsigned created,uploaded,deleted;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;Tex *t=calloc(1,sizeof *t);assert(t);t->levels=levels;t->base=w*h;*native=t;++created;return 0;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){
    (void)ctx;(void)level;(void)data;Tex *t=native;assert(t&&n);++uploaded;return 0;
}
static void tex_delete(void *ctx,void *native){(void)ctx;Tex *t=native;if(t){free(t->data);free(t);++deleted;}}
static int begin(void *ctx,uint32_t color){(void)ctx;(void)color;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *st){(void)ctx;(void)v;(void)n;(void)m;(void)t;(void)st;return 0;}
static void end(void *ctx){(void)ctx;}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end};

static uint32_t call(D9ComDemo *s,uint32_t obj,unsigned slot,const uint32_t *a,unsigned n){
    uint32_t r=0;assert(!d9_com_demo_call(s,obj,slot,a,n,&r));return r;
}
static uint32_t out32(D9ComDemo *s,uint32_t addr){uint32_t v=0;assert(!guest_read32(&s->memory,addr,&v));return v;}

int main(void){
    D9Device *d=d9_create_device(&ops,NULL,4u<<20);assert(d);
    D9ComDemo s;
    if(d9_com_demo_init(&s,d)){fprintf(stderr,"COM init failed: %s\n",s.message);return 1;}
    assert(s.texture&&created==1&&uploaded>=1);

    /* GetSurfaceLevel(0) returns a real surface with the level's dimensions. */
    uint32_t lvl[]={0,0x10010400};
    assert(call(&s,s.texture,18,lvl,2)==0);
    uint32_t surf=out32(&s,0x10010400);assert(surf);
    uint32_t desc=0x10010440;
    assert(call(&s,surf,12,&desc,1)==0);
    uint32_t m[8];assert(!guest_read(&s.memory,desc,m,sizeof m));
    assert(m[0]==D9_A8R8G8B8&&m[6]==64&&m[7]==64); /* Format, Width, Height */

    /* The level view locks the parent store: pitch and pixels are the level's. */
    unsigned before=uploaded;
    uint32_t lock[]={0x10010480,0,0}; /* pLockedRect, pRect=NULL, Flags=0 */
    uint32_t lrc=call(&s,surf,13,lock,3);
    if(lrc){printf("surface lock rc=0x%08x surf=0x%08x\n",lrc,surf);return 2;}
    uint32_t pitch=out32(&s,0x10010480),pixels=out32(&s,0x10010484);
    assert(pitch==256&&pixels);
    uint32_t *p=guest_ptr(&s.memory,pixels,pitch*64);assert(p);
    p[0]=0xff112233u;p[64*63]=0xff445566u;
    assert(call(&s,surf,14,NULL,0)==0);
    assert(uploaded==before+1); /* the parent texture was re-uploaded */

    /* The texture is still bindable after the surface write. */
    assert(!d9_com_demo_select_texture(&s,0));

    /* GetContainer(IID_IDirect3DTexture9) returns the parent texture. */
    const uint8_t tex_iid[16]={0x27,0x12,0xc3,0x85,0xe5,0x3d,0x00,0x4f,0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5};
    assert(!guest_write(&s.memory,0x100104b0,tex_iid,16));
    uint8_t check[16];assert(!guest_read(&s.memory,0x100104b0,check,16)&&!memcmp(check,tex_iid,16));
    uint32_t container[]={0x100104b0,0x10010490};
    uint32_t crc=call(&s,surf,11,container,2);
    if(crc){printf("container rc=0x%08x\n",crc);return 3;}
    uint32_t container_obj=out32(&s,0x10010490);
    assert(container_obj==s.texture);
    /* GetContainer returns a reference; the caller must release it.  Release
     * returns the remaining ULONG refcount, and the demo still owns the
     * texture, so the result stays positive. */
    assert(call(&s,container_obj,2,NULL,0)>0);

    /* AddDirtyRect is advisory: NULL and in-bounds accepted, outside rejected. */
    uint32_t none[]={0};assert(call(&s,s.texture,21,none,1)==0);
    uint32_t rect_addr=0x100104a0,arg=rect_addr;
    assert(!guest_write32(&s.memory,rect_addr+0,0)&&!guest_write32(&s.memory,rect_addr+4,0)&&
           !guest_write32(&s.memory,rect_addr+8,64)&&!guest_write32(&s.memory,rect_addr+12,64));
    assert(call(&s,s.texture,21,&arg,1)==0);
    assert(!guest_write32(&s.memory,rect_addr+8,65));
    assert(call(&s,s.texture,21,&arg,1)==0x8876086c);

    /* Invalid level: no surface, explicit failure. */
    uint32_t bad[]={1,0x10010400};assert(call(&s,s.texture,18,bad,2)==0x8876086c);
    assert(out32(&s,0x10010400)==0);

    /* Releasing the view releases its reference to the texture. */
    assert(call(&s,surf,2,NULL,0)==0);
    assert(!d9_com_demo_select_texture(&s,0));
    assert(!d9_com_demo_destroy(&s));
    printf("texture level surface: PASS\n");
    return 0;
}
