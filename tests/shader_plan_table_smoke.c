/* Host test: exact-hash native plan table, named refusals, unchanged caps.
 *
 * The table is generated offline by tools/fxp_compile.py from
 * docs/shader_plans.json.  This test does not need retail bytecode: it checks
 * the descriptors and the draw gate only.
 */

#include "d3d9_shader_plan.h"
#include "d3d9_adapter.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LOGO_SHA "8725393bf3ed950789e50501893ab141efdfd0377309271f6ef7493239be5ff9"
#define COPY_SHA "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de"
#define GENERATED_VS_SHA "0ccc7542aa06b2e20a41283acc2d5473602fc03f5495cc95166ec3b01820d2f7"
#define FOREIGN_SHA "0000000000000000000000000000000000000000000000000000000000000000"

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define DR(type_, reg, mask, mod) (0x80000000u | (((type_) & 7u) << 28) | \
    (((type_) & 0x18u) << 8) | ((reg) & 0x7FFu) | (((mask) & 0xFu) << 16) | \
    (((mod) & 0xFu) << 20))
#define SR(type_, reg, sw, mod) (0x80000000u | (((type_) & 7u) << 28) | \
    (((type_) & 0x18u) << 8) | ((reg) & 0x7FFu) | (((sw) & 0xFFu) << 16) | \
    (((mod) & 0xFu) << 24))
#define SW_ID 0xE4u

static int checks, failures;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { \
    ++failures; printf("FAIL: %s (%s:%d)\n", text, __FILE__, __LINE__); \
} } while (0)

static const D9ShaderPlan *lookup(unsigned stage, const char *sha, int expect) {
    const D9ShaderPlan *plan = NULL;
    char why[256] = "";
    int rc = d9_shader_plan_lookup(stage, sha, &plan, why, sizeof why);
    if (rc != expect) {
        ++failures;
        printf("FAIL: lookup(%u, %.12s) rc=%d expected %d why=%s\n",
               stage, sha, rc, expect, why);
    }
    ++checks;
    return plan;
}

