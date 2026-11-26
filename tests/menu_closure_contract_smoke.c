/* Host smoke test for the first-main-menu shader closure runtime contract.
 *
 * Compile: clang -std=gnu11 -O1 -g -I<repo>/source -I<runtime> \
 *            tests/menu_closure_contract_smoke.c <runtime>/d3d9_menu_closure.c \
 *            <repo>/source/d3d9_adapter.c -o menu_closure_contract_smoke
 *
 * Positive cases use the real retail bytecode in
 * tests/fixtures/menu_closure_real_shaders.h.  Negative cases mutate that
 * bytecode or build minimal ps_3_0/vs_3_0 streams.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9_menu_closure.h"
#include "d3d9_menu_shader_runtime.h"
#include "d3d9_menu_light.h"
#include "d3d9_adapter.h"
#include "fixtures/menu_closure_real_shaders.h"

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long a_ = (long long)(actual); \
    long long e_ = (long long)(expected); \
    ++g_checks; \
    if (a_ != e_) { \
        ++g_failures; \
        printf("FAIL %s:%d: %s = %lld, expected %lld\n", \
               __FILE__, __LINE__, #actual, a_, e_); \
    } \
} while (0)

#define CHECK_STR(actual, expected) do { \
    ++g_checks; \
    if (!(actual) || strcmp((actual), (expected)) != 0) { \
        ++g_failures; \
        printf("FAIL %s:%d: %s = %s, expected %s\n", __FILE__, __LINE__, \
               #actual, (actual) ? (actual) : "(null)", (expected)); \
    } \
} while (0)

static uint32_t *copy_words(const uint32_t *src, size_t words) {
    uint32_t *out = (uint32_t *)malloc(words * 4u);
    if (!out) return 0;
    memcpy(out, src, words * 4u);
    return out;
}

/* Parse the generated 64-hex-digit SHA-256 macro into bytes. */
static void hex_sha256(const char *hex, uint8_t out[32]) {
    for (unsigned i = 0; i < 32; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        out[i] = (uint8_t)strtoul(pair, 0, 16);
    }
}

static int request_ps(const uint32_t *code, size_t words, D9MenuClosureVerdict *v) {
    D9MenuClosureRequest r;
    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_PS;
    r.code = code;
    r.available_words = words;
    return d9_menu_closure_certify(&r, v);
}

static int request_vs(const uint32_t *code, size_t words, D9MenuClosureVerdict *v) {
    D9MenuClosureRequest r;
    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_VS;
    r.code = code;
    r.available_words = words;
    return d9_menu_closure_certify(&r, v);
}

/* --- minimal token builders (same encoding as tests/menu_light_smoke.c) --- */
#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define SW_ID 0xE4u
#define END_TOKEN 0xFFFFu

static uint32_t dr(unsigned type_, unsigned reg, unsigned mask, unsigned mod) {
    return 0x80000000u | ((type_ & 7u) << 28) | ((type_ & 0x18u) << 8) |
           (reg & 0x7FFu) | ((mask & 0xFu) << 16) | ((mod & 0xFu) << 20);
}

static uint32_t sr(unsigned type_, unsigned reg, unsigned sw, unsigned mod) {
    return 0x80000000u | ((type_ & 7u) << 28) | ((type_ & 0x18u) << 8) |
           (reg & 0x7FFu) | ((sw & 0xFFu) << 16) | ((mod & 0xFu) << 24);
}

/* dcl_position v0 / dcl_texcoord0 v1 / dcl_color0 v2 / dcl_color1 v3,
 * mov o0,v0 / mov o1,v1 / mov o2,v2.  This is a member of the bounded VS
 * family but contains no fixed hash (the engine picks the key). */
static uint32_t g_vs_family_member[] = {
    0xFFFE0300u,
    DI(0x1F, 2), 0x80000000u, 0u, /* dcl_position v0 */
    DI(0x1F, 2), 0x80000005u, 0u, /* dcl_texcoord0 v1 */
    DI(0x1F, 2), 0x8000000Au, 0u, /* dcl_color0 v2 */
    DI(0x1F, 2), 0x8001000Au, 0u, /* dcl_color1 v3 */
    DI(0x01, 2), 0u, 0u,
    DI(0x01, 2), 0u, 0u,
    DI(0x01, 2), 0u, 0u,
    END_TOKEN
};

