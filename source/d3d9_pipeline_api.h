#pragma once
/* Data-driven specialised graphics-pipeline registry.
 *
 * This is a descriptor/matcher/plan library, not a renderer integration. It
 * deliberately does not edit or replace the D3D9 COM, TEV or core paths. A
 * caller must choose a family and satisfy the descriptor's quality and
 * preprocessing contracts before a plan can be built.
 *
 * Descriptor data is generated from tools/pipelines/registry.json. Acceptance
 * is structural (SM3 tokens + CTAB features) or an explicit fixed-material
 * contract; bytecode hashes are never acceptance keys.
 */

#include <stddef.h>
#include <stdint.h>
#include "d3d9_tev.h"

#define D9_PIPELINE_MAX_FAMILIES 8
#define D9_PIPELINE_MAX_DESCRIPTORS 24
#define D9_PIPELINE_MAX_PASSES 6
#define D9_PIPELINE_MAX_REQUIRED_INPUTS 8
#define D9_PIPELINE_MAX_REQUIRED_CONSTANTS 16
#define D9_PIPELINE_MAX_FORBIDDEN_CONSTANTS 48
#define D9_PIPELINE_MAX_DECLARATIONS 16
#define D9_PIPELINE_MAX_SAMPLERS 16
#define D9_PIPELINE_MAX_CONSTANT_NAMES 64
#define D9_PIPELINE_MAX_CONSTANT_NAME 64
#define D9_PIPELINE_MAX_INSTRUCTIONS 512
#define D9_PIPELINE_MAX_TEXT 192
#define D9_PIPELINE_OPCODE_WORDS 4 /* 256 opcode bits */

typedef enum {
    D9_PIPELINE_INTERFACE_TEXT = 0,
    D9_PIPELINE_SKY,
    D9_PIPELINE_LANDSCAPE,
    D9_PIPELINE_STATIC_GEOMETRY,
    D9_PIPELINE_CHARACTERS_ANIMATION,
    D9_PIPELINE_VEGETATION,
    D9_PIPELINE_WATER,
    D9_PIPELINE_PARTICLES,
    D9_PIPELINE_FAMILY_ANY = 0xFF
} D9PipelineFamily;

typedef enum {
    D9_PIPELINE_SUPPORT_STRUCTURAL = 1, /* matcher and host plan implemented */
    D9_PIPELINE_SUPPORT_PREPROCESS,     /* matcher/plan implemented, gated */
    D9_PIPELINE_SUPPORT_PLANNED         /* documentation only; always rejects */
} D9PipelineSupport;

typedef enum {
    D9_PIPELINE_SOURCE_FXP_PS3 = 1,
    D9_PIPELINE_SOURCE_FIXED_SCALEFORM,
    D9_PIPELINE_SOURCE_FIXED_MATERIAL
} D9PipelineSource;

typedef enum {
    D9_PIPELINE_MATCHER_NONE = 0,
    D9_PIPELINE_MATCHER_BLIT_EXACT,
    D9_PIPELINE_MATCHER_SIMPLE_TEV,
    D9_PIPELINE_MATCHER_MENU_LIGHT,
    D9_PIPELINE_MATCHER_SCALEFORM_GLYPH
} D9PipelineMatcher;

typedef enum {
    D9_PIPELINE_CONSTANTS_NONE = 0,
    D9_PIPELINE_CONSTANTS_FORBID,
    D9_PIPELINE_CONSTANTS_REQUIRE_ANY,
    D9_PIPELINE_CONSTANTS_REQUIRE_ALL
} D9PipelineConstantPolicy;

typedef enum {
    D9_PIPELINE_QUALITY_LDR = 1u << 0,
    D9_PIPELINE_QUALITY_DEGRADED = 1u << 1
} D9PipelineQuality;

/* Explicit preprocessing/runtime contracts. Missing bits cause build-time
 * rejection; they are never inferred from a package name or shader hash. */
