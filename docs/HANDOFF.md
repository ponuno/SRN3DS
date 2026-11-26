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

# ARM build (TESV) / com build: -NoClean for incremental, -Jobs for parallel make
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Mode tesv -NoClean -Jobs 8 `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen -TesvArmSteps 2000000000 `
  -ForceMemProbe -Isolated d3dxNN
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Mode com -Isolated name

# CIA from an ELF
powershell -ExecutionPolicy Bypass -File tools\build_cia.ps1 `
  -Elf dist\Skyrim_N3DS_tesv_d3dxNN\...elf -Out build_cia\....cia
```

### Fast iteration (measured 2026-11-26)

`run_host_tests.ps1` keeps a **shared clang object cache** (default
`E:\host_objcache`, override `HOST_OBJ_CACHE`). Each `(relative source,
content, flags, clang build)` compiles once; every test, worktree and agent
links from it.

- Full suite (145 tests + Python): **145 s** warm, previously ~25 min.
- Targeted 3 tests: **19 s** warm / 27 s cold; add `-SkipPython` for an 8 s
  loop (most of the rest is the one-time vcvars import).
- Do not run the full suite per iteration. Use `-Only a,b,c` (optionally
  `-SkipPython`); run the full suite once before the final commit of a layer.
- ARM: `-NoClean -Jobs 8` is safe (shader/bin2s dependencies are explicit in
  the Makefile). Measured full `-Mode com -Jobs 8` build: **12 s** (was ~2 min).
  Full clean only when flags or generated inputs change.
- The cache is content-addressed and safe to share between worktrees. Delete
  `E:\host_objcache` to force a clean rebuild.
- Triage a census log with **`python tools/census_report.py <log>`** instead of
  grepping megabytes of trace: it prints the first fault with context, per-method
  call/result counts, common diagnostics, depth binds, the last trace lines and
  the unresolved-import count (~40 lines).
- Timebox each layer to ~20–25 minutes. If it is not solved, commit the honest
  partial state plus the exact blocker and return it; do not spend an hour
  before the first commit. A clean partial commit is cheap to continue from.

Azahar install/run (SD card is `%APPDATA%\Azahar\sdmc`):
- `"C:\Program Files\Azahar\azahar.exe" -i <cia>` installs;
- for emulator debugging, drag the `.3dsx` into Azahar; automation of its GUI is
  unreliable, ask the user to double-click the title.
- Device logs: `sdmc:/Skyrim_N3DS_tesv.log` (rc/exit), and
  `sdmc:/Skyrim_N3DS_stderr.log` (mirrored stderr).

## Current state (2026-11-26, current `main`)

- Latest ARM: `d3dx42`, `.3dsx` 2,349,980 B SHA256
  `804e704741bc9d31ef27cb329a0a45b89ed11f54da08694aab93d743caa20054`.
- Host suite: **146 passed, 1 diagnostic, 0 failed** in ~150 s warm.
  The diagnostic is `tesv_slice` (reporting a documented guest fault, not a
  render result).
- No Azahar frame or pixel proof exists. The device boots far enough to load
  BSAs and initialise the renderer, then stops on honest gates.

### TESV route (the hard blocker)

Measurement-only `TESV_D3D9_DEPTH_PAIR_PROBE=1` (SD flag
`sdmc:/Skyrim/tesv_depth_pair_probe.flag`) creates a retained 512x1024 back
buffer and Present scales it to 400x240. With it, the host census now:

- binds the engine's 512x1024 depth to the implicit 512x1024 target
  (`Depth=0x50002060 512x1024 RenderTarget=0x50002030 512x1024 Implicit=1`),
  `SetDepthStencilSurface -> S_OK`;
- runs four `BeginScene`/`EndScene`/`Present` cycles with `S_OK`, 64
  `SetTexture`, 1756 `CreateVertexShader`, 744 `CreateVertexDeclaration`;
- reaches **no Draw call yet** and then faults at
  `Guest fault at 00f520e0: unknown function/import` (after a 29 ms Sleep).
- `SetSamplerState(stage=0,type=8,value=0)` returns `INVALIDCALL`; a separate
  track owns that audit.
- The depth pair is fixed only under the measurement probe; the shipping path
  still refuses the 400x240-vs-512x1024 pair honestly. HW T&L and SM3 remain
  measurement-only flips.

### Device (Azahar) state

- `GUEST_MAX_SLABS=32` fixed a 9th-slab `VirtualAlloc commit failed: 4104 KiB`.
- heap (`__ctru_heap_size`) is 96 MiB, guest budget 92 MiB.
- Path mapper accepts `C:\Skyrim\...` and `sdmc:/Skyrim\...` (mixed separators,
  trailing `\`), stripping a duplicated `Skyrim` component.
- With `sdmc:/Skyrim/tesv_caps_probe.flag` present, the runner sets
  `TESV_D3D9_CAPS_PROBE`, `TESV_D3D9_SM3_GATE_PROBE`,
  `TESV_D3D9_SHADER_SKIP_PROBE`; `sdmc:/Skyrim/tesv_depth_pair_probe.flag`
  adds `TESV_D3D9_DEPTH_PAIR_PROBE` (all measurement-only). The last observed
  run passes T&L/SM3 and faults at
  `Guest fault at 00000030: unmapped guest memory` (NULL renderer pointer
  after shader creation) — see `docs/DEVICE_BRINGUP.md`.

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
2. **Device run**: use `E:\azahar_run\Skyrim_N3DS_tesv_lazy_depth.3dsx` with
   the `tesv_caps_probe.flag`. The `0x30` NULL is diagnosed in
   `docs/DEVICE_BRINGUP.md`: the second renderer-init group was NULL after a
   256x256 PICA render-target allocation failed, because every explicit target
   also paid for a full-size depth attachment. Explicit targets now get
   `DEPTH24_STENCIL8` lazily on `SetDepthStencilSurface`; the next log shows
   either the next layer or the free-VRAM numbers.
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
