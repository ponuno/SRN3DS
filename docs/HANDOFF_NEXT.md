# Next-session handoff — 2026-11-28 evening

Read this first, then `docs/HANDOFF.md` (current-state section),
`docs/CIA_INSTALL.md`, `docs/AZAHAR_MENU.md` and `docs/VRAM_BUDGET.md`.

## Repository / artifacts

- `main` = `330bb5d`; `tools\run_host_tests.ps1` = **219 passed, 0 failed**.
- Device artifact: `E:\cia_menu\Skyrim_N3DS_tesv_96x24_memgate.cia`
  (1 580 992 B, SHA-256 `d4ceddc4…683e`), also in
  `artifacts/cia/Skyrim_N3DS_tesv_96x24_memgate.cia`.
- Device cfg: `artifacts/cia/Skyrim_N3DS.cfg` → `sdmc:/Skyrim_N3DS.cfg`.
- Canonical IR container: `E:\census_ir2\ir_container\tesv.irc` (65 688 624 B,
  SHA-256 `4DD060A5…0C12`).
- Fast AOT gen: `E:\Skyrim_N3DS_build\live_arm_check2\gen` (32 bodies + the
  63 912-entry IR function map). Full gen: `E:\Skyrim_N3DS_build\gen_full_check`
  (90 620 bodies; links but does **not** fit — see below).
- Experimental branch: `startup-patch` `bb1da34` (not merged, faults).
- Device-probe branch: `vram-budget` (diag commits `0bdb8c7`, `43ed90a`,
  `7d05b38`; forced-push to origin may be in flight).

## What works

- Host reaches the real StartMenu: `00876f60 → 00876fc0 → 00876b90`,
  `singleton=41f8eaf0`, GFx message switch `008779d0` every frame, movie loaded
  (`[movie+0x10]=1`).
- CIA boots on the console at **96x24** (the libctru heap maximum; 120x24
  aborts inside `__system_allocateHeaps`).
- Startup memory check and Bink are handled:
  `[mem] startup minimum-config check … compat` and
  `[bink] … no Bink decoder … empty clip`.
- No guest fault/OOM in the long Azahar runs; swap reports no failures.

## Blockers (in priority order)

### 1. Device never finishes the startup path

- The gate `0x6982c0` is called from WinMain `0x69dd9f`; its branch at
  `0x69ddf9` selects menu (`0x69de08`) vs load (`0x69de54`). Host takes menu,
  device takes load (`0x8c71e0 → 0x6f4045` loading pump, `mode=2/2 inload=1`,
  `singleton=0`).
- The VM-init handoff (`h=50c`) alone takes **~12–13 minutes** under IR on
  Azahar; 500 s probes never reach the gate. `release h=504` (GFx worker at
  `0xa5ac42`) never happens.
- Block probes fire for `0xa4b500` (positive control) but not for the gate.
  The gate is not one of the 32 AOT bodies; it should run through the
  **container** executor (`irc_run`). Verify that the probe is compiled into the
  `ir_container.c` copy of `ir_exec.c`, or add a function-entry probe in
  `n3ds_irc_exec`/`container_dispatch`, then run **900–1000 s**.
- The experiment in `startup-patch` (NOP the `je` at `0x69ddf9` in
  `pe_image.c`, exclude WinMain `0x69d1d0` and gate from the AOT lookup) applies
  the image patch (`[patch] image patch 0069ddf9: 2 byte(s) applied`) but the
  IR WinMain route faults at `0009007d` (`0xac6580`, `ir_ip=0xc389d3`) after the
  character/menu textures load. Do not merge until that IR path is fixed.

Next actions: (a) probe the container executor at the gate; (b) if AL is 0,
fix the settings/out object or patch WinMain **inside the container/AOT path**
instead of excluding it; (c) get a hardware log from the current CIA — real
hardware is much faster than Azahar and may already reach the gate.

### 2. Menu is not drawn: the fader is missing

- `FadeInMenu` is **message type 0 inside `008779d0`**, not a function. It
  arrives with `ptr=0`; the StartMenu movie exists but the global fader
  `[0x1b10dcc]+0x28` is **0**, so the case is skipped.
