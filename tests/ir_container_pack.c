/* Deterministic IR container packer.
 *
 * This is the *real* C decoder path: every function is decoded with
 * n3ds_ir_encode_block() from source/ir_decode.c, exactly like the slice IR
 * tier and tools/ir_measure.py. The Python driver (tools/ir_pack.py) builds the
 * IRM1 input and invokes this binary; it never re-implements the IR encoding.
 *
 * Input (little-endian, shared with tests/ir_measure.c):
 *   u32 magic 'IRM1', u32 image_base, u32 blob_bytes, u32 function_count,
 *   u8  blob[blob_bytes],
 *   function_count * {
 *       u32 entry, u32 function_end, u32 range_count,
 *       range_count * { u32 start, u32 end }
 *   }
 *
 * Output: one container file plus an optional JSON report and index CSV.
 *
 * Usage:
 *   ir_container_pack INPUT.bin OUTPUT.irc [--aot aot.txt] [--forced f.txt]
 *                      [--report report.json] [--index-csv index.csv]
 */
#include "ir_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRC_VERSION 1u
#define IRC_HEADER 64u
#define IRC_INDEX_ENTRY 40u
#define IRC_RECORD_HEADER 40u
#define IRC_BLOCK_ENTRY 20u

enum {
    FN_DECODED = 1u << 0,
    FN_AOT = 1u << 1,
    FN_FORCED = 1u << 2,
    FN_COMPLETE = 1u << 3,
    FN_INVALID = 1u << 4,
    FN_PREENTRY = 1u << 5,
};

enum {
    DEC_OK = 0,
    DEC_UNSUPPORTED = 1,
    DEC_NO_TERMINATOR = 2,
    DEC_EMPTY = 3,
    DEC_BAD_RANGE = 4,
    DEC_TOO_LARGE = 5,
    DEC_NOT_DECODED = 6,
    DEC_UNKNOWN = 7,
};

static uint8_t *g_code;
static uint32_t g_base, g_size;

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    if (!bytes) return NULL;
    if (addr < g_base || (uint64_t)addr - g_base > g_size ||
        bytes > g_size - (addr - g_base))
        return NULL;
    return g_code + (addr - g_base);
}

void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "pack: fault at %08lx: %s\n", (unsigned long)addr,
            what ? what : "unknown");
    exit(2);
}

/* ------------------------------------------------------------------ crc */

static uint32_t g_crc_table[256];
static int g_crc_ready;

static void crc_init(void) {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (unsigned k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        g_crc_table[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc_update(uint32_t c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; ++i) c = g_crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c;
}

static uint32_t crc_of(const void *data, size_t n) {
    crc_init();
    return crc_update(0xffffffffu, data, n) ^ 0xffffffffu;
}

/* ---------------------------------------------------------------- bytes */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static size_t align4(size_t v) { return (v + 3u) & ~(size_t)3u; }
static size_t align8(size_t v) { return (v + 7u) & ~(size_t)7u; }

/* ----------------------------------------------------------------- data */

typedef struct { uint32_t lo, hi; int closure; } Range;

typedef struct {
    uint32_t entry, end;
    uint32_t nranges;
    Range *ranges;
    uint32_t flags;
    uint16_t status;
    uint16_t blocks;
    uint32_t insns;
    uint32_t record_off, record_size, record_crc;
    char error[64];
} Fn;

typedef struct {
    uint32_t start;
    uint32_t x86_len;
    uint32_t insn_count;
    uint32_t nwords;
    uint16_t term_op;
    uint32_t *words;
} PackBlock;

static int range_cmp(const void *a, const void *b) {
    const Range *x = (const Range *)a, *y = (const Range *)b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    /* A closure copy has the same span as the discovery leader it promotes;
     * keep the promoted copy first so the excluded discovery range cannot
     * shadow it in the build loop. */
    if (x->closure != y->closure) return x->closure ? -1 : 1;
    return 0;
}

static int block_cmp(const void *a, const void *b) {
    const PackBlock *x = (const PackBlock *)a, *y = (const PackBlock *)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return 0;
}

static int fn_cmp(const void *a, const void *b) {
    const Fn *x = (const Fn *)a, *y = (const Fn *)b;
    return x->entry < y->entry ? -1 : x->entry > y->entry;
}

static int read_u32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = rd32(b);
    return 0;
}

static int read_list(const char *path, uint32_t **out, unsigned *count) {
    *out = NULL; *count = 0;
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    unsigned cap = 64, n = 0;
    uint32_t *list = (uint32_t *)malloc(cap * sizeof *list);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char *endp = NULL;
        unsigned long v = strtoul(p, &endp, 16);
        if (endp == p) continue;
        if (n == cap) { cap *= 2; list = (uint32_t *)realloc(list, cap * sizeof *list); }
        list[n++] = (uint32_t)v;
    }
    fclose(f);
    *out = list; *count = n;
    return 0;
}

