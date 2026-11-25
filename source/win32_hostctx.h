/* Host stacks and context switching for cooperative guest threads.
 *
 * A guest thread's *continuation* is a host C call stack: the lifted AOT code
 * keeps guest registers in locals and arithmetic flags in a local `Flags`, and
 * the interpreter does the same. Only the host stack can preserve that. A CPU
 * struct alone cannot (see THREAD_MODEL.md, "What must be saved").
 *
 * Backends:
 *   1  Windows fibers (ConvertThreadToFiber/SwitchToFiber)
 *   2  manual ARMv6K switch (AAPCS hard-float)
 *   3  manual x86-64 switch (Win64 ABI)
 *
 * Only backends 1 and 3 are exercised by the host tests. 2 is compile-guarded
 * and has not run on hardware; THREAD_MODEL.md says so explicitly.
 */
#pragma once
#include <stddef.h>

#if !defined(WIN32_HOSTCTX_BACKEND)
#  if defined(_WIN32)
#    define WIN32_HOSTCTX_BACKEND 1
#  elif defined(__arm__)
#    define WIN32_HOSTCTX_BACKEND 2
#  elif defined(__x86_64__)
#    define WIN32_HOSTCTX_BACKEND 3
#  else
#    define WIN32_HOSTCTX_BACKEND 0
#  endif
#endif

typedef struct Win32HostCtx Win32HostCtx;

/* Make the calling host thread switchable. Idempotent. Returns 0 on success. */
int win32_hostctx_init(void);
/* Allocate a context for the already-running host thread (the main guest
 * thread); it owns no stack. */
int win32_hostctx_main(Win32HostCtx **out);
/* Allocate a context (and its host stack) that first enters fn(arg). fn must
 * never return; it must switch away permanently. */
int win32_hostctx_create(Win32HostCtx **out, void (*fn)(void *), void *arg,
                         size_t stack_bytes);
/* Switch from the caller to `to`. `from` receives the caller's context. */
void win32_hostctx_switch(Win32HostCtx *from, Win32HostCtx *to);
/* Release a context whose stack is no longer executing. Must be called from a
 * different context (never the one being destroyed). */
void win32_hostctx_destroy(Win32HostCtx *ctx);
size_t win32_hostctx_default_stack(void);
const char *win32_hostctx_name(void);
