#pragma once
/* D3D9 SM3 pixel-shader -> PICA200 TEV translation subset.
 *
 * This module is deliberately independent of ``d3d9_shader.c``.  The shader
 * module keeps its two exact SM2 templates and its diagnostics; when it sees a
 * ps_3_0 chunk it can call ``d9_tev_translate_pixel`` and, on success, replace
 * the unknown-kind rejection with a real TEV program.
 *
 * The emitted stage descriptors use the same numeric encodings as libctru's
 * ``GPU_TEVSRC`` / ``GPU_TEVOP_*`` / ``GPU_COMBINEFUNC`` / ``GPU_TEVSCALE``
 * enums (see <3ds/gpu/enums.h>), so a citro3d backend can copy them into
 * ``C3D_TexEnv`` without a translation table.  They are declared locally so
 * this file also builds for the host tests without devkitPro.
 *
 * Scope: the first family is the straight-line, unlit textured pass (blit /
 * UI tint / simple effect).  Anything outside the subset is rejected with the
 * offending opcode's name in ``why``.  Literal ``def`` components must be
 * finite and in [0,1] because PICA stage constants are LDR RGBA8 values.
 * See docs/SHADERS.md section 5.
 */

#include <stdint.h>
#include <stddef.h>

#define D9TEV_MAX_STAGES 6
#define D9TEV_MAX_TEXTURES 3
#define D9TEV_MAX_INPUTS 16
#define D9TEV_MAX_CONSTANTS 8
#define D9TEV_MAX_DEFS 8

/* GPU_TEVSRC */
enum {
    D9TEV_SRC_PRIMARY        = 0x00,
    D9TEV_SRC_FRAG_PRIMARY   = 0x01,
    D9TEV_SRC_FRAG_SECONDARY = 0x02,
    D9TEV_SRC_TEX0           = 0x03,
    D9TEV_SRC_TEX1           = 0x04,
    D9TEV_SRC_TEX2           = 0x05,
    D9TEV_SRC_CONST          = 0x0E,
    D9TEV_SRC_PREV           = 0x0F,
};

/* GPU_TEVOP_RGB */
enum {
    D9TEV_RGB_COLOR    = 0x00,
    D9TEV_RGB_1M_COLOR = 0x01,
    D9TEV_RGB_ALPHA    = 0x02,
    D9TEV_RGB_1M_ALPHA = 0x03,
    D9TEV_RGB_R        = 0x04,
    D9TEV_RGB_1M_R     = 0x05,
    D9TEV_RGB_G        = 0x08,
    D9TEV_RGB_1M_G     = 0x09,
    D9TEV_RGB_B        = 0x0C,
    D9TEV_RGB_1M_B     = 0x0D,
};

/* GPU_TEVOP_A */
enum {
    D9TEV_A_ALPHA    = 0x00,
    D9TEV_A_1M_ALPHA = 0x01,
    D9TEV_A_R        = 0x02,
    D9TEV_A_1M_R     = 0x03,
    D9TEV_A_G        = 0x04,
    D9TEV_A_1M_G     = 0x05,
    D9TEV_A_B        = 0x06,
    D9TEV_A_1M_B     = 0x07,
};

/* GPU_COMBINEFUNC */
enum {
    D9TEV_FUNC_REPLACE      = 0,
    D9TEV_FUNC_MODULATE     = 1,
    D9TEV_FUNC_ADD          = 2,
    D9TEV_FUNC_ADD_SIGNED   = 3,
    D9TEV_FUNC_INTERPOLATE  = 4,
    D9TEV_FUNC_SUBTRACT     = 5,
    D9TEV_FUNC_DOT3_RGB     = 6,
    D9TEV_FUNC_DOT3_RGBA    = 7,
    D9TEV_FUNC_MULTIPLY_ADD = 8,
    D9TEV_FUNC_ADD_MULTIPLY = 9,
};

/* GPU_TEVSCALE */
enum { D9TEV_SCALE_1 = 0, D9TEV_SCALE_2 = 1, D9TEV_SCALE_4 = 2 };

/* D3D9 register types after the SM2 split. */
enum {
    D9TEV_REG_TEMP = 0, D9TEV_REG_INPUT = 1, D9TEV_REG_CONST = 2,
    D9TEV_REG_COLOROUT = 8, D9TEV_REG_DEPTHOUT = 9, D9TEV_REG_SAMPLER = 10,
};

