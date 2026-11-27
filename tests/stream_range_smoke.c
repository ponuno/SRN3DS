/* Host test: bsa_read_range() semantics plus the stream-index header parser.
 *
 * Every archive is synthetic and generated in this file.  A counting
 * BsaReadAt callback injects short and failed physical reads, so the test can
 * prove that one range read performs one physical read of exactly the
 * requested bytes, with no buffer for the whole record.
 */

#include "asset_stream.h"
#include "bsa_reader.h"

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

static uint32_t adler32_bytes(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* One stored (uncompressed) deflate block in a zlib envelope. */
static size_t zlib_store(uint8_t *dst, size_t cap, const uint8_t *src,
                         size_t n) {
    size_t need = 2 + 5 + n + 4;
    if (cap < need || n > 65535u) return 0;
    size_t at = 0;
    dst[at++] = 0x78;
    dst[at++] = 0x01;
    dst[at++] = 0x01;
    dst[at++] = (uint8_t)n;
    dst[at++] = (uint8_t)(n >> 8);
    dst[at++] = (uint8_t)~n;
    dst[at++] = (uint8_t)(~n >> 8);
    memcpy(dst + at, src, n);
    at += n;
    uint32_t adler = adler32_bytes(src, n);
    dst[at++] = (uint8_t)(adler >> 24);
    dst[at++] = (uint8_t)(adler >> 16);
    dst[at++] = (uint8_t)(adler >> 8);
    dst[at++] = (uint8_t)adler;
    return at;
}

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
} BuiltBsa;

static uint8_t g_comp[TEST_MAX_FILES][8192];
static size_t g_comp_len[TEST_MAX_FILES];

