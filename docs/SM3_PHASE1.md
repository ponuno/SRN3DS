# SM3 phase 1: one IR, one plan schema, six shader packs

Status: **host-only infrastructure. No runtime SM3 translation, no capability
change, no new draw and no rendered frame is claimed.** `GetDeviceCaps` is
still `DevCaps 0x00082000`, VS 2.0, PS 2.0, zero SM3 slots,
`MaxVertexShaderConst 4`. This phase exists to make six family agents able to
work in parallel on a measured, shared baseline instead of guessing.

## 0. Results at a glance

| Quantity | Value |
|---|---:|
| Chunks dumped through MojoShader | **10,475 / 10,475** (9,621 PS + 854 VS) |
| Parse failures | **0** |
| Instructions decoded | **1,019,869** (987,684 PS + 32,185 VS) |
| Distinct bytecode SHA-256 | 6,659 (6,350 PS + 309 VS) |
| Distinct instruction streams (census definition) | 6,655 |
| Field-level divergences vs `tools/shaders/asm.py` | **0** over 6,659 unique streams |
| Plan entries | 4 (3 `translated`, 1 `draw-enabled`) |
| Plan schema errors | 0 |
| Symbolic regeneration vs committed manifest | identical, 4/4 entries |
| Packs | 6, disjoint and exhaustive: 10,475 chunks / 6,659 hashes |
| ARM compile-only | unchanged (see section 8) |

The headline is the third row: the earlier "8,921 of 9,621 chunks differ" was a
counting artefact. Compared field by field, MojoShader and the project decoder
agree on **every** retail chunk. That makes MojoShader a safe reference IR for
the six agents rather than a second opinion.

## 1. MojoShader, host only

