/* Runtime selection for the certified first-menu TEV programs.
 *
 * See d3d9_menu_shader_runtime.h for the contract.  This file has no D3D9
 * device dependency: it consumes the self-contained closure walker and the
 * host-side planners, so the same decisions can be unit-tested without a
 * backend and reused on the 3DS.
 */
#include "d3d9_menu_shader_runtime.h"

#include "d3d9_menu_closure.h"
#include "d3d9_menu_light.h"
#include "d3d9_shader.h"

#include <stdio.h>
#include <string.h>

static uint32_t g_preconditions;

static void zero_match(D9MenuShaderMatch *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->entry = D9_MENU_ENTRY_NONE;
}

static void set_reason(char *why, size_t why_size, const char *text) {
    if (why && why_size) snprintf(why, why_size, "%s", text);
}

static int fail(D9MenuShaderMatch *out, char *why, size_t why_size,
                const char *text) {
    if (why && why_size) snprintf(why, why_size, "%s", text);
    if (out) zero_match(out);
    return -1;
}

static void fill_identity(D9MenuShaderMatch *out, int kind, int entry,
                          uint32_t required) {
    out->kind = kind;
    out->entry = entry;
    out->required_preconditions = required;
    snprintf(out->id, sizeof out->id, "%s", d9_menu_closure_entry_id(entry));
}

/* --------------------------------------------------------------------- */
/* Plan builders                                                         */
/* --------------------------------------------------------------------- */

/* The exact certified copy must translate to one REPLACE(TEXTURE0) stage.
 * A generic blit that happens to share the opcode shape is not accepted here;
 * the closure hash has already established the exact bytes. */
static int build_copy(const uint32_t *code, size_t words, D9TevProgram *out,
                      char *why, size_t why_size) {
    char translation[192];
    if (d9_tev_translate_pixel(code, words, out, translation, sizeof translation) < 0) {
        if (why && why_size)
            snprintf(why, why_size, "menu.composite.ps: TEV translation rejected: %s",
                     translation);
        return -1;
    }
    if (out->stages != 1u || out->textures != 1u || out->inputs != 1u ||
        out->constants != 0u || out->uses_vertex_color) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.composite.ps: plan is not the exact copy (%u stages, %u textures, %u inputs, %u constants, vertex_color=%u)",
                     out->stages, out->textures, out->inputs, out->constants,
                     out->uses_vertex_color);
        return -1;
    }
    const D9TevStage *stage = &out->stage[0];
    if (stage->func_rgb != D9TEV_FUNC_REPLACE || stage->func_a != D9TEV_FUNC_REPLACE ||
        stage->src_rgb[0] != D9TEV_SRC_TEX0 || stage->src_a[0] != D9TEV_SRC_TEX0) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.composite.ps: stage 0 is not REPLACE(TEXTURE0)");
        return -1;
    }
    if (out->texture[0].sampler_type != 2u || out->texture[0].sampler_reg != 0u ||
        out->texture[0].texcoord_reg != 0u) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.composite.ps: copy must use 2D sampler s0 and texcoord v0");
        return -1;
    }
    return 0;
}

/* The logo closure entry is p1200005.  The menu-light matcher also accepts
 * p5, so the signature is checked explicitly after the exact hash. */
static int build_logo(const uint32_t *code, size_t words, D9TevProgram *out,
                      char *why, size_t why_size) {
    D9MenuLightMatch match;
    char light_why[192];
    if (!d9_menu_light_match(code, words, &match, light_why, sizeof light_why)) {
        if (why && why_size)
            snprintf(why, why_size, "menu.logo.ps: %s", light_why);
        return -1;
    }
    if (match.signature != D9_MENU_LIGHT_P1200005_ADD_NORMAL) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.logo.ps: signature is %s, expected p1200005 ADD normal",
                     d9_menu_light_signature_name(match.signature));
        return -1;
    }
    if (d9_menu_light_plan(&match, out, light_why, sizeof light_why) < 0) {
        if (why && why_size)
            snprintf(why, why_size, "menu.logo.ps: %s", light_why);
        return -1;
    }
    if (out->stages != 5u || out->textures != 2u ||
        out->constant_reg[0] != 3u || out->constant_reg[1] != 4u ||
        out->constant_reg[2] != 7u) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.logo.ps: unexpected pre-baked plan shape (%u stages, %u textures)",
                     out->stages, out->textures);
        return -1;
    }
    return 0;
}

