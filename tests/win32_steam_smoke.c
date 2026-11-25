/* Host smoke test for the standalone offline SteamAPI module.
 *
 * This deliberately calls the GuestFn thunks directly so it can prove the
 * cdecl ESP delta and the untouched caller argument slots without relying on
 * another shim module's stdcall conventions. */
#include "win32_steam.h"
#include "win32_sync.h"
#include "win32_user.h"
#include "win32_locale.h"
#include "win32_shell.h"
#include "win32_audio.h"
#include "win32_input.h"
#include "win32_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x20000000u
#define DATA_SIZE 0x10000u
#define STACK 0x21000000u
#define STACK_SIZE 0x1000u
#define EDGE 0x22000000u
#define EDGE_SIZE 0x1000u
#define RETURN_MARKER 0xfeedbeefu
#define EAX_SENTINEL 0xabcdef01u
#define INITIAL_ID 0xccccccccu

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

static int invoke(CPU *c, const char *name, const uint32_t *args, unsigned count,
                  uint32_t *result) {
    uint32_t address = win32_steam_resolve("steam_api.dll", name);
    const FnEntry *entry = address ? win32_steam_lookup(address) : NULL;
    if (!entry || !result || (count && !args)) return -1;
    uint32_t sp = c->r[4];
    if (write32(sp, RETURN_MARKER)) return -1;
    for (unsigned i = 0; i < count; ++i)
        if (write32(sp + 4u + 4u * i, args[i])) return -1;

    c->r[0] = EAX_SENTINEL;
    entry->fn(c);

    /* cdecl removes only the return address. The caller's arguments and marker
     * must still be readable at their original addresses. */
    CHECK(c->r[4] == sp + 4u);
    uint32_t value = 0;
    CHECK(!read32(sp, &value) && value == RETURN_MARKER);
    for (unsigned i = 0; i < count; ++i) {
        value = 0;
        CHECK(!read32(sp + 4u + 4u * i, &value) && value == args[i]);
    }
    /* Model the cdecl caller's add esp,count*4 after observing the callee's
     * ret-only stack change. */
    c->r[4] = sp;
    *result = c->r[0];
    return 0;
}

static void init_object(uint32_t callback) {
    uint8_t object[12];
    memset(object, 0xaa, sizeof object);
    CHECK(!guest_write(test_mem, callback, object, sizeof object));
}

static int object_is(uint32_t callback, const uint8_t expected[12]) {
    uint8_t actual[12];
    return !guest_read(test_mem, callback, actual, sizeof actual) &&
           !memcmp(actual, expected, sizeof actual);
}

static void registered_object(uint8_t expected[12], uint32_t id) {
    memset(expected, 0xaa, 12);
    expected[4] = 0xab; /* preserve all bits except setting bit 0 */
    expected[8] = (uint8_t)id;
    expected[9] = (uint8_t)(id >> 8);
    expected[10] = (uint8_t)(id >> 16);
    expected[11] = (uint8_t)(id >> 24);
}

static int core_name(const char *name) {
    return !strcmp(name, "SteamAPI_Init") ||
           !strcmp(name, "SteamAPI_InitSafe") ||
           !strcmp(name, "SteamAPI_RunCallbacks") ||
           !strcmp(name, "SteamAPI_RegisterCallback") ||
           !strcmp(name, "SteamAPI_UnregisterCallback") ||
           !strcmp(name, "SteamAPI_Shutdown");
}

static char *read_log(FILE *log) {
    if (!log || fflush(log)) return NULL;
    if (fseek(log, 0, SEEK_END)) return NULL;
    long length = ftell(log);
    if (length < 0 || length > 1024L * 1024L || fseek(log, 0, SEEK_SET)) return NULL;
    char *text = malloc((size_t)length + 1u);
    if (!text) return NULL;
    size_t got = fread(text, 1, (size_t)length, log);
    if (got != (size_t)length) { free(text); return NULL; }
    text[got] = 0;
    return text;
}

