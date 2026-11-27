# Shader workarounds: derived constants and multi-pass plans

`d9-shader-plan-v1` has two optional escape hatches that keep shader packs
inside the existing PICA200 limits without adding a DevCaps claim or a new
refusal:

* **`constant_ops`** — build a pixel-shader constant register from the
  `SetPixelShaderConstantF` bank when the guest never uploads it (the common
  `c0.zwzw` non-uniform swizzle).
* **`passes[]`** — replay one D3D9 draw through 2..4 TEV programs when the
  shader needs more than the six hardware stages.

Both are additive: a plan with neither field behaves exactly as before
(`docs/shader_plan_schema.json` still accepts every committed plan byte for
byte).

## 1. `constant_ops`

An operation builds one destination register from 1..3 source registers:

```json
"constant_ops": [
  { "dst_reg": 7, "op": "pack",
    "src": [{ "reg": 0, "swizzle": "zwzw", "negate": 0 }] }
]
```

Fields:

| field | meaning |
| --- | --- |
| `dst_reg` (alias `dst`) | destination D3D `c#`, `0..31` (32-row derived bank) |
| `op` | `pack`, `add`, `mul`, `lerp` |
| `src[]` | 1 source for `pack`, 2 for `add`/`mul`, 3 for `lerp` |
| `src[].reg` | source `c#`, `0..31` |
| `src[].swizzle` | four lanes from `x/y/z/w`, in destination order |
| `src[].negate` | `1` subtracts the swizzled source |

Operator semantics (per lane):

```
pack : d = s0
add  : d = s0 + s1
mul  : d = s0 * s1
lerp : d = s0*(1-t) + s1*t        (t = s2, per lane)
```

### Example: `c0.zwzw`

The guest shader samples `c0` and uses `c0.zwzw`; the runtime only has the raw
`c0 = (x, y, z, w)`. A stage can reference `c7` instead, and the plan derives
it:

```json
{
  "id": "pack.c0.zwzw.ps",
  "sha256": "…64 hex…",
  "stage": "ps_3_0",
  "kind": "tev",
  "draw_enabled": 1,
  "note": "derived c0.zwzw into c7",
  "stages": 1,
  "stage_data": [{
    "func_rgb": 1, "func_a": 1,
    "src_rgb": [14, 3, 15], "src_a": [14, 3, 15],
    "op_rgb": [0, 0, 0], "op_a": [0, 0, 0],
    "const_reg": 7, "const_is_literal": 0, "const_rgba8": 0,
    "scale_rgb": 0, "scale_a": 0
  }],
  "textures": [{"texcoord_reg": 0, "sampler_reg": 0, "sampler_type": 2}],
  "constants": [0, 7],
  "constant_ops": [{
    "dst_reg": 7, "op": "pack",
    "src": [{"reg": 0, "swizzle": "zwzw", "negate": 0}]
  }]
}
```

At every draw the runtime copies the raw `c0` from
`SetPixelShaderConstantF`, evaluates the op and writes `c7` into the TEV
constant bank before the batch. The raw bank (`GetPixelShaderConstantF`)
still returns the guest's values.

### Gate behaviour

`d9_shader_plan_draw_allowed` rejects a malformed op by name (destination
outside `c0..c31`, wrong source count, bad swizzle lane, `negate` not 0/1).
The per-draw gate additionally refuses with a named reason when a source was
never uploaded:

```
pixel plan pack.c0.zwzw.ps: plan …: derived c7 needs source c0.zwzw,
but c0 was never uploaded by SetPixelShaderConstantF
```

No partial draw is submitted.

The derived bank has 32 rows.  Rows `c9..c31` are the normal allocation for
GFx-style plans: guest material constants `c0..c3` stay untouched and a
stage keeps reading them directly.  A derived destination that would shadow a
raw constant another stage still uses is refused by name.  PICA200 exposes no
indexable per-draw TEV uniform bank — the citro3d backend folds every
referenced row into a per-stage literal RGBA8 colour — so 32 rows cost host
memory only.

## 2. `passes[]`

A pass plan contains 2..4 entries. Each entry is a full TEV pass with its own
stage data, textures, constants and blend/depth state:

```json
"passes": [
  {
    "stage_data": [ /* up to 6 stages */ ],
    "textures": [{"texcoord_reg": 0, "sampler_reg": 0, "sampler_type": 2}],
    "constants": [0, 1],
    "inputs": [],
    "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
    "depth_write": 0
  },
  {
    "stage_data": [ /* the seventh stage */ ],
    "textures": [],
    "constants": [],
    "inputs": [],
    "blend": {"enable": 1, "src": 2, "dst": 1, "op": 1},
    "depth_write": 1
  }
]
```

* `stage_data` — 1..6 stages per pass (the PICA hardware limit).
* `textures` — at most 3 texture units per pass.
* `constants`, `inputs` — the same shapes as the top-level fields.
* `blend` — raw D3D render-state values: `enable` 0/1,
  `src`/`dst` are `D3DBLEND` 1..11 (`1 = ZERO`, `2 = ONE`, …),
  `op` is `D3DBLENDOP` 1..5 (`1 = ADD`, …).
* `depth_write` — `D3DRS_ZWRITEENABLE` 0/1 for the pass.

`passes[0]` must mirror the entry's top-level `stage_data`, `textures`,
`constants` and `stages`. The top-level descriptor stays the single-pass
description used by tooling that does not understand passes.

### Example: seven stages

A seven-stage shader is split 6+1. The first pass writes the six-stage result;
the second pass applies the seventh stage and blends it into the same render
target:

```json
{
  "stages": 6,
  "stage_data": [ /* six stages */ ],
  "passes": [
    {"stage_data": [ /* six identical stages */ ],
     "textures": [{"texcoord_reg": 0, "sampler_reg": 0, "sampler_type": 2}],
     "constants": [],
     "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
     "depth_write": 0},
    {"stage_data": [ /* seventh stage */ ],
     "textures": [],
     "constants": [],
     "blend": {"enable": 1, "src": 2, "dst": 1, "op": 1},
     "depth_write": 1}
  ]
}
```

### Runtime behaviour

`DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP` and
`DrawIndexedPrimitiveUP` detect a plan with `pass_count > 1` and call
`d9_draw_plan_passes` (`source/d3d9_core.c`). That function:

1. validates every pass descriptor and blend/depth state **before** the first
   submit — a bad pass is a named refusal and nothing is drawn;
2. replays the same geometry once per pass, setting that pass's TEV program,
   blend and depth-write state;
3. restores the caller's render state, pixel-shader pointer and TEV constant
   bank afterwards, so intermediate state never leaks into the next draw.

## Where the plan is validated

* `tools/shaders/plan_verify.py` mirrors the schema: `constant_ops` arity and
  swizzles, source availability, `passes` length 2..4, per-pass stage/texture
  limits, blend ranges, and `passes[0] ==` the top-level descriptor.
* `tools/fxp_compile.py emit-c` emits the new fields after the existing TEV
  descriptor. Plans without them generate exactly the previous bytes.
* `source/d3d9_shader_plan.c` owns the structural gate and the constant
  evaluator; `source/d3d9_core.c` owns derived-constant dirty tracking and the
  multi-pass replay.

DevCaps, `GetDeviceCaps` and the draw certificate are unchanged: both fields
describe how an already-selected plan is executed, not a new hardware
capability.