enum {
    D9_PIPELINE_PRE_NORMAL_PREBAKED = 1u << 0,
    D9_PIPELINE_PRE_TANGENT_BASIS_PREBAKED = 1u << 1,
    D9_PIPELINE_PRE_AMBIENT_EMISSIVE_VARYING = 1u << 2,
    D9_PIPELINE_PRE_MATERIAL_ALPHA_C4 = 1u << 3,
    D9_PIPELINE_PRE_NORMALS_PREDECODED = 1u << 4,
    D9_PIPELINE_PRE_BONE_PALETTE_READY = 1u << 5,
    D9_PIPELINE_PRE_WIND_PARAMETERS_READY = 1u << 6,
    D9_PIPELINE_PRE_REFLECTION_TARGET_READY = 1u << 7,
    D9_PIPELINE_PRE_REFRACTION_TARGET_READY = 1u << 8,
    D9_PIPELINE_PRE_PARTICLE_BATCH_READY = 1u << 9,
    D9_PIPELINE_PRE_SPRITE_SWIZZLE_BAKED = 1u << 10
};

typedef enum {
    D9_PIPELINE_MATERIAL_NONE = 0,
    D9_PIPELINE_MATERIAL_SCALEFORM_GLYPH_ATLAS = 1,
    D9_PIPELINE_MATERIAL_STATIC_DIFFUSE_ONLY,
    D9_PIPELINE_MATERIAL_VEGETATION_ALPHA_TESTED,
    D9_PIPELINE_MATERIAL_EFFECT_BILLBOARD
} D9PipelineMaterialKind;

typedef struct {
    uint8_t usage;         /* D9TEV_USAGE_*; samplers use usage 0/position */
    uint8_t index;
    uint8_t reg;
    uint8_t sampler_type;  /* 0 for varyings, 2 for a 2D sampler */
} D9PipelineInputRequirement;

typedef struct {
    uint32_t version;
    uint16_t min_instructions, max_instructions;
    uint8_t min_textures, max_textures, max_samplers;
    uint8_t max_branches, max_loops;
    uint8_t max_tev_stages, max_tev_textures, max_color_inputs;
    uint8_t require_single_output, forbid_mrt, require_2d, require_ldr;
    D9PipelineConstantPolicy constant_policy;
    uint64_t allowed_opcodes[D9_PIPELINE_OPCODE_WORDS];
    uint16_t allowed_input_usages;
    uint8_t required_input_count;
    D9PipelineInputRequirement required_input[D9_PIPELINE_MAX_REQUIRED_INPUTS];
} D9PipelineFeatures;

typedef struct {
    const char *name;
    uint8_t stages, textures;
    const char *vertex_inputs;
    const char *texture_units;
    const char *blend;
    const char *depth;
    const char *alpha_test;
    const char *target;
    const char *summary;
} D9PipelinePass;

typedef struct {
    const char *id;
    D9PipelineFamily family;
    D9PipelineSupport support;
    D9PipelineSource source;
    uint8_t priority;
    D9PipelineMatcher matcher;
    int menu_signature;
    D9PipelineMaterialKind fixed_material;
    uint32_t precondition_mask;
    const char *inventory_signature;
    const char *accepted_original;
    const char *package_rule;
    const char *vertex_inputs;
    const char *texture_units;
    const char *quality_reductions;
    const char *unsupported;
    const char *fallback;
    const char *fallback_id;
    const char *memory;
    const char *preconditions;
    const char *const *required_constants;
    const char *const *forbidden_constants;
    D9PipelineFeatures features;
    uint8_t pass_count;
    D9PipelinePass passes[D9_PIPELINE_MAX_PASSES];
} D9PipelineDescriptor;

typedef struct {
    const char *name;
    const char *title;
    const char *default_fallback;
    const char *inventory_note;
} D9PipelineFamilyInfo;

typedef struct {
    uint32_t schema_version;
    size_t family_count, descriptor_count;
    const D9PipelineFamilyInfo *families;
    const D9PipelineDescriptor *descriptors;
} D9PipelineRegistry;

/* Defined by the generated source/d3d9_pipeline_registry.c. */
extern const D9PipelineRegistry d9_pipeline_registry_data;

/* Bounded structural view of one ps_3_0 chunk. Comments/CTAB are decoded, not
 * skipped blindly. Overflow or malformed input is an error and fails closed.
 * code/words are borrowed for structural matchers; the caller must keep the
 * token stream alive through try/build. The library never stores it globally. */
