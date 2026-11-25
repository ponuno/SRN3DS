/* Isolated ARM/Azahar probe for the PICA200 TEV adapter.
 *
 * Scope: citro3d state programming only.  It does not draw a frame, read back
 * pixels, run Skyrim, or prove visual equivalence.  It verifies that the helper
 * links against ARM citro3d, copies a validated tint into C3D_TexEnv, converts
 * the literal to ABGR8888, initialises unused stages, accepts a zero-texture
 * literal, rejects a missing texture, and restores deterministic fixed-function
 * TEV state.
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_pica_citro3d.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void tint_program(D9TevProgram *p) {
    memset(p, 0, sizeof *p);
    p->version = 0xFFFF0300u;
    p->instructions = 2;
    p->stage_limit = D9_PICA_TEV_MAX_STAGES;
    p->stages = 1;
    p->textures = 1;
    p->texture[0].texcoord_reg = 0;
    p->texture[0].sampler_reg = 0;
    p->texture[0].sampler_type = 2;
    p->inputs = 1;
    p->input[0].usage = D9TEV_USAGE_TEXCOORD;
    p->input[0].reg_index = 0;
    p->input[0].tev_source = 0xFF;
    /* The translator counts literal def c# registers in this table too. */
    p->constants = 1;
    p->constant_reg[0] = 0;
    D9TevStage *s = &p->stage[0];
    s->func_rgb = s->func_a = D9TEV_FUNC_MODULATE;
    s->src_rgb[0] = s->src_a[0] = D9TEV_SRC_TEX0;
    s->src_rgb[1] = s->src_a[1] = D9TEV_SRC_CONST;
    s->src_rgb[2] = s->src_a[2] = D9TEV_SRC_PREV;
    for (unsigned i = 0; i < 3; ++i) {
        s->op_rgb[i] = D9TEV_RGB_COLOR;
        s->op_a[i] = D9TEV_A_ALPHA;
    }
    s->scale_rgb = s->scale_a = D9TEV_SCALE_1;
    s->const_reg = 0;
    s->const_is_literal = 1;
    s->const_rgba8 = 0xFFC08040u; /* 0.25, 0.5, 0.75, 1.0 */
}

static void check_passthrough(unsigned stage) {
    C3D_TexEnv *env = C3D_GetTexEnv((int)stage);
    check(env != NULL, "TEV stage exists");
    if (!env) return;
    check(env->srcRgb == GPU_TEVSOURCES(GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS),
          "unused stage RGB source is PREVIOUS");
    check(env->srcAlpha == env->srcRgb, "unused stage alpha source is PREVIOUS");
    check(env->funcRgb == GPU_REPLACE && env->funcAlpha == GPU_REPLACE,
          "unused stage is REPLACE");
    check(env->color == 0xFFFFFFFFu, "unused stage constant is white");
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    printf("pica TEV ARM: citro3d state probe (no draw)\n");

    int gpu_ready = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) != 0;
    check(gpu_ready, "C3D_Init");
    C3D_Tex texture;
    int texture_ready = 0;
    if (gpu_ready) {
        texture_ready = C3D_TexInit(&texture, 8, 8, GPU_RGBA8);
        check(texture_ready, "2D texture init");
    }

    if (gpu_ready && texture_ready) {
        D9TevProgram program;
        tint_program(&program);
        char why[192];
        check(d9_pica_citro3d_apply_tev(&program, &texture, why, sizeof why) == 0,
              why);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        check(env != NULL, "stage 0 exists");
        if (env) {
            check(env->srcRgb == GPU_TEVSOURCES(GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS),
                  "tint RGB sources copied");
            check(env->srcAlpha == env->srcRgb, "tint alpha sources copied");
            check(env->opRgb == GPU_TEVOPERANDS(GPU_TEVOP_RGB_SRC_COLOR,
                                                GPU_TEVOP_RGB_SRC_COLOR,
                                                GPU_TEVOP_RGB_SRC_COLOR),
                  "tint RGB operands copied");
            check(env->opAlpha == GPU_TEVOPERANDS(GPU_TEVOP_A_SRC_ALPHA,
                                                  GPU_TEVOP_A_SRC_ALPHA,
                                                  GPU_TEVOP_A_SRC_ALPHA),
                  "tint alpha operands copied");
            check(env->funcRgb == GPU_MODULATE && env->funcAlpha == GPU_MODULATE,
                  "tint combine functions copied");
            check(env->scaleRgb == GPU_TEVSCALE_1 && env->scaleAlpha == GPU_TEVSCALE_1,
                  "tint scales copied");
            check(env->color == 0x4080C0FFu, "literal converted to ABGR8888");
        }
        for (unsigned i = 1; i < D9_PICA_TEV_MAX_STAGES; ++i) check_passthrough(i);

        check(d9_pica_citro3d_apply_tev(&program, NULL, why, sizeof why) < 0,
              "missing texture rejected");
        check(strstr(why, "no texture") != NULL, "missing texture named");

        /* Zero-texture literal programs must not require a native texture. */
        program.textures = 0;
        program.inputs = 0;
        program.stage[0].func_rgb = program.stage[0].func_a = D9TEV_FUNC_REPLACE;
        program.stage[0].src_rgb[0] = program.stage[0].src_a[0] = D9TEV_SRC_CONST;
        program.stage[0].src_rgb[1] = program.stage[0].src_a[1] = D9TEV_SRC_PREV;
        program.stage[0].src_rgb[2] = program.stage[0].src_a[2] = D9TEV_SRC_PREV;
        check(d9_pica_citro3d_apply_tev(&program, NULL, why, sizeof why) == 0,
              "zero-texture literal accepted");
        env = C3D_GetTexEnv(0);
        check(env && env->srcRgb == GPU_TEVSOURCES(GPU_CONSTANT,
                                                   GPU_PREVIOUS,
                                                   GPU_PREVIOUS),
              "zero-texture literal uses CONSTANT");

        d9_pica_citro3d_restore_tev();
        env = C3D_GetTexEnv(0);
        check(env && env->srcRgb == GPU_TEVSOURCES(GPU_PRIMARY_COLOR,
                                                   GPU_PRIMARY_COLOR,
                                                   GPU_PRIMARY_COLOR),
              "restore stage 0 uses PRIMARY");
        for (unsigned i = 1; i < D9_PICA_TEV_MAX_STAGES; ++i) check_passthrough(i);
    }

    if (texture_ready) C3D_TexDelete(&texture);
    if (gpu_ready) C3D_Fini();

    printf("pica TEV ARM: %d checks, %d failures\n", checks, failures);
    FILE *log = fopen("sdmc:/Skyrim_N3DS.log", "w");
    if (log) {
        fprintf(log, "pica_tev_arm: %s (%d checks, errors=%d; no draw)\n",
                failures ? "FAIL" : "PASS", checks, failures);
        fclose(log);
    }
    gspWaitForVBlank();
    svcSleepThread(1000000000LL);
    gfxExit();
    return failures ? 1 : 0;
}