/* Same family but writes only o0/o1: must be rejected structurally. */
static uint32_t g_vs_missing_output[] = {
    0xFFFE0300u,
    DI(0x1F, 2), 0x80000000u, 0u,
    DI(0x1F, 2), 0x80000005u, 0u,
    DI(0x1F, 2), 0x8000000Au, 0u,
    DI(0x1F, 2), 0x8001000Au, 0u,
    DI(0x01, 2), 0u, 0u,
    DI(0x01, 2), 0u, 0u,
    END_TOKEN
};

/* A well-formed ps_2_0 stream that the legacy SM2 path could translate but
 * which is not part of the first-menu closure. */
static uint32_t g_ps20_outside[] = {
    0xFFFF0200u,
    DI(0x1F, 2), 0x80000005u, 0u, /* dcl_texcoord0 v0 */
    DI(0x42, 2), 0u, 0u,          /* texld r0, v0, s0 */
    END_TOKEN
};

static void init_vs_streams(void) {
    /* dst: v0/v1/v2/v3 inputs */
    g_vs_family_member[3] = dr(1, 0, 0xF, 0);
    g_vs_family_member[6] = dr(1, 1, 0xF, 0);
    g_vs_family_member[9] = dr(1, 2, 0xF, 0);
    g_vs_family_member[12] = dr(1, 3, 0xF, 0);
    g_vs_family_member[14] = dr(6, 0, 0xF, 0); g_vs_family_member[15] = sr(1, 0, SW_ID, 0);
    g_vs_family_member[17] = dr(6, 1, 0xF, 0); g_vs_family_member[18] = sr(1, 1, SW_ID, 0);
    g_vs_family_member[20] = dr(6, 2, 0xF, 0); g_vs_family_member[21] = sr(1, 2, SW_ID, 0);

    g_vs_missing_output[3] = dr(1, 0, 0xF, 0);
    g_vs_missing_output[6] = dr(1, 1, 0xF, 0);
    g_vs_missing_output[9] = dr(1, 2, 0xF, 0);
    g_vs_missing_output[12] = dr(1, 3, 0xF, 0);
    g_vs_missing_output[14] = dr(6, 0, 0xF, 0); g_vs_missing_output[15] = sr(1, 0, SW_ID, 0);
    g_vs_missing_output[17] = dr(6, 1, 0xF, 0); g_vs_missing_output[18] = sr(1, 1, SW_ID, 0);

    g_ps20_outside[3] = dr(1, 0, 0xF, 0);
    g_ps20_outside[5] = dr(3, 0, 0xF, 0);
    g_ps20_outside[6] = sr(1, 0, SW_ID, 0);
}

