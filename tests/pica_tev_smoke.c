/* Host tests for the honest PICA200 backend gate and descriptor.
 * All shader tokens below are assembled test data; no game bytes are used.
 *
 * Standalone build (from the repository root, in a VS developer command prompt):
 *   clang -std=gnu11 -O2 -Wall -Wextra -Isource \
 *       source/d3d9_tev.c source/d3d9_pica_tev.c tests/pica_tev_smoke.c \
 *       -o pica_tev_smoke.exe
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_pica_tev.h"
#include "d3d9_menu_light.h"

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define SW_ID 0xE4u

typedef struct {
    uint32_t word[128];
    size_t count;
} Asm;

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static uint32_t dr(unsigned type, unsigned reg, unsigned mask) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((mask & 0xFu) << 16);
}

static uint32_t sr(unsigned type, unsigned reg, unsigned swizzle) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((swizzle & 0xFFu) << 16);
}

static uint32_t fbits(float value) {
    union { float f; uint32_t u; } bits;
    bits.f = value;
    return bits.u;
}

static void emit(Asm *a, uint32_t word) {
    if (a->count < sizeof a->word / sizeof a->word[0]) a->word[a->count++] = word;
}

static void begin(Asm *a) {
    memset(a, 0, sizeof *a);
    emit(a, 0xFFFF0300u);
}

static void end(Asm *a) { emit(a, 0xFFFFu); }

static void dcl(Asm *a, unsigned usage, unsigned usage_index,
                unsigned texture_type, unsigned type, unsigned reg) {
    emit(a, DI(0x1Fu, 2u));
    emit(a, 0x80000000u | (usage & 0x1Fu) | ((usage_index & 0xFu) << 16) |
             ((texture_type & 0xFu) << 27));
    emit(a, dr(type, reg, 0xFu));
}

static void def(Asm *a, unsigned reg, float r, float g, float b, float alpha) {
    emit(a, DI(0x51u, 5u));
    emit(a, dr(2u, reg, 0xFu));
    emit(a, fbits(r)); emit(a, fbits(g)); emit(a, fbits(b)); emit(a, fbits(alpha));
}

static void texld(Asm *a, unsigned dst_reg, unsigned sampler) {
    emit(a, DI(0x42u, 3u));
    emit(a, dr(0u, dst_reg, 0xFu));
    emit(a, sr(1u, 0u, SW_ID));
    emit(a, sr(10u, sampler, SW_ID));
}

static void alu(Asm *a, unsigned op, uint32_t dst,
                uint32_t src0, uint32_t src1, uint32_t src2, unsigned sources) {
    emit(a, DI(op, 1u + sources));
    emit(a, dst);
    emit(a, src0);
    if (sources > 1) emit(a, src1);
    if (sources > 2) emit(a, src2);
}

static void mov(Asm *a, uint32_t dst, uint32_t src) {
    alu(a, 0x01u, dst, src, 0, 0, 1);
}

static void textured_decls(Asm *a, unsigned sampler) {
    dcl(a, 5u, 0u, 0u, 1u, 0u); /* dcl_texcoord v0 */
    dcl(a, 0u, 0u, 2u, 10u, sampler); /* dcl_2d s# */
}

static int validate_asm(const Asm *a, D9TevProgram *translated,
                        D9PicaTevProgram *descriptor, D9PicaTevReject *reason,
                        char *why, size_t why_size) {
    return d9_pica_tev_validate_pixel(a->word, a->count, translated, descriptor,
                                      reason, why, why_size);
}

static void expect_reject(const char *name, const Asm *a, D9PicaTevReject expected,
                          const char *needle) {
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason = D9_PICA_TEV_REJECT_COUNT;
    char why[256];
    memset(&translated, 0xA5, sizeof translated);
    memset(&descriptor, 0xA5, sizeof descriptor);
    int rc = validate_asm(a, &translated, &descriptor, &reason, why, sizeof why);
    if (rc >= 0) printf("  accepted unexpectedly: %s\n", name);
    if (reason != expected)
        printf("  %s: reason %s, expected %s (%s)\n", name,
               d9_pica_tev_reject_name(reason), d9_pica_tev_reject_name(expected), why);
    check(rc < 0, name);
    check(reason == expected, name);
    check(strstr(why, needle) != NULL, name);
    if (!strstr(why, needle)) printf("  %s: missing '%s' in: %s\n", name, needle, why);
    if (rc < 0) {
        const uint8_t *bytes = (const uint8_t *)&translated;
        size_t nonzero = 0;
        for (size_t i = 0; i < sizeof translated; ++i) nonzero |= bytes[i];
        check(nonzero == 0, "rejection zeroes translated output");
    }
}

