# PIPELINES — specialised Skyrim graphics-pipeline registry

## Status and scope

This directory pair adds a **data-driven descriptor, structural-matcher and
host-plan library** for specialised Skyrim rendering families. It does not
attempt universal SM3 translation.

Implemented in this preparation step:

- interface/text: exact copy, restricted unlit `.fxp` utility, fixed Scaleform
  straight-alpha glyph contract;
- static geometry: the two measured base-lit menu/logo signatures, with an
  explicit preprocessing gate;
- structural validation, synthetic host tests, and a real-chunk coverage
  measurement over the base archive;
- documented descriptors for sky, landscape, characters/animation, vegetation,
  water and particles. Those six families remain **planned**, not accepted.

**No rendering is complete.** The registry is not wired into `CreatePixelShader`,
D3D9 state emission, citro3d upload, vertex processing, asset preprocessing or a
menu/world frame. Existing `d3d9_tev`, `d3d9_shader`, `d3d9_com` and core files
were not edited, and no shared build list was changed.

## Files

| File | Role |
|---|---|
| `tools/pipelines/registry.json` | Editable source of truth for all family/descriptor requirements |
| `tools/pipelines/generate_registry.py` | Validates JSON and generates the C descriptor table |
| `source/d3d9_pipeline_api.h` | Public registry, view, request, match and plan API |
| `source/d3d9_pipeline_match.c` | Bounded SM3/CTAB decoder, feature checks, structural matchers, plan builders |
| `source/d3d9_pipeline_registry.c` | Generated static descriptor data |
| `tools/pipelines/inventory.py` | Read-only retail measurement and temporary chunk-dump writer |
| `tools/pipelines/coverage_base.json` | Derived base-archive coverage and real chunk metadata |
| `tests/pipeline_registry_smoke.c` | Synthetic structural/plan/fallback tests |
| `tests/pipeline_real.c` | Optional real-chunk measurement driver |
| `tests/pipeline_test.py` | Registry schema, generated-data and coverage tests |
| `tools/pipelines/run.ps1` | Standalone build/test runner; does not alter shared build lists |

Game bytecode is never embedded or committed. The real measurement writes a
temporary local dump under the approved temp directory, runs the C matcher, and
deletes the dump. Coverage stores derived package/meta/offset/trailer metadata
and counts. Instruction-stream hashes are used only for deduplication, never for
acceptance.

## Registry model

A descriptor declares:

- family, support level, source kind and priority;
- original inventory signature/features and package provenance;
- required/forbidden CTAB vocabulary, declarations, opcode/instruction/texture
  limits, output constraints and LDR contract;
- vertex inputs and texture units;
- one or more passes, with TEV stage/texture budgets and blend/depth/alpha-test
  intent;
- permitted quality reductions, unsupported behavior, explicit fallback,
  preprocessing contracts and memory/working-set implications.

Support levels are intentionally conservative:

| Support | Meaning |
|---|---|
| `structural` | Matcher and host-side `D9TevProgram` plan are implemented and tested |
| `preprocess` | Matcher/plan exist, but named asset/runtime preconditions must be supplied |
| `planned` | Documentation only; `try` never accepts and `build` never emits a plan |

Selection chooses the lowest-priority implemented structural match in the
requested family. `D9_PIPELINE_FAMILY_ANY` is available for inventory measurement.
A failed match does **not** trigger a fallback. The caller must explicitly select
a fallback descriptor and request degraded quality; the only registered fallback
so far, `static.diffuse_fallback`, is still planned and therefore rejects.

### API sketch

