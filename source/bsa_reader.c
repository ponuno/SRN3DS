#include "bsa_reader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define BSA_FSEEK _fseeki64
#define BSA_FTELL _ftelli64
#else
#include <sys/types.h>
#define BSA_FSEEK fseeko
#define BSA_FTELL ftello
#endif

/* Compact directory.  Paths stay in the archive: a lookup probes a u32
 * open-addressed table, then reads the candidate's record/name through the
 * bounded page cache to compare the exact name.  The archive's 64-bit record
 * hash is returned to callers but the lookup key is our own case-folded path
 * hash, so the on-disk hash algorithm is still never guessed. */
typedef struct {
    uint32_t records_at;    /* absolute offset of the first 16-byte record */
    uint32_t name_at;       /* absolute offset of the raw folder name */
    uint32_t first_entry;   /* first global entry index in this folder */
    uint32_t count;         /* file records in this folder */
    uint16_t name_stored;   /* stored bytes, including the trailing NUL */
    uint16_t name_length;   /* canonical length, without the NUL */
} BsaFolderInternal;

/* Bounded read-through page cache.  Not charged to max_index_bytes: the
 * resident index stays under 2 MiB while the cache is a separate, fixed cost
 * (BSA_PAGE_CACHE_BYTES per archive). */
#define BSA_PAGE_CACHE_BYTES (32u * 1024u)
#define BSA_NAME_GROUP 16u

struct BsaArchive {
    BsaReadAt read_at;
    void *ctx;
    void *owned_ctx;
    FILE *file;
    uint64_t size;
    BsaOptions opt;
    BsaHeader header;
    BsaFolderInternal *folders;
    uint32_t *table;
    uint8_t *tags;              /* 8-bit fingerprint; skips name reads on miss */
    size_t table_size;
    uint32_t *name_checkpoints;  /* block-relative offsets, every 16 names */
    size_t name_checkpoint_count;
    uint64_t name_block_at;
    uint32_t name_block_len;
    size_t index_bytes;
    int alloc_budget;
    uint8_t *page;
    size_t page_cap;
    uint64_t page_off;
    size_t page_len;
};

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

const char *bsa_status_name(BsaStatus status) {
    switch (status) {
    case BSA_OK: return "ok";
    case BSA_ERR_ARGUMENT: return "bad argument";
    case BSA_ERR_ACCESS: return "cannot open archive";
    case BSA_ERR_IO: return "I/O error";
    case BSA_ERR_NOT_FOUND: return "not found";
    case BSA_ERR_MAGIC: return "not a BSA";
    case BSA_ERR_VERSION: return "unsupported BSA version";
    case BSA_ERR_BOUNDS: return "offset/length outside the archive";
    case BSA_ERR_COUNT: return "implausible directory count";
    case BSA_ERR_NAME: return "invalid archive name";
    case BSA_ERR_DUPLICATE: return "duplicate archive path";
    case BSA_ERR_UNSUPPORTED: return "unsupported archive feature";
    case BSA_ERR_FORMAT: return "archive layout mismatch";
    case BSA_ERR_TRUNCATED: return "truncated archive";
    case BSA_ERR_DEFLATE: return "deflate stream error";
    case BSA_ERR_BUDGET: return "configured limit exceeded";
    default: return "unknown BSA error";
    }
}

void bsa_options_default(BsaOptions *options) {
    if (!options) return;
    options->max_folders = BSA_DEFAULT_MAX_FOLDERS;
    options->max_files = BSA_DEFAULT_MAX_FILES;
    options->max_name_bytes = BSA_DEFAULT_MAX_NAME_BYTES;
    options->max_index_bytes = BSA_DEFAULT_MAX_INDEX_BYTES;
    options->max_stored_bytes = BSA_DEFAULT_MAX_STORED_BYTES;
    options->max_unpacked_bytes = BSA_DEFAULT_MAX_UNPACKED_BYTES;
    options->verify_embedded_names = 1;
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static BsaStatus read_exact(const BsaArchive *a, uint64_t offset, void *dst,
                            size_t bytes, char *reason, size_t reason_cap) {
    if (!bytes) return BSA_OK;
    if (!dst) {
        set_reason(reason, reason_cap, "read destination is null");
        return BSA_ERR_ARGUMENT;
    }
    if (offset > a->size || (uint64_t)bytes > a->size - offset) {
        set_reason(reason, reason_cap,
                   "read %llu..%llu is outside the %llu-byte archive",
                   (unsigned long long)offset,
                   (unsigned long long)(offset + bytes),
                   (unsigned long long)a->size);
        return BSA_ERR_BOUNDS;
    }
    if (a->read_at(a->ctx, offset, dst, bytes) != 0) {
        set_reason(reason, reason_cap, "read failed at offset %llu",
                   (unsigned long long)offset);
        return BSA_ERR_IO;
    }
    return BSA_OK;
}

static void *index_alloc(BsaArchive *a, size_t bytes) {
    if (bytes > a->opt.max_index_bytes ||
        a->index_bytes > (size_t)a->opt.max_index_bytes - bytes) {
        a->alloc_budget = 1;
        return NULL;
    }
    void *p = calloc(1, bytes);
    if (p) a->index_bytes += bytes;
    return p;
}

static BsaStatus alloc_failure(const BsaArchive *a) {
    return a->alloc_budget ? BSA_ERR_BUDGET : BSA_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* zlib / raw deflate (RFC 1950 / 1951), bounded output.               */
/* ------------------------------------------------------------------ */

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMBOLS 288

typedef struct {
    uint16_t count[HUFF_MAX_BITS + 1];
    uint16_t symbol[HUFF_MAX_SYMBOLS];
    unsigned used;
} Huffman;

typedef struct {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bitbuf;
    unsigned bitcnt;
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
    size_t extra;
    int allow_partial;
    int partial;
} InflateState;

static int inf_bits(InflateState *s, unsigned need, uint32_t *value) {
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    *value = s->bitbuf & ((1u << need) - 1u);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return 0;
}

static void inf_align(InflateState *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
}

static void inf_put(InflateState *s, uint8_t byte) {
    if (s->out_pos < s->out_cap) s->out[s->out_pos++] = byte;
    else s->extra++;
}

static int huff_build(Huffman *h, const uint8_t *lengths, unsigned count) {
    uint16_t offs[HUFF_MAX_BITS + 2];
    memset(h, 0, sizeof *h);
    if (count > HUFF_MAX_SYMBOLS) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (lengths[i] > HUFF_MAX_BITS) return -1;
        h->count[lengths[i]]++;
    }
    int left = 1;
    for (unsigned len = 1; len <= HUFF_MAX_BITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (unsigned len = 1; len < HUFF_MAX_BITS; ++len)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    h->used = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!lengths[i]) continue;
        unsigned slot = offs[lengths[i]]++;
        if (slot >= HUFF_MAX_SYMBOLS) return -1;
        h->symbol[slot] = (uint16_t)i;
        h->used++;
    }
    return 0;
}

