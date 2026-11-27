# TESV CIA on hardware — install and first-run guide

Status: **build + file layer**. This document describes a local diagnostic CIA
that boots the real `TESV.exe` under the lifted x86 runtime on a New 3DS. It
does not claim a playable frame. The CIA contains locally generated code derived
from the owner's game files; do not distribute it.

## 1. Why a CIA and not a `.3dsx`

The slice needs the New 3DS **178 MiB application region**, 804 MHz and the L2
cache. Those are exheader properties of a CIA; a `.3dsx` under hbmenu gets the
default memory mode. `tools/cia/template.rsf` sets:

```
MemoryType      : Application
SystemMode      : 96MB
SystemModeExt   : 178MB
CpuSpeed        : 804MHz
EnableL2Cache   : true
UniqueId        : 0xF0D59   -> title ID 000400000F0D5900
```

`tools/build_cia.ps1` re-parses the built CIA and refuses if the title ID or the
`178MB` mode does not match the RSF.

## 2. Build

```powershell
# from the worktree root; needs staged/ and pilot_out_mult/ (build artifacts)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 `
    -Mode tesv `
    -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
    -MemProfile 136x8 `
    -TesvArmSteps 2000000000 `
    -Isolated cia136 -Jobs 8

powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1 `
    -Elf dist\Skyrim_N3DS_tesv_cia136\Skyrim_N3DS_tesv_cia136.elf `
    -Out E:\cia_menu\Skyrim_N3DS_tesv_136x8.cia `
    -WorkDir E:\cia_menu\work_136x8