static int in_list(const uint32_t *list, unsigned n, uint32_t addr) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (list[mid] < addr) lo = mid + 1; else hi = mid;
    }
    return lo < n && list[lo] == addr;
}

static int list_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint16_t classify(const char *err) {
    if (!err || !*err) return DEC_UNKNOWN;
    if (strstr(err, "unsupported opcode")) return DEC_UNSUPPORTED;
    if (strstr(err, "no terminator")) return DEC_NO_TERMINATOR;
    if (strstr(err, "empty block")) return DEC_EMPTY;
    if (strstr(err, "too long") || strstr(err, "larger than 64")) return DEC_TOO_LARGE;
    return DEC_UNKNOWN;
}

static void free_blocks(PackBlock *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) free(b[i].words);
    free(b);
}

/* Queue an already discovered cold leader after a conditional branch proves
 * it is reachable. Do not invent missing ranges: jmp-root entries can share
 * code with an enclosing function, and guessing their frame would change guest
 * control flow. A genuinely absent target remains a named runtime refusal. */
static int promote_closure_range(Fn *fn, uint32_t start) {
    if (start >= fn->entry) return 0;
    int base = -1;
    for (uint32_t i = 0; i < fn->nranges; ++i) {
        if (fn->ranges[i].lo != start) continue;
        /* Discovery can list the same leader more than once. Return as soon as
         * any copy is already promoted; appending per duplicate (or once per
         * decoded edge) would grow the range list without bound. */
        if (fn->ranges[i].closure) return 0;
        if (base < 0) base = (int)i;
    }
    if (base < 0) return 0;
    if (fn->nranges >= 0xffffu) {
        snprintf(fn->error, sizeof fn->error, "more than 65535 closure leaders");
        return -1;
    }
    Range *grown = (Range *)realloc(fn->ranges, (fn->nranges + 1u) * sizeof *grown);
    if (!grown) {
        snprintf(fn->error, sizeof fn->error, "out of memory");
        return -1;
    }
    fn->ranges = grown;
    fn->ranges[fn->nranges] = fn->ranges[base];
    fn->ranges[fn->nranges].closure = 1;
    fn->nranges++;
    return 1;
}

/* Return the final instruction's signed direct-branch displacement. */
static int block_displacement(const PackBlock *b, int32_t *out) {
    size_t p = 0;
    uint32_t core = 0, imm = 0;
    int have = 0;
    while (p < b->nwords) {
        core = b->words[p++];
        uint32_t addr = 0;
        if (IR_HAS_ADDR(core)) {
            if (p >= b->nwords) return -1;
            addr = b->words[p++];
            if (IR_A_DISPK(addr) == 2 && p >= b->nwords) return -1;
            if (IR_A_DISPK(addr) == 2) p++;
        }
        if (IR_IMMK(core) == 2) {
            if (p >= b->nwords) return -1;
            imm = b->words[p++];
        } else {
            imm = IR_IMM8U(core);
        }
        have = 1;
    }
    if (!have || p != b->nwords) return -1;
    *out = IR_IMMK(core) == 2 ? (int32_t)imm : IR_IMM8(core);
    return 0;
}

/* Discovery normally supplies every leader, but optimized CRT code can list a
 * backward cold exit block before the callable entry. Promote only such an
 * already discovered leader when the real decoded terminator proves the edge;
 * a missing target is not guessed into this frame. */
