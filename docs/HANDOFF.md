# Handoff — Skyrim 2011 → New 3DS port (`SRN3DS`)

This is the working context for the next agent. Read this, then `docs/PORT_LOG.md`
(the append-only journal) and `docs/DEVICE_BRINGUP.md` (real emulator runs).

## What the project is

Recompiled/lifted Skyrim (TESV 1.9.32.0.8, x86) running on New 3DS:
- the guest PE is lifted to C/IR (`source/lift*`, `staged/`), imports are bound to
  host shims (`source/win32_*.c`, `source/d3dx_*`), D3D9 is reimplemented on top
  of a PICA200/citro3d backend (`source/d3d9_*`).
- There is also a parallel, honest **menu-SWF** pipeline (Scaleform SWF parsing,
  tessellation, A8 glyph atlas, real D3D9 draws on the host COM demo).
- Shipping target is a CIA (`tools/build_cia.ps1`); `.3dsx` is used for Azahar.

## Non-negotiable rules

- `DevCaps` is exactly `0x00082000`. No fake `HWTRANSFORMANDLIGHT`, no fake SM3.
  `CheckDeviceFormat`/`CheckDepthStencilMatch` must answer truthfully.
- Measurement-only probes are env/flag gated, log `MEASUREMENT ONLY ... not
  shippable`, and never become defaults. Never fabricate pixels/widths/cubes.
- No game EXE/DLL/assets in git; generated dirs are ignored.
- Fix the first divergent layer; add a minimal regression that fails before it.
- Run targeted tests, then `tools\run_host_tests.ps1`; report separately what ran
  on Azahar. Never claim a frame/pixel result without it.
- `docs/PORT_LOG.md` is append-only, numbered rows. Add one row per landed layer.

## Commands (Windows)

```powershell
# host suite (use E: because C: has hit "no space on device")
$env:HOST_CLANG_JOBS='8'
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -TesvIrc F:\Skyrim\ir\tesv.irc `
  -TesvSliceGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -TesvExe F:\Skyrim\TESV.exe -AllowDiagnosticFault `
  -OutDir E:\host_full -TestTimeoutSec 300

# targeted
... -Only com,render_texture,cube_rt -OutDir E:\host_t -TestTimeoutSec 300

# ARM build (TESV) / com build
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Mode tesv `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen -TesvArmSteps 2000000000 `
  -ForceMemProbe -Isolated d3dxNN
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Mode com -Isolated name

# CIA from an ELF
powershell -ExecutionPolicy Bypass -File tools\build_cia.ps1 `
  -Elf dist\Skyrim_N3DS_tesv_d3dxNN\...elf -Out build_cia\....cia
