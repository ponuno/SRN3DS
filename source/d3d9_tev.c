/* D3D9 SM3 (ps_3_0) -> PICA200 TEV translation, first family.
 *
 * Supported subset (all other opcodes are rejected by name):
 *   nop, mov, add, sub, mad, mul, dp3, lrp, texld, dcl, def
 * with full or partial destination masks, source negate only, 2D samplers,
 * texcoord/colour inputs, straight-line code, at most 3 texture units,
 * at most 6 combiner stages and at most two distinct constants per
 * instruction (one of which may be materialised as a PREVIOUS stage).
 *
 * The model is the real PICA200 combiner, not a hash lookup:
 *   each stage has three sources, each source is one of
 *   PRIMARY / TEXTURE0..2 / CONSTANT / PREVIOUS, each has an operand selector
 *   (colour, alpha, R, G, B and their one-minus forms), and a combine function
 *   (replace, modulate, add, add_signed, interpolate, subtract, dot3,
 *   multiply_add, add_multiply).  That is exactly C3D_TexEnv's shape.
 * Temporary provenance is tracked per channel: partial writes carry their
 * untouched RGB/alpha side, so a live r0.a survives an unrelated RGB stage and
 * independent oC0.rgb/oC0.w writes do not stale the other side.
 *
 * Known precision limits (documented in docs/SHADERS.md):
 *   - TEV stage outputs and the per-stage constant colour are clamped/
 *     quantised to RGBA8.  The subset is therefore an LDR subset; HDR
 *     intermediates and constants needing more than 8 bits are not exact.
 *     Literal def components must be finite and already in [0,1]; invalid
 *     literals are rejected rather than silently clamped.
 */

#include "d3d9_tev.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* D3DSIO_* names, for rejection diagnostics. */
static const char *const tev_opcode_names[0x61] = {
    [0x00] = "nop", [0x01] = "mov", [0x02] = "add", [0x03] = "sub",
    [0x04] = "mad", [0x05] = "mul", [0x06] = "rcp", [0x07] = "rsq",
    [0x08] = "dp3", [0x09] = "dp4", [0x0A] = "min", [0x0B] = "max",
    [0x0C] = "slt", [0x0D] = "sge", [0x0E] = "exp", [0x0F] = "log",
    [0x10] = "lit", [0x11] = "dst", [0x12] = "lrp", [0x13] = "frc",
    [0x14] = "m4x4", [0x15] = "m4x3", [0x16] = "m3x4", [0x17] = "m3x3",
    [0x18] = "m3x2", [0x19] = "call", [0x1A] = "callnz", [0x1B] = "loop",
    [0x1C] = "ret", [0x1D] = "endloop", [0x1E] = "label", [0x1F] = "dcl",
    [0x20] = "pow", [0x21] = "crs", [0x22] = "sgn", [0x23] = "abs",
    [0x24] = "nrm", [0x25] = "sincos", [0x26] = "rep", [0x27] = "endrep",
    [0x28] = "if", [0x29] = "ifc", [0x2A] = "else", [0x2B] = "endif",
    [0x2C] = "break", [0x2D] = "breakc", [0x2E] = "mova", [0x2F] = "defb",
    [0x30] = "defi", [0x40] = "texcoord", [0x41] = "texkill", [0x42] = "texld",
    [0x43] = "texbem", [0x44] = "texbeml", [0x45] = "texreg2ar",
    [0x46] = "texreg2gb", [0x47] = "texm3x2pad", [0x48] = "texm3x2tex",
    [0x49] = "texm3x3pad", [0x4A] = "texm3x3tex", [0x4B] = "texm3x3diff",
    [0x4C] = "texm3x3spec", [0x4D] = "texm3x3vspec", [0x4E] = "expp",
    [0x4F] = "logp", [0x50] = "cnd", [0x51] = "def", [0x52] = "texreg2rgb",
    [0x53] = "texdp3tex", [0x54] = "texm3x2depth", [0x55] = "texdp3",
    [0x56] = "texm3x3", [0x57] = "texdepth", [0x58] = "cmp", [0x59] = "bem",
    [0x5A] = "dp2add", [0x5B] = "dsx", [0x5C] = "dsy", [0x5D] = "texldd",
    [0x5E] = "setp", [0x5F] = "texldl", [0x60] = "breakp",
};

const char *d9_tev_opcode_name(unsigned opcode) {
    if (opcode < 0x61u && tev_opcode_names[opcode]) return tev_opcode_names[opcode];
    switch (opcode) {
    case 0xFFFD: return "phase";
    case 0xFFFE: return "comment";
    case 0xFFFF: return "end";
    default: return "unknown";
    }
}

static void reject(char *why, size_t cap, const char *fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Token helpers                                                             */
/* ------------------------------------------------------------------------- */

#define REGTYPE(t) ((((t) >> 28) & 7u) | (((t) & 0x1800u) >> 8))
#define REGNUM(t)  ((t) & 0x7FFu)
#define DSTMASK(t) (((t) >> 16) & 0xFu)
#define DSTMOD(t)  (((t) >> 20) & 0xFu)
#define SWZ(t)     (((t) >> 16) & 0xFFu)
#define SRCMOD(t)  (((t) >> 24) & 0xFu)
#define RELATIVE(t) (((t) >> 13) & 0x7u)

/* (component extraction is inlined at each use site) */

/* A temporary is tracked per channel.  PICA can still address a texture,
 * input or constant after PREVIOUS has moved on, and a partial TEV write can
 * carry the untouched RGB or alpha side.  Keeping one coarse Value kind made
 * a live r0.a look stale as soon as an unrelated RGB result replaced PREVIOUS. */
typedef struct {
    uint8_t src[4];          /* D9TEV_SRC_*, or 0xFF when the channel is stale */
    uint8_t comp[4];         /* component selected from src (0..3) */
    uint8_t const_reg[4];    /* c# when src == D9TEV_SRC_CONST */
    uint8_t defined;
} Value;

enum { VAL_NONE = 0xFF };

typedef struct {
    int is_def;
    float value[4];
} DefConst;

typedef struct {
    D9TevProgram *out;
    Value temp[16];
    DefConst def[D9TEV_MAX_DEFS];
    int def_reg[D9TEV_MAX_DEFS];
    int def_count;
    int sampler_type[16];        /* 0 = undeclared, 2 = 2D, 3 = cube, 4 = volume */
    int input_usage[16];
    int input_usage_index[16];
    uint8_t input_tev_source[16];/* 0xFF = not assigned; else D9TEV_SRC_* */
    unsigned stage_constant;     /* current stage's constant (c#) or 0xFFFFFFFF */
    int current_stage;
    int saw_output_mask;
    uint8_t output_mask;
} Ctx;

static DefConst *lookup_def(Ctx *ctx, int reg) {
    for (int i = 0; i < ctx->def_count; ++i)
        if (ctx->def_reg[i] == reg) return &ctx->def[i];
    return NULL;
}

/* PICA stage constants are LDR RGBA8 values.  Clamping here would change the
 * shader's algebra (for example mad(-1,2,2) would become 1 instead of 0), so
 * reject literals that cannot be represented without a range-changing rewrite. */
static int validate_def_literal(unsigned reg, const float value[4],
                                char *why, size_t cap) {
    static const char components[4] = {'x', 'y', 'z', 'w'};
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(value[i])) {
            reject(why, cap, "def c%u.%c literal is not finite", reg, components[i]);
            return -1;
        }
        if (value[i] < 0.0f || value[i] > 1.0f) {
            reject(why, cap, "def c%u.%c literal %.9g is outside [0,1]",
                   reg, components[i], value[i]);
            return -1;
        }
    }
    return 0;
}

