/* Host check for the measured BSA/stream fields in the mem_stats snapshot.
 *
 * Read-only.  Opens the retail archives under `E:\harness_skyrim\Data`
 * (or TESV_DATA_ROOT / argv[1]), sums bsa_resident_stats() over every open
 * archive, builds a one-resource stream index against a real compressed record
 * and drives one prefetch + demand read through it.  The final checks assert:
 *   - the resident index (locator + tags + checkpoints + folders) stays at or
 *     below the 2 MiB profile line, with page and range caches accounted apart;
 *   - inflating a compressed record grows range_cache and the 8 MiB LRU bound
 *     is never exceeded;
 *   - queued/staged/physical/demand counters in StreamStatsSnapshot move after
 *     a prefetch and a demand hit, and the mem_stats line shows them.
 * Skips with exit 0 when no archives are present.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define FILE_EXISTS(p) (_access((p), 0) == 0)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define FILE_EXISTS(p) (access((p), F_OK) == 0)
#endif

#include "asset_stream.h"
#include "bsa_reader.h"
#include "mem_stats.h"

#define MAX_ARCHIVES 64u
#define INDEX_LIMIT (2u * 1024u * 1024u)
#define RANGE_LIMIT (8u * 1024u * 1024u)
#define PAGE_BYTES (32u * 1024u)

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

typedef struct {
    char name[256];
    char full[1024];
    uint64_t size;
} ArchiveFile;

static ArchiveFile g_files[MAX_ARCHIVES];
static unsigned g_count;

static uint64_t file_size(const char *path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return ok ? (uint64_t)sz.QuadPart : 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
#endif
}

static int cmp_name(const void *pa, const void *pb) {
    const ArchiveFile *a = (const ArchiveFile *)pa;
    const ArchiveFile *b = (const ArchiveFile *)pb;
    return strcmp(a->name, b->name);
}

static int list_archives(const char *dir) {
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s/*.bsa", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return 0;
    do {
        if (fd.attrib & _A_SUBDIR) continue;
        if (g_count >= MAX_ARCHIVES) break;
        ArchiveFile *f = &g_files[g_count];
        snprintf(f->name, sizeof f->name, "%s", fd.name);
        snprintf(f->full, sizeof f->full, "%s/%s", dir, fd.name);
        f->size = file_size(f->full);
        if (f->size) ++g_count;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcasecmp(e->d_name + len - 4, ".bsa")) continue;
        if (g_count >= MAX_ARCHIVES) break;
        ArchiveFile *f = &g_files[g_count];
        snprintf(f->name, sizeof f->name, "%s", e->d_name);
        snprintf(f->full, sizeof f->full, "%s/%s", dir, e->d_name);
        f->size = file_size(f->full);
        if (f->size) ++g_count;
    }
    closedir(d);
#endif
    qsort(g_files, g_count, sizeof g_files[0], cmp_name);
    return g_count;
}

/* ---- tiny little-endian writer for the synthetic one-line index ---------- */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

/* Build a schema-2 index with one archive and one resource.  `path` points
 * into the index pool (no NUL); `entry`/`unpacked` come from bsa_find(). */
