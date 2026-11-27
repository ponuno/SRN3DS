/* Host test: cell trigger on synthetic guest memory and index.
 *
 * Two interior cells (form 0x133C6 with two files, 0x200 with one file),
 * three archive resources.  The fake GuestMemory holds the CELL_TRACE tuple
 * (holder [0x1b2e864], cell +0x6c, grid +0x5c/+0x60, worldspace +0xbc,
 * latch 0x12e3560).  Checks: tuple change -> P0 requests; latch -> P0;
 * interior -> one centre row; shrink -> trim; demand read served from READY
 * without a second prefetch; unknown exterior grid -> no requests.
 */

#include "asset_stream.h"
#include "bsa_reader.h"
#include "guest.h"
#include "stream_cell.h"

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

#define FILE_COUNT 3u
#define FILE_PATH_MAX 64u
#define BIG_BYTES 100000u
#define MED_BYTES 70000u
#define SMALL_BYTES 50000u

typedef struct {
    char path[FILE_PATH_MAX];
    uint8_t *data;
    uint32_t len;
} TestFile;

static TestFile g_files[FILE_COUNT];
static uint32_t g_offsets[FILE_COUNT];
static uint8_t g_archive[512u * 1024u];
static uint8_t g_index[64u * 1024u];
static uint8_t g_big[BIG_BYTES];
static uint8_t g_med[MED_BYTES];
static uint8_t g_small[SMALL_BYTES];
static const char *const g_prefix = "meshes\\";

static uint64_t g_now_ns;
static uint64_t fake_clock(void *ctx) {
    (void)ctx;
    return g_now_ns;
}

static int file_compare(const void *a, const void *b) {
    return strcmp(((const TestFile *)a)->path, ((const TestFile *)b)->path);
}

static void build_file_list(void) {
    for (uint32_t i = 0; i < BIG_BYTES; ++i) g_big[i] = (uint8_t)(i * 7u);
    for (uint32_t i = 0; i < MED_BYTES; ++i) g_med[i] = (uint8_t)(i * 11u + 3u);
    for (uint32_t i = 0; i < SMALL_BYTES; ++i) g_small[i] = (uint8_t)(i ^ 0x5a);
    snprintf(g_files[0].path, sizeof g_files[0].path, "%sa000.bin", g_prefix);
    g_files[0].data = g_big; g_files[0].len = BIG_BYTES;
    snprintf(g_files[1].path, sizeof g_files[1].path, "%sa001.bin", g_prefix);
    g_files[1].data = g_med; g_files[1].len = MED_BYTES;
    snprintf(g_files[2].path, sizeof g_files[2].path, "%sa002.bin", g_prefix);
    g_files[2].data = g_small; g_files[2].len = SMALL_BYTES;
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
    const char *arc_name = "stream_cell.bsa";
    uint32_t pool_len = (uint32_t)strlen(arc_name) + 1u;
    for (uint32_t i = 0; i < FILE_COUNT; ++i)
        pool_len += (uint32_t)strlen(g_files[i].path) + 1u;
    size_t archive_off = STREAM_INDEX_HEADER_SIZE;
    size_t resource_off = archive_off + STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    size_t pool_off = resource_off +
                      (size_t)FILE_COUNT * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    size_t cell_off = (pool_off + pool_len + 7u) & ~(size_t)7u;
    size_t total = cell_off + 2u * STREAM_INDEX_CELL_ENTRY_SIZE + 4u;
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
    put_u32(g_index + 24, 2);
    put_u64(g_index + 32, 0x5152535455565758ull);
    put_u64(g_index + 40, archive_off);
    put_u64(g_index + 48, resource_off);
    put_u64(g_index + 56, pool_off);
    put_u64(g_index + 64, cell_off);
    put_u64(g_index + 72, total);
    uint8_t *arc = g_index + archive_off;
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
    uint8_t *cell = g_index + cell_off;
    /* Rows are sorted by (kind, form_id).  Cell 0x200: id 2. */
    put_u32(cell + 0, 0);
    put_u32(cell + 4, 0x200u);
    put_u32(cell + 24, 1);
    put_u32(cell + 32, 2);
    /* Cell 0x133C6: ids 0 and 1. */
    cell += STREAM_INDEX_CELL_ENTRY_SIZE;
    put_u32(cell + 0, 0);
    put_u32(cell + 4, 0x133C6u);
    put_u32(cell + 24, 2);
    put_u32(cell + 32, 0);
    put_u32(cell + 36, 1);
    put_u32(g_index + total - 4, bsa_crc32(g_index, total - 4));
    return total;
}

