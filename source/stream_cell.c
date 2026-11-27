#include "stream_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct StreamCellState {
    StreamIndex *index;
    StreamMount *mount;
    StreamCellMetrics metrics;
    StreamHandle handles[STREAM_CELL_IDS];
    uint32_t handle_count;
    uint32_t working_bytes;
    int have_prev;
    uint32_t prev_cell;
    uint32_t prev_ws;
    int32_t prev_gx;
    int32_t prev_gy;
};

static int env_on(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1' && !v[1];
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

StreamStatus stream_cell_open(StreamCellState **out, const char *index_path,
                              const char *data_dir, char *reason,
                              size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out || !index_path || !data_dir) return STREAM_ERR_ARGUMENT;
    *out = NULL;
    StreamCellState *state = (StreamCellState *)calloc(1, sizeof *state);
    if (!state) return STREAM_ERR_NOMEM;
    StreamStatus st = stream_index_load(index_path, &state->index, reason,
                                        reason_cap);
    if (st != STREAM_OK) {
        free(state);
        return st;
    }
    st = stream_mount_open(state->index, data_dir, &state->mount, reason,
                           reason_cap);
    if (st != STREAM_OK) {
        stream_index_free(state->index);
        free(state);
        return st;
    }
    *out = state;
    return STREAM_OK;
}

void stream_cell_close(StreamCellState *state) {
    if (!state) return;
    stream_mount_close(state->mount);
    stream_index_free(state->index);
    free(state);
}

const StreamCellMetrics *stream_cell_metrics(const StreamCellState *state) {
    return state ? &state->metrics : NULL;
}

const StreamMount *stream_cell_mount(const StreamCellState *state) {
    return state ? state->mount : NULL;
}

const StreamIndex *stream_cell_index(const StreamCellState *state) {
    return state ? state->index : NULL;
}

uint32_t stream_cell_handle_count(const StreamCellState *state) {
    return state ? state->handle_count : 0;
}

StreamHandle stream_cell_handle_at(const StreamCellState *state, uint32_t i) {
    if (!state || i >= state->handle_count) return 0;
    return state->handles[i];
}

static int rd32(GuestMemory *mem, uint32_t addr, uint32_t *v) {
    return mem && v && guest_read32(mem, addr, v) == 0;
}

static int rdi32(GuestMemory *mem, uint32_t addr, int32_t *v) {
    uint32_t raw = 0;
    if (!rd32(mem, addr, &raw)) return 0;
    *v = (int32_t)raw;
    return 1;
}

static void clear_visible(StreamCellState *state) {
    for (uint32_t i = 0; i < state->handle_count; ++i)
        (void)stream_mount_mark_visible(state->mount, state->handles[i], 0,
                                        NULL, 0);
    state->handle_count = 0;
}

static uint32_t prefetch_cell_row(StreamCellState *state,
                                  const StreamCellKey *key, int centre) {
    StreamCellRow row;
    char reason[STREAM_REASON_MAX];
    StreamStatus st = stream_index_find_cell(state->index, key, &row, reason,
                                             sizeof reason);
    if (st != STREAM_OK) {
        ++state->metrics.rows_missing;
        if (centre) {
            state->metrics.last_resource_count = 0;
            state->metrics.visible_handles = state->handle_count;
        }
        return 0;
    }
    ++state->metrics.rows_found;
    uint32_t working = 0;
    for (uint32_t i = 0; i < row.resource_count; ++i) {
        StreamResource res;
        if (stream_index_find_id(state->index, row.resource_id[i], &res, reason,
                                 sizeof reason) != STREAM_OK)
            continue;
        if (res.flags & STREAM_RESOURCE_FLAG_COMPRESSED) {
            ++state->metrics.skipped_compressed;
            continue;
        }
        if (!res.payload_length) continue;
        StreamHandle handle = 0;
        st = stream_mount_prefetch_ex(state->mount, &res, 0, 0, STREAM_PRIO_P0,
                                      0, &handle, reason, sizeof reason);
        if (st != STREAM_OK) continue;
        ++state->metrics.prefetch_requests;
        if (centre && state->handle_count < STREAM_CELL_IDS)
            state->handles[state->handle_count++] = handle;
        uint32_t window = res.payload_length < STREAM_WINDOW_MAX
                              ? res.payload_length
                              : STREAM_WINDOW_MAX;
        working += window;
    }
    if (centre) {
        for (uint32_t i = 0; i < state->handle_count; ++i)
            (void)stream_mount_mark_visible(state->mount, state->handles[i], 1,
                                            NULL, 0);
        state->metrics.visible_handles = state->handle_count;
        state->metrics.last_resource_count = row.resource_count;
    }
    return working;
}