static int huff_decode(InflateState *s, const Huffman *h, int *symbol) {
    int code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= HUFF_MAX_BITS; ++len) {
        uint32_t bit;
        if (inf_bits(s, 1, &bit)) return -1;
        code |= (int)bit;
        int count = h->count[len];
        if (code - count < first) {
            int at = index + (code - first);
            if (at < 0 || (unsigned)at >= h->used) return -1;
            *symbol = h->symbol[at];
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inflate_codes(InflateState *s, const Huffman *lit,
                         const Huffman *dist) {
    for (;;) {
        int sym;
        if (huff_decode(s, lit, &sym)) return -1;
        if (sym < 256) {
            inf_put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            if (sym < 257 || sym > 285) return -1;
            unsigned li = (unsigned)sym - 257u;
            uint32_t extra = 0;
            if (len_extra[li] && inf_bits(s, len_extra[li], &extra)) return -1;
            unsigned length = len_base[li] + extra;
            int dsym;
            if (huff_decode(s, dist, &dsym)) return -1;
            if (dsym < 0 || dsym > 29) return -1;
            extra = 0;
            if (dist_extra[dsym] && inf_bits(s, dist_extra[dsym], &extra)) return -1;
            unsigned distance = dist_base[dsym] + extra;
            if (!distance || distance > s->out_pos) return -1;
            for (unsigned i = 0; i < length; ++i)
                inf_put(s, s->out[s->out_pos - distance]);
        }
        if (s->allow_partial && s->out_pos == s->out_cap) {
            s->partial = 1;
            return 0;
        }
    }
}

static int inflate_fixed(InflateState *s) {
    uint8_t lengths[288];
    unsigned i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    Huffman lit, dist;
    if (huff_build(&lit, lengths, 288)) return -1;
    uint8_t dl[32];
    memset(dl, 5, sizeof dl);
    if (huff_build(&dist, dl, 32)) return -1;
    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(InflateState *s) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint32_t hlit, hdist, hclen;
    if (inf_bits(s, 5, &hlit) || inf_bits(s, 5, &hdist) ||
        inf_bits(s, 4, &hclen)) return -1;
    hlit += 257; hdist += 1; hclen += 4;
    if (hlit > 286 || hdist > 32) return -1;
    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof cl_lengths);
    for (unsigned i = 0; i < hclen; ++i) {
        uint32_t v;
        if (inf_bits(s, 3, &v)) return -1;
        cl_lengths[order[i]] = (uint8_t)v;
    }
    Huffman cl;
    if (huff_build(&cl, cl_lengths, 19)) return -1;
    uint8_t lengths[288 + 32];
    memset(lengths, 0, sizeof lengths);
    unsigned total = hlit + hdist, at = 0;
    while (at < total) {
        int sym;
        if (huff_decode(s, &cl, &sym)) return -1;
        if (sym < 16) {
            lengths[at++] = (uint8_t)sym;
        } else if (sym == 16) {
            uint32_t rep;
            if (!at || inf_bits(s, 2, &rep)) return -1;
            rep += 3;
            if (at + rep > total) return -1;
            uint8_t prev = lengths[at - 1];
            while (rep--) lengths[at++] = prev;
        } else if (sym == 17) {
            uint32_t rep;
            if (inf_bits(s, 3, &rep)) return -1;
            rep += 3;
            if (at + rep > total) return -1;
            while (rep--) lengths[at++] = 0;
        } else if (sym == 18) {
            uint32_t rep;
            if (inf_bits(s, 7, &rep)) return -1;
            rep += 11;
            if (at + rep > total) return -1;
            while (rep--) lengths[at++] = 0;
        } else {
            return -1;
        }
    }
    Huffman lit, dist;
    if (huff_build(&lit, lengths, hlit)) return -1;
    if (huff_build(&dist, lengths + hlit, hdist)) return -1;
    return inflate_codes(s, &lit, &dist);
}

static uint32_t adler32_bytes(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a += p[i];
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static BsaStatus inflate_raw(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t cap, size_t *out_len,
                             int allow_partial, uint32_t expected_adler,
                             int verify_adler, size_t *consumed, int *ended,
                             char *reason, size_t reason_cap) {
    InflateState s;
    memset(&s, 0, sizeof s);
    s.in = src;
    s.in_len = src_len;
    s.out = dst;
    s.out_cap = cap;
    s.allow_partial = allow_partial;

    uint32_t final = 0;
    while (!final) {
        uint32_t type;
        if (inf_bits(&s, 1, &final) || inf_bits(&s, 2, &type)) {
            set_reason(reason, reason_cap, "deflate: truncated block header");
            return BSA_ERR_DEFLATE;
        }
        if (type == 0) {
            inf_align(&s);
            if (s.in_pos + 4 > s.in_len) {
                set_reason(reason, reason_cap, "deflate: truncated stored block");
                return BSA_ERR_DEFLATE;
            }
            unsigned len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            unsigned nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if ((len ^ 0xffffu) != nlen) {
                set_reason(reason, reason_cap, "deflate: stored LEN/NLEN mismatch");
                return BSA_ERR_DEFLATE;
            }
            if (s.in_pos + len > s.in_len) {
                set_reason(reason, reason_cap, "deflate: truncated stored payload");
                return BSA_ERR_DEFLATE;
            }
            for (unsigned i = 0; i < len; ++i) inf_put(&s, s.in[s.in_pos++]);
            if (s.allow_partial && s.out_pos == s.out_cap) {
                s.partial = 1;
                break;
            }
        } else if (type == 1) {
            if (inflate_fixed(&s)) {
                set_reason(reason, reason_cap, "deflate: bad fixed-Huffman stream");
                return BSA_ERR_DEFLATE;
            }
            if (s.partial) break;
        } else if (type == 2) {
            if (inflate_dynamic(&s)) {
                set_reason(reason, reason_cap, "deflate: bad dynamic-Huffman stream");
                return BSA_ERR_DEFLATE;
            }
            if (s.partial) break;
        } else {
            set_reason(reason, reason_cap, "deflate: reserved block type 3");
            return BSA_ERR_DEFLATE;
        }
    }

    if (!s.partial && s.extra) {
        set_reason(reason, reason_cap,
                   "deflate: produced %llu bytes beyond the %llu-byte budget",
                   (unsigned long long)s.extra,
                   (unsigned long long)cap);
        return BSA_ERR_FORMAT;
    }
    if (!s.partial && verify_adler &&
        adler32_bytes(dst ? dst : (const uint8_t *)"", s.out_pos) != expected_adler) {
        set_reason(reason, reason_cap, "zlib: Adler-32 mismatch");
        return BSA_ERR_FORMAT;
    }
    if (consumed) *consumed = s.in_pos;
    if (ended) *ended = !s.partial;
    *out_len = s.out_pos;
    return BSA_OK;
}

static BsaStatus zlib_stream(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t cap, size_t *out_len,
                             int allow_partial, size_t *consumed, int *ended,
                             char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!src || src_len < 6 || (!dst && cap) || !out_len) {
        set_reason(reason, reason_cap,
                   "inflate needs a zlib stream, output pointer and length");
        return BSA_ERR_ARGUMENT;
    }
    unsigned cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0) {
        set_reason(reason, reason_cap, "zlib: bad CMF/FLG header");
        return BSA_ERR_DEFLATE;
    }
    if (flg & 0x20) {
        set_reason(reason, reason_cap, "zlib: preset dictionaries are unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    uint32_t expected = ((uint32_t)src[src_len - 4] << 24) |
                        ((uint32_t)src[src_len - 3] << 16) |
                        ((uint32_t)src[src_len - 2] << 8) |
                        (uint32_t)src[src_len - 1];
    return inflate_raw(src + 2, src_len - 6, dst, cap, out_len, allow_partial,
                       expected, !allow_partial, consumed, ended,
                       reason, reason_cap);
}

BsaStatus bsa_inflate_zlib(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t cap, size_t *out_len,
                           int allow_partial, char *reason, size_t reason_cap) {
    return zlib_stream(src, src_len, dst, cap, out_len, allow_partial,
                       NULL, NULL, reason, reason_cap);
}

BsaStatus bsa_inflate_zlib_exact(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t cap, size_t *out_len,
                                 char *reason, size_t reason_cap) {
    size_t consumed = 0;
    int ended = 0;
    BsaStatus st = zlib_stream(src, src_len, dst, cap, out_len, 0,
                               &consumed, &ended, reason, reason_cap);
    if (st != BSA_OK) return st;
    if (!ended) {
        set_reason(reason, reason_cap, "zlib: stream did not reach its final block");
        return BSA_ERR_DEFLATE;
    }
    if (*out_len != cap) {
        set_reason(reason, reason_cap,
                   "zlib: produced %llu bytes, expected %llu",
                   (unsigned long long)*out_len, (unsigned long long)cap);
        return BSA_ERR_FORMAT;
    }
    if (consumed != src_len - 6) {
        set_reason(reason, reason_cap,
                   "zlib: %llu trailing bytes after the deflate stream",
                   (unsigned long long)(src_len - 6 - consumed));
        return BSA_ERR_FORMAT;
    }
    return BSA_OK;
}

uint32_t bsa_crc32(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* Names and archive directory.                                        */
/* ------------------------------------------------------------------ */

static size_t canonical_path(const char *in, char *out, size_t cap) {
    if (!in || !out || cap < 2) return 0;
    size_t n = strlen(in);
    size_t start = 0, end = n;
    while (start < end && (in[start] == '\\' || in[start] == '/')) ++start;
    while (end > start && (in[end - 1] == '\\' || in[end - 1] == '/')) --end;
    size_t len = end - start;
    if (!len || len >= cap) return 0;
    for (size_t i = 0; i < len; ++i) {
        char c = in[start + i];
        out[i] = c == '/' ? '\\' : c;
    }
    out[len] = 0;
    return len;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int path_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static uint64_t path_hash64(const char *path) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h ^= c;
        h *= 1099511628211ull;
    }
    /* Final avalanche so the low bits used for the slot are well mixed. */
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

static int name_has_separator(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] == '\\' || s[i] == '/') return 1;
    return 0;
}

/* Read through the fixed per-archive cache.  `n` is always small here
 * (16-byte records, name bytes); large payload reads keep using read_exact. */
static BsaStatus cache_read(const BsaArchive *archive, uint64_t offset,
                            void *dst, size_t bytes, char *reason,
                            size_t reason_cap) {
    BsaArchive *a = (BsaArchive *)(uintptr_t)archive;
    if (!bytes) return BSA_OK;
    if (offset > a->size || (uint64_t)bytes > a->size - offset)
        return read_exact(a, offset, dst, bytes, reason, reason_cap);
    if (!a->page || bytes > a->page_cap / 4u)
        return read_exact(a, offset, dst, bytes, reason, reason_cap);
    if (offset >= a->page_off && offset - a->page_off <= a->page_len &&
        bytes <= a->page_len - (size_t)(offset - a->page_off)) {
        memcpy(dst, a->page + (size_t)(offset - a->page_off), bytes);
        return BSA_OK;
    }
    size_t want = a->page_cap;
    if ((uint64_t)want > a->size - offset) want = (size_t)(a->size - offset);
    BsaStatus st = read_exact(a, offset, a->page, want, reason, reason_cap);
    if (st != BSA_OK) { a->page_len = 0; return st; }
    a->page_off = offset;
    a->page_len = want;
    if (bytes <= want) {
        memcpy(dst, a->page, bytes);
        return BSA_OK;
    }
    memcpy(dst, a->page, want);
    return read_exact(a, offset + want, (uint8_t *)dst + want, bytes - want,
                      reason, reason_cap);
}

static const BsaFolderInternal *folder_for(const BsaArchive *a,
                                           uint32_t entry_index) {
    uint32_t lo = 0, hi = a->header.folder_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const BsaFolderInternal *f = &a->folders[mid];
        if (entry_index < f->first_entry) {
            hi = mid;
        } else if (entry_index - f->first_entry >= f->count) {
            lo = mid + 1u;
        } else {
            return f;
        }
    }
    return NULL;
}

