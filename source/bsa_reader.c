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

typedef struct {
    uint64_t hash;
    uint32_t stored_size;
    uint32_t offset;
    uint32_t name_offset;   /* into archive->names */
    uint32_t name_length;
    uint32_t folder_index;
} BsaEntryInternal;

typedef struct {
    uint32_t name_offset;   /* into archive->folder_names */
    uint32_t name_length;
} BsaFolderInternal;

struct BsaArchive {
    BsaReadAt read_at;
    void *ctx;
    void *owned_ctx;
    FILE *file;
    uint64_t size;
    BsaOptions opt;
    BsaHeader header;
    BsaEntryInternal *entries;
    BsaFolderInternal *folders;
    char *folder_names;
    char *names;
    uint32_t *table;
    size_t table_size;
    size_t index_bytes;
    int alloc_budget;
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

/* Temporary parser blocks (folder records, name block) are charged while they
 * are live and released before the next allocation.  Accounting the release is
 * what keeps a tight max_index_bytes usable. */
static void index_free(BsaArchive *a, void *p, size_t bytes) {
    if (!p) return;
    free(p);
    if (a->index_bytes >= bytes) a->index_bytes -= bytes;
    else a->index_bytes = 0;
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

static uint32_t path_hash(const char *path) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static int name_has_separator(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] == '\\' || s[i] == '/') return 1;
    return 0;
}

static BsaStatus index_insert(BsaArchive *a, uint32_t entry_index,
                              char *reason, size_t reason_cap) {
    const char *path = a->names + a->entries[entry_index].name_offset;
    if (!path[0]) {
        set_reason(reason, reason_cap, "empty archive path at record %u", entry_index);
        return BSA_ERR_NAME;
    }
    uint32_t slot = path_hash(path) & (uint32_t)(a->table_size - 1);
    for (;;) {
        uint32_t existing = a->table[slot];
        if (existing == 0xffffffffu) {
            a->table[slot] = entry_index;
            return BSA_OK;
        }
        if (path_equal(a->names + a->entries[existing].name_offset, path)) {
            set_reason(reason, reason_cap, "duplicate archive path '%s'", path);
            return BSA_ERR_DUPLICATE;
        }
        slot = (slot + 1) & (uint32_t)(a->table_size - 1);
    }
}

static BsaStatus build_normal_names(BsaArchive *a, const uint8_t *block,
                                    uint32_t block_len, char *reason,
                                    size_t reason_cap) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < a->header.file_count; ++i) {
        uint32_t begin = pos;
        while (pos < block_len && block[pos]) ++pos;
        if (pos >= block_len) {
            set_reason(reason, reason_cap,
                       "file-name block ends before record %u", i);
            return BSA_ERR_FORMAT;
        }
        uint32_t len = pos - begin;
        if (!len || len >= BSA_PATH_MAX) {
            set_reason(reason, reason_cap,
                       "file record %u has an invalid base name length %u", i, len);
            return BSA_ERR_NAME;
        }
        if (name_has_separator((const char *)block + begin, len)) {
            set_reason(reason, reason_cap,
                       "file record %u base name contains a separator", i);
            return BSA_ERR_NAME;
        }
        BsaEntryInternal *e = &a->entries[i];
        e->name_offset = begin;       /* temporarily relative to block */
        e->name_length = len;
        ++pos;                        /* consume the NUL */
    }
    if (pos != block_len) {
        set_reason(reason, reason_cap,
                   "file-name block has %u trailing bytes", block_len - pos);
        return BSA_ERR_FORMAT;
    }

    uint64_t total = 0;
    for (uint32_t i = 0; i < a->header.file_count; ++i) {
        BsaEntryInternal *e = &a->entries[i];
        BsaFolderInternal *f = &a->folders[e->folder_index];
        uint64_t path_len = (uint64_t)f->name_length + (f->name_length ? 1u : 0u) +
                            e->name_length;
        if (path_len >= BSA_PATH_MAX) {
            set_reason(reason, reason_cap, "path for record %u is too long", i);
            return BSA_ERR_NAME;
        }
        total += path_len + 1;
        if (total > a->opt.max_name_bytes) {
            set_reason(reason, reason_cap,
                       "archive names need %llu bytes (limit %u)",
                       (unsigned long long)total, a->opt.max_name_bytes);
            return BSA_ERR_BUDGET;
        }
    }
    a->names = (char *)index_alloc(a, (size_t)total);
    if (!a->names) return alloc_failure(a);
    uint32_t out = 0;
    for (uint32_t i = 0; i < a->header.file_count; ++i) {
        BsaEntryInternal *e = &a->entries[i];
        BsaFolderInternal *f = &a->folders[e->folder_index];
        uint32_t base_off = e->name_offset;   /* block-relative */
        uint32_t base_len = e->name_length;
        e->name_offset = out;
        if (f->name_length) {
            memcpy(a->names + out, a->folder_names + f->name_offset, f->name_length);
            out += f->name_length;
            a->names[out++] = '\\';
        }
        memcpy(a->names + out, block + base_off, base_len);
        out += base_len;
        e->name_length = out - e->name_offset;
        a->names[out++] = 0;
    }
    return BSA_OK;
}

