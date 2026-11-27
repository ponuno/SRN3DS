/* Read-only Data\... BSA overlay regression test.
 *
 * The synthetic root contains:
 *   Data\Test.bsa         stored   strings\x.strings + strings\y.strings
 *   Data\TestCooked.bsa   zlib     meshes\x.nif
 *   Data\BrokenCooked.bsa garbage  (must be named and skipped)
 *   Data\Strings\X.strings         loose (must win over the archive)
 *   Skyrim.ini            [Archive] sResourceArchiveList=Test.bsa
 *
 * GetFileAttributesExA must report FILE_ATTRIBUTE_ARCHIVE with the real size,
 * and CreateFileA/ReadFile must return exactly that many bytes.  The compressed
 * archive exercises the compressed range path (inflate once, bounded LRU);
 * the overlay no longer needs the whole-record bsa_read() fallback.  When a
 * real E:\harness_skyrim install is present, the same path is checked for
 * Skyrim_English.STRINGS plus one NIF and one DDS.
 */
#include "win32_shims.h"
#include "n3ds_lift_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define DATA 0x10000000u
#define DATA_SIZE 0x800000u
#define STACK 0x18000000u
#define STACK_SIZE 0x100000u
#define RETURN_MARKER 0xfeedbeefu
#define EAX_SENTINEL 0xabcdef01u

#define S_IN 0x1000u
#define S_OUT 0x2000u
#define BUF0 0x40000u
#define BUF1 0x100000u
#define BUF2 0x600000u

static GuestMemory *g_mem;
static GuestMemory g_memory;
static int g_bound;
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
static uint32_t rd32(uint32_t address) {
    uint32_t v = 0;
    CHECK(!guest_read32(g_mem, address, &v));
    return v;
}
static uint64_t rd64(uint32_t address) {
    uint64_t v = 0;
    CHECK(!guest_read(g_mem, address, &v, sizeof v));
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
    c->r[0] = EAX_SENTINEL;
    entry->fn(c);
    CHECK(c->r[4] == sp + 4u + 4u * count);
    *result = c->r[0];
    c->r[4] = sp;
    return 0;
}

static int get_attrs(CPU *c, const char *path, uint32_t *attr, uint64_t *size) {
    uint32_t args[3], r = 0;
    args[0] = put_str(DATA + S_IN, path);
    args[1] = 0;
    args[2] = DATA + S_OUT;
    if (invoke(c, "kernel32.dll", "GetFileAttributesExA", args, 3, &r) || !r)
        return 0;
    if (attr) *attr = rd32(DATA + S_OUT);
    if (size)
        *size = ((uint64_t)rd32(DATA + S_OUT + 28u) << 32) |
                rd32(DATA + S_OUT + 32u);
    return 1;
}

static uint32_t open_read(CPU *c, const char *path) {
    uint32_t args[7], r = 0;
    args[0] = put_str(DATA + S_IN, path);
    args[1] = 0x80000000u; /* GENERIC_READ */
    args[2] = 1;           /* FILE_SHARE_READ */
    args[3] = 0;
    args[4] = 3;           /* OPEN_EXISTING */
    args[5] = 0;
    args[6] = 0;
    if (invoke(c, "kernel32.dll", "CreateFileA", args, 7, &r)) return 0;
    return r;
}

static uint32_t read_file(CPU *c, uint32_t h, uint32_t dst, uint32_t n) {
    uint32_t args[5], r = 0;
    args[0] = h;
    args[1] = dst;
    args[2] = n;
    args[3] = DATA + S_OUT;
    args[4] = 0;
    if (invoke(c, "kernel32.dll", "ReadFile", args, 5, &r) || !r)
        return UINT32_MAX;
    return rd32(DATA + S_OUT);
}

static void close_handle(CPU *c, uint32_t h) {
    uint32_t args[1] = {h}, r = 0;
    CHECK(!invoke(c, "kernel32.dll", "CloseHandle", args, 1, &r) && r);
}

