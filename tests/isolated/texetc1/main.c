/* Isolated ARM/Azahar check for the ETC1 texture path and residency cache.
 *
 * Build (from this directory, devkitPro MSYS2 shell):
 *   make
 * Run:
 *   powershell -File tools/run_azahar.ps1 `
 *       -ThreeDsx tests/isolated/texetc1/texetc1.3dsx -Name texetc1 -TimeoutSec 12
 *
 * Evidence scope: this verifies that citro3d accepts GPU_ETC1/GPU_ETC1A4,
 * that a PICA upload of the block layout produced by the host pipeline
 * succeeds, that a frame clear completes, and that the residency accounting
 * contract holds on ARM.  It does not read back sampled pixels, so it is not a
 * visual/colour-equivalence test.
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_texcache.h"
#include "d3d9_texture_data.h"

static int failures;
static int checks;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

/* Four identical 8x8 ETC1 blocks (solid red), as emitted by tex3ds -r for the
 * host pipeline's 8x8 red fixture.  This is derived test data, not game data. */
static const u8 red_etc1[32] = {
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
};

static const u8 red_etc1a4[64] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
};

typedef struct {
    int guest_releases;
    int gpu_releases;
} Fake;

static int release_guest(void *ctx, void *guest, size_t bytes) {
    Fake *fake = ctx;
    (void)guest;
    (void)bytes;
    ++fake->guest_releases;
    return 0;
}

static int release_gpu(void *ctx, void *gpu, size_t bytes) {
    Fake *fake = ctx;
    (void)gpu;
    (void)bytes;
    ++fake->gpu_releases;
    return 0;
}

static void residency_check(void) {
    D9TexCache cache;
    D9TexEntry entries[8];
    Fake fake = {0, 0};
    D9TexCacheOps ops = {&fake, release_guest, release_gpu};
    int marker = 0, gpu_marker = 0;
    check(d9_texcache_init(&cache, entries, 8, &ops, 0, 0) == D9TEXCACHE_OK,
          "cache init");
    check(d9_texcache_add_guest(&cache, 1, &marker, 1024, 1) ==
          D9TEXCACHE_OK, "cache guest");
    check(d9_texcache_begin_upload(&cache, 1) == D9TEXCACHE_OK, "cache begin");
    check(d9_texcache_commit_upload(&cache, 1, &gpu_marker, 2048, 1) ==
          D9TEXCACHE_OK, "cache commit");
    D9TexCacheStats stats;
    d9_texcache_stats(&cache, &stats);
    check(stats.guest_bytes == 0, "guest dropped after upload");
    check(stats.gpu_bytes == 2048, "gpu bytes accounted");
    check(stats.peak_both == 3072, "peak coexistence captured");
    check(fake.guest_releases == 1, "guest release callback");
    d9_texcache_destroy(&cache);
}

static C3D_RenderTarget *target;
static C3D_Tex etc1_tex;
static C3D_Tex etc1a4_tex;
static int gpu_ready;

static int gpu_setup(void) {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return 0;
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!target) return 0;
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    if (!C3D_TexInit(&etc1_tex, 8, 8, GPU_ETC1)) return 0;
    if (!C3D_TexInit(&etc1a4_tex, 8, 8, GPU_ETC1A4)) return 0;
    C3D_TexLoadImage(&etc1_tex, red_etc1, GPU_TEXFACE_2D, 0);
    C3D_TexLoadImage(&etc1a4_tex, red_etc1a4, GPU_TEXFACE_2D, 0);
    C3D_TexFlush(&etc1_tex);
    C3D_TexFlush(&etc1a4_tex);
    gpu_ready = 1;
    return 1;
}

static void gpu_frame(void) {
    if (!gpu_ready) return;
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0xFF800000u, 0);
    C3D_FrameDrawOn(target);
    C3D_FrameEnd(0);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    printf("texetc1 isolated: ETC1 upload + residency accounting\n");

    residency_check();
    check(d9_sktx_crc32("123456789", 9) == 0xCBF43926u,
          "SKTX parser linked on ARM");
    check(gpu_setup(), "citro3d ETC1/ETC1A4 init and upload");
    for (int i = 0; i < 3; ++i) gpu_frame();
    check(gpu_ready, "frame clear completed");

    printf("texetc1: %d checks, %d failures\n", checks, failures);
    FILE *log = fopen("sdmc:/Skyrim_N3DS.log", "w");
    if (log) {
        fprintf(log, "texetc1: %s (%d checks, %d failures)\n",
                failures ? "FAIL" : "PASS", checks, failures);
        fprintf(log, "gpu_ready=%d guest_release_contract=checked\n", gpu_ready);
        fclose(log);
    }
    if (gpu_ready) {
        C3D_TexDelete(&etc1_tex);
        C3D_TexDelete(&etc1a4_tex);
        C3D_RenderTargetDelete(target);
        C3D_Fini();
    }
    gspWaitForVBlank();
    svcSleepThread(1000000000LL);
    gfxExit();
    return failures ? 1 : 0;
}
