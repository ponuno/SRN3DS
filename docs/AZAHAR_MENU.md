# Real TESV in Azahar: what boots, what stops it (2026-11-27)

Worktree `azahar-ready` (base `c56f2d7`, branch `azahar-ready`). This note is
empirical: it records what the real TESV slice does in desktop Azahar 2126.2
when launched as a `.3dsx` with the harness data, and which failure is real
versus which was a harness/configuration artefact. No playable build, no
frame, and no pixel claim. `DevCaps` was not changed.

## TL;DR

1. A **136x8** heap profile cannot boot as a `.3dsx` in Azahar. The homebrew
   application region there is **96 MiB** (`os_region_size=100663296` in the
   mem log). libctru breaks at heap setup before `main`:
   `svcBreak` / `Reason: PANIC` at emulated t=1.42 s, PC `0x002732c0`,
   `R1=0x00800000` (8 MiB linear), `R3=0x08800000` (136 MiB requested heap).
   136x8 is a 178 MiB **CIA** profile, not a `.3dsx` profile.
2. With the only viable `.3dsx` profile (**96x8**, guest budget 92 MiB) the
   slice boots and executes real TESV code. It does **not** fail on unresolved
   imports: those are bound to call-time traps at boot and listed at exit.
3. The first real stop is the engine's own memory gate at **`0x00f5894a`**:
   `[messagebox] Not enough memory to run application.` then
   `ExitProcess(0)`. This is the known 512 MiB `AvailPageFile` gate; with the
   probe off no guest budget passes it (`guest_used` was only 32.2 MiB of 92).
   The measurement-only `-ForceMemProbe` build passes it.
4. With `-ForceMemProbe` the run proceeds through the loader and reaches
   `0x00a4b470`/`0x00a4b500` and the `0x006e3e30` / `0x006ebc10` menu-chain
   entries. The StartMenu checkpoint `0x00876f60` is the next target.
5. After fixing the 4 MiB D3D9 resource budget, the engine loads D3DX
   textures and submits menu shaders, then dies because four 512x512 PICA
   colour targets do not fit in 6 MiB VRAM. That is the current wall
   (`Guest fault at 00000030`, `last_call=00c8fc90`).
6. `run_azahar.ps1 -SdRoot` only controls where the `.3dsx` is copied and
   where logs are archived. **Azahar always reads its own virtual SD**
   (`%APPDATA%\Azahar\sdmc`) unless custom storage is enabled in its GUI.
   Editing `qt-config.ini` for custom storage did not stick (Azahar rewrites
   it to `use_custom_storage=false` on start). The final run therefore uses a
   junction `%APPDATA%\Azahar\sdmc\Skyrim -> E:\azahar_sd\Skyrim`, which keeps
   the game data on `E:` and never touches `F:`.

## Build

```powershell
# 136x8 (the brief): builds, but cannot boot as .3dsx (see above)
tools\build.ps1 -Mode tesv -MemProfile 136x8 `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -Isolated azmenu2 -TesvArmSteps 2000000000
# artifact sha256 772bf818... (2,839,908 B)
```

`TESV_ARM_STEPS` is supported by the Makefile (`ifdef TESV_ARM_STEPS ->
-DTESV_ARM_STEPS=$(TESV_ARM_STEPS)`) and is passed by
`tools/build.ps1 -TesvArmSteps 2000000000`; no source edit was needed for the
step count. `-O2` is the default.

The runnable `.3dsx` used here is 96x8 plus the measurement-only probe:

```powershell
tools\build.ps1 -Mode tesv -MemProfile 96x8 `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -Isolated azmenu2 -TesvArmSteps 2000000000 -ForceMemProbe -NoClean
# -> dist\Skyrim_N3DS_tesv_azmenu2\Skyrim_N3DS_tesv_azmenu2.3dsx
#    final (resource budget fix): sha256
#    be2ad6ee4536af6d09d3f2d08bcca3febb606bf35b515132df136c484406969f
#    2,840,404 B; .text 2,816,584 B; generated dir live_arm_check2/gen
```

