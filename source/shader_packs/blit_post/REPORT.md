# blit_post phase-2 report

Status: **host-only catalog. 69/69 hashes have a plan; no new runtime slot,
no draw claim and no frame.** `DevCaps` is untouched (`0x00082000`), the shared
16-slot runtime table still has its original entries, and every new catalog
entry has `"runtime": 0`.

Worktree `shader-blit2` from `0f45021`; only
`source/shader_packs/blit_post/` was edited by hand. Generated shared files
(`docs/shader_coverage.json`, `source/shader_packs/index.json`, this pack's
`manifest.json`) were refreshed with `tools/shader_packs.py`; no other pack's
row was kept changed.

## 1. Result

| quantity | before | after |
|---|---:|---:|
| unique hashes | 69 | 69 |
| descriptors in this catalog | 32 | **67** |
| hashes with a plan (catalog + shared runtime) | 34 | **69** |
| translated (non-draw) | 27 | **62** |
| draw-enabled | 7 | **7** |
| refused | 35 | **0** |
| contract-only VS | 0 | 0 |
| runtime-table entries from this pack | 0 | **0** |

The 67 catalog entries are 59 PS `tev` (6 exact/draw, 53 approximate) and
8 VS `pica_vs`. The remaining two hashes are the already-certified shared
runtime entries (`menu.composite.ps`, `game.vs.0ccc7542`), deliberately not
duplicated in the pack catalog.

## 2. The 35 former refusals

Every class now has a named descriptor and a declared approximation.

| class | hashes | escape hatch |
|---|---:|---|
| `dataflow` | 14 | **`passes[]` two-pass accumulator**: 0.5 centred tap per pass, `ONE/ONE/ADD` blend keeps the sum at 1.0. Offset constants c1..cN are not TEV-addressable, so N-1 taps are dropped. |
| `unsupported_opcode` | 11 | 7 `dp3` shaders keep DOT3 luma and drop the `cmp`/`lerp` tone curve; 4 multi-texture `cmp`/`lrp`/`texldd` shaders collapse to a fixed 0.5 two-texture `INTERPOLATE`. |
| `def_literal_out_of_range` | 8 | **`constant_ops`** derives c7 from c0 (`pack`, optional `negate`) and the TEV stage reads c7 instead of an out-of-range `def`; 4 of these also replay two accumulating passes. |
| `tev_prebake` | 1 | Records `preconditions: ["tev_prebake_guest_state", "colorselect_dp4_alpha_channel_kept"]` and a DOT3-luma simplified plan. Guest prebaked TEV selectors are not read back. |
| `input_not_a_colour_varying` | 1 | Binds the texcoord1 scalar as the TEV primary colour, multiplied by literal `(0,1,0,0)`; matches only 0/1 masks, so it stays approximate. |

`constant_ops` uses the schema's `dst_reg` (c0..c7), not the older UI-pack
`dst` spelling, and `constants` names both the source and the derived register
so `plan_verify.py` proves the source was uploaded.

## 3. Draw-enabled candidates

No refusal became exact. The six exact one-stage fills/copies are unchanged:
0 or 1 texture, no varyings beyond UV, literal constants only, alpha fixup not
needed, one TEV stage. Runtime promotion still needs a measured decision and a
free table slot, so `runtime` stays 0 everywhere.

## 4. Physical limit

PICA200 allows six TEV stages per pass and three texture units; a plan may
replay 2..4 passes. The former refusals were not a `DevCaps` gap:

* per-tap UV offsets live in guest VS/constant uploads and have no TEV stage
  input, so multi-tap blurs cannot be made exact by adding stages;
* `texldd` derivatives and `cmp` select chains have no TEV equivalent, only a
  fixed-threshold coalesce;
* a `def` outside RGBA8 cannot be baked into a literal constant, so it becomes
  a derived/uniform register.

The `--data` symbolic layer is `skipped` because the retail data directory is
absent in this worktree; the pack tests validate the declared descriptor
against the host IR instead.

## 5. Checks

```text
python source/shader_packs/blit_post/pack_plans.py
python -m unittest discover -s source/shader_packs/blit_post -p "test_*.py"
python tools/shader_packs.py check
python tools/shader_packs.py coverage --out docs/shader_coverage.json
python tools/shaders/plan_verify.py --plans source/shader_packs/blit_post/plans.json
```

* `pack_plans.py`: 67 planned (6 exact/draw, 53 approx, 8 VS contract),
  0 refused.
* 16 pack tests pass, including the new escape-hatch coverage test
  (`constant_ops` 8, `passes` 18, all refusal classes planned).
* `shader_packs.py check`: 6 packs, 10,475 chunks, 6,659 hashes, **OK**.
* `plan_verify.py`: **0 schema errors in this pack**. The command still exits
  non-zero because the pre-existing `ui` pack's legacy `passes`/`constant_ops`
  rows fail the current schema; those rows were not touched.
* coverage: `blit_post` translated 62, draw-enabled 7, refused 0.

## 6. Remaining work (outside this pack)

1. Runtime promotion of the six exact one-stage plans needs a measured
   `runtime: 1` decision plus a free slot.
2. `01ab60cd`/`583f98e0` need a declared alpha rule before they can be exact.
3. The 256x16 packed LUT is the route for `8fb14655`, not a wider TEV
   descriptor.
4. The UI pack's legacy `passes`/`constant_ops` rows need a separate migration
   to the current schema (`stage_data`, numeric `blend`, `dst_reg`).
