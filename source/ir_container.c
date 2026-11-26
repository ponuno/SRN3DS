/* Indexed SD-backed predecoded IR container: runtime implementation.
 *
 * See source/ir_container.h for the on-disk format and the public API.
 *
 * The executor intentionally reuses the real interpreter body from ir_exec.c
 * through a unity-style include with only its three entry points renamed. This
 * is not an edit to ir_exec.c and it guarantees the container tier has exactly
 * the same instruction semantics as the slice IR tier. The included runner is
 * never used; irc_run() below drives the same exec_block() with blocks fetched
 * from the loaded container record instead of the decode cache.
 */
#include "ir_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__3DS__)
#include <3ds.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* The only symbols the included ir_exec.c would export. Names are unique so a
 * build that also links the normal ir_exec.o remains valid. */
static int irc_fetch_block(CPU *c, uint32_t start, uint32_t func_end,
                           IrExecBlock *out);
#define n3ds_ir_fetch_block irc_fetch_block
#define n3ds_ir_run  n3ds_irc_unity_run
#define n3ds_ir_call n3ds_irc_unity_call
#include "ir_exec.c"
#undef n3ds_ir_fetch_block
#undef n3ds_ir_run
#undef n3ds_ir_call

/* local ir_exec.c statics are now in this translation unit */

#define IRC_FILE_MAGIC0 'I'
#define IRC_RECORD_MAGIC 0x46435249u /* "IRCF" */

typedef struct {
    uint32_t addr;
    uint32_t end;
    uint32_t size;      /* record_size */
    uint32_t block_count;
    uint8_t *data;
    uint64_t tick;
    uint32_t pins;
    int used;
} IrcSlot;

typedef struct {
    N3dsIrc *irc;
    IrcSlot *slot;
} IrcExecFrame;

struct N3dsIrc {
    FILE *f;
    uint64_t file_size;
    uint32_t file_flags;
    uint32_t nfun;
    uint32_t ndec;
    uint32_t index_crc;
    uint32_t data_crc;
    uint32_t data_offset;
    uint32_t data_bytes;
    N3dsIrcEntry *index;

    IrcSlot *slots;
    unsigned slot_cap;
    size_t max_bytes;
    size_t cache_bytes;
    uint64_t tick;

    N3dsIrcAotPredicate aot_pred;
    void *aot_user;

    N3dsIrcStats st;
};

/* Executor frames are global because ir_exec.c's dispatch path is global too:
 * the runtime has exactly one guest thread. A fault longjmps past n3ds_irc_exec
 * and leaves the frame behind; n3ds_irc_recover() releases those pins. */
static IrcExecFrame g_irc_frames[N3DS_IRC_MAX_EXEC_DEPTH];
static unsigned g_irc_frame_count;

uint32_t n3ds_irc_last_function;
uint32_t n3ds_irc_last_block;
CPU *n3ds_irc_current_cpu;

/* ---------------------------------------------------------------- bytes */

static uint16_t irc_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t irc_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void irc_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t n3ds_irc_crc32(const void *data, size_t bytes) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (unsigned k = 0; k < 8; ++k)
                c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
            table[i] = c;
        }
        ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < bytes; ++i) c = table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint64_t irc_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) return 0;
    return (uint64_t)((double)now.QuadPart * 1.0e9 / (double)freq.QuadPart);
#elif defined(__3DS__)
    return (uint64_t)svcGetSystemTick() * 1000000000ull / SYSCLOCK_ARM11;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static size_t irc_align4(size_t v) { return (v + 3u) & ~(size_t)3u; }

const char *n3ds_irc_strstatus(int status) {
    switch (status) {
    case N3DS_IRC_OK: return "ok";
    case N3DS_IRC_ERR_ARG: return "invalid argument";
    case N3DS_IRC_ERR_OPEN: return "open failed";
    case N3DS_IRC_ERR_HEADER: return "bad header";
    case N3DS_IRC_ERR_VERSION: return "unsupported version";
    case N3DS_IRC_ERR_INDEX_CRC: return "index crc mismatch";
    case N3DS_IRC_ERR_INDEX_BOUNDS: return "invalid index";
    case N3DS_IRC_ERR_NOT_FOUND: return "function not found";
    case N3DS_IRC_ERR_RECORD_IO: return "record seek/read failed";
    case N3DS_IRC_ERR_RECORD_SHORT: return "record short read";
    case N3DS_IRC_ERR_RECORD_CRC: return "record crc mismatch";
    case N3DS_IRC_ERR_RECORD_BAD: return "invalid record";
    case N3DS_IRC_ERR_UNDECODED: return "function not decoded";
    case N3DS_IRC_ERR_CACHE: return "cache error";
    case N3DS_IRC_ERR_CACHE_PINNED: return "cache full of pinned records";
    case N3DS_IRC_ERR_TOO_LARGE: return "record larger than cache budget";
    case N3DS_IRC_ERR_NESTING: return "container nesting too deep";
    default: return "unknown error";
    }
}

