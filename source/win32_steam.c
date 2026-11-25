/* Standalone offline SteamAPI compatibility.
 *
 * The retail steam_api.dll exports used here are cdecl: the callee removes only
 * its 4-byte return address and leaves every argument for the caller. None of
 * these thunks executes guest code from steam_api.dll or fabricates a Steam
 * service/event. */
#include "win32_steam.h"
#include <string.h>

#define CALLBACK_OBJECT_BYTES 12u
#define CALLBACK_FLAGS_OFFSET 4u
#define CALLBACK_ID_OFFSET 8u
#define CALLBACK_REGISTERED_BIT UINT32_C(1)

typedef struct {
    uint32_t callback;
    uint32_t id;
} SteamCallback;

static struct {
    GuestMemory *mem;
    SteamCallback callbacks[WIN32_STEAM_CALLBACK_CAPACITY];
    unsigned count;
    FILE *log;
} g;

static FILE *diagnostics(void) { return g.log ? g.log : stderr; }

void win32_steam_set_log_stream(FILE *stream) { g.log = stream; }

unsigned win32_steam_callback_count(void) { return g.count; }

/* Binding replaces module state only. A configured diagnostics sink is host
 * configuration (a front-end may install it before win32_init()), so it must
 * not silently fall back to stderr just because the module was bound. */
int win32_steam_init(GuestMemory *m) {
    FILE *log = g.log;
    if (!m || g.mem) return -1;
    memset(&g, 0, sizeof g);
    g.log = log;
    g.mem = m;
    return 0;
}

/* Teardown drops the sink as well: the owner normally closes it next, and
 * retaining a closed FILE* would be worse than returning to stderr. */
void win32_steam_shutdown(void) {
    memset(&g, 0, sizeof g);
}

static int arg32(CPU *c, unsigned index, uint32_t *value) {
    uint64_t address = (uint64_t)c->r[4] + 4u + (uint64_t)index * 4u;
    if (!g.mem || !value || address > UINT32_MAX) return -1;
    return guest_read32(g.mem, (uint32_t)address, value);
}

/* Validate the complete 12-byte CCallbackBase before touching either mutable
 * field. guest_read() validates/backs every page before returning any bytes. */
static int callback_flags(uint32_t callback, uint8_t *flags) {
    uint8_t object[CALLBACK_OBJECT_BYTES];
    if (!g.mem || !flags || callback > UINT32_MAX - (CALLBACK_OBJECT_BYTES - 1u))
        return -1;
    if (guest_read(g.mem, callback, object, sizeof object)) return -1;
    *flags = object[CALLBACK_FLAGS_OFFSET];
    return 0;
}

static int callback_write_flag(uint32_t callback, uint8_t flags) {
    return guest_write(g.mem, callback + CALLBACK_FLAGS_OFFSET, &flags, 1);
}

static int registry_find(uint32_t callback) {
    for (unsigned i = 0; i < g.count; ++i)
        if (g.callbacks[i].callback == callback) return (int)i;
    return -1;
}

static void registry_remove(unsigned index) {
    --g.count;
    if (index != g.count) g.callbacks[index] = g.callbacks[g.count];
    g.callbacks[g.count].callback = 0;
    g.callbacks[g.count].id = 0;
}

static void registry_clear(void) {
    memset(g.callbacks, 0, sizeof g.callbacks);
    g.count = 0;
}

/* cdecl: pop only the return address. Arguments remain at the caller's old
 * [esp+4...] locations for the caller to clean up. Void exports leave EAX alone. */
static void return_void(CPU *c) { c->r[4] += 4u; }
static void return_zero(CPU *c) { c->r[0] = 0; return_void(c); }

static void offline_init(CPU *c, const char *name) {
    fprintf(diagnostics(), "[steam] offline: no Steam client (%s failed)\n", name);
    return_zero(c);
}

static void offline_zero(CPU *c, const char *name) {
    fprintf(diagnostics(), "[steam] offline: %s unavailable (no Steam client)\n", name);
    return_zero(c);
}

#define OFFLINE_INIT(name) \
    static void s_##name(CPU *c) { offline_init(c, #name); }
#define OFFLINE_ZERO(name) \
    static void s_##name(CPU *c) { offline_zero(c, #name); }

