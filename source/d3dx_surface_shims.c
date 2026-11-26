/* Plain D3DX9_42 surface/shader entry points.
 *
 * The three exports below are not hotpatch wrappers: TESV imports them by name
 * and the loader would otherwise bind the real D3DX body at 0x080b... and fault
 * there.  Resolving them to host thunks keeps the call stream inside the
 * already-implemented D3D9 COM surface path.
 *
 * Honesty rules for this module:
 *   - only same-format X8R8G8B8/A8R8G8B8 pixels are copied;
 *   - format conversion, colour-key transparency and scaling return
 *     SHIM_HR_NOTAVAILABLE with a named reason, never a partial fake result;
 *   - D3DXCompileShader returns E_NOTIMPL and zeroes caller outputs; it never
 *     produces empty bytecode that would look like a successful compile.
 *
 * All three are stdcall: the epilogue pops the return address plus exactly the
 * documented argument count. */
#include "d3dx_surface_shims.h"
#include "d3d9_com.h"
#include "d3d9_core.h"
#include "win32_shims.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SHIM_HR_INVALIDCALL UINT32_C(0x8876086c)
#define SHIM_HR_NOTAVAILABLE UINT32_C(0x8876086a)
#define SHIM_HR_OUTOFMEMORY UINT32_C(0x8876017c)
#define SHIM_E_NOTIMPL UINT32_C(0x80004001)

#define D3DX_FILTER_NONE 0u
#define D3DX_FILTER_POINT 1u
#define D3DX_FILTER_LINEAR 2u
#define D3DX_DEFAULT UINT32_C(0xffffffff)

static GuestMemory *memory;
static char last_error[320];

static uint32_t arg(CPU *c, unsigned n) { return M32(c->r[4] + 4 + n * 4); }

static void diagnostic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof last_error, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[d3dx] %s\n", last_error);
}

static void ret(CPU *c, uint32_t value, unsigned args) {
    c->r[0] = value;
    c->r[4] += 4u + 4u * args;
}

static const char *format_name(unsigned format) {
    switch (format) {
    case D9_X8R8G8B8: return "X8R8G8B8";
    case D9_A8R8G8B8: return "A8R8G8B8";
    default: return "a non-LDR format";
    }
}

static int ldr_format(unsigned format) {
    return format == D9_X8R8G8B8 || format == D9_A8R8G8B8;
}

/* 0 = NULL (whole surface), 1 = read, -1 = outside guest memory. */
static int read_rect(uint32_t address, int32_t out[4]) {
    if (!address) return 0;
    const int32_t *p = memory ? guest_ptr(memory, address, 16u) : NULL;
    if (!p) return -1;
    memcpy(out, p, 16u);
    return 1;
}

static int rect_inside(const int32_t r[4], unsigned width, unsigned height) {
    return r[0] >= 0 && r[1] >= 0 && r[2] > r[0] && r[3] > r[1] &&
           (uint64_t)r[2] <= width && (uint64_t)r[3] <= height;
}

static int filter_supported(uint32_t filter) {
    return filter == D3DX_DEFAULT || filter == D3DX_FILTER_NONE ||
           filter == D3DX_FILTER_POINT || filter == D3DX_FILTER_LINEAR;
}

/* Maps the d3d9_core lock result onto a guest HRESULT with a role-specific
 * reason.  A backend refusal is NOTAVAILABLE, not a fake success. */
static uint32_t lock_failure(int rc, const char *role) {
    switch (rc) {
    case D9_INVALID:
        diagnostic("%s: LockRect rejected the object, rectangle or lock flags "
                   "(only CPU off-screen and lockable surfaces can be locked)", role);
        return SHIM_HR_INVALIDCALL;
    case D9_MEMORY:
        diagnostic("%s: LockRect could not map guest memory for the surface span", role);
        return SHIM_HR_OUTOFMEMORY;
    case D9_BACKEND:
    case D9_UNSUPPORTED:
    default:
        diagnostic("%s: the backend has no readable pixels for this surface "
                   "(render targets are not CPU-lockable here)", role);
        return SHIM_HR_NOTAVAILABLE;
    }
}

static uint32_t unlock_failure(int rc, const char *role) {
    (void)role;
    switch (rc) {
    case D9_MEMORY: return SHIM_HR_OUTOFMEMORY;
    case D9_BACKEND:
    case D9_UNSUPPORTED: return SHIM_HR_NOTAVAILABLE;
    default: return SHIM_HR_INVALIDCALL;
    }
}