```

Two generated tables are viable:

* `live_arm_check2\gen` — 32 AOT bodies + a `tesv_ir_table.c` function map. It
  compiles and links in ~2 minutes; every other function is decoded and
  executed from `tesv.irc` by the IR engine. This is the delivered CIA.
* `gen_full_check` — 90,620 AOT bodies (about 380 MB of generated C). It is the
  fast runtime route (no per-call IR decoding) but needs ~30+ minutes of
  `-O2` compile and was not completed in this timebox. Build it the same way,
  swapping `-TesvGen`; the object directory must be clean because the generated
  source names are reused.

With the 32-body table the IR container is **mandatory**: without
`sdmc:/Skyrim/ir/tesv.irc` there is almost no AOT code to run. `N3DS_IRC_ENABLE`
defaults to 1 and the path defaults to that file.

`-TesvArmSteps` is a compile define (`TESV_ARM_STEPS`), default 2,000,000. The
menu needs roughly 2e9 blocks; the host run stops at
`pilot basic-block budget exceeded` with 2e9 and 4e9 as well, so 2e9 is the
documented measurement value, not a limit that makes the menu finish.
`make` does not track `DEFINES`, so changing `-MemProfile` requires a clean
build. The three delivered profiles were produced by rebuilding only
`tesv_slice_main.o` in a copied object directory (no other object reads the
profile macros); `build.ps1` itself still performs a clean build by default.

### Delivered artifacts (2026-11-27, worktree `azahar-menu` @ 82d84dd)

| profile | CIA | bytes | sha256 |
|---|---|---:|---|
| **136x8** | `E:\cia_menu\Skyrim_N3DS_tesv_136x8.cia` | 1,552,832 | `15e06bb0e2352ece455388cf9ac11831499d79188ab652dcbe37eb95c1270525` |
| 132x12 | `E:\cia_menu\Skyrim_N3DS_tesv_132x12.cia` | 1,552,832 | `d656dc0f64845f1fd2065b02c3bab39b0a08168172acc37a52631c15cb22cc4b` |
| 128x16 | `E:\cia_menu\Skyrim_N3DS_tesv_128x16.cia` | 1,552,832 | `7215c9ffd8f1d95e8cddcbdb43b588e5caf1e0ffefafaa5c60c16512a0138b04` |

All three report title ID `000400000F0D5900` and exheader
`SystemModeExt=178MB CpuSpeed=804MHz EnableL2Cache=true`. The profile macros
baked into each object are recorded in `build_manifest.json`
(`defines=-DTESV_HEAP_MIB=… -DTESV_LINEAR_MIB=…`).

Whether both heaps are actually created is only observable on device: the first
`Skyrim_N3DS_mem.log` lines must say `heap_mib=… linear_mib=… have_newlib=1`
and the console prints `[mem] profile …`. No hardware run was available while
building this, so the table is arithmetic + build evidence, not a boot result.

The IR container is opened at `sdmc:/Skyrim/ir/tesv.irc` (override
`N3DS_IRC_PATH`). Use the MMX container
`E:\census_ir2\ir_container\tesv.irc` (90,900 functions, 90,027 decoded,
65,688,624 B; the container JSON still says 64,445,696 B from before the MMX
rebuild, so trust the file size).

## 3. Memory profile

The device envelope is `heap + linear + 34 MiB non-heap <= 178 MiB`.

| profile | newlib heap | linear | guest budget | region sum | verdict |
|---|---:|---:|---:|---:|---|
| 128x16 | 128 | 16 | 124 | 178 | guest peak 129.4 > 124 — expect guest OOM |
| 132x12 | 132 | 12 | 128 | 178 | 1.4 MiB short of the measured peak |
| **136x8** | **136** | **8** | **132** | **178** | 2.6 MiB guest headroom; tight linear |

Measured host guest peak is **129.4 MiB**, so only `136x8` leaves guest
headroom. The price is an 8 MiB linear heap. Arenas alone take ~4.35 MiB, so the
menu's render targets/textures can hit `linear_fail_n > 0`. That is visible in
`Skyrim_N3DS_mem.log` (`linear_fail_n`, `linear_fail_bytes`, `alloc_failures`).
If it binds, the next candidates are `132x12` (12 MiB linear, 128 MiB guest) and
`128x16` (16 MiB linear, 124 MiB guest), both of which need the guest peak
reduced first. The SD config described in section 5 can lower texture-cache
pressure without a rebuild, but it cannot enlarge the compiled 8 MiB linear
heap.

`guest_budget` is only a cap on guest pages: the host-side structures live in
the *same* newlib heap. At menu time the measured residents are roughly 4 MiB
IRC record cache + 3.5 MiB IRC index + 2.5 MiB BSA resident + 2 MiB stream
cache + 4 MiB decoded-IR cache ≈ 16 MiB. With 129.4 MiB of guest pages that is
~145 MiB, more than the 136 MiB heap, so `136x8` may still fail on `malloc`
before or during StartMenu even though the guest cap alone fits. The log tells
the two apart: `newlib_free`/`malloc` failure with low `guest_used` is a host
resident problem, while `guest_backing_failures` with `guest_used` near
`guest_limit` is the guest cap. The levers are the IRC cache
(`N3DS_IRC_CACHE_BYTES`), the BSA/stream limits and, as a last resort, dropping
`Skyrim - Meshes.bsa`/`- Textures.bsa` from the card.

Expected boot lines on device:

```
[mem] profile 136x8 MiB (newlib/linear): heap=136 MiB linear=8 MiB reserve=4 MiB guest_budget=132 MiB
App region: 178 MiB, free <N> MiB
```

If `App region` is not 178, the CIA was installed with the wrong memory mode;
reinstall it (a `.3dsx` cannot reach 178).

## 4. SD card layout

Root: `sdmc:/Skyrim/`.

```
sdmc:/Skyrim_N3DS.cfg                      optional (runtime env-style settings, section 5)
sdmc:/Skyrim/TESV.exe                       17,462,064 B   (required)
sdmc:/Skyrim/Skyrim.ini                          < 200 B   (required, see below)
sdmc:/Skyrim/ir/tesv.irc                 65,688,624 B      (required, MMX container)
sdmc:/Skyrim/Data/Skyrim.esm               249,371,467 B   (required: master)
sdmc:/Skyrim/Data/Update.esm                 1,518,686 B   (required)
sdmc:/Skyrim/Data/Update.bsa                17,428,297 B   (required: winning StartMenu.swf)
sdmc:/Skyrim/Data/Skyrim - Interface.bsa    34,708,832 B   (required: fonts, fontconfig, ControlMap)
sdmc:/Skyrim/Data/Skyrim - Shaders.bsa      25,690,987 B   (required: engine shader preload)
sdmc:/Skyrim/Data/Skyrim - Misc.bsa        177,490,631 B   (required: global data)
sdmc:/Skyrim/Data/Strings/Skyrim_English.STRINGS    833,753 B (required)
sdmc:/Skyrim/Data/Strings/Skyrim_English.ILSTRINGS 2,711,842 B (required)
sdmc:/Skyrim/Data/Strings/Skyrim_English.DLSTRINGS 2,442,981 B (required)
sdmc:/Skyrim/Data/Strings/Update_English.*          3 files   (required)
sdmc:/Skyrim/Data/Skyrim - Textures.bsa  1,388,866,502 B   (needed for the menu background/movie art)
sdmc:/Skyrim/Data/Skyrim - Meshes.bsa    1,520,899,887 B   (not needed for StartMenu; drop it first)
```

With an empty archive list the overlay falls back to
`Skyrim - Interface.bsa` then `Update.bsa`; the INI above mounts the four
archives explicitly instead. `Update.bsa` supplies the effective
`Interface/StartMenu.swf` (60,284 B, CRC32 `0x6910248f`); the copy inside
`Skyrim - Interface.bsa` is shadowed.

Do **not** copy `Data/Video/*`; the intro is skipped by the INI below. Do not
copy `Skyrim - Sounds.bsa` / `Voices*.bsa` for a menu run.

Minimal `sdmc:/Skyrim/Skyrim.ini`:

```ini
[General]
sLanguage=ENGLISH
sIntroSequence=
bPreloadIntroSequence=0

[Archive]
sResourceArchiveList=Skyrim - Misc.bsa, Skyrim - Shaders.bsa, Skyrim - Interface.bsa, Skyrim - Textures.bsa
```

`bPreloadIntroSequence=0` plus an empty `sIntroSequence` is the only supported
way to skip Bink; the ARM build returns NULL from the real decoder. The archive
list must name (at least) the files you copied: the engine mounts the configured
list, and the port's read-only overlay mounts the same list for loose-path
fallback. `Update.bsa` is mounted automatically from its fixed name and must
**not** be added to the list. An archive named in the list but absent from the
card is logged and skipped, so `Skyrim - Textures.bsa` can be removed from both
the card and this line to test whether menu art is the only texture consumer.
`Data/Skyrim.ini` is not needed when the root INI has the list. If the game
writes settings, `Documents/My Games/Skyrim/` and `LocalAppData/Skyrim/` are
optional.

## 5. Runtime config (`sdmc:/Skyrim_N3DS.cfg`)

A CIA has no shell, so the device launcher reads an allowlisted `KEY=VALUE`
file before graphics, memory setup, `n3ds_boot_prepare()` or `guest_init()`.
The default path is `sdmc:/Skyrim_N3DS.cfg`. An env-aware launcher may override
it with its first `main` argument, or with `TESV_CFG`; argument takes precedence.
`TESV_CFG` itself must be set by the launcher, not by this file.

Minimal/recommended file:

```cfg
# Skyrim N3DS device runtime settings
N3DS_TEX_GPU_LIMIT=6M
N3DS_TEX_GUEST_LIMIT=16M
N3DS_STREAM=0

# Optional overrides (uncomment to use):
# N3DS_IRC_PATH=sdmc:/Skyrim/ir/tesv.irc
# N3DS_IRC_CACHE_BYTES=4194304
# N3DS_MEM_EVERY=300
# TESV_MEMPROBE=1
```

Format rules: blank lines and lines starting with `#` or `;` are comments;
inline `#`/`;` comments after whitespace are accepted. Spaces around keys and
values are trimmed. Values may be bare, single-quoted or double-quoted. For a
duplicate supported key the last line wins. Unknown keys and malformed lines
are ignored (a count is logged). A missing file is not an error.

Values are applied with `setenv(KEY, VALUE, 0)`: a variable already supplied by
a launcher is never overwritten. If a key is absent from both the environment
and the file, the launcher supplies these defaults:

* `N3DS_TEX_GPU_LIMIT=6M`: the texture cache's GPU allocation budget stays
  within the PICA200's 6 MiB VRAM instead of competing without a cap with render
  targets and depth buffers.
* `N3DS_TEX_GUEST_LIMIT=16M`: a conservative guest-side texture-cache cap for
  the measured ~29 MiB host texture working set. It reduces unbounded guest/
  host growth under the 136x8 profile's tight heaps; it is a starting value,
  not a claim of a measured optimum.
* `N3DS_STREAM=0`: the menu route does not need the cooked cell stream. Keeping
  it off avoids opening `stream_index.bin` and allocating its staging/cache
  structures. Set it to `1` only with a valid index and data directory.

Every applied config/default key is written as `[cfg] KEY=VALUE` to stderr and
`sdmc:/Skyrim_N3DS_stderr.log`. Check those lines after launch to confirm the
file was read.

### Supported keys

| key | built-in/default | meaning |
|---|---|---|
| `N3DS_TEX_GPU_LIMIT` | `6M` | texture-cache GPU bytes; plain bytes or `K`/`M`/`G` suffix |
| `N3DS_TEX_GUEST_LIMIT` | `16M` | texture-cache guest bytes; same suffix format |
| `N3DS_STREAM` | `0` | `1` enables cooked cell streaming; `0` disables it |
| `N3DS_STREAM_INDEX` | `sdmc:/Skyrim/stream_index.bin` | stream index path when enabled |
| `N3DS_STREAM_DATA` | `sdmc:/Skyrim/Data` | archive data directory when enabled |
| `N3DS_IRC_PATH` | `sdmc:/Skyrim/ir/tesv.irc` | IR container path |
| `N3DS_IRC_ENABLE` | `1` | enable the IR container tier |
| `N3DS_IRC_REQUIRED` | `0` | `1` makes a missing/bad container a named guest fault |
| `N3DS_IRC_CACHE_BYTES` | `4194304` | decoded-record cache byte budget |
| `N3DS_IRC_CACHE_RECORDS` | `256` | decoded-record cache entry limit |
| `N3DS_MEM_EVERY` | `300` | periodic memory-snapshot interval in frames; `0` disables the periodic line |
| `TESV_MEMPROBE` | unset/off | **measurement only**: reports a fictional 512 MiB commit limit; never enable for a normal run |

The allowlist deliberately excludes `TESV_D3D9_CAPS_PROBE`,
`TESV_D3D9_SM3_GATE_PROBE`, `TESV_D3D9_SHADER_SKIP_PROBE`, and all other
probe/capability flags. This config cannot change `DevCaps`; it stays
`0x00082000` without the separate probe flag files below.

### Changing limits without rebuilding

Edit `sdmc:/Skyrim_N3DS.cfg` in a text editor and relaunch the CIA. The file is
read once at startup, so no CIA rebuild is needed. For example, if the memory
log shows texture-cache pressure, try guest limits `12M` or `8M` while keeping
the GPU limit at or below `6M`:

```cfg
N3DS_TEX_GPU_LIMIT=6M
N3DS_TEX_GUEST_LIMIT=12M
N3DS_STREAM=0
```

Lower texture limits can reduce cache growth, but they do not add linear heap.
If `linear_fail_n` keeps rising, non-evictable render targets or the compiled
8 MiB linear heap are still the limit; a different memory profile requires a
rebuild. Do not “fix” OOM by enabling `TESV_MEMPROBE`: its 512 MiB value is
intentionally false and only useful for a controlled measurement.

Probe/flag files (measurement only, never ship):

```
sdmc:/Skyrim/tesv_caps_probe.flag        # HW T&L + SM3 gate skip (measurement only)
sdmc:/Skyrim/tesv_depth_pair_probe.flag  # depth-pair probe (measurement only)
```

Both are opt-in and absent by default. `DevCaps` stays `0x00082000` without
them; do not create them for a real StartMenu run.

## 6. Install and run

1. Install the CIA with FBI/GM9 (or Azahar `File -> Install CIA`). The title
   must show as using the New 3DS / 178 MiB mode; a title installed without the
   RSF memory mode will report `App region: 96 MiB` or fail to allocate heaps.
2. Copy `sdmc:/Skyrim` as in section 4. FAT32, no junction points.
3. Create `sdmc:/Skyrim_N3DS.cfg` using the example in section 5. It is
   optional; without it the launcher still applies the `6M`/`16M`/stream-off
   defaults.
4. Launch from the home menu. The top screen shows the diagnostic console.
5. Confirm `[cfg] ...` lines in `sdmc:/Skyrim_N3DS_stderr.log`, then wait. The
   host needs ~228 s to the menu load stall; on hardware, allow **15–25
   minutes** before calling it stuck. Watch `sdmc:/Skyrim_N3DS_mem.log` (append,
   flushed every line) rather than the scrolling console.

## 7. Checkpoints and diagnosis

`Skyrim_N3DS_tesv.log` (guest stdout/stderr) must contain, in order:

```
[mem] profile 136x8 MiB ...
App region: 178 MiB, free ...
[menu] enter 00876f60                 StartMenu create
[menu] enter 00876b90                 ctor
[menu] enter 00a60c80 / 00a60c10      Menu::CreateMovie / path builder
[menu] enter 008779d0                 GFx message dispatch
CreateVertexShader / CreatePixelShader counts > 0
BeginScene=… Present=… Clear=…
DrawIndexedPrimitive=…                menu draw (the current wall)
```

Known-good host counts at this point: 874/874 engine VS `S_OK`, 368
declarations, 40/40 GFx PS `S_OK`, 2 `BeginScene`/`EndScene`/`Present`, then
`pilot basic-block budget exceeded` (2e9 and 4e9) with one thread asleep at
`0x6f40cf`. A hardware run that stops there has hit the same budget, not a
memory fault; a higher `TESV_ARM_STEPS` only makes it run longer.

Distinguishing failure modes:

| symptom | meaning | action |
|---|---|---|
| `[mem] FATAL: guest budget arithmetic failed` | profile macros inconsistent | rebuild; do not flash |
| `App region: 96 MiB` | CIA without 178 MB extheader | reinstall with the RSF |
| `alloc_failures` / `guest_backing_failures` / `guest ... failed` | guest pages or host residents exhausted (129.4 MiB peak > 132 budget) | stop; lower `N3DS_TEX_GUEST_LIMIT`/IRC cache in the SD config, reduce guest peak, or move to 132x12/128x16 after trimming |
| `linear_fail_n>0` / `linear_fail_bytes=…` | 8 MiB linear heap bound first | lower texture limits in the SD config; if non-evictable targets still bind, rebuild 132x12 and trim guest |
| no `[cfg] KEY=VALUE` lines | config missing or key already present in launcher env | check `sdmc:/Skyrim_N3DS.cfg`, path override and stderr log |
| `pilot basic-block budget exceeded` | 2e9 blocks consumed | expected ceiling; not OOM |
| `Guest fault at <addr>` in the log | real guest bug at that VA | report address + `irc_fn` + last call |

`Skyrim_N3DS_mem.log` snapshots (`boot`, `arenas`, `start`, `exit`,
`alloc_fail`) carry `heap_mib`, `linear_mib`, `guest_used/peak/limit`,
`newlib_free`, `linear_free/linear_min_free`, `linear_fail_n/bytes`,
`vram_*`, `irc_*`, `bsa_*`, `stream_*`, `tex_*` and `os_region_*`. The first
`boot` and `arenas` lines prove whether both heaps were created before any guest
code ran.

## 8. Limits

- StartMenu creation and `Present` are the measured frontier; a GFx menu draw
  and a visible menu are not claimed.
- The 2e9 step budget does not complete the menu on the host.
- `136x8` is chosen by arithmetic (guest needs 129.4 MiB), not by a hardware
  run. The linear heap risk is real and must be read from `linear_fail_*`.
- No `DevCaps`, pixel or frame claim. Screenshots are only real emulator or
  hardware output.