static size_t build_index(uint8_t *blob, size_t cap, const char *archive_name,
                          uint64_t archive_bytes, uint32_t archive_files,
                          const char *path, const BsaEntry *entry,
                          uint32_t unpacked, int compressed) {
    size_t name_len = strlen(archive_name);
    size_t path_len = strlen(path);
    size_t total = STREAM_INDEX_HEADER_SIZE + STREAM_INDEX_ARCHIVE_ENTRY_SIZE +
                   STREAM_INDEX_RESOURCE_ENTRY_SIZE + name_len + 1u + path_len;
    total = (total + 7u) & ~(size_t)7u;
    total += 4u; /* empty cell table + CRC */
    if (total > cap) return 0;
    memset(blob, 0, total);
    size_t archive_off = STREAM_INDEX_HEADER_SIZE;
    size_t resource_off = archive_off + STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    size_t pool_off = resource_off + STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    size_t cell_off = (pool_off + name_len + 1u + path_len + 7u) & ~(size_t)7u;

    memcpy(blob, "SRNX", 4);
    put_u16(blob + 4, STREAM_INDEX_SCHEMA);
    put_u16(blob + 6, STREAM_INDEX_HEADER_SIZE);
    put_u16(blob + 8, STREAM_INDEX_ARCHIVE_ENTRY_SIZE);
    put_u16(blob + 10, STREAM_INDEX_RESOURCE_ENTRY_SIZE);
    put_u16(blob + 12, STREAM_INDEX_CELL_ENTRY_SIZE);
    put_u16(blob + 14, 0x0001);
    put_u32(blob + 16, 1);          /* archive count */
    put_u32(blob + 20, 1);          /* resource count */
    put_u32(blob + 24, 0);          /* cell count */
    put_u64(blob + 32, 0x4d454d535441u); /* generation */
    put_u64(blob + 40, archive_off);
    put_u64(blob + 48, resource_off);
    put_u64(blob + 56, pool_off);
    put_u64(blob + 64, cell_off);
    put_u64(blob + 72, total);

    uint8_t *a = blob + archive_off;
    put_u32(a + 0, 0);
    put_u32(a + 4, (uint32_t)name_len);
    put_u64(a + 8, archive_bytes);
    put_u32(a + 48, 0);
    put_u32(a + 52, 1);             /* rows for this archive */
    (void)archive_files;
    memcpy(blob + pool_off, archive_name, name_len);

    uint8_t *r = blob + resource_off;
    put_u32(r + 0, 0);              /* id must equal the record index */
    put_u32(r + 4, (uint32_t)(name_len + 1u));
    put_u32(r + 8, (uint32_t)path_len);
    put_u32(r + 12, 0);             /* lod / reserved */
    put_u64(r + 16, entry->offset);
    /* Validation compares physical + payload with the archive size; use the
     * stored extent.  A compressed record's unpacked size is resolved by
     * stream_mount_stage_extent() from the archive itself. */
    put_u32(r + 24, entry->stored_size);
    put_u32(r + 28, entry->stored_size);
    put_u32(r + 32, entry->offset);
    put_u32(r + 36, compressed ? STREAM_RESOURCE_FLAG_COMPRESSED : 0u);
    (void)unpacked;
    memcpy(blob + pool_off + name_len + 1u, path, path_len);

    put_u32(blob + total - 4, bsa_crc32(blob, total - 4));
    return total;
}

static const char *k_paths[] = {
    "interface/startmenu.swf",
    "interface/translate_english.txt",
    "strings/skyrim_english.strings",
    "meshes/architecture/farmhouse/inn01.nif",
    "meshes/clutter/common/barrel01.nif",
    "textures/architecture/farmhouse/inn01.dds",
    "sound/ambience/atmosphere/ambr_wind.wav",
    "scripts/actor.pex",
};

/* Find the first compressed archive + record that can be inflated into the
 * bounded LRU.  Returns the archive index and fills out the match/path. */
static int pick_compressed(BsaArchive *const *arch, BsaMatch *out,
                           char *path, size_t path_cap, uint32_t *unpacked) {
    for (unsigned i = 0; i < g_count; ++i) {
        if (!arch[i]) continue;
        const BsaHeader *h = bsa_header(arch[i]);
        if (!h || !h->compressed) continue;
        for (unsigned p = 0; p < sizeof k_paths / sizeof k_paths[0]; ++p) {
            BsaMatch m;
            char reason[BSA_REASON_MAX];
            if (bsa_find(arch[i], k_paths[p], &m) != BSA_OK) continue;
            uint32_t size = 0;
            if (bsa_unpacked_size(arch[i], &m.entry, &size, reason,
                                  sizeof reason) != BSA_OK)
                continue;
            if (!size || size > RANGE_LIMIT / 2u) continue;
            *out = m;
            *unpacked = size;
            snprintf(path, path_cap, "%s", k_paths[p]);
            return (int)i;
        }
    }
    return -1;
}

