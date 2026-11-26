# Real StartMenu shader set (measured, priority list)

Status: **measurement only; no menu frame.** This is the shader set the retail
TESV/GFx creates on the way to `Interface/StartMenu.swf`, captured with
`TESV_D3D9_SHADER_USAGE=1` (one JSON line per `Create*Shader`) and the
`tools/menu_harness.py --gfx-shaders` table. No bytecode is stored here.

Evidence run: host slice from `E:\Skyrim_N3DS_build\analysis\growth_probe\gen01`
+ D3DXMatrixMultiply pilot, args `F:\Skyrim\TESV.exe E:\host_menu_run\game
2000000000 167772160`, `N3DS_IRC_PATH=E:\census_ir2\ir_container\tesv.irc`,
`BUDGET_MB=92`, `CECDC0_SOURCE_FILL=1`, SHADER_SKIP+USAGE+TRACE. Full JSON:
`E:\host_menu_run\used_menu_shaders.json` (regenerate with
`python tools/menu_shader_usage.py LOG --json OUT`).

## 1. What is actually created

| Group | Caller VA | Objects | Distinct hashes | Bytes | Meaning |
|---|---|---:|---:|---|---|
| GFx VS | `0xf8838c` | **5** | **3** | 128 / 172 | GFx's built-in D3D9 vertex programs |
| GFx PS level 2 | `0xf883df` | **40** | **7** | 132–332 | GFx's built-in pixel programs, current honest level |
| GFx PS level 3 | (same table, not run) | 0 | (4) | 272–432 | only if level `>= 3` is advertised; **do not do this** |
| Engine Shaders001/002 preload | `0xccbbc5` | **874** | **309** | 232–2068 | world/renderer package preload, not the SWF overlay |
| Engine PS | — | **0** | 0 | — | PS preload/menu-logo path not reached |

So the **menu overlay** priority is **5 VS + 40 PS objects = 3 + 7 distinct
hash families**, not the 9,621-PS corpus and not the 874-VS preload.

## 2. GFx vertex shaders (5 objects, 3 hashes)

| Bytes | SHA-256 | Table entries |
|---:|---|---:|
| 172 | `77bc67a86e38ea6565c82f95313185a48420430c4f4c7afa6144d144ce075376` | 3 |
| 128 | `4281bd82262c5a5a8d26588a60fea83d52a265f8ef08fcbf34ce2b0045eb826a` | 1 |
| 172 | `ad97e55f922355acfd8f40e8d37f53ab122c8d3f7dbc5e2af1cc5e4adbe1f924` | 1 |

Table `0x12dfca8`, 6 slots (slot 0 NULL). All are `vs_3_0`.

## 3. GFx pixel shaders (40 objects, 7 hashes, level 2)

| Bytes | SHA-256 | Objects |
|---:|---|---:|
| 132 | `f031c9f821e5316df05109dd1464e2de244827ce39e7a597b12024765320e652` | 2 |
| 160 | `01fd3eb5bf6aa39447bef056b2263e19e1199cf999c7a3aabe5d268b56ce4d6b` | 8 |
| 200 | `2c4a728003938d0b599f535c0c000cd6f15e96b275254871847ed6db4f82025a` | 14 |
| 220 | `8ced9de968aad07942ea3b454f127dfb00bd94761fa2c980f2c770ed504fa93d` | 4 |
| 244 | `b527a0101301d171f291649401d9c45e64b82fb32e73e182f0725df71244275b` | 4 |
| 288 | `1b279738cdd7559ca1bbd05e8b46c63d8f54f412f8bb8b45d2456278ab94f907` | 4 |
| 332 | `cd1c433b3e73bd079e9d9449c0267a2e74f0c5311db04b2ac0920fae0e297918` | 4 |

Table `0x12dfcd8`, 45 slots (5 NULL), all `ps_3_0`. They are D3DX blobs with a
CTAB block; the code length is the high half of the second token. Hash and size
are computed over the **whole blob from offset 0** with that length, which is
why the usage log and `--gfx-shaders` agree.

Level-3 extra table `0x12dfa80` (25 slots, 24 non-NULL, 4 hashes, 272–432 B) is
**not** created at the honest level 2. Keep it out of the first port pass.

## 4. Coverage against `docs/shader_plans.json`

`shader_plans.json` has 4 entries: `menu.logo.ps`, `menu.composite.ps`,
`game.ps.f5239354`, `game.vs.0ccc7542`.

| Group | Unique hashes | Already in plans | Draw-enabled |
|---|---:|---:|---:|
| GFx VS | 3 | 0 | 0 |
| GFx PS level 2 | 7 | 0 | 0 |
| Engine preload VS | 309 | **1** (`game.vs.0ccc7542`) | 0 |
| **Menu overlay total** | **10** | **0** | **0** |
| **All observed** | **319** | **1** | **0** |

The one matched engine VS is a package-preload program; it does not draw the
menu. `menu.logo.ps`/`menu.composite.ps` are the 3D background plans and were
not created in this run (the menu was not opened), so they are not evidence for
the SWF overlay.

## 5. Priority for the shader agents

1. **7 GFx PS + 3 GFx VS** — the only shaders that must run for the StartMenu
   overlay. Start here; each is a bounded, small straight-line program.
2. `menu.composite.ps` (already draw-enabled) and `menu.logo.ps` (needs
   normal/tangent/ambient preconditions) — only for the 3D menu background,
   after the overlay.
3. Do **not** translate the 309 engine-preload VS hashes for the menu; they are
   Shaders001/002 world shaders and 874 objects of dead weight for this goal.
4. Do **not** touch the 24 level-3 PS blobs unless a future menu explicitly
   needs them; the honest level is 2.

## 6. Reproduce

```text
TESV_D3D9_SHADER_USAGE=1
TESV_D3D9_SHADER_SKIP_PROBE=1
TESV_D3D9_TRACE=1
TESV_D3D9_BUDGET_MB=92
TESV_CECDC0_SOURCE_FILL=1
N3DS_IRC_PATH=E:\census_ir2\ir_container\tesv.irc
```

`TESV_D3D9_SHADER_USAGE` only prints; it changes no HRESULT, object or caps
field. Without the env the bridge is byte-for-byte unchanged.