static void test_sha256_vectors(void) {
    static const char abc[] = "abc";
    static const uint8_t empty_sha[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    static const uint8_t abc_sha[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t out[32];
    uint8_t expected[32];
    d9_menu_closure_sha256("", 0, out);
    CHECK(memcmp(out, empty_sha, 32) == 0);
    d9_menu_closure_sha256(abc, 3, out);
    CHECK(memcmp(out, abc_sha, 32) == 0);
    d9_menu_closure_sha256(menu_logo_ps, MENU_LOGO_PS_WORDS * 4u, out);
    hex_sha256(MENU_LOGO_PS_SHA256, expected);
    CHECK(memcmp(out, expected, 32) == 0);
}

static void test_manifest_table(void) {
    CHECK_EQ(d9_menu_closure_entry_count(), 5);
    CHECK_STR(d9_menu_closure_entry_id(D9_MENU_ENTRY_MENU_LOGO_PS), "menu.logo.ps");
    CHECK_STR(d9_menu_closure_entry_id(D9_MENU_ENTRY_MENU_COMPOSITE_PS), "menu.composite.ps");
    CHECK_STR(d9_menu_closure_entry_id(D9_MENU_ENTRY_MENU_GENERIC_VS), "menu.generic.vs");
    CHECK(d9_menu_closure_entry_id(D9_MENU_ENTRY_COUNT) == 0);
}

static void test_positive_ps(void) {
    D9MenuClosureVerdict v;
    int rc = request_ps(menu_logo_ps, MENU_LOGO_PS_WORDS, &v);
    CHECK_EQ(rc, D9_MENU_MATCH);
    CHECK_EQ(v.match, D9_MENU_MATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_LOGO_PS);
    CHECK_EQ(v.stage, D9_MENU_STAGE_PS);
    CHECK_EQ(v.key, 0x01200005u);
    CHECK_EQ(v.used_words, MENU_LOGO_PS_WORDS);
    CHECK_EQ(v.instructions, 25);

    /* The available region may be longer than the program.  Only the bytes
     * through END are hashed and counted. */
    uint32_t padded[MENU_LOGO_PS_WORDS + 8];
    memset(padded, 0xCD, sizeof padded);
    memcpy(padded, menu_logo_ps, MENU_LOGO_PS_WORDS * 4u);
    rc = request_ps(padded, sizeof padded / 4u, &v);
    CHECK_EQ(rc, D9_MENU_MATCH);
    CHECK_EQ(v.used_words, MENU_LOGO_PS_WORDS);

    rc = request_ps(menu_composite_ps, MENU_COMPOSITE_PS_WORDS, &v);
    CHECK_EQ(rc, D9_MENU_MATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_COMPOSITE_PS);

    /* Key-directed certification accepts both real keys. */
    D9MenuClosureRequest r;
    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_PS;
    r.key_known = 1;
    r.key = 0x01200005u;
    r.code = menu_logo_ps;
    r.available_words = MENU_LOGO_PS_WORDS;
    CHECK_EQ(d9_menu_closure_certify(&r, &v), D9_MENU_MATCH);
    r.key = 0x00000000u;
    r.code = menu_composite_ps;
    r.available_words = MENU_COMPOSITE_PS_WORDS;
    CHECK_EQ(d9_menu_closure_certify(&r, &v), D9_MENU_MATCH);
}

static void test_positive_vs_family(void) {
    D9MenuClosureVerdict v;
    int rc = request_vs(menu_generic_vs, MENU_GENERIC_VS_WORDS, &v);
    CHECK_EQ(rc, D9_MENU_MATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_GENERIC_VS);

    rc = request_vs(g_vs_family_member, sizeof g_vs_family_member / 4u, &v);
    CHECK_EQ(rc, D9_MENU_MATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_GENERIC_VS);

    /* A package-aware caller may know the engine VS key; the bounded family
     * has no fixed key, so the structural family test still applies. */
    D9MenuClosureRequest r;
    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_VS;
    r.key_known = 1;
    r.key = 0x12345678u;
    r.code = menu_generic_vs;
    r.available_words = MENU_GENERIC_VS_WORDS;
    CHECK_EQ(d9_menu_closure_certify(&r, &v), D9_MENU_MATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_GENERIC_VS);
}

static void test_wrong_hash(void) {
    D9MenuClosureVerdict v;
    D9MenuClosureRequest r;
    uint32_t *mutated = copy_words(menu_logo_ps, MENU_LOGO_PS_WORDS);
    CHECK(mutated != 0);
    if (!mutated) return;
    /* Word 6 is the w component of the single `def c8` literal.  Framing,
     * declarations, opcode sequence and length are unchanged. */
    mutated[6] ^= 0x3F800000u;

    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_PS;
    r.key_known = 1;
    r.key = 0x01200005u;
    r.code = mutated;
    r.available_words = MENU_LOGO_PS_WORDS;
    CHECK_EQ(d9_menu_closure_certify(&r, &v), D9_MENU_HASH_MISMATCH);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_NONE);

    /* Without a key the same bytes are simply outside the closure. */
    CHECK_EQ(request_ps(mutated, MENU_LOGO_PS_WORDS, &v), D9_MENU_NOT_MEMBER);
    free(mutated);
}

static void test_wrong_key(void) {
    D9MenuClosureVerdict v;
    D9MenuClosureRequest r;
    memset(&r, 0, sizeof r);
    r.stage = D9_MENU_STAGE_PS;
    r.key_known = 1;
    r.key = 0x01200006u;
    r.code = menu_logo_ps;
    r.available_words = MENU_LOGO_PS_WORDS;
    CHECK_EQ(d9_menu_closure_certify(&r, &v), D9_MENU_KEY_MISMATCH);
    CHECK(strstr(v.reason, "0x01200006") != 0);

    /* Right key, wrong stage: the logo bytes are not a vertex program. */
    CHECK_EQ(request_vs(menu_logo_ps, MENU_LOGO_PS_WORDS, &v), D9_MENU_NOT_MEMBER);
}

static void test_wrong_declaration(void) {
    D9MenuClosureVerdict v;
    CHECK_EQ(request_vs(g_vs_missing_output, sizeof g_vs_missing_output / 4u, &v),
             D9_MENU_NOT_MEMBER);
    CHECK(strstr(v.reason, "outputs") != 0);

    /* Structural API: a valid composite program is not the logo contract. */
    CHECK_EQ(d9_menu_closure_check_structure(D9_MENU_ENTRY_MENU_LOGO_PS,
                                             menu_composite_ps,
                                             MENU_COMPOSITE_PS_WORDS, &v),
             D9_MENU_STRUCTURE_MISMATCH);
    CHECK_EQ(d9_menu_closure_check_structure(D9_MENU_ENTRY_COUNT,
                                             menu_composite_ps,
                                             MENU_COMPOSITE_PS_WORDS, &v),
             D9_MENU_BAD_ARGUMENT);
}

static void test_missing_entry(void) {
    D9MenuClosureVerdict v;
    /* A valid ps_3_0 stream with one extra nop: not in the closure. */
    enum { WORDS = MENU_COMPOSITE_PS_WORDS + 1 };
    uint32_t stream[WORDS];
    memcpy(stream, menu_composite_ps, (MENU_COMPOSITE_PS_WORDS - 1) * 4u);
    stream[MENU_COMPOSITE_PS_WORDS - 1] = 0x00000000u; /* nop */
    stream[WORDS - 1] = END_TOKEN;
    CHECK_EQ(request_ps(stream, WORDS, &v), D9_MENU_NOT_MEMBER);
    CHECK(strstr(v.reason, "closed menu set") != 0);

    /* A truncated stream is a named non-member, never a success. */
    CHECK_EQ(request_ps(menu_composite_ps, 3, &v), D9_MENU_NOT_MEMBER);
    CHECK(strstr(v.reason, "no END") != 0 || strstr(v.reason, "past the stream") != 0);
}

static void test_state_machine_pass(void) {
    D9MenuClosureVerdict v;
    D9MenuClosureCapsClaim claim;
    D9Caps9 caps;
    char line[256];

    d9_menu_closure_reset();
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_UNARMED);
    CHECK_EQ(d9_menu_closure_predicate(), D9_MENU_PREDICATE_NOT_CERTIFIED);
    CHECK_EQ(d9_menu_closure_ready(), 0);

    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_ARMED);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 0);
    CHECK_EQ(d9_menu_closure_shader_set_predicate(), D9_MENU_PREDICATE_NOT_CERTIFIED);

    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_logo_ps,
                                       MENU_LOGO_PS_WORDS, &v), 0);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_LOGO_PS);
    d9_menu_closure_commit(&v);

    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_composite_ps,
                                       MENU_COMPOSITE_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);

    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_VS, menu_generic_vs,
                                       MENU_GENERIC_VS_WORDS, &v), 0);
    CHECK_EQ(v.entry, D9_MENU_ENTRY_MENU_GENERIC_VS);
    d9_menu_closure_commit(&v);

    /* Bytecode set complete, but the required fixed fog path has not reported. */
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_ARMED);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_predicate(), D9_MENU_PREDICATE_NOT_CERTIFIED);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 1);
    CHECK_EQ(d9_menu_closure_shader_set_predicate(), D9_MENU_PREDICATE_CERTIFIED);

    /* UI is declared required:false; noting it must not complete readiness. */
    d9_menu_closure_note_fixed(D9_MENU_ENTRY_MENU_UI_SCALEFORM_FIXED);
    CHECK_EQ(d9_menu_closure_ready(), 0);

    d9_menu_closure_note_fixed(D9_MENU_ENTRY_MENU_FOG_FIXED);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_READY);
    CHECK_EQ(d9_menu_closure_ready(), 1);
    CHECK_EQ(d9_menu_closure_predicate(), D9_MENU_PREDICATE_CERTIFIED);

    d9_menu_closure_describe(line, sizeof line);
    CHECK(strstr(line, "menu_shader_closure=ready") != 0);
    CHECK(strstr(line, "shaders=3/3") != 0);
    CHECK(strstr(line, "sm3_slots=0") != 0);

    /* The closure never changes GetDeviceCaps. */
    d9_menu_closure_caps_claim(&claim);
    CHECK_EQ(claim.vertex_shader_version, 0xFFFE0200u);
    CHECK_EQ(claim.pixel_shader_version, 0xFFFF0200u);
    CHECK_EQ(claim.sm3_instruction_slots, 0);
    CHECK_EQ(d9_adapter_caps(0, 1, &caps), D9_ADAPTER_OK);
    CHECK_EQ(caps.vertex_shader_version, 0xFFFE0200u);
    CHECK_EQ(caps.pixel_shader_version, 0xFFFF0200u);
    CHECK_EQ(caps.max_vertex_shader_30_instruction_slots, 0);
    CHECK_EQ(caps.max_pixel_shader_30_instruction_slots, 0);
}