static int add_closure_edges(Fn *fn, const PackBlock *b) {
    uint32_t op = b->term_op;
    int conditional = (op >= IR_OP_JCC_BASE && op < IR_OP_JCC_BASE + 16) ||
                      op == IR_OP_LOOP || op == IR_OP_LOOPE ||
                      op == IR_OP_LOOPNE || op == IR_OP_JECXZ;
    if (!conditional) return 0;
    int32_t disp;
    if (block_displacement(b, &disp)) {
        snprintf(fn->error, sizeof fn->error, "malformed direct-branch stream");
        return -1;
    }
    uint32_t fallthrough = b->start + b->x86_len;
    uint32_t target = fallthrough + (uint32_t)disp;
    /* Discovery already covers ordinary in-frame leaders and fallthroughs.
     * Only close the observed cold-block-before-entry gap; treating every
     * omitted edge as internal would swallow shared/tail-call code. */
    if (target >= fn->entry) return 0;
    return promote_closure_range(fn, target) < 0 ? -1 : 0;
}

/* Decode one function into a record. Returns 0 on success, -1 on decode
 * failure (fn->status set). */
static int build_record(Fn *fn, uint8_t **out, size_t *out_size) {
    *out = NULL; *out_size = 0;
    qsort(fn->ranges, fn->nranges, sizeof *fn->ranges, range_cmp);
    PackBlock *blocks = NULL;
    uint32_t nblocks = 0, cap = 0;
    uint64_t insns = 0;
    uint32_t last_start = 0;
    int have_start = 0;
    int failed = 0;
    uint16_t status = DEC_OK;

    for (uint32_t r = 0; r < fn->nranges; ++r) {
        uint32_t start = fn->ranges[r].lo;
        uint32_t hi = fn->ranges[r].hi;
        if (hi <= start) continue;
        /* Discovery ranges are basic-block leaders. Two leaders can overlap
         * when one decoded block runs past another leader (a conditional
         * branch target inside a later block). The interpreter decodes from
         * whichever address control flow reaches, so every leader must have
         * its own record block. Only exact duplicate starts are merged. */
        /* Explicit discovery ranges before the entry remain excluded: they
         * are shared cold blocks owned by other functions. A closure-added
         * pre-entry leader is different: a decoded conditional branch in this
         * function proved that it is reachable in this frame. Check this before
         * the duplicate-start merge: the closure copy intentionally has the
         * same start as the excluded discovery range. */
        if (start < fn->entry && !fn->ranges[r].closure) continue;
        if (have_start && start == last_start) continue;
        last_start = start;
        have_start = 1;
        if (start >= fn->end) {
            failed = 1; status = DEC_BAD_RANGE;
            snprintf(fn->error, sizeof fn->error, "leader %08x outside entry..end", start);
            break;
        }

        uint32_t pos = start;
        unsigned splits = 0;
        while (pos < hi) {
            /* n3ds_ir_encode_block() stops at the first terminator. A call is
             * a terminator for the interpreter (it pushes the return address
             * and resumes at a fresh block), but discovery keeps the call and
             * the code after it in one span. Emit the continuation as its own
             * block so the predecoded record covers the return address. */
            if (++splits > 4096u) {
                failed = 1; status = DEC_TOO_LARGE;
                snprintf(fn->error, sizeof fn->error, "more than 4096 blocks in one span");
                break;
            }
            uint32_t words_cap = 4096;
            uint32_t *words = (uint32_t *)malloc(words_cap * 4);
            if (!words) {
                failed = 1; status = DEC_TOO_LARGE;
                snprintf(fn->error, sizeof fn->error, "out of memory");
                break;
            }
            IrBlockInfo info;
            memset(&info, 0, sizeof info);
            size_t n = n3ds_ir_encode_block(NULL, pos, fn->end, words, words_cap, &info, 1);
            if (!n || info.end_ip <= pos) {
                free(words);
                /* Closure is an enrichment, not a reason to invalidate an
                 * otherwise decoded function when discovery excluded a shared
                 * or non-decodable pre-entry target. If control flow reaches
                 * it, the runtime still raises a named missing-block fault. */
                if (fn->ranges[r].closure) {
                    fn->error[0] = 0;
                    pos = hi;
                    break;
                }
                failed = 1;
                status = classify(info.error);
                snprintf(fn->error, sizeof fn->error, "%s", info.error);
                break;
            }
            if (nblocks == cap) {
                cap = cap ? cap * 2 : 32;
                PackBlock *grown = (PackBlock *)realloc(blocks, cap * sizeof *blocks);
                if (!grown) {
                    free(words);
                    failed = 1; status = DEC_TOO_LARGE;
                    snprintf(fn->error, sizeof fn->error, "out of memory");
                    break;
                }
                blocks = grown;
            }
            PackBlock *b = &blocks[nblocks++];
            b->start = pos;
            b->x86_len = info.end_ip - pos;
            b->insn_count = info.insn_count;
            b->nwords = (uint32_t)(n / 4);
            b->term_op = info.term_op;
            b->words = words;
            if (add_closure_edges(fn, b)) {
                failed = 1;
                status = DEC_BAD_RANGE;
                break;
            }
            if ((info.term_op != IR_OP_CALL && info.term_op != IR_OP_CALL_IND) ||
                info.end_ip >= hi)
                break;
            pos = info.end_ip;
        }
        if (failed) break;
    }
    if (!failed && nblocks == 0) {
        failed = 1; status = DEC_BAD_RANGE;
        snprintf(fn->error, sizeof fn->error, "no decodable discovery leader");
    }
    if (!failed) {
        /* Splits after a call can coincide with a later discovery leader.
         * Sort so the record's block table stays binary-searchable, then keep
         * exactly one block per start address. */
        qsort(blocks, nblocks, sizeof *blocks, block_cmp);
        uint32_t w = 0;
        for (uint32_t i = 0; i < nblocks; ++i) {
            if (w && blocks[i].start == blocks[w - 1].start) {
                free(blocks[i].words);
                continue;
            }
            blocks[w++] = blocks[i];
        }
        nblocks = w;
        for (uint32_t i = 0; i < nblocks; ++i) insns += blocks[i].insn_count;
    }
    if (!failed && nblocks > 0xffffu) {
        failed = 1; status = DEC_TOO_LARGE;
        snprintf(fn->error, sizeof fn->error, "more than 65535 blocks");
    }
    if (!failed && insns > 0xffffffffu) {
        failed = 1; status = DEC_TOO_LARGE;
        snprintf(fn->error, sizeof fn->error, "instruction count overflow");
    }

    if (failed) {
        free_blocks(blocks, nblocks);
        fn->status = status;
        fn->flags |= FN_INVALID;
        return -1;
    }

    size_t stream_begin = align4(IRC_RECORD_HEADER + (size_t)nblocks * IRC_BLOCK_ENTRY);
    size_t stream_bytes = 0;
    for (uint32_t i = 0; i < nblocks; ++i) stream_bytes += (size_t)blocks[i].nwords * 4;
    size_t total = align8(stream_begin + stream_bytes);
    uint8_t *rec = (uint8_t *)calloc(1, total);
    if (!rec) {
        free_blocks(blocks, nblocks);
        fn->status = DEC_TOO_LARGE;
        fn->flags |= FN_INVALID;
        snprintf(fn->error, sizeof fn->error, "record allocation failed");
        return -1;
    }

    wr32(rec + 0, 0x46435249u); /* IRCF */
    wr16(rec + 4, IRC_VERSION);
    wr16(rec + 6, 1);
    wr32(rec + 8, fn->entry);
    wr32(rec + 12, fn->end);
    wr32(rec + 16, nblocks);
    wr32(rec + 20, (uint32_t)insns);
    wr32(rec + 24, IRC_RECORD_HEADER);
    wr32(rec + 28, nblocks * IRC_BLOCK_ENTRY);
    wr32(rec + 32, (uint32_t)stream_bytes);

    size_t off = stream_begin;
    for (uint32_t i = 0; i < nblocks; ++i) {
        PackBlock *b = &blocks[i];
        uint8_t *p = rec + IRC_RECORD_HEADER + (size_t)i * IRC_BLOCK_ENTRY;
        wr32(p + 0, b->start);
        wr32(p + 4, (uint32_t)off);
        wr32(p + 8, b->nwords);
        wr16(p + 12, (uint16_t)b->x86_len);
        p[14] = (uint8_t)b->insn_count;
        p[15] = 1; /* terminator */
        memcpy(rec + off, b->words, (size_t)b->nwords * 4);
        off += (size_t)b->nwords * 4;
    }
    int preentry = nblocks && blocks[0].start < fn->entry;
    free_blocks(blocks, nblocks);
    *out = rec;
    *out_size = total;
    fn->status = DEC_OK;
    fn->flags = (fn->flags & (FN_AOT | FN_FORCED)) | FN_DECODED | FN_COMPLETE;
    if (preentry) fn->flags |= FN_PREENTRY;
    fn->blocks = (uint16_t)nblocks;
    fn->insns = (uint32_t)insns;
    return 0;
}

