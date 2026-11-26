#include "d3dx_texture_shims.h"
#include "d3d9_com.h"
#include "win32_shims.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3DX_OK 0u
#define D3DX_E_NOTIMPL 0x80004001u
#define D3DX_D3DERR_INVALIDCALL 0x8876086cu
#define D3DX_D3DERR_NOTAVAILABLE 0x8876086au
#define D3DX_D3DERR_OUTOFMEMORY 0x8876017cu
#define D3DXERR_INVALIDDATA 0x88760b59u
#define D3DX_DEFAULT 0xffffffffu

#define D3DX_FILETYPE_DDS 4u
#define D3DRTYPE_TEXTURE 3u
#define D3DRTYPE_VOLUMETEXTURE 4u
#define D3DRTYPE_CUBETEXTURE 5u

#define D3DFMT_R8G8B8 20u
#define D3DFMT_A8R8G8B8 21u
#define D3DFMT_X8R8G8B8 22u
#define D3DFMT_R5G6B5 23u
#define D3DFMT_X1R5G5B5 24u
#define D3DFMT_A1R5G5B5 25u
#define D3DFMT_A4R4G4B4 26u
#define D3DFMT_A8B8G8R8 32u
#define D3DFMT_X8B8G8R8 33u
#define D3DFMT_L8 50u
#define D3DFMT_A8L8 51u
#define D3DFMT_DXT1 0x31545844u
#define D3DFMT_DXT2 0x32545844u
#define D3DFMT_DXT3 0x33545844u
#define D3DFMT_DXT4 0x34545844u
#define D3DFMT_DXT5 0x35545844u

/* DDS_HEADER flags/caps (the subset this parser gives meaning to). */
#define DDSD_MIPMAPCOUNT 0x00020000u
#define DDSD_DEPTH 0x00800000u
#define DDSCAPS2_CUBEMAP 0x00000200u
#define DDSCAPS2_VOLUME 0x00200000u
#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_FOURCC 0x00000004u
#define DDPF_RGB 0x00000040u
#define DDPF_LUMINANCE 0x00020000u

typedef struct {
    unsigned width,height,depth,mips,format,resource;
    size_t data_offset;
} DdsImage;

static struct {
    GuestMemory *memory;
    char last_error[256];
} g;

