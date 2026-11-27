/* Real Bink 1 (BIK) frame source for the win32 shim and host tests.
 *
 * The retail binkw32.dll bodies are x86 and are not lifted.  On the Windows
 * host we decode with FFmpeg's real `binkvideo` decoder into a raw BGRA frame
 * store; the shim then copies those decoded pixels into guest memory.  There
 * is no synthetic/gray fallback: if the decoder is unavailable the open fails
 * and the caller reports a named refusal.
 *
 * On the 3DS (__3DS__) there is no FFmpeg, so bink_decode_open() returns
 * BINK_DECODE_NO_DECODER and the shim keeps its named middleware refusal.
 */
#ifndef BINK_DECODE_H
#define BINK_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define BINK_DECODE_OK           0
#define BINK_DECODE_BAD_ARG     -1
#define BINK_DECODE_NO_DECODER  -2
#define BINK_DECODE_FAILED      -3
#define BINK_DECODE_BAD_FORMAT  -4
#define BINK_DECODE_BAD_RECT    -5

typedef struct {
    int32_t  width;
    int32_t  height;
    uint32_t frames;
    uint32_t fps_num;
    uint32_t fps_den;
    size_t   frame_bytes;      /* width*height*4, BGRA */
    char     raw_path[512];    /* decoded frame store, one BGRA frame after another */
    FILE    *fp;               /* open raw store, NULL when closed */
} BinkDecoded;

/* Decode host_path (a real .bik) into a private raw BGRA store.
 * Returns BINK_DECODE_OK or a negative BINK_DECODE_* code. */
int bink_decode_open(const char *host_path, BinkDecoded *out);

/* Release the frame store.  Safe on a zeroed struct. */
void bink_decode_close(BinkDecoded *out);

/* Span in bytes that bink_decode_copy_rect() writes into `dst`, i.e. the
 * bytes that must be mapped and contiguous.  0 means the rectangle or flags
 * are invalid (the copy would return BINK_DECODE_BAD_RECT).  `dst_height` is
 * the destination buffer height for bounds checking, not the number of rows
 * written: callers that tile a frame (256x256 copy into a 1024-pitch, 720-row
 * surface) must validate only dst_y+src_h rows, not the whole height. */
uint64_t bink_copy_span(uint32_t dst_pitch, uint32_t dst_height,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t src_w, uint32_t src_h, uint32_t flags);

/* Copy a rectangle of one decoded frame into dst.
 * dst_pitch/dst_height describe the destination guest buffer, dst_x/dst_y are
 * its pixel origin, src_* select the frame rectangle.  `flags` follows the
 * Bink copy flags: low three bits select BGRA/RGBA/ABGR/ARGB/RGB/BGR/565,
 * bit 0x20 flips vertically.  Returns BINK_DECODE_OK or a negative code. */
int bink_decode_copy_rect(BinkDecoded *d, uint32_t frame,
                          void *dst, uint32_t dst_pitch, uint32_t dst_height,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t src_w, uint32_t src_h, uint32_t flags);

/* Bink copy-flag constants (bink.h values for the formats we implement). */
#define BINK_COPY_BGRA 0x0u
#define BINK_COPY_RGBA 0x1u
#define BINK_COPY_ABGR 0x2u
#define BINK_COPY_ARGB 0x3u
#define BINK_COPY_RGB  0x4u
#define BINK_COPY_BGR  0x5u
#define BINK_COPY_565  0x6u
#define BINK_COPY_FLIP 0x20u

#endif /* BINK_DECODE_H */