static void test_real_bsa(void) {
    BsaArchive *arch[MAX_ARCHIVES];
    memset(arch, 0, sizeof arch);
    unsigned opened = 0, compressed_archives = 0;
    BsaResidentStats sum = {0};
    uint64_t pages = 0;

    printf("%-28s %8s %10s %10s %10s %10s\n", "archive", "files", "index",
           "page", "range", "total");
    for (unsigned i = 0; i < g_count; ++i) {
        char reason[BSA_REASON_MAX] = "";
        BsaStatus st = bsa_open_path(g_files[i].full, NULL, &arch[i], reason,
                                     sizeof reason);
        if (st != BSA_OK) {
            fprintf(stderr, "mem_stats_bsa: skip %s: %s (%s)\n", g_files[i].name,
                    bsa_status_name(st), reason);
            continue;
        }
        if (bsa_header(arch[i])->compressed) ++compressed_archives;
        BsaResidentStats rs;
        bsa_resident_stats(arch[i], &rs);
        size_t once = bsa_resident_bytes(arch[i]);
        CHECK(rs.total == once);
        CHECK(rs.total == rs.index + rs.page_cache + rs.range_cache +
                              rs.overhead);
        sum.index += rs.index;
        sum.page_cache += rs.page_cache;
        sum.range_cache += rs.range_cache;
        sum.overhead += rs.overhead;
        sum.total += rs.total;
        pages += rs.page_cache;
        ++opened;
        printf("%-28s %8u %10llu %10llu %10llu %10llu\n", g_files[i].name,
               (unsigned)bsa_file_count(arch[i]),
               (unsigned long long)rs.index,
               (unsigned long long)rs.page_cache,
               (unsigned long long)rs.range_cache,
               (unsigned long long)rs.total);
    }

    printf("mem_stats_bsa: %u archive(s) open, %u compressed: index=%llu "
           "page=%llu range=%llu overhead=%llu resident=%llu bytes\n",
           opened, compressed_archives, (unsigned long long)sum.index,
           (unsigned long long)sum.page_cache,
           (unsigned long long)sum.range_cache,
           (unsigned long long)sum.overhead, (unsigned long long)sum.total);

    CHECK(opened > 0);
    CHECK(sum.index <= INDEX_LIMIT);
    CHECK(sum.page_cache <= (uint64_t)opened * PAGE_BYTES);
    CHECK(sum.page_cache == pages);
    CHECK(sum.range_cache == 0); /* no compressed reads yet */
    CHECK(sum.total == sum.index + sum.page_cache + sum.range_cache +
                           sum.overhead);

    /* Snapshot line carries the measured numbers, not estimates. */
    mem_stats_configure(120, 24, 4);
    mem_stats_set_bsa_detail(sum.total, sum.index, sum.page_cache,
                             sum.range_cache, g_count, opened);
    char line[3072];
    char expect[192];
    mem_stats_format(line, sizeof line, "real", NULL);
    snprintf(expect, sizeof expect, "bsa_resident=%llu",
             (unsigned long long)sum.total);
    CHECK(strstr(line, expect) != NULL);
    snprintf(expect, sizeof expect, "bsa_index=%llu",
             (unsigned long long)sum.index);
    CHECK(strstr(line, expect) != NULL);
    snprintf(expect, sizeof expect, "bsa_page_cache=%llu",
             (unsigned long long)sum.page_cache);
    CHECK(strstr(line, expect) != NULL);
    snprintf(expect, sizeof expect, "bsa_range_cache=%llu",
             (unsigned long long)sum.range_cache);
    CHECK(strstr(line, expect) != NULL);
    snprintf(expect, sizeof expect, "bsa_archives=%u", g_count);
    CHECK(strstr(line, expect) != NULL);
    snprintf(expect, sizeof expect, "bsa_mounted=%u", opened);
    CHECK(strstr(line, expect) != NULL);
    CHECK(strstr(line, "have_bsa=1") != NULL);

    /* ---- compressed range cache: grow, then stay inside the 8 MiB LRU ---- */
    BsaMatch m;
    char picked[STREAM_PATH_MAX] = "";
    uint32_t picked_size = 0;
    int ai = pick_compressed(arch, &m, picked, sizeof picked, &picked_size);
    if (ai >= 0) {
        /* The cooked index stores lowercase backslash paths. */
        char canon[STREAM_PATH_MAX];
        size_t cl = strlen(picked);
        if (cl >= sizeof canon) cl = sizeof canon - 1u;
        for (size_t i = 0; i < cl; ++i) {
            char c = picked[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            canon[i] = c == '/' ? '\\' : c;
        }
        canon[cl] = 0;
        BsaRangeCacheStats before, after, peak;
        bsa_range_cache_stats(arch[ai], &before);
        uint8_t buf[4096];
        uint32_t got = 0;
        char reason[BSA_REASON_MAX];
        BsaStatus st = bsa_read_range(arch[ai], &m.entry, 0, buf,
                                      sizeof buf < picked_size ? sizeof buf
                                                               : picked_size,
                                      &got, reason, sizeof reason);
        CHECK(st == BSA_OK && got > 0);
        bsa_range_cache_stats(arch[ai], &after);
        CHECK(after.misses > before.misses);
        CHECK(after.bytes > 0);
        printf("mem_stats_bsa: %s '%s': inflated %u B, range_cache %llu -> %llu "
               "bytes (hits=%llu misses=%llu evictions=%llu)\n",
               g_files[ai].name, picked, picked_size,
               (unsigned long long)before.bytes,
               (unsigned long long)after.bytes,
               (unsigned long long)after.hits,
               (unsigned long long)after.misses,
               (unsigned long long)after.evictions);

        /* Walk other records in the same archive to push the LRU; resident
         * bytes must never exceed the configured 8 MiB. */
        unsigned inflated = 1;
        for (unsigned i = 0; i < g_count && inflated < 24; ++i) {
            if (!arch[i]) continue;
            const BsaHeader *h = bsa_header(arch[i]);
            if (!h || !h->compressed) continue; /* only zlib records cache */
            for (unsigned p = 0; p < sizeof k_paths / sizeof k_paths[0] &&
                                 inflated < 24; ++p) {
                BsaMatch mm;
                if (bsa_find(arch[i], k_paths[p], &mm) != BSA_OK) continue;
                uint32_t size = 0;
                if (bsa_unpacked_size(arch[i], &mm.entry, &size, reason,
                                      sizeof reason) != BSA_OK ||
                    !size || size > RANGE_LIMIT / 2u)
                    continue;
                uint32_t n = size < sizeof buf ? size : (uint32_t)sizeof buf;
                got = 0;
                if (bsa_read_range(arch[i], &mm.entry, 0, buf, n, &got, reason,
                                   sizeof reason) == BSA_OK)
                    ++inflated;
            }
        }
        bsa_range_cache_stats(arch[ai], &peak);
        printf("mem_stats_bsa: after %u record(s): range_cache=%llu bytes "
               "(limit %u)\n", inflated, (unsigned long long)peak.bytes,
               (unsigned)RANGE_LIMIT);
        CHECK(peak.bytes <= (uint64_t)RANGE_LIMIT);

        /* ---- stream snapshot over that real record ------------------------ */
        uint8_t *blob = (uint8_t *)malloc(64u * 1024u);
        CHECK(blob != NULL);
        if (blob) {
            const char *arc_name = g_files[ai].name;
            size_t len = build_index(blob, 64u * 1024u, arc_name,
                                     g_files[ai].size,
                                     bsa_file_count(arch[ai]), canon, &m.entry,
                                     picked_size, 1);
            CHECK(len > 0);
            StreamIndex *index = NULL;
            char reason2[STREAM_REASON_MAX];
            StreamStatus sst = stream_index_parse(blob, len, &index, reason2,
                                                  sizeof reason2);
            CHECK(sst == STREAM_OK);
            if (sst != STREAM_OK)
                fprintf(stderr, "mem_stats_bsa: index parse: %s (%s)\n",
                        stream_status_name(sst), reason2);
            if (sst == STREAM_OK) {
                StreamMount *mount = NULL;
                const char *data_dir = "E:/harness_skyrim/Data";
                char dir[1024];
                if (g_files[0].full[0]) {
                    snprintf(dir, sizeof dir, "%s", g_files[0].full);
                    char *slash = strrchr(dir, '/');
                    if (slash) *slash = 0;
                    data_dir = dir;
                }
                sst = stream_mount_open(index, data_dir, &mount, reason2,
                                        sizeof reason2);
                CHECK(sst == STREAM_OK);
                if (sst == STREAM_OK) {
                    StreamStatsSnapshot cold, queued, done, demand;
                    stream_stats_snapshot(mount, &cold);
                    CHECK(cold.queued == 0 && cold.staged_bytes == 0 &&
                          cold.physical_reads == 0);

                    StreamResource res;
                    CHECK(stream_index_find_path(index, canon, &res, reason2,
                                                 sizeof reason2) == STREAM_OK);
                    StreamHandle h = 0;
                    sst = stream_mount_prefetch_ex(
                        mount, &res, 65536u, 0, STREAM_PRIO_P1, 0, &h, reason2,
                        sizeof reason2);
                    CHECK(sst == STREAM_OK && h != 0);
                    stream_stats_snapshot(mount, &queued);
                    CHECK(queued.queued >= 1u);
                    CHECK(queued.staged_bytes > 0);

                    uint32_t quanta = 0;
                    for (unsigned i = 0; i < 8; ++i) {
                        sst = stream_mount_pump(mount, 2, &quanta, reason2,
                                                sizeof reason2);
                        if (sst != STREAM_OK || quanta == 0) break;
                    }
                    stream_stats_snapshot(mount, &done);
                    CHECK(done.physical_reads > queued.physical_reads);
                    CHECK(done.physical_bytes > 0);
                    CHECK(done.staged_bytes > 0);

                    uint8_t out[1024];
                    uint32_t got2 = 0;
                    int served = 0;
                    sst = stream_mount_demand_path(
                        mount, canon, 0, out, sizeof out, &got2, &served,
                        reason2, sizeof reason2);
                    CHECK(sst == STREAM_OK || sst == STREAM_ERR_UNSUPPORTED);
                    stream_stats_snapshot(mount, &demand);
                    if (served) {
                        CHECK(demand.demand_hits > done.demand_hits);
                        CHECK(demand.demand_bytes > 0);
                        CHECK(demand.prefetch_demand_hits > 0);
                    }
                    printf("mem_stats_bsa: stream queued=%u staged=%u "
                           "reads=%llu bytes=%llu misses=%llu demand_hits=%llu "
                           "prefetch_demand_hits=%llu cancelled=%llu "
                           "budget_denied=%llu\n",
                           demand.queued, demand.staged_bytes,
                           (unsigned long long)demand.physical_reads,
                           (unsigned long long)demand.physical_bytes,
                           (unsigned long long)demand.physical_misses,
                           (unsigned long long)demand.demand_hits,
                           (unsigned long long)demand.prefetch_demand_hits,
                           (unsigned long long)demand.cancelled,
                           (unsigned long long)demand.budget_denied);

                    mem_stats_set_stream(
                        demand.queued, demand.staged_bytes,
                        demand.physical_reads, demand.physical_bytes,
                        demand.physical_misses, demand.demand_hits,
                        demand.demand_bytes, demand.prefetch_demand_hits,
                        demand.cancelled, demand.budget_denied);
                    mem_stats_format(line, sizeof line, "stream", NULL);
                    snprintf(expect, sizeof expect, "stream_reads=%llu",
                             (unsigned long long)demand.physical_reads);
                    CHECK(strstr(line, expect) != NULL);
                    snprintf(expect, sizeof expect, "stream_staged=%u",
                             demand.staged_bytes);
                    CHECK(strstr(line, expect) != NULL);
                    snprintf(expect, sizeof expect, "stream_demand_hits=%llu",
                             (unsigned long long)demand.demand_hits);
                    CHECK(strstr(line, expect) != NULL);
                    stream_mount_close(mount);
                }
            }
            free(index);
            free(blob);
        }
    } else {
        printf("mem_stats_bsa: no compressed record matched the probe list; "
               "range/stream checks skipped\n");
    }

    for (unsigned i = 0; i < g_count; ++i)
        if (arch[i]) bsa_close(arch[i]);
}

int main(int argc, char **argv) {
    const char *data = argc > 1 ? argv[1] : getenv("TESV_DATA_ROOT");
    if (!data || !*data) data = getenv("SKYRIM_DATA");
    if (!data || !*data) data = "E:/harness_skyrim/Data";
    if (!list_archives(data)) {
        printf("mem_stats_bsa: skip (no *.bsa under %s)\n", data);
        return 0;
    }
    printf("mem_stats_bsa: %u archive(s) under %s\n", g_count, data);
    test_real_bsa();
    if (failures) {
        fprintf(stderr, "mem_stats_bsa: %d failure(s)\n", failures);
        return 1;
    }
    printf("mem_stats_bsa: ok\n");
    return 0;
}
