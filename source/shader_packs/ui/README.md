# Shader pack `ui`

Owner: `sm3-agent-ui`

menu/UI pixel and vertex shaders: A8 atlas copies, text tint, simple colour math

* census families: `ui/interface`
* chunks: **314** / unique hashes: **113**
* coverage: 0 translated, 0 draw-enabled, 113 refused
* first step: re-plug the certified A8 text plan; keep draw blocked

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.

## StartMenu / GFx priority set (phase 2)

`docs/MENU_SHADERS.md` names 10 hashes created on the way to
`Interface/StartMenu.swf`: 5 VS objects and 40 PS objects.  Those blobs are not
in `Skyrim - Shaders.bsa`; they are in `TESV.exe` in the GFx renderer tables
(VA `0x12dfca8`, 6 slots, and VA `0x12dfcd8`, 45 slots).  `menu_plan_tool.py`
reads them in place (nothing retail is copied) and writes:

* `menu_plans.json` — the ten measured prefix groups (5 descriptors, 5 named
  refusals), with both the measured prefix hash and the true full-blob SHA-256;
* `plans.json` — the shared catalogue, one entry per distinct full blob
  (5 VS + 14 PS translated, 24 PS refused); only exact, fully contracted PS
  set `runtime: 1`;
* `menu_ir.json` — derived IR metadata only (opcode names, declarations,
  symbolic final expression); no bytecode.

### Measured finding: the 10 hashes are not 10 programs

The usage log hashes `high16(second_token) * 4` bytes from offset 0.  In these
D3DX blobs the second token is the leading `CTAB` comment, so that length is
the comment size, not the shader size.  Walking tokens to `END` gives the real
blob.  On the same run:

| | usage-log prefix hashes | distinct full blobs | objects |
|---|---:|---:|---:|
| GFx VS | 3 | **5** | 5 |
| GFx PS | 7 | **38** | 40 |

So the "3 VS + 7 PS unique" statement collapses distinct code.  The 10 plans
here are the 10 *measured prefix groups*; `menu_plans.json` records
`measurement.prefix_hash_defect` and the full-blob counts.  This is a
measurement defect in the log, not a translation result.

### Coverage of the full catalogue (38 PS / 5 VS full blobs)

| Status | VS | PS |
|---|---:|---:|
| translated | **5** | **14** |
| draw-enabled / `runtime: 1` | 0 | **3** |
| named refusal | 0 | 24 |

Runtime entries are exact descriptors with a complete declared material
contract:

* `bfc0b3e0` — `oC0 = c0` (solid colour; c0 is the GFx colour transform).
* `0eb58f00` — `oC0 = tex0 * c2 + c3` (sampler0 2D, c2/c3 colour transform).
* `9c152cf8` — `oC0 = c0` variant with a second stage.

Four PS are declared approximations (`quality: approx`): the `8ced9de9` group
(`ee79ab27`, `95d3d096`, `71d4da27`, `ed5e7b5d`).  TEV has no 4-component dot,
so the 4x4 `dp4` basis is collapsed to one `DOT3` row; the other rows and the
c4 alpha/offset term are dropped.  The plan is exact for that declared spec and
carries the text in `approximation`.

Remaining 24 PS refusals are named: `non_uniform_swizzle: c0.zwzw` (YUV rows),
`too_many_stages (>6)`, `stage_needs_two_constants`, and `plan_mismatch` for
the alpha-recombine patterns that need a second live temp.  These need either a
second pass or a deeper TEV scheduler, not a silent simplification.

### Verify

```text
python source\shader_packs\ui\menu_plan_tool.py check --tesv <TESV.exe>
python source\shader_packs\ui\test_menu_plans.py --tesv <TESV.exe>
```

`check` replays each TEV descriptor to its final `oC0` expression and compares
it with the IR expression; with `--tesv` it re-extracts the blobs, re-runs
`tools/shaders/compile.py` and regenerates the plans from retail bytes.

### Shared integration (main contract `8abe218`)

Plans live in two files:

* `plans.json` — schema `d9-shader-plan-v1`, the catalog the shared tools
  discover.  All five entries have `"runtime": 0`; a translated descriptor does
  not take a slot in the 16-entry runtime table.  Promote an entry to
  `"runtime": 1` only after its material binding and draw path are certified.
* `menu_plans.json` / `menu_ir.json` — the measurement record for all ten
  prefix groups: named refusals, both hash forms, and the IR expression the
  symbolic check compares against.

Checked against this tree:

```text
fxp_compile.py manifest --data F:\Skyrim\Data   -> 4 runtime entries (unchanged)
plan_verify.py --data F:\Skyrim\Data            -> pack_plans ui: 5, ok true
shader_packs.py check                           -> 6 packs, 10475 chunks, OK
```

The ten menu hashes are outside the BSA census, so the aggregate
`shader_coverage.json` totals are unchanged (3 translated / 1 draw-enabled /
6655 refused).  The five translated menu representatives appear in
`plan_verify`'s `pack_plans` and in `plans.json`; the five refusals stay in
`menu_plans.json` because a refusal is not a plan entry.
