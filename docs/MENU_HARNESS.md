# Real StartMenu path in TESV.exe (measurement map)

Status: **static map + harness; no menu frame has been executed or claimed.**
This note is about the retail engine's own Scaleform/GFx menu, not the parallel
SWF reimplementation in `source/menu_swf*`. Binary: `F:\Skyrim\TESV.exe`
(1.9.32.0.8, SHA256 `d97eb0cc...f5e605`). Addresses are VAs in that image.

`tools/menu_harness.py --exe F:\Skyrim\TESV.exe` re-verifies every anchor byte
and prints the harness below; `tests/menu_harness_test.py` runs without game
bytes and is registered in `tools/run_host_tests.ps1`.

## 1. Entry from WinMain to the menu object

| VA | Meaning |
|---|---|
| `0x0069d1d0` | `WinMain` (the whole startup block is inline, `0x69d1d0..0x69de5f`) |
| `0x0069dadf` | reads `[0x1272b38]` = `sIntroSequence:General`; empty -> byte `0x1b3fced = 1` |
| `0x0069db6a..0x69db7c` | if the string is non-empty, `0xf500f0` plays it (Bink `BGS_LOGO.BIK`) |
| `0x010cd560` / `0x010cd570` | `"BGS_LOGO.BIK"` / `"sIntroSequence:General"` |
| `0x0118e070` | `"bPreloadIntroSequence:General"` (default 1) |
| `0x006982c0` | startup-save probe; returns 1 (show menu) on a normal launch |
| `0x0069de08` | `push 1; call 0x876f60` -> open the Main Menu |
| `0x00876f60` | Main-Menu show; creates the singleton via `0x876fc0` and drives `MenuManager` `0x1b10dcc` |

So no real input and no world load are needed to open the menu: the normal
"no startup save" path calls `0x876f60(1)` by itself.

## 2. Where `StartMenu.swf` is loaded

The menu class is registered as **"Main Menu"**; its movie is **"StartMenu"**.

| VA | Meaning |
|---|---|
| `0x0100f0e0` | static init: creates type table `0x12e3548`, registers `"Main Menu"` (`0x12b3a94`) with factory `0x876fc0` via `0xa5d2a0` |
| `0x00876fc0` | factory: allocates `0x24` through `[0x1b40e3c]` vtable slot `0x28`, then calls the ctor |
| `0x00876b90` | ctor: vtable `0x10e6788`, `"StartMenu"` at `+8` (`0x10e67ac`), flags `0x581` |
| `0x00876bc7` | ctor calls `0xa60c80` (CreateMovie) with GFx loader `0x1b2e9b0` |
| `0x00a60c10` | formats `"Interface/%s.swf"` (`0x110e24c`) and resolves it through `0xa5eaa0` |
| `0x00a60c80` | `Menu::CreateMovie`: `0xa60200` (GFx loader CreateMovie), `0xa60500` (setup), viewport/scale, `0xa648b0`/`0xa64a50` |
| `0x00a5ed20` | `BSScaleformFileOpener`; logs `"Error, BSScaleformFileOpener failed to open %s"` (`0x110e0e8`) |
| `0x001b3fc98` | Main Menu singleton (non-NULL after `0x876f60`) |

The file opener reads through the BSA layer, so `Update.bsa!interface/startmenu.swf`
(60,284 B, CRC32 `0x6910248f`) is the asset the engine will ask for. Text records
import fonts 34/41 from `gfxfontlib.swf`; that URL is absent, and
`Interface/fontconfig.txt` + `Interface/fonts_en.swf` are the real provider
(`$EverywhereMediumFont` -> Futura Condensed, `$EverywhereFont` ->
FuturaCondensedLight). GFx itself does the SWF parse and glyph work; the local
`source/menu_swf*` parser is not on this path.

## 3. First GFx frame: what must exist

GFx 3.3 is statically linked (RTTI strings at `0x11043a8+`, e.g.
`GFxMovieRoot::Display` at `0x110467c`). The menu object stores the movie view at
`+8`; `0xa64ba0` forwards to the view helper `0xa64a50`.

For the first displayed frame the engine must already have:

1. a real D3D9 device and an active render target/viewport (engine renderer init
   `0x691030`/`0x696810`, then `0xc74710`);