```c
D9PipelineShaderView view;
D9PipelineRequest request = {0};
D9PipelineMatch match;
D9TevProgram plan;
char why[256];

request.source = D9_PIPELINE_SOURCE_FXP_PS3;
request.quality = D9_PIPELINE_QUALITY_LDR;
/* Static plans additionally need all four D9_PIPELINE_PRE_* bits. */

if (d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) == 0) {
    const D9PipelineDescriptor *p = d9_pipeline_select(
        D9_PIPELINE_STATIC_GEOMETRY, &view, &request, &match, why, sizeof why);
    if (p && d9_pipeline_build(p, &view, &request, &match, &plan,
                               why, sizeof why) == 0) {
        /* Host plan only. Backend upload and draw submission do not exist yet. */
    }
}
```

The view borrows the token pointer for matching; callers must keep it alive
through `try`/`build`. Parsing is bounded and fails closed on truncated comments,
CTAB overflow, missing `END`, trailing words, malformed register operands,
relative addressing, unsupported declaration capacity or malformed instructions.
The bounded view is larger than 4 KiB because it stores up to 64 CTAB names;
parse one chunk at a time and do not put an array of views on the 3DS stack.
Descriptor strings/tables are static storage; the matcher and plan builder use
no heap allocation.

## Measured coverage by family

Source: `Skyrim - Shaders.bsa`, the **122-package / 10,475-chunk base archive**
used by `docs/SHADERS.md`. PS/VS split is 9,621/854, with 6,655 distinct
instruction streams. `Update.bsa` is not included in this table.

“Selected” means one highest-priority implemented structural match. It is not a
percentage and not a rendering claim.

| Pipeline family | Inventory reference | Descriptor support | Selected PS chunks | Distinct selected streams |
|---|---|---|---:|---:|
| Interface/text | `ui/interface` 314; `blit/copy` 11; cross-family imagespace/effect utilities | 3 structural | 17 | 6 |
| Sky | `sky` 10 PS / 7 streams | planned | 0 | 0 |
| Landscape | `terrain/landscape` 327 PS / 247 streams | planned | 0 | 0 |
| Static geometry | semantic subset of `lighting/deferred/shadows` (7,175 chunks) | 2 preprocess + 1 planned fallback | 6 | 2 |
| Characters/animation | `skinning/characters` 2,162 / 1,194 streams | planned | 0 | 0 |
| Vegetation | `grass/trees` **0 shipped chunks** | planned material contract | 0 | 0 |
| Water | `water` 72 PS / 38 streams | planned | 0 | 0 |
| Particles | `particles/effects` 284 / 167 streams; `BSEffectShaderProperty` has no `.fxp` | 2 planned | 0 | 0 |

Interface/text's 17 selected chunks are deliberately cross-family:

| Descriptor | Selected chunks | Source families |
|---|---:|---|
| `ui.copy` | 7 | 4 `blit/copy`, 3 `post-process/imagespace` |
| `ui.simple_fxp` | 10 | 4 `blit/copy`, 5 `particles/effects`, 1 `post-process/imagespace` |
| `ui.scaleform_glyph` | 0 from `.fxp` | fixed Scaleform contract; validated synthetically |

No `ui/interface`-classified PS chunk is silently claimed as accepted. The
family's real menu text path is Scaleform and has no shipped `.fxp`.

Static geometry's six matches are five `p5_MAD_NORMAL` chunks and one
`p1200005_ADD_NORMAL` chunk. They are **not** coverage of all 7,175 lighting
chunks. All six have 78-byte reflection trailers; the measured bytecode sizes
are 840 and 852 bytes. See `coverage_base.json` for package/meta/offset examples.

## Family contracts

### 1. Interface/text

Accepted original signatures:

- `ui.copy`: exactly `dcl_texcoord t0.xy`, `dcl_2d s0`,
  `texld oC0,t0,s0`; registers, masks, swizzles and sampler type are checked.
  Seven base chunks match, with CTAB `Image`, `HeightMap` or `BaseSampler`.
- `ui.simple_fxp`: the existing straight-line TEV subset
  (`nop/mov/add/sub/mad/mul/dp3/lrp/texld/dcl/def`), restricted to texcoord/color
  PS inputs, no lighting/water/sky/terrain/character/shadow vocabulary, at most
  three textures and six stages. Ten base utility chunks match after exact
  copies are assigned to `ui.copy`.
