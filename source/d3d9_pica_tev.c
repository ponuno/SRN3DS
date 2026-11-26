#include "d3d9_pica_tev.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PICA_REG_TYPE(t) ((((t) >> 28) & 7u) | (((t) & 0x1800u) >> 8))
#define PICA_REG_NUM(t)  ((t) & 0x7FFu)

static int reject(D9PicaTevReject *reason, D9PicaTevReject code,
                  char *why, size_t why_size, const char *fmt, ...) {
    if (reason) *reason = code;
    if (why && why_size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, why_size, fmt, ap);
        va_end(ap);
    }
    return -1;
}

void d9_pica_tev_program_init(D9PicaTevProgram *program) {
    if (!program) return;
    memset(program, 0, sizeof *program);
    program->sampler_reg = D9_PICA_TEV_NO_SAMPLER;
    program->texcoord_reg = D9_PICA_TEV_NO_SAMPLER;
}

const char *d9_pica_tev_reject_name(D9PicaTevReject reason) {
    switch (reason) {
    case D9_PICA_TEV_OK: return "ok";
    case D9_PICA_TEV_NULL_PROGRAM: return "null-program";
    case D9_PICA_TEV_PROGRAM_VERSION: return "program-version";
    case D9_PICA_TEV_MALFORMED: return "malformed-program";
    case D9_PICA_TEV_PROGRAM_COUNT: return "program-count";
    case D9_PICA_TEV_TEXTURE_COUNT: return "texture-count";
    case D9_PICA_TEV_SAMPLER: return "sampler";
    case D9_PICA_TEV_SAMPLER_TYPE: return "sampler-type";
    case D9_PICA_TEV_STAGE_COUNT: return "stage-count";
    case D9_PICA_TEV_EXTERNAL_CONSTANT: return "external-constant";
    case D9_PICA_TEV_VERTEX_COLOR: return "vertex-color";
    case D9_PICA_TEV_FRAGMENT_VARYING: return "fragment-varying";
    case D9_PICA_TEV_VPOS: return "vpos";
    case D9_PICA_TEV_INPUT_USAGE: return "input-usage";
    case D9_PICA_TEV_INPUT_COUNT: return "input-count";
    case D9_PICA_TEV_TEV_SOURCE: return "tev-source";
    case D9_PICA_TEV_TEXTURE_SOURCE: return "texture-source";
    case D9_PICA_TEV_TEV_OPERAND: return "tev-operand";
    case D9_PICA_TEV_TEV_FUNCTION: return "tev-function";
    case D9_PICA_TEV_TEV_SCALE: return "tev-scale";
    case D9_PICA_TEV_TEV_CONSTANT: return "tev-constant";
    case D9_PICA_TEV_TRANSLATION: return "translation";
    default: return "unknown";
    }
}

uint32_t d9_pica_tev_color_abgr(uint32_t rgba8) {
    return ((rgba8 & 0x000000FFu) << 24) |
           ((rgba8 & 0x0000FF00u) << 8) |
           ((rgba8 & 0x00FF0000u) >> 8) |
           ((rgba8 & 0xFF000000u) >> 24);
}

static const char *input_usage_name(unsigned usage) {
    switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 8: return "TESSFACTOR";
    case 9: return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return "unknown";
    }
}

static int valid_rgb_operand(unsigned op) {
    switch (op) {
    case D9TEV_RGB_COLOR: case D9TEV_RGB_1M_COLOR:
    case D9TEV_RGB_ALPHA: case D9TEV_RGB_1M_ALPHA:
    case D9TEV_RGB_R: case D9TEV_RGB_1M_R:
    case D9TEV_RGB_G: case D9TEV_RGB_1M_G:
    case D9TEV_RGB_B: case D9TEV_RGB_1M_B:
        return 1;
    default:
        return 0;
    }
}

static int valid_alpha_operand(unsigned op) {
    return op <= D9TEV_A_1M_B;
}

