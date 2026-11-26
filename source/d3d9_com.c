#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "d3d9_bc.h"
#include "d3d9_shader.h"
#include "d3d9_pica_tev.h"
#include "d3d9_menu_closure.h"
#include "d3d9_menu_shader_runtime.h"
/* First-draw capture is an optional diagnostic.  It is compiled by default,
 * can be removed entirely with -DD9_CAPTURE_HOOK=0, and stays inert at run
 * time unless TESV_D3D9_CAPTURE_DRAWS requests one or more records. */
#ifndef D9_CAPTURE_HOOK
#define D9_CAPTURE_HOOK 1
#endif
#if D9_CAPTURE_HOOK
#include "d3d9_capture.h"
static void capture_present_event(void);
#endif
#include "win32_shims.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>

#define BASE 0x50000000u
#define THUNK 0xfe000000u
#define OBJ_BASE (BASE+0x2000)
#define LOCK_BASE 0x31000000u
#define LOCK_STRIDE 0x00400000u
#define MAX_OBJECTS 128u
#define MAX_SLOTS 119u
#define SURFACE_BUDGET (8u<<20)
#define OK 0u
#define INVALID 0x8876086cu
#define UNAVAILABLE 0x8876086au
#define NOTFOUND 0x88760866u
#define OUTMEM 0x8876017cu
#define NOINTERFACE 0x80004002u
#define NOAUTOGEN 0x0876086fu /* D3DOK_NOAUTOGEN: adapter is not WHQL certified */
#define FVF_XYZ_TEX1 0x102u
#define GFX_FVF_MASK 0x1ffu
#define USAGE_WRITEONLY 0x0008u
#define USAGE_DYNAMIC 0x0200u
#define USAGE_VB_MASK (USAGE_WRITEONLY|USAGE_DYNAMIC)
#define SURFACE_FORMAT(f) ((f)==D9_X8R8G8B8||(f)==D9_A8R8G8B8)
#define MAX_PALETTES 256u
#define PALETTE_BYTES 1024u /* 256 PALETTEENTRY (RGBA bytes) */
enum {FACTORY=1,DEVICE,VB,IB,TEX,VS,PS,DECL,SURFACE,CUBE,VOLUME,QUERY,STATEBLOCK,SWAPCHAIN,KIND_COUNT};
typedef struct {uint32_t x,y,width,height,minz,maxz;} D9Viewport9;
struct Obj;
typedef struct {
    unsigned type,captured;
    unsigned rs_count;uint32_t rs_ids[32],rs_values[32];
    unsigned sampler_count;uint8_t sampler_stage[16],sampler_id[16];uint32_t sampler_values[16];
    unsigned tss_count;uint8_t tss_stage[16],tss_id[16];uint32_t tss_values[16];
    struct Obj *tex[2];
    unsigned fvf;struct Obj *decl;
    struct Obj *streams[D9_MAX_STREAMS];uint32_t offsets[D9_MAX_STREAMS],strides[D9_MAX_STREAMS];
    struct Obj *ib;
    D9Matrix transforms[3];uint32_t viewport[6];
    D9TnlState tnl;uint32_t scissor[4];
} D9ComStateBlock;
typedef struct Obj {
    unsigned kind,refs,deferred;uint32_t guest;D9Handle handle;
    unsigned bytes,width,height,format,usage,pool,fvf,pitch,lockable;
    unsigned block_bytes; /* non-zero for compressed cube-surface views */
    uint32_t lock_guest;unsigned lock_bytes,readonly,lock_level,lock_face,lock_rows,lock_pitch,lock_row_bytes;void *lock_host;
    unsigned lock_slices,lock_slice_pitch,lock_front,volume_depth;
    unsigned lock_x,lock_y,lock_w,lock_h;
    /* IDirect3DQuery9 state.  The bridge is synchronous, so EVENT is the only
     * accepted type; query_type stores the D3DQUERYTYPE, query_issued is set
     * by Issue and never cleared by GetData. */
    unsigned query_type,query_issued;
    uint8_t *pixels;
    /* Explicit depth-stencil surface: a descriptor for the single PICA depth
     * attachment, not an LDR colour surface. */
    D9Handle depth;int is_depth;
    uint32_t *code;unsigned words;
    D9TevProgram tev;int has_tev;
    /* Exact first-menu selection (d3d9_menu_shader_runtime.h).  A menu plan is
     * still subject to draw-time preconditions and a named backend gate. */
    D9MenuShaderMatch menu;int has_menu,has_menu_vs,menu_backend_ok;
    char menu_backend_why[D9_MENU_SHADER_REASON_MAX];
    int probe_skip; /* TEST-ONLY shader-skip object: bytecode kept, no TEV/plan */
    D9Decl *declaration;
    D9ComStateBlock *state_block;
    /* Level surface view: pixels point into the parent texture's CPU store. */
    struct Obj *view_parent;D9Handle view_texture;unsigned view_level,view_face;int is_view,view_cube;
} Obj;
static struct {
    GuestMemory *mem;D9Device *device;const D9Backend *backend;void *backend_ctx;size_t backend_limit;int owns_device;
    Obj o[MAX_OBJECTS];unsigned next;
    Obj *factory,*dev,*back,*rt,*depth,*streams[D9_MAX_STREAMS],*ib,*tex,*tex1,*cube,*cube1,*volume,*volume1,*vs,*ps,*declaration,*recording_block,*swapchain;
    unsigned offsets[D9_MAX_STREAMS],strides[D9_MAX_STREAMS],phase,fvf,constants_mask,clear_pending,frame_clear;uint32_t clear,clear_flags,clear_z,clear_stencil;D9Handle clear_target;D9Matrix constants,constants_hi;float constants_c8[4];
    uint32_t clip_status_union,clip_status_intersection;
    int32_t cursor_hotspot_x,cursor_hotspot_y,cursor_x,cursor_y;
    uint32_t cursor_surface;unsigned cursor_shown;
    unsigned current_palette;uint8_t *palettes[MAX_PALETTES];
    uint32_t pp[14],creation[4],viewport[6],scissor[4];unsigned scissor_warned,gamma_warned;float npatch_segments;
    uint16_t gamma[3][256];size_t surface_bytes;
    unsigned frame_index,draw_index,capture_present;uint32_t capture_present_flags;
    char last_shader_rejection[320];
    char last_diagnostic[224];
    char last_stretch_rejection[192];
    void (*handlers[KIND_COUNT][MAX_SLOTS])(CPU *c);
    int trace;const char *trace_method;unsigned trace_kind,trace_slot;uint32_t trace_this;
    int hdr_ldr_fallback; /* measurement-only: store 0x71/0x72 RTs as A8R8G8B8 */int tnl_proto;
    int shader_skip_probe,shader_probe_logged; /* host-slice measurement-only */
} g;
static void upload_tev_constants(void);
static uint32_t present_common(CPU *c);
static uint32_t arg(CPU *c,unsigned n){return M32(c->r[4]+4+n*4);}
#define A(n) arg(c,n)
static void trace_result(uint32_t value);
static void trace_result_float(double value);
static void capture_stop(void);
#define RET(v,n) do{uint32_t retval=(uint32_t)(v);trace_result(retval);c->r[0]=retval;c->r[4]+=4+4*(n);return;}while(0)
static void d3d_log(const char *method,const char *reason);
static uint32_t hr(int rc){return rc==D9_OK?OK:rc==D9_MEMORY?OUTMEM:rc==D9_UNSUPPORTED?UNAVAILABLE:rc==D9_BACKEND||rc==D9_RELEASE_FAILED?UNAVAILABLE:INVALID;}
static void d3d_fail(const char *method,const char *why,uint32_t result){
    snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s%s%s",method?method:"D3D9",why?": ":"",why?why:"");
    d3d_log(method?method:"D3D9",why?why:"rejected");(void)result;
}
const char *d9_com_last_diagnostic(void){return g.last_diagnostic;}
/* IDirect3DDevice9 methods that return a float (GetNPatchMode) place it in
 * ST(0), not EAX; the guest pops it with FSTP.  Keep the return trace honest. */
#define RETF(v,n) do{double retval_f=(double)(v);fpush(c,retval_f);trace_result_float(retval_f);c->r[0]=0;c->r[4]+=4+4*(n);return;}while(0)
static void *ptr(uint32_t address,size_t n){return address&&g.mem?guest_ptr(g.mem,address,n):NULL;}
static void trace_detail(const char *fmt,...);
static void d3d_log(const char *method,const char *reason){fprintf(stderr,"[d3d9] %s: %s\n",method,reason);if(g.trace)trace_detail("diagnostic: %s",reason);}
static uint32_t stretch_reject(uint32_t result,const char *reason){
    snprintf(g.last_stretch_rejection,sizeof g.last_stretch_rejection,"%s",reason);
    d3d_log("StretchRect",reason);return result;
}
static const char *surface_format_name(unsigned format){
    return format==D9_X8R8G8B8?"X8R8G8B8":format==D9_A8R8G8B8?"A8R8G8B8":"unsupported";
}
static const char *interface_name(unsigned kind);
static uint32_t trace_arg(CPU *c,unsigned n){
    uint64_t address=(uint64_t)c->r[4]+4+(uint64_t)n*4;
    return address<=UINT32_MAX&&guest_ptr(g.mem,(uint32_t)address,4)?M32((uint32_t)address):0;
}
static void trace_detail(const char *fmt,...){
    if(!g.trace)return;
    va_list ap;va_start(ap,fmt);fprintf(stderr,"[d3d9-trace]   ");vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);
}
static void trace_enter(const char *method,unsigned kind,unsigned slot,CPU *c){
    g.trace_method=method;g.trace_kind=kind;g.trace_slot=slot;g.trace_this=g.trace?trace_arg(c,0):0;
    if(!g.trace)return;
    uint32_t caller=M32(c->r[4]);
    fprintf(stderr,"[d3d9-trace] %s::%s slot=%u this=0x%08lx caller=%08lx args=[0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx]\n",
        kind?interface_name(kind):"export",method,slot,(unsigned long)trace_arg(c,0),(unsigned long)caller,(unsigned long)trace_arg(c,1),(unsigned long)trace_arg(c,2),(unsigned long)trace_arg(c,3),
        (unsigned long)trace_arg(c,4),(unsigned long)trace_arg(c,5),(unsigned long)trace_arg(c,6),(unsigned long)trace_arg(c,7));
}
static void trace_result(uint32_t value){
    if(!g.trace)return;
    fprintf(stderr,"[d3d9-trace] %s::%s slot=%u this=0x%08lx -> 0x%08lx\n",
        g.trace_kind?interface_name(g.trace_kind):"export",g.trace_method?g.trace_method:"D3D9 export",g.trace_slot,
        (unsigned long)g.trace_this,(unsigned long)value);
}
static void trace_result_float(double value){
    if(!g.trace)return;
    fprintf(stderr,"[d3d9-trace] %s::%s slot=%u this=0x%08lx -> float %g (ST0)\n",
        g.trace_kind?interface_name(g.trace_kind):"export",g.trace_method?g.trace_method:"D3D9 export",g.trace_slot,
        (unsigned long)g.trace_this,value);
}
static uint32_t adapter_result(int rc){return rc==D9_ADAPTER_OK?OK:rc==D9_ADAPTER_INVALID?INVALID:UNAVAILABLE;}
static const char *interface_name(unsigned kind){
    static const char *names[]={"unknown object","IDirect3D9","IDirect3DDevice9","IDirect3DVertexBuffer9",
        "IDirect3DIndexBuffer9","IDirect3DTexture9","IDirect3DVertexShader9","IDirect3DPixelShader9",
        "IDirect3DVertexDeclaration9","IDirect3DSurface9","IDirect3DCubeTexture9",
        "IDirect3DVolumeTexture9","IDirect3DQuery9","IDirect3DStateBlock9","IDirect3DSwapChain9"};
    return kind<sizeof names/sizeof *names?names[kind]:"D3D9 object";
}
static Obj *obj(uint32_t addr,unsigned kind){
    if(addr<OBJ_BASE||(addr-OBJ_BASE)%16)return NULL;
    unsigned i=(addr-OBJ_BASE)/16;if(i>=g.next)return NULL;Obj *o=&g.o[i];
    return o->refs&&(!kind||o->kind==kind)?o:NULL;
}
static uint32_t table_addr(unsigned kind){return BASE+kind*0x200;}
static Obj *new_obj(unsigned kind){
    if(g.next>=MAX_OBJECTS)return NULL;
    unsigned i=g.next++;Obj *o=&g.o[i];memset(o,0,sizeof *o);o->kind=kind;o->refs=1;o->guest=OBJ_BASE+i*16;
    guest_write32(g.mem,o->guest,table_addr(kind));return o;
}
static void retain(Obj *o){if(o)++o->refs;}
static void release(Obj *o);
static void destroy_obj(Obj *o){
    Obj *parent=NULL;
    if(o->kind==VB||o->kind==IB){d9_release_buffer(g.device,o->handle);parent=g.dev;}
    else if(o->kind==TEX){
        int rc=d9_release_texture(g.device,o->handle);
        if(rc==D9_RELEASE_FAILED){o->refs=1;o->deferred=1;return;}
        parent=g.dev;
    }
    else if(o->kind==CUBE){
        int rc=d9_release_cube_texture(g.device,o->handle);
        if(rc==D9_RELEASE_FAILED){o->refs=1;o->deferred=1;return;}
        parent=g.dev;
    }
    else if(o->kind==VOLUME){
        int rc=d9_release_volume_texture(g.device,o->handle);
        if(rc==D9_RELEASE_FAILED){o->refs=1;o->deferred=1;return;}
        parent=g.dev;
    }
    else if(o->kind==VS||o->kind==PS){free(o->code);o->code=NULL;parent=g.dev;}
    else if(o->kind==DECL){free(o->declaration);o->declaration=NULL;parent=g.dev;}
    else if(o->kind==QUERY)parent=g.dev;
    else if(o->kind==SWAPCHAIN){if(g.swapchain==o)g.swapchain=NULL;parent=g.dev;}
    else if(o->kind==SURFACE){
        if(o->is_depth){
            if(g.depth==o){g.depth=NULL;if(g.device)(void)d9_set_depth_surface(g.device,0);}
            (void)d9_release_depth_surface(g.device,o->depth);
            o->depth=0;o->is_depth=0;parent=g.dev;
        }
        else if(o==g.back){g.back=NULL;g.rt=NULL;}
        else if(o->is_view){
            /* A level view owns no pixels and no target; it only holds a
             * reference to its parent texture. */
            o->is_view=0;o->pixels=NULL;
            parent=o->view_parent;o->view_parent=NULL;
        }
        else if(o->handle){
            if(g.rt==o)g.rt=g.back;
            (void)d9_release_render_target(g.device,o->handle);
            parent=g.dev;
        }else{free(o->pixels);o->pixels=NULL;g.surface_bytes-=o->bytes;parent=g.dev;}
    }
    else if(o->kind==STATEBLOCK){
        D9ComStateBlock *sb=o->state_block;
        if(g.recording_block==o)g.recording_block=NULL;
        if(sb){
            for(unsigned i=0;i<2;++i)release(sb->tex[i]);
            release(sb->decl);release(sb->ib);
            for(unsigned i=0;i<D9_MAX_STREAMS;++i)release(sb->streams[i]);
            free(sb);o->state_block=NULL;
        }
        parent=g.dev;
    }
    else if(o->kind==DEVICE){
        capture_stop();
        if(g.device)d9_set_pixel_shader(g.device,NULL);
        if(g.owns_device&&g.device){
            if(d9_destroy_device_ex(g.device)==D9_RELEASE_FAILED){o->refs=1;o->deferred=1;return;}
            g.device=NULL;g.owns_device=0;
        }
        Obj *back=g.back;g.back=NULL;g.rt=NULL;
        memset(g.streams,0,sizeof g.streams);memset(g.offsets,0,sizeof g.offsets);memset(g.strides,0,sizeof g.strides);
        g.ib=g.tex=g.tex1=g.cube=g.cube1=g.volume=g.volume1=g.vs=g.ps=g.declaration=NULL;
        g.dev=NULL;release(back);parent=g.factory;
    }
    else if(o->kind==FACTORY)g.factory=NULL;
    o->deferred=0;guest_write32(g.mem,o->guest,0);if(parent)release(parent);
}
static void release(Obj *o){if(!o||!o->refs)return;if(--o->refs)return;if(g.phase){o->deferred=1;return;}destroy_obj(o);}
static void collect(void){for(unsigned i=0;i<g.next;++i)if(g.o[i].deferred)destroy_obj(&g.o[i]);}
static void bind(Obj **slot,Obj *o){if(*slot==o)return;retain(o);Obj *old=*slot;*slot=o;release(old);}
static void unsupported(CPU *c){Obj *o=obj(A(0),0);
    trace_detail("unsupported: %s COM method is not implemented by this bridge",interface_name(o?o->kind:0));
    fprintf(stderr,"[d3d9] %s@0x%08lx: unsupported COM method\n",interface_name(o?o->kind:0),(unsigned long)A(0));
    rt_unimpl(c,A(0),"unsupported D3D9 COM method");}
static void direct_create(CPU *c){trace_enter("Direct3DCreate9",0,UINT_MAX,c);trace_detail("SDKVersion=%lu",(unsigned long)A(0));
    if(!g.mem||A(0)!=32)RET(0,1);
    if(g.factory){retain(g.factory);RET(g.factory->guest,1);}g.factory=new_obj(FACTORY);RET(g.factory?g.factory->guest:0,1);}
static void addref(CPU *c){Obj *o=obj(A(0),0);if(!o)RET(0,1);retain(o);RET(o->refs,1);}
static void release_fn(CPU *c){Obj *o=obj(A(0),0);if(!o)RET(0,1);
    if(o->refs==1&&o->lock_guest)rt_unimpl(c,A(0),"last Release while resource locked");
    unsigned n=o->refs-1;release(o);RET(n,1);}
/* Host-ABI entry points used by the D3DX texture shims.  They share the
 * validated COM resource path: the same usage/pool/layout checks, D9Device
 * texture creation and lock/upload/unlock sequencing. */
uint32_t d9_com_host_create_texture(uint32_t device,unsigned w,unsigned h,
                                    unsigned levels,unsigned usage,unsigned format,
                                    unsigned pool,uint32_t *out_texture){
    if(out_texture)*out_texture=0;
    if(!g.mem||!g.device||!out_texture||g.phase)return INVALID;
    if(!obj(device,DEVICE)){d3d_fail("CreateTexture","invalid device object",INVALID);return INVALID;}
    if(usage&~0x00220200u){d3d_fail("CreateTexture",
        "autogen, render-target, depth-stencil and non-DYNAMIC texture usages are not implemented",UNAVAILABLE);return UNAVAILABLE;}
    if(pool>2){d3d_fail("CreateTexture","invalid pool",UNAVAILABLE);return UNAVAILABLE;}
    size_t pitch,bytes;
    if(d9_texture_layout(w,h,format,&pitch,&bytes)){
        d3d_fail("CreateTexture",
            "texture format/dimensions are not implemented by the D3D9 bridge",INVALID);return INVALID;}
    if(g.next>=MAX_OBJECTS)return OUTMEM;
    D9Handle handle;int rc=d9_create_texture_ex(g.device,w,h,format,levels,pool,usage,0,&handle);
    if(rc){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
            "CreateTexture: %s",d9_last_draw_diagnostic(g.device));
        return hr(rc);
    }
    Obj *o=new_obj(TEX);
    if(!o){d9_release_texture(g.device,handle);return OUTMEM;}
    o->handle=handle;o->width=w;o->height=h;o->bytes=(unsigned)bytes;o->pitch=(unsigned)pitch;
    o->format=format;o->usage=usage;o->pool=pool;retain(g.dev);
    *out_texture=o->guest;g.last_diagnostic[0]=0;return OK;
}
uint32_t d9_com_host_upload_texture_level(uint32_t texture,unsigned level,
                                          const void *src,size_t src_pitch,
                                          size_t src_bytes){
    Obj *o=obj(texture,TEX);
    if(!o||!g.device||!src||!src_pitch||g.phase)return INVALID;
    unsigned w,h;size_t pitch,bytes;
    if(d9_texture_level_info(g.device,o->handle,level,&w,&h,&pitch,&bytes))return INVALID;
    unsigned bb=d9_bc_block_bytes(o->format);
    size_t rows=bb?(size_t)((h+3u)/4u):(size_t)h;
    size_t row=bb?(size_t)((w+3u)/4u)*bb:
               (size_t)w*(o->format==D9_A8?1u:4u);
    if(!rows||src_pitch<row)return INVALID;
    if(src_bytes&&((rows-1u)>((SIZE_MAX-row)/src_pitch)||
       (rows-1u)*src_pitch+row>src_bytes))return INVALID;
    if((rows-1u)>(SIZE_MAX-row)/pitch||(rows-1u)*pitch+row>bytes)return INVALID;
    void *dst=NULL;size_t unlocked_pitch=0;
    int rc=d9_texture_lock_level_ex(g.device,o->handle,level,&dst,&unlocked_pitch,0);
    if(rc)return hr(rc);
    for(size_t y=0;y<rows;++y)
        memcpy((uint8_t *)dst+y*pitch,(const uint8_t *)src+y*src_pitch,row);
    rc=d9_texture_unlock_level(g.device,o->handle,level);
    if(rc)return hr(rc);
    g.last_diagnostic[0]=0;return OK;
}
uint32_t d9_com_host_release(uint32_t object){
    Obj *o=obj(object,0);
    if(!o)return INVALID;
    release(o);return OK;
}
/* IUnknown, requested concrete interface, plus resource/base-texture ancestors. */
static const uint8_t guids[][16]={
    {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},
    {0xca,0xcb,0xbd,0x81,0xd4,0x64,0x6d,0x42,0xae,0x8d,0xad,1,0x47,0xf4,0x27,0x5c},
    {0x96,0x3b,0x22,0xd0,0x7a,0xbf,0xfd,0x43,0x92,0xbd,0xa4,0x3b,0x0d,0x82,0xb9,0xeb},
    {0xb5,0xb1,0x4b,0xb6,0x70,0xfd,0xf6,0x4d,0xbf,0x91,0x19,0xd0,0xa1,0x24,0x55,0xe3},
    {0x5e,0xd6,0x9d,0x7c,0xf7,0xd3,0x29,0x45,0xac,0xee,0x78,0x58,0x30,0xac,0xde,0x35},
    {0x27,0x12,0xc3,0x85,0xe5,0x3d,0x00,0x4f,0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5},
    {0x7e,0x55,0xc5,0xef,0x65,0x62,0x13,0x46,0x8a,0x94,0x43,0x85,0x78,0x89,0xeb,0x36},
    {0xdc,0xdb,0x3b,0x6d,0x02,0x5b,0x15,0x44,0xb8,0x52,0xce,0x5e,0x8b,0xcc,0xb2,0x89},
    {0x9c,0xc5,0x13,0xdd,0xfa,0x36,0x98,0x40,0xa8,0xfb,0xc7,0xed,0x39,0xdc,0x85,0x46},
    {0x3a,0xaf,0xfb,0x0c,0xf6,0x99,0x4a,0x29,0x99,0xb3,0xa2,0x79,0x6a,0xf8,0xb8,0x9b},
    {0x5d,0xc0,0xee,0x05,0x7d,0x8f,0x62,0x43,0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,4},
    {0x7e,0xa8,0x0c,0x58,0x3c,0x1d,0x54,0x4d,0x99,0x1d,0xb7,0xd3,0xe3,0xc2,0x98,0xce},
    {0x81,0x2f,0xf3,0xff,0x53,0xd9,0x3a,0x47,0x92,0x23,0x93,0xd6,0x52,0xab,0xa9,0x3f},
    {0x6c,0x52,0x18,0x25,0x89,0xe7,0x11,0x41,0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6},
    {0x60,0x14,0x77,0xd9,0x95,0xa6,0x26,0x4f,0xbb,0xd3,0x27,0xb8,0x40,0xb5,0x41,0xcc},
    {0xe5,0x4f,0x7c,0xb0,0x0d,0x31,0xa8,0x4b,0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b},
    {0xf2,0x50,0x49,0x79,0xfc,0xad,0x8a,0x45,0x90,0x5e,0x10,0xa1,0x0b,0x0b,0x50,0x3b}};
static void query(CPU *c){Obj *o=obj(A(0),0);const void *iid=ptr(A(1),16);uint32_t out=A(2);if(!o||!iid||!ptr(out,4))RET(INVALID,3);
    const uint8_t *concrete=o->kind==CUBE?guids[12]:o->kind==VOLUME?guids[13]:
        o->kind==QUERY?guids[14]:o->kind==STATEBLOCK?guids[15]:o->kind==SWAPCHAIN?guids[16]:guids[o->kind];
    M32(out)=0;if(memcmp(iid,guids[0],16)&&memcmp(iid,concrete,16)&&
        !((o->kind>=VB&&o->kind<=TEX||o->kind==SURFACE||o->kind==CUBE||o->kind==VOLUME)&&!memcmp(iid,guids[10],16))&&
        !((o->kind==TEX||o->kind==CUBE||o->kind==VOLUME)&&!memcmp(iid,guids[11],16)))RET(NOINTERFACE,3);
    retain(o);M32(out)=o->guest;RET(OK,3);}
