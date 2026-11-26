# SM3 gate: first-main-menu shader closure (scratch investigation)

Status: **scratch investigation, now partially connected at runtime.**
The investigation artifacts live in `C:\Skyrim_N3DS_build\agent_work\sm3`.
`GetDeviceCaps` is not changed and no fake SM3 flag exists.

> **Runtime update (2026-11-26, branch `menu-shaders`).** The bounded selector
> is now wired into `CreatePixelShader` as
> `source/d3d9_menu_shader_runtime.{c,h}`: exact SHA-256 selection of
> `menu.logo.ps` (p1200005 five-stage plan) and `menu.composite.ps` (one-stage
> copy), plus explicit fixed fog/tint contract plans. Material preconditions
> and the PICA backend gap are named draw refusals; no partial draw. The
> `vs_3_0` path, automatic material selection and the other ~6.6k blobs remain
> outside. See `docs/MENU_SHADER_RUNTIME.md` for the exact boundary and tests.
>
> `D3DXCompileShader` is **not** the SM3 menu path and is unchanged: named
> `E_NOTIMPL`, cleared outputs, no trap. Trace12 shows only two import
> bindings and no call before the SM3 gate; TESV uses precompiled FXP/bytecode,
> and the SM3 work is in `CreatePixelShader`/`CreateVertexShader`.

## 1. Static gate trace

The renderer init function is `0xc6dbf0`.

- `0xc6dc97 -> 0xc6d510`: adapter/version scan. The candidate record exposes
  VS/PS version words at `[esi+0xc4]` / `[esi+0xcc]`; `0xc6d8c8` keeps the
  highest `low16` values in `0x12cf5a0` / `0x12cf59c`.
- `0xc6dddb -> 0xcdb620`: second factory/device wrapper. This is before the
  SM3 check. Existing evidence puts `CreateDevice` at `0xcd8890` / slot 16
  `0xcd8b72`.
- `0xc6e0a7 -> factory slot 10 (CheckDeviceFormat)`: `0x71`
  (`A16B16G16R16F`), usage `0x00080000`.
- `0xc6e21e -> 0xc71080`: computes a shader level from the two version words.
  `0xc71080` compares `ah`/`ch`: both >= 3 -> level 7; both >= 2 -> level 2;
  otherwise 1/0. Result is stored at `0x01ba749c`.
- `0xc6e22d`:
  ```asm
  00c6e22d  cmp dword ptr [0x01ba749c], 7
  00c6e23b  jge 0xc6e247
  00c6e23d  push 0x114e04c
  00c6e242  jmp 0xc6dfd3          ; renderer failure
  ```
- After the gate: `ATOC` / `RESZ` probes at `0xc6e26b` / `0xc6e293` /
  `0xc6e2c0` / `0xc6e328`, then `0xc6e2cc -> 0xc6cc40` and
  `0xc6e302 -> 0xc72350`.

So reporting VS/PS 3.0 would only satisfy this branch; it would not make the
device accept arbitrary ps_3_0 programs.

## 2. Package / technique path

Static strings and functions:

| Item | Address / value |
|---|---|
| `shadersfx\%s\%03d\P0%s.fxp` | `0x01156e5c` |
| `shadersfx\%s\%03d\V0%s.fxp` | `0x01156e40` |
| generic package loader | `0xcbe070` (checks `"BSFX"`), wrappers `0xcbe1b0` / `0xcbe260` |
| lighting index function | `0x00ca8e70` returns `0x3b` = **59** |
| `"Lighting"` / `"P%X.fxp"` | `0x01154a7c` / `0x01154a88` |
| menu/global package preload | `0x00ca38d0`, called from `0xc74bf1` |
| package ref/load helper | `0x00ca31f0` |

`0x00ca38d0` pushes **124 keys** into `0x00ca31f0`, then returns. They cover
the shipped lighting package set plus the four special keys
`0x00c00001`, `0x00c00003`, `0x01800001`, `0x00800807`. The full key list is
recorded in `menu_closure_manifest.json` under `engine_evidence.preload_keys`.
`p1`, `p5`, `p7` are `push 1/5/7` (no `0x` prefix in the disassembly).

`0x00ca9230` extracts the lighting key class from shader flags:

```text
mask 0x0009000f, optional 0x70 -> 0x10, optional 0x41400 -> 0x400,
then mask 0x07e00000 and OR
```

For the logo NIF flags:

```text
flags1 = 0x82400301
flags2 = 0x00008021
ca9230(flags1) = 0x02400001
```

Code the base-lit package keys. Exactly one shipped base-lit package carries
the corresponding high bits `0x01000000|0x00200000` with the `0x5` low
stage/variant bits:

```text
0x01200005 -> shadersfx\lighting\059\p1200005.fxp
```

This is the measured key-class match; the final composition of the key from
the masked word is inferred from the key set, not from a decoded symbol.

## 3. Exact first-menu shader closure

### 3.1 Pixel shaders (bytecode-backed)

| Role | Package | Index | Key | Stage | Bytes / instr | Signature | SHA-256 |
|---|---|---|---:|---|---|---|---|
| Logo lit PS | `shadersfx\lighting\059\p1200005.fxp` | 059 | `0x01200005` | PS ps_3_0 | 852 B / 25 | `p1200005_ADD_NORMAL` | `8725393bf3ed950789e50501893ab141efdfd0377309271f6ef7493239be5ff9` |
| Menu composite/copy | `shadersfx\imagespace002.fxp` | 002 | `0x00000000` | PS ps_3_0 | 184 B / 3 | `blit_texld` (`dcl;dcl;texld oC0,v0,s0`) | `6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de` |

`p1200005` CTAB (exact set, 8 names):

```text
DiffuseSampler, NormalSampler,
DirLightColor, DirLightDirection, DirectionalAmbient,
EmitColor, FogColor, MaterialData
```

Declarations: `texcoord0 v0`, `color0 v1`, `color1 v2`, `2d s0`, `2d s1`.
The only literal is `def c8 {-0.5, 1, 0, 0}`. No branches. The 25-opcode
sequence is stored verbatim in `menu_closure_manifest.json`.

The copy chunk is one of two identical 184-byte PS chunks at offset `4534` in
`imagespace002`; both match. `shaders001` contains four more 188-byte copies
with keys `0x00000002`, `0x04000002`, `0x04000003`, `0x0c000003`.

### 3.2 Base-lit class (why `p1200005`)

The exact 8-constant class occurs in 28 chunks / 5 distinct SHA-1s:

| Package | Key | Bytes | Instr | SHA-1 prefix |
|---|---:|---:|---:|---|
| `p1` | `0x1` (`0x8081`, `0x8101`, `0x8181`, `0x8201`) | 936 | 31 | `e71524e515e5` |
| `p5` | `0x5` (plus `0x8085`, `0x8105`, `0x8185`, `0x8205`) | 840 | 24 | `585feb16c65c` |
| `p7` | `0x7` (plus variants) | 936 | 31 | `f057b25ebd8e` |
| `p20001` | `0x20001` (plus variants) | 936 | 31 | `e71524e515e5` |
| `p1200005` | `0x1200005` | 852 | 25 | `fc143f74d6aa` |
| `p1800001` | `0x1800001` (plus variants) | 920 | 30 | `4847ed7e38b1` |
| `p1a00001` | `0x1a00001` | 920 | 30 | `4847ed7e38b1` |
| `p1e00001` | `0x1e00001` | 920 | 30 | `4847ed7e38b1` |

Only `p1200005` has the `0x01000000|0x00200000` high-bit class without the
`0x00800000` bit, matching the logo flag class.

### 3.3 Vertex shader family

Lighting packages are PS-only. The logo/copy VS comes from the generic
`shadersfx\shaders001.fxp` family. The manifest contract requires a shipped
`vs_3_0` program with:

```text
dcl position0, texcoord0, color0, color1
outputs o0, o1, o2
```

The base archive contains **390** matching VS chunks (e.g. key `0x00000000`,
1528 B, 67 instructions). The exact runtime VS key is selected by the engine's
vertex-declaration path, so the contract is a bounded family, not one key.
The Update archive has `shaders002.fxp` as an optional override.

### 3.4 Fixed pipelines (no `.fxp`)

| Role | Why no bytecode | Contract |
|---|---|---|
| Menu fog/smoke | `BSEffectShaderProperty` has no shipped `.fxp` | 2 texture units, vertex colour, straight alpha blend, depth test no write, <=3 TEV stages; `FXSteamThinAnim.dds` + `GradSteamThin.dds` |
| Scaleform UI | `Interface.bsa` / SWF, not `.fxp` | 1 texture, bilinear, straight alpha blend, depth test no write, 1 TEV stage |

## 4. Truthful gating policy

**Reporting VS/PS 3.0 with named per-shader rejection is not defensible as a
general D3D9 capability.** A PS3.0 device must accept valid ps_3_0 programs;
failing most of them is a capability lie even if each failure is named.

Recommended policy:

1. Keep `GetDeviceCaps` at VS 2.0 / PS 2.0 and zero SM3 slots.
2. Add a separate **named menu shader-set contract**,
   `d9_menu_shader_closure_ready()`, backed by this validator/manifest.
