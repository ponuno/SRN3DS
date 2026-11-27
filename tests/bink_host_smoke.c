/* Host test: decode a real Bink 1 file with the FFmpeg-backed frame source and
 * prove the pixels/pitch/rectangle contract used by the win32 shim.
 *
 * This is a video-frame result, not a menu frame.  It opens the retail
 * Data\Video\BGS_Logo.bik when present and writes the decoded frame as raw
 * BGRA plus a PPM for inspection. */
#include "bink_decode.h"
#include "guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

static const char *video_path(int argc, char **argv) {
    const char *p = getenv("BINK_TEST_FILE");
    if (p && *p) return p;
    if (argc > 1 && argv[1][0]) return argv[1];
    return "E:/harness_skyrim/Data/Video/BGS_Logo.bik";
}

static void out_dir(char *out, size_t cap) {
    const char *d = getenv("BINK_OUT_DIR");
    if (d && *d) { snprintf(out, cap, "%s", d); return; }
    const char *t = getenv("TEMP");
    if (!t || !*t) t = ".";
    snprintf(out, cap, "%s\\srn3ds_host_%ld", t, (long)GETPID());
}

static uint32_t hash_px(const unsigned char *p) {
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void write_ppm(const char *path, const unsigned char *bgra, int w, int h) {
    FILE *f = fopen(path, "wb");
    int x, y;
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        const unsigned char *p = bgra + ((size_t)y * w + x) * 4u;
        fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *path = video_path(argc, argv);
    BinkDecoded d;
    char dir[512], raw0[600], raw1[600], ppm[600], info[600];
    unsigned char *f0, *f0b, *f1, *rect;
    size_t full;
    int rc, w, h, x, y;
    const uint32_t F0 = 80, F1 = 0;   /* logo has faded in by frame 80 */
    size_t nonzero = 0, diff = 0;
    uint32_t colors[4096];
    unsigned ncolors = 0;

    /* Destination span: the engine tiles the frame into locked 256x256
     * surface rectangles and passes the surface height (720) as dh. The shim
     * must validate only dst_y+sh rows; asking for pitch*dh rejected every
     * tiled copy as unmapped (the TESV BGS_LOGO thread died there). */
    assert(bink_copy_span(1024, 720, 0, 0, 256, 256, BINK_COPY_ARGB) == 256u * 1024u);
    assert(bink_copy_span(1024, 720, 0, 464, 256, 256, BINK_COPY_ARGB) == 720u * 1024u);
    assert(bink_copy_span(5120, 720, 0, 0, 1280, 720, BINK_COPY_BGRA) == 720u * 5120u);
    assert(bink_copy_span(5120, 720, 3, 0, 1280, 1, BINK_COPY_RGB) == 3849u);
    assert(bink_copy_span(100, 8, 0, 0, 25, 8, BINK_COPY_565) == 750u);
    assert(bink_copy_span(1024, 720, 0, 0, 257, 256, BINK_COPY_ARGB) == 0); /* > pitch/4 */
    assert(bink_copy_span(1024, 720, 0, 465, 256, 256, BINK_COPY_ARGB) == 0); /* > height */
    assert(bink_copy_span(1024, 720, 0, 0, 256, 256, 7) == 0);                /* bad format */
    assert(bink_copy_span(1024, 720, 0, 0, 0, 256, BINK_COPY_ARGB) == 0);
    printf("BINK_HOST span contract OK\n");

    rc = bink_decode_open(path, &d);
    if (rc == BINK_DECODE_BAD_ARG) {
        printf("BINK_HOST SKIP: no readable video at %s\n", path);
        return 0;
    }
    assert(rc == BINK_DECODE_OK);
    w = d.width; h = d.height;
    assert(w == 1280 && h == 720);          /* retail BGS_Logo.bik */
    assert(d.frames == 166);
    assert(d.fps_num == 24000 && d.fps_den == 1001);
    assert(d.frame_bytes == (size_t)w * h * 4u);
    printf("BINK_HOST open %s: %dx%d frames=%u fps=%u/%u\n",
           path, w, h, d.frames, d.fps_num, d.fps_den);

    full = d.frame_bytes;
    f0 = (unsigned char *)malloc(full);
    f0b = (unsigned char *)malloc(full);
    f1 = (unsigned char *)malloc(full);
    rect = (unsigned char *)malloc((size_t)w * h * 4u + 64u * h);
    assert(f0 && f0b && f1 && rect);

    /* A representative frame through the same full-frame copy the engine uses. */
    assert(bink_decode_copy_rect(&d, F0, f0, (uint32_t)w * 4u, (uint32_t)h,
                                 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, BINK_COPY_BGRA) == 0);
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        const unsigned char *p = f0 + ((size_t)y * w + x) * 4u;
        if (p[0] | p[1] | p[2]) ++nonzero;
    }
    assert(nonzero > (size_t)w * h / 100u);  /* at least 1% lit pixels */
    assert(nonzero < (size_t)w * h);         /* not a white/gray fill */

    /* Quantised colour census: a real logo frame has many colours, a gray
     * fill has one. */
    for (y = 0; y < h; y += 7) for (x = 0; x < w; x += 7) {
        uint32_t c = hash_px(f0 + ((size_t)y * w + x) * 4u);
        unsigned i;
        for (i = 0; i < ncolors; ++i) if (colors[i] == c) break;
        if (i == ncolors && ncolors < 4096) colors[ncolors++] = c;
    }
    assert(ncolors > 16);
    printf("BINK_HOST frame0: nonzero=%lu/%lu distinct=%u\n",
           (unsigned long)nonzero, (unsigned long)((size_t)w * h), ncolors);

    /* Same frame twice must be byte-identical (deterministic store). */
    assert(bink_decode_copy_rect(&d, F0, f0b, (uint32_t)w * 4u, (uint32_t)h,
                                 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, BINK_COPY_BGRA) == 0);
    assert(memcmp(f0, f0b, full) == 0);

    /* The same copy into a mapped GuestMemory region (the CPU's address space
     * primitive), proving the guest-buffer/pitch contract used by the shim. */
    {
        GuestMemory gm;
        const uint32_t gbase = 0x40000000u;
        guest_init(&gm, 64u << 20);
        assert(guest_map(&gm, gbase, (uint32_t)full) == 0);
        assert(bink_decode_copy_rect(&d, F0, guest_ptr(&gm, gbase, full),
                                     (uint32_t)w * 4u, (uint32_t)h,
                                     0, 0, 0, 0, (uint32_t)w, (uint32_t)h,
                                     BINK_COPY_BGRA) == 0);
        assert(memcmp(guest_ptr(&gm, gbase, full), f0, full) == 0);
        guest_unmap(&gm, gbase);
        guest_destroy(&gm);
        printf("BINK_HOST guest-buffer copy %ux%u pitch=%d OK\n", w, h, w * 4);
    }

    /* A different frame must differ: the stream is moving video, not a freeze. */
    assert(bink_decode_copy_rect(&d, F1, f1, (uint32_t)w * 4u, (uint32_t)h,
                                 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, BINK_COPY_BGRA) == 0);
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x)
        if (memcmp(f0 + ((size_t)y * w + x) * 4u, f1 + ((size_t)y * w + x) * 4u, 4)) ++diff;
    assert(diff > (size_t)w * h / 500u);
    printf("BINK_HOST frames %u/%u differ in %lu pixels\n", F0, F1, (unsigned long)diff);

    /* Pitch + rectangle: copy 64x32 from (100,50) to (16,8) in a buffer whose
     * pitch is w*4+64, then compare every pixel and prove the padding and the
     * area outside the rectangle stayed untouched. */
    memset(rect, 0, (size_t)w * h * 4u + 64u * h);
    assert(bink_decode_copy_rect(&d, F0, rect, (uint32_t)w * 4u + 64u, (uint32_t)h,
                                 16, 8, 100, 50, 64, 32, BINK_COPY_BGRA) == 0);
    for (y = 0; y < 32; ++y) for (x = 0; x < 64; ++x) {
        const unsigned char *got = rect + (size_t)(8 + y) * ((size_t)w * 4u + 64u)
                                         + (size_t)(16 + x) * 4u;
        const unsigned char *want = f0 + (size_t)(50 + y) * ((size_t)w * 4u)
                                         + (size_t)(100 + x) * 4u;
        assert(memcmp(got, want, 4) == 0);
    }
    assert(rect[(size_t)8 * ((size_t)w * 4u + 64u) + 15u * 4u + 3u] == 0);
    assert(rect[(size_t)(8 + 32) * ((size_t)w * 4u + 64u) + 16u * 4u] == 0);
    assert(rect[(size_t)8 * ((size_t)w * 4u + 64u) + (size_t)(16 + 64) * 4u] == 0);

    /* RGBA conversion swaps red and blue, alpha stays opaque. */
    {
        unsigned char a[4], b[4];
        assert(bink_decode_copy_rect(&d, F0, a, 4, 1, 0, 0, 100, 50, 1, 1, BINK_COPY_BGRA) == 0);
        assert(bink_decode_copy_rect(&d, F0, b, 4, 1, 0, 0, 100, 50, 1, 1, BINK_COPY_RGBA) == 0);
        assert(a[0] == b[2] && a[1] == b[1] && a[2] == b[0] && a[3] == b[3]);
        assert(a[3] == 255);
    }

    out_dir(dir, sizeof dir);
    MKDIR(dir);
    snprintf(raw0, sizeof raw0, "%s\\bink_frame.raw", dir);
    snprintf(raw1, sizeof raw1, "%s\\bink_frame_first.raw", dir);
    snprintf(ppm, sizeof ppm, "%s\\bink_frame.ppm", dir);
    snprintf(info, sizeof info, "%s\\bink_frame_info.txt", dir);
    {
        FILE *f = fopen(raw0, "wb"); assert(f); assert(fwrite(f0, 1, full, f) == full); fclose(f);
        f = fopen(raw1, "wb"); assert(f); assert(fwrite(f1, 1, full, f) == full); fclose(f);
        write_ppm(ppm, f0, w, h);
        f = fopen(info, "w"); assert(f);
        fprintf(f, "source=%s\nwidth=%d\nheight=%d\nframes=%u\nfps=%u/%u\n"
                   "format=BGRA\npitch=%d\nframe=%u_nonzero=%lu\ndistinct=%u\n"
                   "frame%u_diff_pixels=%lu\n",
                path, w, h, d.frames, d.fps_num, d.fps_den, w * 4,
                F0, (unsigned long)nonzero, ncolors, F1, (unsigned long)diff);
        fclose(f);
    }
    printf("BINK_HOST PASS raw=%s ppm=%s\n", raw0, ppm);
    bink_decode_close(&d);
    free(f0); free(f0b); free(f1); free(rect);
    return 0;
}