static void test_constant_blit(void) {
    Asm a;
    begin(&a);
    def(&a, 0u, 0.25f, 0.5f, 0.75f, 1.0f);
    mov(&a, dr(8u, 0u, 0xFu), sr(2u, 0u, SW_ID));
    end(&a);

    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(descriptor.stage_count == 1, "constant blit: one stage");
    check(descriptor.texture_count == 0, "constant blit: no texture");
    check(descriptor.sampler_reg == D9_PICA_TEV_NO_SAMPLER,
          "constant blit: absent sampler marker");
    check(descriptor.texcoord_reg == D9_PICA_TEV_NO_SAMPLER,
          "constant blit: absent texcoord marker");
    check(descriptor.stage[0].func_rgb == D9TEV_FUNC_REPLACE,
          "constant blit: REPLACE");
    check(descriptor.stage[0].src_rgb[0] == D9TEV_SRC_CONST,
          "constant blit: constant source");
    check(descriptor.stage[0].uses_constant == 1,
          "constant blit: literal constant");
    check(descriptor.stage[0].constant_abgr == 0x4080BFFFu,
          "constant blit: RGBA8 to ABGR8888");
}

static void make_textured_blit(Asm *a) {
    begin(a);
    textured_decls(a, 0u);
    texld(a, 0u, 0u);
    mov(a, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID));
    end(a);
}

static void test_textured_blit(void) {
    Asm a;
    make_textured_blit(&a);
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(translated.stages == 1, "textured blit: one translated stage");
    check(descriptor.stage_count == 1, "textured blit: one descriptor stage");
    check(descriptor.texture_count == 1, "textured blit: one texture");
    check(descriptor.sampler_reg == 0 && descriptor.texcoord_reg == 0,
          "textured blit: sampler/texcoord 0");
    check(descriptor.stage[0].src_rgb[0] == D9TEV_SRC_TEX0,
          "textured blit: TEXTURE0");
    check(descriptor.stage[0].func_rgb == D9TEV_FUNC_REPLACE,
          "textured blit: REPLACE");
    check(descriptor.stage[0].uses_constant == 0,
          "textured blit: no constant");
}

static void test_texkill_descriptor(void) {
    Asm a;
    begin(&a);
    textured_decls(&a, 0u);
    texld(&a, 0u, 0u);
    emit(&a, DI(0x41u, 1u));
    emit(&a, sr(0u, 0u, 0xFFu));
    mov(&a, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID));
    end(&a);
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(translated.requires_alpha_test && translated.texkill_count == 1,
          "texkill: translator flag");
    check(descriptor.requires_alpha_test == 1,
          "texkill: PICA descriptor carries alpha-test requirement");
}

static void make_tint(Asm *a) {
    begin(a);
    textured_decls(a, 0u);
    def(a, 0u, 0.5f, 0.25f, 1.0f, 1.0f);
    texld(a, 0u, 0u);
    alu(a, 0x05u, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID), sr(2u, 0u, SW_ID), 0, 2u);
    end(a);
}

static void test_tint(void) {
    Asm a;
    make_tint(&a);
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(descriptor.stage_count == 1, "tint: one stage");
    check(descriptor.stage[0].func_rgb == D9TEV_FUNC_MODULATE &&
          descriptor.stage[0].func_a == D9TEV_FUNC_MODULATE,
          "tint: MODULATE RGB and alpha");
    check(descriptor.stage[0].src_rgb[0] == D9TEV_SRC_TEX0 &&
          descriptor.stage[0].src_rgb[1] == D9TEV_SRC_CONST,
          "tint: texture * constant");
    check(descriptor.stage[0].uses_constant == 1,
          "tint: literal constant");
}

