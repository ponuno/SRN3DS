/* Host regression for the D3DX9_42 cube-texture loader and the
 * IDirect3DCubeTexture9 COM path.
 *
 * The synthetic DDS files are built in guest memory: six faces, face-major
 * level payloads, A8R8G8B8 / X8R8G8B8 / DXT1 / DXT3 / DXT5.  The test checks
 * every face and mip through GetCubeMapSurface + LockRect, rejects truncated
 * or incomplete cubes, proves the 2D loaders still refuse a cube DDS, and
 * verifies SetTexture/GetTexture/refcounts. */
#include "d3d9_bc.h"
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "d3dx_texture_shims.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP (SCRATCH+0x200)
#define INFO (SCRATCH+0x400)
#define OUT (SCRATCH+0x500)
#define OUT2 (SCRATCH+0x600)
#define DATA (SCRATCH+0x800)
#define DATA2 (SCRATCH+0x4000)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_E_NOTIMPL 0x80004001u
#define HR_INVALIDDATA 0x88760b59u
#define HR_INVALIDCALL 0x8876086cu
#define HR_NOTAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_DXT1 0x31545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT5 0x35545844u
#define CUBE_CAPS 0x0000fe00u /* CUBEMAP | six face bits */

static void wr32(uint8_t *p,uint32_t v){
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
extern void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache);
static void guest_dispatch_probe(CPU *c){
    uint32_t sp=c->r[4],target=M32(sp+4),n=M32(sp+8);
    if(n>16)rt_unimpl(c,target,"probe argument count");
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=M32(sp+8+i*4);}
    c->r[4]-=4;M32(c->r[4])=PROBE_FN+4;
    static const FnEntry *cache;
    if(cache&&cache->addr==target)cache->fn(c);else rt_call_ic(c,target,&cache);
    if(c->r[4]!=sp)rt_unimpl(c,target,"D3DX cube stdcall stack mismatch");
    c->r[4]=sp+4;
}
typedef struct {GuestMemory memory;CPU cpu;uint32_t factory,device;} Harness;
static int invoke(Harness *h,uint32_t target,const uint32_t *args,unsigned n,uint32_t *result){
    if(n>16)return -1;
    uint32_t frame[18]={target,n};for(unsigned i=0;i<n;++i)frame[i+2]=args[i];
    return n3ds_lift_invoke(&h->cpu,PROBE_FN,frame,n+2,result);
}
static uint32_t call(Harness *h,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t vt=0,fn=0;assert(!guest_read32(&h->memory,object,&vt)&&vt);
    assert(!guest_read32(&h->memory,vt+slot*4,&fn)&&fn);
    uint32_t frame[18]={object};for(unsigned i=0;i<n;++i)frame[i+1]=args[i];
    uint32_t r=0;assert(!invoke(h,fn,frame,n+1,&r));return r;
}
static uint32_t rd(Harness *h,uint32_t address){uint32_t v=0;assert(!guest_read32(&h->memory,address,&v));return v;}
static uint32_t resolve(const char *name){PeImport e={.dll="d3dx9_42.dll",.name=name};return win32_resolve(&e,NULL);}
static uint32_t shim_call(Harness *h,const char *name,const uint32_t *args,unsigned n){
    uint32_t fn=resolve(name);assert(fn);
    uint32_t r=0;assert(!invoke(h,fn,args,n,&r));return r;
}

