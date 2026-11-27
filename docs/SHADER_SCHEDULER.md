# TEV stage scheduler and op-lowering library

Status: **host-only planning layer**.  It adds TEV plans for the pixel shaders
that the phase-1 direct planner refused; it does not change `DevCaps`, does not
enable a draw, and does not claim a frame.  Every generated row has
`draw_enabled = 0` and `runtime = 0`.

## 1. What was added

`tools/shaders/tev_sched.py` is the scheduler between the narrow exact planner
in `tools/shaders/compile.py` and the per-family pack recipes:

* **Expression DAG.**  `build_values()` turns the canonical IR into a
  per-channel expression tree.  Repeated temp writes are versioned; partial
  writes carry the untouched side; `def` literals become constant leaves.
* **One-accumulator scheduler.**  `TevScheduler` keeps at most one computed
  value in `PREV`.  It schedules independent operands, carries RGB and alpha
  independently (the descriptor has separate `func_rgb`/`func_a` and source
  selectors), and emits a stage only when the PICA function actually matches
  (`ADD`, `SUBTRACT`, `MODULATE`, `MULTIPLY_ADD`, `INTERPOLATE`, `DOT3`).
* **`passes[]`.**  When a value has to survive more than one stage, the
  scheduler closes the current pass, writes the value to an intermediate
  texture (`blend = "replace"`), starts a new pass and reads that texture as
  `TEX0..2`.  Each pass is at most six stages.  The top-level descriptor is the
  final pass; `passes[]` is extra catalog metadata.
* **Flow reduction.**  Constant `ifc` conditions are folded; variable branches
  are lowered to their then arm (the declared branchless-select stand-in);
  `rep`/`loop` bodies are unrolled for a constant trip count <= 4 and
  truncated to two iterations otherwise.
* **Derived constants.**  Literal leaves (including all `def` values outside
  RGBA8 `[0,1]`) are bound to derived constant registers `c200..`; the plan
  records `constant_values`/`derived_constants` and the precondition
  `def_uniformized`.  This is the literal-range rewrite the phase-1 census
  identified as the largest lever.

## 2. Op library

| IR op | TEV lowering | Quality |
|---|---|---|
| `mov/add/sub/mad/mul` | direct stage | exact |
| `lrp` | `INTERPOLATE` (`b*(1-a)+c*a` after source reorder) | exact |
| `dp3` | `DOT3_RGB`/`DOT3_RGBA` | exact (swizzled operands are refused) |
| `nrm` | unnormalised vector, `nrm_lowered_to_unnormalised_vector` | approx |
| `rcp` | constant/linear fit on `[0.25,4]`, coefficients recorded | approx |
| `rsq` | constant/linear fit on `[0.0625,16]` | approx |
| `pow(x,e)` | integer `e` expanded to `e-1` multiplies; other exponents use base | exact for integer e (approx if drop) |
| `cmp` | then arm (`cmp_lowered_to_then_arm`) | approx |
| `slt/sge` | zero constant (`slt_lowered_to_zero`) | approx |
| `min/max` | clamp forms fold to REPLACE; others keep first operand | approx |
| `abs` | identity (`abs_lowered_to_identity`) | approx |
| `frc` | identity (`frc_lowered_to_identity`) | approx |
| `texkill` | dropped; external alpha-test precondition | exact for oC0 |
| `dp4` | `DOT3` (w dropped) | approx |
| `dsx/dsy/exp/log` | zero/one constant | approx |
| `ifc/cmp` constant | folded | exact |
| `ifc` variable | then arm, `branchless_select_then_arm` | approx |
| `rep` <=4 | unrolled | approx (loop metadata recorded) |
| `rep/loop` >4 | two iterations, `loop_truncated` | approx |

Every approximate row carries:

```json
"quality": "approx",
"approximation": {
  "policy": "scheduler_approximation",
  "lost_terms": ["rcp_lowered_to_linear_fit", "..."],
  "reason": "...",
  "control_flow": {"mode": "...", "loops": [], "ifs": []},
  "op_lowering": [{"opcode": "rcp", "mode": "linear", "coefficients": "..."}],
  "def_literals": [{"reg": 8, "literal": [2.0, -1.0, 1.0, 0.0]}]
},
"approx_error": {"samples": 8, "max_abs_error": "...", "mean_abs_error": "..."}
```

`approx_error` is a numeric cross-check against an independent IR interpreter
inside `tev_sched.py` (`eval_ir`) over sampled LDR textures, colours and
constants.  Exact rows carry the same block under `verification`.

## 3. Catalog and coverage

`docs/scheduler_plans.json` is a cross-pack catalog (`runtime = 0`).  It is
included by coverage but not by the per-pack manifests, so pack-owned
generators stay authoritative for their own `plans.json`/`refusals.json`.
`tools/fxp_compile.py manifest` merges only entries with `"runtime": 1`, so
none of these rows consumes a runtime slot.

Measured over `Skyrim - Shaders.bsa` (base archive, 2026-11-27):

| Quantity | Before | After |
|---|---:|---:|
| Distinct census hashes | 6,659 | 6,659 |
| translated | 460 | **6,432** |
| draw-enabled | 10 | 10 |
| refused | 6,189 | **217** |
| pixel-shader refusals | 5,975 | **0** |
| scheduler catalog rows | -- | 5,972 (9 exact, 5,963 approx, 23 multipass) |

All 5,975 previously refused PS hashes now have a plan: exact where the
expression is symbolically inside the TEV op set, otherwise an approximation
with its lost term, fit and measured error, or an explicit `passes[]` split.
The 217 remaining refusals are **vertex-stage** programs only:

* 184 `pica_vs`: they fit the PICA VS envelope but this layer is the PS TEV
  scheduler; a VS contract/emitter is owned by the skinning/lighting VS work.
* 33 `too_many_vs_varyings (8 > 7)`/`(9 > 7)`: PICA VS has seven varying
  outputs; reducing them needs varying packing/dropping or a multi-pass VS.

Nothing in the catalog is draw-enabled.  `DevCaps` is unchanged.

## 4. Commands

```powershell
python tools\shaders\tev_sched.py --out docs\scheduler_plans.json
python tools\shader_packs.py coverage --out docs\shader_coverage.json
python tools\shader_packs.py check
python tools\shaders\plan_verify.py --plans docs\shader_plans.json `
    --data "F:\Skyrim\Data"
python -m unittest tests.tev_sched_test
```

`tests/tev_sched_test.py` covers a synthetic exact copy/tint/mad, a seven-stage
chain that must split into two passes, constant and variable branches, small
and large loops, every op-lowering family, and the >=90 % refusal coverage
target.