OFFLINE_INIT(SteamAPI_Init)
OFFLINE_INIT(SteamAPI_InitSafe)
OFFLINE_ZERO(SteamAPI_IsSteamRunning)
OFFLINE_ZERO(SteamAPI_RestartAppIfNecessary)
OFFLINE_ZERO(SteamAPI_GetHSteamPipe)
OFFLINE_ZERO(SteamAPI_GetHSteamUser)
OFFLINE_ZERO(SteamAPI_GetSteamInstallPath)
OFFLINE_ZERO(Steam_GetHSteamUserCurrent)
OFFLINE_ZERO(SteamApps)
OFFLINE_ZERO(SteamClient)
OFFLINE_ZERO(SteamController)
OFFLINE_ZERO(SteamFriends)
OFFLINE_ZERO(SteamGameServer)
OFFLINE_ZERO(SteamGameServerApps)
OFFLINE_ZERO(SteamGameServerHTTP)
OFFLINE_ZERO(SteamGameServerNetworking)
OFFLINE_ZERO(SteamGameServerStats)
OFFLINE_ZERO(SteamGameServerUtils)
OFFLINE_ZERO(SteamHTMLSurface)
OFFLINE_ZERO(SteamHTTP)
OFFLINE_ZERO(SteamMatchmaking)
OFFLINE_ZERO(SteamMatchmakingServers)
OFFLINE_ZERO(SteamMusic)
OFFLINE_ZERO(SteamMusicRemote)
OFFLINE_ZERO(SteamNetworking)
OFFLINE_ZERO(SteamRemoteStorage)
OFFLINE_ZERO(SteamScreenshots)
OFFLINE_ZERO(SteamUGC)
OFFLINE_ZERO(SteamUnifiedMessages)
OFFLINE_ZERO(SteamUser)
OFFLINE_ZERO(SteamUserStats)
OFFLINE_ZERO(SteamUtils)
#undef OFFLINE_INIT
#undef OFFLINE_ZERO

static void s_SteamAPI_RunCallbacks(CPU *c) {
    /* The registry has no queue and there is no offline event source. Never
     * invoke a callback or synthesize a Steam event. */
    fprintf(diagnostics(),
            "[steam] offline: SteamAPI_RunCallbacks dispatched 0 events (no Steam client)\n");
    return_void(c);
}

static void s_SteamAPI_RegisterCallback(CPU *c) {
    uint32_t callback, id;
    uint8_t flags;
    if (!g.mem) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback ignored: module is not bound to guest memory\n");
        return_void(c);
        return;
    }
    if (arg32(c, 0, &callback) || arg32(c, 1, &id)) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback ignored: invalid argument stack\n");
        return_void(c);
        return;
    }
    if (!callback) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback ignored null callback\n");
        return_void(c);
        return;
    }
    if (registry_find(callback) >= 0) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback rejected duplicate callback %08lx\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    if (g.count >= WIN32_STEAM_CALLBACK_CAPACITY) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback rejected callback %08lx: registry full (%u)\n",
                (unsigned long)callback, (unsigned)WIN32_STEAM_CALLBACK_CAPACITY);
        return_void(c);
        return;
    }
    if (callback_flags(callback, &flags)) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback rejected callback %08lx: invalid callback memory\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }

    /* Match the observed DLL order: set byte bit 0 at +4, then write the id at
     * +8. Roll back the flag if the second write ever fails. */
    if (callback_write_flag(callback, (uint8_t)(flags | CALLBACK_REGISTERED_BIT))) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback could not write callback %08lx flags\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    if (guest_write32(g.mem, callback + CALLBACK_ID_OFFSET, id)) {
        (void)callback_write_flag(callback, flags);
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_RegisterCallback could not write callback %08lx id\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    g.callbacks[g.count].callback = callback;
    g.callbacks[g.count].id = id;
    ++g.count;
    return_void(c);
}

static void s_SteamAPI_UnregisterCallback(CPU *c) {
    uint32_t callback;
    uint8_t flags;
    if (!g.mem) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback ignored: module is not bound to guest memory\n");
        return_void(c);
        return;
    }
    if (arg32(c, 0, &callback)) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback ignored: invalid argument stack\n");
        return_void(c);
        return;
    }
    if (!callback) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback ignored null callback\n");
        return_void(c);
        return;
    }
    int index = registry_find(callback);
    if (index < 0) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback ignored unregistered callback %08lx\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    if (callback_flags(callback, &flags)) {
        /* Keep the registry entry: the guest object could not be updated, so
         * claiming the callback was unregistered would be dishonest. */
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback rejected callback %08lx: invalid callback memory\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    if (callback_write_flag(callback, (uint8_t)(flags & ~CALLBACK_REGISTERED_BIT))) {
        fprintf(diagnostics(),
                "[steam] offline: SteamAPI_UnregisterCallback could not clear callback %08lx flags\n",
                (unsigned long)callback);
        return_void(c);
        return;
    }
    registry_remove((unsigned)index);
    return_void(c);
}

