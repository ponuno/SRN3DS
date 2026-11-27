# World shader closure (terrain, lighting, skinning, sky/water, blit/post)

Status: **static audit, gate wired, eight world runtime paths added
(2026-11-27).** This is the world-pack companion to
`docs/MENU_SHADER_CLOSURE.md` §0. It changes neither
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

`tests/blit_vs_runtime_test.py` adds the byte-level gate for the six new
fullscreen-quad programs: DVLB/DVLE/DVLP, pinned SHA-256, main word count, and
(when a retail archive is mounted) a re-run of the 64-sample IR↔PICA
differential whose re-emitted `.shbin` must equal the committed bytes.

## 1. Per-pack summary

`plans` counts pack-owned rows plus the shared `docs` rows reachable from that
pack's census; `remaining` is `plans - runtime` (all explicitly
`draw_enabled: 0`, with a note). `refused` is the number of census hashes in the
pack that have no plan row at all.

| Pack | Plans | Draw-enabled | Runtime paths | Remaining (offline-only) | Census hashes | Refused |
|---|---:|---:|---:|---:|---:|---:|
| `terrain` | 138 | 0 | 0 | 138 TEV | 247 | 109 |
| `lighting` | 237 | 7 | 7 | 123 VS + 107 TEV | 4,991 | 4,754 |
| `skinning` | 202 | 4 | 4 | 104 VS + 94 TEV | 1,194 | 992 |
| `sky_water` | 45 | 0 | 0 | 45 TEV | 45 | 0 |
| `blit_post` | 69 | 7 | 7 | 2 VS + 59 TEV + 1 docs contract | 69 | 0 |
| **Total** | **691** | **18** | **18** | **673** | **6,546** | **5,855** |

`691 plans + 5,855 refused = 6,546 census hashes`; the five world packs
partition their census exactly. The global report is unchanged at
`6,659 hashes / 6,542 translated / 95 draw-enabled / 22 refused`; eight
census hashes moved from `translated` to `draw-enabled`.

### Runtime rows (the only world draws that claim a path)

| Plan | Kind | Path | Proof |
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
| `menu.composite.ps` | TEV | generated plan-table row | 1 stage, 1 texture |
| `blit.vs.c7ef0cd4` | PICA VS | `blit` table, c0, 248 B | 38 chunks |
| `blit.vs.31d368ce` | PICA VS | `blit` table, c0, 248 B | 10 chunks |
| `blit.vs.57e51db9` | PICA VS | `blit` table, c0..c3, 248 B | 3 chunks |
| `blit.vs.d48c8642` | PICA VS | `blit` table, c0..c5, 248 B | 2 chunks |
| `blit.vs.ce758c79` | PICA VS | `blit` table, c0..c2, 284 B | 1 chunk |
| `blit.vs.d6896494` | PICA VS | `blit` table, c0..c1, 292 B | 1 chunk |
| `lit.vs.b3a9babe` | PICA VS | `wide` table, c0, 400 B | 16 chunks, t2/t3 → tc0/tc1 |
| `skin.vs.8513aaac` | PICA VS | `wide` table, 17 rows, 636 B | 16 chunks, t2/t3 → tc0/tc1 |

`menu.composite.ps` is a shared `docs` row reachable from a `blit_post` census
hash. There is still no `terrain`/`sky_water` runtime draw.

## 2. The six new fullscreen-quad paths

Generator: `python tools/blit_vs_emit.py --data <Data>`. It reuses the
skinning/lighting emitter and verifier (`source/shader_packs/skinning/
pica_vs_emit.py`) as a library; it does not fork the ISA lowering.

For each `pica_vs` row in `source/shader_packs/blit_post/plans.json`:

1. decode the retail `Skyrim - Shaders.bsa` chunk to canonical vs_3_0 IR;
2. lower outputs to PICA's `position`/`texcoord0..2` (`lower_varyings`);
3. run 256 deterministic samples of IR vs emitted PICA, requiring
   `max |dpos| <= 1e-4` and every kept varying within the same bound;
4. compile with the real picasso assembler and pin the `.shbin` SHA-256.