2. GFx's own built-in shader set and vertex/index buffers (created by GFx's
   D3D9 renderer, not by `shadersfx`), plus its font provider for
   `fonts_en.swf`;
3. the movie's vector fills/text, which need no bitmap texture: real
   `StartMenu.swf` has **0 bitmap tags** and 40 solid fills;
4. `CreateVertexShader`/`CreatePixelShader` calls from GFx, then
   `SetStreamSource`/`SetIndices`/`SetVertexDeclaration`/`DrawIndexedPrimitive`
   and finally `Present` on the menu's target.

### 3.1 Measured GFx shader set (2026-11-27)

With the 160 MiB guest budget (see below) the run passes renderer init and
reaches GFx's D3D9 renderer. The shader creation loop is
`0xf88360..0xf8843b`:

| Table | VA | Entries | Non-NULL | Role |
|---|---|---:|---:|---|
| `gfx.vs` | `0x12dfca8` | 6 | 5 | `CreateVertexShader` at `0xf8838c` |
| `gfx.ps.level2` | `0x12dfcd8` | 45 | 40 | `CreatePixelShader` at `0xf883df` |
| `gfx.ps.level3` | `0x12dfa80` | 25 | 24 | second PS loop, only when shader level `>= 3` |

All blobs are `vs_3_0` / `ps_3_0`; the DWORD length is in the high half of the
second token. Observe 5 VS + 40 PS created (all `S_OK` only because
`TESV_D3D9_SHADER_SKIP_PROBE=1` makes them non-executable objects).

Distinct `gfx.vs` hashes (3):

| Bytes | SHA-256 | Entries |
|---:|---|---:|
| 172 | `77bc67a86e38ea6565c82f95313185a48420430c4f4c7afa6144d144ce075376` | 3 |
| 128 | `4281bd82262c5a5a8d26588a60fea83d52a265f8ef08fcbf34ce2b0045eb826a` | 1 |
| 172 | `ad97e55f922355acfd8f40e8d37f53ab122c8d3f7dbc5e2af1cc5e4adbe1f924` | 1 |

Distinct `gfx.ps.level2` hashes (7, 40 objects):

| Bytes | SHA-256 | Entries |
|---:|---|---:|
| 132 | `f031c9f821e5316df05109dd1464e2de244827ce39e7a597b12024765320e652` | 2 |
| 160 | `01fd3eb5bf6aa39447bef056b2263e19e1199cf999c7a3aabe5d268b56ce4d6b` | 6 |
| 200 | `2c4a728003938d0b599f535c0c000cd6f15e96b275254871847ed6db4f82025a` | 24 |
| 220 | `8ced9de968aad07942ea3b454f127dfb00bd94761fa2c980f2c770ed504fa93d` | 2 |
| 244 | `b527a0101301d171f291649401d9c45e64b82fb32e73e182f0725df71244275b` | 2 |
| 288 | `1b279738cdd7559ca1bbd05e8b46c63d8f54f412f8bb8b45d2456278ab94f907` | 2 |
| 332 | `cd1c433b3e73bd079e9d9449c0267a2e74f0c5311db04b2ac0920fae0e297918` | 2 |

`gfx.ps.level3` (24 objects, 4 hashes, not created at the current honest level
2): 404 B `a226495a...9c307`, 432 B `9742395e...b239d8`, 328 B
`5d4ce7e4...6116c`, 272 B `159a088e...98b93`.

`python tools/menu_harness.py --gfx-shaders` reproduces the full address/size/
hash table. This is the real native shader requirement for the StartMenu
overlay: **5 VS + 40 PS objects at level 2 (or +24 PS if level 3 is ever
advertised), 3 + 7 distinct hashes**. The earlier `p1200005` / 83-VS closure is
the 3D menu background, not these GFx blobs.

## 4. Layers that actually block, in order