static int build_bsa(uint8_t *out, size_t cap, const TestFile *files,
                     size_t nfiles, int compressed, int embed,
                     BuiltBsa *built) {
    if (!out || !files || nfiles > TEST_MAX_FILES || !built) return 0;
    char folder[TEST_MAX_FOLDERS][192];
    size_t count[TEST_MAX_FOLDERS];
    size_t nf = 0;
    const char *base[TEST_MAX_FILES];
    for (size_t i = 0; i < nfiles; ++i) {
        const char *slash = strrchr(files[i].path, '\\');
        if (!slash) return 0;
        size_t flen = (size_t)(slash - files[i].path);
        if (!flen || flen >= sizeof folder[0]) return 0;
        base[i] = slash + 1;
        if (nf == 0 || strlen(folder[nf - 1]) != flen ||
            strncmp(folder[nf - 1], files[i].path, flen) != 0) {
            if (nf == TEST_MAX_FOLDERS) return 0;
            memcpy(folder[nf], files[i].path, flen);
            folder[nf][flen] = 0;
            count[nf] = 0;
            ++nf;
        }
        ++count[nf - 1];
    }
    size_t stored[TEST_MAX_FILES], offset[TEST_MAX_FILES];
    for (size_t i = 0; i < nfiles; ++i) {
        size_t prefix = embed ? 1u + strlen(files[i].path) : 0;
        if (compressed) {
            g_comp_len[i] = zlib_store(g_comp[i], sizeof g_comp[i],
                                       files[i].data, files[i].len);
            if (!g_comp_len[i]) return 0;
            stored[i] = prefix + 4u + g_comp_len[i];
        } else {
            stored[i] = prefix + files[i].len;
        }
    }
    size_t folder_name_bytes = 0;
    for (size_t f = 0; f < nf; ++f)
        folder_name_bytes += 1u + strlen(folder[f]) + 1u;
    size_t name_bytes = 0;
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
    put_u32(out + 4, 104);
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
    size_t file_index = 0;
    for (size_t f = 0; f < nf; ++f) {
        size_t flen = strlen(folder[f]);
        out[cursor++] = (uint8_t)(flen + 1);
        memcpy(out + cursor, folder[f], flen);
        cursor += flen;
        out[cursor++] = 0;
        for (size_t j = 0; j < count[f]; ++j, ++file_index) {
            put_u64(out + cursor, 0);
            put_u32(out + cursor + 8, (uint32_t)stored[file_index]);
            put_u32(out + cursor + 12, (uint32_t)offset[file_index]);
            cursor += 16;
        }
    }
    for (size_t i = 0; i < nfiles; ++i) {
        size_t len = strlen(base[i]);
        memcpy(out + cursor, base[i], len);
        cursor += len;
        out[cursor++] = 0;
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

/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t size;
    int calls;
    uint64_t bytes;
    int fail_call;
    int short_read;
} CountingReader;

static int counting_read(void *ctx, uint64_t offset, void *dst, size_t bytes) {
    CountingReader *reader = (CountingReader *)ctx;
    ++reader->calls;
    if (reader->fail_call && reader->calls == reader->fail_call) return -1;
    if (offset > reader->size || bytes > reader->size - (size_t)offset)
        return -1;
    size_t give = bytes;
    int failed = 0;
    if (reader->short_read && bytes > 1) {
        give = bytes - 1;
        failed = 1;
    }
    memcpy(dst, reader->data + (size_t)offset, give);
    reader->bytes += give;
    return failed ? -1 : 0;
}

static uint8_t g_archive[65536];
static uint8_t g_payload_a[300];
static uint8_t g_payload_b[1] = { 0x5a };
static uint8_t g_payload_c[5000];

static void fill_payloads(void) {
    for (size_t i = 0; i < sizeof g_payload_a; ++i)
        g_payload_a[i] = (uint8_t)(i * 7u + 3u);
    for (size_t i = 0; i < sizeof g_payload_c; ++i)
        g_payload_c[i] = (uint8_t)((i * 13u) ^ (i >> 3));
}

static const TestFile g_files[3] = {
    { "meshes\\a.nif", g_payload_a, sizeof g_payload_a },
    { "meshes\\b.nif", g_payload_b, sizeof g_payload_b },
    { "textures\\x.dds", g_payload_c, sizeof g_payload_c },
};

static void test_ranges(void) {
    BuiltBsa built;
    memset(g_archive, 0, sizeof g_archive);
    check(build_bsa(g_archive, sizeof g_archive, g_files, 3, 0, 0, &built),
          "uncompressed fixture builds");

    CountingReader reader;
    memset(&reader, 0, sizeof reader);
    reader.data = g_archive;
    reader.size = built.len;
    BsaArchive *archive = NULL;
    char reason[BSA_REASON_MAX];
    check(bsa_open_reader(counting_read, &reader, built.len, NULL, &archive,
                          reason, sizeof reason) == BSA_OK,
          "uncompressed fixture opens");

    BsaMatch a, c;
    check(bsa_find(archive, "meshes\\a.nif", &a) == BSA_OK, "find a.nif");
    check(bsa_find(archive, "TEXTURES/X.DDS", &c) == BSA_OK, "find x.dds");

    uint8_t full[5000];
    size_t full_len = 0;
    check(bsa_read(archive, &a.entry, full, sizeof full, &full_len, reason,
                   sizeof reason) == BSA_OK &&
              full_len == sizeof g_payload_a &&
              memcmp(full, g_payload_a, full_len) == 0,
          "full read reference matches the fixture");

    uint8_t buf[5000];
    uint32_t got = 0;

    reader.calls = 0;
    reader.bytes = 0;
    check(bsa_read_range(archive, &a.entry, 0, buf, 1, &got, reason,
                         sizeof reason) == BSA_OK && got == 1 &&
              buf[0] == g_payload_a[0],
          "range reads the first byte");
    check(reader.calls == 1 && reader.bytes == 1,
          "one range reads exactly one physical byte");

    memset(buf, 0, sizeof buf);
    got = 0;
    reader.calls = 0;
    reader.bytes = 0;
    check(bsa_read_range(archive, &c.entry, 2500, buf, 700, &got, reason,
                         sizeof reason) == BSA_OK && got == 700 &&
              memcmp(buf, g_payload_c + 2500, 700) == 0,
          "range reads the middle of the payload");
    check(reader.calls == 1 && reader.bytes == 700,
          "middle range does not read the whole record");

    got = 0;
    check(bsa_read_range(archive, &c.entry, 4999, buf, 1, &got, reason,
                         sizeof reason) == BSA_OK && got == 1 &&
              buf[0] == g_payload_c[4999],
          "range reads the last byte");

    got = 77;
    check(bsa_read_range(archive, &c.entry, 5000, buf, 0, &got, reason,
                         sizeof reason) == BSA_OK && got == 0,
          "zero bytes at the end are valid");
    got = 77;
    check(bsa_read_range(archive, &c.entry, 0, NULL, 0, &got, reason,
                         sizeof reason) == BSA_OK && got == 0,
          "zero-length range accepts a null destination");

    got = 55;
    reason[0] = 0;
    check(bsa_read_range(archive, &c.entry, 5001, buf, 0, &got, reason,
                         sizeof reason) == BSA_ERR_BOUNDS && got == 0 &&
              reason[0] != 0,
          "rel past the end is refused with a named reason");
    got = 55;
    check(bsa_read_range(archive, &c.entry, 4999, buf, 2, &got, reason,
                         sizeof reason) == BSA_ERR_BOUNDS && got == 0,
          "range crossing the end is refused");
    got = 55;
    check(bsa_read_range(archive, &c.entry, 0, buf, 0xffffffffu, &got, reason,
                         sizeof reason) == BSA_ERR_BOUNDS && got == 0,
          "n above the payload length is refused, not truncated");
    got = 55;
    check(bsa_read_range(archive, &c.entry, 0xffffffffu, buf, 1, &got, reason,
                         sizeof reason) == BSA_ERR_BOUNDS && got == 0,
          "rel = UINT32_MAX does not wrap");
    got = 55;
    check(bsa_read_range(archive, &c.entry, 0xfffffffeu, buf, 0xffffffffu,
                         &got, reason, sizeof reason) == BSA_ERR_BOUNDS &&
              got == 0,
          "rel + n overflow is refused");
    check(bsa_read_range(archive, &a.entry, 0, NULL, 1, &got, reason,
                         sizeof reason) == BSA_ERR_ARGUMENT && got == 0,
          "null destination with a non-zero length is an argument error");

    /* Injected physical failures must publish zero bytes. */
    reader.fail_call = reader.calls + 1;
    memset(buf, 0xAA, sizeof buf);
    got = 55;
    reason[0] = 0;
    check(bsa_read_range(archive, &a.entry, 10, buf, 7, &got, reason,
                         sizeof reason) == BSA_ERR_IO && got == 0 &&
              reason[0] != 0 && buf[0] == 0xAA,
          "failed physical read publishes nothing");
    reader.fail_call = 0;

    reader.short_read = 1;
    got = 55;
    check(bsa_read_range(archive, &a.entry, 10, buf, 7, &got, reason,
                         sizeof reason) == BSA_ERR_IO && got == 0,
          "short physical read is an I/O error");
    reader.short_read = 0;

    bsa_close(archive);
}

static void test_embedded_prefix(void) {
    BuiltBsa built;
    memset(g_archive, 0, sizeof g_archive);
    check(build_bsa(g_archive, sizeof g_archive, g_files, 3, 0, 1, &built),
          "embedded-name fixture builds");
    CountingReader reader;
    memset(&reader, 0, sizeof reader);
    reader.data = g_archive;
    reader.size = built.len;
    BsaArchive *archive = NULL;
    char reason[BSA_REASON_MAX];
    check(bsa_open_reader(counting_read, &reader, built.len, NULL, &archive,
                          reason, sizeof reason) == BSA_OK,
          "embedded-name fixture opens");
    BsaMatch c;
    check(bsa_find(archive, "textures\\x.dds", &c) == BSA_OK,
          "embedded-name find");
    uint8_t buf[700];
    uint32_t got = 0;
    check(bsa_read_range(archive, &c.entry, 0, buf, sizeof buf, &got, reason,
                         sizeof reason) == BSA_OK &&
              got == 700 && memcmp(buf, g_payload_c, 700) == 0,
          "embedded prefix is skipped");
    got = 0;
    check(bsa_read_range(archive, &c.entry, 4300, buf, 700, &got, reason,
                         sizeof reason) == BSA_OK &&
              got == 700 && memcmp(buf, g_payload_c + 4300, 700) == 0,
          "embedded-name tail range matches");
    bsa_close(archive);
}

static void test_compressed(void) {
    BuiltBsa built;
    memset(g_archive, 0, sizeof g_archive);
    check(build_bsa(g_archive, sizeof g_archive, g_files, 3, 1, 0, &built),
          "compressed fixture builds");
    CountingReader reader;
    memset(&reader, 0, sizeof reader);
    reader.data = g_archive;
    reader.size = built.len;
    BsaArchive *archive = NULL;
    char reason[BSA_REASON_MAX];
    check(bsa_open_reader(counting_read, &reader, built.len, NULL, &archive,
                          reason, sizeof reason) == BSA_OK,
          "compressed fixture opens");
    BsaMatch a;
    check(bsa_find(archive, "meshes\\a.nif", &a) == BSA_OK, "compressed find");
    uint8_t buf[512];
    uint32_t got = 42;
    reason[0] = 0;
    check(bsa_read_range(archive, &a.entry, 0, buf, sizeof buf, &got, reason,
                         sizeof reason) == BSA_ERR_UNSUPPORTED && got == 0 &&
              reason[0] != 0,
          "compressed range read is unsupported");
    size_t prefix_len = 0;
    check(bsa_read_prefix(archive, &a.entry, buf, sizeof buf, &prefix_len,
                          reason, sizeof reason) == BSA_OK &&
              prefix_len == sizeof g_payload_a &&
              memcmp(buf, g_payload_a, prefix_len) == 0,
          "bsa_read_prefix still handles compressed entries");
    size_t full_len = 0;
    check(bsa_read(archive, &a.entry, buf, sizeof buf, &full_len, reason,
                   sizeof reason) == BSA_OK &&
              full_len == sizeof g_payload_a &&
              memcmp(buf, g_payload_a, full_len) == 0,
          "bsa_read still inflates compressed entries");
    bsa_close(archive);
}

/* ------------------------------------------------------------------ */

static size_t make_empty_index(uint8_t *out, int corrupt_crc) {
    memset(out, 0, 84);
    memcpy(out, "SRNX", 4);
    out[4] = 1;                     /* schema */
    out[6] = STREAM_INDEX_HEADER_SIZE;
    out[8] = STREAM_INDEX_ARCHIVE_ENTRY_SIZE;
    out[10] = STREAM_INDEX_RESOURCE_ENTRY_SIZE;
    out[12] = STREAM_INDEX_CELL_ENTRY_SIZE;
    out[14] = 1;                    /* little-endian */
    put_u64(out + 32, 0x1122334455667788ull);
    put_u64(out + 40, 80);
    put_u64(out + 48, 80);
    put_u64(out + 56, 80);
    put_u64(out + 64, 80);
    put_u64(out + 72, 84);
    uint32_t crc = bsa_crc32(out, 80);
    if (corrupt_crc) crc ^= 1u;
    put_u32(out + 80, crc);
    return 84;
}

static void test_index_parser(void) {
    uint8_t blob[128];
    size_t size = make_empty_index(blob, 0);
    StreamIndex *index = NULL;
    char reason[STREAM_REASON_MAX];
    check(stream_index_parse(blob, size, &index, reason, sizeof reason)
              == STREAM_OK && index != NULL,
          "empty index parses");
    const StreamIndexInfo *info = stream_index_info(index);
    check(info && info->archive_count == 0 && info->resource_count == 0 &&
              info->generation == 0x1122334455667788ull,
          "empty index exposes its header");
    StreamResource res;
    check(stream_index_find_path(index, "MESHES/A.NIF", &res, reason,
                                 sizeof reason) == STREAM_ERR_NOT_FOUND,
          "absent path is not found");
    check(stream_index_find_path(index, "MESHES/A.NIF", &res, reason,
                                 sizeof reason) == STREAM_ERR_NOT_FOUND &&
              strstr(reason, "negative") != NULL,
          "second absent lookup hits the negative cache");
    stream_index_negative_reset(index);
    check(stream_index_find_path(index, "MESHES/A.NIF", &res, reason,
                                 sizeof reason) == STREAM_ERR_NOT_FOUND,
          "negative cache reset keeps the answer");
    check(stream_index_find_id(index, 0, &res, reason, sizeof reason)
              == STREAM_ERR_NOT_FOUND,
          "absent id is not found");
    stream_index_free(index);

    size = make_empty_index(blob, 1);
    check(stream_index_parse(blob, size, &index, reason, sizeof reason)
              == STREAM_ERR_FORMAT,
          "bad CRC is refused");
    size = make_empty_index(blob, 0);
    blob[0] = 'X';
    check(stream_index_parse(blob, size, &index, reason, sizeof reason)
              == STREAM_ERR_MAGIC,
          "bad magic is refused");
    size = make_empty_index(blob, 0);
    put_u64(blob + 72, size + 1);
    check(stream_index_parse(blob, size, &index, reason, sizeof reason)
              == STREAM_ERR_BOUNDS,
          "declared size past the buffer is refused");
    check(stream_index_parse(blob, 20, &index, reason, sizeof reason)
              == STREAM_ERR_ARGUMENT,
          "truncated buffer is refused");
}

int main(void) {
    fill_payloads();
    test_ranges();
    test_embedded_prefix();
    test_compressed();
    test_index_parser();
    printf("stream_range_smoke: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