static uint32_t rd32(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void wr32(uint8_t *p,uint32_t v){
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static void reject(const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    vsnprintf(g.last_error,sizeof g.last_error,fmt,ap);
    va_end(ap);
    fprintf(stderr,"[d3dx] %s\n",g.last_error);
}
const char *d3dx_texture_shims_last_error(void){return g.last_error;}
static uint32_t arg(CPU *c,unsigned n){return M32(c->r[4]+4+n*4);}
#define A(n) arg(c,n)
#define RET(v,n) do{c->r[0]=(uint32_t)(v);c->r[4]+=4+4u*(n);return;}while(0)

static int same_dll(const char *a,const char *b){
    if(!a||!b)return 0;
    while(*a&&*b){
        unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;
        if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;
        if(x!=y)return 0;
    }
    return *a==*b;
}

static unsigned dds_format(uint32_t pf_flags,uint32_t fourcc,uint32_t bits,
                           uint32_t rmask,uint32_t gmask,uint32_t bmask,uint32_t amask){
    if(pf_flags&DDPF_FOURCC){
        switch(fourcc){
        case D3DFMT_DXT1:return D3DFMT_DXT1;
        case D3DFMT_DXT2:return D3DFMT_DXT2;
        case D3DFMT_DXT3:return D3DFMT_DXT3;
        case D3DFMT_DXT4:return D3DFMT_DXT4;
        case D3DFMT_DXT5:return D3DFMT_DXT5;
        default:return 0;
        }
    }
    if(pf_flags&DDPF_RGB){
        if(bits==32){
            if(rmask==0x00ff0000u&&gmask==0x0000ff00u&&bmask==0x000000ffu)
                return amask==0xff000000u?D3DFMT_A8R8G8B8:D3DFMT_X8R8G8B8;
            if(rmask==0x000000ffu&&gmask==0x0000ff00u&&bmask==0x00ff0000u)
                return amask==0xff000000u?D3DFMT_A8B8G8R8:D3DFMT_X8B8G8R8;
            return 0;
        }
        if(bits==24&&rmask==0x00ff0000u&&gmask==0x0000ff00u&&bmask==0x000000ffu)
            return D3DFMT_R8G8B8;
        if(bits==16){
            if(rmask==0xf800u&&gmask==0x07e0u&&bmask==0x001fu)
                return amask?0:D3DFMT_R5G6B5;
            if(rmask==0x7c00u&&gmask==0x03e0u&&bmask==0x001fu)
                return amask==0x8000u?D3DFMT_A1R5G5B5:D3DFMT_X1R5G5B5;
            if(rmask==0x0f00u&&gmask==0x00f0u&&bmask==0x000fu)
                return amask==0xf000u?D3DFMT_A4R4G4B4:0;
            return 0;
        }
    }
    if(pf_flags&DDPF_LUMINANCE){
        if(bits==8)return D3DFMT_L8;
        if(bits==16)return D3DFMT_A8L8;
    }
    return 0;
}

/* Returns 0 for a structurally valid legacy (128-byte header) DDS.  DX10
 * headers and unknown pixel formats are rejected explicitly: silently guessing
 * a D3D9 format would be worse than the named failure. */
static int parse_dds(const uint8_t *p,size_t n,DdsImage *out){
    if(!p||!out||n<128)return -1;
    if(memcmp(p,"DDS ",4)!=0)return -1;
    if(rd32(p+4)!=124u)return -1;          /* DDS_HEADER.dwSize */
    uint32_t flags=rd32(p+8);
    unsigned height=rd32(p+12),width=rd32(p+16);
    if(!width||!height)return -1;
    if(rd32(p+76)!=32u)return -1;          /* DDS_PIXELFORMAT.dwSize */
    uint32_t pf_flags=rd32(p+80);
    uint32_t fourcc=rd32(p+84);
    if((pf_flags&DDPF_FOURCC)&&fourcc==0x30315844u)return -2; /* "DX10" */
    unsigned format=dds_format(pf_flags,fourcc,rd32(p+88),rd32(p+92),
                               rd32(p+96),rd32(p+100),rd32(p+104));
    if(!format)return -2;
    unsigned mips=1;
    if(flags&DDSD_MIPMAPCOUNT){unsigned m=rd32(p+28);if(m)mips=m;}
    if(mips>16u)return -1;
    uint32_t caps2=rd32(p+112);
    unsigned resource=D3DRTYPE_TEXTURE,depth=1;
    if(caps2&DDSCAPS2_CUBEMAP)resource=D3DRTYPE_CUBETEXTURE;
    else if(caps2&DDSCAPS2_VOLUME){
        resource=D3DRTYPE_VOLUMETEXTURE;
        depth=(flags&DDSD_DEPTH)?rd32(p+24):1u;
        if(!depth)depth=1;
    }
    out->width=width;out->height=height;out->depth=depth;out->mips=mips;
    out->format=format;out->resource=resource;out->data_offset=128u;
    return 0;
}

static unsigned bc_bytes(unsigned format){
    if(format==D3DFMT_DXT1)return 8u;
    if(format==D3DFMT_DXT3||format==D3DFMT_DXT5)return 16u;
    return 0;
}
static int texture_format_supported(unsigned format){
    return bc_bytes(format)||format==D3DFMT_A8R8G8B8||format==D3DFMT_X8R8G8B8;
}
static const char *format_name(unsigned f){
    switch(f){
    case D3DFMT_A8R8G8B8:return "A8R8G8B8";
    case D3DFMT_X8R8G8B8:return "X8R8G8B8";
    case D3DFMT_A8B8G8R8:return "A8B8G8R8";
    case D3DFMT_X8B8G8R8:return "X8B8G8R8";
    case D3DFMT_DXT1:return "DXT1";
    case D3DFMT_DXT2:return "DXT2";
    case D3DFMT_DXT3:return "DXT3";
    case D3DFMT_DXT4:return "DXT4";
    case D3DFMT_DXT5:return "DXT5";
    case D3DFMT_R8G8B8:return "R8G8B8";
    case D3DFMT_R5G6B5:return "R5G6B5";
    case D3DFMT_A1R5G5B5:return "A1R5G5B5";
    case D3DFMT_A4R4G4B4:return "A4R4G4B4";
    case D3DFMT_L8:return "L8";
    default:return "unknown";
    }
}
static int level_layout(unsigned format,unsigned w,unsigned h,size_t *pitch,size_t *bytes){
    if(!w||!h||!pitch||!bytes)return -1;
    unsigned bb=bc_bytes(format);
    if(bb){*pitch=(size_t)((w+3u)/4u)*bb;*bytes=*pitch*((h+3u)/4u);return 0;}
    if(format==D3DFMT_A8R8G8B8||format==D3DFMT_X8R8G8B8){
        *pitch=(size_t)w*4u;*bytes=*pitch*h;return 0;
    }
    return -1;
}

#define D3DX_FILE_LIMIT (64u<<20) /* 64 MiB: refuse to buffer larger files */

static int parse_checked(const char *who,const uint8_t *p,size_t n,DdsImage *d){
    int rc=parse_dds(p,n,d);
    if(rc)
        reject("%s: %s (host shim supports legacy DDS only)",who,
               rc==-2?"unsupported DDS pixel format/header":"not a DDS file");
    return rc;
}

static void write_image_info(uint8_t *info,const DdsImage *d,const char *who){
    wr32(info+0,d->width);wr32(info+4,d->height);wr32(info+8,d->depth);
    wr32(info+12,d->mips);wr32(info+16,d->format);wr32(info+20,d->resource);
    wr32(info+24,D3DX_FILETYPE_DDS);
    if(d->resource!=D3DRTYPE_TEXTURE)
        fprintf(stderr,"[d3dx] %s: reporting %s DDS metadata "
                "(%ux%ux%u, %u mip(s), %s); upload for this resource type is not implemented\n",
                who,
                d->resource==D3DRTYPE_CUBETEXTURE?"cube":d->resource==D3DRTYPE_VOLUMETEXTURE?"volume":"non-texture",
                d->width,d->height,d->depth,d->mips,format_name(d->format));
}

/* Create and fill an IDirect3DTexture9 from an already-parsed DDS.  Every
 * rejection happens before d9_com_host_create_texture(); any failure after
 * creation releases the texture.  *out is only written on success. */
static uint32_t create_2d_from_dds(const char *who,uint32_t device,const uint8_t *p,
                                   size_t size,DdsImage *d,uint32_t *out){
    if(d->resource==D3DRTYPE_CUBETEXTURE||d->resource==D3DRTYPE_VOLUMETEXTURE){
        reject("%s: %s DDS needs IDirect3D%sTexture9, which the D3D9 bridge does not "
               "implement (d3d9_core/d3d9_com expose 2D textures only); E_NOTIMPL",
               who,d->resource==D3DRTYPE_CUBETEXTURE?"cube":"volume",
               d->resource==D3DRTYPE_CUBETEXTURE?"Cube":"Volume");
        return D3DX_E_NOTIMPL;
    }
    if(!texture_format_supported(d->format)){
        reject("%s: DDS format %s is not implemented by the D3D9 bridge",
               who,format_name(d->format));
        return D3DX_D3DERR_NOTAVAILABLE;
    }
    unsigned max_mips=1,w=d->width,h=d->height;
    if(w>1024u||h>1024u){
        reject("%s: %ux%u exceeds the D3D9 bridge's 1024 texture limit",
               who,d->width,d->height);
        return D3DX_D3DERR_NOTAVAILABLE;
    }
    if((w&(w-1u))||(h&(h-1u))){
        reject("%s: %ux%u is not power-of-two; "
               "the D3D9 bridge only creates pow2 textures",who,d->width,d->height);
        return D3DX_D3DERR_NOTAVAILABLE;
    }
    if(bc_bytes(d->format)&&(w<8u||h<8u)){
        reject("%s: compressed base level %ux%u "
               "is below the D3D9 bridge's 8x8 floor",who,d->width,d->height);
        return D3DX_D3DERR_NOTAVAILABLE;
    }
    while(w>1||h>1){if(w>1)w>>=1;if(h>1)h>>=1;++max_mips;}
    if(d->mips>max_mips){
        reject("%s: %u mip levels do not fit %ux%u",who,
               d->mips,d->width,d->height);
        return D3DXERR_INVALIDDATA;
    }
    /* Validate the entire mip chain before creating any resource. */
    size_t offset=d->data_offset;w=d->width;h=d->height;
    for(unsigned i=0;i<d->mips;++i){
        size_t pitch,bytes;
        if(level_layout(d->format,w,h,&pitch,&bytes)||
           offset>size||bytes>size-offset){
            reject("%s: truncated DDS mip chain at level %u",who,i);
            return D3DXERR_INVALIDDATA;
        }
        offset+=bytes;
        if(w>1)w>>=1;if(h>1)h>>=1;
    }
    uint32_t texture=0;
    /* D3DX uses D3DPOOL_MANAGED, usage 0 for file-created textures. */
    uint32_t hr=d9_com_host_create_texture(device,d->width,d->height,d->mips,0,d->format,
                                           D9_POOL_MANAGED,&texture);
    if(hr){
        reject("%s: %s",who,d9_com_last_diagnostic());
        return hr;
    }
    offset=d->data_offset;w=d->width;h=d->height;
    for(unsigned i=0;i<d->mips;++i){
        size_t pitch=0,bytes=0;
        if(level_layout(d->format,w,h,&pitch,&bytes)){ /* validated above */ }
        hr=d9_com_host_upload_texture_level(texture,i,p+offset,pitch,bytes);
        if(hr){
            reject("%s: level %u upload failed (%s)",who,i,d9_com_last_diagnostic());
            d9_com_host_release(texture);
            return hr;
        }
        offset+=bytes;
        if(w>1)w>>=1;if(h>1)h>>=1;
    }
    *out=texture;
    fprintf(stderr,"[d3dx] %s: %ux%u %s, %u level(s)\n",
            who,d->width,d->height,format_name(d->format),d->mips);
    return D3DX_OK;
}

/* Read a guest-named file through the win32 sandbox mapping.  The returned
 * buffer belongs to the caller; failures never write it. */
static uint32_t read_sandbox_file(const char *who,const char *guest_path,
                                  uint8_t **out,size_t *out_size){
    *out=NULL;*out_size=0;
    char host[640];
    if(win32_guest_path(guest_path,host,sizeof host)){
        reject("%s(\"%s\"): path is outside the sandbox game root",who,guest_path);
        return D3DX_D3DERR_INVALIDCALL;
    }
    FILE *f=fopen(host,"rb");
    if(!f){
        reject("%s(\"%s\"): cannot open the file inside the sandbox "
               "(missing, unreadable, or not a regular file)",who,guest_path);
        return D3DXERR_INVALIDDATA;
    }
    if(fseek(f,0,SEEK_END)!=0){
        fclose(f);
        reject("%s(\"%s\"): cannot determine the file size",who,guest_path);
        return D3DXERR_INVALIDDATA;
    }
    long length=ftell(f);
    if(length<=0){
        fclose(f);
        reject("%s(\"%s\"): file is empty or its size cannot be read",who,guest_path);
        return D3DXERR_INVALIDDATA;
    }
    if((unsigned long)length>D3DX_FILE_LIMIT){
        fclose(f);
        reject("%s(\"%s\"): file is %ld bytes, above the 64 MiB shim limit",
               who,guest_path,length);
        return D3DXERR_INVALIDDATA;
    }
    if(fseek(f,0,SEEK_SET)!=0){
        fclose(f);
        reject("%s(\"%s\"): cannot rewind the file",who,guest_path);
        return D3DXERR_INVALIDDATA;
    }
    uint8_t *buffer=malloc((size_t)length);
    if(!buffer){
        fclose(f);
        reject("%s(\"%s\"): out of host memory for %ld bytes",who,guest_path,length);
        return D3DX_D3DERR_OUTOFMEMORY;
    }
    size_t got=fread(buffer,1,(size_t)length,f);
    int read_failed=ferror(f)||got!=(size_t)length;
    fclose(f);
    if(read_failed){
        free(buffer);
        reject("%s(\"%s\"): short read (%lu of %ld bytes)",who,guest_path,
               (unsigned long)got,length);
        return D3DXERR_INVALIDDATA;
    }
    *out=buffer;*out_size=(size_t)length;
    return D3DX_OK;
}

static void shim_get_image_info_mem(CPU *c){
    uint32_t src=A(0),size=A(1),out=A(2);
    const uint8_t *p=src&&size&&g.memory?guest_ptr(g.memory,src,size):NULL;
    uint8_t *info=out&&g.memory?guest_ptr(g.memory,out,28):NULL;
    if(!info){reject("D3DXGetImageInfoFromFileInMemory: invalid D3DXIMAGE_INFO pointer");RET(D3DX_D3DERR_INVALIDCALL,3);}
    memset(info,0,28);
    if(!p){
        reject("D3DXGetImageInfoFromFileInMemory: source buffer is null or outside guest memory");
        RET(D3DX_D3DERR_INVALIDCALL,3);
    }
    DdsImage d;
    if(parse_checked("D3DXGetImageInfoFromFileInMemory",p,size,&d))
        RET(D3DXERR_INVALIDDATA,3);
    write_image_info(info,&d,"D3DXGetImageInfoFromFileInMemory");
    g.last_error[0]=0;
    RET(D3DX_OK,3);
}

static void shim_create_tex_mem(CPU *c){
    uint32_t device=A(0),src=A(1),size=A(2),out=A(3);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(!out_guest){reject("D3DXCreateTextureFromFileInMemory: invalid output pointer");RET(D3DX_D3DERR_INVALIDCALL,4);}
    *out_guest=0;
    const uint8_t *p=src&&size&&g.memory?guest_ptr(g.memory,src,size):NULL;
    if(!p){
        reject("D3DXCreateTextureFromFileInMemory: source buffer is null or outside guest memory");
        RET(D3DX_D3DERR_INVALIDCALL,4);
    }
    DdsImage d;
    if(parse_checked("D3DXCreateTextureFromFileInMemory",p,size,&d))
        RET(D3DXERR_INVALIDDATA,4);
    uint32_t texture=0;
    uint32_t hr=create_2d_from_dds("D3DXCreateTextureFromFileInMemory",device,p,size,&d,&texture);
    if(hr)RET(hr,4);
    *out_guest=texture;
    g.last_error[0]=0;
    RET(D3DX_OK,4);
}

/* File-path exports.  The guest path is bounded, mapped below the sandbox root
 * by win32_guest_path(), buffered on the host, then parsed by the exact same
 * legacy-DDS code as the InMemory entry points. */
static void shim_get_image_info_file_a(CPU *c){
    uint32_t out=A(1);
    uint8_t *info=out&&g.memory?guest_ptr(g.memory,out,28):NULL;
    if(!info){
        reject("D3DXGetImageInfoFromFileA: invalid D3DXIMAGE_INFO pointer");
        RET(D3DX_D3DERR_INVALIDCALL,2);
    }
    memset(info,0,28);
    const char *path=(A(0)&&g.memory)?guest_cstr(g.memory,A(0),260):NULL;
    if(!path){
        reject("D3DXGetImageInfoFromFileA: source path is null, unterminated, "
               "or longer than 259 bytes");
        RET(D3DX_D3DERR_INVALIDCALL,2);
    }
    uint8_t *buffer=NULL;size_t size=0;
    uint32_t hr=read_sandbox_file("D3DXGetImageInfoFromFileA",path,&buffer,&size);
    if(hr)RET(hr,2);
    DdsImage d;
    int prc=parse_checked("D3DXGetImageInfoFromFileA",buffer,size,&d);
    if(prc){free(buffer);RET(D3DXERR_INVALIDDATA,2);}
    write_image_info(info,&d,"D3DXGetImageInfoFromFileA");
    free(buffer);
    g.last_error[0]=0;
    RET(D3DX_OK,2);
}

static void shim_create_tex_ex_a(CPU *c){
    uint32_t device=A(0);
    uint32_t width=A(2),height=A(3),mip_levels=A(4),usage=A(5),format=A(6),pool=A(7);
    uint32_t filter=A(8),mip_filter=A(9),color_key=A(10),info_addr=A(11),out=A(13);
    /* A(12) is PALETTEENTRY*.  It is deliberately ignored: only non-paletted
     * 32-bit/BC formats reach the create path.  The real D3DX9_42 export has
     * 14 arguments and a `ret 0x38` epilogue (verified from the DLL), so the
     * shim must pop 14 slots, not 13. */
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(!out_guest){
        reject("D3DXCreateTextureFromFileExA: invalid output texture pointer");
        RET(D3DX_D3DERR_INVALIDCALL,14);
    }
    *out_guest=0;
    if(width||height||mip_levels||usage||format||pool!=D9_POOL_MANAGED){
        reject("D3DXCreateTextureFromFileExA: only the file-derived subset is implemented "
               "(Width=0, Height=0, MipLevels=0, Usage=0, Format=D3DFMT_UNKNOWN, "
               "Pool=D3DPOOL_MANAGED); got %lux%lu, mips=%lu, usage=0x%lx, format=%lu, pool=%lu",
               (unsigned long)width,(unsigned long)height,(unsigned long)mip_levels,
               (unsigned long)usage,(unsigned long)format,(unsigned long)pool);
        RET(D3DX_D3DERR_NOTAVAILABLE,14);
    }
    if((filter!=D3DX_DEFAULT&&filter!=0u)||(mip_filter!=D3DX_DEFAULT&&mip_filter!=0u)){
        reject("D3DXCreateTextureFromFileExA: Filter/MipFilter must be D3DX_DEFAULT "
               "(0xffffffff) or 0 because no resize/filter path exists; got 0x%lx/0x%lx",
               (unsigned long)filter,(unsigned long)mip_filter);
        RET(D3DX_D3DERR_NOTAVAILABLE,14);
    }
    if(color_key){
        reject("D3DXCreateTextureFromFileExA: ColorKey 0x%08lx is not implemented; "
               "use 0",(unsigned long)color_key);
        RET(D3DX_D3DERR_NOTAVAILABLE,14);
    }
    uint8_t *info=info_addr&&g.memory?guest_ptr(g.memory,info_addr,28):NULL;
    if(info_addr&&!info){
        reject("D3DXCreateTextureFromFileExA: invalid D3DXIMAGE_INFO output pointer");
        RET(D3DX_D3DERR_INVALIDCALL,14);
    }
    const char *path=(A(1)&&g.memory)?guest_cstr(g.memory,A(1),260):NULL;
    if(!path){
        reject("D3DXCreateTextureFromFileExA: source path is null, unterminated, "
               "or longer than 259 bytes");
        RET(D3DX_D3DERR_INVALIDCALL,14);
    }
    uint8_t *buffer=NULL;size_t size=0;
    uint32_t hr=read_sandbox_file("D3DXCreateTextureFromFileExA",path,&buffer,&size);
    if(hr)RET(hr,14);
    DdsImage d;
    int prc=parse_checked("D3DXCreateTextureFromFileExA",buffer,size,&d);
    if(prc){free(buffer);RET(D3DXERR_INVALIDDATA,14);}
    if(info)write_image_info(info,&d,"D3DXCreateTextureFromFileExA");
    uint32_t texture=0;
    hr=create_2d_from_dds("D3DXCreateTextureFromFileExA",device,buffer,size,&d,&texture);
    free(buffer);
    if(hr)RET(hr,14);
    *out_guest=texture;
    g.last_error[0]=0;
    RET(D3DX_OK,14);
}

static void shim_create_cube_mem(CPU *c){
    uint32_t out=A(3);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(out_guest)*out_guest=0;
    reject("D3DXCreateCubeTextureFromFileInMemory: the D3D9 bridge has no "
           "IDirect3DCubeTexture9 object type, cube-face surfaces, or six-face backend "
           "upload (d3d9_core/d3d9_com are 2D-only); E_NOTIMPL");
    RET(D3DX_E_NOTIMPL,4);
}
static void shim_create_volume_mem(CPU *c){
    uint32_t out=A(3);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(out_guest)*out_guest=0;
    reject("D3DXCreateVolumeTextureFromFileInMemory: the D3D9 bridge has no "
           "IDirect3DVolumeTexture9 object type, depth slices, or volume upload "
           "(d3d9_core/d3d9_com are 2D-only); E_NOTIMPL");
    RET(D3DX_E_NOTIMPL,4);
}

#define D3DX_FUNCTIONS(X) X(get_image_info_mem) X(create_tex_mem) X(get_image_info_file_a) \
    X(create_tex_ex_a) X(create_cube_mem) X(create_volume_mem)
#define D3DX_ENUM(f) D3DX_##f,
enum { D3DX_FUNCTIONS(D3DX_ENUM) D3DX_FUNCTION_COUNT };
static const struct {const char *name;unsigned index;} export_names[]={
    {"D3DXGetImageInfoFromFileInMemory",D3DX_get_image_info_mem},
    {"D3DXCreateTextureFromFileInMemory",D3DX_create_tex_mem},
    {"D3DXGetImageInfoFromFileA",D3DX_get_image_info_file_a},
    {"D3DXCreateTextureFromFileExA",D3DX_create_tex_ex_a},
    {"D3DXCreateCubeTextureFromFileInMemory",D3DX_create_cube_mem},
    {"D3DXCreateVolumeTextureFromFileInMemory",D3DX_create_volume_mem},
};
#define ENTRY(f) {D3DX_TEXTURE_THUNK_BASE+D3DX_##f*4,shim_##f},
static const FnEntry d3dx_entries[]={D3DX_FUNCTIONS(ENTRY)};

uint32_t d3dx_texture_shims_resolve(const PeImport *entry,void *ctx){
    (void)ctx;
    if(!g.memory||!entry||!entry->dll||!entry->name)return 0;
    if(!same_dll(entry->dll,"d3dx9_42.dll")&&!same_dll(entry->dll,"d3dx9_42"))return 0;
    for(unsigned i=0;i<sizeof export_names/sizeof export_names[0];++i)
        if(!strcmp(entry->name,export_names[i].name))
            return D3DX_TEXTURE_THUNK_BASE+export_names[i].index*4;
    return 0;
}
int d3dx_texture_shims_install(GuestMemory *memory){
    if(!memory)return -1;
    if(win32_add_external_resolver(d3dx_texture_shims_resolve,NULL))return -1;
    g.memory=memory;g.last_error[0]=0;
    return 0;
}
void d3dx_texture_shims_uninstall(void){g.memory=NULL;g.last_error[0]=0;}
FnModule d3dx_texture_shims_module(void){
    return (FnModule){"d3dx-texture-shims",d3dx_entries,D3DX_FUNCTION_COUNT};
}