static void make_textured_chain(Asm *a, unsigned multiplies) {
    begin(a);
    textured_decls(a, 0u);
    for (unsigned i = 0; i < multiplies; ++i)
        def(a, i, 0.5f, 0.5f, 0.5f, 1.0f);
    texld(a, 0u, 0u);
    for (unsigned i = 0; i < multiplies; ++i)
        alu(a, 0x05u, dr(0u, 0u, 0xFu), sr(0u, 0u, SW_ID), sr(2u, i, SW_ID), 0, 2u);
    mov(a, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID));
    end(a);
}

static void test_textured_chain(void) {
    Asm a;
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];

    make_textured_chain(&a, 2u);
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(descriptor.stage_count == 3, "textured chain: three stages");
    check(descriptor.stage[0].func_rgb == D9TEV_FUNC_MODULATE &&
          descriptor.stage[1].func_rgb == D9TEV_FUNC_MODULATE &&
          descriptor.stage[2].func_rgb == D9TEV_FUNC_REPLACE,
          "textured chain: PREVIOUS chain");
    check(descriptor.stage[1].src_rgb[0] == D9TEV_SRC_PREV &&
          descriptor.stage[2].src_rgb[0] == D9TEV_SRC_PREV,
          "textured chain: stage 0 seeds PREVIOUS");
    D9PicaTevStage zero;
    memset(&zero, 0, sizeof zero);
    for (unsigned i = descriptor.stage_count; i < D9_PICA_TEV_MAX_STAGES; ++i)
        check(memcmp(&descriptor.stage[i], &zero, sizeof zero) == 0,
              "descriptor tail is zero");

    make_textured_chain(&a, 5u);
    check(validate_asm(&a, &translated, &descriptor, &reason, why, sizeof why) == 0,
          why);
    check(descriptor.stage_count == 6, "textured chain: exactly six stages accepted");
}

static D9TevProgram accepted_blit_program(void) {
    Asm a;
    make_textured_blit(&a);
    D9TevProgram program;
    D9PicaTevReject reason;
    char why[256];
    if (validate_asm(&a, &program, NULL, &reason, why, sizeof why)) {
        fprintf(stderr, "test setup failed: %s\n", why);
        failures++;
        memset(&program, 0, sizeof program);
    }
    return program;
}

static void expect_descriptor_reject(const char *name, const D9TevProgram *program,
                                     D9PicaTevReject expected, const char *needle) {
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason = D9_PICA_TEV_REJECT_COUNT;
    char why[256];
    memset(&descriptor, 0xA5, sizeof descriptor);
    int rc = d9_pica_tev_validate(program, &descriptor, &reason, why, sizeof why);
    check(rc < 0, name);
    check(reason == expected, name);
    check(strstr(why, needle) != NULL, name);
}

