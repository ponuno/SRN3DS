/* Host test: multi-pass plan replay through one d9_draw_plan_passes call.
 *
 * A seven-stage shader is split into 6+1.  The core must submit the same
 * geometry twice, hand each pass its own TEV program and blend/depth state,
 * never leak a pass state into the next draw, and refuse a malformed pass by
 * name before the first submit.  The final pass output is checked numerically
 * with the software TEV evaluator against an independently written reference.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_shader_plan.h"
#include "d3d9_tev.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned draws;
static const D9TevProgram *seen_program[4];
static unsigned seen_blend[4], seen_src[4], seen_dst[4], seen_op[4], seen_zwrite[4];

static int record_draw(void *ctx, const D9Vertex *vertices, size_t count,
                       const D9Matrix *clip, void *texture,
                       const D9DrawState *state) {
    (void)ctx; (void)clip; (void)texture;
    assert(vertices && count == 3 && state && state->pixel_shader);
    assert(draws < 4);
    seen_program[draws] = state->pixel_shader;
    seen_blend[draws] = state->blend;
    seen_src[draws] = state->src_blend;
    seen_dst[draws] = state->dst_blend;
    seen_op[draws] = state->blend_op;
    seen_zwrite[draws] = state->z_write;
    ++draws;
    return 0;
}

static D9TevProgram stage_program(unsigned func, unsigned src0, unsigned src1,
                                  unsigned op0, unsigned op1) {
    D9TevProgram p;
    memset(&p, 0, sizeof p);
    p.stages = 1;
    p.stage_limit = D9TEV_MAX_STAGES;
    p.stage[0].func_rgb = (uint8_t)func;
    p.stage[0].func_a = (uint8_t)func;
    p.stage[0].src_rgb[0] = (uint8_t)src0;
    p.stage[0].src_rgb[1] = (uint8_t)src1;
    p.stage[0].src_a[0] = (uint8_t)src0;
    p.stage[0].src_a[1] = (uint8_t)src1;
    p.stage[0].op_rgb[0] = (uint8_t)op0;
    p.stage[0].op_rgb[1] = (uint8_t)op1;
    p.stage[0].op_a[0] = (uint8_t)op0;
    p.stage[0].op_a[1] = (uint8_t)op1;
    p.stage[0].const_reg = 255;
    return p;
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.draw = record_draw;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 4096);
    assert(d);

    D9VertexElement elems[2] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, elems, 2));
    assert(!d9_set_vertex_declaration(d, &decl));
    assert(!d9_set_render_state(d, 27, 0)); /* blend off before the draw */
    assert(!d9_set_render_state(d, 19, 2));
    assert(!d9_set_render_state(d, 20, 1));

    D9Vertex v[3] = {
        {0, 0, 0, 0, 0},
        {1, 0, 0, 1, 0},
        {0, 1, 0, 0, 1},
    };

    /* Seven stages: pass 0 has six (in this test one representative stage),
     * pass 1 has the seventh.  pass[0] mirrors the main plan. */
    D9ShaderPlan plan;
    memset(&plan, 0, sizeof plan);
    plan.id = "unit.7stage";
    plan.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    plan.kind = D9_SHADER_PLAN_TEV;
    plan.draw_enabled = 1;
    plan.tev = stage_program(D9TEV_FUNC_REPLACE, D9TEV_SRC_PRIMARY, 0, 0, 0);
    plan.pass_count = 2;
    plan.pass[0].tev = plan.tev;
    plan.pass[0].blend = 0;
    plan.pass[0].depth_write = 0;
    plan.pass[1].tev = stage_program(D9TEV_FUNC_MODULATE, D9TEV_SRC_PRIMARY,
                                     D9TEV_SRC_CONST, 0, 0);
    plan.pass[1].tev.stage[0].const_is_literal = 1;
    plan.pass[1].tev.stage[0].const_rgba8 = 0xFFC08040u;
    plan.pass[1].blend = 1;
    plan.pass[1].src_blend = 2; /* ONE */
    plan.pass[1].dst_blend = 1; /* ZERO */
    plan.pass[1].blend_op = 1;  /* ADD */
    plan.pass[1].depth_write = 1;

    char why[192];
    assert(!d9_shader_plan_draw_allowed(&plan, why, sizeof why));
    assert(!d9_begin_scene(d, 0));

    D9PlanDraw desc = {0};
    desc.type = D9_TRIANGLELIST;
    desc.primitives = 1;
    desc.vertex_data = v;
    desc.vertex_stride = sizeof v[0];
    desc.vertex_count = 3;
    int rc = d9_draw_plan_passes(d, &plan, &desc);
    if (rc) fprintf(stderr, "multipass rc=%d: %s\n", rc,
                    d9_last_draw_diagnostic(d));
    assert(!rc && draws == 2);
    assert(seen_program[0] == &plan.pass[0].tev);
    assert(seen_program[1] == &plan.pass[1].tev);
    assert(seen_blend[0] == 0 && seen_zwrite[0] == 0);
    assert(seen_blend[1] == 1 && seen_src[1] == 2 && seen_dst[1] == 1 &&
           seen_op[1] == 1 && seen_zwrite[1] == 1);

    /* Intermediate state does not leak: the caller's blend state is restored. */
    unsigned blend = 99, zwrite = 99;
    assert(!d9_get_render_state(d, 27, &blend));
    assert(!d9_get_render_state(d, 14, &zwrite));
    assert(blend == 0 && zwrite == 1);

    /* Numeric reference: pass0 = primary, pass1 = prev * c0, blend ONE/ZERO. */
    float primary[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float stage_const[D9TEV_MAX_STAGES][4] = {{0}};
    stage_const[0][0] = 0.5f; stage_const[0][1] = 0.25f;
    stage_const[0][2] = 2.0f; stage_const[0][3] = 1.0f;
    float tex[D9TEV_MAX_TEXTURES][4] = {{0}};
    float out[D9TEV_MAX_STAGES][4];
    float prev[4], final[4];
    assert(!d9_tev_eval_pixel(&plan.pass[0].tev, tex, stage_const, primary,
                              prev));
    assert(!d9_tev_eval_pixel(&plan.pass[1].tev, tex, stage_const, prev,
                              final));
    float reference[4];
    for (unsigned i = 0; i < 4; ++i)
        reference[i] = primary[i] *
            (float)((plan.pass[1].tev.stage[0].const_rgba8 >> (8u * i)) & 0xFFu)
            / 255.0f;
    for (unsigned i = 0; i < 4; ++i) {
        assert(fabsf(prev[i] - primary[i]) < 1e-6f);
        assert(fabsf(final[i] - reference[i]) < 1e-6f);
    }

    /* Any malformed pass is a named refusal and submits nothing. */
    plan.pass[1].tev.stages = 7;
    unsigned before = draws;
    why[0] = 0;
    rc = d9_draw_plan_passes(d, &plan, &desc);
    assert(rc == D9_UNSUPPORTED);
    assert(draws == before);
    assert(strstr(d9_last_draw_diagnostic(d), "pass 1") != NULL);
    plan.pass[1].tev.stages = 1;

    assert(!d9_end_scene(d));
    d9_destroy_device(d);
    puts("shader plan multipass: 2 passes, per-pass blend/depth, numeric "
         "reference, named refusal PASS");
    return 0;
}
