/* Real-index prefetch test: RiverwoodSleepingGiantInn (form 0x133C6).
 *
 * Unlike stream_cell_real_smoke this drives the real `stream_cell` trigger and
 * pump against the retail archive, so it exercises compressed range reads:
 * every centre resource must be admitted, `skipped_compressed` must stay 0 and
 * the executor must perform physical reads.  It needs an index built by
 * tools/stream_index_build.py for the cell:
 *
 *   python tools/stream_index_build.py --data E:/harness_skyrim/Data \
 *       --cell RiverwoodSleepingGiantInn --out build/stream_index.bin
 *   stream_cell_real_prefetch.exe build/stream_index.bin E:/harness_skyrim/Data
 *
 * Skips (exit 0) when no index path is supplied or the file is absent.
 */

#include "asset_stream.h"
#include "guest.h"
#include "stream_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define FILE_EXISTS(p) (_access((p), 0) == 0)
#else
#include <unistd.h>
#define FILE_EXISTS(p) (access((p), F_OK) == 0)
#endif

#define HOLDER_ADDR 0x1b2e864u
#define HOLDER_OBJ 0x1b30000u
#define LATCH_ADDR 0x12e3560u
#define UGRIDS_ADDR 0x1241000u
#define CELL_OBJ 0x20000000u
#define WS_OBJ 0x20001000u

static int checks;
static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
    ++checks; \
} while (0)

static uint64_t g_now_ns;
static uint64_t fake_clock(void *ctx) {
    (void)ctx;
    return g_now_ns;
}

static void set_tuple(GuestMemory *mem, uint32_t cell, int32_t gx, int32_t gy,
                      uint32_t ws) {
    uint32_t holder = HOLDER_OBJ;
    CHECK(guest_write32(mem, HOLDER_ADDR, holder) == 0);
    CHECK(guest_write32(mem, holder + 0x6c, cell) == 0);
    CHECK(guest_write32(mem, holder + 0x5c, (uint32_t)gx) == 0);
    CHECK(guest_write32(mem, holder + 0x60, (uint32_t)gy) == 0);
    CHECK(guest_write32(mem, holder + 0xbc, ws) == 0);
}

static void drain(const StreamMount *mount) {
    for (unsigned i = 0; i < 128; ++i) {
        g_now_ns += 50ull * 1000000ull;
        uint32_t quanta = 0;
        char reason[STREAM_REASON_MAX];
        (void)stream_mount_pump((StreamMount *)mount, 4, &quanta, reason,
                                sizeof reason);
        if (!quanta) break;
    }
}

