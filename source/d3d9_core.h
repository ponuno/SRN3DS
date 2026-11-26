#pragma once
#include <stddef.h>
#include <stdint.h>
#include "d3d9_vertex.h"
#include "d3d9_tev.h"
#include "d3d9_texcache.h"

/* Deliberately small fixed-function D3D9 subset, separate from guest COM ABI. */
typedef struct D9Device D9Device;
typedef uint32_t D9Handle;
typedef struct {float x,y,z,u,v;} D9Vertex; /* D3DFVF_XYZ | D3DFVF_TEX1 */
/* Extended vertex used only when a declaration/FVF supplies COLOR0.  Colour is
 * kept as normalized RGBA so a PICA attribute of GPU_FLOAT/4 is a direct copy;
 * no tint is ever dropped on the floor. */
typedef struct {float x,y,z,u,v;float r,g,b,a;} D9ColorVertex;
typedef struct {float m[4][4];} D9Matrix; /* D3D row-vector convention */
enum {D9_TRIANGLELIST=4,D9_TRIANGLESTRIP=5,D9_TRIANGLEFAN=6,D9_A8R8G8B8=21,D9_X8R8G8B8=22,
      D9_A8=28,D9_D24S8=75,D9_D24X8=77,D9_INDEX16=101,D9_INDEX32=102};
enum {D9_DXT1=0x31545844u,D9_DXT3=0x33545844u,D9_DXT5=0x35545844u};
enum {D9_POOL_DEFAULT=0,D9_POOL_MANAGED=1,D9_POOL_SYSTEMMEM=2};
enum {D9_USAGE_RENDERTARGET=1};
enum {D9_TEX_ETC1=0,D9_TEX_ETC1A4=1};
enum {D9_WORLD=0,D9_VIEW=1,D9_PROJECTION=2};
/* Fixed-function texture stage subset used by the GFx UI batch.  Only stage 0
 * contributes; later stages must be disabled. */
enum {D9_TSS_COLOROP=1,D9_TSS_COLORARG1=2,D9_TSS_COLORARG2=3,D9_TSS_ALPHAOP=4,
      D9_TSS_ALPHAARG1=5,D9_TSS_ALPHAARG2=6,D9_TSS_TEXCOORDINDEX=11};
enum {D9_TOP_DISABLE=1,D9_TOP_SELECTARG1=2,D9_TOP_SELECTARG2=3,D9_TOP_MODULATE=4};
enum {D9_TA_DIFFUSE=0,D9_TA_CURRENT=1,D9_TA_TEXTURE=2,D9_TA_CONSTANT=5};
/* D3DTEXTUREFILTERTYPE values accepted by StretchRect. */
enum {D9_FILTER_NONE=0,D9_FILTER_POINT=1,D9_FILTER_LINEAR=2};
/* A rectangle in top-left surface pixels.  D3D RECT uses LONG; INT_MIN and
 * negative coordinates are rejected by d9_blit() rather than normalised. */
typedef struct {int left,top,right,bottom;} D9Rect;
typedef struct {
    unsigned z_enable,z_write,z_func,cull,alpha_test,alpha_func,alpha_ref,blend,src_blend,dst_blend,blend_op,color_write;
    unsigned address_u,address_v,mag_filter,min_filter,mip_filter,max_mip,resident_mips;
    /* Stage 0 fixed-function state.  Defaults are SELECTARG1(TEXTURE) so the
     * existing fixed-function backend result is unchanged. */
    unsigned stage_colorop,stage_colorarg1,stage_colorarg2;
    unsigned stage_alphaop,stage_alphaarg1,stage_alphaarg2;
    unsigned stage_texcoordindex;
    const D9TevProgram *pixel_shader; /* borrowed; transient, not guest render state */
} D9DrawState;

/* --- test-only fixed-function T&L prototype --------------------------------
 * Enabled per device by d9_set_tnl_proto(); COM sets it from
 * TESV_D3D9_TNL_PROTO (host/test only).  It never changes DevCaps: the lighting
 * runs on the CPU before the existing draw path and is not a hardware-T&L
 * claim.  Supported subset: world/view/projection transforms, material
 * ambient/diffuse/emissive, up to eight directional or point lights, per-vertex
 * diffuse+ambient lighting, optional vertex-diffuse source and normal
 * normalization.  Spot lights, specular, local viewer, two-sided lighting and
 * texture transforms are rejected with names. */
#define D9_TNL_MAX_LIGHTS 8
enum {D9_LIGHT_POINT=1,D9_LIGHT_SPOT=2,D9_LIGHT_DIRECTIONAL=3};
enum {
    D9_RS_SPECULARENABLE=29,
    D9_RS_LIGHTING=137,
    D9_RS_AMBIENT=139,
    D9_RS_COLORVERTEX=141,
    D9_RS_NORMALIZENORMALS=143,
    D9_RS_DIFFUSEMATERIALSOURCE=145,
    D9_RS_LOCALVIEWER=146,
    D9_RS_AMBIENTMATERIALSOURCE=147,
    D9_RS_SPECULARMATERIALSOURCE=148,
    D9_RS_EMISSIVEMATERIALSOURCE=149,
    D9_RS_CLIPPLANEENABLE=152
};
enum {D9_MCS_MATERIAL=0,D9_MCS_COLOR1=1,D9_MCS_COLOR2=2};
typedef struct {
    unsigned type;
    float diffuse[4],specular[4],ambient[4];
    float position[3],direction[3];
    float range,falloff,attenuation0,attenuation1,attenuation2,theta,phi;
} D9LightState;
typedef struct {
    float diffuse[4],ambient[4],specular[4],emissive[4],power;
} D9MaterialState;
typedef struct {
    D9LightState lights[D9_TNL_MAX_LIGHTS];
    unsigned enabled;            /* bit i = light i enabled */
    D9MaterialState material;
    uint32_t global_ambient;     /* D3DCOLOR 0xAARRGGBB */
    unsigned lighting,color_vertex,normalize_normals,local_viewer,specular_enable;
    unsigned diffuse_source,ambient_source,specular_source,emissive_source;
} D9TnlState;