static void adapter_count(CPU *c){
    if(!obj(A(0),FACTORY)){d3d_log("GetAdapterCount","invalid factory object");RET(0,1);}
    RET(D9_ADAPTER_COUNT,1);
}
static void display_mode(CPU *c){
    uint32_t out=A(2);
    if(!obj(A(0),FACTORY)||!ptr(out,sizeof(D9DisplayMode))){d3d_log("GetAdapterDisplayMode","invalid factory or output buffer");RET(INVALID,3);}
    D9DisplayMode mode;
    if(d9_adapter_display_mode(A(1),D9_X8R8G8B8,0,&mode)){
        d3d_log("GetAdapterDisplayMode",A(1)>=D9_ADAPTER_COUNT?"adapter does not exist":"display mode is unavailable");RET(INVALID,3);}
    memcpy(ptr(out,sizeof mode),&mode,sizeof mode);RET(OK,3);
}
static void adapter_identifier(CPU *c){
    uint32_t flags=A(2),out=A(3);
    if(!obj(A(0),FACTORY)||!ptr(out,sizeof(D9AdapterIdentifier))){d3d_log("GetAdapterIdentifier","invalid factory or output buffer");RET(INVALID,4);}
    if(flags&~2u){d3d_log("GetAdapterIdentifier","only D3DENUM_WHQL_LEVEL is recognized");RET(INVALID,4);}
    D9AdapterIdentifier id;
    if(d9_adapter_identifier(A(1),&id)){
        d3d_log("GetAdapterIdentifier",A(1)>=D9_ADAPTER_COUNT?"adapter does not exist":"adapter identifier is unavailable");RET(INVALID,4);}
    memcpy(ptr(out,sizeof id),&id,sizeof id);
    RET(flags&2u?NOAUTOGEN:OK,4);
}
static void adapter_mode_count(CPU *c){
    if(!obj(A(0),FACTORY)){d3d_log("GetAdapterModeCount","invalid factory object");RET(0,3);}
    unsigned count=d9_adapter_mode_count(A(1),A(2));
    if(!count)d3d_log("GetAdapterModeCount",A(1)>=D9_ADAPTER_COUNT?"adapter does not exist":"only X8R8G8B8 has a display mode");
    RET(count,3);
}
static void enum_modes(CPU *c){
    uint32_t out=A(4);
    if(!obj(A(0),FACTORY)||!ptr(out,sizeof(D9DisplayMode))){d3d_log("EnumAdapterModes","invalid factory or output buffer");RET(INVALID,5);}
    D9DisplayMode mode;
    if(d9_adapter_display_mode(A(1),A(2),A(3),&mode)){
        const char *why=A(1)>=D9_ADAPTER_COUNT?"adapter does not exist":
            A(2)!=D9_X8R8G8B8?"format is not the X8R8G8B8 display format":"mode index is out of range";
        d3d_log("EnumAdapterModes",why);RET(INVALID,5);
    }
    memcpy(ptr(out,sizeof mode),&mode,sizeof mode);RET(OK,5);
}
static void check_device_type(CPU *c){
    int rc=d9_adapter_check_device_type(A(1),A(2),A(3),A(4),A(5)!=0);
    if(rc){
        const char *why=(A(1)>=D9_ADAPTER_COUNT||A(2)!=1)?"invalid adapter or device type":
            !A(5)?"exclusive fullscreen/display-mode ownership is not implemented":
            A(3)!=D9_X8R8G8B8?"display format is not the single 400x240 X8R8G8B8 mode":
            "back-buffer format is not X8R8G8B8, A8R8G8B8 or UNKNOWN";
        d3d_log("CheckDeviceType",why);RET(adapter_result(rc),6);
    }
    RET(OK,6);
}
static void check_format(CPU *c){
    int rc=d9_adapter_check_device_format(A(1),A(2),A(3),A(4),A(5),A(6));
    if(rc){
        const char *why=(A(1)>=D9_ADAPTER_COUNT||A(2)!=1)?"invalid adapter or device type":
            A(3)!=D9_X8R8G8B8?"adapter format is not X8R8G8B8":
            A(4)&~0x00220001u?"usage flags other than RENDERTARGET or QUERY_FILTER/QUERY_WRAPANDMIP are not implemented":
            (A(5)!=3&&A(5)!=1)?"only D3DRTYPE_TEXTURE and plain D3DRTYPE_SURFACE are implemented":
            A(5)==1&&A(4)&&A(4)!=1?"only usage zero or D3DUSAGE_RENDERTARGET is implemented for surfaces":
            A(5)==1?"plain/render-target surfaces require a 32-bit X8R8G8B8/A8R8G8B8 format":
            A(4)==1?"render-target textures are available only as X8R8G8B8/A8R8G8B8":
            "format is not A8R8G8B8, X8R8G8B8, DXT1, DXT3 or DXT5";
        d3d_log("CheckDeviceFormat",why);RET(adapter_result(rc),7);
    }
    RET(OK,7);
}
static void check_multisample(CPU *c){
    uint32_t out=A(6);
    if(!obj(A(0),FACTORY)||(out&&!ptr(out,4))){d3d_log("CheckDeviceMultiSampleType","invalid factory or quality-levels pointer");RET(INVALID,7);}
    int rc=d9_adapter_check_multisample(A(1),A(2),A(3),A(4)!=0,A(5));
    if(rc){
        const char *why=(A(1)>=D9_ADAPTER_COUNT||A(2)!=1)?"invalid adapter or device type":
            !A(4)?"exclusive fullscreen/display-mode ownership is not implemented":
            A(5)?"only D3DMULTISAMPLE_NONE is implemented":
            "surface format is not a supported back-buffer format";
        d3d_log("CheckDeviceMultiSampleType",why);RET(adapter_result(rc),7);
    }
    if(out)M32(out)=1;
    RET(OK,7);
}
static void check_depth_stencil(CPU *c){
    int rc=d9_adapter_check_depth_stencil(A(1),A(2),A(3),A(4),A(5));
    if(rc){
        const char *why=(A(1)>=D9_ADAPTER_COUNT||A(2)!=1)?"invalid adapter or device type":
            "D3D9 depth-stencil surfaces are not implemented";
        d3d_log("CheckDepthStencilMatch",why);RET(adapter_result(rc),6);
    }
    RET(OK,6);
}
static void check_format_conversion(CPU *c){
    int rc=d9_adapter_check_format_conversion(A(1),A(2),A(3),A(4));
    if(rc){
        const char *why=(A(1)>=D9_ADAPTER_COUNT||A(2)!=1)?"invalid adapter or device type":
            "surface format conversion is not implemented";
        d3d_log("CheckDeviceFormatConversion",why);RET(adapter_result(rc),5);
    }
    RET(OK,5);
}
static void register_software(CPU *c){
    if(!obj(A(0),FACTORY)){d3d_log("RegisterSoftwareDevice","invalid factory object");RET(INVALID,2);}
    d3d_log("RegisterSoftwareDevice","software rasterizer plugins are not implemented");RET(UNAVAILABLE,2);
}
static void adapter_monitor(CPU *c){
    d3d_log("GetAdapterMonitor","the 3DS has no desktop monitor; returning NULL");
    RET(0,2);
}
static int perf_logged;
static void perf_note(const char *method){
    (void)method;if(perf_logged)return;perf_logged=1;
    trace_detail("no profiler is attached; performance marker accepted as a no-op");
}
static void d3dperf_set_options(CPU *c){trace_enter("D3DPERF_SetOptions",0,UINT_MAX,c);perf_note("D3DPERF_SetOptions");RET(0,1);}
static void d3dperf_begin(CPU *c){trace_enter("D3DPERF_BeginEvent",0,UINT_MAX,c);perf_note("D3DPERF_BeginEvent");RET(0,2);}
static void d3dperf_end(CPU *c){trace_enter("D3DPERF_EndEvent",0,UINT_MAX,c);perf_note("D3DPERF_EndEvent");RET(0,0);}
static void d3dperf_status(CPU *c){trace_enter("D3DPERF_GetStatus",0,UINT_MAX,c);perf_note("D3DPERF_GetStatus");RET(0,0);}
static void backend_stage_caps(D9Caps9 *caps){
    unsigned count=g.device?d9_stage_count(g.device):(g.backend&&g.backend->draw_two?2u:1u);
    caps->max_texture_blend_stages=count;
    caps->max_simultaneous_textures=count;
}
static void factory_caps(CPU *c){
    uint32_t out=A(3);
    if(!obj(A(0),FACTORY)||!ptr(out,sizeof(D9Caps9))){d3d_log("GetDeviceCaps","invalid factory or caps buffer");RET(INVALID,4);}
    D9Caps9 caps;
    if(d9_adapter_caps(A(1),A(2),&caps)){
        d3d_log("GetDeviceCaps",A(1)>=D9_ADAPTER_COUNT?"adapter does not exist":"device type is not D3DDEVTYPE_HAL");
        RET(INVALID,4);
    }
    backend_stage_caps(&caps);
    memcpy(ptr(out,sizeof caps),&caps,sizeof caps);RET(OK,4);
}
static void device_caps(CPU *c){
    uint32_t out=A(1);
    if(!obj(A(0),DEVICE)||!ptr(out,sizeof(D9Caps9))){d3d_log("GetDeviceCaps","invalid device or caps buffer");RET(INVALID,2);}
    D9Caps9 caps;
    if(d9_adapter_caps(0,1,&caps)){d3d_log("GetDeviceCaps","adapter caps are unavailable");RET(INVALID,2);}
    backend_stage_caps(&caps);
    memcpy(ptr(out,sizeof caps),&caps,sizeof caps);RET(OK,2);
}
static const char *presentation_reason(const uint32_t *pp){
    unsigned w=pp[0],h=pp[1],fmt=pp[2],count=pp[3],ms=pp[4],quality=pp[5],swap=pp[6];
    if(!((w==D9_DISPLAY_WIDTH&&h==D9_DISPLAY_HEIGHT)||(w==0&&h==0)))return "back buffer is not the 400x240 display mode";
    if(fmt&&!SURFACE_FORMAT(fmt))return "back-buffer format is not 32-bit X8R8G8B8/A8R8G8B8";
    if(count>1)return "more than one back buffer is not implemented";
    if(ms)return "multisampling is not implemented";
    if(quality)return "non-zero multisample quality is not implemented";
    if(swap!=1)return "only D3DSWAPEFFECT_DISCARD is implemented";
    if(!pp[8])return "only the single windowed display mode is implemented";
    /* Windowed guests in the existing slice carry a legacy refresh value. It
     * does not select a display mode here, so accept and ignore it. */
    if(pp[9])return "automatic depth-stencil surfaces are not implemented";
    if(pp[11]&~1u)return "presentation flags other than LOCKABLE_BACKBUFFER are not implemented";
    if((pp[11]&1u)&&!d9_backend_supports_lockable_back_buffer(g.backend))
        return "the backend cannot retain, read back, write back and present a lockable back buffer";
    if(pp[13]>1)return "only immediate or one-frame presentation intervals are implemented";
    return NULL;
}
static void presentation_store(const uint32_t *pp){
    memcpy(g.pp,pp,14*sizeof *pp);
    if(!g.pp[0])g.pp[0]=D9_DISPLAY_WIDTH;
    if(!g.pp[1])g.pp[1]=D9_DISPLAY_HEIGHT;
    if(!g.pp[2])g.pp[2]=D9_X8R8G8B8;
    if(!g.pp[3])g.pp[3]=1;
}
static void free_palettes(void){
    for(unsigned i=0;i<MAX_PALETTES;++i){free(g.palettes[i]);g.palettes[i]=NULL;}
}
static void default_gamma(void){
    /* D3DGAMMARAMP is 3 x 256 WORD; i * 0x0101 is the identity ramp. */
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<256;++i)
        g.gamma[c][i]=(uint16_t)(i*0x0101u);
}
static int gamma_identity(const uint16_t *ramp){
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<256;++i)
        if(ramp[c*256+i]!=(uint16_t)(i*0x0101u))return 0;
    return 1;
}
static void default_viewport(void){
    unsigned width=g.rt?g.rt->width:g.pp[0],height=g.rt?g.rt->height:g.pp[1];
    g.viewport[0]=g.viewport[1]=0;g.viewport[2]=width;g.viewport[3]=height;
    g.viewport[4]=0;g.viewport[5]=0x3f800000u;
    if(g.device)(void)d9_set_viewport_size(g.device,width,height);
    /* D3D9 resets the scissor rectangle to the whole render target whenever the
     * device or the bound target changes.  The scissor test stays off. */
    g.scissor[0]=g.scissor[1]=0;g.scissor[2]=width;g.scissor[3]=height;
}
static void create_device(CPU *c){uint32_t pp=A(5),out=A(6);
    if(!obj(A(0),FACTORY)||!ptr(out,4)||!ptr(pp,56)){d3d_log("CreateDevice","invalid factory, presentation parameters or output pointer");RET(INVALID,7);}
    M32(out)=0;
    if(A(1)>=D9_ADAPTER_COUNT||A(2)!=1){d3d_log("CreateDevice","invalid adapter or device type");RET(INVALID,7);}
    trace_detail("Adapter=%lu DeviceType=%lu FocusWindow=0x%08lx BehaviorFlags=0x%08lx BackBuffer=%lux%lu Format=%lu",
        (unsigned long)A(1),(unsigned long)A(2),(unsigned long)A(3),(unsigned long)A(4),
        (unsigned long)M32(pp),(unsigned long)M32(pp+4),(unsigned long)M32(pp+8));
    const char *why=g.dev?"a device already exists":presentation_reason(ptr(pp,56));
    if(why){d3d_log("CreateDevice",why);RET(UNAVAILABLE,7);}
    if(!g.device){
        if(!g.backend){d3d_log("CreateDevice","no GPU backend is registered; this is an enumeration-only session");RET(UNAVAILABLE,7);}
        if(!g.backend_limit){d3d_log("CreateDevice","no d3d9_core resource budget is configured for the backend");RET(UNAVAILABLE,7);}
        g.device=d9_create_device(g.backend,g.backend_ctx,g.backend_limit);
        if(!g.device){d3d_log("CreateDevice","d3d9_core could not create the device from the backend");RET(UNAVAILABLE,7);}
        g.owns_device=1;
    }
    (void)d9_set_tnl_proto(g.device,g.tnl_proto);
    presentation_store(ptr(pp,56));
    int bbrc=d9_set_back_buffer_info_ex(g.device,g.pp[0],g.pp[1],g.pp[2],g.pp[11]&1u);
    if(bbrc){
        if(g.owns_device){d9_destroy_device(g.device);g.device=NULL;g.owns_device=0;}
        d3d_log("CreateDevice",bbrc==D9_MEMORY?
            "the retained lockable back buffer does not fit the device memory budget":
            bbrc==D9_UNSUPPORTED?
            "the backend cannot allocate the retained lockable back buffer":
            "back-buffer presentation metadata was rejected");
        RET(bbrc==D9_MEMORY?OUTMEM:UNAVAILABLE,7);
    }
    g.dev=new_obj(DEVICE);
    if(!g.dev){
        if(g.owns_device){d9_destroy_device(g.device);g.device=NULL;g.owns_device=0;}
        d3d_log("CreateDevice","COM object table is full");RET(OUTMEM,7);
    }
    retain(g.factory);
    default_viewport();default_gamma();
    g.creation[0]=A(1);g.creation[1]=A(2);g.creation[2]=A(3);g.creation[3]=A(4);
    g.back=new_obj(SURFACE);
    if(!g.back){
        release(g.dev);d3d_log("CreateDevice","COM object table is full");RET(OUTMEM,7);
    }
    g.back->width=g.pp[0];g.back->height=g.pp[1];g.back->format=g.pp[2];g.back->usage=1;g.back->pool=0;
    g.back->handle=d9_back_buffer_target(g.device);
    g.back->lockable=g.back->handle?1:0;
    g.back->pitch=g.back->width*4;g.back->bytes=g.back->width*g.back->height*4;
    if(g.back->handle)
        trace_detail("lockable back buffer: retained target handle=%lu logical %lux%lu",
            (unsigned long)g.back->handle,(unsigned long)g.back->width,(unsigned long)g.back->height);
    g.rt=g.back;g.fvf=g.constants_mask=g.clear_pending=g.frame_clear=0;g.clear=0xff000000u;g.clear_flags=1;g.clear_target=0;
    (void)d9_set_render_target(g.device,0);
    /* A new device starts with an unarmed, empty menu closure observation and
     * no material-precondition evidence. */
    d9_menu_closure_reset();
    d9_menu_shader_runtime_reset();
    memset(&g.constants_c8,0,sizeof g.constants_c8);upload_tev_constants();
    M32(out)=g.dev->guest;RET(OK,7);}