static void test_descriptor_rejections(void) {
    D9TevProgram p = accepted_blit_program();

    p.textures = 2;
    p.texture[1] = p.texture[0];
    p.texture[1].sampler_reg = 1;
    expect_descriptor_reject("reject multiple textures", &p,
                             D9_PICA_TEV_TEXTURE_COUNT, "multiple textures");
    p = accepted_blit_program();

    p.stage[0].src_rgb[0] = p.stage[0].src_a[0] = D9TEV_SRC_CONST;
    p.stage[0].op_rgb[0] = D9TEV_RGB_COLOR;
    p.stage[0].op_a[0] = D9TEV_A_ALPHA;
    p.stage[0].const_reg = 7;
    p.stage[0].const_is_literal = 0;
    p.constants = 1;
    p.constant_reg[0] = 7;
    D9PicaTevProgram external;
    D9PicaTevReject external_reason;
    char external_why[256];
    check(d9_pica_tev_validate(&p, &external, &external_reason,
                               external_why, sizeof external_why) == 0,
          "generic external constant accepted");
    check(external.constant_count == 1 && external.constant_reg[0] == 7 &&
          external.stage[0].const_is_literal == 0,
          "generic external c7 reaches descriptor");
    p = accepted_blit_program();

    p.uses_vertex_color = 1;
    expect_descriptor_reject("reject vertex colour flag", &p,
                             D9_PICA_TEV_VERTEX_COLOR, "vertex colour");
    p = accepted_blit_program();

    p.inputs = 1;
    p.input[0].usage = D9TEV_USAGE_POSITION;
    expect_descriptor_reject("reject VPOS metadata", &p,
                             D9_PICA_TEV_VPOS, "VPOS");
    p = accepted_blit_program();

    p.stage[0].src_rgb[0] = D9TEV_SRC_FRAG_PRIMARY;
    p.stage[0].src_a[0] = D9TEV_SRC_FRAG_SECONDARY;
    expect_descriptor_reject("reject fragment varyings", &p,
                             D9_PICA_TEV_FRAGMENT_VARYING, "fragment varying");
    p = accepted_blit_program();

    p.stage[0].src_rgb[0] = D9TEV_SRC_PRIMARY;
    expect_descriptor_reject("reject PRIMARY source", &p,
                             D9_PICA_TEV_VERTEX_COLOR, "PRIMARY");
    p = accepted_blit_program();

    p.stages = 7;
    expect_descriptor_reject("reject >6 descriptor stages", &p,
                             D9_PICA_TEV_STAGE_COUNT, "7 TEV stages");
    p = accepted_blit_program();

    p.texture[0].sampler_reg = 1;
    expect_descriptor_reject("reject sampler 1", &p,
                             D9_PICA_TEV_SAMPLER, "sampler s1");
    p = accepted_blit_program();

    p.texture[0].sampler_type = 3;
    expect_descriptor_reject("reject cube sampler", &p,
                             D9_PICA_TEV_SAMPLER_TYPE, "2D");
    p = accepted_blit_program();

    p.textures = 0;
    expect_descriptor_reject("reject texture source without texture", &p,
                             D9_PICA_TEV_TEXTURE_SOURCE, "no texture");
    p = accepted_blit_program();

    p.stage[0].src_rgb[0] = D9TEV_SRC_TEX1;
    expect_descriptor_reject("reject texture unit 1 source", &p,
                             D9_PICA_TEV_TEXTURE_SOURCE, "texture unit 1");
    p = accepted_blit_program();

    p.stage[0].src_rgb[0] = 0x0D; /* GPU_PREVIOUS_BUFFER */
    expect_descriptor_reject("reject PREVIOUS_BUFFER source", &p,
                             D9_PICA_TEV_TEV_SOURCE, "PREVIOUS_BUFFER");
    p = accepted_blit_program();

    p.stage[0].func_rgb = 10;
    expect_descriptor_reject("reject invalid combine function", &p,
                             D9_PICA_TEV_TEV_FUNCTION, "combine function");
    p = accepted_blit_program();

    p.stage[0].scale_rgb = 3;
    expect_descriptor_reject("reject invalid scale", &p,
                             D9_PICA_TEV_TEV_SCALE, "scale");
    p = accepted_blit_program();

    p.stage[0].op_rgb[0] = 6;
    expect_descriptor_reject("reject invalid RGB operand", &p,
                             D9_PICA_TEV_TEV_OPERAND, "RGB selector");
    p = accepted_blit_program();

    p.stage[0].func_rgb = D9TEV_FUNC_MODULATE;
    p.stage[0].func_a = D9TEV_FUNC_MODULATE;
    p.stage[0].src_rgb[0] = D9TEV_SRC_PREV;
    expect_descriptor_reject("reject meaningful stage-0 PREVIOUS", &p,
                             D9_PICA_TEV_TEV_SOURCE, "PREVIOUS");
    p = accepted_blit_program();

    p.textures = UINT32_MAX;
    expect_descriptor_reject("reject malformed texture count", &p,
                             D9_PICA_TEV_TEXTURE_COUNT, "invalid texture count");
}