- `ui.scaleform_glyph`: no bytecode signature exists. The explicit contract is
  clip position + texcoord0 + color0 and one straight-alpha LDR glyph atlas.

Inputs/units:

- copy: position, texcoord0; one 2D texture; one `REPLACE` stage;
- unlit utility: declared texcoord/color inputs, up to three sampled 2D units,
  one translated pass of up to six stages;
- glyph: position, texcoord0, color0; atlas in unit 0; one `MODULATE` stage for
  RGB/A, src-alpha blending, depth test without write.

Reductions: LDR/RGBA8 TEV, no HDR, no SDF/outline/subpixel/colour glyphs, no
arbitrary swizzles. Unsupported: branches, `rcp/rsq/nrm/cmp/dp4`, `texkill`, cube
or volume sampling, MRT/depth output and more than three live PS input vectors.
Fallback: reject with a named mismatch; do not substitute a world material.

Working set: one to three textures and at most six stage descriptors. Glyph
atlases must remain resident while their batch is alive; quad vertices are
caller-owned. Original SM3/CTAB data need not remain resident after planning.

### 2. Sky

Observed: 10 PS, 7 streams, `PParams`/colour-range/sun vocabulary, 1-2 samples,
no branches. The current translator accepts only two; `frc` and multi-temp
atmospheric programs remain unsupported.

Required inputs: clip position and view direction; optional cloud UV. Units:
optional gradient/cloud texture, at most one resident unit after baking.

Planned passes: `sky.base` (analytic LDR gradient + sun/moon disc, up to four
stages) and optional `sky.clouds` (one pre-baked cloud texture, two stages).
Reductions: drop procedural `frc` noise, multi-temp scattering, HDR and dynamic
weather branching. Unsupported: cube/volume sampling and multiple live cloud
layers. Fallback: clear to the supplied horizon/fog colour and skip the sky draw.
Working set: at most one small LDR texture; no full-screen target.

### 3. Landscape

Observed: 327 PS, 247 streams, 11 packages,
`MultiTextureDiffuse0..5Sampler` / `MultiTextureNormal0..5Sampler`, optional LOD,
height and world-map overlays. Most chunks use 13-15 samples.

Required inputs: position, normal, texcoord0, optional vertex colour. Original
units: six diffuse + six normal layers plus overlays. PICA can retain only three
units per pass.

Planned passes with offline-decoded normals:

1. layers 0-2: three textures, up to six stages, opaque accumulation;
2. layers 3-5: three textures, depth-equal/no-write;
3. optional LOD/height/map overlay: up to three textures and six stages.

Without offline normal decode, the faithful layer path needs four layer passes
plus the overlay pass. Reductions: six near diffuse layers, 1-2 baked far layers,
no parallax/live LOD noise/map overlay. Unsupported: single-pass 12+ samples and
HDR blending. Fallback: explicit baked far tile or reject; never silently drop
layers. Working set: stream three textures per pass; baked far tiles replace the
six near layers.

### 4. Static geometry

The first accepted subset is the measured base-lit logo class, not a universal
lighting permutation:

- `static.p5_mad_normal`: 24 instructions, 840 bytes,
  `normal.xyz = normal.xzyw * 2 - 1`; five chunks;
- `static.p1200005_add_normal`: 25 instructions, 852 bytes,
  `normal.xyz = (normal.xzyw - 0.5) * 2`; one chunk.

Both require texcoord0/v0, color0/v1, color1/v2, 2D s0/s1 and the eight-name
base-lit CTAB (`DiffuseSampler`, `NormalSampler`, `DirLightColor`,
`DirLightDirection`, `DirectionalAmbient`, `EmitColor`, `FogColor`,
`MaterialData`). Every opcode, register, mask, swizzle, modifier and `def c8`
literal is matched structurally. `FogColor` remains in the original CTAB
vocabulary, but the fixed plan does not blend toward its RGB components.

