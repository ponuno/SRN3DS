#include "d3d9_adapter.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(D9AdapterIdentifier)==1100,"D3DADAPTER_IDENTIFIER9 x86 layout");
_Static_assert(offsetof(D9AdapterIdentifier,whql_level)==1096,"D3DADAPTER_IDENTIFIER9 WHQLLevel");
_Static_assert(sizeof(D9Caps9)==304,"D3DCAPS9 layout");
_Static_assert(offsetof(D9Caps9,vertex_shader_version)==196,"D3DCAPS9 VertexShaderVersion");
_Static_assert(offsetof(D9Caps9,max_vertex_shader_const)==200,"D3DCAPS9 MaxVertexShaderConst");
_Static_assert(offsetof(D9Caps9,pixel_shader_version)==204,"D3DCAPS9 PixelShaderVersion");
_Static_assert(offsetof(D9Caps9,decl_types)==236,"D3DCAPS9 DeclTypes");
_Static_assert(offsetof(D9Caps9,vs20)==248,"D3DCAPS9 VS20Caps");
_Static_assert(offsetof(D9Caps9,ps20)==264,"D3DCAPS9 PS20Caps");
_Static_assert(offsetof(D9Caps9,max_vertex_shader_30_instruction_slots)==296,"D3DCAPS9 SM3 slots");

enum {DEVTYPE_HAL=1,RTYPE_SURFACE=1,RTYPE_TEXTURE=3};
#define DEV_DRAWPRIMITIVES2 0x00002000u
#define DEV_DRAWPRIMITIVES2EX 0x00008000u
#define DEV_HWTRANSFORMANDLIGHT 0x00010000u
#define DEV_HWRASTERIZATION 0x00080000u
#define MISC_MASKZ 0x00000002u
#define MISC_CULLNONE 0x00000010u
#define MISC_CULLCW 0x00000020u
#define MISC_CULLCCW 0x00000040u
#define MISC_COLORWRITEENABLE 0x00000080u
#define MISC_BLENDOP 0x00000800u
#define RASTER_ZTEST 0x00000010u
#define TEX_PERSPECTIVE 0x00000001u
#define TEX_POW2 0x00000002u
#define TEX_ALPHA 0x00000004u
#define TEX_MIPMAP 0x00004000u
#define FILTER_MAGPOINT 0x00000001u
#define FILTER_MAGLINEAR 0x00000002u
#define FILTER_MINPOINT 0x00000100u
#define FILTER_MINLINEAR 0x00000200u
#define FILTER_MIPPOINT 0x00010000u
#define FILTER_MIPLINEAR 0x00020000u
#define ADDR_WRAP 0x00000001u
#define ADDR_MIRROR 0x00000002u
#define ADDR_CLAMP 0x00000004u
#define ADDR_INDEPENDENTUV 0x00000010u
#define DECL_TYPES 0x000003ffu /* UBYTE4..FLOAT16_4 as decoded by d9_decl_decode */
#define BLEND_ALL 0x000007ffu  /* ZERO..SRCALPHASAT as mapped by d9_citro3d */
#define USAGE_QUERY_FILTER 0x00020000u
#define USAGE_QUERY_WRAPANDMIP 0x00200000u
#define USAGE_RENDERTARGET 0x00000001u
#define DEVCAPS2_STREAMOFFSET 0x00000001u
#define VS_VERSION_2_0 0xfffe0200u
#define PS_VERSION_2_0 0xffff0200u

static int display_format(unsigned format){return format==D9_X8R8G8B8;}
/* Measurement-only caps probe.  It reports the HW T&L bit so the engine's
 * post-gate call stream can be recorded before any honest implementation
 * decision.  Off unless TESV_D3D9_CAPS_PROBE=1; never a shipping value. */
static int devcaps_probe_state;
static int devcaps_probe_enabled(void){
    if(!devcaps_probe_state){
        const char *v=getenv("TESV_D3D9_CAPS_PROBE");
        int on=v&&v[0]&&strcmp(v,"0");
        devcaps_probe_state=on?2:1;
        if(on)fprintf(stderr,"[d3d9] MEASUREMENT ONLY: GetDeviceCaps reports "
            "D3DDEVCAPS_HWTRANSFORMANDLIGHT (0x10000) although fixed-function "
            "lighting is absent. Not shippable and not a capability claim.\n");
    }
    return devcaps_probe_state==2;
}
static int texture_format(unsigned format){
    return format==D9_A8R8G8B8||format==D9_X8R8G8B8||format==D9_DXT1||format==D9_DXT3||format==D9_DXT5;
}