/* Backend description of one LDR surface participating in a blit.  native ==
 * NULL selects the implicit back buffer.  width/height are the logical D3D
 * surface dimensions (the 400x240 presentation size for the back buffer);
 * storage_width/storage_height are the native texture dimensions when they
 * differ, e.g. 512x256 for the retained lockable back buffer or 256x256 for a
 * bounded 1024-logical target.  Zero means the logical dimensions.  A blit
 * backend maps logical rectangles through this storage. */
typedef struct {
    void *native;
    unsigned width,height,format;
    unsigned storage_width,storage_height;
} D9BlitSurface;

typedef struct {
    D9BlitSurface surface;
    unsigned x,y,width,height; /* non-empty, already clipped/validated by core */
} D9BlitRegion;

/* Read-only cube-map view passed to an optional draw_cube backend callback.
 * The core owns all six face stores and their mip chain.  Face order is the
 * D3DCUBEMAP_FACES order: +X,-X,+Y,-Y,+Z,-Z.  pixels is face-major. */
enum {D9_CUBE_MAX_LEVELS=11};
enum {D9_CUBE_FACE_POSITIVE_X=0,D9_CUBE_FACE_NEGATIVE_X=1,
      D9_CUBE_FACE_POSITIVE_Y=2,D9_CUBE_FACE_NEGATIVE_Y=3,
      D9_CUBE_FACE_POSITIVE_Z=4,D9_CUBE_FACE_NEGATIVE_Z=5};
typedef struct {
    void *native;
    unsigned edge,levels,format;
    const uint8_t *pixels; /* six faces, face-major */
    size_t face_bytes;
    unsigned level_edge[D9_CUBE_MAX_LEVELS];
    size_t level_pitch[D9_CUBE_MAX_LEVELS];
    size_t level_bytes[D9_CUBE_MAX_LEVELS];
} D9CubeView;

/* Result of a backend target_blit() call.  Negative values are rejections and
 * must not leave a partially written destination. */
enum {
    D9_BLIT_OK=0,
    D9_BLIT_UNSUPPORTED=-1,      /* source/destination/filter combination */
    D9_BLIT_BACK_BUFFER_SRC=-2,  /* backend cannot sample the implicit back buffer */
    D9_BLIT_NO_SCENE=-3,         /* GPU path needs BeginScene/EndScene */
    D9_BLIT_UNALIGNED=-4,        /* exact-copy path requires 8-pixel alignment */
    D9_BLIT_BACKEND=-5,          /* allocation or GPU submission failure */
    D9_BLIT_ARGUMENT=-6,         /* invalid handle, filter or back-buffer metadata */
    D9_BLIT_FORMAT=-7,           /* source/destination formats differ */
    D9_BLIT_RECT=-8,             /* empty or out-of-bounds rectangle */
    D9_BLIT_SAME_SURFACE=-9      /* source and destination are the same surface */
};

