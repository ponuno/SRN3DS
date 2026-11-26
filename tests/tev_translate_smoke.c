/* Host test: D3D9 ps_3_0 -> PICA200 TEV translation (structure + rejection).
 *
 * Programs are assembled by hand here; no game bytes.  The arithmetic accuracy
 * comparison lives in tests/tev_accuracy_smoke.c.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_tev.h"

#define DI(op, extra) ((uint32_t)(op) | ((uint32_t)(extra) << 24))
#define DST_R(n) (0x80000000u | (uint32_t)(n) | (0xFu << 16))
#define DST_C0 (0x80000000u | 0x0800u | (0xFu << 16))
#define DST_C(n) (0xA0000000u | (uint32_t)(n) | (0xFu << 16))
#define DST_C0_RGB (0x80000000u | 0x0800u | (0x7u << 16))
#define SRC_R(n) (0x80000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_RX(n) (0x80000000u | (uint32_t)(n))
#define SRC_C(n) (0xA0000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_CX(n) (0xA0000000u | (uint32_t)(n))
#define SRC_T(n) (0x90000000u | (uint32_t)(n) | (0xE4u << 16))
#define DCL_TEXCOORD(n) (0x80000005u | ((uint32_t)(n) << 16))
#define DCL_COLOR(n) (0x8000000Au | ((uint32_t)(n) << 16))
#define DCL_SAMPLER2D (0x90000000u)
#define DCL_SAMPLERCUBE (0x98000000u)
#define SAMP(n) (0xA00F0800u | (uint32_t)(n))
#define TEXD(n) (0x90000000u | (uint32_t)(n) | (0x03u << 16))
#define END_TOKEN 0xFFFFu
#define VER_PS30 0xFFFF0300u

static int failures = 0;
static int checks = 0;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void check_near(float a, float b, const char *what) {
    ++checks;
    if (fabsf(a - b) > 1e-6f) {
        ++failures;
        printf("FAIL: %s (%.6f != %.6f)\n", what, a, b);
    }
}

static uint32_t fbits(float value) {
    union { float f; uint32_t u; } bits;
    bits.f = value;
    return bits.u;
}

static const float k_tex[3][4] = {
    {0.20f, 0.40f, 0.60f, 0.80f},
    {0.90f, 0.10f, 0.30f, 0.50f},
    {0.05f, 0.15f, 0.25f, 0.35f},
};

static void eval_with(const D9TevProgram *p, const float constants[D9TEV_MAX_STAGES][4],
                      float out[4]) {
    const float primary[4] = {1.0f, 0.5f, 0.25f, 0.75f};
    d9_tev_eval_pixel(p, k_tex, constants, primary, out);
}

static int translate(const uint32_t *code, size_t words, D9TevProgram *out,
                     char *why, size_t cap) {
    memset(out, 0, sizeof *out);
    return d9_tev_translate_pixel(code, words, out, why, cap);
}

static void test_blit(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x42, 3), DST_C0, SRC_T(0), SAMP(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 1, "blit: one TEV stage");
    check(p.textures == 1, "blit: one texture unit");
    check(p.texture[0].texcoord_reg == 0, "blit: texcoord register 0");
    check(p.texture[0].sampler_reg == 0, "blit: sampler 0");
    check(p.stage[0].func_rgb == D9TEV_FUNC_REPLACE, "blit: REPLACE");
    check(p.stage[0].src_rgb[0] == D9TEV_SRC_TEX0, "blit: source is TEXTURE0");
    check(p.stage[0].op_rgb[0] == D9TEV_RGB_COLOR, "blit: colour operand");
    check(strcmp(d9_tev_family_name(&p), "blit") == 0, "blit: family name");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    for (int i = 0; i < 4; ++i) check_near(out[i], k_tex[0][i], "blit: passthrough");
}

static void test_texcoord_alias(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x01, 2), 0x80030000u, SRC_T(0), /* mov r0.xy, v0 */
        DI(0x42, 3), DST_C0, SRC_R(0), SAMP(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 1 && p.textures == 1 && p.texture[0].texcoord_reg == 0,
          "texcoord alias: direct input provenance");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    for (int i = 0; i < 4; ++i) check_near(out[i], k_tex[0][i], "texcoord alias: passthrough");
}