static void s_SteamAPI_Shutdown(CPU *c) {
    unsigned cleared = g.count;
    registry_clear();
    fprintf(diagnostics(),
            "[steam] offline: SteamAPI_Shutdown cleared %u callback(s); no Steam service was running\n",
            cleared);
    return_void(c);
}

static const Win32SteamDef defs[] = {
    {"steam_api.dll", "SteamAPI_Init", s_SteamAPI_Init},
    {"steam_api.dll", "SteamAPI_InitSafe", s_SteamAPI_InitSafe},
    {"steam_api.dll", "SteamAPI_IsSteamRunning", s_SteamAPI_IsSteamRunning},
    {"steam_api.dll", "SteamAPI_RestartAppIfNecessary", s_SteamAPI_RestartAppIfNecessary},
    {"steam_api.dll", "SteamAPI_RunCallbacks", s_SteamAPI_RunCallbacks},
    {"steam_api.dll", "SteamAPI_RegisterCallback", s_SteamAPI_RegisterCallback},
    {"steam_api.dll", "SteamAPI_UnregisterCallback", s_SteamAPI_UnregisterCallback},
    {"steam_api.dll", "SteamAPI_Shutdown", s_SteamAPI_Shutdown},
    {"steam_api.dll", "SteamAPI_GetHSteamPipe", s_SteamAPI_GetHSteamPipe},
    {"steam_api.dll", "SteamAPI_GetHSteamUser", s_SteamAPI_GetHSteamUser},
    {"steam_api.dll", "SteamAPI_GetSteamInstallPath", s_SteamAPI_GetSteamInstallPath},
    {"steam_api.dll", "Steam_GetHSteamUserCurrent", s_Steam_GetHSteamUserCurrent},
    {"steam_api.dll", "SteamApps", s_SteamApps},
    {"steam_api.dll", "SteamClient", s_SteamClient},
    {"steam_api.dll", "SteamController", s_SteamController},
    {"steam_api.dll", "SteamFriends", s_SteamFriends},
    {"steam_api.dll", "SteamGameServer", s_SteamGameServer},
    {"steam_api.dll", "SteamGameServerApps", s_SteamGameServerApps},
    {"steam_api.dll", "SteamGameServerHTTP", s_SteamGameServerHTTP},
    {"steam_api.dll", "SteamGameServerNetworking", s_SteamGameServerNetworking},
    {"steam_api.dll", "SteamGameServerStats", s_SteamGameServerStats},
    {"steam_api.dll", "SteamGameServerUtils", s_SteamGameServerUtils},
    {"steam_api.dll", "SteamHTMLSurface", s_SteamHTMLSurface},
    {"steam_api.dll", "SteamHTTP", s_SteamHTTP},
    {"steam_api.dll", "SteamMatchmaking", s_SteamMatchmaking},
    {"steam_api.dll", "SteamMatchmakingServers", s_SteamMatchmakingServers},
    {"steam_api.dll", "SteamMusic", s_SteamMusic},
    {"steam_api.dll", "SteamMusicRemote", s_SteamMusicRemote},
    {"steam_api.dll", "SteamNetworking", s_SteamNetworking},
    {"steam_api.dll", "SteamRemoteStorage", s_SteamRemoteStorage},
    {"steam_api.dll", "SteamScreenshots", s_SteamScreenshots},
    {"steam_api.dll", "SteamUGC", s_SteamUGC},
    {"steam_api.dll", "SteamUnifiedMessages", s_SteamUnifiedMessages},
    {"steam_api.dll", "SteamUser", s_SteamUser},
    {"steam_api.dll", "SteamUserStats", s_SteamUserStats},
    {"steam_api.dll", "SteamUtils", s_SteamUtils},
};
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32SteamDef *win32_steam_defs(unsigned *count) {
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

uint32_t win32_steam_resolve(const char *dll, const char *name) {
    if (!dll || !name || (!dll_is(dll, "steam_api.dll") && !dll_is(dll, "steam_api")))
        return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (!strcmp(name, defs[i].name)) return WIN32_STEAM_BASE + 4u * i;
    return 0;
}

static FnEntry entries[NDEFS];
const FnEntry *win32_steam_lookup(uint32_t addr) {
    if (addr < WIN32_STEAM_BASE || (addr & 3u) ||
        addr - WIN32_STEAM_BASE >= 4u * NDEFS) return NULL;
    unsigned i = (addr - WIN32_STEAM_BASE) / 4u;
    entries[i] = (FnEntry){addr, defs[i].fn};
    return &entries[i];
}