int main(int argc, char **argv) {
    const char *index_path = argc > 1 ? argv[1] : getenv("STREAM_INDEX_REAL");
    const char *data = argc > 2 ? argv[2] : getenv("STREAM_DATA_REAL");
    if (!data || !*data) data = getenv("TESV_DATA_ROOT");
    if (!data || !*data) data = "E:/harness_skyrim/Data";
    if (!index_path || !*index_path || !FILE_EXISTS(index_path)) {
        printf("stream_cell_real_prefetch: skipped (set STREAM_INDEX_REAL)\n");
        return 0;
    }

    StreamCellState *state = NULL;
    char reason[STREAM_REASON_MAX];
    StreamStatus st = stream_cell_open(&state, index_path, data, reason,
                                       sizeof reason);
    if (st != STREAM_OK) {
        fprintf(stderr, "stream_cell_real_prefetch: open: %s (%s)\n",
                stream_status_name(st), reason);
        return 1;
    }

    GuestMemory mem;
    guest_init(&mem, 64u << 20);
    CHECK(guest_map(&mem, 0x01200000u, 0x01000000u) == 0);
    CHECK(guest_map(&mem, 0x1b000000u, 0x00100000u) == 0);
    CHECK(guest_map(&mem, 0x20000000u, 0x00100000u) == 0);
    uint32_t ugrids = 5;
    CHECK(guest_write32(&mem, UGRIDS_ADDR, ugrids) == 0);
    uint8_t off = 0;
    CHECK(guest_write(&mem, LATCH_ADDR, &off, 1) == 0);
    CHECK(guest_write32(&mem, CELL_OBJ + 0x0c, 0x133C6u) == 0);
    const char *env_world = getenv("STREAM_CELL_WORLD");
    const char *env_gx = getenv("STREAM_CELL_GX");
    const char *env_gy = getenv("STREAM_CELL_GY");
    uint32_t ext_world = env_world ? (uint32_t)strtoul(env_world, NULL, 0)
                                   : 0x3cu;
    int32_t ext_gx = env_gx ? (int32_t)strtol(env_gx, NULL, 0) : 0;
    int32_t ext_gy = env_gy ? (int32_t)strtol(env_gy, NULL, 0) : 0;
    CHECK(guest_write32(&mem, WS_OBJ + 0x0c, ext_world) == 0);
    set_tuple(&mem, CELL_OBJ, 0, 0, 0);

    g_now_ns = 1000000ull;
    stream_mount_set_clock((StreamMount *)stream_cell_mount(state), fake_clock,
                           NULL);

    const StreamCellMetrics *m = stream_cell_metrics(state);
    StreamMetrics sm;
    stream_mount_metrics(stream_cell_mount(state), &sm);

    int ran = 0;

    /* Interior: RiverwoodSleepingGiantInn (0x133C6). */
    StreamCellKey key;
    memset(&key, 0, sizeof key);
    key.form_id = 0x133C6u;
    StreamCellRow row;
    if (stream_index_find_cell(stream_cell_index(state), &key, &row, reason,
                               sizeof reason) == STREAM_OK &&
        row.resource_count > 0) {
        ran = 1;
        unsigned compressed = 0;
        for (uint32_t i = 0; i < row.resource_count; ++i) {
            StreamResource res;
            if (stream_index_find_id(stream_cell_index(state), row.resource_id[i],
                                     &res, reason, sizeof reason) == STREAM_OK &&
                (res.flags & STREAM_RESOURCE_FLAG_COMPRESSED))
                ++compressed;
        }
        printf("interior 0x133c6: row has %u id(s), %u compressed\n",
               row.resource_count, compressed);

        uint64_t transitions = m->transitions;
        set_tuple(&mem, CELL_OBJ, 0, 0, 0);
        CHECK(stream_cell_tick(state, &mem, reason, sizeof reason) == STREAM_OK);
        CHECK(m->transitions == transitions + 1);
        CHECK(m->last_resource_count == row.resource_count);
        CHECK(m->prefetch_requests > 0);
        CHECK(m->skipped_compressed == 0);

        drain(stream_cell_mount(state));
        stream_mount_metrics(stream_cell_mount(state), &sm);
        CHECK(sm.ready > 0);
        CHECK(sm.request_errors == 0);
        StreamResource first;
        if (stream_index_find_id(stream_cell_index(state), row.resource_id[0],
                                 &first, reason, sizeof reason) == STREAM_OK) {
            char path[STREAM_PATH_MAX + 1];
            uint32_t n = first.path_length < STREAM_PATH_MAX ? first.path_length
                                                             : STREAM_PATH_MAX;
            memcpy(path, first.path, n);
            path[n] = 0;
            uint8_t buf[4096];
            uint32_t want = first.payload_length < sizeof buf
                                ? first.payload_length : (uint32_t)sizeof buf;
            uint32_t got = 0;
            int served = stream_cell_demand_read(state, path, 0, buf, want,
                                                 &got);
            printf("interior demand '%s' served=%d got=%u\n", path, served, got);
            CHECK(served == 1 && got == want && want > 0);
        }
    }

    /* Exterior: Tamriel (0x3c) around the configured grid (default 0,0). */
    memset(&key, 0, sizeof key);
    key.kind = 1;
    key.grid_x = ext_gx;
    key.grid_y = ext_gy;
    key.worldspace_form_id = ext_world;
    if (stream_index_find_cell(stream_cell_index(state), &key, &row, reason,
                               sizeof reason) == STREAM_OK &&
        row.resource_count > 0) {
        ran = 1;
        unsigned compressed = 0;
        for (uint32_t i = 0; i < row.resource_count; ++i) {
            StreamResource res;
            if (stream_index_find_id(stream_cell_index(state), row.resource_id[i],
                                     &res, reason, sizeof reason) == STREAM_OK &&
                (res.flags & STREAM_RESOURCE_FLAG_COMPRESSED))
                ++compressed;
        }
        printf("exterior 0x%x %d,%d: row has %u id(s), %u compressed\n",
               ext_world, (int)ext_gx, (int)ext_gy, row.resource_count,
               compressed);

        uint64_t transitions = m->transitions;
        uint64_t prefetch = m->prefetch_requests;
        set_tuple(&mem, CELL_OBJ, ext_gx, ext_gy, WS_OBJ);
        CHECK(stream_cell_tick(state, &mem, reason, sizeof reason) == STREAM_OK);
        CHECK(m->transitions == transitions + 1);
        CHECK(m->exterior_ticks > 0);
        CHECK(m->last_resource_count == row.resource_count);
        CHECK(m->prefetch_requests >= prefetch + row.resource_count);
        CHECK(m->skipped_compressed == 0);

        drain(stream_cell_mount(state));
        stream_mount_metrics(stream_cell_mount(state), &sm);
        CHECK(sm.physical_reads > 0);
        CHECK(sm.ready > 0);
        CHECK(sm.request_errors == 0);
        StreamResource first;
        if (stream_index_find_id(stream_cell_index(state), row.resource_id[0],
                                 &first, reason, sizeof reason) == STREAM_OK) {
            char path[STREAM_PATH_MAX + 1];
            uint32_t n = first.path_length < STREAM_PATH_MAX ? first.path_length
                                                             : STREAM_PATH_MAX;
            memcpy(path, first.path, n);
            path[n] = 0;
            uint8_t buf[4096];
            uint32_t want = first.payload_length < sizeof buf
                                ? first.payload_length : (uint32_t)sizeof buf;
            uint32_t got = 0;
            int served = stream_cell_demand_read(state, path, 0, buf, want,
                                                 &got);
            printf("exterior demand '%s' served=%d got=%u\n", path, served, got);
            CHECK(served == 1 && got == want && want > 0);
        }
    }

    if (!ran) {
        printf("stream_cell_real_prefetch: index has no 0x133c6 or exterior "
               "row for 0x%x %d,%d; skipped\n", ext_world, (int)ext_gx,
               (int)ext_gy);
        stream_cell_close(state);
        guest_destroy(&mem);
        return 0;
    }

    stream_mount_metrics(stream_cell_mount(state), &sm);
    printf("stream_cell_real_prefetch: prefetch_requests=%llu "
           "skipped_compressed=%llu physical_reads=%llu quanta=%llu "
           "bytes_returned=%llu ready=%llu\n",
           (unsigned long long)m->prefetch_requests,
           (unsigned long long)m->skipped_compressed,
           (unsigned long long)sm.physical_reads,
           (unsigned long long)sm.quanta,
           (unsigned long long)sm.bytes_returned,
           (unsigned long long)sm.ready);

    stream_cell_close(state);
    guest_destroy(&mem);
    printf("stream_cell_real_prefetch: %d checks, %d failure(s)\n", checks,
           failures);
    return failures ? 1 : 0;
}
