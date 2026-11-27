#include "asset_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t name_off;
    uint32_t name_len;
    uint64_t bytes;
    uint32_t priority;
    uint32_t files;
} StreamArchiveRec;

struct StreamIndex {
    const uint8_t *blob;
    size_t size;
    int owned;
    StreamIndexInfo info;
    uint32_t archive_off;
    uint32_t resource_off;
    uint32_t pool_off;
    uint32_t pool_len;
    uint32_t cell_off;
    uint32_t neg_hash[STREAM_NEGATIVE_SLOTS];
    uint8_t neg_used[STREAM_NEGATIVE_SLOTS];
    uint64_t neg_generation;
};

typedef struct StreamRequest {
    uint8_t used;
    uint8_t state;
    uint8_t priority;
    uint8_t requested_priority;
    uint8_t pinned;
    uint8_t visible;
    uint8_t entry_valid;
    uint8_t pad;
    uint16_t consumers;
    uint32_t epoch;
    uint32_t seq;
    StreamRequestKey key;
    StreamResource res;
    BsaEntry entry;
    uint8_t *data;
    uint32_t target;
    uint32_t fetched;
    uint32_t quanta;
    uint64_t deadline_ms;
    uint64_t enqueued_ms;
    uint64_t last_touch_ms;
} StreamRequest;

struct StreamMount {
    const StreamIndex *index;
    char *data_dir;
    BsaArchive **archives;
    StreamMetrics metrics;
    StreamConfig config;
    StreamClockFn clock;
    void *clock_ctx;
    StreamVerifyFn verify;
    void *verify_ctx;
    StreamRequest requests[STREAM_MAX_REQUESTS];
    uint8_t *io[STREAM_IO_BUFFERS];
    uint32_t io_cur;
    uint32_t descriptors;
    uint32_t cache_bytes;
    uint32_t peak_cache_bytes;
    uint64_t budget_bytes;
    uint64_t budget_ns;
    uint64_t seq;
    int last_archive;
    int last_tail_valid;
    uint64_t last_tail;
};

static StreamMount *g_global_mount;

static uint64_t default_clock_ns(void *ctx) {
    (void)ctx;
    clock_t now = clock();
    if (now == (clock_t)-1) return 0;
    return (uint64_t)now * 1000000000ull / (uint64_t)CLOCKS_PER_SEC;
}

const char *stream_priority_name(StreamPriority priority) {
    switch (priority) {
    case STREAM_PRIO_DEMAND: return "demand";
    case STREAM_PRIO_P0: return "p0";
    case STREAM_PRIO_P1: return "p1";
    case STREAM_PRIO_P2: return "p2";
    default: return "unknown";
    }
}

