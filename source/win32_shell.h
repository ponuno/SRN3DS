#pragma once
#include "win32_shims.h"

/* shell32 surface used by the guest.
 *
 * SHGetFolderPathA/W do not invent a Windows profile. They answer with a
 * *relative* sandbox path ("LocalAppData", "Documents", "AppData" or
 * "ProgramData") rooted at the configured game directory, because the file
 * shims only accept paths inside that directory. Callers that need an
 * absolute path must resolve it through the regular file APIs. Unknown CSIDL
 * values return E_FAIL. */

void win32_shell_init(GuestMemory *m);
void win32_shell_set_root(const char *game_root);
void win32_shell_shutdown(void);

uint32_t win32_shell_resolve(const char *dll, const char *name);
const FnEntry *win32_shell_lookup(uint32_t addr);

typedef struct { const char *dll, *name; GuestFn fn; } Win32ShellDef;
const Win32ShellDef *win32_shell_defs(unsigned *count);

#define WIN32_SHELL_BASE UINT32_C(0xff040000)