static BsaStatus read_record(const BsaArchive *archive, uint32_t entry_index,
                             uint64_t *hash, uint32_t *stored_size,
                             uint32_t *offset, uint32_t *folder_index,
                             char *reason, size_t reason_cap) {
    if (entry_index >= archive->header.file_count) {
        set_reason(reason, reason_cap, "record %u is outside the archive",
                   entry_index);
        return BSA_ERR_ARGUMENT;
    }
    const BsaFolderInternal *f = folder_for(archive, entry_index);
    if (!f) {
        set_reason(reason, reason_cap, "record %u has no folder", entry_index);
        return BSA_ERR_FORMAT;
    }
    uint8_t rec[16];
    uint64_t at = (uint64_t)f->records_at +
                  (uint64_t)(entry_index - f->first_entry) * 16u;
    BsaStatus st = cache_read(archive, at, rec, sizeof rec, reason, reason_cap);
    if (st != BSA_OK) return st;
    if (hash) *hash = get_u64(rec);
    if (stored_size) *stored_size = get_u32(rec + 8);
    if (offset) *offset = get_u32(rec + 12);
    if (folder_index)
        *folder_index = (uint32_t)(f - archive->folders);
    return BSA_OK;
}

static BsaStatus folder_name_canon(const BsaArchive *archive,
                                   const BsaFolderInternal *f, char *out,
                                   size_t cap, char *reason,
                                   size_t reason_cap) {
    char raw[BSA_PATH_MAX];
    if (!f->name_stored || f->name_stored >= BSA_PATH_MAX) {
        set_reason(reason, reason_cap, "stored folder name length is invalid");
        return BSA_ERR_NAME;
    }
    BsaStatus st = cache_read(archive, f->name_at, raw, f->name_stored,
                              reason, reason_cap);
    if (st != BSA_OK) return st;
    uint32_t n = f->name_stored;
    if (raw[n - 1] == 0) --n;
    raw[n] = 0;
    out[0] = 0;
    canonical_path(raw, out, cap);
    return BSA_OK;
}

/* The base name of entry `i` lives in the file-name block.  Checkpoints every
 * BSA_NAME_GROUP names bound the forward scan to 15 NULs. */
static BsaStatus read_base_name(const BsaArchive *archive, uint32_t i,
                                char *out, size_t cap, char *reason,
                                size_t reason_cap) {
    if (!archive->name_block_len || !archive->name_checkpoints) {
        set_reason(reason, reason_cap, "archive has no file-name block");
        return BSA_ERR_FORMAT;
    }
    size_t group = i / BSA_NAME_GROUP;
    if (group >= archive->name_checkpoint_count) {
        set_reason(reason, reason_cap, "record %u has no name checkpoint", i);
        return BSA_ERR_FORMAT;
    }
    uint64_t pos = archive->name_block_at + archive->name_checkpoints[group];
    uint64_t end = archive->name_block_at + archive->name_block_len;
    unsigned skip = i % BSA_NAME_GROUP;
    for (;;) {
        size_t o = 0;
        for (;;) {
            if (pos >= end) {
                set_reason(reason, reason_cap,
                           "file-name block ends inside record %u", i);
                return BSA_ERR_FORMAT;
            }
            uint8_t c;
            BsaStatus st = cache_read(archive, pos++, &c, 1, reason, reason_cap);
            if (st != BSA_OK) return st;
            if (c == 0) break;
            if (o + 1 >= cap) {
                set_reason(reason, reason_cap,
                           "base name of record %u is too long", i);
                return BSA_ERR_NAME;
            }
            out[o++] = (char)c;
        }
        out[o] = 0;
        if (!skip) {
            if (!o) {
                set_reason(reason, reason_cap,
                           "record %u has an empty base name", i);
                return BSA_ERR_NAME;
            }
            return BSA_OK;
        }
        --skip;
    }
}

