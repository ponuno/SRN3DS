/* Host smoke test for the bounded in-memory ADVAPI32 registry module.
 *
 * Calls the GuestFn thunks directly so the stdcall ESP contract (return address
 * plus exactly N arguments) is checked for every export. */
#include "win32_registry.h"
#include "win32_sync.h"
#include "win32_user.h"
#include "win32_locale.h"
#include "win32_shell.h"
#include "win32_audio.h"
#include "win32_input.h"
#include "win32_thread.h"
#include "win32_steam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x20000000u
#define DATA_SIZE 0x20000u
#define STACK 0x21000000u
#define STACK_SIZE 0x2000u
#define UNMAPPED 0xdead0000u
#define RETURN_MARKER 0xfeedbeefu
#define EAX_SENTINEL 0xabcdef01u

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_SYSTEM_RESOURCES 1450u

#define KEY_READ 0x00020019u
#define KEY_WRITE 0x00020006u
#define KEY_ALL_ACCESS 0x000f003fu
#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u

static GuestMemory *test_mem;
static unsigned failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static int read32(uint32_t address, uint32_t *value) {
    return guest_read32(test_mem, address, value);
}
static int write32(uint32_t address, uint32_t value) {
    return guest_write32(test_mem, address, value);
}
static uint32_t put_str(uint32_t address, const char *text) {
    size_t n = strlen(text) + 1u;
    CHECK(!guest_write(test_mem, address, text, n));
    return address;
}

static int invoke(CPU *c, const char *name, const uint32_t *args,
                  unsigned count, uint32_t *result) {
    uint32_t address = win32_registry_resolve("advapi32.dll", name);
    const FnEntry *entry = address ? win32_registry_lookup(address) : NULL;
    if (!entry || !result || (count && !args)) return -1;
    uint32_t sp = c->r[4];
    if (write32(sp, RETURN_MARKER)) return -1;
    for (unsigned i = 0; i < count; ++i)
        if (write32(sp + 4u + 4u * i, args[i])) return -1;

    c->r[0] = EAX_SENTINEL;
    entry->fn(c);

    /* stdcall removes the return address plus exactly `count` arguments. */
    CHECK(c->r[4] == sp + 4u + 4u * count);
    uint32_t value = 0;
    CHECK(!read32(sp, &value) && value == RETURN_MARKER);
    for (unsigned i = 0; i < count; ++i) {
        value = 0;
        CHECK(!read32(sp + 4u + 4u * i, &value) && value == args[i]);
    }
    c->r[4] = sp;
    *result = c->r[0];
    return 0;
}

static uint32_t open_key(CPU *c, uint32_t hkey, uint32_t subkey,
                         uint32_t access, uint32_t out, uint32_t *error) {
    uint32_t a[5] = {hkey, subkey, 0, access, out};
    uint32_t r = 0;
    CHECK(!invoke(c, "RegOpenKeyExA", a, 5, &r));
    if (error) *error = r;
    return r;
}

static uint32_t create_key(CPU *c, uint32_t hkey, uint32_t subkey,
                           uint32_t options, uint32_t access, uint32_t out,
                           uint32_t disposition, uint32_t *error) {
    uint32_t a[9] = {hkey, subkey, 0, 0, options, access, 0, out, disposition};
    uint32_t r = 0;
    CHECK(!invoke(c, "RegCreateKeyExA", a, 9, &r));
    if (error) *error = r;
    return r;
}

static uint32_t query_value(CPU *c, uint32_t hkey, uint32_t name, uint32_t type,
                            uint32_t data, uint32_t size, uint32_t *error) {
    uint32_t a[6] = {hkey, name, 0, type, data, size};
    uint32_t r = 0;
    CHECK(!invoke(c, "RegQueryValueExA", a, 6, &r));
    if (error) *error = r;
    return r;
}

