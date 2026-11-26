/* Host regression for the plain D3DX9_42 texture imports.
 *
 * Proves the three things that used to be false when TESV's IAT pointed at the
 * staged d3dx9_42.dll body:
 *   1. imports resolve to host thunks, not guest DLL addresses;
 *   2. D3DXGetImageInfoFromFileInMemory parses a synthetic DDS from guest
 *      memory and fills all seven D3DXIMAGE_INFO fields;
 *   3. D3DXCreateTextureFromFileInMemory creates a real IDirect3DTexture9
 *      through the d3d9_com/d3d9_core path and LockRect shows the file pixels.
 * Cube/volume creation, file-path exports and non-DDS input must fail with a
 * named HRESULT, never S_OK.
 */
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
#define PROBE_FN 0x00401000u

#define HR_OK 0u
#define HR_E_NOTIMPL 0x80004001u
#define HR_INVALIDDATA 0x88760b59u
#define HR_INVALIDCALL 0x8876086cu

#define FMT_A8R8G8B8 21u
#define FMT_DXT1 0x31545844u

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
    if(c->r[4]!=sp)rt_unimpl(c,target,"D3DX stdcall stack mismatch");
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

/* DDS_HEADER: 4-byte magic + 124-byte header.  Only the fields the parser and
 * D3DX use are populated; pitchOrLinearSize is deliberately left zero because
 * the shim derives the tight layout itself. */
static size_t dds_header(uint8_t *b,unsigned w,unsigned h,unsigned mips,
                         uint32_t pf_flags,uint32_t fourcc,uint32_t bits,
                         uint32_t rmask,uint32_t gmask,uint32_t bmask,uint32_t amask,
                         uint32_t caps2,unsigned depth){
    memset(b,0,128);
    memcpy(b,"DDS ",4);
    wr32(b+4,124);
    uint32_t flags=0x1u|0x2u|0x4u|0x1000u;
    if(mips>1)flags|=0x20000u;
    if(caps2&0x00200000u)flags|=0x00800000u;
    wr32(b+8,flags);
    wr32(b+12,h);wr32(b+16,w);wr32(b+24,depth);
    if(mips>1)wr32(b+28,mips);
    wr32(b+76,32);wr32(b+80,pf_flags);wr32(b+84,fourcc);wr32(b+88,bits);
    wr32(b+92,rmask);wr32(b+96,gmask);wr32(b+100,bmask);wr32(b+104,amask);
    wr32(b+108,0x1000u|(mips>1?0x400008u:0u));
    wr32(b+112,caps2);
    return 128;
}
static size_t make_a8r8g8b8_dds(uint8_t *b,unsigned w,unsigned h,unsigned mips,unsigned seed){
    size_t off=dds_header(b,w,h,mips,0x41u,0,32,0x00ff0000u,0x0000ff00u,
                          0x000000ffu,0xff000000u,0,1);
    unsigned cw=w,ch=h;
    for(unsigned l=0;l<mips;++l){
        size_t bytes=(size_t)cw*4u*ch;
        for(size_t k=0;k<bytes;++k)b[off+k]=(uint8_t)(seed+l*37u+k);
        off+=bytes;
        if(cw>1)cw>>=1;if(ch>1)ch>>=1;
    }
    return off;
}
static size_t make_dxt1_dds(uint8_t *b,unsigned w,unsigned h,unsigned seed){
    size_t off=dds_header(b,w,h,1,0x4u,FMT_DXT1,0,0,0,0,0,0,1);
    size_t bytes=(size_t)((w+3u)/4u)*((h+3u)/4u)*8u;
    for(size_t k=0;k<bytes;++k)b[off+k]=(uint8_t)(seed+k);
    return off+bytes;
}

