/* Diagnostic overlay for tools/alloc_probe_nod3d.ps1.
 * Production source/win32_shell.c has an ABI bug: ret() removes only the
 * arguments, not the return address. This copy applies the exact one-line fix
 * so the allocator/file-paging probe can continue. The shell owner must apply
 * the same change to source/win32_shell.c. */
#include "win32_shell.h"
#include <string.h>

#define SHELL_MAX_PATH 260

static GuestMemory *shell_mem;
static char shell_root[512];

void win32_shell_init(GuestMemory *m) { shell_mem = m; }
void win32_shell_set_root(const char *game_root) {
    if (!game_root) { shell_root[0] = 0; return; }
    size_t n = strlen(game_root);
    if (n >= sizeof shell_root) n = sizeof shell_root - 1;
    memcpy(shell_root, game_root, n);
    shell_root[n] = 0;
}
void win32_shell_shutdown(void) { shell_mem = NULL; shell_root[0] = 0; }

static uint32_t rd32(uint32_t a) {
    const uint8_t *p = guest_ptr(shell_mem, a, 4);
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint32_t a, uint32_t v) {
    uint8_t *p = guest_ptr(shell_mem, a, 4);
    if (!p) return;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t A(CPU *c, unsigned n) { return rd32(c->r[4] + 4 + 4 * n); }
static void set_last_error(CPU *c, uint32_t err) { if (c->fs_base) wr32(c->fs_base + 0x34, err); }
static void ret(CPU *c, uint32_t v, unsigned args) { c->r[0] = v; c->r[4] += 4 + args * 4; }
static int same(const char *a, const char *b) {
    while (*a && *b) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

/* Known CSIDL values map to a sandbox-relative directory. Everything else is
 * E_FAIL: no other profile location is invented. A configured root is required,
 * otherwise the answer would be meaningless. */
static const char *csidl_path(uint32_t csidl) {
    switch (csidl) {
    case 0x1a: return "AppData";         /* CSIDL_APPDATA */
    case 0x1c: return "LocalAppData";    /* CSIDL_LOCAL_APPDATA */
    case 0x05: return "Documents";       /* CSIDL_PERSONAL */
    case 0x23: return "ProgramData";     /* CSIDL_COMMON_APPDATA */
    default: return NULL;
    }
}
static void s_SHGetFolderPathA(CPU *c) {
    uint32_t path = A(c, 4);
    const char *rel = shell_root[0] ? csidl_path(A(c, 1)) : NULL;
    set_last_error(c, 0);
    if (!rel) {
        if (path) { uint8_t *p = guest_ptr(shell_mem, path, SHELL_MAX_PATH); if (p) p[0] = 0; }
        ret(c, 0x80004005u, 5);
        return;
    }
    uint8_t *p = path ? guest_ptr(shell_mem, path, SHELL_MAX_PATH) : NULL;
    if (!p) { ret(c, 0x80004005u, 5); return; }
    size_t n = strlen(rel);
    if (n + 1 > SHELL_MAX_PATH) { p[0] = 0; ret(c, 0x8007007Au, 5); return; } /* E_INVALIDARG */
    memcpy(p, rel, n + 1);
    ret(c, 0, 5);
}
static void s_SHGetFolderPathW(CPU *c) {
    uint32_t path = A(c, 4);
    const char *rel = shell_root[0] ? csidl_path(A(c, 1)) : NULL;
    set_last_error(c, 0);
    if (!rel) {
        if (path) { uint8_t *p = guest_ptr(shell_mem, path, SHELL_MAX_PATH * 2); if (p) { p[0] = 0; p[1] = 0; } }
        ret(c, 0x80004005u, 5);
        return;
    }
    uint8_t *p = path ? guest_ptr(shell_mem, path, SHELL_MAX_PATH * 2) : NULL;
    if (!p) { ret(c, 0x80004005u, 5); return; }
    size_t n = strlen(rel);
    for (size_t i = 0; i <= n; ++i) { p[i * 2] = (uint8_t)rel[i]; p[i * 2 + 1] = 0; }
    ret(c, 0, 5);
}
static void s_SHEmptyRecycleBinA(CPU *c) { ret(c, 0x80004005u, 3); }
static void s_ShellExecuteA(CPU *c) { ret(c, 31u, 6); /* SE_ERR_NOASSOC */ }

static const Win32ShellDef defs[] = {
    {"shell32.dll", "SHGetFolderPathA", s_SHGetFolderPathA},
    {"shell32.dll", "SHGetFolderPathW", s_SHGetFolderPathW},
    {"shell32.dll", "SHEmptyRecycleBinA", s_SHEmptyRecycleBinA},
    {"shell32.dll", "ShellExecuteA", s_ShellExecuteA},
};
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32ShellDef *win32_shell_defs(unsigned *count) {
    if (count) *count = NDEFS;
    return defs;
}

uint32_t win32_shell_resolve(const char *dll, const char *name) {
    if (!dll || !name || (!same(dll, "shell32.dll") && !same(dll, "shell32"))) return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (!strcmp(name, defs[i].name)) return WIN32_SHELL_BASE + 4 * i;
    return 0;
}

const FnEntry *win32_shell_lookup(uint32_t addr) {
    if (addr < WIN32_SHELL_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_SHELL_BASE) / 4;
    if (i >= NDEFS) return NULL;
    static FnEntry e[NDEFS];
    e[i] = (FnEntry){addr, defs[i].fn};
    return &e[i];
}