static void fixed_header(D9TevProgram *out) {
    memset(out, 0, sizeof *out);
    out->version = 0xFFFF0300u;
    out->stage_limit = D9TEV_MAX_STAGES;
    out->backend_flags = D9TEV_PLAN_MENU_FIXED;
}

/* menu.fog.fixed has no shipped .fxp.  The closure contract is two texture
 * units, vertex colour, straight alpha, no depth write and at most three TEV
 * stages.  This plan is the minimal contract form: effect texture * vertex
 * colour, then a second gradient/mask texture.  It is not a bytecode match. */
static int build_fog(D9TevProgram *out, char *why, size_t why_size) {
    (void)why;
    (void)why_size;
    fixed_header(out);
    out->instructions = 2u;
    out->textures = 2u;
    out->texture[0].texcoord_reg = 0u;
    out->texture[0].sampler_reg = 0u;
    out->texture[0].sampler_type = 2u;
    out->texture[1].texcoord_reg = 0u;
    out->texture[1].sampler_reg = 1u;
    out->texture[1].sampler_type = 2u;
    out->inputs = 1u;
    out->input[0].usage = D9TEV_USAGE_COLOR;
    out->input[0].usage_index = 0u;
    out->input[0].reg_index = 1u;
    out->input[0].tev_source = D9TEV_SRC_PRIMARY;
    out->uses_vertex_color = 1u;

    D9TevStage *stage = &out->stage[out->stages++];
    stage->func_rgb = stage->func_a = D9TEV_FUNC_MODULATE;
    stage->src_rgb[0] = stage->src_a[0] = D9TEV_SRC_TEX0;
    stage->op_rgb[0] = D9TEV_RGB_COLOR;
    stage->op_a[0] = D9TEV_A_ALPHA;
    stage->src_rgb[1] = stage->src_a[1] = D9TEV_SRC_PRIMARY;
    stage->op_rgb[1] = D9TEV_RGB_COLOR;
    stage->op_a[1] = D9TEV_A_ALPHA;
    stage->const_reg = 0xFFu;

    stage = &out->stage[out->stages++];
    stage->func_rgb = stage->func_a = D9TEV_FUNC_MODULATE;
    stage->src_rgb[0] = stage->src_a[0] = D9TEV_SRC_TEX1;
    stage->op_rgb[0] = D9TEV_RGB_COLOR;
    stage->op_a[0] = D9TEV_A_ALPHA;
    stage->src_rgb[1] = stage->src_a[1] = D9TEV_SRC_PREV;
    stage->op_rgb[1] = D9TEV_RGB_COLOR;
    stage->op_a[1] = D9TEV_A_ALPHA;
    stage->const_reg = 0xFFu;
    return 0;
}

/* Fixed texture * vertex-colour tint, one stage, one 2D texture. */
static int build_tint(D9TevProgram *out, char *why, size_t why_size) {
    (void)why;
    (void)why_size;
    fixed_header(out);
    out->instructions = 1u;
    out->textures = 1u;
    out->texture[0].texcoord_reg = 0u;
    out->texture[0].sampler_reg = 0u;
    out->texture[0].sampler_type = 2u;
    out->inputs = 1u;
    out->input[0].usage = D9TEV_USAGE_COLOR;
    out->input[0].usage_index = 0u;
    out->input[0].reg_index = 1u;
    out->input[0].tev_source = D9TEV_SRC_PRIMARY;
    out->uses_vertex_color = 1u;

    D9TevStage *stage = &out->stage[out->stages++];
    stage->func_rgb = stage->func_a = D9TEV_FUNC_MODULATE;
    stage->src_rgb[0] = stage->src_a[0] = D9TEV_SRC_TEX0;
    stage->op_rgb[0] = D9TEV_RGB_COLOR;
    stage->op_a[0] = D9TEV_A_ALPHA;
    stage->src_rgb[1] = stage->src_a[1] = D9TEV_SRC_PRIMARY;
    stage->op_rgb[1] = D9TEV_RGB_COLOR;
    stage->op_a[1] = D9TEV_A_ALPHA;
    stage->const_reg = 0xFFu;
    return 0;
}