static int explicit_resources(void){
    for(unsigned i=0;i<g.next;++i){Obj *o=&g.o[i];if(o->refs&&o->kind!=FACTORY&&o->kind!=DEVICE&&o->kind!=SWAPCHAIN&&!(o->kind==SURFACE&&o==g.back))return 1;}
    return 0;
}
static void reset_device(CPU *c){uint32_t pp=A(1);
    if(!obj(A(0),DEVICE)||!ptr(pp,56)){d3d_log("Reset","invalid device or presentation parameters");RET(INVALID,2);}
    if(g.phase){d3d_log("Reset","Present must complete the active scene before Reset");RET(INVALID,2);}
    if(!g.owns_device){d3d_log("Reset","state reset is unavailable for a caller-adopted D9Device");RET(UNAVAILABLE,2);}
    const char *why=presentation_reason(ptr(pp,56));
    if(why){d3d_log("Reset",why);RET(UNAVAILABLE,2);}
    if(explicit_resources()){d3d_log("Reset","release all explicit resources before Reset");RET(INVALID,2);}
    trace_detail("BackBuffer=%lux%lu Format=%lu",(unsigned long)M32(pp),(unsigned long)M32(pp+4),(unsigned long)M32(pp+8));
    D9Device *replacement=d9_create_device(g.backend,g.backend_ctx,g.backend_limit);
    if(!replacement){d3d_log("Reset","could not allocate replacement device state");RET(OUTMEM,2);}
    d9_destroy_device(g.device);g.device=replacement;
    (void)d9_set_tnl_proto(g.device,g.tnl_proto);
    presentation_store(ptr(pp,56));
    int bbrc=d9_set_back_buffer_info_ex(g.device,g.pp[0],g.pp[1],g.pp[2],g.pp[11]&1u);
    if(bbrc){
        g.back->handle=0;g.back->lockable=0;
        d3d_log("Reset",bbrc==D9_MEMORY?
            "the retained lockable back buffer does not fit the device memory budget":
            bbrc==D9_UNSUPPORTED?
            "the backend cannot allocate the retained lockable back buffer":
            "back-buffer presentation metadata was rejected");
        RET(bbrc==D9_MEMORY?OUTMEM:UNAVAILABLE,2);
    }
    g.back->width=g.pp[0];g.back->height=g.pp[1];g.back->format=g.pp[2];
    g.back->handle=d9_back_buffer_target(g.device);g.back->lockable=g.back->handle?1:0;
    free_palettes();g.current_palette=0;
    g.cursor_hotspot_x=g.cursor_hotspot_y=g.cursor_x=g.cursor_y=0;g.cursor_surface=0;g.cursor_shown=0;
    g.fvf=g.constants_mask=g.clear_pending=g.frame_clear=0;g.clear=0xff000000u;g.clear_flags=1;g.clear_target=0;g.clear_z=g.clear_stencil=0;g.depth=NULL;memset(&g.constants,0,sizeof g.constants);memset(&g.constants_hi,0,sizeof g.constants_hi);memset(&g.constants_c8,0,sizeof g.constants_c8);g.clip_status_union=g.clip_status_intersection=0;
    g.rt=g.back;(void)d9_set_render_target(g.device,0);default_viewport();default_gamma();
    /* Reset also discards every closure observation and material evidence. */
    d9_menu_closure_reset();
    d9_menu_shader_runtime_reset();
    upload_tev_constants();
#if D9_CAPTURE_HOOK
    g.frame_index=g.draw_index=0;g.capture_present=0;g.capture_present_flags=0;
#endif
    M32(pp)=M32(pp+4)=M32(pp+12)=0;M32(pp+8)=0; /* D3D9 Reset writes back UNKNOWN/zero dimensions. */
    RET(OK,2);
}
static void device_display_mode(CPU *c){uint32_t out=A(2);
    if(!obj(A(0),DEVICE)||A(1)||!ptr(out,sizeof(D9DisplayMode))){d3d_log("GetDisplayMode","invalid device, swap chain or output buffer");RET(INVALID,3);}
    D9DisplayMode mode;if(d9_adapter_display_mode(0,D9_X8R8G8B8,0,&mode))RET(UNAVAILABLE,3);
    memcpy(ptr(out,sizeof mode),&mode,sizeof mode);RET(OK,3);
}
static void creation_parameters(CPU *c){uint32_t out=A(1);
    if(!obj(A(0),DEVICE)||!ptr(out,16))RET(INVALID,2);
    memcpy(ptr(out,16),g.creation,sizeof g.creation);RET(OK,2);
}
static void get_back_buffer(CPU *c){uint32_t out=A(4);if(!ptr(out,4))RET(INVALID,5);M32(out)=0;
    if(!obj(A(0),DEVICE)||A(1)||A(2)||A(3))RET(INVALID,5);
    retain(g.back);M32(out)=g.back->guest;RET(OK,5);
}
static void number_swap_chains(CPU *c){RET(obj(A(0),DEVICE)?1:0,1);}
/* ---- implicit swap chain ------------------------------------------------ */
static void create_additional_swap_chain(CPU *c){
    uint32_t pp=A(1),out=A(2);
    if(!obj(A(0),DEVICE)||!ptr(pp,56)||!ptr(out,4)){
        d3d_log("CreateAdditionalSwapChain","invalid device, presentation parameters or output pointer");
        RET(INVALID,3);
    }
    M32(out)=0;
    d3d_fail("CreateAdditionalSwapChain","second swap chains are not implemented",UNAVAILABLE);
    RET(UNAVAILABLE,3);
}
static void get_swap_chain(CPU *c){
    uint32_t out=A(2);
    if(!obj(A(0),DEVICE)||!ptr(out,4))RET(INVALID,3);
    M32(out)=0;
    if(A(1)){
        d3d_fail("GetSwapChain","only the implicit swap chain 0 exists",INVALID);RET(INVALID,3);
    }
    if(!g.dev||!g.device){
        d3d_fail("GetSwapChain","there is no created device",UNAVAILABLE);RET(UNAVAILABLE,3);
    }
    if(!g.swapchain){
        if(g.next>=MAX_OBJECTS)RET(OUTMEM,3);
        Obj *o=new_obj(SWAPCHAIN);
        if(!o)RET(OUTMEM,3);
        g.swapchain=o;retain(g.dev);
        M32(out)=o->guest;RET(OK,3);
    }
    retain(g.swapchain);M32(out)=g.swapchain->guest;RET(OK,3);
}
static void swapchain_present(CPU *c){
    if(!obj(A(0),SWAPCHAIN)||g.swapchain!=obj(A(0),SWAPCHAIN)||!g.dev)RET(INVALID,5);
    RET(present_common(c),5);
}
static void swapchain_get_back_buffer(CPU *c){
    uint32_t out=A(3);
    if(!ptr(out,4))RET(INVALID,4);M32(out)=0;
    if(!obj(A(0),SWAPCHAIN)||A(1)||A(2))RET(INVALID,4);
    retain(g.back);M32(out)=g.back->guest;RET(OK,4);
}
static void swapchain_raster_status(CPU *c){
    uint32_t out=A(1);
    if(!obj(A(0),SWAPCHAIN)||!ptr(out,8))RET(INVALID,2);
    M32(out)=0;M32(out+4)=0;
    d3d_fail("GetRasterStatus","PICA has no scanline/vblank raster status",UNAVAILABLE);
    RET(UNAVAILABLE,2);
}
static void swapchain_display_mode(CPU *c){
    uint32_t out=A(1);
    if(!obj(A(0),SWAPCHAIN)||!ptr(out,sizeof(D9DisplayMode)))RET(INVALID,2);
    D9DisplayMode mode;
    if(d9_adapter_display_mode(0,D9_X8R8G8B8,0,&mode))RET(UNAVAILABLE,2);
    memcpy(ptr(out,sizeof mode),&mode,sizeof mode);RET(OK,2);
}
static void swapchain_get_device(CPU *c){
    if(!obj(A(0),SWAPCHAIN)||!ptr(A(1),4))RET(INVALID,2);
    retain(g.dev);M32(A(1))=g.dev?g.dev->guest:0;RET(OK,2);
}
static void swapchain_present_parameters(CPU *c){
    if(!obj(A(0),SWAPCHAIN)||!ptr(A(1),56))RET(INVALID,2);
    memcpy(ptr(A(1),56),g.pp,sizeof g.pp);RET(OK,2);
}
static void create_render_target(CPU *c){uint32_t out=A(7);if(!ptr(out,4))RET(INVALID,9);M32(out)=0;
    if(!obj(A(0),DEVICE)||g.phase)RET(INVALID,9);
    if(A(8)){d3d_log("CreateRenderTarget","shared handles are not supported");RET(INVALID,9);}
    if(A(4)||A(5)){d3d_log("CreateRenderTarget","only D3DMULTISAMPLE_NONE with quality zero is implemented");RET(UNAVAILABLE,9);}
    if(A(1)<8||A(2)<8||A(1)>1024||A(2)>1024||(A(1)&(A(1)-1))||(A(2)&(A(2)-1))){
        d3d_log("CreateRenderTarget","only power-of-two dimensions from 8 through 1024 are implemented");RET(INVALID,9);}
    if(!SURFACE_FORMAT(A(3))){d3d_log("CreateRenderTarget","only 32-bit X8R8G8B8/A8R8G8B8 targets are implemented");RET(UNAVAILABLE,9);}
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,9);
    D9Handle h=0;int rc=d9_create_render_target(g.device,A(1),A(2),A(3),A(6)!=0,&h);
    if(rc){
        const char *why=d9_last_draw_diagnostic(g.device);
        if((rc==D9_UNSUPPORTED||rc==D9_BACKEND)&&why&&*why)d3d_fail("CreateRenderTarget",why,hr(rc));
        else d3d_log("CreateRenderTarget",rc==D9_MEMORY?"render-target memory budget exceeded":rc==D9_BACKEND?"backend render-target allocation failed":"backend cannot create the requested target (including lockable without readback)");
        RET(rc==D9_INVALID?UNAVAILABLE:hr(rc),9);
    }
    Obj *o=new_obj(SURFACE);if(!o){d9_release_render_target(g.device,h);RET(OUTMEM,9);}
    o->handle=h;o->width=A(1);o->height=A(2);o->format=A(3);o->usage=1;o->pool=0;o->pitch=A(1)*4;o->bytes=A(1)*A(2)*4;o->lockable=A(6)!=0;
    retain(g.dev);M32(out)=o->guest;
    trace_detail("RenderTarget=0x%08lx %lux%lu Format=%lu Lockable=%lu",(unsigned long)o->guest,
        (unsigned long)o->width,(unsigned long)o->height,(unsigned long)o->format,(unsigned long)A(6));
    RET(OK,9);
}
static void create_depth_stencil(CPU *c){uint32_t out=A(7);if(!ptr(out,4))RET(INVALID,9);M32(out)=0;
    if(!obj(A(0),DEVICE)||g.phase)RET(INVALID,9);
    if(A(8)){d3d_log("CreateDepthStencilSurface","shared handles are not supported");RET(INVALID,9);}
    if(A(4)||A(5)){d3d_log("CreateDepthStencilSurface","only D3DMULTISAMPLE_NONE with quality zero is implemented");RET(UNAVAILABLE,9);}
    if(A(6)){d3d_log("CreateDepthStencilSurface","lockable depth-stencil surfaces are not implemented");RET(UNAVAILABLE,9);}
    /* A descriptor of the target's single DEPTH24_STENCIL8 attachment; no VRAM
     * is allocated here.  Binding checks the size against the active target.
     * Power-of-two 8..1024 covers render-target textures; the exact 400x240
     * presentation size stays accepted (it is not a power of two). */
    unsigned back_size=A(1)==g.pp[0]&&A(2)==g.pp[1];
    unsigned pow2=A(1)>=8&&A(2)>=8&&A(1)<=1024&&A(2)<=1024&&
                  !(A(1)&(A(1)-1))&&!(A(2)&(A(2)-1));
    if(!back_size&&!pow2){
        d3d_fail("CreateDepthStencilSurface","only power-of-two 8..1024 or the exact back-buffer size is implemented",INVALID);
        RET(INVALID,9);}
    if(A(3)!=D9_D24S8&&A(3)!=D9_D24X8){
        d3d_fail("CreateDepthStencilSurface","only D3DFMT_D24S8/D24X8 (the PICA depth attachment) is implemented",UNAVAILABLE);
        RET(UNAVAILABLE,9);}
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,9);
    D9Handle h=0;int rc=d9_create_depth_surface(g.device,A(1),A(2),A(3),A(4),A(5),A(6)!=0,&h);
    if(rc){
        d3d_log("CreateDepthStencilSurface",rc==D9_MEMORY?"depth-stencil slot table is full":
            rc==D9_UNSUPPORTED?"the backend has no depth attachment or depth clear":
            rc==D9_INVALID?"invalid depth-stencil dimensions or format":"invalid depth-stencil request");
        RET(rc==D9_MEMORY?OUTMEM:rc==D9_INVALID?INVALID:UNAVAILABLE,9);
    }
    Obj *o=new_obj(SURFACE);if(!o){d9_release_depth_surface(g.device,h);RET(OUTMEM,9);}
    o->depth=h;o->is_depth=1;o->width=A(1);o->height=A(2);o->format=A(3);o->usage=0;o->pool=0;
    retain(g.dev);M32(out)=o->guest;
    trace_detail("DepthStencil=0x%08lx %lux%lu Format=%lu (PICA depth attachment)",
        (unsigned long)o->guest,(unsigned long)o->width,(unsigned long)o->height,(unsigned long)o->format);
    RET(OK,9);
}
static void create_offscreen(CPU *c){uint32_t out=A(5);if(!ptr(out,4))RET(INVALID,7);M32(out)=0;
    if(!obj(A(0),DEVICE))RET(INVALID,7);
    if(A(6)){d3d_log("CreateOffscreenPlainSurface","shared handles are not supported");RET(INVALID,7);}
    if(A(4)==1){d3d_log("CreateOffscreenPlainSurface","D3DPOOL_MANAGED is invalid for an off-screen plain surface");RET(INVALID,7);}
    if(A(4)==0){d3d_log("CreateOffscreenPlainSurface","D3DPOOL_DEFAULT off-screen surfaces require GPU operations that are not implemented");RET(UNAVAILABLE,7);}
    if(A(4)!=2&&A(4)!=3){d3d_log("CreateOffscreenPlainSurface","invalid pool");RET(INVALID,7);}
    if(!A(1)||!A(2)||A(1)>INT32_MAX/4||A(2)>INT32_MAX/4){d3d_log("CreateOffscreenPlainSurface","invalid dimensions");RET(INVALID,7);}
    if(!SURFACE_FORMAT(A(3))){d3d_log("CreateOffscreenPlainSurface","only 32-bit X8R8G8B8/A8R8G8B8 CPU surfaces are implemented");RET(UNAVAILABLE,7);}
    uint64_t bytes=(uint64_t)A(1)*A(2)*4;
    if(bytes>LOCK_STRIDE||bytes>SURFACE_BUDGET-g.surface_bytes){d3d_log("CreateOffscreenPlainSurface","surface memory budget exceeded");RET(OUTMEM,7);}
    if(g.next>=MAX_OBJECTS){d3d_log("CreateOffscreenPlainSurface","COM object table is full");RET(OUTMEM,7);}
    uint8_t *pixels=calloc(1,(size_t)bytes);if(!pixels){d3d_log("CreateOffscreenPlainSurface","out of memory");RET(OUTMEM,7);}
    Obj *o=new_obj(SURFACE);if(!o){free(pixels);RET(OUTMEM,7);}
    o->pixels=pixels;o->bytes=(unsigned)bytes;o->width=A(1);o->height=A(2);o->format=A(3);o->pool=A(4);o->pitch=A(1)*4;
    g.surface_bytes+=o->bytes;retain(g.dev);M32(out)=o->guest;
    trace_detail("CPU surface=%lux%lu Format=%lu Pool=%lu Bytes=%lu",(unsigned long)o->width,(unsigned long)o->height,
        (unsigned long)o->format,(unsigned long)o->pool,(unsigned long)o->bytes);
    RET(OK,7);
}
static void bind_render_target(Obj *o){
    if(g.rt==o)return;
    Obj *old=g.rt;if(o&&o!=g.back)retain(o);g.rt=o;
    /* The back buffer is owned by the device, not by the binding slot. */
    if(old&&old!=g.back)release(old);
}
static void set_render_target(CPU *c){Obj *o=obj(A(2),SURFACE);
    if(!obj(A(0),DEVICE)||A(1)||(A(2)&&(!o||(o!=g.back&&!o->handle)))||(o&&o->lock_guest))RET(INVALID,3);
    D9Handle h=(!o||o==g.back)?0:o->handle;
    if(g.clear_pending&&g.clear_target!=h){d3d_log("SetRenderTarget","queued Clear belongs to the previous render target");RET(INVALID,3);}
    int rc=d9_set_render_target(g.device,h);
    if(rc){
        if(rc==D9_UNSUPPORTED)d3d_fail("SetRenderTarget",d9_last_draw_diagnostic(g.device),UNAVAILABLE);
        RET(hr(rc),3);
    }
    bind_render_target(o?o:g.back);
    default_viewport();
    trace_detail("RenderTarget=0x%08lx Implicit=%u",(unsigned long)(o?o->guest:0),(unsigned)(!o||o==g.back));
    RET(OK,3);
}
static void get_render_target(CPU *c){uint32_t out=A(2);if(!ptr(out,4))RET(INVALID,3);M32(out)=0;
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    if(!g.rt)RET(NOTFOUND,3);
    if(A(1))RET(NOTFOUND,3);
    retain(g.rt);M32(out)=g.rt->guest;RET(OK,3);
}
/* 0 = ok, 1 = pointer outside guest memory, 2 = empty/out-of-bounds. */
static int stretch_read_rect(uint32_t address,unsigned w,unsigned h,D9Rect *out){
    if(!address){*out=(D9Rect){0,0,(int)w,(int)h};return 0;}
    const int32_t *p=ptr(address,16);if(!p)return 1;
    int32_t v[4];memcpy(v,p,sizeof v);
    if(v[0]<0||v[1]<0||v[2]<=v[0]||v[3]<=v[1]||(uint64_t)v[2]>w||(uint64_t)v[3]>h)return 2;
    *out=(D9Rect){v[0],v[1],v[2],v[3]};return 0;
}
static uint32_t stretch_backend_failure(int rc){
    if(rc==D9_MEMORY)return stretch_reject(OUTMEM,"the copy exceeded the device memory budget");
    if(rc==D9_INVALID)return stretch_reject(INVALID,"d3d9_core rejected the copy arguments");
    switch(d9_blit_last_error(g.device)){
    case D9_BLIT_UNSUPPORTED:return stretch_reject(UNAVAILABLE,"the backend has no StretchRect path for this source/destination/filter combination");
    case D9_BLIT_BACK_BUFFER_SRC:return stretch_reject(UNAVAILABLE,"the PICA backend cannot sample the implicit back buffer as a source");
    case D9_BLIT_NO_SCENE:return stretch_reject(UNAVAILABLE,"this PICA copy requires an active BeginScene/EndScene pair");
    case D9_BLIT_UNALIGNED:return stretch_reject(UNAVAILABLE,"this no-stretch copy needs 8-pixel aligned rectangles on the PICA texture-copy path");
    case D9_BLIT_SAME_SURFACE:return stretch_reject(INVALID,"source and destination must be different surfaces");
    case D9_BLIT_FORMAT:return stretch_reject(INVALID,"source and destination formats must match");
    case D9_BLIT_RECT:return stretch_reject(INVALID,"a rectangle is empty or outside its surface");
    case D9_BLIT_ARGUMENT:return stretch_reject(INVALID,"the copy arguments are invalid");
    default:return stretch_reject(UNAVAILABLE,"the backend rejected or failed to submit the copy");
    }
}
static void stretch_rect(CPU *c){
    char why[192];
    if(!obj(A(0),DEVICE)){RET(INVALID,6);}
    Obj *s=obj(A(1),SURFACE),*d=obj(A(3),SURFACE);
    if(!s||!d)RET(stretch_reject(INVALID,"source and destination must both be valid IDirect3DSurface9 objects"),6);
    if(s!=g.back&&!s->handle)
        RET(stretch_reject(INVALID,"source surface must be a render target or the back buffer; CPU off-screen plain surfaces are not in the default memory pool"),6);
    if(d!=g.back&&!d->handle)
        RET(stretch_reject(INVALID,"destination surface must be a render target or the back buffer"),6);
    if(s==d)RET(stretch_reject(INVALID,"source and destination must be different surfaces"),6);
    if(s->lock_guest||d->lock_guest)
        RET(stretch_reject(INVALID,"a locked surface cannot participate in StretchRect"),6);
    if(s->format!=d->format){
        snprintf(why,sizeof why,"source format %s and destination format %s differ; StretchRect does not convert formats",
            surface_format_name(s->format),surface_format_name(d->format));
        RET(stretch_reject(INVALID,why),6);
    }
    if(A(5)>D9_FILTER_LINEAR)
        RET(stretch_reject(INVALID,"filter must be D3DTEXF_NONE, D3DTEXF_POINT or D3DTEXF_LINEAR"),6);
    D9Rect srect,drect;
    int srr=stretch_read_rect(A(2),s->width,s->height,&srect);
    if(srr){
        snprintf(why,sizeof why,"source rectangle %s (surface is %ux%u)",srr==1?"is outside guest memory":"must be non-empty and inside the surface",
            s->width,s->height);
        RET(stretch_reject(INVALID,why),6);
    }
    int drr=stretch_read_rect(A(4),d->width,d->height,&drect);
    if(drr){
        snprintf(why,sizeof why,"destination rectangle %s (surface is %ux%u)",drr==1?"is outside guest memory":"must be non-empty and inside the surface",
            d->width,d->height);
        RET(stretch_reject(INVALID,why),6);
    }
    int rc=d9_blit(g.device,d==g.back?0:d->handle,&drect,s==g.back?0:s->handle,&srect,A(5));
    if(rc)RET(stretch_backend_failure(rc),6);
    g.last_stretch_rejection[0]=0;
    trace_detail("Source=0x%08lx Rect=%d,%d..%d,%d Destination=0x%08lx Rect=%d,%d..%d,%d Filter=%lu",
        (unsigned long)(s==g.back?0:s->guest),srect.left,srect.top,srect.right,srect.bottom,
        (unsigned long)(d==g.back?0:d->guest),drect.left,drect.top,drect.right,drect.bottom,(unsigned long)A(5));
    RET(OK,6);
}
static int read_update_rect(uint32_t address,unsigned w,unsigned h,int32_t r[4]){
    if(!address){r[0]=0;r[1]=0;r[2]=(int32_t)w;r[3]=(int32_t)h;return 0;}
    const int32_t *p=ptr(address,16);if(!p)return 1;
    memcpy(r,p,16);
    if(r[0]<0||r[1]<0||r[2]<=r[0]||r[3]<=r[1]||
       (uint64_t)r[2]>w||(uint64_t)r[3]>h)return 2;
    return 0;
}
static int read_update_point(uint32_t address,int32_t *x,int32_t *y){
    if(!address){*x=0;*y=0;return 0;}
    const int32_t *p=ptr(address,8);if(!p)return 1;
    *x=p[0];*y=p[1];
    return (*x<0||*y<0)?2:0;
}
static void update_surface(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,5);
    Obj *s=obj(A(1),SURFACE),*d=obj(A(3),SURFACE);
    if(!s||!d)RET(INVALID,5);
    const char *why=NULL;
    if(g.phase)why="UpdateSurface cannot run inside BeginScene/EndScene";
    else if(s->is_depth||d->is_depth)why="depth-stencil surfaces are not supported by UpdateSurface";
    else if(s->view_cube||d->view_cube)why="cube-face surfaces are not supported by UpdateSurface";
    else if(s->handle||d->handle)why="GPU-only render-target/back-buffer surfaces are not supported by UpdateSurface";
    else if(!s->pixels||!d->pixels)why="both surfaces need a CPU-backed pixel store";
    else if(s->lock_guest||d->lock_guest)why="a locked surface cannot participate in UpdateSurface";
    else if(s->block_bytes||d->block_bytes)why="compressed surfaces are not supported by UpdateSurface";
    else if(s->format!=d->format)why="source and destination formats must match";
    if(why){d3d_fail("UpdateSurface",why,INVALID);RET(INVALID,5);}
    int32_t r[4];
    int rr=read_update_rect(A(2),s->width,s->height,r);
    if(rr){d3d_fail("UpdateSurface",rr==1?
        "source rectangle is outside guest memory":"source rectangle must be non-empty and inside the source surface",INVALID);RET(INVALID,5);}
    int32_t px,py;
    int pr=read_update_point(A(4),&px,&py);
    if(pr){d3d_fail("UpdateSurface",pr==1?
        "destination point is outside guest memory":"destination point must be non-negative",INVALID);RET(INVALID,5);}
    unsigned cw=(unsigned)(r[2]-r[0]),ch=(unsigned)(r[3]-r[1]);
    if((uint64_t)px+cw>d->width||(uint64_t)py+ch>d->height){
        d3d_fail("UpdateSurface","the copied rectangle does not fit the destination surface",INVALID);RET(INVALID,5);}
    size_t row=(size_t)cw*4u;
    for(unsigned y=0;y<ch;++y)
        memmove(d->pixels+(size_t)(py+y)*d->pitch+(size_t)px*4,
                s->pixels+(size_t)(r[1]+y)*s->pitch+(size_t)r[0]*4,row);
    if(d->is_view&&d->view_parent&&!d->view_cube){
        int rc=d9_texture_surface_unlocked(g.device,d->view_texture,d->view_level);
        if(rc){d3d_fail("UpdateSurface",d9_last_draw_diagnostic(g.device),hr(rc));RET(hr(rc),5);}
    }
    g.last_diagnostic[0]=0;
    trace_detail("UpdateSurface src=0x%08lx rect=%ld,%ld..%ld,%ld dst=0x%08lx point=%ld,%ld",
                 (unsigned long)s->guest,(long)r[0],(long)r[1],(long)r[2],(long)r[3],
                 (unsigned long)d->guest,(long)px,(long)py);
    RET(OK,5);
}
static void update_texture(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    Obj *s=obj(A(1),0),*d=obj(A(2),0);
    if(!s||!d){d3d_fail("UpdateTexture","source and destination must be valid texture objects",INVALID);RET(INVALID,3);}
    if(s->kind!=TEX||d->kind!=TEX){
        d3d_fail("UpdateTexture","only IDirect3DTexture9 objects are supported; cube/volume textures are not substituted",UNAVAILABLE);
        RET(UNAVAILABLE,3);
    }
    int rc=d9_texture_copy_levels(g.device,d->handle,s->handle);
    if(rc){d3d_fail("UpdateTexture",d9_last_draw_diagnostic(g.device),hr(rc));RET(hr(rc),3);}
    g.last_diagnostic[0]=0;
    RET(OK,3);
}
static void color_fill(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,4);
    Obj *o=obj(A(1),SURFACE);uint32_t color=A(2);
    if(!o){d3d_fail("ColorFill","argument is not a valid IDirect3DSurface9",INVALID);RET(INVALID,4);}
    if(o->is_depth){d3d_fail("ColorFill","depth-stencil surfaces cannot be colour-filled",UNAVAILABLE);RET(UNAVAILABLE,4);}
    if(o->view_cube){d3d_fail("ColorFill","cube-face surfaces are not supported",UNAVAILABLE);RET(UNAVAILABLE,4);}
    if(o->lock_guest){d3d_fail("ColorFill","a locked surface cannot be colour-filled",INVALID);RET(INVALID,4);}
    int32_t r[4];
    int rr=read_update_rect(A(3),o->width,o->height,r);
    if(rr){d3d_fail("ColorFill",rr==1?
        "fill rectangle is outside guest memory":"fill rectangle must be non-empty and inside the surface",INVALID);RET(INVALID,4);}
    unsigned x=(unsigned)r[0],y=(unsigned)r[1];
    unsigned w=(unsigned)(r[2]-r[0]),h=(unsigned)(r[3]-r[1]);
    if(o->handle){
        size_t bytes=(size_t)w*h*4u;
        uint32_t *buf=malloc(bytes);if(!buf)RET(OUTMEM,4);
        for(size_t i=0;i<(size_t)w*h;++i)buf[i]=color;
        int rc=d9_render_target_write(g.device,o->handle,buf,(size_t)w*4u,x,y,w,h);
        free(buf);
        if(rc){
            d3d_fail("ColorFill",rc==D9_UNSUPPORTED?d9_last_draw_diagnostic(g.device):
                     "backend cannot write this render-target surface pixel store",hr(rc));
            RET(hr(rc),4);
        }
    }else{
        if(!o->pixels){
            d3d_fail("ColorFill","surface has neither a CPU store nor a writable backend render target",UNAVAILABLE);
            RET(UNAVAILABLE,4);
        }
        for(unsigned yy=0;yy<h;++yy){
            uint32_t *row=(uint32_t *)(o->pixels+(size_t)(y+yy)*o->pitch)+x;
            for(unsigned xx=0;xx<w;++xx)row[xx]=color;
        }
        if(o->is_view&&o->view_parent&&!o->view_cube){
            int rc=d9_texture_surface_unlocked(g.device,o->view_texture,o->view_level);
            if(rc){d3d_fail("ColorFill",d9_last_draw_diagnostic(g.device),hr(rc));RET(hr(rc),4);}
        }
    }
    g.last_diagnostic[0]=0;
    trace_detail("ColorFill=0x%08lx color=0x%08lx rect=%ld,%ld..%ld,%ld",
                 (unsigned long)o->guest,(unsigned long)color,(long)r[0],(long)r[1],
                 (long)r[2],(long)r[3]);
    RET(OK,4);
}
static void get_render_target_data(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    Obj *s=obj(A(1),SURFACE),*d=obj(A(2),SURFACE);
    if(!s||!d){d3d_fail("GetRenderTargetData","source and destination must be valid surfaces",INVALID);RET(INVALID,3);}
    const char *why=NULL;uint32_t fail=INVALID;
    if(g.phase)why="GetRenderTargetData cannot read back inside BeginScene/EndScene";
    else if(s->is_depth||d->is_depth){why="depth-stencil surfaces cannot be read back";fail=UNAVAILABLE;}
    else if(s->view_cube||d->view_cube){why="cube-face surfaces are not supported";fail=UNAVAILABLE;}
    else if(!s->handle)why="source surface is not a render target";
    else if(d->handle){why="destination must be a CPU offscreen/system surface, not a GPU-only target";fail=UNAVAILABLE;}
    else if(!d->pixels||d->is_view)why="destination needs its own CPU pixel store";
    else if(s->lock_guest||d->lock_guest)why="a locked surface cannot participate in GetRenderTargetData";
    else if(s->format!=d->format||s->width!=d->width||s->height!=d->height)
        why="source and destination must share size and format";
    else if(d->pool!=2&&d->pool!=3)why="destination must be D3DPOOL_SYSTEMMEM or D3DPOOL_SCRATCH";
    if(why){d3d_fail("GetRenderTargetData",why,fail);RET(fail,3);}
    int rc=d9_render_target_read(g.device,s->handle,d->pixels,d->pitch,0,0,s->width,s->height);
    if(rc){
        const char *detail=d9_last_draw_diagnostic(g.device);
        if(rc==D9_BACKEND&&(!detail||!*detail))
            detail="backend cannot read this render target (target_read unavailable)";
        d3d_fail("GetRenderTargetData",detail,hr(rc));
        RET(hr(rc),3);
    }
    g.last_diagnostic[0]=0;
    trace_detail("GetRenderTargetData src=0x%08lx dst=0x%08lx %lux%lu",
                 (unsigned long)s->guest,(unsigned long)d->guest,
                 (unsigned long)s->width,(unsigned long)s->height);
    RET(OK,3);
}
static void get_front_buffer_data(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    d3d_fail("GetFrontBufferData","front buffer readback is not provided by this bridge; "
             "no real front-buffer source exists and the back buffer is not substituted",UNAVAILABLE);
    RET(UNAVAILABLE,3);
}
static void set_depth_stencil(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(!A(1)){
        if(g.depth){if(g.device)(void)d9_set_depth_surface(g.device,0);bind(&g.depth,NULL);}
        RET(OK,2);
    }
    Obj *o=obj(A(1),SURFACE);
    if(!o||!o->is_depth||o->lock_guest){d3d_log("SetDepthStencilSurface","argument is not a depth-stencil surface");RET(INVALID,2);}
    int rc=d9_set_depth_surface(g.device,o->depth);
    if(rc){d3d_fail("SetDepthStencilSurface",d9_last_draw_diagnostic(g.device),hr(rc));RET(hr(rc),2);}
    bind(&g.depth,o);RET(OK,2);
}
static void get_depth_stencil(CPU *c){uint32_t out=A(1);if(!ptr(out,4))RET(INVALID,2);M32(out)=0;
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(!g.depth)RET(NOTFOUND,2);
    retain(g.depth);M32(out)=g.depth->guest;RET(OK,2);
}
static void set_viewport(CPU *c){uint32_t p=A(1);
    if(!obj(A(0),DEVICE)||!ptr(p,24))RET(INVALID,2);
    uint32_t v[6];memcpy(v,ptr(p,24),24);float minz,maxz;memcpy(&minz,v+4,4);memcpy(&maxz,v+5,4);
    if(!v[2]||!v[3]||(uint64_t)v[0]+v[2]>g.rt->width||(uint64_t)v[1]+v[3]>g.rt->height||
        !isfinite(minz)||!isfinite(maxz)||minz<0.0f||maxz>1.0f||minz>maxz)RET(INVALID,2);
    if(v[0]||v[1]||v[2]!=g.rt->width||v[3]!=g.rt->height||minz!=0.0f||maxz!=1.0f){
        d3d_log("SetViewport","only the full render-target viewport is implemented");RET(UNAVAILABLE,2);
    }
    memcpy(g.viewport,v,sizeof g.viewport);(void)d9_set_viewport_size(g.device,v[2],v[3]);RET(OK,2);
}
static void get_viewport(CPU *c){uint32_t p=A(1);if(!obj(A(0),DEVICE)||!ptr(p,24))RET(INVALID,2);
    memcpy(ptr(p,24),g.viewport,sizeof g.viewport);RET(OK,2);
}
static void set_scissor_rect(CPU *c){uint32_t p=A(1);
    if(!obj(A(0),DEVICE)||g.phase==2||!ptr(p,16)){
        d3d_log("SetScissorRect","invalid device, scene phase or RECT pointer");RET(INVALID,2);}
    uint32_t r[4];memcpy(r,ptr(p,16),sizeof r);
    unsigned width=g.rt?g.rt->width:g.pp[0],height=g.rt?g.rt->height:g.pp[1];
    if(r[0]>=r[2]||r[1]>=r[3]||r[2]>width||r[3]>height){
        d3d_log("SetScissorRect","rectangle must satisfy left < right <= width and top < bottom <= height");
        RET(INVALID,2);}
    if((r[0]||r[1]||r[2]!=width||r[3]!=height)&&!g.scissor_warned){
        g.scissor_warned=1;
        d3d_log("SetScissorRect","rectangle recorded; the renderer has no scissor stage, so draws are not clipped to it");
    }
    memcpy(g.scissor,r,sizeof g.scissor);
    trace_detail("left=%lu top=%lu right=%lu bottom=%lu",(unsigned long)r[0],(unsigned long)r[1],
        (unsigned long)r[2],(unsigned long)r[3]);
    RET(OK,2);
}
static void get_scissor_rect(CPU *c){uint32_t p=A(1);
    if(!obj(A(0),DEVICE)||!ptr(p,16)){d3d_log("GetScissorRect","invalid device or RECT pointer");RET(INVALID,2);}
    memcpy(ptr(p,16),g.scissor,sizeof g.scissor);RET(OK,2);
}
static void set_gamma_ramp(CPU *c){uint32_t p=A(3);
    if(!obj(A(0),DEVICE)||A(1)||(A(2)&~1u)||!ptr(p,3*256*2)){
        d3d_log("SetGammaRamp","only swap chain 0, a valid flags word and a 3x256 WORD ramp are implemented");
        RET(INVALID,4);}
    const uint16_t *r=ptr(p,3*256*2);
    memcpy(g.gamma,r,sizeof g.gamma);
    if(!g.gamma_warned&&!gamma_identity(r)){
        g.gamma_warned=1;
        d3d_log("SetGammaRamp","non-identity ramp recorded; the renderer applies no output gamma correction");
    }
    trace_detail("gamma ramp recorded");
    RET(OK,4);
}
static void get_gamma_ramp(CPU *c){uint32_t p=A(2);
    if(!obj(A(0),DEVICE)||A(1)||!ptr(p,3*256*2)){
        d3d_log("GetGammaRamp","invalid device, swap chain or ramp pointer");RET(INVALID,3);}
    memcpy(ptr(p,3*256*2),g.gamma,sizeof g.gamma);RET(OK,3);
}
static void set_npatch_mode(CPU *c){uint32_t bits=A(1);float f;memcpy(&f,&bits,4);
    if(!obj(A(0),DEVICE)||g.phase==2||!isfinite(f)||f<0.0f||f>64.0f){
        d3d_log("SetNPatchMode","segments must be a finite value in [0,64]");RET(INVALID,2);}
    g.npatch_segments=f;
    trace_detail("Segments=%g",(double)f);
    RET(OK,2);
}
static void get_npatch_mode(CPU *c){
    if(!obj(A(0),DEVICE)){d3d_log("GetNPatchMode","invalid device object");RETF(0.0f,1);}
    RETF(g.npatch_segments,1);
}
static void cooperative(CPU *c){RET(obj(A(0),DEVICE)?OK:INVALID,1);}
static void available_texture_mem(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,1);
    size_t avail=d9_available_texture_mem(g.device);
    uint32_t value=avail>0xffffffffu?0xffffffffu:(uint32_t)avail;
    trace_detail("GetAvailableTextureMem: %lu bytes remaining of the device budget",(unsigned long)value);
    RET(value,1);
}
static void evict_managed(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,1);
    int rc=d9_evict_managed_resources(g.device);
    if(rc){
        d3d_fail("EvictManagedResources",d9_last_draw_diagnostic(g.device),rc==D9_INVALID?INVALID:UNAVAILABLE);
        RET(rc==D9_INVALID?INVALID:UNAVAILABLE,1);
    }
    g.last_diagnostic[0]=0;
    RET(OK,1);
}
static void raster_status(CPU *c){
    if(!obj(A(0),DEVICE)||!ptr(A(1),8))RET(INVALID,2);
    /* PICA has no scanline or vblank status register exposed through this
     * bridge.  Refuse rather than invent an InVBlank/ScanLine pair. */
    M32(A(1))=0;M32(A(1)+4)=0;
    d3d_fail("GetRasterStatus","PICA has no scanline/vblank raster status",UNAVAILABLE);
    RET(UNAVAILABLE,2);
}
static void dialog_box_mode(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(A(1)){
        d3d_fail("SetDialogBoxMode","TRUE is not implemented; pass FALSE",UNAVAILABLE);
        RET(UNAVAILABLE,2);
    }
    g.last_diagnostic[0]=0;
    RET(OK,2);
}
static void validate_device(CPU *c){
    uint32_t out=A(1);
    if(!obj(A(0),DEVICE)||!ptr(out,4))RET(INVALID,2);
    M32(out)=0;
    unsigned passes=0;
    int rc=d9_validate_device(g.device,&passes);
    if(rc){
        d3d_fail("ValidateDevice",d9_last_draw_diagnostic(g.device),rc==D9_INVALID?INVALID:UNAVAILABLE);
        RET(rc==D9_INVALID?INVALID:UNAVAILABLE,2);
    }
    M32(out)=passes;g.last_diagnostic[0]=0;
    RET(OK,2);
}
/* ---- IDirect3DQuery9 ---------------------------------------------------- */
/* The backend is synchronous: EVENT is always already complete.  Occlusion,
 * resource-manager, timestamp and timing queries need GPU counters this bridge
 * does not expose and are refused by name rather than returning invented data. */
static const char *query_type_name(unsigned type){
    switch(type){
    case 4:return "D3DQUERYTYPE_VCACHE";
    case 5:return "D3DQUERYTYPE_RESOURCEMANAGER";
    case 6:return "D3DQUERYTYPE_VERTEXSTATS";
    case 8:return "D3DQUERYTYPE_EVENT";
    case 9:return "D3DQUERYTYPE_OCCLUSION";
    case 10:return "D3DQUERYTYPE_TIMESTAMP";
    case 11:return "D3DQUERYTYPE_TIMESTAMPDISJOINT";
    case 12:return "D3DQUERYTYPE_TIMESTAMPFREQ";
    case 13:return "D3DQUERYTYPE_PIPELINETIMINGS";
    case 14:return "D3DQUERYTYPE_INTERFACETIMINGS";
    case 15:return "D3DQUERYTYPE_VERTEXTIMINGS";
    case 16:return "D3DQUERYTYPE_PIXELTIMINGS";
    case 17:return "D3DQUERYTYPE_BANDWIDTHTIMINGS";
    case 18:return "D3DQUERYTYPE_CACHEUTILIZATION";
    case 19:return "D3DQUERYTYPE_MEMORYPRESSURE";
    default:return "unknown query type";
    }
}
static void create_query(CPU *c){
    uint32_t out=A(2);
    if(!obj(A(0),DEVICE)||!ptr(out,4)){d3d_log("CreateQuery","invalid device or output pointer");RET(INVALID,3);}
    M32(out)=0;
    unsigned type=A(1);
    if(type!=8u){
        char why[192];
        snprintf(why,sizeof why,
                 "%s (%u) is not implemented; only D3DQUERYTYPE_EVENT is available on the synchronous backend",
                 query_type_name(type),type);
        d3d_fail("CreateQuery",why,UNAVAILABLE);RET(UNAVAILABLE,3);
    }
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,3);
    Obj *o=new_obj(QUERY);
    if(!o)RET(OUTMEM,3);
    o->query_type=type;o->query_issued=0;retain(g.dev);
    M32(out)=o->guest;g.last_diagnostic[0]=0;RET(OK,3);
}
static void query_get_device(CPU *c){
    if(!obj(A(0),QUERY)||!ptr(A(1),4))RET(INVALID,2);
    retain(g.dev);M32(A(1))=g.dev?g.dev->guest:0;RET(OK,2);
}
static void query_get_type(CPU *c){
    Obj *o=obj(A(0),QUERY);
    if(!o||!ptr(A(1),4))RET(INVALID,2);
    M32(A(1))=o->query_type;RET(OK,2);
}
static void query_get_data_size(CPU *c){
    if(!obj(A(0),QUERY)||!ptr(A(1),4))RET(INVALID,2);
    M32(A(1))=4;RET(OK,2);
}
static void query_issue(CPU *c){
    Obj *o=obj(A(0),QUERY);
    if(!o)RET(INVALID,2);
    if(A(1)!=1u){ /* D3DISSUE_END */
        d3d_fail("Issue","EVENT queries accept only D3DISSUE_END",INVALID);RET(INVALID,2);
    }
    o->query_issued=1;g.last_diagnostic[0]=0;RET(OK,2);
}
static void query_get_data(CPU *c){
    Obj *o=obj(A(0),QUERY);uint32_t data=A(1),size=A(2),flags=A(3);
    if(!o||!ptr(data,4)||size<4u){
        d3d_fail("GetData","EVENT query needs a readable 4-byte BOOL destination",INVALID);RET(INVALID,4);
    }
    if(flags&~1u){
        d3d_fail("GetData","only D3DGETDATA_FLUSH is recognized",INVALID);RET(INVALID,4);
    }
    if(!o->query_issued){
        d3d_fail("GetData","query has not been issued",INVALID);RET(INVALID,4);
    }
    /* The synchronous bridge has already completed the work Issue covered. */
    M32(data)=1;g.last_diagnostic[0]=0;RET(OK,4);
}
static void direct3d(CPU *c){if(!obj(A(0),DEVICE)||!g.factory||!ptr(A(1),4))RET(INVALID,2);retain(g.factory);M32(A(1))=g.factory->guest;RET(OK,2);}
static void resource_device(CPU *c){Obj *o=obj(A(0),0);if(!o||o->kind<VB||!g.dev||!ptr(A(1),4))RET(INVALID,2);retain(g.dev);M32(A(1))=g.dev->guest;RET(OK,2);}
static int fvf_admitted(unsigned fvf){
    if(!g.tnl_proto)return !fvf||fvf==0x102u||fvf==0x104u||fvf==0x142u||fvf==0x144u;
    D9Decl decl;const char *why=NULL;
    return d9_decl_from_fvf(fvf,1,&decl,&why)==D9_OK;
}
static void create_vb(CPU *c){uint32_t out=A(5);if(!obj(A(0),DEVICE)||g.phase||!ptr(out,4))RET(INVALID,7);M32(out)=0;
    if(A(6))RET(INVALID,7);
    if(!A(1)||A(1)>LOCK_STRIDE||!fvf_admitted(A(3))||(A(2)&~USAGE_VB_MASK)||A(4)>2){
        d3d_fail("CreateVertexBuffer",(A(2)&~USAGE_VB_MASK)?"only WRITEONLY|DYNAMIC usage is implemented":
            !fvf_admitted(A(3))?"FVF is not XYZ(XYZRHW)|DIFFUSE|TEX1":"invalid length, pool or shared handle",UNAVAILABLE);
        RET(UNAVAILABLE,7);
    }
    if(A(4)==1)d3d_log("CreateVertexBuffer","MANAGED accepted as CPU-backed bridge storage; device-loss restoration is not implemented");
    if(A(2)&USAGE_DYNAMIC)d3d_log("CreateVertexBuffer","DYNAMIC accepted as CPU-staged storage; every Lock/Unlock is copied and upload is deferred to the draw");
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,7);D9Handle h;int rc=d9_create_vertex_buffer(g.device,A(1),&h);if(rc)RET(hr(rc),7);
    Obj *o=new_obj(VB);if(!o){d9_release_buffer(g.device,h);RET(OUTMEM,7);}
    o->handle=h;o->bytes=A(1);o->usage=A(2);o->fvf=A(3);o->pool=A(4);retain(g.dev);M32(out)=o->guest;g.last_diagnostic[0]=0;RET(OK,7);}
static void create_ib(CPU *c){uint32_t out=A(5);if(!obj(A(0),DEVICE)||g.phase||!ptr(out,4))RET(INVALID,7);M32(out)=0;
    if(A(6))RET(INVALID,7);
    if(!A(1)||A(1)>LOCK_STRIDE||(A(3)!=D9_INDEX16&&A(3)!=D9_INDEX32)||(A(2)&~USAGE_VB_MASK)||A(4)>2){
        d3d_fail("CreateIndexBuffer",(A(2)&~USAGE_VB_MASK)?"only WRITEONLY|DYNAMIC usage is implemented":"invalid length, index format, pool or shared handle",UNAVAILABLE);
        RET(UNAVAILABLE,7);
    }
    if(A(4)==1)d3d_log("CreateIndexBuffer","MANAGED accepted as CPU-backed bridge storage; device-loss restoration is not implemented");
    if(A(2)&USAGE_DYNAMIC)d3d_log("CreateIndexBuffer","DYNAMIC accepted as CPU-staged storage; every Lock/Unlock is copied");
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,7);D9Handle h;int rc=d9_create_index_buffer(g.device,A(1),A(3),&h);if(rc)RET(hr(rc),7);
    Obj *o=new_obj(IB);if(!o){d9_release_buffer(g.device,h);RET(OUTMEM,7);}
    o->handle=h;o->bytes=A(1);o->usage=A(2);o->format=A(3);o->pool=A(4);retain(g.dev);M32(out)=o->guest;g.last_diagnostic[0]=0;RET(OK,7);}
static void create_tex(CPU *c){uint32_t out=A(7);if(!obj(A(0),DEVICE)||g.phase||!ptr(out,4))RET(INVALID,9);M32(out)=0;
    if(A(8))RET(INVALID,9);
    if(A(4)==1u){
        unsigned w=A(1),height=A(2),levels=A(3),format=A(5),pool=A(6);
        unsigned storage=format;
        int hdr=format==0x71u||format==0x72u; /* A16B16G16R16F / A32B32G32R32F */
        if(hdr&&g.hdr_ldr_fallback)storage=D9_A8R8G8B8;
        const char *why=pool!=0?"render-target textures require D3DPOOL_DEFAULT":
            levels!=1?"render-target textures are implemented with exactly one level":
            (w<8||height<8||w>1024||height>1024||(w&(w-1))||(height&(height-1)))?
                "render-target textures must be power-of-two 8..1024":
            (storage!=D9_A8R8G8B8&&storage!=D9_X8R8G8B8)?
                "float/HDR render-target textures are not implemented; A8R8G8B8 and X8R8G8B8 only":NULL;
        if(why){d3d_fail("CreateTexture",why,UNAVAILABLE);RET(UNAVAILABLE,9);}
        if(hdr&&g.hdr_ldr_fallback)
            trace_detail("MEASUREMENT ONLY: HDR target %ux%u format 0x%x stored as A8R8G8B8",w,height,format);
        if(g.next>=MAX_OBJECTS)RET(OUTMEM,9);D9Handle h;int rc=d9_create_render_texture(g.device,w,height,storage,pool,1u,&h);
        if(rc){
            trace_detail("CreateTexture: render-target creation rc=%d",rc);
            const char *why=d9_last_draw_diagnostic(g.device);
            if((rc==D9_UNSUPPORTED||rc==D9_BACKEND)&&why&&*why)d3d_fail("CreateTexture",why,UNAVAILABLE);
            else d3d_log("CreateTexture",rc==D9_MEMORY?
                "render-target texture memory budget exceeded":
                "render-target texture creation failed");
            RET(rc==D9_MEMORY?OUTMEM:UNAVAILABLE,9);
        }
        Obj *o=new_obj(TEX);if(!o){d9_release_texture(g.device,h);RET(OUTMEM,9);}
        o->handle=h;o->width=w;o->height=height;o->bytes=w*height*4u;o->pitch=w*4u;
        o->format=format;o->usage=1u;o->pool=pool;retain(g.dev);M32(out)=o->guest;g.last_diagnostic[0]=0;RET(OK,9);
    }
    if(A(4)&~0x00220200u){d3d_fail("CreateTexture","autogen, depth-stencil and non-DYNAMIC texture usages are not implemented",UNAVAILABLE);RET(UNAVAILABLE,9);}
    if(A(6)>2)RET(UNAVAILABLE,9);
    if(A(6)==1)d3d_log("CreateTexture","MANAGED keeps a guest-backed restoration source; device-loss restoration is pending");
    if(A(6)==2)d3d_log("CreateTexture","SYSTEMMEM is CPU-only and cannot be bound as a GPU texture");
    if(A(4)&USAGE_DYNAMIC)d3d_log("CreateTexture","DYNAMIC accepted; LockRect/UnlockRect re-upload the level like MANAGED");
    size_t pitch,bytes;if(d9_texture_layout(A(1),A(2),A(5),&pitch,&bytes))RET(INVALID,9);
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,9);D9Handle h;int rc=d9_create_texture_ex(g.device,A(1),A(2),A(5),A(3),A(6),A(4),0,&h);if(rc)RET(hr(rc),9);
    Obj *o=new_obj(TEX);if(!o){d9_release_texture(g.device,h);RET(OUTMEM,9);}
    o->handle=h;o->width=A(1);o->height=A(2);o->bytes=(unsigned)bytes;o->pitch=(unsigned)pitch;o->format=A(5);o->usage=A(4);o->pool=A(6);retain(g.dev);M32(out)=o->guest;RET(OK,9);}
