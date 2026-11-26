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

static int map_source(unsigned src, GPU_TEVSRC *out) {
    switch (src) {
    case D9TEV_SRC_TEX0: *out = GPU_TEXTURE0; return 0;
    case D9TEV_SRC_TEX1: *out = GPU_TEXTURE1; return 0;
    case D9TEV_SRC_PRIMARY: *out = GPU_PRIMARY_COLOR; return 0;
    case D9TEV_SRC_CONST: *out = GPU_CONSTANT; return 0;
    case D9TEV_SRC_PREV: *out = GPU_PREVIOUS; return 0;
    default: return -1;
    }
}

int d9_pica_citro3d_apply_tev_ex(const D9TevProgram *program,
                                 C3D_Tex *texture0, C3D_Tex *texture1,
                                 const float (*constants)[4],
                                 unsigned constant_count,
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
    if (plan.texture_count >= 1 && !texture0) {
        if (why && why_size)
            snprintf(why, why_size, "TEV program samples texture unit 0 but no texture is bound");
        return -1;
    }
    if (plan.texture_count >= 1 && C3D_TexGetType(texture0) != GPU_TEX_2D) {
        if (why && why_size)
            snprintf(why, why_size, "TEV texture unit 0 is not a 2D texture");
        return -1;
    }
    if (plan.texture_count >= 2 && !texture1) {
        if (why && why_size)
            snprintf(why, why_size, "TEV program samples texture unit 1 but s1 has no 2D texture");
        return -1;
    }
    if (plan.texture_count >= 2 && C3D_TexGetType(texture1) != GPU_TEX_2D) {
        if (why && why_size)
            snprintf(why, why_size, "TEV texture unit 1 is not a 2D texture");
        return -1;
    }

    for (unsigned i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) init_passthrough(env[i]);
    for (unsigned i = 0; i < plan.stage_count; ++i) {
        const D9PicaTevStage *stage = &plan.stage[i];
        GPU_TEVSRC rgb[3], alpha[3];
        for (unsigned k = 0; k < 3; ++k) {
            if (map_source(stage->src_rgb[k], &rgb[k]) ||
                map_source(stage->src_a[k], &alpha[k])) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "TEV stage %u source 0x%02x/0x%02x has no PICA mapping",
                             i, stage->src_rgb[k], stage->src_a[k]);
                return -1;
            }
        }
        C3D_TexEnvSrc(env[i], C3D_RGB, rgb[0], rgb[1], rgb[2]);
        C3D_TexEnvSrc(env[i], C3D_Alpha, alpha[0], alpha[1], alpha[2]);
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
        uint32_t color = 0xFFFFFFFFu;
        if (stage->uses_constant) {
            if (stage->const_is_literal) {
                color = stage->constant_abgr;
            } else {
                if (!constants || stage->const_reg >= constant_count) {
                    if (why && why_size)
                        snprintf(why, why_size,
                                 "TEV stage %u needs external constant c%u but the bank is absent/short",
                                 i, stage->const_reg);
                    return -1;
                }
                color = d9_pica_tev_color_abgr(d9_tev_pack_rgba8(constants[stage->const_reg]));
            }
        }
        C3D_TexEnvColor(env[i], color);
    }
    if (plan.texture_count >= 1) C3D_TexBind(0, texture0);
    if (plan.texture_count >= 2) C3D_TexBind(1, texture1);
    for (unsigned i = 0; i < D9_PICA_TEV_MAX_STAGES; ++i) C3D_DirtyTexEnv(env[i]);
    return 0;
}

int d9_pica_citro3d_apply_tev(const D9TevProgram *program, C3D_Tex *texture,
                              char *why, size_t why_size) {
    return d9_pica_citro3d_apply_tev_ex(program, texture, NULL, NULL, 0,
                                        why, why_size);
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
