# blit_post phase-1 report

Status: **host-only catalog. No runtime plan was added, no draw and no frame is
claimed.** `DevCaps` is untouched (`0x00082000`), the 16-slot runtime table
still has its original four entries, and every new catalog entry has
`"runtime": 0`.

Worktree `shader-blitpost` from `d66ef6f`; only
`source/shader_packs/blit_post/` was edited by hand. Generated shared files
(`docs/shader_coverage.json`, `source/shader_packs/index.json`, this pack's
`manifest.json`) were refreshed with `tools/shader_packs.py`; no other pack's
row or README was kept changed.

## 1. Result

| quantity | before | after |
|---|---:|---:|
| unique hashes | 69 | 69 |
| descriptors | 2 (1 PS copy, 1 VS contract) | **34** |
| translated (non-draw) | 1 | **27** |
| draw-enabled | 1 | **7** |
| refused | 67 | **35** |
| contract-only VS | 8 | **0** |
| runtime-table entries from this pack | 0 | **0** |

The 34 descriptors are 25 PS `tev` + 9 VS `pica_vs`. Two of them are the
already-certified shared entries (`menu.composite.ps`,
`game.vs.0ccc7542`), so the pack catalog itself adds 32 entries (24 PS + 8 VS).

Target was 20-35 translated hashes and 5-10 full-contract candidates: **34**
and **6 new draw-enabled** (7 including the pre-existing copy).

## 2. Classification of all 69 hashes

| shape | hashes | planned | what the IR actually is |
|---|---:|---:|---|
| copy / tint / constant clear | 9 | 8 | `texld oC0,v0,s0`, one `mad` tint, or `mov oC0,cN` |
| blend / composite | 7 | 3 | 2-3 textures, `lrp`/`cmp`, frame-adaptive weights |
| blur / bloom / DOF / sharpen | 41 | 12 | 3-49 taps, loops (`rep i0,7`), one accumulating register |
| tonemap / colour-grade | 3 | 2 | `dp3` luma, `cmp`/`lerp` curve, 16-slice LUT |
| PICA VS fullscreen quad | 9 | 8 (+1 runtime) | position + 1 texcoord in, 1-5 varyings out |
| debug | 0 | 0 | none in this pack |

The 35 refusals keep their census class: `dataflow` 14, `unsupported_opcode`
11, `def_literal_out_of_range` 8, `tev_prebake` 1,
`input_not_a_colour_varying` 1. `contract_only` is 0 because the VS contracts
are now committed; they are still not draw-enabled because native PICA VS
codegen does not exist.

## 3. Full-contract, draw-enabled candidates

Six new one-stage TEV descriptors satisfy the material contract: 0 or 1
texture unit, no varyings beyond UV, literal constants only, alpha fixup not
needed, and an IR-level symbolic check of the declared spec.

| hash (8) | declaration | IR evidence |
|---|---|---|
| `911297c9` | copy s0 -> oC0 | `texld oC0, v0, s0` |
| `39f0dfdf` | copy s0 -> oC0 | `texld oC0, v0, s0` |
| `817340bf` | clear 0.5 grey opaque | `mov oC0, c0.x`, def `0.5` |
| `4813dba3` | fill (0.5,0.5,0,0) | `mov_pp oC0, c0.xxy` |
| `6628a644` | fill black opaque | `mov oC0, c0.xxxy`, def `(0,1,0,0)` |
| `9d2692a9` | fill white opaque | `mov oC0, c0.x`, def `1` |

`01ab60cd` is the same shape but writes `oC0.xy` only; its alpha is undefined
in SM3, so it is declared `quality: "approx"` with alpha 0 and is **not**
draw-enabled. `583f98e0` computes alpha as `1-r`; TEV alpha cannot read red, so
it is a copy approximation, not a full contract.

## 4. What was simplified