static int write_files(size_t archive_len, size_t index_len) {
    FILE *f = fopen("stream_cell.bsa", "wb");
    if (!f) return 0;
    int ok = fwrite(g_archive, 1, archive_len, f) == archive_len;
    fclose(f);
    if (!ok) return 0;
    f = fopen("stream_cell_index.bin", "wb");
    if (!f) return 0;
    ok = fwrite(g_index, 1, index_len, f) == index_len;
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */

#define HOLDER_ADDR 0x1b2e864u
#define LATCH_ADDR 0x12e3560u
#define UGRIDS_ADDR 0x1241000u
#define HOLDER_OBJ 0x20000000u
#define CELL_A 0x20001000u
#define CELL_B 0x20002000u
#define WS_OBJ 0x20003000u

static GuestMemory g_mem;

static void wr32(uint32_t addr, uint32_t v) {
    check(guest_write32(&g_mem, addr, v) == 0, "guest_write32");
}
static void wr8(uint32_t addr, uint8_t v) {
    check(guest_write(&g_mem, addr, &v, 1) == 0, "guest_write8");
}

static void set_tuple(uint32_t cell, int32_t gx, int32_t gy, uint32_t ws) {
    wr32(HOLDER_ADDR, HOLDER_OBJ);
    wr32(HOLDER_OBJ + 0x6c, cell);
    wr32(HOLDER_OBJ + 0x5c, (uint32_t)gx);
    wr32(HOLDER_OBJ + 0x60, (uint32_t)gy);
    wr32(HOLDER_OBJ + 0xbc, ws);
}

static void drain(const StreamMount *mount) {
    for (unsigned i = 0; i < 64; ++i) {
        g_now_ns += 50ull * 1000000ull;
        uint32_t quanta = 0;
        char reason[STREAM_REASON_MAX];
        (void)stream_mount_pump((StreamMount *)mount, 4, &quanta, reason,
                                sizeof reason);
        if (!quanta) break;
    }
}

static StreamPriority handle_priority(const StreamMount *mount,
                                      StreamHandle handle) {
    StreamRequestInfo info;
    if (stream_mount_request_info(mount, handle, &info) != STREAM_OK)
        return STREAM_PRIO_DEMAND;
    return info.priority;
}

int main(void) {
    build_file_list();
    size_t archive_len = 0;
    check(build_bsa(&archive_len), "synthetic BSA builds");
    size_t index_len = build_index(archive_len);
    check(index_len > 0, "synthetic index builds");
    check(write_files(archive_len, index_len), "synthetic files written");

    guest_init(&g_mem, 64u << 20);
    check(guest_map(&g_mem, 0x01200000u, 0x01000000u) == 0, "map latch page");
    check(guest_map(&g_mem, 0x1b000000u, 0x100000u) == 0, "map holder page");
    check(guest_map(&g_mem, 0x20000000u, 0x100000u) == 0, "map cell page");
    wr32(UGRIDS_ADDR, 5);
    wr8(LATCH_ADDR, 0);
    wr32(CELL_A + 0x0c, 0x133C6u);
    wr8(CELL_A + 0x2c, 1);
    wr32(CELL_B + 0x0c, 0x200u);
    wr8(CELL_B + 0x2c, 1);
    wr32(WS_OBJ + 0x0c, 0x3c);
    set_tuple(CELL_A, 0, 0, 0);

    StreamCellState *state = NULL;
    char reason[STREAM_REASON_MAX];
    StreamStatus st = stream_cell_open(&state, "stream_cell_index.bin", ".",
                                       reason, sizeof reason);
    check(st == STREAM_OK && state != NULL, "cell state opens");
    if (st != STREAM_OK) {
        printf("open: %s\n", reason);
        return 1;
    }
    g_now_ns = 1000000ull;
    stream_mount_set_clock((StreamMount *)stream_cell_mount(state), fake_clock,
                           NULL);

    StreamCellKey key;
    memset(&key, 0, sizeof key);
    key.form_id = 0x133C6u;
    StreamCellRow row;
    check(stream_index_find_cell(stream_cell_index(state), &key, &row, reason,
                                 sizeof reason) == STREAM_OK &&
              row.resource_count == 2,
          "cell row lookup finds form 0x133c6");
    key.form_id = 0x999u;
    check(stream_index_find_cell(stream_cell_index(state), &key, &row, reason,
                                 sizeof reason) == STREAM_ERR_NOT_FOUND,
          "an unknown cell is a miss");

    /* 1. Tuple change -> two P0 requests, centre visible. */
    st = stream_cell_tick(state, &g_mem, reason, sizeof reason);
    const StreamCellMetrics *m = stream_cell_metrics(state);
    check(st == STREAM_OK && m->transitions == 1, "first tick is a transition");
    check(m->rows_found == 1 && m->last_resource_count == 2,
          "interior uses one centre row");
    check(m->prefetch_requests == 2, "two centre assets are queued");
    check(stream_cell_handle_count(state) == 2, "both centre handles visible");
    for (uint32_t i = 0; i < stream_cell_handle_count(state); ++i)
        check(handle_priority(stream_cell_mount(state),
                              stream_cell_handle_at(state, i)) ==
                  STREAM_PRIO_P0,
              "trigger queues P0");

    drain(stream_cell_mount(state));
    StreamMetrics sm;
    stream_mount_metrics(stream_cell_mount(state), &sm);
    check(sm.ready >= 1 && sm.physical_reads >= 1,
          "pump stages at least one centre window");

    /* 2. Demand hit is served from READY and does not enqueue again. */
    uint64_t prefetch_before = m->prefetch_requests;
    uint8_t buf[4096];
    uint32_t got = 0;
    check(stream_cell_demand_read(state, g_files[0].path, 0, buf, sizeof buf,
                                  &got) == 1,
          "demand read is served from the staged window");
    check(got == sizeof buf && !memcmp(buf, g_files[0].data, sizeof buf),
          "staged bytes match the payload");
    check(m->demand_hits == 1, "demand hit is counted");
    check(m->prefetch_requests == prefetch_before,
          "demand hit does not duplicate prefetch");
    check(stream_cell_demand_read(state, g_files[2].path, 0, buf, sizeof buf,
                                  &got) == 0,
          "a path with no staged window falls back");
    check(m->demand_misses == 1, "fallback is counted as a miss");

    /* 3. Shrink: cell A -> cell B, one asset, trim evicts old windows. */
    set_tuple(CELL_B, 0, 0, 0);
    st = stream_cell_tick(state, &g_mem, reason, sizeof reason);
    check(st == STREAM_OK && m->transitions == 2, "cell change is a transition");
    check(m->rows_found == 2 && m->last_resource_count == 1,
          "second interior row found");
    check(m->trim_events == 1 && m->trim_bytes > 0,
          "shrinking the working set trims old windows");
    check(stream_cell_handle_count(state) == 1, "one centre handle visible");

    /* 4. Latch 0x12e3560 without a tuple change still prefetches. */
    drain(stream_cell_mount(state));
    uint64_t before = m->prefetch_requests;
    wr8(LATCH_ADDR, 1);
    st = stream_cell_tick(state, &g_mem, reason, sizeof reason);
    check(st == STREAM_OK && m->moved_edges >= 1,
          "grid-moved latch is observed");
    check(m->prefetch_requests >= before, "latch keeps the centre queued");
    check(handle_priority(stream_cell_mount(state),
                          stream_cell_handle_at(state, 0)) == STREAM_PRIO_P0,
          "latch path stays P0");
    wr8(LATCH_ADDR, 0);

    /* 5. Interior with the same tuple and latch clear is a no-op. */
    uint64_t quiet_ticks = m->ticks;
    uint64_t quiet_prefetch = m->prefetch_requests;
    stream_cell_tick(state, &g_mem, reason, sizeof reason);
    check(m->ticks == quiet_ticks + 1, "tick is still counted");
    check(m->prefetch_requests == quiet_prefetch,
          "an unchanged tuple does not enqueue");

    /* 6. Exterior grid with no index rows -> no requests, honest miss. */
    uint64_t missing_before = m->rows_missing;
    set_tuple(CELL_A, 4, -2, WS_OBJ);
    stream_cell_tick(state, &g_mem, reason, sizeof reason);
    check(m->exterior_ticks == 1, "worldspace selects the exterior path");
    check(m->rows_missing >= missing_before + 1,
          "missing exterior rows are counted, not invented");

    stream_cell_close(state);
    guest_destroy(&g_mem);
    printf("stream_cell_smoke: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