/* Exact, case-folded name of one entry, materialized on demand from the
 * archive (folder name + base name, or the embedded prefix). */
static BsaStatus entry_name(const BsaArchive *archive, uint32_t i, char *out,
                            size_t cap, char *reason, size_t reason_cap) {
    if (i >= archive->header.file_count) {
        set_reason(reason, reason_cap, "record %u is outside the archive", i);
        return BSA_ERR_ARGUMENT;
    }
    if (archive->name_block_len) {
        const BsaFolderInternal *f = folder_for(archive, i);
        if (!f) {
            set_reason(reason, reason_cap, "record %u has no folder", i);
            return BSA_ERR_FORMAT;
        }
        char fcan[BSA_PATH_MAX];
        char base[BSA_PATH_MAX];
        BsaStatus st = folder_name_canon(archive, f, fcan, sizeof fcan,
                                         reason, reason_cap);
        if (st != BSA_OK) return st;
        st = read_base_name(archive, i, base, sizeof base, reason, reason_cap);
        if (st != BSA_OK) return st;
        size_t fl = strlen(fcan), bl = strlen(base);
        if (!bl) {
            set_reason(reason, reason_cap, "record %u has an empty path", i);
            return BSA_ERR_NAME;
        }
        if (fl + (fl ? 1u : 0u) + bl >= cap) {
            set_reason(reason, reason_cap, "path for record %u is too long", i);
            return BSA_ERR_NAME;
        }
        if (fl) {
            memcpy(out, fcan, fl);
            out[fl] = '\\';
            memcpy(out + fl + 1u, base, bl + 1u);
        } else {
            memcpy(out, base, bl + 1u);
        }
        return BSA_OK;
    }

    uint64_t hash;
    uint32_t stored = 0, offset = 0;
    BsaStatus st = read_record(archive, i, &hash, &stored, &offset, NULL,
                               reason, reason_cap);
    (void)hash;
    if (st != BSA_OK) return st;
    if (!stored) {
        set_reason(reason, reason_cap, "entry %u has no embedded name prefix", i);
        return BSA_ERR_FORMAT;
    }
    uint8_t head[1];
    st = cache_read(archive, offset, head, 1, reason, reason_cap);
    if (st != BSA_OK) return st;
    uint32_t n = head[0];
    if (!n || n >= BSA_PATH_MAX || (uint64_t)1u + n > stored) {
        set_reason(reason, reason_cap,
                   "record %u has invalid embedded name length %u", i, n);
        return BSA_ERR_NAME;
    }
    char raw[BSA_PATH_MAX];
    st = cache_read(archive, (uint64_t)offset + 1u, raw, n, reason, reason_cap);
    if (st != BSA_OK) return st;
    if (raw[n - 1] == 0) --n;
    raw[n] = 0;
    if (!canonical_path(raw, out, cap)) {
        set_reason(reason, reason_cap, "record %u embedded name is invalid", i);
        return BSA_ERR_NAME;
    }
    return BSA_OK;
}

static uint8_t path_tag(uint64_t hash) {
    return (uint8_t)(hash >> 40);
}

/* Open addressing keyed by path_hash64().  Each slot keeps an 8-bit tag next
 * to the u32 record index.  Only a matching tag can be the same path, so a
 * lookup that misses reads no archive bytes at all; a hit still reads the
 * record and exact name for the required verification. */
static BsaStatus index_insert_path(BsaArchive *a, uint32_t entry_index,
                                   uint64_t hash, const char *path,
                                   char *reason, size_t reason_cap) {
    uint8_t tag = path_tag(hash);
    uint32_t slot = (uint32_t)hash & (uint32_t)(a->table_size - 1u);
    for (;;) {
        uint32_t existing = a->table[slot];
        if (existing == 0xffffffffu) {
            a->table[slot] = entry_index;
            a->tags[slot] = tag;
            return BSA_OK;
        }
        if (a->tags[slot] == tag) {
            char cand[BSA_PATH_MAX];
            BsaStatus st = entry_name(a, existing, cand, sizeof cand,
                                      reason, reason_cap);
            if (st != BSA_OK) return st;
            if (path_equal(cand, path)) {
                set_reason(reason, reason_cap, "duplicate archive path '%s'",
                           path);
                return BSA_ERR_DUPLICATE;
            }
        }
        slot = (slot + 1u) & (uint32_t)(a->table_size - 1u);
    }
}

static BsaStatus entry_prefix(const BsaArchive *a, const BsaEntry *entry,
                              uint32_t *payload_off, uint32_t *payload_len,
                              uint32_t *unpacked, char *reason,
                              size_t reason_cap) {
    if (!entry || entry->record_index >= a->header.file_count) {
        set_reason(reason, reason_cap, "entry does not belong to this archive");
        return BSA_ERR_ARGUMENT;
    }
    /* Re-read the 16-byte record from the archive; the compact index no longer
     * mirrors offsets/sizes, so this also detects a changed record. */
    uint64_t rec_hash;
    uint32_t rec_stored = 0, rec_offset = 0;
    BsaStatus st = read_record(a, entry->record_index, &rec_hash, &rec_stored,
                               &rec_offset, NULL, reason, reason_cap);
    (void)rec_hash;
    if (st != BSA_OK) return st;
    if (rec_offset != entry->offset || rec_stored != entry->stored_size) {
        set_reason(reason, reason_cap, "entry record was modified");
        return BSA_ERR_ARGUMENT;
    }
    uint64_t off = entry->offset;
    uint32_t len = entry->stored_size;
    if (a->header.embedded_names) {
        if (!len) {
            set_reason(reason, reason_cap, "embedded-name entry has no prefix");
            return BSA_ERR_FORMAT;
        }
        uint8_t head[1];
        st = cache_read(a, off, head, 1, reason, reason_cap);
        if (st != BSA_OK) return st;
        uint32_t name_len = head[0];
        if (!name_len || name_len >= BSA_PATH_MAX ||
            (uint64_t)1u + name_len > len) {
            set_reason(reason, reason_cap,
                       "bad embedded name length %u (stored %u)", name_len, len);
            return BSA_ERR_NAME;
        }
        if (a->opt.verify_embedded_names) {
            char raw[BSA_PATH_MAX];
            st = cache_read(a, off + 1u, raw, name_len, reason, reason_cap);
            if (st != BSA_OK) return st;
            uint32_t visible = raw[name_len - 1] == 0 ? name_len - 1 : name_len;
            raw[visible] = 0;
            char canon[BSA_PATH_MAX];
            char expect[BSA_PATH_MAX];
            if (!canonical_path(raw, canon, sizeof canon) ||
                entry_name(a, entry->record_index, expect, sizeof expect,
                           reason, reason_cap) != BSA_OK ||
                !path_equal(canon, expect)) {
                set_reason(reason, reason_cap,
                           "embedded name does not match archive path");
                return BSA_ERR_NAME;
            }
        }
        off += 1u + name_len;
        len -= 1u + name_len;
    }
    if (a->header.compressed) {
        if (len < 4) {
            set_reason(reason, reason_cap, "compressed entry lacks the unpacked size");
            return BSA_ERR_FORMAT;
        }
        uint8_t raw[4];
        BsaStatus st = read_exact(a, off, raw, 4, reason, reason_cap);
        if (st != BSA_OK) return st;
        uint32_t size = get_u32(raw);
        if (size > a->opt.max_unpacked_bytes) {
            set_reason(reason, reason_cap,
                       "unpacked size %u exceeds limit %u",
                       size, a->opt.max_unpacked_bytes);
            return BSA_ERR_BUDGET;
        }
        off += 4;
        len -= 4;
        *unpacked = size;
    } else {
        *unpacked = len;
    }
    if (off > UINT32_MAX) {
        set_reason(reason, reason_cap,
                   "payload offset %llu exceeds the 32-bit archive offset field",
                   (unsigned long long)off);
        return BSA_ERR_BOUNDS;
    }
    *payload_off = (uint32_t)off;
    *payload_len = len;
    if (*unpacked > a->opt.max_unpacked_bytes) {
        set_reason(reason, reason_cap,
                   "unpacked size %u exceeds limit %u",
                   *unpacked, a->opt.max_unpacked_bytes);
        return BSA_ERR_BUDGET;
    }
    return BSA_OK;
}