const char *stream_request_state_name(StreamRequestState state) {
    switch (state) {
    case STREAM_REQ_QUEUED: return "queued";
    case STREAM_REQ_READING: return "reading";
    case STREAM_REQ_VERIFIED: return "verified";
    case STREAM_REQ_READY: return "ready";
    case STREAM_REQ_ERROR: return "error";
    case STREAM_REQ_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

void stream_mount_config_default(StreamConfig *out) {
    if (!out) return;
    out->rate_bytes_per_sec = STREAM_RATE_DEFAULT;
    out->quantum_bytes = STREAM_QUANTUM_MAX;
    out->window_max_bytes = STREAM_WINDOW_MAX;
    out->cache_limit_bytes = STREAM_CACHE_LIMIT_DEFAULT;
    out->max_accrual_quanta = STREAM_MAX_ACCRUAL_QUANTA;
}

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *stream_status_name(StreamStatus status) {
    switch (status) {
    case STREAM_OK: return "ok";
    case STREAM_ERR_ARGUMENT: return "bad argument";
    case STREAM_ERR_ACCESS: return "cannot open file";
    case STREAM_ERR_IO: return "I/O error";
    case STREAM_ERR_MAGIC: return "not a stream index";
    case STREAM_ERR_SCHEMA: return "unsupported stream index schema";
    case STREAM_ERR_BOUNDS: return "table bounds violated";
    case STREAM_ERR_FORMAT: return "stream index layout mismatch";
    case STREAM_ERR_NOT_FOUND: return "resource not found";
    case STREAM_ERR_UNSUPPORTED: return "not implemented in phase 1";
    case STREAM_ERR_NOMEM: return "out of memory";
    case STREAM_ERR_BUDGET: return "configured limit exceeded";
    default: return "unknown stream error";
    }
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* Canonical device path: lowercase, backslash separators, no edges. */
static int canon_path(const char *in, char *out, size_t cap) {
    if (!in || !out || cap < 2) return 0;
    size_t n = strlen(in);
    size_t start = 0, end = n;
    while (start < end && (in[start] == '\\' || in[start] == '/')) ++start;
    while (end > start && (in[end - 1] == '\\' || in[end - 1] == '/')) --end;
    size_t len = end - start;
    if (!len || len >= cap) return 0;
    for (size_t i = 0; i < len; ++i) {
        char c = in[start + i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = c;
    }
    out[len] = 0;
    /* Reject empty, "." and ".." components. */
    size_t component = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || out[i] == '\\') {
            size_t clen = i - component;
            if (!clen) return 0;
            if (clen == 1 && out[component] == '.') return 0;
            if (clen == 2 && out[component] == '.' && out[component + 1] == '.')
                return 0;
            component = i + 1;
        }
    }
    return 1;
}

static uint32_t path_hash(const char *path) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static int stored_path_ok(const uint8_t *p, uint32_t len) {
    if (!len || len >= STREAM_PATH_MAX) return 0;
    size_t component = 0;
    for (uint32_t i = 0; i <= len; ++i) {
        if (i == len || p[i] == '\\') {
            size_t clen = i - component;
            if (!clen) return 0;
            if (clen == 1 && p[component] == '.') return 0;
            if (clen == 2 && p[component] == '.' && p[component + 1] == '.')
                return 0;
            component = i + 1;
            continue;
        }
        char c = (char)p[i];
        if (c >= 'A' && c <= 'Z') return 0;
        if (c == '/') return 0;
    }
    return 1;
}

static int path_compare(const uint8_t *stored, uint32_t len, const char *want) {
    size_t i = 0;
    for (; i < len && want[i]; ++i) {
        int a = stored[i], b = (unsigned char)want[i];
        if (a != b) return a < b ? -1 : 1;
    }
    if (i < len) return 1;      /* stored is longer */
    if (want[i]) return -1;     /* want is longer */
    return 0;
}

static StreamArchiveRec archive_rec(const StreamIndex *index, uint32_t i) {
    StreamArchiveRec rec;
    const uint8_t *p = index->blob + index->archive_off +
                       (size_t)i * STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    rec.name_off = le32(p + 0);
    rec.name_len = le32(p + 4);
    rec.bytes = le64(p + 8);
    rec.priority = le32(p + 48);
    rec.files = le32(p + 52);
    return rec;
}

static StreamStatus fill_resource(const StreamIndex *index, uint32_t i,
                                  StreamResource *out) {
    const uint8_t *p = index->blob + index->resource_off +
                       (size_t)i * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    out->id = le32(p + 0);
    out->path = (const char *)(index->blob + index->pool_off + le32(p + 4));
    out->path_length = le32(p + 8);
    out->archive_index = le32(p + 12);
    out->physical_offset = le64(p + 16);
    out->payload_length = le32(p + 24);
    out->stored_size = le32(p + 28);
    out->entry_offset = le32(p + 32);
    out->flags = le32(p + 36);
    out->sha256 = p + 40;
    out->source_sha256 = p + 72;
    return STREAM_OK;
}

static StreamCellKey cell_key_read(const uint8_t *p) {
    StreamCellKey key;
    key.kind = le32(p + 0);
    key.form_id = le32(p + 4);
    key.grid_x = (int32_t)le32(p + 8);
    key.grid_y = (int32_t)le32(p + 12);
    key.worldspace_form_id = le32(p + 16);
    key.reserved = le32(p + 20);
    return key;
}

static int cell_key_compare(const StreamCellKey *a, const StreamCellKey *b) {
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->form_id != b->form_id) return a->form_id < b->form_id ? -1 : 1;
    if (a->grid_x != b->grid_x) return a->grid_x < b->grid_x ? -1 : 1;
    if (a->grid_y != b->grid_y) return a->grid_y < b->grid_y ? -1 : 1;
    if (a->worldspace_form_id != b->worldspace_form_id)
        return a->worldspace_form_id < b->worldspace_form_id ? -1 : 1;
    return 0;
}

static StreamStatus fill_cell_row(const StreamIndex *index, uint32_t i,
                                  StreamCellRow *out) {
    const uint8_t *p = index->blob + index->cell_off +
                       (size_t)i * STREAM_INDEX_CELL_ENTRY_SIZE;
    memset(out, 0, sizeof *out);
    out->key = cell_key_read(p);
    out->resource_count = le32(p + 24);
    out->flags = le32(p + 28);
    for (uint32_t k = 0; k < STREAM_CELL_IDS; ++k)
        out->resource_id[k] = le32(p + 32 + 4 * k);
    return STREAM_OK;
}

static StreamStatus index_validate(StreamIndex *index, char *reason,
                                    size_t cap) {
    const uint8_t *b = index->blob;
    if (index->size < STREAM_INDEX_HEADER_SIZE + 4) {
        set_reason(reason, cap, "index is shorter than its header");
        return STREAM_ERR_BOUNDS;
    }
    if (memcmp(b, "SRNX", 4) != 0) {
        set_reason(reason, cap, "bad index magic %.4s", (const char *)b);
        return STREAM_ERR_MAGIC;
    }
    uint16_t schema = le16(b + 4);
    uint16_t header_size = le16(b + 6);
    uint16_t archive_size = le16(b + 8);
    uint16_t resource_size = le16(b + 10);
    uint16_t cell_size = le16(b + 12);
    uint16_t flags = le16(b + 14);
    uint32_t archive_count = le32(b + 16);
    uint32_t resource_count = le32(b + 20);
    uint32_t cell_count = le32(b + 24);
    uint64_t total = le64(b + 72);
    if (schema < STREAM_INDEX_SCHEMA_MIN || schema > STREAM_INDEX_SCHEMA) {
        set_reason(reason, cap, "unsupported index schema %u", schema);
        return STREAM_ERR_SCHEMA;
    }
    if (header_size != STREAM_INDEX_HEADER_SIZE ||
        archive_size != STREAM_INDEX_ARCHIVE_ENTRY_SIZE ||
        resource_size != STREAM_INDEX_RESOURCE_ENTRY_SIZE ||
        cell_size != STREAM_INDEX_CELL_ENTRY_SIZE) {
        set_reason(reason, cap, "index entry sizes do not match schema 1/2");
        return STREAM_ERR_SCHEMA;
    }
    if (flags != 0x0001u) {
        set_reason(reason, cap, "unsupported index flags 0x%04x", flags);
        return STREAM_ERR_SCHEMA;
    }
    if (le32(b + 28) != 0) {
        set_reason(reason, cap, "reserved header field is not zero");
        return STREAM_ERR_FORMAT;
    }
    if (total != index->size) {
        set_reason(reason, cap, "index declares %llu bytes, buffer has %llu",
                   (unsigned long long)total,
                   (unsigned long long)index->size);
        return STREAM_ERR_BOUNDS;
    }
    if (bsa_crc32(b, index->size - 4) != le32(b + index->size - 4)) {
        set_reason(reason, cap, "index CRC-32 mismatch");
        return STREAM_ERR_FORMAT;
    }
    uint32_t archive_off = (uint32_t)le64(b + 40);
    uint32_t resource_off = (uint32_t)le64(b + 48);
    uint32_t pool_off = (uint32_t)le64(b + 56);
    uint32_t cell_off = (uint32_t)le64(b + 64);
    if (le64(b + 40) != archive_off || le64(b + 48) != resource_off ||
        le64(b + 56) != pool_off || le64(b + 64) != cell_off) {
        set_reason(reason, cap, "index table offset exceeds 32 bits");
        return STREAM_ERR_BOUNDS;
    }
    if (archive_count > STREAM_MAX_ARCHIVES ||
        resource_count > STREAM_MAX_RESOURCES ||
        cell_count > STREAM_MAX_CELLS) {
        set_reason(reason, cap, "implausible index counts %u/%u/%u",
                   archive_count, resource_count, cell_count);
        return STREAM_ERR_BOUNDS;
    }
    uint64_t want_archive = STREAM_INDEX_HEADER_SIZE;
    uint64_t want_resource = want_archive +
        (uint64_t)archive_count * STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    uint64_t want_pool = want_resource +
        (uint64_t)resource_count * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    uint64_t want_cell = (uint64_t)cell_off + (uint64_t)cell_count *
        STREAM_INDEX_CELL_ENTRY_SIZE + 4;
    if (archive_off != want_archive || resource_off != want_resource ||
        pool_off != want_pool) {
        set_reason(reason, cap, "index table offsets are inconsistent");
        return STREAM_ERR_FORMAT;
    }
    if (cell_off < pool_off || (cell_off & 7u) ||
        want_cell != index->size) {
        set_reason(reason, cap, "index cell table is misaligned or truncated");
        return STREAM_ERR_BOUNDS;
    }
    index->archive_off = archive_off;
    index->resource_off = resource_off;
    index->pool_off = pool_off;
    index->pool_len = cell_off - pool_off;
    index->cell_off = cell_off;
    index->info.generation = le64(b + 32);
    index->info.archive_count = archive_count;
    index->info.resource_count = resource_count;
    index->info.cell_count = cell_count;

    uint32_t counts[STREAM_MAX_ARCHIVES];
    memset(counts, 0, sizeof counts);
    for (uint32_t i = 0; i < archive_count; ++i) {
        StreamArchiveRec rec = archive_rec(index, i);
        if (!rec.name_len || rec.name_off > index->pool_len ||
            rec.name_len > index->pool_len - rec.name_off) {
            set_reason(reason, cap, "archive %u name is outside the pool", i);
            return STREAM_ERR_BOUNDS;
        }
        if (rec.priority != i) {
            set_reason(reason, cap, "archive %u priority is not its index", i);
            return STREAM_ERR_FORMAT;
        }
    }
    const uint8_t *pool = b + pool_off;
    const uint8_t *last = NULL;
    uint32_t last_len = 0;
    for (uint32_t i = 0; i < resource_count; ++i) {
        const uint8_t *p = b + resource_off +
                           (size_t)i * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
        uint32_t id = le32(p + 0);
        uint32_t path_off = le32(p + 4);
        uint32_t path_len = le32(p + 8);
        uint32_t archive = le32(p + 12);
        uint64_t physical = le64(p + 16);
        uint32_t payload = le32(p + 24);
        if (id != i) {
            set_reason(reason, cap, "resource %u has id %u", i, id);
            return STREAM_ERR_FORMAT;
        }
        if (archive >= archive_count) {
            set_reason(reason, cap, "resource %u archive index is invalid", i);
            return STREAM_ERR_BOUNDS;
        }
        if (!stored_path_ok(pool + path_off, path_len) ||
            path_off > index->pool_len ||
            path_len > index->pool_len - path_off) {
            set_reason(reason, cap, "resource %u path is not canonical", i);
            return STREAM_ERR_FORMAT;
        }
        if (last && path_compare(last, last_len, (const char *)pool + path_off)
                       >= 0) {
            set_reason(reason, cap, "resource paths are not strictly sorted");
            return STREAM_ERR_FORMAT;
        }
        last = pool + path_off;
        last_len = path_len;
        StreamArchiveRec rec = archive_rec(index, archive);
        if (physical > rec.bytes || (uint64_t)payload > rec.bytes - physical) {
            set_reason(reason, cap, "resource %u range %llu+%u is outside %s",
                       i, (unsigned long long)physical, payload,
                       (const char *)pool + rec.name_off);
            return STREAM_ERR_BOUNDS;
        }
        if (physical + payload > 0xffffffffull) {
            set_reason(reason, cap, "resource %u needs a 33-bit offset", i);
            return STREAM_ERR_BOUNDS;
        }
        ++counts[archive];
    }
    for (uint32_t i = 0; i < archive_count; ++i) {
        StreamArchiveRec rec = archive_rec(index, i);
        if (rec.files != counts[i]) {
            set_reason(reason, cap, "archive %u declares %u files, %u rows",
                       i, rec.files, counts[i]);
            return STREAM_ERR_FORMAT;
        }
    }
    int have_last = 0;
    StreamCellKey last_key;
    for (uint32_t i = 0; i < cell_count; ++i) {
        const uint8_t *p = b + cell_off +
                           (size_t)i * STREAM_INDEX_CELL_ENTRY_SIZE;
        StreamCellKey key = cell_key_read(p);
        uint32_t ids = le32(p + 24);
        uint32_t cell_flags = le32(p + 28);
        if (key.kind > 1 || key.reserved != 0) {
            set_reason(reason, cap, "cell %u key kind/reserved is invalid", i);
            return STREAM_ERR_FORMAT;
        }
        if (ids > STREAM_CELL_IDS || cell_flags != 0) {
            set_reason(reason, cap, "cell %u has %u ids / flags 0x%x", i, ids,
                       cell_flags);
            return STREAM_ERR_FORMAT;
        }
        if (have_last && cell_key_compare(&last_key, &key) >= 0) {
            set_reason(reason, cap, "cell keys are not strictly sorted at %u", i);
            return STREAM_ERR_FORMAT;
        }
        for (uint32_t k = 0; k < ids; ++k) {
            uint32_t id = le32(p + 32 + 4 * k);
            if (id >= resource_count) {
                set_reason(reason, cap, "cell %u id %u is outside the table",
                           i, id);
                return STREAM_ERR_BOUNDS;
            }
        }
        last_key = key;
        have_last = 1;
    }
    return STREAM_OK;
}

StreamStatus stream_index_parse(const void *blob, size_t size,
                                StreamIndex **out, char *reason,
                                size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!blob || !out || size < STREAM_INDEX_HEADER_SIZE + 4) {
        set_reason(reason, reason_cap, "parse needs a non-empty index buffer");
        return STREAM_ERR_ARGUMENT;
    }
    *out = NULL;
    StreamIndex *index = (StreamIndex *)calloc(1, sizeof *index);
    if (!index) return STREAM_ERR_NOMEM;
    index->blob = (const uint8_t *)blob;
    index->size = size;
    index->neg_generation = 1;   /* no cache until a parse succeeds */
    StreamStatus st = index_validate(index, reason, reason_cap);
    if (st != STREAM_OK) {
        free(index);
        return st;
    }
    index->neg_generation = index->info.generation;
    *out = index;
    return STREAM_OK;
}