static size_t dds_header(uint8_t *b,unsigned w,unsigned h,unsigned mips,unsigned format,
                         uint32_t caps2){
    memset(b,0,128);memcpy(b,"DDS ",4);wr32(b+4,124);
    uint32_t flags=0x1u|0x2u|0x4u|0x1000u|0x80000u;
    if(mips>1)flags|=0x20000u;
    wr32(b+8,flags);wr32(b+12,h);wr32(b+16,w);if(mips>1)wr32(b+28,mips);
    wr32(b+76,32);
    if(format==FMT_A8R8G8B8){
        wr32(b+80,0x41u);wr32(b+88,32);wr32(b+92,0x00ff0000u);
        wr32(b+96,0x0000ff00u);wr32(b+100,0x000000ffu);wr32(b+104,0xff000000u);
    }else if(format==FMT_X8R8G8B8){
        wr32(b+80,0x41u);wr32(b+88,32);wr32(b+92,0x00ff0000u);
        wr32(b+96,0x0000ff00u);wr32(b+100,0x000000ffu);
    }else{
        wr32(b+80,0x4u);wr32(b+84,format);
    }
    wr32(b+108,0x1000u|(mips>1?0x400008u:0u));
    wr32(b+112,caps2);
    return 128;
}
static size_t a8_cube(uint8_t *b,unsigned edge,unsigned mips,unsigned seed){
    size_t off=dds_header(b,edge,edge,mips,FMT_A8R8G8B8,CUBE_CAPS);
    unsigned dim=edge;
    for(unsigned face=0;face<6;++face){
        dim=edge;
        for(unsigned level=0;level<mips;++level){
            size_t words=(size_t)dim*dim;
            for(size_t i=0;i<words;++i){
                uint32_t v=0xff000000u|((face+1u)<<16)|(level<<8)|((seed+i)&0xffu);
                wr32(b+off+i*4,v);
            }
            off+=words*4;if(dim>1)dim>>=1;
        }
    }
    return off;
}
static size_t x8_cube(uint8_t *b,unsigned edge,unsigned seed){
    size_t off=dds_header(b,edge,edge,1,FMT_X8R8G8B8,CUBE_CAPS);
    for(unsigned face=0;face<6;++face)
        for(size_t i=0;i<(size_t)edge*edge;++i)
            wr32(b+off+face*edge*edge*4+i*4,0xff000000u|((face+1u)<<16)|((seed+i)&0xffu));
    return off+6u*edge*edge*4u;
}
static void bc_block(uint8_t *p,unsigned format,unsigned color565,uint8_t alpha){
    if(format==FMT_DXT1){
        p[0]=(uint8_t)color565;p[1]=(uint8_t)(color565>>8);
        p[2]=p[3]=0;p[4]=p[5]=p[6]=p[7]=0;return;
    }
    if(format==FMT_DXT3){
        memset(p,0x11,8); /* alpha nibble 1 -> 17 */
    }else{
        p[0]=alpha;p[1]=alpha;memset(p+2,0,6); /* DXT5 alpha indices 0 */
    }
    p[8]=(uint8_t)color565;p[9]=(uint8_t)(color565>>8);
    p[10]=p[11]=0;p[12]=p[13]=p[14]=p[15]=0;
}
static size_t bc_cube(uint8_t *b,unsigned edge,unsigned mips,unsigned format,
                      const uint16_t *colors,uint8_t alpha){
    size_t off=dds_header(b,edge,edge,mips,format,CUBE_CAPS);
    unsigned block=(format==FMT_DXT1)?8u:16u;
    for(unsigned face=0;face<6;++face){
        unsigned dim=edge;
        for(unsigned level=0;level<mips;++level){
            unsigned blocks=((dim+3u)/4u)*((dim+3u)/4u);
            for(unsigned i=0;i<blocks;++i)
                bc_block(b+off+(size_t)i*block,format,colors[(face+level)%6],alpha);
            off+=(size_t)blocks*block;if(dim>1)dim>>=1;
        }
    }
    return off;
}
static uint32_t create_cube(Harness *h,const uint8_t *data,size_t bytes){
    assert(!guest_write(&h->memory,DATA,data,bytes));
    uint32_t args[]={h->device,DATA,(uint32_t)bytes,OUT};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(shim_call(h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_OK);
    uint32_t cube=rd(h,OUT);assert(cube);return cube;
}
static uint32_t cube_surface(Harness *h,uint32_t cube,unsigned face,unsigned level){
    uint32_t args[]={face,level,OUT2};
    assert(!guest_write32(&h->memory,OUT2,0));
    assert(call(h,cube,18,args,3)==HR_OK);
    uint32_t surface=rd(h,OUT2);assert(surface);return surface;
}
static void check_a8_cube(Harness *h,uint32_t cube,unsigned edge,unsigned mips,unsigned seed){
    assert(call(h,cube,13,NULL,0)==mips);
    for(unsigned face=0;face<6;++face){
        for(unsigned level=0;level<mips;++level){
            unsigned dim=edge>>level;if(!dim)dim=1;
            uint32_t surface=cube_surface(h,cube,face,level);
            uint32_t desc[]={level,OUT};
            assert(call(h,cube,17,desc,2)==HR_OK);
            assert(rd(h,OUT+0)==FMT_A8R8G8B8&&rd(h,OUT+24)==dim&&rd(h,OUT+28)==dim);
            uint32_t lock[]={OUT,0,0};
            assert(call(h,surface,13,lock,3)==HR_OK);
            uint32_t pitch=rd(h,OUT),ptr=rd(h,OUT+4);
            assert(pitch==dim*4);
            const uint32_t *px=guest_ptr(&h->memory,ptr,(size_t)pitch*dim);assert(px);
            for(unsigned y=0;y<dim;++y)for(unsigned x=0;x<dim;++x){
                uint32_t want=0xff000000u|((face+1u)<<16)|(level<<8)|((seed+y*dim+x)&0xffu);
                assert(px[y*dim+x]==want);
            }
            assert(call(h,surface,14,NULL,0)==HR_OK);
            assert(call(h,surface,2,NULL,0)==0);
        }
    }
}
static void check_bc_cube(Harness *h,uint32_t cube,unsigned edge,unsigned mips,
                          unsigned format,const uint16_t *colors,uint8_t alpha){
    assert(call(h,cube,13,NULL,0)==mips);
    for(unsigned face=0;face<6;++face){
        for(unsigned level=0;level<mips;++level){
            unsigned dim=edge>>level;if(!dim)dim=1;
            uint32_t surface=cube_surface(h,cube,face,level);
            uint32_t lock[]={OUT,0,0};
            assert(call(h,surface,13,lock,3)==HR_OK);
            uint32_t pitch=rd(h,OUT),ptr=rd(h,OUT+4);
            unsigned bb=(format==FMT_DXT1)?8u:16u;
            assert(pitch==((dim+3u)/4u)*bb);
            const uint8_t *blocks=guest_ptr(&h->memory,ptr,(size_t)pitch*((dim+3u)/4u));
            assert(blocks);
            uint8_t rgba[64];
            assert(d9_bc_decode_block(format,blocks,bb,rgba)==D9_OK);
            unsigned r=(colors[(face+level)%6]>>11)&31,g=(colors[(face+level)%6]>>5)&63,
                     b=colors[(face+level)%6]&31;
            uint8_t er=(uint8_t)((r<<3)|(r>>2)),eg=(uint8_t)((g<<2)|(g>>4)),eb=(uint8_t)((b<<3)|(b>>2));
            uint8_t ea=format==FMT_DXT3?17u:format==FMT_DXT5?alpha:255u;
            assert(rgba[0]==er&&rgba[1]==eg&&rgba[2]==eb&&rgba[3]==ea);
            assert(call(h,surface,14,NULL,0)==HR_OK);
            assert(call(h,surface,2,NULL,0)==0);
        }
    }
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x6000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());
    assert(!d3dx_texture_shims_install(&h.memory));
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[3]={{"probe",probe,1},d3dx_texture_shims_module(),d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,3));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);

    uint32_t create9=d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"},NULL);
    assert(create9);
    uint32_t sdk=32;assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    assert(!guest_write(&h.memory,PP,pp,sizeof pp));
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==HR_OK);
    assert(!guest_read32(&h.memory,OUT,&h.device)&&h.device);

    uint8_t dds[8192];
    /* A8R8G8B8 cube, 8x8, two mips, all six faces. */
    size_t bytes=a8_cube(dds,8,2,0x20);
    uint32_t cube=create_cube(&h,dds,bytes);
    check_a8_cube(&h,cube,8,2,0x20);
    assert(call(&h,cube,10,NULL,0)==5); /* D3DRTYPE_CUBETEXTURE */
    /* Direct IDirect3DCubeTexture9::LockRect/UnlockRect too. */
    {uint32_t args[]={0,0,OUT,0,0};
     assert(call(&h,cube,19,args,5)==HR_OK);
     uint32_t pitch=rd(&h,OUT),ptr=rd(&h,OUT+4);
     assert(pitch==32);
     const uint32_t *px=guest_ptr(&h.memory,ptr,256);assert(px);
     assert(px[0]==(0xff000000u|(1u<<16)|0x20u));
     uint32_t un[]={0,0};assert(call(&h,cube,20,un,2)==HR_OK);}
    /* SetTexture/GetTexture/refcount. */
    {uint32_t set[]={0,cube};assert(call(&h,h.device,65,set,2)==HR_OK);
     uint32_t get[]={0,OUT};assert(call(&h,h.device,64,get,2)==HR_OK&&rd(&h,OUT)==cube);
     assert(call(&h,rd(&h,OUT),2,NULL,0)>=1);
     uint32_t unbind[]={0,0};assert(call(&h,h.device,65,unbind,2)==HR_OK);}
    assert(call(&h,cube,2,NULL,0)==0);

    /* X8R8G8B8 cube face creation. */
    bytes=x8_cube(dds,8,0x40);cube=create_cube(&h,dds,bytes);
    {uint32_t desc[]={0,OUT};assert(call(&h,cube,17,desc,2)==HR_OK);
     assert(rd(&h,OUT)==FMT_X8R8G8B8);
     uint32_t surface=cube_surface(&h,cube,3,0);
     uint32_t lock[]={OUT,0,0};assert(call(&h,surface,13,lock,3)==HR_OK);
     assert(rd(&h,OUT)==32);
     const uint32_t *px=guest_ptr(&h.memory,rd(&h,OUT+4),256);assert(px);
     assert(px[0]==(0xff000000u|(4u<<16)|0x40u));
     assert(call(&h,surface,14,NULL,0)==HR_OK);
     assert(call(&h,surface,2,NULL,0)==0);}
    assert(call(&h,cube,2,NULL,0)==0);

    /* BC1/BC3/BC5 cube faces, two mips where useful. */
    static const uint16_t colors[6]={0xf800u,0x07e0u,0x001fu,0xffe0u,0xf81fu,0x07ffu};
    bytes=bc_cube(dds,8,1,FMT_DXT1,colors,255);cube=create_cube(&h,dds,bytes);
    check_bc_cube(&h,cube,8,1,FMT_DXT1,colors,255);
    assert(call(&h,cube,2,NULL,0)==0);
    bytes=bc_cube(dds,8,2,FMT_DXT3,colors,255);cube=create_cube(&h,dds,bytes);
    check_bc_cube(&h,cube,8,2,FMT_DXT3,colors,255);
    assert(call(&h,cube,2,NULL,0)==0);
    bytes=bc_cube(dds,8,1,FMT_DXT5,colors,0x80);cube=create_cube(&h,dds,bytes);
    check_bc_cube(&h,cube,8,1,FMT_DXT5,colors,0x80);
    assert(call(&h,cube,2,NULL,0)==0);

    /* Incomplete/truncated/non-cube DDS rejection. */
    {size_t n=dds_header(dds,8,8,1,FMT_A8R8G8B8,0x200u);
     assert(!guest_write(&h.memory,DATA,dds,n));
     uint32_t args[]={h.device,DATA,(uint32_t)n,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"face bits"));}
    {bytes=a8_cube(dds,8,1,0x10);size_t short_bytes=bytes-64; /* last face incomplete */
     assert(!guest_write(&h.memory,DATA,dds,short_bytes));
     uint32_t args[]={h.device,DATA,(uint32_t)short_bytes,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"truncated"));}
    {size_t n=dds_header(dds,8,16,1,FMT_A8R8G8B8,CUBE_CAPS);
     assert(!guest_write(&h.memory,DATA,dds,n));
     uint32_t args[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(strstr(d3dx_texture_shims_last_error(),"square"));}
    {size_t n=dds_header(dds,8,8,1,FMT_A8R8G8B8,0); /* ordinary 2D DDS */
     assert(!guest_write(&h.memory,DATA,dds,n));
     uint32_t args[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(strstr(d3dx_texture_shims_last_error(),"not a cube"));}
    /* The 2D loader still refuses a cube DDS by name. */
    {bytes=a8_cube(dds,8,1,0x10);assert(!guest_write(&h.memory,DATA,dds,bytes));
     uint32_t args[]={h.device,DATA,(uint32_t)bytes,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",args,4)==HR_E_NOTIMPL);
     assert(rd(&h,OUT)==0);}

    /* A backend without cube callbacks (the PICA shape) must refuse cube
     * creation by name rather than pretend a CPU-only cube can be sampled. */
    {D9Backend ops=*d9_null_backend();ops.texture_create_cube=NULL;ops.texture_upload_cube=NULL;
     D9Device *dev=d9_create_device(&ops,NULL,1u<<20);assert(dev);
     D9Handle hcube=0;assert(d9_create_cube_texture_ex(dev,8,1,D9_A8R8G8B8,0,0,&hcube)==D9_UNSUPPORTED);
     assert(strstr(d9_last_draw_diagnostic(dev),"cube-map"));
     d9_destroy_device(dev);}

    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("D3DX9_42 cube texture shims: PASS");
    return 0;
}
