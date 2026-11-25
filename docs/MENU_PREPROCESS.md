# MENU_PREPROCESS — offline contracts for specialised menu shaders

Status: **host preprocessing and validation only**. Nothing in this document is
a claim of rendering, runtime material loading, COM integration or PICA
integration. The runtime files (`source/`, including core/COM/PICA and
`d3d9_menu_light.c`) were not changed by this work.

The new tool is `tools/menu_preprocess/`. It consumes DDS/JSON and two small
host-only binary vertex fixtures, writes derived DDS/JSON artefacts, and fails
with named errors instead of guessing. It never writes into a game install.
The existing registry entries in `tools/pipelines/registry.json` remain the
pipeline-selection vocabulary; this tool validates data against them.

## 1. Commands

Run from the repository root:

```powershell
.\.venv\Scripts\python.exe tests\menu_preprocess_test.py

.\.venv\Scripts\python.exe -m tools.menu_preprocess normal `
  source_normal.dds --signature p5_MAD_NORMAL `
  --output build\menu\logo02_n.normal.dds `
  --report build\menu\logo02_n.normal.json

.\.venv\Scripts\python.exe -m tools.menu_preprocess material `
  build\menu\material.json --output build\menu\material.validated.json

.\.venv\Scripts\python.exe -m tools.menu_preprocess glyph `
  build\menu\glyph.json --output build\menu\glyph.validated.json

# Optional, read-only retail sample; derived files go outside Data:
.\.venv\Scripts\python.exe -m tools.menu_preprocess sample `
  --data E:\SteamLibrary\steamapps\common\Skyrim\Data `
  --out build_host\menu_preprocess_sample