| # | Layer | Measured state | Honest fix |
|---|---|---|---|
| 1 | SM3 word gate `0xc6e22d` | honest caps VS/PS 2.0 -> renderer failure | measurement-only byte-verified `TESV_D3D9_SM3_GATE_PROBE`; DevCaps stays `0x00082000` |
| 2 | HW T&L gate `0xc6d8c6` | honest caps lack `HWTRANSFORMANDLIGHT` | measurement-only `TESV_D3D9_CAPS_PROBE` |
| 3 | renderer-init targets/depth | 9 targets (5x1024 `0x72`, 3x512 `0x15`, 1x512 `0x71`), 512 cube, 4 depth surfaces; cube and D24S8 pass | LDR fallback + depth-pair probe, already measurement-only |
| 4 | guest memory | 64 MiB guest budget: `549a9038` OOM inside the 1024 shadow group. **160 MiB (167772160) passes**; 92 MiB D3D budget is enough for all targets | use 160 MiB guest / 92 MiB D3D for the menu harness; 64 MiB is not enough for renderer init + shader preload |
| 5 | shader execution | `TESV_D3D9_SHADER_SKIP_PROBE` turns valid SM3 streams into non-executable objects; GFx's 5 VS + 40 PS are created `S_OK` but cannot draw | a real bounded SM3 execution path for the 3 VS / 7 PS hashes above; do not fake caps |
| 6 | GFx init/first draw | reached: `CreatePixelShader` 40/40, `CreateVertexBuffer` 2, `CreateIndexBuffer` 2, 2× `BeginScene`/`EndScene`/`Present`, but **0 `Draw*`**; 2e9 blocks end at `pilot basic-block budget exceeded` | raise the step budget (>2e9) and/or instrument the menu-open milestone; then capture the first `DrawIndexedPrimitive` |
| 7 | Bink/video | `_BinkOpen` returns NULL; no frame; not fatal | empty `sIntroSequence` skips it entirely |
| 8 | audio/input | not required for the first menu frame | input only for navigation (`TESV_INPUT_PROFILE=menu`; see `docs/MENU_INPUT.md`); audio failures are non-fatal |

## 5. Minimal menu harness

The only engine-level change is an INI value; it is not a code patch:

```ini
[General]
sIntroSequence=
bPreloadIntroSequence=0
```

Place it in the game root's `Skyrim.ini` (the path mapper accepts
`C:\Skyrim\...` / `sdmc:/Skyrim\...`). `0x69dadf` then sets `0x1b3fced` and the
`0xf500f0` Bink call at `0x69db7c` is skipped.

Gotchas, both measured:

* add **only** these two keys; a malformed `[Display] iSize W/H` block makes
  the engine exit at `0x0069107d` with `Failed to initialize renderer.` before
  D3D;
* a custom game root must also contain the port profile files
  `Documents\My Games\Skyrim\SkyrimPrefs.ini` and
  `LocalAppData\Skyrim\Skyrim.ini` (copied from the Azahar seed). Without them
  the engine falls back to PC display defaults and fails the same renderer
  gate. The `F:\Skyrim` root already has both directories, which is why the
  first 160 MiB census worked without extra setup.

Measurement environment (host census or device flag set):

```text
TESV_D3D9_TRACE=1
TESV_D3D9_CAPTURE_DRAWS=8
TESV_D3D9_CAPS_PROBE=1
TESV_D3D9_SM3_GATE_PROBE=1
TESV_D3D9_DEPTH_PAIR_PROBE=1
TESV_D3D9_HDR_LDR_FALLBACK=1
TESV_D3D9_SHADER_SKIP_PROBE=1
TESV_D3D9_BUDGET_MB=92
TESV_CECDC0_SOURCE_FILL=1
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
TESV_INPUT_PROFILE=menu
```

`TESV_CECDC0_SOURCE_FILL=1` is required to get past the cube/RT descriptor
(`0xced041`); without it the run dies at `NiRenderedCubeMap` before GFx. The
slice args are `F:\Skyrim\TESV.exe <root> 2000000000 167772160`; the 160 MiB
guest budget is the one that reaches GFx (64 MiB stops at `549a9038`).

Launch normally (no `-psave`/load): `0x6982c0` returns 1, WinMain calls
`0x876f60(1)` at `0x69de08`, the factory `0x876fc0` runs, the ctor loads
`Interface/StartMenu.swf`, and GFx owns the draw. Every probe above logs
`MEASUREMENT ONLY ... not shippable`; `DevCaps` is unchanged at `0x00082000`.

Do **not** patch `0x69de08`/`0x876f60` to force the menu earlier: the menu
object allocates through the engine allocator and needs `MenuManager`, GFx and a
device that already exist. Patching only shifts the crash.