static unsigned combine_operands(unsigned func) {
    switch (func) {
    case D9TEV_FUNC_REPLACE: return 1;
    case D9TEV_FUNC_MODULATE: case D9TEV_FUNC_ADD:
    case D9TEV_FUNC_ADD_SIGNED: case D9TEV_FUNC_SUBTRACT:
    case D9TEV_FUNC_DOT3_RGB: case D9TEV_FUNC_DOT3_RGBA: return 2;
    case D9TEV_FUNC_INTERPOLATE: case D9TEV_FUNC_MULTIPLY_ADD:
    case D9TEV_FUNC_ADD_MULTIPLY: return 3;
    default: return 0;
    }
}

static int validate_source(unsigned source, unsigned texture_count, int menu,
                           D9PicaTevReject *reason,
                           char *why, size_t why_size, unsigned stage) {
    switch (source) {
    case D9TEV_SRC_CONST:
    case D9TEV_SRC_PREV:
        return 0;
    case D9TEV_SRC_TEX0:
        if (texture_count >= 1) return 0;
        return reject(reason, D9_PICA_TEV_TEXTURE_SOURCE, why, why_size,
                      "TEV stage %u reads texture unit 0 but the program has no texture", stage);
    case D9TEV_SRC_TEX1:
        if (menu && texture_count >= 2) return 0;
        return reject(reason, D9_PICA_TEV_TEXTURE_SOURCE, why, why_size,
                      "TEV stage %u reads texture unit 1 but only %s is bound",
                      stage, menu ? "texture unit 1" : "texture unit 0");
    case D9TEV_SRC_TEX2:
    case 0x06: /* GPU_TEXTURE3 */
        return reject(reason, D9_PICA_TEV_TEXTURE_SOURCE, why, why_size,
                      "TEV stage %u reads texture unit %u; at most two units are supported",
                      stage, source - D9TEV_SRC_TEX0);
    case D9TEV_SRC_PRIMARY:
        if (menu) return 0;
        return reject(reason, D9_PICA_TEV_VERTEX_COLOR, why, why_size,
                      "TEV stage %u reads PRIMARY colour (vertex colour is not provided)", stage);
    case D9TEV_SRC_FRAG_PRIMARY:
        return reject(reason, D9_PICA_TEV_FRAGMENT_VARYING, why, why_size,
                      "TEV stage %u reads fragment varying FRAG_PRIMARY; only the menu-light ambient fold is supported",
                      stage);
    case D9TEV_SRC_FRAG_SECONDARY:
        return reject(reason, D9_PICA_TEV_FRAGMENT_VARYING, why, why_size,
                      "TEV stage %u reads fragment varying FRAG_SECONDARY; specular fragment lighting is not configured",
                      stage);
    case 0x0D: /* GPU_PREVIOUS_BUFFER */
        return reject(reason, D9_PICA_TEV_TEV_SOURCE, why, why_size,
                      "TEV stage %u reads PREVIOUS_BUFFER; only PREVIOUS is supported", stage);
    default:
        return reject(reason, D9_PICA_TEV_TEV_SOURCE, why, why_size,
                      "TEV stage %u uses unsupported source 0x%02x", stage, source);
    }
}

/* The menu-light plan is a five-stage pre-baked D3D plan:
 *   NdotL, NdotL*c4 + color1, diffuse*light, vertex tint, c7.a fade.
 * PICA has one interpolated vertex colour and one TEV constant per stage, so
 * the c4*NdotL + color1 stage cannot be emitted literally.  Fold color1 into
 * the internal c8 material slot and split that one MAD into MUL + ADD.  This
 * keeps the algebra (rounding aside) and six stages, and is only allowed for
 * D9TEV_PLAN_MENU_LIGHT.  Per-vertex color1 interpolation is not claimed. */
