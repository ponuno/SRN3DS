#include "d3d9_pica_citro3d.h"

#include <stdio.h>

static void init_passthrough(C3D_TexEnv *env) {
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA,
                      GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvScale(env, C3D_Both, GPU_TEVSCALE_1);
    C3D_TexEnvColor(env, 0xFFFFFFFFu);
}

int d9_pica_citro3d_apply_tev(const D9TevProgram *program, C3D_Tex *texture,
                              char *why, size_t why_size) {
    D9PicaTevProgram plan;
    if (d9_pica_tev_validate(program, &plan, NULL, why, why_size)) return -1;

    C3D_TexEnv *env[D9_PICA_TEV_MAX_STAGES];
    for (unsigned i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) {
        env[i] = C3D_GetTexEnv((int)i);
        if (!env[i]) {
            if (why && why_size)
                snprintf(why, why_size, "C3D context is not active for TEV stage %u", i);
            return -1;
        }
    }
    if (plan.texture_count && !texture) {
        if (why && why_size)
            snprintf(why, why_size, "TEV program samples texture unit 0 but no texture is bound");
        return -1;
    }
    if (plan.texture_count && C3D_TexGetType(texture) != GPU_TEX_2D) {
        if (why && why_size)
            snprintf(why, why_size, "TEV texture unit 0 is not a 2D texture");
        return -1;
    }

    for (unsigned i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) init_passthrough(env[i]);
    for (unsigned i = 0; i < plan.stage_count; ++i) {
        const D9PicaTevStage *stage = &plan.stage[i];
        C3D_TexEnvSrc(env[i], C3D_RGB,
                      (GPU_TEVSRC)stage->src_rgb[0],
                      (GPU_TEVSRC)stage->src_rgb[1],
                      (GPU_TEVSRC)stage->src_rgb[2]);
        C3D_TexEnvSrc(env[i], C3D_Alpha,
                      (GPU_TEVSRC)stage->src_a[0],
                      (GPU_TEVSRC)stage->src_a[1],
                      (GPU_TEVSRC)stage->src_a[2]);
        C3D_TexEnvOpRgb(env[i], (GPU_TEVOP_RGB)stage->op_rgb[0],
                        (GPU_TEVOP_RGB)stage->op_rgb[1],
                        (GPU_TEVOP_RGB)stage->op_rgb[2]);
        C3D_TexEnvOpAlpha(env[i], (GPU_TEVOP_A)stage->op_a[0],
                          (GPU_TEVOP_A)stage->op_a[1],
                          (GPU_TEVOP_A)stage->op_a[2]);
        C3D_TexEnvFunc(env[i], C3D_RGB, (GPU_COMBINEFUNC)stage->func_rgb);
        C3D_TexEnvFunc(env[i], C3D_Alpha, (GPU_COMBINEFUNC)stage->func_a);
        C3D_TexEnvScale(env[i], C3D_RGB, (GPU_TEVSCALE)stage->scale_rgb);
        C3D_TexEnvScale(env[i], C3D_Alpha, (GPU_TEVSCALE)stage->scale_a);
        C3D_TexEnvColor(env[i], stage->constant_abgr);
    }
    if (plan.texture_count) C3D_TexBind(0, texture);
    for (unsigned i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) C3D_DirtyTexEnv(env[i]);
    return 0;
}

void d9_pica_citro3d_restore_tev(void) {
    for (int i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        if (!env) return;
        init_passthrough(env);
        C3D_DirtyTexEnv(env);
    }
    C3D_TexEnv *first = C3D_GetTexEnv(0);
    if (!first) return;
    C3D_TexEnvSrc(first, C3D_Both, GPU_PRIMARY_COLOR,
                  GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_DirtyTexEnv(first);
}