/* --------------------------------------------------------------------- */
/* Vertex family                                                         */
/* --------------------------------------------------------------------- */

enum {
    D3DSIO_END = 0xFFFF,
    D3DSIO_COMMENT = 0xFFFE,
    D3DSIO_PHASE = 0xFFFD,
    D3DSIO_DCL = 0x1F,
    D3DSIO_TEXLD = 0x42
};

/* Measured static menu VS opcode set.  It excludes texture samples, branches,
 * loops, comparisons that need flow control, and the wide mask/MSAA paths. */
static int vs_opcode_allowed(unsigned op) {
    switch (op) {
    case 0x01: /* mov */
    case 0x02: /* add */
    case 0x04: /* mad */
    case 0x05: /* mul */
    case 0x06: /* rcp */
    case 0x07: /* rsq */
    case 0x08: /* dp3 */
    case 0x09: /* dp4 */
    case 0x0A: /* min */
    case 0x0B: /* max */
    case 0x0C: /* slt */
    case 0x0D: /* sge */
    case 0x13: /* frc */
    case 0x1F: /* dcl */
    case 0x20: /* pow */
    case 0x25: /* sincos */
    case 0x2E: /* mova */
    case 0x51: /* def */
        return 1;
    default:
        return 0;
    }
}

static int vs_input_allowed(unsigned usage, unsigned index) {
    if (usage == 0u && index == 0u) return 1;       /* position0 */
    if (usage == 5u && index <= 4u) return 1;       /* texcoord0..4 */
    if (usage == 10u && index == 0u) return 1;      /* color0 */
    return 0;
}

static const char *usage_name(unsigned usage) {
    switch (usage) {
    case 0: return "position";
    case 1: return "blendweight";
    case 2: return "blendindices";
    case 3: return "normal";
    case 5: return "texcoord";
    case 6: return "tangent";
    case 7: return "binormal";
    case 10: return "color";
    default: return "usage";
    }
}

static int add_vs_input(D9MenuShaderMatch *out, unsigned usage, unsigned index) {
    for (uint32_t i = 0; i < out->input_count; ++i)
        if (out->input_usage[i] == usage && out->input_index[i] == index)
            return 0;
    if (out->input_count >= D9_MENU_SHADER_MAX_INPUTS) return -1;
    out->input_usage[out->input_count] = (uint8_t)usage;
    out->input_index[out->input_count] = (uint8_t)index;
    ++out->input_count;
    return 0;
}

/* Walk a validated vs_3_0 stream and collect its declarations.  Framing and
 * instruction lengths are already checked by d9_shader_analyze(); this walk
 * only applies the menu-specific structural contract. */
