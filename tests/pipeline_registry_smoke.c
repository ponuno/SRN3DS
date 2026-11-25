/* Host tests for the specialised pipeline registry. All shader streams are
 * assembled from semantic fields; no retail bytecode is embedded. Real chunk
 * coverage is measured separately by tests/pipeline_real.c.
 */

#include "d3d9_pipeline_api.h"
#include "d3d9_menu_light.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DI(op, extra) ((uint32_t)(op) | ((uint32_t)(extra) << 24))
#define SW_ID 0xE4u
#define SW_X 0x00u
#define SW_Y 0x55u
#define SW_Z 0xAAu
#define SW_W 0xFFu
#define SW_XZYW 0xD8u
#define VER_PS30 0xFFFF0300u
#define END_TOKEN 0xFFFFu

static int checks, failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static uint32_t dr(unsigned type, unsigned reg, unsigned mask, unsigned mod) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((mask & 0xFu) << 16) | ((mod & 0xFu) << 20);
}

static uint32_t sr(unsigned type, unsigned reg, unsigned sw, unsigned mod) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((sw & 0xFFu) << 16) | ((mod & 0xFu) << 24);
}

static uint32_t fbits(float value) {
    union { float f; uint32_t u; } bits;
    bits.f = value;
    return bits.u;
}

static void put_alu(uint32_t *code, size_t *n, unsigned op, uint32_t dst,
                    uint32_t a, uint32_t b, uint32_t c, unsigned sources) {
    code[(*n)++] = DI(op, 1u + sources);
    code[(*n)++] = dst;
    code[(*n)++] = a;
    if (sources > 1u) code[(*n)++] = b;
    if (sources > 2u) code[(*n)++] = c;
}

static void put_texld(uint32_t *code, size_t *n, unsigned dst, unsigned sampler) {
    code[(*n)++] = DI(0x42u, 3u);
    code[(*n)++] = dr(0u, dst, 0xFu, 0u);
    code[(*n)++] = sr(1u, 0u, SW_ID, 0u);
    code[(*n)++] = sr(10u, sampler, SW_ID, 0u);
}

static void put32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static size_t find_opcode(const uint32_t *code, size_t words, unsigned opcode) {
    size_t pos = 1;
    while (pos < words) {
        uint32_t token = code[pos];
        unsigned op = token & 0xFFFFu;
        if (op == END_TOKEN) break;
        if (op == 0xFFFEu) {
            pos += 1u + ((token >> 16) & 0x7FFFu);
            continue;
        }
        if (op == opcode) return pos;
        pos += 1u + ((token >> 24) & 0xFu);
    }
    return words;
}

/* Build a valid first CTAB comment with bounded constant names. */
static size_t put_ctab(uint32_t *code, size_t at, const char *const *names,
                       unsigned count) {
    unsigned char body[1024];
    memset(body, 0, sizeof body);
    memcpy(body, "CTAB", 4);
    unsigned char *table = body + 4;
    size_t info = 28;
    size_t cursor = info + 20u * count;
    put16(table + 12, count);
    put32(table + 16, info);
    for (unsigned i = 0; i < count; ++i) {
        size_t length = strlen(names[i]) + 1u;
        unsigned char *entry = table + info + 20u * i;
        put32(entry, (uint32_t)cursor);
        put32(entry + 4, 2u);       /* float constant register set */
        put32(entry + 8, (uint32_t)i);
        put32(entry + 12, 1u);      /* one register */
        memcpy(table + cursor, names[i], length);
        cursor += (length + 3u) & ~3u;
    }
    size_t bytes = 4u + ((cursor + 3u) & ~3u);
    size_t dwords = bytes / 4u;
    code[at++] = 0xFFFEu | ((uint32_t)dwords << 16);
    for (size_t i = 0; i < dwords; ++i) {
        code[at++] = (uint32_t)body[4u * i] |
                     ((uint32_t)body[4u * i + 1u] << 8) |
                     ((uint32_t)body[4u * i + 2u] << 16) |
                     ((uint32_t)body[4u * i + 3u] << 24);
    }
    return at;
}

