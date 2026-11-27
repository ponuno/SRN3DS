# World shader closure (terrain, lighting, skinning, sky/water, blit/post)

Status: **static audit, gate wired (2026-11-27).** This is the world-pack
companion to `docs/MENU_SHADER_CLOSURE.md` §0. It changes neither
`GetDeviceCaps`/`DevCaps` nor the renderer, and it claims no frame. Every
number below is re-derived from committed catalogs and the generated C tables;
`python tools/shaders/world_runtime_audit.py` is the single source.

## 0. Plan → runtime path

`tools/shaders/world_runtime_audit.py` reuses the global
`runtime_audit.py` parsers/validators and scopes them to the five world packs.
For every plan it joins:

* the pack catalog (`source/shader_packs/<pack>/plans.json`),
* pack plans reached from `docs/shader_plans.json` (the runtime rows that live
  in the shared document, e.g. `menu.composite.ps` for `blit_post`),
* `source/d3d9_pica_vs_*_table.inc` (exact-hash PICA VS + embedded `.shbin`),
* `source/d3d9_shader_plan_table.inc` (generated TEV/pica_vs descriptors),
* `source/shader_packs/<pack>/manifest.json` and `docs/shader_coverage.json`.

A `draw_enabled: 1` row passes only with `runtime: 1` (when claimed) **and** a
validated artifact: an exact-hash DVLB program whose pinned SHA-256 matches the
plan emitter, or a generated TEV descriptor whose own `draw_enabled` is 1.
TEV stages/passes/constant binding/textures and PICA VS lengths/registers/
varyings (≤ 7)/uniform bank (≤ c31) are validated in the same pass.

`tests/world_shader_audit_test.py` fails on an unmatched draw row, an invalid
descriptor, a draw-disabled plan with no stated reason, a manifest/plan
mismatch, or a coverage counter that disagrees with the committed plans. It is
registered in `tools/run_host_tests.ps1` and includes a negative check: deleting
the TEV row for `menu.composite.ps` must make the audit report it unmatched.

## 1. Per-pack summary

`plans` counts pack-owned rows plus the shared `docs` rows reachable from that
pack's census; `remaining` is `plans - runtime` (all explicitly
`draw_enabled: 0`, with a note). `refused` is the manifest's named-refusal
count.

| Pack | Plans | Draw-enabled | Runtime paths | Remaining (offline-only) | Census hashes | Refused |
|---|---:|---:|---:|---:|---:|---:|
| `terrain` | 138 | 0 | 0 | 138 TEV | 247 | 109 |
| `lighting` | 237 | 6 | 6 | 124 VS + 107 TEV | 4,991 | 4,754 |
| `skinning` | 202 | 3 | 3 | 105 VS + 94 TEV | 1,194 | 992 |
| `sky_water` | 45 | 0 | 0 | 45 TEV | 45 | 0 |
| `blit_post` | 69 | 1 | 1 | 9 VS + 59 TEV | 69 | 0 |
| **Total** | **691** | **10** | **10** | **681** | **6,546** | **5,855** |

`691 plans + 5,855 refused = 6,546 census hashes`; the five world packs
partition their census exactly. The global report is unchanged at
`6,659 hashes / 6,550 translated / 22 refused`.

### Runtime rows (the only world draws that claim a path)

| Plan | Kind | Path | Contract |
|---|---|---|---|
| `lit.vs.0c1d7df0` | PICA VS | exact hash, 8 uniform rows | 3 varyings |
| `lit.vs.2516943c` | PICA VS | exact hash, 4 uniform rows | position only |
| `lit.vs.5b644ad2` | PICA VS | exact hash, 4 uniform rows | position only |
| `lit.vs.d156a5a0` | PICA VS | exact hash, 4 uniform rows | position only |
| `lit.vs.de0f8a34` | PICA VS | exact hash, 9 uniform rows | 3 varyings |
| `lit.vs.fffecd8a` | PICA VS | exact hash, 8 uniform rows | 3 varyings |
| `skin.vs.20f4db07` | PICA VS | exact hash, 16 rows (12 bone + WVP) | 2 varyings |
| `skin.vs.60dbd11a` | PICA VS | exact hash, 17 rows (12 bone + WVP) | 3 varyings |
| `skin.vs.f8ffaecd` | PICA VS | exact hash, 16 rows (12 bone + WVP) | 1 varying |
| `menu.composite.ps` | TEV | generated plan-table row | 1 stage, 1 texture, no constants |

`menu.composite.ps` is a shared `docs` row reachable from a `blit_post` census
hash; it is the one TEV draw in the world set. There is no `terrain`/`sky_water`
runtime draw.

## 2. Top-10 most frequent world shaders

By census chunk count (`manifest.json` `hashes[3]`); status is the committed
plan state, not a capability claim.