static Value empty_value(void) {
    Value value;
    memset(&value, 0, sizeof value);
    for (int c = 0; c < 4; ++c) value.src[c] = VAL_NONE;
    return value;
}

static void set_temp(Ctx *ctx, int reg, Value value) {
    if (reg < 0 || reg >= 16) return;
    ctx->temp[reg] = value;
}

static Value temp_value(Ctx *ctx, int reg) {
    if (reg < 0 || reg >= 16) return empty_value();
    return ctx->temp[reg];
}

/* Advance the hardware PREVIOUS register.  A REPLACE(PREVIOUS) on one side
 * keeps every temporary channel sourced from that side addressable; channels
 * overwritten by the new stage become stale. */
static void advance_prev(Ctx *ctx, int keep_rgb, int keep_a) {
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 4; ++c) {
            int keep = c == 3 ? keep_a : keep_rgb;
            if (ctx->temp[r].src[c] == D9TEV_SRC_PREV && !keep)
                ctx->temp[r].src[c] = VAL_NONE;
        }
    }
}

static void invalidate_prev(Ctx *ctx) {
    advance_prev(ctx, 0, 0);
}

static int add_texture(Ctx *ctx, int texcoord, int sampler) {
    D9TevProgram *p = ctx->out;
    for (unsigned i = 0; i < p->textures; ++i) {
        if (p->texture[i].texcoord_reg == texcoord &&
            p->texture[i].sampler_reg == sampler)
            return (int)i;
    }
    if (p->textures >= D9TEV_MAX_TEXTURES) return -1;
    unsigned i = p->textures++;
    p->texture[i].texcoord_reg = (uint8_t)texcoord;
    p->texture[i].sampler_reg = (uint8_t)sampler;
    p->texture[i].sampler_type = (uint8_t)(ctx->sampler_type[sampler] ? ctx->sampler_type[sampler] : 2);
    return (int)i;
}

static int add_input(Ctx *ctx, int usage, int usage_index, int reg) {
    D9TevProgram *p = ctx->out;
    for (unsigned i = 0; i < p->inputs; ++i) {
        if (p->input[i].reg_index == reg) return 0;
    }
    if (p->inputs >= D9TEV_MAX_INPUTS) return -1;
    unsigned i = p->inputs++;
    p->input[i].usage = (uint8_t)usage;
    p->input[i].usage_index = (uint8_t)usage_index;
    p->input[i].reg_index = (uint8_t)reg;
    p->input[i].tev_source = 0xFF;
    if (usage == D9TEV_USAGE_COLOR) p->uses_vertex_color = 1;
    return 0;
}

/* Pick a PICA TEV "colour" source for a pixel-shader input register, and
 * record it in the input table so the vertex-shader plan can route it.
 * PRIMARY is the D3D vertex colour; FRAG_PRIMARY/SECONDARY are two extra
 * generic varyings. */
static int ensure_input_source(Ctx *ctx, int reg, char *why, size_t cap,
                               const char *opname) {
    if (reg < 0 || reg >= 16) {
        reject(why, cap, "%s: input register %d out of range", opname, reg);
        return -1;
    }
    if (ctx->input_tev_source[reg] != 0xFF) return ctx->input_tev_source[reg];
    unsigned free_sources[3] = {D9TEV_SRC_PRIMARY, D9TEV_SRC_FRAG_PRIMARY,
                                D9TEV_SRC_FRAG_SECONDARY};
    int used[3] = {0, 0, 0};
    for (unsigned i = 0; i < ctx->out->inputs; ++i) {
        unsigned src = ctx->out->input[i].tev_source;
        for (int k = 0; k < 3; ++k) if (src == free_sources[k]) used[k] = 1;
    }
    int chosen = -1;
    if (ctx->input_usage[reg] == D9TEV_USAGE_COLOR && !used[0]) chosen = 0;
    if (chosen < 0) for (int k = 0; k < 3; ++k) if (!used[k]) { chosen = k; break; }
    if (chosen < 0) {
        reject(why, cap, "%s: more than 3 PS input vectors need a TEV colour "
               "source (PICA has PRIMARY + two fragment colours)", opname);
        return -1;
    }
    ctx->input_tev_source[reg] = (uint8_t)free_sources[chosen];
    for (unsigned i = 0; i < ctx->out->inputs; ++i)
        if (ctx->out->input[i].reg_index == (uint8_t)reg)
            ctx->out->input[i].tev_source = ctx->input_tev_source[reg];
    return ctx->input_tev_source[reg];
}

static int note_constant(Ctx *ctx, int reg) {
    D9TevProgram *p = ctx->out;
    for (unsigned i = 0; i < p->constants; ++i)
        if (p->constant_reg[i] == reg) return 0;
    if (p->constants >= D9TEV_MAX_CONSTANTS) return -1;
    p->constant_reg[p->constants++] = (uint8_t)reg;
    return 0;
}

int d9_tev_text_plan(D9TevProgram *out, int has_a8_unit,
                     int has_second_texture, int has_pixel_shader,
                     char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!out) {
        if (why && why_size) snprintf(why, why_size, "text TEV plan needs an output plan");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (has_pixel_shader) {
        if (why && why_size)
            snprintf(why, why_size,
                     "text TEV plan requires the fixed-function stage-0 path; a pixel shader owns the TEV state");
        return -1;
    }
    if (has_second_texture) {
        if (why && why_size)
            snprintf(why, why_size,
                     "text TEV plan accepts exactly one texture unit");
        return -1;
    }
    if (!has_a8_unit) {
        if (why && why_size)
            snprintf(why, why_size,
                     "text TEV plan requires an A8 texture unit");
        return -1;
    }

    out->version = 0xFFFF0300u;
    out->instructions = 1u;
    out->stages = 1u;
    out->stage_limit = D9TEV_MAX_STAGES;
    out->backend_flags = D9TEV_PLAN_MENU_TEXT;
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

    D9TevStage *s = &out->stage[0];
    s->func_rgb = s->func_a = D9TEV_FUNC_MODULATE;
    s->src_rgb[0] = s->src_a[0] = D9TEV_SRC_TEX0;
    s->op_rgb[0] = D9TEV_RGB_COLOR;
    s->op_a[0] = D9TEV_A_ALPHA;
    s->src_rgb[1] = s->src_a[1] = D9TEV_SRC_PRIMARY;
    s->op_rgb[1] = D9TEV_RGB_COLOR;
    s->op_a[1] = D9TEV_A_ALPHA;
    s->src_rgb[2] = s->src_a[2] = D9TEV_SRC_PRIMARY;
    s->op_rgb[2] = D9TEV_RGB_COLOR;
    s->op_a[2] = D9TEV_A_ALPHA;
    s->const_reg = 0xFFu;
    return 0;
}