static size_t assemble_copy(uint32_t *code, const char *ctab_name) {
    size_t n = 0;
    code[n++] = VER_PS30;
    if (ctab_name) {
        const char *names[] = {ctab_name};
        n = put_ctab(code, n, names, 1);
    }
    code[n++] = DI(0x1Fu, 2u);
    code[n++] = 0x80000005u;
    code[n++] = dr(1u, 0u, 0x3u, 0u);
    code[n++] = DI(0x1Fu, 2u);
    code[n++] = 0x90000000u;
    code[n++] = dr(10u, 0u, 0xFu, 0u);
    put_texld(code, &n, 0u, 0u);
    /* put_texld targets r0; replace it with the direct oC0 copy. */
    code[n - 3u] = dr(8u, 0u, 0xFu, 0u);
    code[n++] = END_TOKEN;
    return n;
}

static size_t assemble_tint(uint32_t *code) {
    size_t n = 0;
    code[n++] = VER_PS30;
    code[n++] = DI(0x1Fu, 2u);
    code[n++] = 0x80000005u;
    code[n++] = dr(1u, 0u, 0x3u, 0u);
    code[n++] = DI(0x1Fu, 2u);
    code[n++] = 0x90000000u;
    code[n++] = dr(10u, 0u, 0xFu, 0u);
    code[n++] = DI(0x51u, 5u);
    code[n++] = dr(2u, 0u, 0xFu, 0u);
    code[n++] = fbits(0.5f);
    code[n++] = fbits(0.25f);
    code[n++] = fbits(1.0f);
    code[n++] = fbits(1.0f);
    put_texld(code, &n, 0u, 0u);
    put_alu(code, &n, 0x05u, dr(8u, 0u, 0xFu, 0u),
            sr(0u, 0u, SW_ID, 0u), sr(2u, 0u, SW_ID, 0u), 0u, 2u);
    code[n++] = END_TOKEN;
    return n;
}

static const char *const base_lit_names[] = {
    "DiffuseSampler", "NormalSampler", "DirLightColor", "DirLightDirection",
    "DirectionalAmbient", "EmitColor", "FogColor", "MaterialData"
};

