/* Regression test for the profile/save path layer.
 *
 * The retail engine builds its profile root from SHGetFolderPathA:
 *   LocalAppData -> "<LocalAppData>\Skyrim\"
 *   Documents    -> "<Documents>\My Games\Skyrim\"
 * and really emits "LocalAppData\Skyrim\.\Plugins.txt" (the "." was rejected
 * by path_for before this test existed).  Saves live below the Documents
 * profile root.  This test pins the mapping and a real write/read/enumerate
 * round-trip through the win32 shims. */
#include "win32_shims.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DATA 0x20000000u
#define DATA_SIZE 0x20000u
#define STACK 0x21000000u
#define STACK_SIZE 0x2000u
#define RETURN_MARKER 0xfeedbeefu
#define EAX_SENTINEL 0xabcdef01u

static GuestMemory *g_mem;
static unsigned failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static uint32_t put_str(uint32_t address, const char *text) {
    size_t n = strlen(text) + 1u;
    CHECK(!guest_write(g_mem, address, text, n));
    return address;
}

static int invoke(CPU *c, const char *dll, const char *name,
                  const uint32_t *args, unsigned count, uint32_t *result) {
    PeImport import = {.dll = dll, .name = name};
    uint32_t address = win32_resolve(&import, NULL);
    const FnEntry *entry = address ? win32_lookup(address) : NULL;
    if (!entry || !result) return -1;
    uint32_t sp = c->r[4];
    if (guest_write32(g_mem, sp, RETURN_MARKER)) return -1;
    for (unsigned i = 0; i < count; ++i)
        if (guest_write32(g_mem, sp + 4u + 4u * i, args[i])) return -1;
    c->r[0] = EAX_SENTINEL;
    entry->fn(c);
    /* stdcall: return address plus exactly `count` arguments. */
    CHECK(c->r[4] == sp + 4u + 4u * count);
    *result = c->r[0];
    c->r[4] = sp;
    return 0;
}

static void expect_map(const char *guest, const char *expected) {
    char host[1024];
    int rc = win32_guest_path(guest, host, sizeof host);
    CHECK(rc == WIN32_GUEST_PATH_OK);
    if (rc == WIN32_GUEST_PATH_OK) {
        if (strcmp(host, expected) != 0)
            fprintf(stderr, "map '%s' -> '%s' (expected '%s')\n",
                    guest, host, expected);
        CHECK(!strcmp(host, expected));
    }
}

static void make_dir(CPU *c, const char *guest) {
    uint32_t args[2];
    args[0] = put_str(DATA + 0x1000, guest);
    args[1] = 0;
    uint32_t r = 0;
    CHECK(!invoke(c, "kernel32.dll", "CreateDirectoryA", args, 2, &r));
    CHECK(r == 1);
}

