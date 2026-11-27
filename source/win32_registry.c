/* Standalone, bounded ADVAPI32 registry compatibility.
 *
 * The guest sees a small fixed in-memory tree. There is no host registry
 * access, no filesystem persistence, and no invented value: a key exists only
 * after RegCreateKeyExA, and a value exists only after RegSetValueExA.
 * Unknown keys and values return ERROR_FILE_NOT_FOUND and are logged (bounded,
 * once per distinct name) so the next run names what TESV actually asked for.
 *
 * All five entry points are stdcall: the callee removes the 4-byte return
 * address plus exactly the documented argument count. Stack validation is
 * performed before any guest memory is touched. */
#include "win32_registry.h"
#include <string.h>

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_SYSTEM_RESOURCES 1450u

#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u

#define REG_OPTION_VOLATILE 1u
#define REG_CREATED_NEW_KEY 1u
#define REG_OPENED_EXISTING_KEY 2u

#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE 0x0002u
#define KEY_CREATE_SUB_KEY 0x0004u
#define KEY_ENUMERATE_SUB_KEYS 0x0008u
#define KEY_NOTIFY 0x0010u
#define KEY_CREATE_LINK 0x0020u
#define KEY_WOW64_64KEY 0x0100u
#define KEY_WOW64_32KEY 0x0200u
#define DELETE 0x00010000u
#define READ_CONTROL 0x00020000u
#define WRITE_DAC 0x00040000u
#define WRITE_OWNER 0x00080000u
#define SYNCHRONIZE 0x00100000u
#define ACCESS_SYSTEM_SECURITY 0x01000000u
#define MAXIMUM_ALLOWED 0x02000000u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define KEY_READ 0x00020019u
#define KEY_WRITE 0x00020006u
#define KEY_ALL_ACCESS 0x000f003fu

#define ACCESS_SUPPORTED (KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | \
                          KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | KEY_CREATE_LINK | \
                          KEY_WOW64_64KEY | KEY_WOW64_32KEY | DELETE | \
                          READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE | \
                          ACCESS_SYSTEM_SECURITY | MAXIMUM_ALLOWED | \
                          GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | \
                          GENERIC_ALL | KEY_ALL_ACCESS)

/* KEY_ALL_ACCESS and KEY_READ/KEY_WRITE are composites; only the specific
 * right (or a generic right) actually grants an operation. */
#define ACCESS_READ (KEY_QUERY_VALUE | GENERIC_READ | GENERIC_ALL | MAXIMUM_ALLOWED)
#define ACCESS_WRITE (KEY_SET_VALUE | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED)

#define UNKNOWN_KEY_LOG 16u
#define UNKNOWN_VALUE_LOG 32u

typedef struct {
    uint8_t used;
    uint8_t type;
    uint16_t size;
    char name[WIN32_REGISTRY_MAX_VALUE_NAME];
    uint8_t data[WIN32_REGISTRY_MAX_VALUE_BYTES];
} RegValue;

typedef struct {
    uint8_t used;
    uint8_t root;
    uint8_t value_count;
    char path[WIN32_REGISTRY_MAX_PATH];
    RegValue values[WIN32_REGISTRY_MAX_VALUES];
} RegKey;

typedef struct {
    uint8_t used;
    uint8_t key;
    uint32_t access;
} RegHandle;

typedef struct {
    uint8_t used;
    uint8_t root;
    char path[WIN32_REGISTRY_MAX_PATH];
    char name[WIN32_REGISTRY_MAX_VALUE_NAME];
} UnknownName;

static struct {
    GuestMemory *mem;
    RegKey keys[WIN32_REGISTRY_MAX_KEYS];
    unsigned key_count;
    RegHandle handles[WIN32_REGISTRY_MAX_HANDLES];
    unsigned handle_count;
    UnknownName unknown_keys[UNKNOWN_KEY_LOG];
    unsigned unknown_key_count;
    int unknown_key_log_full;
    UnknownName unknown_values[UNKNOWN_VALUE_LOG];
    unsigned unknown_value_count;
    int unknown_value_log_full;
    FILE *log;
} g;

static FILE *diagnostics(void) { return g.log ? g.log : stderr; }

void win32_registry_set_log_stream(FILE *stream) { g.log = stream; }

unsigned win32_registry_key_count(void) { return g.key_count; }
unsigned win32_registry_handle_count(void) { return g.handle_count; }
unsigned win32_registry_unknown_value_count(void) { return g.unknown_value_count; }