static size_t assemble_static(uint32_t *code, int add_normal,
                              const char *const *names, unsigned name_count) {
    size_t n = 0;
    code[n++] = VER_PS30;
    if (names) n = put_ctab(code, n, names, name_count);
    code[n++] = DI(0x51u, 5u);
    code[n++] = dr(2u, 8u, 0xFu, 0u);
    code[n++] = fbits(add_normal ? -0.5f : 2.0f);
    code[n++] = fbits(add_normal ? 1.0f : -1.0f);
    code[n++] = fbits(add_normal ? 0.0f : 1.0f);
    code[n++] = 0u;
    code[n++] = DI(0x1Fu, 2u); code[n++] = 0x80000005u; code[n++] = dr(1u, 0u, 0x3u, 0u);
    code[n++] = DI(0x1Fu, 2u); code[n++] = 0x8000000Au; code[n++] = dr(1u, 1u, 0xFu, 0u);
    code[n++] = DI(0x1Fu, 2u); code[n++] = 0x8001000Au; code[n++] = dr(1u, 2u, 0xFu, 0u);
    code[n++] = DI(0x1Fu, 2u); code[n++] = 0x90000000u; code[n++] = dr(10u, 0u, 0xFu, 0u);
    code[n++] = DI(0x1Fu, 2u); code[n++] = 0x90000000u; code[n++] = dr(10u, 1u, 0xFu, 0u);
    put_texld(code, &n, 0u, 0u);
    put_alu(code, &n, 0x05u, dr(0u, 0u, 0x8u, 0u), sr(0u, 0u, SW_W, 0u), sr(2u, 5u, SW_Z, 0u), 0u, 2u);
    put_alu(code, &n, 0x05u, dr(8u, 0u, 0x8u, 2u), sr(0u, 0u, SW_W, 0u), sr(1u, 1u, SW_W, 0u), 0u, 2u);
    put_texld(code, &n, 1u, 1u);
    if (add_normal) {
        put_alu(code, &n, 0x02u, dr(0u, 1u, 0x7u, 0u), sr(0u, 1u, SW_XZYW, 0u), sr(2u, 8u, SW_X, 0u), 0u, 2u);
        put_alu(code, &n, 0x02u, dr(0u, 1u, 0x7u, 0u), sr(0u, 1u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 0u, 2u);
        put_alu(code, &n, 0x01u, dr(0u, 1u, 0x8u, 0u), sr(2u, 8u, SW_Y, 0u), 0u, 0u, 1u);
    } else {
        put_alu(code, &n, 0x04u, dr(0u, 1u, 0x7u, 0u), sr(0u, 1u, SW_XZYW, 0u), sr(2u, 8u, SW_X, 0u), sr(2u, 8u, SW_Y, 0u), 3u);
        put_alu(code, &n, 0x01u, dr(0u, 1u, 0x8u, 0u), sr(2u, 8u, SW_Z, 0u), 0u, 0u, 1u);
    }
    put_alu(code, &n, 0x09u, dr(0u, 2u, 0x1u, 0u), sr(2u, 0u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x09u, dr(0u, 2u, 0x2u, 0u), sr(2u, 1u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x09u, dr(0u, 2u, 0x4u, 0u), sr(2u, 2u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x08u, dr(0u, 0u, 0x8u, 1u), sr(0u, 1u, SW_ID, 0u), sr(2u, 3u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x02u, dr(0u, 1u, 0x7u, 0u), sr(0u, 2u, SW_ID, 0u), sr(2u, 6u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x04u, dr(0u, 1u, 0x7u, 0u), sr(2u, 4u, SW_ID, 0u), sr(0u, 0u, SW_W, 0u), sr(0u, 1u, SW_ID, 0u), 3u);
    put_alu(code, &n, 0x05u, dr(0u, 0u, 0x7u, 0u), sr(0u, 0u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x05u, dr(0u, 1u, 0x7u, 0u), sr(0u, 0u, SW_ID, 0u), sr(1u, 1u, SW_ID, 0u), 0u, 2u);
    put_alu(code, &n, 0x01u, dr(0u, 2u, 0x7u, 0u), sr(1u, 1u, SW_ID, 0u), 0u, 0u, 1u);
    put_alu(code, &n, 0x04u, dr(0u, 0u, 0x7u, 0u), sr(0u, 0u, SW_ID, 0u), sr(0u, 2u, SW_ID, 1u), sr(1u, 2u, SW_ID, 0u), 3u);
    put_alu(code, &n, 0x04u, dr(0u, 0u, 0x7u, 0u), sr(1u, 2u, SW_W, 0u), sr(0u, 0u, SW_ID, 0u), sr(0u, 1u, SW_ID, 0u), 3u);
    put_alu(code, &n, 0x05u, dr(8u, 0u, 0x7u, 2u), sr(0u, 0u, SW_ID, 0u), sr(2u, 7u, SW_W, 0u), 0u, 2u);
    code[n++] = END_TOKEN;
    return n;
}

static D9PipelineRequest fx_request(void) {
    D9PipelineRequest request;
    memset(&request, 0, sizeof request);
    request.source = D9_PIPELINE_SOURCE_FXP_PS3;
    request.quality = D9_PIPELINE_QUALITY_LDR;
    request.package_path = "synthetic.fxp";
    request.meta = 0;
    return request;
}

static void test_registry(void) {
    const D9PipelineRegistry *registry = d9_pipeline_registry();
    char why[256];
    check(d9_pipeline_registry_validate(registry, why, sizeof why) == 0, why);
    check(registry->family_count == 8, "registry has eight required families");
    check(registry->descriptor_count == 13, "registry has thirteen initial descriptors");
    check(d9_pipeline_find("ui.copy") != NULL, "find ui.copy");
    check(d9_pipeline_find("does.not.exist") == NULL, "unknown id is not invented");
    for (size_t i = 0; i < registry->family_count; ++i) {
        unsigned seen = 0;
        for (size_t j = 0; j < registry->descriptor_count; ++j)
            if (registry->descriptors[j].family == (D9PipelineFamily)i) ++seen;
        check(seen != 0, "every family has a descriptor");
    }
    const D9PipelineDescriptor *planned = d9_pipeline_find("sky.atmosphere");
    check(planned && planned->support == D9_PIPELINE_SUPPORT_PLANNED,
          "sky remains explicitly planned");
    check(planned && planned->pass_count == 2, "sky documents optional cloud pass");

    D9PipelineRegistry bad = *registry;
    bad.family_count = 7;
    check(d9_pipeline_registry_validate(&bad, why, sizeof why) < 0,
          "validation rejects bad family count");
    D9PipelineDescriptor broken = registry->descriptors[0];
    broken.passes[0].stages = 7;
    D9PipelineDescriptor *one = &broken;
    D9PipelineFamilyInfo families[8];
    memcpy(families, registry->families, sizeof families);
    for (unsigned i = 0; i < 8; ++i) families[i].default_fallback = "";
    bad = *registry;
    bad.families = families;
    bad.descriptors = one;
    bad.descriptor_count = 1;
    check(d9_pipeline_registry_validate(&bad, why, sizeof why) < 0,
          "validation rejects pass above six TEV stages");
}

static void test_parser_rejections(void) {
    D9PipelineShaderView view;
    char why[256];
    uint32_t bad[] = {VER_PS30, DI(0x00u, 0u)};
    check(d9_pipeline_parse_ps3(bad, 2, &view, why, sizeof why) < 0,
          "parser rejects missing END");
    check(strstr(why, "END") != NULL, "missing END is named");
    uint32_t trailing[] = {VER_PS30, END_TOKEN, 0u};
    check(d9_pipeline_parse_ps3(trailing, 3, &view, why, sizeof why) < 0,
          "parser rejects words after END");
    uint32_t truncated[] = {VER_PS30, 0xFFFEu | (8u << 16), 0u};
    check(d9_pipeline_parse_ps3(truncated, 3, &view, why, sizeof why) < 0,
          "parser rejects truncated CTAB/comment");
    uint32_t vertex[] = {0xFFFE0300u, END_TOKEN};
    check(d9_pipeline_parse_ps3(vertex, 2, &view, why, sizeof why) < 0,
          "parser accepts ps_3_0 only");

    uint32_t copy[64];
    size_t words = assemble_copy(copy, NULL);
    for (size_t length = 0; length < words; ++length)
        check(d9_pipeline_parse_ps3(copy, length, &view, why, sizeof why) < 0,
              "parser rejects every truncated copy prefix");
    uint32_t malformed_operand[64];
    memcpy(malformed_operand, copy, words * sizeof *copy);
    malformed_operand[9] &= ~0x80000000u; /* texld coordinate source */
    check(d9_pipeline_parse_ps3(malformed_operand, words, &view, why, sizeof why) < 0,
          "parser rejects register token without high bit");
    memcpy(malformed_operand, copy, words * sizeof *copy);
    malformed_operand[9] |= 1u << 13; /* relative addressing */
    check(d9_pipeline_parse_ps3(malformed_operand, words, &view, why, sizeof why) < 0,
          "parser rejects relative register addressing");
}

static void test_copy(void) {
    uint32_t code[128];
    size_t words = assemble_copy(code, "BaseSampler");
    D9PipelineShaderView view;
    D9PipelineMatch match;
    D9TevProgram plan;
    char why[256];
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    check(view.instructions == 3 && view.textures == 1 && view.constant_count == 1,
          "copy view has real chunk shape");
    D9PipelineRequest request = fx_request();
    const D9PipelineDescriptor *copy = d9_pipeline_find("ui.copy");
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 1, why);
    const D9PipelineDescriptor *selected = d9_pipeline_select(
        D9_PIPELINE_FAMILY_ANY, &view, &request, &match, why, sizeof why);
    check(selected == copy, "exact copy wins priority over generic unlit");
    check(d9_pipeline_build(selected, &view, &request, &match, &plan, why, sizeof why) == 0, why);
    check(plan.stages == 1 && plan.textures == 1, "copy emits one-stage/one-texture plan");
    const float tex[3][4] = {{0.2f, 0.4f, 0.6f, 0.8f}, {0}, {0}};
    float constants[6][4] = {{0}};
    float primary[4] = {0};
    float out[4];
    check(d9_tev_eval_pixel(&plan, tex, constants, primary, out) == 0,
          "copy plan evaluates");
    for (unsigned i = 0; i < 4; ++i)
        check(fabsf(out[i] - tex[0][i]) < 1e-6f, "copy plan passes texel through");

    uint32_t mutated[128];
    words = assemble_copy(mutated, NULL);
    mutated[3] = dr(1u, 1u, 0x3u, 0u); /* texcoord declaration register */
    check(d9_pipeline_parse_ps3(mutated, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_build(copy, &view, &request, &match, &plan, why, sizeof why) < 0,
          "build rejects a stale match against changed tokens");
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 0,
          "copy rejects changed texcoord register");

    words = assemble_copy(mutated, NULL);
    mutated[5] = 0x98000000u; /* cube sampler declaration */
    check(d9_pipeline_parse_ps3(mutated, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 0,
          "copy rejects cube sampler");

    words = assemble_copy(mutated, NULL);
    mutated[8] = dr(8u, 0u, 0x7u, 0u); /* oC0.rgb only */
    check(d9_pipeline_parse_ps3(mutated, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 0,
          "copy rejects changed output mask");

    words = assemble_copy(code, "DirLightColor");
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 0,
          "copy rejects lighting CTAB vocabulary");

    words = assemble_copy(code, NULL);
    request.quality = 0;
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(copy, &view, &request, &match, why, sizeof why) == 0,
          "copy requires explicit LDR contract");
}

static void test_simple_unlit(void) {
    uint32_t code[128];
    size_t words = assemble_tint(code);
    D9PipelineShaderView view;
    D9PipelineMatch match;
    D9TevProgram plan;
    char why[256];
    D9PipelineRequest request = fx_request();
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    const D9PipelineDescriptor *simple = d9_pipeline_find("ui.simple_fxp");
    check(d9_pipeline_try(simple, &view, &request, &match, why, sizeof why) == 1, why);
    check(strstr(match.detail, "1 stages") != NULL, "tint matcher reports one stage");
    check(d9_pipeline_build(simple, &view, &request, &match, &plan, why, sizeof why) == 0, why);
    check(plan.stages == 1 && plan.textures == 1, "tint plan shape");

    code[words - 5u] = DI(0x06u, 2u); /* mul -> rcp */
    memmove(code + words - 2u, code + words - 1u, sizeof *code);
    --words;
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(simple, &view, &request, &match, why, sizeof why) == 0,
          "simple unlit rejects rcp");
}

static void test_glyph(void) {
    const D9PipelineDescriptor *glyph = d9_pipeline_find("ui.scaleform_glyph");
    D9PipelineRequest request;
    memset(&request, 0, sizeof request);
    request.source = D9_PIPELINE_SOURCE_FIXED_SCALEFORM;
    request.material_kind = D9_PIPELINE_MATERIAL_SCALEFORM_GLYPH_ATLAS;
    request.quality = D9_PIPELINE_QUALITY_LDR;
    D9PipelineMatch match;
    D9TevProgram plan;
    char why[256];
    check(d9_pipeline_try(glyph, NULL, &request, &match, why, sizeof why) == 1, why);
    const D9PipelineDescriptor *selected = d9_pipeline_select(
        D9_PIPELINE_INTERFACE_TEXT, NULL, &request, &match, why, sizeof why);
    check(selected == glyph, "glyph material selects fixed text pipeline");
    check(d9_pipeline_build(glyph, NULL, &request, &match, &plan, why, sizeof why) == 0, why);
    check(plan.stages == 1 && plan.textures == 1 && plan.inputs == 1,
          "glyph plan is one texture/one varying/one stage");
    const float tex[3][4] = {{0.8f, 0.6f, 0.4f, 0.5f}, {0}, {0}};
    float constants[6][4] = {{0}};
    float tint[4] = {0.5f, 0.25f, 1.0f, 0.8f};
    float out[4];
    check(d9_tev_eval_pixel(&plan, tex, constants, tint, out) == 0, "glyph plan evaluates");
    for (unsigned i = 0; i < 4; ++i)
        check(fabsf(out[i] - tex[0][i] * tint[i]) < 1e-6f,
              "glyph plan modulates texture by vertex tint");
    request.material_kind = D9_PIPELINE_MATERIAL_NONE;
    check(d9_pipeline_try(glyph, NULL, &request, &match, why, sizeof why) == 0,
          "glyph rejects unrelated fixed material");
}

static void test_static_geometry(void) {
    uint32_t code[256];
    char why[256];
    D9PipelineRequest request = fx_request();
    request.preconditions = D9_PIPELINE_PRE_NORMAL_PREBAKED |
                            D9_PIPELINE_PRE_TANGENT_BASIS_PREBAKED |
                            D9_PIPELINE_PRE_AMBIENT_EMISSIVE_VARYING |
                            D9_PIPELINE_PRE_MATERIAL_ALPHA_C4;
    for (int variant = 0; variant < 2; ++variant) {
        size_t words = assemble_static(code, variant, base_lit_names, 8);
        D9PipelineShaderView view;
        D9PipelineMatch match;
        D9TevProgram plan;
        check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
        check(view.instructions == (unsigned)(variant ? 25 : 24) &&
              view.textures == 2 && view.constant_count == 8,
              "static view has measured instruction/texture/CTAB shape");
        const char *id = variant ? "static.p1200005_add_normal" : "static.p5_mad_normal";
        const D9PipelineDescriptor *descriptor = d9_pipeline_find(id);
        check(d9_pipeline_try(descriptor, &view, &request, &match, why, sizeof why) == 1, why);
        check(match.signature == (variant ? D9_MENU_LIGHT_P1200005_ADD_NORMAL
                                          : D9_MENU_LIGHT_P5_MAD_NORMAL),
              "static matcher identifies exact normal-decode variant");
        check(d9_pipeline_build(descriptor, &view, &request, &match, &plan, why, sizeof why) == 0, why);
        check(plan.stages == 5 && plan.textures == 2, "static plan is five stages/two textures");
        const D9TevStage *final = &plan.stage[4];
        check(final->func_rgb == D9TEV_FUNC_MODULATE && final->func_a == D9TEV_FUNC_REPLACE,
              "static final stage modulates RGB and passes alpha through");
        check(final->src_rgb[0] == D9TEV_SRC_PREV && final->op_rgb[0] == D9TEV_RGB_COLOR,
              "static final stage starts from previous RGB");
        check(final->src_rgb[1] == D9TEV_SRC_CONST &&
              final->op_rgb[1] == D9TEV_RGB_ALPHA && final->const_reg == 7,
              "static final stage consumes only c7.a as the RGB fade");
        check(final->src_a[0] == D9TEV_SRC_PREV && final->op_a[0] == D9TEV_A_ALPHA,
              "static final stage alpha source is previous alpha");

        D9MenuLightInputs inputs = {
            .diffuse = {0.4f, 0.5f, 0.6f, 0.7f},
            .normal_raw = {0.2f, 0.3f, 0.4f, 0.0f},
            .vertex_color = {0.8f, 0.7f, 0.6f, 0.5f},
            .secondary_color = {0.1f, 0.2f, 0.3f, 0.4f},
            .c0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .c1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .c2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .c3 = {0.2f, 0.3f, 0.4f, 0.0f},
            .c4 = {0.6f, 0.5f, 0.4f, 0.8f},
            .c7 = {0.9f, 0.8f, 0.7f, 0.5f}
        };
        D9MenuLightResult first, second, dark;
        check(d9_menu_light_eval_plan(&plan, &inputs, &first) == 0,
              "static fade plan evaluates");
        inputs.c7[0] = 0.1f; inputs.c7[1] = 0.2f; inputs.c7[2] = 0.3f;
        check(d9_menu_light_eval_plan(&plan, &inputs, &second) == 0,
              "static fade plan evaluates with changed c7.rgb");
        check(memcmp(first.rgba, second.rgba, sizeof first.rgba) == 0,
              "static final result ignores c7.rgb");
        inputs.c7[3] = 0.0f;
        check(d9_menu_light_eval_plan(&plan, &inputs, &dark) == 0,
              "static zero-fade plan evaluates");
        check(dark.rgba[0] == 0.0f && dark.rgba[1] == 0.0f && dark.rgba[2] == 0.0f &&
              dark.rgba[3] == first.rgba[3],
              "c7.a fades RGB without changing alpha");

        request.preconditions = 0;
        check(d9_pipeline_build(descriptor, &view, &request, &match, &plan, why, sizeof why) == -2,
              "static plan is gated by preprocessing");
        check(strstr(why, "normal_prebaked") && strstr(why, "material_alpha_c4"),
              "missing static preconditions are named");
        request.preconditions = D9_PIPELINE_PRE_NORMAL_PREBAKED |
                                D9_PIPELINE_PRE_TANGENT_BASIS_PREBAKED |
                                D9_PIPELINE_PRE_AMBIENT_EMISSIVE_VARYING |
                                D9_PIPELINE_PRE_MATERIAL_ALPHA_C4;

        uint32_t mutated[256];
        memcpy(mutated, code, words * sizeof *code);
        size_t def_at = find_opcode(mutated, words, 0x51u);
        check(def_at < words, "static fixture contains def c8");
        mutated[def_at + 2u] = fbits(1.5f); /* first def c8 literal */
        check(d9_pipeline_parse_ps3(mutated, words, &view, why, sizeof why) == 0, why);
        check(d9_pipeline_try(descriptor, &view, &request, &match, why, sizeof why) == 0,
              "static matcher rejects changed normal-decode literal");
    }

    const char *missing_names[] = {
        "DiffuseSampler", "NormalSampler", "DirLightColor", "DirLightDirection",
        "DirectionalAmbient", "EmitColor", "FogColor"
    };
    size_t words = assemble_static(code, 0, missing_names, 7);
    D9PipelineShaderView view;
    D9PipelineMatch match;
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    check(d9_pipeline_try(d9_pipeline_find("static.p5_mad_normal"), &view,
                          &request, &match, why, sizeof why) == 0,
          "static matcher rejects incomplete base-lit CTAB");

    const char *extended_names[][9] = {
        {"DiffuseSampler", "NormalSampler", "DirLightColor", "DirLightDirection",
         "DirectionalAmbient", "EmitColor", "FogColor", "MaterialData", "ShadowMaskSampler"}
    };
    words = assemble_static(code, 0, extended_names[0], 9);
    check(d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0, why);
    int shadow_match = d9_pipeline_try(d9_pipeline_find("static.p5_mad_normal"), &view,
                                      &request, &match, why, sizeof why);
    check(shadow_match == 0, "static matcher rejects shadow permutation");
    if (shadow_match != 0) printf("  shadow result=%d reason=%s\n", shadow_match, why);
}

static void test_planned_and_fallback(void) {
    const D9PipelineDescriptor *sky = d9_pipeline_find("sky.atmosphere");
    D9PipelineShaderView view;
    memset(&view, 0, sizeof view);
    D9PipelineMatch match;
    D9TevProgram plan;
    char why[256];
    D9PipelineRequest request = fx_request();
    check(d9_pipeline_try(sky, &view, &request, &match, why, sizeof why) == 0,
          "planned descriptor never accepts");
    check(d9_pipeline_build(sky, &view, &request, &match, &plan, why, sizeof why) < 0,
          "planned descriptor emits no plan");
    check(d9_pipeline_select(D9_PIPELINE_SKY, &view, &request, &match,
                             why, sizeof why) == NULL,
          "selection does not auto-fallback or invent sky acceptance");
    check(strstr(why, "fallback is explicit") != NULL,
          "selection explains explicit fallback policy");
    const D9PipelineDescriptor *fallback = d9_pipeline_find("static.diffuse_fallback");
    check(fallback && fallback->support == D9_PIPELINE_SUPPORT_PLANNED,
          "static diffuse fallback is registered but not implemented");
}

int main(void) {
    test_registry();
    test_parser_rejections();
    test_copy();
    test_simple_unlit();
    test_glyph();
    test_static_geometry();
    test_planned_and_fallback();
    printf("pipeline registry: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