Only programs the *existing generic loader* can bind are promoted
(`draw_enabled: 1`, `runtime: 1`): inputs `position`+`texcoord0`, one
`texcoord0` varying, identity uniform bank, no dropped output. Promoted rows
live in `source/d3d9_pica_vs_blit_table.inc` /
`..._blit_shbin.inc` and are admitted by `d9_pica_vs_count/at/lookup` in
`source/d3d9_pica_vs.c`.

| Plan (exact vs_3_0 SHA-256) | Chunks | Uniform rows | `.shbin` bytes | Pinned `.shbin` SHA-256 |
|---|---:|---:|---:|---|
| `blit.vs.c7ef0cd4` `c7ef0cd44146…` | 38 | 1 | 248 | `73432d9f1218…` |
| `blit.vs.31d368ce` `31d368cecad5…` | 10 | 1 | 248 | `73432d9f1218…` |
| `blit.vs.57e51db9` `57e51db9dfcc…` | 3 | 4 | 248 | `69f5212e685f…` |
| `blit.vs.d48c8642` `d48c8642ffa0…` | 2 | 6 | 248 | `3976efb6c48c…` |
| `blit.vs.ce758c79` `ce758c790d96…` | 1 | 3 | 284 | `7767e7e3ceee…` |
| `blit.vs.d6896494` `d68964943e95…` | 1 | 2 | 292 | `5a9b287a19f8…` |

`c7ef0cd4` and `31d368ce` are different D3D bytecode hashes that lower to the
same PICA program and therefore the same `.shbin` bytes; both keys are kept
because the runtime lookup is exact-hash. Total added embedded bytes: 1,568.

Two `blit_post` VS stay `draw_enabled: 0` with a named limitation:

* `blit.vs.4d155cd6` (1 chunk) writes `texcoord1`;
* `blit.vs.c43418bb` (3 chunks) writes `texcoord1,2` and drops `texcoord3,4`.

Both are emitted and numerically verified (`max |dpos| = 0`) but
`d9_pica_vs_declare` binds usage index 0 of each attribute only, so promoting
them would silently feed the PS the wrong varying. They need a wider varying
loader plus a paired PS contract, not a counter change.

## 2b. Wide-varying paths (paired PS rank contract)

`tools/vs_wide_emit.py` promotes the two 16-chunk VS that write D3D
TEXCOORD2/TEXCOORD3:

| Plan | Chunks | Bank | `.shbin` bytes | Pinned SHA-256 | Map |
|---|---:|---|---:|---|---|
| `lit.vs.b3a9babe` | 16 | identity, 5 rows | 400 | `ad58098a97d8…` | t2→tc0, t3→tc1 |
| `skin.vs.8513aaac` | 16 | 4-bone, 17 rows | 636 | `9419e3ca6835…` | t2→tc0, t3→tc1 |

The rank rule is shared with the PS side: the retail PS family in
`shadersfx\shaders001.fxp` that samples exactly `{t2,t3}` (12 distinct blobs,
e.g. `ui.ps.a5712176`) already stores `textures[].texcoord_reg = 1` for t3.
The VS packs the same ascending rank, so `t2` and `t3` arrive on the slots
the PS reads. The plan keeps `varying_map`/`contract_outputs`, states
`paired_ps_texcoord_rank_map` as a precondition, and is `quality: exact`
because the component values are unchanged (the same rule the skinning packed
representatives use).

The loader itself is widened in the same pass: `D9PicaVsAttr` now carries a
D3D `usage_index`, and `d9_pica_vs_declare` matches `TEXCOORDn` against the
declaration's `TEXCOORDn` instead of always index 0. That unlocks the 15
emitted-verified lighting/skinning VS that read `TEXCOORD1..4` once their
output envelope and PS pair are certified; none is promoted by this change.
`pica_vs_runtime_smoke` covers both directions (TEXCOORD1 binds, TEXCOORD0
does not satisfy a TEXCOORD1 input).

## 3. Top-30 world plans by frequency

By census chunk count (`manifest.json` `hashes[3]`); status is the committed
state, and `needs` names the concrete missing feature. This is the work queue,
most frequent first.