static uint32_t cube_create_obj(uint32_t device,unsigned edge,unsigned levels,unsigned usage,
                                unsigned format,unsigned pool,uint32_t *out){
    if(out)*out=0;
    if(!g.mem||!g.device||!out||g.phase||!obj(device,DEVICE))return INVALID;
    if(usage&~0x00220200u){
        char why[96];snprintf(why,sizeof why,"unsupported cube texture usage 0x%x",usage);
        d3d_fail("CreateCubeTexture",why,UNAVAILABLE);return UNAVAILABLE;
    }
    if(pool>2)return UNAVAILABLE;
    if(g.next>=MAX_OBJECTS)return OUTMEM;
    D9Handle h=0;int rc=d9_create_cube_texture_ex(g.device,edge,levels,format,pool,usage,&h);
    if(rc){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"CreateCubeTexture: %s",
                 d9_last_draw_diagnostic(g.device));
        return hr(rc);
    }
    Obj *o=new_obj(CUBE);
    if(!o){d9_release_cube_texture(g.device,h);return OUTMEM;}
    o->handle=h;o->width=o->height=edge;o->format=format;o->usage=usage;o->pool=pool;
    retain(g.dev);*out=o->guest;return OK;
}
static void create_cube_tex(CPU *c){uint32_t out=A(6);
    if(!obj(A(0),DEVICE)||g.phase||!ptr(out,4))RET(INVALID,8);M32(out)=0;
    if(A(7)){d3d_fail("CreateCubeTexture","shared handles are not supported",INVALID);RET(INVALID,8);}
    uint32_t guest=0;uint32_t rc=cube_create_obj(A(0),A(1),A(2),A(3),A(4),A(5),&guest);
    if(rc)RET(rc,8);
    M32(out)=guest;RET(OK,8);}
uint32_t d9_com_host_create_cube_texture(uint32_t device,unsigned edge,unsigned levels,
                                         unsigned usage,unsigned format,unsigned pool,
                                         uint32_t *out_cube){
    return cube_create_obj(device,edge,levels,usage,format,pool,out_cube);
}
uint32_t d9_com_host_upload_cube_face_level(uint32_t cube,unsigned face,unsigned level,
                                            const void *src,size_t src_pitch,
                                            size_t src_bytes){
    Obj *o=obj(cube,CUBE);
    if(!o||!g.device||!src||!src_pitch||g.phase)return INVALID;
    int rc=d9_cube_upload_face_level(g.device,o->handle,face,level,src,src_pitch,src_bytes);
    if(rc){snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
        "UploadCubeFace: %s",d9_last_draw_diagnostic(g.device));return hr(rc);}
    g.last_diagnostic[0]=0;return OK;
}
static uint32_t volume_create_obj(uint32_t device,unsigned w,unsigned h,unsigned depth,
                                  unsigned levels,unsigned usage,unsigned format,
                                  unsigned pool,uint32_t *out){
    if(out)*out=0;
    if(!g.mem||!g.device||!out||g.phase||!obj(device,DEVICE))return INVALID;
    if(usage&~0x00220200u)return UNAVAILABLE;
    if(pool>2)return UNAVAILABLE;
    if(g.next>=MAX_OBJECTS)return OUTMEM;
    D9Handle handle=0;
    int rc=d9_create_volume_texture_ex(g.device,w,h,depth,levels,format,pool,usage,&handle);
    if(rc){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"CreateVolumeTexture: %s",
                 d9_last_draw_diagnostic(g.device));
        return hr(rc);
    }
    Obj *o=new_obj(VOLUME);
    if(!o){d9_release_volume_texture(g.device,handle);return OUTMEM;}
    o->handle=handle;o->width=w;o->height=h;o->volume_depth=depth;
    o->format=format;o->usage=usage;o->pool=pool;
    retain(g.dev);*out=o->guest;return OK;
}
static void create_volume_tex(CPU *c){uint32_t out=A(8);
    if(!obj(A(0),DEVICE)||g.phase||!ptr(out,4))RET(INVALID,10);M32(out)=0;
    if(A(9)){d3d_fail("CreateVolumeTexture","shared handles are not supported",INVALID);RET(INVALID,10);}
    uint32_t guest=0;
    uint32_t rc=volume_create_obj(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),&guest);
    if(rc)RET(rc,10);
    M32(out)=guest;RET(OK,10);
}
uint32_t d9_com_host_create_volume_texture(uint32_t device,unsigned w,unsigned h,
                                           unsigned depth,unsigned levels,unsigned usage,
                                           unsigned format,unsigned pool,uint32_t *out){
    return volume_create_obj(device,w,h,depth,levels,usage,format,pool,out);
}
uint32_t d9_com_host_upload_volume_level(uint32_t volume,unsigned level,
                                         const void *src,size_t src_pitch,
                                         size_t src_slice_pitch,size_t src_bytes){
    Obj *o=obj(volume,VOLUME);
    if(!o||!g.device||!src||!src_pitch||!src_slice_pitch||g.phase)return INVALID;
    int rc=d9_volume_upload_level(g.device,o->handle,level,src,src_pitch,src_slice_pitch,src_bytes);
    if(rc){snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
        "UploadVolumeLevel: %s",d9_last_draw_diagnostic(g.device));return hr(rc);}
    g.last_diagnostic[0]=0;return OK;
}
static uint32_t lock_addr(Obj *o){return LOCK_BASE+(unsigned)(o-g.o)*LOCK_STRIDE;}
static int lock_buffer(Obj *o,unsigned offset,unsigned bytes,unsigned flags){
    if(!o||(o->kind!=VB&&o->kind!=IB)||o->lock_guest||g.phase||offset>=o->bytes||flags&~0x3010u)return D9_INVALID;
    if(!bytes)bytes=o->bytes-offset;if(bytes>o->bytes-offset)return D9_INVALID;
    uint32_t address=lock_addr(o);if(guest_map(g.mem,address,bytes))return D9_MEMORY;
    void *host;int rc=d9_buffer_lock(g.device,o->handle,offset,bytes,&host);
    if(rc){guest_unmap(g.mem,address);return rc;}
    if(!(flags&0x2000))memcpy(ptr(address,bytes),host,bytes);
    o->lock_guest=address;o->lock_host=host;o->lock_bytes=bytes;o->readonly=!!(flags&0x10);o->lock_rows=1;o->lock_pitch=o->lock_row_bytes=bytes;return D9_OK;
}
static int lock_texture(Obj *o,unsigned level,uint32_t rect,unsigned flags){
    if(!o||o->kind!=TEX||o->lock_guest||g.phase||flags&~0x3010u||(rect&&(flags&0x2000)))return D9_INVALID;
    unsigned w,h;size_t pitch,bytes;if(d9_texture_level_info(g.device,o->handle,level,&w,&h,&pitch,&bytes))return D9_INVALID;
    unsigned x=0,y=0,r=w,b=h;if(rect){if(!ptr(rect,16))return D9_INVALID;x=M32(rect);y=M32(rect+4);r=M32(rect+8);b=M32(rect+12);if(x>=r||y>=b||r>w||b>h)return D9_INVALID;}
    unsigned bb=d9_bc_block_bytes(o->format);size_t offset,row_bytes,rows;
    if(bb){if((x&3)||(y&3)||((r&3)&&r!=w)||((b&3)&&b!=h))return D9_INVALID;
        offset=(size_t)(y/4)*pitch+(x/4)*bb;row_bytes=(size_t)((r+3)/4-x/4)*bb;rows=(b+3)/4-y/4;}
    else{unsigned bpp=o->format==D9_A8?1u:4u;offset=(size_t)y*pitch+(size_t)x*bpp;
         row_bytes=(size_t)(r-x)*bpp;rows=b-y;}
    size_t span=(rows-1)*pitch+row_bytes;if(offset>bytes||span>bytes-offset||span>LOCK_STRIDE)return D9_INVALID;
    uint32_t address=lock_addr(o);if(guest_map(g.mem,address,(uint32_t)span))return D9_MEMORY;
    void *host;size_t actual;int rc=d9_texture_lock_level_ex(g.device,o->handle,level,&host,&actual,!!(flags&0x10));
    if(rc){guest_unmap(g.mem,address);return rc;}
    uint8_t *src=(uint8_t *)host+offset,*dst=ptr(address,span);
    if(!(flags&0x2000))for(size_t i=0;i<rows;++i)memcpy(dst+i*pitch,src+i*pitch,row_bytes);
    o->lock_guest=address;o->lock_host=src;o->lock_bytes=(unsigned)span;o->readonly=!!(flags&0x10);o->lock_level=level;
    o->lock_rows=(unsigned)rows;o->lock_pitch=(unsigned)pitch;o->lock_row_bytes=(unsigned)row_bytes;return D9_OK;
}
static int unlock(Obj *o){if(!o||!o->lock_guest||g.phase)return D9_INVALID;
    if(!o->readonly){uint8_t *host=o->lock_host,*src=ptr(o->lock_guest,o->lock_bytes);for(unsigned i=0;i<o->lock_rows;++i)
        memcpy(host+(size_t)i*o->lock_pitch,src+(size_t)i*o->lock_pitch,o->lock_row_bytes);}
    int rc=o->kind==TEX?(o->readonly?d9_texture_unlock_readonly(g.device,o->handle,o->lock_level):d9_texture_unlock_level(g.device,o->handle,o->lock_level)):d9_buffer_unlock(g.device,o->handle);
    if(!rc){guest_unmap(g.mem,o->lock_guest);o->lock_guest=0;o->lock_host=NULL;o->lock_bytes=0;}return rc;}
static void buffer_lock(CPU *c){Obj *o=obj(A(0),0);if(!o||(o->kind!=VB&&o->kind!=IB)||!ptr(A(3),4))RET(INVALID,5);
    M32(A(3))=0;int rc=lock_buffer(o,A(1),A(2),A(4));if(!rc)M32(A(3))=o->lock_guest;RET(hr(rc),5);}
static void buffer_unlock(CPU *c){Obj *o=obj(A(0),0);if(!o||(o->kind!=VB&&o->kind!=IB))RET(INVALID,1);RET(hr(unlock(o)),1);}
static void texture_lock(CPU *c){Obj *o=obj(A(0),TEX);uint32_t out=A(2);if(!o||!ptr(out,8))RET(INVALID,5);
    M32(out)=M32(out+4)=0;int rc=lock_texture(o,A(1),A(3),A(4));if(!rc){M32(out)=o->lock_pitch;M32(out+4)=o->lock_guest;}RET(hr(rc),5);}
static void texture_unlock(CPU *c){Obj *o=obj(A(0),TEX);if(!o||A(1)!=o->lock_level)RET(INVALID,2);RET(hr(unlock(o)),2);}
static void buffer_desc(CPU *c){Obj *o=obj(A(0),0);uint32_t p=A(1);if(!o||(o->kind!=VB&&o->kind!=IB)||!ptr(p,o->kind==VB?24:20))RET(INVALID,2);
    M32(p)=o->kind==VB?100:o->format;M32(p+4)=o->kind==VB?6:7;M32(p+8)=o->usage;M32(p+12)=o->pool;M32(p+16)=o->bytes;if(o->kind==VB)M32(p+20)=o->fvf;RET(OK,2);}
static void level_count(CPU *c){Obj *o=obj(A(0),TEX);RET(o?d9_texture_levels(g.device,o->handle):0,1);}
static void texture_desc(CPU *c){Obj *o=obj(A(0),TEX);uint32_t p=A(2);unsigned w,h;if(!o||!ptr(p,32)||d9_texture_level_info(g.device,o->handle,A(1),&w,&h,NULL,NULL))RET(INVALID,3);
    M32(p)=o->format;M32(p+4)=1;M32(p+8)=o->usage;M32(p+12)=o->pool;M32(p+16)=M32(p+20)=0;M32(p+24)=w;M32(p+28)=h;RET(OK,3);}
static void resource_type(CPU *c){Obj *o=obj(A(0),0);RET(!o?0:o->kind==VB?6:o->kind==IB?7:o->kind==TEX?3:o->kind==CUBE?5:o->kind==VOLUME?4:o->kind==SURFACE?1:0,1);}
/* IDirect3DTexture9::GetSurfaceLevel.  The returned surface is a view into the
 * parent texture's CPU level store.  Only uncompressed 32-bit levels are
 * exposed; BC levels have no 4-byte-per-pixel lock layout here. */
static void texture_get_surface_level(CPU *c){
    Obj *t=obj(A(0),TEX);uint32_t level=A(1),out=A(2);
    if(!t||!ptr(out,4))RET(INVALID,3);
    M32(out)=0;
    D9Handle rt=d9_texture_render_target(g.device,t->handle);
    if(rt){
        if(level){d3d_log("GetSurfaceLevel","a render-target texture exposes only level 0");RET(INVALID,3);}
        if(g.next>=MAX_OBJECTS)RET(OUTMEM,3);
        Obj *o=new_obj(SURFACE);if(!o)RET(OUTMEM,3);
        o->width=t->width;o->height=t->height;o->format=t->format;o->usage=t->usage;o->pool=t->pool;
        o->pitch=t->pitch;o->bytes=t->bytes;o->handle=rt;
        o->is_view=1;o->view_parent=t;o->view_texture=t->handle;o->view_level=0;
        retain(t);M32(out)=o->guest;
        trace_detail("Render-target surface=0x%08lx texture=0x%08lx target=%lu %lux%lu",
            (unsigned long)o->guest,(unsigned long)t->guest,(unsigned long)rt,
            (unsigned long)t->width,(unsigned long)t->height);
        RET(OK,3);
    }
    void *pixels=NULL;unsigned w=0,h=0;size_t pitch=0,bytes=0;
    int rc=d9_texture_level_view(g.device,t->handle,level,&pixels,&w,&h,&pitch,&bytes);
    if(rc){
        d3d_log("GetSurfaceLevel",rc==D9_UNSUPPORTED?
            "only uncompressed RGBA8/XRGB8 levels expose a CPU-backed surface view":
            "invalid texture level");
        RET(rc==D9_UNSUPPORTED?UNAVAILABLE:INVALID,3);
    }
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,3);
    Obj *o=new_obj(SURFACE);if(!o)RET(OUTMEM,3);
    o->width=w;o->height=h;o->format=t->format;o->usage=t->usage;o->pool=t->pool;
    o->pitch=(unsigned)pitch;o->bytes=(unsigned)bytes;o->pixels=pixels;
    o->is_view=1;o->view_parent=t;o->view_texture=t->handle;o->view_level=level;
    retain(t);M32(out)=o->guest;
    trace_detail("Level surface=0x%08lx texture=0x%08lx level=%lu %lux%lu pitch=%lu",
        (unsigned long)o->guest,(unsigned long)t->guest,(unsigned long)level,
        (unsigned long)w,(unsigned long)h,(unsigned long)pitch);
    RET(OK,3);
}
/* IDirect3DTexture9::AddDirtyRect is an advisory hint.  Uploads are explicit
 * (texture or level-surface unlock), so there is nothing to dirty-track; the
 * rectangle is still validated against the level-0 dimensions. */
static void texture_add_dirty_rect(CPU *c){
    Obj *t=obj(A(0),TEX);uint32_t r=A(1);
    if(!t)RET(INVALID,2);
    if(r){
        if(!ptr(r,16))RET(INVALID,2);
        int32_t v[4];memcpy(v,ptr(r,16),16);
        if(v[0]<0||v[1]<0||v[2]<=v[0]||v[3]<=v[1]||
           (uint32_t)v[2]>t->width||(uint32_t)v[3]>t->height)RET(INVALID,2);
    }
    trace_detail("AddDirtyRect%s",r?"":"(NULL)");
    RET(OK,2);
}
/* ---- IDirect3DCubeTexture9 ------------------------------------------------ */
static void cube_level_count(CPU *c){Obj *o=obj(A(0),CUBE);RET(o?d9_cube_levels(g.device,o->handle):0,1);}
static void cube_desc(CPU *c){
    Obj *o=obj(A(0),CUBE);uint32_t p=A(2);
    unsigned edge;size_t pitch,bytes;
    if(!o||!ptr(p,32)||d9_cube_level_info(g.device,o->handle,A(1),&edge,&pitch,&bytes))RET(INVALID,3);
    M32(p)=o->format;M32(p+4)=1;M32(p+8)=o->usage;M32(p+12)=o->pool;
    M32(p+16)=M32(p+20)=0;M32(p+24)=edge;M32(p+28)=edge;RET(OK,3);
}
static void cube_get_surface(CPU *c){
    Obj *t=obj(A(0),CUBE);uint32_t face=A(1),level=A(2),out=A(3);
    if(!t||face>=D9_CUBE_FACES||!ptr(out,4))RET(INVALID,4);
    M32(out)=0;
    void *pixels=NULL;unsigned w=0,h=0;size_t pitch=0,bytes=0;
    int rc=d9_cube_face_level_view(g.device,t->handle,face,level,&pixels,&w,&h,&pitch,&bytes);
    if(rc){d3d_log("GetCubeMapSurface","invalid cube face or mip level");RET(INVALID,4);}
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,4);
    Obj *o=new_obj(SURFACE);if(!o)RET(OUTMEM,4);
    o->width=w;o->height=h;o->format=t->format;o->usage=t->usage;o->pool=t->pool;
    o->pitch=(unsigned)pitch;o->bytes=(unsigned)bytes;o->pixels=pixels;
    o->block_bytes=d9_bc_block_bytes(t->format);
    o->is_view=1;o->view_cube=1;o->view_parent=t;o->view_face=face;o->view_level=level;
    retain(t);M32(out)=o->guest;
    trace_detail("Cube surface=0x%08lx cube=0x%08lx face=%lu level=%lu %lux%lu pitch=%lu",
        (unsigned long)o->guest,(unsigned long)t->guest,(unsigned long)face,
        (unsigned long)level,(unsigned long)w,(unsigned long)h,(unsigned long)pitch);
    RET(OK,4);
}
static int lock_cube_rect(Obj *o,unsigned face,unsigned level,uint32_t locked,
                          uint32_t rect,unsigned flags){
    if(!o||o->kind!=CUBE||o->lock_guest||g.phase||!ptr(locked,8))return D9_INVALID;
    if(flags&~0xf810u||(rect&&(flags&0x2000)))return D9_INVALID;
    unsigned edge;size_t pitch,bytes;void *pixels=NULL;
    if(d9_cube_face_level_info(g.device,o->handle,face,level,NULL,NULL,&pitch,&bytes)||
       d9_cube_face_level_view(g.device,o->handle,face,level,&pixels,&edge,NULL,NULL,NULL))
        return D9_INVALID;
    int32_t r[4]={0,0,(int32_t)edge,(int32_t)edge};
    if(rect){if(!ptr(rect,16))return D9_INVALID;memcpy(r,ptr(rect,16),16);}
    if(r[0]<0||r[1]<0||r[2]<=r[0]||r[3]<=r[1]||(uint64_t)r[2]>edge||(uint64_t)r[3]>edge)
        return D9_INVALID;
    unsigned x=(unsigned)r[0],y=(unsigned)r[1],bb=d9_bc_block_bytes(o->format);
    size_t row_bytes,rows,offset;
    if(bb){
        if((x&3u)||(y&3u))return D9_INVALID;
        row_bytes=(size_t)(((unsigned)r[2]-x+3u)/4u)*bb;
        rows=((unsigned)r[3]-y+3u)/4u;
        offset=(size_t)(y/4u)*pitch+(size_t)(x/4u)*bb;
    }else{
        row_bytes=(size_t)((unsigned)r[2]-x)*4u;
        rows=(unsigned)r[3]-y;
        offset=(size_t)y*pitch+(size_t)x*4u;
    }
    size_t span=(rows-1)*pitch+row_bytes;
    if(offset>bytes||span>bytes-offset||span>LOCK_STRIDE)return D9_INVALID;
    uint32_t address=lock_addr(o);if(guest_map(g.mem,address,(uint32_t)span))return D9_MEMORY;
    uint8_t *src=(uint8_t *)pixels+offset,*dst=ptr(address,(uint32_t)span);
    if(!(flags&0x2000))
        for(size_t i=0;i<rows;++i)memcpy(dst+i*pitch,src+i*pitch,row_bytes);
    o->lock_guest=address;o->lock_host=src;o->lock_bytes=(unsigned)span;
    o->readonly=!!(flags&0x10);o->lock_face=face;o->lock_level=level;
    o->lock_rows=(unsigned)rows;o->lock_pitch=(unsigned)pitch;o->lock_row_bytes=(unsigned)row_bytes;
    M32(locked)=pitch;M32(locked+4)=address;return D9_OK;
}
static void cube_lock(CPU *c){
    Obj *o=obj(A(0),CUBE);
    if(!o)RET(INVALID,6);
    int rc=lock_cube_rect(o,A(1),A(2),A(3),A(4),A(5));
    RET(hr(rc),6);
}
static void cube_unlock(CPU *c){
    Obj *o=obj(A(0),CUBE);
    if(!o||!o->lock_guest)RET(INVALID,3);
    if(!o->readonly){
        uint8_t *host=o->lock_host,*src=ptr(o->lock_guest,o->lock_bytes);
        for(unsigned i=0;i<o->lock_rows;++i)
            memcpy(host+(size_t)i*o->lock_pitch,src+(size_t)i*o->lock_pitch,o->lock_row_bytes);
    }
    unsigned face=o->lock_face,level=o->lock_level;
    guest_unmap(g.mem,o->lock_guest);o->lock_guest=0;o->lock_host=NULL;
    o->lock_bytes=o->lock_rows=o->lock_pitch=o->lock_row_bytes=0;
    int rc=o->readonly?D9_OK:d9_cube_face_unlocked(g.device,o->handle,face,level);
    RET(hr(rc),3);
}
static void cube_add_dirty_rect(CPU *c){
    Obj *o=obj(A(0),CUBE);uint32_t face=A(1),r=A(2);
    if(!o||face>=D9_CUBE_FACES)RET(INVALID,3);
    if(r){
        if(!ptr(r,16))RET(INVALID,3);
        int32_t v[4];memcpy(v,ptr(r,16),16);
        if(v[0]<0||v[1]<0||v[2]<=v[0]||v[3]<=v[1]||
           (uint32_t)v[2]>o->width||(uint32_t)v[3]>o->height)RET(INVALID,3);
    }
    RET(OK,3);
}
/* ---- IDirect3DVolumeTexture9 --------------------------------------------- */
static void volume_level_count(CPU *c){Obj *o=obj(A(0),VOLUME);RET(o?d9_volume_levels(g.device,o->handle):0,1);}
static void volume_desc(CPU *c){
    Obj *o=obj(A(0),VOLUME);uint32_t p=A(2);unsigned w,h,d;size_t pitch,plane,bytes;
    if(!o||!ptr(p,28)||d9_volume_level_info(g.device,o->handle,A(1),&w,&h,&d,
                                            &pitch,&plane,&bytes))RET(INVALID,3);
    M32(p)=o->format;M32(p+4)=4;M32(p+8)=o->usage;M32(p+12)=o->pool;
    M32(p+16)=w;M32(p+20)=h;M32(p+24)=d;RET(OK,3);
}
static void volume_get_level(CPU *c){
    uint32_t out=A(2);
    if(!obj(A(0),VOLUME))RET(INVALID,3);
    if(ptr(out,4))M32(out)=0;
    d3d_log("GetVolumeLevel","IDirect3DVolume9 is not implemented; use LockBox/UnlockBox "
            "on the volume texture");
    RET(UNAVAILABLE,3);
}
static int lock_volume_box(Obj *o,unsigned level,uint32_t locked,uint32_t box,unsigned flags){
    if(!o||o->kind!=VOLUME||o->lock_guest||g.phase||!ptr(locked,12))return D9_INVALID;
    if(flags&~0xf810u||(box&&(flags&0x2000)))return D9_INVALID;
    void *pixels=NULL;unsigned w=0,h=0,depth=0;size_t pitch=0,plane=0,bytes=0;
    if(d9_volume_level_view(g.device,o->handle,level,&pixels,&w,&h,&depth,
                            &pitch,&plane,&bytes))return D9_INVALID;
    int32_t b[6]={0,0,0,(int32_t)w,(int32_t)h,(int32_t)depth};
    if(box){
        const int32_t *p=ptr(box,24);if(!p)return D9_INVALID;memcpy(b,p,24);
    }
    if(b[0]<0||b[1]<0||b[2]<0||b[3]<=b[0]||b[4]<=b[1]||b[5]<=b[2]||
       (uint64_t)b[3]>w||(uint64_t)b[4]>h||(uint64_t)b[5]>depth)return D9_INVALID;
    unsigned x=(unsigned)b[0],y=(unsigned)b[1],z=(unsigned)b[2];
    unsigned rw=(unsigned)b[3]-x,rh=(unsigned)b[4]-y,rd=(unsigned)b[5]-z;
    unsigned bb=d9_bc_block_bytes(o->format);
    size_t row_bytes,rows,offset,slice_pitch=plane;
    size_t row_pitch=pitch;
    if(bb){
        if((x&3u)||(y&3u))return D9_INVALID;
        row_bytes=(size_t)((rw+3u)/4u)*bb;
        rows=((rh+3u)/4u);
        offset=(size_t)z*plane+(size_t)(y/4u)*pitch+(size_t)(x/4u)*bb;
    }else{
        row_bytes=(size_t)rw*4u;
        rows=rh;
        offset=(size_t)z*plane+(size_t)y*pitch+(size_t)x*4u;
    }
    size_t span=(size_t)(rd-1u)*slice_pitch+(rows-1u)*row_pitch+row_bytes;
    if(offset>bytes||span>bytes-offset||span>LOCK_STRIDE)return D9_INVALID;
    uint32_t address=lock_addr(o);
    if(guest_map(g.mem,address,(uint32_t)span))return D9_MEMORY;
    const uint8_t *src=(const uint8_t *)pixels+offset;
    uint8_t *dst=ptr(address,(uint32_t)span);
    if(!(flags&0x2000))
        for(unsigned s=0;s<rd;++s)
            for(size_t row=0;row<rows;++row)
                memcpy(dst+(size_t)s*slice_pitch+row*row_pitch,
                       src+(size_t)s*slice_pitch+row*row_pitch,row_bytes);
    o->lock_guest=address;o->lock_host=(void *)src;o->lock_bytes=(unsigned)span;
    o->readonly=!!(flags&0x10);o->lock_level=level;o->lock_rows=(unsigned)rows;
    o->lock_pitch=(unsigned)row_pitch;o->lock_row_bytes=(unsigned)row_bytes;
    o->lock_slices=rd;o->lock_slice_pitch=(unsigned)slice_pitch;o->lock_front=z;
    M32(locked)=row_pitch;M32(locked+4)=slice_pitch;M32(locked+8)=address;
    return D9_OK;
}
static void volume_lock_box(CPU *c){
    Obj *o=obj(A(0),VOLUME);
    if(!o)RET(INVALID,5);
    RET(hr(lock_volume_box(o,A(1),A(2),A(3),A(4))),5);
}
static void volume_unlock_box(CPU *c){
    Obj *o=obj(A(0),VOLUME);
    if(!o||!o->lock_guest)RET(INVALID,3);
    if(!o->readonly){
        uint8_t *host=o->lock_host,*src=ptr(o->lock_guest,o->lock_bytes);
        for(unsigned s=0;s<o->lock_slices;++s)
            for(unsigned row=0;row<o->lock_rows;++row)
                memcpy(host+(size_t)s*o->lock_slice_pitch+(size_t)row*o->lock_pitch,
                       src+(size_t)s*o->lock_slice_pitch+(size_t)row*o->lock_pitch,
                       o->lock_row_bytes);
    }
    unsigned level=o->lock_level;
    guest_unmap(g.mem,o->lock_guest);o->lock_guest=0;o->lock_host=NULL;
    o->lock_bytes=o->lock_rows=o->lock_pitch=o->lock_row_bytes=0;
    o->lock_slices=o->lock_slice_pitch=o->lock_front=0;
    int rc=o->readonly?D9_OK:d9_volume_level_unlocked(g.device,o->handle,level);
    RET(hr(rc),2);
}
static void volume_add_dirty_box(CPU *c){
    Obj *o=obj(A(0),VOLUME);uint32_t level=A(1),b=A(2);
    unsigned w,h,d;size_t pitch,plane,bytes;
    if(!o||d9_volume_level_info(g.device,o->handle,level,&w,&h,&d,&pitch,&plane,&bytes))
        RET(INVALID,3);
    if(b){
        const int32_t *p=ptr(b,24);if(!p)RET(INVALID,3);
        int32_t v[6];memcpy(v,p,24);
        if(v[0]<0||v[1]<0||v[2]<0||v[3]<=v[0]||v[4]<=v[1]||v[5]<=v[2]||
           (uint32_t)v[3]>w||(uint32_t)v[4]>h||(uint32_t)v[5]>d)RET(INVALID,3);
    }
    RET(OK,3);
}
static void surface_desc(CPU *c){Obj *o=obj(A(0),SURFACE);uint32_t p=A(1);
    if(!o||!ptr(p,32))RET(INVALID,2);
    M32(p)=o->format;M32(p+4)=1;M32(p+8)=o->usage;M32(p+12)=o->pool;
    M32(p+16)=M32(p+20)=0;M32(p+24)=o->width;M32(p+28)=o->height;RET(OK,2);
}
static void surface_container(CPU *c){Obj *o=obj(A(0),SURFACE);uint32_t out=A(2);
    if(!o||!ptr(A(1),16)||!ptr(out,4))RET(INVALID,3);M32(out)=0;
    if(o==g.back){d3d_log("GetContainer","the implicit swap-chain container is not implemented");RET(UNAVAILABLE,3);}
    if(!memcmp(ptr(A(1),16),guids[0],16)||!memcmp(ptr(A(1),16),guids[DEVICE],16)){
        retain(g.dev);M32(out)=g.dev->guest;RET(OK,3);
    }
    if(o->is_view&&!o->view_cube&&o->view_parent&&!memcmp(ptr(A(1),16),guids[TEX],16)){
        retain(o->view_parent);M32(out)=o->view_parent->guest;RET(OK,3);
    }
    if(o->is_view&&o->view_cube&&o->view_parent&&
       (!memcmp(ptr(A(1),16),guids[12],16)||!memcmp(ptr(A(1),16),guids[11],16))){
        retain(o->view_parent);M32(out)=o->view_parent->guest;RET(OK,3);
    }
    RET(NOINTERFACE,3);
}
static int lock_surface_rect(Obj *o,const int32_t r[4],unsigned flags,
                             unsigned *pitch_out,uint32_t *address_out,unsigned *rows_out){
    if(!o||o->kind!=SURFACE||!g.mem||!r||!pitch_out||!address_out||!rows_out)return D9_INVALID;
    if(o->is_depth)return D9_INVALID;
    if(o==g.back&&!o->handle)return D9_BACKEND; /* no D3DPRESENTFLAG_LOCKABLE_BACKBUFFER */
    if(o->lock_guest||g.phase)return D9_INVALID;
    /* Explicit/retained targets are readable, and lockable ones are writable. */
    if(o->handle&&(!o->lockable||(flags&~0x8010u)))return D9_INVALID;
    if(!o->handle&&(flags&~0xc810u))return D9_INVALID;
    if(r[0]<0||r[1]<0||r[2]<=r[0]||r[3]<=r[1]||(uint64_t)r[2]>o->width||(uint64_t)r[3]>o->height)return D9_INVALID;
    unsigned x=(unsigned)r[0],y=(unsigned)r[1];
    size_t row_bytes,rows,offset;
    if(o->block_bytes){
        if((x&3u)||(y&3u))return D9_INVALID;
        row_bytes=(size_t)(((unsigned)r[2]-x+3u)/4u)*o->block_bytes;
        rows=((unsigned)r[3]-y+3u)/4u;
        offset=(size_t)(y/4u)*o->pitch+(size_t)(x/4u)*o->block_bytes;
    }else{
        row_bytes=(size_t)((unsigned)r[2]-x)*4u;
        rows=(unsigned)r[3]-y;
        offset=(size_t)y*o->pitch+(size_t)x*4u;
    }
    size_t span=(rows-1)*o->pitch+row_bytes;
    if(offset>o->bytes||span>o->bytes-offset||span>LOCK_STRIDE)return D9_INVALID;
    uint32_t address=lock_addr(o);if(guest_map(g.mem,address,(uint32_t)span))return D9_MEMORY;
    if(o->handle){
        int rc=d9_render_target_read(g.device,o->handle,ptr(address,(uint32_t)span),o->pitch,x,y,r[2]-x,r[3]-y);
        if(rc){guest_unmap(g.mem,address);return rc;}
        o->lock_host=NULL;
    }else{
        uint8_t *src=o->pixels+offset,*dst=ptr(address,(uint32_t)span);
        for(unsigned i=0;i<rows;++i)memcpy(dst+(size_t)i*o->pitch,src+(size_t)i*o->pitch,row_bytes);
        o->lock_host=src;
    }
    o->lock_guest=address;o->lock_bytes=(unsigned)span;o->readonly=!!(flags&0x10);
    o->lock_rows=rows;o->lock_pitch=o->pitch;o->lock_row_bytes=row_bytes;
    o->lock_x=x;o->lock_y=y;o->lock_w=(unsigned)r[2]-x;o->lock_h=rows;
    *pitch_out=o->pitch;*address_out=address;*rows_out=rows;return D9_OK;
}
static int lock_surface(Obj *o,uint32_t locked,uint32_t rect,unsigned flags){
    if(!o||o->kind!=SURFACE||!ptr(locked,8))return D9_INVALID;
    if(rect&&(flags&0x2000))return D9_INVALID;
    int32_t r[4]={0,0,(int32_t)o->width,(int32_t)o->height};
    if(rect){if(!ptr(rect,16))return D9_INVALID;memcpy(r,ptr(rect,16),16);}
    uint32_t address=0;unsigned pitch=0,rows=0;
    int rc=lock_surface_rect(o,r,flags,&pitch,&address,&rows);
    if(!rc){M32(locked)=pitch;M32(locked+4)=address;}
    return rc;
}
static int unlock_surface(Obj *o){
    if(!o||o->kind!=SURFACE||!o->lock_guest)return D9_INVALID;
    int write_back=!o->readonly&&o->handle&&!o->is_view;
    if(!o->readonly&&!write_back){uint8_t *host=o->lock_host,*src=ptr(o->lock_guest,o->lock_bytes);
        for(unsigned i=0;i<o->lock_rows;++i)memcpy(host+(size_t)i*o->lock_pitch,src+(size_t)i*o->lock_pitch,o->lock_row_bytes);}
    int is_view=o->is_view;D9Handle view_texture=o->view_texture;unsigned view_level=o->view_level;int readonly=o->readonly;
    int is_cube_view=o->view_cube;unsigned view_face=o->view_face;Obj *view_parent=o->view_parent;
    int rc=D9_OK;
    if(write_back)
        rc=d9_render_target_write(g.device,o->handle,ptr(o->lock_guest,o->lock_bytes),o->lock_pitch,
                                  o->lock_x,o->lock_y,o->lock_w,o->lock_h);
    guest_unmap(g.mem,o->lock_guest);o->lock_guest=0;o->lock_host=NULL;o->lock_bytes=0;o->lock_rows=0;o->lock_pitch=o->lock_row_bytes=0;
    o->lock_x=o->lock_y=o->lock_w=o->lock_h=0;
    if(rc){
        d3d_log("UnlockRect",rc==D9_BACKEND?
            "the backend cannot write the render-target pixels back":
            "render-target write-back was rejected");
        return rc;
    }
    /* Writing a level view updates the parent texture's CPU store; the GPU copy
     * is re-uploaded here so a following SetTexture sees the new pixels. */
    if(is_cube_view&&!readonly&&view_parent){
        int rc2=d9_cube_face_unlocked(g.device,view_parent->handle,view_face,view_level);
        if(rc2)return rc2;
    }else if(is_view&&!readonly){
        int rc2=d9_texture_surface_unlocked(g.device,view_texture,view_level);
        if(rc2)return rc2;
    }
    return D9_OK;
}
static void surface_lock(CPU *c){Obj *o=obj(A(0),SURFACE);uint32_t out=A(1);
    if(!o||!ptr(out,8))RET(INVALID,4);M32(out)=M32(out+4)=0;
    int rc=lock_surface(o,out,A(2),A(3));
    if(rc==D9_BACKEND)d3d_log("LockRect",o->handle?
        "the backend has no readable render-target pixels":
        "the back buffer is not lockable; D3DPRESENTFLAG_LOCKABLE_BACKBUFFER was not requested");
    else if(rc==D9_INVALID&&o->is_depth)d3d_log("LockRect","depth-stencil surfaces are not lockable here");
    RET(hr(rc),4);
}
static void surface_unlock(CPU *c){Obj *o=obj(A(0),SURFACE);if(!o)RET(INVALID,1);RET(hr(unlock_surface(o)),1);}
/* Host-facing surface access used by d3dx_surface_shims.c.  Rectangles arrive
 * as host int32_t[4] and no guest stack slot is involved. */
