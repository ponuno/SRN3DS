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
#define D9_SHADER_PLAN_MAX_ENTRIES 64u
/* Derived pixel-shader constants: a plan-level program that builds registers
 * the guest never uploads (for example c0.zwzw) from the c0..c7 bank. */
#define D9_SHADER_PLAN_MAX_CONST_OPS 8u
#define D9_SHADER_PLAN_MAX_CONST_SRC 3u
#define D9_SHADER_PLAN_CONST_REGS 8u
/* Multi-pass: one draw call re-submits the same geometry through 2..4 TEV
 * programs with per-pass blend/depth state.  Six hardware stages per pass. */
#define D9_SHADER_PLAN_MAX_PASSES 4u

enum {
    D9_SHADER_PLAN_STAGE_PS = 0,
    D9_SHADER_PLAN_STAGE_VS = 1
};

enum {
    D9_SHADER_PLAN_TEV = 1,
    D9_SHADER_PLAN_PICA_VS = 2
};

/* constant_ops operator.  `pack` copies one swizzled source, `add`/`mul`
 * combine two componentwise, and `lerp` is src0*(1-t)+src1*t with t taken from
 * src2 componentwise. */
enum {
    D9_SHADER_PLAN_CONST_PACK = 0,
    D9_SHADER_PLAN_CONST_ADD = 1,
    D9_SHADER_PLAN_CONST_MUL = 2,
    D9_SHADER_PLAN_CONST_LERP = 3
};

typedef struct {
    uint8_t reg;                      /* source D3D c# (0..7) */
    uint8_t swizzle[4];               /* per-lane x/y/z/w selector (0..3) */
    uint8_t negate;                   /* 1: subtract the swizzled source */
} D9ShaderPlanConstSrc;

typedef struct {
    uint8_t dst_reg;                  /* destination D3D c# (0..7) */
    uint8_t op;                       /* D9_SHADER_PLAN_CONST_* */
    uint8_t src_count;
    D9ShaderPlanConstSrc src[D9_SHADER_PLAN_MAX_CONST_SRC];
} D9ShaderPlanConstOp;

typedef struct {
    D9TevProgram tev;                 /* at most six stages and three textures */
    uint8_t blend;                    /* D3DRS_ALPHABLENDENABLE 0/1 */
    uint8_t src_blend, dst_blend;     /* D3DBLEND 1..11 */
    uint8_t blend_op;                 /* D3DBLENDOP 1..5 */
    uint8_t depth_write;              /* D3DRS_ZWRITEENABLE 0/1 */
} D9ShaderPlanPass;

typedef struct D9ShaderPlan {
    const char *id;
    const char *sha256;               /* lower-case hex, 64 chars */
    uint8_t stage;                    /* D9_SHADER_PLAN_STAGE_* */
    uint8_t kind;                     /* D9_SHADER_PLAN_* */
    uint8_t draw_enabled;             /* 0: named refusal at draw */
    const char *note;                 /* why it is blocked / what it needs */
    D9TevProgram tev;                 /* kind == TEV only; zero otherwise */
    /* Optional plan-level derived registers (docs/SHADER_WORKAROUNDS.md). */
    uint8_t const_op_count;
    D9ShaderPlanConstOp const_op[D9_SHADER_PLAN_MAX_CONST_OPS];
    /* Optional multi-pass program.  pass_count == 0 keeps the single-pass
     * `tev` behaviour; pass[0] mirrors `tev` when present. */
    uint8_t pass_count;
    D9ShaderPlanPass pass[D9_SHADER_PLAN_MAX_PASSES];
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

/* Evaluate the plan's derived constant program against the raw c0..c7 bank.
 * `src_mask` has one bit per uploaded register (d9_ps_constants_f_mask).
 * `out` must hold D9_SHADER_PLAN_CONST_REGS rows; *out_mask receives the
 * written bits.  Returns 0, or -1 with a named reason when a source register
 * was never uploaded, a swizzle is malformed, or a result is not finite. */
int d9_shader_plan_build_constants(const D9ShaderPlan *plan,
                                   const float (*src)[4], uint32_t src_mask,
                                   float (*out)[4], uint32_t *out_mask,
                                   char *why, size_t why_size);

/* Hex comparison that accepts upper/lower case.  1 equal, 0 different. */
int d9_shader_plan_sha256_equal(const char *a, const char *b);

#ifdef D9_SHADER_PLAN_TEST_HOOKS
/* Test-only: route lookup at a caller-owned table so a host test can exercise
 * the exact-hash create path without retail bytecode.  The generated table is
 * unchanged and production builds never compile this seam.  Passing NULL/0
 * restores the generated table. */
int d9_shader_plan_test_set(const D9ShaderPlan *entries, unsigned count);
#endif

/* Per-family shader packs (source/shader_packs/<pack>/manifest.json).
 *
 * Generated by tools/shader_packs.py from the census, append-only: a committed
 * row is never rewritten, new packs are appended.  This is a coverage index,
 * not a capability flag: it says how many hashes a family owns and how many
 * are translated / draw-enabled / refused.  It never changes GetDeviceCaps
 * and never enables a draw. */
typedef struct {
    const char *pack;
    const char *owner;
    unsigned chunks;
    unsigned unique_hashes;
    unsigned translated;
    unsigned draw_enabled;
    unsigned refused;
} D9ShaderPackRow;

unsigned d9_shader_pack_count(void);
const D9ShaderPackRow *d9_shader_pack_at(unsigned index);

/* Emitted PICA VS assembly table (docs/generated_vs.json, append-only). *
 * These helpers answer only "does an exact-hash .v.pica exist?".  They do not
 * certify equivalence and do not change caps or the draw gate.  Wire them into
 * CreateVertexShader only together with a real shader-binary upload path and
 * an exact-hash descriptor match; a miss must stay on the legacy path. */
unsigned d9_generated_vs_count(void);
const char *d9_generated_vs_id_at(unsigned index);
/* 1 hit (and sets *id/*asm_name when non-NULL), 0 miss, -1 bad argument. */
int d9_generated_vs_lookup(const char *sha256_hex,
                           const char **id, const char **asm_name);