/* Binding preserves host configuration (the diagnostics sink) exactly like the
 * Steam module: a stream installed before win32_init() must keep working. */
static int create_tree(uint8_t root, const char *path);
static void seed_install_key(void);

int win32_registry_init(GuestMemory *m) {
    FILE *log = g.log;
    if (!m || g.mem) return -1;
    memset(&g, 0, sizeof g);
    g.log = log;
    g.mem = m;
    /* Reproduce an installed system: the game we run is installed in the
     * configured virtual root, and its installer writes this key/value. TESV
     * asks for it during WinMain (observed: value "sBasePath"). This is the
     * only preloaded state; everything else exists only if the guest writes it. */
    if (create_tree(2, "Software\\Bethesda Softworks\\Skyrim") == 0)
        seed_install_key();
    return 0;
}

void win32_registry_shutdown(void) { memset(&g, 0, sizeof g); }

static unsigned lower_ascii(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + 32u : c;
}

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void copy_text(char *dst, size_t capacity, const char *src) {
    size_t n = strlen(src);
    if (n >= capacity) n = capacity - 1u;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *root_name(unsigned root) {
    switch (root) {
    case 0: return "HKCR";
    case 1: return "HKCU";
    case 2: return "HKLM";
    case 3: return "HKU";
    case 4: return "HKCC";
    default: return "HKEY?";
    }
}

static int root_from_predefined(uint32_t hkey, uint8_t *root) {
    switch (hkey) {
    case WIN32_REGISTRY_HKCR: *root = 0; return 0;
    case WIN32_REGISTRY_HKCU: *root = 1; return 0;
    case WIN32_REGISTRY_HKLM: *root = 2; return 0;
    case WIN32_REGISTRY_HKU:  *root = 3; return 0;
    case WIN32_REGISTRY_HKCC: *root = 4; return 0;
    default: return -1;
    }
}

static int handle_slot(uint32_t hkey) {
    if (hkey < WIN32_REGISTRY_HANDLE_BASE + 4u ||
        (hkey & 3u)) return -1;
    uint32_t index = (hkey - WIN32_REGISTRY_HANDLE_BASE) / 4u - 1u;
    if (index >= WIN32_REGISTRY_MAX_HANDLES || !g.handles[index].used)
        return -1;
    return (int)index;
}

static uint32_t handle_value(unsigned slot) {
    return WIN32_REGISTRY_HANDLE_BASE + 4u * (slot + 1u);
}

/* Resolve any accepted HKEY into a root id plus a relative key path. Dynamic
 * handles carry their own root and path. */
static int resolve_key(uint32_t hkey, uint8_t *root, char *path) {
    if (root_from_predefined(hkey, root) == 0) {
        path[0] = 0;
        return 0;
    }
    int slot = handle_slot(hkey);
    if (slot < 0) return -1;
    RegKey *key = &g.keys[g.handles[slot].key];
    if (!key->used) return -1;
    *root = key->root;
    memcpy(path, key->path, sizeof key->path);
    return 0;
}

static int arg32(CPU *c, unsigned index, uint32_t *value) {
    uint64_t address = (uint64_t)c->r[4] + 4u + (uint64_t)index * 4u;
    if (!g.mem || !value || address > UINT32_MAX) return -1;
    return guest_read32(g.mem, (uint32_t)address, value);
}

static void set_error(CPU *c, uint32_t error) {
    if (c->fs_base) guest_write32(g.mem, c->fs_base + 0x34u, error);
}

/* stdcall epilogue: return address + exact argument count. This is explicit
 * because a mistaken slot count shifts the caller's ESP for every later call. */
static void stdcall_return(CPU *c, uint32_t value, unsigned args) {
    c->r[0] = value;
    c->r[4] += 4u + 4u * args;
}

/* Copy a NUL-terminated guest string into a bounded host buffer. Returns 0 on
 * success, -1 for an invalid pointer or a string that does not fit. */
static int guest_name(uint32_t address, char *out, size_t capacity) {
    if (!g.mem || !out || !capacity) return -1;
    const char *p = guest_cstr(g.mem, address, capacity);
    if (!p) return -1;
    size_t n = strlen(p);
    if (n >= capacity) return -1;
    memcpy(out, p, n + 1u);
    return 0;
}

/* Validate a guest output pointer before any state change. */
static int guest_out32(uint32_t address) {
    return g.mem && guest_ptr(g.mem, address, 4u) ? 0 : -1;
}

static void log_missing_key(uint8_t root, const char *path) {
    for (unsigned i = 0; i < g.unknown_key_count; ++i) {
        UnknownName *u = &g.unknown_keys[i];
        if (u->root == root && name_eq(u->path, path)) return;
    }
    if (g.unknown_key_count >= UNKNOWN_KEY_LOG) {
        if (!g.unknown_key_log_full) {
            g.unknown_key_log_full = 1;
            fprintf(diagnostics(), "[registry] missing-key log full (%u); "
                    "further distinct missing keys are not named\n",
                    (unsigned)UNKNOWN_KEY_LOG);
        }
        return;
    }
    UnknownName *u = &g.unknown_keys[g.unknown_key_count++];
    u->used = 1;
    u->root = root;
    copy_text(u->path, sizeof u->path, path);
    fprintf(diagnostics(), "[registry] open miss: %s\\%s (ERROR_FILE_NOT_FOUND)\n",
            root_name(root), path);
}

static void log_missing_value(uint8_t root, const char *path, const char *name) {
    for (unsigned i = 0; i < g.unknown_value_count; ++i) {
        UnknownName *u = &g.unknown_values[i];
        if (u->root == root && name_eq(u->path, path) &&
            name_eq(u->name, name)) return;
    }
    if (g.unknown_value_count >= UNKNOWN_VALUE_LOG) {
        if (!g.unknown_value_log_full) {
            g.unknown_value_log_full = 1;
            fprintf(diagnostics(), "[registry] missing-value log full (%u); "
                    "further distinct missing values are not named\n",
                    (unsigned)UNKNOWN_VALUE_LOG);
        }
        return;
    }
    UnknownName *u = &g.unknown_values[g.unknown_value_count++];
    u->used = 1;
    u->root = root;
    copy_text(u->path, sizeof u->path, path);
    copy_text(u->name, sizeof u->name, name);
    fprintf(diagnostics(), "[registry] query miss: %s\\%s value \"%s\" "
            "(ERROR_FILE_NOT_FOUND)\n", root_name(root), path, name);
}

static int key_find(uint8_t root, const char *path) {
    for (unsigned i = 0; i < WIN32_REGISTRY_MAX_KEYS; ++i)
        if (g.keys[i].used && g.keys[i].root == root &&
            name_eq(g.keys[i].path, path))
            return (int)i;
    return -1;
}

static int key_alloc(uint8_t root, const char *path) {
    for (unsigned i = 0; i < WIN32_REGISTRY_MAX_KEYS; ++i) {
        if (!g.keys[i].used) {
            RegKey *key = &g.keys[i];
            memset(key, 0, sizeof *key);
            key->used = 1;
            key->root = root;
            copy_text(key->path, sizeof key->path, path);
            ++g.key_count;
            return (int)i;
        }
    }
    return -1;
}

static int append_component(char *dst, size_t capacity, const char *base,
                            const char *component, size_t component_len) {
    size_t used = strlen(base);
    size_t add = component_len;
    size_t need = used + (used ? 1u : 0u) + add;
    if (need >= capacity) return -1;
    if (used) dst[used++] = '\\';
    memcpy(dst + used, component, add);
    dst[used + add] = 0;
    return 0;
}

/* Strict subkey syntax: no leading/trailing separator and no empty component.
 * A single level is accepted; deeper paths are created level by level. */
static int valid_subkey(const char *subkey, uint16_t *length) {
    if (!subkey || !*subkey) return -1;
    if (subkey[0] == '\\') return -1;
    size_t n = strlen(subkey);
    if (!n || n >= WIN32_REGISTRY_MAX_PATH || subkey[n - 1u] == '\\') return -1;
    for (size_t i = 0; i < n; ++i)
        if (subkey[i] == '\\' && subkey[i + 1u] == '\\') return -1;
    *length = (uint16_t)n;
    return 0;
}

/* Count the prefixes in `path` that are absent, and refuse before creating
 * anything if the bounded key table cannot hold all of them. */
static int missing_components(uint8_t root, const char *path, unsigned *missing) {
    unsigned absent = 0;
    char prefix[WIN32_REGISTRY_MAX_PATH];
    size_t n = strlen(path);
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || path[i] == '\\') {
            size_t end = i;
            if (end >= sizeof prefix) return -1;
            memcpy(prefix, path, end);
            prefix[end] = 0;
            if (end && key_find(root, prefix) < 0) ++absent;
        }
    }
    if (absent > WIN32_REGISTRY_MAX_KEYS - g.key_count) return -1;
    *missing = absent;
    return 0;
}

