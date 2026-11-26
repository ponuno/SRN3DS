#include "d3d9_shader_plan.h"

#include <stdio.h>
#include <string.h>

/* Generated from docs/shader_plans.json.  Exactly one entry per compiled
 * SHA-256; no bytecode is embedded. */
#include "d3d9_shader_plan_table.inc"

unsigned d9_shader_plan_count(void) {
    return (unsigned)D9_SHADER_PLAN_ENTRY_COUNT;
}

const D9ShaderPlan *d9_shader_plan_at(unsigned index) {
    if (index >= D9_SHADER_PLAN_ENTRY_COUNT) return NULL;
    return &d9_shader_plan_entries[index];
}

static char lower_hex(char c) {
    return (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
}

int d9_shader_plan_sha256_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (unsigned i = 0; i < 64u; ++i) {
        if (lower_hex(a[i]) != lower_hex(b[i])) return 0;
        if (!a[i] || !b[i]) return 0;
    }
    return a[64] == 0 && b[64] == 0;
}

int d9_shader_plan_lookup(unsigned stage, const char *sha256_hex,
                          const D9ShaderPlan **out,
                          char *why, size_t why_size) {
    if (out) *out = NULL;
    if (why && why_size) why[0] = 0;
    if (!sha256_hex || strlen(sha256_hex) != 64u) {
        if (why && why_size) snprintf(why, why_size, "invalid SHA-256 argument");
        return -1;
    }
    for (unsigned i = 0; i < D9_SHADER_PLAN_ENTRY_COUNT; ++i) {
        const D9ShaderPlan *plan = &d9_shader_plan_entries[i];
        if (plan->stage != stage) continue;
        if (!d9_shader_plan_sha256_equal(plan->sha256, sha256_hex)) continue;
        if (out) *out = plan;
        return 1;
    }
    if (why && why_size) {
        snprintf(why, why_size, "no compiled %s plan for SHA-256 %.16s...",
                 stage == D9_SHADER_PLAN_STAGE_PS ? "pixel" : "vertex",
                 sha256_hex);
    }
    return 0;
}

int d9_shader_plan_draw_allowed(const D9ShaderPlan *plan,
                                char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!plan) {
        if (why && why_size) snprintf(why, why_size, "no plan selected");
        return -1;
    }
    if (!plan->draw_enabled) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s is not draw-enabled: %s",
                     plan->id ? plan->id : "(unnamed)",
                     plan->note ? plan->note : "no note");
        return -1;
    }
    if (plan->kind == D9_SHADER_PLAN_PICA_VS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: PICA VS code emitter is absent",
                     plan->id ? plan->id : "(unnamed)");
        return -1;
    }
    if (plan->kind != D9_SHADER_PLAN_TEV) {
        if (why && why_size) snprintf(why, why_size, "plan has unknown kind");
        return -1;
    }
    const D9TevProgram *tev = &plan->tev;
    if (tev->stages == 0 || tev->stages > D9TEV_MAX_STAGES) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u stages outside 1..%u",
                     plan->id ? plan->id : "(unnamed)", tev->stages,
                     (unsigned)D9TEV_MAX_STAGES);
        return -1;
    }
    if (tev->textures > D9TEV_MAX_TEXTURES) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u textures outside 0..%u",
                     plan->id ? plan->id : "(unnamed)", tev->textures,
                     (unsigned)D9TEV_MAX_TEXTURES);
        return -1;
    }
    if (tev->constants > D9TEV_MAX_CONSTANTS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u constants outside 0..%u",
                     plan->id ? plan->id : "(unnamed)", tev->constants,
                     (unsigned)D9TEV_MAX_CONSTANTS);
        return -1;
    }
    return 0;
}