static void test_tint(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3E800000u, 0x3F800000u, 0x3F800000u,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x05, 3), DST_C0, SRC_R(0), SRC_C(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 1, "tint: one TEV stage");
    check(p.stage[0].func_rgb == D9TEV_FUNC_MODULATE, "tint: MODULATE");
    check(p.stage[0].const_is_literal == 1, "tint: literal constant");
    check(p.stage[0].const_rgba8 == d9_tev_pack_rgba8((float[4]){0.5f, 0.25f, 1.0f, 1.0f}),
          "tint: packed RGBA8 constant");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q50 = 128.0f / 255.0f, q25 = 64.0f / 255.0f;
    const float expect[4] = {0.20f * q50, 0.40f * q25, 0.60f, 0.80f};
    for (int i = 0; i < 4; ++i) check_near(out[i], expect[i], "tint: tex * const");
}

static void test_channel_carry(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x1F, 2), DCL_COLOR(0), SRC_T(1),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3E800000u, 0x3F800000u, 0x3F800000u,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x05, 3), DST_R(0), SRC_R(0), SRC_T(1),
        DI(0x05, 3), DST_C0_RGB, SRC_R(0), SRC_CX(0),
        DI(0x01, 2), 0x80080800u, SRC_R(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 3, "channel carry: three stages");
    check(p.stage[1].func_a == D9TEV_FUNC_REPLACE &&
          p.stage[1].src_a[0] == D9TEV_SRC_PREV,
          "channel carry: RGB output write preserves live alpha");
    check(p.stage[2].func_rgb == D9TEV_FUNC_REPLACE &&
          p.stage[2].src_rgb[0] == D9TEV_SRC_PREV,
          "channel carry: alpha output write preserves live RGB");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q50 = 128.0f / 255.0f;
    const float expect[4] = {k_tex[0][0] * q50,
                             k_tex[0][1] * 0.5f * q50,
                             k_tex[0][2] * 0.25f * q50,
                             k_tex[0][3] * 0.75f};
    for (int i = 0; i < 4; ++i) check_near(out[i], expect[i], "channel carry: math");
}

static void test_split_channel_sources(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(1),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3E800000u, 0x3F400000u, 0x3E000000u,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x42, 3), DST_R(1), SRC_T(0), SAMP(1),
        DI(0x01, 2), 0x80080000u, 0x80FF0001u, /* mov r0.a, r1.a */
        DI(0x05, 3), DST_C0, SRC_R(0), SRC_C(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 1 && p.textures == 2,
          "split channels: one stage, two textures");
    check(p.stage[0].src_rgb[0] == D9TEV_SRC_TEX0 &&
          p.stage[0].src_a[0] == D9TEV_SRC_TEX1,
          "split channels: RGB TEXTURE0, alpha TEXTURE1");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q50 = 128.0f / 255.0f, q25 = 64.0f / 255.0f;
    const float q75 = 191.0f / 255.0f, q125 = 32.0f / 255.0f;
    const float expect[4] = {k_tex[0][0] * q50, k_tex[0][1] * q25,
                             k_tex[0][2] * q75, k_tex[1][3] * q125};
    for (int i = 0; i < 4; ++i) check_near(out[i], expect[i], "split channels: math");
}

