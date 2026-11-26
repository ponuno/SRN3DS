#include "d3dx_texture_shims.h"
#include "d3d9_com.h"
#include "win32_shims.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define D3DX_OK 0u
#define D3DX_E_NOTIMPL 0x80004001u
#define D3DX_D3DERR_INVALIDCALL 0x8876086cu
#define D3DX_D3DERR_NOTAVAILABLE 0x8876086au
#define D3DXERR_INVALIDDATA 0x88760b59u

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
    int rc=parse_dds(p,size,&d);
    if(rc){
        reject("D3DXGetImageInfoFromFileInMemory: %s (host shim supports legacy DDS only)",
               rc==-2?"unsupported DDS pixel format/header":"not a DDS file");
        RET(D3DXERR_INVALIDDATA,3);
    }
    wr32(info+0,d.width);wr32(info+4,d.height);wr32(info+8,d.depth);
    wr32(info+12,d.mips);wr32(info+16,d.format);wr32(info+20,d.resource);
    wr32(info+24,D3DX_FILETYPE_DDS);
    if(d.resource!=D3DRTYPE_TEXTURE)
        fprintf(stderr,"[d3dx] D3DXGetImageInfoFromFileInMemory: reporting %s DDS metadata "
                "(%ux%ux%u, %u mip(s), %s); texture upload for this resource type is not implemented\n",
                d.resource==D3DRTYPE_CUBETEXTURE?"cube":d.resource==D3DRTYPE_VOLUMETEXTURE?"volume":"non-texture",
                d.width,d.height,d.depth,d.mips,format_name(d.format));
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
    int prc=parse_dds(p,size,&d);
    if(prc){
        reject("D3DXCreateTextureFromFileInMemory: %s (host shim supports legacy DDS only)",
               prc==-2?"unsupported DDS pixel format/header":"not a DDS file");
        RET(D3DXERR_INVALIDDATA,4);
    }
    if(d.resource==D3DRTYPE_CUBETEXTURE){
        reject("D3DXCreateTextureFromFileInMemory: cube DDS is named unsupported (E_NOTIMPL)");
        RET(D3DX_E_NOTIMPL,4);
    }
    if(d.resource==D3DRTYPE_VOLUMETEXTURE){
        reject("D3DXCreateTextureFromFileInMemory: volume DDS is named unsupported (E_NOTIMPL)");
        RET(D3DX_E_NOTIMPL,4);
    }
    if(!texture_format_supported(d.format)){
        reject("D3DXCreateTextureFromFileInMemory: DDS format %s is not implemented by the D3D9 bridge",
               format_name(d.format));
        RET(D3DX_D3DERR_NOTAVAILABLE,4);
    }
    unsigned max_mips=1,w=d.width,h=d.height;
    if(w>1024u||h>1024u){
        reject("D3DXCreateTextureFromFileInMemory: %ux%u exceeds the D3D9 bridge's 1024 texture limit",
               d.width,d.height);
        RET(D3DX_D3DERR_NOTAVAILABLE,4);
    }
    if((w&(w-1u))||(h&(h-1u))){
        reject("D3DXCreateTextureFromFileInMemory: %ux%u is not power-of-two; "
               "the D3D9 bridge only creates pow2 textures",d.width,d.height);
        RET(D3DX_D3DERR_NOTAVAILABLE,4);
    }
    if(bc_bytes(d.format)&&(w<8u||h<8u)){
        reject("D3DXCreateTextureFromFileInMemory: compressed base level %ux%u "
               "is below the D3D9 bridge's 8x8 floor",d.width,d.height);
        RET(D3DX_D3DERR_NOTAVAILABLE,4);
    }
    while(w>1||h>1){if(w>1)w>>=1;if(h>1)h>>=1;++max_mips;}
    if(d.mips>max_mips){
        reject("D3DXCreateTextureFromFileInMemory: %u mip levels do not fit %ux%u",
               d.mips,d.width,d.height);
        RET(D3DXERR_INVALIDDATA,4);
    }
    /* Validate the entire mip chain before creating any resource. */
    size_t offset=d.data_offset;w=d.width;h=d.height;
    for(unsigned i=0;i<d.mips;++i){
        size_t pitch,bytes;
        if(level_layout(d.format,w,h,&pitch,&bytes)||
           offset>size||bytes>size-offset){
            reject("D3DXCreateTextureFromFileInMemory: truncated DDS mip chain at level %u",i);
            RET(D3DXERR_INVALIDDATA,4);
        }
        offset+=bytes;
        if(w>1)w>>=1;if(h>1)h>>=1;
    }
    uint32_t texture=0;
    /* D3DX uses D3DPOOL_MANAGED, usage 0 for file-created textures. */
    uint32_t hr=d9_com_host_create_texture(device,d.width,d.height,d.mips,0,d.format,
                                           D9_POOL_MANAGED,&texture);
    if(hr){
        reject("D3DXCreateTextureFromFileInMemory: %s",d9_com_last_diagnostic());
        RET(hr,4);
    }
    offset=d.data_offset;w=d.width;h=d.height;
    for(unsigned i=0;i<d.mips;++i){
        size_t pitch=0,bytes=0;
        if(level_layout(d.format,w,h,&pitch,&bytes)){ /* validated above */ }
        hr=d9_com_host_upload_texture_level(texture,i,p+offset,pitch,bytes);
        if(hr){
            reject("D3DXCreateTextureFromFileInMemory: level %u upload failed (%s)",
                   i,d9_com_last_diagnostic());
            d9_com_host_release(texture);
            RET(hr,4);
        }
        offset+=bytes;
        if(w>1)w>>=1;if(h>1)h>>=1;
    }
    *out_guest=texture;
    g.last_error[0]=0;
    fprintf(stderr,"[d3dx] D3DXCreateTextureFromFileInMemory: %ux%u %s, %u level(s)\n",
            d.width,d.height,format_name(d.format),d.mips);
    RET(D3DX_OK,4);
}

/* Named-unsupported exports.  They still resolve to a host thunk so the IAT no
 * longer points at an unlifted guest body; the call returns a real failure and
 * logs which export and path were rejected. */
static void shim_get_image_info_file_a(CPU *c){
    const char *path=(A(0)&&g.memory)?guest_cstr(g.memory,A(0),260):NULL;
    reject("D3DXGetImageInfoFromFileA(\"%s\"): file-path shim is not implemented (E_NOTIMPL)",
           path?path:"(invalid path)");
    RET(D3DX_E_NOTIMPL,2);
}
static void shim_create_tex_ex_a(CPU *c){
    const char *path=(A(0)&&g.memory)?guest_cstr(g.memory,A(0),260):NULL;
    uint32_t out=A(12);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(out_guest)*out_guest=0;
    reject("D3DXCreateTextureFromFileExA(\"%s\"): file-path shim is not implemented (E_NOTIMPL)",
           path?path:"(invalid path)");
    RET(D3DX_E_NOTIMPL,13);
}
static void shim_create_cube_mem(CPU *c){
    uint32_t out=A(3);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(out_guest)*out_guest=0;
    reject("D3DXCreateCubeTextureFromFileInMemory: cube texture creation is named unsupported (E_NOTIMPL)");
    RET(D3DX_E_NOTIMPL,4);
}
static void shim_create_volume_mem(CPU *c){
    uint32_t out=A(3);
    uint32_t *out_guest=out&&g.memory?guest_ptr(g.memory,out,4):NULL;
    if(out_guest)*out_guest=0;
    reject("D3DXCreateVolumeTextureFromFileInMemory: volume texture creation is named unsupported (E_NOTIMPL)");
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
