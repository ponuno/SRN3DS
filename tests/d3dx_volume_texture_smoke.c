/* Host regression for D3DX9_42 volume textures and the bounded D3D9 volume COM
 * path.  A volume is a depth-slice store accessed through LockBox/UnlockBox;
 * IDirect3DVolume9/GetVolumeLevel and volume sampling stay named refusals. */
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
#define DESC (SCRATCH+0x1000)
#define BOX (SCRATCH+0x1100)
#define LOCKED (SCRATCH+0x1200)
#define GUID (SCRATCH+0x1300)
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_E_NOTIMPL 0x80004001u
#define HR_NOINTERFACE 0x80004002u
#define HR_INVALIDDATA 0x88760b59u
#define HR_INVALIDCALL 0x8876086cu
#define HR_NOTAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u
#define FMT_DXT1 0x31545844u
#define DDSD_DEPTH 0x00800000u
#define VOLUME_CAPS 0x00200000u

/* d3d9.h IIDs (little-endian in-memory form). */
static const uint8_t iid_iunknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_resource[16]={0x5d,0xc0,0xee,0x05,0x7d,0x8f,0x62,0x43,
                                       0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,0x04};
static const uint8_t iid_base[16]={0x7e,0xa8,0x0c,0x58,0x3c,0x1d,0x54,0x4d,
                                   0x99,0x1d,0xb7,0xd3,0xe3,0xc2,0x98,0xce};
static const uint8_t iid_volume[16]={0x6c,0x52,0x18,0x25,0x89,0xe7,0x11,0x41,
                                     0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6};
