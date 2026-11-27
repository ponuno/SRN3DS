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
#include "d3d9_vertex.h"

#define D9_MENU_SHADER_ID_MAX 32u
#define D9_MENU_SHADER_REASON_MAX 224u
#define D9_MENU_SHADER_MAX_INPUTS 8u
#define D9_MENU_FXP_KEY_ANY UINT32_C(0xffffffff)

typedef enum {
    D9_MENU_SHADER_NONE = 0,
    D9_MENU_SHADER_LOGO_PS = 1,      /* p1200005 exact closure hash */
    D9_MENU_SHADER_COMPOSITE_PS = 2, /* menu copy exact closure hash */
    D9_MENU_SHADER_FOG_FIXED = 3,    /* menu.fog.fixed, no bytecode */
    D9_MENU_SHADER_TINT_FIXED = 4,   /* fixed texture*vertex-colour tint */
    D9_MENU_SHADER_GENERIC_VS = 5    /* bounded static menu VS family */
} D9MenuShaderKind;

/* Output semantics a certified menu VS must declare.  These are D3DDECLUSAGE
 * bits in a compact mask so a test or diagnostics line can name the gap. */
enum {
    D9_MENU_VS_OUT_POSITION0 = 1u << 0,
    D9_MENU_VS_OUT_TEXCOORD0 = 1u << 1,
    D9_MENU_VS_OUT_COLOR0 = 1u << 2,
    D9_MENU_VS_OUT_COLOR1 = 1u << 3
};

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
    /* VS-only structural contract: exact input semantics read by the program
     * and the output semantics it declares.  Zero for PS/fixed entries. */
    uint32_t input_count;
    uint8_t input_usage[D9_MENU_SHADER_MAX_INPUTS];
    uint8_t input_index[D9_MENU_SHADER_MAX_INPUTS];
    uint32_t output_semantic_mask;        /* D9_MENU_VS_OUT_* */
    /* VS-only: 1 when the program's visible 2D outputs (position0, texcoord0,
     * color0) are exactly the fixed CPU/COM transform the core already applies
     * (dp4 c0..c3 * position, texcoord passthrough, colour passthrough).
     * Extra logo-lighting work (normal input, color1) is then provably not on
     * the position/UV/color0 path and is ignored.  Zero means the backend
     * cannot execute the member and the draw stays a named refusal. */
    uint32_t backend_cpu_safe;
    /* Extra fixed-path state needed by a CPU-safe member.  backend_uvgen means
     * texcoord0 is dp4(position, c4/c5); backend_color_swap means COLOR0 is
     * read with the zyxw swizzle (red/blue swap). */
    uint32_t backend_uvgen;
    uint32_t backend_color_swap;
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

/* Match a vs_3_0 stream against the bounded static menu family.
 *
 * Certification is structural, not a single hash: the shipped family has
 * several declaration-compatible programs selected by the engine's vertex
 * declaration.  The runtime contract is:
 *   - vs_3_0, no texture sample and no branch/loop opcode;
 *   - opcodes restricted to the measured static menu set;
 *   - input semantics only position0, texcoord0..4 and color0;
 *   - output declarations for position0, texcoord0, color0 and color1;
 *   - 1..96 instructions.
 * The old manifest's "390" count used output declarations to satisfy its
 * input requirement.  This matcher tightens that to the static menu subset.
 *
 * Returns 1 for a member, 0 for a non-vs_3_0 stream (the legacy SM2 template
 * may decide) and -1 with a named reason for any vs_3_0 stream outside the
 * static family.  No partial plan is left in `out`. */
int d9_menu_shader_match_vs(const uint32_t *code, size_t words,
                            D9MenuShaderMatch *out,
                            char *why, size_t why_size);

/* Check that a vertex declaration (explicit declaration or FVF-derived) pro-
 * vides every input the selected VS reads.  Returns 0 when compatible, -1 and
 * a named missing semantic otherwise. */
int d9_menu_shader_vs_check_decl(const D9MenuShaderMatch *match,
                                 const D9Decl *decl,
                                 char *why, size_t why_size);

/* Measured FXP package+technique key -> closure entry.  For the two PS entries
 * the package and key are exact.  The VS family has one package rule and many
 * keys, so an exact VS key is not a single entry; `key` may be
 * D9_MENU_FXP_KEY_ANY and the bytecode still has to pass match_vs().
 * Returns 1 and writes *entry when known, 0 otherwise (with a named reason). */
int d9_menu_shader_lookup_fxp(int stage, const char *package_path, uint32_t key,
                              int *entry, char *why, size_t why_size);

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