static uint32_t set_value(CPU *c, uint32_t hkey, uint32_t name, uint32_t type,
                          uint32_t data, uint32_t bytes, uint32_t *error) {
    uint32_t a[6] = {hkey, name, 0, type, data, bytes};
    uint32_t r = 0;
    CHECK(!invoke(c, "RegSetValueExA", a, 6, &r));
    if (error) *error = r;
    return r;
}

static uint32_t close_key(CPU *c, uint32_t hkey, uint32_t *error) {
    uint32_t a[1] = {hkey};
    uint32_t r = 0;
    CHECK(!invoke(c, "RegCloseKey", a, 1, &r));
    if (error) *error = r;
    return r;
}

int main(void) {
    int rc = 2;
    FILE *log = NULL;
    char *log_text = NULL;
    GuestMemory m;
    guest_init(&m, 1u << 20);
    if (guest_map(&m, DATA, DATA_SIZE) || guest_map(&m, STACK, STACK_SIZE))
        goto done;
    test_mem = &m;
#ifdef _WIN32
    if (tmpfile_s(&log)) goto done;
#else
    log = tmpfile();
    if (!log) goto done;
#endif
    /* Sink installed before binding must survive init(). */
    win32_registry_set_log_stream(log);

    CHECK(win32_registry_init(NULL) == -1);
    if (win32_registry_init(&m)) goto done;
    CHECK(win32_registry_init(&m) == -1);

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 64u;
    /* No TEB in this harness: set_error() must simply skip the write. */
    uint32_t result = 0;

    /* Window and definition table. */
    CHECK(WIN32_REGISTRY_BASE == 0xff090000u);
    CHECK(WIN32_REGISTRY_BASE != WIN32_THUNK_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_SYNC_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_USER_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_LOCALE_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_SHELL_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_AUDIO_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_INPUT_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_THREAD_BASE);
    CHECK(WIN32_REGISTRY_BASE != WIN32_STEAM_BASE);

    unsigned def_count = 0;
    const Win32RegistryDef *defs = win32_registry_defs(&def_count);
    CHECK(defs && def_count == 5u);
    for (unsigned i = 0; i < def_count; ++i) {
        uint32_t address = WIN32_REGISTRY_BASE + 4u * i;
        CHECK(!strcmp(defs[i].dll, "advapi32.dll"));
        CHECK(defs[i].fn != NULL);
        CHECK(win32_registry_resolve("ADVAPI32.DLL", defs[i].name) == address);
        const FnEntry *entry = win32_registry_lookup(address);
        CHECK(entry && entry->addr == address && entry->fn == defs[i].fn);
        for (unsigned j = 0; j < i; ++j)
            CHECK(strcmp(defs[i].name, defs[j].name) != 0);
    }
    CHECK(!win32_registry_resolve("kernel32.dll", "RegOpenKeyExA"));
    CHECK(!win32_registry_resolve("advapi32.dll", "RegEnumKeyExA"));
    CHECK(!win32_registry_resolve("advapi32.dll", "regopenkeyexa"));
    CHECK(!win32_registry_resolve(NULL, "RegOpenKeyExA"));
    CHECK(!win32_registry_resolve("advapi32.dll", NULL));
    CHECK(win32_registry_resolve("advapi32", "RegCloseKey") == WIN32_REGISTRY_BASE);
    CHECK(!win32_registry_lookup(WIN32_REGISTRY_BASE - 4u));
    CHECK(!win32_registry_lookup(WIN32_REGISTRY_BASE + 2u));
    CHECK(!win32_registry_lookup(WIN32_REGISTRY_BASE + 4u * def_count));
    CHECK(!win32_registry_lookup(WIN32_THUNK_BASE));

    const uint32_t subkey = DATA + 0x100u;
    const uint32_t value_name = DATA + 0x300u;
    const uint32_t type_slot = DATA + 0x400u;
    const uint32_t size_slot = DATA + 0x404u;
    const uint32_t data_slot = DATA + 0x500u;
    const uint32_t out_handle = DATA + 0x600u;
    const uint32_t disposition = DATA + 0x604u;

    uint32_t err = 0xffffffffu;
    /* The module preloads exactly one install key (a real installer writes it):
     * HKLM\Software\Bethesda Softworks\Skyrim = 3 tree entries, sBasePath. */
    uint32_t keys0 = win32_registry_key_count();
    CHECK(keys0 == 3u);
    put_str(subkey, "Software\\Bethesda Softworks\\Skyrim");
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_SUCCESS);
    uint32_t seed_handle = 0;
    CHECK(!read32(out_handle, &seed_handle));
    put_str(value_name, "sBasePath");
    write32(size_slot, 64u);
    query_value(&cpu, seed_handle, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(type_slot, &result) && result == REG_SZ);
    CHECK(!read32(size_slot, &result) && result == (uint32_t)strlen("C:\\Skyrim\\") + 1u);
    close_key(&cpu, seed_handle, &err);
    CHECK(err == ERROR_SUCCESS);

    /* Unknown keys: FILE_NOT_FOUND, no fabricated handle, one log line for the
     * same name even when the guest changes case. */
    put_str(subkey, "Software\\NoSuchVendor\\NoSuchProduct");
    write32(out_handle, 0x11111111u);
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_FILE_NOT_FOUND);
    CHECK(!read32(out_handle, &result) && result == 0x11111111u);
    CHECK(win32_registry_unknown_value_count() == 0);
    put_str(subkey, "SOFTWARE\\nosuchVENDOR\\nosuchPRODUCT");
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_FILE_NOT_FOUND);
    CHECK(win32_registry_key_count() == keys0);

    /* Create all intermediate keys, then open with different spelling. */
    put_str(subkey, "Software\\NoSuchVendor\\NoSuchProduct");
    CHECK(!read32(disposition, &result) && result == 0u);
    create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_ALL_ACCESS,
               out_handle, disposition, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(disposition, &result) && result == 1u); /* REG_CREATED_NEW_KEY */
    uint32_t hkey = 0;
    CHECK(!read32(out_handle, &hkey) && hkey >= WIN32_REGISTRY_HANDLE_BASE);
    CHECK(win32_registry_key_count() == keys0 + 2u);
    CHECK(win32_registry_handle_count() == 1u);
    /* Creating the same key again opens it and reports disposition 2. */
    create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_ALL_ACCESS,
               out_handle, disposition, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(disposition, &result) && result == 2u);
    uint32_t hkey_again = 0;
    CHECK(!read32(out_handle, &hkey_again) && hkey_again != hkey);
    CHECK(win32_registry_key_count() == keys0 + 2u);
    close_key(&cpu, hkey_again, &err);
    CHECK(err == ERROR_SUCCESS);

    put_str(subkey, "software\\NOSUCHvendor\\noSUCHproduct");
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_SUCCESS);
    uint32_t hkey_read = 0;
    CHECK(!read32(out_handle, &hkey_read));
    close_key(&cpu, hkey_read, &err);
    CHECK(err == ERROR_SUCCESS);

    /* Unknown value: FILE_NOT_FOUND, requested name logged once, size and type
     * slots untouched. */
    put_str(value_name, "Installed Path");
    write32(size_slot, 0x22222222u);
    write32(type_slot, 0x33333333u);
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_FILE_NOT_FOUND);
    CHECK(win32_registry_unknown_value_count() == 1u);
    CHECK(!read32(size_slot, &result) && result == 0x22222222u);
    CHECK(!read32(type_slot, &result) && result == 0x33333333u);
    put_str(value_name, "installed PATH");
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_FILE_NOT_FOUND);
    CHECK(win32_registry_unknown_value_count() == 1u);

    /* REG_SZ write then read back (type and exact byte count). */
    const char *install = "C:\\Games\\Skyrim";
    uint32_t install_len = (uint32_t)strlen(install) + 1u;
    put_str(value_name, "Installed Path");
    put_str(data_slot, install);
    CHECK(!read32(size_slot, &result) && result == 0x22222222u);
    set_value(&cpu, hkey, value_name, REG_SZ, data_slot, install_len, &err);
    CHECK(err == ERROR_SUCCESS);
    write32(size_slot, 64u);
    write32(type_slot, 0);
    CHECK(!guest_write(test_mem, data_slot, "XXXXXXXXXXXXXXXXXXXX", 21));
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(type_slot, &result) && result == REG_SZ);
    CHECK(!read32(size_slot, &result) && result == install_len);
    {
        char got[32];
        CHECK(!guest_read(test_mem, data_slot, got, install_len));
        CHECK(!memcmp(got, install, install_len));
    }
    /* NULL data is a size/type probe. */
    write32(size_slot, 0u);
    write32(type_slot, 0u);
    query_value(&cpu, hkey, value_name, type_slot, 0, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(size_slot, &result) && result == install_len);
    CHECK(!read32(type_slot, &result) && result == REG_SZ);

    /* Short buffer: ERROR_MORE_DATA, required size reported, bytes untouched. */
    uint8_t before[8];
    CHECK(!guest_read(test_mem, data_slot, before, sizeof before));
    write32(size_slot, install_len - 1u);
    write32(type_slot, 0u);
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_MORE_DATA);
    CHECK(!read32(size_slot, &result) && result == install_len);
    CHECK(!read32(type_slot, &result) && result == REG_SZ);
    {
        uint8_t after[8];
        CHECK(!guest_read(test_mem, data_slot, after, sizeof after));
        CHECK(!memcmp(before, after, sizeof before));
    }

    /* REG_DWORD and REG_BINARY round-trip. */
    put_str(value_name, "Version");
    write32(data_slot, 0x12345678u);
    set_value(&cpu, hkey, value_name, REG_DWORD, data_slot, 4u, &err);
    CHECK(err == ERROR_SUCCESS);
    set_value(&cpu, hkey, value_name, REG_DWORD, data_slot, 3u, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    write32(size_slot, 4u);
    write32(data_slot, 0);
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(type_slot, &result) && result == REG_DWORD);
    CHECK(!read32(size_slot, &result) && result == 4u);
    CHECK(!read32(data_slot, &result) && result == 0x12345678u);
    put_str(value_name, "Blob");
    CHECK(!guest_write(test_mem, data_slot, "\x01\x02\x03\x04", 4));
    set_value(&cpu, hkey, value_name, REG_BINARY, data_slot, 4u, &err);
    CHECK(err == ERROR_SUCCESS);
    write32(size_slot, 4u);
    query_value(&cpu, hkey, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(type_slot, &result) && result == REG_BINARY);
    CHECK(!read32(data_slot, &result) && result == 0x04030201u);
    /* Unsupported type is refused, not coerced. */
    put_str(value_name, "Expanded");
    put_str(data_slot, "%TEMP%");
    set_value(&cpu, hkey, value_name, 2u /* REG_EXPAND_SZ */, data_slot, 8u, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);

    /* Read-only handle: query succeeds, set is ACCESS_DENIED, close succeeds. */
    put_str(subkey, "Software\\NoSuchVendor\\NoSuchProduct");
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(out_handle, &hkey_read));
    put_str(value_name, "Version");
    write32(size_slot, 4u);
    query_value(&cpu, hkey_read, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_SUCCESS);
    set_value(&cpu, hkey_read, value_name, REG_DWORD, data_slot, 4u, &err);
    CHECK(err == ERROR_ACCESS_DENIED);
    close_key(&cpu, hkey_read, &err);
    CHECK(err == ERROR_SUCCESS);

    /* Invalid handles and NULL. */
    close_key(&cpu, 0x12345678u, &err);
    CHECK(err == ERROR_INVALID_HANDLE);
    close_key(&cpu, 0u, &err);
    CHECK(err == ERROR_INVALID_HANDLE);
    query_value(&cpu, 0x12345678u, value_name, type_slot, data_slot, size_slot, &err);
    CHECK(err == ERROR_INVALID_HANDLE);
    put_str(subkey, "Software");
    open_key(&cpu, 0x12345678u, subkey, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_INVALID_HANDLE);
    /* Predefined roots have no stored values. */
    put_str(value_name, "Anything");
    query_value(&cpu, WIN32_REGISTRY_HKLM, value_name, type_slot,
                data_slot, size_slot, &err);
    CHECK(err == ERROR_FILE_NOT_FOUND);
    close_key(&cpu, WIN32_REGISTRY_HKLM, &err);
    CHECK(err == ERROR_SUCCESS);

    /* Guest-memory bounds: every refusal happens before any state change. */
    uint32_t keys_before = win32_registry_key_count();
    put_str(subkey, "Bounds\\Never");
    open_key(&cpu, WIN32_REGISTRY_HKLM, UNMAPPED, KEY_READ, out_handle, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, UNMAPPED, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_ALL_ACCESS,
               UNMAPPED, disposition, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    CHECK(win32_registry_key_count() == keys_before);
    create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_ALL_ACCESS,
               out_handle, UNMAPPED, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    CHECK(win32_registry_key_count() == keys_before);
    query_value(&cpu, hkey, value_name, type_slot, data_slot, UNMAPPED, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    put_str(value_name, "Version");
    write32(size_slot, 64u);
    query_value(&cpu, hkey, value_name, type_slot, UNMAPPED, size_slot, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    put_str(value_name, "Bounds value");
    set_value(&cpu, hkey, UNMAPPED, REG_DWORD, data_slot, 4u, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    set_value(&cpu, hkey, value_name, REG_DWORD, UNMAPPED, 4u, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);
    /* Overlong value name (64 chars needs a NUL at index 64). */
    {
        char long_name[WIN32_REGISTRY_MAX_VALUE_NAME + 1u];
        memset(long_name, 'A', sizeof long_name - 1u);
        long_name[sizeof long_name - 1u] = 0;
        put_str(value_name, long_name);
        set_value(&cpu, hkey, value_name, REG_DWORD, data_slot, 4u, &err);
        CHECK(err == ERROR_INVALID_PARAMETER);
    }
    /* Overlong key path. */
    {
        char long_key[WIN32_REGISTRY_MAX_PATH + 8u];
        memset(long_key, 'K', sizeof long_key - 1u);
        long_key[sizeof long_key - 1u] = 0;
        put_str(subkey, long_key);
        open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
        CHECK(err == ERROR_INVALID_PARAMETER);
    }
    /* Unsupported access rights are refused, not silently narrowed. */
    put_str(subkey, "Software");
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0x00000040u, out_handle, &err);
    CHECK(err == ERROR_INVALID_PARAMETER);

    /* Handle table: 16 live handles maximum. */
    close_key(&cpu, hkey, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(win32_registry_handle_count() == 0u);
    put_str(subkey, "Software\\Bethesda Softworks\\Skyrim");
    uint32_t held[24];
    unsigned held_count = 0, handle_failures = 0;
    for (unsigned i = 0; i < 24; ++i) {
        open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_READ, out_handle, &err);
        if (err == ERROR_SUCCESS) {
            CHECK(held_count < 24u);
            if (held_count < 24u) CHECK(!read32(out_handle, &held[held_count++]));
        } else {
            CHECK(err == ERROR_NO_SYSTEM_RESOURCES);
            ++handle_failures;
        }
    }
    CHECK(held_count == WIN32_REGISTRY_MAX_HANDLES);
    CHECK(handle_failures == 24u - WIN32_REGISTRY_MAX_HANDLES);
    for (unsigned i = 0; i < held_count; ++i) {
        close_key(&cpu, held[i], &err);
        CHECK(err == ERROR_SUCCESS);
    }
    CHECK(win32_registry_handle_count() == 0u);

    /* Reopen the main key for the remaining tests. */
    open_key(&cpu, WIN32_REGISTRY_HKLM, subkey, KEY_ALL_ACCESS, out_handle, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(!read32(out_handle, &hkey));

    /* Value table: 8 values per key, then ERROR_NO_SYSTEM_RESOURCES. */
    put_str(subkey, "ValueFull");
    create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_ALL_ACCESS,
               out_handle, disposition, &err);
    CHECK(err == ERROR_SUCCESS);
    uint32_t hkey_values = 0;
    CHECK(!read32(out_handle, &hkey_values));
    unsigned stored = 0, value_failures = 0;
    for (unsigned i = 0; i <= WIN32_REGISTRY_MAX_VALUES; ++i) {
        char name[16];
        snprintf(name, sizeof name, "V%u", i);
        put_str(value_name, name);
        write32(data_slot, i);
        set_value(&cpu, hkey_values, value_name, REG_DWORD, data_slot, 4u, &err);
        if (err == ERROR_SUCCESS) {
            ++stored;
        } else {
            CHECK(err == ERROR_NO_SYSTEM_RESOURCES);
            ++value_failures;
        }
    }
    CHECK(stored == WIN32_REGISTRY_MAX_VALUES);
    CHECK(value_failures == 1u);
    /* Overwriting an existing value in a full key still works. */
    put_str(value_name, "V0");
    write32(data_slot, 0x55555555u);
    set_value(&cpu, hkey_values, value_name, REG_DWORD, data_slot, 4u, &err);
    CHECK(err == ERROR_SUCCESS);
    close_key(&cpu, hkey_values, &err);
    CHECK(err == ERROR_SUCCESS);

    /* Key table: bounded at 64; the create that would overflow is refused. */
    unsigned created = 0, key_failures = 0;
    for (unsigned i = 0; i < 200 && key_failures == 0; ++i) {
        char name[16];
        snprintf(name, sizeof name, "K%u", i);
        put_str(subkey, name);
        create_key(&cpu, WIN32_REGISTRY_HKLM, subkey, 0, KEY_READ,
                   out_handle, disposition, &err);
        if (err == ERROR_SUCCESS) {
            uint32_t tmp = 0;
            CHECK(!read32(out_handle, &tmp));
            close_key(&cpu, tmp, &err);
            CHECK(err == ERROR_SUCCESS);
            ++created;
        } else {
            CHECK(err == ERROR_NO_SYSTEM_RESOURCES);
            ++key_failures;
        }
    }
    CHECK(created > 0u);
    CHECK(key_failures == 1u);
    CHECK(win32_registry_key_count() == WIN32_REGISTRY_MAX_KEYS);
    CHECK(win32_registry_handle_count() == 1u); /* hkey is still open */

    close_key(&cpu, hkey, &err);
    CHECK(err == ERROR_SUCCESS);
    CHECK(win32_registry_handle_count() == 0u);

    /* Shutdown drops the whole tree. */
    win32_registry_shutdown();
    CHECK(win32_registry_key_count() == 0u);
    CHECK(win32_registry_handle_count() == 0u);
    CHECK(win32_registry_unknown_value_count() == 0u);

    if (fflush(log)) goto done;
    if (fseek(log, 0, SEEK_END)) goto done;
    long length = ftell(log);
    if (length < 0 || fseek(log, 0, SEEK_SET)) goto done;
    log_text = malloc((size_t)length + 1u);
    if (!log_text) goto done;
    if (fread(log_text, 1, (size_t)length, log) != (size_t)length) goto done;
    log_text[length] = 0;
    CHECK(strstr(log_text, "open miss: HKLM\\Software\\NoSuchVendor\\NoSuchProduct"));
    CHECK(strstr(log_text, "value \"Installed Path\""));
    rc = failures ? 1 : 0;
    if (!rc) printf("PASS: win32_registry smoke\n");

done:
    free(log_text);
    if (log) fclose(log);
    guest_destroy(&m);
    return rc;
}