/* ---- minimal BSA v104 writer (stored or one zlib stored-block stream) ---- */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}
static int write_zlib_stored(FILE *f, const uint8_t *p, size_t n) {
    uint8_t hdr[2] = {0x78, 0x01};
    if (fwrite(hdr, 1, 2, f) != 2) return -1;
    size_t left = n;
    do {
        size_t chunk = left > 65535u ? 65535u : left;
        int final = chunk == left;
        uint8_t b[5];
        b[0] = final ? 1 : 0;
        b[1] = (uint8_t)chunk; b[2] = (uint8_t)(chunk >> 8);
        b[3] = (uint8_t)~chunk; b[4] = (uint8_t)(~chunk >> 8);
        if (fwrite(b, 1, 5, f) != 5) return -1;
        if (chunk && fwrite(p, 1, chunk, f) != chunk) return -1;
        p += chunk;
        left -= chunk;
    } while (left);
    uint32_t ad = adler32(p - n, n);
    uint8_t tail[4] = {(uint8_t)(ad >> 24), (uint8_t)(ad >> 16),
                       (uint8_t)(ad >> 8), (uint8_t)ad};
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

typedef struct {
    const char *path; /* "folder\\name" */
    const uint8_t *data;
    uint32_t len;
} BsaItem;

typedef struct {
    const char *path;
    uint8_t *data;
    uint32_t len;
    uint32_t offset;
    uint32_t stored;
} PendingItem;

static int bsa_write(const char *file, const BsaItem *items, unsigned n,
                     int compressed) {
    PendingItem p[8];
    if (n > 8) return -1;
    for (unsigned i = 0; i < n; ++i) {
        p[i].path = items[i].path;
        p[i].data = (uint8_t *)items[i].data;
        p[i].len = items[i].len;
        p[i].stored = items[i].len;
    }
    size_t names = 0;
    unsigned folders = 0;
    const char *prev = NULL;
    size_t prev_len = 0;
    for (unsigned i = 0; i < n; ++i) {
        const char *sep = strrchr(p[i].path, '\\');
        size_t flen = sep ? (size_t)(sep - p[i].path) : 0;
        if (!prev || prev_len != flen || memcmp(prev, p[i].path, flen)) {
            ++folders;
            prev = p[i].path;
            prev_len = flen;
        }
        names += strlen(sep ? sep + 1 : p[i].path) + 1u;
    }
    uint32_t dir = 36u + 16u * folders;
    for (unsigned i = 0; i < n;) {
        const char *sep = strrchr(p[i].path, '\\');
        size_t flen = sep ? (size_t)(sep - p[i].path) : 0;
        unsigned j = i;
        while (j < n) {
            const char *s2 = strrchr(p[j].path, '\\');
            size_t f2 = s2 ? (size_t)(s2 - p[j].path) : 0;
            if (f2 != flen || memcmp(p[j].path, p[i].path, flen)) break;
            ++j;
        }
        dir += 1u + (uint32_t)flen + 1u + 16u * (j - i);
        i = j;
    }
    uint32_t cursor = dir + (uint32_t)names;
    for (unsigned i = 0; i < n; ++i) {
        p[i].offset = cursor;
        uint32_t stored = p[i].len;
        if (compressed) {
            size_t left = p[i].len;
            stored = 4u + 2u + 4u;
            do {
                size_t chunk = left > 65535u ? 65535u : left;
                stored += 5u + (uint32_t)chunk;
                left -= chunk;
            } while (left);
        }
        p[i].stored = stored;
        cursor += stored;
    }

    FILE *f = fopen(file, "wb");
    if (!f) return -1;
    uint8_t h[36];
    memset(h, 0, sizeof h);
    memcpy(h, "BSA\0", 4);
    put_u32(h + 4, 104);
    put_u32(h + 8, 36);
    put_u32(h + 12, 0x001u | 0x002u | (compressed ? 0x004u : 0));
    put_u32(h + 16, folders);
    put_u32(h + 20, n);
    uint32_t total_folder = 0;
    for (unsigned i = 0; i < n;) {
        const char *sep = strrchr(p[i].path, '\\');
        size_t flen = sep ? (size_t)(sep - p[i].path) : 0;
        uint32_t count = 0;
        unsigned j = i;
        while (j < n) {
            const char *s2 = strrchr(p[j].path, '\\');
            size_t f2 = s2 ? (size_t)(s2 - p[j].path) : 0;
            if (f2 != flen || memcmp(p[j].path, p[i].path, flen)) break;
            ++j; ++count;
        }
        total_folder += (uint32_t)flen + 1u;
        i = j;
    }
    put_u32(h + 24, total_folder);
    put_u32(h + 28, (uint32_t)names);
    put_u32(h + 32, 0);
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h;
    for (unsigned i = 0; ok && i < n;) {
        const char *sep = strrchr(p[i].path, '\\');
        size_t flen = sep ? (size_t)(sep - p[i].path) : 0;
        unsigned j = i;
        while (j < n) {
            const char *s2 = strrchr(p[j].path, '\\');
            size_t f2 = s2 ? (size_t)(s2 - p[j].path) : 0;
            if (f2 != flen || memcmp(p[j].path, p[i].path, flen)) break;
            ++j;
        }
        uint8_t rec[16] = {0};
        put_u32(rec + 8, j - i);
        ok = fwrite(rec, 1, 16, f) == 16;
        i = j;
    }
    for (unsigned i = 0; ok && i < n;) {
        const char *sep = strrchr(p[i].path, '\\');
        size_t flen = sep ? (size_t)(sep - p[i].path) : 0;
        unsigned j = i;
        while (j < n) {
            const char *s2 = strrchr(p[j].path, '\\');
            size_t f2 = s2 ? (size_t)(s2 - p[j].path) : 0;
            if (f2 != flen || memcmp(p[j].path, p[i].path, flen)) break;
            ++j;
        }
        uint8_t len = (uint8_t)(flen + 1u);
        ok = fwrite(&len, 1, 1, f) == 1;
        if (ok && flen) ok = fwrite(p[i].path, 1, flen, f) == flen;
        uint8_t nul = 0;
        if (ok) ok = fwrite(&nul, 1, 1, f) == 1;
        for (unsigned k = i; ok && k < j; ++k) {
            uint8_t rec[16] = {0};
            put_u32(rec + 8, p[k].stored);
            put_u32(rec + 12, p[k].offset);
            ok = fwrite(rec, 1, 16, f) == 16;
        }
        i = j;
    }
    for (unsigned i = 0; ok && i < n; ++i) {
        const char *sep = strrchr(p[i].path, '\\');
        const char *name = sep ? sep + 1 : p[i].path;
        size_t len = strlen(name) + 1u;
        ok = fwrite(name, 1, len, f) == len;
    }
    for (unsigned i = 0; ok && i < n; ++i) {
        if (compressed) {
            uint8_t sz[4];
            put_u32(sz, p[i].len);
            ok = fwrite(sz, 1, 4, f) == 4 &&
                 write_zlib_stored(f, p[i].data, p[i].len) == 0;
        } else {
            ok = p[i].len == 0 || fwrite(p[i].data, 1, p[i].len, f) == p[i].len;
        }
    }
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}
static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = n == 0 || fwrite(data, 1, n, f) == n;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}
static uint64_t host_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (uint64_t)n : 0;
}
static void env_set(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static const char k_strings[] =
    "STRINGS synthetic overlay payload";
static const char k_nif[] = "Gamebryo File Format, Version 20.0.0.5\n";

/* The shims read their arguments through n3ds_guest_ptr(), whose current
 * GuestMemory is installed by the lift runtime.  Bind one inert function so
 * the test exercises the real shim bodies rather than a second stub path. */
static void test_noop(CPU *c) { (void)c; }
static const FnEntry k_noop_table[] = {{0x1000u, test_noop}};

static void test_root(void) {
    make_dir("bsa_overlay_root");
    make_dir("bsa_overlay_root/Data");
    make_dir("bsa_overlay_root/Data/Strings");
    make_dir("bsa_overlay_root/Data/meshes");
    make_dir("bsa_overlay_root/Data/textures");

    BsaItem items[2] = {
        {"strings\\x.strings", (const uint8_t *)k_strings,
         (uint32_t)strlen(k_strings)},
        {"strings\\y.strings", (const uint8_t *)k_strings,
         (uint32_t)strlen(k_strings)}
    };
    CHECK(bsa_write("bsa_overlay_root/Data/Test.bsa", items, 2, 0) == 0);
    BsaItem nif = {"meshes\\x.nif", (const uint8_t *)k_nif,
                   (uint32_t)strlen(k_nif)};
    CHECK(bsa_write("bsa_overlay_root/Data/TestCooked.bsa", &nif, 1, 1) == 0);
    CHECK(write_file("bsa_overlay_root/Data/BrokenCooked.bsa", "not a bsa", 9) == 0);
    CHECK(write_file("bsa_overlay_root/Data/Strings/X.strings", "LOOSE", 5) == 0);
    const char ini[] =
        "[General]\n"
        "sIntroSequence=\n"
        "[Archive]\n"
        "sResourceArchiveList=Test.bsa\n";
    CHECK(write_file("bsa_overlay_root/Skyrim.ini", ini, sizeof ini - 1) == 0);

    GuestMemory m;
    guest_init(&m, 24u << 20);
    CHECK(!guest_map(&m, DATA, DATA_SIZE));
    CHECK(!guest_map(&m, STACK, STACK_SIZE));
    g_memory = m;
    g_mem = &g_memory;
    if (!g_bound) {
        CHECK(!n3ds_lift_bind(&g_memory, k_noop_table, 1));
        g_bound = 1;
    }
    CHECK(win32_init(&g_memory, 4u << 20, "bsa_overlay_root") == 0);

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 256u;

    uint32_t attr = 0;
    uint64_t size = 0;
    CHECK(get_attrs(&cpu, "Data\\Strings\\Y.strings", &attr, &size));
    CHECK(attr == 0x20u);
    CHECK(size == strlen(k_strings));

    uint32_t h = open_read(&cpu, "Data\\Strings\\Y.strings");
    CHECK(h != 0 && h != UINT32_MAX);
    uint32_t args[5], r = 0;
    args[0] = h; args[1] = DATA + S_OUT;
    CHECK(!invoke(&cpu, "kernel32.dll", "GetFileSizeEx", args, 2, &r) && r);
    CHECK(rd64(DATA + S_OUT) == (uint64_t)strlen(k_strings));
    CHECK(read_file(&cpu, h, DATA + BUF0, (uint32_t)strlen(k_strings)) ==
          strlen(k_strings));
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 16), k_strings, 16));
    CHECK(read_file(&cpu, h, DATA + BUF0, 8) == 0);
    args[0] = h; args[1] = 3; args[2] = 0; args[3] = DATA + S_OUT; args[4] = 0;
    CHECK(!invoke(&cpu, "kernel32.dll", "SetFilePointerEx", args, 5, &r) && r);
    CHECK(rd64(DATA + S_OUT) == 3);
    CHECK(read_file(&cpu, h, DATA + BUF0, 4) == 4);
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 4), k_strings + 3, 4));
    close_handle(&cpu, h);

    /* Compressed entry: attrs size, then the range-read path (the reader
     * inflates once into its bounded LRU; no whole-record fallback). */
    CHECK(get_attrs(&cpu, "Data\\meshes\\X.nif", &attr, &size));
    CHECK(attr == 0x20u && size == strlen(k_nif));
    h = open_read(&cpu, "Data\\meshes\\X.nif");
    CHECK(h != 0 && h != UINT32_MAX);
    CHECK(read_file(&cpu, h, DATA + BUF0, (uint32_t)strlen(k_nif)) ==
          strlen(k_nif));
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 20), k_nif, 20));
    close_handle(&cpu, h);

    /* Loose files must win over the archive by default. */
    CHECK(get_attrs(&cpu, "Data\\Strings\\X.strings", &attr, &size));
    CHECK(size == 5);
    h = open_read(&cpu, "Data\\Strings\\X.strings");
    CHECK(h != 0 && h != UINT32_MAX);
    CHECK(read_file(&cpu, h, DATA + BUF0, 8) == 5);
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 5), "LOOSE", 5));
    close_handle(&cpu, h);

    /* A missing path still fails; the broken archive is skipped, not fatal. */
    CHECK(!get_attrs(&cpu, "Data\\Strings\\Missing.strings", NULL, NULL));
    CHECK(open_read(&cpu, "Data\\Strings\\Missing.strings") == UINT32_MAX);

    Win32BsaOverlayStats st;
    win32_bsa_overlay_stats(&st);
    CHECK(st.archives >= 3u);
    CHECK(st.archives_failed >= 1u);
    CHECK(st.stat_resolves >= 2u);
    CHECK(st.open_resolves >= 2u);
    CHECK(st.range_reads >= 3u);
    CHECK(st.full_reads == 0u);
    CHECK(st.bytes_read >= strlen(k_nif));
    CHECK(st.resident_archives >= 1u);
    CHECK(st.index_bytes > 0u);
    CHECK(st.page_cache_bytes > 0u);
    CHECK(st.resident_bytes >= st.index_bytes + st.page_cache_bytes +
                                st.range_cache_bytes);
    CHECK(st.resident_bytes <= st.index_bytes + st.page_cache_bytes +
                                st.range_cache_bytes +
                                (uint64_t)st.resident_archives * 2048u);
    {
        uint64_t per_archive = 0;
        for (unsigned i = 0; i < WIN32_BSA_OVERLAY_MAX_ARCHIVES; ++i)
            per_archive += st.archive_resident[i];
        CHECK(per_archive == st.resident_bytes);
    }
    if (failures)
        fprintf(stderr, "overlay stats: archives=%u failed=%u stat=%llu open=%llu range=%llu full=%llu bytes=%llu\n",
                st.archives, st.archives_failed,
                (unsigned long long)st.stat_resolves,
                (unsigned long long)st.open_resolves,
                (unsigned long long)st.range_reads,
                (unsigned long long)st.full_reads,
                (unsigned long long)st.bytes_read);

    win32_shutdown();
}