typedef struct {
    int (*texture_create)(void *ctx,unsigned w,unsigned h,unsigned levels,void **native);
    int (*texture_upload)(void *ctx,void *native,unsigned level,const void *tiled,size_t bytes);
    void (*texture_delete)(void *ctx,void *native);
    int (*begin)(void *ctx,uint32_t argb);
    int (*draw)(void *ctx,const D9Vertex *vertices,size_t count,const D9Matrix *pica_clip,void *texture,const D9DrawState *state);
    void (*end)(void *ctx);
    /* Optional retryable release.  When present it replaces texture_delete for
     * cache-owned natives; non-zero means the native is still live. */
    int (*texture_release)(void *ctx,void *native);
    /* Optional pre-tiled ETC1/ETC1A4 path used by d9_create_texture_sktx. */
    int (*texture_create_encoded)(void *ctx,unsigned w,unsigned h,unsigned levels,
                                  unsigned format,void **native);
    int (*texture_upload_encoded)(void *ctx,void *native,unsigned level,
                                  const void *data,size_t bytes);
    /* Optional single LDR render-target surface.  native == NULL selects the
     * implicit back buffer. */
    int (*target_create)(void *ctx,unsigned w,unsigned h,unsigned format,
                         void **native);
    void (*target_delete)(void *ctx,void *native);
    int (*target_bind)(void *ctx,void *native);
    int (*target_clear)(void *ctx,void *native,uint32_t argb);
    int (*target_read)(void *ctx,void *native,void *dst,size_t dst_pitch,
                       unsigned x,unsigned y,unsigned w,unsigned h);
    /* Optional GPU rectangle copy used by d9_blit().  The backend must leave
     * its currently bound target unchanged (or the core rebinds it); NULL means
     * the implicit back buffer.  Filters are D9_FILTER_*.  Return D9_BLIT_OK
     * or a negative D9_BLIT_* rejection.  Implementations must not claim a
     * stretch/filter they cannot produce. */
    int (*target_blit)(void *ctx,const D9BlitRegion *dst,
                       const D9BlitRegion *src,unsigned filter);
    /* Optional colour-capable draw, appended so every existing positional
     * D9Backend initializer keeps its meaning.  Present only when the backend's
     * PICA vertex shader exposes a colour attribute and its TEV stage can
     * source primary (vertex) colour.  When a draw needs COLOR0 and this is
     * NULL the core refuses with D9_UNSUPPORTED instead of dropping the tint. */
    int (*draw_colored)(void *ctx,const D9ColorVertex *vertices,size_t count,const D9Matrix *pica_clip,void *texture,const D9DrawState *state);
    int (*draw_two)(void *ctx,const D9ColorVertex *vertices,size_t count,
                    const D9Matrix *clip,void *tex0,const D9DrawState *stage0,
                    void *tex1,const D9DrawState *stage1);
    /* Optional whole-surface BGRA8 write-back for a retained render target.
     * bgra is top-first, pitch bytes per row; the backend must copy every pixel
     * of the w x h surface.  Used by the lockable back buffer's UnlockRect.  A
     * backend that cannot write a target must not advertise it. */
    int (*target_write)(void *ctx,void *native,const void *bgra,size_t pitch,unsigned w,unsigned h);
    /* Optional clear of the depth attachment of the currently bound target.
     * flags are D3DCLEAR_ZBUFFER (2) / D3DCLEAR_STENCIL (4); depth_stencil is
     * the packed value (24-bit depth in bits 0..23, stencil in bits 24..31).
     * A backend must fail (not ignore) a clear it cannot perform. */
    int (*depth_clear)(void *ctx,unsigned flags,uint32_t depth_stencil);
    /* Optional logical viewport for the currently bound target.  The core calls
     * it after every offscreen target bind with the D3D surface dimensions, so
     * a retained power-of-two target (512x256 for 400x240) is rasterised with
     * the logical viewport instead of the padded storage size.  The implicit
     * screen is never passed here: its native rotated size is what the screen
     * clip mapping expects. */
    int (*target_viewport)(void *ctx,unsigned w,unsigned h);
    /* Optional menu-TEV submission: up to two texture units, color0 vertices,
     * and a c0..c8 float bank for external constants.  Called only when the
     * selected pixel plan needs one of those features; a NULL callback keeps
     * the older single-texture/no-vertex-colour path. */
    int (*draw_menu)(void *ctx,const D9ColorVertex *vertices,size_t count,
                     const D9Matrix *clip,void *tex0,void *tex1,
                     const D9DrawState *state,const float (*constants)[4]);
    /* Sampleable view of a render-target native handle.  Required for
     * D3DUSAGE_RENDERTARGET textures: core stores this result in
     * Texture.native instead of assuming the target itself is a texture.
     * NULL means the backend cannot sample its targets and RT-texture creation
     * is refused by name. */
    void *(*target_texture)(void *ctx,void *native);
    /* Optional cube-map texture creation/upload.  The core owns all six faces
     * and their mip chain as CPU pixels; these callbacks create/update a
     * backend-native cube handle and receive the D3D format because PICA needs
     * it for its C3D_TexInitParams.  NULL means the backend cannot represent
     * cube maps, so d9_create_cube_texture_ex() returns D9_UNSUPPORTED instead
     * of pretending a CPU-only cube can be sampled.  Deletion uses
     * texture_delete_cube when present, otherwise texture_release/delete. */
    int (*texture_create_cube)(void *ctx,unsigned edge_length,unsigned levels,
                               unsigned format,void **native);
    int (*texture_upload_cube)(void *ctx,void *native,unsigned face,unsigned level,
                               const void *data,size_t bytes);
    /* Logical-to-native viewport for a bounded render target.  Core caps a
     * target's native storage at 512 per axis and keeps the D3D width/height
     * logical.  This callback must map the full logical surface into the
     * native storage (citro3d sets the PICA viewport to native_w x native_h).
     * Required only when native_w < logical_w or native_h < logical_h; a
     * backend without it refuses such targets by name. */
    int (*target_viewport_scaled)(void *ctx,unsigned logical_w,unsigned logical_h,
                                  unsigned native_w,unsigned native_h);
    /* Optional volume-texture creation/upload.  Appended after the cube and
     * bounded-target callbacks so existing positional D9Backend initializers
     * keep their meaning.  The core owns width x height x depth slices for
     * every mip level as CPU pixels; NULL means volume textures are refused
     * by name. */
    int (*texture_create_volume)(void *ctx,unsigned w,unsigned h,unsigned depth,
                                 unsigned levels,void **native);
    int (*texture_upload_volume)(void *ctx,void *native,unsigned level,
                                 const void *data,size_t bytes);
    /* Optional cube-map draw.  Appended after the volume callbacks.  The core
     * has already selected one face per vertex: object normals are transformed
     * by the world normal matrix and the major axis picks the D3D cube face;
     * without a NORMAL element the direction (u-0.5, v-0.5, 0) is used instead.
     * faces[i] is 0..5 in +X,-X,+Y,-Y,+Z,-Z order.  NULL means this backend
     * cannot sample cube maps, so a cube-bound draw is refused by name and
     * never substitutes face 0. */
    int (*draw_cube)(void *ctx,const D9ColorVertex *vertices,size_t count,
                     const unsigned *faces,const D9Matrix *clip,
                     const D9CubeView *cube,const D9DrawState *state);
    /* Optional cube-native release.  Appended after draw_cube.  Called instead
     * of texture_release/texture_delete for cube handles; non-zero means the
     * native is still live and release stays retryable.  NULL falls back to the
     * generic release path. */
    int (*texture_delete_cube)(void *ctx,void *native);
    /* Optional format-aware 2D texture create/upload.  Appended last so all
     * existing positional initializers keep their meaning.  A8 textures use
     * this pair; a NULL pair refuses A8 by name instead of creating an RGBA
     * native with the wrong layout.  The upload callback derives the format
     * from the native handle. */
    int (*texture_create_format)(void *ctx,unsigned w,unsigned h,
                                 unsigned levels,unsigned format,
                                 void **native);
    int (*texture_upload_format)(void *ctx,void *native,unsigned level,
                                 const void *data,size_t bytes);
} D9Backend;