static void test_unsupported_shader_invalidates(void) {
    D9MenuClosureVerdict v;
    d9_menu_closure_reset();
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_logo_ps,
                                       MENU_LOGO_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);

    enum { WORDS = MENU_COMPOSITE_PS_WORDS + 1 };
    uint32_t unknown[WORDS];
    memcpy(unknown, menu_composite_ps, (MENU_COMPOSITE_PS_WORDS - 1) * 4u);
    unknown[MENU_COMPOSITE_PS_WORDS - 1] = 0x00000000u;
    unknown[WORDS - 1] = END_TOKEN;

    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, unknown, WORDS, &v), -1);
    CHECK_EQ(d9_menu_closure_invalidated(), 1);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_INVALIDATED);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_predicate(), D9_MENU_PREDICATE_INVALIDATED);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 0);
    CHECK_EQ(d9_menu_closure_shader_set_predicate(), D9_MENU_PREDICATE_INVALIDATED);
    CHECK(strlen(d9_menu_closure_invalidation_reason()) > 0);

    /* A later valid closure member cannot resurrect the state. */
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_composite_ps,
                                       MENU_COMPOSITE_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);
    d9_menu_closure_note_fixed(D9_MENU_ENTRY_MENU_FOG_FIXED);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_INVALIDATED);

    /* arm() must not clear the invalidation. */
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_INVALIDATED);
}