static StreamStatus read_whole_file(const char *path, uint8_t **out,
                                    size_t *out_size, char *reason,
                                    size_t reason_cap) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_reason(reason, reason_cap, "cannot open '%s'", path);
        return STREAM_ERR_ACCESS;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_reason(reason, reason_cap, "cannot seek '%s'", path);
        return STREAM_ERR_IO;
    }
    long end = ftell(file);
    if (end <= 0) {
        fclose(file);
        set_reason(reason, reason_cap, "'%s' is empty", path);
        return STREAM_ERR_IO;
    }
    if ((unsigned long)end > 64u * 1024u * 1024u) {
        fclose(file);
        set_reason(reason, reason_cap, "'%s' is implausibly large", path);
        return STREAM_ERR_BUDGET;
    }
    rewind(file);
    uint8_t *data = (uint8_t *)malloc((size_t)end);
    if (!data) {
        fclose(file);
        return STREAM_ERR_NOMEM;
    }
    if (fread(data, 1, (size_t)end, file) != (size_t)end) {
        free(data);
        fclose(file);
        set_reason(reason, reason_cap, "short read from '%s'", path);
        return STREAM_ERR_IO;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)end;
    return STREAM_OK;
}

StreamStatus stream_index_load(const char *path, StreamIndex **out,
                               char *reason, size_t reason_cap) {
    if (!path || !out) {
        set_reason(reason, reason_cap, "load needs a path and an output");
        return STREAM_ERR_ARGUMENT;
    }
    *out = NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    StreamStatus st = read_whole_file(path, &data, &size, reason, reason_cap);
    if (st != STREAM_OK) return st;
    st = stream_index_parse(data, size, out, reason, reason_cap);
    if (st != STREAM_OK) {
        free(data);
        return st;
    }
    (*out)->owned = 1;
    return STREAM_OK;
}

void stream_index_free(StreamIndex *index) {
    if (!index) return;
    if (index->owned) free((void *)index->blob);
    free(index);
}

const StreamIndexInfo *stream_index_info(const StreamIndex *index) {
    return index ? &index->info : NULL;
}

StreamStatus stream_index_archive(const StreamIndex *index, uint32_t slot,
                                  const char **name, uint64_t *bytes,
                                  const uint8_t **sha256) {
    if (!index || slot >= index->info.archive_count) return STREAM_ERR_ARGUMENT;
    StreamArchiveRec rec = archive_rec(index, slot);
    if (name) *name = (const char *)(index->blob + index->pool_off + rec.name_off);
    if (bytes) *bytes = rec.bytes;
    if (sha256) *sha256 = index->blob + index->archive_off +
                           (size_t)slot * STREAM_INDEX_ARCHIVE_ENTRY_SIZE + 16;
    return STREAM_OK;
}

static StreamStatus index_find_path_impl(StreamIndex *index,
                                         const char *path,
                                         StreamResource *out, int *cached,
                                         char *reason, size_t reason_cap) {
    if (cached) *cached = 0;
    if (!index || !path || !out) return STREAM_ERR_ARGUMENT;
    char canon[STREAM_PATH_MAX];
    if (!canon_path(path, canon, sizeof canon)) {
        set_reason(reason, reason_cap, "invalid lookup path '%s'", path);
        return STREAM_ERR_ARGUMENT;
    }
    uint32_t h = path_hash(canon);
    uint32_t slot = h % STREAM_NEGATIVE_SLOTS;
    if (index->neg_used[slot] && index->neg_hash[slot] == h &&
        index->neg_generation == index->info.generation) {
        if (cached) *cached = 1;
        set_reason(reason, reason_cap,
                   "negative cache: '%s' absent in generation %llu", canon,
                   (unsigned long long)index->info.generation);
        return STREAM_ERR_NOT_FOUND;
    }
    uint32_t lo = 0, hi = index->info.resource_count;
    const uint8_t *pool = index->blob + index->pool_off;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *p = index->blob + index->resource_off +
                           (size_t)mid * STREAM_INDEX_RESOURCE_ENTRY_SIZE;
        uint32_t path_off = le32(p + 4);
        uint32_t path_len = le32(p + 8);
        int order = path_compare(pool + path_off, path_len, canon);
        if (order < 0) {
            lo = mid + 1;
        } else if (order > 0) {
            hi = mid;
        } else {
            return fill_resource(index, mid, out);
        }
    }
    index->neg_hash[slot] = h;
    index->neg_used[slot] = 1;
    set_reason(reason, reason_cap, "'%s' is not in the cooked index", canon);
    return STREAM_ERR_NOT_FOUND;
}

StreamStatus stream_index_find_path(const StreamIndex *index, const char *path,
                                    StreamResource *out, char *reason,
                                    size_t reason_cap) {
    /* The miss cache is mutable state owned by the index; the rest of the
     * index is immutable for its generation. */
    return index_find_path_impl((StreamIndex *)index, path, out, NULL, reason,
                                reason_cap);
}

StreamStatus stream_index_find_id(const StreamIndex *index, uint32_t id,
                                  StreamResource *out, char *reason,
                                  size_t reason_cap) {
    if (!index || !out) return STREAM_ERR_ARGUMENT;
    if (id >= index->info.resource_count) {
        set_reason(reason, reason_cap, "resource id %u is outside the table", id);
        return STREAM_ERR_NOT_FOUND;
    }
    return fill_resource(index, id, out);
}