D9Device *d9_create_device(const D9Backend *ops,void *ctx,size_t byte_limit);
void d9_destroy_device(D9Device *d);
/* Returns D9_RELEASE_FAILED without freeing the device when a cache-owned
 * release callback fails.  Successful partial releases remain accounted, so a
 * later call can retry safely. */
int d9_destroy_device_ex(D9Device *d);
int d9_create_vertex_buffer(D9Device *d,size_t bytes,D9Handle *out);
int d9_create_index_buffer(D9Device *d,size_t bytes,unsigned format,D9Handle *out);
int d9_buffer_lock(D9Device *d,D9Handle h,size_t offset,size_t bytes,void **ptr);
int d9_buffer_unlock(D9Device *d,D9Handle h);
int d9_release_buffer(D9Device *d,D9Handle h);
int d9_create_texture(D9Device *d,unsigned w,unsigned h,unsigned format,D9Handle *out);
int d9_create_texture_levels(D9Device *d,unsigned w,unsigned h,unsigned format,unsigned levels,D9Handle *out);
/* Pool-aware guest-backed creation.  disposable is recorded for the future
 * full-mip drop contract; this pass always retains the guest source. */
int d9_create_texture_ex(D9Device *d,unsigned w,unsigned h,unsigned format,
                         unsigned levels,unsigned pool,unsigned usage,
                         int disposable,D9Handle *out);
/* Parse and validate an entire SKTX before any GPU allocation, then create and
 * upload a GPU-only ETC1/ETC1A4 texture.  The container is not retained. */
int d9_create_texture_sktx(D9Device *d,const void *sktx,size_t bytes,
                           unsigned pool,unsigned usage,D9Handle *out);
unsigned d9_texture_levels(D9Device *d,D9Handle h);
int d9_texture_level_info(D9Device *d,D9Handle h,unsigned level,unsigned *w,unsigned *height,size_t *pitch,size_t *bytes);
/* Level surface view: a pointer into the parent texture's CPU level store. */
int d9_texture_level_view(D9Device *d,D9Handle h,unsigned level,void **pixels,
                          unsigned *w,unsigned *height,size_t *pitch,size_t *bytes);
/* Re-upload a level after a surface view was written.  Marks the level
 * uploaded and commits the pending GPU copy. */
int d9_texture_surface_unlocked(D9Device *d,D9Handle h,unsigned level);
/* Copy every mip level from src's CPU store into dst and re-upload the
 * destination levels through the normal surface-unlock rules.  Both handles
 * must name 2D textures with a guest copy; formats, dimensions and level
 * counts must match.  Render-target/GPU-only textures and locked resources are
 * refused by name. */
int d9_texture_copy_levels(D9Device *d,D9Handle dst,D9Handle src);
int d9_texture_lock(D9Device *d,D9Handle h,void **ptr,size_t *pitch);
int d9_texture_unlock(D9Device *d,D9Handle h);
int d9_texture_lock_level(D9Device *d,D9Handle h,unsigned level,void **ptr,size_t *pitch);
int d9_texture_lock_level_ex(D9Device *d,D9Handle h,unsigned level,void **ptr,
                             size_t *pitch,int readonly);
int d9_texture_unlock_level(D9Device *d,D9Handle h,unsigned level);
int d9_texture_unlock_readonly(D9Device *d,D9Handle h,unsigned level);
int d9_release_texture(D9Device *d,D9Handle h);
/* D3DUSAGE_RENDERTARGET texture: a Texture handle backed by a Target's colour
 * buffer.  Only pool DEFAULT, one level and a pow2 8..1024 X8R8G8B8/A8R8G8B8
 * format are accepted.  The texture carries no guest copy; GetSurfaceLevel(0)
 * exposes the backing target and SetTexture samples it after rendering. */
int d9_create_render_texture(D9Device *d,unsigned w,unsigned h,unsigned format,
                             unsigned pool,unsigned usage,D9Handle *out);
/* Returns the backing target handle for a render-target texture, or zero. */
D9Handle d9_texture_render_target(const D9Device *d,D9Handle h);
/* ---- cube textures -------------------------------------------------------
 * Six CPU face stores (+X,-X,+Y,-Y,+Z,-Z) with a shared mip chain.  Cube
 * creation requires the backend texture_create_cube/upload_cube callbacks;
 * the null backend advertises them, the PICA backend does not.  Locking is
 * exposed per face/level for host loaders and GetCubeMapSurface views. */
enum {D9_CUBE_FACES=6};
int d9_create_cube_texture_ex(D9Device *d,unsigned edge_length,unsigned levels,
                              unsigned format,unsigned pool,unsigned usage,
                              D9Handle *out);
unsigned d9_cube_levels(D9Device *d,D9Handle h);
int d9_cube_level_info(D9Device *d,D9Handle h,unsigned level,
                       unsigned *edge_length,size_t *pitch,size_t *bytes);
int d9_cube_face_level_info(D9Device *d,D9Handle h,unsigned face,unsigned level,
                            unsigned *w,unsigned *height,size_t *pitch,
                            size_t *bytes);
