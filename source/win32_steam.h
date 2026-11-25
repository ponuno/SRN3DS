#pragma once
#include <stdint.h>
#include <stdio.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE only; no shim internals */

/* Offline SteamAPI compatibility for the guest.
 *
 * There is no Steam client, service, IPC, or network implementation here.
 * SteamAPI_Init() always fails, RunCallbacks() never dispatches an event, and
 * every known service/interface accessor returns zero/NULL with a diagnostic.
 * The callback registry only preserves the guest-visible CCallbackBase writes
 * made by RegisterCallback/UnregisterCallback; it is not an event source.
 *
 * Thunks are WIN32_STEAM_BASE + 4*i. The window is disjoint from the core
 * window and the sync/user/locale/shell/audio/input/thread windows. A dispatcher
 * must chain win32_steam_lookup(), and import resolution must chain
 * win32_steam_resolve() before falling back to loaded PE exports. */
#define WIN32_STEAM_BASE (WIN32_THUNK_BASE + 0x00080000u) /* 0xff080000 */
#define WIN32_STEAM_CALLBACK_CAPACITY 32u

typedef struct { const char *dll, *name; GuestFn fn; } Win32SteamDef;

/* Binds guest memory for callback-object validation. Creates no guest mapping
 * and starts no service. Returns -1 for NULL or an already-bound module. */
int win32_steam_init(GuestMemory *m);
/* Drops the memory binding and clears the entire host callback registry. */
void win32_steam_shutdown(void);

const Win32SteamDef *win32_steam_defs(unsigned *count);
/* DLL names are case-insensitive; export names match exactly. Unknown names
 * return 0 so the normal unresolved-import policy still applies. */
uint32_t win32_steam_resolve(const char *dll, const char *name);
const FnEntry *win32_steam_lookup(uint32_t addr);

/* Diagnostics/test hook. NULL restores stderr. This is host-side only and is
 * never exposed as a guest import. A configured stream survives
 * win32_steam_init(); win32_steam_shutdown() drops it along with the memory
 * binding, because the owner normally closes the stream next. */
void win32_steam_set_log_stream(FILE *stream);
/* Number of live entries in the bounded registry (diagnostics/tests only). */
unsigned win32_steam_callback_count(void);
