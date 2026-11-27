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
    case 0x23: /* abs (measured menu VS permutation) */
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
    if (usage == 3u && index == 0u) return 1;       /* normal0 (logo light) */
    if (usage == 5u && index <= 4u) return 1;       /* texcoord0..4 */
    if (usage == 10u && index == 0u) return 1;      /* color0 */
    if (usage == 10u && index == 1u) return 1;      /* color1 (logo tint) */
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

/* Decode one SM3 register token.  dst selects the write-mask reading; src
 * selects the swizzle/source-modifier reading. */
static void vs_reg(uint32_t token, int dst, unsigned *type, unsigned *num,
                   unsigned *swizzle, unsigned *modifier, unsigned *mask,
                   int *relative) {
    *type = (token >> 28) & 0x7u;
    *num = token & 0x7FFu;
    *swizzle = (token >> 16) & 0xFFu;
    *modifier = (token >> 24) & 0xFu;
    *mask = (token >> 16) & 0xFu;
    *relative = (token & (1u << 13)) != 0;
}

/* The core's COM draw applies exactly four clip rows (c0..c3) to position,
 * copies the declaration's texcoord and copies the declaration's COLOR0.  A
 * menu VS member is executable only when its *visible* outputs are precisely
 * that program.  The logo path also computes normal lighting and color1; those
 * instructions are allowed but must not feed position0/texcoord0/color0. */