| # | Chunks | Pack | SHA-256 | Kind | Status |
|---:|---:|---|---|---|---|
| 1 | 38 | `blit_post` | `c7ef0cd44146…` | vs | catalog-only: `blit.vs.c7ef0cd4`, codegen pending |
| 2 | 16 | `lighting` | `b3a9babe7949…` | vs | catalog-only: `lit.vs.b3a9babe` (not a certified rep) |
| 3 | 16 | `skinning` | `06aa117c6779…` | ps | refused: `unsupported_opcode` |
| 4 | 16 | `skinning` | `14db34d71a4b…` | ps | refused: `unsupported_opcode` |
| 5 | 16 | `skinning` | `8513aaac88ef…` | vs | catalog-only: `skin.vs.8513aaac` (not a certified rep) |
| 6 | 10 | `blit_post` | `31d368cecad5…` | vs | catalog-only: `blit.vs.31d368ce`, codegen pending |
| 7 | 10 | `lighting` | `53fe44bbf0df…` | ps | refused: `def_literal_out_of_range` |
| 8 | 10 | `lighting` | `5c6bfb974a97…` | ps | catalog-only: `lighting.approx.5c6bfb97` (approx) |
| 9 | 10 | `lighting` | `6b95db62925c…` | ps | refused: `def_literal_out_of_range` |
| 10 | 10 | `lighting` | `997083d1e3f0…` | ps | refused: `def_literal_out_of_range` |

The most frequent world shader (`blit.vs.c7ef0cd4`, 38 chunks) is a
fullscreen-quad PICA VS contract with no emitted `.shbin`; it stays
`draw_enabled: 0` with the reason in its note. Promoting it needs a generated
program and a measured upload path, not a counter change.

## 3. Holes found and closed

1. **No world-scoped gate.** The global `pica_runtime_audit_test.py` proved the
   union of all packs, but a world-only regression (manifest/coverage vs plan)
   had no owner. Added `tools/shaders/world_runtime_audit.py` and
   `tests/world_shader_audit_test.py` (negative draw-row removal included).
2. **`blit_post` generator desync.** `pack_plans.py` still asked for
   `draw_enabled: 1` on six curated exact copies/clears, while the committed
   catalog and `refusals.json` say `exact_draw_enabled: 0` (they have no
   runtime slot). Regenerating would have resurrected six “green” draw rows.
   The generator now forces catalog-only and appends the same
   `Catalog-only: … no shared runtime slot` reason as the committed notes.
3. **105 silent skinning PICA VS rows.** They are `emitted-verified` with a
   pinned `.shbin` but are not one of the three certified runtime
   representatives. Their notes now say
   `draw disabled: not one of the three runtime-certified representatives … in
   d3d9_pica_vs_table.inc`.
4. **Stale lighting reason.** 124 off VS notes claimed the runtime contract
   “only carries skinning and GFx menu programs”, which stopped being true when
   the six lighting reps landed. The note now names the six lighting reps.
5. **Counter/manifest drift.** The gate now joins every manifest plan row
   (kind + draw status) and re-derives the per-pack coverage `draw_enabled` and
   `unique_hashes`, so a hand-edited plan cannot leave a stale counter behind.

## 4. Honest remaining gaps

These are documented refusals/offline plans, not hidden draw paths:

* `terrain` — 138 port-lite TEV plans (`quality: approx`) are draw 0; 109
  hashes are refused (`needs_multipass` 103, `stage_budget` 6).
* `sky_water` — all 45 TEV plans are draw 0; this is the pack the menu audit
  deliberately demoted (catalog-only, no shared runtime slot).
* `lighting` — 124/130 PICA VS programs are emitted-verified but outside the
  six-slot runtime catalog; 107 TEV plans are offline recipes. 4,754 hashes are
  refused, including 22 VS with 8–9 varyings (`too_many_vs_varyings`).
* `skinning` — 105/108 PICA VS are emitted-verified but outside the three-slot
  runtime catalog; 94 TEV plans need the material contract; 992 refused.
* `blit_post` — 8 fullscreen-quad VS contracts have `codegen: pending`; 59 TEV
  plans are offline (including the six exact copies demoted above). The pack's
  only runtime draw is the shared `menu.composite.ps` row.

## 5. Verification

```
python tools/shader_packs.py check                     # 6 packs, 6659 hashes, OK
python tools/shaders/world_runtime_audit.py            # 10/10 world draws, 0 holes
python tests/world_shader_audit_test.py                # 21 checks, 0 failures
python tests/pica_runtime_audit_test.py                # 128/128, 30 VS programs
python tests/tev_sched_test.py                         # 10 tests, OK
python tools/shaders/plan_verify.py --data E:\SteamLibrary\steamapps\common\Skyrim\Data
                                                       # 115 entries, symbolic pass
tools/run_host_tests.ps1 -Only lift,com                # 2 passed
```