StreamStatus stream_index_find_cell(const StreamIndex *index,
                                    const StreamCellKey *key,
                                    StreamCellRow *out, char *reason,
                                    size_t reason_cap) {
    if (!index || !key || !out) return STREAM_ERR_ARGUMENT;
    if (reason && reason_cap) reason[0] = 0;
    uint32_t lo = 0, hi = index->info.cell_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *p = index->blob + index->cell_off +
                           (size_t)mid * STREAM_INDEX_CELL_ENTRY_SIZE;
        StreamCellKey have = cell_key_read(p);
        int order = cell_key_compare(&have, key);
        if (order < 0) {
            lo = mid + 1;
        } else if (order > 0) {
            hi = mid;
        } else {
            return fill_cell_row(index, mid, out);
        }
    }
    set_reason(reason, reason_cap, "no cell row for this key");
    return STREAM_ERR_NOT_FOUND;
}

void stream_index_negative_reset(StreamIndex *index) {
    if (!index) return;
    memset(index->neg_used, 0, sizeof index->neg_used);
    memset(index->neg_hash, 0, sizeof index->neg_hash);
    index->neg_generation = index->info.generation;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */

static uint64_t file_size_of(const char *path, int *exists) {
    *exists = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    *exists = 1;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long end = ftell(file);
    fclose(file);
    return end > 0 ? (uint64_t)end : 0;
}

static StreamStatus resource_path_copy(const StreamResource *res, char *out,
                                       size_t cap, char *reason,
                                       size_t reason_cap) {
    if (!res->path || !res->path_length || res->path_length >= cap) {
        set_reason(reason, reason_cap, "resource path length %u is invalid",
                   res->path ? res->path_length : 0u);
        return STREAM_ERR_FORMAT;
    }
    memcpy(out, res->path, res->path_length);
    out[res->path_length] = 0;
    return STREAM_OK;
}

static StreamStatus mount_archive(StreamMount *mount, uint32_t slot,
                                  BsaArchive **out, char *reason,
                                  size_t reason_cap) {
    if (slot >= mount->index->info.archive_count)
        return STREAM_ERR_ARGUMENT;
    if (mount->archives[slot]) {
        *out = mount->archives[slot];
        return STREAM_OK;
    }
    /* The pool concatenates names without terminators; use the record
     * length instead of reading past the archive name. */
    StreamArchiveRec rec = archive_rec(mount->index, slot);
    char name[STREAM_PATH_MAX];
    if (!rec.name_len || rec.name_len >= sizeof name ||
        rec.name_off > mount->index->pool_len ||
        rec.name_len > mount->index->pool_len - rec.name_off) {
        set_reason(reason, reason_cap, "archive %u name is not bounded", slot);
        return STREAM_ERR_BOUNDS;
    }
    memcpy(name, mount->index->blob + mount->index->pool_off + rec.name_off,
           rec.name_len);
    name[rec.name_len] = 0;
    uint64_t bytes = rec.bytes;
    char path[STREAM_PATH_MAX * 2];
    int n = snprintf(path, sizeof path, "%s/%s", mount->data_dir, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        set_reason(reason, reason_cap, "archive path is too long");
        return STREAM_ERR_BOUNDS;
    }
    int exists = 0;
    uint64_t real = file_size_of(path, &exists);
    if (!exists) {
        set_reason(reason, reason_cap, "archive '%s' does not exist", path);
        return STREAM_ERR_ACCESS;
    }
    if (real != bytes) {
        set_reason(reason, reason_cap,
                   "archive '%s' is %llu bytes, generation says %llu",
                   name, (unsigned long long)real, (unsigned long long)bytes);
        return STREAM_ERR_FORMAT;
    }
    BsaArchive *archive = NULL;
    BsaStatus st = bsa_open_path(path, NULL, &archive, reason, reason_cap);
    if (st != BSA_OK) return STREAM_ERR_FORMAT;
    mount->archives[slot] = archive;
    ++mount->metrics.archive_opens;
    *out = archive;
    return STREAM_OK;
}

StreamStatus stream_mount_open(const StreamIndex *index, const char *data_dir,
                               StreamMount **out, char *reason,
                               size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!index || !data_dir || !*data_dir || !out) {
        set_reason(reason, reason_cap, "mount needs an index and a data dir");
        return STREAM_ERR_ARGUMENT;
    }
    *out = NULL;
    StreamMount *mount = (StreamMount *)calloc(1, sizeof *mount);
    if (!mount) return STREAM_ERR_NOMEM;
    size_t len = strlen(data_dir);
    mount->data_dir = (char *)malloc(len + 1);
    mount->archives = (BsaArchive **)calloc(index->info.archive_count ?
                                            index->info.archive_count : 1,
                                            sizeof(BsaArchive *));
    for (uint32_t i = 0; i < STREAM_IO_BUFFERS; ++i)
        mount->io[i] = (uint8_t *)malloc(STREAM_IO_BUFFER_BYTES);
    if (!mount->data_dir || !mount->archives || !mount->io[0] || !mount->io[1]) {
        for (uint32_t i = 0; i < STREAM_IO_BUFFERS; ++i) free(mount->io[i]);
        free(mount->data_dir);
        free(mount->archives);
        free(mount);
        return STREAM_ERR_NOMEM;
    }
    memcpy(mount->data_dir, data_dir, len + 1);
    while (len > 1 && (mount->data_dir[len - 1] == '/' ||
                       mount->data_dir[len - 1] == '\\')) {
        mount->data_dir[--len] = 0;
    }
    mount->index = index;
    stream_mount_config_default(&mount->config);
    mount->clock = default_clock_ns;
    mount->clock_ctx = NULL;
    mount->last_archive = -1;
    mount->budget_bytes = (uint64_t)mount->config.quantum_bytes *
                          mount->config.max_accrual_quanta;
    mount->budget_ns = mount->clock(mount->clock_ctx);
    *out = mount;
    /* The newest mount is the one diagnostic snapshots sample; a second mount
     * can still override with stream_set_global_mount(). */
    g_global_mount = mount;
    return STREAM_OK;
}

void stream_mount_close(StreamMount *mount) {
    if (!mount) return;
    if (g_global_mount == mount) g_global_mount = NULL;
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        if (mount->requests[i].used && mount->requests[i].data) {
            free(mount->requests[i].data);
            mount->requests[i].data = NULL;
            ++mount->metrics.frees;
        }
    }
    for (uint32_t i = 0; i < STREAM_IO_BUFFERS; ++i) free(mount->io[i]);
    if (mount->archives) {
        for (uint32_t i = 0; i < mount->index->info.archive_count; ++i)
            if (mount->archives[i]) bsa_close(mount->archives[i]);
    }
    free(mount->archives);
    free(mount->data_dir);
    free(mount);
}

