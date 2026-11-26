#pragma once
#include "d3d9_core.h"
#include "guest.h"
#include "pe_imports.h"
#include "rt_table.h"
#include "d3d9_tev.h"

/* Single-session x86 COM bridge. The caller owns the backend context.
 * d9_com_init(m,device): adopt an already-created D9Device; device may be NULL
 *   for an enumeration-only session.
 * d9_com_init_backend(m,ops,ctx,limit): enumeration-only until the guest calls
 *   IDirect3D9::CreateDevice, which creates the D9Device through ops.
 * d9_com_set_backend(ops,ctx,limit): install the backend later (before the
 *   first successful CreateDevice) and is ignored once a device exists.
 * A non-zero limit is the d3d9_core resource budget for buffers and textures.
 * The COM surface subset is intentionally headless: one implicit back buffer
 * with no pixel access, plus CPU-backed SYSTEM/SCRATCH off-screen surfaces.
 * Set TESV_D3D9_TRACE=1 before initialization to trace exports and COM slots. */
int d9_com_init(GuestMemory *memory,D9Device *device);
int d9_com_init_backend(GuestMemory *memory,const D9Backend *ops,void *ctx,size_t limit);
int d9_com_set_backend(const D9Backend *ops,void *ctx,size_t limit); /* 0 if no session/device exists */
void d9_com_shutdown(void);
/* Host-side regression introspection for the CreatePixelShader path. */
int d9_com_pixel_shader_tev(uint32_t address,D9TevProgram *out);
const char *d9_com_last_shader_rejection(void);
/* Reason recorded by the most recent rejected StretchRect call, or "" after a
 * successful one.  Intended for host tests and trace diagnostics. */
const char *d9_com_last_stretch_reject(void);
/* Named result of the most recent SetFVF / CreateVertexBuffer / texture-stage
 * or draw rejection.  Empty after a successful operation. */
const char *d9_com_last_diagnostic(void);
/* ---- Host-side surface access for the D3DX shims ---------------------------
 * These are host APIs, not guest imports, and never touch the guest stack.
 * They reuse the same lock_surface/unlock_surface path as
 * IDirect3DSurface9::LockRect, so pool/usage/flags/byte-budget validation is
 * identical.  Rectangles are host D3D pixels; a NULL rectangle means the whole
 * surface.  The returned address is the guest mapping LockRect would expose for
 * that rectangle; row i starts at address + i*pitch. */
typedef struct {
    unsigned width, height, format;
    unsigned pool, usage, pitch;
    int render_target; /* backend target; LockRect is read-back only */
    int lockable;
} D9ComSurfaceInfo;
int d9_com_surface_info(uint32_t surface, D9ComSurfaceInfo *out);
int d9_com_surface_lock(uint32_t surface, const int32_t rect[4], unsigned flags,
                        unsigned *pitch, uint32_t *address, unsigned *rows);
int d9_com_surface_unlock(uint32_t surface);
GuestMemory *d9_com_memory(void);
uint32_t d9_com_resolve(const PeImport *entry,void *ctx);
FnModule d9_com_module(void);
/* Host-ABI texture creation for the D3DX shims.  These run the same validated
 * CreateTexture/LockRect path as the COM slots but take a guest object address
 * directly, so a non-COM thunk can create and fill an IDirect3DTexture9.
 * Return S_OK or the D3D9 HRESULT of the first rejected step; named causes also
 * land in d9_com_last_diagnostic(). */
uint32_t d9_com_host_create_texture(uint32_t device,unsigned w,unsigned h,
                                    unsigned levels,unsigned usage,unsigned format,
                                    unsigned pool,uint32_t *out_texture);
uint32_t d9_com_host_upload_texture_level(uint32_t texture,unsigned level,
                                          const void *src,size_t src_pitch,
                                          size_t src_bytes);
uint32_t d9_com_host_release(uint32_t object);