static void test_lrp(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(1),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0, 0, 0,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x42, 3), DST_R(1), SRC_T(0), SAMP(1),
        DI(0x12, 4), DST_C0, SRC_CX(0), SRC_R(0), SRC_R(1),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 1, "lrp: one TEV stage");
    check(p.textures == 2, "lrp: two texture units");
    check(p.stage[0].func_rgb == D9TEV_FUNC_INTERPOLATE, "lrp: INTERPOLATE");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    /* lrp(a, b, c) = a*c + b*(1-c), with a=c0 (128/255), b=r0, c=r1. */
    const float q = 128.0f / 255.0f;
    for (int i = 0; i < 4; ++i)
        check_near(out[i], q * k_tex[1][i] + k_tex[0][i] * (1.0f - k_tex[1][i]),
                   "lrp: math");
}

static void test_mad_two_constants(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u,
        DI(0x51, 5), DST_C(1), 0x3E800000u, 0x3E800000u, 0x3E800000u, 0x3E800000u,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x04, 4), DST_C0, SRC_R(0), SRC_C(0), SRC_C(1),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 2, "mad: constant preload + multiply-add");
    check(p.stage[1].func_rgb == D9TEV_FUNC_MULTIPLY_ADD, "mad: MULTIPLY_ADD");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q50 = 128.0f / 255.0f, q25 = 64.0f / 255.0f;
    for (int i = 0; i < 4; ++i)
        check_near(out[i], k_tex[0][i] * q50 + q25, "mad: math");
}

static void test_dp3(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3F000000u, 0x3F000000u, 0,
        DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
        DI(0x08, 3), DST_C0, SRC_R(0), SRC_C(0),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stage[0].func_rgb == D9TEV_FUNC_DOT3_RGBA, "dp3: DOT3_RGBA");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q = 128.0f / 255.0f;
    float dot = (k_tex[0][0] + k_tex[0][1] + k_tex[0][2]) * q;
    for (int i = 0; i < 4; ++i) check_near(out[i], dot, "dp3: dot3 replicated");
}

static void test_alpha_fixup(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3E800000u, 0x3F800000u, 0x3F800000u,
        DI(0x01, 2), DST_C0_RGB, SRC_C(0), /* mov oC0.rgb, c0 */
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.requires_alpha_fixup == 1, "alpha fixup: flag");
    check(p.stages == 2, "alpha fixup: extra stage");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    check_near(out[0], 128.0f / 255.0f, "alpha fixup: red");
    check_near(out[3], 1.0f, "alpha fixup: alpha = 1");
}

static void check_texld_reject(const char *name, uint32_t uv_decl,
                              uint32_t dst, uint32_t uv, uint32_t sampler,
                              const char *needle) {
    uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), uv_decl, TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x42, 3), dst, uv, sampler,
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    int rc = translate(code, sizeof code / sizeof *code, &p, why, sizeof why);
    check(rc < 0, name);
    if (rc >= 0)
        printf("FAIL: %s accepted malformed texld\n", name);
    ++checks;
    if (!strstr(why, needle)) {
        ++failures;
        printf("FAIL: %s reason does not name '%s': %s\n", name, needle, why);
    }
}