StreamStatus stream_mount_find(StreamMount *mount, const char *path,
                               StreamResource *out, char *reason,
                               size_t reason_cap) {
    if (!mount || !path || !out) return STREAM_ERR_ARGUMENT;
    int cached = 0;
    StreamStatus st = index_find_path_impl((StreamIndex *)mount->index, path,
                                           out, &cached, reason, reason_cap);
    if (st == STREAM_ERR_NOT_FOUND) {
        if (cached) ++mount->metrics.negative_hits;
        else ++mount->metrics.misses;
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Phase 2: queue, budget, executor, readiness and eviction            */

static uint64_t mount_now_ns(const StreamMount *mount) {
    if (!mount->clock) return 0;
    return mount->clock(mount->clock_ctx);
}

static uint64_t mount_now_ms(const StreamMount *mount) {
    return mount_now_ns(mount) / 1000000ull;
}

uint64_t stream_mount_now_ms(const StreamMount *mount) {
    return mount ? mount_now_ms(mount) : 0;
}

void stream_mount_set_clock(StreamMount *mount, StreamClockFn clock, void *ctx) {
    if (!mount) return;
    mount->clock = clock ? clock : default_clock_ns;
    mount->clock_ctx = clock ? ctx : NULL;
    mount->budget_ns = mount_now_ns(mount);
}

void stream_mount_set_verify(StreamMount *mount, StreamVerifyFn verify,
                             void *ctx) {
    if (!mount) return;
    mount->verify = verify;
    mount->verify_ctx = ctx;
}

static StreamStatus config_apply(StreamMount *mount, const StreamConfig *cfg,
                                 char *reason, size_t reason_cap) {
    if (!cfg) return STREAM_ERR_ARGUMENT;
    if (cfg->quantum_bytes < STREAM_QUANTUM_MIN ||
        cfg->quantum_bytes > STREAM_QUANTUM_MAX) {
        set_reason(reason, reason_cap, "quantum %u is outside %u..%u",
                   cfg->quantum_bytes, STREAM_QUANTUM_MIN, STREAM_QUANTUM_MAX);
        return STREAM_ERR_ARGUMENT;
    }
    if (cfg->window_max_bytes < cfg->quantum_bytes ||
        cfg->window_max_bytes > 16u * 1024u * 1024u) {
        set_reason(reason, reason_cap, "window %u is outside %u..16 MiB",
                   cfg->window_max_bytes, cfg->quantum_bytes);
        return STREAM_ERR_ARGUMENT;
    }
    if (cfg->cache_limit_bytes < cfg->quantum_bytes) {
        set_reason(reason, reason_cap, "cache limit %u is below one quantum",
                   cfg->cache_limit_bytes);
        return STREAM_ERR_ARGUMENT;
    }
    if (!cfg->rate_bytes_per_sec) {
        set_reason(reason, reason_cap, "byte rate must be non-zero");
        return STREAM_ERR_ARGUMENT;
    }
    if (cfg->max_accrual_quanta < 1 ||
        cfg->max_accrual_quanta > STREAM_MAX_ACCRUAL_QUANTA) {
        set_reason(reason, reason_cap, "accrual %u is outside 1..%u",
                   cfg->max_accrual_quanta, STREAM_MAX_ACCRUAL_QUANTA);
        return STREAM_ERR_ARGUMENT;
    }
    mount->config = *cfg;
    mount->budget_bytes = (uint64_t)cfg->quantum_bytes *
                          cfg->max_accrual_quanta;
    mount->budget_ns = mount_now_ns(mount);
    return STREAM_OK;
}

StreamStatus stream_mount_configure(StreamMount *mount,
                                    const StreamConfig *config, char *reason,
                                    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || !config) return STREAM_ERR_ARGUMENT;
    if (mount->descriptors) {
        set_reason(reason, reason_cap,
                   "cannot reconfigure while %u descriptors are live",
                   mount->descriptors);
        return STREAM_ERR_BUDGET;
    }
    return config_apply(mount, config, reason, reason_cap);
}

static int handle_decode(const StreamMount *mount, StreamHandle handle,
                         StreamRequest **out) {
    uint32_t slot = handle & 0xFFu;
    uint32_t epoch = handle >> 8;
    if (!handle || slot >= STREAM_MAX_REQUESTS || !epoch) return 0;
    StreamRequest *req = (StreamRequest *)&mount->requests[slot];
    if (!req->used || req->epoch != epoch) return 0;
    *out = req;
    return 1;
}

static StreamHandle handle_encode(const StreamRequest *req, uint32_t slot) {
    return ((req->epoch & 0xFFFFFFu) << 8) | slot;
}

static void request_drop_data(StreamMount *mount, StreamRequest *req) {
    if (!req->data) return;
    free(req->data);
    req->data = NULL;
    ++mount->metrics.frees;
    mount->metrics.bytes_freed += req->target;
    if (mount->cache_bytes >= req->target) mount->cache_bytes -= req->target;
    else mount->cache_bytes = 0;
    mount->metrics.cache_bytes = mount->cache_bytes;
}

static void request_reclaim(StreamMount *mount, StreamRequest *req) {
    request_drop_data(mount, req);
    req->used = 0;
    req->state = STREAM_REQ_QUEUED;
    req->epoch = (req->epoch + 1u) & 0xFFFFFFu;
    if (!req->epoch) req->epoch = 1;
    if (mount->descriptors) --mount->descriptors;
    mount->metrics.descriptors = mount->descriptors;
}

static StreamRequest *request_slot(StreamMount *mount) {
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (req->used) continue;
        uint32_t epoch = req->epoch ? req->epoch : 1;
        memset(req, 0, sizeof *req);
        req->epoch = epoch;
        req->used = 1;
        ++mount->descriptors;
        mount->metrics.descriptors = mount->descriptors;
        return req;
    }
    /* Reclaim a terminal descriptor before refusing. */
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (req->used && (req->state == STREAM_REQ_ERROR ||
                          req->state == STREAM_REQ_CANCELLED)) {
            request_reclaim(mount, req);
            return request_slot(mount);
        }
    }
    return NULL;
}

static StreamRequest *request_find_key(StreamMount *mount,
                                       const StreamRequestKey *key) {
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used) continue;
        if (req->state > STREAM_REQ_READY) continue;
        if (req->key.generation == key->generation &&
            req->key.source == key->source && req->key.id == key->id &&
            req->key.lod == key->lod)
            return req;
    }
    return NULL;
}

static int request_lru_before(const StreamRequest *a, const StreamRequest *b) {
    if (a->last_touch_ms != b->last_touch_ms)
        return a->last_touch_ms < b->last_touch_ms;
    return a->seq < b->seq;
}

static int evict_one(StreamMount *mount, const StreamRequest *except,
                     uint32_t *out_freed) {
    StreamRequest *best = NULL;
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used || req == except) continue;
        if (req->state != STREAM_REQ_READY) continue;
        if (req->consumers || req->pinned || req->visible) continue;
        if (!best || request_lru_before(req, best)) best = req;
    }
    if (!best) return 0;
    uint32_t freed = best->target;
    request_drop_data(mount, best);
    request_reclaim(mount, best);
    ++mount->metrics.evicted;
    mount->metrics.evicted_bytes += freed;
    if (out_freed) *out_freed += freed;
    return 1;
}

static int drop_unused_p2(StreamMount *mount, const StreamRequest *except,
                          uint32_t *out_freed) {
    StreamRequest *best = NULL;
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used || req == except) continue;
        if (req->state != STREAM_REQ_QUEUED) continue;
        if (req->priority != STREAM_PRIO_P2) continue;
        if (req->consumers || req->pinned || req->visible) continue;
        if (!best || request_lru_before(req, best)) best = req;
    }
    if (!best) return 0;
    uint32_t freed = best->target;
    request_drop_data(mount, best);
    request_reclaim(mount, best);
    ++mount->metrics.cancelled;
    if (out_freed) *out_freed += freed;
    return 1;
}

static StreamStatus make_room(StreamMount *mount, uint32_t need,
                              const StreamRequest *except, char *reason,
                              size_t reason_cap) {
    uint64_t limit = mount->config.cache_limit_bytes;
    while ((uint64_t)mount->cache_bytes + need > limit) {
        if (drop_unused_p2(mount, except, NULL)) continue;
        if (evict_one(mount, except, NULL)) continue;
        set_reason(reason, reason_cap,
                   "cache limit %u B: %u live + %u requested",
                   mount->config.cache_limit_bytes, mount->cache_bytes, need);
        return STREAM_ERR_BUDGET;
    }
    return STREAM_OK;
}

static StreamStatus request_reserve(StreamMount *mount, StreamRequest *req,
                                    uint32_t target, char *reason,
                                    size_t reason_cap) {
    uint32_t old = req->data ? req->target : 0;
    if (target <= old) return STREAM_OK;
    StreamStatus st = make_room(mount, target - old, req, reason, reason_cap);
    if (st != STREAM_OK) return st;
    uint8_t *data = (uint8_t *)malloc(target);
    if (!data) {
        set_reason(reason, reason_cap, "cannot allocate %u-byte window", target);
        return STREAM_ERR_NOMEM;
    }
    if (req->data) {
        memcpy(data, req->data, req->fetched < old ? req->fetched : old);
        free(req->data);
        ++mount->metrics.frees;
        mount->metrics.bytes_freed += old;
        mount->cache_bytes -= old;
    }
    req->data = data;
    req->target = target;
    ++mount->metrics.allocs;
    mount->metrics.bytes_allocated += target;
    mount->cache_bytes += target;
    mount->metrics.cache_bytes = mount->cache_bytes;
    if (mount->cache_bytes > mount->peak_cache_bytes) {
        mount->peak_cache_bytes = mount->cache_bytes;
        mount->metrics.peak_cache_bytes = mount->peak_cache_bytes;
    }
    return STREAM_OK;
}