/* D3D9 declaration usages we care about. */
enum {
    D9TEV_USAGE_POSITION = 0, D9TEV_USAGE_BLENDWEIGHT = 1,
    D9TEV_USAGE_BLENDINDICES = 2, D9TEV_USAGE_NORMAL = 3,
    D9TEV_USAGE_TEXCOORD = 5, D9TEV_USAGE_COLOR = 10,
};

typedef struct {
    uint8_t func_rgb, func_a;     /* D9TEV_FUNC_* */
    uint8_t src_rgb[3], src_a[3]; /* D9TEV_SRC_* */
    uint8_t op_rgb[3], op_a[3];   /* D9TEV_RGB_* / D9TEV_A_* */
    uint8_t scale_rgb, scale_a;   /* D9TEV_SCALE_* */
    uint8_t const_reg;            /* D3D c# supplying D9TEV_SRC_CONST, or 0xFF */
    uint8_t const_is_literal;     /* 1: use const_rgba8, 0: upload from c# */
    uint32_t const_rgba8;         /* literal constant packed RGBA8 */
} D9TevStage;

typedef struct {
    uint8_t usage;                /* D9TEV_USAGE_* */
    uint8_t usage_index;
    uint8_t reg_index;
    uint8_t tev_source;           /* D9TEV_SRC_PRIMARY/FRAG_PRIMARY/FRAG_SECONDARY, 0xFF = texcoord only */
} D9TevInput;

typedef struct {
    uint8_t texcoord_reg;         /* v#/t# supplying the UV */
    uint8_t sampler_reg;          /* s# bound to this unit */
    uint8_t sampler_type;         /* 2 = 2D */
} D9TevTexture;

typedef struct {
    uint32_t version;             /* raw ps_3_0 token */
    unsigned instructions;
    unsigned stages;
    D9TevStage stage[D9TEV_MAX_STAGES];
    unsigned textures;
    D9TevTexture texture[D9TEV_MAX_TEXTURES];
    unsigned inputs;
    D9TevInput input[D9TEV_MAX_INPUTS];
    unsigned constants;           /* distinct external D3D c# referenced */
    uint8_t constant_reg[D9TEV_MAX_CONSTANTS];
    unsigned uses_vertex_color;
    unsigned requires_alpha_fixup;/* final alpha was undefined and forced to 1 */
    unsigned stage_limit;         /* 6: PICA200 hardware limit */
} D9TevProgram;

/* Translate one ps_3_0 program.
 * Returns 0 on success and fills *out.  Returns -1 and writes a human-readable
 * reason (including the opcode name when an instruction is unsupported) into
 * why when the program is outside the subset.  why may be NULL/0. */
int d9_tev_translate_pixel(const uint32_t *code, size_t words,
                           D9TevProgram *out, char *why, size_t why_size);

/* Evaluate a translated program in software.  tex[unit][rgba] are the sampled
 * texels, stage_const[stage][rgba] the resolved per-stage constants (literal
 * constants are already expanded by the caller from stage.const_rgba8).
 * This is used by the host accuracy tests and by frame-debug tooling; the
 * port itself uploads the descriptors to the PICA200. */
int d9_tev_eval_pixel(const D9TevProgram *program,
                      const float tex[D9TEV_MAX_TEXTURES][4],
                      const float stage_const[D9TEV_MAX_STAGES][4],
                      const float primary[4],
                      float out[4]);
/* Full evaluator including the two extra PICA fragment-colour varyings used
 * when a ps_3_0 input register is routed through FRAG_PRIMARY/SECONDARY. */
int d9_tev_eval_pixel_varyings(const D9TevProgram *program,
                               const float tex[D9TEV_MAX_TEXTURES][4],
                               const float stage_const[D9TEV_MAX_STAGES][4],
                               const float primary[4],
                               const float frag_primary[4],
                               const float frag_secondary[4],
                               float out[4]);

/* "texld", "cmp", ... or "unknown". */
const char *d9_tev_opcode_name(unsigned opcode);

/* Short family name of an accepted program: "blit", "tint", "textured". */
const char *d9_tev_family_name(const D9TevProgram *program);

/* Pack an in-range float literal the way PICA's per-stage TEV colour does
 * (RGBA8).  This utility clamps for callers with already-validated LDR data;
 * the translator rejects non-finite/out-of-range def literals before packing. */
uint32_t d9_tev_pack_rgba8(const float rgba[4]);
