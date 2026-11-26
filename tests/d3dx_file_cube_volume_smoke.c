/* D3DX9_42 file-path cube/volume loaders.
 *
 * Real files are written below a temporary win32 sandbox root and loaded
 * through D3DXCreateCubeTextureFromFileA/ExA and
 * D3DXCreateVolumeTextureFromFileA/ExA.  The four exports share the existing
 * InMemory parser and creator; this test covers every face/slice, the strict
 * ExA tuple, path/IO/size/format refusals, cross-loader rejection, and the
 * real stdcall epilogues (3/13/3/15 arguments) through the probe harness. */
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "d3dx_texture_shims.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"

#include <assert.h>
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
#define FMT_DXT1 0x31545844u
#define D3DX_FROM_FILE 0xfffffffdu
#define CUBE_CAPS 0x0000fe00u
#define VOLUME_CAPS 0x00200000u
#define DDSD_DEPTH 0x00800000u

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
    if(c->r[4]!=sp)rt_unimpl(c,target,"D3DX file cube/volume stdcall stack mismatch");
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

static char sandbox[512];
static char path_buf[640];
static int sandbox_path(const char *name){
    int n=snprintf(path_buf,sizeof path_buf,"%s/%s",sandbox,name);
    return n>0&&(size_t)n<sizeof path_buf?0:-1;
}
static int sandbox_write(const char *name,const void *data,size_t bytes){
    if(sandbox_path(name))return -1;
    FILE *f=fopen(path_buf,"wb");if(!f)return -1;
    int ok=fwrite(data,1,bytes,f)==bytes;
    if(fclose(f))ok=0;return ok?0:-1;
}
static int sandbox_write_oversize(const char *name,const void *header,size_t bytes){
    if(sandbox_path(name))return -1;
    FILE *f=fopen(path_buf,"wb");if(!f)return -1;
    int ok=bytes&&fwrite(header,1,bytes,f)==bytes;
    if(ok)ok=fseek(f,(long)(64u<<20),SEEK_SET)==0;
    if(ok)ok=fputc(0,f)!=EOF;
    if(fclose(f))ok=0;return ok?0:-1;
}
static void sandbox_remove(void){
    static const char *const files[]={"cube.dds","volume.dds","bad.dds","huge.dds"};
    for(unsigned i=0;i<sizeof files/sizeof files[0];++i)
        if(!sandbox_path(files[i]))TEST_UNLINK(path_buf);
    TEST_RMDIR(sandbox);
}
static void set_guest_path(Harness *h,const char *path){
    assert(strlen(path)<260);
    assert(!guest_write(&h->memory,DATA,path,strlen(path)+1));
}

