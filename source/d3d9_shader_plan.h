#pragma once
/* Native shader-plan table generated offline by tools/fxp_compile.py.
 *
 * Each entry maps a bytecode SHA-256 to a fully materialised PICA200
 * descriptor: a TEV program (stages/textures/constants) or a vertex-shader
 * contract.  Selection is by exact hash only; there is no structural
 * fallback and no "close plan".
 *
 * This module never reports a D3D9 capability.  `GetDeviceCaps` stays
 * VS/PS 2.0 and DevCaps 0x00082000.  A plan with `draw_enabled == 0` is
 * discoverable for diagnostics but must not be submitted: the draw gate
 * returns D3DERR_NOTAVAILABLE with the plan's `note` as the named reason.
 *
 * The include is generated from docs/shader_plans.json and the host test
 * regenerates it byte for byte; do not edit the include by hand.
 */

#include <stddef.h>
#include <stdint.h>

#include "d3d9_tev.h"

#define D9_SHADER_PLAN_SHA256 65u
#define D9_SHADER_PLAN_NOTE 160u
#define D9_SHADER_PLAN_MAX_ENTRIES 16u

enum {
    D9_SHADER_PLAN_STAGE_PS = 0,
    D9_SHADER_PLAN_STAGE_VS = 1
};

enum {
    D9_SHADER_PLAN_TEV = 1,
    D9_SHADER_PLAN_PICA_VS = 2
};

typedef struct {
    const char *id;
    const char *sha256;               /* lower-case hex, 64 chars */
    uint8_t stage;                    /* D9_SHADER_PLAN_STAGE_* */
    uint8_t kind;                     /* D9_SHADER_PLAN_* */
    uint8_t draw_enabled;             /* 0: named refusal at draw */
    const char *note;                 /* why it is blocked / what it needs */
    D9TevProgram tev;                 /* kind == TEV only; zero otherwise */
} D9ShaderPlan;

/* Table bounds. */
unsigned d9_shader_plan_count(void);
const D9ShaderPlan *d9_shader_plan_at(unsigned index);

/* Exact-hash lookup.  `stage` is D9_SHADER_PLAN_STAGE_PS/VS.  Returns 1 and
 * sets *out on a hit, 0 when no compiled plan exists (with a named reason),
 * and -1 for invalid arguments.  A found plan may still be draw-disabled. */
int d9_shader_plan_lookup(unsigned stage, const char *sha256_hex,
                          const D9ShaderPlan **out,
                          char *why, size_t why_size);

/* Draw gate.  Returns 0 when the plan is draw-enabled and internally
 * consistent, -1 otherwise with a named reason in `why`.  A PICA VS
 * descriptor is never draw-enabled until a native code emitter exists. */
int d9_shader_plan_draw_allowed(const D9ShaderPlan *plan,
                                char *why, size_t why_size);

/* Hex comparison that accepts upper/lower case.  1 equal, 0 different. */
int d9_shader_plan_sha256_equal(const char *a, const char *b);
