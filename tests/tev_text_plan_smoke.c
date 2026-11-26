/* Certified menu-text TEV plan and its PICA descriptor.
 *
 * The plan is one stage: MODULATE(TEXTURE0, PRIMARY) for colour and alpha,
 * one 2D sampler at texcoord0, vertex COLOR0, no constants and one stage only.
 * Sampler POINT/CLAMP is supplied by the menu-text draw state.  The builder
 * refuses a pixel shader, a second texture or a non-A8 texture unit by name.
 */
#include "d3d9_adapter.h"
#include "d3d9_pica_tev.h"
#include "d3d9_tev.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    D9Caps9 caps_before, caps_after;
    assert(!d9_adapter_caps(0, 1, &caps_before));

    D9TevProgram plan;
    char why[256];
    assert(d9_tev_text_plan(&plan, 1, 0, 0, why, sizeof why) == 0);
    assert(why[0] == 0);
    assert(plan.stages == 1);
    assert(plan.textures == 1 && plan.inputs == 1 && plan.constants == 0);
    assert(plan.uses_vertex_color == 1);
    assert(plan.backend_flags == D9TEV_PLAN_MENU_TEXT);
    assert(plan.texture[0].sampler_reg == 0);
    assert(plan.texture[0].texcoord_reg == 0);
    assert(plan.texture[0].sampler_type == 2);
    assert(plan.input[0].usage == D9TEV_USAGE_COLOR);
    assert(plan.input[0].usage_index == 0 && plan.input[0].reg_index == 1);
    assert(plan.input[0].tev_source == D9TEV_SRC_PRIMARY);
    assert(plan.stage[0].func_rgb == D9TEV_FUNC_MODULATE);
    assert(plan.stage[0].func_a == D9TEV_FUNC_MODULATE);
    assert(plan.stage[0].src_rgb[0] == D9TEV_SRC_TEX0);
    assert(plan.stage[0].src_rgb[1] == D9TEV_SRC_PRIMARY);
    assert(plan.stage[0].src_a[0] == D9TEV_SRC_TEX0);
    assert(plan.stage[0].src_a[1] == D9TEV_SRC_PRIMARY);
    assert(plan.stage[0].const_reg == 0xFFu);
    assert(plan.stage[1].func_rgb == 0 && plan.stage[1].func_a == 0);
    assert(plan.stage[1].src_rgb[0] == 0 && plan.stage[1].src_a[0] == 0);

    D9PicaTevProgram desc;
    D9PicaTevReject reject = D9_PICA_TEV_OK;
    assert(d9_pica_tev_validate(&plan, &desc, &reject, why, sizeof why) == 0);
    assert(reject == D9_PICA_TEV_OK);
    assert(desc.stage_count == 1);
    assert(desc.texture_count == 1 && desc.sampler_reg == 0 &&
           desc.texcoord_reg == 0);
    assert(desc.uses_primary_color == 1);
    assert(desc.uses_ambient_fold == 0);
    assert(desc.constant_count == 0);
    assert(desc.stage[0].func_rgb == D9TEV_FUNC_MODULATE);
    assert(desc.stage[0].func_a == D9TEV_FUNC_MODULATE);
    assert(desc.stage[0].src_rgb[0] == D9TEV_SRC_TEX0 &&
           desc.stage[0].src_rgb[1] == D9TEV_SRC_PRIMARY);
    assert(desc.stage[0].src_a[0] == D9TEV_SRC_TEX0 &&
           desc.stage[0].src_a[1] == D9TEV_SRC_PRIMARY);
    assert(desc.stage[0].uses_constant == 0);

    /* A second texture, a pixel shader and a non-A8 unit are named refusals. */
    D9TevProgram refused;
    memset(why, 0, sizeof why);
    assert(d9_tev_text_plan(&refused, 0, 0, 0, why, sizeof why) == -1);
    assert(strstr(why, "A8") != NULL);
    memset(why, 0, sizeof why);
    assert(d9_tev_text_plan(&refused, 1, 1, 0, why, sizeof why) == -1);
    assert(strstr(why, "one texture") != NULL);
    memset(why, 0, sizeof why);
    assert(d9_tev_text_plan(&refused, 1, 0, 1, why, sizeof why) == -1);
    assert(strstr(why, "pixel shader") != NULL);
    assert(d9_tev_text_plan(NULL, 1, 0, 0, why, sizeof why) == -1);

    assert(!d9_adapter_caps(0, 1, &caps_after));
    assert(!memcmp(&caps_before, &caps_after, sizeof caps_before));
    puts("TEV text plan: MODULATE(TEXTURE0,PRIMARY), one stage and named refusals PASS");
    return 0;
}
