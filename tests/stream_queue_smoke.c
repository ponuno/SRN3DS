/* Host test: phase-2 prefetch queue on a synthetic stream index and BSA.
 *
 * Deterministic model: an artificial monotonic clock, a 10 MB/s byte budget,
 * 64 KiB quanta, two buffers, demand/P0/P1/P2 ordering, dedup, cancellation,
 * deadlines, verification, LRU eviction, the 256-descriptor ceiling and
 * alloc/free balance.  The archive is generated in this file; every copy is
 * compared with the original payload bytes.
 */

#include "asset_stream.h"
#include "bsa_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* Synthetic archive and index                                         */

#define BIG_BYTES 300000u
#define MED_BYTES 100000u
#define SMALL_BYTES 50000u
#define TINY_COUNT 260u
#define FILE_COUNT (4u + TINY_COUNT)
#define FILE_PATH_MAX 64u

typedef struct {
    char path[FILE_PATH_MAX];
    uint8_t *data;
    uint32_t len;
} TestFile;

static TestFile g_files[FILE_COUNT];
static uint32_t g_offsets[FILE_COUNT];
static uint8_t g_archive[4u * 1024u * 1024u];
static uint8_t g_index[256u * 1024u];
static uint8_t g_payload_big[BIG_BYTES];
static uint8_t g_payload_med[MED_BYTES];
static uint8_t g_payload_small[SMALL_BYTES];
static uint8_t g_payload_tiny[TINY_COUNT];

static const char *const g_big_path = "meshes\\a000.bin";
static const char *const g_med_path = "meshes\\a001.bin";
static const char *const g_small_path = "meshes\\a002.bin";
static const char *const g_other_path = "meshes\\a003.bin";
static const char *const g_prefix = "meshes\\";

static int file_compare(const void *a, const void *b) {
    const TestFile *fa = (const TestFile *)a;
    const TestFile *fb = (const TestFile *)b;
    return strcmp(fa->path, fb->path);
}

static void fill_payloads(void) {
    for (uint32_t i = 0; i < BIG_BYTES; ++i)
        g_payload_big[i] = (uint8_t)(i * 31u + (i >> 8));
    for (uint32_t i = 0; i < MED_BYTES; ++i)
        g_payload_med[i] = (uint8_t)(i * 17u + 9u);
    for (uint32_t i = 0; i < SMALL_BYTES; ++i)
        g_payload_small[i] = (uint8_t)((i * 13u) ^ (i >> 3));
    for (uint32_t i = 0; i < TINY_COUNT; ++i)
        g_payload_tiny[i] = (uint8_t)(0x40u + i);
}

static void build_file_list(void) {
    uint32_t n = 0;
    TestFile *f;
    f = &g_files[n++];
    snprintf(f->path, sizeof f->path, "%s", g_big_path);
    f->data = g_payload_big; f->len = BIG_BYTES;
    f = &g_files[n++];
    snprintf(f->path, sizeof f->path, "%s", g_med_path);
    f->data = g_payload_med; f->len = MED_BYTES;
    f = &g_files[n++];
    snprintf(f->path, sizeof f->path, "%s", g_small_path);
    f->data = g_payload_small; f->len = SMALL_BYTES;
    f = &g_files[n++];
    snprintf(f->path, sizeof f->path, "%s", g_other_path);
    f->data = g_payload_small; f->len = SMALL_BYTES;
    for (uint32_t i = 0; i < TINY_COUNT; ++i) {
        f = &g_files[n++];
        snprintf(f->path, sizeof f->path, "%st%03u.bin", g_prefix, i);
        f->data = &g_payload_tiny[i]; f->len = 1;
    }
    qsort(g_files, FILE_COUNT, sizeof g_files[0], file_compare);
}