```

Exit code `0` means validation passed; `2` means a named validation, budget or
I/O failure. Normal-map failures write a `status: "fail"` report when
`--report` is supplied and do **not** commit an output DDS. Successful outputs
are committed atomically. Input, output and report paths must be distinct.

Every command accepts `--max-input-bytes`, `--max-memory-bytes`,
`--max-json-bytes`, `--max-output-bytes`, `--max-pixels` and
`--max-vertex-records`. Defaults are listed in §6.

## 2. Menu-light normal map

### 2.1 Retail contract

Both accepted signatures sample `s1` as RGBA and apply the SM3 swizzle `xzyw`:

| Signature | Instructions / bytecode | Decode |
|---|---:|---|
| `p5_MAD_NORMAL` | 24 / 840 B | `N.xyz = normal.xzyw * 2 - 1` |
| `p1200005_ADD_NORMAL` | 25 / 852 B | `N.xyz = (normal.xzyw - 0.5) * 2` |

The literal `c8` is respectively `{2,-1,1,0}` and `{-0.5,1,0,0}`. Therefore the
physical source channels are:

```
source R -> normal X
source A -> normal Y
source G -> normal Z
source B -> unused by the retail swizzle
```

`source/d3d9_menu_light.c` remains the authoritative **full structural**
bytecode matcher. `probe_bytecode()` in the host tool is a partial inventory
probe (version, count, `c8`, declarations and the decode sequence); it never
accepts a shader for runtime use.

For 8-bit UNORM input, the two formulas are equivalent after round-to-nearest
UNORM8 output encoding. They are not claimed equivalent for arbitrary floating
point or signed source data.

### 2.2 Validation and conversion

Supported DDS sources are 2D, linear, UNORM/LDR formats understood by
`tools/skyrim/dds.py`. SNORM and sRGB inputs are rejected explicitly. Every
complete stored mip is decoded, transformed and written; missing mips are not
silently generated.

Default layout detection classifies a clearly opaque A channel (mean at least
224/255) as conventional `rgba_xyzw` and rejects it because retail requires
`rgba_xzyw`. A conventional map can only be converted deliberately:

```powershell
--source-layout rgba_xyzw --allow-layout-conversion
```

Hard data checks include:

* declared mip chain completeness and dimensions;
* enough variation overall to reject an entirely constant/degenerate normal
  map (an individual constant axis is reported as a warning, because flat
  axis-aligned normals are valid);
* input/output/memory budgets;
* finite, non-degenerate decoded vectors within the configured zero-vector
  fraction limit;
* optional strict statistical range gate.

`--strict-range` fails when the decoded mean vector length exceeds `0.5`.
Without it, a strong directional bias is a **warning**, not a failure. This is
intentional: the real `Logo02_n.dds` has a mean decoded vector length of about
`0.716` because source A is heavily biased toward zero. Rejecting that asset
with a generic “normal maps must be centred” rule would contradict the retail
shader.

Detection limits are explicit in the report: statistics can catch an opaque
coverage channel, constant/wrong-range data and gross bias, but cannot prove
arbitrary internal channel swaps or semantic axis labels. Source provenance is
still required.

### 2.3 Output DDS

The output is a legacy 128-byte-header DDS containing linear RGBA8:

```
R = decoded normal X
G = decoded normal Y
B = decoded normal Z
A = 255 (opaque; source A was consumed as Y, not coverage)
```

Rows are top-down, little-endian, with the same dimensions and complete stored
mip count as the source. No `xzyw` swizzle or signed decode remains for the
future loader. Mip levels are transformed independently from their source
levels; the tool does not re-filter normals.

### 2.4 Normal manifest

`kind` is `menu_light_normal`, `schema_version` is `1`, and `status` is `pass`.
Important fields:

```jsonc
{
  "schema_version": 1,
  "kind": "menu_light_normal",
  "status": "pass",
  "signature": {
    "name": "p5_MAD_NORMAL",
    "c8": [2.0, -1.0, 1.0, 0.0],
    "formula": "normal.xyz = normal.xzyw * 2 - 1",
    "source_channels": {"x": "r", "y": "a", "z": "g"},
    "output_channels": {"x": "r", "y": "g", "z": "b", "a": "opaque_255"}
  },
  "source": {"name": "...", "sha256": "...", "width": 512, "height": 1024},
  "decode": {
    "source_layout": "rgba_xzyw",
    "layout_conversion": false,
    "output_kind": "rgba8_xyz_opaque",
    "quantization": "UNORM8 round-to-nearest, ties upward",
    "mip_policy": "transform every complete stored source mip; no generated mips"
  },
  "validation": {
    "errors": [],
    "warnings": [],
    "channel_order": {"selected": "rgba_xzyw", "alpha_mean": 36.221321},
    "source_channel_stats_top_mip": {"r": {}, "g": {}, "b": {}, "a": {}},
    "decoded_normal_stats_top_mip": {
      "mean_xyz": [], "rms_xyz": [], "min_xyz": [], "max_xyz": [],
      "mean_vector_length": 0.715945294, "zero_vector_fraction": 0.0
    }
  },
  "output": {
    "file": "logo02_n.normal.dds",       // basename, relative to this manifest
    "sha256": "...",
    "bytes": 2796332,
    "width": 512,
    "height": 1024,
    "mip_count": 11,
    "dds_format": "RGBA8",
    "kind": "rgba8_xyz_opaque",
    "channel_order": "xyz",
    "alpha": 255,
    "linear": true,
    "already_decoded": true,
    "levels": [{"level": 0, "width": 512, "height": 1024, "bytes": 2097152}]
  },
  "memory": {"peak_bytes": 4959056, "peak_live": {}, "limit_bytes": 16777216},
  "runtime_integrated": false
}
```

JSON is UTF-8, sorted by key and rejects duplicate keys/non-finite numbers.
Hashes are SHA-256. A future loader must re-check the DDS header, dimensions,
mip count and hash rather than trusting the manifest alone.

## 3. Tangent basis and c0..c7 material contract

### 3.1 Required constants

The source program uses `c0..c7`; `c8` belongs to the normal signature and must
not be supplied as material data.

| Register | Required meaning | Offline validation |
|---|---|---|
| `c0` | tangent basis row | unit XYZ, orthogonal, `w=0` |
| `c1` | bitangent basis row | unit XYZ, orthogonal, `w=0` |
| `c2` | normal basis row | unit XYZ, orthogonal, `w=0` |
| `c3` | directional light direction | unit XYZ; all components `[0,1]` for RGBA8 TEV |
| `c4` | directional light colour | LDR RGBA `[0,1]`; output alpha is replaced by `c5.z` |
| `c5` | material data | `c5.z` is the material-alpha scale `[0,1]`; other components are unused by this signature |
| `c6` | emissive colour | LDR RGB `[0,1]` |
| `c7` | fog/output tint | LDR RGBA `[0,1]`; `c7.a` is the fog factor |

The three basis rows must form a right-handed orthonormal matrix
(`bitangent = normal x tangent`, determinant `+1`, tolerance `2e-4`). This is
the same row-dot algebra used by the three retail `dp4` instructions. The rows
and `c3` must use the same lighting space; the vertex normal/tangent/bitangent
records validated against them must be supplied in that space as well.

### 3.2 CTAB conflict is explicit, not guessed

The measured retail CTAB binds:

```
DiffuseSampler       set 3 index 0     NormalSampler       set 3 index 1
DirLightDirection    set 2 index 3     DirLightColor       set 2 index 4
MaterialData         set 2 index 5     EmitColor           set 2 index 6
FogColor             set 2 index 7     DirectionalAmbient  set 2 index 0
```

`DirectionalAmbient` therefore has a measured `c0` binding, while the accepted
pixel program consumes `c0` as a tangent-basis row. The specialised offline
contract resolves this deliberately:

* `c0..c2` must contain basis rows, never ambient colour;
* `DirectionalAmbient` is retained as required registry vocabulary with
  `disposition: "unused_shadowed_by_basis"`;
* ambient RGB is supplied separately as `ambient_rgb` and folded into
  `color1.rgb` together with `c6.rgb`.

A descriptor that tries to use ambient data as a basis row fails the unit or
orthogonality check. The tool does not pretend the CTAB name proves a usable PS
constant value.

### 3.3 Vertex inputs

The future draw declaration is:

| Input | Contract |
|---|---|
| position | `float4`, clip space |
| texcoord0 | `float2` |
| color0 | RGBA8 UNORM vertex tint, including alpha |
| color1 | RGBA8 UNORM; RGB is ambient + emissive, alpha remains the secondary-colour blend factor |
| normal | `float3`, lighting space |
| tangent | `float3`, lighting space |
| bitangent | stored `float3`, lighting space; not silently recomputed |

For one pre-baked normal texture, this contract requires a **constant**
right-handed basis. Every record in the optional `MVB1` validation stream must
match `c0..c2`. Varying per-vertex bases are rejected: one tangent-space texture
cannot represent a different basis transform at every vertex without a
different runtime path.

`MVB1` is a host validation fixture, not a guest vertex format:

```
offset  size  field
0       4     magic "MVB1"
4       4     u32 version = 1
8       4     u32 record count (>0, <= max_vertex_records)
12      4     u32 record bytes = 36
16      36*n  records, little-endian float32:
                 tangent.xyz, bitangent.xyz, normal.xyz