uint32_t d9_tev_pack_rgba8(const float rgba[4]) {
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        float v = rgba[i];
        if (!(v > 0.0f)) v = 0.0f;         /* also maps NaN to 0 */
        if (v > 1.0f) v = 1.0f;
        unsigned q = (unsigned)(v * 255.0f + 0.5f);
        if (q > 255u) q = 255u;
        out |= (uint32_t)q << (8u * (unsigned)i);
    }
    return out;
}

static unsigned rgb_selector(unsigned comp, int negate) {
    unsigned op = comp == 0 ? D9TEV_RGB_R : comp == 1 ? D9TEV_RGB_G
               : comp == 2 ? D9TEV_RGB_B : D9TEV_RGB_ALPHA;
    return negate ? op ^ 1u : op;
}

static unsigned alpha_selector(unsigned comp, int negate) {
    unsigned op = comp == 3 ? D9TEV_A_ALPHA : comp == 0 ? D9TEV_A_R
               : comp == 1 ? D9TEV_A_G : D9TEV_A_B;
    return negate ? op ^ 1u : op;
}

/* Map one D3D operand to independent PICA RGB and alpha sources/selectors.
 * This is what lets r0.rgb come from TEXTURE0 while r0.a comes from TEXTURE1,
 * instead of forcing both sides through the last PREVIOUS result. */
static int map_operand(Ctx *ctx, uint32_t token,
                       unsigned *src_rgb, unsigned *rgb_op,
                       unsigned *src_a, unsigned *a_op,
                       unsigned creg[2], uint8_t *taint,
                       unsigned needed,
                       char *why, size_t cap, const char *opname) {
    if (!(token & 0x80000000u)) {
        reject(why, cap, "%s: malformed operand token 0x%08x", opname, token);
        return -1;
    }
    if (RELATIVE(token)) {
        reject(why, cap, "%s: relative addressing is outside the TEV subset", opname);
        return -1;
    }
    unsigned mod = SRCMOD(token);
    if (mod != 0 && mod != 1) {
        reject(why, cap, "%s: source modifier %u is outside the TEV subset "
               "(only negate is expressible)", opname, mod);
        return -1;
    }
    unsigned type = REGTYPE(token);
    unsigned index = REGNUM(token);
    uint32_t sw = SWZ(token);
    unsigned selected[4] = {(sw >> 0) & 3u, (sw >> 2) & 3u,
                            (sw >> 4) & 3u, (sw >> 6) & 3u};
    unsigned psrc[4], pcomp[4], preg[4];
    *taint = 0;
    creg[0] = creg[1] = 0xFFu;
    if (type == D9TEV_REG_TEMP) {
        Value value = temp_value(ctx, (int)index);
        for (int k = 0; k < 4; ++k) {
            unsigned channel = selected[k];
            psrc[k] = value.src[channel];
            pcomp[k] = value.comp[channel];
            preg[k] = value.const_reg[channel];
            if (psrc[k] == VAL_NONE && (needed & (1u << k)))
                *taint |= (uint8_t)(1u << k);
        }
        if (*taint) {
            reject(why, cap, "%s: reads r%u before it is written or after the "
                   "TEV previous stage moved on", opname, index);
            return -1;
        }
        unsigned fallback = (needed & 1u) ? 0u : (needed & 2u) ? 1u :
                            (needed & 4u) ? 2u : 3u;
        for (int k = 0; k < 4; ++k) {
            if (needed & (1u << k)) continue;
            psrc[k] = psrc[fallback];
            pcomp[k] = pcomp[fallback];
            preg[k] = preg[fallback];
        }
        for (int k = 0; k < 4; ++k) {
            if (psrc[k] != D9TEV_SRC_CONST) continue;
            if (note_constant(ctx, (int)preg[k]) < 0) {
                reject(why, cap, "%s: more than %d distinct constants",
                       opname, D9TEV_MAX_CONSTANTS);
                return -1;
            }
        }
        /* Channel provenance was recorded above. */



    } else {
        unsigned base_src;
        if (type == D9TEV_REG_CONST) {
            if (note_constant(ctx, (int)index) < 0) {
                reject(why, cap, "%s: more than %d distinct constants", opname, D9TEV_MAX_CONSTANTS);
                return -1;
            }
            base_src = D9TEV_SRC_CONST;
        } else if (type == D9TEV_REG_INPUT) {
            int source = ensure_input_source(ctx, (int)index, why, cap, opname);
            if (source < 0) return -1;
            base_src = (unsigned)source;
        } else {
            reject(why, cap, "%s: source register type %u is outside the TEV subset",
                   opname, type);
            return -1;
        }
        for (int k = 0; k < 4; ++k) {
            psrc[k] = base_src;
            pcomp[k] = selected[k];
            preg[k] = type == D9TEV_REG_CONST ? index : 0xFFu;
        }
    }

    /* RGB selector: colour (per-channel) or one replicated component. */
    if (psrc[0] != psrc[1] || psrc[1] != psrc[2]) {
        reject(why, cap, "%s: RGB channels need different PICA sources; "
               "one TEV RGB operand cannot select them independently", opname);
        return -1;
    }
    if (pcomp[0] == 0 && pcomp[1] == 1 && pcomp[2] == 2) {
        *rgb_op = mod ? D9TEV_RGB_1M_COLOR : D9TEV_RGB_COLOR;
    } else if (pcomp[0] == pcomp[1] && pcomp[1] == pcomp[2]) {
        *rgb_op = rgb_selector(pcomp[0], mod == 1);
    } else {
        reject(why, cap, "%s: non-uniform RGB swizzle .%c%c%c is not "
               "expressible by one PICA TEV operand", opname,
               "xyzw"[pcomp[0]], "xyzw"[pcomp[1]], "xyzw"[pcomp[2]]);
        return -1;
    }
    *src_rgb = psrc[0];
    *src_a = psrc[3];
    *a_op = alpha_selector(pcomp[3], mod == 1);
    if (*src_rgb == D9TEV_SRC_CONST) creg[0] = preg[0];
    if (*src_a == D9TEV_SRC_CONST) creg[1] = preg[3];
    return 0;
}

static int stage_full(Ctx *ctx, char *why, size_t cap, const char *opname) {
    if (ctx->out->stages >= D9TEV_MAX_STAGES) {
        reject(why, cap, "%s: needs more than %d PICA TEV stages", opname, D9TEV_MAX_STAGES);
        return -1;
    }
    return 0;
}

/* Emit one combiner stage.  Each operand already has independent RGB/alpha
 * PICA sources and selectors. */