static BsaStatus entry_prefix(const BsaArchive *a, const BsaEntry *entry,
                              uint32_t *payload_off, uint32_t *payload_len,
                              uint32_t *unpacked, char *reason,
                              size_t reason_cap) {
    if (!entry || entry->record_index >= a->header.file_count) {
        set_reason(reason, reason_cap, "entry does not belong to this archive");
        return BSA_ERR_ARGUMENT;
    }
    const BsaEntryInternal *internal = &a->entries[entry->record_index];
    if (internal->offset != entry->offset ||
        internal->stored_size != entry->stored_size) {
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
        BsaStatus st = read_exact(a, off, head, 1, reason, reason_cap);
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
            st = read_exact(a, off + 1u, raw, name_len, reason, reason_cap);
            if (st != BSA_OK) return st;
            uint32_t visible = raw[name_len - 1] == 0 ? name_len - 1 : name_len;
            raw[visible] = 0;
            char canon[BSA_PATH_MAX];
            if (!canonical_path(raw, canon, sizeof canon) ||
                !path_equal(canon, a->names + internal->name_offset)) {
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
    cursor += (uint64_t)h->folder_count * 16u;
    if (cursor > a->size) {
        set_reason(reason, reason_cap, "folder records run past EOF");
        return BSA_ERR_BOUNDS;
    }

    uint8_t *records = (uint8_t *)index_alloc(a, (size_t)h->folder_count * 16u);
    if (h->folder_count && !records) return alloc_failure(a);
    BsaStatus st = read_exact(a, folder_records_at, records,
                              (size_t)h->folder_count * 16u, reason, reason_cap);
    if (st != BSA_OK) { index_free(a, records, (size_t)h->folder_count * 16u); return st; }

    a->folder_names = (char *)index_alloc(a, (size_t)h->total_folder_name_length + 1u);
    if (h->total_folder_name_length && !a->folder_names) {
        index_free(a, records, (size_t)h->folder_count * 16u);
        return alloc_failure(a);
    }
    uint32_t folder_name_bytes = 0;
    uint32_t entry_index = 0;

    for (uint32_t i = 0; i < h->folder_count; ++i) {
        uint8_t name_len_byte;
        st = read_exact(a, cursor, &name_len_byte, 1, reason, reason_cap);
        if (st != BSA_OK) { index_free(a, records, (size_t)h->folder_count * 16u); return st; }
        ++cursor;
        uint32_t name_len = name_len_byte;
        if (!name_len || name_len >= BSA_PATH_MAX) {
            set_reason(reason, reason_cap,
                       "folder %u has invalid name length %u", i, name_len);
            index_free(a, records, (size_t)h->folder_count * 16u);
            return BSA_ERR_NAME;
        }
        if (folder_name_bytes + name_len > h->total_folder_name_length) {
            set_reason(reason, reason_cap,
                       "folder %u name runs past the declared name block", i);
            index_free(a, records, (size_t)h->folder_count * 16u);
            return BSA_ERR_FORMAT;
        }
        char raw[BSA_PATH_MAX];
        st = read_exact(a, cursor, raw, name_len, reason, reason_cap);
        if (st != BSA_OK) { index_free(a, records, (size_t)h->folder_count * 16u); return st; }
        cursor += name_len;
        folder_name_bytes += name_len;
        if (raw[name_len - 1] == 0) name_len--;
        raw[name_len] = 0;
        char canon[BSA_PATH_MAX];
        size_t canon_len = canonical_path(raw, canon, sizeof canon);
        if (!canon_len) {
            /* An empty folder is tolerated: the path is just the base name. */
            canon_len = 0;
            canon[0] = 0;
        }
        BsaFolderInternal *folder = &a->folders[i];
        folder->name_offset = (uint32_t)strlen(a->folder_names);
        memcpy(a->folder_names + folder->name_offset, canon, canon_len + 1);
        folder->name_length = (uint32_t)canon_len;

        uint32_t count = get_u32(records + (size_t)i * 16u + 8u);
        if (count > h->file_count - entry_index) {
            set_reason(reason, reason_cap,
                       "folder %u count %u exceeds the %u remaining files",
                       i, count, h->file_count - entry_index);
            index_free(a, records, (size_t)h->folder_count * 16u);
            return BSA_ERR_COUNT;
        }
        for (uint32_t j = 0; j < count; ++j) {
            uint8_t rec[16];
            st = read_exact(a, cursor, rec, 16, reason, reason_cap);
            if (st != BSA_OK) { index_free(a, records, (size_t)h->folder_count * 16u); return st; }
            cursor += 16;
            BsaEntryInternal *e = &a->entries[entry_index];
            e->hash = get_u64(rec);
            e->stored_size = get_u32(rec + 8);
            e->offset = get_u32(rec + 12);
            e->folder_index = i;
            ++entry_index;
        }
    }
    index_free(a, records, (size_t)h->folder_count * 16u);
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

    uint8_t *block = NULL;
    if (h->total_file_name_length) {
        if (h->total_file_name_length > a->opt.max_name_bytes) {
            set_reason(reason, reason_cap, "file-name block exceeds the name budget");
            return BSA_ERR_BUDGET;
        }
        block = (uint8_t *)index_alloc(a, h->total_file_name_length);
        if (!block) return alloc_failure(a);
        st = read_exact(a, cursor, block, h->total_file_name_length,
                        reason, reason_cap);
        if (st != BSA_OK) { index_free(a, block, h->total_file_name_length); return st; }
        cursor += h->total_file_name_length;
    } else if (h->file_count && !h->embedded_names) {
        set_reason(reason, reason_cap,
                   "archive has neither a file-name block nor embedded names");
        return BSA_ERR_FORMAT;
    }

    if (block) {
        st = build_normal_names(a, block, h->total_file_name_length,
                                reason, reason_cap);
        index_free(a, block, h->total_file_name_length);
        if (st != BSA_OK) return st;
    } else if (h->file_count) {
        /* Embedded-name-only archive: scan the bounded prefixes.  A path may
         * not exceed BSA_PATH_MAX, so one stack buffer is enough. */
        uint64_t total = 0;
        for (uint32_t i = 0; i < h->file_count; ++i) {
            BsaEntryInternal *e = &a->entries[i];
            uint8_t head[1];
            st = read_exact(a, e->offset, head, 1, reason, reason_cap);
            if (st != BSA_OK) return st;
            uint32_t len = head[0];
            if (!len || len >= BSA_PATH_MAX || 1u + len > e->stored_size) {
                set_reason(reason, reason_cap,
                           "record %u has invalid embedded name length %u", i, len);
                return BSA_ERR_NAME;
            }
            char raw[BSA_PATH_MAX];
            st = read_exact(a, (uint64_t)e->offset + 1u, raw, len,
                            reason, reason_cap);
            if (st != BSA_OK) return st;
            if (raw[len - 1] == 0) --len;
            raw[len] = 0;
            char canon[BSA_PATH_MAX];
            size_t canon_len = canonical_path(raw, canon, sizeof canon);
            if (!canon_len) {
                set_reason(reason, reason_cap,
                           "record %u embedded name is empty", i);
                return BSA_ERR_NAME;
            }
            total += canon_len + 1;
            if (total > a->opt.max_name_bytes) {
                set_reason(reason, reason_cap, "embedded names exceed the name budget");
                return BSA_ERR_BUDGET;
            }
        }
        a->names = (char *)index_alloc(a, (size_t)total);
        if (!a->names) return alloc_failure(a);
        uint32_t out = 0;
        for (uint32_t i = 0; i < h->file_count; ++i) {
            BsaEntryInternal *e = &a->entries[i];
            uint8_t head[1];
            st = read_exact(a, e->offset, head, 1, reason, reason_cap);
            if (st != BSA_OK) return st;
            uint32_t len = head[0];
            if (!len || len >= BSA_PATH_MAX ||
                (uint64_t)1u + len > e->stored_size) {
                set_reason(reason, reason_cap,
                           "record %u embedded name changed to length %u", i, len);
                return BSA_ERR_NAME;
            }
            char raw[BSA_PATH_MAX];
            st = read_exact(a, (uint64_t)e->offset + 1u, raw, len,
                            reason, reason_cap);
            if (st != BSA_OK) return st;
            if (raw[len - 1] == 0) --len;
            raw[len] = 0;
            char canon[BSA_PATH_MAX];
            size_t canon_len = canonical_path(raw, canon, sizeof canon);
            if (!canon_len || (uint64_t)out + canon_len + 1u > total) {
                set_reason(reason, reason_cap,
                           "record %u embedded name changed between reads", i);
                return BSA_ERR_NAME;
            }
            e->name_offset = out;
            memcpy(a->names + out, canon, canon_len);
            out += (uint32_t)canon_len;
            a->names[out++] = 0;
            e->name_length = (uint32_t)canon_len;
        }
    }

    h->data_start = cursor;
    for (uint32_t i = 0; i < h->file_count; ++i) {
        BsaEntryInternal *e = &a->entries[i];
        if (e->offset < h->data_start || e->offset > a->size ||
            (uint64_t)e->stored_size > a->size - e->offset) {
            set_reason(reason, reason_cap,
                       "record %u data %u+%u is outside the data area %llu..%llu",
                       i, e->offset, e->stored_size,
                       (unsigned long long)h->data_start,
                       (unsigned long long)a->size);
            return BSA_ERR_BOUNDS;
        }
    }

    size_t want = 16;
    while (want < (size_t)h->file_count * 2u) {
        if (want > (1u << 30)) return BSA_ERR_COUNT;
        want <<= 1;
    }
    a->table = (uint32_t *)index_alloc(a, want * sizeof(uint32_t));
    if (!a->table) return alloc_failure(a);
    a->table_size = want;
    memset(a->table, 0xff, want * sizeof(uint32_t));
    for (uint32_t i = 0; i < h->file_count; ++i) {
        st = index_insert(a, i, reason, reason_cap);
        if (st != BSA_OK) return st;
    }
    return BSA_OK;
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

    a->entries = (BsaEntryInternal *)index_alloc(
        a, (size_t)a->header.file_count * sizeof(BsaEntryInternal));
    if (a->header.file_count && !a->entries) return alloc_failure(a);
    a->folders = (BsaFolderInternal *)index_alloc(
        a, (size_t)a->header.folder_count * sizeof(BsaFolderInternal));
    if (a->header.folder_count && !a->folders) return alloc_failure(a);

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
    free(archive->entries);
    free(archive->folders);
    free(archive->folder_names);
    free(archive->names);
    free(archive->table);
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
    uint32_t slot = path_hash(canon) & (uint32_t)(archive->table_size - 1);
    for (;;) {
        uint32_t index = archive->table[slot];
        if (index == 0xffffffffu) return BSA_ERR_NOT_FOUND;
        const BsaEntryInternal *e = &archive->entries[index];
        if (path_equal(canon, archive->names + e->name_offset)) {
            memset(out, 0, sizeof *out);
            out->entry.hash = e->hash;
            out->entry.stored_size = e->stored_size;
            out->entry.offset = e->offset;
            out->entry.folder_index = e->folder_index;
            out->entry.record_index = index;
            memcpy(out->path, archive->names + e->name_offset, e->name_length);
            out->path[e->name_length] = 0;
            return BSA_OK;
        }
        slot = (slot + 1) & (uint32_t)(archive->table_size - 1);
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