```

The file size must be exactly `16 + 36*n`; records are streamed in bounded
chunks.

### 3.4 Material descriptor example

Paths are relative to the descriptor. The normal manifest is the output from
§2.4; the diffuse DDS must have identical dimensions.

```json
{
  "schema_version": 1,
  "pipeline": "static.p5_mad_normal",
  "signature": "p5_MAD_NORMAL",
  "constants": {
    "c0": [1.0, 0.0, 0.0, 0.0],
    "c1": [0.0, 1.0, 0.0, 0.0],
    "c2": [0.0, 0.0, 1.0, 0.0],
    "c3": [0.0, 0.0, 1.0, 0.0],
    "c4": [1.0, 1.0, 1.0, 1.0],
    "c5": [0.0, 0.0, 0.5, 0.0],
    "c6": [0.1, 0.2, 0.3, 0.0],
    "c7": [1.0, 1.0, 1.0, 0.25]
  },
  "ctab": {
    "DiffuseSampler": {"set": 3, "index": 0},
    "NormalSampler": {"set": 3, "index": 1},
    "DirLightDirection": {"set": 2, "index": 3},
    "DirLightColor": {"set": 2, "index": 4},
    "MaterialData": {"set": 2, "index": 5},
    "EmitColor": {"set": 2, "index": 6},
    "FogColor": {"set": 2, "index": 7},
    "DirectionalAmbient": {"set": 2, "index": 0,
                           "disposition": "unused_shadowed_by_basis"}
  },
  "ambient_rgb": [0.05, 0.1, 0.15],
  "normal_map": "logo02_n.normal.json",
  "diffuse_texture": "logo02_d.dds",
  "vertex_declaration": {
    "position": {"components": 4, "space": "clip"},
    "texcoord0": {"components": 2},
    "color0": {"components": 4, "encoding": "rgba8_unorm"},
    "color1": {"components": 4, "encoding": "rgba8_unorm",
               "rgb": "ambient_plus_emissive"},
    "normal": {"components": 3, "space": "lighting"},
    "tangent": {"components": 3, "space": "lighting"},
    "bitangent": {"components": 3, "space": "lighting", "source": "stored"}
  },
  "basis": {"scope": "constant", "handedness": "right", "records": "basis.mvb1"},
  "preconditions": {
    "normal_decoded": true,
    "basis_prebaked": true,
    "secondary_rgb": "ambient_plus_emissive",
    "material_alpha_register": "c4",
    "fog_rgb_register": "c7",
    "fog_factor_component": "a"
  }
}
```

Unknown keys are rejected so a typo cannot silently weaken a precondition.

### 3.5 Material output and future loader hand-off

The successful manifest has `kind: "menu_light_material"` and
`runtime_integrated: false`. In addition to validated source constants, CTAB,
vertex declaration, basis statistics and texture hashes, it contains:

```jsonc
"future_loader_inputs": {
  "normal_texture": "already-decoded RGBA8 XYZ; sample directly, no xzyw/decode",
  "color1_rgb": [0.15, 0.3, 0.45],
  "color1_rgb_unclamped": [0.15, 0.3, 0.45],
  "color1_rgb_clamped": false,
  "color1_rgb_source": "clamp01(material.ambient_rgb + c6.rgb)",
  "material_alpha": {"source": "c5.z", "target": "c4.a", "value": 0.5},
  "tev_constants_rgba8": {
    "c3": [0, 0, 255, 0],
    "c4": [255, 255, 255, 128],
    "c7": [255, 255, 255, 64]
  },
  "basis_rows": {"c0": "tangent", "c1": "bitangent", "c2": "normal",
                 "consumed_offline": true}
}
```

A future runtime/material loader should:

1. validate schema/status, registry identity, all hashes and DDS headers;
2. register the normalized normal DDS without applying the retail decode again;
3. use `c3`, the rewritten `c4` and `c7` as the pre-baked TEV constants;
4. supply `color1.rgb = clamp01(ambient_rgb + c6.rgb)` and retain source
   `color1.a`; the manifest records both unclamped and clamped values;
5. bind position/texcoord/color varyings; the basis was already consumed
   offline, so `c0..c2` are not live TEV constants in the five-stage plan.

Steps 1–5 are **not implemented in the runtime by this work**.

## 4. Scaleform glyph contract

There is no `.fxp` for this path. The host validator checks the explicit
`ui.scaleform_glyph` registry contract, never a shader hash or bytecode stream.

### 4.1 Atlas and fixed state

Required atlas properties:

* one 2D linear LDR UNORM texture with a real alpha channel (DXT3/BC2 or
  DXT5/BC3, not premultiplied DXT2/DXT4);
* straight alpha and a white/greyscale coverage mask (`rgb: "white_mask"`);
* transparent and non-transparent coverage;
* partial-alpha pixels must remain white, not RGB scaled by alpha;
* no colour-font layers, SDF, outline, shadow or subpixel variant.

The single fixed pass is `RGB/A = atlas * PRIMARY vertex colour`, bilinear
filtering, `src-alpha / one-minus-src-alpha`, depth test without write, no alpha
test, one texture unit, one TEV stage and no constants. Both quad windings are
accepted; the future UI path must disable back-face culling.

### 4.2 Glyph descriptor example

```json
{
  "schema_version": 1,
  "material": {"source": "fixed_scaleform", "kind": "scaleform_glyph_atlas"},
  "bytecode": null,
  "atlas": "atlas.dds",
  "atlas_contract": {"alpha": "straight", "rgb": "white_mask",
                     "color_space": "linear"},
  "vertices": "quads.mgv1",
  "vertex_contract": {
    "topology": "triangle_list", "vertices_per_quad": 6,
    "position": "clip_xyzw", "texcoord0": "normalized_2d",
    "color0": "rgba8_unorm"
  },
  "state": {
    "texture_units": 1, "sampler": "2d", "filter": "bilinear",
    "blend": "src_alpha_one_minus_src_alpha",
    "depth": "test_only_no_write", "alpha_test": "none",
    "tev_stages": 1, "constants": 0
  },
  "variants": {"sdf": false, "outline": false, "shadow": false,
               "color_layers": false, "subpixel": false}
}
```

`bytecode` must be exactly `null`. A supplied FXP/SM3 path is a contract error.

### 4.3 MGV1 quad stream

`MGV1` is a canonical host fixture, not an existing Scaleform or guest format:

```
offset  size  field
0       4     magic "MGV1"
4       4     u32 version = 1
8       4     u32 vertex count (>0, divisible by 6)
12      4     u32 record bytes = 28
16      28*n  little-endian records:
                 clip position  float32 x,y,z,w   (16 bytes)
                 texcoord0      float32 u,v       (8 bytes)
                 colour0        uint8 r,g,b,a     (4 bytes)
