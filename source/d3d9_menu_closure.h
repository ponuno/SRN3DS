#pragma once
/* First-main-menu shader closure: integration-ready runtime contract.
 *
 * This module certifies the *named* TESV first-menu shader set and nothing
 * else.  It is deliberately separate from D3D9 capability reporting:
 *
 *   - GetDeviceCaps keeps reporting VS 2.0 / PS 2.0 and zero SM3 slots.
 *   - `d9_menu_closure_ready()` never changes a caps field.  It is a
 *     TESV-specific compatibility predicate that a caller may consult before
 *     satisfying the engine's level-7 branch at 0x00c6e22d.
 *   - Certification requires observed, successfully created shader objects.
 *     A manifest listing alone is not a success.
 *   - Any shader outside the closure that is created while the contract is
 *     armed invalidates it permanently (until d9_menu_closure_reset()).
 *
 * The generated table in d3d9_menu_closure_manifest.h comes from
 * menu_closure_manifest.json (stage, key, SHA-256, version, declarations,
 * opcode sequence, fixed-pipeline state).  Regenerate it with
 * tools/gen_menu_closure_manifest.py instead of editing it by hand.
 */

#include <stddef.h>
#include <stdint.h>

#define D9_MENU_CLOSURE_REASON_MAX 192u
#define D9_MENU_CLOSURE_SHA256_BYTES 32u

typedef enum {
    D9_MENU_STAGE_PS = 0,
    D9_MENU_STAGE_VS = 1,
    D9_MENU_STAGE_FIXED = 2
} D9MenuClosureStage;

/* Result of matching one token stream against the closure. */
typedef enum {
    D9_MENU_MATCH = 0,               /* exact closure member */
    D9_MENU_NOT_MEMBER = 1,          /* well-formed but outside the closure */
    D9_MENU_BAD_ARGUMENT = 2,        /* null/short/unknown request */
    D9_MENU_HASH_MISMATCH = 3,       /* key names an entry, SHA-256 differs */
    D9_MENU_KEY_MISMATCH = 4,        /* key is not in the closure */
    D9_MENU_STRUCTURE_MISMATCH = 5   /* hash matched, declaration/opcode contract did not */
} D9MenuClosureMatch;

typedef struct {
    uint32_t usage;
    uint32_t usage_index;
    uint32_t reg_type;   /* D3D9 register class; 255 = unconstrained */
    uint32_t reg_index;
    int32_t texture_type; /* 2 = 2D; -1 = unconstrained */
} D9MenuClosureDecl;

typedef struct {
    const char *id;
    uint32_t stage;          /* D9_MENU_STAGE_PS/VS/FIXED */
    uint32_t required;       /* 1 = readiness fails without this entry */
    uint32_t runtime_gated;  /* 1 = bytecode create must be observed */
    uint32_t key_known;      /* 0 for the bounded VS family */
    uint32_t key;            /* engine package chunk meta */
    uint32_t chunk_bytes;    /* exact bytecode length through END */
    uint32_t version;        /* D3D9 version token */
    uint32_t instruction_count;
    int32_t texld_count;
    int32_t branch_count;    /* -1 = unconstrained */
    const uint8_t *sha256;   /* NULL for a structural family entry */
    const D9MenuClosureDecl *declarations;
    uint32_t declaration_count;
    const uint16_t *opcodes;
    uint32_t opcode_count;
    uint32_t required_output_mask;
    const char *fixed_state; /* fixed-pipeline contract, or NULL */
    const char *why_no_fxp;  /* fixed-pipeline reason, or NULL */
} D9MenuClosureEntry;

#include "d3d9_menu_closure_manifest.h"

typedef struct {
    uint32_t stage;
    uint32_t key_known;
    uint32_t key;
    const uint32_t *code;
    size_t available_words; /* readable words; the program must fit inside */
} D9MenuClosureRequest;

typedef struct {
    uint32_t match;      /* D9MenuClosureMatch */
    int entry;           /* D9_MENU_ENTRY_* or D9_MENU_ENTRY_NONE */
    uint32_t stage;
    uint32_t key;
    uint32_t version;
    uint32_t used_words;
    uint32_t instructions;
    uint8_t sha256[D9_MENU_CLOSURE_SHA256_BYTES];
    char reason[D9_MENU_CLOSURE_REASON_MAX];
} D9MenuClosureVerdict;

typedef enum {
    D9_MENU_STATE_UNARMED = 0,
    D9_MENU_STATE_ARMED = 1,
    D9_MENU_STATE_READY = 2,
    D9_MENU_STATE_INVALIDATED = 3
} D9MenuClosureState;

typedef enum {
    D9_MENU_PREDICATE_NOT_CERTIFIED = 0,
    D9_MENU_PREDICATE_CERTIFIED = 1,
    D9_MENU_PREDICATE_INVALIDATED = 2
} D9MenuClosurePredicate;

/* --------------------------------------------------------------------- */
/* Pure certification (no state change; safe for tests and diagnostics). */
/* --------------------------------------------------------------------- */

/* SHA-256 over an exact token byte range. */
void d9_menu_closure_sha256(const void *data, size_t size,
                            uint8_t out[D9_MENU_CLOSURE_SHA256_BYTES]);