The PICA backend now takes its resource budget from `TESV_D3D9_BUDGET_MB`
(clamped 8..128, default 4 when unset), so the same value that the host sets
in the environment can be supplied on device by
`sdmc:/Skyrim/tesv_d3d9_budget.txt`. The host build and `DevCaps` are
unchanged; the edit is inside the `__3DS__` branch and the device main.

`-ForceMemProbe` is a build-time diagnostic: it tells the engine it can commit
512 MiB. It is required to get past `0xf5894a`; it is not shippable and it does
not create memory.

## Virtual SD

Final layout read by Azahar:

```
%APPDATA%\Azahar\sdmc\Skyrim  -> junction to E:\azahar_sd\Skyrim
E:\azahar_sd\Skyrim\TESV.exe          (17,462,064 B)
E:\azahar_sd\Skyrim\Skyrim.ini        (sIntroSequence=, bPreloadIntroSequence=0)
E:\azahar_sd\Skyrim\ir\tesv.irc       (65,688,624 B, MMX)
E:\azahar_sd\Skyrim\Data              -> junction to E:\harness_skyrim\Data
E:\azahar_sd\Skyrim\d3dx9_42.dll      (1,892,184 B, real D3DX exports)
E:\azahar_sd\Skyrim\binkw32.dll       (214,016 B)
E:\azahar_sd\Skyrim\X3DAudio1_7.dll   (22,360 B)
E:\azahar_sd\Skyrim\steam_api.dll     (145,600 B)
E:\azahar_sd\Skyrim\Documents\My Games\Skyrim\SkyrimPrefs.ini  (iSize W=400 H=240)
E:\azahar_sd\Skyrim\LocalAppData\Skyrim\Skyrim.ini             (no intro, ENGLISH)
E:\azahar_sd\Skyrim\tesv_trace.flag        (device-only menu/draw trace)
E:\azahar_sd\Skyrim\tesv_caps_probe.flag   (measurement-only HW T&L/SM3/skip)
E:\azahar_sd\Skyrim\tesv_d3d9_budget.txt   ("32" -> 32 MiB resource budget)
```

The two INI files and the four DLLs came from the previous
`%APPDATA%\Azahar\sdmc\Skyrim` seed (they are game inputs, not emulator
state). Without the INIs the engine uses PC display defaults and fails the
renderer; without `d3dx9_42.dll` its exports are traps instead of lifted
bodies.

The old `%APPDATA%\Azahar\sdmc\Skyrim` (whose `Data` junction pointed at `F:`)
was renamed to `Skyrim.f-backup-azmenu2`; restore it by renaming back after
deleting the junction. The run itself never reads `F:`.

`run_azahar.ps1` was called with `-SdRoot "$env:APPDATA\Azahar\sdmc"` so its
log archiving matches the SD Azahar actually uses.

Azahar profile: Azahar 2126.2, `System_IsNew3ds=true`, 400% CPU clock,
virtual SD, no GDB stub. The `.3dsx` application region is still 96 MiB.

## Device-only tracing

The 3DS process cannot inherit host environment variables, so
`TESV_MENU_TRACE=1` / `TESV_DRAW_PROBE=1` had no effect in Azahar. Added in
`source/tesv_slice_main.c` (host behaviour unchanged):

```c
if (access("sdmc:/Skyrim/tesv_trace.flag", F_OK) == 0) {
    setenv("TESV_MENU_TRACE", "1", 1);
    setenv("TESV_DRAW_PROBE", "1", 1);
}
```

The full `TESV_D3D9_TRACE` is deliberately not enabled from the flag: it logs
every COM call. This flag only adds the `[menu]` and `[drawprobe]` lines.

## What the log says

`Skyrim_N3DS_tesv.log` from the 96x8/probe-off run (4900 B), at exit:

```
  unresolved import GDI32.dll!GetTextMetricsA
  ... (font/DC/GDI helpers)
  unresolved import USER32.dll!BeginPaint
  unresolved import USER32.dll!ChangeDisplaySettingsA
  unresolved import msvcrt.dll!_adjust_fdiv
executed functions: 16, calls: 464
IR cache: blocks=727 bytes=72264 hits=5851 misses=727
```

and in `Skyrim_N3DS_stderr.log`:

```
[registry] install state: HKLM\Software\Bethesda Softworks\Skyrim sBasePath=C:\Skyrim\ ...
IR container: 114534 functions (113618 decoded) from sdmc:/Skyrim/ir/tesv.irc
[messagebox] Not enough memory to run application.
[guest] ExitProcess(0) from 00f5894a stack: 00f5894a ...
```

So the import list is **informational**: `n3ds_boot_prepare` succeeds because
`win32_resolve` manufactures a trap thunk for every unresolved import
(`win32_set_unresolved_trap(1)`), and `n3ds_boot_prepare` only rejects an
import when even the fallback returns 0. A trap fires only if called, and at
that point it faults with a named `unresolved import DLL!Func` reason. None of
the GDI32/USER32 entries above needed to run before the gate.

Memory snapshot at that exit (96x8): `guest_used=32,223,232`,
`guest_peak=32,223,232`, `newlib_free=270,480`, `linear_fail_n=0`,
`guest_backing_failures=0`, `os_region_size=100,663,296`, `os_region_used=
113,926,144` (region query includes the linear heap). This is not allocator
exhaustion: `newlib_free` was still 264 KiB and no backing/linear failure
counter moved. It is the explicit 512 MiB gate.

## Size of the real problem

Host peak at the menu is 129.4 MiB guest under 136x8 (budget 132 MiB). A 96x8
`.3dsx` exposes 92 MiB of guest budget. Even after the `0xf5894a` gate, the
menu path cannot fit in 92 MiB. The only profiles that can carry the menu are
the 178 MiB CIA profiles (`136x8`, `132x12`). Therefore:

- `.3dsx` + Azahar: 96x8 max, enough to measure the loader and prove the gate,
  not enough for StartMenu.
- CIA (178 MiB exheader) + 136x8: the only configuration that can reach
  `0x00876f60` with the measured 129.4 MiB peak.

## Commands

```powershell
tools\run_azahar.ps1 `
  -ThreeDsx dist\Skyrim_N3DS_tesv_azmenu2\Skyrim_N3DS_tesv_azmenu2.3dsx `
  -SdRoot "$env:APPDATA\Azahar\sdmc" -Name azmenu96x8probe `
  -TimeoutSec 1500 -SkipAppLogCheck -AllowFaultReport
