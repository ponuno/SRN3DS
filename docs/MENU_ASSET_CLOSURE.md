# Skyrim N3DS main-menu closure (scratch)

Read-only census of the **first main-menu** asset/UI path. No game bytes are
stored or copied; outputs contain names, sizes, SHA-256 values and parsed
metadata only. The live repository was not modified.

## Commands

```powershell
# tests (38 synthetic tests, no game data)
.\run_tests.ps1

# real census
python -X utf8 -m menu_assets.census `
  --data F:\Skyrim\Data --root F:\Skyrim --out out
```

Latest run: exit 0, `errors: []`, all three checks PASS.

## Menu identity (static TESV.exe evidence)

| Item | Value | Evidence |
|---|---|---|
| Main menu class | `MainMenu` | RTTI `0xeb1a94`; ctor `0x876b90` |
| Menu name | `StartMenu` | ctor pushes VA `0x10e7ac` |
| Movie path | `Interface/StartMenu.swf` | template `Interface/%s.swf` VA `0x110e24c`, menu base ctor `0xa60c10` |
| Title-sequence menu | `TitleSequenceMenu` / `Titles` / `Interface/Titles.swf` | ctor `0x8934c0` pushes VA `0x10e7f60`; separate menu |
| Main-menu logo | `Interface/Logo/Logo.nif` | literal at `0xce5aa8` |
| Main-menu mist | `Interface/INTMenuFogParticles.nif` | literal at `0xce5388` |
| Font config | `Interface/FontConfig.txt` | literal at `0xd0cab8` |
| Localisation | `Interface/Translate_%s.txt` | literal at `0xd0ca9c` |

`gfxfontlib.swf` is a **virtual** GFx font library, not a file in any archive.
It resolves through `FontConfig.txt` `fontlib` + `map` entries.

## Effective archive order and precedence

`Skyrim_default.ini` order: Misc, Shaders, Textures, Interface, Animations,
Meshes, Sounds, then Voices/VoicesExtra (absent on this SD install), then
`Update.bsa` last.

`startmenu.swf` winner: **`Update.bsa`, 60,284 bytes**
`44b71ffc0986799a3e95c2ca9627346939e9e03662845bd243812c5755fcbb0f`

Shadowed base: `Skyrim - Interface.bsa`, 60,157 bytes
`26a46704c034536fb64ce9064cd70df74fe656bda1ea1a5241f20dc4e6d38922`

## Measured closure (15 resources, all present)

| Resource | Stored | Unpacked | SHA-256 |
|---|---:|---:|---|
| `interface\startmenu.swf` (Update.bsa) | 60,284 | 60,284 | `44b71ffc0986799a3e95c2ca9627346939e9e03662845bd243812c5755fcbb0f` |
| `interface\fontconfig.txt` | 1,095 | 1,095 | `ca8b7491d37b44729171e6b65ccab0574a42530822510b6bb7712a955c3f877f` |
| `interface\fonts_en.swf` | 417,809 | 417,809 | `97e45bbdfe5e030ec48f652204686862b3791dcf69b1e43151330cb2c838f331` |
| `interface\fonts_console.swf` | 14,752 | 14,752 | `5ea8fc49293f28e3c13d03e16b8e22bcb1b3d10ba0f564845610c390d684ea46` |
| `interface\titles.swf` (separate menu) | 16,024 | 16,024 | `6e06dc5eb8723424291dab763518deb2c7953366fae855f03445a0410b864f8d` |
| `meshes\interface\logo\logo.nif` | 126,710 | 237,735 | `c0b9c97f3ac4d128e877893856a99de653f6f14e69b1ae47630df711e821f023` |
| `meshes\interface\intmenufogparticles.nif` | 15,316 | 50,446 | `e5e79515ea3dfafaf0177e7b8fe68cb348bf7b45dfffe3a9be647cd4bb5874df` |
| `meshes\effects\fxambblowingfogmenu.nif` (candidate) | 40,382 | 57,403 | `a13e5528048bca99db647c3fdef56baf09fdc35439ec893d699c2dae4b5f06b9` |
| `textures\interface\objects\logo02_d.dds` | 161,107 | 349,672 | `f91712b0532db4195dcd426b2d2ef7f442283e46e4715c4699994ab032464eec` |
| `textures\interface\objects\logo02_n.dds` | 435,094 | 699,216 | `77a07b3baac1133a315ae2a1f3c1e39b9e90bb8d778f086c2aa36cfa8cc56854` |
| `textures\effects\fxsteamthinanim.dds` | 153,599 | 349,680 | `df705c92558a9ae9dab0c195d665f3dce04263fdedf71de8f05172490210e9d6` |
| `textures\effects\gradients\gradsteamthin.dds` | 44,011 | 87,536 | `b1be3cac7fa680954043609a2be5a232bd7cdbbbe7603888a4f3b8d34dfdcb9b` |
| `textures\effects\fxfog01tile.dds` | 156,206 | 174,928 | `c7d6d545989228e000dd787a7a2db459c6b0cfdb01789f7dbddc28566fd1432a` |
| `textures\effects\gradients\gradfogdisssoft.dds` | 21,405 | 43,856 | `97b7f670cb21402a653ac64c922bded435f896cb2e39c62c21a0610c7fdb68ff` |
| `music\special\mus_maintheme.xwm` (optional) | 1,864,158 | 1,864,158 | `4403908e2c0fc62fe0c8a3bc816e3a4f3f821ef9252449e76828240f34712343` |

Masters: `Skyrim.esm` 249,371,467 B `bc84992a...a5b6ed`;
`Update.esm` 1,518,686 B `b033d282...ba0f4`.
`TESV.exe` 17,462,064 B `d97eb0cc...f5e605`.

## SWF facts

- All four measured movies are **CWS/zlib, SWF version 10**.
- `startmenu.swf`: 1,186 tags; 47 exports; imports `gfxfontlib.swf` symbols
  `$EverywhereMediumFont` (id 34) and `$EverywhereFont` (id 41); defines one
  local font id 1 “Futura Condensed” (243 glyphs).
- `fonts_en.swf`: 12 `DefineFont3` + 12 `DefineFontName`; supplies both imported
  fonts.
- Font resolution: `$EverywhereMediumFont` → id 3 “Futura Condensed Medium”
  (187 glyphs); `$EverywhereFont` → id 1 “Futura Condensed Light” (187).
- `sharedcomponents.swf` is **not** statically imported by `startmenu.swf`.
- `titles.swf` belongs to the separate title-sequence menu.
- `Translate_ENGLISH.txt`: UTF-16LE BOM, CRLF, 20,356 B,
  `e59f31c5f3a217ac8abb78f4e45c742e07f61c38e18e39ef65e23d3275bccc20`.

## First guest-side blocker (after caps/shader gates)

Primary: **the GFx batch vertex contract**. `source/d3d9_com.c`
`create_vb` accepts only Usage 0 and FVF `0x102` (`XYZ|TEX1`). GFx's D3D9
renderer submits pre-transformed, per-vertex-tinted geometry
(`FVF_XYZRHW|DIFFUSE|TEX1` ≈ `0x144`, or an equivalent declaration with
`POSITIONT + COLOR + TEXCOORD0`). The first menu mesh therefore cannot create
its vertex buffer or set its FVF.

Adjacent failures in the same first batch:
- `CreateVertexBuffer` rejects `D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC` (`0x208`).
- `CreateTexture` rejects `D3DUSAGE_DYNAMIC` (`0x200`), needed by dynamic glyph
  textures.
- `SetTextureStageState` (device slot 67) has **no handler**; GFx sets
  COLOROP/ALPHAOP/ARG1/ARG2/TEXCOORDINDEX.
- `SetFVF` only accepts `0x102`; `DrawIndexedPrimitive` requires it.

Exact shim/renderer work:
1. `create_vb`: accept `0x208`, map to CPU staging + per-frame upload; accept
   `0x144`/`0x142` or a declaration.
2. `d9_decl_init`/`set_decl`: support `POSITIONT`, `COLOR0`, `TEXCOORD0`.
3. `create_tex`: accept `D3DUSAGE_DYNAMIC` and upload on `UnlockRect`.
4. Wire slot 67 `SetTextureStageState` to a fixed GFx state set (MODULATE,
   SELECTARG1/2, TEXCOORD0) and translate to the existing TEV path.
5. Pre-transformed draw path: bypass the matrix transform and pass vertex
   colour into the TEV/vertex-colour path; keep the straight-alpha glyph atlas
   contract.
6. Confirm the first failing call at runtime with `TESV_D3D9_TRACE=1`; the
   static order above is from the COM/FVF contract, not a traced run.

## Limits

- NIF references are printable-string declarations, not a block-graph trace.
- The exact main-menu ESM cell/weather record was not resolved.
- `fxambblowingfogmenu.nif` is a name-based candidate, not confirmed from an
  ESM weather record.
- No frame, pixel proof or hardware run is claimed here.

## Runtime follow-up

`docs/MENU_BSA_RUNTIME.md` implements and validates the read-only path:
a bounded C BSA reader, the measured Interface-then-Update order,
`Interface/StartMenu.swf` copied into guest memory within a byte budget, and
SWF FWS/CWS/ZWS header checks.  It still does not decompress the GFx body or
draw the menu.