static int vs_cpu_safe(const uint32_t *code, size_t words,
                       int *uvgen_out, int *color_swap_out) {
    unsigned position_input = 0xFFFFu;
    unsigned texcoord_input[8]; unsigned texcoord_count = 0;
    unsigned color_input = 0xFFFFu;
    unsigned position_out = 0xFFFFu, texcoord_out = 0xFFFFu;
    unsigned color_out = 0xFFFFu;
    size_t i = 1;
    while (i < words) {
        uint32_t token = code[i];
        unsigned op = token & 0xFFFFu;
        if (op == D3DSIO_END) break;
        if (op == D3DSIO_COMMENT) {
            size_t skip = (size_t)((token >> 16) & 0x7FFFu);
            if (skip > words - i - 1u) return 0;
            i += skip + 1u;
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - i) return 0;
        if (op == D3DSIO_DCL && total == 3u) {
            uint32_t decl = code[i + 1], dst = code[i + 2];
            unsigned usage = decl & 0x1Fu, index = (decl >> 16) & 0xFu;
            unsigned type, num, swz, mod, mask; int rel;
            vs_reg(dst, 1, &type, &num, &swz, &mod, &mask, &rel);
            if (type == 1u) {
                if (usage == 0u && index == 0u) position_input = num;
                else if (usage == 5u) {
                    if (texcoord_count < sizeof texcoord_input / sizeof texcoord_input[0])
                        texcoord_input[texcoord_count++] = num;
                } else if (usage == 10u && index == 0u) color_input = num;
            } else if (type == 6u) {
                if (usage == 0u && index == 0u) position_out = num;
                else if (usage == 5u && index == 0u) texcoord_out = num;
                else if (usage == 10u && index == 0u) color_out = num;
            }
        }
        i += total;
    }
    if (uvgen_out) *uvgen_out = 0;
    if (color_swap_out) *color_swap_out = 0;
    if (position_input == 0xFFFFu || position_out == 0xFFFFu ||
        texcoord_out == 0xFFFFu)
        return 0;

    unsigned pos_rows = 0, tc_writes = 0, tcgen_writes = 0, tcgen_rows = 0;
    unsigned color_writes = 0;
    int uvgen = 0, color_swap = 0;
    i = 1;
    while (i < words) {
        uint32_t token = code[i];
        unsigned op = token & 0xFFFFu;
        if (op == D3DSIO_END) break;
        if (op == D3DSIO_COMMENT) {
            size_t skip = (size_t)((token >> 16) & 0x7FFFu);
            if (skip > words - i - 1u) return 0;
            i += skip + 1u;
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - i) return 0;
        if (op == D3DSIO_DCL || (op >= 0x51u && op <= 0x53u)) {
            i += total;
            continue;
        }
        if (total < 2u) { i += total; continue; }
        unsigned dtype, dnum, dswz, dmod, dmask, drel;
        vs_reg(code[i + 1], 1, &dtype, &dnum, &dswz, &dmod, &dmask, &drel);
        if (dtype == 6u && dnum == position_out) {
            if (op != 0x09u || total != 4u) return 0;
            unsigned mask = (code[i + 1] >> 16) & 0xFu;
            if (!mask || mask & (mask - 1u)) return 0;
            unsigned component = 0;
            while (!(mask & (1u << component))) ++component;
            unsigned src_pos = 0xFFFFu, src_row = 0xFFFFu;
            for (unsigned s = 0; s < 2u; ++s) {
                unsigned type, num, swz, mod, m; int rel;
                vs_reg(code[i + 2u + s], 0, &type, &num, &swz, &mod, &m, &rel);
                if (rel || mod != 0u || swz != 0xE4u) return 0;
                if (type == 1u && num == position_input) src_pos = num;
                else if (type == 2u && num < 4u) src_row = num;
                else return 0;
            }
            if (src_pos == 0xFFFFu || src_row != component) return 0;
            pos_rows |= 1u << component;
        } else if (dtype == 6u && dnum == texcoord_out) {
            unsigned mask = (code[i + 1] >> 16) & 0xFu;
            if (!mask || (mask & ~3u)) return 0;
            if (op == 0x01u && total == 3u) {
                /* mov texcoord0.xy, texcoordN */
                ++tc_writes;
                unsigned type, num, swz, mod, m; int rel;
                vs_reg(code[i + 2], 0, &type, &num, &swz, &mod, &m, &rel);
                if (rel || mod != 0u || swz != 0xE4u) return 0;
                int known_tc = 0;
                if (type == 1u)
                    for (unsigned t = 0; t < texcoord_count; ++t)
                        if (texcoord_input[t] == num) known_tc = 1;
                if (!known_tc) return 0;
            } else if (op == 0x09u && total == 4u) {
                /* dp4 texcoord0.xy, position, c4/c5 (GFx texgen). */
                for (unsigned s = 0; s < 2u; ++s) {
                    unsigned type, num, swz, mod, m; int rel;
                    vs_reg(code[i + 2u + s], 0, &type, &num, &swz, &mod, &m, &rel);
                    if (rel || mod != 0u || swz != 0xE4u) return 0;
                    if (type == 1u && num == position_input) continue;
                    if (type == 2u && (num == 4u || num == 5u)) {
                        tcgen_rows |= 1u << (num - 4u);
                        continue;
                    }
                    return 0;
                }
                uvgen = 1;
                ++tcgen_writes;
            } else {
                return 0;
            }
        } else if (dtype == 6u && dnum == color_out) {
            if (op != 0x01u || total != 3u || color_input == 0xFFFFu) return 0;
            unsigned type, num, swz, mod, m; int rel;
            vs_reg(code[i + 2], 0, &type, &num, &swz, &mod, &m, &rel);
            if (rel || mod != 0u) return 0;
            if (type != 1u || num != color_input) return 0;
            /* xyzw is identity; zyxw is the GFx BGR vertex-colour order. */
            if (swz == 0xE4u) color_swap = 0;
            else if (swz == 0xC6u) color_swap = 1;  /* zyxw */
            else return 0;
            ++color_writes;
        }
        i += total;
    }
    if (pos_rows != 0xFu) return 0;
    if (!((tc_writes == 1u && tcgen_writes == 0u) ||
          (tc_writes == 0u && tcgen_writes == 2u && uvgen &&
           tcgen_rows == 3u)))
        return 0;
    if (color_out != 0xFFFFu && color_writes != 1u) return 0;
    if (color_out == 0xFFFFu && color_writes != 0u) return 0;
    if (uvgen_out) *uvgen_out = uvgen;
    if (color_swap_out) *color_swap_out = color_swap;
    return 1;
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
    /* color0/color1 are optional: the measured GFx set includes a
     * position+texgen member (6186c545) and members without color1. */
    int uvgen = 0, color_swap = 0;
    /* The member is selectable even when it computes logo lighting.  The
     * visible 2D path is executable only when position0/texcoord0/color0 are
     * the fixed transform the COM core already performs; normal and color1
     * work is then provably off that path. */
    out->backend_cpu_safe = (uint32_t)vs_cpu_safe(code, words, &uvgen,
                                                  &color_swap);
    out->backend_uvgen = (uint32_t)uvgen;
    out->backend_color_swap = (uint32_t)color_swap;
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
