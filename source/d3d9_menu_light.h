#pragma once
/* Pre-specialised simple-lit menu/logo pipeline prototype.
 *
 * Two retail instruction signatures are recognised structurally (no bytecode
 * blob or hash template):
 *   P5_MAD_NORMAL      840-byte program, normal = xzyw*2-1
 *   P1200005_ADD_NORMAL 852-byte program, normal = (xzyw-0.5)*2
 *
 * The exact CPU evaluator follows those instructions.  The five-stage TEV plan
 * is a deliberately pre-baked menu variant: normal-map channel order and the
 * tangent basis transform are asset/vertex work, ambient+emissive is folded
 * into one varying, and c4.a carries material alpha.  The final stage
 * multiplies secondary RGB by c7.a with alpha passthrough, matching the retail
 * evaluator; c7.rgb is not used.  It is not offered by the generic translator
 * and must not be selected unless those preconditions hold.
 */

#include <stdint.h>
#include <stddef.h>
#include "d3d9_tev.h"

enum {
    D9_MENU_LIGHT_NONE = 0,
    D9_MENU_LIGHT_P5_MAD_NORMAL = 1,
    D9_MENU_LIGHT_P1200005_ADD_NORMAL = 2,
};

typedef struct {
    int signature;
    unsigned instructions;
    float normal_decode[4];       /* literal c8 */
} D9MenuLightMatch;

typedef struct {
    float diffuse[4];
    float normal_raw[4];
    float vertex_color[4];        /* v1 */
    float secondary_color[4];     /* v2 */
    float c0[4],c1[4],c2[4];      /* tangent basis rows */
    float c3[4];                  /* light direction */
    float c4[4];                  /* light colour */
    float c5[4];                  /* material data; alpha scale is c5.z */
    float c6[4];                  /* emissive */
    float c7[4];                  /* output tint/fade; RGB factor is c7.a */
} D9MenuLightInputs;

typedef struct {
    float rgba[4];
    float ndotl;
    float transformed_normal[3];
    float lighting[3];
    float vertex_modulated[3];
} D9MenuLightResult;

/* Structural signature recognition. Returns 1 on match, 0 otherwise; when why
 * is non-NULL it receives a named mismatch reason. */
int d9_menu_light_match(const uint32_t *code,size_t words,D9MenuLightMatch *out,
                        char *why,size_t why_size);
const char *d9_menu_light_signature_name(int signature);

/* Exact CPU reference for the two accepted instruction signatures. */
int d9_menu_light_eval_exact(const D9MenuLightMatch *signature,
                             const D9MenuLightInputs *in,D9MenuLightResult *out);

/* Pre-baked five-stage TEV plan. Inputs use the same struct, but the caller
 * must satisfy the preconditions in the file comment. */
int d9_menu_light_plan(const D9MenuLightMatch *signature,D9TevProgram *out,
                       char *why,size_t why_size);
int d9_menu_light_eval_plan(const D9TevProgram *plan,
                            const D9MenuLightInputs *in,D9MenuLightResult *out);