typedef struct {
    int32_t dx, dy;
    int ring, distance;
} GridOffset;

static int offset_before(const GridOffset *a, const GridOffset *b) {
    if (a->ring != b->ring) return a->ring < b->ring;
    if (a->distance != b->distance) return a->distance < b->distance;
    if (a->dy != b->dy) return a->dy < b->dy;
    return a->dx < b->dx;
}

static uint32_t prefetch_exterior(StreamCellState *state, GuestMemory *mem,
                                  int32_t gx, int32_t gy, uint32_t ws) {
    uint32_t ugrids = 0;
    (void)rd32(mem, STREAM_GUEST_UGRIDS_ADDR, &ugrids);
    if (ugrids < 1 || ugrids > 7) ugrids = 5;
    int32_t radius = (int32_t)(ugrids / 2);
    GridOffset offsets[49];
    uint32_t count = 0;
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            GridOffset o;
            o.dx = dx;
            o.dy = dy;
            o.ring = dx < 0 ? -dx : dx;
            int32_t ay = dy < 0 ? -dy : dy;
            if (ay > o.ring) o.ring = ay;
            o.distance = o.ring + (dx < 0 ? -dx : dx) + ay;
            offsets[count++] = o;
        }
    }
    for (uint32_t i = 1; i < count; ++i) {
        GridOffset key = offsets[i];
        uint32_t j = i;
        while (j && offset_before(&key, &offsets[j - 1])) {
            offsets[j] = offsets[j - 1];
            --j;
        }
        offsets[j] = key;
    }
    uint32_t ws_form = 0;
    (void)rd32(mem, ws + STREAM_GUEST_FORM_ID_OFFSET, &ws_form);
    uint32_t working = 0;
    for (uint32_t i = 0; i < count; ++i) {
        StreamCellKey key;
        memset(&key, 0, sizeof key);
        key.kind = 1;
        key.grid_x = gx + offsets[i].dx;
        key.grid_y = gy + offsets[i].dy;
        key.worldspace_form_id = ws_form;
        working += prefetch_cell_row(state, &key, offsets[i].dx == 0 &&
                                                       offsets[i].dy == 0);
    }
    return working;
}

StreamStatus stream_cell_tick(StreamCellState *state, GuestMemory *mem,
                              char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!state || !mem) return STREAM_ERR_ARGUMENT;
    ++state->metrics.ticks;
    uint32_t holder = 0;
    if (!rd32(mem, STREAM_GUEST_HOLDER_ADDR, &holder) || !holder)
        return STREAM_OK;
    uint32_t cell = 0, ws = 0;
    int32_t gx = 0, gy = 0;
    (void)rd32(mem, holder + STREAM_GUEST_CELL_OFFSET, &cell);
    (void)rdi32(mem, holder + STREAM_GUEST_GRID_X_OFFSET, &gx);
    (void)rdi32(mem, holder + STREAM_GUEST_GRID_Y_OFFSET, &gy);
    (void)rd32(mem, holder + STREAM_GUEST_WORLDSPACE_OFFSET, &ws);
    uint8_t latch = 0;
    (void)guest_read(mem, STREAM_GUEST_GRID_MOVED_ADDR, &latch, 1);
    if (!cell && !ws) return STREAM_OK;   /* no cell committed yet */

    int changed = !state->have_prev || state->prev_cell != cell ||
                  state->prev_ws != ws || state->prev_gx != gx ||
                  state->prev_gy != gy;
    int edge = latch == 1;
    if (!changed && !edge) return STREAM_OK;
    if (changed) ++state->metrics.transitions;
    if (edge) ++state->metrics.moved_edges;

    clear_visible(state);
    uint32_t working = 0;
    if (ws == 0) {
        StreamCellKey key;
        memset(&key, 0, sizeof key);
        key.kind = 0;
        if (!rd32(mem, cell + STREAM_GUEST_FORM_ID_OFFSET, &key.form_id))
            return STREAM_OK;
        working = prefetch_cell_row(state, &key, 1);
    } else {
        ++state->metrics.exterior_ticks;
        working = prefetch_exterior(state, mem, gx, gy, ws);
    }
    char pump_reason[STREAM_REASON_MAX];
    uint32_t quanta = 0;
    (void)stream_mount_pump(state->mount, 2, &quanta, pump_reason,
                            sizeof pump_reason);
    if (working < state->working_bytes) {
        uint32_t freed = 0;
        (void)stream_mount_trim(state->mount, working, &freed, pump_reason,
                                sizeof pump_reason);
        ++state->metrics.trim_events;
        state->metrics.trim_bytes += freed;
    }
    state->working_bytes = working;
    state->prev_cell = cell;
    state->prev_ws = ws;
    state->prev_gx = gx;
    state->prev_gy = gy;
    state->have_prev = 1;
    return STREAM_OK;
}

