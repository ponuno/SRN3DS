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

## Current state (2026-11-28, `main`)

The section below describes 2026-11-27 and is kept for history. This one is current.

- **`main` is green**: `tools\run_host_tests.ps1` reports 219+ passed, 0 failed.
  The MMX IR container is canonical: `E:\census_ir2\ir_container\tesv.irc`
  (90 110 decoded, SHA-256 `4DD060A5…0C12`; backup `.pre_mmx` beside it).
- **Menu creation is proven on the host.** The WinMain gate falls through
  (`[menu] enter 00876f60 from=0069de0f`, then `00876fc0` → `00876b90`), the
  singleton is non-zero (`[menustate] singleton=41f8eaf0 view=...`) and the GFx
  message switch `008779d0` runs every frame. `FadeInMenu` is **message type 0
  inside that switch**, not a separate function: it arrives with `ptr=0`, the
  StartMenu movie itself is loaded (`[movie+0x10]=1`), and the global fader
  `[0x1b10dcc]+0x28` is **0**, so the fade case skips and no indexed menu frame
  is drawn. Routing the type-1 movie into the type-0 message was tried and is
  wrong: the handler calls that pointer as a function and the run faults with
  `unknown indirect function` at the movie address. The real FaderMenu/fader
  object is still missing; that is the draw wall.
- **Device menu path.** The gate `0x6982c0` is in the AOT table
  (`tesv_ir_table.c`), so IR probes cannot see it. The main thread (tid=1) is
  `ready` and executes the **load loop** `0x8c71e0`; `release h=50c` happens
  once (thread 10), `release h=504` never. Under Azahar the VM-init handoff
  (`0x50c`) alone takes **~12–13 minutes**; a 500 s run with the gate removed
  from the AOT table only reaches `VMInitThread`, so the IR gate path is still
  unproven. `docs/AZAHAR_MENU.md` and `docs/VRAM_BUDGET.md` carry the tables.
- **Startup-menu experiment (not shipped, branch `startup-patch` `bb1da34`).**
  A two-byte image patch in `pe_image.c` replaces the `je load-path` at
  `0x69ddf9` with NOPs and excludes WinMain (`0x69d1d0`) plus the gate from the
  AOT lookup, so the patched image would run through the IR container. The
  patch applies (`[patch] image patch 0069ddf9: 2 byte(s) applied`), but the
  resulting IR-WinMain route faults at `0009007d` (`0xac6580`/`ir_ip=0xc389d3`)
  after the character/menu background textures load, and no `00876f60` is
  reached. Do not merge without fixing that IR-container path; the shipping CIA
  stays `96x24_memgate`.
- **Memory work landed** (all measured, no `DevCaps` change):
  - pageable read-only `.text` in a bounded window (`N3DS_TEXT_WINDOW_MB`,
    default 5): guest 57.2 → 44.4 MiB, +2.8% time.
  - compressed guest swap (vendored LZ4, safe-point eviction only, COW for
    file-backed pages): guest 44.4 → 35.3 MiB, 3.21× compression, +1.6% time.
    Device (`swap48`): `swap_out=920`, resident exactly 48 MiB, live ratio
    13.45×, no swap failures.
  - global 6 MiB VRAM budget (`N3DS_VRAM_LIMIT`, enforced 5.75 MiB; the 3DS
    reserves 4 of the 10 MiB): evict → downscale → linear → named refusal.
    `[d3d9-vram]` peak 5042/5888 KiB, 8 evictions, 0 refusals.
  - compact IRC index (16 B/entry, 3.64 → 1.48 MB), IRC record cap 2 MB, decode
    cache accounting fixed (4.98 MB real), churn removed.
  - main-thread stack: libctru's 32 KiB overflowed in the menu path; strong
    `__stacksize__ = 128 KiB` in `source/n3ds_boot.c` (`.3dsx`; the CIA
    exheader already used 256 KiB).
- **Device artifact**: `E:\cia_menu\Skyrim_N3DS_tesv_96x24_memgate.cia`
  (1 580 992 B, SHA-256 `d4ceddc4…683e`, title `000400000F0D5900`, 178 MB /
  804 MHz, profile **96x24**, guest budget 92 MiB). Also published at
  `artifacts/cia/Skyrim_N3DS_tesv_96x24_memgate.cia` with the device
  `Skyrim_N3DS.cfg`. Install instructions: `docs/CIA_INSTALL.md`.
  **120x24 never booted**: libctru's application heap area is
  `0x08000000..0x0E000000` (96 MiB), so a 120 MiB `__ctru_heap_size` aborts in
  `__system_allocateHeaps` (`svcBreak`) before `main`. `build.ps1` has 96x24 and
  `tesv_slice_main.c` rejects `__3DS__ && TESV_HEAP_MIB > 96` at compile time.