static int stage_is_menu_ambient_mad(const D9TevStage *s) {
    if (s->func_rgb != D9TEV_FUNC_MULTIPLY_ADD || s->func_a != D9TEV_FUNC_MULTIPLY_ADD)
        return 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (s->src_rgb[i] != s->src_a[i]) return 0;
        unsigned want = i == 0 ? D9TEV_SRC_PREV :
                        i == 1 ? D9TEV_SRC_CONST : D9TEV_SRC_FRAG_PRIMARY;
        if (s->src_rgb[i] != want) return 0;
    }
    return s->const_reg != 0xFFu;
}

static int expand_menu_ambient(const D9TevProgram *program,
                               D9TevStage out[D9_PICA_TEV_MAX_STAGES],
                               unsigned *out_count, int *ambient_fold,
                               char *why, size_t why_size) {
    unsigned count = 0;
    *ambient_fold = 0;
    for (unsigned s = 0; s < program->stages; ++s) {
        const D9TevStage *stage = &program->stage[s];
        int uses_frag = 0;
        for (unsigned i = 0; i < 3; ++i)
            if (stage->src_rgb[i] == D9TEV_SRC_FRAG_PRIMARY ||
                stage->src_a[i] == D9TEV_SRC_FRAG_PRIMARY) uses_frag = 1;
        if (!uses_frag) {
            if (count >= D9_PICA_TEV_MAX_STAGES) {
                if (why && why_size) snprintf(why, why_size, "more than %d TEV stages",
                                              D9_PICA_TEV_MAX_STAGES);
                return -1;
            }
            out[count++] = *stage;
            continue;
        }
        if (!(program->backend_flags & D9TEV_PLAN_MENU_LIGHT) ||
            !stage_is_menu_ambient_mad(stage) || count + 1 >= D9_PICA_TEV_MAX_STAGES) {
            if (why && why_size)
                snprintf(why, why_size,
                         "FRAG_PRIMARY (color1) has no PICA vertex output; only the menu-light c4*factor + ambient fold is supported");
            return -1;
        }
        D9TevStage a = *stage;
        a.func_rgb = a.func_a = D9TEV_FUNC_MODULATE;
        for (unsigned i = 0; i < 3; ++i) {
            a.src_rgb[i] = a.src_a[i] = (uint8_t)(i == 0 ? D9TEV_SRC_PREV :
                                                  i == 1 ? D9TEV_SRC_CONST : D9TEV_SRC_PREV);
            a.op_rgb[i] = D9TEV_RGB_COLOR;
            a.op_a[i] = D9TEV_A_ALPHA;
        }
        a.const_is_literal = 0;
        a.const_rgba8 = 0;
        D9TevStage b;
        memset(&b, 0, sizeof b);
        b.func_rgb = b.func_a = D9TEV_FUNC_ADD;
        for (unsigned i = 0; i < 3; ++i) {
            b.src_rgb[i] = b.src_a[i] = (uint8_t)(i == 0 ? D9TEV_SRC_PREV :
                                                  i == 1 ? D9TEV_SRC_CONST : D9TEV_SRC_PREV);
            b.op_rgb[i] = D9TEV_RGB_COLOR;
            b.op_a[i] = D9TEV_A_ALPHA;
        }
        b.const_reg = D9_PICA_TEV_AMBIENT_CONST;
        b.const_is_literal = 0;
        b.const_rgba8 = 0;
        out[count++] = a;
        out[count++] = b;
        *ambient_fold = 1;
    }
    *out_count = count;
    return 0;
}

static int add_descriptor_constant(D9PicaTevProgram *result, unsigned reg) {
    for (unsigned i = 0; i < result->constant_count; ++i)
        if (result->constant_reg[i] == reg) return 0;
    if (result->constant_count >= sizeof result->constant_reg) return -1;
    result->constant_reg[result->constant_count++] = (uint8_t)reg;
    return 0;
}