const char *n3ds_irc_decode_status_name(unsigned status) {
    switch (status) {
    case N3DS_IRC_DEC_OK: return "ok";
    case N3DS_IRC_DEC_UNSUPPORTED: return "unsupported opcode";
    case N3DS_IRC_DEC_NO_TERMINATOR: return "block has no terminator";
    case N3DS_IRC_DEC_EMPTY: return "empty block";
    case N3DS_IRC_DEC_BAD_RANGE: return "bad range";
    case N3DS_IRC_DEC_TOO_LARGE: return "block too large";
    case N3DS_IRC_DEC_NOT_DECODED: return "not decoded";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------- metadata */

unsigned n3ds_irc_function_count(const N3dsIrc *irc) { return irc ? irc->nfun : 0; }
unsigned n3ds_irc_decoded_count(const N3dsIrc *irc) { return irc ? irc->ndec : 0; }
uint32_t n3ds_irc_file_flags(const N3dsIrc *irc) { return irc ? irc->file_flags : 0; }
uint64_t n3ds_irc_file_size(const N3dsIrc *irc) { return irc ? irc->file_size : 0; }
uint64_t n3ds_irc_index_size(const N3dsIrc *irc) {
    return irc && irc->index ? (uint64_t)irc->nfun * N3DS_IRC_INDEX_ENTRY : 0;
}
uint32_t n3ds_irc_index_crc32(const N3dsIrc *irc) { return irc ? irc->index_crc : 0; }
uint32_t n3ds_irc_data_crc32(const N3dsIrc *irc) { return irc ? irc->data_crc : 0; }

int n3ds_irc_lookup(const N3dsIrc *irc, uint32_t addr, N3dsIrcEntry *out) {
    if (!irc || !irc->index || !out) return N3DS_IRC_ERR_ARG;
    N3dsIrc *m = (N3dsIrc *)irc;
    m->st.lookups++;
    unsigned lo = 0, hi = irc->nfun;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (irc->index[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= irc->nfun || irc->index[lo].addr != addr) {
        m->st.lookup_misses++;
        return N3DS_IRC_ERR_NOT_FOUND;
    }
    *out = irc->index[lo];
    m->st.lookup_hits++;
    return N3DS_IRC_OK;
}

int n3ds_irc_entry_at(const N3dsIrc *irc, unsigned ordinal, N3dsIrcEntry *out) {
    if (!irc || !irc->index || !out || ordinal >= irc->nfun) return N3DS_IRC_ERR_ARG;
    *out = irc->index[ordinal];
    return N3DS_IRC_OK;
}

/* ----------------------------------------------------------------- open */

static int irc_read_exact(FILE *f, void *buf, size_t bytes) {
    return fread(buf, 1, bytes, f) == bytes ? 0 : -1;
}

static int irc_parse_index(N3dsIrc *irc, const uint8_t *raw, size_t bytes) {
    if (bytes != (size_t)irc->nfun * N3DS_IRC_INDEX_ENTRY) return N3DS_IRC_ERR_INDEX_BOUNDS;
    irc->index = (N3dsIrcEntry *)calloc(irc->nfun ? irc->nfun : 1, sizeof *irc->index);
    if (!irc->index) return N3DS_IRC_ERR_CACHE;
    for (uint32_t i = 0; i < irc->nfun; ++i) {
        const uint8_t *p = raw + (size_t)i * N3DS_IRC_INDEX_ENTRY;
        N3dsIrcEntry *e = &irc->index[i];
        e->addr = irc_rd32(p + 0);
        e->end = irc_rd32(p + 4);
        e->flags = irc_rd32(p + 8);
        e->decode_status = irc_rd16(p + 12);
        e->block_count = irc_rd16(p + 14);
        e->insn_count = irc_rd32(p + 16);
        e->record_offset = irc_rd32(p + 20);
        e->record_size = irc_rd32(p + 24);
        e->record_crc32 = irc_rd32(p + 28);
        e->reserved[0] = irc_rd32(p + 32);
        e->reserved[1] = irc_rd32(p + 36);

        if (i && irc->index[i - 1].addr >= e->addr) return N3DS_IRC_ERR_INDEX_BOUNDS;
        if (e->end <= e->addr) return N3DS_IRC_ERR_INDEX_BOUNDS;
        if (e->decode_status > N3DS_IRC_DEC_UNKNOWN) return N3DS_IRC_ERR_INDEX_BOUNDS;
        int decoded = (e->flags & N3DS_IRC_FN_DECODED) != 0;
        if (decoded) {
            if (e->record_size < N3DS_IRC_RECORD_HEADER) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if ((e->record_size & 7u) != 0) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (e->record_offset & 7u) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (irc->data_offset > e->record_offset) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (e->record_size > irc->data_bytes) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (e->record_offset - irc->data_offset > irc->data_bytes - e->record_size)
                return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (e->decode_status != N3DS_IRC_DEC_OK) return N3DS_IRC_ERR_INDEX_BOUNDS;
        } else {
            if (e->record_size != 0) return N3DS_IRC_ERR_INDEX_BOUNDS;
            if (e->decode_status == N3DS_IRC_DEC_OK) return N3DS_IRC_ERR_INDEX_BOUNDS;
        }
    }
    unsigned decoded = 0;
    for (uint32_t i = 0; i < irc->nfun; ++i)
        decoded += (irc->index[i].flags & N3DS_IRC_FN_DECODED) != 0;
    if (decoded != irc->ndec) return N3DS_IRC_ERR_INDEX_BOUNDS;
    return N3DS_IRC_OK;
}

N3dsIrc *n3ds_irc_open_ex(const char *path, int *status) {
    int st = N3DS_IRC_ERR_OPEN;
    FILE *f = NULL;
    uint8_t *index_raw = NULL;
    N3dsIrc *irc = NULL;
    if (!path) { if (status) *status = N3DS_IRC_ERR_ARG; return NULL; }

    f = fopen(path, "rb");
    if (!f) { if (status) *status = st; return NULL; }
    if (fseek(f, 0, SEEK_END)) { st = N3DS_IRC_ERR_OPEN; goto fail; }
    long end = ftell(f);
    if (end < 0 || (uint64_t)end < N3DS_IRC_HEADER_BYTES) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (fseek(f, 0, SEEK_SET)) { st = N3DS_IRC_ERR_OPEN; goto fail; }

    uint8_t h[N3DS_IRC_HEADER_BYTES];
    if (irc_read_exact(f, h, sizeof h)) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (h[0] != IRC_FILE_MAGIC0 || h[1] != 'R' || h[2] != 'C' || h[3] != 'N' ||
        h[4] != 'T' || h[5] != '0' || h[6] != '0' || h[7] != '1') {
        st = N3DS_IRC_ERR_HEADER;
        goto fail;
    }

    uint32_t version = irc_rd32(h + 8);
    uint32_t header_bytes = irc_rd32(h + 12);
    uint32_t file_flags = irc_rd32(h + 16);
    uint32_t nfun = irc_rd32(h + 20);
    uint32_t ndec = irc_rd32(h + 24);
    uint32_t index_offset = irc_rd32(h + 28);
    uint32_t index_bytes = irc_rd32(h + 32);
    uint32_t entry_bytes = irc_rd32(h + 36);
    uint32_t data_offset = irc_rd32(h + 40);
    uint32_t data_bytes = irc_rd32(h + 44);
    uint32_t stored_index_crc = irc_rd32(h + 48);
    uint32_t stored_data_crc = irc_rd32(h + 52);
    uint32_t stored_header_crc = irc_rd32(h + 56);
    uint32_t total_bytes = irc_rd32(h + 60);

    if (version != N3DS_IRC_VERSION) { st = N3DS_IRC_ERR_VERSION; goto fail; }
    if (header_bytes != N3DS_IRC_HEADER_BYTES) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (entry_bytes != N3DS_IRC_INDEX_ENTRY) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (!(file_flags & N3DS_IRC_FILE_LE)) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (nfun > 4000000u || ndec > nfun) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (index_offset < N3DS_IRC_HEADER_BYTES) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if (index_bytes != nfun * N3DS_IRC_INDEX_ENTRY) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if ((uint64_t)index_offset + index_bytes > data_offset) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if ((data_offset & 7u) != 0) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if ((uint64_t)data_offset + data_bytes != total_bytes) { st = N3DS_IRC_ERR_HEADER; goto fail; }
    if ((uint64_t)end != total_bytes) { st = N3DS_IRC_ERR_HEADER; goto fail; }

    uint8_t hcrc[N3DS_IRC_HEADER_BYTES];
    memcpy(hcrc, h, sizeof hcrc);
    irc_wr32(hcrc + 56, 0);
    if (n3ds_irc_crc32(hcrc, sizeof hcrc) != stored_header_crc) {
        st = N3DS_IRC_ERR_HEADER;
        goto fail;
    }

    index_raw = (uint8_t *)malloc(index_bytes ? index_bytes : 1);
    if (!index_raw) { st = N3DS_IRC_ERR_CACHE; goto fail; }
    if (fseek(f, (long)index_offset, SEEK_SET) || irc_read_exact(f, index_raw, index_bytes)) {
        st = N3DS_IRC_ERR_INDEX_BOUNDS;
        goto fail;
    }
    if (n3ds_irc_crc32(index_raw, index_bytes) != stored_index_crc) {
        st = N3DS_IRC_ERR_INDEX_CRC;
        goto fail;
    }

    irc = (N3dsIrc *)calloc(1, sizeof *irc);
    if (!irc) { st = N3DS_IRC_ERR_CACHE; goto fail; }
    irc->f = f;
    irc->file_size = (uint64_t)end;
    irc->file_flags = file_flags;
    irc->nfun = nfun;
    irc->ndec = ndec;
    irc->data_offset = data_offset;
    irc->data_bytes = data_bytes;
    irc->index_crc = stored_index_crc;
    irc->data_crc = stored_data_crc;
    f = NULL;

    st = irc_parse_index(irc, index_raw, index_bytes);
    free(index_raw);
    index_raw = NULL;
    if (st != N3DS_IRC_OK) { n3ds_irc_close(irc); irc = NULL; goto done; }
    if (n3ds_irc_cache_init(irc, 0, 0) != N3DS_IRC_OK) {
        n3ds_irc_close(irc);
        irc = NULL;
        st = N3DS_IRC_ERR_CACHE;
        goto done;
    }
    irc->st.opens++;
    if (status) *status = N3DS_IRC_OK;
    return irc;

fail:
    if (f) fclose(f);
    free(index_raw);
    if (irc) {
        if (irc->f) fclose(irc->f);
        free(irc->index);
        free(irc->slots);
        free(irc);
    }
    if (status) *status = st;
    return NULL;

done:
    if (status) *status = st;
    return NULL;
}

N3dsIrc *n3ds_irc_open(const char *path) { return n3ds_irc_open_ex(path, NULL); }

/* ---------------------------------------------------------------- cache */

static void irc_slot_release(IrcSlot *s, int evict) {
    if (!s || !s->used) return;
    free(s->data);
    memset(s, 0, sizeof *s);
    (void)evict;
}

int n3ds_irc_cache_init(N3dsIrc *irc, size_t max_bytes, unsigned max_records) {
    if (!irc) return N3DS_IRC_ERR_ARG;
    if (!max_bytes) max_bytes = N3DS_IRC_DEFAULT_CACHE_BYTES;
    if (!max_records) max_records = N3DS_IRC_DEFAULT_CACHE_RECORDS;
    if (max_records > (1u << 20)) max_records = 1u << 20;
    IrcSlot *slots = (IrcSlot *)calloc(max_records, sizeof *slots);
    if (!slots) return N3DS_IRC_ERR_CACHE;
    if (irc->slots) {
        for (unsigned i = 0; i < irc->slot_cap; ++i) irc_slot_release(&irc->slots[i], 0);
        free(irc->slots);
    }
    irc->slots = slots;
    irc->slot_cap = max_records;
    irc->max_bytes = max_bytes;
    irc->cache_bytes = 0;
    irc->tick = 0;
    irc->st.cache_hits = irc->st.cache_misses = 0;
    irc->st.load_attempts = irc->st.loads = irc->st.load_bytes = irc->st.load_ns = 0;
    irc->st.evictions = irc->st.evicted_bytes = 0;
    irc->st.pins = irc->st.unpins = 0;
    irc->st.cache_bytes = 0;
    irc->st.cache_records = 0;
    return N3DS_IRC_OK;
}

void n3ds_irc_cache_clear(N3dsIrc *irc) {
    if (!irc) return;
    if (irc->slots) {
        for (unsigned i = 0; i < irc->slot_cap; ++i) irc_slot_release(&irc->slots[i], 0);
    }
    irc->cache_bytes = 0;
    irc->tick = 0;
    irc->st.cache_bytes = 0;
    irc->st.cache_records = 0;
}

void n3ds_irc_close(N3dsIrc *irc) {
    if (!irc) return;
    n3ds_irc_cache_clear(irc);
    free(irc->slots);
    free(irc->index);
    if (irc->f) fclose(irc->f);
    free(irc);
}

static IrcSlot *irc_find_slot(N3dsIrc *irc, uint32_t addr) {
    for (unsigned i = 0; i < irc->slot_cap; ++i)
        if (irc->slots[i].used && irc->slots[i].addr == addr) return &irc->slots[i];
    return NULL;
}

static int irc_make_room(N3dsIrc *irc, size_t need) {
    if (need > irc->max_bytes) return N3DS_IRC_ERR_TOO_LARGE;
    unsigned used = 0;
    for (unsigned i = 0; i < irc->slot_cap; ++i) used += irc->slots[i].used != 0;
    for (;;) {
        if (irc->cache_bytes + need <= irc->max_bytes && used < irc->slot_cap) return 0;
        IrcSlot *victim = NULL;
        for (unsigned i = 0; i < irc->slot_cap; ++i) {
            IrcSlot *s = &irc->slots[i];
            if (!s->used || s->pins) continue;
            if (!victim || s->tick < victim->tick) victim = s;
        }
        if (!victim) return N3DS_IRC_ERR_CACHE_PINNED;
        irc->st.evictions++;
        irc->st.evicted_bytes += victim->size;
        irc->cache_bytes -= victim->size;
        irc_slot_release(victim, 1);
        used--;
        irc->st.cache_bytes = irc->cache_bytes;
        irc->st.cache_records = used;
    }
}

static int irc_validate_record(N3dsIrc *irc, const N3dsIrcEntry *e, const uint8_t *data) {
    (void)irc;
    if (e->record_size < N3DS_IRC_RECORD_HEADER) return N3DS_IRC_ERR_RECORD_BAD;
    uint32_t magic = irc_rd32(data + 0);
    uint16_t version = irc_rd16(data + 4);
    uint32_t entry = irc_rd32(data + 8);
    uint32_t end = irc_rd32(data + 12);
    uint32_t blocks = irc_rd32(data + 16);
    uint32_t insns = irc_rd32(data + 20);
    uint32_t blocks_off = irc_rd32(data + 24);
    uint32_t block_bytes = irc_rd32(data + 28);
    uint32_t stream_bytes = irc_rd32(data + 32);

    if (magic != IRC_RECORD_MAGIC || version != N3DS_IRC_VERSION) return N3DS_IRC_ERR_RECORD_BAD;
    if (entry != e->addr || end != e->end) return N3DS_IRC_ERR_RECORD_BAD;
    if (blocks != e->block_count || insns != e->insn_count) return N3DS_IRC_ERR_RECORD_BAD;
    if (blocks_off != N3DS_IRC_RECORD_HEADER) return N3DS_IRC_ERR_RECORD_BAD;
    if (block_bytes != blocks * N3DS_IRC_BLOCK_ENTRY) return N3DS_IRC_ERR_RECORD_BAD;
    if ((uint64_t)blocks_off + block_bytes > e->record_size) return N3DS_IRC_ERR_RECORD_BAD;
    size_t stream_begin = irc_align4((size_t)blocks_off + block_bytes);
    if (stream_begin > e->record_size || stream_bytes > e->record_size - stream_begin)
        return N3DS_IRC_ERR_RECORD_BAD;

    for (uint32_t i = 0; i < blocks; ++i) {
        const uint8_t *p = data + blocks_off + (size_t)i * N3DS_IRC_BLOCK_ENTRY;
        uint32_t start = irc_rd32(p + 0);
        uint32_t off = irc_rd32(p + 4);
        uint32_t words = irc_rd32(p + 8);
        uint16_t x86_len = irc_rd16(p + 12);
        uint8_t count = p[14];
        if (i && irc_rd32(data + blocks_off + (size_t)(i - 1) * N3DS_IRC_BLOCK_ENTRY) >= start)
            return N3DS_IRC_ERR_RECORD_BAD;
        if (start < e->addr || start >= e->end || !x86_len || !count || !words)
            return N3DS_IRC_ERR_RECORD_BAD;
        if ((uint64_t)start + x86_len > e->end) return N3DS_IRC_ERR_RECORD_BAD;
        if (off & 3u) return N3DS_IRC_ERR_RECORD_BAD;
        if (off < stream_begin || (uint64_t)off + (uint64_t)words * 4u > e->record_size)
            return N3DS_IRC_ERR_RECORD_BAD;
        /* The final word of a block is usually an address or immediate word,
         * not a core opcode, so there is no cheap last-word opcode check. The
         * record CRC plus the exact record/block bounds protect the stream;
         * exec_block() faults explicitly on a malformed instruction. */
    }
    return N3DS_IRC_OK;
}

static int irc_load_entry(N3dsIrc *irc, const N3dsIrcEntry *e, IrcSlot **out) {
    if (!irc || !e || !out) return N3DS_IRC_ERR_ARG;
    IrcSlot *s = irc_find_slot(irc, e->addr);
    if (s) {
        s->tick = ++irc->tick;
        irc->st.cache_hits++;
        *out = s;
        return N3DS_IRC_OK;
    }
    irc->st.cache_misses++;
    if (!(e->flags & N3DS_IRC_FN_DECODED) || e->decode_status != N3DS_IRC_DEC_OK ||
        !e->record_size) {
        irc->st.invalid_entries++;
        return N3DS_IRC_ERR_UNDECODED;
    }
    irc->st.load_attempts++;
    int rc = irc_make_room(irc, e->record_size);
    if (rc != N3DS_IRC_OK) return rc;
    uint8_t *data = (uint8_t *)malloc(e->record_size);
    if (!data) return N3DS_IRC_ERR_CACHE;

    uint64_t t0 = irc_now_ns();
    if (fseek(irc->f, (long)e->record_offset, SEEK_SET)) {
        free(data);
        irc->st.io_errors++;
        irc->st.load_ns += irc_now_ns() - t0;
        return N3DS_IRC_ERR_RECORD_IO;
    }
    size_t got = fread(data, 1, e->record_size, irc->f);
    if (got != e->record_size) {
        free(data);
        irc->st.short_reads++;
        irc->st.load_ns += irc_now_ns() - t0;
        return N3DS_IRC_ERR_RECORD_SHORT;
    }
    if (n3ds_irc_crc32(data, e->record_size) != e->record_crc32) {
        free(data);
        irc->st.crc_errors++;
        irc->st.load_ns += irc_now_ns() - t0;
        return N3DS_IRC_ERR_RECORD_CRC;
    }
    rc = irc_validate_record(irc, e, data);
    if (rc != N3DS_IRC_OK) {
        free(data);
        irc->st.record_errors++;
        irc->st.load_ns += irc_now_ns() - t0;
        return rc;
    }
    irc->st.load_ns += irc_now_ns() - t0;

    for (unsigned i = 0; i < irc->slot_cap; ++i) {
        if (irc->slots[i].used) continue;
        s = &irc->slots[i];
        s->addr = e->addr;
        s->end = e->end;
        s->size = e->record_size;
        s->block_count = e->block_count;
        s->data = data;
        s->tick = ++irc->tick;
        s->pins = 0;
        s->used = 1;
        irc->cache_bytes += e->record_size;
        irc->st.loads++;
        irc->st.load_bytes += e->record_size;
        {
            unsigned nrec = 0;
            for (unsigned k = 0; k < irc->slot_cap; ++k) nrec += irc->slots[k].used != 0;
            irc->st.cache_records = nrec;
            irc->st.cache_bytes = irc->cache_bytes;
        }
        *out = s;
        return N3DS_IRC_OK;
    }
    free(data);
    return N3DS_IRC_ERR_CACHE;
}

int n3ds_irc_load(N3dsIrc *irc, uint32_t addr) {
    N3dsIrcEntry e;
    int rc = n3ds_irc_lookup(irc, addr, &e);
    if (rc != N3DS_IRC_OK) return rc;
    IrcSlot *s = NULL;
    rc = irc_load_entry(irc, &e, &s);
    return rc;
}

int n3ds_irc_pin(N3dsIrc *irc, uint32_t addr) {
    N3dsIrcEntry e;
    int rc = n3ds_irc_lookup(irc, addr, &e);
    if (rc != N3DS_IRC_OK) return rc;
    IrcSlot *s = NULL;
    rc = irc_load_entry(irc, &e, &s);
    if (rc != N3DS_IRC_OK) return rc;
    s->pins++;
    irc->st.pins++;
    return N3DS_IRC_OK;
}

int n3ds_irc_unpin(N3dsIrc *irc, uint32_t addr) {
    if (!irc) return N3DS_IRC_ERR_ARG;
    IrcSlot *s = irc_find_slot(irc, addr);
    if (!s) return N3DS_IRC_ERR_NOT_FOUND;
    if (!s->pins) return N3DS_IRC_ERR_CACHE;
    s->pins--;
    s->tick = ++irc->tick;
    irc->st.unpins++;
    return N3DS_IRC_OK;
}

int n3ds_irc_is_loaded(const N3dsIrc *irc, uint32_t addr) {
    if (!irc) return 0;
    return irc_find_slot((N3dsIrc *)irc, addr) != NULL;
}

void n3ds_irc_stats(const N3dsIrc *irc, N3dsIrcStats *out) {
    if (out) *out = irc ? irc->st : (N3dsIrcStats){0};
}

void n3ds_irc_stats_reset(N3dsIrc *irc) {
    if (!irc) return;
    memset(&irc->st, 0, sizeof irc->st);
    if (irc->slots) {
        unsigned nrec = 0;
        for (unsigned i = 0; i < irc->slot_cap; ++i) {
            if (!irc->slots[i].used) continue;
            nrec++;
            irc->st.cache_bytes += irc->slots[i].size;
        }
        irc->st.cache_records = nrec;
    }
}

void n3ds_irc_set_aot_predicate(N3dsIrc *irc, N3dsIrcAotPredicate pred, void *user) {
    if (!irc) return;
    irc->aot_pred = pred;
    irc->aot_user = user;
}

/* --------------------------------------------------------------- execute */

static int irc_fetch_block(CPU *c, uint32_t start, uint32_t func_end, IrExecBlock *out) {
    (void)c;
    (void)func_end;
    if (!g_irc_frame_count || !out) return -1;
    IrcSlot *s = g_irc_frames[g_irc_frame_count - 1].slot;
    if (!s || !s->used || start < s->addr || start >= s->end) return -1;
    const uint8_t *base = s->data;
    uint32_t blocks_off = irc_rd32(base + 24);
    unsigned lo = 0, hi = s->block_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const uint8_t *p = base + blocks_off + (size_t)mid * N3DS_IRC_BLOCK_ENTRY;
        if (irc_rd32(p) < start) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s->block_count) {
        g_irc_frames[g_irc_frame_count - 1].irc->st.block_misses++;
        return -1;
    }
    const uint8_t *p = base + blocks_off + (size_t)lo * N3DS_IRC_BLOCK_ENTRY;
    if (irc_rd32(p) != start) {
        g_irc_frames[g_irc_frame_count - 1].irc->st.block_misses++;
        return -1;
    }
    g_irc_frames[g_irc_frame_count - 1].irc->st.block_hits++;
    out->start = start;
    out->x86_len = irc_rd16(p + 12);
    out->insn_count = p[14];
    out->stream_words = irc_rd32(p + 8);
    out->stream = (const uint32_t *)(base + irc_rd32(p + 4));
    /* Streams were validated 4-byte aligned; ARM allows the aligned load. */
    return 0;
}

static void irc_run(CPU *c, uint32_t addr, uint32_t func_end) {
    Flags f = {0};
    uint32_t ip = addr;
    for (;;) {
        IrExecBlock b;
        n3ds_irc_last_function = addr;
        n3ds_irc_last_block = ip;
        n3ds_irc_current_cpu = c;
        if (irc_fetch_block(c, ip, func_end, &b)) {
            char what[128];
            snprintf(what, sizeof what,
                     "IR container: block %08lx is not in the predecoded record",
                     (unsigned long)ip);
            rt_unimpl(c, ip, what);
        }
        uint32_t next = 0;
        if (exec_block(c, &b, &f, addr, func_end, &next)) return;
        ip = next;
    }
}

static int irc_exec_common(N3dsIrc *irc, CPU *c, uint32_t addr, int allow_aot,
                           int fault) {
    if (!irc || !c) return N3DS_IRC_ERR_ARG;
    if (allow_aot && irc->aot_pred && irc->aot_pred(irc->aot_user, addr)) {
        irc->st.aot_preempted++;
        return N3DS_IRC_TIER_AOT;
    }
    N3dsIrcEntry e;
    int rc = n3ds_irc_lookup(irc, addr, &e);
    if (rc != N3DS_IRC_OK) {
        if (fault) {
            char what[128];
            snprintf(what, sizeof what, "IR container: function %08lx is not indexed",
                     (unsigned long)addr);
            rt_unimpl(c, addr, what);
        }
        return rc;
    }
    if (!(e.flags & N3DS_IRC_FN_DECODED) || e.decode_status != N3DS_IRC_DEC_OK || !e.record_size) {
        irc->st.invalid_entries++;
        if (fault) {
            char what[160];
            snprintf(what, sizeof what,
                     "IR container: function %08lx is not decoded (%s)",
                     (unsigned long)addr, n3ds_irc_decode_status_name(e.decode_status));
            rt_unimpl(c, addr, what);
        }
        return N3DS_IRC_ERR_UNDECODED;
    }
    if (g_irc_frame_count >= N3DS_IRC_MAX_EXEC_DEPTH) {
        if (fault) rt_unimpl(c, addr, "IR container: nesting too deep");
        return N3DS_IRC_ERR_NESTING;
    }
    IrcSlot *slot = NULL;
    rc = irc_load_entry(irc, &e, &slot);
    if (rc != N3DS_IRC_OK) {
        if (fault) {
            char what[160];
            snprintf(what, sizeof what, "IR container: %s at %08lx",
                     n3ds_irc_strstatus(rc), (unsigned long)addr);
            rt_unimpl(c, addr, what);
        }
        return rc;
    }
    slot->pins++;
    irc->st.pins++;
    g_irc_frames[g_irc_frame_count].irc = irc;
    g_irc_frames[g_irc_frame_count].slot = slot;
    g_irc_frame_count++;

    irc_run(c, addr, e.end);

    g_irc_frame_count--;
    slot->pins--;
    slot->tick = ++irc->tick;
    irc->st.unpins++;
    irc->st.execs++;
    return N3DS_IRC_TIER_CONTAINER;
}

int n3ds_irc_exec_status(N3dsIrc *irc, CPU *c, uint32_t addr, int allow_aot) {
    return irc_exec_common(irc, c, addr, allow_aot, 0);
}

int n3ds_irc_exec(N3dsIrc *irc, CPU *c, uint32_t addr) {
    return irc_exec_common(irc, c, addr, 1, 1);
}

int n3ds_irc_exec_forced(N3dsIrc *irc, CPU *c, uint32_t addr) {
    return irc_exec_common(irc, c, addr, 0, 1);
}

void n3ds_irc_recover(N3dsIrc *irc) {
    (void)irc;
    n3ds_irc_recover_to(irc, 0);
}

uint32_t n3ds_irc_depth(const N3dsIrc *irc) {
    (void)irc;
    return g_irc_frame_count;
}

uint32_t n3ds_irc_frames_save(void *dst) {
    if (!dst) return 0;
    memcpy(dst, g_irc_frames, (size_t)g_irc_frame_count * sizeof g_irc_frames[0]);
    return g_irc_frame_count;
}

void n3ds_irc_frames_load(const void *src, uint32_t count) {
    if (count > N3DS_IRC_MAX_EXEC_DEPTH) count = N3DS_IRC_MAX_EXEC_DEPTH;
    if (!src || !count) {
        g_irc_frame_count = 0;
        return;
    }
    memcpy(g_irc_frames, src, (size_t)count * sizeof g_irc_frames[0]);
    g_irc_frame_count = count;
}

void n3ds_irc_recover_to(N3dsIrc *irc, uint32_t depth) {
    (void)irc;
    if (depth > g_irc_frame_count) depth = g_irc_frame_count;
    while (g_irc_frame_count > depth) {
        IrcExecFrame *fr = &g_irc_frames[--g_irc_frame_count];
        if (fr->slot && fr->slot->pins) {
            fr->slot->pins--;
            if (fr->irc) fr->irc->st.unpins++;
        }
        memset(fr, 0, sizeof *fr);
    }
}