- **Two device gates closed.** (a) On the 3DS there is no Bink decoder, so
  `BinkOpen` now returns a valid **empty clip** (0x0, one frame, no-op copies)
  instead of NULL; the intro state finishes and StartMenu is created. (b) The
  PC minimum-config check calls `GlobalMemoryStatusEx` from `0x00a48a5d` and
  requires a 512 MiB commit limit; only that call is answered with 512 MiB
  (`[mem] startup minimum-config check: ... compat`), every other query stays
  honest and the 92 MiB guest budget is enforced. `TESV_MEMPROBE` is not needed
  and remains measurement-only.
- **Fast loop**: `tools\fast_slice.ps1` (auto-stop on fault/draw, stderr
  aware). Worktrees need `tools\prepare_worktree.ps1` (junctions `staged`,
  `pilot_out_mult`, `.venv`); without the D3DX pilot table the slice faults at
  `0x802fa80`, which is a build artifact, not a guest gap.
- **World**: stream index schema 2 covers exteriors; compressed BSA ranges
  work; Havok gravity runs in a host harness (world/body/context/queue/
  integrate). World shader runtime paths are 18/691 — the honest backlog.

## Older state (2026-11-27, `main`)

- **Paths.** The game install lives at `E:\SteamLibrary\steamapps\common\Skyrim`;
  the host runs the decrypted exe `E:\skyr\TESV.exe` (entry `0xb568a1`; the Steam
  exe has a DRM stub at `0x19882ee`). IRC: `E:\census_ir2\ir_container\tesv.irc`.
  Device builds use `E:\Skyrim_N3DS_build\live_arm_check2\gen` (32 AOT
  entries). Full-exe tracing needs the 90,620-entry table at
  `E:\Skyrim_N3DS_build\gen_full_check`; see "Build a full AOT table".
  **`F:` is the user's 3DS SD card and must never be read or written by tools.**
- **Menu blocker.** `0xa3cdd0` is *not* a retry loop: `0xac6c10` is a buffered
  stream read (0 = success) and the loop advances through ~2.8 MB of shader
  blobs. `0xa43f80` is a leaf range accessor; `0xa43fb5` is only the last
  published IR leader, not proof of a spin. After the preload the first
  `Present` happens and thread 11 runs D3D9 state setup (`0xf87bxx`,
  `SetTextureStageState`), but **no `Draw*` follows**. That is the current
  first divergent layer; the watchdog now dumps after a positive stall
  threshold (the old zero-threshold arm fired after 6 ms).
- **Heap.** `shader_new` clamped the retained word count: `realloc(code, 0)`
  with `info.words == 0` used to free the buffer and leave a dangling
  `o->code`. The fix and its regression are in; `0xC0000374` itself was not
  reproduced.
- **Shaders.** Six packs are merged: ui (19), skinning (83 VS contracts, 3
  `emitted-verified` `.shbin`), terrain (19 approx), lighting (34), blit_post
  (27), sky_water (14). Runtime table: 7 entries (4 canonical + 3 menu).
  Coverage: 177 translated / 7 draw-enabled / 6,475 refused + 19 extra-census.
  An external review of the PICA VS path (VS-01..VS-10) is being fixed;
  `VS-02` (independent bone indices hidden by the verifier) is first.
- **Menu BSA.** Boot now reads `sdmc:/Skyrim/Skyrim.ini` then
  `Data/Skyrim.ini`, mounts `sResourceArchiveList`/`List2` in order (32
  archives / 96 MiB index caps), and falls back to Interface+Update.
- **Deploy.** `tools/deploy_device.py plan|stage` resolves load-order winners
  and can write cooked archives (`--pack cooked`, uncompressed, group-split,
  `stream_index` phase 1 in progress). It never touches `F:`.

## Census history (superseded)

- Commit `8db224e` trims each shader copy to its declared token stream. The
  walker steps over instruction/comment lengths, so operand words that equal
  `0xffff` cannot truncate `GetFunction`; a regression covers that case.
- The fresh diagnostic container is
  `E:\census_ir2\ir_container\tesv.irc`: 90,900 functions / 90,027 decoded,
  64,445,696 B, index CRC verified, and `00f520f0` carries 25 blocks (including
  `00f520e0`) with `N3DS_IRC_FN_PREENTRY`. `F:\Skyrim\ir\tesv.irc` has the same
  entry but only 24 blocks and no PREENTRY flag, so it still faults at
  `00f520e0`; use the E: container for diagnosis.