static D9TevStage *emit_stage(Ctx *ctx, unsigned func_rgb, unsigned func_a,
                               const unsigned src_rgb[3], const unsigned rop[3],
                               const unsigned src_a[3], const unsigned aop[3]) {
    D9TevStage *stage = &ctx->out->stage[ctx->out->stages++];
    memset(stage, 0, sizeof *stage);
    stage->func_rgb = (uint8_t)func_rgb;
    stage->func_a = (uint8_t)func_a;
    stage->const_reg = 0xFF;
    for (int i = 0; i < 3; ++i) {
        stage->src_rgb[i] = (uint8_t)src_rgb[i];
        stage->src_a[i] = (uint8_t)src_a[i];
        stage->op_rgb[i] = (uint8_t)rop[i];
        stage->op_a[i] = (uint8_t)aop[i];
    }
    stage->scale_rgb = stage->scale_a = D9TEV_SCALE_1;
    ++ctx->current_stage;
    return stage;
}

/* ------------------------------------------------------------------------- */
/* Translation                                                               */
/* ------------------------------------------------------------------------- */

static int is_supported_alu(unsigned op) {
    switch (op) {
    case 0x00: /* nop */
    case 0x01: /* mov */
    case 0x02: /* add */
    case 0x03: /* sub */
    case 0x04: /* mad */
    case 0x05: /* mul */
    case 0x08: /* dp3 */
    case 0x12: /* lrp */
        return 1;
    default:
        return 0;
    }
}