/* ---------------------------------------------------------------- report */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void json_escape(FILE *f, const char *s) {
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fprintf(f, "\\n");
        else if (c == '\r') fprintf(f, "\\r");
        else if (c == '\t') fprintf(f, "\\t");
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

static void write_report(const char *path, const Fn *fns, uint32_t n,
                         const char *out_path, uint64_t file_bytes,
                         uint64_t index_bytes, uint64_t data_bytes,
                         uint32_t index_crc, uint32_t data_crc) {
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    uint32_t counts[8] = {0};
    uint64_t total_insns = 0, total_blocks = 0;
    uint32_t aot = 0, forced = 0, decoded = 0, invalid = 0;
    uint32_t *sizes = (uint32_t *)malloc((size_t)(n ? n : 1) * sizeof *sizes);
    uint32_t nsz = 0;
    uint32_t min_size = 0xffffffffu, max_size = 0;
    for (uint32_t i = 0; i < n; ++i) {
        counts[fns[i].status < 8 ? fns[i].status : 7]++;
        if (fns[i].flags & FN_DECODED) {
            decoded++;
            total_insns += fns[i].insns;
            total_blocks += fns[i].blocks;
            sizes[nsz++] = fns[i].record_size;
            if (fns[i].record_size < min_size) min_size = fns[i].record_size;
            if (fns[i].record_size > max_size) max_size = fns[i].record_size;
        } else {
            invalid++;
        }
        if (fns[i].flags & FN_AOT) aot++;
        if (fns[i].flags & FN_FORCED) forced++;
    }
    qsort(sizes, nsz, sizeof *sizes, cmp_u32);
    uint32_t p50 = nsz ? sizes[nsz / 2] : 0;
    uint32_t p90 = nsz ? sizes[(size_t)nsz * 90 / 100] : 0;
    uint32_t p99 = nsz ? sizes[(size_t)nsz * 99 / 100] : 0;
    double avg = nsz ? (double)data_bytes / nsz : 0.0;

    char file_json[512];
    size_t fj = 0;
    for (size_t i = 0; out_path[i] && fj + 2 < sizeof file_json; ++i)
        file_json[fj++] = out_path[i] == '\\' ? '/' : out_path[i];
    file_json[fj] = 0;

    fprintf(f, "{\n");
    fprintf(f, "  \"format\": \"n3ds-ir-container\",\n");
    fprintf(f, "  \"version\": %u,\n", IRC_VERSION);
    fprintf(f, "  \"file\": \"%s\",\n", file_json);
    fprintf(f, "  \"file_bytes\": %llu,\n", (unsigned long long)file_bytes);
    fprintf(f, "  \"header_bytes\": %u,\n", IRC_HEADER);
    fprintf(f, "  \"index_bytes\": %llu,\n", (unsigned long long)index_bytes);
    fprintf(f, "  \"data_bytes\": %llu,\n", (unsigned long long)data_bytes);
    fprintf(f, "  \"index_crc32\": %u,\n", index_crc);
    fprintf(f, "  \"data_crc32\": %u,\n", data_crc);
    fprintf(f, "  \"functions\": %u,\n", n);
    fprintf(f, "  \"decoded\": %u,\n", decoded);
    fprintf(f, "  \"invalid\": %u,\n", invalid);
    fprintf(f, "  \"aot\": %u,\n", aot);
    fprintf(f, "  \"forced\": %u,\n", forced);
    fprintf(f, "  \"blocks\": %llu,\n", (unsigned long long)total_blocks);
    fprintf(f, "  \"insns\": %llu,\n", (unsigned long long)total_insns);
    fprintf(f, "  \"status\": {\"ok\":%u,\"unsupported\":%u,\"no_terminator\":%u,"
               "\"empty\":%u,\"bad_range\":%u,\"too_large\":%u,\"not_decoded\":%u,"
               "\"unknown\":%u},\n",
            counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
            counts[6], counts[7]);
    fprintf(f, "  \"record_size\": {\"min\":%u,\"max\":%u,\"avg\":%.1f,"
               "\"p50\":%u,\"p90\":%u,\"p99\":%u},\n",
            nsz ? min_size : 0, max_size, avg, p50, p90, p99);
    /* Size histogram: powers of two up to 64 KiB. */
    fprintf(f, "  \"record_size_hist\": {");
    static const uint32_t edges[] = {64, 128, 256, 512, 1024, 2048, 4096,
                                     8192, 16384, 32768, 65536, 0xffffffffu};
    const uint32_t nedges = sizeof edges / sizeof edges[0];
    for (uint32_t k = 0; k < nedges; ++k) {
        uint32_t lo = k ? edges[k - 1] : 0, hi = edges[k];
        uint64_t count = 0;
        for (uint32_t i = 0; i < nsz; ++i)
            if (sizes[i] >= lo && sizes[i] < hi) count++;
        if (hi == 0xffffffffu)
            fprintf(f, "%s\"%u+\": %llu", k ? "," : "", lo,
                    (unsigned long long)count);
        else
            fprintf(f, "%s\"%u-%u\": %llu", k ? "," : "", lo, hi,
                    (unsigned long long)count);    }
    fprintf(f, "},\n");
    fprintf(f, "  \"first_invalid\": [");
    unsigned shown = 0;
    for (uint32_t i = 0; i < n && shown < 64; ++i) {
        if (fns[i].flags & FN_DECODED) continue;
        fprintf(f, "%s{\"addr\":%u,\"status\":%u,\"end\":%u,\"error\":\"", shown ? "," : "",
                fns[i].entry, fns[i].status, fns[i].end);
        json_escape(f, fns[i].error);
        fprintf(f, "\"}");
        shown++;
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    free(sizes);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s INPUT.bin OUTPUT.irc [--aot FILE] [--forced FILE] "
                        "[--report FILE] [--index-csv FILE]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1], *out_path = argv[2];
    const char *aot_path = NULL, *forced_path = NULL, *report_path = NULL,
               *csv_path = NULL, *invalid_path = NULL;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--aot") && i + 1 < argc) aot_path = argv[++i];
        else if (!strcmp(argv[i], "--forced") && i + 1 < argc) forced_path = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report_path = argv[++i];
        else if (!strcmp(argv[i], "--index-csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--invalid-csv") && i + 1 < argc) invalid_path = argv[++i];
        else { fprintf(stderr, "pack: unknown argument %s\n", argv[i]); return 2; }
    }

    FILE *in = fopen(in_path, "rb");
    if (!in) { perror(in_path); return 2; }
    uint32_t magic, base, blob_bytes, nfun;
    if (read_u32(in, &magic) || read_u32(in, &base) || read_u32(in, &blob_bytes) ||
        read_u32(in, &nfun) || magic != 0x49524d31u) {
        fprintf(stderr, "pack: bad input header\n");
        return 2;
    }
    if (!nfun || nfun > 4000000u || blob_bytes > (1u << 28)) {
        fprintf(stderr, "pack: implausible input counts\n");
        return 2;
    }
    g_base = base;
    g_size = blob_bytes;
    g_code = (uint8_t *)malloc(blob_bytes ? blob_bytes : 1);
    if (!g_code || fread(g_code, 1, blob_bytes, in) != blob_bytes) {
        fprintf(stderr, "pack: truncated image blob\n");
        return 2;
    }

    Fn *fns = (Fn *)calloc(nfun, sizeof *fns);
    if (!fns) return 2;
    for (uint32_t i = 0; i < nfun; ++i) {
        uint32_t entry, fend, nr;
        if (read_u32(in, &entry) || read_u32(in, &fend) || read_u32(in, &nr)) {
            fprintf(stderr, "pack: truncated function table\n");
            return 2;
        }
        if (nr > 1000000u) { fprintf(stderr, "pack: implausible range count\n"); return 2; }
        Range *rs = nr ? (Range *)malloc((size_t)nr * sizeof *rs) : NULL;
        for (uint32_t k = 0; k < nr; ++k) {
            uint32_t lo, hi;
            if (read_u32(in, &lo) || read_u32(in, &hi)) {
                fprintf(stderr, "pack: truncated ranges\n");
                return 2;
            }
            if (rs) { rs[k].lo = lo; rs[k].hi = hi; rs[k].closure = 0; }
        }
        fns[i].entry = entry;
        fns[i].end = fend > entry ? fend : entry + 1;
        fns[i].nranges = nr;
        fns[i].ranges = rs;
    }
    fclose(in);

    qsort(fns, nfun, sizeof *fns, fn_cmp);
    for (uint32_t i = 1; i < nfun; ++i) {
        if (fns[i - 1].entry == fns[i].entry) {
            fprintf(stderr, "pack: duplicate function entry %08x\n", fns[i].entry);
            return 2;
        }
    }

    uint32_t *aot = NULL, *forced = NULL;
    unsigned naot = 0, nforced = 0;
    if (read_list(aot_path, &aot, &naot) || read_list(forced_path, &forced, &nforced))
        return 2;
    qsort(aot, naot, sizeof *aot, list_cmp);
    qsort(forced, nforced, sizeof *forced, list_cmp);
    for (uint32_t i = 0; i < nfun; ++i) {
        if (in_list(aot, naot, fns[i].entry)) fns[i].flags |= FN_AOT;
        if (in_list(forced, nforced, fns[i].entry)) fns[i].flags |= FN_FORCED;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 2; }
    uint64_t index_bytes = (uint64_t)nfun * IRC_INDEX_ENTRY;
    uint64_t data_off = align8((uint64_t)IRC_HEADER + index_bytes);
    uint8_t *zeros = (uint8_t *)calloc(1, 65536);
    if (!zeros) return 2;
    for (uint64_t left = data_off; left;) {
        size_t n = left > 65536 ? 65536 : (size_t)left;
        if (fwrite(zeros, 1, n, out) != n) { fprintf(stderr, "pack: write failed\n"); return 2; }
        left -= n;
    }
    free(zeros);

    crc_init();
    uint32_t data_crc = 0xffffffffu;
    uint64_t data_pos = 0;
    uint8_t *index_raw = (uint8_t *)calloc(1, (size_t)index_bytes);
    if (!index_raw) return 2;

    for (uint32_t i = 0; i < nfun; ++i) {
        Fn *fn = &fns[i];
        uint8_t *rec = NULL;
        size_t rec_size = 0;
        build_record(fn, &rec, &rec_size);
        if (rec) {
            uint64_t off = data_off + data_pos;
            if (off > 0xffffffffu) { fprintf(stderr, "pack: container > 4 GiB\n"); return 2; }
            if ((off & 7u) != 0) { /* cannot happen: every record is 8-aligned */ return 2; }
            fn->record_off = (uint32_t)off;
            fn->record_size = (uint32_t)rec_size;
            fn->record_crc = crc_of(rec, rec_size);
            if (fwrite(rec, 1, rec_size, out) != rec_size) {
                fprintf(stderr, "pack: write failed\n");
                return 2;
            }
            data_crc = crc_update(data_crc, rec, rec_size);
            data_pos += rec_size;
            free(rec);
        }
        uint8_t *e = index_raw + (size_t)i * IRC_INDEX_ENTRY;
        wr32(e + 0, fn->entry);
        wr32(e + 4, fn->end);
        wr32(e + 8, fn->flags);
        wr16(e + 12, fn->status);
        wr16(e + 14, fn->blocks);
        wr32(e + 16, fn->insns);
        wr32(e + 20, fn->record_off);
        wr32(e + 24, fn->record_size);
        wr32(e + 28, fn->record_crc);
    }

    uint32_t data_crc_final = data_crc ^ 0xffffffffu;
    uint32_t index_crc = crc_of(index_raw, (size_t)index_bytes);
    if (fseek(out, (long)IRC_HEADER, SEEK_SET) ||
        fwrite(index_raw, 1, (size_t)index_bytes, out) != (size_t)index_bytes) {
        fprintf(stderr, "pack: index write failed\n");
        return 2;
    }

    uint8_t h[IRC_HEADER];
    memset(h, 0, sizeof h);
    memcpy(h, "IRCNT001", 8);
    wr32(h + 8, IRC_VERSION);
    wr32(h + 12, IRC_HEADER);
    uint32_t file_flags = (1u << 3) | (1u << 2);
    if (naot) file_flags |= 1u << 0;
    if (nforced) file_flags |= 1u << 1;
    wr32(h + 16, file_flags);
    wr32(h + 20, nfun);
    uint32_t ndec = 0;
    for (uint32_t i = 0; i < nfun; ++i) ndec += (fns[i].flags & FN_DECODED) != 0;
    wr32(h + 24, ndec);
    wr32(h + 28, IRC_HEADER);
    wr32(h + 32, (uint32_t)index_bytes);
    wr32(h + 36, IRC_INDEX_ENTRY);
    wr32(h + 40, (uint32_t)data_off);
    wr32(h + 44, (uint32_t)data_pos);
    wr32(h + 48, index_crc);
    wr32(h + 52, data_crc_final);
    wr32(h + 56, 0);
    uint64_t total = data_off + data_pos;
    if (total > 0xffffffffu) { fprintf(stderr, "pack: container > 4 GiB\n"); return 2; }
    wr32(h + 60, (uint32_t)total);
    wr32(h + 56, crc_of(h, sizeof h));
    if (fseek(out, 0, SEEK_SET) || fwrite(h, 1, sizeof h, out) != sizeof h) {
        fprintf(stderr, "pack: header write failed\n");
        return 2;
    }
    if (fclose(out)) { fprintf(stderr, "pack: close failed\n"); return 2; }

    if (csv_path) {
        FILE *cf = fopen(csv_path, "w");
        if (!cf) { perror(csv_path); return 2; }
        fprintf(cf, "addr,end,record_size,blocks,insns,status,flags\n");
        for (uint32_t i = 0; i < nfun; ++i)
            fprintf(cf, "%08x,%08x,%u,%u,%u,%u,%u\n", fns[i].entry, fns[i].end,
                    fns[i].record_size, fns[i].blocks, fns[i].insns,
                    fns[i].status, fns[i].flags);
        fclose(cf);
    }
    if (invalid_path) {
        FILE *vf = fopen(invalid_path, "w");
        if (!vf) { perror(invalid_path); return 2; }
        fprintf(vf, "addr,end,status,error\n");
        for (uint32_t i = 0; i < nfun; ++i) {
            if (fns[i].flags & FN_DECODED) continue;
            fprintf(vf, "%08x,%08x,%u,\"", fns[i].entry, fns[i].end, fns[i].status);
            for (const char *p = fns[i].error; p && *p; ++p) {
                if (*p == '"' || *p == '\\') fputc('\\', vf);
                fputc(*p, vf);
            }
            fprintf(vf, "\"\n");
        }
        fclose(vf);
    }

    write_report(report_path, fns, nfun, out_path, total, index_bytes, data_pos,
                 index_crc, data_crc_final);

    printf("packed %u functions (%u decoded, %u invalid) into %s: "
           "%llu bytes (index %llu, data %llu)\n",
           nfun, ndec, nfun - ndec, out_path, (unsigned long long)total,
           (unsigned long long)index_bytes, (unsigned long long)data_pos);
    return 0;
}