int d9_cube_face_level_view(D9Device *d,D9Handle h,unsigned face,unsigned level,
                            void **pixels,unsigned *w,unsigned *height,
                            size_t *pitch,size_t *bytes);
int d9_cube_upload_face_level(D9Device *d,D9Handle h,unsigned face,unsigned level,
                              const void *src,size_t src_pitch,size_t src_bytes);
/* Re-upload a face level after a GetCubeMapSurface view was written. */
int d9_cube_face_unlocked(D9Device *d,D9Handle h,unsigned face,unsigned level);
/* Residency view of a cube: guest bytes are the six face stores plus mips,
 * gpu_bytes is the backend-native estimate.  Both live in the D9TexCache. */
int d9_cube_resource_info(D9Device *d,D9Handle h,unsigned *levels,
                          unsigned *edge_length,unsigned *format,
                          size_t *guest_bytes,size_t *gpu_bytes,int *cpu_only);
int d9_release_cube_texture(D9Device *d,D9Handle h);
/* Bind a ready cube to stage 0/1.  h==0 clears that stage.  Cube sampling is
 * not implemented by the draw path, so a later draw with a cube bound fails
 * with D9_UNSUPPORTED rather than silently dropping the cube. */
int d9_bind_cube_texture(D9Device *d,unsigned stage,D9Handle h);
/* ---- volume textures -----------------------------------------------------
 * Width x height x depth CPU slices with a shared mip chain.  Creation requires
 * the append-only backend texture_create_volume/upload_volume callbacks; the
 * null backend advertises them, the PICA backend does not.  LockBox/UnlockBox
 * are exposed through a level view; IDirect3DVolume9 is not implemented. */
int d9_create_volume_texture_ex(D9Device *d,unsigned w,unsigned h,unsigned depth,
                                unsigned levels,unsigned format,unsigned pool,
                                unsigned usage,D9Handle *out);
unsigned d9_volume_levels(D9Device *d,D9Handle h);
int d9_volume_level_info(D9Device *d,D9Handle h,unsigned level,
                         unsigned *w,unsigned *height,unsigned *depth,
                         size_t *pitch,size_t *plane_bytes,size_t *bytes);
int d9_volume_level_view(D9Device *d,D9Handle h,unsigned level,void **pixels,
                         unsigned *w,unsigned *height,unsigned *depth,
                         size_t *pitch,size_t *plane_bytes,size_t *bytes);
int d9_volume_upload_level(D9Device *d,D9Handle h,unsigned level,
                           const void *src,size_t src_pitch,size_t src_slice_pitch,
                           size_t src_bytes);
/* Re-upload a whole level after a LockBox view was written. */
int d9_volume_level_unlocked(D9Device *d,D9Handle h,unsigned level);
/* Residency view of a volume: guest bytes are all depth slices plus mips,
 * gpu_bytes is the backend-native estimate.  Both live in the D9TexCache. */
int d9_volume_resource_info(D9Device *d,D9Handle h,unsigned *levels,
                            unsigned *w,unsigned *height,unsigned *depth,
                            unsigned *format,size_t *guest_bytes,
                            size_t *gpu_bytes,int *cpu_only);
int d9_release_volume_texture(D9Device *d,D9Handle h);
/* Bind a ready volume to stage 0/1.  h==0 clears that stage.  Volume sampling
 * is not implemented by the draw path, so a later draw with a volume bound
 * fails with D9_UNSUPPORTED rather than silently dropping it. */
int d9_bind_volume_texture(D9Device *d,unsigned stage,D9Handle h);
/* One LDR render target at a time.  Handle zero is the implicit back buffer. */
int d9_create_render_target(D9Device *d,unsigned w,unsigned h,unsigned format,
                            int lockable,D9Handle *out);
int d9_set_render_target(D9Device *d,D9Handle h);
D9Handle d9_get_render_target(const D9Device *d);
int d9_render_target_info(D9Device *d,D9Handle h,unsigned *w,unsigned *height,
                          unsigned *format,unsigned *lockable,size_t *bytes);
/* Native colour-storage dimensions for a render target.  These are equal to
 * the logical width/height for 512x512 and smaller targets, and capped at 512
 * per axis for larger logical targets.  `bytes` from d9_render_target_info is
 * the native allocation size, not logical_w*logical_h*4. */
int d9_render_target_storage_info(D9Device *d,D9Handle h,unsigned *w,unsigned *height);
/* Effective native cap for explicit render targets on this device.  Captured
 * at device creation from TESV_D3D9_TARGET_NATIVE_MAX (64..512, rounded down to
 * a power of two); default 256.  The retained back buffer is not capped. */
unsigned d9_target_native_max(const D9Device *d);
/* TESV_D3D9_SCALED_READBACK=1 (captured at device creation).  Off by default:
 * scaled LockRect/UnlockRect are named lossy refusals.  On: LockRect reads
 * native pixels and nearest-upscales to the logical rectangle; UnlockRect
 * nearest-downscales the logical rectangle back into native storage. */
int d9_scaled_readback_enabled(const D9Device *d);
int d9_render_target_read(D9Device *d,D9Handle h,void *dst,size_t dst_pitch,
                          unsigned x,unsigned y,unsigned w,unsigned height);
int d9_release_render_target(D9Device *d,D9Handle h);
/* Presentation metadata for the implicit back buffer (handle zero).  COM calls
 * this from CreateDevice and Reset; a device without it rejects blits that
 * involve the back buffer instead of guessing dimensions. */