GuestMemory *d9_com_memory(void){return g.mem;}
int d9_com_surface_info(uint32_t surface,D9ComSurfaceInfo *out){
    Obj *o=obj(surface,SURFACE);
    if(!o||!out)return -1;
    out->width=o->width;out->height=o->height;out->format=o->format;
    out->pool=o->pool;out->usage=o->usage;out->pitch=o->pitch;
    out->render_target=o->handle!=0;out->lockable=o->lockable!=0;
    return 0;
}
int d9_com_surface_lock(uint32_t surface,const int32_t rect[4],unsigned flags,
                        unsigned *pitch,uint32_t *address,unsigned *rows){
    Obj *o=obj(surface,SURFACE);
    if(!o||!pitch||!address||!rows)return D9_INVALID;
    int32_t whole[4]={0,0,(int32_t)o->width,(int32_t)o->height};
    return lock_surface_rect(o,rect?rect:whole,flags,pitch,address,rows);
}
int d9_com_surface_unlock(uint32_t surface){
    Obj *o=obj(surface,SURFACE);
    return o?unlock_surface(o):D9_INVALID;
}
static void surface_dc(CPU *c){Obj *o=obj(A(0),SURFACE);uint32_t out=A(1);
    if(!o||!ptr(out,4))RET(INVALID,2);M32(out)=0;
    d3d_log("GetDC","GDI device contexts are not implemented for D3D9 surfaces");RET(UNAVAILABLE,2);
}
static void surface_release_dc(CPU *c){if(!obj(A(0),SURFACE))RET(INVALID,2);RET(INVALID,2);}
static void clear(CPU *c){if(!obj(A(0),DEVICE)||g.phase){d3d_log("Clear","Clear is only valid outside BeginScene/EndScene");RET(INVALID,7);}
    unsigned flags=A(3),count=A(1);uint32_t rects=A(2);
    if(!flags||flags&~7u){d3d_log("Clear","invalid clear flags");RET(INVALID,7);}
    float z=0.0f;uint32_t zbits=A(5);memcpy(&z,&zbits,4);
    if(flags&6u){
        if(!g.depth||!g.depth->is_depth){
            d3d_log("Clear","depth/stencil clear requested but no depth-stencil surface is attached");RET(INVALID,7);}
        if(!isfinite(z)||z<0.0f||z>1.0f){d3d_log("Clear","z must be a finite value in [0,1]");RET(INVALID,7);}
    }
    if((count==0)!=(rects==0)||(count&&(count>(UINT32_MAX-rects)/16||!ptr(rects,(size_t)count*16)))){
        d3d_log("Clear","invalid rectangle count or pointer");RET(INVALID,7);}
    if(count){d3d_log("Clear","rectangle clears are not implemented; use a full-target clear");RET(UNAVAILABLE,7);}
    if(g.clear_pending){d3d_log("Clear","multiple Clear calls before BeginScene are not implemented");RET(UNAVAILABLE,7);}
    g.clear=A(4);g.clear_flags=flags;g.clear_z=A(5);g.clear_stencil=A(6);g.clear_pending=1;g.frame_clear=1;g.clear_target=d9_get_render_target(g.device);
    if(g.backend==d9_null_backend())d9_null_note_clear(flags);
    trace_detail("Flags=0x%lx Color=0x%08lx Rects=%lu",(unsigned long)flags,(unsigned long)g.clear,(unsigned long)count);
    RET(OK,7);
}
static void begin_scene(CPU *c){if(!obj(A(0),DEVICE)||g.phase)RET(INVALID,1);
    int queued=g.clear_pending;if(!queued)g.frame_clear=0;
    /* A depth/stencil-only Clear must not clear the colour target. */
    int color_clear=queued&&(g.clear_flags&1u);
    int rc=d9_begin_scene_ex(g.device,g.clear,color_clear);
    if(!rc&&queued&&(g.clear_flags&6u)){
        float z;memcpy(&z,&g.clear_z,4);
        rc=d9_clear_depth(g.device,g.clear_flags,z,g.clear_stencil);
        if(rc)d3d_log("BeginScene",rc==D9_UNSUPPORTED?
            "the bound depth-stencil surface does not match the active render target":
            "the backend rejected the depth/stencil clear");
    }
    if(!rc){g.phase=1;g.clear_pending=0;g.clear_target=0;
#if D9_CAPTURE_HOOK
        /* The Present latch survives BeginScene: the first captured draw of the
         * next frame reports the previous frame's Present, then clears it. */
        ++g.frame_index;g.draw_index=0;
#endif
    }else if(g.device&&g.phase==0){
        /* A rejected depth clear happens after the backend frame started; end
         * it so the BeginScene/EndScene state machine stays consistent. */
        (void)d9_end_scene(g.device);
        g.clear_pending=0;g.clear_target=0;
    }
    RET(hr(rc),1);}
static void end_scene(CPU *c){if(!obj(A(0),DEVICE)||g.phase!=1)RET(INVALID,1);g.phase=2;RET(OK,1);}
/* Device and implicit-swap-chain Present share the exact same scene state
 * machine and presentation flags; only the COM `this` object differs. */
static uint32_t present_common(CPU *c){
    if(g.phase!=2){d3d_fail("Present","Present requires EndScene",INVALID);return INVALID;}
    if(A(1)||A(2)||A(3)||A(4))return INVALID;
    int rc=d9_present(g.device);if(!rc){g.phase=0;
#if D9_CAPTURE_HOOK
        g.capture_present=1;g.capture_present_flags=A(4);
        capture_present_event();
#endif
        collect();
    }
    return hr(rc);
}
static void present(CPU *c){if(!obj(A(0),DEVICE))RET(INVALID,5);RET(present_common(c),5);}
static int transform_index(unsigned which){
    return which==256?D9_WORLD:which==2?D9_VIEW:which==3?D9_PROJECTION:-1;
}
static void transform(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(2),sizeof(D9Matrix)))RET(INVALID,3);
    int t=transform_index(A(1));if(t<0){d3d_log("SetTransform","only WORLD/VIEW/PROJECTION transforms exist");RET(INVALID,3);}
    D9Matrix m;memcpy(&m,ptr(A(2),sizeof m),sizeof m);RET(hr(d9_set_transform(g.device,t,&m)),3);}