| # | Chunks | Pack | SHA-256 | Kind | Plan | Needs |
|---:|---:|---|---|---|---|---|
| 1 | 38 | `blit_post` | `c7ef0cd44146` | vs | `blit.vs.c7ef0cd4` | closed (runtime) |
| 2 | 16 | `lighting` | `b3a9babe7949` | vs | `lit.vs.b3a9babe` | closed (runtime, wide rank contract) |
| 3 | 16 | `skinning` | `06aa117c6779` | ps | — | `ps_dependent_texture_read`: dataflow scheduler (sample coordinate from another sample) |
| 4 | 16 | `skinning` | `14db34d71a4b` | ps | — | same dataflow scheduler |
| 5 | 16 | `skinning` | `8513aaac88ef` | vs | `skin.vs.8513aaac` | closed (runtime, wide rank contract) |
| 6 | 10 | `blit_post` | `31d368cecad5` | vs | `blit.vs.31d368ce` | closed (runtime) |
| 7 | 10 | `lighting` | `53fe44bbf0df` | ps | — | PS `def` literal rewrite (outside RGBA8 [0,1]) |
| 8 | 10 | `lighting` | `5c6bfb974a97` | ps | `lighting.approx.5c6bfb97` | runtime TEV slot for an offline approximation |
| 9 | 10 | `lighting` | `6b95db62925c` | ps | — | PS `def` literal rewrite |
| 10 | 10 | `lighting` | `997083d1e3f0` | ps | — | PS `def` literal rewrite |
| 11 | 10 | `lighting` | `a47654d2b4aa` | ps | — | PS `def` literal rewrite |
| 12 | 10 | `lighting` | `bf65fd8352df` | ps | `lighting.contract.bf65fd83` | runtime TEV descriptor (recipe only) |
| 13 | 10 | `lighting` | `c157fddc8095` | ps | — | PS `def` literal rewrite |
| 14 | 10 | `lighting` | `c2b26bfeb2b3` | ps | — | PS `def` literal rewrite |
| 15 | 10 | `lighting` | `cb4019103c40` | ps | `lighting.approx.cb401910` | runtime TEV slot for an offline approximation |
| 16 | 10 | `lighting` | `d223dc0698a3` | ps | `lighting.approx.d223dc06` | runtime TEV slot for an offline approximation |
| 17 | 10 | `lighting` | `d8a0ed61a53d` | ps | — | PS `def` literal rewrite |
| 18 | 10 | `lighting` | `e37fdbba22b6` | ps | `lighting.approx.e37fdbba` | runtime TEV slot for an offline approximation |
| 19 | 10 | `lighting` | `e737715b5028` | ps | `lighting.approx.e737715b` | runtime TEV slot for an offline approximation |
| 20 | 10 | `lighting` | `f43b6dc2e147` | ps | — | PS `def` literal rewrite |
| 21 | 10 | `skinning` | `954fb18dd8c8` | ps | — | PS `def` literal rewrite |
| 22 | 10 | `skinning` | `e4db6c8a1835` | ps | `skin.ps.e4db6c8a` | runtime TEV descriptor (recipe only) |
| 23 | 9 | `lighting` | `cfbba75c4130` | ps | — | PS opcode lowering (`nrm/rcp/rsq/pow/cmp/branch`) |
| 24 | 8 | `lighting` | `032687db8217` | ps | — | PS `def` literal rewrite |
| 25 | 8 | `lighting` | `32e4418ba35e` | ps | — | PS `def` literal rewrite |
| 26 | 8 | `lighting` | `3d8aaa6bb2f9` | vs | `lit.vs.3d8aaa6b` | wider varying loader (2nd/3rd texcoord remap) |
| 27 | 8 | `lighting` | `4445b3f44030` | vs | `lit.vs.4445b3f4` | wider varying loader + dropped colour1 |
| 28 | 8 | `lighting` | `5e5c1dfefb3d` | ps | — | PS `def` literal rewrite |
| 29 | 8 | `lighting` | `5fb795186c47` | ps | — | PS `def` literal rewrite |
| 30 | 8 | `lighting` | `6682cb26c1f0` | ps | — | PS `def` literal rewrite |