int d9_set_back_buffer_info(D9Device *d,unsigned w,unsigned h,unsigned format);
/* Presentation metadata plus the D3DPRESENTFLAG_LOCKABLE_BACKBUFFER promise.
 * lockable != 0 creates a retained offscreen colour target (next power of two
 * at or above w x h, RGBA8) that BeginScene binds, Present blits to the screen
 * and LockRect can read/write.  The backend must provide target_create,
 * target_delete, target_bind, target_clear, target_read, target_write,
 * target_blit and target_viewport; otherwise D9_UNSUPPORTED is returned and no
 * promise is made.  A non-lockable back buffer keeps the previous
 * implicit-screen behaviour. */
int d9_set_back_buffer_info_ex(D9Device *d,unsigned w,unsigned h,unsigned format,int lockable);
/* Retained back-buffer render-target handle, or 0 when the back buffer is the
 * implicit screen.  The handle is owned by the device and must not be released
 * by the caller. */
D9Handle d9_back_buffer_target(const D9Device *d);
/* True when the backend can implement D3DPRESENTFLAG_LOCKABLE_BACKBUFFER. */
int d9_backend_supports_lockable_back_buffer(const D9Backend *ops);
/* Evidence that the retained back buffer exists (it is a real offscreen
 * target) and that LockRect/Present can do what the flag promises. */
int d9_back_buffer_info_ex(const D9Device *d,unsigned *w,unsigned *h,
                           unsigned *format,unsigned *tex_w,unsigned *tex_h,
                           int *lockable);
int d9_get_back_buffer_info(const D9Device *d,unsigned *w,unsigned *h,
                            unsigned *format);
/* Write a rectangle of a render target from top-first BGRA rows.  This is a
 * real read-modify-write of the backend surface; it fails when the backend has
 * no target_write or no target_read. */
int d9_render_target_write(D9Device *d,D9Handle h,const void *src,size_t src_pitch,
                           unsigned x,unsigned y,unsigned w,unsigned height);
/* --- explicit depth-stencil surfaces -------------------------------------
 * PICA has exactly one depth attachment, attached to the render target.  A
 * depth surface therefore describes that attachment: creation validates the
 * format/size against the back buffer, binding selects it, clearing reaches the
 * GPU attachment of the currently bound target, and NULL restores "no surface
 * bound" (depth testing may still use the attachment, but Clear(Z) is then
 * rejected).  D24S8 is the measured TESV request; D24X8 is accepted because it
 * is the same 32-bit attachment. */
int d9_create_depth_surface(D9Device *d,unsigned w,unsigned h,unsigned format,
                            unsigned multisample,unsigned quality,int lockable,
                            D9Handle *out);
int d9_release_depth_surface(D9Device *d,D9Handle h);
int d9_set_depth_surface(D9Device *d,D9Handle h);
D9Handle d9_get_depth_surface(const D9Device *d);
int d9_depth_surface_info(D9Device *d,D9Handle h,unsigned *width,unsigned *height,
                          unsigned *format);
/* Clear the bound depth surface's attachment.  Requires an active scene, a
 * bound depth surface whose size matches the active target, and a backend
 * depth_clear.  flags may be D3DCLEAR_ZBUFFER (2), D3DCLEAR_STENCIL (4) or
 * both. */
int d9_clear_depth(D9Device *d,unsigned flags,float z,uint32_t stencil);
/* StretchRect-class GPU copy between LDR surfaces.  dst/src are render-target
 * handles (zero = the back buffer: the retained target when lockable, the
 * implicit screen otherwise); either rect may be NULL for the whole surface.
 * The source and destination must be different, must have the same format, and
 * must stay inside their surfaces.  CPU-only surfaces are not part of this
 * API.  Returns D9_OK, D9_INVALID for caller errors, D9_MEMORY, or D9_BACKEND
 * when the backend rejects/fails the copy; d9_blit_last_error() then returns
 * the D9_BLIT_* reason.  The selected render target is re-bound after every
 * call, including failures. */
int d9_blit(D9Device *d,D9Handle dst,const D9Rect *dst_rect,
            D9Handle src,const D9Rect *src_rect,unsigned filter);
int d9_blit_last_error(const D9Device *d);
/* Inverse of d9_swizzle_rgba8 for a top-first BGRA rectangle. */
int d9_unswizzle_rgba8(void *dst,size_t dst_pitch,const void *src,
                       unsigned w,unsigned h,unsigned format,
                       unsigned x,unsigned y,unsigned rw,unsigned rh);
int d9_set_stream_source(D9Device *d,D9Handle h,size_t offset,size_t stride);
int d9_set_stream(D9Device *d,unsigned stream,D9Handle h,size_t offset,size_t stride);
int d9_set_vertex_declaration(D9Device *d,const D9Decl *decl);
/* Viewport size used to map POSITIONT (XYZRHW) pixels to PICA clip space. */
int d9_set_viewport_size(D9Device *d,unsigned width,unsigned height);
/* Closed D3D9 SetTextureStageState subset.  Stage 0 accepts COLOROP/ALPHAOP/
 * ARG1/ARG2 and TEXCOORDINDEX 0..7; a two-stage backend also accepts the same
 * subset on stage 1 (TEXCOORDINDEX 0 only).  Remaining stages may only be
 * DISABLE.  Unsupported values return D9_UNSUPPORTED and name the rejection in
 * d9_last_draw_diagnostic(). */
int d9_set_texture_stage_state(D9Device *d,unsigned stage,unsigned type,unsigned value);
/* Read back exactly what d9_set_texture_stage_state() accepted.  Stages above
 * the backend stage count report COLOROP/ALPHAOP as DISABLE; any other type
 * there is D9_UNSUPPORTED.  An unsupported type leaves *value untouched. */
