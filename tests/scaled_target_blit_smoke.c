/* Scaled render-target blits on the null backend.
 *
 * Bounded targets store a 1024/512 logical surface in 256x256 (default cap) or
 * another capped native size.  StretchRect must map the logical rectangles
 * through that storage instead of treating them as native pixels.  The null
 * backend owns real BGRA pixels, so the full 1024->1024, 1024->512 and
 * 512->1024 paths are checked pixel by pixel, including both filters.
 *
 * GetRenderTargetData and UpdateSurface only operate on CPU-backed sources or
 * destinations; GPU-only/bounded targets are refused, so there is no silent
 * scaled copy path to test here.  Scaled LockRect/UnlockRect remain named
 * refusals with the lossy reason.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_cap_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_TARGET_NATIVE_MAX", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_TARGET_NATIVE_MAX", v, 1);
    else unsetenv("TESV_D3D9_TARGET_NATIVE_MAX");
#endif
}
static void set_scaled_readback_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_SCALED_READBACK", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_SCALED_READBACK", v, 1);
    else unsetenv("TESV_D3D9_SCALED_READBACK");
#endif
}

static void fill_blocks(uint8_t *p, unsigned w, unsigned h) {
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            uint8_t *d = p + ((size_t)y * w + x) * 4;
            d[0] = 0x40;                 /* B */
            d[1] = (uint8_t)(y / 16 * 16); /* G */
            d[2] = (uint8_t)(x / 16 * 16); /* R */
            d[3] = 0xff;
        }
    }
}
static int blocks_match(const uint8_t *p, const uint8_t *q, unsigned w, unsigned h) {
    return memcmp(p, q, (size_t)w * h * 4) == 0;
}
static void fill_rows(uint8_t *p, unsigned w, unsigned h) {
    for (unsigned y = 0; y < h; ++y) {
        uint8_t v = (y & 1) ? 0xff : 0x00;
        for (unsigned x = 0; x < w; ++x) {
            uint8_t *d = p + ((size_t)y * w + x) * 4;
            d[0] = d[1] = d[2] = v;
            d[3] = 0xff;
        }
    }
}
static D9Handle make_target(D9Device *d, unsigned w, unsigned h) {
    D9Handle out = 0;
    assert(d9_create_render_target(d, w, h, D9_A8R8G8B8, 0, &out) == D9_OK);
    return out;
}
static void read_target(D9Device *d, D9Handle h, uint8_t *pixels, unsigned w) {
    assert(d9_render_target_read(d, h, pixels, (size_t)w * 4, 0, 0, w, w) == D9_OK);
}
static void blit_full(D9Device *d, D9Handle dst, D9Handle src, unsigned filter) {
    assert(d9_blit(d, dst, NULL, src, NULL, filter) == D9_OK);
}

int main(void) {
    /* Default cap 256: 1024 and 512 logical targets both use 256x256 native
     * storage.  The pattern survives 1024->1024, 1024->512, 512->1024. */
    set_cap_env(NULL);
    set_scaled_readback_env(NULL);
    d9_null_reset();
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 32u << 20);
    assert(d);
    assert(d9_target_native_max(d) == 256);

    static uint8_t pattern[256 * 256 * 4];
    fill_blocks(pattern, 256, 256);

    D9Handle a = make_target(d, 256, 256);
    D9Handle s1024 = make_target(d, 1024, 1024);
    D9Handle d1024 = make_target(d, 1024, 1024);
    D9Handle s512 = make_target(d, 512, 512);
    D9Handle d512 = make_target(d, 512, 512);
    D9Handle b = make_target(d, 256, 256);
    assert(d9_render_target_write(d, a, pattern, 256u * 4, 0, 0, 256, 256) == D9_OK);

    /* 256 normal -> 1024 bounded */
    blit_full(d, s1024, a, D9_FILTER_NONE);
    /* 1024 bounded -> 1024 bounded, bilinear */
    unsigned linear_before = d9_null_stats()->blit_linear;
    blit_full(d, d1024, s1024, D9_FILTER_LINEAR);
    assert(d9_null_stats()->blit_linear == linear_before + 1);
    /* bounded -> 256 normal, readable */
    blit_full(d, b, d1024, D9_FILTER_NONE);
    static uint8_t out[256 * 256 * 4];
    read_target(d, b, out, 256);
    assert(blocks_match(pattern, out, 256, 256));

    /* 1024 bounded -> 512 bounded -> 256 normal */
    blit_full(d, s512, d1024, D9_FILTER_NONE);
    blit_full(d, b, s512, D9_FILTER_NONE);
    read_target(d, b, out, 256);
    assert(blocks_match(pattern, out, 256, 256));

    /* 512 bounded -> 1024 bounded, bilinear -> 256 normal */
    linear_before = d9_null_stats()->blit_linear;
    blit_full(d, d512, s512, D9_FILTER_LINEAR);
    assert(d9_null_stats()->blit_linear == linear_before + 1);
    blit_full(d, b, d512, D9_FILTER_NONE);
    read_target(d, b, out, 256);
    assert(blocks_match(pattern, out, 256, 256));

    /* Scaled LockRect/UnlockRect are named refusals with the exact lossy reason. */
    uint8_t row[1024 * 4];
    assert(d9_render_target_read(d, s1024, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "lossy") != NULL);
    assert(strstr(d9_last_draw_diagnostic(d), "256x256") != NULL);
    assert(d9_render_target_write(d, s1024, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "lossy") != NULL);

    assert(d9_destroy_device_ex(d) == 0);

    /* Native-size-changing path for bilinear: cap 512 gives a 512x256 source
     * (normal) and a 1024x512 bounded destination (512x512 native).  Nearest
     * repeats source rows; bilinear interpolates them. */
    set_cap_env("512");
    d9_null_reset();
    D9Device *db = d9_create_device(d9_null_backend(), d9_null_context(), 32u << 20);
    assert(db && d9_target_native_max(db) == 512);
    static uint8_t rows[512 * 256 * 4];
    fill_rows(rows, 512, 256);
    D9Handle src = make_target(db, 512, 256);
    D9Handle dstn = make_target(db, 1024, 512);
    D9Handle dstl = make_target(db, 1024, 512);
    D9Handle outn = make_target(db, 512, 512);
    D9Handle outl = make_target(db, 512, 512);
    assert(d9_render_target_write(db, src, rows, 512u * 4, 0, 0, 512, 256) == D9_OK);
    blit_full(db, dstn, src, D9_FILTER_NONE);
    blit_full(db, outn, dstn, D9_FILTER_NONE);
    unsigned lb = d9_null_stats()->blit_linear;
    blit_full(db, dstl, src, D9_FILTER_LINEAR);
    assert(d9_null_stats()->blit_linear == lb + 1);
    blit_full(db, outl, dstl, D9_FILTER_NONE);
    static uint8_t nearest[512 * 512 * 4], linear[512 * 512 * 4];
    read_target(db, outn, nearest, 512);
    read_target(db, outl, linear, 512);
    unsigned diff = 0;
    for (size_t i = 0; i < sizeof nearest; i += 4)
        if (memcmp(nearest + i, linear + i, 4) != 0) ++diff;
    assert(diff != 0);
    assert(d9_destroy_device_ex(db) == 0);

    set_cap_env(NULL);
    printf("scaled target blit: 1024/512 logical chains, nearest+bilinear, lossy lock refusal PASS\n");
    return 0;
}