- With the E: container the census passes `00f520e0`, creates the 512x1024
  depth/target pair, runs `BeginScene`/`EndScene`/`Present`, and then spends the
  remaining step budget in an untraced guest loop whose last shim is
  `[sync] Sleep 28 ms from 006f40cf thread=11`. There is no fault message; a
  100M-step run does not write a profile, so the next step is to identify that
  spin (likely a guest thread waiting on a lock the main thread never releases)
  before the next fault can be named. The trace before the spin contains 878
  `CreateVertexShader` (558 ok, 320 x `0x8876086a`), 40 `CreatePixelShader`
  (all ok), 372 `CreateVertexDeclaration` (364 ok, 8 x `0x8876086c`), 64
  `SetVertexShader`/`SetPixelShader`, one 512x1024 depth/target pair and the
  repeated `Present` cycle. The earlier `549a9038` guest OOM did not recur; a
  300M-step run later ended with host `0xC0000374` heap corruption instead.
- The earlier 69 GiB `tools/ir_pack.py` blow-up was duplicate promotion in the
  closure queue: every decoded edge appended another copy of the same pre-entry
  leader. `8db224e` keeps one closure copy per leader and orders it before the
  excluded discovery range. A clean 90,900-function pack now finishes at
  64,445,696 B (index 3,636,000 / data 60,809,632) with 982 `PREENTRY` entries;
  `E:\census_ir2\ir_container\tesv.irc` is that file.
- `TESV_HANG_WATCHDOG_MS=<ms>` (host slice only, default off) arms after the
  first real Present and dumps every thread, wait set, stack, IR block, sync
  objects, CS owners and the Interlocked ring before exiting 70. The menu hang
  is a real guest retry loop, not a bad leader: the actual guest image has
  `0xa3ce84: cmp [esp+0x10],0`, and the main loops over `0xa3cdd0..0xa3d01a`
  with the Interlocked word `0x541e118c`. Thread 8 waits on semaphore `0x504`;
  thread 12 waits on `0x537`, which is no longer a live sync object. Closing a
  sync object now wakes waiters and the wait loop returns
  `ERROR_INVALID_HANDLE` instead of blocking forever, but thread 12 still never
  reaches that path, so its producer is the next lead.

## Commands (Windows)

```powershell
# host suite (use E: because C: has hit "no space on device")
$env:HOST_CLANG_JOBS='8'
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -TesvIrc E:\census_ir2\ir_container\tesv.irc `
  -TesvSliceGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -TesvExe E:\skyr\TESV.exe -AllowDiagnosticFault `
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

### Build a full AOT table (`gen`) from the decrypted EXE

`live_arm_check2/gen` is a 32-entry device table. It cannot trace
`0xa43f80`/`0xa4b4a0`. The full table is a lift of the whole discovery
manifest; it is generated output and is gitignored, so it can be lost.

Stable inputs (verified 2026-11-27):

```
exe:        E:\skyr\TESV.exe        17,462,064 B
            SHA256 D97EB0CC0CEEF0923D2234D42B382C0D033853356528B271BC043542B1F5E605
functions:  E:\Skyrim_N3DS_build\analysis\tesv\functions.json
            18,721,072 B, 90,900 functions
            SHA256 1FC28CE90D668D635ACE8FD641D95720782E24C0F64EAF08CBAF8BF170D15B6D
```

The only surviving copy of `functions.json` after the `analysis/` cleanup was
`E:\Skyrim_N3DS_build\sd_cleanup_20261124_160253\analysis\tesv\functions.json`
(byte-identical); it is restored at the stable path above. Do not delete it.

`staged/` is gitignored. In a fresh worktree copy it from the main tree first
(`lift.py`, `peimg.py`, `x86.h`, `rt_table.h`, `lift_prelude.h`, `fpu.c`), or
the host compile fails with `'x86.h' file not found`.

```powershell
$py = 'C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe'
$env:HOST_LIFT_JOBS = '8'   # ~79 s for 90,900 functions on this workstation

# 1. lift all discovered functions (do NOT target live_arm_check2)
& $py staged\lift.py E:\skyr\TESV.exe `
    E:\Skyrim_N3DS_build\analysis\tesv\functions.json `
    E:\Skyrim_N3DS_build\gen_full_check tesv 400000
# expected: round 1 90900 pending / 90620 accepted / 280 rejected
#           round 2 no change; "lifted 90620 rejected 280"

# 2. instrument (needed by build.ps1 and n3ds_pilot_tick_pc)
& $py tools\instrument_generated.py E:\Skyrim_N3DS_build\gen_full_check

