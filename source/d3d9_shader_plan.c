#include "d3d9_shader_plan.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Generated from docs/shader_plans.json.  Exactly one entry per compiled
 * SHA-256; no bytecode is embedded. */
#include "d3d9_shader_plan_table.inc"

/* Exact SHA-256 -> emitted .v.pica id, generated from docs/generated_vs.json
 * by tools/vs_emit.py.  This table is append-only and separate from the
 * descriptor table: it records that assembly exists, not that a draw is
 * certified. */
#include "d3d9_generated_vs_table.inc"

/* Per-family coverage index (tools/shader_packs.py), append-only. */
#include "d3d9_shader_pack_table.inc"

unsigned d9_shader_pack_count(void) {
    return (unsigned)D9_SHADER_PACK_ROW_COUNT;
}

const D9ShaderPackRow *d9_shader_pack_at(unsigned index) {
    if (index >= D9_SHADER_PACK_ROW_COUNT) return NULL;
    return &d9_shader_pack_rows[index];
}

unsigned d9_shader_plan_count(void) {
    return (unsigned)D9_SHADER_PLAN_ENTRY_COUNT;
}

const D9ShaderPlan *d9_shader_plan_at(unsigned index) {
    if (index >= D9_SHADER_PLAN_ENTRY_COUNT) return NULL;
    return &d9_shader_plan_entries[index];
}

#ifdef D9_SHADER_PLAN_TEST_HOOKS
static const D9ShaderPlan *d9_test_entries;
static unsigned d9_test_count;

int d9_shader_plan_test_set(const D9ShaderPlan *entries, unsigned count) {
    if (!entries || !count) {
        d9_test_entries = NULL;
        d9_test_count = 0;
        return 0;
    }
    d9_test_entries = entries;
    d9_test_count = count;
    return 0;
}
#endif

