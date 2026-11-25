#pragma once
#include <stdint.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* only for WIN32_THUNK_BASE; no shim internals */

/* Minimal user32/GDI compatibility for a single-threaded static recompilation.
 * No host window is created and no message is ever fabricated: there is no
 * input or window manager behind these thunks. Unsupported cases fail with a
 * Win32 error instead of reporting success.
 *
 * Documented limitations:
 *  - Classes and windows live in fixed 16/32-entry static tables; handles are
 *    slot indices (ATOM 0xC000+i, HWND 0x00010000+i), not host objects.
 *  - CreateWindowExA ignores style/menu/instance/param. CW_USEDEFAULT maps to
 *    x=y=0 and to 400x240 for the size; negative sizes are stored as given.
 *  - GetWindowRect and GetClientRect are both origin-relative and identical:
 *    there is no desktop or non-client area. SetWindowPos stores x/y but no
 *    API exposes them; only w/h are observable through the rect calls.
 *  - GetSystemMetrics models only 0/1/76/77 (400/240) and 80 (1 monitor).
 *    Every other index returns 0.
 *  - Titles are capped at 255 characters plus NUL.
 *  - MessageBoxA prints "[messagebox] <text>" to stderr and returns IDOK; it
 *    never blocks. Caption and type are ignored.
 *  - PeekMessageA always returns 0. WM_QUIT is never synthesized, so callers
 *    must not rely on a message loop to terminate.
 *  - GetKeyState/GetAsyncKeyState always return 0; there is no input yet.
 *  - FindWindowA always returns NULL. LoadIconA/LoadCursorA return fixed fake
 *    handles (0x00020001/0x00020002) and ignore the resource name.
 *    GetStockObject accepts indices 0..19 and returns 0x00040000+index;
 *    GetDC returns 0x00030001. These are opaque pseudo-handles; nothing is
 *    actually drawn.
 *  - SetWindowLongA stores only GWL_USERDATA (-21); other indices fail with
 *    ERROR_INVALID_PARAMETER. GetWindowLongA/GetClassLongA report 0 for them.
 *  - Last-error is written to the guest TEB at fs_base+0x34 only when fs_base
 *    is nonzero; there is no host-side per-thread fallback.
 *
 * The future 3DS renderer targets 400x240, so screen and virtual-screen
 * metrics report that size. The thunk range is private and disjoint from
 * win32_shims (WIN32_THUNK_BASE + 4*i, small i). Callers that look up thunk
 * addresses must chain win32_user_lookup() after win32_lookup(). */
#define WIN32_USER_BASE (WIN32_THUNK_BASE + 0x00020000u)

typedef struct { const char *dll,*name; GuestFn fn; } Win32UserDef;

/* Binds guest memory. Creates no window, class, DC or host resource. The
 * module owns no guest mappings, so shutdown only drops the binding. */
int win32_user_init(GuestMemory *m);
void win32_user_shutdown(void);
const Win32UserDef *win32_user_defs(unsigned *count);
/* Thunk address for dll!name, or 0. DLL names are case-insensitive; the
 * function name must match exactly, as in win32_resolve(). */
uint32_t win32_user_resolve(const char *dll,const char *name);
/* FnEntry for one of our thunk addresses, or NULL. */
const FnEntry *win32_user_lookup(uint32_t addr);