static int walk_vs(const uint32_t *code, size_t words, D9MenuShaderMatch *out,
                   char *why, size_t why_size) {
    D9ShaderInfo info;
    size_t i;
    int end = 0;
    if (d9_shader_analyze(code, words, &info) == D9_SHADER_INVALID ||
        info.stream != D9_SHADER_STREAM_VALID || !info.saw_end) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: malformed vs token stream");
        return -1;
    }
    if (info.version != 0xFFFE0300u) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: version 0x%08x is not vs_3_0",
                     (unsigned)info.version);
        return -1;
    }
    if (!info.instructions || info.instructions > 96u) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.generic.vs: %u instructions is outside the static menu bound (1..96)",
                     info.instructions);
        return -1;
    }

    i = 1;
    while (i < words) {
        uint32_t token = code[i];
        unsigned op = token & 0xFFFFu;
        if (op == D3DSIO_END) { end = 1; break; }
        if (op == D3DSIO_COMMENT) {
            size_t skip = (size_t)((token >> 16) & 0x7FFFu);
            if (skip > words - i - 1u) {
                if (why && why_size)
                    snprintf(why, why_size, "menu.generic.vs: comment past the stream");
                return -1;
            }
            i += skip + 1u;
            continue;
        }
        if (op == D3DSIO_PHASE) {
            if (why && why_size)
                snprintf(why, why_size, "menu.generic.vs: phase token is not a VS3 declaration");
            return -1;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - i) {
            if (why && why_size)
                snprintf(why, why_size, "menu.generic.vs: instruction past the stream");
            return -1;
        }
        if (op == D3DSIO_TEXLD) {
            if (why && why_size)
                snprintf(why, why_size, "menu.generic.vs: texture sample is outside the static VS family");
            return -1;
        }
        if (!vs_opcode_allowed(op)) {
            if (why && why_size)
                snprintf(why, why_size,
                         "menu.generic.vs: opcode '0x%02x' is outside the measured static VS set",
                         op);
            return -1;
        }
        if (op == D3DSIO_DCL) {
            uint32_t decl, dst;
            unsigned usage, index, reg_type, reg_index;
            if (total != 3u) {
                if (why && why_size)
                    snprintf(why, why_size, "menu.generic.vs: dcl must carry two operands");
                return -1;
            }
            decl = code[i + 1];
            dst = code[i + 2];
            usage = decl & 0x1Fu;
            index = (decl >> 16) & 0xFu;
            reg_type = (dst >> 28) & 0x7u;
            reg_index = dst & 0x7FFu;
            if (reg_type == 1u) {
                if (!vs_input_allowed(usage, index)) {
                    if (why && why_size)
                        snprintf(why, why_size,
                                 "menu.generic.vs: input %s%u is outside the static menu family",
                                 usage_name(usage), index);
                    return -1;
                }
                if (reg_index > 15u && usage != 10u) {
                    if (why && why_size)
                        snprintf(why, why_size, "menu.generic.vs: input register v%u is too high",
                                 reg_index);
                    return -1;
                }
                if (add_vs_input(out, usage, index) < 0) {
                    if (why && why_size)
                        snprintf(why, why_size, "menu.generic.vs: too many input semantics");
                    return -1;
                }
            } else if (reg_type == 6u) {
                if (usage == 0u && index == 0u) out->output_semantic_mask |= D9_MENU_VS_OUT_POSITION0;
                else if (usage == 5u && index == 0u) out->output_semantic_mask |= D9_MENU_VS_OUT_TEXCOORD0;
                else if (usage == 10u && index == 0u) out->output_semantic_mask |= D9_MENU_VS_OUT_COLOR0;
                else if (usage == 10u && index == 1u) out->output_semantic_mask |= D9_MENU_VS_OUT_COLOR1;
            } else {
                if (why && why_size)
                    snprintf(why, why_size,
                             "menu.generic.vs: dcl register type %u is not an input or output",
                             reg_type);
                return -1;
            }
        }
        i += total;
    }
    if (!end) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: no END token");
        return -1;
    }
    if (!out->input_count) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: no input declarations");
        return -1;
    }
    if (!(out->output_semantic_mask & D9_MENU_VS_OUT_POSITION0)) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: missing position0 output declaration");
        return -1;
    }
    if (!(out->output_semantic_mask & D9_MENU_VS_OUT_TEXCOORD0)) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: missing texcoord0 output declaration");
        return -1;
    }
    if (!(out->output_semantic_mask & D9_MENU_VS_OUT_COLOR0)) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: missing color0 output declaration");
        return -1;
    }
    if (!(out->output_semantic_mask & D9_MENU_VS_OUT_COLOR1)) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: missing color1 output declaration");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------- */
/* Public selection                                                      */
/* --------------------------------------------------------------------- */