/* Shared source-rectangle resolution for FromMemory.  A NULL source rect means
 * "copy the destination-sized block from the origin", which is the only source
 * extent implied by a raw memory pointer. */
static uint32_t memory_source_span(uint32_t src, uint32_t src_pitch,
                                   const int32_t sr[4], uint32_t *base,
                                   size_t *span, size_t *row_bytes) {
    uint64_t width = (uint64_t)(sr[2] - sr[0]);
    uint64_t height = (uint64_t)(sr[3] - sr[1]);
    uint64_t bytes = width * 4u;
    if (!src) {
        diagnostic("D3DXLoadSurfaceFromMemory: source memory pointer is NULL");
        return SHIM_HR_INVALIDCALL;
    }
    if (src_pitch < bytes) {
        diagnostic("D3DXLoadSurfaceFromMemory: source pitch %lu is smaller than "
                   "the %llu-byte source row",
                   (unsigned long)src_pitch, (unsigned long long)bytes);
        return SHIM_HR_INVALIDCALL;
    }
    uint64_t at = (uint64_t)src + (uint64_t)sr[1] * src_pitch +
                  (uint64_t)sr[0] * 4u;
    uint64_t bytes_span = (height - 1u) * src_pitch + bytes;
    if (at > UINT32_MAX || bytes_span > SIZE_MAX ||
        !guest_ptr(memory, (uint32_t)at, (size_t)bytes_span)) {
        diagnostic("D3DXLoadSurfaceFromMemory: source rectangle %ld,%ld..%ld,%ld "
                   "with pitch %lu is outside guest memory",
                   (long)sr[0], (long)sr[1], (long)sr[2], (long)sr[3],
                   (unsigned long)src_pitch);
        return SHIM_HR_INVALIDCALL;
    }
    *base = (uint32_t)at;
    *span = (size_t)bytes_span;
    *row_bytes = (size_t)bytes;
    return 0;
}

/* IDirect3DSurface9::LockRect(reinterpret_cast<void **>(&locked), pRect, 0). */
static uint32_t surface_lock(uint32_t surface, const int32_t rect[4],
                             unsigned flags, unsigned *pitch, uint32_t *address,
                             unsigned *rows) {
    return d9_com_surface_lock(surface, rect, flags, pitch, address, rows);
}