static void test_no_fake_success(void) {
    D9MenuClosureVerdict v;

    /* Unarmed Create* is not a certification and grants nothing. */
    d9_menu_closure_reset();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_logo_ps,
                                       MENU_LOGO_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_UNARMED);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_predicate(), D9_MENU_PREDICATE_NOT_CERTIFIED);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 0);
    CHECK_EQ(d9_menu_closure_shader_set_predicate(), D9_MENU_PREDICATE_NOT_CERTIFIED);

    /* Armed, all shader creates succeed, but no fixed fog path: not ready. */
    d9_menu_closure_reset();
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_logo_ps,
                                       MENU_LOGO_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, menu_composite_ps,
                                       MENU_COMPOSITE_PS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_VS, menu_generic_vs,
                                       MENU_GENERIC_VS_WORDS, &v), 0);
    d9_menu_closure_commit(&v);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 1);

    /* A backend rejection of a certified entry is a failure, not a success. */
    d9_menu_closure_reset();
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_VS, menu_generic_vs,
                                       MENU_GENERIC_VS_WORDS, &v), 0);
    d9_menu_closure_commit_failed(&v, "PICA200 backend: vertex program unsupported");
    CHECK_EQ(d9_menu_closure_invalidated(), 1);
    CHECK(strstr(d9_menu_closure_invalidation_reason(), "menu.generic.vs") != 0);
    CHECK_EQ(d9_menu_closure_ready(), 0);
    CHECK_EQ(d9_menu_closure_shader_set_ready(), 0);

    /* Attempting a fixed entry through the bytecode API is a bad argument. */
    CHECK_EQ(d9_menu_closure_check_structure(D9_MENU_ENTRY_MENU_FOG_FIXED,
                                             menu_composite_ps,
                                             MENU_COMPOSITE_PS_WORDS, &v),
             D9_MENU_BAD_ARGUMENT);
}

static void test_outside_supported_stream_invalidates(void) {
    D9MenuClosureVerdict v;
    /* Unarmed: the legacy path may handle it; the closure does not claim it. */
    d9_menu_closure_reset();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, g_ps20_outside,
                                       sizeof g_ps20_outside / 4u, &v), 0);
    CHECK_EQ(d9_menu_closure_invalidated(), 0);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_UNARMED);

    /* Armed: a supported-by-the-old-backend but out-of-closure shader is
     * exactly the case the closed-set contract must reject. */
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, g_ps20_outside,
                                       sizeof g_ps20_outside / 4u, &v), -1);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_INVALIDATED);
    CHECK(strstr(d9_menu_closure_invalidation_reason(), "closed menu set") != 0);
}

static void test_precreate_wrong_hash_invalidates(void) {
    D9MenuClosureVerdict v;
    uint32_t *mutated = copy_words(menu_logo_ps, MENU_LOGO_PS_WORDS);
    CHECK(mutated != 0);
    if (!mutated) return;
    mutated[6] ^= 0x3F800000u;
    d9_menu_closure_reset();
    d9_menu_closure_arm();
    CHECK_EQ(d9_menu_closure_precreate(D9_MENU_STAGE_PS, mutated,
                                       MENU_LOGO_PS_WORDS, &v), -1);
    CHECK_EQ(d9_menu_closure_state(), D9_MENU_STATE_INVALIDATED);
    free(mutated);
}