```

Expected time: the host reaches the menu in ~228 s. Azahar at 400% clock took
≈9-11 min to the renderer and ≈12 min to the current fault. Budget 20-30 min
for the loader + menu path; `TimeoutSec` does not shorten the run because the
script always waits the full interval before the screenshot.
The first evidence of progress is `Skyrim_N3DS_stderr.log` growing with
`[menu] enter ...` lines; `Skyrim_N3DS_tesv.log` is only written at exit.

## Results of this attempt (seven Azahar runs)

| Run | Profile / flags | Result |
|---|---|---|
| 1 `manual-136x8-panic` | 136x8 | libctru `svcBreak` PANIC at emulated t=1.42 s, before `main`. `.3dsx` region is 96 MiB. |
| 2 `manual-run-96x8-appdata` | 96x8, no probe, old `F:`-backed AppData SD | boot + 32 MiB guest, then `[messagebox] Not enough memory` / `ExitProcess(0)` at `0x00f5894a`; unresolved imports listed at exit, trap-bound, not fatal. |
| 3 `20261127-233607-azmenu96x8caps` | 96x8, `-ForceMemProbe`, caps probe | past the 512 MiB gate; D3D9 `GetDeviceCaps` reports HW T&L (measurement only); `CreateDevice` refuses `back buffer is not the 400x240 display mode` x4; `[messagebox] Failed to initialize renderer / Unknown error` at `0x0069107d`. |
| 4 `azmenu96x8prefs` | + `Documents\My Games\Skyrim\SkyrimPrefs.ini` (`iSize W=400 H=240`) and `LocalAppData\Skyrim\Skyrim.ini` | `CreateDevice` succeeds and the renderer path runs. First guest fault: `Guest fault at 00000000: unresolved import d3dx9_42.dll!D3DXMatrixMultiply`; `newlib_free`/alloc counters still clean. The module was missing from `E:\azahar_sd\Skyrim`. |
| 5 `azmenu96x8dll` | + `d3dx9_42.dll`, `binkw32.dll`, `X3DAudio1_7.dll`, `steam_api.dll` | D3DX is resolved through the lifted pilot body. The renderer creates 128x128/8x8 colour-only targets until the hardcoded **4 MiB** resource budget is full (`used 4192344/4194304`, `still short 1046616`); the next 1 MiB render-target texture is NULL, so the guest dereferences NULL+0x30: `Guest fault at 00000030: unmapped guest memory`, `last_call=00c8fc90`, `irc_fn=00fb5f30`, `ir_ip=00c8fc90`. `guest_used=56,621,248`; VA/allocation failure counters 0. |
| 6 `azmenu96x8bud32` | + `tesv_d3d9_budget.txt=32` | The file was read (`[probe] TESV_D3D9_BUDGET_MB=32`) but the PICA backend still used 4 MiB: that path passed an explicit `4u<<20` to `d9_com_set_backend` and only the null backend honoured the env. Same `0x30` fault, identical budget lines. |
| 7 `20261128-003715-azmenu96x8bud32b` | artifact `be2ad6ee...` after the limit fix | Budget fix works (no more budget-trim lines). Rendering goes much further: D3DX textures load, menu shaders are submitted (`menu.generic.vs`), four 512x512 colour targets are requested. **PICA VRAM is exhausted**: `CreateRenderTarget: 512x512 VRAM texture allocation failed (vram free 274 KiB)`, then `77 KiB`; the failed target is NULL and the guest faults at `0x30` again (`last_call=00c8fc90`, `irc_fn=00fb5f30`, `ir_ip=00c8fc90`). `guest_used=56,617,152`, `newlib_peak=79,036,664`, `newlib_free=48,992`, `linear_fail_n=0`, `guest_backing_failures=0`, `alloc_failures=4` (the four failed target creations). |

Screenshots and full logs are under `build_azahar/`. The probe-off run's
console screenshot (`manual-run-96x8-appdata/screen.png`) is exactly the
"unresolved import ... / START: exit" screen: it is the post-run trap list, not
a bootstrap refusal.

### Ordered blockers observed on the device

1. `.3dsx` region is 96 MiB, so 136x8 cannot boot (CIA/178 MiB needed).
2. Engine 512 MiB gate at `0xf5894a` — needs the measurement-only memprobe.
3. No HW T&L in the honest adapter caps — engine refuses the renderer unless
   the measurement-only caps probe is on (or real HW T&L is implemented).
4. Missing per-profile INI files make the engine use PC display defaults and
   `CreateDevice` rejects the non-400x240 back buffer.
5. Missing `d3dx9_42.dll` / `binkw32.dll` / `X3DAudio1_7.dll` in the custom
   game root leaves the real D3DX exports absent, so `D3DXMatrixMultiply`
   becomes a trap. The lifted pilot table for `0x0802fa80` is already linked;
   the DLL image must be present for the import to reach it.
6. The TESV-mode backend installs a **4 MiB** texture cache
   (`d9_com_set_backend(..., 4u<<20)`), while the menu path needs at least a
   1 MiB render-target texture on top of ~4 MiB of live textures. Host runs
   override it with `TESV_D3D9_BUDGET_MB` (census used 92). On device the
   environment is not inherited; a one-line
   `sdmc:/Skyrim/tesv_d3d9_budget.txt` now sets the same variable
   (`[probe] TESV_D3D9_BUDGET_MB=32 from flag file`) and the PICA path now
   uses it. This is a measurement setting, not a capability change.
7. **PICA VRAM (6 MiB) is the current wall.** After the budget fix the engine
   requests at least four 512x512 RGBA8 colour targets (1 MiB each) on top of
   the 512x256 retained back buffer + depth and the smaller targets. The
   fourth allocation fails (`vram free 274 KiB`, then `77 KiB`), the target
   handle is NULL, and the guest dies at `0x30` in the same
   `NiRenderTargetGroup` getter (`0x00c8fc90`). This is real VRAM pressure,
   not guest RAM: `guest_used` is 54 MiB, `newlib_free` is not zero and every
   VA/allocation failure counter stays 0.

- Screenshot (latest): `build_azahar/20261128-003715-azmenu96x8bud32b/screen.png`
  (console after exit: `START: exit` + trap list; no menu pixels)
- Screenshot (probe-off run): `build_azahar/manual-run-96x8-appdata/screen.png`
- Last checkpoint: menu shader/texture path, 512x512 target creation
- `[menu] enter 00876f60`: not yet observed
- First guest fault (latest): `0x00000030`, `unmapped guest memory`,
  `last_call=00c8fc90`, `irc_fn=00fb5f30`
- Earlier import traps were **not** the cause; they are bound at boot and only
  named if called.

## Next exact steps (no half-fixes)

1. **VRAM target set.** The immediate blocker is four 512x512 colour targets
   plus the 512x256 retained back buffer inside 6 MiB. The honest fixes are:
   reuse/share the transient 512x512 render-target natives at `Present`
   instead of keeping all of them resident, or allocate them colour-only (already
   done) and evict the ones no longer bound. Shrinking the native allocation
   silently would change what the guest samples and is not acceptable.
2. **178 MiB profile.** Once the target set fits, package the 136x8 ELF as a
   CIA (`tools\build_cia.ps1`) and install it in Azahar (GUI: File > Install
   CIA). The exheader requests 178 MiB; only then does `os_region_size` become
   178 MiB and 136x8 boot. With 92 MiB guest budget the menu may still hit the
   host-measured 129.4 MiB peak, which is why the CIA is the target profile.
3. Keep `-ForceMemProbe` and the caps probe out of any shippable build. They
   only remove arithmetic/caps gates and are labelled measurement-only in the
   log; `DevCaps` itself is unchanged.
4. The shader-skip probe rejects the real `menu.generic.vs` (`blendweight0`,
   opcode `0x24`, `lrp`, 107 instructions > 96). That is expected for the
   measurement build and means no draw/pixel claim; a real menu frame needs
   those VS paths implemented, not skipped.

## Emulator versus hardware

- Azahar runs the ARM11 code in a JIT with `cpu_clock_percentage=400`, so the
  wall-clock times here (≈5 min of early loader, ≈9-11 min to the renderer)
  are **not** hardware timings. A New 3DS at 804 MHz will differ.
- PICA VRAM is emulated as a 6 MiB budget; `vram free` lines are the
  emulator's accounting. The same 6 MiB limit exists on hardware, but the
  failure order and exact allocation sizes still need a hardware run.
- The virtual SD is a host directory (`%APPDATA%\Azahar\sdmc`), so file I/O
  latency and the `CreateDirectory: Path not found` messages are emulator FS
  behaviour, not the console's SD driver.
- No frame or pixel result is claimed. The only screenshot here is the console
  text screen; the top screen never received a menu frame in these runs.

## Restoring the host state

- `%APPDATA%\Azahar\sdmc\Skyrim` is now a junction to `E:\azahar_sd\Skyrim`.
  The previous directory was renamed to
  `%APPDATA%\Azahar\sdmc\Skyrim.f-backup-azmenu2` (its `Data` junction pointed
  at `F:`). To restore: remove the junction and rename the backup back.
- `qt-config.ini` was backed up as `qt-config.ini.bak-azmenu2` and restored;
  `use_custom_storage` remains `false`.
- The only source change is `source/tesv_slice_main.c`: device-only trace,
  budget and caps flag handling plus the PICA resource-budget selection. Both
  edits are inside the `__3DS__` main/run path, so the host build and the
  full host suite are unchanged. The host suite was **not re-run** in this
  timebox; run `tools\run_host_tests.ps1` before merging.

## 2026-11-28: device still takes the load branch after the empty-clip fix

`b03c33c` gives `BinkOpen` an empty finished clip on a decoder-less platform.
Azahar does not call `_BinkOpen` at all (only `_BinkSetSoundSystem`); the run
stays in the `mode=2/2` loading pump with `singleton=0`, `view=0` and never
enters `00876f60`.  Empty and `BGS_Logo.bik` intro values, and removing the
guest `binkw32.dll`, all produce the same branch.  The next gate is the
startup probe `0x6982c0` at `0x69ddf5`; log its `AL`, out object and the
`0x1272fb4/0x1272fd8/0x1272fe4` settings there.