static uint32_t load_from_memory(CPU *c) {
    uint32_t dest = arg(c, 0);
    /* Palettes are irrelevant for 32-bit LDR formats and are ignored. */
    uint32_t dest_rect_addr = arg(c, 2);
    uint32_t src = arg(c, 3), src_format = arg(c, 4), src_pitch = arg(c, 5);
    uint32_t src_rect_addr = arg(c, 7), filter = arg(c, 8), color_key = arg(c, 9);
    D9ComSurfaceInfo info;
    uint32_t hr;

    last_error[0] = 0;
    if (!memory || !d9_com_memory()) {
        diagnostic("D3DXLoadSurfaceFromMemory: no D3D9 session is bound");
        return SHIM_HR_NOTAVAILABLE;
    }
    if (!dest || d9_com_surface_info(dest, &info)) {
        diagnostic("D3DXLoadSurfaceFromMemory: destination 0x%08lx is not a live "
                   "IDirect3DSurface9", (unsigned long)dest);
        return SHIM_HR_INVALIDCALL;
    }
    if (info.render_target) {
        diagnostic("D3DXLoadSurfaceFromMemory: destination is a render target or "
                   "back buffer and cannot be CPU-written");
        return SHIM_HR_NOTAVAILABLE;
    }
    if (!ldr_format(info.format) || !ldr_format(src_format)) {
        diagnostic("D3DXLoadSurfaceFromMemory: only X8R8G8B8/A8R8G8B8 LDR "
                   "pixels are supported (source %s, destination %s)",
                   format_name(src_format), format_name(info.format));
        return SHIM_HR_NOTAVAILABLE;
    }
    if (src_format != info.format) {
        diagnostic("D3DXLoadSurfaceFromMemory: source %s -> destination %s "
                   "format conversion is not implemented",
                   format_name(src_format), format_name(info.format));
        return SHIM_HR_NOTAVAILABLE;
    }
    if (!filter_supported(filter)) {
        diagnostic("D3DXLoadSurfaceFromMemory: filter 0x%08lx is not "
                   "NONE/POINT/LINEAR (equal-size copies only)",
                   (unsigned long)filter);
        return SHIM_HR_NOTAVAILABLE;
    }
    if (color_key) {
        diagnostic("D3DXLoadSurfaceFromMemory: colour-key 0x%08lx "
                   "transparency is not implemented", (unsigned long)color_key);
        return SHIM_HR_NOTAVAILABLE;
    }

    int32_t dr[4] = {0, 0, (int32_t)info.width, (int32_t)info.height};
    int has_dest = read_rect(dest_rect_addr, dr);
    if (has_dest < 0) {
        diagnostic("D3DXLoadSurfaceFromMemory: destination rectangle is outside "
                   "guest memory");
        return SHIM_HR_INVALIDCALL;
    }
    if (!rect_inside(dr, info.width, info.height)) {
        diagnostic("D3DXLoadSurfaceFromMemory: destination rectangle "
                   "%ld,%ld..%ld,%ld is empty or outside the %ux%u surface",
                   (long)dr[0], (long)dr[1], (long)dr[2], (long)dr[3],
                   info.width, info.height);
        return SHIM_HR_INVALIDCALL;
    }

    int32_t sr[4] = {0, 0, dr[2] - dr[0], dr[3] - dr[1]};
    int has_src = read_rect(src_rect_addr, sr);
    if (has_src < 0) {
        diagnostic("D3DXLoadSurfaceFromMemory: source rectangle is outside "
                   "guest memory");
        return SHIM_HR_INVALIDCALL;
    }
    if (sr[0] < 0 || sr[1] < 0 || sr[2] <= sr[0] || sr[3] <= sr[1]) {
        diagnostic("D3DXLoadSurfaceFromMemory: source rectangle is empty or "
                   "negative");
        return SHIM_HR_INVALIDCALL;
    }
    if (sr[2] - sr[0] != dr[2] - dr[0] ||
        sr[3] - sr[1] != dr[3] - dr[1]) {
        diagnostic("D3DXLoadSurfaceFromMemory: scaling a %ldx%ld source "
                   "rectangle into %ldx%ld is not implemented",
                   (long)(sr[2] - sr[0]), (long)(sr[3] - sr[1]),
                   (long)(dr[2] - dr[0]), (long)(dr[3] - dr[1]));
        return SHIM_HR_NOTAVAILABLE;
    }

    uint32_t src_base = 0;
    size_t src_span = 0, row_bytes = 0;
    hr = memory_source_span(src, src_pitch, sr, &src_base, &src_span, &row_bytes);
    if (hr) return hr;

    unsigned pitch = 0, rows = 0;
    uint32_t address = 0;
    int rc = d9_com_surface_lock(dest, dr, 0, &pitch, &address, &rows);
    if (rc) return lock_failure(rc, "D3DXLoadSurfaceFromMemory destination");
    size_t dst_span = (size_t)(rows - 1u) * pitch + row_bytes;
    uint8_t *dst = guest_ptr(memory, address, dst_span);
    const uint8_t *source = guest_ptr(memory, src_base, src_span);
    if (!dst || !source) {
        (void)d9_com_surface_unlock(dest);
        diagnostic("D3DXLoadSurfaceFromMemory: locked destination or source "
                   "span is no longer mapped");
        return SHIM_HR_INVALIDCALL;
    }
    for (unsigned y = 0; y < rows; ++y)
        memcpy(dst + (size_t)y * pitch, source + (size_t)y * src_pitch, row_bytes);

    int urc = d9_com_surface_unlock(dest);
    if (urc) {
        diagnostic("D3DXLoadSurfaceFromMemory: destination UnlockRect failed (%d)",
                   urc);
        return unlock_failure(urc, "D3DXLoadSurfaceFromMemory destination");
    }
    return 0;
}