/* All material evidence a menu plan can ask for.  This is test-only evidence;
 * the runtime starts with nothing and refuses draws by name. */
static uint32_t menu_all_preconditions(void) {
    return D9_MENU_SHADER_PRE_NORMAL_PREBAKED |
           D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED |
           D9_MENU_SHADER_PRE_AMBIENT_EMISSIVE_VARYING |
           D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4 |
           D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7 |
           D9_MENU_SHADER_PRE_LDR_QUALITY |
           D9_MENU_SHADER_PRE_TEXTURE0_2D |
           D9_MENU_SHADER_PRE_TEXTURE1_2D |
           D9_MENU_SHADER_PRE_VERTEX_COLOR;
}

static void test_menu_runtime_logo(void) {
    D9MenuShaderMatch m;
    char why[256];
    d9_menu_shader_runtime_reset();
    CHECK_EQ(d9_menu_shader_match_ps(menu_logo_ps, MENU_LOGO_PS_WORDS,
                                     &m, why, sizeof why), 1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_LOGO_PS);
    CHECK_EQ(m.entry, D9_MENU_ENTRY_MENU_LOGO_PS);
    CHECK_STR(m.id, "menu.logo.ps");
    CHECK_EQ(m.plan.stages, 5);
    CHECK_EQ(m.plan.textures, 2);
    CHECK_EQ(m.plan.uses_vertex_color, 1);
    CHECK_EQ(m.plan.constants, 3);
    CHECK_EQ(m.plan.stage[4].func_rgb, D9TEV_FUNC_MODULATE);
    CHECK_EQ(m.plan.stage[4].src_rgb[1], D9TEV_SRC_CONST);
    CHECK_EQ(m.plan.stage[4].op_rgb[1], D9TEV_RGB_ALPHA);
    CHECK_EQ(m.plan.stage[4].const_reg, 7);
    CHECK((m.required_preconditions & D9_MENU_SHADER_PRE_NORMAL_PREBAKED) != 0);
    CHECK((m.required_preconditions & D9_MENU_SHADER_PRE_TANGENT_BASIS_PREBAKED) != 0);
    CHECK((m.required_preconditions & D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7) != 0);
    CHECK((m.required_preconditions & D9_MENU_SHADER_PRE_TEXTURE1_2D) != 0);

    /* No material evidence: a named refusal, not an implicit draw. */
    CHECK(d9_menu_shader_missing_preconditions(&m, 0, why, sizeof why) > 0);
    CHECK(strstr(why, "normal_prebaked") != 0);
    CHECK(strstr(why, "tangent_basis_prebaked") != 0);
    d9_menu_shader_note_preconditions(menu_all_preconditions());
    CHECK_EQ(d9_menu_shader_missing_preconditions(&m, d9_menu_shader_preconditions(),
                                                  why, sizeof why), 0);
    d9_menu_shader_runtime_reset();
}

static void test_menu_runtime_composite(void) {
    D9MenuShaderMatch m;
    char why[256];
    d9_menu_shader_runtime_reset();
    CHECK_EQ(d9_menu_shader_match_ps(menu_composite_ps, MENU_COMPOSITE_PS_WORDS,
                                     &m, why, sizeof why), 1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_COMPOSITE_PS);
    CHECK_EQ(m.entry, D9_MENU_ENTRY_MENU_COMPOSITE_PS);
    CHECK_STR(m.id, "menu.composite.ps");
    CHECK_EQ(m.plan.stages, 1);
    CHECK_EQ(m.plan.textures, 1);
    CHECK_EQ(m.plan.inputs, 1);
    CHECK_EQ(m.plan.constants, 0);
    CHECK_EQ(m.plan.uses_vertex_color, 0);
    CHECK_EQ(m.plan.stage[0].func_rgb, D9TEV_FUNC_REPLACE);
    CHECK_EQ(m.plan.stage[0].func_a, D9TEV_FUNC_REPLACE);
    CHECK_EQ(m.plan.stage[0].src_rgb[0], D9TEV_SRC_TEX0);
    CHECK_EQ(m.plan.stage[0].src_a[0], D9TEV_SRC_TEX0);
    /* The copy's LDR/one-2D-sampler contract is structural and backend-checked;
     * no separate material mask is required. */
    CHECK_EQ(m.required_preconditions, 0u);
    CHECK_EQ(d9_menu_shader_missing_preconditions(&m, 0, why, sizeof why), 0);
    d9_menu_shader_runtime_reset();
}

