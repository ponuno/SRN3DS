/* Host regression for the plain D3DX9_42 texture imports.
 *
 * Proves the three things that used to be false when TESV's IAT pointed at the
 * staged d3dx9_42.dll body:
 *   1. imports resolve to host thunks, not guest DLL addresses;
 *   2. D3DXGetImageInfoFromFileInMemory parses a synthetic DDS from guest
 *      memory and fills all seven D3DXIMAGE_INFO fields;
 *   3. D3DXCreateTextureFromFileInMemory creates a real IDirect3DTexture9
 *      through the d3d9_com/d3d9_core path and LockRect shows the file pixels;
 *   4. D3DXGetImageInfoFromFileA and D3DXCreateTextureFromFileExA read a
 *      synthetic file from the win32 sandbox root, reject paths outside it,
 *      and reject the unsupported Ex resize/format parameters by name.
 * Cube creation is covered by tests/d3dx_cube_texture_smoke.c; volume creation
 * and non-DDS input must still fail with a named HRESULT, never S_OK.
 */
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "d3d9_bc.h"
#include "d3dx_texture_shims.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_UNLINK(p) _unlink(p)
#define TEST_RMDIR(p) _rmdir(p)
#define TEST_PID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_MKDIR(p) mkdir(p,0700)
#define TEST_UNLINK(p) unlink(p)
#define TEST_RMDIR(p) rmdir(p)
#define TEST_PID() getpid()
#endif

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
#define HR_NOTAVAILABLE 0x8876086au