int d9_menu_shader_match_ps(const uint32_t *code, size_t words,
                            D9MenuShaderMatch *out,
                            char *why, size_t why_size) {
    D9MenuClosureRequest request;
    D9MenuClosureVerdict verdict;
    int result;
    zero_match(out);
    if (why && why_size) why[0] = 0;
    if (!out) return -1;
    if (!code || words < 2)
        return fail(out, why, why_size, "menu shader: no readable ps_3_0 token stream");

    memset(&request, 0, sizeof request);
    request.stage = D9_MENU_STAGE_PS;
    request.code = code;
    request.available_words = words;
    result = d9_menu_closure_certify(&request, &verdict);
    if (result != D9_MENU_MATCH ||
        (verdict.entry != D9_MENU_ENTRY_MENU_LOGO_PS &&
         verdict.entry != D9_MENU_ENTRY_MENU_COMPOSITE_PS)) {
        if (why && why_size) {
            if (verdict.reason[0])
                snprintf(why, why_size, "not a certified menu PS entry: %s",
                         verdict.reason);
            else
                snprintf(why, why_size,
                         "not the certified menu.logo.ps/menu.composite.ps bytecode");
        }
        return 0;
    }

    if (verdict.entry == D9_MENU_ENTRY_MENU_LOGO_PS) {
        if (build_logo(code, words, &out->plan, why, why_size) < 0) {
            zero_match(out);
            return -1;
        }
        fill_identity(out, D9_MENU_SHADER_LOGO_PS, verdict.entry,
                      D9_MENU_SHADER_PRE_NORMAL_PREBAKED |
                      D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED |
                      D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING |
                      D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4 |
                      D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7 |
                      D9_MENU_SHADER_PRE_LDR_QUALITY |
                      D9_MENU_SHADER_PRE_TEXTURE0_2D |
                      D9_MENU_SHADER_PRE_TEXTURE1_2D);
        return 1;
    }

    if (build_copy(code, words, &out->plan, why, why_size) < 0) {
        zero_match(out);
        return -1;
    }
    /* The exact copy's only runtime needs (LDR output, 2D sampler 0) are
     * already structural and are re-checked by the PICA descriptor gate, so
     * there is no extra material mask to satisfy. */
    fill_identity(out, D9_MENU_SHADER_COMPOSITE_PS, verdict.entry, 0u);
    return 1;
}

int d9_menu_shader_match_fixed(int kind, uint32_t preconditions,
                               D9MenuShaderMatch *out,
                               char *why, size_t why_size) {
    int entry;
    uint32_t required;
    zero_match(out);
    if (why && why_size) why[0] = 0;
    if (!out) return -1;

    switch (kind) {
    case D9_MENU_SHADER_FOG_FIXED:
        entry = D9_MENU_ENTRY_MENU_FOG_FIXED;
        required = D9_MENU_SHADER_PRE_LDR_QUALITY |
                   D9_MENU_SHADER_PRE_TEXTURE0_2D |
                   D9_MENU_SHADER_PRE_TEXTURE1_2D |
                   D9_MENU_SHADER_PRE_VERTEX_COLOR;
        break;
    case D9_MENU_SHADER_TINT_FIXED:
        entry = D9_MENU_ENTRY_MENU_UI_SCALEFORM_FIXED;
        required = D9_MENU_SHADER_PRE_LDR_QUALITY |
                   D9_MENU_SHADER_PRE_TEXTURE0_2D |
                   D9_MENU_SHADER_PRE_VERTEX_COLOR;
        break;
    default:
        return fail(out, why, why_size,
                    "menu shader: fixed selection needs fog or tint kind");
    }

    if (entry < 0 || entry >= D9_MENU_ENTRY_COUNT ||
        d9_menu_closure_entries[entry].stage != D9_MENU_STAGE_FIXED)
        return fail(out, why, why_size,
                    "menu shader: fixed entry is not in the closure table");

    fill_identity(out, kind, entry, required);
    if (d9_menu_shader_missing_preconditions(out, preconditions,
                                             why, why_size) > 0) {
        char detail[D9_MENU_SHADER_REASON_MAX];
        snprintf(detail, sizeof detail, "%s: missing preconditions: %s",
                 out->id, why && why_size ? why : "(unspecified)");
        zero_match(out);
        if (why && why_size) snprintf(why, why_size, "%s", detail);
        return -1;
    }

    if (kind == D9_MENU_SHADER_FOG_FIXED) {
        if (build_fog(&out->plan, why, why_size) < 0) {
            zero_match(out);
            return -1;
        }
    } else {
        if (build_tint(&out->plan, why, why_size) < 0) {
            zero_match(out);
            return -1;
        }
    }
    return 1;
}

