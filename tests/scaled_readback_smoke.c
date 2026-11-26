/* Env-gated scaled readback for bounded render targets.
 *
 * TESV_D3D9_SCALED_READBACK=1 (default off) makes LockRect read native pixels
 * and nearest-upscale them into the logical rectangle; UnlockRect samples the
 * logical rectangle at native pixel centres and writes it back.  The mapping is
 * intentionally lossy: a 1024 logical / 256 native target exposes each native
 * pixel as a 4x4 logical block, and write-back keeps the native-centre pixel.
 *
 * The null backend has real pixels, so both directions are checked.  Default
 * off keeps the explicit lossy refusal and refuses lockable >512 targets.  The
 * retained back buffer stays on its unscaled path.
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
static void set_readback_env(const char *v) {
#ifdef _WIN32
    _putenv_s("TESV_D3D9_SCALED_READBACK", v ? v : "");
#else
    if (v && *v) setenv("TESV_D3D9_SCALED_READBACK", v, 1);
    else unsetenv("TESV_D3D9_SCALED_READBACK");
#endif
}

static void fill_blocks(uint8_t *p, unsigned w, unsigned h, unsigned block) {
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            uint8_t *d = p + ((size_t)y * w + x) * 4;
            unsigned bx = x / block, by = y / block;
            d[0] = (uint8_t)(bx * 17u + 3u);
            d[1] = (uint8_t)(by * 11u + 7u);
            d[2] = (uint8_t)((bx ^ by) * 5u + 1u);
            d[3] = 0xff;
        }
    }
}
static uint8_t *block_pixel(uint8_t *p, unsigned w, unsigned x, unsigned y, unsigned block) {
    return p + ((size_t)(y / block) * w + (x / block)) * 4;
}
static D9Handle make_target(D9Device *d, unsigned w, unsigned h, int lockable) {
    D9Handle out = 0;
    assert(d9_create_render_target(d, w, h, D9_A8R8G8B8, lockable, &out) == D9_OK);
    return out;
}

int main(void) {
    set_cap_env(NULL);
    set_readback_env(NULL);
    d9_null_reset();
    D9Device *off = d9_create_device(d9_null_backend(), d9_null_context(), 32u << 20);
    assert(off);
    assert(!d9_scaled_readback_enabled(off));

    /* Default-off: read/write remain named lossy refusals. */
    D9Handle bounded_off = make_target(off, 1024, 1024, 0);
    uint8_t row[1024 * 4];
    assert(d9_render_target_read(off, bounded_off, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(off), "lossy") != NULL);
    assert(strstr(d9_last_draw_diagnostic(off), "TESV_D3D9_SCALED_READBACK=1") != NULL);
    assert(d9_render_target_write(off, bounded_off, row, sizeof row, 0, 0, 1024, 1)
           == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(off), "lossy") != NULL);

    /* Default-off also refuses a lockable >512 target at creation. */
    D9Handle refused = 0;
    assert(d9_create_render_target(off, 1024, 1024, D9_A8R8G8B8, 1, &refused)
           == D9_UNSUPPORTED);
    assert(refused == 0);
    assert(strstr(d9_last_draw_diagnostic(off), "SCALED_READBACK=1") != NULL);
    assert(d9_destroy_device_ex(off) == 0);

    /* Enabled: lockable 1024 target; 256-native pixels become 4x4 logical
     * blocks on read, and write-back keeps the native-centre sample. */
    set_readback_env("1");
    d9_null_reset();
    D9Device *on = d9_create_device(d9_null_backend(), d9_null_context(), 32u << 20);
    assert(on && d9_scaled_readback_enabled(on));
    D9Handle bounded = make_target(on, 1024, 1024, 1);
    unsigned w = 0, h = 0, format = 0, lockable = 0, sw = 0, sh = 0;
    size_t bytes = 0;
    assert(d9_render_target_info(on, bounded, &w, &h, &format, &lockable,
                                 &bytes) == D9_OK);
    assert(w == 1024 && h == 1024 && lockable == 1);
    assert(d9_render_target_storage_info(on, bounded, &sw, &sh) == D9_OK);
    assert(sw == 256 && sh == 256);

    /* Fill a normal 256 target and blit it into the bounded target. */
    static uint8_t native_pattern[256 * 256 * 4];
    static uint8_t logical_pattern[1024 * 1024 * 4];
    static uint8_t readback[1024 * 1024 * 4];
    fill_blocks(native_pattern, 256, 256, 1);
    D9Handle normal = make_target(on, 256, 256, 1);
    assert(d9_render_target_write(on, normal, native_pattern, 256u * 4,
                                  0, 0, 256, 256) == D9_OK);
    assert(d9_blit(on, bounded, NULL, normal, NULL, D9_FILTER_NONE) == D9_OK);

    assert(d9_render_target_read(on, bounded, readback, 1024u * 4,
                                 0, 0, 1024, 1024) == D9_OK);
    for (unsigned y = 0; y < 1024; ++y) {
        for (unsigned x = 0; x < 1024; ++x) {
            const uint8_t *want = block_pixel(native_pattern, 256, x / 4, y / 4, 1);
            const uint8_t *got = readback + ((size_t)y * 1024 + x) * 4;
            assert(memcmp(want, got, 4) == 0);
        }
    }

    /* Logical 4x4 block pattern -> one native pixel per block -> read back. */
    fill_blocks(logical_pattern, 1024, 1024, 4);
    assert(d9_render_target_write(on, bounded, logical_pattern, 1024u * 4,
                                  0, 0, 1024, 1024) == D9_OK);
    memset(readback, 0, sizeof readback);
    assert(d9_render_target_read(on, bounded, readback, 1024u * 4,
                                 0, 0, 1024, 1024) == D9_OK);
    for (unsigned y = 0; y < 1024; ++y) {
        for (unsigned x = 0; x < 1024; ++x) {
            const uint8_t *want = logical_pattern + ((size_t)y * 1024 + x) * 4;
            const uint8_t *got = readback + ((size_t)y * 1024 + x) * 4;
            assert(memcmp(want, got, 4) == 0);
        }
    }

    /* Retained back buffer keeps its 512x256 unscaled read path. */
    assert(d9_set_back_buffer_info_ex(on, 400, 240, D9_A8R8G8B8, 1) == D9_OK);
    D9Handle back = d9_back_buffer_target(on);
    assert(back);
    assert(d9_render_target_storage_info(on, back, &sw, &sh) == D9_OK);
    assert(sw == 512 && sh == 256);
    assert(d9_render_target_read(on, back, row, 400u * 4, 0, 0, 400, 1) == D9_OK);

    assert(d9_destroy_device_ex(on) == 0);

    set_readback_env(NULL);
    printf("scaled readback: default-off lossy refusal, 4x4 nearest upscale/downscale, back buffer unscaled PASS\n");
    return 0;
}