int main(void) {
    CHECK(d9_shader_plan_count() >= 4, "table has at least four entries");
    CHECK(d9_shader_plan_at(d9_shader_plan_count()) == NULL,
          "out-of-range index returns NULL");

    /* Copy: the certified one-stage TEV plan. */
    const D9ShaderPlan *copy = lookup(D9_SHADER_PLAN_STAGE_PS, COPY_SHA, 1);
    CHECK(copy != NULL && strcmp(copy->id, "menu.composite.ps") == 0,
          "copy entry id");
    CHECK(copy && copy->kind == D9_SHADER_PLAN_TEV, "copy is TEV");
    CHECK(copy && copy->draw_enabled == 1, "copy is draw-enabled");
    CHECK(copy && copy->tev.stages == 1, "copy has one stage");
    CHECK(copy && copy->tev.stage_limit == D9TEV_MAX_STAGES,
          "copy keeps the PICA stage limit");
    CHECK(copy && copy->tev.backend_flags == 0 &&
          copy->tev.requires_alpha_test == 0 && copy->tev.texkill_count == 0,
          "copy optional flags stay zero");
    CHECK(copy && copy->tev.stage[0].func_rgb == D9TEV_FUNC_REPLACE,
          "copy stage 0 replaces");
    CHECK(copy && copy->tev.stage[0].src_rgb[0] == D9TEV_SRC_TEX0,
          "copy stage 0 reads TEXTURE0");
    CHECK(copy && copy->tev.textures == 1 && copy->tev.texture[0].sampler_reg == 0,
          "copy uses sampler s0");
    char why[256] = "";
    CHECK(d9_shader_plan_draw_allowed(copy, why, sizeof why) == 0,
          "copy draw gate allows");

    /* Upper-case hash must resolve to the same entry. */
    const char *upper =
        "6022044EF19E4B1C241482207ACCA5A6BE484081E629B6D72CDEFDD9568A45DE";
    const D9ShaderPlan *copy2 = lookup(D9_SHADER_PLAN_STAGE_PS, upper, 1);
    CHECK(copy2 == copy, "upper-case SHA maps to the same entry");

    /* Logo: five-stage plan, draw refused by material preconditions. */
    const D9ShaderPlan *logo = lookup(D9_SHADER_PLAN_STAGE_PS, LOGO_SHA, 1);
    CHECK(logo != NULL && strcmp(logo->id, "menu.logo.ps") == 0, "logo entry id");
    CHECK(logo && logo->tev.stages == 5, "logo has five stages");
    CHECK(logo && logo->tev.stage[0].func_rgb == D9TEV_FUNC_DOT3_RGBA,
          "logo stage 0 is dot3");
    CHECK(logo && logo->tev.stage[0].src_rgb[0] == D9TEV_SRC_TEX1,
          "logo stage 0 reads TEXTURE1");
    CHECK(logo && logo->tev.stage[0].const_reg == 3, "logo stage 0 uses c3");
    CHECK(logo && logo->tev.textures == 2, "logo uses two textures");
    CHECK(logo && logo->tev.constants == 3 && logo->tev.constant_reg[2] == 7,
          "logo constant list c3/c4/c7");
    CHECK(logo && logo->draw_enabled == 0, "logo draw is disabled");
    why[0] = 0;
    CHECK(logo && d9_shader_plan_draw_allowed(logo, why, sizeof why) == -1,
          "logo draw gate refuses");
    CHECK(strstr(why, "not draw-enabled") != NULL, "logo refusal is named");
    CHECK(strstr(why, "normal/tangent") != NULL,
          "logo refusal names the material precondition");

    /* A PS hash looked up as a VS is a miss, not a silent success. */
    const D9ShaderPlan *wrong = NULL;
    why[0] = 0;
    CHECK(d9_shader_plan_lookup(D9_SHADER_PLAN_STAGE_VS, LOGO_SHA, &wrong,
                                why, sizeof why) == 0, "logo is not a VS plan");
    CHECK(wrong == NULL, "wrong-stage lookup leaves the output NULL");
    CHECK(strstr(why, "no compiled vertex plan") != NULL,
          "wrong-stage refusal is named");

    /* Foreign hash. */
    why[0] = 0;
    CHECK(d9_shader_plan_lookup(D9_SHADER_PLAN_STAGE_PS, FOREIGN_SHA, &wrong,
                                why, sizeof why) == 0, "foreign hash is refused");
    CHECK(wrong == NULL, "foreign hash leaves the output NULL");
    CHECK(strstr(why, "no compiled pixel plan") != NULL,
          "foreign refusal is named");

    /* Malformed argument. */
    why[0] = 0;
    CHECK(d9_shader_plan_lookup(D9_SHADER_PLAN_STAGE_PS, "abc", &wrong,
                                why, sizeof why) == -1, "short hash is invalid");
    CHECK(d9_shader_plan_lookup(D9_SHADER_PLAN_STAGE_PS, NULL, &wrong,
                                why, sizeof why) == -1, "NULL hash is invalid");

    /* The pilot game PS is compiled but not certified: named refusal. */
    const D9ShaderPlan *game_ps = NULL;
    for (unsigned i = 0; i < d9_shader_plan_count(); ++i) {
        const D9ShaderPlan *plan = d9_shader_plan_at(i);
        if (plan->stage == D9_SHADER_PLAN_STAGE_PS &&
            strncmp(plan->id, "game.ps.", 8) == 0)
            game_ps = plan;
    }
    CHECK(game_ps != NULL, "pilot game PS is present");
    CHECK(game_ps && game_ps->tev.stages >= 2, "pilot game PS has >= 2 stages");
    why[0] = 0;
    CHECK(game_ps && d9_shader_plan_draw_allowed(game_ps, why, sizeof why) == -1,
          "pilot game PS draw is refused");
    CHECK(strstr(why, "not draw-enabled") != NULL,
          "pilot game PS refusal is named");

    /* The pilot game VS is a descriptor, not an executable plan. */
    const D9ShaderPlan *game_vs = NULL;
    for (unsigned i = 0; i < d9_shader_plan_count(); ++i) {
        const D9ShaderPlan *plan = d9_shader_plan_at(i);
        if (plan->stage == D9_SHADER_PLAN_STAGE_VS)
            game_vs = plan;
    }
    CHECK(game_vs != NULL && game_vs->kind == D9_SHADER_PLAN_PICA_VS,
          "pilot game VS is a PICA VS descriptor");
    why[0] = 0;
    CHECK(game_vs && d9_shader_plan_draw_allowed(game_vs, why, sizeof why) == -1,
          "PICA VS descriptor draw is refused");
    CHECK(strstr(why, "code emitter") != NULL || strstr(why, "not draw-enabled") != NULL,
          "PICA VS refusal is named");

    /* The table's draw-enabled copy entry must be the same descriptor the
     * existing C translator builds from the same three-instruction stream. */
    {
        uint32_t code[16];
        size_t n = 0;
        code[n++] = 0xFFFF0300u;
        code[n++] = DI(0x1F, 2);
        code[n++] = 5u;                       /* D3DDECLUSAGE_TEXCOORD, index 0 */
        code[n++] = DR(1, 0, 0xF, 0);         /* v0 */
        code[n++] = DI(0x1F, 2);
        code[n++] = 2u << 27;                 /* texture type 2D */
        code[n++] = DR(10, 0, 0xF, 0);        /* s0 */
        code[n++] = DI(0x42, 3);
        code[n++] = DR(8, 0, 0xF, 0);         /* oC0 */
        code[n++] = SR(1, 0, SW_ID, 0);       /* v0 */
        code[n++] = SR(10, 0, SW_ID, 0);      /* s0 */
        code[n++] = 0x0000FFFFu;
        D9TevProgram translated;
        char tev_why[256] = "";
        CHECK(d9_tev_translate_pixel(code, n, &translated, tev_why,
                                     sizeof tev_why) == 0,
              "C translator accepts the synthetic copy");
        CHECK(copy && translated.stages == copy->tev.stages,
              "copy entry stage count equals the C plan");
        if (copy && translated.stages == copy->tev.stages) {
            int same = 1;
            for (unsigned s = 0; s < translated.stages; ++s) {
                const D9TevStage *a = &translated.stage[s];
                const D9TevStage *b = &copy->tev.stage[s];
                unsigned rgb_arity = a->func_rgb == D9TEV_FUNC_REPLACE ? 1u : 2u;
                if (a->func_rgb == D9TEV_FUNC_MULTIPLY_ADD ||
                    a->func_rgb == D9TEV_FUNC_INTERPOLATE ||
                    a->func_rgb == D9TEV_FUNC_ADD_MULTIPLY)
                    rgb_arity = 3u;
                unsigned a_arity = a->func_a == D9TEV_FUNC_REPLACE ? 1u : 2u;
                if (a->func_a == D9TEV_FUNC_MULTIPLY_ADD ||
                    a->func_a == D9TEV_FUNC_INTERPOLATE ||
                    a->func_a == D9TEV_FUNC_ADD_MULTIPLY)
                    a_arity = 3u;
                if (a->func_a == D9TEV_FUNC_DOT3_RGBA)
                    a_arity = 0;
                same &= a->func_rgb == b->func_rgb && a->func_a == b->func_a;
                for (unsigned i = 0; i < rgb_arity; ++i) {
                    same &= a->src_rgb[i] == b->src_rgb[i];
                    same &= a->op_rgb[i] == b->op_rgb[i];
                }
                for (unsigned i = 0; i < a_arity; ++i) {
                    same &= a->src_a[i] == b->src_a[i];
                    same &= a->op_a[i] == b->op_a[i];
                }
                same &= a->scale_rgb == b->scale_rgb && a->scale_a == b->scale_a;
                same &= a->const_reg == b->const_reg;
                same &= a->const_is_literal == b->const_is_literal;
                same &= a->const_rgba8 == b->const_rgba8;
            }
            CHECK(same, "copy entry used stage fields equal the C plan");
        }
        CHECK(copy && translated.textures == copy->tev.textures,
              "copy entry texture count equals the C plan");
        CHECK(copy && translated.uses_vertex_color == copy->tev.uses_vertex_color,
              "copy entry vertex-colour flag equals the C plan");
        CHECK(copy && translated.requires_alpha_fixup == copy->tev.requires_alpha_fixup,
              "copy entry alpha-fixup flag equals the C plan");
    }

    /* Generated VS assembly table: exact hash only, append-only, no draw claim. */
    {
        const char *id = NULL, *asm_name = NULL;
        CHECK(d9_generated_vs_count() == 3, "three emitted VS assemblies");
        CHECK(d9_generated_vs_lookup(GENERATED_VS_SHA, &id, &asm_name) == 1,
              "pilot hash selects an emitted VS");
        CHECK(id && strcmp(id, "game.vs.0ccc7542") == 0, "generated VS id");
        CHECK(asm_name && strcmp(asm_name, "game.vs.0ccc7542.v.pica") == 0,
              "generated VS asm name");
        const char *upper_id = NULL;
        const char *upper_sha =
            "0CCC7542AA06B2E20A41283ACC2D5473602FC03F5495CC95166EC3B01820D2F7";
        CHECK(d9_generated_vs_lookup(upper_sha, &upper_id, NULL) == 1 &&
              upper_id == id, "generated VS lookup accepts upper case");
        id = (const char *)1;
        CHECK(d9_generated_vs_lookup(FOREIGN_SHA, &id, NULL) == 0,
              "foreign hash is not generated");
        CHECK(id == NULL, "miss clears the id");
        CHECK(d9_generated_vs_lookup("abc", &id, NULL) == -1,
              "short hash is invalid");
        CHECK(d9_generated_vs_lookup(NULL, &id, NULL) == -1,
              "NULL hash is invalid");
        for (unsigned i = 0; i < d9_generated_vs_count(); ++i) {
            const char *entry_id = d9_generated_vs_id_at(i);
            CHECK(entry_id && strncmp(entry_id, "game.vs.", 8) == 0,
                  "generated VS ids are game.vs.*");
        }
        CHECK(d9_generated_vs_id_at(d9_generated_vs_count()) == NULL,
              "generated VS out-of-range index returns NULL");
    }

    /* Per-family packs partition the whole census, append-only. */
    {
        unsigned chunks = 0, hashes = 0, draws = 0;
        CHECK(d9_shader_pack_count() == 6, "six shader packs");
        for (unsigned i = 0; i < d9_shader_pack_count(); ++i) {
            const D9ShaderPackRow *row = d9_shader_pack_at(i);
            CHECK(row && row->pack && row->owner, "pack row is named");
            CHECK(row && row->translated + row->draw_enabled + row->refused
                  == row->unique_hashes, "pack statuses cover its hashes");
            if (row) {
                chunks += row->chunks;
                hashes += row->unique_hashes;
                draws += row->draw_enabled;
            }
        }
        CHECK(chunks == 10475u, "packs cover all 10,475 chunks");
        CHECK(hashes == 6659u, "packs cover all 6,659 hashes");
        CHECK(draws == 1u, "exactly one draw-enabled plan across the packs");
        CHECK(d9_shader_pack_at(6) == NULL, "pack out-of-range returns NULL");
    }

    /* Derived constants: c0.zwzw builds a register the guest never uploads. */
    {
        D9ShaderPlan plan;
        memset(&plan, 0, sizeof plan);
        plan.id = "unit.derived";
        plan.sha256 = FOREIGN_SHA;
        plan.kind = D9_SHADER_PLAN_TEV;
        plan.draw_enabled = 1;
        plan.tev.stages = 1;
        plan.tev.textures = 0;
        plan.tev.constants = 1;
        plan.tev.constant_reg[0] = 0;
        plan.const_op_count = 1;
        plan.const_op[0].dst_reg = 7;
        plan.const_op[0].op = D9_SHADER_PLAN_CONST_PACK;
        plan.const_op[0].src_count = 1;
        plan.const_op[0].src[0].reg = 0;
        plan.const_op[0].src[0].swizzle[0] = 2; /* z */
        plan.const_op[0].src[0].swizzle[1] = 3; /* w */
        plan.const_op[0].src[0].swizzle[2] = 2;
        plan.const_op[0].src[0].swizzle[3] = 3;
        float bank[D9_SHADER_PLAN_CONST_REGS][4] = {{0}};
        float out[D9_SHADER_PLAN_CONST_REGS][4] = {{0}};
        bank[0][0] = 0.25f; bank[0][1] = 0.5f;
        bank[0][2] = 0.75f; bank[0][3] = 1.0f;
        uint32_t mask = 0;
        why[0] = 0;
        CHECK(d9_shader_plan_build_constants(&plan, bank, 1u, out, &mask,
                                             why, sizeof why) == 0,
              "c0.zwzw derives");
        CHECK(mask == (1u << 7), "derived c7 is marked");
        CHECK(out[7][0] == 0.75f && out[7][1] == 1.0f &&
              out[7][2] == 0.75f && out[7][3] == 1.0f,
              "c0.zwzw -> (z,w,z,w) numerically");
        CHECK(d9_shader_plan_draw_allowed(&plan, why, sizeof why) == 0,
              "derived plan passes the structural gate");
        mask = 0xFFFFFFFFu;
        why[0] = 0;
        CHECK(d9_shader_plan_build_constants(&plan, bank, 0u, out, &mask,
                                             why, sizeof why) == -1,
              "missing source is refused");
        CHECK(mask == 0 && strstr(why, "derived c7 needs source c0.zwzw"),
              "missing-source refusal names c0.zwzw");

        /* add/mul/lerp componentwise, including a negated source. */
        bank[1][0] = 1.5f; bank[1][1] = -0.5f;
        bank[1][2] = 2.0f; bank[1][3] = 0.0f;
        memset(out, 0, sizeof out);
        plan.const_op_count = 3;
        plan.const_op[0].dst_reg = 4;
        plan.const_op[0].op = D9_SHADER_PLAN_CONST_ADD;
        plan.const_op[0].src_count = 2;
        plan.const_op[0].src[0].reg = 0;
        plan.const_op[0].src[0].swizzle[0] = 0;
        plan.const_op[0].src[0].swizzle[1] = 1;
        plan.const_op[0].src[0].swizzle[2] = 2;
        plan.const_op[0].src[0].swizzle[3] = 3;
        plan.const_op[0].src[1].reg = 1;
        plan.const_op[0].src[1].swizzle[0] = 0;
        plan.const_op[0].src[1].swizzle[1] = 0;
        plan.const_op[0].src[1].swizzle[2] = 0;
        plan.const_op[0].src[1].swizzle[3] = 0;
        plan.const_op[1].dst_reg = 5;
        plan.const_op[1].op = D9_SHADER_PLAN_CONST_MUL;
        plan.const_op[1].src_count = 2;
        plan.const_op[1].src[0] = plan.const_op[0].src[0];
        plan.const_op[1].src[1] = plan.const_op[0].src[1];
        plan.const_op[1].src[1].negate = 1;
        plan.const_op[2].dst_reg = 6;
        plan.const_op[2].op = D9_SHADER_PLAN_CONST_LERP;
        plan.const_op[2].src_count = 3;
        plan.const_op[2].src[0] = plan.const_op[0].src[0];
        plan.const_op[2].src[1] = plan.const_op[0].src[0];
        plan.const_op[2].src[1].negate = 1;
        plan.const_op[2].src[2] = plan.const_op[0].src[1];
        CHECK(d9_shader_plan_build_constants(&plan, bank, 3u, out, &mask,
                                             why, sizeof why) == 0,
              "add/mul/lerp derive");
        CHECK(out[4][0] == 1.75f && out[5][0] == -0.375f &&
              out[6][0] == 0.25f * (1.0f - 1.5f) + (-0.25f) * 1.5f,
              "add/mul/lerp numeric results");
        CHECK(d9_shader_plan_draw_allowed(&plan, why, sizeof why) == 0,
              "three-op constant plan passes the gate");
    }

    /* Multi-pass: two six-plus-one style passes with per-pass blend/depth. */
    {
        D9ShaderPlan mp;
        memset(&mp, 0, sizeof mp);
        mp.id = "unit.multipass";
        mp.sha256 = FOREIGN_SHA;
        mp.kind = D9_SHADER_PLAN_TEV;
        mp.draw_enabled = 1;
        mp.tev.stages = 1;
        mp.tev.textures = 0;
        mp.pass_count = 2;
        for (unsigned i = 0; i < 2; ++i) {
            mp.pass[i].tev.stages = 1;
            mp.pass[i].tev.textures = 0;
            mp.pass[i].blend = i == 0 ? 0 : 1;
            mp.pass[i].src_blend = 2; /* SRCALPHA */
            mp.pass[i].dst_blend = 1; /* ZERO */
            mp.pass[i].blend_op = 1;  /* ADD */
            mp.pass[i].depth_write = i == 1 ? 1 : 0;
        }
        why[0] = 0;
        CHECK(d9_shader_plan_draw_allowed(&mp, why, sizeof why) == 0,
              "two-pass plan passes the gate");
        mp.pass[1].tev.stages = 7;
        why[0] = 0;
        CHECK(d9_shader_plan_draw_allowed(&mp, why, sizeof why) == -1 &&
              strstr(why, "pass 1 stages 7"),
              "a seven-stage pass is refused by name");
        mp.pass[1].tev.stages = 1;
        mp.pass[1].src_blend = 0;
        why[0] = 0;
        CHECK(d9_shader_plan_draw_allowed(&mp, why, sizeof why) == -1 &&
              strstr(why, "invalid blend/depth"),
              "an invalid blend state is refused by name");
        mp.pass[1].src_blend = 2;
        mp.pass_count = 1;
        why[0] = 0;
        CHECK(d9_shader_plan_draw_allowed(&mp, why, sizeof why) == -1 &&
              strstr(why, "passes outside 2..4"),
              "a one-pass 'multipass' plan is refused by name");
    }

    /* Caps are untouched by plan lookup/selection. */
    D9Caps9 caps;
    memset(&caps, 0xAA, sizeof caps);
    CHECK(d9_adapter_caps(0, 1 /* DEVTYPE_HAL */, &caps) == 0, "caps query");
    CHECK(caps.dev_caps == 0x00082000u, "DevCaps stays 0x00082000");
    CHECK(caps.vertex_shader_version == 0xFFFE0200u, "VS stays 2.0");
    CHECK(caps.pixel_shader_version == 0xFFFF0200u, "PS stays 2.0");
    CHECK(caps.max_vertex_shader_30_instruction_slots == 0, "no SM3 VS slots");
    CHECK(caps.max_pixel_shader_30_instruction_slots == 0, "no SM3 PS slots");
    CHECK(caps.max_vertex_shader_const == 4, "MaxVertexShaderConst stays 4");

    if (failures) {
        printf("shader_plan_table: %d checks, %d failures\n", checks, failures);
        return 1;
    }
    printf("shader_plan_table: %d checks, 0 failures\n", checks);
    return 0;
}