The next VS-only work is therefore **not** opcode work: it is a runtime loader
that binds more than one texcoord attribute/varying and a PS-side remap that
matches it. The next large block is **PS** work (literal rewrite, then a
dataflow scheduler), which is why no further world draw was promoted this
round: each would have been a half-path.

## 4. Holes found and closed

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
   It also preserves committed `emitted-verified` rows (and their pinned
   `.shbin`/promotion) so a regeneration cannot silently undo a runtime path;
   `pack_plans.py --check` reproduces the committed catalog byte-for-byte.
3. **105 silent skinning PICA VS rows.** They are `emitted-verified` with a
   pinned `.shbin` but are not one of the three certified runtime
   representatives. Their notes now say why.
4. **Stale lighting reason.** 124 off VS notes claimed the runtime contract
   “only carries skinning and GFx menu programs”, which stopped being true when
   the six lighting reps landed. The note now names the six lighting reps.
5. **Counter/manifest drift.** The gate joins every manifest plan row (kind +
   draw status) and re-derives the per-pack coverage `draw_enabled` and
   `unique_hashes`, so a hand-edited plan cannot leave a stale counter behind.
6. **The most frequent world shader had no artifact.** All eight `blit_post`
   `pica_vs` rows were `codegen: pending`. `tools/blit_vs_emit.py` now emits,
   numerically verifies and pins them; six are runtime-enabled because the
   existing loader can bind them unchanged. The two that cannot are still
   emitted and carry the exact missing loader feature in their note.

## 5. Honest remaining gaps

These are documented refusals/offline plans, not hidden draw paths:

* `terrain` — 138 port-lite TEV plans (`quality: approx`) are draw 0; 109
  hashes have no plan (`needs_multipass` 103, `stage_budget` 6).
* `sky_water` — all 45 TEV plans are draw 0; this is the pack the menu audit
  deliberately demoted (catalog-only, no shared runtime slot).
* `lighting` — 123/130 PICA VS programs are emitted-verified but outside the
  seven-slot runtime catalog; 107 TEV plans are offline recipes; 4,754 hashes
  have no plan, including 22 VS with 8–9 varyings
  (`too_many_vs_varyings`). Promoting more VS needs the multi-texcoord loader
  above, not a new ALU opcode.
* `skinning` — 104/108 PICA VS are emitted-verified but outside the four-slot
  runtime catalog; 94 TEV plans need the material contract; 992 hashes have no
  plan. The 16-chunk PS pair `06aa117c`/`14db34d7` needs a dataflow scheduler
  (dependent texture read), not a single missing opcode.
* `blit_post` — 59 TEV plans are offline; the six exact copies remain
  catalog-only. Two PICA VS (`4d155cd6`, `c43418bb`) need the wider varying
  loader. The family's 55 chunks of fullscreen-quad VS now have a runtime path.

## 6. Verification

```
python tools/shader_packs.py check                     # 6 packs, 6659 hashes, OK
python source/shader_packs/blit_post/pack_plans.py --check
                                                       # catalog reproducible
python tools/shaders/world_runtime_audit.py            # 18/18 world draws, 0 holes
python tests/world_shader_audit_test.py                # 21 checks, 0 failures
python tests/blit_vs_runtime_test.py                   # 108 checks, 0 failures
python tests/vs_wide_runtime_test.py                   # 33 checks, 0 failures
python tests/pica_runtime_audit_test.py                # 136/136, 38 VS programs
python tests/tev_sched_test.py                         # 10 tests, OK
python tools/shaders/plan_verify.py --data E:\SteamLibrary\steamapps\common\Skyrim\Data
                                                       # 115 entries, symbolic pass
tools/run_host_tests.ps1 -Only pica_vs_runtime         # 280 checks passed
```

`lift`/`com` could not be built in this worktree because the generated
`staged/fpu.c` is absent (`run_host_tests.ps1` resolves it at compile time);
`staged/` is produced by `tools/stage_upstream.py` and was deliberately not
touched here. The compile failure is a missing generated input, not a shader
regression.
