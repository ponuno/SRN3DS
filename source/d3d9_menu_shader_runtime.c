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