static void budget_refill(StreamMount *mount, uint64_t now_ns) {
    uint64_t cap = (uint64_t)mount->config.quantum_bytes *
                   mount->config.max_accrual_quanta;
    if (mount->budget_bytes > cap) mount->budget_bytes = cap;
    if (now_ns <= mount->budget_ns) {
        mount->budget_ns = now_ns;
        return;
    }
    uint64_t delta = now_ns - mount->budget_ns;
    mount->budget_ns = now_ns;
    uint64_t rate = mount->config.rate_bytes_per_sec;
    uint64_t add = (delta / 1000000000ull) * rate +
                   ((delta % 1000000000ull) * rate) / 1000000000ull;
    if (add >= cap - mount->budget_bytes) mount->budget_bytes = cap;
    else mount->budget_bytes += add;
}

static void expire_requests(StreamMount *mount, uint64_t now_ms) {
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used) continue;
        if (req->state != STREAM_REQ_QUEUED && req->state != STREAM_REQ_READING)
            continue;
        if (req->priority == STREAM_PRIO_DEMAND || !req->deadline_ms)
            continue;
        if (req->consumers || req->pinned || req->visible) continue;
        if (now_ms <= req->deadline_ms) continue;
        request_drop_data(mount, req);
        req->state = STREAM_REQ_CANCELLED;
        ++mount->metrics.expired;
        ++mount->metrics.cancelled;
    }
}

static int request_better(const StreamRequest *a, const StreamRequest *b) {
    if (a->priority != b->priority) return a->priority < b->priority;
    uint64_t ad = a->deadline_ms ? a->deadline_ms : UINT64_MAX;
    uint64_t bd = b->deadline_ms ? b->deadline_ms : UINT64_MAX;
    if (ad != bd) return ad < bd;
    if (a->key.source != b->key.source) return a->key.source < b->key.source;
    if (a->res.physical_offset != b->res.physical_offset)
        return a->res.physical_offset < b->res.physical_offset;
    return a->seq < b->seq;
}

static StreamRequest *next_request(StreamMount *mount) {
    StreamRequest *best = NULL;
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used || req->state != STREAM_REQ_QUEUED) continue;
        if (req->fetched >= req->target) continue;
        if (!best || request_better(req, best)) best = req;
    }
    return best;
}

static StreamStatus request_verify(StreamMount *mount, StreamRequest *req) {
    req->state = STREAM_REQ_VERIFIED;
    if (mount->verify &&
        mount->verify(mount->verify_ctx, &req->res, req->data, req->target)) {
        request_drop_data(mount, req);
        req->state = STREAM_REQ_ERROR;
        ++mount->metrics.request_errors;
        return STREAM_ERR_FORMAT;
    }
    req->state = STREAM_REQ_READY;
    req->last_touch_ms = mount_now_ms(mount);
    ++mount->metrics.verified;
    ++mount->metrics.ready;
    return STREAM_OK;
}

/* Run one quantum.  `*issued` is set when bsa_read_range was called. */
static StreamStatus request_quantum(StreamMount *mount, StreamRequest *req,
                                    int *issued, char *reason,
                                    size_t reason_cap) {
    uint32_t remain = req->target - req->fetched;
    uint32_t quantum = mount->config.quantum_bytes;
    if (quantum > remain) quantum = remain;
    if (mount->budget_bytes < quantum) {
        ++mount->metrics.budget_denied;
        set_reason(reason, reason_cap, "byte budget has %llu B, needs %u",
                   (unsigned long long)mount->budget_bytes, quantum);
        return STREAM_ERR_BUDGET;
    }
    BsaArchive *archive = NULL;
    StreamStatus st = mount_archive(mount, req->res.archive_index, &archive,
                                    reason, reason_cap);
    if (st != STREAM_OK) {
        request_drop_data(mount, req);
        req->state = STREAM_REQ_ERROR;
        ++mount->metrics.request_errors;
        ++mount->metrics.errors;
        return st;
    }
    if (!req->entry_valid) {
        BsaMatch match;
        char want[STREAM_PATH_MAX];
        StreamStatus pst = resource_path_copy(&req->res, want, sizeof want,
                                              reason, reason_cap);
        if (pst != STREAM_OK) {
            request_drop_data(mount, req);
            req->state = STREAM_REQ_ERROR;
            ++mount->metrics.request_errors;
            ++mount->metrics.errors;
            return pst;
        }
        BsaStatus bs = bsa_find(archive, want, &match);
        if (bs != BSA_OK) {
            request_drop_data(mount, req);
            req->state = STREAM_REQ_ERROR;
            ++mount->metrics.request_errors;
            ++mount->metrics.errors;
            set_reason(reason, reason_cap,
                       "archive directory lost '%s'", want);
            return STREAM_ERR_FORMAT;
        }
        if (match.entry.offset != req->res.entry_offset ||
            match.entry.stored_size != req->res.stored_size) {
            request_drop_data(mount, req);
            req->state = STREAM_REQ_ERROR;
            ++mount->metrics.request_errors;
            ++mount->metrics.errors;
            set_reason(reason, reason_cap,
                       "directory record for '%s' changed", want);
            return STREAM_ERR_FORMAT;
        }
        req->entry = match.entry;
        req->entry_valid = 1;
    }
    uint64_t tail = req->res.physical_offset + req->fetched;
    if (mount->last_archive != (int)req->res.archive_index ||
        !mount->last_tail_valid || mount->last_tail != tail)
        ++mount->metrics.seeks;
    uint32_t buf = mount->io_cur;
    uint32_t got = 0;
    uint32_t rel = req->fetched;
    req->state = STREAM_REQ_READING;
    BsaStatus bs = bsa_read_range(archive, &req->entry, rel, mount->io[buf],
                                  quantum, &got, reason, reason_cap);
    *issued = 1;
    if (bs != BSA_OK || got != quantum) {
        request_drop_data(mount, req);
        req->state = STREAM_REQ_ERROR;
        ++mount->metrics.request_errors;
        ++mount->metrics.errors;
        if (bs == BSA_OK)
            set_reason(reason, reason_cap, "short prefetch quantum %u/%u", got,
                       quantum);
        return STREAM_ERR_IO;
    }
    memcpy(req->data + req->fetched, mount->io[buf], got);
    req->fetched += got;
    ++req->quanta;
    ++mount->metrics.quanta;
    mount->metrics.quantum_bytes += got;
    ++mount->metrics.physical_reads;
    if (mount->budget_bytes >= got) mount->budget_bytes -= got;
    else mount->budget_bytes = 0;
    mount->last_archive = (int)req->res.archive_index;
    mount->last_tail = tail + got;
    mount->last_tail_valid = 1;
    mount->io_cur ^= 1u;
    req->last_touch_ms = mount_now_ms(mount);
    if (req->fetched >= req->target) {
        st = request_verify(mount, req);
        if (st != STREAM_OK) return st;
    } else {
        req->state = STREAM_REQ_QUEUED;
    }
    return STREAM_OK;
}

StreamStatus stream_mount_pump(StreamMount *mount, uint32_t max_quanta,
                               uint32_t *out_quanta, char *reason,
                               size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || !out_quanta) return STREAM_ERR_ARGUMENT;
    *out_quanta = 0;
    uint64_t now_ns = mount_now_ns(mount);
    expire_requests(mount, now_ns / 1000000ull);
    for (uint32_t i = 0; i < max_quanta; ++i) {
        budget_refill(mount, now_ns);
        StreamRequest *req = next_request(mount);
        if (!req) break;
        int issued = 0;
        StreamStatus st = request_quantum(mount, req, &issued, reason,
                                          reason_cap);
        if (issued) ++*out_quanta;
        if (st == STREAM_ERR_BUDGET) break;
        /* A failed request frees its bytes and the pump moves on. */
    }
    return STREAM_OK;
}