static const D9ShaderPlan *lookup_entries(unsigned *count) {
#ifdef D9_SHADER_PLAN_TEST_HOOKS
    if (d9_test_entries) {
        *count = d9_test_count;
        return d9_test_entries;
    }
#endif
    *count = (unsigned)D9_SHADER_PLAN_ENTRY_COUNT;
    return d9_shader_plan_entries;
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

unsigned d9_generated_vs_count(void) {
    return (unsigned)D9_GENERATED_VS_ENTRY_COUNT;
}

const char *d9_generated_vs_id_at(unsigned index) {
    if (index >= D9_GENERATED_VS_ENTRY_COUNT) return NULL;
    return d9_generated_vs_entries[index].id;
}

int d9_generated_vs_lookup(const char *sha256_hex,
                           const char **id, const char **asm_name) {
    if (id) *id = NULL;
    if (asm_name) *asm_name = NULL;
    if (!sha256_hex || strlen(sha256_hex) != 64u) return -1;
    for (unsigned i = 0; i < D9_GENERATED_VS_ENTRY_COUNT; ++i) {
        if (!d9_shader_plan_sha256_equal(d9_generated_vs_entries[i].sha256,
                                         sha256_hex))
            continue;
        if (id) *id = d9_generated_vs_entries[i].id;
        if (asm_name) *asm_name = d9_generated_vs_entries[i].asm_name;
        return 1;
    }
    return 0;
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
    unsigned count = 0;
    const D9ShaderPlan *entries = lookup_entries(&count);
    for (unsigned i = 0; i < count; ++i) {
        const D9ShaderPlan *plan = &entries[i];
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

static int plan_tev_check(const D9ShaderPlan *plan, const D9TevProgram *tev,
                          const char *id, const char *label,
                          char *why, size_t why_size) {
    if (tev->stages == 0 || tev->stages > D9TEV_MAX_STAGES) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %s stages %u outside 1..%u",
                     id ? id : "(unnamed)", label, tev->stages,
                     (unsigned)D9TEV_MAX_STAGES);
        return -1;
    }
    if (tev->textures > D9TEV_MAX_TEXTURES) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %s textures %u outside 0..%u",
                     id ? id : "(unnamed)", label, tev->textures,
                     (unsigned)D9TEV_MAX_TEXTURES);
        return -1;
    }
    if (tev->constants > D9TEV_MAX_CONSTANTS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %s constants %u outside 0..%u",
                     id ? id : "(unnamed)", label, tev->constants,
                     (unsigned)D9TEV_MAX_CONSTANTS);
        return -1;
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
        /* A PICA VS descriptor is draw-enabled only through the emitted
         * program contract (d3d9_pica_vs.c: exact-hash .shbin, declaration and
         * 4-bone uniform bank).  The plan table alone cannot certify that, so
         * the per-draw gate in d3d9_com.c owns the decision. */
        if (why && why_size)
            snprintf(why, why_size,
                     "plan %s: PICA VS needs the emitted-program draw contract",
                     plan->id ? plan->id : "(unnamed)");
        return -1;
    }
    if (plan->kind != D9_SHADER_PLAN_TEV) {
        if (why && why_size) snprintf(why, why_size, "plan has unknown kind");
        return -1;
    }
    if (plan_tev_check(plan, &plan->tev, plan->id, "main", why, why_size))
        return -1;
    if (plan->const_op_count > D9_SHADER_PLAN_MAX_CONST_OPS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u constant ops outside 0..%u",
                     plan->id ? plan->id : "(unnamed)",
                     (unsigned)plan->const_op_count,
                     (unsigned)D9_SHADER_PLAN_MAX_CONST_OPS);
        return -1;
    }
    uint32_t derived_mask = 0;
    for (unsigned i = 0; i < plan->const_op_count; ++i) {
        const D9ShaderPlanConstOp *op = &plan->const_op[i];
        if (op->dst_reg >= D9_SHADER_PLAN_CONST_REGS) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: constant op %u writes c%u outside c0..c%u",
                         plan->id ? plan->id : "(unnamed)", i,
                         (unsigned)op->dst_reg,
                         (unsigned)(D9_SHADER_PLAN_CONST_REGS - 1u));
            return -1;
        }
        unsigned need = op->op == D9_SHADER_PLAN_CONST_PACK ? 1u
                      : op->op == D9_SHADER_PLAN_CONST_LERP ? 3u : 2u;
        if (op->op > D9_SHADER_PLAN_CONST_LERP || op->src_count != need) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: constant op %u needs %u source(s) for op %u",
                         plan->id ? plan->id : "(unnamed)", i, need,
                         (unsigned)op->op);
            return -1;
        }
        for (unsigned s = 0; s < op->src_count; ++s) {
            const D9ShaderPlanConstSrc *src = &op->src[s];
            if (src->reg >= D9_SHADER_PLAN_CONST_REGS) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: constant op %u reads c%u outside c0..c%u",
                             plan->id ? plan->id : "(unnamed)", i,
                             (unsigned)src->reg,
                             (unsigned)(D9_SHADER_PLAN_CONST_REGS - 1u));
                return -1;
            }
            for (unsigned lane = 0; lane < 4; ++lane)
                if (src->swizzle[lane] > 3u) {
                    if (why && why_size)
                        snprintf(why, why_size,
                                 "plan %s: constant op %u has swizzle lane %u "
                                 "outside x/y/z/w", plan->id ? plan->id
                                 : "(unnamed)", i, lane);
                    return -1;
                }
            if (src->negate > 1u) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: constant op %u has negate=%u (0/1)",
                             plan->id ? plan->id : "(unnamed)", i,
                             (unsigned)src->negate);
                return -1;
            }
        }
        derived_mask |= 1u << op->dst_reg;
    }
    /* A derived register must not overwrite a raw guest constant another stage
     * still reads (GFx keeps c0..c3 material data alongside derived c9+). */
    if (derived_mask) {
        uint32_t raw_mask = 0;
        for (unsigned i = 0; i < plan->tev.constants; ++i) {
            unsigned reg = plan->tev.constant_reg[i];
            if (reg < D9_SHADER_PLAN_CONST_REGS &&
                !(derived_mask & (1u << reg)))
                raw_mask |= 1u << reg;
        }
        uint32_t clash = raw_mask & derived_mask;
        if (clash) {
            unsigned reg = 0;
            while (!(clash & (1u << reg))) ++reg;
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: derived c%u collides with a raw constant "
                         "used by a stage", plan->id ? plan->id : "(unnamed)",
                         reg);
            return -1;
        }
    }
    if (plan->pass_count) {
        if (plan->pass_count < 2u ||
            plan->pass_count > D9_SHADER_PLAN_MAX_PASSES) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: %u passes outside 2..%u",
                         plan->id ? plan->id : "(unnamed)",
                         (unsigned)plan->pass_count,
                         (unsigned)D9_SHADER_PLAN_MAX_PASSES);
            return -1;
        }
        for (unsigned i = 0; i < plan->pass_count; ++i) {
            const D9ShaderPlanPass *pass = &plan->pass[i];
            char label[24];
            snprintf(label, sizeof label, "pass %u", i);
            if (plan_tev_check(plan, &pass->tev, plan->id, label,
                               why, why_size))
                return -1;
            if (pass->blend > 1u || pass->depth_write > 1u ||
                (pass->blend && (pass->src_blend < 1u ||
                                 pass->src_blend > 11u ||
                                 pass->dst_blend < 1u ||
                                 pass->dst_blend > 11u ||
                                 pass->blend_op < 1u ||
                                 pass->blend_op > 5u))) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: pass %u has an invalid blend/depth state",
                             plan->id ? plan->id : "(unnamed)", i);
                return -1;
            }
        }
    }
    return 0;
}

