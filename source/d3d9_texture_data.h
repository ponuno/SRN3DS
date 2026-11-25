#pragma once
/* SKTX container parser: the host pipeline's ETC1/ETC1A4 level bundle.
 *
 * The host tool (tools/skyrim/etc1.py) writes:
 *   "SKTX" u32 version=1 u16 format u16 levels u16 width u16 height
 *   u32 data_bytes u32 crc32
 *   levels * { u16 w, u16 h, u32 offset, u32 bytes }
 *   level data (PICA-tiled ETC1 = 8 B/block, ETC1A4 = 16 B/block)
 *
 * This parser validates the structure and block sizes and returns pointers
 * into the caller's buffer; it does not allocate or upload.  A backend can pass
 * each level to C3D_TexLoadImage with GPU_ETC1/GPU_ETC1A4.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D9SKTX_MAX_LEVELS 16u
#define D9SKTX_FORMAT_ETC1 0u
#define D9SKTX_FORMAT_ETC1A4 1u

typedef struct {
    unsigned width;
    unsigned height;
    const uint8_t *data;
    size_t bytes;
} D9SktxLevel;

typedef struct {
    unsigned version;
    unsigned format;        /* D9SKTX_FORMAT_* */
    unsigned width;
    unsigned height;
    unsigned levels;
    size_t data_bytes;
    uint32_t crc32;
    const uint8_t *container;
    size_t container_bytes;
    D9SktxLevel level[D9SKTX_MAX_LEVELS];
} D9Sktx;

/* Returns 0 on success; -1 on malformed data (no partial output trusted). */
int d9_sktx_parse(const void *data, size_t bytes, D9Sktx *out);

/* zlib CRC-32, exposed for the host/container tests. */
uint32_t d9_sktx_crc32(const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