static void test_texld_validation(void) {
    D9TevProgram p;
    char why[256];
    const uint32_t valid[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
        DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
        DI(0x42, 3), DST_C0, SRC_T(0), SAMP(0),
        END_TOKEN,
    };
    check(translate(valid, sizeof valid / sizeof *valid, &p, why, sizeof why) == 0,
          "texld: valid identity operands");

    check_texld_reject("texld: reject destination without high bit",
                       DCL_TEXCOORD(0), DST_C0 & ~0x80000000u,
                       SRC_T(0), SAMP(0), "malformed destination token");
    check_texld_reject("texld: reject relative destination",
                       DCL_TEXCOORD(0), DST_C0 | (1u << 13),
                       SRC_T(0), SAMP(0), "relative destination");
    check_texld_reject("texld: reject destination modifier",
                       DCL_TEXCOORD(0), DST_C0 | (1u << 20),
                       SRC_T(0), SAMP(0), "destination modifier");
    check_texld_reject("texld: reject UV without high bit",
                       DCL_TEXCOORD(0), DST_C0,
                       SRC_T(0) & ~0x80000000u, SAMP(0),
                       "malformed UV operand token");
    /* input_usage[16] aliases input_usage_index[0] in the old parser.  Giving
     * t0 usage index 5 made the out-of-bounds read look like a texcoord usage. */
    check_texld_reject("texld: reject UV register 16 before input_usage lookup",
                       0x80050005u, DST_C0, SRC_T(16), SAMP(0),
                       "UV register 16 out of range");
    check_texld_reject("texld: reject relative UV",
                       DCL_TEXCOORD(0), DST_C0,
                       SRC_T(0) | (1u << 13), SAMP(0), "relative addressing");
    check_texld_reject("texld: reject UV source modifier",
                       DCL_TEXCOORD(0), DST_C0,
                       SRC_T(0) | (2u << 24), SAMP(0), "source modifier");
    check_texld_reject("texld: reject sampler without high bit",
                       DCL_TEXCOORD(0), DST_C0, SRC_T(0),
                       SAMP(0) & ~0x80000000u, "malformed sampler operand token");
    check_texld_reject("texld: reject relative sampler",
                       DCL_TEXCOORD(0), DST_C0, SRC_T(0),
                       SAMP(0) | (1u << 13), "relative addressing");
    check_texld_reject("texld: reject sampler source modifier",
                       DCL_TEXCOORD(0), DST_C0, SRC_T(0),
                       SAMP(0) | (1u << 24), "source modifier");
}