Inputs/units: position, texcoord0, color0, color1; diffuse in unit 0 and
pre-decoded/pre-swizzled normal in unit 1. The existing five-stage prototype is:

1. `dot3(pre-baked normal, c3)` → saturated NdotL;
2. NdotL × light colour + ambient/emissive varying;
3. diffuse × light; alpha = diffuse.a × vertex.a;
4. RGB × vertex RGB; alpha × material alpha;
5. RGB × `c7.a` fade modulation; alpha passthrough. `c7.rgb` is ignored.

The plan is gated by `normal_prebaked`, `tangent_basis_prebaked`,
`ambient_emissive_varying` and `material_alpha_c4`. Retail `dp4` basis transforms,
`xzyw` decode and multiple live pixel temporaries are not emitted directly.

Reductions: one directional light, LDR, pre-baked tangent basis, folded
ambient/emissive, and one final `c7.a` fade-modulation stage. `c7.rgb` is ignored
and alpha passes through. Unsupported: shadows, point lights, envmap, specular,
subsurface/backlight, projected noise, parallax, HDR and skinning.
Fallback: `static.diffuse_fallback` only by explicit degraded request, and only
once that planned one-texture pipeline is implemented.

Working set: two textures, three constant vectors (`c3/c4/c7`, with only `c7.a`
consumed), two colour varyings and five stages. Preprocessed normals match
diffuse dimensions; tangent basis data is per vertex.

### 5. Characters/animation

Observed: 2,162 chunks (1,872 PS, 290 VS), 1,194 streams, 18 packages;
blendweight+blendindices VS signatures and membrane/facegen/tint-mask CTAB names.

Required inputs: position, normal, tangent/binormal as needed, blendweight,
blendindices, texcoord0, color0. Bone matrices/light/fog must fit the 96 VS float
uniform registers. Units: diffuse, normal and one detail/mask texture; the 16
tint colours are constants, not textures.

Planned passes: `character.skin` (four-bone vertex skinning + specialised 2-4
stage PS, up to three units) and optional `character.detail` (facegen/tint,
two units, depth-equal). Reductions: four influences, one directional light,
constant tint palette, no subsurface/point/envmap detail, hardware alpha test.
Unsupported: more than four influences, morph targets, dynamic branching, HDR.
Fallback: reject; a frozen diffuse pose would be a separate explicit debug mode.
Working set: matrix palette + animation buffers + at most three textures.

### 6. Vegetation

Observed bytecode family: **zero chunks**. Grass/foliage rides `lighting/059`
with alpha test and vertex colour. Selection must come from mesh/material
metadata, not CTAB guessing or a fabricated hash family.

Required inputs: position, normal, texcoord0, color0, wind parameters. Units:
diffuse/alpha and optional pre-decoded normal. Planned single pass: texture ×
vertex colour plus optional cheap normal lighting, at most three stages, hardware
alpha test, depth write. Reductions: no normal/shadows/specular, two-level wind.
Unsupported: alpha-blended depth-sorted foliage and dense grass without
culling/instancing. Fallback: cull or reject. Working set: one or two foliage
textures, small wind uniforms and caller-owned instance/vertex buffers.

### 7. Water

Observed: 72 PS, 38 streams, 24 branch-bearing chunks, usually 4-7 samples;
water/reflection/refraction/normal/displacement vocabulary.

Required inputs: position, normal, texcoord0, view/world position. Original
units include normal layers, reflection, refraction and optional cube/height
maps. Planned surface pass retains reflection + refraction + one packed normal
layer.

Planned passes:

1. render mirrored scene to reflection colour/depth;
2. render scene behind the surface to refraction colour/depth;
3. water surface: distortion, Fresnel blend and depth tint, up to six stages.