StreamStatus stream_mount_stage_extent(StreamMount *mount,
                                       const StreamResource *res,
                                       uint32_t *out, char *reason,
                                       size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || !res || !out) return STREAM_ERR_ARGUMENT;
    *out = res->payload_length;
    if (!(res->flags & STREAM_RESOURCE_FLAG_COMPRESSED)) return STREAM_OK;
    BsaArchive *archive = NULL;
    StreamStatus st = mount_archive(mount, res->archive_index, &archive,
                                    reason, reason_cap);
    if (st != STREAM_OK) return st;
    char want[STREAM_PATH_MAX];
    st = resource_path_copy(res, want, sizeof want, reason, reason_cap);
    if (st != STREAM_OK) return st;
    BsaMatch match;
    BsaStatus bs = bsa_find(archive, want, &match);
    if (bs != BSA_OK) {
        set_reason(reason, reason_cap, "archive directory lost '%s'", want);
        return STREAM_ERR_FORMAT;
    }
    uint32_t unpacked = 0;
    bs = bsa_unpacked_size(archive, &match.entry, &unpacked, reason,
                           reason_cap);
    if (bs != BSA_OK) return STREAM_ERR_FORMAT;
    *out = unpacked;
    return STREAM_OK;
}

StreamStatus stream_mount_prefetch_ex(StreamMount *mount,
                                      const StreamResource *res, uint32_t n,
                                      uint32_t lod, StreamPriority priority,
                                      uint64_t deadline_ms, StreamHandle *out,
                                      char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (out) *out = 0;
    if (!mount || !res || !out) return STREAM_ERR_ARGUMENT;
    if ((unsigned)priority > STREAM_PRIO_P2) {
        set_reason(reason, reason_cap, "invalid priority %d", (int)priority);
        return STREAM_ERR_ARGUMENT;
    }
    uint32_t target = res->payload_length;
    if (res->flags & STREAM_RESOURCE_FLAG_COMPRESSED) {
        uint32_t unpacked = 0;
        char why[STREAM_REASON_MAX];
        if (stream_mount_stage_extent(mount, res, &unpacked, why,
                                      sizeof why) == STREAM_OK)
            target = unpacked;
    }
    if (n && n < target) target = n;
    if (target > mount->config.window_max_bytes)
        target = mount->config.window_max_bytes;
    if (!target) {
        set_reason(reason, reason_cap, "'%s' has no stageable payload",
                   res->path ? res->path : "?");
        return STREAM_ERR_ARGUMENT;
    }
    StreamRequestKey key;
    key.generation = mount->index->info.generation;
    key.source = res->archive_index;
    key.id = res->id;
    key.lod = lod;
    uint64_t now = mount_now_ms(mount);
    StreamRequest *req = request_find_key(mount, &key);
    if (req) {
        if (priority < req->priority) req->priority = (uint8_t)priority;
        if (priority < req->requested_priority)
            req->requested_priority = (uint8_t)priority;
        if (deadline_ms && (!req->deadline_ms || deadline_ms < req->deadline_ms))
            req->deadline_ms = deadline_ms;
        if (priority == STREAM_PRIO_DEMAND) ++req->consumers;
        if (target > req->target) {
            int was_ready = req->state == STREAM_REQ_READY;
            StreamStatus st = request_reserve(mount, req, target, reason,
                                              reason_cap);
            if (st != STREAM_OK) {
                if (priority == STREAM_PRIO_DEMAND && req->consumers)
                    --req->consumers;
                return st;
            }
            if (was_ready) req->state = STREAM_REQ_QUEUED;
        }
        req->last_touch_ms = now;
        ++mount->metrics.prefetch_requests;
        ++mount->metrics.dedup_hits;
        *out = handle_encode(req, (uint32_t)(req - mount->requests));
        return STREAM_OK;
    }
    req = request_slot(mount);
    if (!req) {
        /* Descriptor pressure: drop an unused P2 before refusing. */
        if (!drop_unused_p2(mount, NULL, NULL)) {
            set_reason(reason, reason_cap,
                       "all %u request descriptors are in use",
                       STREAM_MAX_REQUESTS);
            return STREAM_ERR_BUDGET;
        }
        req = request_slot(mount);
        if (!req) return STREAM_ERR_BUDGET;
    }
    req->res = *res;
    req->key = key;
    req->priority = (uint8_t)priority;
    req->requested_priority = (uint8_t)priority;
    req->state = STREAM_REQ_QUEUED;
    req->target = 0;
    req->fetched = 0;
    req->quanta = 0;
    req->deadline_ms = deadline_ms;
    req->enqueued_ms = now;
    req->last_touch_ms = now;
    req->seq = (uint32_t)(++mount->seq);
    if (priority == STREAM_PRIO_DEMAND) req->consumers = 1;
    StreamStatus st = request_reserve(mount, req, target, reason, reason_cap);
    if (st != STREAM_OK) {
        request_reclaim(mount, req);
        return st;
    }
    ++mount->metrics.prefetch_requests;
    *out = handle_encode(req, (uint32_t)(req - mount->requests));
    return STREAM_OK;
}

StreamStatus stream_mount_prefetch(StreamMount *mount, const StreamResource *res,
                                   uint32_t n, char *reason,
                                   size_t reason_cap) {
    StreamHandle handle = 0;
    return stream_mount_prefetch_ex(mount, res, n, 0, STREAM_PRIO_P1, 0,
                                    &handle, reason, reason_cap);
}

StreamStatus stream_mount_request_info(const StreamMount *mount,
                                       StreamHandle handle,
                                       StreamRequestInfo *out) {
    if (!mount || !out) return STREAM_ERR_ARGUMENT;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) return STREAM_ERR_NOT_FOUND;
    memset(out, 0, sizeof *out);
    out->key = req->key;
    out->priority = (StreamPriority)req->priority;
    out->requested_priority = (StreamPriority)req->requested_priority;
    out->state = (StreamRequestState)req->state;
    out->target_bytes = req->target;
    out->fetched_bytes = req->fetched;
    out->payload_length = req->res.payload_length;
    out->consumers = req->consumers;
    out->pinned = req->pinned;
    out->visible = req->visible;
    out->deadline_ms = req->deadline_ms;
    out->enqueued_ms = req->enqueued_ms;
    out->last_touch_ms = req->last_touch_ms;
    out->quanta = req->quanta;
    return STREAM_OK;
}

StreamStatus stream_mount_request_copy(StreamMount *mount, StreamHandle handle,
                                       uint32_t rel, void *dst, uint32_t n,
                                       uint32_t *out_len, char *reason,
                                       size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || (!dst && n) || !out_len) return STREAM_ERR_ARGUMENT;
    *out_len = 0;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) {
        set_reason(reason, reason_cap, "stale request handle");
        return STREAM_ERR_NOT_FOUND;
    }
    if (req->state != STREAM_REQ_READY) {
        set_reason(reason, reason_cap, "request is %s, not ready",
                   stream_request_state_name((StreamRequestState)req->state));
        return STREAM_ERR_UNSUPPORTED;
    }
    if (rel > req->fetched || n > req->fetched - rel) {
        set_reason(reason, reason_cap,
                   "copy %u+%u is outside the %u-byte window", rel, n,
                   req->fetched);
        return STREAM_ERR_BOUNDS;
    }
    if (n) memcpy(dst, req->data + rel, n);
    *out_len = n;
    req->last_touch_ms = mount_now_ms(mount);
    mount->metrics.cache_hits++;
    mount->metrics.cache_hit_bytes += n;
    return STREAM_OK;
}

StreamStatus stream_mount_pin(StreamMount *mount, StreamHandle handle,
                              int pinned, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount) return STREAM_ERR_ARGUMENT;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) {
        set_reason(reason, reason_cap, "stale request handle");
        return STREAM_ERR_NOT_FOUND;
    }
    req->pinned = pinned ? 1 : 0;
    return STREAM_OK;
}

StreamStatus stream_mount_mark_visible(StreamMount *mount, StreamHandle handle,
                                       int visible, char *reason,
                                       size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount) return STREAM_ERR_ARGUMENT;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) {
        set_reason(reason, reason_cap, "stale request handle");
        return STREAM_ERR_NOT_FOUND;
    }
    req->visible = visible ? 1 : 0;
    return STREAM_OK;
}