int d9_get_texture_stage_state(D9Device *d,unsigned stage,unsigned type,unsigned *value);
int d9_set_indices(D9Device *d,D9Handle h);
unsigned d9_stage_count(const D9Device *d);
int d9_bind_texture(D9Device *d,unsigned stage,D9Handle texture);
int d9_set_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned value);
int d9_get_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned *value);
int d9_set_texture(D9Device *d,D9Handle h);
/* Borrowed ps_3_0 TEV program; NULL selects the fixed-function backend path. */
int d9_set_pixel_shader(D9Device *d,const D9TevProgram *program);
/* Material constants for menu TEV plans: c0..c7 are the D3D registers, c8 is
 * the internal ambient+emissive fold.  Values are copied; non-finite entries
 * are rejected.  This is evidence for the menu path, not a D3D9 cap.
 * Rows written through d9_set_ps_constants_f() take priority over this bank
 * and are re-applied after every upload. */
int d9_set_tev_constants(D9Device *d,const float values[9][4]);
/* Read back the nine-row bank actually passed to the menu draw. */
int d9_get_tev_constants(const D9Device *d,float out[9][4]);
/* Pixel-shader constants.  The float window is c0..c7: that is the PICA TEV
 * external-constant limit (D9TEV_MAX_CONSTANTS); a wider D3D9 range is not
 * advertised or accepted.  Integer and bool registers are stored for honest
 * Set/Get round-trip but the accepted ps_3_0 subset has no integer, predicate
 * or flow-control opcode, so no accepted program consumes them.  Counts are
 * vectors for F/I and individual BOOLs for B; n==0 is a valid no-op. */
#define D9_PS_CONST_F_REGS 8u
#define D9_PS_CONST_I_REGS 8u
#define D9_PS_CONST_B_REGS 8u
#define D9_VS_CONST_I_REGS 8u
#define D9_VS_CONST_B_REGS 8u
int d9_set_ps_constants_f(D9Device *d,unsigned start,const float *values,unsigned count);
int d9_get_ps_constants_f(const D9Device *d,unsigned start,float *values,unsigned count);
int d9_set_ps_constants_i(D9Device *d,unsigned start,const int32_t *values,unsigned count);
int d9_get_ps_constants_i(const D9Device *d,unsigned start,int32_t *values,unsigned count);
int d9_set_ps_constants_b(D9Device *d,unsigned start,const int32_t *values,unsigned count);
int d9_get_ps_constants_b(const D9Device *d,unsigned start,int32_t *values,unsigned count);
/* Vertex-shader integer/bool registers.  Same bounded API-state treatment as
 * PS I/B: exact round-trip, but the accepted vertex subset has no integer,
 * predicate or flow-control opcode, so they do not feed a draw.  The float
 * c0..c8 bank remains the legacy menu material bank documented above. */
int d9_set_vs_constants_i(D9Device *d,unsigned start,const int32_t *values,unsigned count);
int d9_get_vs_constants_i(const D9Device *d,unsigned start,int32_t *values,unsigned count);
int d9_set_vs_constants_b(D9Device *d,unsigned start,const int32_t *values,unsigned count);
int d9_get_vs_constants_b(const D9Device *d,unsigned start,int32_t *values,unsigned count);
/* Bits 0..7 of the float registers that have been written. */
unsigned d9_ps_constants_f_mask(const D9Device *d);
int d9_set_transform(D9Device *d,unsigned which,const D9Matrix *m);
/* Test-only CPU T&L prototype.  d9_set_tnl_proto resets the fixed-function
 * lighting state to the D3D defaults when enabling; disabling leaves the
 * existing path untouched.  None of these calls changes DevCaps.
 * d9_set_light returns D9_UNSUPPORTED for a spot light and D9_INVALID for a
 * bad index/type/NaN; d9_multiply_transform implements World = World * m
 * (row-vector convention) for WORLD/VIEW/PROJECTION. */
void d9_tnl_defaults(D9TnlState *s);
int d9_set_tnl_proto(D9Device *d,int enabled);
int d9_tnl_proto(const D9Device *d);
int d9_set_light(D9Device *d,unsigned index,const D9LightState *light);
int d9_get_light(const D9Device *d,unsigned index,D9LightState *out);
int d9_set_light_enable(D9Device *d,unsigned index,int enable);
int d9_get_light_enable(const D9Device *d,unsigned index,int *enable);
int d9_set_material(D9Device *d,const D9MaterialState *m);
int d9_get_material(const D9Device *d,D9MaterialState *m);
/* Complete stored fixed-function T&L state for state-block capture/apply.  This
 * does not enable the prototype and does not change DevCaps. */
int d9_get_tnl_state(const D9Device *d,D9TnlState *out);
int d9_set_tnl_state(D9Device *d,const D9TnlState *state);
int d9_multiply_transform(D9Device *d,unsigned which,const D9Matrix *m);
/* Shader constants c0..c3 as four D3D column-vector clip rows, or NULL for fixed transforms. */
int d9_set_shader_clip(D9Device *d,const D9Matrix *rows);
/* Six D3D user clip planes.  Values are copied and must be finite.  PICA has
 * no user clip-plane stage, so any draw with D3DRS_CLIPPLANEENABLE != 0 is a
 * named D9_UNSUPPORTED refusal; disabled planes are stored but never applied. */