/* ---- test-only CPU T&L state surface ------------------------------------- */
static int guest_floats(uint32_t address,float *out,unsigned count){
    size_t bytes=(size_t)count*4;
    if(!ptr(address,bytes))return -1;
    memcpy(out,ptr(address,bytes),bytes);return 0;
}
static int guest_put_floats(uint32_t address,const float *in,unsigned count){
    size_t bytes=(size_t)count*4;
    if(!ptr(address,bytes))return -1;
    memcpy(ptr(address,bytes),in,bytes);return 0;
}
static int read_material(uint32_t p,D9MaterialState *m){
    if(!ptr(p,68))return -1;
    return guest_floats(p,m->diffuse,4)||guest_floats(p+16,m->ambient,4)||
           guest_floats(p+32,m->specular,4)||guest_floats(p+48,m->emissive,4)||
           guest_floats(p+64,&m->power,1)?-1:0;
}
static int write_material(uint32_t p,const D9MaterialState *m){
    if(!ptr(p,68))return -1;
    return guest_put_floats(p,m->diffuse,4)||guest_put_floats(p+16,m->ambient,4)||
           guest_put_floats(p+32,m->specular,4)||guest_put_floats(p+48,m->emissive,4)||
           guest_put_floats(p+64,&m->power,1)?-1:0;
}
static int read_light(uint32_t p,D9LightState *l){
    if(!ptr(p,104))return -1;
    l->type=M32(p);
    return guest_floats(p+4,l->diffuse,4)||guest_floats(p+20,l->specular,4)||
           guest_floats(p+36,l->ambient,4)||guest_floats(p+52,l->position,3)||
           guest_floats(p+64,l->direction,3)||guest_floats(p+76,&l->range,1)||
           guest_floats(p+80,&l->falloff,1)||guest_floats(p+84,&l->attenuation0,1)||
           guest_floats(p+88,&l->attenuation1,1)||guest_floats(p+92,&l->attenuation2,1)||
           guest_floats(p+96,&l->theta,1)||guest_floats(p+100,&l->phi,1)?-1:0;
}
static int write_light(uint32_t p,const D9LightState *l){
    if(!ptr(p,104))return -1;
    M32(p)=l->type;
    return guest_put_floats(p+4,l->diffuse,4)||guest_put_floats(p+20,l->specular,4)||
           guest_put_floats(p+36,l->ambient,4)||guest_put_floats(p+52,l->position,3)||
           guest_put_floats(p+64,l->direction,3)||guest_put_floats(p+76,&l->range,1)||
           guest_put_floats(p+80,&l->falloff,1)||guest_put_floats(p+84,&l->attenuation0,1)||
           guest_put_floats(p+88,&l->attenuation1,1)||guest_put_floats(p+92,&l->attenuation2,1)||
           guest_put_floats(p+96,&l->theta,1)||guest_put_floats(p+100,&l->phi,1)?-1:0;
}
static void get_transform(CPU *c){
    if(!obj(A(0),DEVICE)||!ptr(A(2),sizeof(D9Matrix)))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("GetTransform","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    int t=transform_index(A(1));if(t<0){d3d_fail("GetTransform","only WORLD/VIEW/PROJECTION transforms exist",INVALID);RET(INVALID,3);}
    D9Matrix m;if(d9_get_transform(g.device,t,&m))RET(INVALID,3);
    memcpy(ptr(A(2),sizeof m),&m,sizeof m);RET(OK,3);
}
static void multiply_transform(CPU *c){
    if(!obj(A(0),DEVICE)||!ptr(A(2),sizeof(D9Matrix)))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("MultiplyTransform","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    int t=transform_index(A(1));if(t<0){d3d_fail("MultiplyTransform","only WORLD/VIEW/PROJECTION transforms exist",INVALID);RET(INVALID,3);}
    D9Matrix m;memcpy(&m,ptr(A(2),sizeof m),sizeof m);
    int rc=d9_multiply_transform(g.device,t,&m);
    if(rc){d3d_fail("MultiplyTransform","the prototype rejected the matrix",INVALID);RET(INVALID,3);}
    RET(OK,3);
}
static void set_material(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(!g.tnl_proto){d3d_fail("SetMaterial","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,2);}
    D9MaterialState m;if(read_material(A(1),&m)){
        d3d_fail("SetMaterial","invalid 68-byte D3DMATERIAL9 pointer",INVALID);RET(INVALID,2);}
    int rc=d9_set_material(g.device,&m);
    if(rc){d3d_fail("SetMaterial","material contains non-finite values or a negative power",INVALID);RET(INVALID,2);}
    RET(OK,2);
}
static void get_material(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(!g.tnl_proto){d3d_fail("GetMaterial","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,2);}
    D9MaterialState m;if(d9_get_material(g.device,&m)){
        d3d_fail("GetMaterial","invalid device state",INVALID);RET(INVALID,2);}
    if(write_material(A(1),&m)){
        d3d_fail("GetMaterial","invalid 68-byte D3DMATERIAL9 pointer",INVALID);RET(INVALID,2);}
    RET(OK,2);
}
static void set_light(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("SetLight","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    unsigned index=A(1);uint32_t p=A(2);
    if(index>=D9_TNL_MAX_LIGHTS||!ptr(p,104)){
        d3d_fail("SetLight","invalid light index or 104-byte D3DLIGHT9 pointer",INVALID);RET(INVALID,3);}
    D9LightState l;if(read_light(p,&l)){
        d3d_fail("SetLight","invalid D3DLIGHT9 pointer",INVALID);RET(INVALID,3);}
    int rc=d9_set_light(g.device,index,&l);
    if(rc==D9_UNSUPPORTED){
        d3d_fail("SetLight","only point and directional lights are implemented by the CPU T&L prototype",UNAVAILABLE);RET(UNAVAILABLE,3);}
    if(rc){d3d_fail("SetLight","invalid light type or non-finite light values",INVALID);RET(INVALID,3);}
    RET(OK,3);
}
static void get_light(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("GetLight","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    unsigned index=A(1);uint32_t p=A(2);
    if(index>=D9_TNL_MAX_LIGHTS||!ptr(p,104)){
        d3d_fail("GetLight","invalid light index or 104-byte D3DLIGHT9 pointer",INVALID);RET(INVALID,3);}
    D9LightState l;if(d9_get_light(g.device,index,&l)||write_light(p,&l)){
        d3d_fail("GetLight","invalid light state or pointer",INVALID);RET(INVALID,3);}
    RET(OK,3);
}
static void light_enable(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("LightEnable","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    unsigned index=A(1),enable=A(2)?1:0;
    if(index>=D9_TNL_MAX_LIGHTS){d3d_fail("LightEnable","invalid light index",INVALID);RET(INVALID,3);}
    if(d9_set_light_enable(g.device,index,(int)enable))RET(INVALID,3);
    RET(OK,3);
}
static void get_light_enable(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,3);
    if(!g.tnl_proto){d3d_fail("GetLightEnable","the CPU T&L prototype is disabled (TESV_D3D9_TNL_PROTO)",UNAVAILABLE);RET(UNAVAILABLE,3);}
    unsigned index=A(1);uint32_t out=A(2);
    if(index>=D9_TNL_MAX_LIGHTS||!ptr(out,4)){d3d_fail("GetLightEnable","invalid light index or output pointer",INVALID);RET(INVALID,3);}
    int enable=0;if(d9_get_light_enable(g.device,index,&enable))RET(INVALID,3);
    M32(out)=enable?1u:0u;RET(OK,3);
}
/* ---- legacy miscellaneous device state -------------------------------- */
static void set_clip_plane(CPU *c){
    unsigned index=A(1);uint32_t p=A(2);float plane[4];
    if(!obj(A(0),DEVICE)||index>=D9_MAX_CLIP_PLANES||!ptr(p,16)){
        d3d_fail("SetClipPlane","only clip planes 0..5 and a readable 4-float pointer are implemented",INVALID);
        RET(INVALID,3);
    }
    memcpy(plane,ptr(p,16),16);
    if(d9_set_clip_plane(g.device,index,plane)){
        d3d_fail("SetClipPlane","clip plane values must be finite",INVALID);RET(INVALID,3);
    }
    g.last_diagnostic[0]=0;RET(OK,3);
}
static void get_clip_plane(CPU *c){
    unsigned index=A(1);uint32_t p=A(2);float plane[4];
    if(!obj(A(0),DEVICE)||index>=D9_MAX_CLIP_PLANES||!ptr(p,16)){
        d3d_fail("GetClipPlane","only clip planes 0..5 and a writable 4-float pointer are implemented",INVALID);
        RET(INVALID,3);
    }
    if(d9_get_clip_plane(g.device,index,plane))RET(INVALID,3);
    memcpy(ptr(p,16),plane,16);RET(OK,3);
}
static void set_clip_status(CPU *c){
    uint32_t p=A(1);
    if(!obj(A(0),DEVICE)||!ptr(p,8)){
        d3d_fail("SetClipStatus","invalid device or 8-byte D3DCLIPSTATUS9 pointer",INVALID);RET(INVALID,2);
    }
    memcpy(&g.clip_status_union,ptr(p,4),4);memcpy(&g.clip_status_intersection,ptr(p+4,4),4);
    g.last_diagnostic[0]=0;RET(OK,2);
}
static void get_clip_status(CPU *c){
    uint32_t p=A(1);
    if(!obj(A(0),DEVICE)||!ptr(p,8)){
        d3d_fail("GetClipStatus","invalid device or 8-byte D3DCLIPSTATUS9 pointer",INVALID);RET(INVALID,2);
    }
    memcpy(ptr(p,4),&g.clip_status_union,4);memcpy(ptr(p+4,4),&g.clip_status_intersection,4);
    RET(OK,2);
}
static void set_software_vp(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    if(A(1)){
        d3d_fail("SetSoftwareVertexProcessing","software vertex processing is not implemented; the bridge is hardware-only",UNAVAILABLE);
        RET(UNAVAILABLE,2);
    }
    RET(OK,2);
}
static void get_software_vp(CPU *c){
    if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);
    M32(A(1))=0;RET(OK,2); /* the bridge always advertises hardware processing */
}
/* ---- cursor and legacy palettes ---------------------------------------- */
static void set_cursor_properties(CPU *c){
    uint32_t surface_address=A(3);
    if(!obj(A(0),DEVICE)||!surface_address){
        d3d_fail("SetCursorProperties","invalid device or NULL cursor surface",INVALID);RET(INVALID,4);
    }
    Obj *s=obj(surface_address,SURFACE);
    if(!s){d3d_fail("SetCursorProperties","cursor surface is not a surface object",INVALID);RET(INVALID,4);}
    if(s->is_depth||s->handle||!s->pixels){
        d3d_fail("SetCursorProperties","cursor surface must be CPU-backed, not a render target or depth surface",UNAVAILABLE);
        RET(UNAVAILABLE,4);
    }
    if(A(1)>=s->width||A(2)>=s->height){
        d3d_fail("SetCursorProperties","hotspot is outside the cursor surface",INVALID);RET(INVALID,4);
    }
    g.cursor_hotspot_x=(int32_t)A(1);g.cursor_hotspot_y=(int32_t)A(2);
    g.cursor_surface=surface_address; /* no ownership: the cursor is not drawn */
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_cursor_position(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,4);
    if(A(3)&~1u){
        d3d_fail("SetCursorPosition","only D3DCURSOR_IMMEDIATE_UPDATE is recognized",INVALID);RET(INVALID,4);
    }
    g.cursor_x=(int32_t)A(1);g.cursor_y=(int32_t)A(2);
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void show_cursor(CPU *c){
    if(!obj(A(0),DEVICE))RET(INVALID,2);
    unsigned previous=g.cursor_shown;
    g.cursor_shown=A(1)?1u:0u;
    RET(previous,2);
}
static void set_palette_entries(CPU *c){
    unsigned number=A(1);uint32_t entries=A(2);
    if(!obj(A(0),DEVICE)||number>=MAX_PALETTES||!ptr(entries,PALETTE_BYTES)){
        d3d_fail("SetPaletteEntries","palette number 0..255 and a 256-entry pointer are required",INVALID);
        RET(INVALID,3);
    }
    if(!g.palettes[number]){
        g.palettes[number]=malloc(PALETTE_BYTES);
        if(!g.palettes[number])RET(OUTMEM,3);
    }
    memcpy(g.palettes[number],ptr(entries,PALETTE_BYTES),PALETTE_BYTES);
    g.last_diagnostic[0]=0;RET(OK,3);
}
static void get_palette_entries(CPU *c){
    unsigned number=A(1);uint32_t entries=A(2);
    if(!obj(A(0),DEVICE)||number>=MAX_PALETTES||!ptr(entries,PALETTE_BYTES)){
        d3d_fail("GetPaletteEntries","palette number 0..255 and a 256-entry pointer are required",INVALID);
        RET(INVALID,3);
    }
    if(g.palettes[number])memcpy(ptr(entries,PALETTE_BYTES),g.palettes[number],PALETTE_BYTES);
    else memset(ptr(entries,PALETTE_BYTES),0,PALETTE_BYTES);
    RET(OK,3);
}
static void set_current_palette(CPU *c){
    unsigned number=A(1);
    if(!obj(A(0),DEVICE)||number>=MAX_PALETTES){
        d3d_fail("SetCurrentTexturePalette","palette number must be in 0..255",INVALID);RET(INVALID,2);
    }
    g.current_palette=number;RET(OK,2);
}
static void get_current_palette(CPU *c){
    if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);
    M32(A(1))=g.current_palette;RET(OK,2);
}
static void set_fvf(CPU *c){if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,2);
    unsigned fvf=A(1);const char *why=NULL;D9Decl decl;
    if(d9_decl_from_fvf(fvf,g.tnl_proto,&decl,&why)){
        d3d_fail("SetFVF",why?why:"FVF is not an admitted XYZ|TEX1/DIFFUSE variant",UNAVAILABLE);RET(UNAVAILABLE,2);
    }
    if(d9_set_vertex_declaration(g.device,&decl)){d3d_fail("SetFVF","declaration rejected by the core",INVALID);RET(INVALID,2);}
    bind(&g.declaration,NULL);g.fvf=fvf;g.last_diagnostic[0]=0;RET(OK,2);}
static void get_fvf(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);M32(A(1))=g.fvf;RET(OK,2);}
static void set_render(CPU *c){if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,3);
    int rc=d9_set_render_state(g.device,A(1),A(2));
    if(rc==D9_UNSUPPORTED){
        d3d_fail("SetRenderState",d9_last_draw_diagnostic(g.device),UNAVAILABLE);RET(UNAVAILABLE,3);}
    RET(hr(rc),3);}
static void get_render(CPU *c){unsigned v;if(!obj(A(0),DEVICE)||!ptr(A(2),4)||d9_get_render_state(g.device,A(1),&v))RET(INVALID,3);M32(A(2))=v;RET(OK,3);}
static void set_sampler(CPU *c){if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,4);RET(hr(d9_set_unit_sampler(g.device,A(1),A(2),A(3))),4);}
/* D3D9 IDirect3DDevice9 slot 67.  The GFx UI batch uses stage 0
 * MODULATE(TEXTURE,DIFFUSE); the value is stored even when the current
 * backend has no vertex-colour shader, so the draw gate is the only failure. */
static void set_texture_stage_state(CPU *c){
    if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,4);
    int rc=d9_set_texture_stage_state(g.device,A(1),A(2),A(3));
    if(rc){d3d_fail("SetTextureStageState",d9_last_draw_diagnostic(g.device),hr(rc));RET(hr(rc),4);}
    g.last_diagnostic[0]=0;
    RET(OK,4);
}
static void get_sampler(CPU *c){unsigned v;if(!obj(A(0),DEVICE)||!ptr(A(3),4)||d9_get_unit_sampler(g.device,A(1),A(2),&v))RET(INVALID,4);M32(A(3))=v;RET(OK,4);}
static void get_texture_stage_state(CPU *c){unsigned v;if(!obj(A(0),DEVICE)||!ptr(A(3),4)||d9_get_texture_stage_state(g.device,A(1),A(2),&v))RET(INVALID,4);M32(A(3))=v;RET(OK,4);}
static void stream(CPU *c){Obj *o=obj(A(2),VB);unsigned s=A(1);if(!obj(A(0),DEVICE)||s>=D9_MAX_STREAMS||(A(2)&&!o)||g.phase==2)RET(INVALID,5);
    int rc=d9_set_stream(g.device,s,o?o->handle:0,A(3),A(4));if(!rc){bind(&g.streams[s],o);g.offsets[s]=A(3);g.strides[s]=A(4);}RET(hr(rc),5);}
static void get_stream(CPU *c){unsigned s=A(1);if(!obj(A(0),DEVICE)||s>=D9_MAX_STREAMS||!ptr(A(2),4)||!ptr(A(3),4)||!ptr(A(4),4))RET(INVALID,5);
    Obj *o=g.streams[s];retain(o);M32(A(2))=o?o->guest:0;M32(A(3))=g.offsets[s];M32(A(4))=g.strides[s];RET(OK,5);}
static void stream_freq(CPU *c){if(!obj(A(0),DEVICE)||A(1)>=D9_MAX_STREAMS||A(2)!=1||g.phase==2)RET(UNAVAILABLE,3);RET(OK,3);}
static void get_stream_freq(CPU *c){if(!obj(A(0),DEVICE)||A(1)>=D9_MAX_STREAMS||!ptr(A(2),4))RET(INVALID,3);M32(A(2))=1;RET(OK,3);}
static void indices(CPU *c){Obj *o=obj(A(1),IB);if(!obj(A(0),DEVICE)||(A(1)&&!o)||g.phase==2)RET(INVALID,2);
    int rc=d9_set_indices(g.device,o?o->handle:0);if(!rc)bind(&g.ib,o);RET(hr(rc),2);}
static void get_indices(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);retain(g.ib);M32(A(1))=g.ib?g.ib->guest:0;RET(OK,2);}
static uint32_t bind_texture_obj(unsigned stage,Obj *o){
    int rc;
    if(o&&o->kind==CUBE)rc=d9_bind_cube_texture(g.device,stage,o->handle);
    else if(o&&o->kind==VOLUME)rc=d9_bind_volume_texture(g.device,stage,o->handle);
    else rc=d9_bind_texture(g.device,stage,o?o->handle:0);
    if(rc)return rc;
    if(o&&o->kind==CUBE){
        bind(stage?&g.cube1:&g.cube,o);bind(stage?&g.tex1:&g.tex,NULL);bind(stage?&g.volume1:&g.volume,NULL);
    }else if(o&&o->kind==VOLUME){
        bind(stage?&g.volume1:&g.volume,o);bind(stage?&g.tex1:&g.tex,NULL);bind(stage?&g.cube1:&g.cube,NULL);
    }else{
        bind(stage?&g.tex1:&g.tex,o);bind(stage?&g.cube1:&g.cube,NULL);bind(stage?&g.volume1:&g.volume,NULL);
    }
    return D9_OK;
}
static void texture(CPU *c){
    uint32_t addr=A(2);Obj *o=NULL;
    if(addr){o=obj(addr,TEX);if(!o)o=obj(addr,CUBE);if(!o)o=obj(addr,VOLUME);}
    if(!obj(A(0),DEVICE)||(addr&&!o)||g.phase==2)RET(INVALID,3);
    int rc=bind_texture_obj(A(1),o);
    if(rc==D9_UNSUPPORTED)d3d_log("SetTexture","unsupported texture unit or non-2D sampling");
    RET(hr(rc),3);}
static void get_texture(CPU *c){
    uint32_t out=A(2);
    if(!obj(A(0),DEVICE)||!ptr(out,4))RET(INVALID,3);
    Obj *o=A(1)?g.tex1:g.tex;
    if(!o)o=A(1)?g.cube1:g.cube;
    if(!o)o=A(1)?g.volume1:g.volume;
    retain(o);M32(out)=o?o->guest:0;RET(OK,3);
}
/* ---- bounded state blocks --------------------------------------------------
 * The block stores only state the bridge really exposes.  Shaders, shader
 * constants, render/depth targets, clip planes/status, gamma, NPatch and
 * palettes are deliberately outside this subset and are not half-captured. */
enum {SB_PIXEL=1u,SB_VERTEX=2u};
static unsigned state_block_mask(unsigned type){
    return type==1?SB_PIXEL|SB_VERTEX:type==2?SB_PIXEL:type==3?SB_VERTEX:0;
}
static void state_block_release_refs(D9ComStateBlock *sb){
    if(!sb)return;
    for(unsigned i=0;i<2;++i)release(sb->tex[i]);
    release(sb->decl);release(sb->ib);
    for(unsigned i=0;i<D9_MAX_STREAMS;++i)release(sb->streams[i]);
    memset(sb,0,sizeof *sb);
}
static int state_block_same_obj(Obj *o,unsigned kind){
    return !o||obj(o->guest,kind)==o;
}
static uint32_t state_block_capture(Obj *block){
    if(!block||block->kind!=STATEBLOCK||!block->state_block||!g.device||g.phase==2)
        return INVALID;
    D9ComStateBlock tmp={0};tmp.type=block->state_block->type;
    unsigned mask=state_block_mask(tmp.type);
    if(!mask)return UNAVAILABLE;
    if(mask&SB_PIXEL){
        static const unsigned ids[]={7,14,15,27,19,20,22,23,24,25,168,171};
        for(unsigned i=0;i<sizeof ids/sizeof *ids;++i){
            unsigned v;
            if(d9_get_render_state(g.device,ids[i],&v)){
                d3d_fail("Capture","a supported render state could not be read",UNAVAILABLE);
                return UNAVAILABLE;
            }
            tmp.rs_ids[tmp.rs_count]=ids[i];tmp.rs_values[tmp.rs_count++]=v;
        }
        static const unsigned sampler_ids[]={1,2,5,6,7,9};
        static const unsigned tss_ids[]={D9_TSS_COLOROP,D9_TSS_COLORARG1,D9_TSS_COLORARG2,
            D9_TSS_ALPHAOP,D9_TSS_ALPHAARG1,D9_TSS_ALPHAARG2,D9_TSS_TEXCOORDINDEX};
        unsigned stages=d9_stage_count(g.device);
        for(unsigned s=0;s<stages;++s){
            for(unsigned i=0;i<sizeof sampler_ids/sizeof *sampler_ids;++i){
                unsigned v;
                if(d9_get_unit_sampler(g.device,s,sampler_ids[i],&v)){
                    d3d_fail("Capture","a supported sampler state could not be read",UNAVAILABLE);
                    return UNAVAILABLE;
                }
                tmp.sampler_stage[tmp.sampler_count]=(uint8_t)s;
                tmp.sampler_id[tmp.sampler_count]=(uint8_t)sampler_ids[i];
                tmp.sampler_values[tmp.sampler_count++]=v;
            }
            for(unsigned i=0;i<sizeof tss_ids/sizeof *tss_ids;++i){
                unsigned v;
                if(d9_get_texture_stage_state(g.device,s,tss_ids[i],&v)){
                    d3d_fail("Capture","a supported texture-stage state could not be read",UNAVAILABLE);
                    return UNAVAILABLE;
                }
                tmp.tss_stage[tmp.tss_count]=(uint8_t)s;
                tmp.tss_id[tmp.tss_count]=(uint8_t)tss_ids[i];
                tmp.tss_values[tmp.tss_count++]=v;
            }
        }
        for(unsigned s=0;s<2;++s){
            Obj *o=s?g.tex1:g.tex;
            if(!o)o=s?g.cube1:g.cube;
            if(!o)o=s?g.volume1:g.volume;
            unsigned kinds=(s?g.tex1!=NULL:g.tex!=NULL)+(s?g.cube1!=NULL:g.cube!=NULL)+
                           (s?g.volume1!=NULL:g.volume!=NULL);
            if(kinds>1){d3d_fail("Capture","multiple texture bindings exist for one stage",UNAVAILABLE);return UNAVAILABLE;}
            tmp.tex[s]=o;
        }
    }
    if(mask&SB_VERTEX){
        tmp.fvf=g.fvf;tmp.decl=g.declaration;
        for(unsigned i=0;i<D9_MAX_STREAMS;++i){
            D9Handle h=0;size_t offset=0,stride=0;
            if(d9_get_stream_info(g.device,i,&h,&offset,&stride)||
               h!=(g.streams[i]?g.streams[i]->handle:0)||offset!=g.offsets[i]||stride!=g.strides[i]){
                d3d_fail("Capture","stream source state is inconsistent",UNAVAILABLE);return UNAVAILABLE;
            }
            tmp.streams[i]=g.streams[i];tmp.offsets[i]=g.offsets[i];tmp.strides[i]=g.strides[i];
        }
        D9Handle ib=0;unsigned format=0;
        if(d9_get_index_info(g.device,&ib,&format)||ib!=(g.ib?g.ib->handle:0)){
            d3d_fail("Capture","index-buffer state is inconsistent",UNAVAILABLE);return UNAVAILABLE;
        }
        tmp.ib=g.ib;
        for(unsigned i=0;i<3;++i)if(d9_get_transform(g.device,i,&tmp.transforms[i])){
            d3d_fail("Capture","a supported transform could not be read",UNAVAILABLE);return UNAVAILABLE;
        }
        memcpy(tmp.viewport,g.viewport,sizeof tmp.viewport);
        if(d9_get_tnl_state(g.device,&tmp.tnl)){
            d3d_fail("Capture","fixed-function state could not be read",UNAVAILABLE);return UNAVAILABLE;
        }
        memcpy(tmp.scissor,g.scissor,sizeof tmp.scissor);
    }
    for(unsigned i=0;i<2;++i){
        if(tmp.tex[i]&&(!state_block_same_obj(tmp.tex[i],tmp.tex[i]->kind)||tmp.tex[i]->lock_guest)){
            d3d_fail("Capture","a bound texture is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
        }
    }
    if(tmp.decl&&!state_block_same_obj(tmp.decl,DECL)){
        d3d_fail("Capture","the vertex declaration is invalid",UNAVAILABLE);return UNAVAILABLE;
    }
    if(tmp.ib&&(!state_block_same_obj(tmp.ib,IB)||tmp.ib->lock_guest)){
        d3d_fail("Capture","the index buffer is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
    }
    for(unsigned i=0;i<D9_MAX_STREAMS;++i){
        if(tmp.streams[i]&&(!state_block_same_obj(tmp.streams[i],VB)||tmp.streams[i]->lock_guest)){
            d3d_fail("Capture","a stream buffer is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
        }
    }
    for(unsigned i=0;i<2;++i)retain(tmp.tex[i]);
    retain(tmp.decl);retain(tmp.ib);
    for(unsigned i=0;i<D9_MAX_STREAMS;++i)retain(tmp.streams[i]);
    state_block_release_refs(block->state_block);
    *block->state_block=tmp;block->state_block->captured=1;
    g.last_diagnostic[0]=0;return OK;
}
static uint32_t state_block_new(unsigned type,int capture,Obj **out){
    if(out)*out=NULL;
    if(!g.device||g.phase==2)return INVALID;
    if(!state_block_mask(type)){
        d3d_fail("CreateStateBlock","unsupported D3DSTATEBLOCKTYPE",UNAVAILABLE);return UNAVAILABLE;
    }
    if(g.next>=MAX_OBJECTS)return OUTMEM;
    Obj *o=new_obj(STATEBLOCK);if(!o)return OUTMEM;
    o->state_block=calloc(1,sizeof *o->state_block);if(!o->state_block){release(o);return OUTMEM;}
    o->state_block->type=type;retain(g.dev);
    if(capture){
        uint32_t rc=state_block_capture(o);
        if(rc){release(o);return rc;}
    }
    *out=o;return OK;
}
static uint32_t state_block_apply(Obj *block){
    if(!block||block->kind!=STATEBLOCK||!block->state_block||!block->state_block->captured||
       !g.device||g.phase==2)return INVALID;
    const D9ComStateBlock *sb=block->state_block;unsigned mask=state_block_mask(sb->type);
    for(unsigned i=0;i<2;++i){
        if(sb->tex[i]&&(!state_block_same_obj(sb->tex[i],sb->tex[i]->kind)||sb->tex[i]->lock_guest)){
            d3d_fail("Apply","a captured texture is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
        }
        if(sb->tex[i]&&i>=d9_stage_count(g.device)){
            d3d_fail("Apply","the captured texture stage is unavailable",UNAVAILABLE);return UNAVAILABLE;
        }
    }
    if(sb->decl&&!state_block_same_obj(sb->decl,DECL)){
        d3d_fail("Apply","the captured declaration is invalid",UNAVAILABLE);return UNAVAILABLE;
    }
    if(sb->ib&&(!state_block_same_obj(sb->ib,IB)||sb->ib->lock_guest)){
        d3d_fail("Apply","the captured index buffer is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
    }
    for(unsigned i=0;i<D9_MAX_STREAMS;++i){
        if(sb->streams[i]&&(!state_block_same_obj(sb->streams[i],VB)||sb->streams[i]->lock_guest)){
            d3d_fail("Apply","a captured stream buffer is invalid or locked",UNAVAILABLE);return UNAVAILABLE;
        }
    }
    if(mask&SB_VERTEX){
        unsigned w=g.rt?g.rt->width:g.pp[0],h=g.rt?g.rt->height:g.pp[1];
        if(sb->viewport[0]||sb->viewport[1]||sb->viewport[2]!=w||sb->viewport[3]!=h||
           sb->scissor[0]>=sb->scissor[2]||sb->scissor[1]>=sb->scissor[3]||
           sb->scissor[2]>w||sb->scissor[3]>h){
            d3d_fail("Apply","captured viewport/scissor does not fit the current render target",UNAVAILABLE);
            return UNAVAILABLE;
        }
        if(!sb->decl&&sb->fvf){
            D9Decl decl;const char *why=NULL;
            if(d9_decl_from_fvf(sb->fvf,g.tnl_proto,&decl,&why)){
                d3d_fail("Apply",why?why:"captured FVF is no longer supported",UNAVAILABLE);return UNAVAILABLE;
            }
        }
    }
    if(mask&SB_PIXEL){
        for(unsigned i=0;i<sb->rs_count;++i){
            int rc=d9_set_render_state(g.device,sb->rs_ids[i],sb->rs_values[i]);
            if(rc){d3d_fail("Apply","render-state restore failed",hr(rc));return hr(rc);}
        }
        for(unsigned i=0;i<sb->sampler_count;++i){
            int rc=d9_set_unit_sampler(g.device,sb->sampler_stage[i],sb->sampler_id[i],sb->sampler_values[i]);
            if(rc){d3d_fail("Apply","sampler-state restore failed",hr(rc));return hr(rc);}
        }
        for(unsigned i=0;i<sb->tss_count;++i){
            int rc=d9_set_texture_stage_state(g.device,sb->tss_stage[i],sb->tss_id[i],sb->tss_values[i]);
            if(rc){d3d_fail("Apply","texture-stage-state restore failed",hr(rc));return hr(rc);}
        }
        for(unsigned s=0;s<2;++s){
            Obj *o=sb->tex[s];int rc=bind_texture_obj(s,o);
            if(rc){d3d_fail("Apply","texture-binding restore failed",hr(rc));return hr(rc);}
        }
    }
    if(mask&SB_VERTEX){
        if(sb->decl){
            if(d9_set_vertex_declaration(g.device,sb->decl->declaration)){
                d3d_fail("Apply","declaration restore failed",INVALID);return INVALID;
            }
            bind(&g.declaration,sb->decl);g.fvf=0;
        }else if(sb->fvf){
            D9Decl decl;const char *why=NULL;
            if(d9_decl_from_fvf(sb->fvf,g.tnl_proto,&decl,&why)||d9_set_vertex_declaration(g.device,&decl)){
                d3d_fail("Apply","FVF restore failed",UNAVAILABLE);return UNAVAILABLE;
            }
            bind(&g.declaration,NULL);g.fvf=sb->fvf;
        }else{
            (void)d9_set_vertex_declaration(g.device,NULL);bind(&g.declaration,NULL);g.fvf=0;
        }
        for(unsigned i=0;i<D9_MAX_STREAMS;++i){
            int rc=d9_set_stream(g.device,i,sb->streams[i]?sb->streams[i]->handle:0,sb->offsets[i],sb->strides[i]);
            if(rc){d3d_fail("Apply","stream-source restore failed",hr(rc));return hr(rc);}
            bind(&g.streams[i],sb->streams[i]);g.offsets[i]=sb->offsets[i];g.strides[i]=sb->strides[i];
        }
        if(d9_set_indices(g.device,sb->ib?sb->ib->handle:0)){
            d3d_fail("Apply","index-buffer restore failed",INVALID);return INVALID;
        }
        bind(&g.ib,sb->ib);
        for(unsigned i=0;i<3;++i){
            if(d9_set_transform(g.device,i,&sb->transforms[i])){
                d3d_fail("Apply","transform restore failed",INVALID);return INVALID;
            }
        }
        memcpy(g.viewport,sb->viewport,sizeof g.viewport);
        if(d9_set_viewport_size(g.device,sb->viewport[2],sb->viewport[3])){
            d3d_fail("Apply","viewport restore failed",INVALID);return INVALID;
        }
        if(d9_set_tnl_state(g.device,&sb->tnl)){
            d3d_fail("Apply","fixed-function state restore failed",INVALID);return INVALID;
        }
        memcpy(g.scissor,sb->scissor,sizeof g.scissor);
    }
    g.last_diagnostic[0]=0;return OK;
}
static void create_state_block(CPU *c){
    uint32_t out=A(2);if(!obj(A(0),DEVICE)||!ptr(out,4))RET(INVALID,3);M32(out)=0;
    Obj *block=NULL;uint32_t rc=state_block_new(A(1),1,&block);
    if(!rc)M32(out)=block->guest;RET(rc,3);
}
static void begin_state_block(CPU *c){
    if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,1);
    if(g.recording_block){d3d_fail("BeginStateBlock","a state block is already being recorded",INVALID);RET(INVALID,1);}
    Obj *block=NULL;uint32_t rc=state_block_new(1,0,&block);
    if(!rc)g.recording_block=block;RET(rc,1);
}
static void end_state_block(CPU *c){
    uint32_t out=A(1);
    if(!obj(A(0),DEVICE)||!ptr(out,4))RET(INVALID,2);
    M32(out)=0;
    if(!g.recording_block){d3d_fail("EndStateBlock","no state block is being recorded",INVALID);RET(INVALID,2);}
    uint32_t rc=state_block_capture(g.recording_block);
    if(rc){Obj *record=g.recording_block;g.recording_block=NULL;release(record);RET(rc,2);}
    M32(out)=g.recording_block->guest;g.recording_block=NULL;RET(OK,2);
}
static void state_block_capture_com(CPU *c){
    Obj *o=obj(A(0),STATEBLOCK);if(!o||g.phase==2)RET(INVALID,1);RET(state_block_capture(o),1);
}
static void state_block_apply_com(CPU *c){
    Obj *o=obj(A(0),STATEBLOCK);if(!o||g.phase==2)RET(INVALID,1);RET(state_block_apply(o),1);
}
/* ---- menu VS / material evidence ---------------------------------------- */
/* The menu path does not guess normal/tangent availability.  It derives only
 * the bits the device can actually observe: bound LDR textures, the current
 * declaration/FVF, and the constants the guest has really uploaded.  Normal,
 * tangent and ambient+emissive evidence remains explicit-only. */
static const D9Decl *menu_current_decl(D9Decl *tmp){
    if(g.declaration&&g.declaration->declaration)return g.declaration->declaration;
    if(g.fvf){const char *why=NULL;if(!d9_decl_from_fvf(g.fvf,0,tmp,&why))return tmp;}
    return NULL;
}
static int menu_decl_has(const D9Decl *decl,unsigned usage,unsigned index){
    if(!decl)return 0;
    for(unsigned i=0;i<decl->count;++i)
        if(decl->elements[i].usage==usage&&decl->elements[i].usage_index==index)return 1;
    return 0;
}
static int menu_texture_ldr(const Obj *t){
    if(!t||!t->width||!t->height)return 0;
    return t->format==D9_A8R8G8B8||t->format==D9_X8R8G8B8||
           t->format==D9_DXT1||t->format==D9_DXT3||t->format==D9_DXT5;
}
static uint32_t menu_observed_preconditions(void){
    uint32_t mask=0;D9Decl tmp;const D9Decl *decl=menu_current_decl(&tmp);
    if(menu_texture_ldr(g.tex)){
        mask|=D9_MENU_SHADER_PRE_TEXTURE0_2D;
        if(!g.tex1||menu_texture_ldr(g.tex1))mask|=D9_MENU_SHADER_PRE_LDR_QUALITY;
    }
    if(menu_texture_ldr(g.tex1))mask|=D9_MENU_SHADER_PRE_TEXTURE1_2D;
    if(menu_decl_has(decl,D9_USAGE_COLOR,0))mask|=D9_MENU_SHADER_PRE_VERTEX_COLOR;
    uint32_t ps_mask=g.device?d9_ps_constants_f_mask(g.device):0u;
    if(((g.constants_mask|ps_mask)&0xFFu)==0xFFu)
        mask|=D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7;
    float alpha=0.0f;int have_alpha=0;
    if(ps_mask&(1u<<4)){
        if(!d9_get_ps_constants_f(g.device,4,&alpha,1))have_alpha=1;
    }else if(g.constants_mask&(1u<<4)){
        alpha=g.constants_hi.m[0][3];have_alpha=1;
    }
    if(have_alpha&&isfinite(alpha)&&alpha>=0.0f&&alpha<=1.0f)
        mask|=D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4;
    if(g.constants_mask&(1u<<8))
        mask|=D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING;
    return mask;
}
uint32_t d9_com_menu_observed_preconditions(void){return menu_observed_preconditions();}
uint32_t d9_com_menu_set_ambient_emissive(const float rgba[4]){
    if(!rgba||!g.device)return INVALID;
    for(unsigned i=0;i<4;++i)if(!isfinite(rgba[i]))return INVALID;
    memcpy(g.constants_c8,rgba,16);g.constants_mask|=1u<<8;
    upload_tev_constants();
    d9_menu_shader_note_preconditions(D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING);
    return OK;
}
static int menu_vs_state(void){
    char why[D9_MENU_SHADER_REASON_MAX];
    if(!g.vs||!g.vs->has_menu_vs)return 0;
    D9Decl tmp;const D9Decl *decl=menu_current_decl(&tmp);
    if(!decl){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
                 "%s: no vertex declaration or FVF is bound",g.vs->menu.id);
        return D9_UNSUPPORTED;
    }
    if(d9_menu_shader_vs_check_decl(&g.vs->menu,decl,why,sizeof why)){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s: %s",
                 g.vs->menu.id,why);
        return D9_UNSUPPORTED;
    }
    if(!g.vs->menu_backend_ok){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s: %s",
                 g.vs->menu.id,g.vs->menu_backend_why);
        return D9_UNSUPPORTED;
    }
    return 0;
}
static int menu_ps_state(void){
    char why[D9_MENU_SHADER_REASON_MAX];
    if(!g.ps||!g.ps->has_menu)return 0;
    uint32_t have=d9_menu_shader_preconditions()|menu_observed_preconditions();
    if(d9_menu_shader_missing_preconditions(&g.ps->menu,have,why,sizeof why)){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
                 "%s: missing preconditions: %s",g.ps->menu.id,why);
        return D9_UNSUPPORTED;
    }
    if(!g.ps->menu_backend_ok){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s: %s",
                 g.ps->menu.id,g.ps->menu_backend_why);
        return D9_UNSUPPORTED;
    }
    return 0;
}
static uint32_t shader_probe_object(unsigned kind,uint32_t out,const char *reason,
                                    uint32_t *code,size_t words){
    if(!code||!words)return INVALID;
    if(g.next>=MAX_OBJECTS){free(code);return OUTMEM;}
    Obj *o=new_obj(kind);if(!o){free(code);return OUTMEM;}
    /* The request asked for a named, non-executable object: keep the original
     * token stream only so GetFunction can report it, but install no menu
     * match, no TEV program and no draw plan. */
    o->probe_skip=1;o->code=code;o->words=(uint32_t)words;o->has_menu=0;o->has_tev=0;
    retain(g.dev);M32(out)=o->guest;
    if(!g.shader_probe_logged){
        g.shader_probe_logged=1;
        fprintf(stderr,"[d3d9] MEASUREMENT ONLY shader-skip probe active: %s (%s); "
                       "non-executable shader object created, draws will be refused\n",
                kind==VS?"CreateVertexShader":"CreatePixelShader",
                reason&&*reason?reason:"shader outside the certified closure");
    }
    return OK;
}
static uint32_t shader_new(unsigned kind,uint32_t address,uint32_t out){
    if(!address||(address&3)||!ptr(out,4)||g.next>=MAX_OBJECTS)return INVALID;M32(out)=0;
    size_t count=0;for(size_t i=0;i<g.mem->count;++i){GuestRegion *r=&g.mem->regions[i];if(address>=r->base&&address-r->base<r->size){count=(r->size-(address-r->base))/4;break;}}
    if(!count)return INVALID;if(count>D9_SHADER_MAX_WORDS)count=D9_SHADER_MAX_WORDS;
    uint32_t *code=malloc(count*4);if(!code)return OUTMEM;for(size_t i=0;i<count;++i)guest_read32(g.mem,address+(uint32_t)i*4,code+i);
    /* Measurement-only skip probe: a shader outside the certified closure is
     * represented by a named, non-executable object so the census can pass the
     * renderer's shader wall.  Draws with it are explicitly refused below.
     * Without the env this branch is not taken and the old closure/legacy
     * rejection path is unchanged. */
    if(g.shader_skip_probe){
        D9MenuClosureVerdict probe_verdict;
        D9MenuClosureRequest request;
        memset(&request,0,sizeof request);
        request.stage=kind==PS?D9_MENU_STAGE_PS:D9_MENU_STAGE_VS;
        request.code=code;request.available_words=count;
        int match=d9_menu_closure_certify(&request,&probe_verdict);
        if(match!=D9_MENU_MATCH){
            char reason[D9_MENU_CLOSURE_REASON_MAX];
            snprintf(reason,sizeof reason,"%s",probe_verdict.reason[0]?
                     probe_verdict.reason:"shader outside the certified closure");
            /* GetFunction must report the real token-stream length.  The
             * scan is the same one the legacy path uses; it never mutates. */
            D9ShaderInfo info;
            d9_shader_analyze_tev(code,count,&info,NULL);
            size_t words=info.saw_end&&info.words?info.words:count;
            uint32_t *exact=realloc(code,words*4);
            if(exact)code=exact;
            return shader_probe_object(kind,out,reason,code,words);
        }
    }
    /* Named first-menu closure.  While the TESV menu path has armed this
     * contract, the only shaders that may be created are the certified
     * menu.logo.ps / menu.composite.ps / menu.generic.vs set.  Anything else
     * invalidates the predicate permanently and is rejected here, before the
     * generic SM2/TEV paths can turn it into a fallback success. */
    D9MenuClosureVerdict closure;
    if(d9_menu_closure_precreate(kind==PS?D9_MENU_STAGE_PS:D9_MENU_STAGE_VS,
                                 code,count,&closure)){
        snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,
                 "menu shader closure: %s",closure.reason);
        fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
        free(code);
        return closure.match==D9_MENU_BAD_ARGUMENT?INVALID:UNAVAILABLE;
    }
    /* Exact menu subset.  PS entries are selected by certified hash, the VS
     * family by its structural/declaration contract.  Anything outside stays
     * on the legacy path; a selected entry whose plan cannot be built is a
     * named failure before any object or partial plan exists. */
    D9MenuShaderMatch menu;int has_menu=0,has_menu_vs=0;
    char menu_why[D9_MENU_SHADER_REASON_MAX];
    if(kind==PS){
        int menu_result=d9_menu_shader_match_ps(code,count,&menu,
                                                menu_why,sizeof menu_why);
        if(menu_result<0){
            snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,
                      "menu shader: %s",menu_why);
            fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
            d9_menu_closure_commit_failed(&closure,g.last_shader_rejection);
            free(code);return UNAVAILABLE;
        }
        has_menu=menu_result>0;
    }else{
        int menu_result=d9_menu_shader_match_vs(code,count,&menu,
                                                menu_why,sizeof menu_why);
        if(menu_result<0){
            snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,
                      "menu shader: %s",menu_why);
            fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
            d9_menu_closure_commit_failed(&closure,g.last_shader_rejection);
            free(code);return UNAVAILABLE;
        }
        has_menu_vs=menu_result>0;
    }
    if(!has_menu&&!has_menu_vs&&kind==PS&&count&&code[0]==0xFFFF0300u){
        D9PicaTevReject reason;char backend_why[256];
        if(d9_pica_tev_preflight_pixel(code,count,&reason,backend_why,sizeof backend_why)){
            snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,"PICA200 backend: %s",backend_why);
            fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
            d9_menu_closure_commit_failed(&closure,g.last_shader_rejection);
            free(code);return (reason==D9_PICA_TEV_MALFORMED||reason==D9_PICA_TEV_PROGRAM_VERSION)?INVALID:UNAVAILABLE;}
    }
    D9ShaderInfo info;D9TevProgram tev;int result;
    if(has_menu||has_menu_vs){
        memset(&info,0,sizeof info);
        info.version=code[0];info.major=3;info.minor=0;
        info.stream=D9_SHADER_STREAM_VALID;info.saw_end=1;
        info.words=(unsigned)closure.used_words;
        info.instructions=closure.instructions;
        if(has_menu){
            info.kind=D9_SHADER_TEV;tev=menu.plan;result=D9_SHADER_TEV;
        }else{
            info.kind=D9_SHADER_VS_MENU;memset(&tev,0,sizeof tev);
            result=D9_SHADER_VS_MENU;
        }
    }else{
        result=d9_shader_analyze_tev(code,count,&info,&tev);
        int wanted=kind==VS?D9_SHADER_VS_MATRIX_UV:D9_SHADER_PS_TEXTURE;
        if(result!=wanted&&!(kind==PS&&result==D9_SHADER_TEV)){
            char diagnostic[320];d9_shader_diagnostic(&info,diagnostic,sizeof diagnostic);
            snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,"%s",diagnostic);
            fprintf(stderr,"D3D9 shader rejected: %s (hash %08lx, mismatch %u)\n",diagnostic,(unsigned long)info.signature,info.first_mismatch);
            d9_menu_closure_commit_failed(&closure,g.last_shader_rejection);
            free(code);return result==D9_SHADER_INVALID?INVALID:UNAVAILABLE;}
    }
    char menu_backend_why[D9_MENU_SHADER_REASON_MAX];menu_backend_why[0]=0;int menu_backend_ok=1;
    if(has_menu_vs){
        /* The fixed PICA VS carries position+texcoord0 only.  The certified
         * menu VS family also declares color0/color1 varyings, so the plan is
         * selected but the draw gate must refuse by name. */
        menu_backend_ok=0;
        snprintf(menu_backend_why,sizeof menu_backend_why,
                  "fixed PICA vertex shader provides position and texcoord0 only; color0/color1 varyings are not produced");
    }
    if(kind==PS&&result==D9_SHADER_TEV){
        D9PicaTevReject reason;char backend_why[256];
        if(d9_pica_tev_validate(&tev,NULL,&reason,backend_why,sizeof backend_why)){
            if(has_menu){
                /* The plan is complete and selected, but this backend cannot
                 * execute it yet.  Keep the object; the draw gate refuses by
                 * name instead of submitting a partial program. */
                menu_backend_ok=0;
                snprintf(menu_backend_why,sizeof menu_backend_why,
                          "PICA200 backend: %s",backend_why);
            }else{
                snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,"PICA200 backend: %s",backend_why);
                fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
                d9_menu_closure_commit_failed(&closure,g.last_shader_rejection);
                free(code);return UNAVAILABLE;}
        }
    }
    g.last_shader_rejection[0]=0;
    /* Retain original bytecode for GetFunction, including comments. */
    uint32_t *exact=realloc(code,(size_t)info.words*4);if(exact)code=exact;
    Obj *o=new_obj(kind);o->code=code;o->words=info.words;
    if(kind==PS&&result==D9_SHADER_TEV){
        o->tev=tev;o->has_tev=1;
        if(has_menu){
            o->menu=menu;o->has_menu=1;o->menu_backend_ok=menu_backend_ok;
            snprintf(o->menu_backend_why,sizeof o->menu_backend_why,"%s",menu_backend_why);
        }
    }else if(kind==VS&&result==D9_SHADER_VS_MENU){
        o->menu=menu;o->has_menu_vs=1;o->menu_backend_ok=menu_backend_ok;
        snprintf(o->menu_backend_why,sizeof o->menu_backend_why,"%s",menu_backend_why);
    }
    retain(g.dev);M32(out)=o->guest;
    /* Observation, not a D3D9 capability: the closure becomes READY only when
     * every required entry has really been created. */
    d9_menu_closure_commit(&closure);
    return OK;
}
static void create_vs(CPU *c){if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,3);RET(shader_new(VS,A(1),A(2)),3);}
static void create_ps(CPU *c){if(!obj(A(0),DEVICE)||g.phase==2)RET(INVALID,3);RET(shader_new(PS,A(1),A(2)),3);}
static void set_vs(CPU *c){Obj *o=obj(A(1),VS);if(!obj(A(0),DEVICE)||(A(1)&&!o)||g.phase==2)RET(INVALID,2);
    if(o&&o->has_menu_vs){
        D9Decl tmp;const D9Decl *decl=menu_current_decl(&tmp);char why[D9_MENU_SHADER_REASON_MAX];
        if(decl&&d9_menu_shader_vs_check_decl(&o->menu,decl,why,sizeof why)){
            snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s: %s",o->menu.id,why);
            RET(UNAVAILABLE,2);
        }
    }
    bind(&g.vs,o);if(!o)d9_set_shader_clip(g.device,NULL);RET(OK,2);}