static uint32_t load_from_surface(CPU *c) {
    uint32_t dest = arg(c, 0), dest_rect_addr = arg(c, 2);
    uint32_t src = arg(c, 3), src_rect_addr = arg(c, 5);
    uint32_t filter = arg(c, 6), color_key = arg(c, 7);
    D9ComSurfaceInfo di, si;

    last_error[0] = 0;
    if (!memory || !d9_com_memory()) {
        diagnostic("D3DXLoadSurfaceFromSurface: no D3D9 session is bound");
        return SHIM_HR_NOTAVAILABLE;
    }
    if (!dest || d9_com_surface_info(dest, &di) ||
        !src || d9_com_surface_info(src, &si)) {
        diagnostic("D3DXLoadSurfaceFromSurface: source 0x%08lx or destination "
                   "0x%08lx is not a live IDirect3DSurface9",
                   (unsigned long)src, (unsigned long)dest);
        return SHIM_HR_INVALIDCALL;
    }
    if (di.render_target) {
        diagnostic("D3DXLoadSurfaceFromSurface: destination is a render target or "
                   "back buffer and cannot be CPU-written");
        return SHIM_HR_NOTAVAILABLE;
    }
    if (dest == src) {
        diagnostic("D3DXLoadSurfaceFromSurface: source and destination are the "
                   "same surface");
        return SHIM_HR_INVALIDCALL;
    }
    if (!ldr_format(di.format) || !ldr_format(si.format)) {
        diagnostic("D3DXLoadSurfaceFromSurface: only X8R8G8B8/A8R8G8B8 LDR "
                   "surfaces are supported");
        return SHIM_HR_NOTAVAILABLE;
    }
    if (di.format != si.format) {
        diagnostic("D3DXLoadSurfaceFromSurface: source %s -> destination %s "
                   "format conversion is not implemented",
                   format_name(si.format), format_name(di.format));
        return SHIM_HR_NOTAVAILABLE;
    }
    if (!filter_supported(filter)) {
        diagnostic("D3DXLoadSurfaceFromSurface: filter 0x%08lx is not "
                   "NONE/POINT/LINEAR", (unsigned long)filter);
        return SHIM_HR_NOTAVAILABLE;
    }
    if (color_key) {
        diagnostic("D3DXLoadSurfaceFromSurface: colour-key 0x%08lx "
                   "transparency is not implemented", (unsigned long)color_key);
        return SHIM_HR_NOTAVAILABLE;
    }

    int32_t dr[4] = {0, 0, (int32_t)di.width, (int32_t)di.height};
    int has_dest = read_rect(dest_rect_addr, dr);
    if (has_dest < 0) {
        diagnostic("D3DXLoadSurfaceFromSurface: destination rectangle is outside "
                   "guest memory");
        return SHIM_HR_INVALIDCALL;
    }
    if (!rect_inside(dr, di.width, di.height)) {
        diagnostic("D3DXLoadSurfaceFromSurface: destination rectangle "
                   "%ld,%ld..%ld,%ld is empty or outside the %ux%u surface",
                   (long)dr[0], (long)dr[1], (long)dr[2], (long)dr[3],
                   di.width, di.height);
        return SHIM_HR_INVALIDCALL;
    }
    int32_t sr[4] = {0, 0, (int32_t)si.width, (int32_t)si.height};
    int has_src = read_rect(src_rect_addr, sr);
    if (has_src < 0) {
        diagnostic("D3DXLoadSurfaceFromSurface: source rectangle is outside "
                   "guest memory");
        return SHIM_HR_INVALIDCALL;
    }
    if (!rect_inside(sr, si.width, si.height)) {
        diagnostic("D3DXLoadSurfaceFromSurface: source rectangle "
                   "%ld,%ld..%ld,%ld is empty or outside the %ux%u surface",
                   (long)sr[0], (long)sr[1], (long)sr[2], (long)sr[3],
                   si.width, si.height);
        return SHIM_HR_INVALIDCALL;
    }
    if (sr[2] - sr[0] != dr[2] - dr[0] ||
        sr[3] - sr[1] != dr[3] - dr[1]) {
        diagnostic("D3DXLoadSurfaceFromSurface: scaling a %ldx%ld source "
                   "rectangle into %ldx%ld is not implemented",
                   (long)(sr[2] - sr[0]), (long)(sr[3] - sr[1]),
                   (long)(dr[2] - dr[0]), (long)(dr[3] - dr[1]));
        return SHIM_HR_NOTAVAILABLE;
    }

    unsigned dpitch = 0, drows = 0, spitch = 0, srows = 0;
    uint32_t daddr = 0, saddr = 0;
    int rc = surface_lock(dest, dr, 0, &dpitch, &daddr, &drows);
    if (rc) return lock_failure(rc, "D3DXLoadSurfaceFromSurface destination");
    rc = surface_lock(src, sr, 0x10u, &spitch, &saddr, &srows);
    if (rc) {
        (void)d9_com_surface_unlock(dest);
        return lock_failure(rc, "D3DXLoadSurfaceFromSurface source");
    }

    size_t row_bytes = (size_t)(dr[2] - dr[0]) * 4u;
    size_t dst_span = (size_t)(drows - 1u) * dpitch + row_bytes;
    size_t src_span = (size_t)(srows - 1u) * spitch + row_bytes;
    uint8_t *dst = guest_ptr(memory, daddr, dst_span);
    const uint8_t *source = guest_ptr(memory, saddr, src_span);
    if (!dst || !source) {
        (void)d9_com_surface_unlock(src);
        (void)d9_com_surface_unlock(dest);
        diagnostic("D3DXLoadSurfaceFromSurface: locked source or destination "
                   "span is no longer mapped");
        return SHIM_HR_INVALIDCALL;
    }
    for (unsigned y = 0; y < drows; ++y)
        memcpy(dst + (size_t)y * dpitch, source + (size_t)y * spitch, row_bytes);

    int urc = d9_com_surface_unlock(src);
    int udrc = d9_com_surface_unlock(dest);
    if (urc) {
        diagnostic("D3DXLoadSurfaceFromSurface: source UnlockRect failed (%d)", urc);
        return unlock_failure(urc, "D3DXLoadSurfaceFromSurface source");
    }
    if (udrc) {
        diagnostic("D3DXLoadSurfaceFromSurface: destination UnlockRect failed (%d)",
                   udrc);
        return unlock_failure(udrc, "D3DXLoadSurfaceFromSurface destination");
    }
    return 0;
}