static size_t dds_header(uint8_t *b,unsigned w,unsigned h,unsigned depth,
                         unsigned mips,unsigned format,uint32_t caps2){
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
static size_t a8_cube_file(uint8_t *b,unsigned edge,unsigned mips,unsigned seed){
    size_t off=dds_header(b,edge,edge,1,mips,FMT_A8R8G8B8,CUBE_CAPS);
    unsigned dim=edge;
    for(unsigned face=0;face<D9_CUBE_FACES;++face){
        dim=edge;
        for(unsigned level=0;level<mips;++level){
            size_t words=(size_t)dim*dim;
            for(size_t i=0;i<words;++i)
                wr32(b+off+i*4,0xff000000u|((face+1u)<<16)|(level<<8)|((seed+i)&0xffu));
            off+=words*4;if(dim>1)dim>>=1;
        }
    }
    return off;
}
static size_t a8_volume_file(uint8_t *b,unsigned w,unsigned h,unsigned depth,
                             unsigned mips,unsigned seed){
    size_t off=dds_header(b,w,h,depth,mips,FMT_A8R8G8B8,VOLUME_CAPS);
    unsigned vw=w,vh=h,vd=depth;
    for(unsigned level=0;level<mips;++level){
        for(unsigned z=0;z<vd;++z)
            for(unsigned y=0;y<vh;++y)
                for(unsigned x=0;x<vw;++x)
                    wr32(b+off+((size_t)z*vh*vw+(size_t)y*vw+x)*4,
                         0xff000000u|(level<<16)|(z<<8)|((seed+y*vw+x)&0xffu));
        off+=(size_t)vw*vh*vd*4;
        if(vw>1)vw>>=1;if(vh>1)vh>>=1;if(vd>1)vd>>=1;
    }
    return off;
}
static uint32_t cube_surface(Harness *h,uint32_t cube,unsigned face,unsigned level){
    uint32_t args[]={face,level,OUT2};
    assert(!guest_write32(&h->memory,OUT2,0));
    assert(call(h,cube,18,args,3)==HR_OK);
    uint32_t surface=rd(h,OUT2);assert(surface);return surface;
}
static void check_cube(Harness *h,uint32_t cube,unsigned edge,unsigned mips,
                       unsigned pool,unsigned seed){
    assert(call(h,cube,13,NULL,0)==mips);
    for(unsigned face=0;face<D9_CUBE_FACES;++face){
        for(unsigned level=0;level<mips;++level){
            unsigned dim=edge>>level;if(!dim)dim=1;
            uint32_t surface=cube_surface(h,cube,face,level);
            uint32_t desc[]={level,OUT};
            assert(call(h,cube,17,desc,2)==HR_OK);
            assert(rd(h,OUT+0)==FMT_A8R8G8B8&&rd(h,OUT+4)==1/*SURFACE*/);
            assert(rd(h,OUT+12)==pool&&rd(h,OUT+24)==dim&&rd(h,OUT+28)==dim);
            uint32_t lock[]={OUT,0,0};
            assert(call(h,surface,13,lock,3)==HR_OK);
            uint32_t pitch=rd(h,OUT),ptr=rd(h,OUT+4);
            assert(pitch==dim*4);
            const uint32_t *px=guest_ptr(&h->memory,ptr,(size_t)pitch*dim);assert(px);
            assert(px[0]==(0xff000000u|((face+1u)<<16)|(level<<8)|(seed&0xffu)));
            assert(px[dim*dim-1]==(0xff000000u|((face+1u)<<16)|(level<<8)|
                                    ((seed+dim*dim-1)&0xffu)));
            assert(call(h,surface,14,NULL,0)==HR_OK);
            assert(call(h,surface,2,NULL,0)==0);
        }
    }
}
static void check_volume(Harness *h,uint32_t volume,unsigned w,unsigned hgt,
                         unsigned depth,unsigned mips,unsigned pool,unsigned seed){
    assert(call(h,volume,13,NULL,0)==mips);
    for(unsigned level=0;level<mips;++level){
        unsigned lw=w>>level;if(!lw)lw=1;
        unsigned lh=hgt>>level;if(!lh)lh=1;
        unsigned ld=depth>>level;if(!ld)ld=1;
        uint32_t desc[]={level,OUT};
        assert(call(h,volume,17,desc,2)==HR_OK);
        assert(rd(h,OUT+0)==FMT_A8R8G8B8&&rd(h,OUT+4)==4/*VOLUMETEXTURE*/);
        assert(rd(h,OUT+12)==pool&&rd(h,OUT+16)==lw&&rd(h,OUT+20)==lh&&
               rd(h,OUT+24)==ld);
        uint32_t lock[]={level,OUT,0,0};
        assert(call(h,volume,19,lock,4)==HR_OK);
        uint32_t rowpitch=rd(h,OUT),slicepitch=rd(h,OUT+4),bits=rd(h,OUT+8);
        assert(rowpitch==lw*4&&slicepitch==rowpitch*lh);
        const uint32_t *px=guest_ptr(&h->memory,bits,(size_t)slicepitch*ld);assert(px);
        assert(px[0]==(0xff000000u|(level<<16)|(seed&0xffu)));
        if(ld>1)assert(px[slicepitch/4]==(0xff000000u|(level<<16)|(1u<<8)|(seed&0xffu)));
        uint32_t un[]={level};
        assert(call(h,volume,20,un,1)==HR_OK);
    }
}

int main(void){
    Harness h;memset(&h,0,sizeof h);
    guest_init(&h.memory,4u<<20);
    assert(!guest_map(&h.memory,0x10000000,0x4000)&&!guest_map(&h.memory,SCRATCH,0x6000));
    assert(!d9_com_init(&h.memory,NULL));
    assert(!d9_null_register());

    const char *tmp=getenv("TEMP");if(!tmp||!*tmp)tmp=getenv("TMP");assert(tmp&&*tmp);
    snprintf(sandbox,sizeof sandbox,"%s/n3ds_d3dx_file_cv_%ld",tmp,(long)TEST_PID());
    (void)TEST_MKDIR(sandbox);
    {char root[520];snprintf(root,sizeof root,"%s/",sandbox);
     assert(!win32_init(&h.memory,0x10000,root));}

    uint8_t dds[16384];
    size_t cube_bytes=a8_cube_file(dds,8,2,0x10);
    assert(!sandbox_write("cube.dds",dds,cube_bytes));
    size_t volume_bytes=a8_volume_file(dds,4,4,2,2,0x20);
    assert(!sandbox_write("volume.dds",dds,volume_bytes));
    {const uint8_t junk[64]="this is not a DDS file";assert(!sandbox_write("bad.dds",junk,sizeof junk));}
    {uint8_t header[128];size_t n=dds_header(header,8,8,1,1,FMT_A8R8G8B8,CUBE_CAPS);
     assert(!sandbox_write_oversize("huge.dds",header,n));}

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

    static const char *const names[]={
        "D3DXCreateCubeTextureFromFileA","D3DXCreateCubeTextureFromFileExA",
        "D3DXCreateVolumeTextureFromFileA","D3DXCreateVolumeTextureFromFileExA"};
    for(unsigned i=0;i<sizeof names/sizeof names[0];++i){
        uint32_t fn=resolve(names[i]);
        assert(fn>=0xfd000000u&&fn<0xfd010000u);
    }

    /* FromFileA: six faces / all depth slices, MANAGED pool. */
    set_guest_path(&h,"cube.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileA",args,3)==HR_OK);
     uint32_t cube=rd(&h,OUT);assert(cube);
     check_cube(&h,cube,8,2,1,0x10);
     assert(call(&h,cube,2,NULL,0)==0);}

    set_guest_path(&h,"volume.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileA",args,3)==HR_OK);
     uint32_t volume=rd(&h,OUT);assert(volume);
     check_volume(&h,volume,4,4,2,2,1,0x20);
     assert(call(&h,volume,2,NULL,0)==0);}

    /* Cube ExA: file dimensions, D3DX_FROM_FILE, DEFAULT pool, pSrcInfo. */
    set_guest_path(&h,"cube.dds");
    {uint32_t args[]={h.device,DATA,0,D3DX_FROM_FILE,0,0,D9_POOL_DEFAULT,
                      0xffffffffu,0xffffffffu,0,INFO,0,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(!guest_write32(&h.memory,INFO,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_OK);
     uint32_t cube=rd(&h,OUT);assert(cube);
     assert(rd(&h,INFO+0)==8&&rd(&h,INFO+4)==8&&rd(&h,INFO+12)==2);
     assert(rd(&h,INFO+16)==FMT_A8R8G8B8&&rd(&h,INFO+20)==5);
     check_cube(&h,cube,8,2,0,0x10);
     assert(call(&h,cube,2,NULL,0)==0);}
    /* Exact Size/format/MANAGED tuple is also accepted. */
    {uint32_t args[]={h.device,DATA,8,0,0,FMT_A8R8G8B8,D9_POOL_MANAGED,
                      1,1,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_OK);
     uint32_t cube=rd(&h,OUT);assert(cube);
     check_cube(&h,cube,8,2,1,0x10);
     assert(call(&h,cube,2,NULL,0)==0);}

    /* Volume ExA: file width/height/depth, MANAGED, exact format. */
    set_guest_path(&h,"volume.dds");
    {uint32_t args[]={h.device,DATA,0,0,0,D3DX_FROM_FILE,0,FMT_A8R8G8B8,
                      D9_POOL_MANAGED,0xffffffffu,0xffffffffu,0,INFO,0,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileExA",args,15)==HR_OK);
     uint32_t volume=rd(&h,OUT);assert(volume);
     assert(rd(&h,INFO+0)==4&&rd(&h,INFO+4)==4&&rd(&h,INFO+8)==2);
     assert(rd(&h,INFO+12)==2&&rd(&h,INFO+16)==FMT_A8R8G8B8&&rd(&h,INFO+20)==4);
     check_volume(&h,volume,4,4,2,2,1,0x20);
     assert(call(&h,volume,2,NULL,0)==0);}
    {uint32_t args[]={h.device,DATA,4,4,2,0,0,0,D9_POOL_DEFAULT,
                      1,1,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileExA",args,15)==HR_OK);
     uint32_t volume=rd(&h,OUT);assert(volume);
     check_volume(&h,volume,4,4,2,2,0,0x20);
     assert(call(&h,volume,2,NULL,0)==0);}

    /* ExA refusals: strict file-derived tuple only. */
    set_guest_path(&h,"cube.dds");
    {uint32_t args[]={h.device,DATA,4,0,0,0,D9_POOL_MANAGED,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"does not match"));}
    {uint32_t args[]={h.device,DATA,0,0,0,FMT_DXT1,D9_POOL_MANAGED,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"format"));}
    {uint32_t args[]={h.device,DATA,0,0,0,0,2/*SYSTEMMEM*/,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"Pool"));}
    {uint32_t args[]={h.device,DATA,0,0,0x200u,0,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"Usage"));}
    {uint32_t args[]={h.device,DATA,0,1,0,0,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"MipLevels"));}
    {uint32_t args[]={h.device,DATA,0,0,0,0,1,
                      0xffffffffu,0xffffffffu,0,0,0x1234u,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileExA",args,13)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"pPalette"));}

    set_guest_path(&h,"volume.dds");
    {uint32_t args[]={h.device,DATA,8,0,0,0,0,FMT_A8R8G8B8,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileExA",args,15)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"Width"));}
    {uint32_t args[]={h.device,DATA,0,0,4,0,0,FMT_A8R8G8B8,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileExA",args,15)==HR_NOTAVAILABLE);
     assert(strstr(d3dx_texture_shims_last_error(),"Depth"));}

    /* Path/IO/parser failures keep exact reasons. */
    set_guest_path(&h,"missing.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileA",args,3)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"cannot open"));}
    set_guest_path(&h,"../escape.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileA",args,3)==HR_INVALIDCALL);
     assert(strstr(d3dx_texture_shims_last_error(),"outside the sandbox"));}
    set_guest_path(&h,"C:\\Windows\\cube.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileA",args,3)==HR_INVALIDCALL);}
    set_guest_path(&h,"huge.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileA",args,3)==HR_INVALIDDATA);
     assert(strstr(d3dx_texture_shims_last_error(),"64 MiB"));}
    set_guest_path(&h,"bad.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileA",args,3)==HR_INVALIDDATA);
     assert(strstr(d3dx_texture_shims_last_error(),"not a DDS"));}

    /* Wrong resource type through the file loaders. */
    set_guest_path(&h,"volume.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateCubeTextureFromFileA",args,3)==HR_E_NOTIMPL);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"volume"));}
    set_guest_path(&h,"cube.dds");
    {uint32_t args[]={h.device,DATA,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateVolumeTextureFromFileA",args,3)==HR_INVALIDDATA);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"not a volume"));}

    /* The 2D file loader still refuses cube/volume DDS with E_NOTIMPL. */
    set_guest_path(&h,"cube.dds");
    {uint32_t args[]={h.device,DATA,0,0,0,0,0,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(!guest_write32(&h.memory,OUT,0xdeadbeefu));
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",args,14)==HR_E_NOTIMPL);
     assert(rd(&h,OUT)==0&&strstr(d3dx_texture_shims_last_error(),"cube"));}
    set_guest_path(&h,"volume.dds");
    {uint32_t args[]={h.device,DATA,0,0,0,0,0,1,
                      0xffffffffu,0xffffffffu,0,0,0,OUT};
     assert(shim_call(&h,"D3DXCreateTextureFromFileExA",args,14)==HR_E_NOTIMPL);
     assert(strstr(d3dx_texture_shims_last_error(),"volume"));}

    assert(call(&h,h.device,2,NULL,0)==0);
    assert(call(&h,h.factory,2,NULL,0)==0);
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_lift_release();
    sandbox_remove();
    guest_destroy(&h.memory);
    puts("D3DX9_42 file cube/volume shims: PASS");
    return 0;
}
