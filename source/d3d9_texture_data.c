#include "d3d9_texture_data.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t d9_sktx_crc32(const void *data, size_t bytes) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static size_t expected_bytes(unsigned w, unsigned h, unsigned format) {
    return (size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) *
           (format == D9SKTX_FORMAT_ETC1A4 ? 16u : 8u);
}

int d9_sktx_parse(const void *data, size_t bytes, D9Sktx *out) {
    if (!data || !out || bytes < 24) return -1;
    const uint8_t *p = data;
    if (memcmp(p, "SKTX", 4) != 0) return -1;
    unsigned version = rd32(p + 4);
    unsigned format = rd16(p + 8);
    unsigned levels = rd16(p + 10);
    unsigned width = rd16(p + 12);
    unsigned height = rd16(p + 14);
    uint32_t data_bytes = rd32(p + 16);
    uint32_t crc = rd32(p + 20);
    if (version != 1 || format > D9SKTX_FORMAT_ETC1A4) return -1;
    if (!levels || levels > D9SKTX_MAX_LEVELS || !width || !height) return -1;
    size_t table_end = 24u + (size_t)levels * 12u;
    if (table_end > bytes) return -1;
    D9Sktx tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.version = version;
    tmp.format = format;
    tmp.width = width;
    tmp.height = height;
    tmp.levels = levels;
    tmp.data_bytes = data_bytes;
    tmp.crc32 = crc;
    tmp.container = p;
    tmp.container_bytes = bytes;
    size_t sum = 0;
    for (unsigned i = 0; i < levels; ++i) {
        const uint8_t *row = p + 24u + (size_t)i * 12u;
        unsigned w = rd16(row);
        unsigned h = rd16(row + 2);
        uint32_t offset = rd32(row + 4);
        uint32_t size = rd32(row + 8);
        if (!w || !h || offset < table_end ||
            (size_t)offset + size > bytes) return -1;
        if ((size_t)size != expected_bytes(w, h, format)) return -1;
        tmp.level[i].width = w;
        tmp.level[i].height = h;
        tmp.level[i].data = p + offset;
        tmp.level[i].bytes = size;
        sum += size;
    }
    if (sum != data_bytes || data_bytes > bytes - table_end) return -1;
    /* The pipeline writes levels in table order; CRC covers that exact
     * concatenation, so validate without building another buffer. */
    uint32_t running = 0xFFFFFFFFu;
    for (unsigned i = 0; i < levels; ++i) {
        const uint8_t *q = tmp.level[i].data;
        for (size_t k = 0; k < tmp.level[i].bytes; ++k) {
            running ^= q[k];
            for (unsigned bit = 0; bit < 8; ++bit)
                running = (running >> 1) ^
                          (0xEDB88320u & (uint32_t)-(int32_t)(running & 1u));
        }
    }
    if ((running ^ 0xFFFFFFFFu) != crc) return -1;
    *out = tmp;
    return 0;
}