/* ------------------------------------------------------------------ */
/* Open / parse.                                                       */
/* ------------------------------------------------------------------ */

static BsaStatus parse_directory(BsaArchive *a, char *reason, size_t reason_cap) {
    BsaHeader *h = &a->header;

    uint64_t cursor = h->data_start; /* set to the header offset by archive_parse */
    uint64_t folder_records_at = cursor;
    if ((uint64_t)h->folder_count * 16u > a->size - cursor) {
        set_reason(reason, reason_cap, "folder records run past EOF");
        return BSA_ERR_BOUNDS;
    }
    cursor += (uint64_t)h->folder_count * 16u;

    /* Only the fixed locator array is resident; folder/file names stay in the
     * archive and are read through the page cache when a lookup needs them. */
    a->folders = (BsaFolderInternal *)index_alloc(
        a, (size_t)h->folder_count * sizeof(BsaFolderInternal));
    if (h->folder_count && !a->folders) return alloc_failure(a);

    uint32_t folder_name_bytes = 0;
    uint32_t entry_index = 0;
    for (uint32_t i = 0; i < h->folder_count; ++i) {
        uint8_t name_len_byte;
        BsaStatus st = cache_read(a, cursor, &name_len_byte, 1, reason,
                                  reason_cap);
        if (st != BSA_OK) return st;
        ++cursor;
        uint32_t name_len = name_len_byte;
        if (!name_len || name_len >= BSA_PATH_MAX) {
            set_reason(reason, reason_cap,
                       "folder %u has invalid name length %u", i, name_len);
            return BSA_ERR_NAME;
        }
        if (folder_name_bytes + name_len > h->total_folder_name_length) {
            set_reason(reason, reason_cap,
                       "folder %u name runs past the declared name block", i);
            return BSA_ERR_FORMAT;
        }
        if (cursor > UINT32_MAX || (uint64_t)name_len > a->size - cursor) {
            set_reason(reason, reason_cap, "folder %u name runs past EOF", i);
            return BSA_ERR_BOUNDS;
        }
        char raw[BSA_PATH_MAX];
        st = cache_read(a, cursor, raw, name_len, reason, reason_cap);
        if (st != BSA_OK) return st;
        BsaFolderInternal *folder = &a->folders[i];
        folder->name_at = (uint32_t)cursor;
        folder->name_stored = (uint16_t)name_len;
        cursor += name_len;
        folder_name_bytes += name_len;
        uint32_t visible = name_len;
        if (raw[visible - 1] == 0) --visible;
        raw[visible] = 0;
        char canon[BSA_PATH_MAX];
        canon[0] = 0;
        size_t canon_len = canonical_path(raw, canon, sizeof canon);
        folder->name_length = (uint16_t)canon_len;

        uint8_t rec[16];
        st = cache_read(a, folder_records_at + (uint64_t)i * 16u, rec,
                        sizeof rec, reason, reason_cap);
        if (st != BSA_OK) return st;
        uint32_t count = get_u32(rec + 8);
        if (count > h->file_count - entry_index) {
            set_reason(reason, reason_cap,
                       "folder %u count %u exceeds the %u remaining files",
                       i, count, h->file_count - entry_index);
            return BSA_ERR_COUNT;
        }
        if ((uint64_t)count * 16u > a->size - cursor) {
            set_reason(reason, reason_cap, "folder %u records run past EOF", i);
            return BSA_ERR_BOUNDS;
        }
        if (cursor > UINT32_MAX) {
            set_reason(reason, reason_cap,
                       "folder %u record offset exceeds 32 bits", i);
            return BSA_ERR_BOUNDS;
        }
        folder->records_at = (uint32_t)cursor;
        folder->first_entry = entry_index;
        folder->count = count;
        cursor += (uint64_t)count * 16u;
        entry_index += count;
    }
    if (folder_name_bytes != h->total_folder_name_length) {
        set_reason(reason, reason_cap,
                   "folder names consume %u bytes, header says %u",
                   folder_name_bytes, h->total_folder_name_length);
        return BSA_ERR_FORMAT;
    }
    if (entry_index != h->file_count) {
        set_reason(reason, reason_cap,
                   "folder records describe %u files, header says %u",
                   entry_index, h->file_count);
        return BSA_ERR_COUNT;
    }

    if (h->total_file_name_length) {
        if (h->total_file_name_length > a->opt.max_name_bytes) {
            set_reason(reason, reason_cap, "file-name block exceeds the name budget");
            return BSA_ERR_BUDGET;
        }
        if ((uint64_t)h->total_file_name_length > a->size - cursor ||
            cursor > UINT32_MAX) {
            set_reason(reason, reason_cap, "file-name block runs past EOF");
            return BSA_ERR_BOUNDS;
        }
        a->name_block_at = cursor;
        a->name_block_len = h->total_file_name_length;
        a->name_checkpoint_count =
            (size_t)(h->file_count + BSA_NAME_GROUP - 1u) / BSA_NAME_GROUP + 1u;
        a->name_checkpoints = (uint32_t *)index_alloc(
            a, a->name_checkpoint_count * sizeof(uint32_t));
        if (!a->name_checkpoints) return alloc_failure(a);
        memset(a->name_checkpoints, 0,
               a->name_checkpoint_count * sizeof(uint32_t));
        cursor += h->total_file_name_length;
    } else if (h->file_count && !h->embedded_names) {
        set_reason(reason, reason_cap,
                   "archive has neither a file-name block nor embedded names");
        return BSA_ERR_FORMAT;
    }

    h->data_start = cursor;

    /* Validate every record against the data area by streaming each folder's
     * contiguous record block; nothing per-entry is kept. */
    for (uint32_t fi = 0; fi < h->folder_count; ++fi) {
        BsaFolderInternal *f = &a->folders[fi];
        for (uint32_t j = 0; j < f->count; ++j) {
            uint8_t rec[16];
            BsaStatus st = cache_read(a, (uint64_t)f->records_at + (uint64_t)j * 16u,
                                      rec, sizeof rec, reason, reason_cap);
            if (st != BSA_OK) return st;
            uint32_t stored = get_u32(rec + 8);
            uint32_t off = get_u32(rec + 12);
            uint32_t idx = f->first_entry + j;
            if (off < h->data_start || off > a->size ||
                (uint64_t)stored > a->size - off) {
                set_reason(reason, reason_cap,
                           "record %u data %u+%u is outside the data area %llu..%llu",
                           idx, off, stored,
                           (unsigned long long)h->data_start,
                           (unsigned long long)a->size);
                return BSA_ERR_BOUNDS;
            }
        }
    }

    size_t want = 16;
    while ((uint64_t)want * 4u < (uint64_t)h->file_count * 5u) {
        if (want > (1u << 30)) return BSA_ERR_COUNT;
        want <<= 1;
    }
    a->table = (uint32_t *)index_alloc(a, want * sizeof(uint32_t));
    if (!a->table) return alloc_failure(a);
    a->tags = (uint8_t *)index_alloc(a, want * sizeof(uint8_t));
    if (!a->tags) return alloc_failure(a);
    a->table_size = want;
    memset(a->table, 0xff, want * sizeof(uint32_t));
    memset(a->tags, 0, want * sizeof(uint8_t));
    BsaStatus st = BSA_OK;

    if (a->name_block_len) {
        uint64_t pos = a->name_block_at;
        uint64_t end = pos + a->name_block_len;
        const BsaFolderInternal *cur_folder = NULL;
        char fcan[BSA_PATH_MAX];
        size_t fcan_len = 0;
        uint64_t total = 0;
        char path[BSA_PATH_MAX];
        for (uint32_t i = 0; i < h->file_count; ++i) {
            if ((i % BSA_NAME_GROUP) == 0)
                a->name_checkpoints[i / BSA_NAME_GROUP] =
                    (uint32_t)(pos - a->name_block_at);
            char base[BSA_PATH_MAX];
            size_t o = 0;
            int done = 0;
            while (!done) {
                if (pos >= end) {
                    set_reason(reason, reason_cap,
                               "file-name block ends before record %u", i);
                    st = BSA_ERR_FORMAT;
                    break;
                }
                uint8_t c;
                st = cache_read(a, pos++, &c, 1, reason, reason_cap);
                if (st != BSA_OK) break;
                if (c == 0) {
                    done = 1;
                } else if (o + 1 >= sizeof base) {
                    set_reason(reason, reason_cap,
                               "file record %u has an invalid base name length",
                               i);
                    st = BSA_ERR_NAME;
                    break;
                } else {
                    base[o++] = (char)c;
                }
            }
            if (st != BSA_OK) break;
            base[o] = 0;
            if (!o) {
                set_reason(reason, reason_cap,
                           "file record %u has an empty base name", i);
                st = BSA_ERR_NAME;
                break;
            }
            if (name_has_separator(base, o)) {
                set_reason(reason, reason_cap,
                           "file record %u base name contains a separator", i);
                st = BSA_ERR_NAME;
                break;
            }
            const BsaFolderInternal *f = folder_for(a, i);
            if (!f) {
                set_reason(reason, reason_cap, "record %u has no folder", i);
                st = BSA_ERR_FORMAT;
                break;
            }
            if (f != cur_folder) {
                st = folder_name_canon(a, f, fcan, sizeof fcan, reason,
                                       reason_cap);
                if (st != BSA_OK) break;
                fcan_len = strlen(fcan);
                cur_folder = f;
            }
            uint64_t path_len = (uint64_t)fcan_len + (fcan_len ? 1u : 0u) + o;
            if (path_len >= BSA_PATH_MAX) {
                set_reason(reason, reason_cap, "path for record %u is too long", i);
                st = BSA_ERR_NAME;
                break;
            }
            total += path_len + 1u;
            if (total > a->opt.max_name_bytes) {
                set_reason(reason, reason_cap,
                           "archive names need %llu bytes (limit %u)",
                           (unsigned long long)total, a->opt.max_name_bytes);
                st = BSA_ERR_BUDGET;
                break;
            }
            if (fcan_len) {
                memcpy(path, fcan, fcan_len);
                path[fcan_len] = '\\';
                memcpy(path + fcan_len + 1u, base, o + 1u);
            } else {
                memcpy(path, base, o + 1u);
            }
            uint64_t hash = path_hash64(path);
            st = index_insert_path(a, i, hash, path, reason, reason_cap);
            if (st != BSA_OK) break;
        }
        if (st == BSA_OK && pos != end) {
            set_reason(reason, reason_cap,
                       "file-name block has %llu trailing bytes",
                       (unsigned long long)(end - pos));
            st = BSA_ERR_FORMAT;
        }
    } else if (h->file_count) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < h->file_count; ++i) {
            char path[BSA_PATH_MAX];
            st = entry_name(a, i, path, sizeof path, reason, reason_cap);
            if (st != BSA_OK) break;
            size_t len = strlen(path);
            total += (uint64_t)len + 1u;
            if (total > a->opt.max_name_bytes) {
                set_reason(reason, reason_cap,
                           "embedded names exceed the name budget");
                st = BSA_ERR_BUDGET;
                break;
            }
            uint64_t hash = path_hash64(path);
            st = index_insert_path(a, i, hash, path, reason, reason_cap);
            if (st != BSA_OK) break;
        }
    }

    return st;
}