int main(void) {
    GuestMemory m;
    guest_init(&m, 1u << 20);
    if (guest_map(&m, DATA, DATA_SIZE) || guest_map(&m, STACK, STACK_SIZE))
        goto done;
    g_mem = &m;
#ifdef _WIN32
    _mkdir("save_paths_root");
#else
    mkdir("save_paths_root", 0777);
#endif
    if (win32_init(&m, 0x100000u, "save_paths_root") != 0) goto done;

    /* --- mapping ------------------------------------------------------- */
    expect_map("LocalAppData\\Skyrim\\.\\Plugins.txt",
               "save_paths_root/LocalAppData/Skyrim/Plugins.txt");
    expect_map(".\\DATA\\Update.bsa", "save_paths_root/DATA/Update.bsa");
    expect_map("C:\\Skyrim\\DATA\\Update.bsa",
               "save_paths_root/DATA/Update.bsa");
    expect_map("F:\\Skyrim\\Data\\Update.bsa",
               "save_paths_root/Data/Update.bsa");
    expect_map("sdmc:/Skyrim\\Data\\Update.bsa",
               "save_paths_root/Data/Update.bsa");
    expect_map("Documents\\My Games\\Skyrim\\SkyrimPrefs.ini",
               "save_paths_root/Documents/My Games/Skyrim/SkyrimPrefs.ini");
    expect_map("LocalAppData\\Skyrim\\Skyrim.ini",
               "save_paths_root/LocalAppData/Skyrim/Skyrim.ini");
    expect_map("Documents\\My Games\\Skyrim\\Saves\\autosave.ess",
               "save_paths_root/Documents/My Games/Skyrim/Saves/autosave.ess");
    CHECK(win32_guest_path("../escape", (char[64]){0}, 64) ==
          WIN32_GUEST_PATH_OUTSIDE);
    CHECK(win32_guest_path("Documents\\..\\..\\escape", (char[64]){0}, 64) ==
          WIN32_GUEST_PATH_OUTSIDE);
    CHECK(win32_guest_path("C:\\Windows\\system32\\kernel32.dll",
                           (char[64]){0}, 64) == WIN32_GUEST_PATH_OUTSIDE);

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 128u;

    /* The profile builder creates these one level at a time on a real run. */
    make_dir(&cpu, "LocalAppData");
    make_dir(&cpu, "LocalAppData\\Skyrim");
    make_dir(&cpu, "Documents");
    make_dir(&cpu, "Documents\\My Games");
    make_dir(&cpu, "Documents\\My Games\\Skyrim");
    make_dir(&cpu, "Documents\\My Games\\Skyrim\\Saves");

    /* The exact path observed from TESV: LocalAppData\Skyrim\.\Plugins.txt.
     * The "." component used to be rejected, so the read never happened. */
    {
        uint32_t plugins = put_str(DATA + 0x2200,
                                   "LocalAppData\\Skyrim\\.\\Plugins.txt");
        uint32_t text = put_str(DATA + 0x2300, "*Skyrim.esm");
        uint32_t args_p[7];
        uint32_t rp = 0;
        args_p[0] = plugins; args_p[1] = 0xC0000000u; args_p[2] = 0;
        args_p[3] = 0; args_p[4] = 2; args_p[5] = 0x80; args_p[6] = 0;
        CHECK(!invoke(&cpu, "kernel32.dll", "CreateFileA", args_p, 7, &rp));
        CHECK(rp != UINT32_MAX);
        uint32_t ph = rp;
        args_p[0] = ph; args_p[1] = text; args_p[2] = 11; args_p[3] = DATA + 0x2600; args_p[4] = 0;
        CHECK(!invoke(&cpu, "kernel32.dll", "WriteFile", args_p, 5, &rp));
        CHECK(rp == 1);
        args_p[0] = ph;
        CHECK(!invoke(&cpu, "kernel32.dll", "CloseHandle", args_p, 1, &rp));
        CHECK(rp == 1);
        args_p[0] = plugins;
        CHECK(!invoke(&cpu, "kernel32.dll", "GetFileAttributesA", args_p, 1, &rp));
        CHECK(rp == 0x80);
    }

    /* --- save write -> close -> reopen read ---------------------------- */
    const char *ess = "Documents\\My Games\\Skyrim\\Saves\\autosave.ess";
    uint32_t save_name = put_str(DATA + 0x2000, ess);
    uint32_t payload = put_str(DATA + 0x2400, "TESV save payload");
    uint32_t io = DATA + 0x2600;
    uint32_t args[7];
    uint32_t r = 0;

    args[0] = save_name; args[1] = 0xC0000000u; args[2] = 0;
    args[3] = 0; args[4] = 2 /* CREATE_ALWAYS */; args[5] = 0x80; args[6] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "CreateFileA", args, 7, &r));
    CHECK(r != UINT32_MAX);
    uint32_t wh = r;

    args[0] = wh; args[1] = payload; args[2] = 17; args[3] = io; args[4] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "WriteFile", args, 5, &r));
    CHECK(r == 1);
    CHECK(!guest_read32(g_mem, io, &r) && r == 17);

    args[0] = wh;
    CHECK(!invoke(&cpu, "kernel32.dll", "CloseHandle", args, 1, &r));
    CHECK(r == 1);

    args[0] = save_name; args[1] = 0x80000000u; args[2] = 1;
    args[3] = 0; args[4] = 3 /* OPEN_EXISTING */; args[5] = 0x80; args[6] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "CreateFileA", args, 7, &r));
    CHECK(r != UINT32_MAX);
    uint32_t rh = r;
    uint32_t read_buf = DATA + 0x2800;
    args[0] = rh; args[1] = read_buf; args[2] = 32; args[3] = io; args[4] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "ReadFile", args, 5, &r));
    CHECK(r == 1);
    CHECK(!guest_read32(g_mem, io, &r) && r == 17);
    {
        char got[32];
        CHECK(!guest_read(g_mem, read_buf, got, 17));
        CHECK(!memcmp(got, "TESV save payload", 17));
    }
    args[0] = rh;
    CHECK(!invoke(&cpu, "kernel32.dll", "CloseHandle", args, 1, &r));
    CHECK(r == 1);

    /* A .skse sidecar takes the same path and stays writable. */
    uint32_t skse = put_str(DATA + 0x2a00,
                            "Documents\\My Games\\Skyrim\\Saves\\autosave.skse");
    args[0] = skse; args[1] = 0xC0000000u; args[2] = 0;
    args[3] = 0; args[4] = 2; args[5] = 0x80; args[6] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "CreateFileA", args, 7, &r));
    CHECK(r != UINT32_MAX);
    uint32_t sh = r;
    args[0] = sh; args[1] = payload; args[2] = 4; args[3] = io; args[4] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "WriteFile", args, 5, &r));
    CHECK(r == 1);
    args[0] = sh;
    CHECK(!invoke(&cpu, "kernel32.dll", "CloseHandle", args, 1, &r));
    CHECK(r == 1);

    /* --- enumeration used by the save list ----------------------------- */
    uint32_t pattern = put_str(DATA + 0x2c00,
                               "Documents\\My Games\\Skyrim\\Saves\\*.ess");
    uint32_t find_data = DATA + 0x3000;
    args[0] = pattern; args[1] = find_data;
    CHECK(!invoke(&cpu, "kernel32.dll", "FindFirstFileA", args, 2, &r));
    CHECK(r != UINT32_MAX);
    CHECK(!strcmp(guest_cstr(g_mem, find_data + 44, 260), "autosave.ess"));
    args[0] = r;
    CHECK(!invoke(&cpu, "kernel32.dll", "FindClose", args, 1, &r));
    CHECK(r == 1);

    args[0] = save_name;
    CHECK(!invoke(&cpu, "kernel32.dll", "GetFileAttributesA", args, 1, &r));
    CHECK(r == 0x80);

    /* --- multiple saves + mtime ordering -------------------------------
     * The Continue list enumerates "Saves\*.ess", reads ftLastWriteTime from
     * WIN32_FIND_DATAA and orders entries newest-first with CompareFileTime.
     * Pin three files with known, distinct times so a shim that loses the
     * mtime (or returns creation time) is visible. */
    {
        static const struct { const char *name; uint64_t unix_sec; }
        saves[] = {
            { "Documents\\My Games\\Skyrim\\Saves\\zz_old.ess", 1577836800ull },
            { "Documents\\My Games\\Skyrim\\Saves\\zz_mid.ess", 1609459200ull },
            { "Documents\\My Games\\Skyrim\\Saves\\zz_new.ess", 1640995200ull },
        };
        /* 1970-01-01 .. year 3000, FILETIME uses 100 ns ticks since 1601. */
        const uint64_t epoch = 11644473600ull;
        for (unsigned i = 0; i < sizeof saves / sizeof saves[0]; ++i) {
            uint32_t path = put_str(DATA + 0x4000 + i * 0x100, saves[i].name);
            uint32_t a7[7];
            a7[0] = path; a7[1] = 0xC0000000u; a7[2] = 0; a7[3] = 0;
            a7[4] = 2; a7[5] = 0x80; a7[6] = 0;
            CHECK(!invoke(&cpu, "kernel32.dll", "CreateFileA", a7, 7, &r));
            CHECK(r != UINT32_MAX);
            uint32_t h = r;
            uint64_t ft = (saves[i].unix_sec + epoch) * 10000000ull;
            CHECK(!guest_write(g_mem, DATA + 0x4800 + i * 8, &ft, 8));
            a7[0] = h; a7[1] = 0; a7[2] = 0; a7[3] = DATA + 0x4800 + i * 8;
            CHECK(!invoke(&cpu, "kernel32.dll", "SetFileTime", a7, 4, &r));
            CHECK(r == 1);
            a7[0] = h;
            CHECK(!invoke(&cpu, "kernel32.dll", "CloseHandle", a7, 1, &r));
            CHECK(r == 1);
        }

        struct found { char name[64]; uint64_t ft; };
        struct found got[8];
        unsigned ngot = 0;
        uint32_t pat = put_str(DATA + 0x4c00,
                               "Documents\\My Games\\Skyrim\\Saves\\zz_*.ess");
        uint32_t fd = DATA + 0x5000;
        args[0] = pat; args[1] = fd;
        CHECK(!invoke(&cpu, "kernel32.dll", "FindFirstFileA", args, 2, &r));
        CHECK(r != UINT32_MAX);
        uint32_t fh = r;
        do {
            const char *nm = guest_cstr(g_mem, fd + 44, 260);
            if (ngot < sizeof got / sizeof got[0]) {
                snprintf(got[ngot].name, sizeof got[ngot].name, "%s", nm);
                uint8_t ts[8];
                CHECK(!guest_read(g_mem, fd + 20, ts, 8));
                memcpy(&got[ngot].ft, ts, 8);
                ++ngot;
            }
            args[0] = fh; args[1] = fd;
        } while (invoke(&cpu, "kernel32.dll", "FindNextFileA", args, 2, &r) && r);
        args[0] = fh;
        CHECK(!invoke(&cpu, "kernel32.dll", "FindClose", args, 1, &r));
        CHECK(r == 1);
        CHECK(ngot == 3);

        /* Insertion sort newest-first using the guest CompareFileTime shim,
         * exactly the comparison the save menu performs. */
        for (unsigned i = 1; i < ngot; ++i) {
            struct found key = got[i];
            unsigned j = i;
            while (j > 0) {
                uint32_t fa = DATA + 0x5800, fb = DATA + 0x5808;
                CHECK(!guest_write(g_mem, fa, &key.ft, 8));
                CHECK(!guest_write(g_mem, fb, &got[j - 1].ft, 8));
                args[0] = fa; args[1] = fb;
                CHECK(!invoke(&cpu, "kernel32.dll", "CompareFileTime", args, 2, &r));
                if ((int32_t)r <= 0) break;
                got[j] = got[j - 1];
                --j;
            }
            got[j] = key;
        }
        for (unsigned i = 0; i < 3; ++i) {
            char want[32];
            snprintf(want, sizeof want, "zz_%s.ess",
                     i == 0 ? "new" : i == 1 ? "mid" : "old");
            if (strcmp(got[i].name, want) != 0)
                fprintf(stderr, "sort[%u]='%s' want '%s'\n", i, got[i].name, want);
            CHECK(!strcmp(got[i].name, want));
        }

        /* Extension matching must be case-insensitive and must not pick up
         * the .skse sidecar written above. */
        uint32_t upper = put_str(DATA + 0x4d00,
                                 "Documents\\My Games\\Skyrim\\Saves\\*.ESS");
        args[0] = upper; args[1] = fd;
        CHECK(!invoke(&cpu, "kernel32.dll", "FindFirstFileA", args, 2, &r));
        CHECK(r != UINT32_MAX);
        fh = r;
        unsigned ess_count = 0;
        do {
            const char *nm = guest_cstr(g_mem, fd + 44, 260);
            CHECK(strstr(nm, ".skse") == NULL);
            ++ess_count;
            args[0] = fh; args[1] = fd;
        } while (invoke(&cpu, "kernel32.dll", "FindNextFileA", args, 2, &r) && r);
        args[0] = fh;
        CHECK(!invoke(&cpu, "kernel32.dll", "FindClose", args, 1, &r));
        CHECK(ess_count == 4);   /* autosave.ess + zz_old/mid/new */
    }

    win32_shutdown();
    if (!failures) puts("PASS: win32_save_paths smoke");
done:
    guest_destroy(&m);
    return failures ? 1 : 0;
}