static void test_menu_runtime_exact_hash_only(void) {
    D9MenuShaderMatch m;
    char why[256];
    uint32_t *mutated = copy_words(menu_logo_ps, MENU_LOGO_PS_WORDS);
    CHECK(mutated != 0);
    if (!mutated) return;

    /* Same framing/opcodes, one literal byte changed.  The exact closure hash
     * fails, so this is not a menu selection and no plan is left behind. */
    mutated[6] ^= 0x3F800000u;
    memset(&m, 0xAA, sizeof m);
    CHECK_EQ(d9_menu_shader_match_ps(mutated, MENU_LOGO_PS_WORDS,
                                     &m, why, sizeof why), 0);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK_EQ(m.entry, D9_MENU_ENTRY_NONE);
    CHECK_EQ(m.plan.stages, 0);
    CHECK(strstr(why, "not") != 0);
    free(mutated);

    /* A valid-looking copy plus one nop is also not in the closed set. */
    enum { WORDS = MENU_COMPOSITE_PS_WORDS + 1 };
    uint32_t stream[WORDS];
    memcpy(stream, menu_composite_ps, (MENU_COMPOSITE_PS_WORDS - 1) * 4u);
    stream[MENU_COMPOSITE_PS_WORDS - 1] = 0x00000000u; /* nop */
    stream[WORDS - 1] = END_TOKEN;
    CHECK_EQ(d9_menu_shader_match_ps(stream, WORDS, &m, why, sizeof why), 0);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK_EQ(m.plan.stages, 0);

    /* A synthetic copy with the same dcl,dcl,texld structure but none of the
     * certified CTAB bytes is still outside the exact-hash closure. */
    uint32_t synthetic[16];
    size_t n = 0;
    synthetic[n++] = 0xFFFF0300u;
    synthetic[n++] = DI(0x1F, 2); synthetic[n++] = 0x80000005u;
    synthetic[n++] = dr(1, 0, 0x3, 0);
    synthetic[n++] = DI(0x1F, 2); synthetic[n++] = 0x90000000u;
    synthetic[n++] = dr(10, 0, 0xF, 0);
    synthetic[n++] = DI(0x42, 3); synthetic[n++] = dr(8, 0, 0xF, 0);
    synthetic[n++] = sr(1, 0, SW_ID, 0); synthetic[n++] = sr(10, 0, SW_ID, 0);
    synthetic[n++] = END_TOKEN;
    CHECK_EQ(d9_menu_shader_match_ps(synthetic, n, &m, why, sizeof why), 0);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK_EQ(m.plan.stages, 0);
    CHECK(strstr(why, "certified") != 0 || strstr(why, "closed") != 0);

    /* Wrong stage: a vertex program is never selected as a menu PS. */
    CHECK_EQ(d9_menu_shader_match_ps(menu_generic_vs, MENU_GENERIC_VS_WORDS,
                                     &m, why, sizeof why), 0);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK(strstr(why, "ps") != 0 || strstr(why, "certified") != 0);
}

static void test_menu_runtime_structural_negatives(void) {
    D9MenuLightMatch match;
    char why[256];
    uint32_t *code = copy_words(menu_logo_ps, MENU_LOGO_PS_WORDS);
    CHECK(code != 0);
    if (!code) return;

    /* Word 138 is the s0 sampler operand of the first texld (the CTAB comment
     * occupies words 1..113).  Wrong sampler: p1200005 decode is rejected by
     * name. */
    code[138] = sr(10, 2, SW_ID, 0);
    CHECK_EQ(d9_menu_light_match(code, MENU_LOGO_PS_WORDS, &match, why, sizeof why), 0);
    CHECK(strstr(why, "texld diffuse") != 0);

    /* Word 139 is the material-alpha multiply opcode. */
    code[138] = sr(10, 0, SW_ID, 0);
    code[139] = DI(0x02, 3);
    CHECK_EQ(d9_menu_light_match(code, MENU_LOGO_PS_WORDS, &match, why, sizeof why), 0);
    CHECK(strstr(why, "material-alpha") != 0);
    free(code);
}