static int build_bsa(size_t *out_len) {
    uint32_t folder_name_bytes = 1u + (uint32_t)strlen("meshes") + 1u;
    uint32_t name_bytes = 0;
    for (uint32_t i = 0; i < FILE_COUNT; ++i)
        name_bytes += (uint32_t)strlen(g_files[i].path + strlen(g_prefix)) + 1u;
    size_t data_start = 36u + 16u + folder_name_bytes + 16u * FILE_COUNT +
                        name_bytes;
    size_t at = data_start;
    for (uint32_t i = 0; i < FILE_COUNT; ++i) {
        g_offsets[i] = (uint32_t)at;
        at += g_files[i].len;
    }
    if (at > sizeof g_archive) return 0;
    memset(g_archive, 0, data_start);
    memcpy(g_archive, "BSA\0", 4);
    put_u32(g_archive + 4, 104);
    put_u32(g_archive + 8, 36);
    put_u32(g_archive + 12, 0x001u | 0x002u);
    put_u32(g_archive + 16, 1);
    put_u32(g_archive + 20, FILE_COUNT);
    put_u32(g_archive + 24, folder_name_bytes - 1u);
    put_u32(g_archive + 28, name_bytes);
    put_u32(g_archive + 32, 0);
    size_t cursor = 36;
    put_u64(g_archive + cursor, 0);
    put_u32(g_archive + cursor + 8, FILE_COUNT);
    put_u32(g_archive + cursor + 12, 0);
    cursor += 16;
    g_archive[cursor++] = 7;
    memcpy(g_archive + cursor, "meshes", 6);
    cursor += 6;
    g_archive[cursor++] = 0;
    for (uint32_t i = 0; i < FILE_COUNT; ++i) {
        put_u64(g_archive + cursor, 0);
        put_u32(g_archive + cursor + 8, g_files[i].len);
        put_u32(g_archive + cursor + 12, g_offsets[i]);
        cursor += 16;
    }
    for (uint32_t i = 0; i < FILE_COUNT; ++i) {
        const char *base = g_files[i].path + strlen(g_prefix);
        size_t len = strlen(base);
        memcpy(g_archive + cursor, base, len);
        cursor += len;
        g_archive[cursor++] = 0;
    }
    if (cursor != data_start) return 0;
    for (uint32_t i = 0; i < FILE_COUNT; ++i) {
        memcpy(g_archive + cursor, g_files[i].data, g_files[i].len);
        cursor += g_files[i].len;
    }
    *out_len = cursor;
    return 1;
}

static size_t build_index(size_t archive_len) {
    uint32_t pool_len = (uint32_t)strlen("stream_queue.bsa") + 1u;
    for (uint32_t i = 0; i < FILE_COUNT; ++i)
        pool_len += (uint32_t)strlen(g_files[i].path) + 1u;
    size_t archive_off = STREAM_INDEX_HEADER_SIZE;
    size_t resource_off = archive_off + STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    size_t pool_off = resource_off +
                      (size_t)FILE_COUNT * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    size_t cell_off = (pool_off + pool_len + 7u) & ~(size_t)7u;
    size_t total = cell_off + 4u;
    if (total > sizeof g_index) return 0;
    memset(g_index, 0, total);
    memcpy(g_index, "SRNX", 4);
    put_u16(g_index + 4, STREAM_INDEX_SCHEMA);
    put_u16(g_index + 6, STREAM_INDEX_HEADER_SIZE);
    put_u16(g_index + 8, STREAM_INDEX_ARCHIVE_ENTRY_SIZE);
    put_u16(g_index + 10, STREAM_INDEX_RESOURCE_ENTRY_SIZE);
    put_u16(g_index + 12, STREAM_INDEX_CELL_ENTRY_SIZE);
    put_u16(g_index + 14, 0x0001);
    put_u32(g_index + 16, 1);
    put_u32(g_index + 20, FILE_COUNT);
    put_u32(g_index + 24, 0);
    put_u64(g_index + 32, 0x5152535455565758ull);
    put_u64(g_index + 40, archive_off);
    put_u64(g_index + 48, resource_off);
    put_u64(g_index + 56, pool_off);
    put_u64(g_index + 64, cell_off);
    put_u64(g_index + 72, total);
    uint8_t *arc = g_index + archive_off;
    const char *arc_name = "stream_queue.bsa";
    put_u32(arc + 0, 0);
    put_u32(arc + 4, (uint32_t)strlen(arc_name));
    put_u64(arc + 8, archive_len);
    put_u32(arc + 48, 0);
    put_u32(arc + 52, FILE_COUNT);
    memcpy(g_index + pool_off, arc_name, strlen(arc_name));
    uint32_t pool_at = (uint32_t)strlen(arc_name) + 1u;
    for (uint32_t i = 0; i < FILE_COUNT; ++i) {
        uint8_t *p = g_index + resource_off +
                     (size_t)i * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
        uint32_t path_len = (uint32_t)strlen(g_files[i].path);
        put_u32(p + 0, i);
        put_u32(p + 4, pool_at);
        put_u32(p + 8, path_len);
        put_u32(p + 12, 0);
        put_u64(p + 16, g_offsets[i]);
        put_u32(p + 24, g_files[i].len);
        put_u32(p + 28, g_files[i].len);
        put_u32(p + 32, g_offsets[i]);
        put_u32(p + 36, 0);
        memcpy(g_index + pool_off + pool_at, g_files[i].path, path_len);
        pool_at += path_len + 1u;
    }
    put_u32(g_index + total - 4, bsa_crc32(g_index, total - 4));
    return total;
}

