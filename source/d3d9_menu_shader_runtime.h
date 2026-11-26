#pragma once
/* Runtime selection of the specialised first-menu TEV programs.
 *
 * This is the small runtime layer between the certified menu closure
 * (`d3d9_menu_closure.h`) and the host-side TEV planners
 * (`d3d9_tev.h`, `d3d9_menu_light.h`).  It never reports a D3D9 capability:
 * `GetDeviceCaps` stays VS/PS 2.0 with zero SM3 slots, and the engine's
 * level-7 branch is still gated by the separate non-D3D9 closure predicate.
 *
 * Selection rules:
 *
 *   - Bytecode-backed entries are accepted only by their exact certified
 *     SHA-256 and then re-checked structurally.  A structural family match
 *     alone (for example p5 or a generic blit) is not enough.
 *   - The logo entry must additionally match the `p1200005_ADD_NORMAL`
 *     instruction signature and produce the five-stage menu-light plan.
 *   - The copy entry must additionally translate to the exact one-stage
 *     REPLACE(TEXTURE0) plan.
 *   - Fixed fog/tint have no shipped `.fxp`.  They are built only when the
 *     caller presents the explicit fixed-material precondition mask; missing
 *     bits are a named refusal.
 *   - Nothing is built "half way": on failure the output match is zeroed.
 *
 * A selected bytecode plan may still be blocked at draw time by unmet
 * material preconditions or by a backend that cannot yet execute the plan
 * (two texture units, vertex colour, external constants).  The caller must
 * refuse that draw by name; it must not partially submit it.
 */

#include <stddef.h>
#include <stdint.h>

#include "d3d9_tev.h"

#define D9_MENU_SHADER_ID_MAX 32u
#define D9_MENU_SHADER_REASON_MAX 224u

typedef enum {
    D9_MENU_SHADER_NONE = 0,
    D9_MENU_SHADER_LOGO_PS = 1,      /* p1200005 exact closure hash */
    D9_MENU_SHADER_COMPOSITE_PS = 2, /* menu copy exact closure hash */
    D9_MENU_SHADER_FOG_FIXED = 3,    /* menu.fog.fixed, no bytecode */
    D9_MENU_SHADER_TINT_FIXED = 4    /* fixed texture*vertex-colour tint */
} D9MenuShaderKind;

/* Material/backend contracts.  These are not D3D9 caps. */
enum {
    D9_MENU_SHADER_PRE_NORMAL_PREBAKED = 1u << 0,
    D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED = 1u << 1,
    D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING = 1u << 2,
    D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4 = 1u << 3,
    D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7 = 1u << 4,
    D9_MENU_SHADER_PRE_LDR_QUALITY = 1u << 5,
    D9_MENU_SHADER_PRE_TEXTURE0_2D = 1u << 6,
    D9_MENU_SHADER_PRE_TEXTURE1_2D = 1u << 7,
    D9_MENU_SHADER_PRE_VERTEX_COLOR = 1u << 8
};

typedef struct {
    int kind;                             /* D9MenuShaderKind */
    int entry;                            /* D9_MENU_ENTRY_* */
    uint32_t required_preconditions;      /* D9_MENU_SHADER_PRE_* */
    D9TevProgram plan;                    /* complete or zeroed */
    char id[D9_MENU_SHADER_ID_MAX];
} D9MenuShaderMatch;

/* Match a ps_3_0 stream against the two certified bytecode menu entries.
 * Returns 1 and fills `out` for menu.logo.ps / menu.composite.ps, 0 when the
 * stream is outside the two-entry menu set (the legacy path may still decide),
 * and -1 with a named reason when it is a certified entry whose specialised
 * plan cannot be built.  `out` is always zeroed first. */
int d9_menu_shader_match_ps(const uint32_t *code, size_t words,
                            D9MenuShaderMatch *out,
                            char *why, size_t why_size);

/* Build a fixed fog/tint plan.  `preconditions` is the caller's evidence that
 * the fixed material contract is actually prepared.  Returns 1 on success,
 * -1 with a named reason (including the missing bits) otherwise.  These
 * entries are not selected from bytecode. */
int d9_menu_shader_match_fixed(int kind, uint32_t preconditions,
                               D9MenuShaderMatch *out,
                               char *why, size_t why_size);

/* Number of required bits absent from `have`.  Writes a comma-separated list
 * of names into `why` (may be NULL/0). */
int d9_menu_shader_missing_preconditions(const D9MenuShaderMatch *match,
                                         uint32_t have,
                                         char *why, size_t why_size);

/* Process-wide material evidence.  Reset at device creation/reset.  Selecting
 * a fixed plan does not set these bits; the caller that really prepares the
 * material must report them. */
void d9_menu_shader_runtime_reset(void);
void d9_menu_shader_note_preconditions(uint32_t mask);
uint32_t d9_menu_shader_preconditions(void);

const char *d9_menu_shader_kind_name(int kind);
const char *d9_menu_shader_precondition_name(uint32_t bit);