static void test_raw_rejections(void) {
    Asm a;

    { /* Two sampled units. */
        begin(&a);
        dcl(&a, 5u, 0u, 0u, 1u, 0u);
        dcl(&a, 0u, 0u, 2u, 10u, 0u);
        dcl(&a, 0u, 0u, 2u, 10u, 1u);
        texld(&a, 0u, 0u); texld(&a, 1u, 1u);
        alu(&a, 0x02u, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID), sr(0u, 1u, SW_ID), 0, 2u);
        end(&a);
        expect_reject("reject raw sampler 1 before multi-texture gate", &a,
                      D9_PICA_TEV_SAMPLER, "sampler s1");
        D9TevProgram translated;
        char translate_why[256];
        check(d9_tev_translate_pixel(a.word, a.count, &translated,
                                     translate_why, sizeof translate_why) == 0,
              "two-texture program reaches backend gate");
        expect_descriptor_reject("reject translated multiple textures", &translated,
                                 D9_PICA_TEV_TEXTURE_COUNT, "multiple textures");
    }
    { /* c0 without def is an external constant, supplied by the draw bank. */
        D9TevProgram translated;
        D9PicaTevProgram descriptor;
        D9PicaTevReject reason;
        char why[256];
        begin(&a);
        mov(&a, dr(8u, 0u, 0xFu), sr(2u, 0u, SW_ID));
        end(&a);
        check(validate_asm(&a, &translated, &descriptor, &reason,
                           why, sizeof why) == 0,
              "external constant c0 reaches generic descriptor");
        check(descriptor.constant_count == 1 && descriptor.constant_reg[0] == 0 &&
              descriptor.stage[0].uses_constant &&
              !descriptor.stage[0].const_is_literal,
              "external constant c0 is non-literal");
    }
    { /* Vertex colour declaration, even before translation. */
        begin(&a);
        dcl(&a, 10u, 0u, 0u, 1u, 0u);
        mov(&a, dr(8u, 0u, 0xFu), sr(1u, 0u, SW_ID));
        end(&a);
        expect_reject("reject raw vertex colour", &a,
                      D9_PICA_TEV_VERTEX_COLOR, "dcl_color");
    }
    { /* ps_3_0 VPOS misc register. */
        begin(&a);
        dcl(&a, 0u, 0u, 0u, 17u, 0u);
        mov(&a, dr(8u, 0u, 0xFu), sr(17u, 0u, SW_ID));
        end(&a);
        expect_reject("reject raw VPOS", &a, D9_PICA_TEV_VPOS, "VPOS");
    }
    { /* Even an unused VPOS declaration is outside the backend contract. */
        begin(&a);
        def(&a, 0u, 1.0f, 1.0f, 1.0f, 1.0f);
        dcl(&a, 0u, 0u, 0u, 17u, 0u);
        mov(&a, dr(8u, 0u, 0xFu), sr(2u, 0u, SW_ID));
        end(&a);
        expect_reject("reject unused VPOS declaration", &a,
                      D9_PICA_TEV_VPOS, "VPOS");
    }
    { /* Seven translated stages. */
        make_textured_chain(&a, 6u);
        expect_reject("reject raw >6 stages", &a,
                      D9_PICA_TEV_TRANSLATION, "TEV stages");
    }
    { /* Sampler 1, even though it is the only sampled unit. */
        begin(&a);
        textured_decls(&a, 1u);
        texld(&a, 0u, 1u);
        mov(&a, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID));
        end(&a);
        expect_reject("reject raw sampler 1", &a,
                      D9_PICA_TEV_SAMPLER, "sampler s1");
    }
    { /* Cube sampler. */
        begin(&a);
        dcl(&a, 5u, 0u, 0u, 1u, 0u);
        dcl(&a, 0u, 0u, 3u, 10u, 0u);
        texld(&a, 0u, 0u);
        mov(&a, dr(8u, 0u, 0xFu), sr(0u, 0u, SW_ID));
        end(&a);
        expect_reject("reject raw cube sampler", &a,
                      D9_PICA_TEV_SAMPLER_TYPE, "2D");
    }
    { /* A generic PS input would need a fragment-colour varying. */
        begin(&a);
        dcl(&a, 3u, 0u, 0u, 1u, 0u);
        mov(&a, dr(8u, 0u, 0xFu), sr(1u, 0u, SW_ID));
        end(&a);
        expect_reject("reject raw non-texcoord input", &a,
                      D9_PICA_TEV_INPUT_USAGE, "NORMAL");
    }
    { /* Missing END is rejected by the preflight. */
        begin(&a);
        def(&a, 0u, 1.0f, 1.0f, 1.0f, 1.0f);
        mov(&a, dr(8u, 0u, 0xFu), sr(2u, 0u, SW_ID));
        expect_reject("reject missing end token", &a,
                      D9_PICA_TEV_MALFORMED, "end token");
    }
}