static int write_archive(size_t len) {
    FILE *f = fopen("stream_queue.bsa", "wb");
    if (!f) return 0;
    int ok = fwrite(g_archive, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */

static uint64_t g_now_ns;
static uint64_t fake_clock(void *ctx) {
    (void)ctx;
    return g_now_ns;
}

static void advance_ms(uint64_t ms) {
    g_now_ns += ms * 1000000ull;
}

static StreamMount *open_mount(const StreamIndex *index,
                               const StreamConfig *config) {
    StreamMount *mount = NULL;
    char reason[STREAM_REASON_MAX];
    if (stream_mount_open(index, ".", &mount, reason, sizeof reason) !=
        STREAM_OK)
        return NULL;
    stream_mount_set_clock(mount, fake_clock, NULL);
    g_now_ns = 1000000000ull;
    if (stream_mount_configure(mount, config, reason, sizeof reason) !=
        STREAM_OK) {
        stream_mount_close(mount);
        return NULL;
    }
    return mount;
}

static int resource_of(const StreamIndex *index, const char *path,
                       StreamResource *out) {
    char reason[STREAM_REASON_MAX];
    return stream_index_find_path(index, path, out, reason, sizeof reason) ==
           STREAM_OK;
}

static StreamHandle enqueue(StreamMount *mount, const StreamResource *res,
                            uint32_t n, StreamPriority prio,
                            uint64_t deadline_ms) {
    char reason[STREAM_REASON_MAX];
    StreamHandle handle = 0;
    if (stream_mount_prefetch_ex(mount, res, n, 0, prio, deadline_ms, &handle,
                                 reason, sizeof reason) != STREAM_OK)
        return 0;
    return handle;
}

static char g_pump_reason[STREAM_REASON_MAX];
static void pump_one(StreamMount *mount) {
    uint32_t done = 0;
    stream_mount_pump(mount, 1, &done, g_pump_reason, sizeof g_pump_reason);
}

static StreamRequestState state_of(StreamMount *mount, StreamHandle handle) {
    StreamRequestInfo info;
    if (stream_mount_request_info(mount, handle, &info) != STREAM_OK)
        return (StreamRequestState)0xff;
    return info.state;
}

/* ------------------------------------------------------------------ */

static void test_budget_and_copy(const StreamIndex *index) {
    StreamConfig cfg;
    stream_mount_config_default(&cfg);
    cfg.rate_bytes_per_sec = 10000000ull;
    cfg.quantum_bytes = 65536;
    cfg.window_max_bytes = 262144;
    cfg.cache_limit_bytes = 1024u * 1024u;
    StreamMount *mount = open_mount(index, &cfg);
    check(mount != NULL, "budget mount opens");
    if (!mount) return;

    StreamResource big;
    check(resource_of(index, g_big_path, &big), "big resource resolves");
    StreamHandle h = enqueue(mount, &big, BIG_BYTES, STREAM_PRIO_DEMAND, 0);
    check(h != 0, "demand window is admitted");
    StreamRequestInfo info;
    char reason[STREAM_REASON_MAX];
    check(stream_mount_request_info(mount, h, &info) == STREAM_OK &&
              info.state == STREAM_REQ_QUEUED && info.target_bytes == 262144 &&
              info.consumers == 1,
          "queued demand clamps to the 256 KiB window");
    uint8_t copy[256];
    uint32_t got = 1;
    check(stream_mount_request_copy(mount, h, 0, copy, sizeof copy, &got,
                                    NULL, 0) == STREAM_ERR_UNSUPPORTED,
          "copy before readiness refuses without I/O");

    pump_one(mount);
    check(stream_mount_request_info(mount, h, &info) == STREAM_OK &&
              info.fetched_bytes == 65536 && info.state == STREAM_REQ_QUEUED,
          "first quantum is 64 KiB and stays queued");
    pump_one(mount);
    check(stream_mount_request_info(mount, h, &info) == STREAM_OK &&
              info.fetched_bytes == 131072,
          "initial budget allows exactly two quanta");
    uint32_t done = 1;
    stream_mount_pump(mount, 1, &done, reason, sizeof reason);
    check(done == 0, "a third quantum waits for the byte budget");
    StreamMetrics m;
    stream_mount_metrics(mount, &m);
    check(m.budget_denied >= 1 && m.quanta == 2 && m.quantum_bytes == 131072,
          "budget denial and charged bytes are counted");

    advance_ms(100);
    stream_mount_pump(mount, 4, &done, reason, sizeof reason);
    check(done == 2 &&
              state_of(mount, h) == STREAM_REQ_READY &&
              stream_mount_request_info(mount, h, &info) == STREAM_OK &&
              info.fetched_bytes == 262144,
          "100 ms at 10 MB/s completes the window as READY");
    stream_mount_metrics(mount, &m);
    check(m.verified == 1 && m.ready == 1 && m.seeks == 1,
          "verification, readiness and one initial seek are counted");

    got = 0;
    check(stream_mount_request_copy(mount, h, 1000, copy, sizeof copy, &got,
                                    reason, sizeof reason) == STREAM_OK &&
              got == sizeof copy &&
              memcmp(copy, g_payload_big + 1000, sizeof copy) == 0,
          "READY copy matches the staged payload");
    uint64_t reads_before = m.physical_reads;
    uint8_t demand[128];
    uint32_t demand_got = 0;
    check(stream_mount_read(mount, &big, 7000, demand, sizeof demand,
                            &demand_got, reason, sizeof reason) == STREAM_OK &&
              demand_got == sizeof demand &&
              memcmp(demand, g_payload_big + 7000, sizeof demand) == 0,
          "synchronous demand read is served from the READY window");
    stream_mount_metrics(mount, &m);
    check(m.physical_reads == reads_before && m.cache_hits >= 2,
          "the demand hit costs no archive read");

    stream_mount_release(mount, h, reason, sizeof reason);
    stream_mount_cancel(mount, h, reason, sizeof reason);
    stream_mount_close(mount);
}

static void test_priority_order(const StreamIndex *index) {
    StreamConfig cfg;
    stream_mount_config_default(&cfg);
    cfg.rate_bytes_per_sec = 1000000000ull;
    cfg.quantum_bytes = 65536;
    cfg.window_max_bytes = 65536;
    cfg.cache_limit_bytes = 1024u * 1024u;
    StreamMount *mount = open_mount(index, &cfg);
    check(mount != NULL, "priority mount opens");
    if (!mount) return;

    StreamResource big, med, small, other;
    check(resource_of(index, g_big_path, &big) &&
              resource_of(index, g_med_path, &med) &&
              resource_of(index, g_small_path, &small) &&
              resource_of(index, g_other_path, &other),
          "priority resources resolve");
    StreamHandle hp2 = enqueue(mount, &big, 8192, STREAM_PRIO_P2, 0);
    StreamHandle hp1 = enqueue(mount, &med, 8192, STREAM_PRIO_P1, 0);
    StreamHandle hp0 = enqueue(mount, &small, 8192, STREAM_PRIO_P0, 0);
    StreamHandle hd = enqueue(mount, &other, 8192, STREAM_PRIO_DEMAND, 0);
    check(hp2 && hp1 && hp0 && hd, "four priorities are admitted");
    StreamRequestInfo info;
    pump_one(mount);
    check(stream_mount_request_info(mount, hd, &info) == STREAM_OK &&
              info.state == STREAM_REQ_READY,
          "demand runs first");
    pump_one(mount);
    check(state_of(mount, hp0) == STREAM_REQ_READY,
          "P0 runs before P1/P2");
    pump_one(mount);
    check(state_of(mount, hp1) == STREAM_REQ_READY,
          "P1 runs before P2");
    pump_one(mount);
    check(state_of(mount, hp2) == STREAM_REQ_READY, "P2 runs last");

    /* Deadline: an unused P2 is dropped before it can run. */
    StreamResource tiny;
    check(resource_of(index, "meshes\\t000.bin", &tiny),
          "deadline resource resolves");
    StreamHandle late = enqueue(mount, &tiny, 8192, STREAM_PRIO_P2,
                                stream_mount_now_ms(mount) + 5);
    check(late != 0, "deadline request is admitted");
    advance_ms(10);
    pump_one(mount);
    check(state_of(mount, late) == STREAM_REQ_CANCELLED,
          "expired P2 becomes CANCELLED");
    StreamMetrics m;
    stream_mount_metrics(mount, &m);
    check(m.expired == 1, "expiry is counted");

    char reason[STREAM_REASON_MAX];
    stream_mount_release(mount, hd, reason, sizeof reason);
    stream_mount_release(mount, hp0, reason, sizeof reason);
    stream_mount_release(mount, hp1, reason, sizeof reason);
    stream_mount_release(mount, hp2, reason, sizeof reason);
    stream_mount_release(mount, late, reason, sizeof reason);
    stream_mount_close(mount);
}

static void test_dedup_and_cancel(const StreamIndex *index) {
    StreamConfig cfg;
    stream_mount_config_default(&cfg);
    cfg.rate_bytes_per_sec = 10000000ull;
    cfg.quantum_bytes = 65536;
    cfg.window_max_bytes = 131072;
    cfg.cache_limit_bytes = 512u * 1024u;
    StreamMount *mount = open_mount(index, &cfg);
    check(mount != NULL, "dedup mount opens");
    if (!mount) return;

    StreamResource big, med;
    check(resource_of(index, g_big_path, &big) &&
              resource_of(index, g_med_path, &med),
          "dedup resources resolve");
    StreamHandle a = enqueue(mount, &big, 65536, STREAM_PRIO_P1, 0);
    StreamHandle b = enqueue(mount, &big, 65536, STREAM_PRIO_P1, 0);
    check(a && b && a == b, "same (generation, source, id, lod) dedups to one handle");
    StreamHandle d = enqueue(mount, &big, 65536, STREAM_PRIO_DEMAND, 0);
    check(d == a, "demand joins the live prefetch");
    StreamRequestInfo info;
    check(stream_mount_request_info(mount, a, &info) == STREAM_OK &&
              info.priority == STREAM_PRIO_DEMAND && info.consumers == 1,
          "joining raises priority and adds one subscriber");
    StreamMetrics m;
    stream_mount_metrics(mount, &m);
    check(m.prefetch_requests == 3 && m.dedup_hits == 2,
          "dedup admissions are counted");
    pump_one(mount);
    check(state_of(mount, a) == STREAM_REQ_READY, "deduped request becomes ready");
    stream_mount_metrics(mount, &m);
    check(m.quanta == 1 && m.physical_reads == 1,
          "one physical read for the shared resource");

    /* A larger repeated request extends the same READY window and re-queues it. */
    StreamHandle grow = enqueue(mount, &big, 131072, STREAM_PRIO_P1, 0);
    check(grow == a, "a larger request keeps the same key handle");
    check(stream_mount_request_info(mount, a, &info) == STREAM_OK &&
              info.target_bytes == 131072 && info.state == STREAM_REQ_QUEUED,
          "extended window returns to QUEUED");
    pump_one(mount);
    check(state_of(mount, a) == STREAM_REQ_READY &&
              stream_mount_request_info(mount, a, &info) == STREAM_OK &&
              info.fetched_bytes == 131072,
          "the extended window resumes and becomes READY");
    uint8_t tail[64];
    uint32_t tail_len = 0;
    char tail_reason[STREAM_REASON_MAX];
    check(stream_mount_request_copy(mount, a, 70000, tail, sizeof tail,
                                    &tail_len, tail_reason,
                                    sizeof tail_reason) == STREAM_OK &&
              tail_len == sizeof tail &&
              memcmp(tail, g_payload_big + 70000, sizeof tail) == 0,
          "bytes past the old window are staged correctly");

    char reason[STREAM_REASON_MAX];
    stream_mount_release(mount, a, reason, sizeof reason);
    StreamHandle partial = enqueue(mount, &med, MED_BYTES, STREAM_PRIO_P0, 0);
    check(partial != 0, "partial request admitted");
    pump_one(mount);
    check(state_of(mount, partial) == STREAM_REQ_QUEUED, "partial request is mid-window");
    stream_mount_cancel(mount, partial, reason, sizeof reason);
    check(state_of(mount, partial) == STREAM_REQ_CANCELLED,
          "cancel frees the staged bytes");
    stream_mount_metrics(mount, &m);
    check(m.cache_bytes == 131072 && m.cancelled == 1,
          "cancel removed exactly the partial window");
    stream_mount_release(mount, partial, reason, sizeof reason);
    check(stream_mount_request_info(mount, partial, &info) == STREAM_ERR_NOT_FOUND,
          "terminal descriptor is reclaimed on release");
    stream_mount_cancel(mount, a, reason, sizeof reason);
    stream_mount_close(mount);
}

static void test_eviction(const StreamIndex *index) {
    StreamConfig cfg;
    stream_mount_config_default(&cfg);
    cfg.rate_bytes_per_sec = 10000000ull;
    cfg.quantum_bytes = 65536;
    cfg.window_max_bytes = 65536;
    cfg.cache_limit_bytes = 131072;
    StreamMount *mount = open_mount(index, &cfg);
    check(mount != NULL, "eviction mount opens");
    if (!mount) return;

    StreamResource big, med, small;
    check(resource_of(index, g_big_path, &big) &&
              resource_of(index, g_med_path, &med) &&
              resource_of(index, g_small_path, &small),
          "eviction resources resolve");
    StreamHandle a = enqueue(mount, &big, 65536, STREAM_PRIO_P1, 0);
    StreamHandle b = enqueue(mount, &med, 65536, STREAM_PRIO_P1, 0);
    check(a && b, "two windows fill the cache");
    pump_one(mount);
    advance_ms(10);
    pump_one(mount);
    check(state_of(mount, a) == STREAM_REQ_READY &&
              state_of(mount, b) == STREAM_REQ_READY,
          "both windows are ready");
    char reason[STREAM_REASON_MAX];
    stream_mount_pin(mount, a, 1, reason, sizeof reason);
    StreamHandle c = enqueue(mount, &small, 65536, STREAM_PRIO_P0, 0);
    check(c != 0 && c != a && c != b, "a third key is admitted");
    StreamMetrics m;
    stream_mount_metrics(mount, &m);
    check(m.evicted == 1 && m.evicted_bytes == 65536,
          "one 64 KiB window is evicted");
    check(state_of(mount, a) == STREAM_REQ_READY,
          "pinned window survives eviction");
    StreamRequestInfo gone;
    check(stream_mount_request_info(mount, b, &gone) == STREAM_ERR_NOT_FOUND,
          "the evicted LRU descriptor is reclaimed");
    stream_mount_cancel(mount, c, reason, sizeof reason);

    uint32_t freed = 0;
    stream_mount_trim(mount, 0, &freed, reason, sizeof reason);
    check(state_of(mount, a) == STREAM_REQ_READY && freed == 0,
          "trim honours the pin");
    stream_mount_pin(mount, a, 0, reason, sizeof reason);
    stream_mount_trim(mount, 0, &freed, reason, sizeof reason);
    stream_mount_metrics(mount, &m);
    check(m.cache_bytes == 0, "trim releases unpinned windows");
    stream_mount_close(mount);
}

static int reject_zero(void *ctx, const StreamResource *res,
                       const uint8_t *data, uint32_t len) {
    (void)ctx; (void)res; (void)len;
    return data[0] == 0x00;
}

static int count_ok;
static int accept_all(void *ctx, const StreamResource *res, const uint8_t *data,
                      uint32_t len) {
    (void)ctx; (void)res; (void)data; (void)len;
    ++count_ok;
    return 0;
}

static void test_verify_and_limits(const StreamIndex *index) {
    StreamConfig cfg;
    stream_mount_config_default(&cfg);
    cfg.rate_bytes_per_sec = 1000000000ull;
    cfg.quantum_bytes = 65536;
    cfg.window_max_bytes = 65536;
    cfg.cache_limit_bytes = 1024u * 1024u;
    StreamMount *mount = open_mount(index, &cfg);
    check(mount != NULL, "verify mount opens");
    if (!mount) return;
    stream_mount_set_verify(mount, reject_zero, NULL);

    StreamResource big, tiny;
    check(resource_of(index, g_big_path, &big) &&
              resource_of(index, "meshes\\t000.bin", &tiny),
          "verify resources resolve");
    StreamHandle bad = enqueue(mount, &big, 65536, STREAM_PRIO_P0, 0);
    pump_one(mount);
    check(bad && state_of(mount, bad) == STREAM_REQ_ERROR,
          "a rejected digest leaves ERROR, never READY");
    StreamMetrics m;
    stream_mount_metrics(mount, &m);
    check(m.request_errors == 1 && m.cache_bytes == 0,
          "verification failure frees the window");
    char reason[STREAM_REASON_MAX];
    stream_mount_release(mount, bad, reason, sizeof reason);

    stream_mount_set_verify(mount, accept_all, NULL);
    StreamHandle good = enqueue(mount, &tiny, 1, STREAM_PRIO_P0, 0);
    pump_one(mount);
    check(good && state_of(mount, good) == STREAM_REQ_READY && count_ok == 1,
          "accepted digest publishes READY");
    stream_mount_release(mount, good, reason, sizeof reason);
    stream_mount_cancel(mount, good, reason, sizeof reason);

    /* 256 descriptors: P0 cannot be dropped, so the 257th is refused. */
    uint32_t admitted = 0;
    StreamHandle handles[STREAM_MAX_REQUESTS];
    memset(handles, 0, sizeof handles);
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        char path[32];
        StreamResource res;
        snprintf(path, sizeof path, "meshes\\t%03u.bin",
                 i < TINY_COUNT ? i : 0);
        if (!resource_of(index, path, &res)) break;
        /* Use distinct keys by LOD even when the path repeats. */
        char reason2[STREAM_REASON_MAX];
        StreamHandle h = 0;
        if (stream_mount_prefetch_ex(mount, &res, 1, i, STREAM_PRIO_P0, 0, &h,
                                     reason2, sizeof reason2) != STREAM_OK)
            break;
        handles[admitted++] = h;
    }
    check(admitted == STREAM_MAX_REQUESTS,
          "all 256 descriptors are available");
    StreamResource last;
    check(resource_of(index, "meshes\\t000.bin", &last), "last resource resolves");
    char reason2[STREAM_REASON_MAX];
    StreamHandle overflow = 0;
    check(stream_mount_prefetch_ex(mount, &last, 1, 999, STREAM_PRIO_P0, 0,
                                   &overflow, reason2, sizeof reason2) ==
              STREAM_ERR_BUDGET,
          "the 257th P0 descriptor is refused with BUDGET");
    stream_mount_metrics(mount, &m);
    check(m.descriptors == STREAM_MAX_REQUESTS,
          "descriptor accounting stops at 256");

    /* Reclaim every descriptor and prove alloc/free balance. */
    for (uint32_t i = 0; i < admitted; ++i) {
        stream_mount_cancel(mount, handles[i], reason, sizeof reason);
        stream_mount_cancel(mount, handles[i], reason, sizeof reason);
    }
    stream_mount_metrics(mount, &m);
    check(m.descriptors == 0 && m.cache_bytes == 0,
          "every descriptor and window is gone");
    check(m.allocs == m.frees && m.bytes_allocated == m.bytes_freed,
          "allocation and free counters balance");
    stream_mount_close(mount);
}

int main(void) {
    fill_payloads();
    build_file_list();
    size_t archive_len = 0;
    check(build_bsa(&archive_len), "synthetic BSA builds");
    check(write_archive(archive_len), "synthetic BSA is written");
    size_t index_len = build_index(archive_len);
    check(index_len > 0, "synthetic index builds");

    StreamIndex *index = NULL;
    char reason[STREAM_REASON_MAX];
    StreamStatus parse_st = stream_index_parse(g_index, index_len, &index, reason,
                                               sizeof reason);
    check(parse_st == STREAM_OK && index != NULL && reason[0] == 0,
          "synthetic index parses");
    if (parse_st != STREAM_OK) printf("index parse: %s\n", reason);

    test_budget_and_copy(index);
    test_priority_order(index);
    test_dedup_and_cancel(index);
    test_eviction(index);
    test_verify_and_limits(index);

    stream_index_free(index);
    printf("stream_queue_smoke: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