static int translate(Ctx *ctx, const uint32_t *code, size_t words,
                     char *why, size_t cap) {
    D9TevProgram *p = ctx->out;
    if (!code || words < 2) {
        reject(why, cap, "bytecode too short");
        return -1;
    }
    uint32_t version = code[0];
    if ((version >> 16) != 0xFFFF || ((version >> 8) & 0xFF) != 3 || (version & 0xFF) != 0) {
        reject(why, cap, "not a ps_3_0 program (version token 0x%08x)", version);
        return -1;
    }
    p->version = version;
    p->stage_limit = D9TEV_MAX_STAGES;
    for (int i = 0; i < 16; ++i) {
        ctx->temp[i] = empty_value();
        ctx->input_tev_source[i] = 0xFF;
    }

    size_t pos = 1;
    int have_output = 0;
    uint8_t output_mask = 0;
    uint8_t output_defined = 0;
    int output_stage = -1;

    while (pos < words) {
        uint32_t token = code[pos];
        unsigned op = token & 0xFFFFu;
        if (op == 0xFFFFu) break;
        if (op == 0xFFFEu) {
            size_t skip = (size_t)((token >> 16) & 0x7FFFu);
            if (skip > words - pos - 1) {
                reject(why, cap, "truncated comment");
                return -1;
            }
            pos += 1 + skip;
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - pos) {
            reject(why, cap, "%s at word %zu is truncated", d9_tev_opcode_name(op), pos);
            return -1;
        }
        const uint32_t *arg = code + pos + 1;
        ++p->instructions;

        if (op == 0x1F) { /* dcl */
            if (total < 3) { reject(why, cap, "dcl has too few operands"); return -1; }
            unsigned usage = arg[0] & 0x1Fu;
            unsigned usage_index = (arg[0] >> 16) & 0xFu;
            unsigned texture_type = (arg[0] >> 27) & 0xFu;
            unsigned dst = arg[1];
            unsigned type = REGTYPE(dst);
            unsigned index = REGNUM(dst);
            if (index >= 16) { reject(why, cap, "dcl register %u out of range", index); return -1; }
            if (type == D9TEV_REG_SAMPLER) {
                ctx->sampler_type[index] = (int)texture_type;
            } else if (type == D9TEV_REG_INPUT) {
                ctx->input_usage[index] = (int)usage;
                ctx->input_usage_index[index] = (int)usage_index;
                if (add_input(ctx, (int)usage, (int)usage_index, (int)index) < 0) {
                    reject(why, cap, "more than %d declared inputs", D9TEV_MAX_INPUTS);
                    return -1;
                }
                if (usage == D9TEV_USAGE_COLOR) {
                    if (ensure_input_source(ctx, (int)index, why, cap, "dcl_color") < 0)
                        return -1;
                }
            } else if (type == 17u) {
                /* ps_3_0 MISCTYPE: dcl_position vPos / dcl_face vFace.  The
                 * declaration is legal and common in lit shaders, but PICA has
                 * no VPOS/VFACE TEV source, so only reject it if it is read. */
                if (usage != D9TEV_USAGE_POSITION) {
                    reject(why, cap, "dcl of ps_3_0 misc register with usage %u "
                           "is outside the TEV subset", usage);
                    return -1;
                }
            } else {
                reject(why, cap, "dcl of register type %u is outside the TEV subset", type);
                return -1;
            }
            pos += total;
            continue;
        }
        if (op == 0x51) { /* def */
            if (total != 6) {
                reject(why, cap, "def does not have four literal operands");
                return -1;
            }
            unsigned dst = arg[0];
            if (!(dst & 0x80000000u)) {
                reject(why, cap, "def: malformed destination token 0x%08x", dst);
                return -1;
            }
            if (REGTYPE(dst) != D9TEV_REG_CONST) {
                reject(why, cap, "def destination is not a float constant");
                return -1;
            }
            if (DSTMOD(dst) != 0 || DSTMASK(dst) != 0xFu) {
                reject(why, cap, "def destination modifier or mask is outside "
                       "the TEV subset (requires an unmodified full write)");
                return -1;
            }
            unsigned index = REGNUM(dst);
            float literal[4];
            memcpy(literal, &arg[1], sizeof literal);
            if (validate_def_literal(index, literal, why, cap) < 0) return -1;
            DefConst *existing = lookup_def(ctx, (int)index);
            if (existing) {
                memcpy(existing->value, literal, sizeof existing->value);
            } else {
                if (ctx->def_count >= D9TEV_MAX_DEFS) {
                    reject(why, cap, "more than %d literal constants", D9TEV_MAX_DEFS);
                    return -1;
                }
                DefConst *def = &ctx->def[ctx->def_count];
                def->is_def = 1;
                memcpy(def->value, literal, sizeof def->value);
                ctx->def_reg[ctx->def_count++] = (int)index;
            }
            pos += total;
            continue;
        }
        if (op == 0x00) { pos += total; continue; } /* nop */

        if (op == 0x42) { /* texld dst, texcoord, sampler */
            if (total != 4) {
                reject(why, cap, "texld has %u operands (expected dst, uv, sampler)", total - 1u);
                return -1;
            }
            unsigned dst_token = arg[0];
            if (!(dst_token & 0x80000000u)) {
                reject(why, cap, "texld: malformed destination token 0x%08x", dst_token);
                return -1;
            }
            if (RELATIVE(dst_token)) {
                reject(why, cap, "texld: relative destination addressing is outside "
                       "the TEV subset");
                return -1;
            }
            if (DSTMOD(dst_token) != 0) {
                reject(why, cap, "texld: destination modifier %u is outside the TEV subset",
                       DSTMOD(dst_token));
                return -1;
            }
            unsigned dst_type = REGTYPE(dst_token);
            unsigned dst_index = REGNUM(dst_token);
            unsigned mask = DSTMASK(dst_token);
            if (!mask) mask = 0xFu;
            if (dst_type != D9TEV_REG_TEMP && dst_type != D9TEV_REG_COLOROUT) {
                reject(why, cap, "texld writes register type %u (only r# and oC0)", dst_type);
                return -1;
            }
            if (dst_type == D9TEV_REG_COLOROUT && dst_index >= 1) {
                reject(why, cap, "texld: MRT write to oC%u is not supported", dst_index);
                return -1;
            }
            unsigned uv_token = arg[1];
            if (!(uv_token & 0x80000000u)) {
                reject(why, cap, "texld: malformed UV operand token 0x%08x", uv_token);
                return -1;
            }
            if (RELATIVE(uv_token)) {
                reject(why, cap, "texld: UV relative addressing is outside the TEV subset");
                return -1;
            }
            unsigned uv_mod = SRCMOD(uv_token);
            if (uv_mod != 0) {
                reject(why, cap, "texld: UV source modifier %u is outside the TEV subset",
                       uv_mod);
                return -1;
            }
            unsigned uv_type = REGTYPE(uv_token);
            unsigned uv_index = REGNUM(uv_token);
            if (uv_index >= 16) {
                reject(why, cap, "texld: UV register %u out of range", uv_index);
                return -1;
            }
            if (uv_type != D9TEV_REG_INPUT || ctx->input_usage[uv_index] != D9TEV_USAGE_TEXCOORD) {
                reject(why, cap, "texld: UV operand is not a declared texcoord input "
                       "(register type %u, index %u)", uv_type, uv_index);
                return -1;
            }
            if (SWZ(uv_token) != 0xE4u) {
                reject(why, cap, "texld: UV swizzle .%c%c%c%c is outside the TEV subset",
                       "xyzw"[(SWZ(uv_token) >> 0) & 3u],
                       "xyzw"[(SWZ(uv_token) >> 2) & 3u],
                       "xyzw"[(SWZ(uv_token) >> 4) & 3u],
                       "xyzw"[(SWZ(uv_token) >> 6) & 3u]);
                return -1;
            }
            unsigned smp_token = arg[2];
            if (!(smp_token & 0x80000000u)) {
                reject(why, cap, "texld: malformed sampler operand token 0x%08x", smp_token);
                return -1;
            }
            if (RELATIVE(smp_token)) {
                reject(why, cap, "texld: sampler relative addressing is outside the TEV subset");
                return -1;
            }
            unsigned sampler_mod = SRCMOD(smp_token);
            if (sampler_mod != 0) {
                reject(why, cap, "texld: sampler source modifier %u is outside the TEV subset",
                       sampler_mod);
                return -1;
            }
            if (REGTYPE(smp_token) != D9TEV_REG_SAMPLER) {
                reject(why, cap, "texld: second operand is not a sampler");
                return -1;
            }
            unsigned sampler = REGNUM(smp_token);
            if (sampler >= 16 || ctx->sampler_type[sampler] == 0) {
                reject(why, cap, "texld: sampler s%u was not declared", sampler);
                return -1;
            }
            if (ctx->sampler_type[sampler] != 2) {
                reject(why, cap, "texld: sampler s%u is not 2D (type %d); the first "
                       "family supports 2D only", sampler, ctx->sampler_type[sampler]);
                return -1;
            }
            int unit = add_texture(ctx, (int)uv_index, (int)sampler);
            if (unit < 0) {
                reject(why, cap, "texld: more than %d texture units", D9TEV_MAX_TEXTURES);
                return -1;
            }
            if (dst_type == D9TEV_REG_COLOROUT) {
                /* A direct texld -> oC0 still needs one REPLACE stage because
                 * PICA has no "write texel directly" path.  On a later partial
                 * output write, carry the untouched side through PREVIOUS. */
                unsigned src_rgb[3] = {(unsigned)(D9TEV_SRC_TEX0 + unit),
                                       D9TEV_SRC_PREV, D9TEV_SRC_PREV};
                unsigned src_a[3] = {(unsigned)(D9TEV_SRC_TEX0 + unit),
                                     D9TEV_SRC_PREV, D9TEV_SRC_PREV};
                unsigned rop[3] = {D9TEV_RGB_COLOR, D9TEV_RGB_COLOR, D9TEV_RGB_COLOR};
                unsigned aop[3] = {D9TEV_A_ALPHA, D9TEV_A_ALPHA, D9TEV_A_ALPHA};
                int keep_rgb = p->stages > 0 && !(mask & 7u);
                int keep_a = p->stages > 0 && !(mask & 8u);
                if (keep_rgb) src_rgb[0] = D9TEV_SRC_PREV;
                if (keep_a) src_a[0] = D9TEV_SRC_PREV;
                if (stage_full(ctx, why, cap, "texld") < 0) return -1;
                emit_stage(ctx, D9TEV_FUNC_REPLACE, D9TEV_FUNC_REPLACE,
                           src_rgb, rop, src_a, aop);
                advance_prev(ctx, keep_rgb, keep_a);
                have_output = 1;
                output_mask = (uint8_t)mask;
                output_defined = (uint8_t)mask;
                output_stage = (int)p->stages - 1;
            } else {
                Value value = empty_value();
                for (int c = 0; c < 4; ++c) {
                    if (!(mask & (1u << c))) continue;
                    value.src[c] = (uint8_t)(D9TEV_SRC_TEX0 + unit);
                    value.comp[c] = (uint8_t)c;
                    value.defined |= (uint8_t)(1u << c);
                }
                set_temp(ctx, (int)dst_index, value);
            }
            pos += total;
            continue;
        }

        if (!is_supported_alu(op)) {
            reject(why, cap, "unsupported opcode '%s' at word %zu", d9_tev_opcode_name(op), pos);
            return -1;
        }

        /* ALU instruction: dst, then 1..3 sources.  The length field counts
         * operands (dst included) in bits 24..27. */
        if (total < 3) { reject(why, cap, "%s has no destination", d9_tev_opcode_name(op)); return -1; }
        unsigned dst_token = arg[0];
        unsigned dst_type = REGTYPE(dst_token);
        unsigned dst_index = REGNUM(dst_token);
        unsigned mask = DSTMASK(dst_token);
        if (!mask) mask = 0xFu;
        unsigned dstmod = DSTMOD(dst_token);
        if (dstmod & ~0x7u) {
            reject(why, cap, "%s: destination modifier %u is outside the TEV subset",
                   d9_tev_opcode_name(op), dstmod);
            return -1;
        }
        if (dst_type != D9TEV_REG_TEMP && dst_type != D9TEV_REG_COLOROUT) {
            reject(why, cap, "%s: writes register type %u (only r# and oC0 are supported)",
                   d9_tev_opcode_name(op), dst_type);
            return -1;
        }
        if (dst_type == D9TEV_REG_COLOROUT && dst_index >= 1) {
            reject(why, cap, "%s: MRT write to oC%u is not supported", d9_tev_opcode_name(op), dst_index);
            return -1;
        }

        unsigned nsrc = (unsigned)total - 2u;   /* total = instruction + dst + sources */
        if (nsrc > 3) {
            reject(why, cap, "%s has %u sources", d9_tev_opcode_name(op), nsrc);
            return -1;
        }
        unsigned expected = op == 0x01 ? 1u : op == 0x02 || op == 0x03 || op == 0x05 || op == 0x08 ? 2u : 3u;
        if (nsrc != expected) {
            reject(why, cap, "%s expects %u sources, token has %u",
                   d9_tev_opcode_name(op), expected, nsrc);
            return -1;
        }

        unsigned src_rgb[3], src_a[3], rop[3], aop[3];
        unsigned creg[3][2];
        uint8_t taint[3] = {0, 0, 0};
        for (unsigned i = 0; i < nsrc; ++i) {
            if (map_operand(ctx, arg[1 + i], &src_rgb[i], &rop[i],
                            &src_a[i], &aop[i], creg[i], &taint[i],
                            op == 0x08 ? 0xFu : mask,
                            why, cap, d9_tev_opcode_name(op)) < 0)
                return -1;
        }
        for (unsigned i = nsrc; i < 3; ++i) {
            src_rgb[i] = src_a[i] = D9TEV_SRC_PREV;
            rop[i] = D9TEV_RGB_COLOR;
            aop[i] = D9TEV_A_ALPHA;
            creg[i][0] = creg[i][1] = 0xFFu;
        }

        /* Distinct constants used by this instruction, including a constant
         * reached through a temporary's per-channel provenance. */
        int const_index[3];
        int nconst = 0;
        int const_is_literal[3];
        for (unsigned i = 0; i < nsrc; ++i) {
            for (int side = 0; side < 2; ++side) {
                unsigned source = side ? src_a[i] : src_rgb[i];
                unsigned reg = creg[i][side];
                if (source != D9TEV_SRC_CONST || reg == 0xFFu) continue;
                int seen = 0;
                for (int k = 0; k < nconst; ++k) if (const_index[k] == (int)reg) seen = 1;
                if (!seen && nconst < 3) {
                    const_index[nconst] = (int)reg;
                    DefConst *def = lookup_def(ctx, (int)reg);
                    const_is_literal[nconst] = def ? 1 : 0;
                    ++nconst;
                }
            }
        }
        if (nconst > 2) {
            reject(why, cap, "%s uses %d distinct constants; one TEV stage can "
                   "supply at most two", d9_tev_opcode_name(op), nconst);
            return -1;
        }

        /* If two constants meet in one instruction, materialise one of them
         * as PREVIOUS.  This is only possible when no operand already needs
         * the previous stage's value. */
        int loaded_const = -1;
        if (nconst == 2) {
            for (unsigned i = 0; i < nsrc; ++i) {
                if (src_rgb[i] == D9TEV_SRC_PREV || src_a[i] == D9TEV_SRC_PREV) {
                    reject(why, cap, "%s uses two constants and a previous-stage "
                           "operand; PICA has no third constant slot",
                           d9_tev_opcode_name(op));
                    return -1;
                }
            }
            if (stage_full(ctx, why, cap, d9_tev_opcode_name(op)) < 0) return -1;
            unsigned load_src_rgb[3] = {D9TEV_SRC_CONST, D9TEV_SRC_PREV, D9TEV_SRC_PREV};
            unsigned load_src_a[3] = {D9TEV_SRC_CONST, D9TEV_SRC_PREV, D9TEV_SRC_PREV};
            unsigned load_rop[3] = {D9TEV_RGB_COLOR, D9TEV_RGB_COLOR, D9TEV_RGB_COLOR};
            unsigned load_aop[3] = {D9TEV_A_ALPHA, D9TEV_A_ALPHA, D9TEV_A_ALPHA};
            invalidate_prev(ctx);
            D9TevStage *load = emit_stage(ctx, D9TEV_FUNC_REPLACE, D9TEV_FUNC_REPLACE,
                                          load_src_rgb, load_rop, load_src_a, load_aop);
            load->const_reg = (uint8_t)const_index[0];
            load->const_is_literal = (uint8_t)const_is_literal[0];
            if (const_is_literal[0]) {
                DefConst *def = lookup_def(ctx, const_index[0]);
                load->const_rgba8 = d9_tev_pack_rgba8(def->value);
            }
            loaded_const = const_index[0];
            for (unsigned i = 0; i < nsrc; ++i) {
                if (src_rgb[i] == D9TEV_SRC_CONST && (int)creg[i][0] == loaded_const)
                    src_rgb[i] = D9TEV_SRC_PREV;
                if (src_a[i] == D9TEV_SRC_CONST && (int)creg[i][1] == loaded_const)
                    src_a[i] = D9TEV_SRC_PREV;
            }
        }

        unsigned func_rgb, func_a;
        switch (op) {
        case 0x01: func_rgb = func_a = D9TEV_FUNC_REPLACE; break;
        case 0x05: func_rgb = func_a = D9TEV_FUNC_MODULATE; break;
        case 0x02: func_rgb = func_a = D9TEV_FUNC_ADD; break;
        case 0x03: func_rgb = func_a = D9TEV_FUNC_SUBTRACT; break;
        case 0x04: func_rgb = func_a = D9TEV_FUNC_MULTIPLY_ADD; break;
        case 0x08: func_rgb = func_a = D9TEV_FUNC_DOT3_RGBA; break;
        case 0x12: /* lrp dst, a, b, c == a*c + b*(1-c) */
            func_rgb = func_a = D9TEV_FUNC_INTERPOLATE;
            {
                unsigned t;
                t = src_rgb[1]; src_rgb[1] = src_rgb[2]; src_rgb[2] = t;
                t = rop[1]; rop[1] = rop[2]; rop[2] = t;
                t = src_a[1]; src_a[1] = src_a[2]; src_a[2] = t;
                t = aop[1]; aop[1] = aop[2]; aop[2] = t;
            }
            break;
        default: func_rgb = func_a = D9TEV_FUNC_REPLACE; break;
        }

        int writes_rgb = (mask & 7u) != 0;
        int writes_a = (mask & 8u) != 0;
        int have_prev = p->stages > 0;
        Value old_temp = dst_type == D9TEV_REG_TEMP ? temp_value(ctx, (int)dst_index)
                                                   : empty_value();
        int keep_rgb = 0, keep_a = 0;

        if (op == 0x08) {
            /* DOT3_RGBA replicates the dot into alpha.  For a partial write,
             * override only the untouched side below. */
            func_rgb = mask == 0xFu ? D9TEV_FUNC_DOT3_RGBA : D9TEV_FUNC_DOT3_RGB;
            func_a = D9TEV_FUNC_DOT3_RGBA;
        }

        /* A partial temp write must preserve the old RGB/alpha side.  If that
         * side is still directly addressable, carry it in the same stage; if
         * it is PREVIOUS, REPLACE(PREVIOUS) keeps it live for a later read. */
        if (!writes_rgb) {
            int addressable = old_temp.src[0] != VAL_NONE &&
                              old_temp.src[1] == old_temp.src[0] &&
                              old_temp.src[2] == old_temp.src[0] &&
                              old_temp.src[0] != D9TEV_SRC_CONST;
            int identity = old_temp.comp[0] == 0 && old_temp.comp[1] == 1 &&
                           old_temp.comp[2] == 2;
            int replicated = old_temp.comp[0] == old_temp.comp[1] &&
                             old_temp.comp[1] == old_temp.comp[2];
            if (dst_type == D9TEV_REG_TEMP && addressable && (identity || replicated)) {
                func_rgb = D9TEV_FUNC_REPLACE;
                src_rgb[0] = old_temp.src[0];
                rop[0] = identity ? D9TEV_RGB_COLOR : rgb_selector(old_temp.comp[0], 0);
                keep_rgb = 1;
            } else if (dst_type == D9TEV_REG_COLOROUT && have_prev) {
                func_rgb = D9TEV_FUNC_REPLACE;
                src_rgb[0] = D9TEV_SRC_PREV;
                rop[0] = D9TEV_RGB_COLOR;
                keep_rgb = 1;
            }
        }
        if (!writes_a) {
            if (dst_type == D9TEV_REG_TEMP && old_temp.src[3] != VAL_NONE &&
                old_temp.src[3] != D9TEV_SRC_CONST) {
                func_a = D9TEV_FUNC_REPLACE;
                src_a[0] = old_temp.src[3];
                aop[0] = alpha_selector(old_temp.comp[3], 0);
                keep_a = 1;
            } else if (dst_type == D9TEV_REG_COLOROUT && have_prev) {
                func_a = D9TEV_FUNC_REPLACE;
                src_a[0] = D9TEV_SRC_PREV;
                aop[0] = D9TEV_A_ALPHA;
                keep_a = 1;
            }
        }

        if (stage_full(ctx, why, cap, d9_tev_opcode_name(op)) < 0) return -1;
        D9TevStage *stage = emit_stage(ctx, func_rgb, func_a, src_rgb, rop, src_a, aop);
        advance_prev(ctx, keep_rgb, keep_a);
        if (nconst == 1) {
            stage->const_reg = (uint8_t)const_index[0];
            stage->const_is_literal = (uint8_t)const_is_literal[0];
            if (const_is_literal[0]) {
                DefConst *def = lookup_def(ctx, const_index[0]);
                stage->const_rgba8 = d9_tev_pack_rgba8(def->value);
            }
        } else if (nconst == 2) {
            int remaining = -1;
            for (int k = 0; k < nconst; ++k) if (const_index[k] != loaded_const) remaining = k;
            if (remaining >= 0) {
                stage->const_reg = (uint8_t)const_index[remaining];
                stage->const_is_literal = (uint8_t)const_is_literal[remaining];
                if (const_is_literal[remaining]) {
                    DefConst *def = lookup_def(ctx, const_index[remaining]);
                    stage->const_rgba8 = d9_tev_pack_rgba8(def->value);
                }
            }
        }

        uint8_t result_taint = 0;
        for (unsigned i = 0; i < nsrc; ++i) {
            if (op == 0x08 && (taint[i] & 7u)) result_taint |= 0xFu;
            else result_taint |= taint[i];
        }
        Value result = old_temp;
        for (int c = 0; c < 4; ++c) {
            int written = (mask & (1u << c)) != 0;
            int overwritten = c == 3 ? writes_a : writes_rgb;
            if (written && !(result_taint & (1u << c))) {
                result.src[c] = D9TEV_SRC_PREV;
                result.comp[c] = (uint8_t)c;
                result.const_reg[c] = 0xFFu;
                result.defined |= (uint8_t)(1u << c);
            } else if (written || (overwritten && !((c == 3 ? keep_a : keep_rgb)))) {
                result.src[c] = VAL_NONE;
                result.defined &= (uint8_t)~(1u << c);
            }
        }
        if (dst_type == D9TEV_REG_COLOROUT) {
            have_output = 1;
            output_mask |= (uint8_t)mask;
            if (writes_rgb) output_defined &= (uint8_t)~7u;
            if (writes_a) output_defined &= (uint8_t)~8u;
            output_defined |= (uint8_t)(mask & result.defined);
            output_stage = (int)p->stages - 1;
        } else {
            set_temp(ctx, (int)dst_index, result);
        }
        pos += total;
    }

    if (!have_output) {
        reject(why, cap, "program does not write oC0");
        return -1;
    }
    if ((output_defined & 0x7u) != 0x7u) {
        reject(why, cap, "oC0 RGB depends on an undefined channel (write mask 0x%x, "
               "defined 0x%x)", output_mask, output_defined);
        return -1;
    }
    if (p->stages != (unsigned)output_stage + 1u) {
        reject(why, cap, "code after the final oC0 write would overwrite the "
               "TEV PREVIOUS stage; reorder or reject this permutation");
        return -1;
    }

    /* If alpha was never written, force it to 1 with one extra stage.  The RGB
     * chain is REPLACE(PREV), so the visible colour is unchanged. */
    if (!(output_defined & 0x8u)) {
        if (stage_full(ctx, why, cap, "alpha fixup") < 0) return -1;
        unsigned src_rgb[3] = {D9TEV_SRC_PREV, D9TEV_SRC_PREV, D9TEV_SRC_PREV};
        unsigned src_a[3] = {D9TEV_SRC_PREV, D9TEV_SRC_PREV, D9TEV_SRC_PREV};
        unsigned rop[3] = {D9TEV_RGB_COLOR, D9TEV_RGB_COLOR, D9TEV_RGB_COLOR};
        unsigned aop[3] = {D9TEV_A_ALPHA, D9TEV_A_ALPHA, D9TEV_A_ALPHA};
        D9TevStage *fix = emit_stage(ctx, D9TEV_FUNC_REPLACE, D9TEV_FUNC_REPLACE,
                                     src_rgb, rop, src_a, aop);
        fix->const_reg = 0xFF;
        fix->const_is_literal = 1;
        fix->const_rgba8 = 0xFFFFFFFFu;
        fix->src_a[0] = D9TEV_SRC_CONST;
        fix->op_a[0] = D9TEV_A_ALPHA;
        p->requires_alpha_fixup = 1;
    }
    return 0;
}

