/* Structural matching and host-side plan construction for specialised Skyrim
 * graphics pipelines. Descriptor data is generated in d3d9_pipeline_registry.c.
 * Nothing here uploads TEV state, changes D3D9 COM acceptance or renders a frame.
 */

#include "d3d9_pipeline_api.h"
#include "d3d9_menu_light.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PIPELINE_REG_TEMP 0u
#define PIPELINE_REG_INPUT 1u
#define PIPELINE_REG_CONST 2u
#define PIPELINE_REG_COLOROUT 8u
#define PIPELINE_REG_DEPTHOUT 9u
#define PIPELINE_REG_SAMPLER 10u
#define PIPELINE_SW_IDENTITY 0xE4u
#define PIPELINE_END 0xFFFFu
#define PIPELINE_COMMENT 0xFFFEu
#define PIPELINE_PS30 0xFFFF0300u

static void pipeline_error(char *why, size_t cap, const char *fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

static unsigned reg_type(uint32_t token) {
    return ((token >> 28) & 7u) | ((token & 0x1800u) >> 8);
}

static unsigned reg_number(uint32_t token) { return token & 0x7FFu; }
static unsigned dst_mask(uint32_t token) { return (token >> 16) & 0xFu; }
static unsigned dst_modifier(uint32_t token) { return (token >> 20) & 0xFu; }
static unsigned src_swizzle(uint32_t token) { return (token >> 16) & 0xFFu; }
static unsigned src_modifier(uint32_t token) { return (token >> 24) & 0xFu; }
static unsigned relative_addressing(uint32_t token) {
    return (token >> 13) & 7u;
}

static int instruction_has_destination(unsigned opcode) {
    switch (opcode) {
    case 0x19u: case 0x1Au: case 0x1Bu: case 0x1Cu: case 0x1Du:
    case 0x26u: case 0x27u: case 0x28u: case 0x29u: case 0x2Au:
    case 0x2Bu: case 0x2Cu: case 0x2Du: case 0x60u:
        return 0;
    default:
        return 1;
    }
}

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int opcode_bit(const uint64_t *mask, unsigned opcode) {
    return (mask[opcode >> 6] >> (opcode & 63u)) & 1u;
}

static unsigned bit_count32(uint32_t value) {
    unsigned count = 0;
    while (value) {
        value &= value - 1u;
        ++count;
    }
    return count;
}

static int list_contains(const char *const *list, const char *name) {
    if (!list || !name) return 0;
    for (size_t i = 0; list[i]; ++i)
        if (!strcmp(list[i], name)) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CTAB and SM3 token decoding                                               */
/* ------------------------------------------------------------------------- */

static int parse_ctab(const unsigned char *body, size_t dwords,
                      D9PipelineShaderView *out, char *why, size_t cap) {
    size_t bytes = dwords * 4u;
    if (bytes < 4u || memcmp(body, "CTAB", 4)) return 0;
    const unsigned char *table = body + 4;
    size_t table_bytes = bytes - 4u;
    if (table_bytes < 28u) {
        pipeline_error(why, cap, "truncated CTAB header");
        return -1;
    }
    unsigned count = read_u16(table + 12);
    uint32_t info_offset = read_u32(table + 16);
    if (count > D9_PIPELINE_MAX_CONSTANT_NAMES) out->ctab_truncated = 1;
    for (unsigned i = 0; i < count; ++i) {
        size_t entry = (size_t)info_offset + (size_t)i * 20u;
        if (entry + 20u > table_bytes) {
            pipeline_error(why, cap, "CTAB entry %u runs past comment", i);
            return -1;
        }
        uint32_t name_offset = read_u32(table + entry);
        if (!name_offset) continue;
        size_t start = (size_t)name_offset;
        if (start >= table_bytes) {
            pipeline_error(why, cap, "CTAB name %u runs past comment", i);
            return -1;
        }
        size_t end = start;
        while (end < table_bytes && table[end]) ++end;
        if (end == table_bytes) {
            pipeline_error(why, cap, "unterminated CTAB name %u", i);
            return -1;
        }
        size_t length = end - start;
        if (length >= D9_PIPELINE_MAX_CONSTANT_NAME) {
            out->ctab_truncated = 1;
            continue;
        }
        if (out->constant_count >= D9_PIPELINE_MAX_CONSTANT_NAMES) {
            out->ctab_truncated = 1;
            continue;
        }
        char *name = out->constant_names[out->constant_count++];
        memcpy(name, table + start, length);
        name[length] = 0;
    }
    return 0;
}

int d9_pipeline_parse_ps3(const uint32_t *code, size_t words,
                          D9PipelineShaderView *out,
                          char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!out) {
        pipeline_error(why, why_size, "null shader view");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (!code || words < 2u) {
        pipeline_error(why, why_size, "not a complete ps_3_0 token stream");
        return -1;
    }
    out->code = code;
    out->words = words;
    out->version = code[0];
    if (out->version != PIPELINE_PS30) {
        pipeline_error(why, why_size, "not a ps_3_0 program");
        return -1;
    }

    int saw_end = 0;
    int saw_ctab = 0;
    size_t pos = 1;
    while (pos < words) {
        uint32_t token = code[pos];
        unsigned opcode = token & 0xFFFFu;
        if (opcode == PIPELINE_END) {
            saw_end = 1;
            ++pos;
            break;
        }
        if (opcode == PIPELINE_COMMENT) {
            size_t dwords = (token >> 16) & 0x7FFFu;
            if (dwords > words - pos - 1u) {
                pipeline_error(why, why_size, "truncated comment at word %zu", pos);
                out->malformed = 1;
                return -1;
            }
            if (!saw_ctab && dwords &&
                memcmp(code + pos + 1u, "CTAB", 4) == 0) {
                saw_ctab = 1;
                if (parse_ctab((const unsigned char *)(code + pos + 1u),
                               dwords, out, why, why_size) < 0) {
                    out->malformed = 1;
                    return -1;
                }
            }
            pos += 1u + dwords;
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (total > words - pos) {
            pipeline_error(why, why_size, "truncated instruction at word %zu", pos);
            out->malformed = 1;
            return -1;
        }
        if (out->instructions >= D9_PIPELINE_MAX_INSTRUCTIONS) {
            pipeline_error(why, why_size, "more than %d instructions",
                           D9_PIPELINE_MAX_INSTRUCTIONS);
            out->malformed = 1;
            return -1;
        }
        ++out->instructions;
        if (opcode < 256u) out->opcodes[opcode >> 6] |= UINT64_C(1) << (opcode & 63u);
        else out->unknown_opcode = 1;

        unsigned first_source = 1u;
        if (opcode == 0x1Fu) {
            if (total != 3u) out->malformed_operand = 1;
            else if (!(code[pos + 1u] & 0x80000000u) ||
                     !(code[pos + 2u] & 0x80000000u))
                out->malformed_operand = 1;
            first_source = total; /* declaration operand is not a source */
        } else if (opcode == 0x51u || opcode == 0x30u ||
                   opcode == 0x2Fu || opcode == 0x1Eu) {
            if (total < 2u || !(code[pos + 1u] & 0x80000000u))
                out->malformed_operand = 1;
            first_source = total; /* remaining operands are literals */
        } else if (instruction_has_destination(opcode)) {
            if (total < 2u || !(code[pos + 1u] & 0x80000000u))
                out->malformed_operand = 1;
            first_source = 2u;
        }
        for (unsigned operand = first_source; operand < total; ++operand) {
            uint32_t source = code[pos + operand];
            if (!(source & 0x80000000u)) out->malformed_operand = 1;
            if (relative_addressing(source)) out->has_relative_addressing = 1;
        }

        if (opcode == 0x1Fu && total == 3u) {
            uint32_t decl = code[pos + 1u];
            uint32_t dst = code[pos + 2u];
            unsigned usage = decl & 0x1Fu;
            unsigned usage_index = (decl >> 16) & 0xFu;
            unsigned texture_type = (decl >> 27) & 0xFu;
            unsigned type = reg_type(dst);
            unsigned index = reg_number(dst);
            if (type == PIPELINE_REG_INPUT) {
                if (out->declaration_count >= D9_PIPELINE_MAX_DECLARATIONS) {
                    pipeline_error(why, why_size, "more than %d input declarations",
                                   D9_PIPELINE_MAX_DECLARATIONS);
                    out->malformed = 1;
                    return -1;
                }
                unsigned slot = out->declaration_count++;
                out->input_usage[slot] = (uint8_t)usage;
                out->input_usage_index[slot] = (uint8_t)usage_index;
                out->input_reg[slot] = (uint8_t)index;
                if (usage == D9TEV_USAGE_COLOR) {
                    int seen = 0;
                    for (unsigned i = 0; i < slot; ++i)
                        if (out->input_reg[i] == index) seen = 1;
                    if (!seen && out->color_input_count < 255u)
                        ++out->color_input_count;
                }
            } else if (type == PIPELINE_REG_SAMPLER) {
                if (index >= D9_PIPELINE_MAX_SAMPLERS) {
                    pipeline_error(why, why_size, "sampler s%u exceeds registry limit", index);
                    out->malformed = 1;
                    return -1;
                }
                out->sampler_declared[index] = 1;
                out->sampler_type[index] = (uint8_t)texture_type;
                ++out->samplers;
            }
        }

        if (total >= 2u) {
            uint32_t dst = code[pos + 1u];
            unsigned type = reg_type(dst);
            unsigned index = reg_number(dst);
            if (type == PIPELINE_REG_COLOROUT) {
                out->has_color_output = 1;
                if (index == 0u) out->single_output = 1;
                else out->has_mrt = 1;
            } else if (type == PIPELINE_REG_DEPTHOUT) {
                out->has_depth_output = 1;
            }
        }

        if (opcode == 0x42u || opcode == 0x5Du || opcode == 0x5Fu) {
            ++out->textures;
            if (total >= 4u) {
                uint32_t sampler = code[pos + 3u];
                if (reg_type(sampler) == PIPELINE_REG_SAMPLER &&
                    reg_number(sampler) < D9_PIPELINE_MAX_SAMPLERS)
                    out->sampler_used |= UINT32_C(1) << reg_number(sampler);
            }
        }
        if (opcode == 0x19u || opcode == 0x1Au || opcode == 0x1Bu ||
            opcode == 0x1Cu || opcode == 0x1Du || opcode == 0x26u ||
            opcode == 0x27u || opcode == 0x28u || opcode == 0x29u ||
            opcode == 0x2Au || opcode == 0x2Bu || opcode == 0x2Cu ||
            opcode == 0x2Du || opcode == 0x60u)
            ++out->branches;
        if (opcode == 0x1Bu || opcode == 0x1Du || opcode == 0x26u || opcode == 0x27u)
            ++out->loops;
        pos += total;
    }
    if (!saw_end) {
        pipeline_error(why, why_size, "missing END token");
        out->malformed = 1;
        return -1;
    }
    if (pos != words) {
        pipeline_error(why, why_size, "%zu trailing words after END", words - pos);
        out->malformed = 1;
        return -1;
    }
    if (out->malformed_operand) {
        pipeline_error(why, why_size, "malformed register operand token");
        out->malformed = 1;
        return -1;
    }
    if (out->has_relative_addressing) {
        pipeline_error(why, why_size, "relative register addressing is outside pipeline contracts");
        out->malformed = 1;
        return -1;
    }
    if (out->ctab_truncated) {
        pipeline_error(why, why_size, "CTAB exceeds bounded registry view");
        out->malformed = 1;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Registry introspection and invariant validation                          */
/* ------------------------------------------------------------------------- */

const D9PipelineRegistry *d9_pipeline_registry(void) {
    return &d9_pipeline_registry_data;
}

size_t d9_pipeline_descriptor_count(void) {
    return d9_pipeline_registry_data.descriptor_count;
}

static const D9PipelineDescriptor *registry_find(const D9PipelineRegistry *registry,
                                                 const char *id) {
    if (!registry || !id) return NULL;
    for (size_t i = 0; i < registry->descriptor_count; ++i)
        if (!strcmp(registry->descriptors[i].id, id)) return &registry->descriptors[i];
    return NULL;
}

const D9PipelineDescriptor *d9_pipeline_find(const char *id) {
    return registry_find(d9_pipeline_registry(), id);
}

const char *d9_pipeline_family_name(D9PipelineFamily family) {
    switch (family) {
    case D9_PIPELINE_INTERFACE_TEXT: return "interface_text";
    case D9_PIPELINE_SKY: return "sky";
    case D9_PIPELINE_LANDSCAPE: return "landscape";
    case D9_PIPELINE_STATIC_GEOMETRY: return "static_geometry";
    case D9_PIPELINE_CHARACTERS_ANIMATION: return "characters_animation";
    case D9_PIPELINE_VEGETATION: return "vegetation";
    case D9_PIPELINE_WATER: return "water";
    case D9_PIPELINE_PARTICLES: return "particles";
    default: return "any";
    }
}

const char *d9_pipeline_support_name(D9PipelineSupport support) {
    switch (support) {
    case D9_PIPELINE_SUPPORT_STRUCTURAL: return "structural";
    case D9_PIPELINE_SUPPORT_PREPROCESS: return "preprocess";
    case D9_PIPELINE_SUPPORT_PLANNED: return "planned";
    default: return "invalid";
    }
}

const char *d9_pipeline_precondition_name(uint32_t bit) {
    switch (bit) {
    case D9_PIPELINE_PRE_NORMAL_PREBAKED: return "normal_prebaked";
    case D9_PIPELINE_PRE_TANGENT_BASIS_PREBAKED: return "tangent_basis_prebaked";
    case D9_PIPELINE_PRE_AMBIENT_EMISSIVE_VARYING: return "ambient_emissive_varying";
    case D9_PIPELINE_PRE_MATERIAL_ALPHA_C4: return "material_alpha_c4";
    case D9_PIPELINE_PRE_NORMALS_PREDECODED: return "normals_predecoded";
    case D9_PIPELINE_PRE_BONE_PALETTE_READY: return "bone_palette_ready";
    case D9_PIPELINE_PRE_WIND_PARAMETERS_READY: return "wind_parameters_ready";
    case D9_PIPELINE_PRE_REFLECTION_TARGET_READY: return "reflection_target_ready";
    case D9_PIPELINE_PRE_REFRACTION_TARGET_READY: return "refraction_target_ready";
    case D9_PIPELINE_PRE_PARTICLE_BATCH_READY: return "particle_batch_ready";
    case D9_PIPELINE_PRE_SPRITE_SWIZZLE_BAKED: return "sprite_swizzle_baked";
    default: return "unknown_precondition";
    }
}

static int nonempty(const char *text) { return text && text[0]; }

int d9_pipeline_registry_validate(const D9PipelineRegistry *registry,
                                  char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!registry || registry->schema_version != 1 ||
        registry->family_count != D9_PIPELINE_MAX_FAMILIES ||
        !registry->families || !registry->descriptors ||
        registry->descriptor_count == 0 ||
        registry->descriptor_count > D9_PIPELINE_MAX_DESCRIPTORS) {
        pipeline_error(why, why_size, "invalid registry header/capacity");
        return -1;
    }
    unsigned family_seen[D9_PIPELINE_MAX_FAMILIES] = {0};
    for (size_t i = 0; i < registry->family_count; ++i) {
        const D9PipelineFamilyInfo *family = &registry->families[i];
        if (!nonempty(family->name) || !nonempty(family->title) ||
            !nonempty(family->inventory_note) ||
            strcmp(family->name, d9_pipeline_family_name((D9PipelineFamily)i))) {
            pipeline_error(why, why_size, "invalid family row %zu", i);
            return -1;
        }
        if (nonempty(family->default_fallback) &&
            !registry_find(registry, family->default_fallback)) {
            pipeline_error(why, why_size, "family %s has unresolved default fallback", family->name);
            return -1;
        }
    }
    for (size_t i = 0; i < registry->descriptor_count; ++i) {
        const D9PipelineDescriptor *d = &registry->descriptors[i];
        if (!nonempty(d->id) || (unsigned)d->family >= D9_PIPELINE_MAX_FAMILIES ||
            d->support < D9_PIPELINE_SUPPORT_STRUCTURAL ||
            d->support > D9_PIPELINE_SUPPORT_PLANNED ||
            d->source < D9_PIPELINE_SOURCE_FXP_PS3 ||
            d->source > D9_PIPELINE_SOURCE_FIXED_MATERIAL ||
            d->matcher < D9_PIPELINE_MATCHER_NONE ||
            d->matcher > D9_PIPELINE_MATCHER_SCALEFORM_GLYPH) {
            pipeline_error(why, why_size, "descriptor %zu has invalid identity", i);
            return -1;
        }
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(d->id, registry->descriptors[j].id)) {
                pipeline_error(why, why_size, "duplicate descriptor id %s", d->id);
                return -1;
            }
        family_seen[d->family] = 1;
        const char *text_fields[] = {
            d->inventory_signature, d->accepted_original, d->package_rule,
            d->vertex_inputs, d->texture_units, d->quality_reductions,
            d->unsupported, d->fallback, d->memory, d->preconditions
        };
        for (size_t f = 0; f < sizeof text_fields / sizeof text_fields[0]; ++f) {
            if (!nonempty(text_fields[f])) {
                pipeline_error(why, why_size, "%s: empty documentation field %zu", d->id, f);
                return -1;
            }
        }
        if (!d->pass_count || d->pass_count > D9_PIPELINE_MAX_PASSES) {
            pipeline_error(why, why_size, "%s: invalid pass count", d->id);
            return -1;
        }
        for (size_t p = 0; p < d->pass_count; ++p) {
            const D9PipelinePass *pass = &d->passes[p];
            if (!nonempty(pass->name) || !nonempty(pass->vertex_inputs) ||
                !nonempty(pass->texture_units) || !nonempty(pass->blend) ||
                !nonempty(pass->depth) || !nonempty(pass->alpha_test) ||
                !nonempty(pass->target) || !nonempty(pass->summary) ||
                pass->stages > D9TEV_MAX_STAGES || pass->textures > D9TEV_MAX_TEXTURES) {
                pipeline_error(why, why_size, "%s: invalid pass %zu", d->id, p);
                return -1;
            }
        }
        if ((d->support == D9_PIPELINE_SUPPORT_PLANNED) !=
            (d->matcher == D9_PIPELINE_MATCHER_NONE)) {
            pipeline_error(why, why_size, "%s: support/matcher mismatch", d->id);
            return -1;
        }
        if (d->source == D9_PIPELINE_SOURCE_FXP_PS3) {
            if (d->features.version != PIPELINE_PS30 ||
                d->features.max_instructions > D9_PIPELINE_MAX_INSTRUCTIONS ||
                d->features.min_instructions > d->features.max_instructions) {
                pipeline_error(why, why_size, "%s: invalid ps_3_0 feature bounds", d->id);
                return -1;
            }
        } else if (d->features.version != 0 || d->features.min_instructions != 0 ||
                   d->features.max_instructions != 0) {
            pipeline_error(why, why_size, "%s: fixed material cannot declare SM3 instructions", d->id);
            return -1;
        }
        if (d->features.max_tev_stages > D9TEV_MAX_STAGES ||
            d->features.max_tev_textures > D9TEV_MAX_TEXTURES ||
            d->features.max_color_inputs > 3 ||
            d->features.max_samplers > D9_PIPELINE_MAX_SAMPLERS ||
            d->features.required_input_count > D9_PIPELINE_MAX_REQUIRED_INPUTS) {
            pipeline_error(why, why_size, "%s: feature capacity overflow", d->id);
            return -1;
        }
        for (size_t r = 0; r < d->features.required_input_count; ++r) {
            const D9PipelineInputRequirement *in = &d->features.required_input[r];
            if (in->usage > 13u || in->reg >= 16u ||
                (in->sampler_type != 0u && in->sampler_type != 2u)) {
                pipeline_error(why, why_size, "%s: invalid required input %zu", d->id, r);
                return -1;
            }
            for (size_t q = 0; q < r; ++q) {
                const D9PipelineInputRequirement *prev = &d->features.required_input[q];
                if (in->usage == prev->usage && in->index == prev->index &&
                    in->reg == prev->reg && in->sampler_type == prev->sampler_type) {
                    pipeline_error(why, why_size, "%s: duplicate required input %zu", d->id, r);
                    return -1;
                }
            }
        }
        size_t required_count = 0, forbidden_count = 0;
        if (!d->required_constants || !d->forbidden_constants) {
            pipeline_error(why, why_size, "%s: missing constant vocabulary", d->id);
            return -1;
        }
        for (; forbidden_count < D9_PIPELINE_MAX_FORBIDDEN_CONSTANTS; ++forbidden_count)
            if (!d->forbidden_constants[forbidden_count]) break;
        if (forbidden_count == D9_PIPELINE_MAX_FORBIDDEN_CONSTANTS &&
            d->forbidden_constants[forbidden_count]) {
            pipeline_error(why, why_size, "%s: forbidden vocabulary overflow", d->id);
            return -1;
        }
        for (; required_count < D9_PIPELINE_MAX_REQUIRED_CONSTANTS; ++required_count) {
            const char *name = d->required_constants[required_count];
            if (!name) break;
            if (list_contains(d->forbidden_constants, name)) {
                pipeline_error(why, why_size, "%s: constant vocabulary conflict", d->id);
                return -1;
            }
        }
        if (required_count == D9_PIPELINE_MAX_REQUIRED_CONSTANTS &&
            d->required_constants[required_count]) {
            pipeline_error(why, why_size, "%s: required vocabulary overflow", d->id);
            return -1;
        }
        if ((d->features.constant_policy == D9_PIPELINE_CONSTANTS_REQUIRE_ANY ||
             d->features.constant_policy == D9_PIPELINE_CONSTANTS_REQUIRE_ALL) &&
            required_count == 0) {
            pipeline_error(why, why_size, "%s: require policy without names", d->id);
            return -1;
        }
        if (nonempty(d->fallback_id)) {
            const D9PipelineDescriptor *fallback = registry_find(registry, d->fallback_id);
            if (!fallback || fallback == d || fallback->family != d->family) {
                pipeline_error(why, why_size, "%s: invalid fallback %s", d->id, d->fallback_id);
                return -1;
            }
        }
    }
    for (size_t i = 0; i < registry->family_count; ++i) {
        if (!family_seen[i]) {
            pipeline_error(why, why_size, "family %s has no descriptor",
                           d9_pipeline_family_name((D9PipelineFamily)i));
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Feature and structural matching                                          */
/* ------------------------------------------------------------------------- */

static int constant_policy_matches(const D9PipelineDescriptor *d,
                                   const D9PipelineShaderView *view,
                                   char *why, size_t cap) {
    unsigned required = 0, present = 0;
    while (d->required_constants[required]) {
        for (unsigned i = 0; i < view->constant_count; ++i)
            if (!strcmp(d->required_constants[required], view->constant_names[i])) {
                ++present;
                break;
            }
        ++required;
    }
    if (d->features.constant_policy != D9_PIPELINE_CONSTANTS_NONE) {
        for (unsigned i = 0; i < view->constant_count; ++i)
            if (list_contains(d->forbidden_constants, view->constant_names[i])) {
                pipeline_error(why, cap, "%s: forbidden CTAB name %s", d->id,
                               view->constant_names[i]);
                return 0;
            }
    }
    switch (d->features.constant_policy) {
    case D9_PIPELINE_CONSTANTS_NONE:
        return 1;
    case D9_PIPELINE_CONSTANTS_FORBID:
        return 1;
    case D9_PIPELINE_CONSTANTS_REQUIRE_ANY:
        if (!present) {
            pipeline_error(why, cap, "%s: no required CTAB vocabulary", d->id);
            return 0;
        }
        return 1;
    case D9_PIPELINE_CONSTANTS_REQUIRE_ALL:
        if (present != required) {
            pipeline_error(why, cap, "%s: missing base-lit CTAB vocabulary (%u/%u)",
                           d->id, present, required);
            return 0;
        }
        return 1;
    default:
        pipeline_error(why, cap, "%s: invalid constant policy", d->id);
        return 0;
    }
}

static int required_input_present(const D9PipelineInputRequirement *required,
                                  const D9PipelineShaderView *view) {
    if (required->sampler_type) {
        return required->reg < D9_PIPELINE_MAX_SAMPLERS &&
               view->sampler_declared[required->reg] &&
               view->sampler_type[required->reg] == required->sampler_type;
    }
    for (unsigned i = 0; i < view->declaration_count; ++i)
        if (view->input_usage[i] == required->usage &&
            view->input_usage_index[i] == required->index &&
            view->input_reg[i] == required->reg)
            return 1;
    return 0;
}

static int features_match(const D9PipelineDescriptor *d,
                          const D9PipelineShaderView *view,
                          const D9PipelineRequest *request,
                          char *why, size_t cap) {
    const D9PipelineFeatures *f = &d->features;
    if (view->malformed || view->unknown_opcode) {
        pipeline_error(why, cap, "%s: malformed or unknown-opcode shader view", d->id);
        return 0;
    }
    if (view->version != f->version ||
        view->instructions < f->min_instructions ||
        view->instructions > f->max_instructions ||
        view->textures < f->min_textures || view->textures > f->max_textures ||
        view->samplers > f->max_samplers ||
        bit_count32(view->sampler_used) > f->max_tev_textures ||
        view->branches > f->max_branches || view->loops > f->max_loops ||
        view->color_input_count > f->max_color_inputs) {
        pipeline_error(why, cap, "%s: instruction/texture/input/branch bounds mismatch", d->id);
        return 0;
    }
    if (f->require_single_output && (!view->has_color_output || !view->single_output)) {
        pipeline_error(why, cap, "%s: requires exactly oC0", d->id);
        return 0;
    }
    if ((f->forbid_mrt && view->has_mrt) || view->has_depth_output) {
        pipeline_error(why, cap, "%s: MRT/depth output is unsupported", d->id);
        return 0;
    }
    if (f->require_2d) {
        for (unsigned i = 0; i < D9_PIPELINE_MAX_SAMPLERS; ++i)
            if (view->sampler_declared[i] && view->sampler_type[i] != 2u) {
                pipeline_error(why, cap, "%s: sampler s%u is not 2D", d->id, i);
                return 0;
            }
    }
    for (unsigned opcode = 0; opcode < 256u; ++opcode) {
        if (opcode_bit(view->opcodes, opcode) && !opcode_bit(f->allowed_opcodes, opcode)) {
            pipeline_error(why, cap, "%s: opcode 0x%02x is outside the signature", d->id, opcode);
            return 0;
        }
    }
    for (unsigned i = 0; i < view->declaration_count; ++i) {
        unsigned usage = view->input_usage[i];
        if (usage >= 16u || !(f->allowed_input_usages & (1u << usage))) {
            pipeline_error(why, cap, "%s: input usage %u is outside the signature", d->id, usage);
            return 0;
        }
    }
    for (unsigned i = 0; i < f->required_input_count; ++i) {
        if (!required_input_present(&f->required_input[i], view)) {
            pipeline_error(why, cap, "%s: missing required declaration %zu", d->id, i);
            return 0;
        }
    }
    if (f->require_ldr && !(request->quality & D9_PIPELINE_QUALITY_LDR)) {
        pipeline_error(why, cap, "%s: caller must contract for LDR quality", d->id);
        return 0;
    }
    return constant_policy_matches(d, view, why, cap);
}

typedef struct {
    unsigned op;
    uint32_t dst;
    uint32_t src[3];
    unsigned nsrc;
    uint32_t decl;
} PipelineIns;

static int decode_instructions(const D9PipelineShaderView *view,
                               PipelineIns *ins, unsigned capacity,
                               unsigned *count, char *why, size_t cap) {
    size_t pos = 1;
    unsigned n = 0;
    while (pos < view->words) {
        uint32_t token = view->code[pos];
        unsigned op = token & 0xFFFFu;
        if (op == PIPELINE_END) break;
        if (op == PIPELINE_COMMENT) {
            pos += 1u + ((token >> 16) & 0x7FFFu);
            continue;
        }
        size_t total = 1u + ((token >> 24) & 0xFu);
        if (n >= capacity) {
            pipeline_error(why, cap, "structural signature has too many instructions");
            return -1;
        }
        PipelineIns *row = &ins[n++];
        memset(row, 0, sizeof *row);
        row->op = op;
        if (op == 0x1Fu && total == 3u) {
            row->decl = view->code[pos + 1u];
            row->dst = view->code[pos + 2u];
            row->nsrc = 1;
        } else if (total >= 2u) {
            row->dst = view->code[pos + 1u];
            row->nsrc = (unsigned)total - 2u;
            if (row->nsrc > 3u) {
                pipeline_error(why, cap, "instruction has more than three sources");
                return -1;
            }
            for (unsigned i = 0; i < row->nsrc; ++i)
                row->src[i] = view->code[pos + 2u + i];
        }
        pos += total;
    }
    *count = n;
    return 0;
}

static int dst_is(uint32_t token, unsigned type, unsigned reg, unsigned mask) {
    return reg_type(token) == type && reg_number(token) == reg &&
           dst_mask(token) == mask && dst_modifier(token) == 0u;
}

static int src_is(uint32_t token, unsigned type, unsigned reg, unsigned swizzle) {
    return (token & 0x80000000u) && reg_type(token) == type &&
           reg_number(token) == reg && src_swizzle(token) == swizzle &&
           src_modifier(token) == 0u && relative_addressing(token) == 0u;
}

static int dcl_is(const PipelineIns *ins, unsigned usage, unsigned index,
                  unsigned type, unsigned reg, unsigned texture_type) {
    return ins->op == 0x1Fu && (ins->decl & 0x1Fu) == usage &&
           ((ins->decl >> 16) & 0xFu) == index &&
           ((ins->decl >> 27) & 0xFu) == texture_type &&
           reg_type(ins->dst) == type && reg_number(ins->dst) == reg;
}

static int match_blit(const D9PipelineDescriptor *d,
                      const D9PipelineShaderView *view,
                      D9PipelineMatch *out, char *why, size_t cap) {
    PipelineIns ins[4];
    unsigned count = 0;
    if (decode_instructions(view, ins, 4, &count, why, cap) < 0) return 0;
    if (count != 3u || ins[0].op != 0x1Fu || ins[1].op != 0x1Fu || ins[2].op != 0x42u) {
        pipeline_error(why, cap, "%s: expected exactly dcl,dcl,texld", d->id);
        return 0;
    }
    if (!dcl_is(&ins[0], D9TEV_USAGE_TEXCOORD, 0u, PIPELINE_REG_INPUT, 0u, 0u) ||
        !dcl_is(&ins[1], 0u, 0u, PIPELINE_REG_SAMPLER, 0u, 2u)) {
        pipeline_error(why, cap, "%s: copy declaration mismatch", d->id);
        return 0;
    }
    if (!dst_is(ins[2].dst, PIPELINE_REG_COLOROUT, 0u, 0xFu) ||
        ins[2].nsrc != 2u ||
        !src_is(ins[2].src[0], PIPELINE_REG_INPUT, 0u, PIPELINE_SW_IDENTITY) ||
        !src_is(ins[2].src[1], PIPELINE_REG_SAMPLER, 0u, PIPELINE_SW_IDENTITY)) {
        pipeline_error(why, cap, "%s: texld register/mask/swizzle mismatch", d->id);
        return 0;
    }
    out->signature = D9_PIPELINE_SIGNATURE_COPY;
    snprintf(out->detail, sizeof out->detail, "exact structural copy");
    return 1;
}

static int match_simple_tev(const D9PipelineDescriptor *d,
                            const D9PipelineShaderView *view,
                            D9PipelineMatch *out, char *why, size_t cap) {
    D9TevProgram program;
    char translation_why[256];
    if (d9_tev_translate_pixel(view->code, view->words, &program,
                               translation_why, sizeof translation_why) < 0) {
        pipeline_error(why, cap, "%s: TEV rejected: %s", d->id, translation_why);
        return 0;
    }
    if (program.stages > d->features.max_tev_stages ||
        program.textures > d->features.max_tev_textures ||
        program.inputs > 3u) {
        pipeline_error(why, cap, "%s: translated plan exceeds descriptor limits", d->id);
        return 0;
    }
    out->signature = D9_PIPELINE_SIGNATURE_SIMPLE_TEV;
    snprintf(out->detail, sizeof out->detail, "%u stages, %u textures",
             program.stages, program.textures);
    return 1;
}

static int match_menu_light(const D9PipelineDescriptor *d,
                            const D9PipelineShaderView *view,
                            D9PipelineMatch *out, char *why, size_t cap) {
    D9MenuLightMatch menu;
    char menu_why[256];
    if (!d9_menu_light_match(view->code, view->words, &menu,
                             menu_why, sizeof menu_why)) {
        pipeline_error(why, cap, "%s: %s", d->id, menu_why);
        return 0;
    }
    if (menu.signature != d->menu_signature) {
        pipeline_error(why, cap, "%s: menu-light signature variant mismatch", d->id);
        return 0;
    }
    out->signature = menu.signature;
    snprintf(out->detail, sizeof out->detail, "%s",
             d9_menu_light_signature_name(menu.signature));
    return 1;
}

static int match_glyph(const D9PipelineDescriptor *d,
                       const D9PipelineShaderView *view,
                       const D9PipelineRequest *request,
                       D9PipelineMatch *out, char *why, size_t cap) {
    (void)view;
    if (request->source != D9_PIPELINE_SOURCE_FIXED_SCALEFORM ||
        request->material_kind != D9_PIPELINE_MATERIAL_SCALEFORM_GLYPH_ATLAS) {
        pipeline_error(why, cap, "%s: requires Scaleform glyph-atlas material contract", d->id);
        return 0;
    }
    out->signature = D9_PIPELINE_SIGNATURE_SCALEFORM_GLYPH;
    snprintf(out->detail, sizeof out->detail, "fixed straight-alpha glyph contract");
    return 1;
}

int d9_pipeline_try(const D9PipelineDescriptor *descriptor,
                    const D9PipelineShaderView *view,
                    const D9PipelineRequest *request,
                    D9PipelineMatch *out, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!descriptor || !request || !out) {
        pipeline_error(why, why_size, "null pipeline match argument");
        return -1;
    }
    out->pipeline = descriptor;
    if (descriptor->support == D9_PIPELINE_SUPPORT_PLANNED) {
        pipeline_error(why, why_size, "%s: descriptor is planned, not implemented", descriptor->id);
        return 0;
    }
    if (request->source != descriptor->source) {
        pipeline_error(why, why_size, "%s: shader/material source mismatch", descriptor->id);
        return 0;
    }
    if (descriptor->features.require_ldr &&
        !(request->quality & D9_PIPELINE_QUALITY_LDR)) {
        pipeline_error(why, why_size, "%s: caller must contract for LDR quality", descriptor->id);
        return 0;
    }
    if (descriptor->source == D9_PIPELINE_SOURCE_FXP_PS3) {
        if (!view || !view->code || view->malformed) {
            pipeline_error(why, why_size, "%s: requires a validated ps_3_0 view", descriptor->id);
            return -1;
        }
        if (!features_match(descriptor, view, request, why, why_size)) return 0;
        out->instructions = view->instructions;
        switch (descriptor->matcher) {
        case D9_PIPELINE_MATCHER_BLIT_EXACT:
            return match_blit(descriptor, view, out, why, why_size);
        case D9_PIPELINE_MATCHER_SIMPLE_TEV:
            return match_simple_tev(descriptor, view, out, why, why_size);
        case D9_PIPELINE_MATCHER_MENU_LIGHT:
            return match_menu_light(descriptor, view, out, why, why_size);
        default:
            pipeline_error(why, why_size, "%s: invalid fxp matcher", descriptor->id);
            return 0;
        }
    }
    if (view) {
        pipeline_error(why, why_size, "%s: fixed material must not carry SM3 bytecode", descriptor->id);
        return 0;
    }
    if (descriptor->matcher == D9_PIPELINE_MATCHER_SCALEFORM_GLYPH)
        return match_glyph(descriptor, view, request, out, why, why_size);
    pipeline_error(why, why_size, "%s: no fixed-material matcher", descriptor->id);
    return 0;
}

const D9PipelineDescriptor *d9_pipeline_select(
    D9PipelineFamily family, const D9PipelineShaderView *view,
    const D9PipelineRequest *request, D9PipelineMatch *out,
    char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!request) {
        pipeline_error(why, why_size, "null pipeline request");
        return NULL;
    }
    const D9PipelineRegistry *registry = d9_pipeline_registry();
    const D9PipelineDescriptor *best = NULL;
    D9PipelineMatch candidate;
    char candidate_why[256];
    for (size_t i = 0; i < registry->descriptor_count; ++i) {
        const D9PipelineDescriptor *d = &registry->descriptors[i];
        if (family != D9_PIPELINE_FAMILY_ANY && d->family != family) continue;
        if (d9_pipeline_try(d, view, request, &candidate,
                            candidate_why, sizeof candidate_why) == 1 &&
            (!best || d->priority < best->priority)) {
            best = d;
            if (out) *out = candidate;
        }
    }
    if (best) return best;
    pipeline_error(why, why_size,
                   "no implemented structural signature for %s; fallback is explicit",
                   d9_pipeline_family_name(family));
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Host-side TEV plans                                                       */
/* ------------------------------------------------------------------------- */

static int missing_preconditions(const D9PipelineDescriptor *d,
                                 const D9PipelineRequest *request,
                                 char *why, size_t cap) {
    uint32_t missing = d->precondition_mask & ~request->preconditions;
    if (!missing) return 0;
    char names[192] = {0};
    size_t used = 0;
    for (unsigned bit = 0; bit < 32u; ++bit) {
        uint32_t value = UINT32_C(1) << bit;
        if (!(missing & value)) continue;
        const char *name = d9_pipeline_precondition_name(value);
        int wrote = snprintf(names + used, sizeof names - used,
                             "%s%s", used ? ", " : "", name);
        if (wrote < 0 || (size_t)wrote >= sizeof names - used) break;
        used += (size_t)wrote;
    }
    pipeline_error(why, cap, "%s: missing preconditions: %s", d->id, names);
    return -1;
}

static int build_glyph(D9TevProgram *out) {
    memset(out, 0, sizeof *out);
    out->version = PIPELINE_PS30;
    out->stage_limit = D9TEV_MAX_STAGES;
    out->stages = 1;
    out->textures = 1;
    out->texture[0].texcoord_reg = 0;
    out->texture[0].sampler_reg = 0;
    out->texture[0].sampler_type = 2;
    out->inputs = 1;
    out->input[0].usage = D9TEV_USAGE_COLOR;
    out->input[0].usage_index = 0;
    out->input[0].reg_index = 0;
    out->input[0].tev_source = D9TEV_SRC_PRIMARY;
    out->uses_vertex_color = 1;
    D9TevStage *stage = &out->stage[0];
    stage->func_rgb = stage->func_a = D9TEV_FUNC_MODULATE;
    stage->src_rgb[0] = stage->src_a[0] = D9TEV_SRC_TEX0;
    stage->op_rgb[0] = D9TEV_RGB_COLOR;
    stage->op_a[0] = D9TEV_A_ALPHA;
    stage->src_rgb[1] = stage->src_a[1] = D9TEV_SRC_PRIMARY;
    stage->op_rgb[1] = D9TEV_RGB_COLOR;
    stage->op_a[1] = D9TEV_A_ALPHA;
    stage->const_reg = 0xFF;
    return 0;
}

int d9_pipeline_build(const D9PipelineDescriptor *descriptor,
                      const D9PipelineShaderView *view,
                      const D9PipelineRequest *request,
                      const D9PipelineMatch *match, D9TevProgram *out,
                      char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!descriptor || !request || !match || !out || match->pipeline != descriptor) {
        pipeline_error(why, why_size, "invalid pipeline build arguments");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (descriptor->support == D9_PIPELINE_SUPPORT_PLANNED) {
        pipeline_error(why, why_size, "%s: planned descriptor has no emitted plan", descriptor->id);
        return -1;
    }
    D9PipelineMatch verified;
    int tried = d9_pipeline_try(descriptor, view, request, &verified, why, why_size);
    if (tried != 1) return -1;
    if (verified.signature != match->signature) {
        pipeline_error(why, why_size, "%s: match is stale or does not match the current input",
                       descriptor->id);
        return -1;
    }
    if (missing_preconditions(descriptor, request, why, why_size) < 0) return -2;
    if (descriptor->features.require_ldr &&
        !(request->quality & D9_PIPELINE_QUALITY_LDR)) {
        pipeline_error(why, why_size, "%s: LDR quality contract is required", descriptor->id);
        return -1;
    }
    switch (descriptor->matcher) {
    case D9_PIPELINE_MATCHER_BLIT_EXACT:
    case D9_PIPELINE_MATCHER_SIMPLE_TEV:
        if (!view || !view->code) {
            pipeline_error(why, why_size, "%s: missing source tokens", descriptor->id);
            return -1;
        }
        if (d9_tev_translate_pixel(view->code, view->words, out, why, why_size) < 0)
            return -1;
        if (match->signature != (descriptor->matcher == D9_PIPELINE_MATCHER_BLIT_EXACT
                                     ? D9_PIPELINE_SIGNATURE_COPY
                                     : D9_PIPELINE_SIGNATURE_SIMPLE_TEV)) {
            pipeline_error(why, why_size, "%s: match/signature mismatch", descriptor->id);
            return -1;
        }
        return 0;
    case D9_PIPELINE_MATCHER_MENU_LIGHT: {
        D9MenuLightMatch menu;
        char menu_why[256];
        if (!view || !d9_menu_light_match(view->code, view->words, &menu,
                                          menu_why, sizeof menu_why) ||
            menu.signature != descriptor->menu_signature ||
            match->signature != menu.signature) {
            pipeline_error(why, why_size, "%s: structural menu-light match changed", descriptor->id);
            return -1;
        }
        if (d9_menu_light_plan(&menu, out, why, why_size) < 0) return -1;
        if (out->stages != 5u || out->textures != 2u) {
            pipeline_error(why, why_size, "%s: unexpected pre-baked plan shape", descriptor->id);
            return -1;
        }
        return 0;
    }
    case D9_PIPELINE_MATCHER_SCALEFORM_GLYPH:
        if (view || request->source != D9_PIPELINE_SOURCE_FIXED_SCALEFORM ||
            request->material_kind != D9_PIPELINE_MATERIAL_SCALEFORM_GLYPH_ATLAS ||
            match->signature != D9_PIPELINE_SIGNATURE_SCALEFORM_GLYPH) {
            pipeline_error(why, why_size, "%s: glyph contract mismatch", descriptor->id);
            return -1;
        }
        return build_glyph(out);
    default:
        pipeline_error(why, why_size, "%s: no plan builder", descriptor->id);
        return -1;
    }
}
