/* Host test: bounded BSA reader, StartMenu.swf runtime resolution, SWF header.
 *
 * All archives are synthetic and generated in this file.  With a data
 * directory argument (or SKYRIM_N3DS_REAL_DATA) an additional read-only check
 * reads the installed Interface.bsa/Update.bsa and compares the measured
 * StartMenu.swf size/CRC with docs/MENU_ASSET_CLOSURE.md.
 */

#include "bsa_reader.h"
#include "menu_bsa_runtime.h"
#include "guest.h"
#include "guest_heap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
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

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t adler32_bytes(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* zlib stream using stored deflate blocks (BFINAL/BTYPE=00). */
static size_t zlib_store(uint8_t *dst, size_t cap, const uint8_t *src, size_t n) {
    if (cap < n + 16) return 0;
    size_t at = 0;
    dst[at++] = 0x78;
    dst[at++] = 0x01;   /* (0x7801 % 31) == 0, FDICT=0 */
    size_t pos = 0;
    do {
        size_t chunk = n - pos > 65535 ? 65535 : n - pos;
        int final = pos + chunk == n;
        dst[at++] = (uint8_t)(final ? 1 : 0);
        dst[at++] = (uint8_t)(chunk & 0xff);
        dst[at++] = (uint8_t)(chunk >> 8);
        unsigned nlen = (unsigned)(~chunk) & 0xffffu;
        dst[at++] = (uint8_t)(nlen & 0xff);
        dst[at++] = (uint8_t)(nlen >> 8);
        memcpy(dst + at, src + pos, chunk);
        at += chunk;
        pos += chunk;
    } while (pos < n);
    uint32_t adler = adler32_bytes(src, n);
    dst[at++] = (uint8_t)(adler >> 24);
    dst[at++] = (uint8_t)(adler >> 16);
    dst[at++] = (uint8_t)(adler >> 8);
    dst[at++] = (uint8_t)adler;
    return at;
}

typedef struct {
    uint8_t *dst;
    size_t cap;
    size_t at;
    uint32_t bitbuf;
    unsigned bitcnt;
    int ok;
} BitWriter;

static void bw_bits(BitWriter *w, uint32_t value, unsigned n) {
    if (!w->ok) return;
    w->bitbuf |= value << w->bitcnt;
    w->bitcnt += n;
    while (w->bitcnt >= 8) {
        if (w->at >= w->cap) { w->ok = 0; return; }
        w->dst[w->at++] = (uint8_t)w->bitbuf;
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}

static void bw_code(BitWriter *w, uint32_t code, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        bw_bits(w, (code >> (n - 1 - i)) & 1u, 1);
}

static void bw_align(BitWriter *w) {
    if (w->bitcnt) bw_bits(w, 0, 8 - w->bitcnt);
}

/* zlib stream using a fixed-Huffman literal-only deflate block. */
static size_t zlib_fixed(uint8_t *dst, size_t cap, const uint8_t *src, size_t n) {
    if (cap < n * 2 + 16) return 0;
    size_t at = 0;
    dst[at++] = 0x78;
    dst[at++] = 0x01;
    BitWriter w = {dst + at, cap - at, 0, 0, 0, 1};
    bw_bits(&w, 1, 1);  /* BFINAL */
    bw_bits(&w, 1, 2);  /* BTYPE=01 fixed */
    for (size_t i = 0; i < n; ++i) {
        unsigned lit = src[i];
        if (lit <= 143) bw_code(&w, 0x30u + lit, 8);
        else bw_code(&w, 0x190u + (lit - 144u), 9);
    }
    bw_code(&w, 0, 7);  /* end of block */
    bw_align(&w);
    if (!w.ok) return 0;
    at += w.at;
    uint32_t adler = adler32_bytes(src, n);
    dst[at++] = (uint8_t)(adler >> 24);
    dst[at++] = (uint8_t)(adler >> 16);
    dst[at++] = (uint8_t)(adler >> 8);
    dst[at++] = (uint8_t)adler;
    return at;
}

/* ------------------------------------------------------------------ */
/* Synthetic SWF containers.                                           */
/* ------------------------------------------------------------------ */

static size_t make_cws(uint8_t *out, size_t cap, uint8_t seed, size_t body_len) {
    uint8_t body[512];
    if (body_len > sizeof body) return 0;
    for (size_t i = 0; i < body_len; ++i)
        body[i] = (uint8_t)(seed + i * 13u + (i >> 3));
    memcpy(out, "CWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    size_t z = zlib_store(out + 8, cap - 8, body, body_len);
    return z ? 8 + z : 0;
}

static size_t make_fws(uint8_t *out, size_t cap, uint8_t seed, size_t body_len) {
    if (cap < 8 + body_len) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    for (size_t i = 0; i < body_len; ++i)
        out[8 + i] = (uint8_t)(seed + i * 7u);
    return 8 + body_len;
}

static size_t make_zws(uint8_t *out, size_t cap, size_t body_len) {
    if (cap < 8 + body_len) return 0;
    memcpy(out, "ZWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    memset(out + 8, 0x5a, body_len);
    return 8 + body_len;
}

/* ------------------------------------------------------------------ */
/* Synthetic BSA writer.                                               */
/* ------------------------------------------------------------------ */

#define TEST_MAX_FILES 8
#define TEST_MAX_FOLDERS 8

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t len;
} TestFile;

typedef struct {
    size_t len;
    size_t data_start;
    size_t first_file_record;
} BuiltBsa;

static uint8_t g_comp[TEST_MAX_FILES][8192];
static size_t g_comp_len[TEST_MAX_FILES];

static int build_bsa_ex(uint8_t *out, size_t cap, const TestFile *files,
                        size_t nfiles, uint32_t version, int compressed,
                        int embed, int name_block, int compressor,
                        BuiltBsa *built) {
    if (!out || !files || nfiles > TEST_MAX_FILES || !built) return 0;
    char folder[TEST_MAX_FOLDERS][192];
    size_t first[TEST_MAX_FOLDERS], count[TEST_MAX_FOLDERS];
    size_t nf = 0;
    const char *base[TEST_MAX_FILES];

    for (size_t i = 0; i < nfiles; ++i) {
        const char *slash = strrchr(files[i].path, '\\');
        if (!slash) return 0;
        size_t flen = (size_t)(slash - files[i].path);
        if (!flen || flen >= sizeof folder[0]) return 0;
        base[i] = slash + 1;
        if (!base[i][0]) return 0;
        if (nf == 0 || strlen(folder[nf - 1]) != flen ||
            strncmp(folder[nf - 1], files[i].path, flen) != 0) {
            if (nf == TEST_MAX_FOLDERS) return 0;
            memcpy(folder[nf], files[i].path, flen);
            folder[nf][flen] = 0;
            first[nf] = i;
            count[nf] = 0;
            ++nf;
        }
        ++count[nf - 1];
    }

    size_t stored[TEST_MAX_FILES], offset[TEST_MAX_FILES];
    for (size_t i = 0; i < nfiles; ++i) {
        size_t prefix = embed ? 1u + strlen(files[i].path) : 0;
        if (compressed) {
            g_comp_len[i] = compressor == 1
                                ? zlib_fixed(g_comp[i], sizeof g_comp[i],
                                             files[i].data, files[i].len)
                                : zlib_store(g_comp[i], sizeof g_comp[i],
                                             files[i].data, files[i].len);
            if (!g_comp_len[i]) return 0;
            stored[i] = prefix + 4u + g_comp_len[i];
        } else {
            stored[i] = prefix + files[i].len;
        }
    }

    size_t folder_name_bytes = 0;
    for (size_t f = 0; f < nf; ++f) folder_name_bytes += 1u + strlen(folder[f]) + 1u;
    size_t name_bytes = 0;
    if (name_block)
        for (size_t i = 0; i < nfiles; ++i) name_bytes += strlen(base[i]) + 1u;
    size_t data_start = 36u + 16u * nf + folder_name_bytes + 16u * nfiles +
                        name_bytes;
    size_t at = data_start;
    for (size_t i = 0; i < nfiles; ++i) {
        offset[i] = at;
        at += stored[i];
    }
    if (at > cap) return 0;

    uint32_t flags = 0x001u | 0x002u;
    if (compressed) flags |= 0x004u;
    if (embed) flags |= 0x100u;

    memset(out, 0, data_start);
    memcpy(out, "BSA\0", 4);
    put_u32(out + 4, version);
    put_u32(out + 8, 36);
    put_u32(out + 12, flags);
    put_u32(out + 16, (uint32_t)nf);
    put_u32(out + 20, (uint32_t)nfiles);
    put_u32(out + 24, (uint32_t)(folder_name_bytes - nf));
    put_u32(out + 28, (uint32_t)name_bytes);
    put_u32(out + 32, 0);

    size_t cursor = 36;
    for (size_t f = 0; f < nf; ++f) {
        put_u64(out + cursor, 0);
        put_u32(out + cursor + 8, (uint32_t)count[f]);
        put_u32(out + cursor + 12, 0);
        cursor += 16;
    }
    built->first_file_record = 0;
    size_t file_index = 0;
    for (size_t f = 0; f < nf; ++f) {
        size_t flen = strlen(folder[f]);
        out[cursor++] = (uint8_t)(flen + 1);
        memcpy(out + cursor, folder[f], flen);
        cursor += flen;
        out[cursor++] = 0;
        for (size_t j = 0; j < count[f]; ++j, ++file_index) {
            if (file_index == 0) built->first_file_record = cursor;
            put_u64(out + cursor, (uint64_t)file_index * 0x9e3779b97f4a7c15ull);
            put_u32(out + cursor + 8, (uint32_t)stored[file_index]);
            put_u32(out + cursor + 12, (uint32_t)offset[file_index]);
            cursor += 16;
        }
    }
    if (name_block) {
        for (size_t i = 0; i < nfiles; ++i) {
            size_t len = strlen(base[i]);
            memcpy(out + cursor, base[i], len);
            cursor += len;
            out[cursor++] = 0;
        }
    }
    if (cursor != data_start) return 0;

    for (size_t i = 0; i < nfiles; ++i) {
        if (embed) {
            size_t len = strlen(files[i].path);
            out[cursor++] = (uint8_t)len;
            memcpy(out + cursor, files[i].path, len);
            cursor += len;
        }
        if (compressed) {
            put_u32(out + cursor, (uint32_t)files[i].len);
            cursor += 4;
            memcpy(out + cursor, g_comp[i], g_comp_len[i]);
            cursor += g_comp_len[i];
        } else {
            memcpy(out + cursor, files[i].data, files[i].len);
            cursor += files[i].len;
        }
    }
    built->len = cursor;
    built->data_start = data_start;
    return 1;
}

static int build_bsa(uint8_t *out, size_t cap, const TestFile *files,
                     size_t nfiles, uint32_t version, int compressed,
                     int embed, int name_block, BuiltBsa *built) {
    return build_bsa_ex(out, cap, files, nfiles, version, compressed, embed,
                        name_block, 0, built);
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

static void test_crc_and_inflate(void) {
    check(bsa_crc32("123456789", 9) == 0xCBF43926u, "CRC-32 test vector");

    static const uint8_t payload[] =
        "The quick brown fox jumps over the lazy dog, twice."
        " The quick brown fox jumps over the lazy dog.";
    uint8_t z[4096], fixed[4096], out[4096];
    size_t zlen = zlib_store(z, sizeof z, payload, sizeof payload - 1);
    size_t flen = zlib_fixed(fixed, sizeof fixed, payload, sizeof payload - 1);
    check(zlen > 0 && flen > 0, "synthetic zlib streams build");
    size_t got = 0;
    char reason[BSA_REASON_MAX];
    check(bsa_inflate_zlib(z, zlen, out, sizeof out, &got, 0, reason,
                           sizeof reason) == BSA_OK &&
              got == sizeof payload - 1 && !memcmp(out, payload, got),
          "inflate stored deflate block");
    got = 0;
    check(bsa_inflate_zlib(fixed, flen, out, sizeof out, &got, 0, reason,
                           sizeof reason) == BSA_OK &&
              got == sizeof payload - 1 && !memcmp(out, payload, got),
          "inflate fixed-Huffman deflate block");

    /* 160-byte payload compressed by Python zlib with a dynamic-Huffman block. */
    static const uint8_t dyn_payload[160] = {
        83,9,19,68,55,25,17,85,29,75,0,48,45,27,74,36,24,23,69,24,27,92,15,10,
        20,26,72,55,70,31,78,12,69,36,36,69,80,39,26,56,84,76,86,42,54,72,3,61,
        63,36,25,42,20,74,43,85,7,88,78,43,42,54,58,66,44,18,66,62,32,43,81,31,
        27,8,8,1,76,42,13,47,9,34,19,12,15,26,70,25,80,57,72,84,36,35,34,18,81,
        3,68,30,19,18,70,12,2,89,45,87,64,35,62,30,79,14,81,87,84,32,63,21,83,9,
        13,82,24,8,41,24,51,63,55,59,87,3,47,48,29,56,35,18,39,59,45,26,36,15,
        50,85,51,10,39,66,86,72,71,29,52,11,4,66
    };
    static const uint8_t dyn_zlib[164] = {
        120,156,5,193,139,14,66,80,0,0,208,74,91,136,44,180,60,230,49,238,102,
        145,177,52,148,69,187,67,38,19,121,213,214,255,127,71,231,116,24,147,6,
        252,118,144,158,51,215,22,75,192,237,51,78,252,81,56,43,20,65,174,212,
        68,6,64,214,24,66,216,87,163,233,23,200,45,1,188,201,150,214,176,250,
        212,150,233,95,225,145,134,177,106,181,138,136,162,243,202,36,29,76,99,
        8,74,200,249,230,82,244,64,215,232,22,73,101,134,206,137,197,215,158,
        238,122,44,191,54,237,212,171,201,174,195,200,55,135,30,56,47,9,162,9,
        113,92,41,212,105,35,178,5,64,157,6,15,55,224,88,60,164,243,122,9,255,
        222,151,28,37
    };
    got = 0;
    check(bsa_inflate_zlib(dyn_zlib, sizeof dyn_zlib, out, sizeof out, &got, 0,
                           reason, sizeof reason) == BSA_OK &&
              got == sizeof dyn_payload && !memcmp(out, dyn_payload, got),
          "inflate dynamic-Huffman deflate block");

    uint8_t bad[4096];
    memcpy(bad, z, zlen);
    bad[0] ^= 1;
    check(bsa_inflate_zlib(bad, zlen, out, sizeof out, &got, 0, reason,
                           sizeof reason) != BSA_OK,
          "inflate rejects bad zlib header");
    memcpy(bad, z, zlen);
    bad[zlen - 1] ^= 1;
    check(bsa_inflate_zlib(bad, zlen, out, sizeof out, &got, 0, reason,
                           sizeof reason) != BSA_OK,
          "inflate rejects Adler-32 mismatch");
    memcpy(bad, z, zlen);
    check(bsa_inflate_zlib(bad, zlen - 3, out, sizeof out, &got, 0, reason,
                           sizeof reason) != BSA_OK,
          "inflate rejects truncation");
    got = 0;
    check(bsa_inflate_zlib(z, zlen, out, 16, &got, 1, reason,
                           sizeof reason) == BSA_OK && got == 16,
          "inflate prefix stops at the requested size");
    check(bsa_inflate_zlib(z, zlen, out, 16, &got, 0, reason,
                           sizeof reason) != BSA_OK,
          "full inflate refuses a short destination");
}

static void test_bsa_matrix(void) {
    uint8_t swf[1024], fws[1024], text[128], nif[64], blob[65536], got[4096];
    size_t swf_len = make_cws(swf, sizeof swf, 0x31, 220);
    size_t fws_len = make_fws(fws, sizeof fws, 0x55, 40);
    memset(text, 0x41, sizeof text);
    memset(nif, 0x4e, sizeof nif);
    TestFile files[] = {
        {"interface\\startmenu.swf", swf, swf_len},
        {"interface\\fontconfig.txt", text, sizeof text},
        {"interface\\other_fws.swf", fws, fws_len},
        {"meshes\\b\\x.nif", nif, sizeof nif}
    };
    const size_t nfiles = sizeof files / sizeof files[0];

    for (int version = 104; version <= 105; ++version) {
        for (int compressed = 0; compressed <= 1; ++compressed) {
            for (int embed = 0; embed <= 1; ++embed) {
                BuiltBsa built;
                check(build_bsa(blob, sizeof blob, files, nfiles,
                                (uint32_t)version, compressed, embed, 1,
                                &built) != 0,
                      "matrix BSA builder");
                BsaArchive *a = NULL;
                char reason[BSA_REASON_MAX] = "";
                BsaStatus st = bsa_open_memory(blob, built.len, NULL, &a,
                                               reason, sizeof reason);
                check(st == BSA_OK && a != NULL, "matrix BSA opens");
                if (st != BSA_OK) {
                    printf("  open reason: %s\n", reason);
                    continue;
                }
                const BsaHeader *h = bsa_header(a);
                check(h->version == (uint32_t)version, "matrix version");
                check(h->compressed == compressed, "matrix compressed flag");
                check(h->embedded_names == embed, "matrix embedded flag");
                check(bsa_file_count(a) == nfiles, "matrix file count");

                for (size_t i = 0; i < nfiles; ++i) {
                    BsaMatch m;
                    st = bsa_find(a, files[i].path, &m);
                    check(st == BSA_OK, "matrix find exact path");
                    if (st != BSA_OK) continue;
                    uint32_t unpacked = 0;
                    st = bsa_unpacked_size(a, &m.entry, &unpacked, reason,
                                           sizeof reason);
                    check(st == BSA_OK && unpacked == files[i].len,
                          "matrix unpacked size");
                    size_t n = 0;
                    st = bsa_read(a, &m.entry, got, sizeof got, &n, reason,
                                  sizeof reason);
                    check(st == BSA_OK && n == files[i].len &&
                              !memcmp(got, files[i].data, n),
                          "matrix read payload");
                    uint8_t prefix[4];
                    n = 0;
                    st = bsa_read_prefix(a, &m.entry, prefix, 4, &n, reason,
                                         sizeof reason);
                    check(st == BSA_OK && n == 4 &&
                              !memcmp(prefix, files[i].data, 4),
                          "matrix read prefix");
                }

                BsaMatch m;
                check(bsa_find(a, "INTERFACE/STARTMENU.SWF", &m) == BSA_OK,
                      "matrix case/separator-insensitive lookup");
                bsa_close(a);
            }
        }
    }

    /* Fixed-Huffman payload through the archive path. */
    BuiltBsa built;
    check(build_bsa_ex(blob, sizeof blob, files, nfiles, 104, 1, 0, 1, 1,
                       &built) != 0,
          "fixed-Huffman BSA builds");
    BsaArchive *a = NULL;
    char reason[BSA_REASON_MAX] = "";
    check(bsa_open_memory(blob, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_OK,
          "fixed-Huffman BSA opens");
    if (a) {
        BsaMatch m;
        check(bsa_find(a, files[0].path, &m) == BSA_OK, "fixed BSA find");
        size_t n = 0;
        check(bsa_read(a, &m.entry, got, sizeof got, &n, reason,
                       sizeof reason) == BSA_OK &&
                  n == files[0].len && !memcmp(got, files[0].data, n),
              "fixed-Huffman BSA read");
        bsa_close(a);
    }
}

static void test_bsa_embedded_and_errors(void) {
    uint8_t swf[1024], text[64], nif[64], blob[65536], got[1024];
    size_t swf_len = make_cws(swf, sizeof swf, 0x71, 64);
    memset(text, 0x42, sizeof text);
    memset(nif, 0x43, sizeof nif);
    TestFile files[] = {
        {"interface\\startmenu.swf", swf, swf_len},
        {"interface\\fontconfig.txt", text, sizeof text},
        {"meshes\\b\\x.nif", nif, sizeof nif}
    };
    const size_t nfiles = sizeof files / sizeof files[0];
    BuiltBsa built;
    char reason[BSA_REASON_MAX];
    BsaArchive *a = NULL;

    reason[0] = 0;
    check(build_bsa(blob, sizeof blob, files, nfiles, 104, 1, 1, 0, &built) != 0,
          "embedded-only BSA builds");
    check(bsa_open_memory(blob, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_OK,
          "embedded-only BSA opens without a name block");
    if (a) {
        BsaMatch m;
        check(bsa_find(a, "INTERFACE/STARTMENU.SWF", &m) == BSA_OK,
              "embedded-only lookup");
        size_t n = 0;
        check(bsa_read(a, &m.entry, got, sizeof got, &n, reason,
                       sizeof reason) == BSA_OK &&
                  n == swf_len && !memcmp(got, swf, n),
              "embedded-only read");
        bsa_close(a);
        a = NULL;
    }

    reason[0] = 0;
    check(build_bsa(blob, sizeof blob, files, nfiles, 104, 0, 0, 0, &built) != 0,
          "nameless BSA builds");
    check(bsa_open_memory(blob, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_ERR_FORMAT,
          "nameless non-embedded archive fails explicitly");
    check(strstr(reason, "neither") != NULL, "nameless failure reason is named");

    /* Bad magic. */
    check(build_bsa(blob, sizeof blob, files, nfiles, 104, 0, 0, 1, &built) != 0,
          "error BSA builds");
    uint8_t bad[65536];
    memcpy(bad, blob, built.len);
    bad[0] = 'N';
    check(bsa_open_memory(bad, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_ERR_MAGIC,
          "bad magic refused");
    check(bsa_status_name(BSA_ERR_MAGIC) != NULL, "status names exist");

    /* Unsupported version. */
    BuiltBsa v99;
    check(build_bsa(bad, sizeof bad, files, nfiles, 99, 0, 0, 1, &v99) != 0,
          "version-99 BSA builds");
    check(bsa_open_memory(bad, v99.len, NULL, &a, reason, sizeof reason) ==
              BSA_ERR_VERSION,
          "unsupported version refused");

    /* Truncation. */
    memcpy(bad, blob, built.len);
    check(bsa_open_memory(bad, built.len - 10, NULL, &a, reason, sizeof reason) !=
              BSA_OK,
          "truncated archive refused");

    /* Count budget. */
    BsaOptions opt;
    bsa_options_default(&opt);
    opt.max_files = 1;
    check(bsa_open_memory(blob, built.len, &opt, &a, reason, sizeof reason) ==
              BSA_ERR_COUNT,
          "file-count statement limit enforced");

    /* Index budget. */
    bsa_options_default(&opt);
    opt.max_index_bytes = 64;
    check(bsa_open_memory(blob, built.len, &opt, &a, reason, sizeof reason) ==
              BSA_ERR_BUDGET,
          "index byte budget named BUDGET");

    /* Per-file unpacked budget. */
    bsa_options_default(&opt);
    opt.max_unpacked_bytes = 8;
    check(bsa_open_memory(blob, built.len, &opt, &a, reason, sizeof reason) ==
              BSA_OK,
          "archive opens before a per-file budget is consulted");
    if (a) {
        BsaMatch m;
        check(bsa_find(a, files[0].path, &m) == BSA_OK, "budget fixture find");
        uint32_t unpacked = 0;
        check(bsa_unpacked_size(a, &m.entry, &unpacked, reason,
                                sizeof reason) == BSA_ERR_BUDGET,
              "per-file unpacked budget refused");
        bsa_close(a);
        a = NULL;
    }

    /* Out-of-bounds file record. */
    memcpy(bad, blob, built.len);
    put_u32(bad + built.first_file_record + 12, 0xfffffff0u);
    check(bsa_open_memory(bad, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_ERR_BOUNDS,
          "out-of-bounds file record refused");

    /* Zero folder-name length. */
    memcpy(bad, blob, built.len);
    uint32_t folders = get_u32(bad + 16);
    bad[36 + folders * 16] = 0;
    check(bsa_open_memory(bad, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_ERR_NAME,
          "zero folder-name length refused");

    /* Duplicate path. */
    TestFile dup[] = {
        {"interface\\same.txt", text, sizeof text},
        {"interface\\same.txt", nif, sizeof nif}
    };
    BuiltBsa dup_built;
    check(build_bsa(blob, sizeof blob, dup, 2, 104, 0, 0, 1, &dup_built) != 0,
          "duplicate BSA builds");
    check(bsa_open_memory(blob, dup_built.len, NULL, &a, reason,
                          sizeof reason) == BSA_ERR_DUPLICATE,
          "duplicate path refused");

    /* M2: folder-record and name-block temporaries must be released from the
     * index budget before the name index and hash table are charged. */
    check(build_bsa(blob, sizeof blob, files, nfiles, 104, 0, 0, 1,
                    &built) != 0,
          "index-accounting BSA builds");
    a = NULL;
    check(bsa_open_memory(blob, built.len, NULL, &a, reason, sizeof reason) ==
              BSA_OK,
          "index-accounting BSA opens");
    size_t final_index = a ? bsa_index_bytes(a) : 0;
    if (a) bsa_close(a);
    a = NULL;
    check(final_index > 0, "index bytes are reported");
    BsaOptions tight;
    bsa_options_default(&tight);
    tight.max_index_bytes = (uint32_t)final_index;
    check(bsa_open_memory(blob, built.len, &tight, &a, reason,
                          sizeof reason) == BSA_OK,
          "temporary index blocks release their budget");
    if (a) bsa_close(a);
}

static void test_swf_header(void) {
    uint8_t fws[128], cws[512], zws[64];
    MenuSwfHeader h;
    char reason[BSA_REASON_MAX];
    size_t flen = make_fws(fws, sizeof fws, 0x10, 40);
    size_t clen = make_cws(cws, sizeof cws, 0x20, 100);
    size_t zlen = make_zws(zws, sizeof zws, 16);

    reason[0] = 0;
    check(menu_swf_header_parse(fws, flen, (uint32_t)flen, &h, reason,
                                sizeof reason) == BSA_OK,
          "FWS header accepted");
    check(h.format == MENU_SWF_FWS && h.version == 10 &&
              h.declared_bytes == flen && h.file_bytes == flen,
          "FWS fields");
    check(menu_swf_header_parse(cws, clen, (uint32_t)clen, &h, reason,
                                sizeof reason) == BSA_OK,
          "CWS header accepted without body decompression");
    check(h.format == MENU_SWF_CWS && h.version == 10 &&
              h.declared_bytes == 108 && h.file_bytes == clen,
          "CWS fields");
    check(menu_swf_header_parse(zws, zlen, (uint32_t)zlen, &h, reason,
                                sizeof reason) == BSA_ERR_UNSUPPORTED &&
              strstr(reason, "ZWS") != NULL,
          "ZWS refused by name");

    uint8_t bad[128];
    memcpy(bad, fws, flen);
    bad[3] = 0;
    check(menu_swf_header_parse(bad, flen, (uint32_t)flen, &h, reason,
                                sizeof reason) == BSA_ERR_UNSUPPORTED,
          "SWF version 0 refused");
    memcpy(bad, fws, flen);
    bad[3] = 99;
    check(menu_swf_header_parse(bad, flen, (uint32_t)flen, &h, reason,
                                sizeof reason) == BSA_ERR_UNSUPPORTED,
          "SWF version 99 refused");
    memcpy(bad, fws, flen);
    put_u32(bad + 4, (uint32_t)flen + 1);
    check(menu_swf_header_parse(bad, flen, (uint32_t)flen, &h, reason,
                                sizeof reason) == BSA_ERR_FORMAT,
          "FWS declared-size mismatch refused");
    memcpy(bad, cws, clen);
    bad[8] ^= 1;
    check(menu_swf_header_parse(bad, clen, (uint32_t)clen, &h, reason,
                                sizeof reason) != BSA_OK,
          "CWS bad zlib prefix refused");
    check(menu_swf_header_parse(cws, 7, (uint32_t)clen, &h, reason,
                                sizeof reason) == BSA_ERR_TRUNCATED,
          "short SWF header refused");
    check(menu_swf_header_parse(cws, 8, (uint32_t)clen, &h, reason,
                                sizeof reason) == BSA_ERR_TRUNCATED,
          "CWS without a zlib prefix refused");
    check(menu_swf_header_parse((const uint8_t *)"GIF89a..", 8, 8, &h, reason,
                                sizeof reason) == BSA_ERR_UNSUPPORTED,
          "non-SWF signature refused");
}

/* ------------------------------------------------------------------ */
/* Runtime resolution.                                                 */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static void test_runtime(void) {
    uint8_t base_swf[1024], ovr_swf[1024], zws_swf[256], comp_swf[1024];
    uint8_t text[64], base_blob[65536], ovr_blob[65536], miss_blob[65536];
    uint8_t zws_blob[65536], comp_blob[65536], corrupt_blob[64], got[4096];
    size_t base_swf_len = make_cws(base_swf, sizeof base_swf, 0x11, 180);
    size_t ovr_swf_len = make_cws(ovr_swf, sizeof ovr_swf, 0x77, 240);
    size_t zws_len = make_zws(zws_swf, sizeof zws_swf, 40);
    size_t comp_swf_len = make_cws(comp_swf, sizeof comp_swf, 0x44, 64);
    memset(text, 0x2e, sizeof text);

    TestFile base_files[] = {
        {"interface\\startmenu.swf", base_swf, base_swf_len},
        {"interface\\fontconfig.txt", text, sizeof text}
    };
    TestFile ovr_files[] = {
        {"interface\\startmenu.swf", ovr_swf, ovr_swf_len}
    };
    TestFile miss_files[] = {
        {"interface\\fontconfig.txt", text, sizeof text}
    };
    TestFile zws_files[] = {
        {"interface\\startmenu.swf", zws_swf, zws_len}
    };
    TestFile comp_files[] = {
        {"interface\\startmenu.swf", comp_swf, comp_swf_len}
    };
    BuiltBsa base_built, ovr_built, miss_built, zws_built, comp_built;
    check(build_bsa(base_blob, sizeof base_blob, base_files, 2, 104, 0, 0, 1,
                    &base_built) != 0, "runtime base BSA builds");
    check(build_bsa(ovr_blob, sizeof ovr_blob, ovr_files, 1, 104, 0, 0, 1,
                    &ovr_built) != 0, "runtime override BSA builds");
    check(build_bsa(miss_blob, sizeof miss_blob, miss_files, 1, 104, 0, 0, 1,
                    &miss_built) != 0, "runtime missing BSA builds");
    check(build_bsa(zws_blob, sizeof zws_blob, zws_files, 1, 104, 0, 0, 1,
                    &zws_built) != 0, "runtime ZWS BSA builds");
    check(build_bsa(comp_blob, sizeof comp_blob, comp_files, 1, 104, 1, 0, 1,
                    &comp_built) != 0, "runtime compressed BSA builds");

    check(write_file("bsa_test_base.bsa", base_blob, base_built.len),
          "write base fixture");
    check(write_file("bsa_test_override.bsa", ovr_blob, ovr_built.len),
          "write override fixture");
    check(write_file("bsa_test_missing.bsa", miss_blob, miss_built.len),
          "write missing fixture");
    check(write_file("bsa_test_zws.bsa", zws_blob, zws_built.len),
          "write ZWS fixture");
    check(write_file("bsa_test_compressed.bsa", comp_blob, comp_built.len),
          "write compressed fixture");
    check(write_file("bsa_test_locked.bsa", base_blob, base_built.len),
          "write locked fixture");
    memcpy(corrupt_blob, base_blob, sizeof corrupt_blob);
    memcpy(corrupt_blob, "NOPE", 4);
    check(write_file("bsa_test_corrupt.bsa", corrupt_blob, sizeof corrupt_blob),
          "write corrupt fixture");

    GuestMemory mem;
    GuestHeap heap;
    guest_init(&mem, 4u * 1024u * 1024u);
    check(heap_create(&heap, &mem, 0x20000000u, 2u * 1024u * 1024u) == 0,
          "guest heap created");

    MenuBsaSource pair[2] = {
        {"bsa_test_base.bsa", "Skyrim - Interface.bsa"},
        {"bsa_test_override.bsa", "Update.bsa"}
    };
    MenuBsaResult r;
    size_t before = heap.count;

    MenuBsaStatus st = menu_bsa_resolve_startmenu(&heap, pair, 2, 256u * 1024u,
                                                  &r);
    check(st == MENU_BSA_OK, "override resolution succeeds");
    if (st != MENU_BSA_OK) printf("  resolve reason: %s\n", r.reason);
    check(r.archive_index == 1, "Update.bsa wins last");
    check(strstr(r.archive_name, "Update.bsa") != NULL, "winner name reported");
    check(r.size == ovr_swf_len, "winner size is the override size");
    check(r.crc32 == bsa_crc32(ovr_swf, ovr_swf_len), "winner CRC-32");
    check(r.swf.format == MENU_SWF_CWS && r.swf.version == 10 &&
              r.swf.declared_bytes == 8 + 240,
          "winner SWF header");
    check(r.guest_address != 0, "winner has a guest address");
    check(guest_read(&mem, r.guest_address, got, r.size) == 0 &&
              !memcmp(got, ovr_swf, r.size),
          "winner bytes in guest memory");
    check(heap_free(&heap, r.guest_address) == 0 && heap.count == before,
          "winner guest block released");

    st = menu_bsa_resolve_startmenu(&heap, pair, 1, 256u * 1024u, &r);
    check(st == MENU_BSA_OK && r.archive_index == 0 &&
              r.size == base_swf_len &&
              r.crc32 == bsa_crc32(base_swf, base_swf_len),
          "base archive used without Update.bsa");
    if (st == MENU_BSA_OK) heap_free(&heap, r.guest_address);

    MenuBsaSource only_missing[1] = {
        {"bsa_test_missing.bsa", "Skyrim - Interface.bsa"}
    };
    st = menu_bsa_resolve_startmenu(&heap, only_missing, 1, 256u * 1024u, &r);
    check(st == MENU_BSA_MISSING && r.guest_address == 0,
          "missing StartMenu refused");
    check(strstr(r.reason, "StartMenu") != NULL, "missing reason names the path");

    MenuBsaSource corrupt_pair[2] = {
        {"bsa_test_corrupt.bsa", "Skyrim - Interface.bsa"},
        {"bsa_test_override.bsa", "Update.bsa"}
    };
    st = menu_bsa_resolve_startmenu(&heap, corrupt_pair, 2, 256u * 1024u, &r);
    check(st == MENU_BSA_CORRUPT, "corrupt archive refused even with an override");
    check(strstr(r.reason, "corrupt") != NULL ||
              strstr(r.reason, "not a BSA") != NULL,
          "corrupt reason is named");

    st = menu_bsa_resolve_startmenu(&heap, pair, 2, (uint32_t)ovr_swf_len - 1u,
                                    &r);
    check(st == MENU_BSA_BUDGET && r.guest_address == 0,
          "entry budget refusal");

    /* M1: a compressed entry's stored payload cannot exceed the caller budget,
     * so a 16-byte header prefix cannot cause a 64 MiB temporary allocation. */
    MenuBsaSource comp_src[1] = {
        {"bsa_test_compressed.bsa", "Skyrim - Interface.bsa"}
    };
    uint32_t tight_budget = (uint32_t)comp_swf_len + 8u;
    st = menu_bsa_resolve_startmenu(&heap, comp_src, 1, tight_budget, &r);
    check(st == MENU_BSA_BUDGET && r.guest_address == 0,
          "compressed stored payload bounded by caller budget");
    st = menu_bsa_resolve_startmenu(&heap, comp_src, 1, 256u * 1024u, &r);
    check(st == MENU_BSA_OK && r.size == comp_swf_len, "compressed entry resolves");
    if (st == MENU_BSA_OK) {
        check(guest_read(&mem, r.guest_address, got, r.size) == 0 &&
                  !memcmp(got, comp_swf, r.size),
              "compressed entry bytes in guest memory");
        heap_free(&heap, r.guest_address);
    }

    /* H1: a present but unopenable archive is an access error, not "missing":
     * resolution must not silently continue into Update.bsa. */
#ifdef _WIN32
    HANDLE locked = CreateFileA("bsa_test_locked.bsa", GENERIC_READ, 0, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(locked != INVALID_HANDLE_VALUE, "exclusive archive fixture opens");
    if (locked != INVALID_HANDLE_VALUE) {
        BsaArchive *locked_archive = NULL;
        char locked_reason[BSA_REASON_MAX] = "";
        check(bsa_open_path("bsa_test_locked.bsa", NULL, &locked_archive,
                            locked_reason, sizeof locked_reason) ==
                  BSA_ERR_ACCESS,
              "present-but-locked archive is BSA_ERR_ACCESS");
        check(strstr(locked_reason, "cannot open") != NULL,
              "access refusal has a named reason");
        MenuBsaSource locked_pair[2] = {
            {"bsa_test_locked.bsa", "Skyrim - Interface.bsa"},
            {"bsa_test_override.bsa", "Update.bsa"}
        };
        st = menu_bsa_resolve_startmenu(&heap, locked_pair, 2, 256u * 1024u,
                                        &r);
        check(st == MENU_BSA_ACCESS && r.guest_address == 0,
              "unopenable base is refused without an Update fallback");
        check(strstr(r.reason, "cannot open") != NULL,
              "resolver access refusal is named");
        CloseHandle(locked);
    }
#endif

    MenuBsaSource zws_pair[1] = {
        {"bsa_test_zws.bsa", "Skyrim - Interface.bsa"}
    };
    st = menu_bsa_resolve_startmenu(&heap, zws_pair, 1, 256u * 1024u, &r);
    check(st == MENU_BSA_UNSUPPORTED && strstr(r.reason, "ZWS") != NULL,
          "ZWS SWF refused by name");

    GuestHeap tiny;
    check(heap_create(&tiny, &mem, 0x30000000u, 64) == 0, "tiny guest heap");
    st = menu_bsa_resolve_startmenu(&tiny, pair, 2, 256u * 1024u, &r);
    check(st == MENU_BSA_GUEST_MEMORY && r.guest_address == 0,
          "guest heap refusal");
    heap_destroy(&tiny);

    MenuBsaSource absent[1] = {{"bsa_test_no_such.bsa", "Update.bsa"}};
    st = menu_bsa_resolve_startmenu(&heap, absent, 1, 256u * 1024u, &r);
    check(st == MENU_BSA_MISSING, "absent archive reported as missing");

    MenuBsaSources defaults;
    char reason[BSA_REASON_MAX];
    check(menu_bsa_sources_init(&defaults, "data/", reason, sizeof reason) == 1 &&
              strstr(defaults.source[0].path, "Skyrim - Interface.bsa") != NULL &&
              strstr(defaults.source[1].path, "Update.bsa") != NULL,
          "default source order helper");

    heap_destroy(&heap);
    guest_destroy(&mem);
    remove("bsa_test_base.bsa");
    remove("bsa_test_override.bsa");
    remove("bsa_test_missing.bsa");
    remove("bsa_test_zws.bsa");
    remove("bsa_test_compressed.bsa");
    remove("bsa_test_locked.bsa");
    remove("bsa_test_corrupt.bsa");
}

/* Optional read-only check against the installed game data. */
static void test_real(const char *data_dir) {
    MenuBsaSources src;
    char reason[BSA_REASON_MAX];
    if (!menu_bsa_sources_init(&src, data_dir, reason, sizeof reason)) {
        printf("real: skip (%s)\n", reason);
        return;
    }
    FILE *probe = fopen(src.source[0].path, "rb");
    if (!probe) {
        printf("real: skip (no %s)\n", src.source[0].path);
        return;
    }
    fclose(probe);

    GuestMemory mem;
    GuestHeap heap;
    guest_init(&mem, 8u * 1024u * 1024u);
    if (heap_create(&heap, &mem, 0x20000000u, 4u * 1024u * 1024u) != 0) {
        printf("real: skip (guest heap)\n");
        guest_destroy(&mem);
        return;
    }
    MenuBsaResult r;
    MenuBsaStatus st = menu_bsa_resolve_startmenu(&heap, src.source, 2,
                                                  1024u * 1024u, &r);
    check(st == MENU_BSA_OK, "real StartMenu resolves");
    if (st == MENU_BSA_OK) {
        printf("real: %s Interface\\StartMenu.swf %u bytes crc32=0x%08x "
               "%s v%u declared=%u\n",
               r.archive_name, r.size, r.crc32,
               menu_swf_format_name(r.swf.format), r.swf.version,
               r.swf.declared_bytes);
        check(r.archive_index == 1, "real Update.bsa overrides Interface.bsa");
        check(r.size == 60284u, "real StartMenu size matches census");
        check(r.crc32 == 0x6910248fu, "real StartMenu CRC-32 matches census");
        check(r.swf.format == MENU_SWF_CWS && r.swf.version == 10 &&
                  r.swf.declared_bytes == 144628u,
              "real StartMenu SWF header matches census");
        heap_free(&heap, r.guest_address);
    } else {
        printf("real: resolve failed: %s\n", r.reason);
    }

    st = menu_bsa_resolve_startmenu(&heap, src.source, 1, 1024u * 1024u, &r);
    check(st == MENU_BSA_OK, "real base archive resolves");
    if (st == MENU_BSA_OK) {
        printf("real: %s Interface\\StartMenu.swf %u bytes crc32=0x%08x "
               "(shadowed by Update.bsa)\n",
               r.archive_name, r.size, r.crc32);
        check(r.size == 60157u, "real base StartMenu size matches census");
        check(r.crc32 == 0xcc1c9285u, "real base StartMenu CRC-32 matches census");
        heap_free(&heap, r.guest_address);
    }

    /* The menu archives are uncompressed.  Exercise the compressed path and
     * embedded-name verification on a known real DDS from Textures.bsa. */
    char tex_path[MENU_BSA_DATA_PATH_MAX];
    snprintf(tex_path, sizeof tex_path, "%s/Skyrim - Textures.bsa", data_dir);
    BsaArchive *tex = NULL;
    BsaOptions tex_opt;
    bsa_options_default(&tex_opt);
    if (bsa_open_path(tex_path, &tex_opt, &tex, reason, sizeof reason) ==
        BSA_OK) {
        BsaMatch tm;
        if (bsa_find(tex, "textures\\actors\\dog\\dog.dds", &tm) == BSA_OK) {
            uint8_t hdr[148];
            size_t hn = 0;
            BsaStatus hs = bsa_read_prefix(tex, &tm.entry, hdr, sizeof hdr,
                                           &hn, reason, sizeof reason);
            uint32_t width = hn >= 20 ? get_u32(hdr + 16) : 0;
            uint32_t height = hn >= 16 ? get_u32(hdr + 12) : 0;
            printf("real: compressed Textures.bsa dog.dds prefix %zu bytes "
                   "magic=%.4s %ux%u\n", hn, (const char *)hdr, width, height);
            check(hs == BSA_OK && hn == sizeof hdr &&
                      !memcmp(hdr, "DDS ", 4) && width == 1024 &&
                      height == 1024,
                  "real compressed DDS prefix and embedded name verify");
        } else {
            printf("real: dog.dds missing from Textures.bsa\n");
        }
        bsa_close(tex);
    } else {
        printf("real: Textures.bsa skip (%s)\n", reason);
    }

    heap_destroy(&heap);
    guest_destroy(&mem);
}

int main(int argc, char **argv) {
    printf("menu BSA runtime tests\n");
    test_crc_and_inflate();
    test_bsa_matrix();
    test_bsa_embedded_and_errors();
    test_swf_header();
    test_runtime();

    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }

    printf("menu BSA runtime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