int d9_tev_translate_pixel(const uint32_t *code, size_t words,
                           D9TevProgram *out, char *why, size_t why_size) {
    if (!out) return -1;
    if (why && why_size) why[0] = 0;
    memset(out, 0, sizeof *out);
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;
    ctx.stage_constant = 0xFFFFFFFFu;
    ctx.current_stage = -1;
    for (int i = 0; i < 16; ++i) {
        ctx.sampler_type[i] = 0;
        ctx.input_usage[i] = -1;
    }
    int rc = translate(&ctx, code, words, why, why_size);
    if (rc < 0) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

const char *d9_tev_family_name(const D9TevProgram *program) {
    if (!program) return "none";
    if (program->stages == 1 && program->textures <= 1 &&
        program->stage[0].func_rgb == D9TEV_FUNC_REPLACE &&
        program->constants == 0)
        return "blit";
    if (program->textures == 1 && program->constants > 0) return "tint";
    if (program->textures == 0) return "constant";
    return "textured";
}

/* ------------------------------------------------------------------------- */
/* Reference evaluator                                                       */
/* ------------------------------------------------------------------------- */

static void operand_rgb(unsigned src, unsigned op, const float prev[4],
                        const float tex[D9TEV_MAX_TEXTURES][4],
                        const float constant[4], const float primary[4],
                        const float frag_primary[4], const float frag_secondary[4],
                        float out[3]) {
    const float *v;
    switch (src) {
    case D9TEV_SRC_TEX0: case D9TEV_SRC_TEX1: case D9TEV_SRC_TEX2:
        v = tex[src - D9TEV_SRC_TEX0]; break;
    case D9TEV_SRC_CONST: v = constant; break;
    case D9TEV_SRC_PRIMARY: v = primary; break;
    case D9TEV_SRC_FRAG_PRIMARY: v = frag_primary; break;
    case D9TEV_SRC_FRAG_SECONDARY: v = frag_secondary; break;
    default: v = prev; break;
    }
    float one_minus = 0.0f;
    switch (op) {
    case D9TEV_RGB_1M_COLOR: one_minus = 1.0f; /* fallthrough */
    case D9TEV_RGB_COLOR: out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; break;
    case D9TEV_RGB_1M_ALPHA: one_minus = 1.0f; /* fallthrough */
    case D9TEV_RGB_ALPHA: out[0] = out[1] = out[2] = v[3]; break;
    case D9TEV_RGB_1M_R: one_minus = 1.0f; /* fallthrough */
    case D9TEV_RGB_R: out[0] = out[1] = out[2] = v[0]; break;
    case D9TEV_RGB_1M_G: one_minus = 1.0f; /* fallthrough */
    case D9TEV_RGB_G: out[0] = out[1] = out[2] = v[1]; break;
    case D9TEV_RGB_1M_B: one_minus = 1.0f; /* fallthrough */
    case D9TEV_RGB_B: out[0] = out[1] = out[2] = v[2]; break;
    default: out[0] = out[1] = out[2] = 0.0f; break;
    }
    if (one_minus) {
        out[0] = 1.0f - out[0];
        out[1] = 1.0f - out[1];
        out[2] = 1.0f - out[2];
    }
}

static float operand_a(unsigned src, unsigned op, const float prev[4],
                       const float tex[D9TEV_MAX_TEXTURES][4],
                       const float constant[4], const float primary[4],
                       const float frag_primary[4], const float frag_secondary[4]) {
    const float *v;
    switch (src) {
    case D9TEV_SRC_TEX0: case D9TEV_SRC_TEX1: case D9TEV_SRC_TEX2:
        v = tex[src - D9TEV_SRC_TEX0]; break;
    case D9TEV_SRC_CONST: v = constant; break;
    case D9TEV_SRC_PRIMARY: v = primary; break;
    case D9TEV_SRC_FRAG_PRIMARY: v = frag_primary; break;
    case D9TEV_SRC_FRAG_SECONDARY: v = frag_secondary; break;
    default: v = prev; break;
    }
    float value;
    switch (op) {
    case D9TEV_A_ALPHA: case D9TEV_A_1M_ALPHA: value = v[3]; break;
    case D9TEV_A_R: case D9TEV_A_1M_R: value = v[0]; break;
    case D9TEV_A_G: case D9TEV_A_1M_G: value = v[1]; break;
    default: value = v[2]; break;
    }
    if (op & 1u) value = 1.0f - value;
    return value;
}

static float clamp01(float v) {
    if (!(v > 0.0f)) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float combine(unsigned func, float o1, float o2, float o3) {
    switch (func) {
    case D9TEV_FUNC_REPLACE: return o1;
    case D9TEV_FUNC_MODULATE: return o1 * o2;
    case D9TEV_FUNC_ADD: return o1 + o2;
    case D9TEV_FUNC_ADD_SIGNED: return o1 + o2 - 0.5f;
    case D9TEV_FUNC_INTERPOLATE: return o1 * o2 + o3 * (1.0f - o2);
    case D9TEV_FUNC_SUBTRACT: return o1 - o2;
    case D9TEV_FUNC_MULTIPLY_ADD: return o1 * o2 + o3;
    case D9TEV_FUNC_ADD_MULTIPLY: return (o1 + o2) * o3;
    default: return o1;
    }
}

int d9_tev_eval_pixel_varyings(const D9TevProgram *program,
                               const float tex[D9TEV_MAX_TEXTURES][4],
                               const float stage_const[D9TEV_MAX_STAGES][4],
                               const float primary[4],
                               const float frag_primary[4],
                               const float frag_secondary[4],
                               float out[4]) {
    if (!program || !out) return -1;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!primary) primary = zero;
    if (!frag_primary) frag_primary = zero;
    if (!frag_secondary) frag_secondary = zero;
    float prev[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (unsigned s = 0; s < program->stages; ++s) {
        const D9TevStage *stage = &program->stage[s];
        float constant[4];
        if (stage->const_is_literal) {
            for (int c = 0; c < 4; ++c)
                constant[c] = (float)((stage->const_rgba8 >> (8u * (unsigned)c)) & 0xFFu) / 255.0f;
        } else {
            memcpy(constant, stage_const[s], sizeof constant);
        }
        float o_rgb[3][3], o_a[3];
        for (int i = 0; i < 3; ++i) {
            operand_rgb(stage->src_rgb[i], stage->op_rgb[i], prev, tex,
                        constant, primary, frag_primary, frag_secondary, o_rgb[i]);
            o_a[i] = operand_a(stage->src_a[i], stage->op_a[i], prev, tex,
                               constant, primary, frag_primary, frag_secondary);
        }
        float value[4];
        if (stage->func_rgb == D9TEV_FUNC_DOT3_RGB || stage->func_rgb == D9TEV_FUNC_DOT3_RGBA) {
            float dot = o_rgb[0][0] * o_rgb[1][0] + o_rgb[0][1] * o_rgb[1][1] +
                        o_rgb[0][2] * o_rgb[1][2];
            value[0] = value[1] = value[2] = dot;
            if (stage->func_rgb == D9TEV_FUNC_DOT3_RGBA) value[3] = dot;
            else value[3] = o_a[0];
        } else {
            for (int c = 0; c < 3; ++c)
                value[c] = combine(stage->func_rgb, o_rgb[0][c], o_rgb[1][c], o_rgb[2][c]);
            value[3] = combine(stage->func_a, o_a[0], o_a[1], o_a[2]);
        }
        float scale_rgb = stage->scale_rgb == D9TEV_SCALE_4 ? 4.0f
                        : stage->scale_rgb == D9TEV_SCALE_2 ? 2.0f : 1.0f;
        float scale_a = stage->scale_a == D9TEV_SCALE_4 ? 4.0f
                      : stage->scale_a == D9TEV_SCALE_2 ? 2.0f : 1.0f;
        for (int c = 0; c < 3; ++c) prev[c] = clamp01(value[c] * scale_rgb);
        prev[3] = clamp01(value[3] * scale_a);
    }
    memcpy(out, prev, sizeof prev);
    return 0;
}

int d9_tev_eval_pixel(const D9TevProgram *program,
                      const float tex[D9TEV_MAX_TEXTURES][4],
                      const float stage_const[D9TEV_MAX_STAGES][4],
                      const float primary[4],
                      float out[4]) {
    return d9_tev_eval_pixel_varyings(program, tex, stage_const, primary,
                                      NULL, NULL, out);
}