static BsaStatus archive_parse(BsaArchive *a, char *reason, size_t reason_cap) {
    uint8_t raw[36];
    BsaStatus st = read_exact(a, 0, raw, sizeof raw, reason, reason_cap);
    if (st != BSA_OK) return st;
    if (memcmp(raw, "BSA\0", 4) != 0) {
        set_reason(reason, reason_cap, "not a BSA (magic %.4s)", (const char *)raw);
        return BSA_ERR_MAGIC;
    }
    a->header.version = get_u32(raw + 4);
    uint32_t header_offset = get_u32(raw + 8);
    a->header.archive_flags = get_u32(raw + 12);
    a->header.folder_count = get_u32(raw + 16);
    a->header.file_count = get_u32(raw + 20);
    a->header.total_folder_name_length = get_u32(raw + 24);
    a->header.total_file_name_length = get_u32(raw + 28);
    a->header.file_flags = get_u32(raw + 32);
    if (a->header.version != 103 && a->header.version != 104 &&
        a->header.version != 105) {
        set_reason(reason, reason_cap,
                   "unsupported BSA version %u (supported 103, 104, 105)",
                   a->header.version);
        return BSA_ERR_VERSION;
    }
    {
        /* A v103 header is documented as 34 bytes, but real Oblivion archives
         * (and the Wrye Bash template) store a u32 file-flags field at 32 and
         * a 36-byte header offset, exactly like v104. */
        uint32_t min_offset = a->header.version == 103 ? 34u : 36u;
        if (header_offset < min_offset || (uint64_t)header_offset > a->size) {
            set_reason(reason, reason_cap, "bad header offset %u", header_offset);
            return BSA_ERR_BOUNDS;
        }
    }
    if (a->header.folder_count > a->opt.max_folders ||
        a->header.file_count > a->opt.max_files) {
        set_reason(reason, reason_cap, "implausible counts %u folders / %u files",
                   a->header.folder_count, a->header.file_count);
        return BSA_ERR_COUNT;
    }
    if (a->header.archive_flags & 0x040u) {
        set_reason(reason, reason_cap, "Xbox 360 archives are unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    if (a->header.version >= 104 && (a->header.archive_flags & 0x200u)) {
        set_reason(reason, reason_cap, "XMem codec archives are unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    a->header.compressed = (a->header.archive_flags & 0x004u) != 0;
    /* v103 writers set 0x100 as part of the retain-flags group and store no
     * embedded name prefix; only v104+ uses it for embedding. */
    a->header.embedded_names =
        a->header.version >= 104 && (a->header.archive_flags & 0x100u) != 0;

    if (a->header.total_folder_name_length > a->opt.max_name_bytes) {
        set_reason(reason, reason_cap, "folder-name block exceeds the name budget");
        return BSA_ERR_BUDGET;
    }

    a->folders = (BsaFolderInternal *)index_alloc(
        a, (size_t)a->header.folder_count * sizeof(BsaFolderInternal));
    if (a->header.folder_count && !a->folders) return alloc_failure(a);

    /* Page cache is deliberately outside max_index_bytes: it is a bounded
     * (BSA_PAGE_CACHE_BYTES), non-resident-index cost.  If malloc fails the
     * reader still works, just with one read per access. */
    a->page_cap = a->size < BSA_PAGE_CACHE_BYTES
                      ? (size_t)a->size
                      : BSA_PAGE_CACHE_BYTES;
    a->page = (uint8_t *)malloc(a->page_cap);
    a->page_off = 0;
    a->page_len = 0;

    a->header.data_start = header_offset;
    st = parse_directory(a, reason, reason_cap);
    if (st != BSA_OK) return st;
    return BSA_OK;
}

BsaStatus bsa_open_reader(BsaReadAt read_at, void *ctx, uint64_t size,
                          const BsaOptions *options, BsaArchive **out,
                          char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!read_at || !out || !size) {
        set_reason(reason, reason_cap,
                   "open needs a reader, a non-zero size and an output pointer");
        return BSA_ERR_ARGUMENT;
    }
    *out = NULL;
    BsaArchive *a = (BsaArchive *)calloc(1, sizeof *a);
    if (!a) return BSA_ERR_IO;
    a->read_at = read_at;
    a->ctx = ctx;
    a->size = size;
    bsa_options_default(&a->opt);
    if (options) {
        if (options->max_folders) a->opt.max_folders = options->max_folders;
        if (options->max_files) a->opt.max_files = options->max_files;
        if (options->max_name_bytes) a->opt.max_name_bytes = options->max_name_bytes;
        if (options->max_index_bytes) a->opt.max_index_bytes = options->max_index_bytes;
        if (options->max_stored_bytes) a->opt.max_stored_bytes = options->max_stored_bytes;
        if (options->max_unpacked_bytes) a->opt.max_unpacked_bytes = options->max_unpacked_bytes;
        a->opt.verify_embedded_names = options->verify_embedded_names;
    }
    BsaStatus st = archive_parse(a, reason, reason_cap);
    if (st != BSA_OK) {
        bsa_close(a);
        return st;
    }
    *out = a;
    return BSA_OK;
}

typedef struct { const uint8_t *data; size_t size; } MemoryContext;
typedef struct { FILE *file; } FileContext;

static int memory_read_at(void *ctx, uint64_t offset, void *dst, size_t bytes) {
    MemoryContext *m = (MemoryContext *)ctx;
    if (!m || offset > m->size || bytes > m->size - (size_t)offset) return -1;
    memcpy(dst, m->data + (size_t)offset, bytes);
    return 0;
}

static int file_read_at(void *ctx, uint64_t offset, void *dst, size_t bytes) {
    FileContext *f = (FileContext *)ctx;
    if (!f || !f->file) return -1;
    if (BSA_FSEEK(f->file, (int64_t)offset, SEEK_SET) != 0) return -1;
    return fread(dst, 1, bytes, f->file) == bytes ? 0 : -1;
}

BsaStatus bsa_open_memory(const void *data, size_t size,
                          const BsaOptions *options, BsaArchive **out,
                          char *reason, size_t reason_cap) {
    if (!data || !size || size > UINT32_MAX) {
        set_reason(reason, reason_cap,
                   "memory archive needs a non-empty 32-bit-sized buffer");
        return BSA_ERR_ARGUMENT;
    }
    MemoryContext *ctx = (MemoryContext *)calloc(1, sizeof *ctx);
    if (!ctx) return BSA_ERR_IO;
    ctx->data = (const uint8_t *)data;
    ctx->size = size;
    BsaStatus st = bsa_open_reader(memory_read_at, ctx, size, options, out,
                                   reason, reason_cap);
    if (st != BSA_OK) { free(ctx); return st; }
    (*out)->owned_ctx = ctx;
    return BSA_OK;
}

BsaStatus bsa_open_path(const char *path, const BsaOptions *options,
                        BsaArchive **out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!path || !out) {
        set_reason(reason, reason_cap, "open needs a path and an output pointer");
        return BSA_ERR_ARGUMENT;
    }
    *out = NULL;
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (!file) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR) {
            set_reason(reason, reason_cap, "'%s' does not exist", path);
            return BSA_ERR_NOT_FOUND;
        }
        set_reason(reason, reason_cap, "cannot open '%s': %s (errno %d)",
                   path, strerror(e), e);
        return BSA_ERR_ACCESS;
    }
    if (BSA_FSEEK(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_reason(reason, reason_cap, "cannot seek '%s'", path);
        return BSA_ERR_IO;
    }
    int64_t end = BSA_FTELL(file);
    if (end <= 0) {
        fclose(file);
        set_reason(reason, reason_cap, "'%s' is empty", path);
        return BSA_ERR_TRUNCATED;
    }
    FileContext *ctx = (FileContext *)calloc(1, sizeof *ctx);
    if (!ctx) {
        fclose(file);
        set_reason(reason, reason_cap, "out of memory opening '%s'", path);
        return BSA_ERR_IO;
    }
    ctx->file = file;
    BsaStatus st = bsa_open_reader(file_read_at, ctx, (uint64_t)end, options,
                                   out, reason, reason_cap);
    if (st != BSA_OK) { free(ctx); fclose(file); return st; }
    (*out)->owned_ctx = ctx;
    (*out)->file = file;
    return BSA_OK;
}