#define FMT_A8R8G8B8 21u
#define FMT_R8G8B8 20u
#define FMT_A8B8G8R8 32u
#define FMT_DXT1 0x31545844u
#define FMT_ATI2 0x32495441u
#define FMT_DX10 0x30315844u

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
/* R8G8B8: 24bpp, DDS rows padded to 4 bytes, memory order B,G,R. */
static size_t make_rgb24_dds(uint8_t *b,unsigned w,unsigned h,unsigned mips,unsigned seed){
    size_t off=dds_header(b,w,h,mips,0x40u,0,24,0x00ff0000u,0x0000ff00u,
                          0x000000ffu,0,0,1);
    unsigned cw=w,ch=h;
    for(unsigned l=0;l<mips;++l){
        size_t pitch=d9_rgb24_pitch(cw);
        for(unsigned y=0;y<ch;++y)
            for(unsigned x=0;x<cw;++x){
                uint8_t *p=b+off+(size_t)y*pitch+(size_t)x*3u;
                p[0]=(uint8_t)(seed+x*3u+l);p[1]=(uint8_t)(seed+y*5u+l);
                p[2]=(uint8_t)(seed+x+y+l);
            }
        off+=pitch*ch;
        if(cw>1)cw>>=1;if(ch>1)ch>>=1;
    }
    return off;
}
/* A8B8G8R8: 32bpp, masks R=0xff/G=0xff00/B=0xff0000/A=0xff000000. */
static size_t make_abgr8_dds(uint8_t *b,unsigned w,unsigned h,unsigned mips,unsigned seed){
    size_t off=dds_header(b,w,h,mips,0x41u,0,32,0x000000ffu,0x0000ff00u,
                          0x00ff0000u,0xff000000u,0,1);
    unsigned cw=w,ch=h;
    for(unsigned l=0;l<mips;++l){
        size_t bytes=(size_t)cw*4u*ch;
        for(size_t k=0;k<bytes;++k)b[off+k]=(uint8_t)(seed+l*17u+k);
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
static size_t make_bc5_dds(uint8_t *b,unsigned w,unsigned h,unsigned seed){
    size_t off=dds_header(b,w,h,1,0x4u,FMT_ATI2,0,0,0,0,0,0,1);
    size_t bytes=(size_t)((w+3u)/4u)*((h+3u)/4u)*16u;
    for(size_t k=0;k<bytes;++k)b[off+k]=(uint8_t)(seed+k);
    return off+bytes;
}
/* Solid red DXT1, one 4x4 block per 8x8 quadrant.  The decoded A8R8G8B8
 * word is 0xFFFF0000 (memory bytes B=0,G=0,R=255,A=255). */
static size_t make_dxt1_red_dds(uint8_t *b,unsigned w,unsigned h){
    size_t off=dds_header(b,w,h,1,0x4u,FMT_DXT1,0,0,0,0,0,0,1);
    unsigned blocks=((w+3u)/4u)*((h+3u)/4u);
    for(unsigned i=0;i<blocks;++i){
        uint8_t *p=b+off+(size_t)i*8u;
        p[0]=0x00;p[1]=0xf8; /* c0 = 0xF800 red */
        p[2]=p[3]=0;         /* c1 = black */
        p[4]=p[5]=p[6]=p[7]=0; /* every index selects c0 */
    }
    return off+(size_t)blocks*8u;
}

static uint32_t resolve(const char *name){
    PeImport e={.dll="d3dx9_42.dll",.name=name};
    return win32_resolve(&e,NULL);
}
static uint32_t shim_call(Harness *h,const char *name,const uint32_t *args,unsigned n){
    uint32_t fn=resolve(name);assert(fn);
    uint32_t r=0;assert(!invoke(h,fn,args,n,&r));return r;
}

/* Synthetic files live outside the repository and are removed at the end. */
static char sandbox[512];
static uint8_t menu_dds[4096];
static size_t menu_dds_size;
static int sandbox_path(char *out,size_t cap,const char *name){
    return snprintf(out,cap,"%s/%s",sandbox,name)<(int)cap?0:-1;
}
static int sandbox_write(const char *name,const void *data,size_t bytes){
    char path[640];if(sandbox_path(path,sizeof path,name))return -1;
    FILE *f=fopen(path,"wb");if(!f)return -1;
    int ok=fwrite(data,1,bytes,f)==bytes;
    if(fclose(f))ok=0;return ok?0:-1;
}
static int sandbox_write_oversize(const char *name,const uint8_t *header,size_t header_bytes){
    char path[640];if(sandbox_path(path,sizeof path,name))return -1;
    FILE *f=fopen(path,"wb");if(!f)return -1;
    int ok=header_bytes&&fwrite(header,1,header_bytes,f)==header_bytes;
    if(ok)ok=fseek(f,(long)(64u<<20),SEEK_SET)==0;
    if(ok)ok=fputc(0,f)!=EOF;
    if(fclose(f))ok=0;return ok?0:-1;
}
static void sandbox_remove(void){
    static const char *const files[]={"menu.dds","dxt1.dds","bad.dds","cube.dds",
                                      "volume.dds","huge.dds",
                                      "Data/Textures/menu.dds"};
    for(unsigned i=0;i<sizeof files/sizeof files[0];++i){
        char path[640];if(!sandbox_path(path,sizeof path,files[i]))TEST_UNLINK(path);
    }
    char path[640];
    if(!sandbox_path(path,sizeof path,"Data/Textures"))TEST_RMDIR(path);
    if(!sandbox_path(path,sizeof path,"Data"))TEST_RMDIR(path);
    TEST_RMDIR(sandbox);
}
static void set_guest_path(Harness *h,const char *path){
    assert(strlen(path)<260);
    assert(!guest_write(&h->memory,DATA,path,strlen(path)+1));
}
/* Optional real-corpus check: only runs when the sample exists outside the
 * repository, so the committed test stays self-contained.  The DDS bytes are
 * loaded through the same D3DXCreateTextureFromFileInMemory export and the
 * resulting texture is locked to prove nonzero expanded pixels and size. */
static void real_sample(Harness *h,const char *path,unsigned want_w,unsigned want_h,
                        unsigned want_fmt){
    FILE *f=fopen(path,"rb");
    if(!f){fprintf(stderr,"[d3dx] real sample absent, skipped: %s\n",path);return;}
    if(fseek(f,0,SEEK_END)||ftell(f)<=0){fclose(f);return;}
    long n=ftell(f);if(fseek(f,0,SEEK_SET)){fclose(f);return;}
    uint8_t *data=malloc((size_t)n);assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    const uint32_t base=SCRATCH+0x10000u;
    uint32_t span=(uint32_t)(((size_t)n+0xfffu)&~0xfffu);
    assert(!guest_map(&h->memory,base,span));
    assert(!guest_write(&h->memory,base,data,(size_t)n));
    free(data);
    uint32_t args[]={base,(uint32_t)n,INFO};
    assert(shim_call(h,"D3DXGetImageInfoFromFileInMemory",args,3)==HR_OK);
    assert(rd(h,INFO+0)==want_w&&rd(h,INFO+4)==want_h&&rd(h,INFO+16)==want_fmt);
    uint32_t ct[]={h->device,base,(uint32_t)n,OUT};
    assert(shim_call(h,"D3DXCreateTextureFromFileInMemory",ct,4)==HR_OK);
    uint32_t tex=rd(h,OUT);assert(tex);
    uint32_t lock[]={0,OUT2,0,0};
    assert(call(h,tex,19,lock,4)==HR_OK);
    size_t expect_pitch=(size_t)want_w*4u;
    assert(rd(h,OUT2)==expect_pitch);
    const uint8_t *px=guest_ptr(&h->memory,rd(h,OUT2+4),expect_pitch*want_h);
    assert(px);
    size_t nonzero=0;for(size_t i=0;i<expect_pitch*want_h;++i)nonzero+=(px[i]!=0);
    assert(nonzero);
    uint32_t level=0;assert(call(h,tex,20,&level,1)==HR_OK);
    assert(call(h,tex,2,NULL,0)==0);
    fprintf(stderr,"[d3dx] real %ux%u fmt %u sample loaded: %lu/%lu nonzero bytes\n",
            want_w,want_h,want_fmt,(unsigned long)nonzero,
            (unsigned long)(expect_pitch*want_h));
    guest_unmap(&h->memory,base);
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,2u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x4000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());
    const char *tmp=getenv("TEMP");if(!tmp||!*tmp)tmp=getenv("TMP");assert(tmp&&*tmp);
    char host_check[640];
    /* No sandbox root yet: the path API must say exactly that, not "outside". */
    assert(win32_guest_path("menu.dds",host_check,sizeof host_check)==
           WIN32_GUEST_PATH_NO_ROOT);
    snprintf(sandbox,sizeof sandbox,"%s/n3ds_d3dx_file_%ld",tmp,(long)TEST_PID());
    (void)TEST_MKDIR(sandbox);
    {char d[640];assert(!sandbox_path(d,sizeof d,"Data"));(void)TEST_MKDIR(d);
     assert(!sandbox_path(d,sizeof d,"Data/Textures"));(void)TEST_MKDIR(d);}
    /* A trailing separator is the real TESV install root shape; win32_init()
     * normalises it and path_for_impl() also tolerates it defensively. */
    {char root_sep[520];snprintf(root_sep,sizeof root_sep,"%s/",sandbox);
     assert(!win32_init(&h.memory,0x10000,root_sep));}
    assert(win32_guest_path("menu.dds",host_check,sizeof host_check)==WIN32_GUEST_PATH_OK);
    {char expected[640];snprintf(expected,sizeof expected,"%s/menu.dds",sandbox);
     assert(!strcmp(host_check,expected));}
    assert(win32_guest_path("Data/Textures/menu.dds",host_check,sizeof host_check)==
           WIN32_GUEST_PATH_OK);
    assert(win32_guest_path("",host_check,sizeof host_check)==WIN32_GUEST_PATH_EMPTY);
    {char long_path[400];memset(long_path,'a',sizeof long_path-1);long_path[sizeof long_path-1]=0;
     assert(win32_guest_path(long_path,host_check,sizeof host_check)==
            WIN32_GUEST_PATH_TOO_LONG);}
    assert(win32_guest_path("..\\escape.dds",host_check,sizeof host_check)==
           WIN32_GUEST_PATH_OUTSIDE);
    assert(!d3dx_texture_shims_install(&h.memory));

    /* Synthetic sandbox inputs: valid 2-mip A8R8G8B8, known red DXT1, junk,
     * cube/volume headers, a subdirectory file and an oversized valid DDS. */
    menu_dds_size=make_a8r8g8b8_dds(menu_dds,8,8,2,0x20);
    assert(!sandbox_write("menu.dds",menu_dds,menu_dds_size));
    assert(!sandbox_write("Data/Textures/menu.dds",menu_dds,menu_dds_size));
    {uint8_t dxt[4096];size_t n=make_dxt1_red_dds(dxt,8,8);
     assert(!sandbox_write("dxt1.dds",dxt,n));}
    {const uint8_t junk[64]="this is not a DDS file";assert(!sandbox_write("bad.dds",junk,sizeof junk));}
    {uint8_t filebuf[256];size_t n=dds_header(filebuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0x200u,1);
     assert(!sandbox_write("cube.dds",filebuf,n));
     n=dds_header(filebuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0x200000u,4);
     assert(!sandbox_write("volume.dds",filebuf,n));
     /* The header is valid even though the payload exceeds the create limit. */
     n=dds_header(filebuf,8,8,1,0x41u,0,32,0x00ff0000u,0x0000ff00u,0x000000ffu,0xff000000u,0,1);
     assert(!sandbox_write_oversize("huge.dds",filebuf,n));}
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

    /* 4a. BC5/ATI2 DDS: recognised, created, 16-byte blocks preserved. */
    dds_a8=make_bc5_dds(ddsbuf,16,16,0x30);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    src=guest_ptr(&h.memory,DATA,dds_a8);assert(src);
    uint32_t bi[]={DATA,(uint32_t)dds_a8,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",bi,3)==HR_OK);
    assert(rd(&h,INFO+12)==1&&rd(&h,INFO+16)==FMT_ATI2);
    uint32_t bct[]={h.device,DATA,(uint32_t)dds_a8,OUT};
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",bct,4)==HR_OK);
    tex=rd(&h,OUT);assert(tex);
    uint32_t bdl[]={0,OUT2,0,0};assert(call(&h,tex,19,bdl,4)==HR_OK);
    assert(rd(&h,OUT2)==64&&guest_ptr(&h.memory,rd(&h,OUT2+4),256));
    assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),256),src+128,256));
    level=0;assert(call(&h,tex,20,&level,1)==HR_OK);
    assert(call(&h,tex,2,NULL,0)==0);

    /* 4a1. 16x16 R8G8B8 (24bpp, B,G,R rows padded to 4 bytes) with 2 mips:
     * the loader must expand to A8R8G8B8 with A=255 and keep each mip's own
     * pitch. */
    dds_a8=make_rgb24_dds(ddsbuf,16,16,2,0x11);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    src=guest_ptr(&h.memory,DATA,dds_a8);assert(src);
    {uint32_t ri[]={DATA,(uint32_t)dds_a8,INFO};
     assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",ri,3)==HR_OK);
     assert(rd(&h,INFO+12)==2&&rd(&h,INFO+16)==FMT_R8G8B8);}
    {uint32_t rct[]={h.device,DATA,(uint32_t)dds_a8,OUT};
     assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",rct,4)==HR_OK);}
    tex=rd(&h,OUT);assert(tex);
    {uint32_t rl[]={0,OUT2,0,0};assert(call(&h,tex,19,rl,4)==HR_OK);
     assert(rd(&h,OUT2)==64);
     uint8_t expected[16*16*4];
     assert(!d9_rgb24_to_bgra8(expected,sizeof expected,src+128,
                               d9_rgb24_pitch(16),d9_rgb24_pitch(16)*16,16,16));
     assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),sizeof expected),expected,
                    sizeof expected));
     level=0;assert(call(&h,tex,20,&level,1)==HR_OK);}
    {uint32_t rl[]={1,OUT2,0,0};assert(call(&h,tex,19,rl,4)==HR_OK);
     assert(rd(&h,OUT2)==32);
     size_t src_bytes=d9_rgb24_pitch(16)*16;
     uint8_t expected[8*8*4];
     assert(!d9_rgb24_to_bgra8(expected,sizeof expected,src+128+src_bytes,
                               d9_rgb24_pitch(8),d9_rgb24_pitch(8)*8,8,8));
     assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),sizeof expected),expected,
                    sizeof expected));
     level=1;assert(call(&h,tex,20,&level,1)==HR_OK);}
    assert(call(&h,tex,2,NULL,0)==0);

    /* 4a2. A8B8G8R8 (memory R,G,B,A) must reach storage as B,G,R,A. */
    dds_a8=make_abgr8_dds(ddsbuf,8,8,2,0x70);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    src=guest_ptr(&h.memory,DATA,dds_a8);assert(src);
    {uint32_t ai[]={DATA,(uint32_t)dds_a8,INFO};
     assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",ai,3)==HR_OK);
     assert(rd(&h,INFO+12)==2&&rd(&h,INFO+16)==FMT_A8B8G8R8);}
    {uint32_t act[]={h.device,DATA,(uint32_t)dds_a8,OUT};
     assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",act,4)==HR_OK);}
    tex=rd(&h,OUT);assert(tex);
    {uint32_t al[]={0,OUT2,0,0};assert(call(&h,tex,19,al,4)==HR_OK);
     assert(rd(&h,OUT2)==32);
     uint8_t expected[8*8*4];
     assert(!d9_abgr8_to_bgra8(expected,sizeof expected,src+128,32,32*8,8,8));
     assert(!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),sizeof expected),expected,
                    sizeof expected));
     level=0;assert(call(&h,tex,20,&level,1)==HR_OK);}
    assert(call(&h,tex,2,NULL,0)==0);

    /* 4a3. Cube and volume: the same expansion covers six faces / slices.
     * Creation succeeds; pixel packing is the shared 2D helper above. */
    {size_t n=dds_header(ddsbuf,8,8,1,0x40u,0,24,0x00ff0000u,0x0000ff00u,
                         0x000000ffu,0,0xfe00u,1);
     size_t face=d9_rgb24_pitch(8)*8u;
     for(size_t k=0;k<6u*face;++k)ddsbuf[n+k]=(uint8_t)(0x40+k);n+=6u*face;
     assert(!guest_write(&h.memory,DATA,ddsbuf,n));
     uint32_t cc[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",cc,4)==HR_OK);
     assert(rd(&h,OUT));assert(call(&h,rd(&h,OUT),2,NULL,0)==0);}
    {size_t n=dds_header(ddsbuf,8,8,1,0x41u,0,32,0x000000ffu,0x0000ff00u,
                         0x00ff0000u,0xff000000u,0xfe00u,1);
     for(size_t k=0;k<6u*8u*8u*4u;++k)ddsbuf[n+k]=(uint8_t)(0x80+k);n+=6u*8u*8u*4u;
     assert(!guest_write(&h.memory,DATA,ddsbuf,n));
     uint32_t cc[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",cc,4)==HR_OK);
     assert(rd(&h,OUT));assert(call(&h,rd(&h,OUT),2,NULL,0)==0);}
    /* The real UBE eye cube is 1x1 A8B8G8R8; the backend floor is 8x8, so this
     * is a named refusal rather than a generic backend failure. */
    {size_t n=dds_header(ddsbuf,1,1,1,0x41u,0,32,0x000000ffu,0x0000ff00u,
                         0x00ff0000u,0xff000000u,0xfe00u,1);
     for(size_t k=0;k<6u*4u;++k)ddsbuf[n+k]=0;n+=6u*4u;
     assert(!guest_write(&h.memory,DATA,ddsbuf,n));
     uint32_t cc[]={h.device,DATA,(uint32_t)n,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",cc,4)==HR_NOTAVAILABLE);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"8x8"));}
    {size_t n=dds_header(ddsbuf,8,8,1,0x40u,0,24,0x00ff0000u,0x0000ff00u,
                         0x000000ffu,0,0x00200000u,2);
     size_t plane=d9_rgb24_pitch(8)*8u;
     for(size_t k=0;k<2u*plane;++k)ddsbuf[n+k]=(uint8_t)(0x20+k);n+=2u*plane;
     assert(!guest_write(&h.memory,DATA,ddsbuf,n));
     uint32_t vc[]={h.device,DATA,(uint32_t)n,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",vc,4)==HR_OK);
     assert(rd(&h,OUT));assert(call(&h,rd(&h,OUT),2,NULL,0)==0);}

    /* 4a2. DX10 BC7 is refused by name, not guessed. */
    {size_t n=dds_header(ddsbuf,8,8,1,0x4u,FMT_DX10,0,0,0,0,0,0,1);
     wr32(ddsbuf+128,98);wr32(ddsbuf+132,3);wr32(ddsbuf+136,0);
     wr32(ddsbuf+140,1);wr32(ddsbuf+144,0);n+=20;
     assert(!guest_write(&h.memory,DATA,ddsbuf,n));
     uint32_t b7[]={DATA,(uint32_t)n,INFO};
     assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
     assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",b7,3)==HR_INVALIDDATA);
     assert(rd(&h,INFO)==0&&strstr(d3dx_texture_shims_last_error(),"BC7"));
     assert(strstr(d3dx_texture_shims_last_error(),"no decoder"));}
    /* 4b. A 4x4 DXT1 is valid metadata but below the bridge's 8x8 compressed
     * floor: creation is a named NOTAVAILABLE before any resource exists. */
    dds_a8=make_dxt1_dds(ddsbuf,4,4,0x50);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    uint32_t small_info[]={DATA,(uint32_t)dds_a8,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",small_info,3)==HR_OK);
    assert(rd(&h,INFO+12)==1&&rd(&h,INFO+16)==FMT_DXT1);
    uint32_t small_ct[]={h.device,DATA,(uint32_t)dds_a8,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",small_ct,4)==HR_NOTAVAILABLE);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"8x8"));

    /* 4c. Non-power-of-two is unsupported, not invalid: named NOTAVAILABLE. */
    dds_a8=make_a8r8g8b8_dds(ddsbuf,12,12,1,0x60);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    uint32_t np2_ct[]={h.device,DATA,(uint32_t)dds_a8,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",np2_ct,4)==HR_NOTAVAILABLE);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"power-of-two"));

    /* 4d. A 4-4-4 header without alpha must not be reported as X1R5G5B5 (whose
     * masks contradict the header).  The enum has no X4R4G4B4 here, so the
     * shim refuses to guess and returns INVALIDDATA. */
    size_t d44=dds_header(ddsbuf,8,8,1,0x41u,0,16,0x0f00u,0x00f0u,0x000fu,0,0,1);
    assert(!guest_write(&h.memory,DATA,ddsbuf,d44));
    uint32_t i44[]={DATA,(uint32_t)d44,INFO};
    assert(shim_call(&h,"D3DXGetImageInfoFromFileInMemory",i44,3)==HR_INVALIDDATA);

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
    size_t cube_bytes=dds_header(ddsbuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0xfe00u,1);
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

    /* 7. File-path exports: guest path -> sandbox root -> host buffer -> the
     *    same legacy-DDS parser.  Every call goes through the stdcall probe,
     *    so the 2-slot and 14-slot epilogues are checked by the harness. */
    uint32_t gi[2]={DATA,INFO};

    set_guest_path(&h,"menu.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+4)==8&&rd(&h,INFO+8)==1);
    assert(rd(&h,INFO+12)==2&&rd(&h,INFO+16)==FMT_A8R8G8B8);
    assert(rd(&h,INFO+20)==3&&rd(&h,INFO+24)==4);
    assert(!*d3dx_texture_shims_last_error());

    /* The recognised C:\Skyrim\ virtual root aliases the same sandbox. */
    set_guest_path(&h,"C:\\Skyrim\\menu.dds");
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+16)==FMT_A8R8G8B8);

    /* Subdirectories are ordinary relative paths below the root. */
    set_guest_path(&h,"Data\\Textures\\menu.dds");
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+12)==2);
    set_guest_path(&h,"C:\\Skyrim\\Data\\Textures\\menu.dds");
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+16)==FMT_A8R8G8B8);

    set_guest_path(&h,"dxt1.dds");
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+12)==1&&rd(&h,INFO+16)==FMT_DXT1);

    set_guest_path(&h,"missing.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDDATA);
    assert(rd(&h,INFO)==0&&strstr(d3dx_texture_shims_last_error(),"cannot open"));

    set_guest_path(&h,"..\\escape.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDCALL);
    assert(rd(&h,INFO)==0&&strstr(d3dx_texture_shims_last_error(),"outside the sandbox"));

    set_guest_path(&h,"C:\\Windows\\menu.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDCALL);
    assert(rd(&h,INFO)==0);

    set_guest_path(&h,"bad.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDDATA);
    assert(rd(&h,INFO)==0);
    assert(strstr(d3dx_texture_shims_last_error(),"not a DDS")||
           strstr(d3dx_texture_shims_last_error(),"shorter than the 128-byte"));

    /* Image info reads only the 128-byte header: a 64 MiB+1 file with a valid
     * header must still report metadata. */
    set_guest_path(&h,"huge.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_OK);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+4)==8&&rd(&h,INFO+8)==1);
    assert(rd(&h,INFO+12)==1&&rd(&h,INFO+16)==FMT_A8R8G8B8);
    assert(rd(&h,INFO+24)==4&&!*d3dx_texture_shims_last_error());

    /* Path reasons are distinct. */
    set_guest_path(&h,"");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDCALL);
    assert(rd(&h,INFO)==0&&strstr(d3dx_texture_shims_last_error(),"empty"));
    {char long_path[300];memset(long_path,'a',sizeof long_path-1);long_path[sizeof long_path-1]=0;
     assert(strlen(long_path)==299);
     assert(!guest_write(&h.memory,DATA,long_path,sizeof long_path));
     assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDCALL);
     assert(strstr(d3dx_texture_shims_last_error(),"longer than 259"));}
    {char no_nul[260];memset(no_nul,'b',sizeof no_nul);
     assert(!guest_write(&h.memory,DATA,no_nul,sizeof no_nul));
     assert(shim_call(&h,"D3DXGetImageInfoFromFileA",gi,2)==HR_INVALIDCALL);
     assert(strstr(d3dx_texture_shims_last_error(),"longer than 259"));}
    {uint32_t bad_ptr[2]={0x70000000u,INFO};
     assert(shim_call(&h,"D3DXGetImageInfoFromFileA",bad_ptr,2)==HR_INVALIDCALL);
     assert(strstr(d3dx_texture_shims_last_error(),"outside guest memory"));}
    {uint32_t null_path[2]={0,INFO};
     assert(shim_call(&h,"D3DXGetImageInfoFromFileA",null_path,2)==HR_INVALIDCALL);
     assert(strstr(d3dx_texture_shims_last_error(),"NULL"));}

    {uint32_t np[2]={0,INFO};assert(shim_call(&h,"D3DXGetImageInfoFromFileA",np,2)==HR_INVALIDCALL);}

    /* 7b. D3DXCreateTextureFromFileExA: 14 real arguments (pPalette at A(12),
     *     ppTexture at A(13)); ret 0x38 verified from D3DX9_42.dll. */
    uint32_t exa_default[14]={h.device,DATA,0,0,0,0,0,1,
                              0xffffffffu,0xffffffffu,0,INFO,0,OUT};
    set_guest_path(&h,"menu.dds");
    assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_OK);
    uint32_t file_tex=rd(&h,OUT);assert(file_tex);
    assert(rd(&h,INFO+0)==8&&rd(&h,INFO+4)==8&&rd(&h,INFO+12)==2);
    assert(rd(&h,INFO+16)==FMT_A8R8G8B8&&rd(&h,INFO+24)==4);
    assert(call(&h,file_tex,13,NULL,0)==2);
    {uint32_t desc[]={0,OUT2};assert(call(&h,file_tex,17,desc,2)==HR_OK);
     assert(rd(&h,OUT2+0)==FMT_A8R8G8B8&&rd(&h,OUT2+8)==0&&rd(&h,OUT2+12)==D9_POOL_MANAGED);}
    {uint32_t fl[]={0,OUT2,0,0};assert(call(&h,file_tex,19,fl,4)==HR_OK);
     assert(rd(&h,OUT2)==32&&!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),256),menu_dds+128,256));
     uint32_t lvl=0;assert(call(&h,file_tex,20,&lvl,1)==HR_OK);}
    assert(call(&h,file_tex,2,NULL,0)==0);

    /* The exact TESV loose-texture tuple: dimensions from image info,
     * MipLevels=D3DX_FROM_FILE, D3DUSAGE_DYNAMIC, A8R8G8B8, SYSTEMMEM,
     * pSrcInfo/pPalette = NULL. */
    uint32_t tesv[14]={h.device,DATA,8,8,0xfffffffd,0x200,FMT_A8R8G8B8,2,
                       0xffffffffu,0xffffffffu,0,0,0,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",tesv,14)==HR_OK);
    uint32_t tesv_tex=rd(&h,OUT);assert(tesv_tex);
    {uint32_t desc[]={0,OUT2};assert(call(&h,tesv_tex,17,desc,2)==HR_OK);
     assert(rd(&h,OUT2+0)==FMT_A8R8G8B8&&rd(&h,OUT2+8)==0x200);
     assert(rd(&h,OUT2+12)==D9_POOL_SYSTEMMEM&&rd(&h,OUT2+24)==8&&rd(&h,OUT2+28)==8);}
    assert(call(&h,tesv_tex,13,NULL,0)==2);
    {uint32_t fl[]={0,OUT2,0,0};assert(call(&h,tesv_tex,19,fl,4)==HR_OK);
     assert(rd(&h,OUT2)==32&&!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),256),menu_dds+128,256));
     uint32_t lvl=0;assert(call(&h,tesv_tex,20,&lvl,1)==HR_OK);
     fl[0]=1;assert(call(&h,tesv_tex,19,fl,4)==HR_OK);
     assert(rd(&h,OUT2)==16&&!memcmp(guest_ptr(&h.memory,rd(&h,OUT2+4),64),menu_dds+128+256,64));
     lvl=1;assert(call(&h,tesv_tex,20,&lvl,1)==HR_OK);}
    assert(call(&h,tesv_tex,2,NULL,0)==0);

    /* DXT1 file requested as A8R8G8B8 must be decoded, not faked. */
    set_guest_path(&h,"dxt1.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",tesv,14)==HR_OK);
    uint32_t dxt_tex=rd(&h,OUT);assert(dxt_tex);
    assert(call(&h,dxt_tex,13,NULL,0)==1);
    {uint32_t desc[]={0,OUT2};assert(call(&h,dxt_tex,17,desc,2)==HR_OK);
     assert(rd(&h,OUT2+0)==FMT_A8R8G8B8&&rd(&h,OUT2+12)==D9_POOL_SYSTEMMEM);}
    {uint32_t fl[]={0,OUT2,0,0};assert(call(&h,dxt_tex,19,fl,4)==HR_OK);
     assert(rd(&h,OUT2)==32);
     const uint32_t *px=guest_ptr(&h.memory,rd(&h,OUT2+4),256);assert(px);
     for(unsigned i=0;i<64;++i)assert(px[i]==0xffff0000u);
     uint32_t lvl=0;assert(call(&h,dxt_tex,20,&lvl,1)==HR_OK);}
    assert(call(&h,dxt_tex,2,NULL,0)==0);

    /* MipLevels=0 uses the file chain in this bridge (no mip generator). */
    set_guest_path(&h,"menu.dds");
    {uint32_t exa[14];memcpy(exa,tesv,sizeof exa);exa[4]=0;
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_OK);
     uint32_t t=rd(&h,OUT);assert(t&&call(&h,t,13,NULL,0)==2);
     assert(call(&h,t,2,NULL,0)==0);}

    /* Real D3DX filter values 1..4 are valid no-ops without resize. */
    static const uint32_t good_filters[]={1u,2u,3u,4u};
    for(unsigned i=0;i<sizeof good_filters/sizeof good_filters[0];++i){
        uint32_t exa[14];memcpy(exa,tesv,sizeof exa);
        exa[8]=exa[9]=good_filters[i];
        assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
        assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_OK);
        uint32_t t=rd(&h,OUT);assert(t);assert(call(&h,t,2,NULL,0)==0);
    }

    /* Resize/format/flag combinations that cannot be honored fail by name,
     * before a resource exists and without touching pSrcInfo. */
    static const struct {unsigned index;uint32_t value;const char *needle;} bad_params[]={
        {2,16,"does not match"},{3,16,"does not match"},
        {6,22,"cannot be produced"}, /* X8R8G8B8 from an A8 file */
        {4,2,"MipLevels"},{5,1,"Usage"},{7,0,"Pool"},
        {8,0,"Filter/MipFilter"},{9,0,"Filter/MipFilter"},
        {8,6,"Filter/MipFilter"},{9,6,"Filter/MipFilter"},
        {10,0x00ff00ffu,"ColorKey"},
    };
    for(unsigned i=0;i<sizeof bad_params/sizeof bad_params[0];++i){
        uint32_t exa[14];memcpy(exa,exa_default,sizeof exa);
        exa[bad_params[i].index]=bad_params[i].value;
        assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
        assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
        assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_NOTAVAILABLE);
        assert(rd(&h,OUT)==0);
        /* Width/Height/Format are checked after parsing but still before the
         * info output is written. */
        assert(rd(&h,INFO)==0xdeadbeefu);
        assert(strstr(d3dx_texture_shims_last_error(),bad_params[i].needle));
    }
    {uint32_t exa[14];memcpy(exa,exa_default,sizeof exa);exa[11]=0x70000000u;
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_INVALIDCALL);
     assert(rd(&h,OUT)==0);}
    {uint32_t exa[14];memcpy(exa,exa_default,sizeof exa);exa[13]=0x70000000u;
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_INVALIDCALL);}
    {uint32_t exa[14];memcpy(exa,exa_default,sizeof exa);exa[0]=SCRATCH;
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa,14)==HR_INVALIDCALL);
     assert(rd(&h,OUT)==0);}

    set_guest_path(&h,"bad.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"not a DDS"));

    set_guest_path(&h,"cube.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"cube"));

    set_guest_path(&h,"volume.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_E_NOTIMPL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"volume"));

    set_guest_path(&h,"missing.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"cannot open"));

    set_guest_path(&h,"..\\escape.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_INVALIDCALL);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"outside the sandbox"));

    set_guest_path(&h,"huge.dds");
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileExA",exa_default,14)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"64 MiB"));

    /* 7c. Dedicated cube/volume exports: the cube header here has no face
     *     payload, so the new cube loader must reject it as truncated; volume
     *     remains named unsupported. */
    cube_bytes=dds_header(ddsbuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0xfe00u,1);
    assert(!guest_write(&h.memory,DATA,ddsbuf,cube_bytes));
    uint32_t cube[4]={h.device,DATA,(uint32_t)cube_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateCubeTextureFromFileInMemory",cube,4)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"truncated"));
    vol_bytes=dds_header(ddsbuf,8,8,1,0x4u,FMT_DXT1,0,0,0,0,0,0x200000u,4);
    assert(!guest_write(&h.memory,DATA,ddsbuf,vol_bytes));
    uint32_t vol[4]={h.device,DATA,(uint32_t)vol_bytes,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileInMemory",vol,4)==HR_INVALIDDATA);
    assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"truncated"));

    /* 8. Device validation: a non-device pointer must not create a texture. */
    dds_a8=make_dxt1_dds(ddsbuf,16,16,0x40);
    assert(!guest_write(&h.memory,DATA,ddsbuf,dds_a8));
    uint32_t wrong[]={0x10010000u,DATA,(uint32_t)dds_a8,OUT};
    assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
    assert(shim_call(&h,"D3DXCreateTextureFromFileInMemory",wrong,4)==HR_INVALIDCALL);
    assert(rd(&h,OUT)==0);

    /* 9. Real corpus samples, when present on this machine.  One retail
     *    R8G8B8 256x256 tint mask (BSA extraction path is external) and one
     *    16x16 UBE A8B8G8R8 eye map.  Both must lock to w*4 pitch with
     *    nonzero decoded bytes. */
    {const char *r8=getenv("SKYRIM_R8G8B8_SAMPLE");
     if(!r8||!*r8)
         r8="E:\\Skyrim_N3DS_build\\assets\\textures\\format_samples\\khajiitnose01.dds";
     real_sample(&h,r8,256,256,FMT_R8G8B8);}
    real_sample(&h,"E:\\SkyrimMods\\mods\\UBE 2.0\\textures\\!COR\\Eyes_ILV\\eye01_outer_m.dds",
                16,16,FMT_A8B8G8R8);

    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    win32_shutdown();
    n3ds_lift_release();
    guest_destroy(&h.memory);
    sandbox_remove();
    puts("D3DX9_42 DDS image-info + texture-create host shims: PASS");
    return 0;
}
