#include "asset_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct StreamMount {
    const StreamIndex *index;
    char *data_dir;
    BsaArchive **archives;
    StreamMetrics metrics;
};

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
    if (schema != STREAM_INDEX_SCHEMA) {
        set_reason(reason, cap, "unsupported index schema %u", schema);
        return STREAM_ERR_SCHEMA;
    }
    if (header_size != STREAM_INDEX_HEADER_SIZE ||
        archive_size != STREAM_INDEX_ARCHIVE_ENTRY_SIZE ||
        resource_size != STREAM_INDEX_RESOURCE_ENTRY_SIZE ||
        cell_size != STREAM_INDEX_CELL_ENTRY_SIZE) {
        set_reason(reason, cap, "index entry sizes do not match schema 1");
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
        resource_count > STREAM_MAX_RESOURCES) {
        set_reason(reason, cap, "implausible index counts %u/%u",
                   archive_count, resource_count);
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

static StreamStatus mount_archive(StreamMount *mount, uint32_t slot,
                                  BsaArchive **out, char *reason,
                                  size_t reason_cap) {
    if (slot >= mount->index->info.archive_count)
        return STREAM_ERR_ARGUMENT;
    if (mount->archives[slot]) {
        *out = mount->archives[slot];
        return STREAM_OK;
    }
    const char *name = NULL;
    uint64_t bytes = 0;
    stream_index_archive(mount->index, slot, &name, &bytes, NULL);
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
    if (!mount->data_dir || !mount->archives) {
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
    *out = mount;
    return STREAM_OK;
}

void stream_mount_close(StreamMount *mount) {
    if (!mount) return;
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
    BsaArchive *archive = NULL;
    StreamStatus st = mount_archive(mount, res->archive_index, &archive,
                                    reason, reason_cap);
    if (st != STREAM_OK) {
        ++mount->metrics.errors;
        return st;
    }
    BsaMatch match;
    BsaStatus bs = bsa_find(archive, res->path, &match);
    if (bs != BSA_OK) {
        ++mount->metrics.misses;
        ++mount->metrics.errors;
        set_reason(reason, reason_cap,
                   "archive directory lost '%s' since the index was written",
                   res->path);
        return bs == BSA_ERR_NOT_FOUND ? STREAM_ERR_FORMAT : STREAM_ERR_IO;
    }
    if (match.entry.offset != res->entry_offset ||
        match.entry.stored_size != res->stored_size) {
        ++mount->metrics.errors;
        set_reason(reason, reason_cap,
                   "directory record for '%s' changed (offset/size)", res->path);
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

void stream_mount_metrics(const StreamMount *mount, StreamMetrics *out) {
    if (!out) return;
    if (mount) *out = mount->metrics;
    else memset(out, 0, sizeof *out);
}

StreamStatus stream_mount_prefetch(StreamMount *mount, const StreamResource *res,
                                   uint32_t n, char *reason,
                                   size_t reason_cap) {
    (void)mount; (void)res; (void)n;
    /* TODO(streaming phase 2): queue, byte quanta, owners, prefetch,
     * eviction and GPU publication.  No silent no-op: callers must see the
     * refusal and keep using synchronous stream_mount_read(). */
    set_reason(reason, reason_cap,
               "prefetch queue is not implemented in streaming phase 1");
    return STREAM_ERR_UNSUPPORTED;
}