int d9_pica_tev_validate(const D9TevProgram *program,
                         D9PicaTevProgram *descriptor,
                         D9PicaTevReject *reason,
                         char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (reason) *reason = D9_PICA_TEV_OK;
    if (descriptor) d9_pica_tev_program_init(descriptor);
    if (!program)
        return reject(reason, D9_PICA_TEV_NULL_PROGRAM, why, why_size,
                      "null D9TevProgram");
    if ((program->version >> 16) != 0xFFFFu ||
        ((program->version >> 8) & 0xFFu) != 3u ||
        (program->version & 0xFFu) != 0u)
        return reject(reason, D9_PICA_TEV_PROGRAM_VERSION, why, why_size,
                      "not a ps_3_0 TEV program (version 0x%08x)", program->version);
    if (program->stage_limit != D9_PICA_TEV_MAX_STAGES)
        return reject(reason, D9_PICA_TEV_STAGE_COUNT, why, why_size,
                      "TEV stage_limit %u is not the PICA200 limit of %d",
                      program->stage_limit, D9_PICA_TEV_MAX_STAGES);
    if (!program->stages || program->stages > D9_PICA_TEV_MAX_STAGES)
        return reject(reason, D9_PICA_TEV_STAGE_COUNT, why, why_size,
                      "%u TEV stages; PICA200 supports 1..%d", program->stages,
                      D9_PICA_TEV_MAX_STAGES);
    if (program->textures > D9TEV_MAX_TEXTURES)
        return reject(reason, D9_PICA_TEV_TEXTURE_COUNT, why, why_size,
                      "invalid texture count %u", program->textures);
    if (program->inputs > D9TEV_MAX_INPUTS)
        return reject(reason, D9_PICA_TEV_INPUT_COUNT, why, why_size,
                      "invalid shader input count %u", program->inputs);
    if (program->constants > D9TEV_MAX_CONSTANTS)
        return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                      "invalid external constant count %u", program->constants);

    int menu = (program->backend_flags &
                (D9TEV_PLAN_MENU_LIGHT | D9TEV_PLAN_MENU_FIXED |
                 D9TEV_PLAN_MENU_TEXT)) != 0;
    if (!menu) {
        if (program->textures > 1)
            return reject(reason, D9_PICA_TEV_TEXTURE_COUNT, why, why_size,
                          "multiple textures (%u); generic translation binds only texture unit 0",
                          program->textures);
        if (program->inputs > 1)
            return reject(reason, D9_PICA_TEV_INPUT_COUNT, why, why_size,
                          "multiple shader inputs (%u); only texcoord0 is available",
                          program->inputs);
        if (program->uses_vertex_color)
            return reject(reason, D9_PICA_TEV_VERTEX_COLOR, why, why_size,
                          "vertex colour input (dcl_color) is not provided by the generic path");
    } else if (program->textures > D9_PICA_TEV_MAX_TEXTURES ||
               program->inputs > 2) {
        return reject(reason, D9_PICA_TEV_TEXTURE_COUNT, why, why_size,
                      "menu TEV plan has %u textures / %u inputs; at most 2/2 are supported",
                      program->textures, program->inputs);
    }

    int uses_primary = 0;
    int wants_ambient = 0;
    for (unsigned i = 0; i < program->inputs; ++i) {
        const D9TevInput *input = &program->input[i];
        if (input->usage == D9TEV_USAGE_POSITION)
            return reject(reason, D9_PICA_TEV_VPOS, why, why_size,
                          "VPOS (dcl_position vPos) is not a PICA TEV source");
        if (input->usage == D9TEV_USAGE_COLOR) {
            if (!menu)
                return reject(reason, D9_PICA_TEV_VERTEX_COLOR, why, why_size,
                              "vertex colour input v%u is not provided by the generic path",
                              input->reg_index);
            if (input->usage_index > 1u)
                return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                              "color%u input v%u is outside the menu colour contract",
                              input->usage_index, input->reg_index);
            if (input->usage_index == 0u) {
                if (input->tev_source != D9TEV_SRC_PRIMARY)
                    return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                                  "color0 must use GPU_PRIMARY_COLOR");
                uses_primary = 1;
            } else {
                if (input->tev_source != D9TEV_SRC_FRAG_PRIMARY)
                    return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                                  "color1 must use the menu-light ambient fold");
                wants_ambient = 1;
            }
            continue;
        }
        if (input->usage != D9TEV_USAGE_TEXCOORD)
            return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                          "%s input v%u is unsupported", input_usage_name(input->usage),
                          input->reg_index);
        if (input->usage_index || input->reg_index)
            return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                          "texcoord v%u.%u is unsupported; only texcoord0 is available",
                          input->reg_index, input->usage_index);
    }

    unsigned stage_count = program->stages;
    D9TevStage stages[D9_PICA_TEV_MAX_STAGES];
    int ambient_fold = 0;
    memset(stages, 0, sizeof stages);
    if (program->backend_flags & D9TEV_PLAN_MENU_LIGHT) {
        if (expand_menu_ambient(program, stages, &stage_count, &ambient_fold,
                                why, why_size)) {
            if (reason) *reason = D9_PICA_TEV_FRAGMENT_VARYING;
            return -1;
        }
    } else {
        for (unsigned s = 0; s < program->stages; ++s) stages[s] = program->stage[s];
    }
    if (wants_ambient && !ambient_fold)
        return reject(reason, D9_PICA_TEV_FRAGMENT_VARYING, why, why_size,
                      "color1 has no PICA vertex output; only the menu-light ambient fold is supported");
    if (stage_count > D9_PICA_TEV_MAX_STAGES)
        return reject(reason, D9_PICA_TEV_STAGE_COUNT, why, why_size,
                      "%u TEV stages after the menu ambient fold; PICA200 supports %d",
                      stage_count, D9_PICA_TEV_MAX_STAGES);

    for (unsigned t = 0; t < program->textures; ++t) {
        const D9TevTexture *texture = &program->texture[t];
        if (texture->sampler_reg != t)
            return reject(reason, D9_PICA_TEV_SAMPLER, why, why_size,
                          "texture %u must use sampler s%u, not sampler s%u",
                          t, t, texture->sampler_reg);
        if (texture->sampler_type != 2)
            return reject(reason, D9_PICA_TEV_SAMPLER_TYPE, why, why_size,
                          "sampler s%u has type %u; only 2D textures are supported",
                          texture->sampler_reg, texture->sampler_type);
        if (texture->texcoord_reg != 0)
            return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                          "texture uses texcoord v%u; only texcoord0 is available",
                          texture->texcoord_reg);
    }

    D9PicaTevProgram result;
    d9_pica_tev_program_init(&result);
    for (unsigned s = 0; s < stage_count; ++s) {
        const D9TevStage *stage = &stages[s];
        D9PicaTevStage out;
        memset(&out, 0, sizeof out);
        int uses_constant = 0;
        int wants_external = 0;

        if (stage->func_rgb > D9TEV_FUNC_ADD_MULTIPLY)
            return reject(reason, D9_PICA_TEV_TEV_FUNCTION, why, why_size,
                          "TEV stage %u has unsupported RGB combine function %u",
                          s, stage->func_rgb);
        if (stage->func_a > D9TEV_FUNC_ADD_MULTIPLY)
            return reject(reason, D9_PICA_TEV_TEV_FUNCTION, why, why_size,
                          "TEV stage %u has unsupported alpha combine function %u",
                          s, stage->func_a);
        if (stage->scale_rgb > D9TEV_SCALE_4 || stage->scale_a > D9TEV_SCALE_4)
            return reject(reason, D9_PICA_TEV_TEV_SCALE, why, why_size,
                          "TEV stage %u has unsupported scale (%u,%u)",
                          s, stage->scale_rgb, stage->scale_a);
        if (stage->const_is_literal > 1)
            return reject(reason, D9_PICA_TEV_TEV_CONSTANT, why, why_size,
                          "TEV stage %u has invalid const_is_literal=%u",
                          s, stage->const_is_literal);

        for (unsigned i = 0; i < 3; ++i) {
            if (validate_source(stage->src_rgb[i], program->textures, menu, reason,
                                why, why_size, s)) return -1;
            if (validate_source(stage->src_a[i], program->textures, menu, reason,
                                why, why_size, s)) return -1;
            if (!valid_rgb_operand(stage->op_rgb[i]))
                return reject(reason, D9_PICA_TEV_TEV_OPERAND, why, why_size,
                              "TEV stage %u operand %u has unsupported RGB selector 0x%02x",
                              s, i, stage->op_rgb[i]);
            if (!valid_alpha_operand(stage->op_a[i]))
                return reject(reason, D9_PICA_TEV_TEV_OPERAND, why, why_size,
                              "TEV stage %u operand %u has unsupported alpha selector 0x%02x",
                              s, i, stage->op_a[i]);
            if (stage->src_rgb[i] == D9TEV_SRC_CONST ||
                stage->src_a[i] == D9TEV_SRC_CONST) uses_constant = 1;
        }

        /* PREVIOUS is only dummy padding in stage 0 for the translator's
         * one/two-operand instructions.  Never accept a real stage-0 read. */
        if (s == 0) {
            unsigned rgb_operands = combine_operands(stage->func_rgb);
            unsigned a_operands = stage->func_a == D9TEV_FUNC_DOT3_RGBA
                                ? 0u : combine_operands(stage->func_a);
            for (unsigned i = 0; i < rgb_operands; ++i)
                if (stage->src_rgb[i] == D9TEV_SRC_PREV)
                    return reject(reason, D9_PICA_TEV_TEV_SOURCE, why, why_size,
                                  "TEV stage 0 reads PREVIOUS before a stage produced it");
            for (unsigned i = 0; i < a_operands; ++i)
                if (stage->src_a[i] == D9TEV_SRC_PREV)
                    return reject(reason, D9_PICA_TEV_TEV_SOURCE, why, why_size,
                                  "TEV stage 0 alpha reads PREVIOUS before a stage produced it");
        }
        if (uses_constant) {
            if (stage->const_reg == 0xFFu)
                return reject(reason, D9_PICA_TEV_TEV_CONSTANT, why, why_size,
                              "TEV stage %u reads a constant but names no c# register", s);
            if (stage->const_is_literal) {
                out.const_is_literal = 1;
                out.const_reg = stage->const_reg;
                out.constant_abgr = d9_pica_tev_color_abgr(stage->const_rgba8);
            } else {
                if (!menu)
                    return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                                  "TEV stage %u uses external constant c%u; generic translation requires literal def c# only",
                                  s, stage->const_reg);
                if (stage->const_reg > D9_PICA_TEV_AMBIENT_CONST)
                    return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                                  "TEV stage %u uses unsupported external constant c%u",
                                  s, stage->const_reg);
                out.const_is_literal = 0;
                out.const_reg = stage->const_reg;
                wants_external = 1;
            }
        }

        memcpy(out.src_rgb, stage->src_rgb, sizeof out.src_rgb);
        memcpy(out.src_a, stage->src_a, sizeof out.src_a);
        memcpy(out.op_rgb, stage->op_rgb, sizeof out.op_rgb);
        memcpy(out.op_a, stage->op_a, sizeof out.op_a);
        out.func_rgb = stage->func_rgb;
        out.func_a = stage->func_a;
        out.scale_rgb = stage->scale_rgb;
        out.scale_a = stage->scale_a;
        out.uses_constant = (uint8_t)uses_constant;
        if (!uses_constant) out.constant_abgr = 0xFFFFFFFFu;
        if (wants_external && add_descriptor_constant(&result, stage->const_reg))
            return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                          "more than 8 external constants");
        if (descriptor) result.stage[s] = out;
    }

    /* Every external c# listed by the program must be materialised by a stage.
     * Literal def values are not external. */
    for (unsigned c = 0; c < program->constants; ++c) {
        int found = 0;
        for (unsigned s = 0; s < stage_count; ++s) {
            int uses_const = 0;
            for (unsigned k = 0; k < 3; ++k)
                if (stages[s].src_rgb[k] == D9TEV_SRC_CONST ||
                    stages[s].src_a[k] == D9TEV_SRC_CONST) uses_const = 1;
            if (uses_const && stages[s].const_reg == program->constant_reg[c])
                found = 1;
        }
        if (!found && !menu)
            return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                          "external constant c%u is not materialised", program->constant_reg[c]);
    }
    if (ambient_fold && add_descriptor_constant(&result, D9_PICA_TEV_AMBIENT_CONST))
        return reject(reason, D9_PICA_TEV_EXTERNAL_CONSTANT, why, why_size,
                      "could not reserve the ambient constant slot");

    if (descriptor) {
        result.stage_count = (uint8_t)stage_count;
        result.texture_count = (uint8_t)program->textures;
        result.sampler_reg = program->textures ? program->texture[0].sampler_reg
                                              : D9_PICA_TEV_NO_SAMPLER;
        result.texcoord_reg = program->textures ? program->texture[0].texcoord_reg
                                               : D9_PICA_TEV_NO_SAMPLER;
        result.sampler_reg1 = program->textures > 1 ? program->texture[1].sampler_reg
                                                   : D9_PICA_TEV_NO_SAMPLER;
        result.texcoord_reg1 = program->textures > 1 ? program->texture[1].texcoord_reg
                                                    : D9_PICA_TEV_NO_SAMPLER;
        result.uses_primary_color = (uint8_t)uses_primary;
        result.uses_ambient_fold = (uint8_t)ambient_fold;
        *descriptor = result;
    }
    return 0;
}