int d9_menu_shader_match_vs(const uint32_t *code, size_t words,
                            D9MenuShaderMatch *out,
                            char *why, size_t why_size) {
    zero_match(out);
    if (why && why_size) why[0] = 0;
    if (!out) return -1;
    if (!code || words < 2)
        return fail(out, why, why_size, "menu.generic.vs: no readable vs_3_0 token stream");
    if ((code[0] >> 16) != 0xFFFEu || ((code[0] >> 8) & 0xFFu) != 3u ||
        (code[0] & 0xFFu) != 0u) {
        if (why && why_size)
            snprintf(why, why_size,
                     "menu.generic.vs: version 0x%08x is not vs_3_0; legacy path may decide",
                     (unsigned)code[0]);
        return 0;
    }

    /* No single hash selects a VS; the engine picks a declaration-compatible
     * member, so the family contract is structural.  d9_shader_analyze()
     * supplies framing/END validation inside walk_vs().  A vs_3_0 stream that
     * is outside the certified family is a named refusal, not a fallback. */
    if (walk_vs(code, words, out, why, why_size) < 0) {
        zero_match(out);
        return -1;
    }
    fill_identity(out, D9_MENU_SHADER_GENERIC_VS, D9_MENU_ENTRY_MENU_GENERIC_VS, 0u);
    return 1;
}

