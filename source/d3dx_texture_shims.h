#pragma once
/* Host shims for the plain (non-COM) D3DX9_42 texture exports that TESV
 * imports directly.  Before this module those IAT slots resolved to guest
 * addresses inside the staged d3dx9_42.dll body, which is not lifted or
 * executable in the slice.  The resolver here wins over the module lookup in
 * pe_link_resolve() and returns a host thunk instead.
 *
 * Implemented:
 *   D3DXGetImageInfoFromFileInMemory      DDS header -> D3DXIMAGE_INFO
 *   D3DXCreateTextureFromFileInMemory     DDS -> IDirect3DTexture9 via the
 *                                        existing d3d9_com/d3d9_core path
 *   D3DXGetImageInfoFromFileA            sandboxed file read -> same parser
 *   D3DXCreateTextureFromFileExA         file-derived subset only; the real
 *                                        ABI has 14 args (pPalette at A(12))
 *                                        and a `ret 0x38` epilogue
 *
 * Named unsupported (never a fake success):
 *   D3DXCreateCubeTextureFromFileInMemory
 *   D3DXCreateVolumeTextureFromFileInMemory
 */
#include "guest.h"
#include "pe_imports.h"
#include "rt_table.h"

/* Host thunk window for the plain D3DX9_42 texture exports.  Below
 * WIN32_THUNK_BASE (0xff000000), disjoint from the D3D9 bridge (0xfe000000)
 * and from the surface module (0xfd010000). */
#define D3DX_TEXTURE_THUNK_BASE UINT32_C(0xfd000000)

#ifdef __cplusplus
extern "C" {
#endif

/* Make d3dx9_42.dll imports resolve to host thunks.  Must be called before
 * n3ds_boot_prepare(); safe to call after d9_com_init() (the resolvers chain).
 * The file-path exports additionally require win32_init() to have configured
 * the sandbox root before they are called.  Returns 0 on success. */
int d3dx_texture_shims_install(GuestMemory *memory);
/* Drop the guest-memory binding; the resolver becomes inert.  The resolver
 * slot itself is released by win32_shutdown(). */
void d3dx_texture_shims_uninstall(void);

uint32_t d3dx_texture_shims_resolve(const PeImport *entry, void *ctx);
FnModule d3dx_texture_shims_module(void);

/* Last named rejection, for host tests and diagnostics.  Empty after a
 * successful implemented call. */
const char *d3dx_texture_shims_last_error(void);

#ifdef __cplusplus
}
#endif