# 3. KNOWN TOOL BUG: instrument_generated.py also rewrites the lifter's
#    `L_dispatch:` label to `n3ds_pilot_tick_pc(0xdispatchu);`, which clang
#    rejects ("invalid suffix 'ispatchu'"). Remove just those lines, or fix the
#    label regex to accept hex-only labels. (live_arm_check2/gen predates this.)
Get-ChildItem E:\Skyrim_N3DS_build\gen_full_check -Filter tesv_*.c | ForEach-Object {
    $t = Get-Content $_.FullName -Raw
    $t = [regex]::Replace($t, '(?m)^\s*n3ds_pilot_tick_pc\(0xdispatchu\);\r?\n?', '')
    Set-Content $_.FullName -Value $t -NoNewline
}
```

Expected output: 32 files, **375,485,718 B**, `tesv_table.c` with **90,620**
entries, `funcs_tesv.h` with 90,620 prototypes, including
`{0xA43F80u, f_A43F80}` and `{0xA4B4A0u, f_A4B4A0}`. Directory hash
`SHA256 bf97c76d09063bd0678d12f6253eb820669b7a555d716f0ea312a7c9121d6fec`
(aggregate over `name<space>file-sha256` sorted by name, as `build.ps1`).
`tesv_table.c` itself is
`6D8B1CE54C6C1A2DC444203FBD2A10EF3CBFC71692C73EA1FE757E2DE1324715`.

Sanity check and host compile (cold cache, ~77 s; `.text` is 372,690,070 B
from `llvm-size`):

```powershell
Select-String E:\Skyrim_N3DS_build\gen_full_check\tesv_table.c -Pattern 'A43F80|A4B4A0'
$env:HOST_CLANG_JOBS = '8'
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -TesvIrc E:\census_ir2\ir_container\tesv.irc `
  -TesvSliceGen E:\Skyrim_N3DS_build\gen_full_check `
  -TesvExe E:\skyr\TESV.exe -AllowDiagnosticFault `
  -OutDir E:\Skyrim_N3DS_build\host_gen_full -TestTimeoutSec 300 `
  -Only tesv_slice -SkipPython
# result 2026-11-27: COMPILE/LINK OK, then "HOST DIAG: guest called
# ExitProcess(0) ... 'Not enough memory to run application.'" at the default
# 200k steps (no TESV_MEMPROBE). Diagnostic, not PASS.
```

For a real trace run, run the built exe with the memory probe and more steps:

```powershell
cd E:\Skyrim_N3DS_build\host_gen_full\run_tesv_slice_<pid>
$env:N3DS_IRC_PATH = 'E:\census_ir2\ir_container\tesv.irc'
$env:TESV_MEMPROBE = '1'
$env:TESV_A43F_TRACE = '1'   # optional
.\tesv_slice.exe E:\skyr\TESV.exe . 200000000 167772160
```

Result 2026-11-27: `CreateTexture` path runs (48 `VirtualAlloc` calls, 721 MiB
requested) but the first fault is
`[guest] unmapped addr=00000008 ... last_call=00ac6400 irc_fn=00000000`;
`0xa43f80` is never entered and there is no `BeginScene`/`Present`. This is the
same `0xac6400`/addr-8 blocker recorded in PORT_LOG row 256 for `gen_trace`;
the full table does make `0xa4b4a0` AOT (`irc_fn=0`), so the next step is to
compare the `0xa4b4a0` block ranges with the IRC (the packer adds pre-entry
leaders that discovery may not list) rather than to rebuild the table.

Never write the table to `live_arm_check2/gen` and never put it on `F:`.

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
- Host suite: **149 passed, 1 diagnostic, 0 failed** in ~150-200 s warm.
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
5. Keep the honest gates honest: solving the census does not mean advertising
   SM3/T&L; measurement flags must stay out of shipping defaults.

## Gotchas

- `docs/PORT_LOG.md` has had mixed CRLF/LF churn. Edit it with a script that
  preserves the current EOL; keep one row per layer and renumber conflicts after
  merges (agent branches are often based on older mains).
- Prefer Python script files over PowerShell inline `-c` strings for edits with
  quotes/backticks; the shell mangles them.
- `staged/` and `pilot_out_mult/` are ignored local inputs, not worktree files.
  Copy them from the main tree or pass `-PilotGen ...\D3DXMatrixMultiply\gen`;
  otherwise the rebuilt slice resolves `D3DXMatrixMultiply` to its real body
  `0x0802fa80` without linking the lifted body and faults there.
- `source/tesv_slice_main.c` must include `<unistd.h>` only inside `#ifdef
  __3DS__` (Windows host clang has no such header).
- `tools/run_host_tests.ps1` gates `tesv_slice`/`tesv_slice_cube_probe` sources on
  the generated slice contents; do not unconditionally link the probe slices.
- GitHub SSH (`ssh.github.com:443`) intermittently times out; retry the push.
- Agents work in `C:\Users\vasya\Documents\Skyrim_N3DS_wt\<name>` worktrees;
  never edit the main tree while a full suite runs.