typedef struct {
    const uint32_t *code;
    size_t words;
    uint32_t version;
    unsigned instructions, textures, samplers, branches, loops;
    unsigned declaration_count, constant_count;
    uint8_t input_usage[D9_PIPELINE_MAX_DECLARATIONS];
    uint8_t input_usage_index[D9_PIPELINE_MAX_DECLARATIONS];
    uint8_t input_reg[D9_PIPELINE_MAX_DECLARATIONS];
    uint8_t sampler_type[D9_PIPELINE_MAX_SAMPLERS];
    uint8_t sampler_declared[D9_PIPELINE_MAX_SAMPLERS];
    uint32_t sampler_used;
    uint8_t color_input_count;
    uint8_t single_output, has_color_output, has_mrt, has_depth_output;
    uint8_t unknown_opcode, ctab_truncated, malformed;
    uint8_t has_relative_addressing, malformed_operand;
    uint64_t opcodes[D9_PIPELINE_OPCODE_WORDS];
    char constant_names[D9_PIPELINE_MAX_CONSTANT_NAMES]
                       [D9_PIPELINE_MAX_CONSTANT_NAME];
    char error[D9_PIPELINE_MAX_TEXT];
} D9PipelineShaderView;

typedef struct {
    D9PipelineSource source;
    D9PipelineMaterialKind material_kind;
    uint32_t quality;
    uint32_t preconditions;
    uint32_t meta;
    const char *package_path; /* provenance only; never sufficient to match */
} D9PipelineRequest;

enum {
    D9_PIPELINE_SIGNATURE_COPY = 0x100,
    D9_PIPELINE_SIGNATURE_SIMPLE_TEV,
    D9_PIPELINE_SIGNATURE_SCALEFORM_GLYPH
};

typedef struct {
    const D9PipelineDescriptor *pipeline;
    int signature;
    unsigned instructions;
    char detail[D9_PIPELINE_MAX_TEXT];
} D9PipelineMatch;

const D9PipelineRegistry *d9_pipeline_registry(void);
size_t d9_pipeline_descriptor_count(void);
const D9PipelineDescriptor *d9_pipeline_find(const char *id);
const char *d9_pipeline_family_name(D9PipelineFamily family);
const char *d9_pipeline_support_name(D9PipelineSupport support);
const char *d9_pipeline_precondition_name(uint32_t bit);

/* Validate all generated descriptor invariants. Returns 0 or -1 with a named
 * reason. This is called by the tests and real-inventory measurement tool. */
int d9_pipeline_registry_validate(const D9PipelineRegistry *registry,
                                  char *why, size_t why_size);

/* Decode a complete ps_3_0 token stream, including its first CTAB and dcl
 * declarations. Returns 0 or -1. The input is never retained. */
int d9_pipeline_parse_ps3(const uint32_t *code, size_t words,
                          D9PipelineShaderView *out,
                          char *why, size_t why_size);

/* Try one descriptor. 1 = structural match, 0 = no match, -1 = invalid input.
 * A planned descriptor always returns 0. Preconditions gate build, not the
 * structural coverage measurement. */
int d9_pipeline_try(const D9PipelineDescriptor *descriptor,
                    const D9PipelineShaderView *view,
                    const D9PipelineRequest *request,
                    D9PipelineMatch *out, char *why, size_t why_size);

/* Select the highest-priority implemented structural match in a family, or in
 * the whole registry with D9_PIPELINE_FAMILY_ANY. No fallback is automatic. */
const D9PipelineDescriptor *d9_pipeline_select(
    D9PipelineFamily family, const D9PipelineShaderView *view,
    const D9PipelineRequest *request, D9PipelineMatch *out,
    char *why, size_t why_size);

/* Emit a host-side D9TevProgram plan. This does not upload anything or render.
 * 0 = plan built, -1 = unsupported/invalid, -2 = named precondition missing. */
int d9_pipeline_build(const D9PipelineDescriptor *descriptor,
                      const D9PipelineShaderView *view,
                      const D9PipelineRequest *request,
                      const D9PipelineMatch *match, D9TevProgram *out,
                      char *why, size_t why_size);