- Routing the type-1 movie into the type-0 message is **wrong**: the handler
  calls that pointer as a function and the run faults with
  `unknown indirect function` at the movie address. That fallback is dormant in
  `main` (env-gated) and must not be forced.
- Next: find why the FaderMenu (`0xa60c80("FaderMenu")` from `0x85b094`) is not
  registered in the manager at `+0x28`; trace the FaderMenu's message switch and
  writes to `[0x1b10dcc]+0x28`. The loading-screen completion likely installs the
  fader.

### 3. Startup speed / 20-minute wait

- Full AOT is **not shippable**: 90 620 bodies → 135.2 MiB `.text`, ends at
  `0x0866BA48`, crossing `OS_HEAP_AREA` (`0x08000000`) by 6.4 MiB; `.3dsx` is
  142.6 MB vs a 92 MiB region. `makerom` packs it silently — that does not make
  it bootable.
- Use a **hybrid hot subset**: pick the hottest functions from a profile, keep
  generated code below `0x08000000` (e.g. 10–20 MiB), the rest from `tesv.irc`.
  Measure `.text/.rodata/.data/.bss` end and A/B `guest_used`, `newlib_free`,
  `linear_live/min_free`, `tex_guest/tex_gpu` at menu and the first cell. **Do
  not ship if any runtime RAM counter grows.** Also consider `-Os` for generated
  units and removing the 821 169 per-block `n3ds_pilot_tick` labels.
- `TESV_ARM_STEPS=0` means unlimited (`n3ds_pilot_set_steps`); pass it via the
  environment (build.ps1 skips falsy `-TesvArmSteps`).

## Pitfalls learned today

- `E:\host_objcache` is not reliable across source edits: stale objects made
  `TESV_FORCE_MEMPROBE` and the fade fallback silently absent. For validation
  use a fresh `HOST_OBJ_CACHE` directory.
- Host slice runs are starved by Azahar/AOT builds; do not judge a host
  experiment that got < ~100k log lines.
- The device 120x24 CIA is invalid; max newlib heap is 96 MiB
  (`OS_HEAP_AREA 0x08000000..0x0E000000`). `tesv_slice_main.c` now has a
  compile-time `#error` for `__3DS__ && TESV_HEAP_MIB > 96`.
- `TESV_MEMPROBE` is measurement-only; the startup check is answered by the
  targeted compat shim at caller `0x00a48a5d`.
- The fallback in `main` is dormant; the `startup-patch` branch is experimental
  and faults.
- `DevCaps` stays `0x00082000`; never fabricate pixels/sizes; `F:` is the
  user's card and is never touched by tools.

## Commands

```powershell
# host menu loop (fresh cache for source changes)
$env:HOST_OBJ_CACHE='E:\host_objcache_next'
tools\fast_slice.ps1 -OutDir E:\slice_next -Irc E:\census_ir2\ir_container\tesv.irc `
  -HangMs 2000 -NoShaderSkipProbe -TimeoutSec 400

# suite
tools\run_host_tests.ps1 -OutDir E:\suite_host -TestTimeoutSec 600

# CIA (96x24 only)
tools\build.ps1 -Mode tesv -MemProfile 96x24 `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -TesvArmSteps 2000000000 -Opt -O2 -Isolated 96x24 -Jobs 8
tools\build_cia.ps1 -Elf dist\Skyrim_N3DS_tesv_96x24\Skyrim_N3DS_tesv_96x24.elf `
  -Out E:\cia_menu\Skyrim_N3DS_tesv_96x24_<name>.cia -WorkDir E:\cia_menu\work_96x24

# Azahar (device evidence; SD root %APPDATA%\Azahar\sdmc)
tools\run_azahar.ps1 -ThreeDsx <file>.3dsx -Name <name> -TimeoutSec 1000 `
  -SkipAppLogCheck -AllowFaultReport
```

## Definition of done for the next session

1. Device reaches `00876f60`/`00876fc0`/`00876b90` and `[menustate]`
   `singleton != 0`, with a log proving it.
2. `FadeInMenu` gets a real fader/movie and a `DrawIndexedPrimitive` menu frame
   is observed (no fabricated pixels).
3. A hybrid AOT build that stays below `0x08000000` and does not increase any
   runtime RAM counter, measured before/after.
