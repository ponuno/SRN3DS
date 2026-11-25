#include "d3d9_color_tev.h"

static int source_of(unsigned arg, int has_texture, unsigned *out,
                     const char **why) {
    switch (arg) {
    case D9_TA_DIFFUSE:
        *out = D9_CTEV_SRC_PRIMARY;
        return D9_OK;
    case D9_TA_CURRENT:
        /* Stage 0 CURRENT is the diffuse colour in D3D9, not a previous TEV
         * result.  There is no earlier stage to read. */
        *out = D9_CTEV_SRC_CURRENT;
        return D9_OK;
    case D9_TA_TEXTURE:
        if (!has_texture) {
            if (why) *why = "stage 0 sources TEXTURE but no texture is bound";
            return D9_UNSUPPORTED;
        }
        *out = D9_CTEV_SRC_TEXTURE;
        return D9_OK;
    default:
        if (why) *why = "stage 0 argument is outside DIFFUSE/TEXTURE/CURRENT";
        return D9_UNSUPPORTED;
    }
}

/* Validate the operation first so a DISABLE rejection is reported as such even
 * when one of its arguments is also unexpressible. */
static int op_of(unsigned op, unsigned arg1, unsigned arg2, int has_texture,
                 unsigned *func, unsigned src[3], const char **why) {
    unsigned a = 0, b = 0;
    if (op != D9_TOP_SELECTARG1 && op != D9_TOP_SELECTARG2 &&
        op != D9_TOP_MODULATE) {
        if (why) *why = op == D9_TOP_DISABLE
            ? "DISABLE on stage 0 has no single-TEV-stage equivalent"
            : "stage 0 operation is outside SELECTARG1/SELECTARG2/MODULATE";
        return D9_UNSUPPORTED;
    }
    if (op == D9_TOP_SELECTARG1) {
        /* ARG2 is not an input of SELECTARG1, so an unexpressible ARG2 must
         * not reject an otherwise drawable stage. */
        if (source_of(arg1, has_texture, &a, why)) return D9_UNSUPPORTED;
        *func = D9_CTEV_REPLACE;
        src[0] = src[1] = src[2] = a;
    } else if (op == D9_TOP_SELECTARG2) {
        if (source_of(arg2, has_texture, &b, why)) return D9_UNSUPPORTED;
        *func = D9_CTEV_REPLACE;
        src[0] = src[1] = src[2] = b;
    } else {
        if (source_of(arg1, has_texture, &a, why)) return D9_UNSUPPORTED;
        if (source_of(arg2, has_texture, &b, why)) return D9_UNSUPPORTED;
        /* MODULATE(a,b): the third operand is unused by GPU_MODULATE. */
        *func = D9_CTEV_MODULATE;
        src[0] = a;
        src[1] = b;
        src[2] = b;
    }
    return D9_OK;
}

int d9_color_tev_stage0(const D9DrawState *state, int has_texture,
                        D9ColorTevStage *out, const char **why) {
    if (why) *why = NULL;
    if (!state || !out) {
        if (why) *why = "null draw state or TEV plan";
        return D9_INVALID;
    }
    if (state->stage_texcoordindex != 0) {
        /* The colour vertex shader writes texcoord0 only. */
        if (why) *why = "colour draw requires stage 0 TEXCOORDINDEX 0";
        return D9_UNSUPPORTED;
    }
    if (op_of(state->stage_colorop, state->stage_colorarg1,
              state->stage_colorarg2, has_texture, &out->color_func,
              out->color_src, why))
        return D9_UNSUPPORTED;
    if (op_of(state->stage_alphaop, state->stage_alphaarg1,
              state->stage_alphaarg2, has_texture, &out->alpha_func,
              out->alpha_src, why))
        return D9_UNSUPPORTED;
    return D9_OK;
}