static int create_tree(uint8_t root, const char *path) {
    char prefix[WIN32_REGISTRY_MAX_PATH];
    size_t n = strlen(path);
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || path[i] == '\\') {
            size_t end = i;
            if (end >= sizeof prefix) return -1;
            memcpy(prefix, path, end);
            prefix[end] = 0;
            if (!end) continue;
            if (key_find(root, prefix) < 0 && key_alloc(root, prefix) < 0)
                return -1;
        }
    }
    return 0;
}

static int value_find(RegKey *key, const char *name) {
    for (unsigned i = 0; i < WIN32_REGISTRY_MAX_VALUES; ++i)
        if (key->values[i].used && name_eq(key->values[i].name, name))
            return (int)i;
    return -1;
}

/* The virtual install root the guest sees (GetModuleFileNameA reports
 * C:\Skyrim\<module>).  A real installer writes both the documented
 * "Installed Path" value (observed as REG_SZ in HKLM\Software\WOW6432Node\
 * Bethesda Softworks\Skyrim) and the engine's own "sBasePath".  Both are the
 * same install root here; the sandbox maps C:\Skyrim\ onto the real game
 * directory.  No language value is invented: on a real install the language
 * lives in SkyrimPrefs.ini (sLanguage), not in this key. */
static int seed_value(RegKey *key, const char *name, const char *data) {
    int vi = value_find(key, name);
    if (vi < 0) {
        if (key->value_count >= WIN32_REGISTRY_MAX_VALUES) return -1;
        for (unsigned i = 0; i < WIN32_REGISTRY_MAX_VALUES; ++i)
            if (!key->values[i].used) { vi = (int)i; break; }
        if (vi < 0) return -1;
        ++key->value_count;
        key->values[vi].used = 1;
        copy_text(key->values[vi].name, sizeof key->values[vi].name, name);
    }
    size_t n = strlen(data) + 1;
    if (n > WIN32_REGISTRY_MAX_VALUE_BYTES) return -1;
    memcpy(key->values[vi].data, data, n);
    key->values[vi].type = REG_SZ;
    key->values[vi].size = (uint16_t)n;
    return 0;
}