#define D9_MAX_CLIP_PLANES 6u
int d9_set_clip_plane(D9Device *d,unsigned index,const float plane[4]);
int d9_get_clip_plane(const D9Device *d,unsigned index,float plane[4]);
/* D3D9 numeric D3DRS/D3DSAMP values. Unsupported enum/state returns D9_INVALID. */
int d9_set_render_state(D9Device *d,unsigned id,unsigned value);
int d9_get_render_state(D9Device *d,unsigned id,unsigned *value);
int d9_set_sampler_state(D9Device *d,unsigned id,unsigned value);
int d9_get_sampler_state(D9Device *d,unsigned id,unsigned *value);
int d9_begin_scene_ex(D9Device *d,uint32_t clear_argb,int clear_target);
int d9_begin_scene(D9Device *d,uint32_t clear_argb);
int d9_draw_primitive(D9Device *d,unsigned type,unsigned start,unsigned primitives);
int d9_draw_indexed_primitive(D9Device *d,unsigned type,int32_t base_vertex,unsigned start_index,unsigned primitives);
int d9_draw_indexed_range(D9Device *d,unsigned type,int32_t base_vertex,unsigned min_vertex,unsigned vertex_count,unsigned start_index,unsigned primitives);
/* D3D9 stream-zero ("UP") draws.  vertex_data is borrowed for the duration of
 * the call and never retained.  The bound stream 0, index buffer, render state,
 * texture and transforms are not modified by these calls.  The currently
 * supported fetch subset is the FVF XYZ|TEX1 case or an active vertex
 * declaration; vertex_stride must cover that subset's required bytes.
 * Return D9_INVALID for null data, a zero/short stride, a non-triangle type,
 * zero or oversized primitive counts, an unsupported index format, malformed
 * vertex ranges, or indices outside [min_vertex, min_vertex+vertex_count). */
int d9_draw_primitive_up(D9Device *d,unsigned type,unsigned primitives,
                         const void *vertex_data,unsigned vertex_stride);
int d9_draw_indexed_primitive_up(D9Device *d,unsigned type,unsigned min_vertex,
                                 unsigned vertex_count,unsigned primitives,
                                 const void *index_data,unsigned index_format,
                                 const void *vertex_data,unsigned vertex_stride);
int d9_end_scene(D9Device *d);
int d9_present(D9Device *d);
size_t d9_memory_used(const D9Device *d);
/* Remaining device budget in bytes (limit - d9_memory_used), never negative.
 * This is the honest GetAvailableTextureMem estimate. */
size_t d9_available_texture_mem(const D9Device *d);
/* Validate the current fixed-function/TEV state against the backend capability
 * set.  On success *passes is 1.  On failure the reason is in
 * d9_last_draw_diagnostic(); returns D9_UNSUPPORTED or D9_INVALID. */
int d9_validate_device(D9Device *d,unsigned *passes);
/* EvictManagedResources backing check: every MANAGED 2D/cube/volume resource
 * must still carry its guest copy, which is the re-upload source.  The method
 * itself is a no-op (it does not free resident GPU copies), so this only
 * answers whether that no-op is honest. */
int d9_evict_managed_resources(D9Device *d);
/* Read-only copy of the device's texture-cache counters (cube/volume included). */
int d9_texture_cache_stats(D9Device *d,D9TexCacheStats *out);
int d9_texture_layout(unsigned w,unsigned h,unsigned format,size_t *pitch,size_t *bytes);
/* Split texture residency accounting and pool/usage metadata. */
int d9_texture_resource_info(D9Device *d,D9Handle h,unsigned *pool,
                             unsigned *usage,unsigned *format,
                             unsigned *levels,size_t *guest_bytes,
                             size_t *gpu_bytes,int *cpu_only,int *gpu_only);

/* Read-only introspection for diagnostic consumers (the first-draw capture).
 * All pointers are borrowed and stay valid until the matching object is
 * released, unlocked or the device is destroyed.  None of these calls mutates
 * device state or guest memory. */
int d9_get_draw_state(const D9Device *d,D9DrawState *out);
int d9_get_transform(const D9Device *d,unsigned which,D9Matrix *out);
int d9_get_stream_info(const D9Device *d,unsigned stream,D9Handle *buffer,
                       size_t *offset,size_t *stride);
int d9_get_index_info(const D9Device *d,D9Handle *buffer,unsigned *format);
int d9_get_buffer_data(const D9Device *d,D9Handle buffer,
                       const void **data,size_t *bytes);
int d9_get_texture_capture(const D9Device *d,D9Handle texture,unsigned level,
                           const void **pixels,size_t *pitch,size_t *bytes,
                           unsigned *width,unsigned *height,unsigned *format);
/* Decoded XYZ+UV scratch stream of the last successful d9_draw_*() call. */
int d9_get_last_draw_vertices(const D9Device *d,const D9Vertex **vertices,
                              size_t *count);
/* Decoded XYZ+UV+RGBA stream of the last draw that carried COLOR0. */
int d9_get_last_draw_color_vertices(const D9Device *d,const D9ColorVertex **vertices,
                                    size_t *count);
/* Named reason for the most recent draw/stage-state rejection.  Never NULL;
 * empty string when the last operation succeeded. */
const char *d9_last_draw_diagnostic(const D9Device *d);

void d9_matrix_identity(D9Matrix *m);
void d9_matrix_multiply(D9Matrix *out,const D9Matrix *a,const D9Matrix *b);
void d9_pica_transform(D9Matrix *out,const D9Matrix *world,const D9Matrix *view,const D9Matrix *projection);
/* Convert linear little-endian D3D BGRA into PICA RGBA8 tile order. */
int d9_swizzle_rgba8(void *dst,size_t dst_bytes,const void *src,size_t pitch,unsigned w,unsigned h,unsigned format);
