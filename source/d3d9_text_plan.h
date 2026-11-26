#pragma once
/* Canonical certified menu-text TEV plan (single source of truth).
 *
 * The menu atlas is an A8 texture: it carries glyph coverage in alpha only and
 * its RGB decode is (0,0,0,A).  The certified shape therefore takes RGB from
 * the vertex colour (REPLACE) and alpha from coverage times vertex alpha
 * (MODULATE).  This header used to exist twice: `core_text_plan()` in
 * d3d9_core.c and `d9_tev_text_plan()` in d3d9_tev.c drifted, with the latter
 * still emitting the old MODULATE(TEXTURE0,PRIMARY) RGB plan.  Both now call
 * this one builder.
 *
 * The body is a static function on purpose: `d3d9_core.c` is linked by many
 * small host tests that do not link `d3d9_tev.c`, so this must not create a
 * hard cross-TU dependency.
 */

#include <string.h>

#include "d3d9_tev.h"

static inline void d9_text_plan_a8(D9TevProgram *p) {
    memset(p, 0, sizeof *p);
    p->version = 0xFFFF0300u;
    p->instructions = 1u;
    p->stages = 1u;
    p->stage_limit = D9TEV_MAX_STAGES;
    p->backend_flags = D9TEV_PLAN_MENU_TEXT;
    p->textures = 1u;
    p->texture[0].texcoord_reg = 0u;
    p->texture[0].sampler_reg = 0u;
    p->texture[0].sampler_type = 2u;
    p->inputs = 1u;
    p->input[0].usage = D9TEV_USAGE_COLOR;
    p->input[0].usage_index = 0u;
    p->input[0].reg_index = 1u;
    p->input[0].tev_source = D9TEV_SRC_PRIMARY;
    p->uses_vertex_color = 1u;

    D9TevStage *s = &p->stage[0];
    s->func_rgb = D9TEV_FUNC_REPLACE;
    s->src_rgb[0] = s->src_rgb[1] = s->src_rgb[2] = D9TEV_SRC_PRIMARY;
    s->func_a = D9TEV_FUNC_MODULATE;
    s->src_a[0] = D9TEV_SRC_TEX0;
    s->src_a[1] = s->src_a[2] = D9TEV_SRC_PRIMARY;
    for (unsigned i = 0; i < 3; ++i) {
        s->op_rgb[i] = D9TEV_RGB_COLOR;
        s->op_a[i] = D9TEV_A_ALPHA;
    }
    s->const_reg = 0xFFu;
}
