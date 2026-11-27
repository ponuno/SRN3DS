#pragma once
#include <stdint.h>
#include <stdio.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE only; no shim internals */

/* Bounded, in-memory ADVAPI32 registry compatibility.
 *
 * This is not a Windows registry and it is not a view of the host registry.
 * Exactly one install key is preloaded, because a real installation writes it:
 * HKLM\Software\Bethesda Softworks\Skyrim with sBasePath and Installed Path
 * (both REG_SZ = "C:\\Skyrim\\", the virtual root the guest also sees through
 * GetModuleFileNameA). Everything else exists only when the guest itself
 * creates or writes it. Missing keys and
 * values are reported as ERROR_FILE_NOT_FOUND, never fabricated, and every
 * distinct missing value is logged once so the next run says exactly what the
 * engine wanted.
 *
 * Thunks are WIN32_REGISTRY_BASE + 4*i. The window is disjoint from the core
 * window and the sync/user/locale/shell/audio/input/thread/steam windows.
 * A dispatcher must chain win32_registry_lookup(), and import resolution must
 * chain win32_registry_resolve() before falling back to loaded PE exports. */
#define WIN32_REGISTRY_BASE (WIN32_THUNK_BASE + 0x00090000u) /* 0xff090000 */

/* Fixed limits. No per-call host allocation; these arrays are static. */
#define WIN32_REGISTRY_MAX_KEYS 64u
#define WIN32_REGISTRY_MAX_HANDLES 16u
#define WIN32_REGISTRY_MAX_PATH 192u       /* relative key path, bytes */
#define WIN32_REGISTRY_MAX_VALUES 8u       /* values per key */
#define WIN32_REGISTRY_MAX_VALUE_NAME 64u
#define WIN32_REGISTRY_MAX_VALUE_BYTES 512u

/* Predefined root handles accepted by RegOpenKeyExA/RegCreateKeyExA. */
#define WIN32_REGISTRY_HKCR UINT32_C(0x80000000)
#define WIN32_REGISTRY_HKCU UINT32_C(0x80000001)
#define WIN32_REGISTRY_HKLM UINT32_C(0x80000002)
#define WIN32_REGISTRY_HKU  UINT32_C(0x80000003)
#define WIN32_REGISTRY_HKCC UINT32_C(0x80000005)

/* Dynamic handles are WIN32_REGISTRY_HANDLE_BASE + 4*(slot+1). */
#define WIN32_REGISTRY_HANDLE_BASE UINT32_C(0x52470000)

typedef struct { const char *dll, *name; GuestFn fn; } Win32RegistryDef;

/* Binds guest memory and clears the entire in-memory tree and handle table.
 * Creates no guest mapping and touches no host registry. Returns -1 for NULL
 * or an already-bound module. */
int win32_registry_init(GuestMemory *m);
/* Drops the memory binding together with every key, value and handle. */
void win32_registry_shutdown(void);

const Win32RegistryDef *win32_registry_defs(unsigned *count);
/* DLL names are case-insensitive; export names match exactly. Unknown names
 * return 0 so the normal unresolved-import policy still applies. */
uint32_t win32_registry_resolve(const char *dll, const char *name);
const FnEntry *win32_registry_lookup(uint32_t addr);

/* Diagnostics/test hook. NULL restores stderr. Host-side only; never a guest
 * import. A configured stream survives win32_registry_init(); shutdown drops
 * it along with the memory binding. */
void win32_registry_set_log_stream(FILE *stream);

/* Diagnostics for the host smoke test (not guest imports). */
unsigned win32_registry_key_count(void);
unsigned win32_registry_handle_count(void);
unsigned win32_registry_unknown_value_count(void);