static const uint8_t iid_texture[16]={0x27,0x12,0xc3,0x85,0xe5,0x3d,0x00,0x4f,
                                      0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5};

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
    if(c->r[4]!=sp)rt_unimpl(c,target,"D3DX volume stdcall stack mismatch");
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
static size_t vol_header(uint8_t *b,unsigned w,unsigned h,unsigned depth,unsigned mips,
                         unsigned format,uint32_t caps2){
    memset(b,0,128);memcpy(b,"DDS ",4);wr32(b+4,124);
    uint32_t flags=0x1u|0x2u|0x4u|0x1000u|0x800u|DDSD_DEPTH;
    if(mips>1)flags|=0x20000u;
    wr32(b+8,flags);wr32(b+12,h);wr32(b+16,w);wr32(b+24,depth);
    if(mips>1)wr32(b+28,mips);
    wr32(b+76,32);
    if(format==FMT_A8R8G8B8){
        wr32(b+80,0x41u);wr32(b+88,32);wr32(b+92,0x00ff0000u);
        wr32(b+96,0x0000ff00u);wr32(b+100,0x000000ffu);wr32(b+104,0xff000000u);
    }else{
        wr32(b+80,0x4u);wr32(b+84,format);
    }
    wr32(b+108,0x1000u|(mips>1?0x400008u:0u));
    wr32(b+112,caps2);
    return 128;
}
static size_t a8_volume(uint8_t *b,unsigned w,unsigned h,unsigned depth,unsigned mips,
                        unsigned seed){
    size_t off=vol_header(b,w,h,depth,mips,FMT_A8R8G8B8,VOLUME_CAPS);
    unsigned vw=w,vh=h,vd=depth;
    for(unsigned level=0;level<mips;++level){
        for(unsigned z=0;z<vd;++z)
            for(unsigned y=0;y<vh;++y)
                for(unsigned x=0;x<vw;++x){
                    uint32_t value=0xff000000u|(level<<16)|(z<<8)|((seed+y*vw+x)&0xffu);
                    wr32(b+off+((size_t)z*vh*vw+(size_t)y*vw+x)*4,value);
                }
        off+=(size_t)vw*vh*vd*4;
        if(vw>1)vw>>=1;if(vh>1)vh>>=1;if(vd>1)vd>>=1;
    }
    return off;
}
static size_t dxt1_volume(uint8_t *b,unsigned w,unsigned h,unsigned depth,unsigned mips){
    size_t off=vol_header(b,w,h,depth,mips,FMT_DXT1,VOLUME_CAPS);
    unsigned vw=w,vh=h,vd=depth;
    for(unsigned level=0;level<mips;++level){
        unsigned bw=(vw+3u)/4u,bh=(vh+3u)/4u;
        for(unsigned z=0;z<vd;++z)
            for(unsigned i=0;i<bw*bh;++i){
                uint8_t *p=b+off+((size_t)z*bw*bh+i)*8u;
                unsigned colour=level?0x07e0u:0xf800u;
                p[0]=(uint8_t)colour;p[1]=(uint8_t)(colour>>8);
                p[2]=p[3]=p[4]=p[5]=p[6]=p[7]=0;
            }
        off+=(size_t)bw*bh*vd*8u;
        if(vw>1)vw>>=1;if(vh>1)vh>>=1;if(vd>1)vd>>=1;
    }
    return off;
}
static uint32_t create_volume(Harness *h,const uint8_t *data,size_t bytes){
    assert(!guest_write(&h->memory,DATA,data,bytes));
    uint32_t args[]={h->device,DATA,(uint32_t)bytes,OUT};
    assert(!guest_write32(&h->memory,OUT,0xdeadbeefu));
    assert(shim_call(h,"D3DXCreateVolumeTextureFromFileInMemory",args,4)==HR_OK);
    uint32_t v=rd(h,OUT);assert(v);return v;
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
    /* 4x4x2 A8 volume, two mips: level0 4x4x2, level1 2x2x1. */
    size_t bytes=a8_volume(dds,4,4,2,2,0x20);
    unsigned volume=create_volume(&h,dds,bytes);
    /* QueryInterface: the concrete volume IID plus both ancestors succeed;
     * a 2D texture IID does not. */
    {static const struct {const uint8_t *iid;uint32_t result;} q[]={
        {iid_iunknown,HR_OK},{iid_volume,HR_OK},{iid_base,HR_OK},
        {iid_resource,HR_OK},{iid_texture,HR_NOINTERFACE}};
     for(unsigned i=0;i<sizeof q/sizeof q[0];++i){
        assert(!guest_write(&h.memory,GUID,q[i].iid,16));
        uint32_t a[]={GUID,OUT};
        assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
        assert(call(&h,volume,0,a,2)==q[i].result);
        if(!q[i].result){assert(rd(&h,OUT)==volume);assert(call(&h,volume,2,NULL,0)>=1);}
        else assert(rd(&h,OUT)==0);
     }}
    assert(call(&h,volume,10,NULL,0)==4); /* D3DRTYPE_VOLUMETEXTURE */
    assert(call(&h,volume,13,NULL,0)==2);
    {uint32_t d[]={0,DESC};assert(call(&h,volume,17,d,2)==HR_OK);
     assert(rd(&h,DESC+0)==FMT_A8R8G8B8&&rd(&h,DESC+4)==4);
     assert(rd(&h,DESC+16)==4&&rd(&h,DESC+20)==4&&rd(&h,DESC+24)==2);}
    {uint32_t d[]={1,DESC};assert(call(&h,volume,17,d,2)==HR_OK);
     assert(rd(&h,DESC+16)==2&&rd(&h,DESC+20)==2&&rd(&h,DESC+24)==1);}
    /* LockBox full level 0: RowPitch 16, SlicePitch 64, two slices. */
    {uint32_t args[]={0,LOCKED,0,0};
     assert(call(&h,volume,19,args,4)==HR_OK);
     uint32_t rowpitch=rd(&h,LOCKED),slicepitch=rd(&h,LOCKED+4),bits=rd(&h,LOCKED+8);
     assert(rowpitch==16&&slicepitch==64&&bits);
     const uint32_t *px=guest_ptr(&h.memory,bits,(size_t)slicepitch*2);assert(px);
     assert(px[0]==(0xff000000u|0x20u));
     assert(px[slicepitch/4]==(0xff000000u|(1u<<8)|0x20u));
     /* Write the first texel of slice 1 and prove UnlockBox reuploads it. */
     uint32_t *wpx=guest_ptr(&h.memory,bits,(size_t)slicepitch*2);
     wpx[slicepitch/4]=0xff112233u;
     uint32_t un[]={0};assert(call(&h,volume,20,un,1)==HR_OK);
     args[3]=0;assert(call(&h,volume,19,args,4)==HR_OK);
     px=guest_ptr(&h.memory,rd(&h,LOCKED+8),(size_t)slicepitch*2);assert(px);
     assert(px[slicepitch/4]==0xff112233u);
     assert(call(&h,volume,20,un,1)==HR_OK);}
    /* AddDirtyBox: full box accepted, outside/empty rejected. */
    {uint32_t d[]={0,0};assert(call(&h,volume,21,d,2)==HR_OK);
     int32_t bad[6]={0,0,0,5,4,2};assert(!guest_write(&h.memory,BOX,bad,sizeof bad));
     uint32_t d2[]={0,BOX};assert(call(&h,volume,21,d2,2)==HR_INVALIDCALL);}
    /* IDirect3DVolume9 is a named refusal; LockBox is the supported path. */
    {uint32_t d[]={0,OUT};assert(call(&h,volume,18,d,2)==HR_NOTAVAILABLE);assert(rd(&h,OUT)==0);}
    /* SetTexture/GetTexture/refcount for a volume. */
    {uint32_t set[]={0,volume};assert(call(&h,h.device,65,set,2)==HR_OK);
     uint32_t get[]={0,OUT};assert(call(&h,h.device,64,get,2)==HR_OK&&rd(&h,OUT)==volume);
     assert(call(&h,rd(&h,OUT),2,NULL,0)>=1);
     uint32_t unbind[]={0,0};assert(call(&h,h.device,65,unbind,2)==HR_OK);}
    assert(call(&h,volume,2,NULL,0)==0);

    /* 4x4x2 DXT1 volume, two mips: all slices present and decoded. */
    bytes=dxt1_volume(dds,4,4,2,2);volume=create_volume(&h,dds,bytes);
    assert(call(&h,volume,13,NULL,0)==2);
    {uint32_t d[]={0,DESC};assert(call(&h,volume,17,d,2)==HR_OK);
     assert(rd(&h,DESC+0)==FMT_DXT1&&rd(&h,DESC+16)==4&&rd(&h,DESC+24)==2);}
    for(unsigned z=0;z<2;++z){
        uint32_t args[]={0,LOCKED,0,0};assert(call(&h,volume,19,args,4)==HR_OK);
        uint32_t rowpitch=rd(&h,LOCKED),slicepitch=rd(&h,LOCKED+4),bits=rd(&h,LOCKED+8);
        assert(rowpitch==8&&slicepitch==8);
        const uint8_t *p=guest_ptr(&h.memory,bits,8);assert(p);
        uint8_t rgba[64];assert(d9_bc_decode_block(D9_DXT1,p,8,rgba)==D9_OK);
        assert(rgba[0]==255&&rgba[1]==0&&rgba[2]==0&&rgba[3]==255);
        uint32_t un[]={0};assert(call(&h,volume,20,un,1)==HR_OK);
    }
    assert(call(&h,volume,2,NULL,0)==0);

    /* Refusals: missing depth, truncated payload, non-volume DDS. */
    {size_t n=vol_header(dds,4,4,1,1,FMT_A8R8G8B8,VOLUME_CAPS);
     assert(!guest_write(&h.memory,DATA,dds,n));
     uint32_t args[]={h.device,DATA,(uint32_t)n,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"depth"));}
    {bytes=a8_volume(dds,4,4,2,2,0x10);
     assert(!guest_write(&h.memory,DATA,dds,bytes-4));
     uint32_t args[]={h.device,DATA,(uint32_t)(bytes-4),OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"truncated"));}
    {size_t n=vol_header(dds,4,4,1,1,FMT_A8R8G8B8,0);
     assert(!guest_write(&h.memory,DATA,dds,n));
     uint32_t args[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",args,4)==HR_INVALIDDATA);
     assert(strstr(d3dx_texture_shims_last_error(),"not a volume"));}
    /* The 2D loader refuses volume by name; the cube loader refuses it too. */
    {bytes=a8_volume(dds,4,4,2,1,0x10);assert(!guest_write(&h.memory,DATA,dds,bytes));
     uint32_t args[]={h.device,DATA,(uint32_t)bytes,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",args,4)==HR_E_NOTIMPL);
     assert(rd(&h,OUT)==0);
     uint32_t c[]={h.device,DATA,(uint32_t)bytes,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",c,4)==HR_E_NOTIMPL);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"volume"));}

    /* Backend without volume callbacks (the PICA shape) must refuse by name. */
    {D9Backend ops=*d9_null_backend();
     ops.texture_create_volume=NULL;ops.texture_upload_volume=NULL;
     D9Device *dev=d9_create_device(&ops,NULL,1u<<20);assert(dev);
     D9Handle v=0;
     assert(d9_create_volume_texture_ex(dev,4,4,2,1,D9_A8R8G8B8,0,0,&v)==D9_UNSUPPORTED);
     assert(strstr(d9_last_draw_diagnostic(dev),"volume-texture API"));
     d9_destroy_device(dev);}
    /* A bound volume still refuses draw with the exact named reason. */
    {D9Device *dev=d9_create_device(d9_null_backend(),d9_null_context(),1u<<20);assert(dev);
     D9Handle v=0;assert(d9_create_volume_texture_ex(dev,4,4,2,1,D9_A8R8G8B8,0,0,&v)==D9_OK);
     uint8_t zeros[4*4*2*4]={0};
     assert(d9_volume_upload_level(dev,v,0,zeros,16,64,sizeof zeros)==D9_OK);
     assert(d9_begin_scene_ex(dev,0,0)==D9_OK);
     assert(d9_bind_volume_texture(dev,0,v)==D9_OK);
     assert(d9_draw_primitive(dev,D9_TRIANGLELIST,0,1)==D9_UNSUPPORTED);
     assert(strstr(d9_last_draw_diagnostic(dev),"3-component TEXCOORD0"));
     assert(d9_end_scene(dev)==D9_OK);
     assert(d9_release_volume_texture(dev,v)==D9_OK);
     d9_destroy_device(dev);}

    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("D3DX9_42 volume texture shims: PASS");
    return 0;
}