void bsa_close(BsaArchive *archive) {
    if (!archive) return;
    if (archive->file) fclose(archive->file);
    free(archive->owned_ctx);
    free(archive->folders);
    free(archive->table);
    free(archive->tags);
    free(archive->name_checkpoints);
    free(archive->page);
    free(archive);
}

const BsaHeader *bsa_header(const BsaArchive *archive) {
    return archive ? &archive->header : NULL;
}

uint32_t bsa_file_count(const BsaArchive *archive) {
    return archive ? archive->header.file_count : 0;
}

size_t bsa_index_bytes(const BsaArchive *archive) {
    return archive ? archive->index_bytes : 0;
}

BsaStatus bsa_find(const BsaArchive *archive, const char *path, BsaMatch *out) {
    if (!archive || !path || !out || !archive->table) return BSA_ERR_ARGUMENT;
    char canon[BSA_PATH_MAX];
    size_t len = canonical_path(path, canon, sizeof canon);
    if (!len) return BSA_ERR_NOT_FOUND;
    uint64_t hash = path_hash64(canon);
    uint8_t tag = path_tag(hash);
    uint32_t slot = (uint32_t)hash & (uint32_t)(archive->table_size - 1u);
    char reason[BSA_REASON_MAX];
    reason[0] = 0;
    for (;;) {
        uint32_t index = archive->table[slot];
        if (index == 0xffffffffu) return BSA_ERR_NOT_FOUND;
        if (archive->tags[slot] != tag) {
            slot = (slot + 1u) & (uint32_t)(archive->table_size - 1u);
            continue;
        }
        char cand[BSA_PATH_MAX];
        BsaStatus st = entry_name(archive, index, cand, sizeof cand, reason,
                                  sizeof reason);
        if (st != BSA_OK) return st;
        if (path_equal(canon, cand)) {
            uint64_t rec_hash = 0;
            uint32_t stored = 0, offset = 0, folder_index = 0;
            st = read_record(archive, index, &rec_hash, &stored, &offset,
                             &folder_index, reason, sizeof reason);
            if (st != BSA_OK) return st;
            memset(out, 0, sizeof *out);
            out->entry.hash = rec_hash;
            out->entry.stored_size = stored;
            out->entry.offset = offset;
            out->entry.folder_index = folder_index;
            out->entry.record_index = index;
            memcpy(out->path, cand, strlen(cand) + 1u);
            return BSA_OK;
        }
        slot = (slot + 1u) & (uint32_t)(archive->table_size - 1u);
    }
}