static void test_literal_only_stage(void) {
    Asm a;
    make_tint(&a);
    D9TevProgram translated;
    D9PicaTevProgram descriptor;
    D9PicaTevReject reason;
    char why[256];
    check(validate_asm(&a, &translated, NULL, &reason, why, sizeof why) == 0, why);
    translated.stage[0].const_is_literal = 0;
    check(d9_pica_tev_validate(&translated, &descriptor, &reason,
                               why, sizeof why) == 0,
          "externalised tint stage reaches descriptor");
    check(descriptor.constant_count == 1 &&
          descriptor.stage[0].uses_constant &&
          !descriptor.stage[0].const_is_literal,
          "externalised tint keeps a non-literal stage constant");
}

static D9TevProgram menu_light_plan(void) {
    D9MenuLightMatch match;
    D9TevProgram plan;
    char why[256];
    memset(&match, 0, sizeof match);
    match.signature = D9_MENU_LIGHT_P1200005_ADD_NORMAL;
    match.instructions = 25;
    match.normal_decode[0] = -0.5f;
    match.normal_decode[1] = 1.0f;
    if (d9_menu_light_plan(&match, &plan, why, sizeof why)) {
        fprintf(stderr, "menu light plan setup failed: %s\n", why);
        ++failures;
        memset(&plan, 0, sizeof plan);
    }
    return plan;
}

static D9TevProgram menu_fixed_plan(int fog) {
    D9TevProgram p;
    memset(&p, 0, sizeof p);
    p.version = 0xFFFF0300u;
    p.stage_limit = D9TEV_MAX_STAGES;
    p.backend_flags = D9TEV_PLAN_MENU_FIXED;
    p.textures = fog ? 2u : 1u;
    p.texture[0].texcoord_reg = 0;
    p.texture[0].sampler_reg = 0;
    p.texture[0].sampler_type = 2;
    if (fog) {
        p.texture[1].texcoord_reg = 0;
        p.texture[1].sampler_reg = 1;
        p.texture[1].sampler_type = 2;
    }
    p.inputs = 1;
    p.input[0].usage = D9TEV_USAGE_COLOR;
    p.input[0].usage_index = 0;
    p.input[0].reg_index = 1;
    p.input[0].tev_source = D9TEV_SRC_PRIMARY;
    p.uses_vertex_color = 1;
    p.stages = fog ? 2u : 1u;
    D9TevStage *s = &p.stage[0];
    s->func_rgb = s->func_a = D9TEV_FUNC_MODULATE;
    s->src_rgb[0] = s->src_a[0] = D9TEV_SRC_TEX0;
    s->op_rgb[0] = D9TEV_RGB_COLOR;
    s->op_a[0] = D9TEV_A_ALPHA;
    s->src_rgb[1] = s->src_a[1] = D9TEV_SRC_PRIMARY;
    s->op_rgb[1] = D9TEV_RGB_COLOR;
    s->op_a[1] = D9TEV_A_ALPHA;
    s->const_reg = 0xFF;
    if (fog) {
        s = &p.stage[1];
        s->func_rgb = s->func_a = D9TEV_FUNC_MODULATE;
        s->src_rgb[0] = s->src_a[0] = D9TEV_SRC_TEX1;
        s->op_rgb[0] = D9TEV_RGB_COLOR;
        s->op_a[0] = D9TEV_A_ALPHA;
        s->src_rgb[1] = s->src_a[1] = D9TEV_SRC_PREV;
        s->op_rgb[1] = D9TEV_RGB_COLOR;
        s->op_a[1] = D9TEV_A_ALPHA;
        s->const_reg = 0xFF;
    }
    return p;
}

static int descriptor_has_constant(const D9PicaTevProgram *d, unsigned reg) {
    for (unsigned i = 0; i < d->constant_count; ++i)
        if (d->constant_reg[i] == reg) return 1;
    return 0;
}

