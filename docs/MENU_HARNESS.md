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

None of item 2-4 is measured yet. The census has **0 `CreatePixelShader`, 0
`Draw*` and 0 `Present`**; the last run stops in NiDX9 renderer init, long before
GFx creates a movie. Consequently the exact GFx shader count is still
unmeasured and must come from a capture of the first `0xa60200`/`0xa60c80`
window. The only shader set with a static census today is the **3D menu
background** closure: `p1200005` (one `ps_3_0`, SHA-256 `8725393b...5ff9`) and
the bounded 83-member `menu.generic.vs` family (28 distinct hashes), documented
in `docs/MENU_SHADER_CLOSURE.md`; that is not yet proven to be the SWF overlay.

## 4. Layers that actually block, in order

| # | Layer | Measured state | Honest fix |
|---|---|---|---|
| 1 | SM3 word gate `0xc6e22d` | honest caps VS/PS 2.0 -> renderer failure | measurement-only byte-verified `TESV_D3D9_SM3_GATE_PROBE`; DevCaps stays `0x00082000` |
| 2 | HW T&L gate `0xc6d8c6` | honest caps lack `HWTRANSFORMANDLIGHT` | measurement-only `TESV_D3D9_CAPS_PROBE` |
| 3 | renderer-init targets/depth | 9 targets (5x1024 `0x72`, 3x512 `0x15`, 1x512 `0x71`), 512 cube, 4 depth surfaces; cube and D24S8 pass | LDR fallback + depth-pair probe, already measurement-only |
| 4 | guest memory | 64 MiB census: `CreateVertexShader` 874/874 `S_OK`, declarations 368 (360 `S_OK`, 8 `INVALIDCALL`), `CreatePixelShader` 0, then `Guest fault at 549a9038: guest memory backing failed: out of memory` inside the 1024 shadow group | raise the *measurement* budget to the device-real 92 MiB (`TESV_D3D9_BUDGET_MB=92`) and stop retaining up to 4096 words per code-less shader; report the next call |
| 5 | shader execution | `TESV_D3D9_SHADER_SKIP_PROBE` turns valid non-closure streams into non-executable objects; every draw is refused by name | a real bounded SM3 execution path (VS family + PS plans) or a GFx fixed-function contract; do not fake caps |
| 6 | GFx init/first draw | never reached; `CreatePixelShader`/`CreateVertexBuffer`/`Draw*`/`Present` all 0 | after layer 4: count GFx's built-in shaders and buffers in the `0xa60200` window, then implement exactly those |
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
N3DS_IRC_PATH=F:\Skyrim\ir\tesv.irc
TESV_INPUT_PROFILE=menu
```

Launch normally (no `-psave`/load): `0x6982c0` returns 1, WinMain calls
`0x876f60(1)` at `0x69de08`, the factory `0x876fc0` runs, the ctor loads
`Interface/StartMenu.swf`, and GFx owns the draw. Every probe above logs
`MEASUREMENT ONLY ... not shippable`; `DevCaps` is unchanged at `0x00082000`.

Do **not** patch `0x69de08`/`0x876f60` to force the menu earlier: the menu
object allocates through the engine allocator and needs `MenuManager`, GFx and a
device that already exist. Patching only shifts the crash.

## 6. Next concrete step

Run the census with `TESV_D3D9_BUDGET_MB=92` (the real `.3dsx` application
budget) and the same probe set, and stop at the first non-`S_OK` call. Expected
outcomes:

* if the shadow group completes, the next lines should be `CreatePixelShader`
  from `0xa60200`/GFx and then the first `CreateVertexBuffer`/`Draw*`; capture
  those caller VAs;
* if `549a9038` repeats, reduce the shader retention in
  `shader_probe_object()` to a validation summary (no 4096-word copy) and log
  the exact allocation request size before the fault;
* then count the GFx built-in shaders exactly and add them, one hash at a time,
  to the bounded runtime. No draw/frame claim until `Present` and a captured
  non-empty draw are both in the log.
