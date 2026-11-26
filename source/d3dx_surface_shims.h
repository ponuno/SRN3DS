#pragma once
#include <stdint.h>
#include "guest.h"
#include "pe_imports.h"
#include "rt_table.h"

/* Host shims for the D3DX9_42 surface and shader entry points that are plain
 * exports (not hotpatch bodies).  This module deliberately owns only three
 * names so it can be merged with the texture/image and matrix shim modules:
 *
 *   D3DXLoadSurfaceFromMemory   same-format LDR pixels -> surface LockRect
 *   D3DXLoadSurfaceFromSurface  same-format surface -> surface CPU copy
 *   D3DXCompileShader           named E_NOTIMPL (SM3 is an architectural block)
 *
 * Everything else in d3dx9_42.dll continues through the normal PE/hotpatch
 * resolution.  Format conversion, colour-key transparency and scaling are
 * rejected with a named diagnostic instead of being approximated.
 *
 * Thunks live at D3DX_SURFACE_THUNK_BASE + 4*i, below WIN32_THUNK_BASE and
 * outside the d3d9 bridge window (0xfe000000).  The window is disjoint by
 * 0x10000 from d3dx_texture_shims (0xfd000000..0xfd000014).  Register the
 * FnModule with n3ds_lift_bind_modules(), then call d3dx_surface_init(); the
 * install does not require a D3D9 session, but the two surface-copy exports do
 * (they return D3DERR_NOTAVAILABLE otherwise). */

#define D3DX_SURFACE_THUNK_BASE UINT32_C(0xfd010000)
enum {
    D3DX_SURFACE_I_LOAD_MEMORY = 0,
    D3DX_SURFACE_I_LOAD_SURFACE,
    D3DX_SURFACE_I_COMPILE_SHADER,
    D3DX_SURFACE_I_COUNT
};

/* Binds the guest memory and installs d3dx_surface_resolve() in the win32
 * extra-resolver chain (the same chain as d3dx_texture_shims).  A D3D9 session
 * is not required: D3DXCompileShader still refuses by name, and the two
 * surface-copy exports return D3DERR_NOTAVAILABLE when d9_com_memory() is not
 * bound.  Returns 0 on success, -1 for NULL memory or a full chain. */
int d3dx_surface_init(GuestMemory *memory);
/* Drops the memory binding and the last diagnostic.  The chain slot itself is
 * cleared by win32_shutdown() and is inert while memory is NULL. */
void d3dx_surface_shutdown(void);

/* PeImportResolve-compatible lookup for d3dx9_42.dll only.  Unknown names and
 * other DLLs return 0, so PE/hotpatch resolution still sees them. */
uint32_t d3dx_surface_resolve(const PeImport *entry, void *ctx);
FnModule d3dx_surface_module(void);

/* Named reason for the most recent call, or "" after success.  Host-facing
 * diagnostic hook; never a guest import. */
const char *d3dx_surface_last_error(void);