static void set_ps(CPU *c){Obj *o=obj(A(1),PS);if(!obj(A(0),DEVICE)||(A(1)&&!o)||g.phase==2)RET(INVALID,2);
    const D9TevProgram *program=o&&o->has_tev?&o->tev:NULL;
    if(d9_set_pixel_shader(g.device,program))RET(INVALID,2);
    bind(&g.ps,o);RET(OK,2);}
static void get_vs(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);retain(g.vs);M32(A(1))=g.vs?g.vs->guest:0;RET(OK,2);}
static void get_ps(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);retain(g.ps);M32(A(1))=g.ps?g.ps->guest:0;RET(OK,2);}
static float *vs_const_row(unsigned i){return i<4?g.constants.m[i]:i<8?g.constants_hi.m[i-4]:g.constants_c8;}
static void upload_tev_constants(void){
    float bank[9][4];memset(bank,0,sizeof bank);
    for(unsigned i=0;i<4;++i){memcpy(bank[i],g.constants.m[i],16);memcpy(bank[4+i],g.constants_hi.m[i],16);}
    memcpy(bank[8],g.constants_c8,16);
    if(g.device)d9_set_tev_constants(g.device,bank);
}
static void set_vs_const(CPU *c){unsigned start=A(1),n=A(3);uint32_t p=A(2);if(!obj(A(0),DEVICE)||start>=9||n>9-start||g.phase==2||(n&&!ptr(p,n*16)))RET(INVALID,4);
    float values[9][4];if(n)memcpy(values,ptr(p,n*16),n*16);for(unsigned i=0;i<n;++i)for(unsigned j=0;j<4;++j)if(!isfinite(values[i][j]))RET(INVALID,4);
    for(unsigned i=0;i<n;++i){memcpy(vs_const_row(start+i),values[i],16);g.constants_mask|=1u<<(start+i);}upload_tev_constants();RET(OK,4);}
static void get_vs_const(CPU *c){unsigned start=A(1),n=A(3);if(!obj(A(0),DEVICE)||start>=9||n>9-start||(n&&!ptr(A(2),n*16)))RET(INVALID,4);
    for(unsigned i=0;i<n;++i)memcpy(ptr(A(2)+i*16,16),vs_const_row(start+i),16);RET(OK,4);}
/* ---- pixel-shader constants --------------------------------------------- */
/* The float bank is exactly the PICA TEV external-constant window c0..c7.
 * I/B registers are API state with no accepted-opcode consumer; they still
 * round-trip honestly and use the same bounded register window. */
