/* Isolated ARM state probe for fixed-function vertex declarations and the
 * citro3d sampler path.  No draw is submitted and no pixel/frame result is
 * claimed.  The implementation is included so the probe can inspect the real
 * static configure_native_texture() helper without widening the public API. */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_citro3d.c"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void check_declarations(void) {
    D9Decl decl;
    D9VertexElement supported[] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {1, 0, D9_DECL_FLOAT16_2, 0, D9_USAGE_TEXCOORD, 0},
    };
    check(d9_decl_init(&decl, supported, 2) == 0, "position+UV declaration accepted");
    check(decl.position == 0 && decl.uv == 1, "position+UV elements retained");

    D9VertexElement color[] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_COLOR, 0, D9_USAGE_COLOR, 0},
    };
    check(d9_decl_init(&decl, color, 2) < 0, "packed colour declaration rejected");
    color[1].type = D9_DECL_FLOAT4;
    check(d9_decl_init(&decl, color, 2) < 0, "float colour declaration rejected");
    color[1].usage_index = 1;
    check(d9_decl_init(&decl, color, 2) < 0, "secondary colour declaration rejected");
}

static void check_sampler(void) {
    C3D_TexInitParams params = {64, 64, 3, GPU_RGBA8, GPU_TEX_2D, false};
    C3D_Tex texture;
    int ready = C3D_TexInitWithParams(&texture, NULL, params);
    check(ready, "mipmapped citro3d texture init");
    if (!ready) return;

    NativeTex native = {0};
    native.tex = texture;
    native.levels = 4;
    D9DrawState state = {
        .resident_mips = 4,
        .max_mip = 1,
        .mip_filter = 0,
        .mag_filter = 2,
        .min_filter = 2,
        .address_u = 3,
        .address_v = 3,
    };

    configure_native_texture(&native, &state);
    check(native.tex.minLevel == 1 && native.tex.maxLevel == 3,
          "MIPFILTER_NONE keeps MAXMIPLEVEL..resident range");
    check((native.tex.param & GPU_TEXTURE_MIP_FILTER(GPU_LINEAR)) == 0,
          "MIPFILTER_NONE disables between-mip interpolation");
    check((native.tex.param & GPU_TEXTURE_MAG_FILTER(GPU_LINEAR)) != 0,
          "MIPFILTER_NONE preserves magnification filtering");
    check((native.tex.param & GPU_TEXTURE_MIN_FILTER(GPU_LINEAR)) != 0,
          "MIPFILTER_NONE preserves minification filtering");

    state.mip_filter = 1;
    configure_native_texture(&native, &state);
    check(native.tex.minLevel == 1 && native.tex.maxLevel == 3,
          "MIPFILTER_POINT keeps resident range");
    check((native.tex.param & GPU_TEXTURE_MIP_FILTER(GPU_LINEAR)) == 0,
          "MIPFILTER_POINT selects nearest mip");

    state.mip_filter = 2;
    configure_native_texture(&native, &state);
    check(native.tex.minLevel == 1 && native.tex.maxLevel == 3,
          "MIPFILTER_LINEAR keeps resident range");
    check((native.tex.param & GPU_TEXTURE_MIP_FILTER(GPU_LINEAR)) != 0,
          "MIPFILTER_LINEAR enables between-mip interpolation");

    state.max_mip = 10;
    configure_native_texture(&native, &state);
    check(native.tex.minLevel == 3 && native.tex.maxLevel == 3,
          "MAXMIPLEVEL clamps to the resident range");

    C3D_TexDelete(&native.tex);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    printf("vertex/citro3d ARM: state probe (no draw)\n");

    check_declarations();
    check_sampler();

    printf("vertex/citro3d ARM: %d checks, %d failures\n", checks, failures);
    FILE *log = fopen("sdmc:/Skyrim_N3DS.log", "w");
    if (log) {
        fprintf(log, "vertex_citro3d_arm: %s (%d checks, errors=%d; no draw)\n",
                failures ? "FAIL" : "PASS", checks, failures);
        fclose(log);
    }
    gspWaitForVBlank();
    svcSleepThread(1000000000LL);
    gfxExit();
    return failures ? 1 : 0;
}