static void test_real_install(void) {
#ifdef _WIN32
    const char *root = "E:/harness_skyrim";
    if (!host_file_size("E:/harness_skyrim/Data/Skyrim - Interface.bsa")) {
        fprintf(stderr, "[bsa-overlay] real install absent, skipping\n");
        return;
    }
    /* The harness ships loose Strings too; force the archive so this check
     * proves the BSA path, not the loose copy. */
    env_set("TESV_BSA_OVERLAY_ALL", "1");
    env_set("TESV_BSA_OVERLAY_FIRST", "1");

    g_mem = &g_memory;
    CHECK(win32_init(&g_memory, 8u << 20, root) == 0);
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK + STACK_SIZE - 256u;

    uint32_t attr = 0;
    uint64_t size = 0;
    CHECK(get_attrs(&cpu, "Data\\Strings\\Skyrim_English.STRINGS", &attr,
                    &size));
    CHECK(attr == 0x20u && size > 800000u);
    uint32_t h = open_read(&cpu, "Data\\Strings\\Skyrim_English.STRINGS");
    CHECK(h != 0 && h != UINT32_MAX);
    CHECK(read_file(&cpu, h, DATA + BUF0, (uint32_t)size) == size);
    close_handle(&cpu, h);

    CHECK(get_attrs(&cpu,
                    "Data\\meshes\\dungeons\\nordic\\levers\\pullchainanim\\norpullchainanim01.nif",
                    &attr, &size));
    CHECK(attr == 0x20u && size > 100000u);
    h = open_read(&cpu,
                  "Data\\meshes\\dungeons\\nordic\\levers\\pullchainanim\\norpullchainanim01.nif");
    CHECK(h != 0 && h != UINT32_MAX);
    CHECK(read_file(&cpu, h, DATA + BUF0, 20) == 20);
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 11), "Gamebryo Fi", 11));
    close_handle(&cpu, h);

    CHECK(get_attrs(&cpu, "Data\\textures\\actors\\dog\\dog.dds", &attr,
                    &size));
    CHECK(attr == 0x20u && size > 128u);
    h = open_read(&cpu, "Data\\textures\\actors\\dog\\dog.dds");
    CHECK(h != 0 && h != UINT32_MAX);
    CHECK(read_file(&cpu, h, DATA + BUF0, 4) == 4);
    CHECK(!memcmp(guest_ptr(g_mem, DATA + BUF0, 4), "DDS ", 4));
    close_handle(&cpu, h);

    Win32BsaOverlayStats st;
    win32_bsa_overlay_stats(&st);
    CHECK(st.archives >= 10u);
    CHECK(st.open_resolves >= 3u);
    CHECK(st.bytes_read >= 800000u);
    CHECK(st.resident_archives >= 1u);
    CHECK(st.index_bytes > 0u);
    /* The nif/DDS reads above go through the compressed range cache. */
    CHECK(st.range_cache_bytes > 0u);
    CHECK(st.resident_bytes >= st.index_bytes + st.page_cache_bytes +
                                st.range_cache_bytes);
    CHECK(st.resident_bytes <= st.index_bytes + st.page_cache_bytes +
                                st.range_cache_bytes +
                                (uint64_t)st.resident_archives * 2048u);
    CHECK(st.index_bytes <= 2u * 1024u * 1024u);
    printf("win32_bsa_overlay: resident=%llu index=%llu page=%llu range=%llu "
           "(%u open archive(s))\n",
           (unsigned long long)st.resident_bytes,
           (unsigned long long)st.index_bytes,
           (unsigned long long)st.page_cache_bytes,
           (unsigned long long)st.range_cache_bytes, st.resident_archives);
    win32_shutdown();
    env_set("TESV_BSA_OVERLAY_ALL", "0");
    env_set("TESV_BSA_OVERLAY_FIRST", "0");
#endif
}

int main(void) {
    test_root();
    test_real_install();
    if (failures) {
        fprintf(stderr, "win32_bsa_overlay: %u failure(s)\n", failures);
        return 1;
    }
    printf("win32_bsa_overlay: ok\n");
    return 0;
}
