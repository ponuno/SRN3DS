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

* `menu_plans.json` — 5 descriptors (3 `pica_vs`, 2 `tev`) and 5 named
  refusals, one row per measured prefix hash, with both the measured prefix
  hash and the true full-blob SHA-256;
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

### Coverage of the ten measured groups

| Status | Count |
|---|---:|
| translated (3 VS `pica_vs` + 2 PS `tev`) | **5** |
| draw-enabled | **0** |
| named refusal | **5** |

Draw stays blocked for every entry:

* PICA VS descriptors cannot be draw-enabled before native codegen (schema
  rule).
* The two TEV plans (`mov oC0,c0`; `tex0*c2+c3`) are symbolically equal to the
  IR, but the c0/c2/c3 material binding has not been measured, so
  `material_contract.complete` is false.

Named refusals: `01fd3eb5` (temp written twice / alpha in a second varying),
`b527a010` (`lrp`), `8ced9de9` (`dp4` normal basis needs prebake),
`1b279738` and `cd1c433b` (YUV literals outside the 8-bit TEV range and
dataflow).

### Verify

```text
python source\shader_packs\ui\menu_plan_tool.py check --tesv <TESV.exe>
python source\shader_packs\ui\test_menu_plans.py --tesv <TESV.exe>
```

`check` replays each TEV descriptor to its final `oC0` expression and compares
it with the IR expression; with `--tesv` it re-extracts the blobs, re-runs
`tools/shaders/compile.py` and regenerates the plans from retail bytes.

### Shared integration still needed

These 10 plans are pack-local on purpose: `tools/fxp_compile.py` only reads
`Skyrim - Shaders.bsa`, so `plan_verify.py --data` would report them as
"regenerated manifest has no plan" and the append-only
`source/d3d9_shader_plan_table.inc` would not match.  Requested shared change:

1. `build_manifest()` merges `source/shader_packs/<pack>/menu_plans.json`
   (validated by the pack verifier) and `plan_verify.py` treats those entries as
   symbolic-pass when the pack verifier passes.
2. `tools/shader_packs.py` coverage ignores hashes outside the census (already
   the case) so the pack table row stays `0 draw-enabled`.
