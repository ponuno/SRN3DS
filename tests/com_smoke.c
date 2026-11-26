#include "d3d9_com_demo.h"
#include "d3d9_shader.h"
#include "d3d9_demo_scene.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct {unsigned created,uploaded,deleted,begin,draw,end;size_t n;int active;D9Matrix clip;D9DrawState state;D9Vertex vertices[36];} Mock;
typedef struct {size_t base;unsigned levels;uint8_t *data;} Tex;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){Mock *m=ctx;Tex *t=calloc(1,sizeof *t);if(!t)return -1;
    t->base=(size_t)w*h*4;t->levels=levels;size_t total=0;for(unsigned i=0;i<levels;++i)total+=t->base>>(2*i);
    t->data=malloc(total);if(!t->data){free(t);return -1;}*native=t;++m->created;return 0;}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){Mock *m=ctx;Tex *t=native;assert(level<t->levels&&n==(t->base>>(2*level)));size_t off=0;for(unsigned i=0;i<level;++i)off+=t->base>>(2*i);memcpy(t->data+off,data,n);++m->uploaded;return 0;}
static void tex_delete(void *ctx,void *native){Mock *m=ctx;Tex *t=native;assert(!m->active);free(t->data);free(t);++m->deleted;}
static int begin(void *ctx,uint32_t color){Mock *m=ctx;(void)color;assert(!m->active);m->active=1;++m->begin;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *state){Mock *s=ctx;assert(s->active&&v&&m&&t&&n<=36);s->n=n;s->clip=*m;s->state=*state;memcpy(s->vertices,v,n*sizeof *v);++s->draw;return 0;}
static void end(void *ctx){Mock *m=ctx;assert(m->active);m->active=0;++m->end;}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end};
static uint32_t method(D9ComDemo *s,uint32_t obj,unsigned slot,const uint32_t *a,unsigned n){uint32_t r=0;assert(!d9_com_demo_call(s,obj,slot,a,n,&r));return r;}
int main(void){
    Mock m={0};D9Device *d=d9_create_device(&ops,&m,4u<<20);assert(d);D9ComDemo s;
    if(d9_com_demo_init(&s,d)){fprintf(stderr,"COM initialization failed: %s\n",s.message);return 1;}
    assert(m.created==1&&m.uploaded==1);
    uint32_t a[]={0,0x10010400,0,0};
    /* Correct descriptor and unsupported/invalid outcomes. */
    assert(!method(&s,s.vb,13,a+1,1));uint32_t bytes;assert(!guest_read32(&s.memory,0x10010410,&bytes)&&bytes==24*sizeof(D9Vertex));
    uint32_t invalid[]={100000,4,0x10010400,0};assert(method(&s,s.vb,11,invalid,4)==0x8876086c);
    uint32_t fvf=0x112;assert(method(&s,s.device,89,&fvf,1)==0x8876086a); /* normal bit is not admitted */
    uint32_t process[] = {0, 0, 1, s.vb, 0, 0};
    uint32_t r;
    assert(!d9_com_demo_call(&s, s.device, 85, process, 6, &r));
    assert(r == 0x8876086au);
    assert(strstr(d9_com_last_diagnostic(), "software vertex processing is not provided"));
    assert(!d9_com_demo_frame(&s,0)&&m.draw==1&&m.n==36&&m.end==1);
    assert(!d9_com_demo_frame(&s,0.7f)&&m.draw==2&&m.end==2);
    D9Matrix fixed=m.clip;D9Vertex fixed_geometry[36];memcpy(fixed_geometry,m.vertices,sizeof fixed_geometry);
    assert(!d9_com_demo_enable_decl(&s)&&!d9_com_demo_select_decl(&s,1));
    assert(!d9_com_demo_frame(&s,0.7f)&&!memcmp(fixed_geometry,m.vertices,sizeof fixed_geometry));
    uint32_t gd[]={0x10010400};assert(!method(&s,s.device,88,gd,1));uint32_t op;
    assert(!guest_read32(&s.memory,gd[0],&op)&&op==s.decl&&method(&s,op,2,NULL,0)==2);
    uint32_t gdata[]={0,0x10010400};assert(!method(&s,s.decl,4,gdata,2));uint32_t ndecl;
    assert(!guest_read32(&s.memory,gdata[1],&ndecl)&&ndecl==4);gdata[0]=0x10010500;
    assert(!method(&s,s.decl,4,gdata,2));D9VertexElement e[4];assert(!guest_read(&s.memory,gdata[0],e,sizeof e)&&e[2].stream==1&&e[2].type==D9_DECL_FLOAT16_2&&e[3].stream==0xff);
    uint32_t getstream[]={1,0x10010400,0x10010404,0x10010408},off,stride;
    assert(!method(&s,s.device,101,getstream,4)&&!guest_read32(&s.memory,getstream[1],&op)&&op==s.decl_uv);
    assert(!guest_read32(&s.memory,getstream[2],&off)&&off==4&&!guest_read32(&s.memory,getstream[3],&stride)&&stride==4&&method(&s,op,2,NULL,0)==2);
    D9Vertex mesh[24];uint16_t idx16[36];uint32_t idx32[36];d9_demo_mesh(mesh,idx16);for(unsigned i=0;i<36;++i)idx32[i]=idx16[i];
    uint32_t ci[]={sizeof idx32,0,D9_INDEX32,1,0x10010400,0},ib32;
    assert(!method(&s,s.device,27,ci,6)&&!guest_read32(&s.memory,ci[4],&ib32));
    uint32_t ilock[]={0,0,ci[4],0},ilp;assert(!method(&s,ib32,11,ilock,4)&&!guest_read32(&s.memory,ilock[2],&ilp));
    assert(!guest_write(&s.memory,ilp,idx32,sizeof idx32)&&!method(&s,ib32,12,NULL,0)&&!method(&s,s.device,104,&ib32,1));
    assert(!d9_com_demo_frame(&s,0.7f)&&!memcmp(fixed_geometry,m.vertices,sizeof fixed_geometry));
    assert(!method(&s,s.device,105,gd,1)&&!guest_read32(&s.memory,gd[0],&op)&&op==ib32&&method(&s,op,2,NULL,0)==2);
    assert(!method(&s,s.device,104,&s.ib,1)&&method(&s,ib32,2,NULL,0)==0);
    assert(!d9_com_demo_select_decl(&s,0));
    assert(!d9_com_demo_enable_bc(&s)&&m.created==4&&m.uploaded==13);
    for(unsigned i=1;i<=3;++i){assert(!d9_com_demo_select_texture(&s,i));assert(!d9_com_demo_frame(&s,0.7f)&&m.state.resident_mips==4);}
    assert(!d9_com_demo_mip(&s,2)&&!d9_com_demo_blend(&s,1));assert(!d9_com_demo_frame(&s,0.7f)&&m.state.mip_filter==2&&m.state.max_mip==2&&m.state.blend&&m.state.src_blend==5&&m.state.dst_blend==6);
    uint32_t samp[]={0,9,0x10010400},rs[]={27,0x10010400},state;
    assert(!method(&s,s.device,68,samp,3)&&!guest_read32(&s.memory,samp[2],&state)&&state==2);
    assert(!method(&s,s.device,58,rs,2)&&!guest_read32(&s.memory,rs[1],&state)&&state==1);
    assert(!d9_com_demo_mip(&s,0)&&!d9_com_demo_blend(&s,0));
    /* Mip tail and partial block-aligned BC locks via real guest pointers. */
    assert(method(&s,s.bc[0],13,NULL,0)==7);uint32_t desc[]={6,0x10010400};
    assert(!method(&s,s.bc[0],17,desc,2)&&!guest_read32(&s.memory,desc[1]+24,&state)&&state==1);
    uint32_t tail[]={6,0x10010400,0,0},tail_ptr,tail_pitch;
    unsigned uploads=m.uploaded;assert(!method(&s,s.bc[0],19,tail,4)&&!guest_read32(&s.memory,tail[1],&tail_pitch)&&tail_pitch==8);
    assert(!guest_read32(&s.memory,tail[1]+4,&tail_ptr)&&!guest_write32(&s.memory,tail_ptr,0x12345678));uint32_t last=6;
    assert(!method(&s,s.bc[0],20,&last,1)&&m.uploaded==uploads);
    tail[3]=0x10;assert(!method(&s,s.bc[0],19,tail,4)&&!guest_read32(&s.memory,tail[1]+4,&tail_ptr)&&!guest_read32(&s.memory,tail_ptr,&state)&&state==0x12345678);
    assert(!method(&s,s.bc[0],20,&last,1)&&m.uploaded==uploads);
    uint32_t rect[]={4,4,12,12};assert(!guest_write(&s.memory,0x10010500,rect,sizeof rect));
    uint32_t part[]={1,0x10010400,0x10010500,0},p1,pitch1,l1=1;
    assert(!method(&s,s.bc[0],19,part,4)&&!guest_read32(&s.memory,part[1],&pitch1)&&pitch1==64&& !guest_read32(&s.memory,part[1]+4,&p1));
    assert(!guest_write32(&s.memory,p1,0x11223344)&&!guest_write32(&s.memory,p1+pitch1,0x55667788));
    assert(!method(&s,s.bc[0],20,&l1,1));part[2]=0;part[3]=0x10;
    assert(!method(&s,s.bc[0],19,part,4)&&!guest_read32(&s.memory,part[1]+4,&p1));
    assert(!guest_read32(&s.memory,p1+72,&state)&&state==0x11223344);
    assert(!guest_read32(&s.memory,p1+136,&state)&&state==0x55667788);
    assert(!method(&s,s.bc[0],20,&l1,1));
    assert(!d9_com_demo_enable_shaders(&s)&&!d9_com_demo_select_shaders(&s,1));
    assert(!d9_com_demo_frame(&s,0.7f)&&!memcmp(&fixed,&m.clip,sizeof fixed)); /* translated VS matches fixed clip math */
    assert(!m.state.pixel_shader); /* accepted SM2 template keeps the fixed-function path */
    uint32_t unknown[32]={0};memcpy(unknown,d9_demo_ps,d9_demo_ps_words*4);unknown[10]=0xa0e40801; /* different sampler */
    assert(!guest_write(&s.memory,0x10010500,unknown,d9_demo_ps_words*4));uint32_t create_shader[]={0x10010500,0x10010400};
    assert(method(&s,s.device,106,create_shader,2)==0x8876086a);uint32_t empty;assert(!guest_read32(&s.memory,create_shader[1],&empty)&&empty==0);
    uint32_t gf[]={0,0x10010400};assert(!method(&s,s.vs,4,gf,2));uint32_t shader_bytes;
    assert(!guest_read32(&s.memory,gf[1],&shader_bytes)&&shader_bytes==60);
    assert(!d9_com_demo_select_shaders(&s,0)&&!d9_com_demo_select_texture(&s,0));
    assert(!d9_com_demo_frame(&s,0.7f)&&!m.state.pixel_shader);
    /* Partial READONLY lock: guest writes must not propagate and staging disappears. */
    uint32_t lock[]={0,sizeof(D9Vertex),0x10010400,0x10};assert(!method(&s,s.vb,11,lock,4));uint32_t p;
    assert(!guest_read32(&s.memory,lock[2],&p)&&guest_ptr(&s.memory,p,sizeof(D9Vertex)));
    uint32_t before;assert(!guest_read32(&s.memory,p,&before));assert(!guest_write32(&s.memory,p,0xdeadbeef));
    assert(!method(&s,s.vb,12,NULL,0)&&!guest_ptr(&s.memory,p,1));
    assert(!method(&s,s.vb,11,lock,4));assert(!guest_read32(&s.memory,lock[2],&p));uint32_t after;
    assert(!guest_read32(&s.memory,p,&after)&&before==after);assert(!method(&s,s.vb,12,NULL,0));
    /* Rebind texture mid-scene, release the last reference and defer its deletion until Present. */
    uint32_t ct[]={32,32,0,0,D9_DXT1,1,0x10010400,0};assert(!method(&s,s.device,23,ct,8));uint32_t temp;
    assert(!guest_read32(&s.memory,ct[6],&temp));uint32_t lr[]={0,ct[6],0,0};assert(!method(&s,temp,19,lr,4));
    assert(!guest_read32(&s.memory,ct[6]+4,&p));memset(guest_ptr(&s.memory,p,512),255,512);uint32_t level=0;
    assert(!method(&s,temp,20,&level,1));lr[0]=2;assert(!method(&s,temp,19,lr,4));
    assert(!guest_read32(&s.memory,ct[6]+4,&p));memset(guest_ptr(&s.memory,p,32),255,32);level=2;assert(!method(&s,temp,20,&level,1));
    assert(!method(&s,s.device,41,NULL,0));
    uint32_t stage[]={0,temp};assert(!method(&s,s.device,65,stage,2));assert(method(&s,temp,2,NULL,0)==1);
    uint32_t draw[]={D9_TRIANGLELIST,0,1};assert(!method(&s,s.device,81,draw,3)&&m.n==3&&m.state.resident_mips==1); /* hole at level 1 */
    /* DrawPrimitiveUP / DrawIndexedPrimitiveUP: stream zero and the index data
     * are borrowed for one call; bound stream/index state, refcounts and the
     * device memory budget must be unchanged. */
    size_t used_before=d9_memory_used(d);
    D9Vertex upverts[3]={{1,2,3,0,1},{4,5,6,1,0},{7,8,9,0,0}};
    uint16_t upindex[3]={0,1,2};
    assert(!guest_write(&s.memory,0x10010700,upverts,sizeof upverts)&&!guest_write(&s.memory,0x10010780,upindex,sizeof upindex));
    unsigned draws=m.draw;uint32_t up[]={D9_TRIANGLELIST,1,0x10010700,sizeof(D9Vertex)};
    assert(!method(&s,s.device,83,up,4)&&m.draw==draws+1&&m.n==3&&!memcmp(m.vertices,upverts,sizeof upverts)); /* stream-zero decode */
    /* Malformed UP requests fail explicitly, not by drawing garbage. */
    uint32_t bad[4]={D9_TRIANGLELIST,1,0,sizeof(D9Vertex)};
    assert(method(&s,s.device,83,bad,4)==0x8876086c); /* null data */
    bad[2]=0x10010700;bad[3]=0;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* zero stride */
    bad[3]=sizeof(D9Vertex)-4;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* stride below XYZ+TEX1 */
    bad[3]=4097;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* stride above MaxStreamStride */
    bad[3]=sizeof(D9Vertex);bad[1]=0;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* zero primitives */
    bad[1]=4096u/3u+1;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* above MaxPrimitiveCount */
    bad[0]=7;bad[1]=1;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* non-triangle type */
    bad[0]=D9_TRIANGLELIST;bad[2]=0x10010ff0;assert(method(&s,s.device,83,bad,4)==0x8876086c); /* crosses mapping */
    assert(m.draw==draws+1&&d9_memory_used(d)==used_before);
    /* The bound stream 0 still points at the real vertex buffer after a UP draw. */
    uint32_t gs[]={0,0x10010800,0x10010804,0x10010808},bound;
    assert(!method(&s,s.device,101,gs,4)&&!guest_read32(&s.memory,gs[1],&bound)&&bound==s.vb);
    assert(!guest_read32(&s.memory,gs[2],&state)&&state==0&&!guest_read32(&s.memory,gs[3],&state)&&state==sizeof(D9Vertex));
    assert(method(&s,bound,2,NULL,0)==2); /* GetStreamSource retain released back to the binding refcount */
    /* Indexed stream-zero draw with a non-zero MinVertexIndex. */
    upindex[0]=upindex[1]=upindex[2]=2;
    assert(!guest_write(&s.memory,0x10010780,upindex,sizeof upindex));
    uint32_t diup[]={D9_TRIANGLELIST,2,1,1,0x10010780,D9_INDEX16,0x10010700,sizeof(D9Vertex)};
    assert(!method(&s,s.device,84,diup,8)&&m.draw==draws+2&&m.n==3&&!memcmp(m.vertices,upverts+2,sizeof(D9Vertex)));
    uint32_t gi[]={0x10010810};assert(!method(&s,s.device,105,gi,1)&&!guest_read32(&s.memory,gi[0],&bound)&&bound==s.ib);
    assert(method(&s,bound,2,NULL,0)==2);
    /* Index format, range and pointer validation. */
    upindex[0]=0;assert(!guest_write(&s.memory,0x10010780,upindex,sizeof upindex));
    diup[2]=1;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* index 0 below MinVertexIndex 2 */
    diup[1]=0;diup[2]=3;upindex[0]=3;assert(!guest_write(&s.memory,0x10010780,upindex,sizeof upindex));
    assert(method(&s,s.device,84,diup,8)==0x8876086c); /* index equals NumVertices */
    diup[2]=0;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* zero NumVertices */
    diup[2]=3;diup[5]=0;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* unsupported index format */
    diup[5]=D9_INDEX16;diup[4]=0;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* null indices */
    diup[4]=0x10010780;diup[6]=0;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* null vertices */
    diup[6]=0x10010ff0;diup[1]=0;diup[2]=3;assert(method(&s,s.device,84,diup,8)==0x8876086c); /* vertex range crosses mapping */
    assert(d9_memory_used(d)==used_before);
    /* Slot 66 readback: only values accepted by slot 67 are returned. */
    uint32_t tss[]={0,1,4};assert(!method(&s,s.device,67,tss,3)); /* COLOROP=MODULATE */
    assert(!guest_write32(&s.memory,0x10010790,0xdeadbeefu));
    tss[0]=0;tss[1]=1;tss[2]=0x10010790;assert(!method(&s,s.device,66,tss,3));
    uint32_t tss_out=0;assert(!guest_read32(&s.memory,0x10010790,&tss_out)&&tss_out==4);
    /* Stage 1 has no second backend stage here, so COLOROP reads DISABLE. */
    tss[0]=1;tss[1]=1;tss[2]=0x10010790;assert(!method(&s,s.device,66,tss,3));
    assert(!guest_read32(&s.memory,0x10010790,&tss_out)&&tss_out==1);
    /* An unimplemented type fails and leaves the output untouched. */
    assert(!guest_write32(&s.memory,0x10010790,0xdeadbeefu));
    tss[0]=0;tss[1]=0x99;tss[2]=0x10010790;assert(method(&s,s.device,66,tss,3)==0x8876086c);
    assert(!guest_read32(&s.memory,0x10010790,&tss_out)&&tss_out==0xdeadbeefu);
    stage[1]=s.texture;assert(!method(&s,s.device,65,stage,2));assert(m.deleted==0);
    assert(!method(&s,s.device,42,NULL,0));uint32_t present[]={0,0,0,0};assert(!method(&s,s.device,17,present,4)&&m.deleted==1);
    /* Binding holds a reference, so an additional Release cannot destroy a bound resource. */
    assert(method(&s,s.vb,1,NULL,0)==3);assert(method(&s,s.vb,2,NULL,0)==2);
    assert(!d9_com_demo_destroy(&s)&&d9_memory_used(d)==0&&m.deleted==5);
    d9_destroy_device(d);puts("COM D3D9: import, x86 vtables, refcounts, resource staging, indexed drawing and Present passed");return 0;
}