/* Match one request.  `out` is optional; when present it is filled for every
 * return value, including failures.  `d9_menu_closure_certify` never mutates
 * the runtime closure state.
 * Returns one of D9MenuClosureMatch. */
int d9_menu_closure_certify(const D9MenuClosureRequest *request,
                            D9MenuClosureVerdict *out);

/* Structural check of a token stream against a named entry: version token,
 * exact length through END, instruction count, texld/branch counts,
 * declarations, opcode sequence and required outputs.  Hash is not checked,
 * so this is the family test for D9_MENU_ENTRY_GENERIC_VS and the second half
 * of a bytecode entry's contract. */
int d9_menu_closure_check_structure(int entry, const uint32_t *code,
                                    size_t available_words,
                                    D9MenuClosureVerdict *out);

/* --------------------------------------------------------------------- */
/* Runtime state machine.                                                */
/* --------------------------------------------------------------------- */

/* Clear every observation: unarmed, all entries unseen, no invalidation.
 * Call at device creation or between tests. */
void d9_menu_closure_reset(void);

/* Start strict closed-set observation.  Idempotent while armed.  A no-op when
 * the closure is already INVALIDATED: only reset() can clear that. */
void d9_menu_closure_arm(void);

/* Stop observing; existing observations are kept for audit but readiness
 * requires the armed state. */
void d9_menu_closure_disarm(void);
int d9_menu_closure_armed(void);

/* Top of IDirect3DDevice9::CreatePixelShader / CreateVertexShader.
 *
 *  - Not armed: returns 0 and only fills `verdict` for diagnostics.
 *  - Armed and a closure member: returns 0 and marks the entry attempted.
 *  - Armed and anything else (wrong hash, wrong key, missing entry, wrong
 *    declarations, unsupported stream): invalidates the closure with a named
 *    reason and returns -1.  The caller must return D3DERR_NOTAVAILABLE.
 *
 * `available_words` is the readable region; the program must terminate inside
 * it.  A successful create must be reported with d9_menu_closure_commit();
 * a backend rejection with d9_menu_closure_commit_failed(). */
int d9_menu_closure_precreate(uint32_t stage, const uint32_t *code,
                              size_t available_words,
                              D9MenuClosureVerdict *verdict);
void d9_menu_closure_commit(const D9MenuClosureVerdict *verdict);
void d9_menu_closure_commit_failed(const D9MenuClosureVerdict *verdict,
                                   const char *why);

/* Fixed-pipeline entries cannot be certified from bytecode.  The path that
 * actually sets up the menu fog/smoke state reports itself here. */
void d9_menu_closure_note_fixed(int entry);

D9MenuClosureState d9_menu_closure_state(void);
const char *d9_menu_closure_state_name(D9MenuClosureState state);
/* True only for the READY state, i.e. armed, every required entry observed as
 * successfully created, every required fixed path reported, no invalidation. */
int d9_menu_closure_ready(void);
int d9_menu_closure_invalidated(void);
const char *d9_menu_closure_invalidation_reason(void);

/* Non-D3D9 menu predicate.  This is the only result the TESV compatibility
 * gate may consult.  It is *not* a GetDeviceCaps answer and must never be
 * routed through d9_req_evaluate()/D9REQ_* as a capability success.
 *
 * There are two honest tiers:
 *
 *  - shader-set certified: every required bytecode-backed entry has really been
 *    created through IDirect3DDevice9::CreatePixelShader/VertexShader.  This is
 *    the strongest result available before the first menu draw and is the only
 *    basis on which the 0x00c6e22d gate may pass.  A manifest listing alone is
 *    never enough.
 *  - closure certified (d9_menu_closure_ready/predicate): additionally the
 *    required fixed-pipeline fog path has reported itself.  Log that as
 *    `menu_shader_closure=certified`; log the gate-time tier as
 *    `menu_shader_closure=shader_set_certified`, never as a frame claim. */
D9MenuClosurePredicate d9_menu_closure_predicate(void);
int d9_menu_closure_shader_set_ready(void);
D9MenuClosurePredicate d9_menu_closure_shader_set_predicate(void);

/* Entry introspection for capture/audit. */
int d9_menu_closure_entry_count(void);
const char *d9_menu_closure_entry_id(int entry);
int d9_menu_closure_entry_status(int entry, uint32_t *seen,
                                 uint32_t *attempted);

/* GetDeviceCaps remains VS/PS 2.0 with zero SM3 slots.  This mirrors the
 * adapter contract so a test can prove the closure never leaks into caps. */
typedef struct {
    uint32_t vertex_shader_version;
    uint32_t pixel_shader_version;
    uint32_t sm3_instruction_slots;
} D9MenuClosureCapsClaim;
void d9_menu_closure_caps_claim(D9MenuClosureCapsClaim *out);

/* One bounded line, e.g.
 * "menu_shader_closure=invalidated entries=2/4 reason=ps key 0x... not certified".
 * Returns the length the complete line needs (snprintf semantics). */
size_t d9_menu_closure_describe(char *buf, size_t cap);
