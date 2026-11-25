/* Host test: SKTX container parser used by the ETC1 runtime uploader. */

#include "d3d9_texture_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void wr16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t build(uint8_t *out, unsigned format, unsigned w, unsigned h,
                    unsigned levels) {
    size_t table = 24u + (size_t)levels * 12u;
    size_t data = 0;
    for (unsigned i = 0; i < levels; ++i) {
        unsigned lw = w >> i, lh = h >> i;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        data += (size_t)((lw + 3u) / 4u) * ((lh + 3u) / 4u) *
                (format == D9SKTX_FORMAT_ETC1A4 ? 16u : 8u);
    }
    memset(out, 0, table + data);
    memcpy(out, "SKTX", 4);
    wr32(out + 4, 1);
    wr16(out + 8, format);
    wr16(out + 10, levels);
    wr16(out + 12, w);
    wr16(out + 14, h);
    wr32(out + 16, (uint32_t)data);
    size_t offset = table;
    for (unsigned i = 0; i < levels; ++i) {
        unsigned lw = w >> i, lh = h >> i;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        size_t size = (size_t)((lw + 3u) / 4u) * ((lh + 3u) / 4u) *
                      (format == D9SKTX_FORMAT_ETC1A4 ? 16u : 8u);
        uint8_t *row = out + 24u + (size_t)i * 12u;
        wr16(row, lw);
        wr16(row + 2, lh);
        wr32(row + 4, (uint32_t)offset);
        wr32(row + 8, (uint32_t)size);
        for (size_t k = 0; k < size; ++k) out[offset + k] = (uint8_t)(k * 7u);
        offset += size;
    }
    wr32(out + 20, d9_sktx_crc32(out + table, data));
    return table + data;
}

int main(int argc, char **argv) {
    uint8_t blob[512];
    D9Sktx sktx;

    check(d9_sktx_crc32("123456789", 9) == 0xCBF43926u, "crc32 test vector");

    size_t bytes = build(blob, D9SKTX_FORMAT_ETC1, 16, 16, 2);
    check(bytes == 24 + 24 + 128 + 32, "fixture size");
    check(d9_sktx_parse(blob, bytes, &sktx) == 0, "parse ETC1 chain");
    check(sktx.version == 1 && sktx.format == D9SKTX_FORMAT_ETC1, "header");
    check(sktx.width == 16 && sktx.height == 16 && sktx.levels == 2, "dims");
    check(sktx.data_bytes == 160, "data bytes");
    check(sktx.level[0].width == 16 && sktx.level[0].bytes == 128, "level 0");
    check(sktx.level[1].width == 8 && sktx.level[1].bytes == 32, "level 1");
    check(sktx.level[0].data == blob + 48, "level 0 pointer");
    check(sktx.level[1].data == blob + 176, "level 1 pointer");
    check(sktx.level[1].data[0] == 0 && sktx.level[1].data[1] == 7, "level data");

    size_t a4 = build(blob, D9SKTX_FORMAT_ETC1A4, 8, 8, 1);
    check(d9_sktx_parse(blob, a4, &sktx) == 0, "parse ETC1A4");
    check(sktx.format == D9SKTX_FORMAT_ETC1A4, "ETC1A4 format");
    check(sktx.level[0].bytes == 64, "ETC1A4 level bytes");

    /* Every validation branch must reject, never partially succeed. */
    bytes = build(blob, D9SKTX_FORMAT_ETC1, 16, 16, 2);
    uint8_t bad[512];
    memcpy(bad, blob, bytes);
    bad[0] = 'X';
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject magic");
    memcpy(bad, blob, bytes);
    wr32(bad + 4, 2);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject version");
    memcpy(bad, blob, bytes);
    wr16(bad + 10, 0);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject zero levels");
    memcpy(bad, blob, bytes);
    wr16(bad + 10, D9SKTX_MAX_LEVELS + 1);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject too many levels");
    memcpy(bad, blob, bytes);
    wr32(bad + 24 + 8, 127);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject wrong block size");
    memcpy(bad, blob, bytes);
    wr32(bad + 24 + 4, 500);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject out-of-range offset");
    memcpy(bad, blob, bytes);
    wr32(bad + 16, 1);
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject data_bytes mismatch");
    memcpy(bad, blob, bytes);
    bad[bytes - 1] ^= 1;
    check(d9_sktx_parse(bad, bytes, &sktx) != 0, "reject crc");
    check(d9_sktx_parse(blob, bytes - 1, &sktx) != 0, "reject truncation");
    check(d9_sktx_parse(NULL, 0, &sktx) != 0, "reject NULL");

    /* Optional: validate a container produced by the Python pipeline. */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        check(f != NULL, "open real container");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *buf = malloc((size_t)size);
            check(buf != NULL, "alloc real container");
            if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
                check(d9_sktx_parse(buf, (size_t)size, &sktx) == 0,
                      "parse real SKTX");
                check(sktx.format == D9SKTX_FORMAT_ETC1A4,
                      "real menu sample is ETC1A4");
                check(sktx.levels > 0, "real levels present");
                printf("  real: %ux%u %s, %u levels, %u data bytes\n",
                       sktx.width, sktx.height,
                       sktx.format == D9SKTX_FORMAT_ETC1A4 ? "ETC1A4" : "ETC1",
                       sktx.levels, (unsigned)sktx.data_bytes);
            }
            free(buf);
            fclose(f);
        }
    }

    printf("texture container: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
