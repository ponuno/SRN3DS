# Shader pack `skinning`

Owner: `sm3-agent-skinning`

character/normal-map/tint-mask shaders and skinned VS

* census families: `skinning/characters`
* chunks: **2162** / unique hashes: **1194**
* pack catalog coverage: **202 translated**, **3 draw-enabled**, **992 refused**
  (`plans.json` / `refusals.json`; the manifest indexes it as 199 translated +
  3 draw-enabled, because `manifest.json` separates the draw rows)

## VS: every contract is emitted and verified

All **108** `pica_vs` contracts in this pack are `codegen: "emitted-verified"`:
picasso emits a non-empty `.shbin`, the D3D vs_3_0 reference interpreter and
the emitted PICA interpreter agree on 256 deterministic samples, and the packed
output map is compared component by component.

| metric | value |
|---|---|
| VS contracts | **108 / 108** emitted-verified |
| contract quality | 83 `exact`, 25 `approx` (second raster colour dropped) |
| max abs position error | 4.4e-7 (tolerance 1e-4) |
| max abs varying error | 2.8e-8 (tolerance 1e-3) |
| draw-enabled / runtime | 3 (`f8ffaecd`, `20f4db07`, `60dbd11a`) |

### PICA output budget and varying packing (`pica_vs_pack.py`)

PICA VS has **seven** output registers.  One is clip position; the other six
carry varyings, and picasso names exactly these semantics:

```
texcoord0, texcoord0w, texcoord1, texcoord2, color, view
```

That is **24 varying components** (six 4-component registers).  A portability
probe (`picasso` 2.7.2) rejects `texcoord3+`, `color1`, `normal` and
`tangent`, and accepts `texcoord0w`/`view` as full 4-component registers; the
pack therefore treats 24 as the physical limit and records it in every
`packing` block (`capacity: 24`).

* **83 contracts** declare at most six varying vectors.  Each declared output
  keeps its own physical register (preferred semantics preserved, so committed
  representative bytes do not move).  This is lossless.
* **25 contracts** declare 8-9 varyings (22-28 written components).  They all
  carry a second raster colour (`color1`) but **no** normal/tangent/binormal
  outputs, so octahedral normal compression is not the lever here.  The pack
  drops `color1` (4 components, the sanctioned `approx` fallback from the
  brief), then packs the remaining 18-24 components into the 24 lanes.  Blocks
  that fit a contiguous slot are written directly with a translated destination
  mask; a block that would straddle registers is accumulated in a free
  temporary and scattered component by component in the epilogue.  **No
  quantisation is used**; the dropped colour is the only approximation and is
  named in `approximation` and in the `dropped` list of `packing`.

`plan["outputs"]` is the physical PICA output list (<=7 entries);
`plan["contract_outputs"]` keeps the original D3D declaration; `packing` maps
every written D3D component to its `(out, dst)` lane.  The emitter and the
numeric verifier both consume this one map.

### Emitter fixes required by the full corpus

* **Relative-constant token.**  Upstream `asm.py` moved the `a0.<comp>` token
  onto `Src.relative`; the old emitter looked for a following `REG_ADDR`
  pseudo-source and refused every skinned program.  The three committed
  representatives did not regenerate before this fix.
* **Absolute/relative bank aliasing.**  `c0` can be read both as `c0[a0.y]`
  and as an absolute row.  D3D has one constant bank, so an absolute read
  below `3*bones` now maps to that palette row instead of allocating a
  separate PICA row.  This was the last numeric mismatch in four contracts.
* **`frc` outside the index-floor idiom** lowers to `x - flr(x)`.
* **`pow`** lowers to `ex2(y * lg2(max(x,0)))`, with a `(0 < x)` mask and an
  exponent clamp (`inf * 0` is NaN); D3D9's undefined negative base is 0.
* **`abs`/`absneg` source modifiers** materialise `max(x, -x)` in distinct
  temporaries (two abs sources in one instruction no longer collide).
* **`dstmod1` saturate** clamps to [0,1] via `max`/`min` with a `.constf`
  one; `dstmod2` (partial precision) is a semantic no-op.
* `dp3`/`add`/`sub`/`min`/`max`/`slt`/`sge`/`rcp`/`rsq`, plus the existing
  `abs`/`nrm`/`lrp` synthesis, cover the remaining straight-line bodies.

### Draw gate

The three representatives keep `draw_enabled: 1`, `runtime: 1` and the full
`emitter` block.  The other 105 verified programs set `runtime: 0` and
`draw_enabled: 0`.  `pack_plans.py emit-runtime` emits only draw-enabled rows,
so the runtime table keeps exactly three programs; its pinned `.shbin` bytes
were regenerated after the emitter changes and the ARM target was rebuilt
compile-only (`tools/build.ps1 -Isolated`).  `DevCaps` and any frame claim are
untouched.

## PS: the basic tint / membrane / facegen class (`ps_tint.py`)

The shared `plan_ps` accepts only a strict single-temp straight-line graph and
an RGBA8 `def`, so none of the 1,086 PS hashes can be lowered 1:1 today; the
census class `def_literal_out_of_range` is only the first refusal and every
such shader also uses `nrm`/`rcp`/`rsq`/`pow`/`cmp` or reuses a temp across
stages (`temp_not_previous` / `written twice`).  A 1:1 lowering needs a
multi-temp TEV scheduler; that remains open.

The pack lands the well-understood **basic albedo tint**: straight-line PS
with at most three 2D textures become one TEV stage per texture plus one
MODULATE by the vertex colour.  Normal/specular/lighting maths and the
`texkill` alpha test are dropped, so these plans are `quality: "approx"`, name
what was dropped in `approximation`, and are never draw-enabled.  No literal
`def` is baked, so the RGBA8 range limit does not apply.  **94 hashes** translate
this way.

## Named refusals (`refusals.json`)

| class | hashes | actual remaining lever |
|---|---:|---|
| `def_literal_out_of_range` | 962 | `ps_not_straight_line` (432 branches) / `ps_texture_budget` (398: 4+ textures) / `ps_dependent_texture_read` (162) |
| `unsupported_opcode` | 30 | `pow`/`texkill`/`lrp` outside the straight-line PS class |

There are **no VS refusals left**: the 14 `pica_vs_output_envelope` and 11
`too_many_vs_varyings` hashes are now packed and emitted-verified.  The
refusal keeps the manifest `class` (the census label is fixed); `reason` and
`needs` name the actual lever.

## Tests

* `test_pack.py` (7 tests): catalog/refusal partition of all 1,194 hashes,
  schema validation of all 202 descriptors, the 24-component packing budget
  and the color1-only drop rule, quality/approximation fields, the draw/runtime
  rule, and symbolic regeneration from the retail archive.
* `test_emitter.py` (7 tests): 108 verified programs, envelope limits, the
  packing map, four-distinct-bone proof, the real c184 row, and the
  "empty/skipped is not verified" rule.
* `pack_plans.py verify --data`: **202/202 pass**.
* `tools/shader_packs.py check` and
  `tools/shaders/plan_verify.py --plans docs/shader_plans.json` are green.