int d9_shader_plan_build_constants(const D9ShaderPlan *plan,
                                   const float (*src)[4], uint32_t src_mask,
                                   float (*out)[4], uint32_t *out_mask,
                                   char *why, size_t why_size) {
    if (out_mask) *out_mask = 0;
    if (why && why_size) why[0] = 0;
    if (!plan || !src || !out) {
        if (why && why_size) snprintf(why, why_size, "invalid constant build");
        return -1;
    }
    uint32_t available = src_mask;
    for (unsigned i = 0; i < plan->const_op_count; ++i) {
        const D9ShaderPlanConstOp *op = &plan->const_op[i];
        if (op->dst_reg >= D9_SHADER_PLAN_CONST_REGS ||
            op->op > D9_SHADER_PLAN_CONST_LERP) {
            if (why && why_size)
                snprintf(why, why_size, "constant op %u is malformed", i);
            return -1;
        }
        unsigned need = op->op == D9_SHADER_PLAN_CONST_PACK ? 1u
                      : op->op == D9_SHADER_PLAN_CONST_LERP ? 3u : 2u;
        if (op->src_count != need) {
            if (why && why_size)
                snprintf(why, why_size, "constant op %u needs %u sources",
                         i, need);
            return -1;
        }
        for (unsigned s = 0; s < op->src_count; ++s) {
            const D9ShaderPlanConstSrc *rs = &op->src[s];
            if (rs->reg >= D9_SHADER_PLAN_CONST_REGS ||
                !(available & (1u << rs->reg))) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: derived c%u needs source c%u.%c%c%c%c, "
                             "but c%u was never uploaded by "
                             "SetPixelShaderConstantF",
                             plan->id ? plan->id : "(unnamed)",
                             (unsigned)op->dst_reg, (unsigned)rs->reg,
                             "xyzw"[rs->swizzle[0]], "xyzw"[rs->swizzle[1]],
                             "xyzw"[rs->swizzle[2]], "xyzw"[rs->swizzle[3]],
                             (unsigned)rs->reg);
                return -1;
            }
            for (unsigned lane = 0; lane < 4; ++lane)
                if (!isfinite(src[rs->reg][lane])) {
                    if (why && why_size)
                        snprintf(why, why_size,
                                 "plan %s: derived c%u source c%u is not "
                                 "finite", plan->id ? plan->id : "(unnamed)",
                                 (unsigned)op->dst_reg, (unsigned)rs->reg);
                    return -1;
                }
        }
        for (unsigned lane = 0; lane < 4; ++lane) {
            float v[D9_SHADER_PLAN_MAX_CONST_SRC];
            for (unsigned s = 0; s < op->src_count; ++s) {
                const D9ShaderPlanConstSrc *rs = &op->src[s];
                v[s] = src[rs->reg][rs->swizzle[lane]];
                if (rs->negate) v[s] = -v[s];
            }
            float r;
            switch (op->op) {
            case D9_SHADER_PLAN_CONST_PACK: r = v[0]; break;
            case D9_SHADER_PLAN_CONST_ADD:  r = v[0] + v[1]; break;
            case D9_SHADER_PLAN_CONST_MUL:  r = v[0] * v[1]; break;
            default: {
                float t = v[2];
                r = v[0] * (1.0f - t) + v[1] * t;
                break;
            }
            }
            if (!isfinite(r)) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: derived c%u lane %u is not finite",
                             plan->id ? plan->id : "(unnamed)",
                             (unsigned)op->dst_reg, lane);
                return -1;
            }
            out[op->dst_reg][lane] = r;
        }
        if (out_mask) *out_mask |= 1u << op->dst_reg;
        available |= 1u << op->dst_reg;
    }
    return 0;
}