| group | IR | plan | expected quality loss |
|---|---|---|---|
| 7x7 loop blur (`70a85a92`) | 49 taps, `rep i0,7` | 1 centred tap | box blur becomes copy |
| 16-tap separable blur (`de28caa3`) | 16 `texld`, 1 accumulator | 1 tap | no wide kernel |
| 3/6/8-tap blur (`6ccbe759`, `b6484c80`, `c91e3328` Sobel) | N taps | 1 tap | soft edges/derivatives gone |
| 5-tap radial (`d01c19fd`) | `rsq`/`rcp`, 5 taps | 1 tap | no radial streak |
| 5-tap bloom (`f50eb44c`) | 5 taps + bias | 1 tap | no bloom halo |
| sharpen/Laplacian (`4a4fa2b2`, `0072af4b`, `00d8a930`) | 5-9 taps | 1 tap | sharpening dropped |
| chromatic aberration (`2cc6e508`) | 3 channel-offset taps | 1 tap | fringes dropped |
| 14-tap luma blur (`041086d9`) | 4 taps + `dp3` luma | 1 tap + DOT3 | luma kept, 13 taps dropped |
| adaptation+bloom (`a90f707a`) | 3 textures, `rcp`/`cmp` | 2-tex 50/50 lerp | eye adaptation fixed at 0.5 |
| crossfade (`21978c95`) | 3 textures, adaptive weight | 2-tex 50/50 lerp | third texture and adaptation term dropped |
| DOF/radial blend (`81623622`) | 2 textures + `rsq`/`rcp` | 2-tex 50/50 lerp | radial weight fixed |
| 16-slice LUT grade (`8fb14655`) | 16 `dcl_2d`, `lrp` chain | 1 slice | 15 slices dropped |
| tone map/grade (`83cc2df2`) | `dp3` luma, `cmp`/`lerp`, negative defs | `MODULATE` by 0.8 | no hue/sat curve |
| tint (`583f98e0`) | RGB tint, alpha `1-r` | copy | alpha becomes `tex.a` |

### LUT arithmetic

A 16x16x16 LUT is 4096 texels; packed as a 256x16 2D strip it is one texture
unit and one sample, so it fits beside the scene texture in PICA's three units.
It is **not** usable here as authored: `8fb14655` binds 16 separate 2D slices
and blends them in a 16-step `lrp` chain, and PICA has no indirect stage. The
catalog therefore samples slice 0 and records the loss. If grading is needed
later, the 256x16 strip is the exact follow-up, not a wider descriptor.

## 5. Tests

```text
python source\shader_packs\blit_post\pack_plans.py
python -m unittest discover -s source\shader_packs\blit_post -p "test_*.py"
python tools\shaders\plan_verify.py --plans source\shader_packs\blit_post\plans.json
```

* 15 tests pass: manifest partition (69 = 34 + 35), schema/semantics, the
  one-stage draw contract, named approximations, VS limits, plan-vs-IR for
  copy/constant fills, real tap reduction for three blur families.
* Negative tests: a draw-enabled approximation is rejected, an approximation
  without its `approximation` field is rejected, a premature `runtime: 1` is
  rejected, an unknown hash breaks the partition.
* `plan_verify.py` reports 0 schema errors for the whole pack catalog.
* IR checks read `E:\host_sm3`; they skip cleanly when the host corpus is
  absent.

## 6. Shared fixes requested

**None.** No C/TEV planner or runtime change is needed for this catalog. The
only follow-ups are pack-local or scheduled separately:

1. Runtime promotion of the six exact one-stage plans needs a measured
   `runtime: 1` decision plus a free slot (the table is capped at 16).
2. `01ab60cd`/`583f98e0` need a declared alpha rule before they can become
   exact; both are honestly approximated today.
3. The 256x16 packed LUT is the route for `8fb14655`, not a bigger TEV
   descriptor.
4. `asm.py` still does not expose `texldp`/`texldb` control bits; none of the
   planned hashes uses them, so it did not block this pack.
