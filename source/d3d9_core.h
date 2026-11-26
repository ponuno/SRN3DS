#pragma once
#include <stddef.h>
#include <stdint.h>
#include "d3d9_vertex.h"
#include "d3d9_tev.h"

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
      D9_D24S8=75,D9_D24X8=77,D9_INDEX16=101,D9_INDEX32=102};
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

/* Backend description of one LDR surface participating in a blit.  native ==
 * NULL selects the implicit back buffer.  width/height are the logical D3D
 * surface dimensions (the 400x240 presentation size for the back buffer);
 * storage_width/storage_height are the native texture dimensions when they
 * differ, e.g. 512x256 for the retained lockable back buffer.  Zero means the
 * logical dimensions. */
typedef struct {
    void *native;
    unsigned width,height,format;
    unsigned storage_width,storage_height;
} D9BlitSurface;

typedef struct {
    D9BlitSurface surface;
    unsigned x,y,width,height; /* non-empty, already clipped/validated by core */
} D9BlitRegion;

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
/* One LDR render target at a time.  Handle zero is the implicit back buffer. */
int d9_create_render_target(D9Device *d,unsigned w,unsigned h,unsigned format,
                            int lockable,D9Handle *out);
int d9_set_render_target(D9Device *d,D9Handle h);
D9Handle d9_get_render_target(const D9Device *d);
int d9_render_target_info(D9Device *d,D9Handle h,unsigned *w,unsigned *height,
                          unsigned *format,unsigned *lockable,size_t *bytes);
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
int d9_set_indices(D9Device *d,D9Handle h);
unsigned d9_stage_count(const D9Device *d);
int d9_bind_texture(D9Device *d,unsigned stage,D9Handle texture);
int d9_set_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned value);
int d9_get_unit_sampler(D9Device *d,unsigned stage,unsigned id,unsigned *value);
int d9_set_texture(D9Device *d,D9Handle h);
/* Borrowed ps_3_0 TEV program; NULL selects the fixed-function backend path. */
int d9_set_pixel_shader(D9Device *d,const D9TevProgram *program);
int d9_set_transform(D9Device *d,unsigned which,const D9Matrix *m);
/* Shader constants c0..c3 as four D3D column-vector clip rows, or NULL for fixed transforms. */
int d9_set_shader_clip(D9Device *d,const D9Matrix *rows);
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