static void test_menu_runtime_fixed(void) {
    D9MenuShaderMatch m;
    char why[256];
    uint32_t all = menu_all_preconditions();

    CHECK_EQ(d9_menu_shader_match_fixed(D9_MENU_SHADER_FOG_FIXED, all,
                                        &m, why, sizeof why), 1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_FOG_FIXED);
    CHECK_EQ(m.entry, D9_MENU_ENTRY_MENU_FOG_FIXED);
    CHECK_STR(m.id, "menu.fog.fixed");
    CHECK_EQ(m.plan.stages, 2);
    CHECK_EQ(m.plan.textures, 2);
    CHECK_EQ(m.plan.uses_vertex_color, 1);
    CHECK_EQ(m.plan.stage[0].src_rgb[0], D9TEV_SRC_TEX0);
    CHECK_EQ(m.plan.stage[0].src_rgb[1], D9TEV_SRC_PRIMARY);
    CHECK_EQ(m.plan.stage[1].src_rgb[0], D9TEV_SRC_TEX1);

    CHECK_EQ(d9_menu_shader_match_fixed(D9_MENU_SHADER_FOG_FIXED,
                                        all & ~D9_MENU_SHADER_PRE_TEXTURE1_2D,
                                        &m, why, sizeof why), -1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK_EQ(m.plan.stages, 0);
    CHECK(strstr(why, "texture1_2d") != 0);

    CHECK_EQ(d9_menu_shader_match_fixed(D9_MENU_SHADER_TINT_FIXED,
                                        D9_MENU_SHADER_PRE_LDR_QUALITY |
                                        D9_MENU_SHADER_PRE_TEXTURE0_2D |
                                        D9_MENU_SHADER_PRE_VERTEX_COLOR,
                                        &m, why, sizeof why), 1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_TINT_FIXED);
    CHECK_EQ(m.plan.stages, 1);
    CHECK_EQ(m.plan.textures, 1);
    CHECK_EQ(m.plan.stage[0].src_rgb[1], D9TEV_SRC_PRIMARY);

    CHECK_EQ(d9_menu_shader_match_fixed(D9_MENU_SHADER_TINT_FIXED,
                                        D9_MENU_SHADER_PRE_LDR_QUALITY |
                                        D9_MENU_SHADER_PRE_TEXTURE0_2D,
                                        &m, why, sizeof why), -1);
    CHECK_EQ(m.kind, D9_MENU_SHADER_NONE);
    CHECK(strstr(why, "vertex_color") != 0);

    CHECK_EQ(d9_menu_shader_match_fixed(D9_MENU_SHADER_LOGO_PS, all,
                                        &m, why, sizeof why), -1);
}

static void test_menu_runtime_caps_unchanged(void) {
    D9MenuShaderMatch m;
    D9MenuClosureCapsClaim claim;
    D9Caps9 caps;
    char why[256];
    d9_menu_shader_runtime_reset();
    CHECK_EQ(d9_menu_shader_match_ps(menu_logo_ps, MENU_LOGO_PS_WORDS,
                                     &m, why, sizeof why), 1);
    d9_menu_shader_note_preconditions(menu_all_preconditions());

    d9_menu_closure_caps_claim(&claim);
    CHECK_EQ(claim.vertex_shader_version, 0xFFFE0200u);
    CHECK_EQ(claim.pixel_shader_version, 0xFFFF0200u);
    CHECK_EQ(claim.sm3_instruction_slots, 0);
    CHECK_EQ(d9_adapter_caps(0, 1, &caps), D9_ADAPTER_OK);
    CHECK_EQ(caps.vertex_shader_version, 0xFFFE0200u);
    CHECK_EQ(caps.pixel_shader_version, 0xFFFF0200u);
    CHECK_EQ(caps.max_vertex_shader_30_instruction_slots, 0);
    CHECK_EQ(caps.max_pixel_shader_30_instruction_slots, 0);
    d9_menu_shader_runtime_reset();
}

int main(void) {
    init_vs_streams();
    test_sha256_vectors();
    test_manifest_table();
    test_positive_ps();
    test_positive_vs_family();
    test_wrong_hash();
    test_wrong_key();
    test_wrong_declaration();
    test_missing_entry();
    test_state_machine_pass();
    test_unsupported_shader_invalidates();
    test_no_fake_success();
    test_outside_supported_stream_invalidates();
    test_precreate_wrong_hash_invalidates();
    test_menu_runtime_logo();
    test_menu_runtime_composite();
    test_menu_runtime_exact_hash_only();
    test_menu_runtime_structural_negatives();
    test_menu_runtime_fixed();
    test_menu_runtime_caps_unchanged();

    printf("menu_closure_contract_smoke: %u checks, %u failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