int d9_adapter_mode_count(unsigned adapter,unsigned format){
    return adapter<D9_ADAPTER_COUNT&&display_format(format)?1:0;
}
int d9_adapter_display_mode(unsigned adapter,unsigned format,unsigned index,D9DisplayMode *out){
    if(adapter>=D9_ADAPTER_COUNT||!display_format(format)||index>=1||!out)return D9_ADAPTER_INVALID;
    *out=(D9DisplayMode){D9_DISPLAY_WIDTH,D9_DISPLAY_HEIGHT,D9_DISPLAY_REFRESH,format};return D9_ADAPTER_OK;
}
int d9_adapter_identifier(unsigned adapter,D9AdapterIdentifier *out){
    if(adapter>=D9_ADAPTER_COUNT||!out)return D9_ADAPTER_INVALID;
    D9AdapterIdentifier id;memset(&id,0,sizeof id);
    /* DriverVersion, VendorId/DeviceId/SubSysId/Revision and WHQLLevel stay
     * zero: this is a software bridge, not a signed hardware driver. */
    memcpy(id.driver,D9_ADAPTER_DRIVER_NAME,sizeof D9_ADAPTER_DRIVER_NAME);
    memcpy(id.description,D9_ADAPTER_DESCRIPTION,sizeof D9_ADAPTER_DESCRIPTION);
    memcpy(id.device_name,D9_ADAPTER_DEVICE_NAME,sizeof D9_ADAPTER_DEVICE_NAME);
    memcpy(id.device_identifier,D9_ADAPTER_DEVICE_ID,sizeof D9_ADAPTER_DEVICE_ID);
    *out=id;return D9_ADAPTER_OK;
}
const char *d9_adapter_devcaps_reason(uint32_t bit){
    switch(bit){
    case DEV_DRAWPRIMITIVES2:
        return "claimed: DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP and DrawIndexedPrimitiveUP are implemented for the supported FVF/declaration subset";
    case DEV_HWRASTERIZATION:
        return "claimed: PICA200 rasterises the submitted triangles through the citro3d backend";
    case DEV_DRAWPRIMITIVES2EX:
        return "not claimed: DRAWPRIMITIVES2EX requires extended DrawPrimitives2 (DX7 driver) conformance; this bridge implements only its documented D3D9 entry points";
    case DEV_HWTRANSFORMANDLIGHT:
        return "not claimed: HWTRANSFORMANDLIGHT requires hardware transformation and lighting and DRAWPRIMITIVES2EX; only one clip-matrix transform exists and SetLight/LightEnable/SetMaterial, normals and per-vertex lighting are absent";
    default:
        return "not claimed: DevCaps bit is not part of the implemented bridge capability set";
    }
}
int d9_adapter_caps(unsigned adapter,unsigned device_type,D9Caps9 *out){
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL||!out)return D9_ADAPTER_INVALID;
    D9Caps9 c;memset(&c,0,sizeof c);
    c.device_type=DEVTYPE_HAL;c.adapter_ordinal=adapter;
    /* Caps/Caps2/Caps3/PresentationIntervals/CursorCaps stay zero: no gamma,
     * scanline reads, autogen mips, managed resources, cursor or interval control. */
    /* DRAWPRIMITIVES2 is claimed because the four draw entry points really run.
     * DRAWPRIMITIVES2EX (DX7 extended DrawPrimitives2 conformance) and
     * HWTRANSFORMANDLIGHT (hardware transformation AND lighting, which also
     * implies DRAWPRIMITIVES2EX) stay clear: the PICA path has one clip-matrix
     * vertex transform and no fixed-function lights, materials or normals.
     * See d9_adapter_devcaps_reason() for the per-bit contract. */
    c.dev_caps=DEV_DRAWPRIMITIVES2|DEV_HWRASTERIZATION;
    if(devcaps_probe_enabled())c.dev_caps|=DEV_HWTRANSFORMANDLIGHT;
    c.primitive_misc_caps=MISC_MASKZ|MISC_CULLNONE|MISC_CULLCW|MISC_CULLCCW|MISC_COLORWRITEENABLE|MISC_BLENDOP;
    c.raster_caps=RASTER_ZTEST;
    c.z_cmp_caps=0xffu;   /* all eight D3DCMP_* map to PICA depth functions */
    c.alpha_cmp_caps=0xffu;
    c.src_blend_caps=c.dest_blend_caps=BLEND_ALL;
    c.texture_caps=TEX_PERSPECTIVE|TEX_POW2|TEX_ALPHA|TEX_MIPMAP;
    c.texture_filter_caps=FILTER_MAGPOINT|FILTER_MAGLINEAR|FILTER_MINPOINT|FILTER_MINLINEAR|FILTER_MIPPOINT|FILTER_MIPLINEAR;
    c.texture_address_caps=ADDR_WRAP|ADDR_MIRROR|ADDR_CLAMP|ADDR_INDEPENDENTUV;
    c.max_texture_width=c.max_texture_height=1024; /* d9_texture_layout: power of two 8..1024 */
    c.max_texture_repeat=1024;
    c.max_texture_aspect_ratio=128; /* 1024/8 */
    c.max_anisotropy=1;
    c.max_vertex_w=1.0e10f;
    c.fvf_caps=1; /* one texture-coordinate set: FVF XYZ|TEX1 */
    c.max_texture_blend_stages=c.max_simultaneous_textures=1; /* one SetTexture stage */
    c.max_primitive_count=4096u/3u; /* d3d9_core MAX_DRAW_VERTICES */
    c.max_vertex_index=0xffffu;     /* guaranteed by the INDEX16 path */
    c.max_streams=16;               /* D9_MAX_STREAMS */
    c.max_stream_stride=4096;       /* d9_set_stream */
    c.vertex_shader_version=VS_VERSION_2_0;
    c.max_vertex_shader_const=4;    /* the exact VS template reads c0..c3 */
    c.pixel_shader_version=PS_VERSION_2_0;
    c.dev_caps2=DEVCAPS2_STREAMOFFSET; /* d9_set_stream accepts an offset */
    c.number_of_adapters_in_group=1;
    c.decl_types=DECL_TYPES;
    c.num_simultaneous_rts=1;       /* one active target: back buffer or one explicit LDR surface */
    c.ps20.num_temps=1;             /* r0 in the exact PS template */
    c.ps20.num_instruction_slots=2; /* texld + mov */
    c.max_vshader_instructions_executed=2; /* m4x4 + mov */
    c.max_pshader_instructions_executed=2;
    *out=c;return D9_ADAPTER_OK;
}
int d9_adapter_check_device_type(unsigned adapter,unsigned device_type,unsigned display_format_value,unsigned backbuffer_format,int windowed){
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL)return D9_ADAPTER_INVALID;
    if(!windowed)return D9_ADAPTER_UNAVAILABLE; /* no exclusive display-mode ownership */
    if(!display_format(display_format_value))return D9_ADAPTER_UNAVAILABLE;
    if(backbuffer_format&&backbuffer_format!=D9_X8R8G8B8&&backbuffer_format!=D9_A8R8G8B8)return D9_ADAPTER_UNAVAILABLE;
    return D9_ADAPTER_OK;
}
int d9_adapter_check_device_format(unsigned adapter,unsigned device_type,unsigned adapter_format,unsigned usage,unsigned resource_type,unsigned format){
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL)return D9_ADAPTER_INVALID;
    if(adapter_format!=D9_X8R8G8B8)return D9_ADAPTER_UNAVAILABLE;
    if(resource_type==RTYPE_SURFACE){
        if(usage!=0&&usage!=USAGE_RENDERTARGET)return D9_ADAPTER_UNAVAILABLE;
        if(format==D9_D24S8||format==D9_D24X8)return usage==0?D9_ADAPTER_OK:D9_ADAPTER_UNAVAILABLE;
        return format==D9_X8R8G8B8||format==D9_A8R8G8B8?D9_ADAPTER_OK:D9_ADAPTER_UNAVAILABLE;
    }
    if(resource_type!=RTYPE_TEXTURE)return D9_ADAPTER_UNAVAILABLE;
    /* The sampler really does point/linear filtering, wrap/mirror/clamp and
     * explicit mip chains; every other usage bit is a capability we do not have. */
    if(usage&~(USAGE_QUERY_FILTER|USAGE_QUERY_WRAPANDMIP))return D9_ADAPTER_UNAVAILABLE;
    return texture_format(format)?D9_ADAPTER_OK:D9_ADAPTER_UNAVAILABLE;
}
int d9_adapter_check_multisample(unsigned adapter,unsigned device_type,unsigned surface_format,int windowed,unsigned multisample_type){
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL)return D9_ADAPTER_INVALID;
    if(!windowed||multisample_type)return D9_ADAPTER_UNAVAILABLE; /* no fullscreen ownership or MSAA */
    return surface_format==D9_X8R8G8B8||surface_format==D9_A8R8G8B8?D9_ADAPTER_OK:D9_ADAPTER_UNAVAILABLE;
}
int d9_adapter_check_depth_stencil(unsigned adapter,unsigned device_type,unsigned display_format_value,unsigned backbuffer_format,unsigned depth_stencil_format){
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL)return D9_ADAPTER_INVALID;
    if(display_format_value!=D9_X8R8G8B8)return D9_ADAPTER_UNAVAILABLE;
    if(backbuffer_format!=D9_X8R8G8B8&&backbuffer_format!=D9_A8R8G8B8)return D9_ADAPTER_UNAVAILABLE;
    /* The single PICA DEPTH24_STENCIL8 attachment backs both formats. */
    return depth_stencil_format==D9_D24S8||depth_stencil_format==D9_D24X8?D9_ADAPTER_OK:D9_ADAPTER_UNAVAILABLE;
}
int d9_adapter_check_format_conversion(unsigned adapter,unsigned device_type,unsigned source_format,unsigned target_format){
    (void)source_format;(void)target_format;
    if(adapter>=D9_ADAPTER_COUNT||device_type!=DEVTYPE_HAL)return D9_ADAPTER_INVALID;
    return D9_ADAPTER_UNAVAILABLE; /* no format conversion */
}