int stream_cell_demand_read(StreamCellState *state, const char *canonical_path,
                            uint32_t rel, void *dst, uint32_t n,
                            uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!state || !canonical_path) return 0;
    int served = 0;
    char reason[STREAM_REASON_MAX];
    StreamStatus st = stream_mount_demand_path(state->mount, canonical_path, rel,
                                               dst, n, out_len, &served,
                                               reason, sizeof reason);
    if (served) {
        ++state->metrics.demand_hits;
        return 1;
    }
    ++state->metrics.demand_misses;
    if (st == STREAM_ERR_UNSUPPORTED) ++state->metrics.demand_cancelled;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Weak host hooks                                                     */

static StreamCellState *g_host_state;
static int g_host_init_done;

static void host_ensure(void) {
    if (g_host_init_done) return;
    g_host_init_done = 1;
    if (!env_on("N3DS_STREAM")) return;
    char reason[STREAM_REASON_MAX] = "";
    const char *index = env_or("N3DS_STREAM_INDEX",
                               "sdmc:/Skyrim/stream_index.bin");
    const char *data = env_or("N3DS_STREAM_DATA", "sdmc:/Skyrim/Data");
    StreamStatus st = stream_cell_open(&g_host_state, index, data, reason,
                                       sizeof reason);
    if (st != STREAM_OK)
        fprintf(stderr, "[stream] disabled: %s (%s)\n",
                stream_status_name(st), reason[0] ? reason : "no reason");
}

void stream_host_frame_tick(GuestMemory *mem) {
    host_ensure();
    if (!g_host_state || !mem) return;
    char reason[STREAM_REASON_MAX];
    (void)stream_cell_tick(g_host_state, mem, reason, sizeof reason);
}

static int ci_equal(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

/* "<root>/Data/meshes/x.nif" -> "meshes\\x.nif".  Returns 0 for paths that do
 * not live under a Data component. */
static int canonical_from_host(const char *host, char *out, size_t cap) {
    if (!host || !out || cap < 2) return 0;
    const char *p = NULL;
    for (const char *s = host; *s; ++s) {
        if ((s == host || s[-1] == '/' || s[-1] == '\\') &&
            ci_equal(s[0], 'd') && ci_equal(s[1], 'a') && ci_equal(s[2], 't') &&
            ci_equal(s[3], 'a') && (s[4] == '/' || s[4] == '\\')) {
            p = s + 5;
            break;
        }
    }
    if (!p) return 0;
    size_t len = strlen(p);
    while (len && (p[len - 1] == '/' || p[len - 1] == '\\')) --len;
    if (!len || len >= cap) return 0;
    for (size_t i = 0; i < len; ++i) {
        char c = p[i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = c;
    }
    out[len] = 0;
    if (strstr(out, "\\\\") || !strncmp(out, "..", 2) ||
        strstr(out, "\\.."))
        return 0;
    return 1;
}

int stream_host_note_read(const char *host_path, uint32_t rel, void *dst,
                          uint32_t n, uint32_t *out_len) {
    host_ensure();
    if (out_len) *out_len = 0;
    if (!g_host_state || !host_path || !dst || !n) return 0;
    char canon[STREAM_PATH_MAX];
    if (!canonical_from_host(host_path, canon, sizeof canon)) return 0;
    return stream_cell_demand_read(g_host_state, canon, rel, dst, n, out_len);
}