int d9_menu_shader_vs_check_decl(const D9MenuShaderMatch *match,
                                 const D9Decl *decl,
                                 char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!match || match->kind != D9_MENU_SHADER_GENERIC_VS) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: no selected vertex family");
        return -1;
    }
    if (!decl) {
        if (why && why_size)
            snprintf(why, why_size, "menu.generic.vs: no vertex declaration or FVF is bound");
        return -1;
    }
    for (uint32_t need = 0; need < match->input_count; ++need) {
        unsigned usage = match->input_usage[need];
        unsigned index = match->input_index[need];
        int found = 0;
        for (unsigned i = 0; i < decl->count; ++i) {
            if (decl->elements[i].usage == usage &&
                decl->elements[i].usage_index == index) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (why && why_size)
                snprintf(why, why_size,
                         "menu.generic.vs: vertex declaration lacks %s%u",
                         usage_name(usage), index);
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------- */
/* Measured FXP key mapping                                              */
/* --------------------------------------------------------------------- */

typedef struct {
    const char *package;
    uint32_t key;
    int stage;
    int entry;
} D9MenuFxpKey;

/* Package paths are canonical lowercase with '/'.  The VS family has many
 * technique keys; the package rule is the measured bound and the bytecode
 * still has to pass d9_menu_shader_match_vs(). */
static const D9MenuFxpKey d9_menu_fxp_keys[] = {
    {"shadersfx/lighting/059/p1200005.fxp", 0x01200005u,
     D9_MENU_STAGE_PS, D9_MENU_ENTRY_MENU_LOGO_PS},
    {"shadersfx/imagespace002.fxp", 0x00000000u,
     D9_MENU_STAGE_PS, D9_MENU_ENTRY_MENU_COMPOSITE_PS},
    {"shadersfx/shaders001.fxp", D9_MENU_FXP_KEY_ANY,
     D9_MENU_STAGE_VS, D9_MENU_ENTRY_MENU_GENERIC_VS},
    {"shadersfx/shaders002.fxp", D9_MENU_FXP_KEY_ANY,
     D9_MENU_STAGE_VS, D9_MENU_ENTRY_MENU_GENERIC_VS},
};

static const char *skip_data_prefix(const char *path) {
    if (!path) return path;
    if ((path[0] == 'd' || path[0] == 'D') &&
        (path[1] == 'a' || path[1] == 'A') &&
        (path[2] == 't' || path[2] == 'T') &&
        (path[3] == 'a' || path[3] == 'A') &&
        (path[4] == '/' || path[4] == '\\') && path[4])
        return path + 5;
    return path;
}

static int package_equal(const char *a, const char *b) {
    a = skip_data_prefix(a);
    b = skip_data_prefix(b);
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned x = (unsigned char)*a++;
        unsigned y = (unsigned char)*b++;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

int d9_menu_shader_lookup_fxp(int stage, const char *package_path, uint32_t key,
                              int *entry, char *why, size_t why_size) {
    if (entry) *entry = D9_MENU_ENTRY_NONE;
    if (why && why_size) why[0] = 0;
    if (!package_path || (stage != D9_MENU_STAGE_PS && stage != D9_MENU_STAGE_VS)) {
        if (why && why_size)
            snprintf(why, why_size, "menu fxp: a ps/vs stage and package path are required");
        return 0;
    }
    for (unsigned i = 0; i < sizeof d9_menu_fxp_keys / sizeof d9_menu_fxp_keys[0]; ++i) {
        const D9MenuFxpKey *m = &d9_menu_fxp_keys[i];
        if (m->stage != stage || !package_equal(package_path, m->package)) continue;
        if (m->key != D9_MENU_FXP_KEY_ANY && m->key != key) {
            if (why && why_size)
                snprintf(why, why_size,
                         "menu fxp: %s key 0x%08x is not the certified entry",
                         m->package, (unsigned)key);
            return 0;
        }
        if (entry) *entry = m->entry;
        return 1;
    }
    if (why && why_size)
        snprintf(why, why_size, "menu fxp: package '%s' is not in the certified menu set",
                 package_path);
    return 0;
}

int d9_menu_shader_missing_preconditions(const D9MenuShaderMatch *match,
                                         uint32_t have,
                                         char *why, size_t why_size) {
    uint32_t missing;
    int count = 0;
    size_t used = 0;
    char dummy[1];
    char *text = (why && why_size) ? why : dummy;
    size_t cap = (why && why_size) ? why_size : sizeof dummy;
    if (why && why_size) why[0] = 0;
    if (!match) return 0;
    missing = match->required_preconditions & ~have;
    if (!missing) return 0;
    for (unsigned bit = 0; bit < 32u; ++bit) {
        uint32_t value = 1u << bit;
        if (!(missing & value)) continue;
        const char *name = d9_menu_shader_precondition_name(value);
        int wrote = snprintf(text + used, cap - used, "%s%s",
                             used ? ", " : "", name);
        if (wrote < 0 || (size_t)wrote >= cap - used) used = cap - 1u;
        else used += (size_t)wrote;
        ++count;
    }
    return count;
}

void d9_menu_shader_runtime_reset(void) {
    g_preconditions = 0;
}

void d9_menu_shader_note_preconditions(uint32_t mask) {
    g_preconditions |= mask;
}

uint32_t d9_menu_shader_preconditions(void) {
    return g_preconditions;
}

const char *d9_menu_shader_kind_name(int kind) {
    switch (kind) {
    case D9_MENU_SHADER_LOGO_PS: return "menu.logo.ps";
    case D9_MENU_SHADER_COMPOSITE_PS: return "menu.composite.ps";
    case D9_MENU_SHADER_FOG_FIXED: return "menu.fog.fixed";
    case D9_MENU_SHADER_TINT_FIXED: return "menu.tint.fixed";
    case D9_MENU_SHADER_GENERIC_VS: return "menu.generic.vs";
    default: return "none";
    }
}

const char *d9_menu_shader_precondition_name(uint32_t bit) {
    switch (bit) {
    case D9_MENU_SHADER_PRE_NORMAL_PREBAKED: return "normal_prebaked";
    case D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED: return "tangent_basis_prebaked";
    case D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING: return "ambient_emissive_varying";
    case D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4: return "material_alpha_c4";
    case D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7: return "material_constants_c0_c7";
    case D9_MENU_SHADER_PRE_LDR_QUALITY: return "ldr_quality";
    case D9_MENU_SHADER_PRE_TEXTURE0_2D: return "texture0_2d";
    case D9_MENU_SHADER_PRE_TEXTURE1_2D: return "texture1_2d";
    case D9_MENU_SHADER_PRE_VERTEX_COLOR: return "vertex_color";
    default: return "unknown";
    }
}