## 6. Measured run (2026-11-27)

Host slice built from `E:\Skyrim_N3DS_build\analysis\growth_probe\gen01` with
the D3DXMatrixMultiply pilot table, args
`F:\Skyrim\TESV.exe F:\Skyrim 2000000000 167772160`, probes CAPS+SM3+DEPTH+
HDR_LDR+SHADER_SKIP+TRACE, `BUDGET_MB=92`, `CECDC0_SOURCE_FILL=1`:

| Event | Count / result |
|---|---|
| `CreateVertexShader` | 879 = 874 engine (`0xccbbc5`, addr `0x64550008`) + 5 GFx (`0xf8838c`) |
| `CreatePixelShader` | 40/40 `S_OK`, all caller `0xf883df` |
| `CreateVertexDeclaration` | 372/372 `S_OK` |
| `CreateVertexBuffer` / `CreateIndexBuffer` | 2 / 2 `S_OK` (`0xf82667` 0xc0000 B; `0xceecee` 0x34f20 B) |
| `BeginScene`/`EndScene`/`Present` | 2 / 2 / 2 `S_OK` (renderer-init cycles, caller `0xce7e03`) |
| `DrawIndexedPrimitive` / `DrawPrimitive` / `*PrimitiveUP` | **0** |
| first fault | `Guest fault at 00000000: pilot basic-block budget exceeded` after 2e9 blocks |

Immediately before the budget fault GFx has bound a complete object state
(declaration `0x50007340`, VS `0x50007380`, PS `0x50007510`, index buffer
`0x50007660`, `SetStreamSourceFreq`, `SetIndices`) and then **unbinds** all
three (`SetPixelShader(NULL)` at `0xf88be5`, `SetVertexShader(NULL)` at
`0xf88bf8`, `SetTexture(NULL)` at `0xf88c0d`) before `EndScene`/`Present`.
There is no `Draw*` between bind and unbind, so this is GFx's setup/clear
cycle, not a menu primitive. Thread 11 then logs `Sleep 25 ms from 0x6f40cf`
and thread 1 exhausts the budget — the run is in the main loop, not stuck in
the menu object.

For contrast, the same binary with the 64 MiB guest budget reaches 874/874 VS
`S_OK`, **0 `CreatePixelShader`, 0 `Draw*`, 0 `Present`**, then faults at
`549a9038` inside the 1024 shadow group. The 160 MiB budget is what unlocks the
GFx phase; no shader-retention change was needed.

The 2e9-block budget ends after the two init `Present` calls, before the menu
draws. A 4e9 run with the same env/root (`Skyrim.ini: sIntroSequence=`,
`bPreloadIntroSequence=0`) produced **identical counts** (40 PS, 2 VB, 2 IB,
2 Present, 0 Draw) and the same `pilot basic-block budget exceeded`; raising the
block count alone does not reach the menu.

**IRC container matters, but did not remove this spin.** The
`F:\Skyrim\ir\tesv.irc` used for the first runs is stale for `0x00f520f0`
(24 blocks, no PREENTRY). The census agent's fresh
`E:\census_ir2\ir_container\tesv.irc` (64,445,696 B, 90,900 functions / 90,027
decoded, index CRC OK) has 25 blocks including `0x00f520e0` with PREENTRY. A
4e9 run with the fresh container produced the **same** 40 PS / 2 VB / 2 IB /
2 Present / 0 Draw and the same budget fault; thread 11 sleeps at `0x6f40cf`
(19 ms), thread 10 at `0xb9ef30` (1 ms), and thread 1 burns the block budget.
So the menu state transition is not fixed by the IRC alone; capture thread 1's
guest stack/EIP next.

`TESV_D3D9_SHADER_USAGE=1` (source/d3d9_com.c) emits one JSON line per
Create VS/PS; `python tools/menu_shader_usage.py LOG --json OUT` builds the
deduplicated set. See `docs/MENU_SHADERS.md` for the measured 3 GFx VS hashes
and 7 GFx PS hashes (40 objects), and why the 874-VS Shaders001/002 preload is
not the menu set.

No frame is claimed: `Present` here is the engine's own init cycle, not a
StartMenu frame.