```

Azahar install/run (SD card is `%APPDATA%\Azahar\sdmc`):
- `"C:\Program Files\Azahar\azahar.exe" -i <cia>` installs;
- for emulator debugging, drag the `.3dsx` into Azahar; automation of its GUI is
  unreliable, ask the user to double-click the title.
- Device logs: `sdmc:/Skyrim_N3DS_tesv.log` (rc/exit), and
  `sdmc:/Skyrim_N3DS_stderr.log` (mirrored stderr).

## Current state (2026-11-26, main `9c6def2`)

- Final ARM: `d3dx41`, `.3dsx` 2,349,536 B SHA256
  `3b182136ad9aa42fecaa8e816a76886d0563b42e96abf0355351119d478fd47d`,
  `.text` 2,329,124 B.
- Host suite: run and confirm the current number; known diagnostic is
  `tesv_slice` (early TESV memory gate / memory probe), not a render result.
- No Azahar frame or pixel proof exists. The device now boots far enough to load
  BSAs and initialise the renderer, then stops on honest gates.

### TESV route (the hard blocker)

Host census (with probe env) now reaches:
`CreateCubeTexture(512,1,1,0x71,0) -> S_OK`, 6 `GetCubeMapSurface`, depth
surfaces, one `SetRenderTarget` (implicit back buffer 400x240), then
`SetDepthStencilSurface` with a **512x1024 depth** -> mismatch.

- The bridge now returns `D3DERR_INVALIDCALL` (correct D3D9 class) with both
  sizes, but the engine retries through
  `NiDX9Renderer::BeginUsingRenderTargetGroup` and overflows a 64 KiB worker
  stack at `0x1d1ffffc`.
- Root cause is the pair: the engine wants a 512x1024 colour target/group while
  our implicit back buffer is 400x240. Next work is a real 512x1024 RT/group
  registration (or a matching back buffer), not another error-code change.
- Before that, two honest gates were passed only with the measurement flag:
  HW T&L and SM3. Do not ship those flips; they are only for measurement.

### Device (Azahar) state

- `GUEST_MAX_SLABS=32` fixed a 9th-slab `VirtualAlloc commit failed: 4104 KiB`.
- heap (`__ctru_heap_size`) is 96 MiB, guest budget 92 MiB.
- Path mapper accepts `C:\Skyrim\...` and `sdmc:/Skyrim\...` (mixed separators,
  trailing `\`), stripping a duplicated `Skyrim` component.
- With `sdmc:/Skyrim/tesv_caps_probe.flag` present, the runner sets
  `TESV_D3D9_CAPS_PROBE`, `TESV_D3D9_SM3_GATE_PROBE`,
  `TESV_D3D9_SHADER_SKIP_PROBE` (all measurement-only). The run then passes
  T&L/SM3 and faults at `Guest fault at 00000030: unmapped guest memory`
  (NULL renderer pointer after shader creation).

### Menu SWF pipeline (works on host)

- Real `Update.bsa/StartMenu.swf`: 216 records, 71 sprites, 53 shape records.
- 57,480 tessellated vertices; fills produce 57,484 batch vertices /
  86,160 indices, submitted as **one 16-bit indexed `DrawPrimitiveUP`** on the
  COM demo; `expanded_vertices=0`, FVF `0x142`, A8 64x64 atlas.
- Layers done: SWF summary/display list, shape tessellation, glyph
  rasterisation (4x4), atlas packing, text layout, quad/index batch, solid
  fills + per-vertex gradients, clip-mask rectangles and culling, HTML style
  runs, AVM action inventory (53 `Stop`, no `Goto`).
- Text records reference imported fonts 34/41 (`$EverywhereFont`,
  `$EverywhereMediumFont`, URL `gfxfontlib.swf`); no exact-id match exists in
  `Interface/fonts_en.swf`, so real text glyphs are 0 and the pipeline reports
  named refusals. Face-to-font mapping is not implemented.
- `StartMenu` has zero bitmap tags and zero clip masks; bitmap tags are decoded
  but not sampled.

### Device/backend coverage (shippable where stated)

- D3D9 COM: state blocks, implicit swap chain, EVENT queries, private data,
  cursor/palette state, clip planes/status, texture update (UpdateTexture/
  UpdateSurface/ColorFill), RT readback (front buffer refused), Reset in place.
- Render targets: LDR downgrade for float RTs under
  `TESV_D3D9_HDR_LDR_FALLBACK` (measurement policy), bounded native storage,
  depth descriptors, cube RT faces (`C3D_RenderTargetCreateFromTex`), RT-cube
  sampling readiness, cube-face readback.
- Textures: A8 (`GPU_A8`), cube create/upload/delete on PICA, volume create/
  upload (PICA has no 3D texture mode -> named refusal), texcache eviction/
  rehydration, budget-pressure trim.
- PICA: cube sampling (`d9shader_cube.v.pica`), menu-text TEV plan, embedded
  depth; SM3/T&L remain refused.

## What to do next (priority)

1. **Depth/RT pair**: make the engine's 512x1024 colour target real (create a
   matching render target/group and bind it) or make the implicit back buffer
   match the engine's expectation with a scaled Present. Then stop the
   `BeginUsingRenderTargetGroup` recursion (return a hard failure or a valid
   group) and re-run the census until `SetTexture`/draw/`Present`.
2. **Device run**: use `dist\Skyrim_N3DS_tesv_d3dx41\...3dsx` (or rebuild) with
   the `tesv_caps_probe.flag`; diagnose the `0x30` NULL after shader creation
   from `sdmc:/Skyrim_N3DS_stderr.log`.
3. **Menu**: face-to-font mapping or the GDI/system-font provider, bitmap fill
   sampling, partial clip-mask tessellation; then a PICA path for the menu
   batch (TEV/vertex colour) and a real frame.
4. Keep the honest gates honest: solving (1) does not mean advertising SM3/T&L;
   measurement flags must stay out of shipping defaults.

## Gotchas

- `docs/PORT_LOG.md` has had mixed CRLF/LF churn. Edit it with a script that
  preserves the current EOL; keep one row per layer and renumber conflicts after
  merges (agent branches are often based on older mains).
- Prefer Python script files over PowerShell inline `-c` strings for edits with
  quotes/backticks; the shell mangles them.
- `source/tesv_slice_main.c` must include `<unistd.h>` only inside `#ifdef
  __3DS__` (Windows host clang has no such header).
- `tools/run_host_tests.ps1` gates `tesv_slice`/`tesv_slice_cube_probe` sources on
  the generated slice contents; do not unconditionally link the probe slices.
- GitHub SSH (`ssh.github.com:443`) intermittently times out; retry the push.
- Agents work in `C:\Users\vasya\Documents\Skyrim_N3DS_wt\<name>` worktrees;
  never edit the main tree while a full suite runs.