static void seed_install_key(void) {
    int ki = key_find(2, "Software\\Bethesda Softworks\\Skyrim");
    if (ki < 0) return;
    RegKey *key = &g.keys[ki];
    const char *data = "C:\\Skyrim\\";
    if (seed_value(key, "sBasePath", data) ||
        seed_value(key, "Installed Path", data)) {
        fprintf(diagnostics(), "[registry] install state refused: value table "
                "full\n");
        return;
    }
    fprintf(diagnostics(), "[registry] install state: HKLM\\Software\\Bethesda "
            "Softworks\\Skyrim sBasePath=%s Installed Path=%s\n", data, data);
}

static int access_bad(uint32_t access) {
    return access & ~(uint32_t)ACCESS_SUPPORTED;
}

static int access_can_read(uint32_t access) {
    return (access & ACCESS_READ) != 0;
}

static int access_can_write(uint32_t access) {
    return (access & ACCESS_WRITE) != 0;
}

static int alloc_handle(unsigned key, uint32_t access, uint32_t *out) {
    for (unsigned i = 0; i < WIN32_REGISTRY_MAX_HANDLES; ++i) {
        if (!g.handles[i].used) {
            g.handles[i].used = 1;
            g.handles[i].key = (uint8_t)key;
            g.handles[i].access = access;
            ++g.handle_count;
            *out = handle_value(i);
            return 0;
        }
    }
    return -1;
}