static void test_literal_validation(void) {
    static const struct {
        uint32_t bits;
        const char *reason;
    } invalid[] = {
        {0xBF800000u, "outside [0,1]"}, /* -1.0 */
        {0x3FC00000u, "outside [0,1]"}, /*  1.5 */
        {0x7F800000u, "not finite"},    /* +infinity */
        {0xFF800000u, "not finite"},    /* -infinity */
        {0x7FC00000u, "not finite"},    /* quiet NaN */
        {0x7F800001u, "not finite"},    /* signalling NaN */
    };
    D9TevProgram p;
    char why[256];
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t code[] = {
                VER_PS30,
                DI(0x51, 5), DST_C(0), 0, 0, 0, 0,
                DI(0x01, 2), DST_C0, SRC_C(0),
                END_TOKEN,
            };
            char needle[64];
            code[3 + lane] = invalid[i].bits;
            snprintf(needle, sizeof needle, "def c0.%c", "xyzw"[lane]);
            int rc = translate(code, sizeof code / sizeof *code, &p, why, sizeof why);
            ++checks;
            if (rc >= 0) {
                ++failures;
                printf("FAIL: invalid literal case %u lane %u was accepted\n", i, lane);
            }
            ++checks;
            if (!strstr(why, needle) || !strstr(why, invalid[i].reason)) {
                ++failures;
                printf("FAIL: invalid literal case %u lane %u reason: %s\n",
                       i, lane, why);
            }
        }
    }

    { /* def destinations are unmodified, full-width constant registers. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0) & ~0x80000000u, 0x3F000000u, 0, 0, 0,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject def destination without high bit");
        check(strstr(why, "malformed destination token") != NULL,
              "literal range: def high-bit reason is named");
    }
    { /* def destination modifier */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0) | (1u << 20), 0x3F000000u, 0, 0, 0,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject modified def destination");
        check(strstr(why, "modifier or mask") != NULL,
              "literal range: def modifier reason is named");
    }
    { /* def destination mask */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0) & ~(0x8u << 16), 0x3F000000u, 0, 0, 0,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject partial def destination");
        check(strstr(why, "modifier or mask") != NULL,
              "literal range: def mask reason is named");
    }
    { /* An out-of-range unused lane is never materialised into the result. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0), 0x3FC00000u, 0x3F000000u, 0, 0x3F800000u,
            DI(0x01, 2), DST_C0_RGB, SRC_C(0) & ~(0xE4u << 16) | (0x55u << 16),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0,
              "literal range: ignore invalid unused component");
        float constants[D9TEV_MAX_STAGES][4] = {{0}};
        float out[4];
        eval_with(&p, constants, out);
        check_near(out[0], 128.0f / 255.0f, "literal range: selected valid lane");
        check_near(out[3], 1.0f, "literal range: alpha fixup");
    }
    { /* Selecting the invalid lane still rejects instead of clamping it. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0), 0x3FC00000u, 0x3F000000u, 0, 0x3F800000u,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject selected invalid component");
        check(strstr(why, "def c0.x") && strstr(why, "outside [0,1]"),
              "literal range: selected invalid component is named");
    }
    { /* A redefinition must not replace a previously valid literal unchecked. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0), 0x3F000000u, 0, 0, 0,
            DI(0x51, 5), DST_C(0), 0x7FC00000u, 0, 0, 0,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject invalid redefinition");
        check(strstr(why, "def c0.x") && strstr(why, "not finite"),
              "literal range: redefinition reason is named");
    }
    { /* def has exactly one destination and four literals. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 6), DST_C(0), 0, 0, 0, 0, 0,
            DI(0x01, 2), DST_C0, SRC_C(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "literal range: reject extra def operand");
        check(strstr(why, "four literal operands") != NULL,
              "literal range: def arity reason is named");
    }
    { /* Previous wrong output: D3D9 computes -1*2+2 == 0, while silently
       * clamping the literals to RGBA8 first computes 0*1+1 == 1. */
        uint32_t code[] = {
            VER_PS30,
            DI(0x51, 5), DST_C(0), fbits(-1.0f), fbits(-1.0f), fbits(-1.0f), fbits(-1.0f),
            DI(0x51, 5), DST_C(1), fbits(2.0f), fbits(2.0f), fbits(2.0f), fbits(2.0f),
            DI(0x04, 4), DST_C0, SRC_C(0), SRC_C(1), SRC_C(1),
            END_TOKEN,
        };
        float reference = -1.0f * 2.0f + 2.0f;
        int rc = translate(code, sizeof code / sizeof *code, &p, why, sizeof why);
        printf("literal range regression: D3D9 mad(-1,2,2)=%.6f; "
               "old RGBA8-clamped TEV=1.000000\n", reference);
        check_near(reference, 0.0f, "literal range: independent wrong-output reference");
        check(rc < 0, "literal range: reject previous wrong-output shader");
        if (rc == 0) {
            float constants[D9TEV_MAX_STAGES][4] = {{0}};
            float out[4];
            eval_with(&p, constants, out);
            printf("FAIL: previous clamped-literal output was %.6f (expected %.6f)\n",
                   out[0], reference);
        }
        check(strstr(why, "def c0.x") && strstr(why, "outside [0,1]"),
              "literal range: wrong-output rejection is named");
    }
}

static void test_dag_fusion(void) {
    static const uint32_t code[] = {
        VER_PS30,
        DI(0x1F, 2), DCL_COLOR(0), SRC_T(0),
        DI(0x51, 5), DST_C(0), 0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u,
        DI(0x51, 5), DST_C(1), 0x3E800000u, 0x3E800000u, 0x3E800000u, 0x3E800000u,
        DI(0x05, 3), DST_R(0), SRC_C(0), SRC_T(0),
        DI(0x05, 3), DST_R(1), SRC_C(1), SRC_T(0),
        DI(0x02, 3), DST_C0, SRC_R(0), SRC_R(1),
        END_TOKEN,
    };
    D9TevProgram p;
    char why[256];
    check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
    check(p.stages == 2, "DAG fusion: two live MUL+ADD becomes two stages");
    check(p.stage[1].func_rgb == D9TEV_FUNC_MULTIPLY_ADD,
          "DAG fusion: ADD becomes MULTIPLY_ADD");
    float c[D9TEV_MAX_STAGES][4] = {{0}};
    float out[4];
    eval_with(&p, c, out);
    const float q50 = 128.0f / 255.0f, q25 = 64.0f / 255.0f;
    const float expect[4] = {(q50 + q25) * 1.0f, (q50 + q25) * 0.5f,
                             (q50 + q25) * 0.25f, (q50 + q25) * 0.75f};
    for (int i = 0; i < 4; ++i) check_near(out[i], expect[i], "DAG fusion: math");
}