StreamStatus stream_mount_release(StreamMount *mount, StreamHandle handle,
                                  char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount) return STREAM_ERR_ARGUMENT;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) {
        set_reason(reason, reason_cap, "stale request handle");
        return STREAM_ERR_NOT_FOUND;
    }
    if (req->consumers) --req->consumers;
    if (req->state == STREAM_REQ_ERROR || req->state == STREAM_REQ_CANCELLED)
        request_reclaim(mount, req);
    return STREAM_OK;
}

StreamStatus stream_mount_cancel(StreamMount *mount, StreamHandle handle,
                                 char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount) return STREAM_ERR_ARGUMENT;
    StreamRequest *req = NULL;
    if (!handle_decode(mount, handle, &req)) {
        set_reason(reason, reason_cap, "stale request handle");
        return STREAM_ERR_NOT_FOUND;
    }
    if (req->state == STREAM_REQ_ERROR || req->state == STREAM_REQ_CANCELLED) {
        request_reclaim(mount, req);
        return STREAM_OK;
    }
    request_drop_data(mount, req);
    req->state = STREAM_REQ_CANCELLED;
    req->consumers = 0;
    ++mount->metrics.cancelled;
    return STREAM_OK;
}

StreamStatus stream_mount_trim(StreamMount *mount, uint32_t target_bytes,
                               uint32_t *out_freed, char *reason,
                               size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount) return STREAM_ERR_ARGUMENT;
    uint32_t freed = 0;
    while (mount->cache_bytes > target_bytes) {
        if (!evict_one(mount, NULL, &freed)) break;
    }
    if (out_freed) *out_freed = freed;
    return STREAM_OK;
}

StreamStatus stream_mount_read(StreamMount *mount, const StreamResource *res,
                               uint32_t rel, void *dst, uint32_t n,
                               uint32_t *out_len, char *reason,
                               size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || !res || (!dst && n) || !out_len)
        return STREAM_ERR_ARGUMENT;
    *out_len = 0;
    ++mount->metrics.requests;
    mount->metrics.bytes_requested += n;
    /* A demand read that is already staged is served without archive I/O. */
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used || req->state != STREAM_REQ_READY) continue;
        if (req->key.generation != mount->index->info.generation ||
            req->key.source != res->archive_index || req->key.id != res->id)
            continue;
        if (rel > req->fetched || n > req->fetched - rel) continue;
        if (n) memcpy(dst, req->data + rel, n);
        *out_len = n;
        req->last_touch_ms = mount_now_ms(mount);
        mount->metrics.bytes_returned += n;
        mount->metrics.cache_hits++;
        mount->metrics.cache_hit_bytes += n;
        mount->metrics.demand_hits++;
        mount->metrics.demand_hit_bytes += n;
        if (req->requested_priority != STREAM_PRIO_DEMAND)
            mount->metrics.prefetch_demand_hits++;
        return STREAM_OK;
    }
    BsaArchive *archive = NULL;
    StreamStatus st = mount_archive(mount, res->archive_index, &archive,
                                    reason, reason_cap);
    if (st != STREAM_OK) {
        ++mount->metrics.errors;
        return st;
    }
    char want[STREAM_PATH_MAX];
    st = resource_path_copy(res, want, sizeof want, reason, reason_cap);
    if (st != STREAM_OK) {
        ++mount->metrics.errors;
        return st;
    }
    BsaMatch match;
    BsaStatus bs = bsa_find(archive, want, &match);
    if (bs != BSA_OK) {
        ++mount->metrics.misses;
        ++mount->metrics.errors;
        set_reason(reason, reason_cap,
                   "archive directory lost '%s' since the index was written",
                   want);
        return bs == BSA_ERR_NOT_FOUND ? STREAM_ERR_FORMAT : STREAM_ERR_IO;
    }
    if (match.entry.offset != res->entry_offset ||
        match.entry.stored_size != res->stored_size) {
        ++mount->metrics.errors;
        set_reason(reason, reason_cap,
                   "directory record for '%s' changed (offset/size)", want);
        return STREAM_ERR_FORMAT;
    }
    uint32_t got = 0;
    bs = bsa_read_range(archive, &match.entry, rel, dst, n, &got, reason,
                        reason_cap);
    if (bs != BSA_OK) {
        ++mount->metrics.errors;
        return bs == BSA_ERR_UNSUPPORTED ? STREAM_ERR_UNSUPPORTED
                                         : STREAM_ERR_IO;
    }
    *out_len = got;
    mount->metrics.bytes_returned += got;
    ++mount->metrics.physical_reads;
    return STREAM_OK;
}

StreamStatus stream_mount_demand_path(StreamMount *mount, const char *path,
                                      uint32_t rel, void *dst, uint32_t n,
                                      uint32_t *out_len, int *served,
                                      char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!mount || !path || (!dst && n) || !out_len || !served)
        return STREAM_ERR_ARGUMENT;
    *out_len = 0;
    *served = 0;
    StreamResource res;
    char why[STREAM_REASON_MAX];
    StreamStatus st = index_find_path_impl((StreamIndex *)mount->index, path,
                                           &res, NULL, why, sizeof why);
    if (st != STREAM_OK) return st;
    int live = 0;
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        StreamRequest *req = &mount->requests[i];
        if (!req->used) continue;
        if (req->state > STREAM_REQ_READY) continue;
        if (req->key.generation != mount->index->info.generation ||
            req->key.source != res.archive_index || req->key.id != res.id)
            continue;
        live = 1;
        if (req->state == STREAM_REQ_READY && rel <= req->fetched &&
            n <= req->fetched - rel) {
            if (n) memcpy(dst, req->data + rel, n);
            *out_len = n;
            *served = 1;
            req->last_touch_ms = mount_now_ms(mount);
            ++mount->metrics.demand_hits;
            mount->metrics.demand_hit_bytes += n;
            if (req->requested_priority != STREAM_PRIO_DEMAND)
                ++mount->metrics.prefetch_demand_hits;
            return STREAM_OK;
        }
        /* The caller is reading this range itself.  Keeping a prefetch of the
         * same bytes alive would duplicate physical I/O; drop unowned work. */
        if (!req->consumers && (req->state == STREAM_REQ_QUEUED ||
                                req->state == STREAM_REQ_READING)) {
            request_drop_data(mount, req);
            req->state = STREAM_REQ_CANCELLED;
            ++mount->metrics.cancelled;
            ++mount->metrics.demand_cancelled_prefetch;
        }
    }
    set_reason(reason, reason_cap,
               live ? "'%s' is not staged for %u+%u" : "'%s' has no stage",
               path, rel, n);
    return live ? STREAM_ERR_UNSUPPORTED : STREAM_ERR_NOT_FOUND;
}

void stream_mount_metrics(const StreamMount *mount, StreamMetrics *out) {
    if (!out) return;
    if (mount) *out = mount->metrics;
    else memset(out, 0, sizeof *out);
}

void stream_set_global_mount(StreamMount *mount) { g_global_mount = mount; }

void stream_stats_snapshot(const StreamMount *mount, StreamStatsSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!mount) return;
    const StreamMount *m = mount; /* requests are only read, never mutated */
    for (uint32_t i = 0; i < STREAM_MAX_REQUESTS; ++i) {
        if (!m->requests[i].used) continue;
        if (m->requests[i].state == STREAM_REQ_QUEUED) ++out->queued;
        else if (m->requests[i].state == STREAM_REQ_READING) ++out->reading;
    }
    out->descriptors = m->descriptors;
    out->staged_bytes = m->cache_bytes;
    out->staged_limit_bytes = m->config.cache_limit_bytes;
    out->peak_staged_bytes = m->peak_cache_bytes;
    out->physical_reads = m->metrics.physical_reads;
    out->physical_bytes = m->metrics.quantum_bytes;
    out->physical_misses = m->metrics.misses;
    out->demand_hits = m->metrics.demand_hits;
    out->demand_bytes = m->metrics.demand_hit_bytes;
    out->prefetch_demand_hits = m->metrics.prefetch_demand_hits;
    out->cancelled = m->metrics.cancelled;
    out->budget_denied = m->metrics.budget_denied;
}

void stream_stats_global(StreamStatsSnapshot *out) {
    stream_stats_snapshot(g_global_mount, out);
}