Reductions: one normal layer, no displacement, static/lower-resolution
reflection, simplified Fresnel. Unsupported: loops, HDR, more than three surface
textures and unprefiltered cube lookup. Fallback: explicit opaque tinted
depth-only surface or reject; never show sky through missing targets.

Working set is target-dominated: two `W*H*4` colour targets plus depth bytes,
three surface textures and saved scene state.

### 8. Particles

Observed: 284 chunks (172 PS, 112 VS), 167 streams. Most effect PS use 6-7
samples and 160 use non-uniform `.zwz` sprite swizzles. `BSEffectShaderProperty`,
including the measured menu fog mesh, has no `.fxp`.

Required inputs: centre/size or camera-facing quad position, texcoord0, color0,
rotation and optional animation parameters. Fixed path units: effect texture and
optional mask. Planned pass: texture × vertex colour with alpha or additive
blending, depth test/no write, at most three stages.

Faithful seven-sample `.fxp` effects need at least three passes. Reductions:
bake sprite swizzles offline, collapse flare/blood/sparkle terms, drop HDR and
soft depth. Unsupported: single-pass seven samples, unsorted alpha and mesh
particles without independent transforms. Fallback: explicit conversion to
`particles.billboard` or reject/omit the batch.

Working set: fixed path uses one or two textures and a packed dynamic quad
buffer; CPU sorting/instance buffers scale with live particle count.

## Validation and tests

Run the standalone suite:

```powershell
powershell -File tools\pipelines\run.ps1
```

It builds `tests/pipeline_registry_smoke.c` with the new registry plus the
existing read-only TEV/menu-light modules, checks generated-data synchronization,
and runs the Python schema/coverage tests. Current synthetic result: **124 checks,
0 failures**.

Coverage includes:

- eight families and complete descriptor documentation;
- unique IDs, resolved same-family fallbacks, pass budgets ≤6 stages/≤3 textures;
- bounded declarations/constants and required-vs-forbidden vocabulary checks;
- exact copy register/mask/swizzle/sampler rejection;
- unlit opcode/dataflow rejection;
- fixed glyph material validation and plan arithmetic;
- both static signatures, literal/register/CTAB mutation rejection and named
  preprocessing failures;
- planned descriptors never accepting or emitting plans;
- no automatic fallback and no hash-based acceptance.

Optional real measurement:

```powershell
powershell -File tools\pipelines\run.ps1 -RealInventory
# Refresh the committed derived snapshot explicitly:
powershell -File tools\pipelines\run.ps1 -RealInventory -UpdateCoverage
```

The real run parses all 9,621 PS chunks with **zero parse failures** and selects
17 interface/text utility chunks plus six static base-lit chunks. It does not
claim those draws have reached a GPU.

## Extending the registry

1. Add or edit a descriptor in `tools/pipelines/registry.json`.
2. For a new structural behavior, add a matcher enum/handler in
   `source/d3d9_pipeline_match.c`; descriptive-only work needs `matcher: none` and
   `support: planned`.
3. Regenerate with `tools/pipelines/generate_registry.py`.
4. Add synthetic token/CTAB fixtures in `tests/pipeline_registry_smoke.c` and
   schema/coverage checks in `tests/pipeline_test.py`.
5. Run `tools/pipelines/run.ps1`, then optionally remeasure real chunks.

Do not add bytecode hashes as acceptance data. Do not convert a planned family
to `structural` until its matcher, plan, preconditions and tests exist.

## Next steps before a first real menu/world frame

- implement asset-side normal/tangent preparation and the static preprocessing
  contracts;
- add a vertex-stage plan for glyph quads and static varyings;
- integrate explicit family selection into the material/COM path without
  silently changing existing shader acceptance;
- implement citro3d TEV/texture/blend/depth upload and draw submission;
- implement the Scaleform glyph atlas source and UI batching;
- implement the planned fixed particle fog pass needed by the measured menu mesh;
- add frame-level tests and only then report menu/world rendering status.