static void test_menu_descriptors(void) {
    D9PicaTevProgram d;
    D9PicaTevReject reason;
    char why[256];

    D9TevProgram p = menu_light_plan();
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) == 0, why);
    check(d.stage_count == 6, "menu light: c4+ambient stage split to six");
    check(d.texture_count == 2, "menu light: two texture units");
    check(d.sampler_reg == 0 && d.texcoord_reg == 0 &&
          d.sampler_reg1 == 1 && d.texcoord_reg1 == 0,
          "menu light: s0/uv0 and s1/uv0");
    check(d.uses_primary_color == 1, "menu light: color0 primary");
    check(d.uses_ambient_fold == 1, "menu light: color1 ambient fold");
    check(descriptor_has_constant(&d, 3) && descriptor_has_constant(&d, 4) &&
          descriptor_has_constant(&d, 7) && descriptor_has_constant(&d, 8),
          "menu light: c3/c4/c7/c8 constants");
    check(d.stage[0].const_reg == 3 && d.stage[1].const_reg == 4 &&
          d.stage[2].const_reg == 8 && d.stage[5].const_reg == 7,
          "menu light: constant stage order");
    check(d.stage[1].func_rgb == D9TEV_FUNC_MODULATE &&
          d.stage[2].func_rgb == D9TEV_FUNC_ADD,
          "menu light: MUL + ADD ambient fold");
    check(d.stage[3].src_rgb[0] == D9TEV_SRC_TEX0 &&
          d.stage[3].src_rgb[1] == D9TEV_SRC_PREV &&
          d.stage[4].src_rgb[1] == D9TEV_SRC_PRIMARY,
          "menu light: diffuse against light then color0");

    /* fog/tint fixed plans: color0 + one/two textures, no external constant. */
    p = menu_fixed_plan(0);
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) == 0, why);
    check(d.texture_count == 1 && d.uses_primary_color == 1 &&
          d.constant_count == 0, "tint: one texture and primary color");
    p = menu_fixed_plan(1);
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) == 0, why);
    check(d.texture_count == 2 && d.sampler_reg1 == 1 &&
          d.uses_primary_color == 1, "fog: two textures and primary color");

    /* Negative: a third texture unit. */
    p = menu_light_plan();
    p.textures = 3;
    p.texture[2] = p.texture[1];
    p.texture[2].sampler_reg = 2;
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) < 0 &&
          reason == D9_PICA_TEV_TEXTURE_COUNT, "menu: third texture rejected");

    /* Negative: missing s1 slot while a stage reads TEXTURE1. */
    p = menu_fixed_plan(1);
    p.texture[1].sampler_reg = 0;
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) < 0 &&
          reason == D9_PICA_TEV_SAMPLER, "menu: duplicate/missing s1 rejected");

    /* Negative: external constant outside the 32-row derived bank. */
    p = menu_fixed_plan(0);
    p.stage[0].const_is_literal = 0;
    p.stage[0].const_reg = 32;
    p.stage[0].src_rgb[1] = p.stage[0].src_a[1] = D9TEV_SRC_CONST;
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) < 0 &&
          reason == D9_PICA_TEV_EXTERNAL_CONSTANT &&
          strstr(why, "c32") != NULL, "menu: external c32 rejected");
    /* c9..c31 are legal derived-plan rows: the backend folds them into a
     * per-stage literal colour, so there is no eight-row hardware bank. */
    p.stage[0].const_reg = 9;
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) == 0,
          "menu: external c9 accepted");

    /* Negative: color1 without the menu-light ambient fold. */
    p = menu_fixed_plan(0);
    p.inputs = 2;
    p.input[1].usage = D9TEV_USAGE_COLOR;
    p.input[1].usage_index = 1;
    p.input[1].reg_index = 2;
    p.input[1].tev_source = D9TEV_SRC_FRAG_PRIMARY;
    check(d9_pica_tev_validate(&p, &d, &reason, why, sizeof why) < 0 &&
          reason == D9_PICA_TEV_FRAGMENT_VARYING,
          "menu: color1 without fold rejected");
}

static void test_helpers(void) {
    check(d9_pica_tev_color_abgr(0x11223344u) == 0x44332211u,
          "ABGR conversion");
    check(strcmp(d9_pica_tev_reject_name(D9_PICA_TEV_VPOS), "vpos") == 0,
          "rejection name");
    D9PicaTevReject reason = D9_PICA_TEV_REJECT_COUNT;
    char why[256];
    check(d9_pica_tev_validate(NULL, NULL, &reason, why, sizeof why) < 0,
          "reject null program");
    check(reason == D9_PICA_TEV_NULL_PROGRAM && strstr(why, "null"),
          "null rejection named");
}

int main(void) {
    test_constant_blit();
    test_textured_blit();
    test_texkill_descriptor();
    test_tint();
    test_textured_chain();
    test_descriptor_rejections();
    test_raw_rejections();
    test_literal_only_stage();
    test_menu_descriptors();
    test_helpers();
    printf("pica TEV backend: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