```

Each quad is six triangle-list vertices in corner order `0,1,2,2,1,3`. The
four shared corners must have identical position, UV and tint. Position must
be inside the D3D clip volume with `w > 0`; UV must be in `[0,1]`; position and
UV area must be non-zero. The file size must be exactly `16 + 28*n`.

### 4.4 Glyph output and future loader hand-off

The output has `kind: "scaleform_glyph"`, `bytecode: null`, validated atlas and
vertex hashes/counts, statistics, registry state and `runtime_integrated:
false`. A future loader must validate the hashes, upload the straight-alpha
atlas, convert MGV1 records to its own vertex layout, and select the fixed
one-stage modulate state above. It must not run an FXP matcher or substitute an
unrelated menu texture. This loader is **not implemented here**.

## 5. Tests and measured retail sample

`tests/menu_preprocess_test.py` contains 29 synthetic tests. It covers:

* both decode formulas, all 256 UNORM byte values and channel reorder;
* partial signature probes and wrong literals/swizzles;
* normal DDS output, mips, layout contradictions, SNORM rejection, strict range
  warnings/failures, truncation, huge headers and atomic failure;
* complete/missing/contradictory `c0..c7`, CTAB, handedness and per-vertex basis
  data; normal-manifest signature/hash/dimension mismatches;
* fixed glyph state, no-FXP rule, straight versus premultiplied alpha, colour
  atlases, canonical quad topology, UV/clip errors and truncated streams;
* duplicate JSON keys, bounded input/memory/record limits and CLI exit/report
  behaviour.

No retail bytes are embedded in tests.

Read-only sample measured on 2026-11-24 against
`E:\SteamLibrary\steamapps\common\Skyrim\Data`:

| Item | Measured result |
|---|---|
| Partial menu-light probe | 5 `p5_MAD_NORMAL` + 1 `p1200005_ADD_NORMAL` |
| Logo normal source | `Logo02_n.dds`, DXT5, 512×1024, 11 mips, 699,216 unpacked B |
| Normal output | RGBA8 XYZ, 512×1024, 11 mips, 2,796,332 B |
| Both signature outputs | identical SHA-256 `3419396d9b15cb00262f58f8e3820eb2637e9da56c30453395d65d647dfb6267` |
| Host memory peak per conversion | 4,959,056 B (tool-owned buffer lower bound) |
| Source channel means | R 127.753485, G 128.353912, B 246.350201, A 36.221321 |
| Decoded normal mean | `(0.001988, -0.715911, 0.006697)`, length `0.715945` |
| Range result | pass with an explicit strong-bias warning; `--strict-range` would reject this real asset |
| Material contract | skipped: no resolved runtime constants or extracted MVB1 stream in the archives |
| Scaleform glyph | skipped: Interface.bsa has font SWFs and exported icon DDS files, but no verified glyph-atlas descriptor/MGV1 stream; this tool does not parse SWF glyph programs |

The sample wrote only derived files under `build_host/menu_preprocess_sample`.
It refuses an output directory inside the retail Data directory or one of its
parent directories.

## 6. Bounded-memory policy

Defaults:

| Limit | Default |
|---|---:|
| input file / unpacked BSA entry | 16 MiB |
| tool-owned live memory | 16 MiB |
| JSON document | 1 MiB |
| output DDS | 64 MiB |
| top-level pixels | 4,194,304 |
| vertex records | 1,000,000 |

DDS headers and declared mip sizes are checked before pixel allocation. Normal
maps decode one mip at a time and stream output rows; material basis and glyph
quad files are read in bounded record chunks. Memory reports account for the
source, decoder workspace/copy, normalized level, fixed write/hash buffer and
vertex chunks. They are a conservative **lower bound on process RSS**: Python
interpreter, allocator fragmentation and small standard-library temporaries
are excluded.

Failures are deterministic and name the exceeded field/limit. No swap file,
unbounded JSON materialization or whole vertex-stream allocation is used.