`tools/mojoshader/` is a verbatim vendor of
[MojoShader](https://github.com/icculus/mojoshader) at commit `ad5dff84830c`,
zlib licence, notice kept in `LICENSE.txt` and in the file headers. Vendored
files: `mojoshader.c`, `mojoshader.h`, `mojoshader_internal.h`,
`mojoshader_common.c`, `profiles/mojoshader_profile.h`,
`profiles/mojoshader_profile_common.c`, `profiles/mojoshader_profile_d3d.c`.
Only `SUPPORT_PROFILE_D3D` is compiled.

It is **never** linked into the 3DS binary: the only consumer is
`tools/sm3_dump.c`, built by `tools/build_sm3_dump.ps1` into
`E:\host_objcache\sm3\sm3_dump.exe`. `tools/sm3_dump.c` reads the project chunk
format (`u32 count; count * {u32 size; bytes}`) and writes a framed IR stream
(`SM3IR1\0`, count, then `{index, status, instructions, text_len, d3d text}`).
`tools/sm3_corpus.py` produces the chunk dumps and the hash/family index from
the retail `Skyrim - Shaders.bsa` read-only. Nothing retail-derived is
committed; `E:\host_sm3\` holds the corpus, the IR and the reports.

Measured, reproduced exactly against `docs/PICA_RESEARCH.md` section 3.1:

```text
PS: chunks=9621 parsed_ok=9621 parse_fail=0 total_instructions=987684
VS: chunks=854  parsed_ok=854  parse_fail=0 total_instructions=32185
```

Parser coverage is not translation coverage. It says every retail token stream
is understood; it says nothing about a draw.

## 2. Field-level differential (`tools/shaders/differential.py`)

`instruction_count` is never compared. Both sides are decoded into the same
normalised record and compared instruction by instruction:

* opcode;
* destination register type, index, write mask and destination modifier
  (`_sat`, `_pp`, `_centroid`, including the stacked `_sat_pp` form);
* each source's register type, index, 4-component swizzle (MojoShader trims
  trailing duplicates: `.zyw` is `zyww`), source modifier (negate, bias, sign,
  complement, `_x2`, `_abs`, `_not`) and relative-addressing tuple
  (register type, index, component);
* comparison function for `ifc`/`breakc`, which lives in the instruction token
  bits 16-23, not in an operand;
* `texldp`/`texldb` instruction control bits (projected/biased `texld`);
* `def`/`defi` literals, as a per-register multiset with a float tolerance;
* `dcl` declarations as a multiset of (register, usage, usage index, sampler
  texture type).

Declarations and literals are compared out of band; the executable body is
compared positionally. Result over the full corpus:

```text
chunks 10475, unique_streams 6659, unique_equal 6659, unique_differ 0
chunks_equal 10475, chunks_differ 0
```

### 2.1 What the differential actually found

Reaching zero required modelling six behaviours; each is a real property of the
bytecode or a real gap in the project decoder, and each would otherwise look
like a divergence:

1. **Stacked destination modifiers.** D3D allows `_sat` and `_pp` together;
   MojoShader prints `mul_sat_pp`. `asm.py` stores the raw modifier field, so
   the differential ORs the modifier bits instead of matching one suffix.
2. **Swizzle trimming.** MojoShader prints the shortest equivalent swizzle;
   `asm.py` always carries four components.
3. **`ifc`/`breakc` comparison.** The comparison is in the opcode token, and
   the encoding is `D3DSHADER_COMPARISON` (`LT = 4`), **not** `D3DCMP`
   (`LT = 3`). Using the wrong table reported 2,197 phantom mismatches.
4. **`texldp`/`texldb`.** These are `TEXLD` with control bits
   (`D3DSI_TEXLD_PROJECT = 0x01`, `BIAS = 0x02`) in the opcode token.
   `asm.py` now exposes that word (`Instruction.control`, `projected`, `biased`)
   and names the instruction `texldp`/`texldb`, so the differential and the
   planner both see it. It previously decoded as a plain `texld`.
5. **No-destination opcodes.** `asm.parse_program()` used to wrap operand 0 as
   a destination for every opcode, so `if`, `ifc`, `loop`, `rep`, `label`,
   `breakc`, `else` and `endif` carried a bogus destination. `NO_DST_OPCODES`
   now decodes those operands as sources, and `dst is None` in the IR.
6. **Relative tokens are not operands.** A relative-addressed source is followed
   by one extra relative token. `asm.py` now swallows it and attaches the full
   `(base, index register, component, offset)` tuple to the source instead of
   counting it as a second source.

Items 4-6 are now fixed in `asm.py`; the differential compares the exposed
fields instead of compensating, and `compile.py` refuses projected/biased
sampling and relative addressing by name rather than silently mis-planning
them. The measured result is unchanged: zero field divergences.

## 3. One plan format, one verifier

`docs/shader_plan_schema.json` (`d9-shader-plan-v1`) is the single contract for
pixel and vertex plans. `tools/shaders/plan_verify.py` enforces three layers:

1. **Schema** — required fields, lower-case 64-hex SHA-256, stage/kind
   agreement, TEV stage data ranges, PICA VS output/input/uniform limits. The
   verifier loads the schema file so the two cannot drift.
2. **Semantics** — status is *derived*: `draw_enabled == 1` is
   `draw-enabled`, a descriptor with `draw_enabled == 0` is `translated`, a
   census hash with no entry is `refused`. Unique ids and hashes, stage count
   vs `stage_data`, constant references, sampler types, and "a PICA VS is never
   draw-enabled without native codegen" are checked. Writing the schema this
   way found a real bug in the first draft: TEV RGB selectors are
   `{0,1,2,3,4,5,8,9,12,13}` (2-bit component steps), not `0..7`.
3. **Symbolic equivalence** — with `--data`, the manifest is regenerated from
   the retail archive by `tools/fxp_compile.py` and compared field for field.
   The generator only emits a TEV plan after its own symbolic check of the
   final `oC0` expression against the IR, so an identical regeneration means
   the committed plan is still the IR's plan.

```text
entries 4, status {translated 3, draw-enabled 1, refused 0}
schema_errors [], symbolic pass, ok true
```

## 4. P0: the menu-text plan no longer exists twice

`core_text_plan()` in `source/d3d9_core.c` (the live A8 plan) and
`d9_tev_text_plan()` in `source/d3d9_tev.c` (the stale
`MODULATE(TEXTURE0,PRIMARY)` duplicate) now both call one inline builder,
`source/d3d9_text_plan.h`:

```text
stage 0: RGB   = REPLACE(PRIMARY)                 -- vertex colour, A8 RGB is 0
         ALPHA = MODULATE(TEXTURE0.a, PRIMARY.a)  -- coverage * vertex alpha
```

The header keeps the `MENU_DEBUG_SOLID` / `MENU_DEBUG_A8_RED` measurement
branches in the core only. `tests/tev_text_plan_smoke.c` now asserts the A8
shape on the plan and on the PICA descriptor. The old plan was not equivalent:
A8 decodes to `(0,0,0,A)`, so `MODULATE(TEXTURE0,PRIMARY)` emitted black RGB.

## 5. Six packs, disjoint and exhaustive

`tools/shader_packs.py` partitions the census by family into the packs the six
agents will own. Manifests live in `source/shader_packs/<pack>/manifest.json`
with one row per bytecode SHA-256 (kind, census class, chunk count), plus a
`README.md` skeleton (owner, first step, coverage).

| pack | families | chunks | hashes | translated | draw-enabled | refused | contract-only |
|---|---|---:|---:|---:|---:|---:|---:|
| `ui` | ui/interface | 314 | 113 | 0 | 0 | 113 | 40 |
| `blit_post` | blit/copy, post-process/imagespace | 131 | 69 | 1 | 1 | 67 | 8 |
| `sky_water` | sky, water | 82 | 45 | 0 | 0 | 45 | 0 |
| `terrain` | terrain/landscape, grass/trees | 327 | 247 | 0 | 0 | 247 | 0 |
| `skinning` | skinning/characters | 2,162 | 1,194 | 0 | 0 | 1,194 | 97 |
| `lighting` | lighting/deferred/shadows, particles/effects | 7,459 | 4,991 | 2 | 0 | 4,989 | 130 |
| **total** | | **10,475** | **6,659** | **3** | **1** | **6,655** | **275** |

`contract-only` counts a `pica_vs` census hash that fits the PICA VS envelope
but has no committed descriptor; it is a subset of `refused`, not a fourth
status.

`source/d3d9_shader_pack_table.inc` is generated **append-only**: an existing
pack row is never rewritten, a new pack is appended. It is exposed by
`d9_shader_pack_count()` / `d9_shader_pack_at()` and covered by
`tests/shader_plan_table_smoke.c` (85 checks, including the partition totals
10,475 / 6,659 and exactly one draw-enabled plan).

## 6. Coverage and what still needs a scheduler

`docs/shader_coverage.json` (generated) carries the per-pack breakdown. The
refusals are not uniform, and that matters for planning:

* **`def_literal_out_of_range` dominates** — 4,804 of 4,991 lighting hashes,
  1,044 skinning, all 247 terrain, 39 of 45 sky/water. A single lever (literal
  range rewriting / subtract formulations) touches most of the archive, but the
  census already showed most of those chunks also use `cmp`/loops/`rcp`.
* **`unsupported_opcode`** — concentrated in `ui` (37) and skinning (42):
  `cmp`, `if`/loop, `rcp`, derivatives, `texldd`.
* **`dataflow`** — 18 blit/post and 17 ui hashes need more than the single
  PICA `PREV` register, i.e. a scheduler or the software route.
* **VS varyings** — 26 skinning and 17 lighting hashes declare 8-9 varyings
  against PICA's 7; these need packing, dropping or a multi-pass plan.
* **Non-uniform swizzles / multiple `oC0` writes** — 5 and 2 chunks; these
  need per-channel TEV selectors and a channel-carry plan.

Nothing in this list is a capability claim. Each is a named next step with a
hash list in the owning pack.

## 7. Tests

| Test | Result |
|---|---|
| `tests/sm3_phase1_test.py` (15 tests) | PASS |
| `tests/shader_plan_table_smoke.c` | PASS, 85 checks |
| `tests/tev_text_plan_smoke.c` | PASS |
| `tests/menu_text_plan_draw_smoke.c` | PASS |
| `tests/tev_real_smoke.c` | PASS |
| Full host suite | **145 passed, 0 diagnostic, 0 failed** |
| ARM compile-only (`d3d9_core.c`, `d3d9_tev.c`) | rc 0, no new warnings |

`sm3_phase1_test.py` covers the differential field model (including each trap
in section 2.1), the schema rules and tampering (status contradiction,
duplicate hash, draw-enabled PICA VS), the pack partition, and — when
`E:\host_sm3\differential.json` exists — asserts the measured report has zero
divergences.

Reproduce the host side:

```powershell
python tools\sm3_corpus.py --out E:\host_sm3
powershell -File tools\build_sm3_dump.ps1 -OutDir E:\host_objcache\sm3
E:\host_objcache\sm3\sm3_dump.exe E:\host_sm3\ps_chunks.bin E:\host_sm3\ps_ir.bin
E:\host_objcache\sm3\sm3_dump.exe E:\host_sm3\vs_chunks.bin E:\host_sm3\vs_ir.bin
python tools\shaders\differential.py --corpus E:\host_sm3
python tools\shaders\plan_verify.py --data "E:\SteamLibrary\steamapps\common\Skyrim\Data"
python tools\shader_packs.py check
```

## 8. Environment notes and remaining blockers

* Full host suite: `powershell -File tools\run_host_tests.ps1 -OutDir
  E:\host_objcache\sm3\full` reported **145 passed, 0 diagnostic, 0 failed**.
* ARM compile-only: both changed translation units pass
  `arm-none-eabi-gcc -march=armv6k -mfloat-abi=hard -mtp=soft -std=gnu11
  -D__3DS__ -Isource -I<libctru> -fsyntax-only` (rc 0; only the pre-existing
  `-Wmisleading-indentation` style warnings from the formatted core).
* `tests/menu_text_plan_draw_smoke.c` needs the git-ignored `staged/` lift
  sources; a fresh worktree must copy them from the main tree before the suite
  can build that test. This is an environment prerequisite, not a code change.
* The three decoder gaps in section 2.1 are closed in `tools/shaders/asm.py`:
  `texld` carries its instruction control word (`texldp`/`texldb`), relative
  sources carry the full `(base, index reg, component, offset)` tuple and no
  longer add a phantom source, and branch/loop opcodes have no destination.
  `tools/shaders/differential.py` compares the exposed fields directly and
  still reports 10,475/10,475 chunks and 6,659/6,659 streams equal. The TEV
  planner now names projected/biased sampling and relative addressing as
  refusals instead of mis-reading them (`tests/sm3_asm_gaps_test.py`).
* The retail archive is only read by host tools. No capability flag, no SM3
  advertisement, no silent fallback and no new draw path were added.

## 9. Ready for six agents

Each agent gets a pack directory with its hash list, its refusal classes and a
first step, plus shared generated files that are append-only:

| agent | pack | first step |
|---|---|---|
| ui | `source/shader_packs/ui` | re-plug the certified A8 text plan; keep draw blocked |
| blit_post | `source/shader_packs/blit_post` | certify one-stage copy plans, then full-screen passes |
| sky_water | `source/shader_packs/sky_water` | map water refraction/reflection samplers to TEV stages |
| terrain | `source/shader_packs/terrain` | reduce 6-way multitexture to the 6-stage TEV budget |
| skinning | `source/shader_packs/skinning` | land the VS skinning contract before any PS work |
| lighting | `source/shader_packs/lighting` | split the normal/basis `dp4` prebake out of the PS plan |

Rules that keep them from colliding: edit only your pack directory; add plan
entries and pack rows, never rewrite another pack's; regenerate
`docs/shader_coverage.json` with the tool, not by hand; and re-run
`plan_verify.py --data` before claiming a plan is equivalent.

### 9.1 Pack catalogs vs the runtime table

A pack has two levels, because the runtime table is capped at
`D9_SHADER_PLAN_MAX_ENTRIES == 16` (`source/d3d9_shader_plan.h`) while a pack
catalog can honestly hold hundreds of entries:

| file | role | limit |
|---|---|---|
| `source/shader_packs/<pack>/plans.json` | every plan the pack owns, schema-identical entries | none |
| `source/shader_packs/<pack>/refusals.json` | named refusal class per hash | none |
| `docs/shader_plans.json` | runtime lookup table (`d9_shader_plan_at`) | 16 |

`fxp_compile.py manifest` discovers every pack `plans.json` and merges only the
entries marked `"runtime": 1`; the rest stay catalog/coverage-only and never
consume a runtime slot. The merge is append-only: a byte-identical duplicate is
idempotent, a hash or id clash is a named error, and the command refuses to
write more than 16 entries instead of silently overflowing the C table.

```powershell
python tools\fxp_compile.py manifest --data F:\Skyrim\Data `
    --out docs\shader_plans.json
python tools\fxp_compile.py emit-c
python tools\shader_packs.py coverage --out docs\shader_coverage.json
python tools\shader_packs.py check
python tools\shaders\plan_verify.py --data F:\Skyrim\Data
```

`shader_packs.py` indexes the runtime file plus all pack catalogs, so a
translated-but-not-runtime plan still counts as `translated` in coverage.
`plan_verify.py` validates every catalog entry against
`docs/shader_plan_schema.json` and merges only `runtime` entries into its
symbolic regeneration, so the committed runtime table must still equal the
generator's output exactly. Regenerating against the retail archive reproduces
the committed 4-entry manifest byte-for-byte.

Coverage totals count census hashes only. Plans whose SHA-256 is not in the
census — GFx's built-in menu programs live in `TESV.exe` tables, not in the
shader BSA — are reported in `docs/shader_coverage.json` under `extra_plans`,
so the census `draw-enabled: 1` cannot hide three certified menu descriptors.