static void test_texkill(void) {
    D9TevProgram p;
    char why[256];
    {
        static const uint32_t code[] = {
            VER_PS30,
            DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
            DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
            DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
            DI(0x41, 1), 0x80FF0000u, /* texkill r0.wwww */
            DI(0x01, 2), DST_C0, SRC_R(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) == 0, why);
        check(p.requires_alpha_test == 1 && p.texkill_count == 1,
              "texkill: folded to output alpha test");
        check(p.stages == 1, "texkill: no combiner stage for discard");
    }
    {
        static const uint32_t code[] = {
            VER_PS30,
            DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
            DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
            DI(0x51, 5), DST_C(0), 0x3F000000u, 0, 0, 0,
            DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
            DI(0x03, 3), DST_R(1), 0x80FF0000u, SRC_CX(0),
            DI(0x41, 1), 0x80FF0001u,
            DI(0x01, 2), DST_C0, SRC_R(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "texkill: reject intermediate scalar");
        check(strstr(why, "texkill") && strstr(why, "intermediate"),
              "texkill: intermediate refusal is named");
    }
    {
        static const uint32_t code[] = {
            VER_PS30,
            DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
            DI(0x1F, 2), DCL_SAMPLER2D, SAMP(0),
            DI(0x42, 3), DST_R(0), SRC_T(0), SAMP(0),
            DI(0x41, 1), SRC_RX(0), /* texkill r0.xxxx */
            DI(0x01, 2), DST_C0, SRC_R(0),
            END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "texkill: reject scalar unlike output alpha");
        check(strstr(why, "not final oC0.a") != NULL,
              "texkill: output-alpha mismatch is named");
    }
}

static void test_rejections(void) {
    D9TevProgram p;
    char why[256];

    { /* every opcode outside the subset must be rejected by name */
        const unsigned supported[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                      0x08, 0x12, 0x1F, 0x42, 0x51};
        for (unsigned op = 0; op <= 0x60; ++op) {
            int skip = 0;
            for (unsigned i = 0; i < sizeof supported / sizeof supported[0]; ++i)
                if (op == supported[i]) skip = 1;
            if (skip) continue;
            uint32_t code[] = {VER_PS30, DI(op, 0), END_TOKEN};
            int rc = translate(code, sizeof code / sizeof *code, &p, why, sizeof why);
            ++checks;
            if (rc >= 0) {
                ++failures;
                printf("FAIL: opcode 0x%02x was accepted\n", op);
                continue;
            }
            const char *name = d9_tev_opcode_name(op);
            if (!strstr(why, name)) {
                ++failures;
                printf("FAIL: opcode 0x%02x rejection does not name '%s': %s\n",
                       op, name, why);
            }
        }
    }

    { /* unsupported opcode cmp */
        static const uint32_t code[] = {
            VER_PS30, DI(0x58, 4), DST_C0, SRC_R(0), SRC_R(1), SRC_R(2), END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: cmp");
        check(strstr(why, "cmp") != NULL, "reject: cmp named");
    }
    { /* branch */
        static const uint32_t code[] = {
            VER_PS30, DI(0x28, 1), SRC_CX(0), DI(0x01, 2), DST_C0, SRC_R(0),
            DI(0x2B, 0), END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: if");
        check(strstr(why, "if") != NULL, "reject: if named");
    }
    { /* cube sampler */
        static const uint32_t code[] = {
            VER_PS30, DI(0x1F, 2), DCL_TEXCOORD(0), TEXD(0),
            DI(0x1F, 2), DCL_SAMPLERCUBE, SAMP(0),
            DI(0x42, 3), DST_C0, SRC_T(0), SAMP(0), END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: cube sampler");
        check(strstr(why, "2D") != NULL, "reject: cube named");
    }
    { /* four texture units */
        uint32_t code[64];
        size_t n = 0;
        code[n++] = VER_PS30;
        for (unsigned i = 0; i < 4; ++i) {
            code[n++] = DI(0x1F, 2); code[n++] = DCL_TEXCOORD(i); code[n++] = TEXD(i);
            code[n++] = DI(0x1F, 2); code[n++] = DCL_SAMPLER2D; code[n++] = SAMP(i);
        }
        for (unsigned i = 0; i < 4; ++i) {
            code[n++] = DI(0x42, 3); code[n++] = DST_R(i); code[n++] = SRC_T(i); code[n++] = SAMP(i);
        }
        code[n++] = DI(0x01, 2); code[n++] = DST_C0; code[n++] = SRC_R(0);
        code[n++] = END_TOKEN;
        check(translate(code, n, &p, why, sizeof why) < 0, "reject: 4 textures");
        check(strstr(why, "texture units") != NULL, "reject: texture limit named");
    }
    { /* MRT */
        static const uint32_t code[] = {
            VER_PS30, DI(0x01, 2), 0x800F0801u, SRC_R(0), END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: MRT oC1");
    }
    { /* non-uniform swizzle */
        static const uint32_t code[] = {
            VER_PS30, DI(0x01, 2), DST_C0, 0xA06C0000u, END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: non-uniform swizzle");
        check(strstr(why, "swizzle") != NULL, "reject: swizzle named");
    }
    { /* source modifier abs */
        static const uint32_t code[] = {
            VER_PS30, DI(0x01, 2), DST_C0, 0xA8E40000u, END_TOKEN,
        };
        check(translate(code, sizeof code / sizeof *code, &p, why, sizeof why) < 0,
              "reject: abs modifier");
        check(strstr(why, "modifier") != NULL, "reject: modifier named");
    }
    { /* seven stages */
        uint32_t code[128];
        size_t n = 0;
        code[n++] = VER_PS30;
        code[n++] = DI(0x51, 5); code[n++] = DST_C(0);
        code[n++] = 0x3F000000u; code[n++] = 0x3F000000u; code[n++] = 0x3F000000u; code[n++] = 0x3F000000u;
        code[n++] = DI(0x01, 2); code[n++] = DST_R(0); code[n++] = SRC_CX(0);
        for (int i = 0; i < 6; ++i) {
            code[n++] = DI(0x05, 3); code[n++] = DST_R(0); code[n++] = SRC_RX(0); code[n++] = SRC_CX(0);
        }
        code[n++] = DI(0x01, 2); code[n++] = DST_C0; code[n++] = SRC_R(0);
        code[n++] = END_TOKEN;
        check(translate(code, n, &p, why, sizeof why) < 0, "reject: >6 stages");
        check(strstr(why, "TEV stages") != NULL, "reject: stage limit named");
    }
}

static void test_pack(void) {
    float rgba[4] = {0.0f, 0.5f, 1.0f, 0.25f};
    uint32_t packed = d9_tev_pack_rgba8(rgba);
    check((packed & 0xFFu) == 0u, "pack: r");
    check(((packed >> 8) & 0xFFu) == 128u, "pack: g");
    check(((packed >> 16) & 0xFFu) == 255u, "pack: b");
    check(((packed >> 24) & 0xFFu) == 64u, "pack: a");
}

int main(void) {
    test_blit();
    test_texcoord_alias();
    test_tint();
    test_channel_carry();
    test_split_channel_sources();
    test_lrp();
    test_mad_two_constants();
    test_dp3();
    test_alpha_fixup();
    test_texld_validation();
    test_literal_validation();
    test_dag_fusion();
    test_texkill();
    test_rejections();
    test_pack();
    printf("tev translate: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