3. Only if every required entry passes may a TESV-specific compatibility gate
   satisfy the `0xc6e22d` check. That gate must log
   `menu_shader_closure=certified` and must not change `GetDeviceCaps`.
4. If any shader outside the closure is created, invalidate the closure and
   return `D3DERR_NOTAVAILABLE` with the exact named reason. Do not silently
   fall back to a generic shader or a fake success.
5. If the closure is not certified, let the renderer fail at `0xc6e22d`.

This is a compatibility contract, not a D3D9 capability claim.

## 4.1 Exact integration points (status 2026-11-26)

| Live file / point | Status |
|---|---|
| `source/d3d9_com.c`, `CreatePixelShader` | **Connected.** `d9_menu_shader_match_ps()` gates on the certified hash, then builds the plan. Anything else stays on the legacy path; an armed closure still invalidates by name. |
| `source/d3d9_shader.c`, `d9_shader_analyze_tev` | Unchanged. The menu plan is selected before the generic analyzer and stored separately on the object. |
| `source/d3d9_renderer_requirements.c` | Unchanged. The closure predicate remains a non-D3D9 answer; it is not mapped to a caps success. |
| `source/d3d9_menu_light.c` | **Used by the runtime.** `menu.logo.ps` must match `p1200005_ADD_NORMAL`; draw is refused until the normal/basis/alpha/constant preconditions are reported. |
| TESV compatibility gate `0x00c6e22d` | Still not flipped. It may consult only `d9_menu_closure_predicate()`; `GetDeviceCaps` stays 2.0 and `D3DXCompileShader` stays `E_NOTIMPL`. |
| `source/d3d9_capture.c` / first-draw capture | **Not connected.** The selector exposes the entry id but capture does not record it yet. |
| Fixed fog/tint | **API only.** Explicit contract plans with named precondition refusals; no material loader calls them yet. |

## 5. Artifacts

| File | Purpose |
|---|---|
| `disasm_tesv.py`, `find_calls.py`, `scan_refs.py`, `containing_function.py` | read-only TESV disassembly helpers |
| `census_menu.py` | real menu shader census/extractor over `Skyrim - Shaders.bsa` |
| `menu_census.json` | derived metadata: 28 base-lit chunks, exact p1200005/copy details |
| `build_menu_manifest.py` | builds the closure manifest from the census |
| `menu_closure_manifest.json` | exact closure contract + 124 preload keys + engine evidence |
| `menu_shader_validator.py` | validator/closed-set gate |
| `menu_closure_report.json` | current real-archive report (status `pass`) |
| `test_menu_closure.py` | 15 tests (13 synthetic + 2 real-archive) |
| `MENU_CLOSURE.md` | this report |

## 6. Tests / results

```text
python C:\Skyrim_N3DS_build\agent_work\sm3\test_menu_closure.py
Ran 15 tests in 0.635s
OK
```

Real-archive report:

```json
{
 "status": "pass",
 "required_failed": [],
 "entries": [
  {"id":"menu.logo.ps","key":"0x01200005","status":"pass"},
  {"id":"menu.composite.ps","key":"0x00000000","status":"pass"},
  {"id":"menu.fog.fixed","status":"pass"},
  {"id":"menu.ui.scaleform_fixed","status":"pass"},
  {"id":"menu.generic.vs","matches":390,"status":"pass"}
 ]
}
```

Negative tests cover wrong key, wrong hash, wrong stage, wrong constant set,
wrong opcode sequence, wrong declaration, missing fixed-pipeline state/reason,
and a VS family that lacks required outputs.

## 7. Confidence / unknowns

| Claim | Confidence |
|---|---|
| `0xc71080` computes level 7 from VS/PS 3.0 and `0xc6e22d` requires 7 | **High** (direct decode) |
| `CreateDevice` precedes `0xc6e22d` | **High** (call order + existing `0xcd8890` evidence) |
| `p1200005` is the base-lit PS with the logo flag class | **High** (exact CTAB/instr class + unique key-class match) |
| `p1200005` is the only possible logo PS | **Medium-high** — the final flags-to-key lookup is inferred from the key set, not executed |
| Exact runtime generic VS key | **Unknown** — selected by vertex declaration; 390 valid candidates |
| Menu fog/UI fixed pipelines | **High for “no .fxp”**, medium for exact TEV stages/state |
| A rendered first menu frame | **Not claimed** — no frame was produced |

The next honest step is not a fake SM3 caps flip. It is to port this closure
validator into the renderer as `d9_menu_shader_closure_ready()`, then make the
TESV gate use that named result while keeping `GetDeviceCaps` at 2.0.
