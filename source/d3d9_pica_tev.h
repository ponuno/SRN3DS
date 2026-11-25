#pragma once
/* Honest PICA200 backend subset for translated D3D9 ps_3_0 programs.
 *
 * This module deliberately does not include citro3d or libctru headers.  It
 * validates D9TevProgram and emits a small, host-testable descriptor that the
 * 3DS adapter can copy directly into C3D_TexEnv.
 *
 * Accepted backend subset:
 *   - 1..6 TEV stages;
 *   - zero or one 2D texture, always sampler 0 / texcoord 0;
 *   - literal (def c#) constants only, quantised by the translator to RGBA8;
 *   - no vertex colour, fragment-colour varying, VPOS/VFACE, or other PS input.
 *
 * A NULL descriptor output performs validation only.  On failure the descriptor
 * is reset to an empty program and the convenience path zeroes its translated
 * output.  The input program is never modified or retained.
 */

#include <stddef.h>
#include <stdint.h>

#include "d3d9_tev.h"

#define D9_PICA_TEV_MAX_STAGES D9TEV_MAX_STAGES
#define D9_PICA_TEV_NO_SAMPLER 0xFFu

typedef enum {
    D9_PICA_TEV_OK = 0,
    D9_PICA_TEV_NULL_PROGRAM,
    D9_PICA_TEV_PROGRAM_VERSION,
    D9_PICA_TEV_MALFORMED,
    D9_PICA_TEV_PROGRAM_COUNT,
    D9_PICA_TEV_TEXTURE_COUNT,
    D9_PICA_TEV_SAMPLER,
    D9_PICA_TEV_SAMPLER_TYPE,
    D9_PICA_TEV_STAGE_COUNT,
    D9_PICA_TEV_EXTERNAL_CONSTANT,
    D9_PICA_TEV_VERTEX_COLOR,
    D9_PICA_TEV_FRAGMENT_VARYING,
    D9_PICA_TEV_VPOS,
    D9_PICA_TEV_INPUT_USAGE,
    D9_PICA_TEV_INPUT_COUNT,
    D9_PICA_TEV_TEV_SOURCE,
    D9_PICA_TEV_TEXTURE_SOURCE,
    D9_PICA_TEV_TEV_OPERAND,
    D9_PICA_TEV_TEV_FUNCTION,
    D9_PICA_TEV_TEV_SCALE,
    D9_PICA_TEV_TEV_CONSTANT,
    D9_PICA_TEV_TRANSLATION,
    D9_PICA_TEV_REJECT_COUNT
} D9PicaTevReject;

typedef struct {
    uint8_t src_rgb[3];
    uint8_t src_a[3];
    uint8_t op_rgb[3];
    uint8_t op_a[3];
    uint8_t func_rgb;
    uint8_t func_a;
    uint8_t scale_rgb;
    uint8_t scale_a;
    uint8_t uses_constant;
    /* PICA TEV constants are ABGR8888, not the translator's RGBA8 packing. */
    uint32_t constant_abgr;
} D9PicaTevStage;

typedef struct {
    uint8_t stage_count;
    uint8_t texture_count;       /* 0 or 1 */
    uint8_t sampler_reg;         /* 0, or D9_PICA_TEV_NO_SAMPLER */
    uint8_t texcoord_reg;        /* 0, or D9_PICA_TEV_NO_SAMPLER */
    D9PicaTevStage stage[D9_PICA_TEV_MAX_STAGES];
} D9PicaTevProgram;

/* Initialise a zeroed descriptor with explicit absent-texture markers. */
void d9_pica_tev_program_init(D9PicaTevProgram *program);

/* Validate an already translated program and optionally emit a descriptor. */
int d9_pica_tev_validate(const D9TevProgram *program,
                         D9PicaTevProgram *descriptor,
                         D9PicaTevReject *reason,
                         char *why, size_t why_size);

/* Name unsupported raw declarations (notably VPOS and vertex colour) without
 * translating.  Returns 0 for a ps_3_0 token stream that may enter translation. */
int d9_pica_tev_preflight_pixel(const uint32_t *code, size_t words,
                                D9PicaTevReject *reason,
                                char *why, size_t why_size);

/* ps_3_0 convenience path: name unsupported declarations (notably VPOS and
 * vertex colour), run the existing translator, then apply the backend gate. */
int d9_pica_tev_validate_pixel(const uint32_t *code, size_t words,
                               D9TevProgram *translated,
                               D9PicaTevProgram *descriptor,
                               D9PicaTevReject *reason,
                               char *why, size_t why_size);

const char *d9_pica_tev_reject_name(D9PicaTevReject reason);

/* Convert translator RGBA8 (R in bits 0..7) to PICA ABGR8888. */
uint32_t d9_pica_tev_color_abgr(uint32_t rgba8);