/* A narrow declaration preflight closes a diagnostic gap in the existing
 * translator: it rejects a VPOS declaration even when vPos is never read, and
 * names vertex-colour/non-texcoord declarations before translation. */
int d9_pica_tev_preflight_pixel(const uint32_t *code, size_t words,
                                D9PicaTevReject *reason,
                                char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (reason) *reason = D9_PICA_TEV_OK;
    if (!code || words < 2)
        return reject(reason, D9_PICA_TEV_MALFORMED, why, why_size,
                      "malformed ps_3_0 bytecode (too short)");
    uint32_t version = code[0];
    if ((version >> 16) != 0xFFFFu ||
        ((version >> 8) & 0xFFu) != 3u ||
        (version & 0xFFu) != 0u)
        return reject(reason, D9_PICA_TEV_PROGRAM_VERSION, why, why_size,
                      "not a ps_3_0 program (version token 0x%08x)", version);

    size_t pos = 1;
    int saw_end = 0;
    while (pos < words) {
        uint32_t token = code[pos];
        unsigned op = token & 0xFFFFu;
        if (op == 0xFFFFu) { saw_end = 1; break; }
        if (op == 0xFFFEu) {
            size_t skip = (token >> 16) & 0x7FFFu;
            if (skip > words - pos - 1)
                return reject(reason, D9_PICA_TEV_MALFORMED, why, why_size,
                              "truncated comment token");
            pos += 1 + skip;
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - pos)
            return reject(reason, D9_PICA_TEV_MALFORMED, why, why_size,
                          "truncated %s instruction", d9_tev_opcode_name(op));
        const uint32_t *arg = code + pos + 1;

        if (op == 0x1Fu) { /* dcl */
            if (total < 3)
                return reject(reason, D9_PICA_TEV_MALFORMED, why, why_size,
                              "dcl has too few operands");
            unsigned usage = arg[0] & 0x1Fu;
            unsigned usage_index = (arg[0] >> 16) & 0xFu;
            unsigned texture_type = (arg[0] >> 27) & 0xFu;
            unsigned type = PICA_REG_TYPE(arg[1]);
            unsigned reg = PICA_REG_NUM(arg[1]);
            if (type == 17u) {
                if (usage == D9TEV_USAGE_POSITION)
                    return reject(reason, D9_PICA_TEV_VPOS, why, why_size,
                                  "VPOS (dcl_position vPos) is not a PICA TEV source");
                return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                              "misc fragment input v%u (VFACE/VPOS) is unsupported", reg);
            }
            if (type == D9TEV_REG_INPUT) {
                if (usage == D9TEV_USAGE_COLOR)
                    return reject(reason, D9_PICA_TEV_VERTEX_COLOR, why, why_size,
                                  "vertex colour input (dcl_color v%u.%u) is not provided",
                                  reg, usage_index);
                if (usage != D9TEV_USAGE_TEXCOORD)
                    return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                                  "%s input v%u.%u is unsupported; only texcoord0 is available",
                                  input_usage_name(usage), reg, usage_index);
                if (usage_index || reg)
                    return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                                  "texcoord v%u.%u is unsupported; only texcoord0 is available",
                                  reg, usage_index);
            }
            if (type == D9TEV_REG_SAMPLER) {
                if (reg != 0)
                    return reject(reason, D9_PICA_TEV_SAMPLER, why, why_size,
                                  "sampler s%u is unsupported; backend binds only sampler 0", reg);
                if (texture_type != 2)
                    return reject(reason, D9_PICA_TEV_SAMPLER_TYPE, why, why_size,
                                  "sampler s%u has type %u; only 2D textures are supported",
                                  reg, texture_type);
            }
        } else if (op != 0x2Fu && op != 0x30u && op != 0x51u) {
            /* defb/defi/def carry immediate bits, not register tokens. */
            for (size_t i = 1; i < total; ++i) {
                uint32_t operand = code[pos + i];
                if ((operand & 0x80000000u) && PICA_REG_TYPE(operand) == 17u) {
                    if (PICA_REG_NUM(operand) == 0)
                        return reject(reason, D9_PICA_TEV_VPOS, why, why_size,
                                      "VPOS (vPos operand) is not a PICA TEV source");
                    return reject(reason, D9_PICA_TEV_INPUT_USAGE, why, why_size,
                                  "VFACE/misc input v%u is unsupported", PICA_REG_NUM(operand));
                }
            }
        }
        pos += total;
    }
    if (!saw_end)
        return reject(reason, D9_PICA_TEV_MALFORMED, why, why_size,
                      "ps_3_0 program is missing its end token");
    return 0;
}

int d9_pica_tev_validate_pixel(const uint32_t *code, size_t words,
                               D9TevProgram *translated,
                               D9PicaTevProgram *descriptor,
                               D9PicaTevReject *reason,
                               char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (reason) *reason = D9_PICA_TEV_OK;
    if (descriptor) d9_pica_tev_program_init(descriptor);
    if (!translated)
        return reject(reason, D9_PICA_TEV_NULL_PROGRAM, why, why_size,
                      "null translated-program output");
    memset(translated, 0, sizeof *translated);
    if (d9_pica_tev_preflight_pixel(code, words, reason, why, why_size)) return -1;
    if (d9_tev_translate_pixel(code, words, translated, why, why_size)) {
        if (reason) *reason = D9_PICA_TEV_TRANSLATION;
        if (why && why_size && !why[0])
            snprintf(why, why_size, "TEV translation rejected the program");
        memset(translated, 0, sizeof *translated);
        return -1;
    }
    int rc = d9_pica_tev_validate(translated, descriptor, reason, why, why_size);
    if (rc) memset(translated, 0, sizeof *translated);
    return rc;
}