BsaStatus bsa_unpacked_size(const BsaArchive *archive, const BsaEntry *entry,
                            uint32_t *out, char *reason, size_t reason_cap) {
    if (!archive || !entry || !out) {
        set_reason(reason, reason_cap,
                   "unpacked-size query needs an archive, entry and output");
        return BSA_ERR_ARGUMENT;
    }
    uint32_t off, len, unpacked;
    BsaStatus st = entry_prefix(archive, entry, &off, &len, &unpacked,
                                reason, reason_cap);
    (void)off; (void)len;
    if (st != BSA_OK) return st;
    *out = unpacked;
    return BSA_OK;
}

static BsaStatus read_payload(const BsaArchive *archive, const BsaEntry *entry,
                              uint8_t **out, uint32_t *stored_out,
                              uint32_t *unpacked_out, char *reason,
                              size_t reason_cap) {
    uint32_t off, len, unpacked;
    BsaStatus st = entry_prefix(archive, entry, &off, &len, &unpacked,
                                reason, reason_cap);
    if (st != BSA_OK) return st;
    if (len > archive->opt.max_stored_bytes) {
        set_reason(reason, reason_cap,
                   "stored payload %u exceeds limit %u",
                   len, archive->opt.max_stored_bytes);
        return BSA_ERR_BUDGET;
    }
    uint8_t *buf = len ? (uint8_t *)malloc(len) : NULL;
    if (len && !buf) return BSA_ERR_IO;
    st = read_exact(archive, off, buf, len, reason, reason_cap);
    if (st != BSA_OK) { free(buf); return st; }
    *out = buf;
    *stored_out = len;
    *unpacked_out = unpacked;
    return BSA_OK;
}

BsaStatus bsa_read_prefix(const BsaArchive *archive, const BsaEntry *entry,
                          uint8_t *dst, size_t cap, size_t *out_len,
                          char *reason, size_t reason_cap) {
    if (!archive || !entry || (!dst && cap) || !out_len) {
        set_reason(reason, reason_cap,
                   "prefix read needs an archive, entry, destination and length");
        return BSA_ERR_ARGUMENT;
    }
    *out_len = 0;
    uint32_t off = 0, len = 0, unpacked = 0;
    BsaStatus st = entry_prefix(archive, entry, &off, &len, &unpacked,
                                reason, reason_cap);
    if (st != BSA_OK) return st;
    if (cap > unpacked) cap = unpacked;
    if (!cap) return BSA_OK;
    if (!archive->header.compressed) {
        if (cap > len) cap = len;
        st = read_exact(archive, off, dst, cap, reason, reason_cap);
        if (st == BSA_OK) *out_len = cap;
        return st;
    }
    if (len > archive->opt.max_stored_bytes) {
        set_reason(reason, reason_cap,
                   "stored payload %u exceeds limit %u",
                   len, archive->opt.max_stored_bytes);
        return BSA_ERR_BUDGET;
    }
    uint8_t *stored = len ? (uint8_t *)malloc(len) : NULL;
    if (len && !stored) return BSA_ERR_IO;
    st = read_exact(archive, off, stored, len, reason, reason_cap);
    if (st == BSA_OK)
        st = bsa_inflate_zlib(stored, len, dst, cap, out_len, 1,
                              reason, reason_cap);
    free(stored);
    return st;
}

BsaStatus bsa_read(const BsaArchive *archive, const BsaEntry *entry,
                   uint8_t *dst, size_t cap, size_t *out_len,
                   char *reason, size_t reason_cap) {
    if (!archive || !entry || (!dst && cap) || !out_len) {
        set_reason(reason, reason_cap,
                   "read needs an archive, entry, destination and length");
        return BSA_ERR_ARGUMENT;
    }
    *out_len = 0;
    uint8_t *stored = NULL;
    uint32_t stored_len = 0, unpacked = 0;
    BsaStatus st = read_payload(archive, entry, &stored, &stored_len,
                                &unpacked, reason, reason_cap);
    if (st != BSA_OK) return st;
    if ((size_t)unpacked > cap) {
        set_reason(reason, reason_cap,
                   "unpacked size %u exceeds the %llu-byte destination",
                   unpacked, (unsigned long long)cap);
        free(stored);
        return BSA_ERR_BUDGET;
    }
    if (!archive->header.compressed) {
        if (unpacked) memcpy(dst, stored, unpacked);
        *out_len = unpacked;
        free(stored);
        return BSA_OK;
    }
    st = bsa_inflate_zlib(stored, stored_len, dst, unpacked, out_len, 0,
                          reason, reason_cap);
    free(stored);
    if (st == BSA_OK && *out_len != unpacked) {
        set_reason(reason, reason_cap,
                   "inflated %llu bytes, header says %u",
                   (unsigned long long)*out_len, unpacked);
        return BSA_ERR_FORMAT;
    }
    return st;
}

BsaStatus bsa_read_range(const BsaArchive *archive, const BsaEntry *entry,
                         uint32_t rel, void *dst, uint32_t n,
                         uint32_t *out_len, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!archive || !entry || (!dst && n) || !out_len) {
        set_reason(reason, reason_cap,
                   "range read needs an archive, entry, destination and length");
        return BSA_ERR_ARGUMENT;
    }
    *out_len = 0;
    uint32_t off = 0, len = 0, unpacked = 0;
    BsaStatus st = entry_prefix(archive, entry, &off, &len, &unpacked,
                                reason, reason_cap);
    if (st != BSA_OK) return st;
    if (archive->header.compressed) {
        set_reason(reason, reason_cap,
                   "range read of a compressed entry is unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    /* Uncompressed: the stored payload and the unpacked record are the same
     * bytes.  Check the relative window before touching the destination. */
    if ((uint64_t)rel > (uint64_t)len || (uint64_t)n > (uint64_t)len - rel) {
        set_reason(reason, reason_cap,
                   "range %u+%u is outside the %u-byte payload",
                   rel, n, len);
        return BSA_ERR_BOUNDS;
    }
    if (!n) return BSA_OK;
    uint64_t physical = (uint64_t)off + (uint64_t)rel;
    st = read_exact(archive, physical, dst, n, reason, reason_cap);
    if (st != BSA_OK) {
        *out_len = 0;
        return st;
    }
    *out_len = n;
    return BSA_OK;
}