static uint32_t open_common(CPU *c, uint32_t hkey, uint32_t subkey_addr,
                            uint32_t options, uint32_t access,
                            uint32_t out_addr, uint32_t disposition_addr,
                            int create) {
    (void)c; /* Stack arguments have already been decoded by the caller. */
    if (!g.mem) {
        fprintf(diagnostics(), "[registry] %s refused: module is not bound to "
                "guest memory\n", create ? "RegCreateKeyExA" : "RegOpenKeyExA");
        return ERROR_INVALID_HANDLE;
    }
    if (access_bad(access)) {
        fprintf(diagnostics(), "[registry] %s refused access %08lx: unsupported "
                "rights\n", create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                (unsigned long)access);
        return ERROR_INVALID_PARAMETER;
    }
    if (options != 0u && !(create && options == REG_OPTION_VOLATILE)) {
        fprintf(diagnostics(), "[registry] %s refused options %08lx\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                (unsigned long)options);
        return ERROR_INVALID_PARAMETER;
    }
    uint8_t root;
    char base[WIN32_REGISTRY_MAX_PATH];
    if (resolve_key(hkey, &root, base)) {
        fprintf(diagnostics(), "[registry] %s refused hkey %08lx: invalid handle\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                (unsigned long)hkey);
        return ERROR_INVALID_HANDLE;
    }
    if (guest_out32(out_addr)) {
        fprintf(diagnostics(), "[registry] %s refused: output handle pointer "
                "%08lx is not writable guest memory\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                (unsigned long)out_addr);
        return ERROR_INVALID_PARAMETER;
    }
    char subkey[WIN32_REGISTRY_MAX_PATH];
    uint16_t subkey_len = 0;
    if (!subkey_addr || guest_name(subkey_addr, subkey, sizeof subkey) ||
        valid_subkey(subkey, &subkey_len)) {
        fprintf(diagnostics(), "[registry] %s refused: invalid subkey\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA");
        return ERROR_INVALID_PARAMETER;
    }
    char path[WIN32_REGISTRY_MAX_PATH];
    if (append_component(path, sizeof path, base, subkey, subkey_len)) {
        fprintf(diagnostics(), "[registry] %s refused: subkey too long for the "
                "bounded tree\n", create ? "RegCreateKeyExA" : "RegOpenKeyExA");
        return ERROR_INVALID_PARAMETER;
    }

    int index = key_find(root, path);
    uint32_t disposition = REG_OPENED_EXISTING_KEY;
    if (index < 0) {
        if (!create) {
            log_missing_key(root, path);
            return ERROR_FILE_NOT_FOUND;
        }
        unsigned missing = 0;
        if (missing_components(root, path, &missing) || create_tree(root, path)) {
            fprintf(diagnostics(), "[registry] %s refused: key table full (%u)\n",
                    create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                    (unsigned)WIN32_REGISTRY_MAX_KEYS);
            return ERROR_NO_SYSTEM_RESOURCES;
        }
        index = key_find(root, path);
        if (index < 0) {
            fprintf(diagnostics(), "[registry] RegCreateKeyExA internal: key "
                    "was not created\n");
            return ERROR_NO_SYSTEM_RESOURCES;
        }
        disposition = REG_CREATED_NEW_KEY;
    }
    if (create && guest_write32(g.mem, disposition_addr, disposition)) {
        /* Output slots were validated before the tree changed, so this is
         * internal; do not allocate a handle for a failed call. */
        fprintf(diagnostics(), "[registry] RegCreateKeyExA could not write "
                "disposition\n");
        return ERROR_INVALID_PARAMETER;
    }
    uint32_t handle;
    if (alloc_handle((unsigned)index, access, &handle)) {
        fprintf(diagnostics(), "[registry] %s refused: handle table full (%u)\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA",
                (unsigned)WIN32_REGISTRY_MAX_HANDLES);
        return ERROR_NO_SYSTEM_RESOURCES;
    }
    if (guest_write32(g.mem, out_addr, handle)) {
        fprintf(diagnostics(), "[registry] %s could not write handle\n",
                create ? "RegCreateKeyExA" : "RegOpenKeyExA");
        /* Release the handle we just reserved. */
        unsigned slot = (handle - WIN32_REGISTRY_HANDLE_BASE) / 4u - 1u;
        g.handles[slot].used = 0;
        g.handles[slot].key = 0;
        g.handles[slot].access = 0;
        --g.handle_count;
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

static void s_RegOpenKeyExA(CPU *c) {
    uint32_t hkey = 0, subkey = 0, options = 0, access = 0, out = 0;
    uint32_t error = ERROR_INVALID_PARAMETER;
    if (arg32(c, 0, &hkey) || arg32(c, 1, &subkey) || arg32(c, 2, &options) ||
        arg32(c, 3, &access) || arg32(c, 4, &out)) {
        fprintf(diagnostics(), "[registry] RegOpenKeyExA refused: invalid "
                "argument stack\n");
    } else {
        error = open_common(c, hkey, subkey, options, access, out, 0, 0);
    }
    set_error(c, error);
    stdcall_return(c, error, 5);
}

static void s_RegCreateKeyExA(CPU *c) {
    uint32_t hkey = 0, subkey = 0, reserved = 0, class_ptr = 0, options = 0;
    uint32_t access = 0, security = 0, out = 0, disposition = 0;
    uint32_t error = ERROR_INVALID_PARAMETER;
    if (arg32(c, 0, &hkey) || arg32(c, 1, &subkey) || arg32(c, 2, &reserved) ||
        arg32(c, 3, &class_ptr) || arg32(c, 4, &options) ||
        arg32(c, 5, &access) || arg32(c, 6, &security) ||
        arg32(c, 7, &out) || arg32(c, 8, &disposition)) {
        fprintf(diagnostics(), "[registry] RegCreateKeyExA refused: invalid "
                "argument stack\n");
        set_error(c, error);
        stdcall_return(c, error, 9);
        return;
    }
    (void)class_ptr;    /* lpClass is ignored, as on a volatile in-memory key. */
    (void)security;     /* No security descriptor is stored or enforced. */
    if (reserved != 0u) {
        fprintf(diagnostics(), "[registry] RegCreateKeyExA refused: Reserved=%lu\n",
                (unsigned long)reserved);
        set_error(c, error);
        stdcall_return(c, error, 9);
        return;
    }
    if (disposition && guest_out32(disposition)) {
        fprintf(diagnostics(), "[registry] RegCreateKeyExA refused: disposition "
                "pointer %08lx is not writable guest memory\n",
                (unsigned long)disposition);
        set_error(c, error);
        stdcall_return(c, error, 9);
        return;
    }
    error = open_common(c, hkey, subkey, options, access, out,
                        disposition, 1);
    set_error(c, error);
    stdcall_return(c, error, 9);
}

static void s_RegQueryValueExA(CPU *c) {
    uint32_t hkey = 0, name_ptr = 0, reserved_ptr = 0, type_ptr = 0;
    uint32_t data_ptr = 0, size_ptr = 0;
    uint32_t error = ERROR_INVALID_PARAMETER;
    if (arg32(c, 0, &hkey) || arg32(c, 1, &name_ptr) ||
        arg32(c, 2, &reserved_ptr) || arg32(c, 3, &type_ptr) ||
        arg32(c, 4, &data_ptr) || arg32(c, 5, &size_ptr)) {
        fprintf(diagnostics(), "[registry] RegQueryValueExA refused: invalid "
                "argument stack\n");
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    (void)reserved_ptr; /* lpReserved is reserved and ignored. */
    if (!g.mem) {
        fprintf(diagnostics(), "[registry] RegQueryValueExA refused: module is "
                "not bound to guest memory\n");
        error = ERROR_INVALID_HANDLE;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (!size_ptr || guest_out32(size_ptr) || (type_ptr && guest_out32(type_ptr))) {
        fprintf(diagnostics(), "[registry] RegQueryValueExA refused: size/type "
                "pointer is not writable guest memory\n");
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    uint8_t root;
    char base[WIN32_REGISTRY_MAX_PATH];
    if (resolve_key(hkey, &root, base)) {
        fprintf(diagnostics(), "[registry] RegQueryValueExA refused hkey %08lx: "
                "invalid handle\n", (unsigned long)hkey);
        error = ERROR_INVALID_HANDLE;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    char value_name[WIN32_REGISTRY_MAX_VALUE_NAME];
    if (name_ptr) {
        if (guest_name(name_ptr, value_name, sizeof value_name)) {
            fprintf(diagnostics(), "[registry] RegQueryValueExA refused: value "
                    "name is invalid or longer than %u bytes\n",
                    (unsigned)WIN32_REGISTRY_MAX_VALUE_NAME - 1u);
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
    } else {
        value_name[0] = 0;
    }
    /* Query on a predefined root has no stored values. */
    int key_index = base[0] ? key_find(root, base) : -1;
    if (key_index < 0) {
        /* A value query names both the key and the value, even when the key
         * itself is absent: that is what the next run needs to see. */
        log_missing_value(root, base, value_name);
        error = ERROR_FILE_NOT_FOUND;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    int slot = handle_slot(hkey);
    uint32_t access = slot >= 0 ? g.handles[slot].access : KEY_ALL_ACCESS;
    if (!access_can_read(access)) {
        fprintf(diagnostics(), "[registry] RegQueryValueExA refused: handle was "
                "opened without read access\n");
        error = ERROR_ACCESS_DENIED;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    RegKey *key = &g.keys[key_index];
    int vi = value_find(key, value_name);
    if (vi < 0) {
        log_missing_value(root, base, value_name);
        error = ERROR_FILE_NOT_FOUND;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    RegValue *value = &key->values[vi];
    uint32_t capacity = 0;
    if (guest_read32(g.mem, size_ptr, &capacity)) {
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (!data_ptr) {
        /* Size probe only: report the exact required size. */
        if (guest_write32(g.mem, size_ptr, value->size) ||
            (type_ptr && guest_write32(g.mem, type_ptr, value->type))) {
            error = ERROR_INVALID_PARAMETER;
        } else {
            error = ERROR_SUCCESS;
        }
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (capacity < value->size) {
        /* Windows reports the required size and leaves the buffer untouched. */
        if (guest_write32(g.mem, size_ptr, value->size) ||
            (type_ptr && guest_write32(g.mem, type_ptr, value->type)))
            error = ERROR_INVALID_PARAMETER;
        else
            error = ERROR_MORE_DATA;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (value->size) {
        void *dst = guest_ptr(g.mem, data_ptr, value->size);
        if (!dst) {
            fprintf(diagnostics(), "[registry] RegQueryValueExA refused: data "
                    "buffer %08lx is not writable guest memory (%u bytes)\n",
                    (unsigned long)data_ptr, (unsigned)value->size);
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
        if (guest_write(g.mem, data_ptr, value->data, value->size)) {
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
    }
    if (guest_write32(g.mem, size_ptr, value->size) ||
        (type_ptr && guest_write32(g.mem, type_ptr, value->type)))
        error = ERROR_INVALID_PARAMETER;
    else
        error = ERROR_SUCCESS;
    set_error(c, error);
    stdcall_return(c, error, 6);
}

static void s_RegSetValueExA(CPU *c) {
    uint32_t hkey = 0, name_ptr = 0, reserved = 0, type = 0;
    uint32_t data_ptr = 0, bytes = 0;
    uint32_t error = ERROR_INVALID_PARAMETER;
    if (arg32(c, 0, &hkey) || arg32(c, 1, &name_ptr) ||
        arg32(c, 2, &reserved) || arg32(c, 3, &type) ||
        arg32(c, 4, &data_ptr) || arg32(c, 5, &bytes)) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: invalid "
                "argument stack\n");
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (!g.mem) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: module is "
                "not bound to guest memory\n");
        error = ERROR_INVALID_HANDLE;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (reserved != 0u) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: Reserved=%lu\n",
                (unsigned long)reserved);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (type != REG_SZ && type != REG_DWORD && type != REG_BINARY) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: type %lu is "
                "not stored (only REG_SZ/REG_DWORD/REG_BINARY)\n",
                (unsigned long)type);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (type == REG_DWORD && bytes != 4u) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: REG_DWORD "
                "size %lu (expected 4)\n", (unsigned long)bytes);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (bytes > WIN32_REGISTRY_MAX_VALUE_BYTES) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: %lu bytes "
                "exceed the %u-byte bound\n", (unsigned long)bytes,
                (unsigned)WIN32_REGISTRY_MAX_VALUE_BYTES);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (bytes && !data_ptr) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: NULL data "
                "for %lu bytes\n", (unsigned long)bytes);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    uint8_t buffer[WIN32_REGISTRY_MAX_VALUE_BYTES];
    if (bytes && (!guest_ptr(g.mem, data_ptr, bytes) ||
                  guest_read(g.mem, data_ptr, buffer, bytes))) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: data pointer "
                "%08lx is not readable guest memory (%lu bytes)\n",
                (unsigned long)data_ptr, (unsigned long)bytes);
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    uint8_t root;
    char base[WIN32_REGISTRY_MAX_PATH];
    if (resolve_key(hkey, &root, base)) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused hkey %08lx: "
                "invalid handle\n", (unsigned long)hkey);
        error = ERROR_INVALID_HANDLE;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    int slot = handle_slot(hkey);
    if (slot < 0) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: predefined "
                "keys have no writable value store\n");
        error = ERROR_ACCESS_DENIED;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    if (!access_can_write(g.handles[slot].access)) {
        fprintf(diagnostics(), "[registry] RegSetValueExA refused: handle was "
                "opened without write access\n");
        error = ERROR_ACCESS_DENIED;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    char value_name[WIN32_REGISTRY_MAX_VALUE_NAME];
    if (name_ptr) {
        if (guest_name(name_ptr, value_name, sizeof value_name)) {
            fprintf(diagnostics(), "[registry] RegSetValueExA refused: value "
                    "name is invalid or longer than %u bytes\n",
                    (unsigned)WIN32_REGISTRY_MAX_VALUE_NAME - 1u);
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
    } else {
        value_name[0] = 0;
    }
    int key_index = base[0] ? key_find(root, base) : -1;
    if (key_index < 0) {
        log_missing_key(root, base);
        error = ERROR_FILE_NOT_FOUND;
        set_error(c, error);
        stdcall_return(c, error, 6);
        return;
    }
    RegKey *key = &g.keys[key_index];
    int vi = value_find(key, value_name);
    if (vi < 0) {
        if (key->value_count >= WIN32_REGISTRY_MAX_VALUES) {
            fprintf(diagnostics(), "[registry] RegSetValueExA refused: value "
                    "table full (%u per key)\n",
                    (unsigned)WIN32_REGISTRY_MAX_VALUES);
            error = ERROR_NO_SYSTEM_RESOURCES;
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
        for (unsigned i = 0; i < WIN32_REGISTRY_MAX_VALUES; ++i) {
            if (!key->values[i].used) {
                vi = (int)i;
                break;
            }
        }
        if (vi < 0) {
            error = ERROR_NO_SYSTEM_RESOURCES;
            set_error(c, error);
            stdcall_return(c, error, 6);
            return;
        }
        ++key->value_count;
        key->values[vi].used = 1;
        copy_text(key->values[vi].name, sizeof key->values[vi].name,
                  value_name);
    }
    RegValue *value = &key->values[vi];
    value->type = (uint8_t)type;
    value->size = (uint16_t)bytes;
    if (bytes) memcpy(value->data, buffer, bytes);
    error = ERROR_SUCCESS;
    set_error(c, error);
    stdcall_return(c, error, 6);
}

static void s_RegCloseKey(CPU *c) {
    uint32_t hkey = 0;
    uint32_t error = ERROR_INVALID_HANDLE;
    if (arg32(c, 0, &hkey)) {
        fprintf(diagnostics(), "[registry] RegCloseKey refused: invalid "
                "argument stack\n");
    } else {
        uint8_t root;
        if (root_from_predefined(hkey, &root) == 0) {
            /* Closing a predefined root is accepted by Windows and has no
             * effect on the tree. */
            error = ERROR_SUCCESS;
        } else {
            int slot = handle_slot(hkey);
            if (slot >= 0) {
                g.handles[slot].used = 0;
                g.handles[slot].key = 0;
                g.handles[slot].access = 0;
                --g.handle_count;
                error = ERROR_SUCCESS;
            } else {
                fprintf(diagnostics(), "[registry] RegCloseKey refused hkey "
                        "%08lx: invalid handle\n", (unsigned long)hkey);
            }
        }
    }
    set_error(c, error);
    stdcall_return(c, error, 1);
}

static const Win32RegistryDef defs[] = {
    {"advapi32.dll", "RegCloseKey", s_RegCloseKey},
    {"advapi32.dll", "RegSetValueExA", s_RegSetValueExA},
    {"advapi32.dll", "RegQueryValueExA", s_RegQueryValueExA},
    {"advapi32.dll", "RegOpenKeyExA", s_RegOpenKeyExA},
    {"advapi32.dll", "RegCreateKeyExA", s_RegCreateKeyExA},
};
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32RegistryDef *win32_registry_defs(unsigned *count) {
    if (count) *count = (unsigned)NDEFS;
    return defs;
}

static int dll_is(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

uint32_t win32_registry_resolve(const char *dll, const char *name) {
    if (!dll || !name ||
        (!dll_is(dll, "advapi32.dll") && !dll_is(dll, "advapi32")))
        return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (!strcmp(name, defs[i].name)) return WIN32_REGISTRY_BASE + 4u * i;
    return 0;
}

static FnEntry entries[NDEFS];
const FnEntry *win32_registry_lookup(uint32_t addr) {
    if (addr < WIN32_REGISTRY_BASE || (addr & 3u) ||
        addr - WIN32_REGISTRY_BASE >= 4u * NDEFS) return NULL;
    unsigned i = (addr - WIN32_REGISTRY_BASE) / 4u;
    entries[i] = (FnEntry){addr, defs[i].fn};
    return &entries[i];
}