static uint32_t resolve(const char *name){
    PeImport e={.dll="d3dx9_42.dll",.name=name};
    return win32_resolve(&e,NULL);
}
static uint32_t shim_call(Harness *h,const char *name,const uint32_t *args,unsigned n){
    uint32_t fn=resolve(name);assert(fn);
    uint32_t r=0;assert(!invoke(h,fn,args,n,&r));return r;
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,2u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x4000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());
    assert(!d3dx_texture_shims_install(&h.memory));
    static const FnEntry probe[]={{PROBE_FN,guest_dispatch_probe}};
    FnModule modules[3]={{"probe",probe,1},d3dx_texture_shims_module(),d9_com_module()};
    assert(!n3ds_lift_bind_modules(&h.memory,modules,3));
    n3ds_lift_init_cpu(&h.cpu,0x10004000,0);

    /* 1. The IAT resolver returns host thunks in the dedicated window. */
    static const char *const names[]={
        "D3DXGetImageInfoFromFileInMemory","D3DXCreateTextureFromFileInMemory",
        "D3DXGetImageInfoFromFileA","D3DXCreateTextureFromFileExA",
        "D3DXCreateCubeTextureFromFileInMemory","D3DXCreateVolumeTextureFromFileInMemory"};
    for(unsigned i=0;i<sizeof names/sizeof names[0];++i){
        uint32_t fn=resolve(names[i]);
        assert(fn>=0xfd000000u&&fn<0xfe000000u);
    }
    /* A different d3dx export must not be captured by this resolver. */
    {PeImport other={.dll="d3dx9_42.dll",.name="D3DXMatrixMultiply"};assert(!win32_resolve(&other,NULL));}

    /* The linker path itself: a synthetic d3dx9_42 import table must bind the
     * IAT cell to the host thunk, not to the staged DLL's export address. */
    {
        const uint32_t imp=SCRATCH+0x1000u;
        guest_write32(&h.memory,imp+0x100,0x180);guest_write32(&h.memory,imp+0x104,0);
        guest_write32(&h.memory,imp+0x108,0);guest_write32(&h.memory,imp+0x10c,0x200);
        guest_write32(&h.memory,imp+0x110,0x2a0);
        guest_write32(&h.memory,imp+0x180,0x220);guest_write32(&h.memory,imp+0x184,0);
        guest_write(&h.memory,imp+0x200,"d3dx9_42.dll",13);
        guest_write32(&h.memory,imp+0x220,0);
        guest_write(&h.memory,imp+0x222,"D3DXGetImageInfoFromFileInMemory",33);
        guest_write32(&h.memory,imp+0x2a0,0x220);guest_write32(&h.memory,imp+0x2a4,0);
        GuestImage image={.base=imp,.size=0x1000,.import_rva=0x100,.import_size=40};
        size_t bound=0;
        assert(!pe_import_bind(&h.memory,&image,win32_resolve,NULL,&bound)&&bound==1);
        uint32_t iat=0;assert(!guest_read32(&h.memory,imp+0x2a0,&iat));
        assert(iat==resolve("D3DXGetImageInfoFromFileInMemory"));
        assert(iat>=0xfd000000u&&iat<0xfe000000u);
    }

    /* A real device, created through the COM slot exactly as the guest does. */
    uint32_t create9=d9_com_resolve(&(PeImport){.dll="d3d9.dll",.name="Direct3DCreate9"},NULL);
    assert(create9);
    uint32_t sdk=32;assert(!invoke(&h,create9,&sdk,1,&h.factory)&&h.factory);
    uint32_t pp[14]={400,240,D9_X8R8G8B8,1,0,0,1,0,1,0,0,0,0,1};
    assert(!guest_write(&h.memory,PP,pp,sizeof pp));
    uint32_t create[]={0,1,0,0x20,PP,OUT};
    assert(call(&h,h.factory,16,create,6)==HR_OK);
    assert(!guest_read32(&h.memory,OUT,&h.device)&&h.device);

    /* 2. 8x8 A8R8G8B8 with two mips: image info fields. */
    uint8_t ddsbuf[4096];
    size_t dds_a8=make_a8r8g8b8_dds(ddsbuf,8,8,2,0x20);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    const uint8_t *src=guest_ptr(&h.memory,DATA,dds_a8);assert(src);
    uint32_t a[]={DATA,(uint32_t)dds_a8,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",a,3)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+4)==8&&rd(&h,INFO+8)==1);
    assert(rd(&h,INFO+12)==2&&rd(&h,INFO+16)==FMT_A8R8G8B8);
    assert(rd(&h,INFO+20)==3&&rd(&h,INFO+24)==4); /* texture, D3DXIFF_DDS */
    assert(!*d3dx_texture_shims_last_error());

    /* 3. Create the texture and verify format/dimensions/levels via COM. */
    uint32_t ct[]={h.device,DATA,(uint32_t)dds_a8,OUT};
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",ct,4)==HR_OK);
    uint32_t tex=rd(&h,OUT);assert(tex);
    assert(call(&h,tex,13,NULL,0)==2);             /* GetLevelCount */
    uint32_t desc[]={0,OUT2};assert(call(&h,tex,17,desc,2)==HR_OK);
    assert(rd(&h,OUT2+0)==FMT_A8R8G8B8&&rd(&h,OUT2+12)==D9_POOL_MANAGED);
    assert(rd(&h,OUT2+24)==8&&rd(&h,OUT2+28)==8);

    /* LockRect at level 0 returns exactly the DDS level bytes. */
    uint32_t lock[]={0,OUT2,0,0};
    assert(call(&h,tex,19,lock,4)==HR_OK);
    uint32_t pitch=rd(&h,OUT2),ptr=rd(&h,OUT2+4);
    assert(pitch==32&&guest_ptr(&h.memory,ptr,256));
    assert(!memcmp(guest_ptr(&h.memory,ptr,256),src+128,256));
    /* Round-trip a guest write through UnlockRect/LockRect. */
    uint8_t *locked=guest_ptr(&h.memory,ptr,256);
    locked[0]^=0x5au;locked[255]^=0xa5u;
    uint32_t level=0;assert(call(&h,tex,20,&level,1)==HR_OK);
    assert(call(&h,tex,19,lock,4)==HR_OK);
    ptr=rd(&h,OUT2+4);locked=guest_ptr(&h.memory,ptr,256);
    assert(locked[0]==(uint8_t)(src[128]^0x5au)&&locked[255]==(uint8_t)(src[383]^0xa5u));
    assert(call(&h,tex,20,&level,1)==HR_OK);
    /* Level 1 is a 4x4 CPU mip; its pitch is 16 and the bytes match. */
    lock[0]=1;assert(call(&h,tex,19,lock,4)==HR_OK);
    assert(rd(&h,OUT2)==16&&guest_ptr(&h.memory,rd(&h,OUT2+4),64));
    assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),64),src+128+256,64));
    level=1;assert(call(&h,tex,20,&level,1)==HR_OK);
    assert(call(&h,tex,2,NULL,0)==0);

    /* 4. 16x16 DXT1: format and compressed LockRect layout. */
    dds_a8=make_dxt1_dds(ddsbuf,16,16,0x40);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    src=guest_ptr(&h.memory,DATA,dds_a8);assert(src);
    uint32_t ii[]={DATA,(uint32_t)dds_a8,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",ii,3)==HR_OK);
    assert(rd(&h,INFO+12)==1&&rd(&h,INFO+16)==FMT_DXT1);
    uint32_t ct2[]={h.device,DATA,(uint32_t)dds_a8,OUT};
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",ct2,4)==HR_OK);
    tex=rd(&h,OUT);assert(tex);
    uint32_t dl[]={0,OUT2,0,0};assert(call(&h,tex,19,dl,4)==HR_OK);
    assert(rd(&h,OUT2)==32&&guest_ptr(&h.memory,rd(&h,OUT2+4),128));
    assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),128),src+128,128));
    level=0;assert(call(&h,tex,20,&level,1)==HR_OK);
    assert(call(&h,tex,2,NULL,0)==0);

    /* 5. Non-DDS input is a named failure and leaves the info zeroed. */
    const uint8_t junk[64]="this is not a DDS file";
    assert(!guest_write(&h.memory,DATA,junk,sizeof junk));
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    uint32_t bad[]={DATA,(uint32_t)sizeof junk,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",bad,3)==HR_INVALIDDATA);
    assert(rd(&h,INFO)==0&&strstr(d3dx_texture_shims_last_error(),"not a DDS"));
    uint32_t badct[]={h.device,DATA,(uint32_t)sizeof junk,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",badct,4)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"not a DDS"));

    /* 6. Cube and volume DDS: image info works, texture creation is named
     *    unsupported with a cleared out pointer. */
    size_t cube_bytes=dds_header(ddsbuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0x200u,1);
    assert(!guest_write(&h.memory,DATA,ddsbuf,cube_bytes));
    uint32_t ci[]={DATA,(uint32_t)cube_bytes,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",ci,3)==HR_OK);
    assert(rd(&h,INFO+20)==5);
    uint32_t cct[]={h.device,DATA,(uint32_t)cube_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",cct,4)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"cube"));

    size_t vol_bytes=dds_header(ddsbuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0x200000u,4);
    assert(!guest_write(&h.memory,DATA,ddsbuf,vol_bytes));
    uint32_t vi[]={DATA,(uint32_t)vol_bytes,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",vi,3)==HR_OK);
    assert(rd(&h,INFO+8)==4&&rd(&h,INFO+20)==4);
    uint32_t vct[]={h.device,DATA,(uint32_t)vol_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",vct,4)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"volume"));

    /* 7. File-path and dedicated cube/volume exports: explicit E_NOTIMPL. */
    uint8_t path[16];memcpy(path,"nonexistent.dds",16);assert(!guest_write(&h.memory,DATA,path,16));
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    uint32_t gi[]={DATA,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_E_NOTIMPL);
    uint32_t exa[13]={DATA,0,0,0,0,FMT_DXT1,1,0,0,0,INFO,0,OUT};
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,13)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"ExA"));
    uint32_t cube[4]={h.device,DATA,(uint32_t)cube_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",cube,4)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0);
    uint32_t vol[4]={h.device,DATA,(uint32_t)vol_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",vol,4)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0);

    /* 8. Device validation: a non-device pointer must not create a texture. */
    dds_a8=make_dxt1_dds(ddsbuf,16,16,0x40);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    uint32_t wrong[]={0x10010000u,DATA,(uint32_t)dds_a8,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",wrong,4)==HR_INVALIDCALL);
    assert(rd(&h,OUT)==0);

    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    puts("D3DX9_42 DDS image-info + texture-create host shims: PASS");
    return 0;
}