static void zero_output(uint32_t address) {
    if (!address || !memory) return;
    uint32_t *p = guest_ptr(memory, address, 4u);
    if (p) *p = 0;
}

static uint32_t compile_shader(CPU *c) {
    uint32_t pp_shader = arg(c, 7), pp_errors = arg(c, 8);
    uint32_t pp_constants = arg(c, 9), profile = arg(c, 5);

    last_error[0] = 0;
    /* No compile is attempted, so no guest bytecode pointer is dereferenced.
     * Outputs are cleared so a caller cannot mistake the failure for an empty
     * successful shader. */
    zero_output(pp_shader);
    zero_output(pp_errors);
    zero_output(pp_constants);

    const char *name = (memory && profile) ? guest_cstr(memory, profile, 32u) : NULL;
    diagnostic("D3DXCompileShader%s%s%s: Shader Model 3 (SM3) bytecode "
               "compilation is an architectural blocker; E_NOTIMPL, no shader "
               "or error buffer was produced",
               name ? "(" : "", name ? name : "", name ? ")" : "");
    return SHIM_E_NOTIMPL;
}

static void shim_load_memory(CPU *c) { ret(c, load_from_memory(c), 10u); }
static void shim_load_surface(CPU *c) { ret(c, load_from_surface(c), 8u); }
static void shim_compile_shader(CPU *c) { ret(c, compile_shader(c), 10u); }

static const FnEntry entries[D3DX_SURFACE_I_COUNT] = {
    {D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_LOAD_MEMORY, shim_load_memory},
    {D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_LOAD_SURFACE, shim_load_surface},
    {D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_COMPILE_SHADER, shim_compile_shader},
};

FnModule d3dx_surface_module(void) {
    return (FnModule){"d3dx-surface", entries, D3DX_SURFACE_I_COUNT};
}

static int dll_is(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

uint32_t d3dx_surface_resolve(const PeImport *entry, void *ctx) {
    (void)ctx;
    if (!entry || !entry->dll || !entry->name) return 0;
    if (!dll_is(entry->dll, "d3dx9_42.dll") && !dll_is(entry->dll, "d3dx9_42"))
        return 0;
    if (!strcmp(entry->name, "D3DXLoadSurfaceFromMemory"))
        return D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_LOAD_MEMORY;
    if (!strcmp(entry->name, "D3DXLoadSurfaceFromSurface"))
        return D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_LOAD_SURFACE;
    if (!strcmp(entry->name, "D3DXCompileShader"))
        return D3DX_SURFACE_THUNK_BASE + 4u * D3DX_SURFACE_I_COMPILE_SHADER;
    return 0;
}

int d3dx_surface_init(GuestMemory *m) {
    if (!m) return -1;
    if (win32_add_external_resolver(d3dx_surface_resolve, NULL)) return -1;
    memory = m;
    last_error[0] = 0;
    return 0;
}

void d3dx_surface_shutdown(void) {
    memory = NULL;
    last_error[0] = 0;
}

const char *d3dx_surface_last_error(void) { return last_error; }