static int ps_const_range(const char *method,unsigned start,unsigned n,unsigned limit){
    if(start>limit||n>limit-start){
        char why[160];
        snprintf(why,sizeof why,"c%u + %u is outside the %u-register PS window",
                 start,n,limit);
        d3d_fail(method,why,INVALID);
        return INVALID;
    }
    return OK;
}
static uint32_t ps_const_setup(CPU *c,const char *method,unsigned limit,size_t unit,void **data){
    unsigned start=A(1),n=A(3);uint32_t p=A(2);
    if(!obj(A(0),DEVICE))return INVALID;
    uint32_t range=ps_const_range(method,start,n,limit);if(range!=OK)return range;
    if(g.phase==2){d3d_fail(method,"cannot set pixel-shader constants after EndScene",INVALID);return INVALID;}
    *data=n?ptr(p,(size_t)n*unit):NULL;
    if(n&&!*data){d3d_fail(method,"constant data is not readable guest memory",INVALID);return INVALID;}
    return OK;
}
static void ps_const_finish(CPU *c,const char *method,int rc){
    if(rc){d3d_fail(method,d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_ps_const_f(CPU *c){
    void *data=NULL;uint32_t setup=ps_const_setup(c,"SetPixelShaderConstantF",D9_PS_CONST_F_REGS,16,&data);
    if(setup!=OK)RET(setup,4);
    ps_const_finish(c,"SetPixelShaderConstantF",d9_set_ps_constants_f(g.device,A(1),(const float *)data,A(3)));
}
static void get_ps_const_f(CPU *c){
    unsigned start=A(1),n=A(3);uint32_t range=ps_const_range("GetPixelShaderConstantF",start,n,D9_PS_CONST_F_REGS);
    if(!obj(A(0),DEVICE)||range!=OK)RET(INVALID,4);
    if(n&&!ptr(A(2),(size_t)n*16))RET(INVALID,4);
    int rc=d9_get_ps_constants_f(g.device,start,n?(float *)ptr(A(2),(size_t)n*16):NULL,n);
    if(rc){d3d_fail("GetPixelShaderConstantF",d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_ps_const_i(CPU *c){
    void *data=NULL;uint32_t setup=ps_const_setup(c,"SetPixelShaderConstantI",D9_PS_CONST_I_REGS,16,&data);
    if(setup!=OK)RET(setup,4);
    ps_const_finish(c,"SetPixelShaderConstantI",d9_set_ps_constants_i(g.device,A(1),(const int32_t *)data,A(3)));
}
static void get_ps_const_i(CPU *c){
    unsigned start=A(1),n=A(3);uint32_t range=ps_const_range("GetPixelShaderConstantI",start,n,D9_PS_CONST_I_REGS);
    if(!obj(A(0),DEVICE)||range!=OK)RET(INVALID,4);
    if(n&&!ptr(A(2),(size_t)n*16))RET(INVALID,4);
    int rc=d9_get_ps_constants_i(g.device,start,n?(int32_t *)ptr(A(2),(size_t)n*16):NULL,n);
    if(rc){d3d_fail("GetPixelShaderConstantI",d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_ps_const_b(CPU *c){
    void *data=NULL;uint32_t setup=ps_const_setup(c,"SetPixelShaderConstantB",D9_PS_CONST_B_REGS,4,&data);
    if(setup!=OK)RET(setup,4);
    ps_const_finish(c,"SetPixelShaderConstantB",d9_set_ps_constants_b(g.device,A(1),(const int32_t *)data,A(3)));
}
static void get_ps_const_b(CPU *c){
    unsigned start=A(1),n=A(3);uint32_t range=ps_const_range("GetPixelShaderConstantB",start,n,D9_PS_CONST_B_REGS);
    if(!obj(A(0),DEVICE)||range!=OK)RET(INVALID,4);
    if(n&&!ptr(A(2),(size_t)n*4))RET(INVALID,4);
    int rc=d9_get_ps_constants_b(g.device,start,n?(int32_t *)ptr(A(2),(size_t)n*4):NULL,n);
    if(rc){d3d_fail("GetPixelShaderConstantB",d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
/* ---- vertex-shader integer/bool constants ------------------------------- */
/* Same bounded API-state treatment as PS I/B.  The accepted VS subset has no
 * integer/predicate/flow-control opcode, so these do not feed a draw. */
static int vs_const_range(const char *method,unsigned start,unsigned n,unsigned limit){
    if(start>limit||n>limit-start){
        char why[160];
        snprintf(why,sizeof why,"c%u + %u is outside the %u-register VS window",
                 start,n,limit);
        d3d_fail(method,why,INVALID);
        return INVALID;
    }
    return OK;
}
static uint32_t vs_const_setup(CPU *c,const char *method,unsigned limit,size_t unit,void **data){
    unsigned start=A(1),n=A(3);uint32_t p=A(2);
    if(!obj(A(0),DEVICE))return INVALID;
    uint32_t range=vs_const_range(method,start,n,limit);if(range!=OK)return range;
    if(g.phase==2){d3d_fail(method,"cannot set vertex-shader constants after EndScene",INVALID);return INVALID;}
    *data=n?ptr(p,(size_t)n*unit):NULL;
    if(n&&!*data){d3d_fail(method,"constant data is not readable guest memory",INVALID);return INVALID;}
    return OK;
}
static void vs_const_finish(CPU *c,const char *method,int rc){
    if(rc){d3d_fail(method,d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_vs_const_i(CPU *c){
    void *data=NULL;uint32_t setup=vs_const_setup(c,"SetVertexShaderConstantI",D9_VS_CONST_I_REGS,16,&data);
    if(setup!=OK)RET(setup,4);
    vs_const_finish(c,"SetVertexShaderConstantI",d9_set_vs_constants_i(g.device,A(1),(const int32_t *)data,A(3)));
}
static void get_vs_const_i(CPU *c){
    unsigned start=A(1),n=A(3);uint32_t range=vs_const_range("GetVertexShaderConstantI",start,n,D9_VS_CONST_I_REGS);
    if(!obj(A(0),DEVICE)||range!=OK)RET(INVALID,4);
    if(n&&!ptr(A(2),(size_t)n*16))RET(INVALID,4);
    int rc=d9_get_vs_constants_i(g.device,start,n?(int32_t *)ptr(A(2),(size_t)n*16):NULL,n);
    if(rc){d3d_fail("GetVertexShaderConstantI",d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void set_vs_const_b(CPU *c){
    void *data=NULL;uint32_t setup=vs_const_setup(c,"SetVertexShaderConstantB",D9_VS_CONST_B_REGS,4,&data);
    if(setup!=OK)RET(setup,4);
    vs_const_finish(c,"SetVertexShaderConstantB",d9_set_vs_constants_b(g.device,A(1),(const int32_t *)data,A(3)));
}
static void get_vs_const_b(CPU *c){
    unsigned start=A(1),n=A(3);uint32_t range=vs_const_range("GetVertexShaderConstantB",start,n,D9_VS_CONST_B_REGS);
    if(!obj(A(0),DEVICE)||range!=OK)RET(INVALID,4);
    if(n&&!ptr(A(2),(size_t)n*4))RET(INVALID,4);
    int rc=d9_get_vs_constants_b(g.device,start,n?(int32_t *)ptr(A(2),(size_t)n*4):NULL,n);
    if(rc){d3d_fail("GetVertexShaderConstantB",d9_last_draw_diagnostic(g.device),INVALID);RET(INVALID,4);}
    g.last_diagnostic[0]=0;RET(OK,4);
}
static void shader_function(CPU *c){Obj *o=obj(A(0),0);uint32_t size_ptr=A(2);if(!o||(o->kind!=VS&&o->kind!=PS)||!ptr(size_ptr,4))RET(INVALID,3);
    uint32_t need=o->words*4,provided=M32(size_ptr);M32(size_ptr)=need;if(!A(1))RET(OK,3);
    if(provided<need||!ptr(A(1),need))RET(INVALID,3);memcpy(ptr(A(1),need),o->code,need);RET(OK,3);}
static void create_decl(CPU *c){uint32_t p=A(1),out=A(2);if(!obj(A(0),DEVICE)||!p||!ptr(out,4)||g.phase==2)RET(INVALID,3);M32(out)=0;
    if(g.next>=MAX_OBJECTS)RET(OUTMEM,3);D9VertexElement elements[D9_MAX_DECL];unsigned n;
    for(n=0;n<=D9_MAX_DECL;++n){uint64_t at=(uint64_t)p+(uint64_t)n*8;if(at>UINT32_MAX||!ptr((uint32_t)at,8))RET(INVALID,3);
        D9VertexElement e;memcpy(&e,ptr((uint32_t)at,8),8);if(e.stream==0xff){if(e.offset||e.type!=D9_DECL_UNUSED||e.method||e.usage||e.usage_index)RET(INVALID,3);break;}
        if(n==D9_MAX_DECL)RET(INVALID,3);elements[n]=e;}
    D9Decl *decl=malloc(sizeof *decl);if(!decl)RET(OUTMEM,3);if(d9_decl_init(decl,elements,n)){free(decl);RET(INVALID,3);}
    Obj *o=new_obj(DECL);o->declaration=decl;retain(g.dev);M32(out)=o->guest;RET(OK,3);}
static void set_decl(CPU *c){Obj *o=obj(A(1),DECL);if(!obj(A(0),DEVICE)||(A(1)&&!o)||g.phase==2)RET(INVALID,2);
    int rc=d9_set_vertex_declaration(g.device,o?o->declaration:NULL);if(!rc){bind(&g.declaration,o);g.fvf=0;}RET(hr(rc),2);}
static void get_decl(CPU *c){if(!obj(A(0),DEVICE)||!ptr(A(1),4))RET(INVALID,2);retain(g.declaration);M32(A(1))=g.declaration?g.declaration->guest:0;RET(OK,2);}
static void declaration_data(CPU *c){Obj *o=obj(A(0),DECL);uint32_t p=A(1),n=A(2);if(!o||!ptr(n,4))RET(INVALID,3);
    unsigned need=o->declaration->count+1,provided=M32(n);M32(n)=need;if(!p)RET(OK,3);
    if(provided<need||!ptr(p,(size_t)need*8))RET(INVALID,3);memcpy(ptr(p,(size_t)need*8),o->declaration->elements,(need-1)*8);
    D9VertexElement end={0xff,0,D9_DECL_UNUSED,0,0,0};memcpy(ptr(p+(need-1)*8,8),&end,8);RET(OK,3);}
static int shader_state(void){
    /* A code-less shader created by the measurement-only skip probe is never
     * executable: refuse the batch by name instead of drawing with no plan. */
    if((g.vs&&g.vs->probe_skip)||(g.ps&&g.ps->probe_skip)){
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,
                 "shader-skip probe: draw with a code-less shader is refused (measurement only)");
        return D9_UNSUPPORTED;
    }
    /* A selected menu plan still needs its declaration/material preconditions
     * and a backend that can execute it.  Both are named refusals here, before
     * the draw, so the batch is never partially submitted. */
    int menu_vs=menu_vs_state();
    if(menu_vs)return menu_vs;
    int menu_ps=menu_ps_state();
    if(menu_ps)return menu_ps;
    int needs_texture=g.ps&&(!g.ps->has_tev||g.ps->tev.textures!=0);if(needs_texture&&!g.tex)return D9_INVALID;if(!g.vs)return d9_set_shader_clip(g.device,NULL);
    if((g.constants_mask&0xFu)!=0xFu)return D9_INVALID;return d9_set_shader_clip(g.device,&g.constants);}
#if D9_CAPTURE_HOOK
/* ---- first-draw capture -------------------------------------------------- */
/* Enabled only when TESV_D3D9_CAPTURE_DRAWS is a positive integer.  The
 * module clamps the record count to D9_CAPTURE_MAX_RECORDS.  Normal runs pay
 * one d9_capture_active() branch per successful draw and write no files. */
static void capture_start(void){
    const char *requested=getenv("TESV_D3D9_CAPTURE_DRAWS");
#ifdef __3DS__
    /* The 3DS has no environment: diagnostic builds capture the first draw. */
    if(!requested||!*requested)requested="1";
#endif
    if(!requested||!*requested)return;
    char *end=NULL;unsigned long n=strtoul(requested,&end,10);
    if(end==requested||!n)return;
    if(n>D9_CAPTURE_MAX_RECORDS)n=D9_CAPTURE_MAX_RECORDS;
    D9CaptureOptions options;d9_capture_options_init(&options);
    options.max_records=(unsigned)n;options.flags|=D9_CAPTURE_OPT_UNIQUE_ONLY;
    if(d9_capture_init(&options)!=D9_CAPTURE_OK){
        fprintf(stderr,"[d3d9-capture] init: %s\n",d9_capture_last_error());
        return;
    }
    fprintf(stderr,"[d3d9-capture] first-draw capture enabled (max %u records, max %u presents)\n",
            (unsigned)options.max_records,(unsigned)options.max_presents);
}
static void capture_stop(void){d9_capture_shutdown();}
static void capture_target(D9CaptureRenderTarget *out){
    if(!out)return;memset(out,0,sizeof *out);
    if(!g.rt){out->implicit=1;return;}
    out->handle=g.rt==g.back?0:g.rt->guest;
    out->width=g.rt->width;out->height=g.rt->height;out->format=g.rt->format;out->usage=g.rt->usage;
    out->implicit=g.rt==g.back?1u:0u;
}
static void capture_present_event(void){
    if(!d9_capture_present_active())return;
    D9CapturePresentEvent event;memset(&event,0,sizeof event);
    event.frame_index=g.frame_index;
    if(g.back){
        event.target.width=g.back->width;event.target.height=g.back->height;
        event.target.format=g.back->format;event.target.usage=g.back->usage;event.target.implicit=1;
    }else event.target.implicit=1;
    event.flags=g.capture_present_flags;event.swap_chain=0;
    int rc=d9_capture_offer_present(&event);
    if(rc<0)fprintf(stderr,"[d3d9-capture] present offer %d: %s\n",rc,d9_capture_last_error());
}
static void capture_draw(unsigned type,unsigned start,unsigned primitives,int indexed,
                         int32_t base,unsigned min_vertex,unsigned vertex_count,
                         unsigned start_index){
    if(!d9_capture_active())return;
    ++g.draw_index;
    D9CaptureSnapshot s;memset(&s,0,sizeof s);
    s.frame_index=g.frame_index;s.draw_index=g.draw_index;
    capture_target(&s.target);
    s.draw.type=type;s.draw.start=start;s.draw.primitives=primitives;
    s.draw.indexed=indexed?1u:0u;s.draw.base_vertex=base;
    s.draw.min_vertex=min_vertex;s.draw.vertex_count=vertex_count;s.draw.start_index=start_index;

    s.viewport.x=g.viewport[0];s.viewport.y=g.viewport[1];
    s.viewport.width=g.viewport[2];s.viewport.height=g.viewport[3];
    memcpy(&s.viewport.minz,&g.viewport[4],sizeof s.viewport.minz);
    memcpy(&s.viewport.maxz,&g.viewport[5],sizeof s.viewport.maxz);

    /* pixel_shader stays a borrowed presence flag; the module never dereferences it. */
    (void)d9_get_draw_state(g.device,&s.state);
    (void)d9_get_transform(g.device,D9_WORLD,&s.world);
    (void)d9_get_transform(g.device,D9_VIEW,&s.view);
    (void)d9_get_transform(g.device,D9_PROJECTION,&s.projection);

    if(g.vs){s.vs.object=g.vs->guest;s.vs.code.host=g.vs->code;
        s.vs.code.bytes=(size_t)g.vs->words*4u;}
    if(g.ps){s.ps.object=g.ps->guest;s.ps.code.host=g.ps->code;
        s.ps.code.bytes=(size_t)g.ps->words*4u;}

    if(g.declaration&&g.declaration->declaration){
        const D9Decl *d=g.declaration->declaration;
        s.declaration.active=1;s.declaration.count=d->count;
        s.declaration.position=d->position;s.declaration.uv=d->uv;
        s.declaration.stream_mask=d->stream_mask;
        memcpy(s.declaration.required,d->required,sizeof s.declaration.required);
        memcpy(s.declaration.elements,d->elements,(size_t)d->count*sizeof d->elements[0]);
    }

    for(unsigned i=0;i<D9_MAX_STREAMS&&s.stream_count<D9_CAPTURE_MAX_STREAMS;++i){
        if(!g.streams[i])continue;
        D9Handle buffer;size_t offset,stride;
        if(d9_get_stream_info(g.device,i,&buffer,&offset,&stride))continue;
        const void *data;size_t bytes;
        if(d9_get_buffer_data(g.device,buffer,&data,&bytes))continue;
        D9CaptureStream *st=&s.streams[s.stream_count++];
        st->index=i;st->handle=g.streams[i]->guest;st->stride=(unsigned)stride;
        st->offset=offset;st->bytes=bytes;st->guest=g.streams[i]->guest;
    }

    /* Decoded XYZ+UV stream of the draw the core just accepted. */
    const D9Vertex *vertices;size_t vertex_n;
    if(!d9_get_last_draw_vertices(g.device,&vertices,&vertex_n)&&vertex_n&&
       vertex_n<=SIZE_MAX/sizeof *vertices){
        s.draw.vertices.data.host=vertices;
        s.draw.vertices.data.bytes=vertex_n*sizeof *vertices;
        s.draw.vertices.stride=(unsigned)sizeof *vertices;
        s.draw.vertices.count=(unsigned)vertex_n;
    }

    /* Raw index buffer slice for the same primitive range. */
    if(indexed&&g.ib){
        D9Handle buffer;unsigned format;
        if(!d9_get_index_info(g.device,&buffer,&format)&&buffer&&
           (format==D9_INDEX16||format==D9_INDEX32)){
            const void *data;size_t bytes;unsigned size=format==D9_INDEX32?4u:2u;
            if(!d9_get_buffer_data(g.device,buffer,&data,&bytes)){
                unsigned used=type==D9_TRIANGLELIST?primitives*3u:primitives+2u;
                size_t first=(size_t)start_index*size;
                if(first<=bytes){
                    size_t available=bytes-first;
                    unsigned captured=used<D9_CAPTURE_MAX_INDICES?used:D9_CAPTURE_MAX_INDICES;
                    if(available>=(size_t)captured*size){
                        s.draw.indices.data.host=(const uint8_t *)data+first;
                        s.draw.indices.data.bytes=available;
                        s.draw.indices.size=size;s.draw.indices.count=used;
                    }
                }
            }
        }
    }

    for(unsigned stage=0; stage<2 && s.texture_count<D9_CAPTURE_MAX_TEXTURES; ++stage){
        Obj *bound=stage?g.tex1:g.tex;
        if(!bound)continue;
        const void *pixels;size_t pitch,bytes;unsigned w,h,format;
        if(!d9_get_texture_capture(g.device,bound->handle,0,&pixels,&pitch,&bytes,
                                   &w,&h,&format)){
            D9CaptureTexture *t=&s.textures[s.texture_count];
            t->stage=stage;t->handle=bound->guest;t->width=w;t->height=h;t->format=format;
            t->levels=d9_texture_levels(g.device,bound->handle);
            t->pool=bound->pool;t->usage=bound->usage;
            t->level=0;t->row_pitch=pitch;t->pixels.host=pixels;t->pixels.bytes=bytes;
            ++s.texture_count;
        }else{
            /* GPU-only SKTX textures have no CPU pixels. Record the bound
             * texture truthfully with an empty payload instead of omitting it. */
            D9CaptureTexture *t=&s.textures[s.texture_count];
            t->stage=stage;t->handle=bound->guest;t->width=bound->width;t->height=bound->height;
            t->format=bound->format;t->levels=d9_texture_levels(g.device,bound->handle);
            t->pool=bound->pool;t->usage=bound->usage;t->level=0;t->row_pitch=bound->pitch;
            t->pixels.host=NULL;t->pixels.bytes=0;++s.texture_count;
        }
    }

    s.clear.valid=g.frame_clear;s.clear.color=g.clear;s.clear.flags=g.clear_flags;s.clear.rect_count=0;
    s.present.present=g.capture_present;s.present.flags=g.capture_present_flags;
    s.present.swap_chain=0;

    int rc=d9_capture_offer(&s,g.mem);
    if(rc<0)fprintf(stderr,"[d3d9-capture] offer %d: %s\n",rc,d9_capture_last_error());
    g.capture_present=0;g.capture_present_flags=0;
}
#else
static void capture_start(void){}
static void capture_stop(void){}
static void capture_draw(unsigned type,unsigned start,unsigned primitives,int indexed,
                         int32_t base,unsigned min_vertex,unsigned vertex_count,
                         unsigned start_index){
    (void)type;(void)start;(void)primitives;(void)indexed;(void)base;
    (void)min_vertex;(void)vertex_count;(void)start_index;
}
#endif
static uint32_t shader_gate(const char *method){
    int rc=shader_state();
    if(!rc)return OK;
    if(!g.last_diagnostic[0])
        snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"%s: shader state rejected",method);
    return hr(rc);
}
static void draw_primitive(CPU *c){
    if(!obj(A(0),DEVICE)||g.phase!=1||(!g.declaration&&!g.fvf))RET(INVALID,4);
    uint32_t gate=shader_gate("DrawPrimitive");if(gate!=OK)RET(gate,4);
    int rc=d9_draw_primitive(g.device,A(1),A(2),A(3));
    if(rc==D9_OK)capture_draw(A(1),A(2),A(3),0,0,0,0,0);
    else snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"DrawPrimitive: %s",d9_last_draw_diagnostic(g.device));
    RET(hr(rc),4);}
static void draw_indexed(CPU *c){
    if(!obj(A(0),DEVICE)||g.phase!=1||(!g.declaration&&!g.fvf)||!g.ib||g.ib->lock_guest||!A(4))RET(INVALID,7);
    uint32_t gate=shader_gate("DrawIndexedPrimitive");if(gate!=OK)RET(gate,7);
    int rc=d9_draw_indexed_range(g.device,A(1),(int32_t)A(2),A(3),A(4),A(5),A(6));
    if(rc==D9_OK)capture_draw(A(1),A(5),A(6),1,(int32_t)A(2),A(3),A(4),A(5));
    else snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"DrawIndexedPrimitive: %s",d9_last_draw_diagnostic(g.device));
    RET(hr(rc),7);}
/* Number of stream-zero vertices a D3D9 triangle primitive consumes. */
static int up_geometry(unsigned type,unsigned primitives,unsigned *vertices){
    if(type!=D9_TRIANGLELIST&&type!=D9_TRIANGLESTRIP&&type!=D9_TRIANGLEFAN)return -1;
    if(!primitives||primitives>4096u/3u)return -1;
    *vertices=type==D9_TRIANGLELIST?primitives*3u:primitives+2u;return 0;
}
/* DrawPrimitiveUP borrows stream zero; it does not change the bound stream,
 * index buffer, transforms, texture or render state. */
static void draw_primitive_up(CPU *c){
    unsigned type=A(1),primitives=A(2),stride=A(4),vertices=0;
    if(!obj(A(0),DEVICE)||g.phase!=1||(!g.declaration&&!g.fvf)||
       up_geometry(type,primitives,&vertices)||!stride||stride>4096u){
        d3d_log("DrawPrimitiveUP","invalid scene, vertex format, primitive type/count or vertex stride");RET(INVALID,5);
    }
    uint32_t gate=shader_gate("DrawPrimitiveUP");if(gate!=OK)RET(gate,5);
    void *data=ptr(A(3),(size_t)vertices*stride);
    if(!data){d3d_log("DrawPrimitiveUP","vertex stream zero data is null or outside guest memory");RET(INVALID,5);}
    int rc=d9_draw_primitive_up(g.device,type,primitives,data,stride);
    /* There is no bound vertex buffer to record; the decoded vertices are still
     * captured from the core. */
    if(rc==D9_OK)capture_draw(type,0,primitives,0,0,0,0,0);
    else snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"DrawPrimitiveUP: %s",d9_last_draw_diagnostic(g.device));
    RET(hr(rc),5);}
static void draw_indexed_up(CPU *c){
    unsigned type=A(1),min=A(2),count=A(3),primitives=A(4),fmt=A(6),stride=A(8),indices=0,used,isize;
    if(!obj(A(0),DEVICE)||g.phase!=1||(!g.declaration&&!g.fvf)||
       up_geometry(type,primitives,&indices)||!count||min>UINT32_MAX-count||
       (fmt!=D9_INDEX16&&fmt!=D9_INDEX32)||!stride||stride>4096u){
        d3d_log("DrawIndexedPrimitiveUP","invalid scene, vertex format, index format/range, primitive count or vertex stride");RET(INVALID,9);
    }
    uint32_t gate=shader_gate("DrawIndexedPrimitiveUP");if(gate!=OK)RET(gate,9);
    size_t need=(size_t)min+count;
    if(need>SIZE_MAX/stride){d3d_log("DrawIndexedPrimitiveUP","vertex range overflows the stream-zero byte size");RET(INVALID,9);}
    isize=fmt==D9_INDEX32?4u:2u;used=type==D9_TRIANGLELIST?primitives*3u:primitives+2u;
    void *idesc=ptr(A(5),(size_t)used*isize),*vdata=ptr(A(7),need*stride);
    if(!idesc||!vdata){d3d_log("DrawIndexedPrimitiveUP","index or vertex stream zero data is null or outside guest memory");RET(INVALID,9);}
    int rc=d9_draw_indexed_primitive_up(g.device,type,min,count,primitives,idesc,fmt,vdata,stride);
    if(rc==D9_OK)capture_draw(type,0,primitives,0,0,0,0,0);
    else snprintf(g.last_diagnostic,sizeof g.last_diagnostic,"DrawIndexedPrimitiveUP: %s",d9_last_draw_diagnostic(g.device));
    RET(hr(rc),9);}

#define FUNCTIONS(X) X(unsupported) X(direct_create) X(query) X(addref) X(release_fn) X(adapter_count) X(display_mode) X(check_format) X(create_device) \
 X(adapter_identifier) X(adapter_mode_count) X(enum_modes) X(check_device_type) X(check_multisample) X(check_depth_stencil) X(check_format_conversion) \
 X(factory_caps) X(device_caps) X(adapter_monitor) X(register_software) \
 X(d3dperf_set_options) X(d3dperf_begin) X(d3dperf_end) X(d3dperf_status) \
 X(cooperative) X(available_texture_mem) X(evict_managed) X(raster_status) X(dialog_box_mode) X(validate_device) X(direct3d) X(resource_device) X(create_vb) X(create_ib) X(create_tex) X(create_cube_tex) X(create_volume_tex) X(buffer_lock) X(buffer_unlock) X(texture_lock) X(texture_unlock) \
 X(buffer_desc) X(level_count) X(texture_desc) X(resource_type) X(clear) X(begin_scene) X(end_scene) X(present) X(transform) X(set_fvf) X(get_fvf) \
 X(get_transform) X(multiply_transform) X(set_material) X(get_material) X(set_light) X(get_light) X(light_enable) X(get_light_enable) \
 X(stream) X(indices) X(texture) X(get_texture) X(draw_primitive) X(draw_indexed) X(draw_primitive_up) X(draw_indexed_up) X(create_vs) X(create_ps) X(set_vs) X(set_ps) X(get_vs) X(get_ps) X(set_vs_const) X(get_vs_const) \
 X(set_vs_const_i) X(get_vs_const_i) X(set_vs_const_b) X(get_vs_const_b) \
 X(set_clip_plane) X(get_clip_plane) X(set_clip_status) X(get_clip_status) X(set_software_vp) X(get_software_vp) \
 X(set_cursor_properties) X(set_cursor_position) X(show_cursor) X(set_palette_entries) X(get_palette_entries) \
 X(set_current_palette) X(get_current_palette) \
 X(create_additional_swap_chain) X(get_swap_chain) X(swapchain_present) X(swapchain_get_back_buffer) \
 X(swapchain_raster_status) X(swapchain_display_mode) X(swapchain_get_device) X(swapchain_present_parameters) \
 X(set_ps_const_f) X(get_ps_const_f) X(set_ps_const_i) X(get_ps_const_i) X(set_ps_const_b) X(get_ps_const_b) X(create_query) \
 X(query_get_device) X(query_get_type) X(query_get_data_size) X(query_issue) X(query_get_data) X(shader_function) \
 X(set_render) X(get_render) X(set_sampler) X(get_sampler) X(get_texture_stage_state) X(set_texture_stage_state) X(texture_get_surface_level) X(texture_add_dirty_rect) X(create_decl) X(set_decl) X(get_decl) X(declaration_data) X(get_stream) X(stream_freq) X(get_stream_freq) X(get_indices) \
 X(reset_device) X(device_display_mode) X(creation_parameters) X(get_back_buffer) X(number_swap_chains) X(create_render_target) X(create_depth_stencil) \
 X(create_offscreen) X(set_render_target) X(get_render_target) X(stretch_rect) X(update_surface) X(update_texture) X(color_fill) X(get_render_target_data) X(get_front_buffer_data) X(set_depth_stencil) X(get_depth_stencil) X(set_viewport) X(get_viewport) \
 X(set_scissor_rect) X(get_scissor_rect) X(set_gamma_ramp) X(get_gamma_ramp) X(set_npatch_mode) X(get_npatch_mode) \
 X(surface_desc) X(surface_container) X(surface_lock) X(surface_unlock) X(surface_dc) X(surface_release_dc) \
 X(cube_level_count) X(cube_desc) X(cube_get_surface) X(cube_lock) X(cube_unlock) X(cube_add_dirty_rect) \
 X(volume_level_count) X(volume_desc) X(volume_get_level) X(volume_lock_box) X(volume_unlock_box) X(volume_add_dirty_box) \
 X(create_state_block) X(begin_state_block) X(end_state_block) X(state_block_capture_com) X(state_block_apply_com)
#define ENUM(f) I_##f,
enum {FUNCTIONS(ENUM) I_COUNT};
#undef ENUM

static const char *const slot_names[KIND_COUNT][MAX_SLOTS]={
    [FACTORY]={
        "QueryInterface","AddRef","Release","RegisterSoftwareDevice","GetAdapterCount","GetAdapterIdentifier",
        "GetAdapterModeCount","EnumAdapterModes","GetAdapterDisplayMode","CheckDeviceType","CheckDeviceFormat",
        "CheckDeviceMultiSampleType","CheckDepthStencilMatch","CheckDeviceFormatConversion","GetDeviceCaps",
        "GetAdapterMonitor","CreateDevice"},
    [DEVICE]={
        "QueryInterface","AddRef","Release","TestCooperativeLevel","GetAvailableTextureMem","EvictManagedResources",
        "GetDirect3D","GetDeviceCaps","GetDisplayMode","GetCreationParameters","SetCursorProperties","SetCursorPosition",
        "ShowCursor","CreateAdditionalSwapChain","GetSwapChain","GetNumberOfSwapChains","Reset","Present","GetBackBuffer",
        "GetRasterStatus","SetDialogBoxMode","SetGammaRamp","GetGammaRamp","CreateTexture","CreateVolumeTexture",
        "CreateCubeTexture","CreateVertexBuffer","CreateIndexBuffer","CreateRenderTarget","CreateDepthStencilSurface",
        "UpdateSurface","UpdateTexture","GetRenderTargetData","GetFrontBufferData","StretchRect","ColorFill",
        "CreateOffscreenPlainSurface","SetRenderTarget","GetRenderTarget","SetDepthStencilSurface","GetDepthStencilSurface",
        "BeginScene","EndScene","Clear","SetTransform","GetTransform","MultiplyTransform","SetViewport","GetViewport",
        "SetMaterial","GetMaterial","SetLight","GetLight","LightEnable","GetLightEnable","SetClipPlane","GetClipPlane",
        "SetRenderState","GetRenderState","CreateStateBlock","BeginStateBlock","EndStateBlock","SetClipStatus","GetClipStatus",
        "GetTexture","SetTexture","GetTextureStageState","SetTextureStageState","GetSamplerState","SetSamplerState",
        "ValidateDevice","SetPaletteEntries","GetPaletteEntries","SetCurrentTexturePalette","GetCurrentTexturePalette",
        "SetScissorRect","GetScissorRect","SetSoftwareVertexProcessing","GetSoftwareVertexProcessing","SetNPatchMode",
        "GetNPatchMode","DrawPrimitive","DrawIndexedPrimitive","DrawPrimitiveUP","DrawIndexedPrimitiveUP","ProcessVertices",
        "CreateVertexDeclaration","SetVertexDeclaration","GetVertexDeclaration","SetFVF","GetFVF","CreateVertexShader",
        "SetVertexShader","GetVertexShader","SetVertexShaderConstantF","GetVertexShaderConstantF","SetVertexShaderConstantI",
        "GetVertexShaderConstantI","SetVertexShaderConstantB","GetVertexShaderConstantB","SetStreamSource","GetStreamSource",
        "SetStreamSourceFreq","GetStreamSourceFreq","SetIndices","GetIndices","CreatePixelShader","SetPixelShader","GetPixelShader",
        "SetPixelShaderConstantF","GetPixelShaderConstantF","SetPixelShaderConstantI","GetPixelShaderConstantI",
        "SetPixelShaderConstantB","GetPixelShaderConstantB","DrawRectPatch","DrawTriPatch","DeletePatch","CreateQuery"},
    [VB]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","Lock","Unlock","GetDesc"},
    [IB]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","Lock","Unlock","GetDesc"},
    [TEX]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType",
        "GenerateMipSubLevels","GetLevelDesc","GetSurfaceLevel","LockRect","UnlockRect","AddDirtyRect"},
    [VS]={"QueryInterface","AddRef","Release","GetDevice","GetFunction"},
    [PS]={"QueryInterface","AddRef","Release","GetDevice","GetFunction"},
    [DECL]={"QueryInterface","AddRef","Release","GetDevice","GetDeclaration"},
    [SURFACE]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","GetContainer","GetDesc","LockRect","UnlockRect","GetDC","ReleaseDC"},
    [CUBE]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType",
        "GenerateMipSubLevels","GetLevelDesc","GetCubeMapSurface","LockRect","UnlockRect","AddDirtyRect"},
    [VOLUME]={
        "QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority",
        "GetPriority","PreLoad","GetType","SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType",
        "GenerateMipSubLevels","GetLevelDesc","GetVolumeLevel","LockBox","UnlockBox","AddDirtyBox"},
    [QUERY]={"QueryInterface","AddRef","Release","GetDevice","GetType","GetDataSize","Issue","GetData"},
    [STATEBLOCK]={"QueryInterface","AddRef","Release","GetDevice","Capture","Apply"},
    [SWAPCHAIN]={"QueryInterface","AddRef","Release","Present","GetBackBuffer","GetRasterStatus",
                 "GetDisplayMode","GetDevice","GetPresentParameters"}
};
static void com_dispatch(unsigned kind,unsigned slot,CPU *c){
    const char *name=slot_names[kind][slot];if(!name)name="Unimplemented";
    trace_enter(name,kind,slot,c);
    void (*handler)(CPU *)=g.handlers[kind][slot];if(!handler)handler=unsupported;
    handler(c);
}
#define COM_SLOTS(X) \
 X(FACTORY,0) X(FACTORY,1) X(FACTORY,2) X(FACTORY,3) X(FACTORY,4) X(FACTORY,5) X(FACTORY,6) X(FACTORY,7) \
 X(FACTORY,8) X(FACTORY,9) X(FACTORY,10) X(FACTORY,11) X(FACTORY,12) X(FACTORY,13) X(FACTORY,14) X(FACTORY,15) X(FACTORY,16) \
 X(DEVICE,0) X(DEVICE,1) X(DEVICE,2) X(DEVICE,3) X(DEVICE,4) X(DEVICE,5) X(DEVICE,6) X(DEVICE,7) \
 X(DEVICE,8) X(DEVICE,9) X(DEVICE,10) X(DEVICE,11) X(DEVICE,12) X(DEVICE,13) X(DEVICE,14) X(DEVICE,15) \
 X(DEVICE,16) X(DEVICE,17) X(DEVICE,18) X(DEVICE,19) X(DEVICE,20) X(DEVICE,21) X(DEVICE,22) X(DEVICE,23) \
 X(DEVICE,24) X(DEVICE,25) X(DEVICE,26) X(DEVICE,27) X(DEVICE,28) X(DEVICE,29) X(DEVICE,30) X(DEVICE,31) \
 X(DEVICE,32) X(DEVICE,33) X(DEVICE,34) X(DEVICE,35) X(DEVICE,36) X(DEVICE,37) X(DEVICE,38) X(DEVICE,39) \
 X(DEVICE,40) X(DEVICE,41) X(DEVICE,42) X(DEVICE,43) X(DEVICE,44) X(DEVICE,45) X(DEVICE,46) X(DEVICE,47) \
 X(DEVICE,48) X(DEVICE,49) X(DEVICE,50) X(DEVICE,51) X(DEVICE,52) X(DEVICE,53) X(DEVICE,54) X(DEVICE,55) \
 X(DEVICE,56) X(DEVICE,57) X(DEVICE,58) X(DEVICE,59) X(DEVICE,60) X(DEVICE,61) X(DEVICE,62) X(DEVICE,63) \
 X(DEVICE,64) X(DEVICE,65) X(DEVICE,66) X(DEVICE,67) X(DEVICE,68) X(DEVICE,69) X(DEVICE,70) X(DEVICE,71) \
 X(DEVICE,72) X(DEVICE,73) X(DEVICE,74) X(DEVICE,75) X(DEVICE,76) X(DEVICE,77) X(DEVICE,78) X(DEVICE,79) \
 X(DEVICE,80) X(DEVICE,81) X(DEVICE,82) X(DEVICE,83) X(DEVICE,84) X(DEVICE,85) X(DEVICE,86) X(DEVICE,87) \
 X(DEVICE,88) X(DEVICE,89) X(DEVICE,90) X(DEVICE,91) X(DEVICE,92) X(DEVICE,93) X(DEVICE,94) X(DEVICE,95) \
 X(DEVICE,96) X(DEVICE,97) X(DEVICE,98) X(DEVICE,99) X(DEVICE,100) X(DEVICE,101) X(DEVICE,102) X(DEVICE,103) \
 X(DEVICE,104) X(DEVICE,105) X(DEVICE,106) X(DEVICE,107) X(DEVICE,108) X(DEVICE,109) X(DEVICE,110) X(DEVICE,111) \
 X(DEVICE,112) X(DEVICE,113) X(DEVICE,114) X(DEVICE,115) X(DEVICE,116) X(DEVICE,117) X(DEVICE,118) \
 X(VB,0) X(VB,1) X(VB,2) X(VB,3) X(VB,4) X(VB,5) X(VB,6) X(VB,7) \
 X(VB,8) X(VB,9) X(VB,10) X(VB,11) X(VB,12) X(VB,13) \
 X(IB,0) X(IB,1) X(IB,2) X(IB,3) X(IB,4) X(IB,5) X(IB,6) X(IB,7) \
 X(IB,8) X(IB,9) X(IB,10) X(IB,11) X(IB,12) X(IB,13) \
 X(TEX,0) X(TEX,1) X(TEX,2) X(TEX,3) X(TEX,4) X(TEX,5) X(TEX,6) X(TEX,7) \
 X(TEX,8) X(TEX,9) X(TEX,10) X(TEX,11) X(TEX,12) X(TEX,13) X(TEX,14) X(TEX,15) \
 X(TEX,16) X(TEX,17) X(TEX,18) X(TEX,19) X(TEX,20) X(TEX,21) \
 X(VS,0) X(VS,1) X(VS,2) X(VS,3) X(VS,4) X(PS,0) X(PS,1) X(PS,2) X(PS,3) X(PS,4) \
 X(DECL,0) X(DECL,1) X(DECL,2) X(DECL,3) X(DECL,4) \
 X(SURFACE,0) X(SURFACE,1) X(SURFACE,2) X(SURFACE,3) X(SURFACE,4) X(SURFACE,5) X(SURFACE,6) X(SURFACE,7) \
 X(SURFACE,8) X(SURFACE,9) X(SURFACE,10) X(SURFACE,11) X(SURFACE,12) X(SURFACE,13) X(SURFACE,14) X(SURFACE,15) X(SURFACE,16) \
 X(CUBE,0) X(CUBE,1) X(CUBE,2) X(CUBE,3) X(CUBE,4) X(CUBE,5) X(CUBE,6) X(CUBE,7) \
 X(CUBE,8) X(CUBE,9) X(CUBE,10) X(CUBE,11) X(CUBE,12) X(CUBE,13) X(CUBE,14) X(CUBE,15) \
 X(CUBE,16) X(CUBE,17) X(CUBE,18) X(CUBE,19) X(CUBE,20) X(CUBE,21) \
 X(VOLUME,0) X(VOLUME,1) X(VOLUME,2) X(VOLUME,3) X(VOLUME,4) X(VOLUME,5) X(VOLUME,6) X(VOLUME,7) \
 X(VOLUME,8) X(VOLUME,9) X(VOLUME,10) X(VOLUME,11) X(VOLUME,12) X(VOLUME,13) X(VOLUME,14) X(VOLUME,15) \
 X(VOLUME,16) X(VOLUME,17) X(VOLUME,18) X(VOLUME,19) X(VOLUME,20) X(VOLUME,21) \
 X(QUERY,0) X(QUERY,1) X(QUERY,2) X(QUERY,3) X(QUERY,4) X(QUERY,5) X(QUERY,6) X(QUERY,7) \
 X(STATEBLOCK,0) X(STATEBLOCK,1) X(STATEBLOCK,2) X(STATEBLOCK,3) X(STATEBLOCK,4) X(STATEBLOCK,5) \
 X(SWAPCHAIN,0) X(SWAPCHAIN,1) X(SWAPCHAIN,2) X(SWAPCHAIN,3) X(SWAPCHAIN,4) X(SWAPCHAIN,5) \
 X(SWAPCHAIN,6) X(SWAPCHAIN,7) X(SWAPCHAIN,8)
#define COM_ENUM(k,s) I_com_##k##_##s,
#define COM_ENTRY(k,s) {THUNK+(I_COUNT+I_com_##k##_##s)*4,com_##k##_##s},
#define COM_WRAP(k,s) static void com_##k##_##s(CPU *c){com_dispatch(k,s,c);}
enum {
COM_SLOTS(COM_ENUM)
I_COM_COUNT
};
COM_SLOTS(COM_WRAP)
#undef COM_WRAP
#undef COM_ENUM
#define ENTRY(f) {THUNK+I_##f*4,f},
static const FnEntry all_entries[]={FUNCTIONS(ENTRY) COM_SLOTS(COM_ENTRY)};
#undef ENTRY
#undef COM_ENTRY
#undef COM_SLOTS
static const unsigned com_offsets[KIND_COUNT]={0,0,17,136,150,164,186,191,196,201,218,240,262,270,276};
#define SLOT(k,s,f) do{g.handlers[k][s]=(f);uint32_t target=g.trace?THUNK+(I_COUNT+com_offsets[k]+(s))*4:THUNK+I_##f*4;guest_write32(g.mem,table_addr(k)+(s)*4,target);}while(0)
static void init_tables(void){
    const unsigned counts[]={0,17,119,14,14,22,5,5,5,17,22,22,8,6,9};
    for(unsigned k=1;k<KIND_COUNT;++k){for(unsigned i=0;i<counts[k];++i)SLOT(k,i,unsupported);SLOT(k,0,query);SLOT(k,1,addref);SLOT(k,2,release_fn);}
    SLOT(FACTORY,3,register_software);SLOT(FACTORY,4,adapter_count);SLOT(FACTORY,5,adapter_identifier);
    SLOT(FACTORY,6,adapter_mode_count);SLOT(FACTORY,7,enum_modes);SLOT(FACTORY,8,display_mode);
    SLOT(FACTORY,9,check_device_type);SLOT(FACTORY,10,check_format);SLOT(FACTORY,11,check_multisample);
    SLOT(FACTORY,12,check_depth_stencil);SLOT(FACTORY,13,check_format_conversion);SLOT(FACTORY,14,factory_caps);
    SLOT(FACTORY,15,adapter_monitor);SLOT(FACTORY,16,create_device);
    SLOT(DEVICE,3,cooperative);SLOT(DEVICE,6,direct3d);SLOT(DEVICE,7,device_caps);SLOT(DEVICE,8,device_display_mode);
    SLOT(DEVICE,10,set_cursor_properties);SLOT(DEVICE,11,set_cursor_position);SLOT(DEVICE,12,show_cursor);
    SLOT(DEVICE,4,available_texture_mem);SLOT(DEVICE,5,evict_managed);
    SLOT(DEVICE,19,raster_status);SLOT(DEVICE,20,dialog_box_mode);
    SLOT(DEVICE,70,validate_device);
    SLOT(DEVICE,71,set_palette_entries);SLOT(DEVICE,72,get_palette_entries);
    SLOT(DEVICE,73,set_current_palette);SLOT(DEVICE,74,get_current_palette);
    SLOT(DEVICE,9,creation_parameters);SLOT(DEVICE,13,create_additional_swap_chain);SLOT(DEVICE,14,get_swap_chain);SLOT(DEVICE,15,number_swap_chains);SLOT(DEVICE,16,reset_device);SLOT(DEVICE,17,present);SLOT(DEVICE,18,get_back_buffer);
    SLOT(DEVICE,23,create_tex);SLOT(DEVICE,24,create_volume_tex);SLOT(DEVICE,25,create_cube_tex);SLOT(DEVICE,26,create_vb);SLOT(DEVICE,27,create_ib);SLOT(DEVICE,28,create_render_target);SLOT(DEVICE,29,create_depth_stencil);
    SLOT(DEVICE,30,update_surface);SLOT(DEVICE,31,update_texture);SLOT(DEVICE,35,color_fill);
    SLOT(DEVICE,32,get_render_target_data);SLOT(DEVICE,33,get_front_buffer_data);
    SLOT(DEVICE,36,create_offscreen);SLOT(DEVICE,37,set_render_target);SLOT(DEVICE,38,get_render_target);SLOT(DEVICE,34,stretch_rect);SLOT(DEVICE,39,set_depth_stencil);
    SLOT(DEVICE,40,get_depth_stencil);SLOT(DEVICE,41,begin_scene);SLOT(DEVICE,42,end_scene);SLOT(DEVICE,43,clear);SLOT(DEVICE,44,transform);
    SLOT(DEVICE,45,get_transform);SLOT(DEVICE,46,multiply_transform);
    SLOT(DEVICE,49,set_material);SLOT(DEVICE,50,get_material);
    SLOT(DEVICE,51,set_light);SLOT(DEVICE,52,get_light);SLOT(DEVICE,53,light_enable);SLOT(DEVICE,54,get_light_enable);
    SLOT(DEVICE,55,set_clip_plane);SLOT(DEVICE,56,get_clip_plane);
    SLOT(DEVICE,62,set_clip_status);SLOT(DEVICE,63,get_clip_status);
    SLOT(DEVICE,77,set_software_vp);SLOT(DEVICE,78,get_software_vp);
    SLOT(DEVICE,47,set_viewport);SLOT(DEVICE,48,get_viewport);SLOT(DEVICE,64,get_texture);SLOT(DEVICE,65,texture);
    SLOT(DEVICE,21,set_gamma_ramp);SLOT(DEVICE,22,get_gamma_ramp);
    SLOT(DEVICE,75,set_scissor_rect);SLOT(DEVICE,76,get_scissor_rect);
    SLOT(DEVICE,79,set_npatch_mode);SLOT(DEVICE,80,get_npatch_mode);
    SLOT(DEVICE,57,set_render);SLOT(DEVICE,58,get_render);SLOT(DEVICE,66,get_texture_stage_state);SLOT(DEVICE,67,set_texture_stage_state);SLOT(DEVICE,68,get_sampler);SLOT(DEVICE,69,set_sampler);
    SLOT(DEVICE,81,draw_primitive);SLOT(DEVICE,82,draw_indexed);SLOT(DEVICE,83,draw_primitive_up);SLOT(DEVICE,84,draw_indexed_up);SLOT(DEVICE,89,set_fvf);SLOT(DEVICE,90,get_fvf);SLOT(DEVICE,100,stream);SLOT(DEVICE,104,indices);
    SLOT(DEVICE,86,create_decl);SLOT(DEVICE,87,set_decl);SLOT(DEVICE,88,get_decl);SLOT(DEVICE,101,get_stream);SLOT(DEVICE,102,stream_freq);SLOT(DEVICE,103,get_stream_freq);SLOT(DEVICE,105,get_indices);
    SLOT(DEVICE,59,create_state_block);SLOT(DEVICE,60,begin_state_block);SLOT(DEVICE,61,end_state_block);
    SLOT(STATEBLOCK,3,resource_device);SLOT(STATEBLOCK,4,state_block_capture_com);SLOT(STATEBLOCK,5,state_block_apply_com);
    SLOT(DEVICE,91,create_vs);SLOT(DEVICE,92,set_vs);SLOT(DEVICE,93,get_vs);SLOT(DEVICE,94,set_vs_const);SLOT(DEVICE,95,get_vs_const);
    SLOT(DEVICE,96,set_vs_const_i);SLOT(DEVICE,97,get_vs_const_i);
    SLOT(DEVICE,98,set_vs_const_b);SLOT(DEVICE,99,get_vs_const_b);
    SLOT(DEVICE,106,create_ps);SLOT(DEVICE,107,set_ps);SLOT(DEVICE,108,get_ps);
    SLOT(DEVICE,109,set_ps_const_f);SLOT(DEVICE,110,get_ps_const_f);
    SLOT(DEVICE,111,set_ps_const_i);SLOT(DEVICE,112,get_ps_const_i);
    SLOT(DEVICE,113,set_ps_const_b);SLOT(DEVICE,114,get_ps_const_b);
    SLOT(DEVICE,118,create_query);
    SLOT(QUERY,3,query_get_device);SLOT(QUERY,4,query_get_type);SLOT(QUERY,5,query_get_data_size);
    SLOT(QUERY,6,query_issue);SLOT(QUERY,7,query_get_data);
    SLOT(SWAPCHAIN,3,swapchain_present);SLOT(SWAPCHAIN,4,swapchain_get_back_buffer);
    SLOT(SWAPCHAIN,5,swapchain_raster_status);SLOT(SWAPCHAIN,6,swapchain_display_mode);
    SLOT(SWAPCHAIN,7,swapchain_get_device);SLOT(SWAPCHAIN,8,swapchain_present_parameters);
    for(unsigned k=VB;k<=TEX;++k){SLOT(k,3,resource_device);SLOT(k,10,resource_type);}
    for(unsigned k=VB;k<=IB;++k){SLOT(k,11,buffer_lock);SLOT(k,12,buffer_unlock);SLOT(k,13,buffer_desc);}
    SLOT(TEX,13,level_count);SLOT(TEX,17,texture_desc);SLOT(TEX,18,texture_get_surface_level);
    SLOT(TEX,19,texture_lock);SLOT(TEX,20,texture_unlock);SLOT(TEX,21,texture_add_dirty_rect);
    SLOT(CUBE,3,resource_device);SLOT(CUBE,10,resource_type);SLOT(CUBE,13,cube_level_count);
    SLOT(CUBE,17,cube_desc);SLOT(CUBE,18,cube_get_surface);SLOT(CUBE,19,cube_lock);
    SLOT(CUBE,20,cube_unlock);SLOT(CUBE,21,cube_add_dirty_rect);
    SLOT(VOLUME,3,resource_device);SLOT(VOLUME,10,resource_type);SLOT(VOLUME,13,volume_level_count);
    SLOT(VOLUME,17,volume_desc);SLOT(VOLUME,18,volume_get_level);SLOT(VOLUME,19,volume_lock_box);
    SLOT(VOLUME,20,volume_unlock_box);SLOT(VOLUME,21,volume_add_dirty_box);
    for(unsigned k=VS;k<=PS;++k){SLOT(k,3,resource_device);SLOT(k,4,shader_function);}
    SLOT(DECL,3,resource_device);SLOT(DECL,4,declaration_data);
    SLOT(SURFACE,3,resource_device);SLOT(SURFACE,10,resource_type);SLOT(SURFACE,11,surface_container);SLOT(SURFACE,12,surface_desc);
    SLOT(SURFACE,13,surface_lock);SLOT(SURFACE,14,surface_unlock);SLOT(SURFACE,15,surface_dc);SLOT(SURFACE,16,surface_release_dc);
}
static int d9_com_start(GuestMemory *m,D9Device *d,const D9Backend *ops,void *ctx,size_t limit){
    if(!m||g.mem||guest_map(m,BASE,0x3000))return -1;
    memset(&g,0,sizeof g);g.mem=m;g.device=d;g.backend=ops;g.backend_ctx=ctx;g.backend_limit=limit;g.clear=0xff000000u;g.clear_flags=1;
    const char *trace=getenv("TESV_D3D9_TRACE");g.trace=trace&&!strcmp(trace,"1");
    const char *tnl=getenv("TESV_D3D9_TNL_PROTO");
    g.tnl_proto=tnl&&tnl[0]&&strcmp(tnl,"0")?1:0;
    if(g.tnl_proto)fprintf(stderr,"[d3d9] CPU T&L prototype enabled (host/test only; DevCaps are unchanged)\n");
    const char *hdr=getenv("TESV_D3D9_HDR_LDR_FALLBACK");
    g.hdr_ldr_fallback=hdr&&!strcmp(hdr,"1");
    if(g.hdr_ldr_fallback)fprintf(stderr,"[d3d9] MEASUREMENT ONLY: float render targets are stored as A8R8G8B8; not shippable\n");
    const char *skip=getenv("TESV_D3D9_SHADER_SKIP_PROBE");
    g.shader_skip_probe=skip&&skip[0]&&strcmp(skip,"0")?1:0;
    g.shader_probe_logged=0;
    if(g.shader_skip_probe)fprintf(stderr,"[d3d9] MEASUREMENT ONLY: shader-skip probe active (host slice only); "
        "non-closure shaders become non-executable objects and their draws are refused\n");
    if(d)(void)d9_set_tnl_proto(d,g.tnl_proto);
    g.frame_index=g.draw_index=0;g.capture_present=0;g.capture_present_flags=0;
    init_tables();win32_set_external_resolver(d9_com_resolve,NULL);
    capture_start();
    return 0;
}
int d9_com_init(GuestMemory *m,D9Device *d){return d9_com_start(m,d,NULL,NULL,0);}
int d9_com_init_backend(GuestMemory *m,const D9Backend *ops,void *ctx,size_t limit){return d9_com_start(m,NULL,ops,ctx,limit);}
int d9_com_set_backend(const D9Backend *ops,void *ctx,size_t limit){
    if(!g.mem||g.device)return 0;
    g.backend=ops;g.backend_ctx=ctx;g.backend_limit=limit;return 1;
}
int d9_null_register(void){
    d9_null_reset();
    size_t limit=D9_NULL_RESOURCE_LIMIT;
    /* Measurement-only budget override.  The default 4 MiB null-backend limit
     * is deliberately smaller than the measured renderer's target set; this
     * env lets a census reach depth/shaders/draw without becoming a shipping
     * memory promise. */
    const char *v=getenv("TESV_D3D9_BUDGET_MB");
    if(v&&v[0]){
        char *end=NULL;unsigned long mb=strtoul(v,&end,10);
        if(end&&*end=='\0'&&mb>=8&&mb<=512){
            limit=(size_t)mb<<20;
            fprintf(stderr,"[d3d9] MEASUREMENT ONLY: null-backend resource budget %lu MiB; not shippable\n",mb);
        }
    }
    return d9_com_set_backend(d9_null_backend(),d9_null_context(),limit)?0:-1;
}
void d9_com_shutdown(void){if(!g.mem)return;capture_stop();win32_set_external_resolver(NULL,NULL);
    free_palettes();
    if(g.phase)d9_present(g.device);g.phase=0;
    if(g.device)d9_set_pixel_shader(g.device,NULL);
    for(unsigned i=0;i<g.next;++i){Obj *o=&g.o[i];if(o->lock_guest){
            if(o->kind==SURFACE)unlock_surface(o);else if(unlock(o))guest_unmap(g.mem,o->lock_guest);
        }
        /* Texture/cube face views point into a parent store owned elsewhere. */
        if(!(o->kind==SURFACE&&o->is_view))free(o->pixels);
        o->pixels=NULL;free(o->code);o->code=NULL;free(o->declaration);o->declaration=NULL;}
    /* Only a device created by CreateDevice is destroyed here; an adopted
     * D9Device belongs to the caller. */
    if(g.owns_device&&g.device){d9_destroy_device(g.device);g.device=NULL;g.owns_device=0;}
    guest_unmap(g.mem,BASE);memset(&g,0,sizeof g);}
int d9_com_pixel_shader_tev(uint32_t address,D9TevProgram *out){
    Obj *o=obj(address,PS);
    if(!o||!o->has_tev||!out)return 0;
    *out=o->tev;return 1;
}
const char *d9_com_last_shader_rejection(void){return g.last_shader_rejection;}
const char *d9_com_last_stretch_reject(void){return g.last_stretch_rejection;}
static int dll_name(const char *s){static const char name[]="d3d9.dll";if(!s)return 0;for(unsigned i=0;i<sizeof name;++i){unsigned a=(unsigned char)s[i];if(a>='A'&&a<='Z')a+=32;if(a!=(unsigned char)name[i])return 0;if(!a)return 1;}return 0;}
uint32_t d9_com_resolve(const PeImport *e,void *ctx){(void)ctx;
    if(!g.mem||!e||!e->name||!dll_name(e->dll))return 0;
    if(!strcmp(e->name,"Direct3DCreate9"))return THUNK+I_direct_create*4;
    if(!strcmp(e->name,"D3DPERF_SetOptions"))return THUNK+I_d3dperf_set_options*4;
    if(!strcmp(e->name,"D3DPERF_BeginEvent"))return THUNK+I_d3dperf_begin*4;
    if(!strcmp(e->name,"D3DPERF_EndEvent"))return THUNK+I_d3dperf_end*4;
    if(!strcmp(e->name,"D3DPERF_GetStatus"))return THUNK+I_d3dperf_status*4;
    return 0;
}
FnModule d9_com_module(void){return (FnModule){"d3d9-host",all_entries,I_COUNT+I_COM_COUNT};}