int main(void) {
    int rc = 2;
    FILE *log = NULL;
    char *log_text = NULL;
    GuestMemory m;
    guest_init(&m, 1u << 20);
    if (guest_map(&m, DATA, DATA_SIZE) || guest_map(&m, STACK, STACK_SIZE)) goto done;
    test_mem = &m;
#ifdef _WIN32
    if (tmpfile_s(&log)) goto done;
#else
    log = tmpfile();
    if (!log) goto done;
#endif
    /* Install the sink before binding: init() must preserve host configuration,
     * so every diagnostic below is captured by this file. */
    win32_steam_set_log_stream(log);

    /* Binding contract: NULL is refused and a bound module cannot be rebound. */
    CHECK(win32_steam_init(NULL) == -1);
    if (win32_steam_init(&m)) goto done;
    CHECK(win32_steam_init(&m) == -1);

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 64u;
    uint32_t result, args[2];

    /* Proposed window and every existing base must remain disjoint. */
    CHECK(WIN32_STEAM_BASE == 0xff080000u);
    CHECK(WIN32_STEAM_BASE != WIN32_THUNK_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_SYNC_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_USER_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_LOCALE_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_SHELL_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_AUDIO_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_INPUT_BASE);
    CHECK(WIN32_STEAM_BASE != WIN32_THREAD_BASE);

    unsigned def_count = 0;
    const Win32SteamDef *defs = win32_steam_defs(&def_count);
    CHECK(defs && def_count > 8u);
    for (unsigned i = 0; i < def_count; ++i) {
        uint32_t address = WIN32_STEAM_BASE + 4u * i;
        CHECK(!strcmp(defs[i].dll, "steam_api.dll"));
        CHECK(defs[i].fn != NULL);
        CHECK(win32_steam_resolve("STEAM_API.DLL", defs[i].name) == address);
        const FnEntry *entry = win32_steam_lookup(address);
        CHECK(entry && entry->addr == address && entry->fn == defs[i].fn);
        for (unsigned j = 0; j < i; ++j)
            CHECK(strcmp(defs[i].name, defs[j].name) != 0);
    }
    CHECK(!win32_steam_resolve("kernel32.dll", "SteamAPI_Init"));
    CHECK(!win32_steam_resolve("steam_api.dll", "SteamAPI_NotKnown"));
    CHECK(!win32_steam_resolve("steam_api.dll", "steamapi_init"));
    CHECK(!win32_steam_resolve(NULL, "SteamAPI_Init"));
    CHECK(!win32_steam_resolve("steam_api.dll", NULL));
    CHECK(!win32_steam_resolve("steam_api_extra.dll", "SteamAPI_Init"));
    /* The module-suffix spelling is accepted, matching the other shims. */
    CHECK(win32_steam_resolve("steam_api", "SteamAPI_Init") == WIN32_STEAM_BASE);
    CHECK(win32_steam_defs(NULL) == defs);
    CHECK(!win32_steam_lookup(WIN32_STEAM_BASE - 4u));
    CHECK(!win32_steam_lookup(WIN32_STEAM_BASE + 2u));
    CHECK(!win32_steam_lookup(WIN32_STEAM_BASE + 4u * def_count));
    CHECK(!win32_steam_lookup(WIN32_STEAM_BASE + 0x1000u));
    CHECK(!win32_steam_lookup(WIN32_THUNK_BASE));

    /* Offline lifecycle: false Init, zero events, and no fake void result. */
    CHECK(!invoke(&cpu, "SteamAPI_Init", NULL, 0, &result) && result == 0);
    CHECK(!invoke(&cpu, "SteamAPI_InitSafe", NULL, 0, &result) && result == 0);
    CHECK(!invoke(&cpu, "SteamAPI_RunCallbacks", NULL, 0, &result) &&
          result == EAX_SENTINEL);
    CHECK(!invoke(&cpu, "SteamAPI_Shutdown", NULL, 0, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);

    /* Every non-lifecycle entry is an explicit named offline zero/NULL. */
    for (unsigned i = 0; i < def_count; ++i) {
        if (core_name(defs[i].name)) continue;
        CHECK(!invoke(&cpu, defs[i].name, NULL, 0, &result) && result == 0);
    }

    /* Null and unmapped callback pointers are rejected without object writes. */
    uint32_t cb0 = DATA + 0x100u, cb1 = DATA + 0x200u;
    uint8_t initial[12], expected[12];
    init_object(cb0);
    CHECK(!guest_read(test_mem, cb0, initial, sizeof initial));
    args[0] = 0; args[1] = 0x11111111u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    CHECK(object_is(cb0, initial));

    uint32_t partial = DATA + DATA_SIZE - 8u;
    uint8_t tail_before[8], tail_after[8];
    CHECK(!guest_read(test_mem, partial, tail_before, sizeof tail_before));
    args[0] = partial; args[1] = 0x22222222u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    CHECK(!guest_read(test_mem, partial, tail_after, sizeof tail_after));
    CHECK(!memcmp(tail_before, tail_after, sizeof tail_before));

    /* An invalid argument stack is also a clean refusal, not a guest fault. */
    const FnEntry *register_fn = win32_steam_lookup(
        win32_steam_resolve("steam_api.dll", "SteamAPI_RegisterCallback"));
    CPU bad_stack = cpu;
    bad_stack.r[4] = 0xdead0000u;
    bad_stack.r[0] = EAX_SENTINEL;
    CHECK(register_fn != NULL);
    if (register_fn) register_fn->fn(&bad_stack);
    CHECK(bad_stack.r[4] == 0xdead0004u && bad_stack.r[0] == EAX_SENTINEL);
    CHECK(win32_steam_callback_count() == 0);

    /* Registration writes exactly bit 0 at +4 and the id at +8. */
    args[0] = cb0; args[1] = 0x12345678u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    registered_object(expected, args[1]);
    CHECK(object_is(cb0, expected));

    /* Duplicate registration is rejected and does not rewrite the object. */
    args[1] = 0x87654321u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    registered_object(expected, 0x12345678u);
    CHECK(object_is(cb0, expected));

    /* RunCallbacks remains empty even with a live registry entry. */
    CHECK(!invoke(&cpu, "SteamAPI_RunCallbacks", NULL, 0, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    CHECK(object_is(cb0, expected));

    /* Unregister refuses null/unknown pointers, then clears only bit 0. */
    init_object(cb1);
    args[0] = 0;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    args[0] = cb1;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    CHECK(object_is(cb1, initial));
    args[0] = cb0;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    memcpy(expected, initial, sizeof initial);
    expected[8] = 0x78; expected[9] = 0x56;
    expected[10] = 0x34; expected[11] = 0x12;
    CHECK(object_is(cb0, expected)); /* id is deliberately retained */

    /* The same pointer can be registered again with a new id. */
    args[0] = cb0; args[1] = 0x0a0b0c0du;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          win32_steam_callback_count() == 1);
    CHECK(!invoke(&cpu, "SteamAPI_Shutdown", NULL, 0, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    registered_object(expected, args[1]);
    CHECK(object_is(cb0, expected)); /* Shutdown clears host state, not guest bytes */

    /* Fill the bounded registry, reject one more, then reuse a freed slot. */
    for (unsigned i = 0; i < WIN32_STEAM_CALLBACK_CAPACITY; ++i) {
        uint32_t callback = DATA + 0x1000u + 16u * i;
        init_object(callback);
        args[0] = callback; args[1] = 0x1000u + i;
        CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result));
        CHECK(win32_steam_callback_count() == i + 1u);
    }
    uint32_t extra = DATA + 0x1000u + 16u * WIN32_STEAM_CALLBACK_CAPACITY;
    init_object(extra);
    CHECK(!guest_read(test_mem, extra, initial, sizeof initial));
    args[0] = extra; args[1] = 0xffffffffu;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL &&
          win32_steam_callback_count() == WIN32_STEAM_CALLBACK_CAPACITY);
    CHECK(object_is(extra, initial));

    uint32_t freed = DATA + 0x1000u + 16u * 7u;
    args[0] = freed;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result));
    CHECK(win32_steam_callback_count() == WIN32_STEAM_CALLBACK_CAPACITY - 1u);
    args[0] = extra; args[1] = 0xffffffffu;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result));
    CHECK(win32_steam_callback_count() == WIN32_STEAM_CALLBACK_CAPACITY);

    /* If a registered object's memory disappears, Unregister refuses to claim
     * success and keeps the host entry until Shutdown clears the registry. */
    CHECK(!invoke(&cpu, "SteamAPI_Shutdown", NULL, 0, &result));
    CHECK(win32_steam_callback_count() == 0);
    CHECK(!guest_map(&m, EDGE, EDGE_SIZE));
    uint32_t edge_callback = EDGE + 0x100u;
    init_object(edge_callback);
    args[0] = edge_callback; args[1] = 0x5555aaaa;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result));
    CHECK(win32_steam_callback_count() == 1);
    CHECK(!guest_unmap(&m, EDGE));
    args[0] = edge_callback;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 1);
    CHECK(!invoke(&cpu, "SteamAPI_Shutdown", NULL, 0, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);

    /* Unbound module: callback thunks still honour cdecl and refuse cleanly
     * with a named diagnostic instead of touching guest memory. Shutdown drops
     * the host binding (and the sink), so reinstall the sink for this check. */
    win32_steam_shutdown();
    win32_steam_set_log_stream(log);
    CHECK(win32_steam_callback_count() == 0);
    args[0] = cb0; args[1] = 0x61616161u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    args[0] = cb0;
    CHECK(!invoke(&cpu, "SteamAPI_UnregisterCallback", args, 1, &result) &&
          result == EAX_SENTINEL && win32_steam_callback_count() == 0);
    CHECK(!invoke(&cpu, "SteamAPI_Init", NULL, 0, &result) && result == 0);

    log_text = read_log(log);
    CHECK(log_text != NULL);
    if (log_text) {
        CHECK(strstr(log_text, "[steam] offline: no Steam client") != NULL);
        CHECK(strstr(log_text, "SteamAPI_Init failed") != NULL);
        CHECK(strstr(log_text, "SteamAPI_RunCallbacks dispatched 0 events") != NULL);
        CHECK(strstr(log_text, "ignored null callback") != NULL);
        CHECK(strstr(log_text, "rejected duplicate callback") != NULL);
        CHECK(strstr(log_text, "registry full (32)") != NULL);
        CHECK(strstr(log_text, "invalid callback memory") != NULL);
        CHECK(strstr(log_text, "ignored unregistered callback") != NULL);
        CHECK(strstr(log_text, "SteamAPI_RegisterCallback ignored: module is not bound") != NULL);
        CHECK(strstr(log_text, "SteamAPI_UnregisterCallback ignored: module is not bound") != NULL);
        CHECK(strstr(log_text, "SteamAPI_Shutdown cleared") != NULL);
        for (unsigned i = 0; i < def_count; ++i) {
            if (core_name(defs[i].name)) continue;
            char wanted[160];
            snprintf(wanted, sizeof wanted,
                     "[steam] offline: %s unavailable (no Steam client)", defs[i].name);
            CHECK(strstr(log_text, wanted) != NULL);
        }
    }

    win32_steam_set_log_stream(NULL);
    if (log) { fclose(log); log = NULL; }
    win32_steam_shutdown();
    CHECK(win32_steam_callback_count() == 0);
    CHECK(!win32_steam_init(&m));
    args[0] = cb0; args[1] = 0x42424242u;
    CHECK(!invoke(&cpu, "SteamAPI_RegisterCallback", args, 2, &result) &&
          win32_steam_callback_count() == 1);
    win32_steam_shutdown();
    CHECK(win32_steam_callback_count() == 0);

    rc = failures ? 1 : 0;
done:
    free(log_text);
    win32_steam_set_log_stream(NULL);
    if (log) fclose(log);
    win32_steam_shutdown();
    guest_destroy(&m);
    if (rc) {
        fprintf(stderr, "FAIL: win32_steam smoke (%u failures)\n", failures);
        return rc;
    }
    puts("PASS: win32_steam smoke");
    return 0;
}
