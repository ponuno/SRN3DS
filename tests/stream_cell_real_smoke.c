/* Real-index test: RiverwoodSleepingGiantInn through the BSA overlay.
 *
 * Skipped unless the first argument (or STREAM_INDEX_REAL) names an index built
 * by tools/stream_index_build.py and a Skyrim root is available (second
 * argument or SKYRIM_ROOT, default E:/SteamLibrary/steamapps/common/Skyrim).
 *
 * For every resource id of cell 0x133C6 this drives the actual guest shims:
 * GetFileAttributesExA must report the archive entry's unpacked size (> 0) and
 * CreateFileA/ReadFile must return bytes.  This is the overlay path the engine
 * uses; asset_stream itself does not read retail compressed entries.
 */
#include "asset_stream.h"
#include "win32_shims.h"
#include "n3ds_lift_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define DATA 0x10000000u
#define DATA_SIZE 0x800000u
#define STACK 0x18000000u
#define STACK_SIZE 0x100000u
#define RETURN_MARKER 0xfeedbeefu
#define S_IN 0x1000u
#define S_OUT 0x2000u
#define BUF0 0x40000u

static GuestMemory g_memory;
static GuestMemory *g_mem;
static int failures;
static int checks;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
    ++checks; \
} while (0)

static void noop(CPU *c) { (void)c; }
static const FnEntry k_noop_table[] = {{0x30000000u, noop}};

static uint32_t put_str(uint32_t address, const char *text) {
    CHECK(!guest_write(g_mem, address, text, strlen(text) + 1u));
    return address;
}
static uint32_t rd32(uint32_t address) {
    uint32_t v = 0;
    CHECK(!guest_read32(g_mem, address, &v));
    return v;
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
    c->r[0] = 0xabcdef01u;
    entry->fn(c);
    if (c->r[4] != sp + 4u + 4u * count) {
        c->r[4] = sp;
        return -1;
    }
    *result = c->r[0];
    c->r[4] = sp;
    return 0;
}

static int get_attrs(CPU *c, const char *path, uint64_t *size) {
    uint32_t args[3] = {put_str(DATA + S_IN, path), 0, DATA + S_OUT};
    uint32_t r = 0;
    if (invoke(c, "kernel32.dll", "GetFileAttributesExA", args, 3, &r) || !r)
        return 0;
    *size = ((uint64_t)rd32(DATA + S_OUT + 28u) << 32) | rd32(DATA + S_OUT + 32u);
    return 1;
}

static uint32_t open_read(CPU *c, const char *path) {
    uint32_t args[7] = {
        put_str(DATA + S_IN, path), 0x80000000u, 1, 0, 3, 0, 0
    };
    uint32_t r = 0;
    if (invoke(c, "kernel32.dll", "CreateFileA", args, 7, &r)) return 0;
    return r;
}

static uint32_t read_file(CPU *c, uint32_t h, uint32_t n) {
    uint32_t args[5] = {h, DATA + BUF0, n, DATA + S_OUT, 0};
    uint32_t r = 0;
    if (invoke(c, "kernel32.dll", "ReadFile", args, 5, &r) || !r)
        return UINT32_MAX;
    return rd32(DATA + S_OUT);
}

static void close_handle(CPU *c, uint32_t h) {
    uint32_t args[1] = {h}, r = 0;
    CHECK(!invoke(c, "kernel32.dll", "CloseHandle", args, 1, &r) && r);
}

int main(int argc, char **argv) {
    const char *index_path = argc > 1 ? argv[1] : getenv("STREAM_INDEX_REAL");
    const char *root = argc > 2 ? argv[2]
                                : getenv("SKYRIM_ROOT");
    if (!root || !*root) root = "E:/SteamLibrary/steamapps/common/Skyrim";
    if (!index_path || !*index_path) {
        printf("stream_cell_real: skipped (set STREAM_INDEX_REAL)\n");
        return 0;
    }

    char reason[STREAM_REASON_MAX];
    StreamIndex *index = NULL;
    StreamStatus st = stream_index_load(index_path, &index, reason,
                                        sizeof reason);
    if (st != STREAM_OK) {
        fprintf(stderr, "stream_cell_real: index: %s (%s)\n",
                stream_status_name(st), reason);
        return 1;
    }
    const StreamIndexInfo *info = stream_index_info(index);
    printf("stream_cell_real: cells=%u resources=%u archives=%u\n",
           info->cell_count, info->resource_count, info->archive_count);

    StreamCellKey key;
    memset(&key, 0, sizeof key);
    key.kind = 0;
    key.form_id = 0x133C6u;
    StreamCellRow row;
    st = stream_index_find_cell(index, &key, &row, reason, sizeof reason);
    CHECK(st == STREAM_OK);
    CHECK(row.resource_count > 0);
    printf("stream_cell_real: RiverwoodSleepingGiantInn row has %u id(s)\n",
           row.resource_count);

#ifdef _WIN32
    _putenv_s("TESV_BSA_OVERLAY_ALL", "1");
#else
    setenv("TESV_BSA_OVERLAY_ALL", "1", 1);
#endif
    memset(&g_memory, 0, sizeof g_memory);
    guest_init(&g_memory, 64u << 20);
    g_mem = &g_memory;
    CHECK(!guest_map(&g_memory, DATA, DATA_SIZE));
    CHECK(!guest_map(&g_memory, STACK, STACK_SIZE));
    CHECK(!n3ds_lift_bind(&g_memory, k_noop_table, 1));
    if (win32_init(&g_memory, 8u << 20, root) != 0) {
        fprintf(stderr, "stream_cell_real: win32_init failed for %s\n", root);
        return 1;
    }
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 256u;

    uint32_t resolved = 0;
    for (uint32_t i = 0; i < row.resource_count; ++i) {
        StreamResource res;
        if (stream_index_find_id(index, row.resource_id[i], &res, reason,
                                 sizeof reason) != STREAM_OK)
            continue;
        char path[STREAM_PATH_MAX + 8];
        int head = snprintf(path, sizeof path, "Data\\");
        if (head <= 0 || (size_t)head + res.path_length >= sizeof path)
            continue;
        memcpy(path + head, res.path, res.path_length);
        path[head + res.path_length] = 0;
        for (char *p = path; *p; ++p)
            if (*p == '/') *p = '\\';
        uint64_t size = 0;
        CHECK(get_attrs(&cpu, path, &size));
        CHECK(size > 0);
        uint32_t h = open_read(&cpu, path);
        CHECK(h != 0 && h != UINT32_MAX);
        if (h && h != UINT32_MAX) {
            uint32_t got = read_file(&cpu, h, 4096);
            CHECK(got != UINT32_MAX && got > 0);
            close_handle(&cpu, h);
        }
        ++resolved;
        printf("  %-64.*s %llu bytes\n", (int)res.path_length, res.path,
               (unsigned long long)size);
    }
    CHECK(resolved == row.resource_count);

    Win32BsaOverlayStats stats;
    win32_bsa_overlay_stats(&stats);
    printf("stream_cell_real: overlay resolves=%u bytes=%llu\n",
           stats.stat_resolves, (unsigned long long)stats.bytes_read);
    CHECK(stats.stat_resolves > 0);
    stream_index_free(index);
    win32_shutdown();
    guest_destroy(&g_memory);
    printf("stream_cell_real: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
