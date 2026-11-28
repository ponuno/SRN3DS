# TESV CIA on hardware — install and first-run guide

Status: **build + file layer**. This document describes a local diagnostic CIA
that boots the real `TESV.exe` under the lifted x86 runtime on a New 3DS. It
does not claim a playable frame. The CIA contains locally generated code derived
from the owner's game files; do not distribute it.

Current artifact: **96x24** profile (newlib 96 MiB / linear 24 MiB) with both
memory fixes enabled — the global 6 MiB VRAM budget and the pageable read-only
`.text` window — plus the raised main-thread stack and the Bink empty-clip gate
that lets the intro state finish so `StartMenu` is created. The guest working set
on the host is **44.4 MiB** (was 57.2 MiB before paging), so this profile fits
the guest budget with large headroom and still leaves a 24 MiB linear heap.

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
# from the worktree root; staged/ and pilot_out_mult/ must exist (they are
# shared, git-ignored inputs: tools\prepare_worktree.ps1 junctions them in)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 `
    -Mode tesv `
    -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
    -MemProfile 96x24 `
    -TesvArmSteps 2000000000 `
    -Opt -O2 `
    -Isolated 96x24 -Jobs 8

powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1 `
    -Elf dist\Skyrim_N3DS_tesv_96x24\Skyrim_N3DS_tesv_96x24.elf `
    -Out E:\cia_menu\Skyrim_N3DS_tesv_96x24_memgate.cia `
    -WorkDir E:\cia_menu\work_96x24_memgate
```

`96x24` is the device ceiling: libctru's application heap area is 96 MiB. Do
not use `-MemProfile 120x24` for a flashable build.

`pilot_out_mult\D3DXMatrixMultiply\gen` is picked up automatically by the
`TESV_SLICE` OFILES block when present (the lifted D3DX bodies must be
dispatchable on device); `staged/` provides `lift.py`/`fpu.c` and the Win32
shims. Both are linked from the worktree via junctions, not copies.

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
build. `build.ps1` performs a clean build by default; `-NoClean` is only safe
when the profile macros are unchanged (it is what was used for the re-link after
the two `n3ds_env_cfg` allowlist keys were added).

### Delivered artifact (2026-11-28, worktree `cia-final` @ `180d102`)

| profile | CIA | bytes | sha256 |
|---|---|---:|---|
| **96x24 + stack + Bink empty clip + startup mem compat** | `E:\cia_menu\Skyrim_N3DS_tesv_96x24_memgate.cia` | 1,580,992 | `d4ceddc4fc6e538a11f75f823855677ef2fa94e277940d0ec6de1ae09e1f683e` |

ELF: `dist\Skyrim_N3DS_tesv_96x24\Skyrim_N3DS_tesv_96x24.elf`
(12,999,172 B, sha256 `016198f16907d63ae57872daa7433baa7ef88d468db5ae6a6a91109d4408d984`).
`build_manifest.json` records `mem_profile=96x24`,
`defines=-DTESV_HEAP_MIB=96 -DTESV_LINEAR_MIB=24`, and the generator hash of
`live_arm_check2\gen`. The CIA reports title ID `000400000F0D5900` and
`SystemModeExt=178MB CpuSpeed=804MHz EnableL2Cache=true` (checked by
`build_cia.ps1`; see `work_96x24_memgate\cia_info.txt`).

In this ELF: `__ctru_heap_size = 0x06000000` (96 MiB),
`__ctru_linear_heap_size = 0x01800000` (24 MiB), `__stacksize__ = 0x00020000`
(128 KiB); the packaged exheader keeps `StackSize = 0x00040000` (256 KiB).

> **120x24 does not boot on hardware.** `…_120x24_menu.cia` and
> `…_120x24_stack.cia` fail before `main` — see *Hardware heap limit* below.
> Install `Skyrim_N3DS_tesv_96x24_memgate.cia`.

Why this replaces `…_96x24_menu.cia`: that build loaded but the guest exited at
startup with `exit code=0 from=00f5894a` — Skyrim's PC minimum-configuration
check demands a 512 MiB commit limit. Since `180d102`, `s_GlobalMemoryStatusEx`
answers 512 MiB **only** for the call from `0x00a48a5d` (return `0x00a48a63`);
every later query reports the honest budget. It is a compatibility shim for a
PC check, not a memory promise: guest allocations still fail honestly at the
budget. Host run: `[mem] startup minimum-config check: 512 MiB commit reported`,
then `[menustate] singleton=41f8eaf0` over ~108k frames.

Why the Bink gate matters: since `b03c33c`, `_BinkOpen@8` in
`source/win32_shims.c` returns a valid **empty clip** when the platform has no
Bink decoder (3DS), when the name/path is empty, or when the file is absent —
0x0 size, 1 frame, no-op copies — instead of NULL. Before that, the intro state
never finished on device and `StartMenu` was never created. The trace line
`[bink] _BinkOpen@8 …: no Bink decoder on this platform; empty clip …` is the
evidence that the gate fell through to the menu. Host check with
`TESV_BINK_NO_DECODER=1`: singleton created, `[menustate]` proceeds, suite
219/0.

Previous artifacts:

| artifact | bytes | sha256 | status |
|---|---:|---|---|
| `Skyrim_N3DS_tesv_96x24_memgate.cia` | 1,580,992 | `d4ceddc4…e1f683e` | **current** |
| `Skyrim_N3DS_tesv_96x24_menu.cia` | 1,580,480 | `a9ede38e…bf0b9501` | previous: **exits at startup memory check** (`exit code=0 from=00f5894a`) |
| `Skyrim_N3DS_tesv_120x24_menu.cia` | 1,580,480 | `d2c6aa34…9036031c` | **crashes on hardware**: 120 MiB heap, svcBreak in `__system_allocateHeaps` |
| `Skyrim_N3DS_tesv_120x24_stack.cia` | 1,577,920 | `47dbfc60c1d420f24f1846ff3f0a361cb051c602cb2fc97b87a03bd9d4e60ea4` | **crashes on hardware**: same heap limit; also pre-Bink-fix |
| `Skyrim_N3DS_tesv_120x24.cia` | 1,566,656 | `6cb076b18f61700975a5ef9f7086ad7c5c965b18ee91fc1ffb0e0c53f4e2e0bf` | **invalid**: pre-`b3915c5`, 32 KiB stack |

### Hardware heap limit: the application heap is 96 MiB, not 120

libctru's `OS_HEAP_AREA` is `0x08000000..0x0E000000`, i.e. **96 MiB**. A CIA with
`__ctru_heap_size` above that makes `svcControlMemory` fail inside
`__system_allocateHeaps`, which calls `svcBreak` — before `main`, so nothing is
logged and no guest code runs. On a real New 3DS this is what a 120x24 build
looks like:

```
Exception type: prefetch abort (svcBreak), Debug event
PC 0027B834  LR 0027B344  SP 0FFFFFC0  R3=07800000 (120 MiB)
```

`LR` symbolizes to `__system_allocateHeaps` (`libctru/source/system/allocateHeaps.c:66`)
called from `__libctru_init`/`initSystem`. It is a build/profile problem, not a
guest fault, a VRAM refusal or an OOM in the slice.

The host has no such area, so 120x24 always passed there. The device profile is
now **96x24**: heap 96 MiB (the maximum), linear 24 MiB, reserve 4 MiB, guest
budget 92 MiB; 96 + 24 + 34 = 154 of 178 MiB, and 24 MiB stay unused. The
guest working set measured on the host is 44.4 MiB, so 92 MiB still leaves more
than 2x headroom.

`tools/build.ps1` gained the `96x24` profile and
`source/tesv_slice_main.c` now fails the build with `#error` when
`__3DS__ && TESV_HEAP_MIB > 96`, so a 120x24 ARM build cannot be produced by
accident. `-MemProfile 120x24` is host-suite only.

### Stack (part of the build since `b3915c5`)

* `source/n3ds_boot.c` defines a strong `__stacksize__ = 128 KiB` for
  `__3DS__`. In the 96x24 ELF the symbol is at `0x003bb000` (`.data`, 4 bytes)
  and its value is **`0x00020000`** (128 KiB), not the 32 KiB `0x8000` default.
* `tools/cia/template.rsf` sets `StackSize: 0x40000`, so the packaged CIA's
  exheader carries a **256 KiB** main-thread stack (`exheader+0x1C =
  0x00040000`, verified by reading the built CIA). On a console title the
  exheader value is what the loader honours; the ELF symbol governs the
  `.3dsx` path. Both are now large enough for the menu call chain.

Build-time evidence in this ELF (checked with `arm-none-eabi-nm` and string
search): paging (`pe_image_load`, `pe_image_touch_report`, `guest_range_mapped`,
`guest_slab_set_window`), VRAM accounting (`d9_target_stats`,
`d9_citro_vram_stats`, literal `[d3d9-vram] budget …`), the device config read
(`n3ds_env_cfg_load`/`n3ds_env_cfg_parse_stream`), the strings
`N3DS_TEXT_WINDOW_MB`, `N3DS_VRAM_LIMIT`, `N3DS_TARGET_VRAM_LIMIT`, and the Bink
gate strings `no Bink decoder on this platform` / `empty clip`.

Previous profiles (2026-11-27, worktree `azahar-menu` @ `82d84dd`, **no paging,
no VRAM budget** — kept only for comparison, all three are superseded):

| profile | CIA | bytes | sha256 |
|---|---|---:|---|
| 136x8 | `E:\cia_menu\Skyrim_N3DS_tesv_136x8.cia` | 1,552,832 | `15e06bb0e2352ece455388cf9ac11831499d79188ab652dcbe37eb95c1270525` |
| 132x12 | `E:\cia_menu\Skyrim_N3DS_tesv_132x12.cia` | 1,552,832 | `d656dc0f64845f1fd2065b02c3bab39b0a08168172acc37a52631c15cb22cc4b` |
| 128x16 | `E:\cia_menu\Skyrim_N3DS_tesv_128x16.cia` | 1,552,832 | `7215c9ffd8f1d95e8cddcbdb43b588e5caf1e0ffefafaa5c60c16512a0138b04` |

Whether both heaps are actually created is only observable on device: the first
`Skyrim_N3DS_mem.log` lines must say `heap_mib=96 linear_mib=24 have_newlib=1`
and the console prints `[mem] profile 96x24 …`. A heap that is too large does
not even reach this point (see *Hardware heap limit* in section 2).

The IR container is opened at `sdmc:/Skyrim/ir/tesv.irc` (override
`N3DS_IRC_PATH`). Use the MMX container
`E:\census_ir2\ir_container\tesv.irc` (90,900 functions, 90,027 decoded,
65,688,624 B; the container JSON still says 64,445,696 B from before the MMX
rebuild, so trust the file size).

## 3. Memory profile

The device envelope is `heap + linear + 34 MiB non-heap <= 178 MiB`. VRAM is a
separate pool: the New 3DS has 10 MiB of physical VRAM but only **6 MiB are
usable** by an application, and the Citro3D backend keeps 256 KiB of slack, so
the enforced cap is 5.75 MiB (section 5).

| profile | newlib heap | linear | guest budget | region sum | verdict |
|---|---:|---:|---:|---:|---|
| 96x8 | 96 | 8 | 92 | 138 | `.3dsx`-era region: guest OOM at 78 MiB used |
| 136x8 | 136 | 8 | 132 | 178 | superseded: 8 MiB linear refuses menu targets |
| 120x24 | 120 | 24 | 116 | 178 | **host only**: exceeds the 96 MiB heap area, `svcBreak` before `main` on hardware |
| **96x24** | **96** | **24** | **92** | **154** | **current device profile**: 44.4 MiB guest used, 24 MiB linear, 24 MiB region left over |

Before paging, the measured host guest peak was 129.4 MiB and only `136x8` fit,
at the price of an 8 MiB linear heap (arenas alone take ~4.35 MiB, so
`linear_fail_n` rose). Paging the read-only image text in a bounded window cut
the guest working set to **44.4 MiB** (3.18 MiB `.text` + 0.87 MiB `.rdata` hot
set at the main menu, 5 MiB window). That is what makes a 24 MiB linear heap
affordable: it holds the render targets, and the 92 MiB guest budget still is
2x the measured need.

`guest_budget` is only a cap on guest pages: the host-side structures live in
the *same* newlib heap. At menu time the measured residents are roughly
1.41 MiB IRC index (compact, was 3.5) + <=0.17 MiB IRC record cache (2 MiB cap,
256 records) + 2.5 MiB BSA resident + 2 MiB stream cache + 4.75 MiB decoded-IR
cache (4 MiB arena + slot table) = about 10.8 MiB. With the measured 44.4 MiB of
guest pages that is ~55 MiB of 120 MiB. The log tells the two failure kinds
apart: `newlib_free`/`malloc` failure with low `guest_used` is a host-resident
problem, while `guest_backing_failures` with `guest_used` near `guest_limit` is
the guest cap. The levers are `N3DS_IRC_CACHE_BYTES`, the IR decode cap
(`N3DS_IR_CACHE_MB`) and the BSA/stream limits.

The paging window itself is a knob: `N3DS_TEXT_WINDOW_MB` (default `5`) sizes
the evictable read-only window in MiB; `0` disables paging and maps the whole
image. Lowering it shrinks `guest_used` further at the cost of more page faults
per frame, so it is the first lever when the guest budget is the binding limit.

Expected boot lines on device:

```
[mem] profile 96x24 MiB (newlib/linear): heap=96 MiB linear=24 MiB reserve=4 MiB guest_budget=92 MiB
App region: 178 MiB, free <N> MiB
```

If `App region` is not 178, the CIA was installed with the wrong memory mode;
reinstall it (a `.3dsx` cannot reach 178; it is limited to a 92 MiB region, where
the guest already reached 78 MiB used before OOM).

## 4. SD card layout

Root: `sdmc:/Skyrim/`.

```
sdmc:/Skyrim_N3DS.cfg                      recommended (runtime settings, section 5)
sdmc:/Skyrim/tesv_d3d9_budget.txt                      2 B   (texture-cache MiB, see below)
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

`sdmc:/Skyrim/tesv_d3d9_budget.txt` is a one-line file holding the texture-cache
budget in MiB; the launcher reads it before graphics init and exports it as
`TESV_D3D9_BUDGET_MB`. The 4 MiB compiled default is too small for the real menu
path, so write:

```
32
```

The first character must be `1`–`9`, otherwise the line is ignored and the
compiled default stays. No trailing spaces or units.

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

Recommended file for the 96x24 CIA:

```cfg
# Skyrim N3DS device runtime settings (96x24 profile)
N3DS_TEX_GPU_LIMIT=6M
N3DS_TEX_GUEST_LIMIT=16M
N3DS_VRAM_LIMIT=6M
N3DS_TARGET_VRAM_LIMIT=4352K
N3DS_TEXT_WINDOW_MB=5
N3DS_STREAM=0

# Optional overrides (uncomment to use):
# N3DS_IRC_PATH=sdmc:/Skyrim/ir/tesv.irc
# N3DS_IRC_CACHE_BYTES=4194304
# N3DS_MEM_EVERY=300
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
* `N3DS_TEX_GUEST_LIMIT=16M`: guest-side texture-cache cap for the measured
  ~29 MiB host texture working set; a starting value, not a measured optimum.
* `N3DS_VRAM_LIMIT=6M`: the global VRAM budget enforced by the Citro3D backend
  (6 MiB usable minus 256 KiB slack = 5.75 MiB). Every on-VRAM allocation —
  LCD render target, 2D colour/depth, SR storage, cube-face depth views — is
  charged here, and a refusal is logged by name.
* `N3DS_TARGET_VRAM_LIMIT=4352K`: the render-target share of that budget (4.25
  MiB), enough for the four 512x512 menu targets plus the retained back
  buffer/depth, the LCD target and the transient targets.
* `N3DS_TEXT_WINDOW_MB=5`: evictable window for the read-only part of
  `TESV.exe` (`.text` + `.rdata`), sized for the 3.18 + 0.87 MiB hot set. `0`
  maps the whole image and brings back the 129.4 MiB-era guest peak.
* `N3DS_STREAM=0`: the menu route does not need the cooked cell stream. Keeping
  it off avoids opening `stream_index.bin` and allocating its staging/cache
  structures. Set it to `1` only with a valid index and data directory.

The first four keys are applied before `d9_citro_open()`, the paging window
before `guest_init()` maps the image, so no key needs a rebuild to take effect.

### Startup minimum-config check (compat shim)

Skyrim's PC minimum-configuration check (`0x00f5894a` exits the process with
`exit code=0` when it fails) requires a commit limit of at least 512 MiB. This
port has no page file, so since `180d102` `s_GlobalMemoryStatusEx` answers
**512 MiB only for the call from `0x00a48a5d`** (return address `0x00a48a63`)
and leaves every later query honest. The one line it logs is:

```
[mem] startup minimum-config check: 512 MiB commit reported (compat; the real guest budget is still enforced)
```

This is a compatibility answer for a PC check, not a memory promise: guest
allocations still fail honestly at the 92 MiB budget (`guest_backing_failures`,
`newlib_free`). It is compiled in, so no config key is needed and
`TESV_MEMPROBE` stays what it always was — a **measurement-only** switch that
lies to every query. Do not enable it for a normal run.

Every applied config/default key is written as `[cfg] KEY=VALUE` to stderr and
`sdmc:/Skyrim_N3DS_stderr.log`. Check those lines after launch to confirm the
file was read; `[cfg] … ignored` means a key is not in the allowlist below.

### Supported keys

| key | built-in/default | meaning |
|---|---|---|
| `N3DS_TEX_GPU_LIMIT` | `6M` | texture-cache GPU bytes; plain bytes or `K`/`M`/`G` suffix |
| `N3DS_TEX_GUEST_LIMIT` | `16M` | texture-cache guest bytes; same suffix format |
| `N3DS_VRAM_LIMIT` | `6M` | global Citro3D VRAM budget; `0` is not valid and falls back to `6M` |
| `N3DS_TARGET_VRAM_LIMIT` | `4352K` | render-target slice of the VRAM budget; `0` disables the target cap |
| `N3DS_TEXT_WINDOW_MB` | `5` | pageable read-only image window in MiB; `0` = whole image resident |
| `N3DS_STREAM` | `0` | `1` enables cooked cell streaming; `0` disables it |
| `N3DS_STREAM_INDEX` | `sdmc:/Skyrim/stream_index.bin` | stream index path when enabled |
| `N3DS_STREAM_DATA` | `sdmc:/Skyrim/Data` | archive data directory when enabled |
| `N3DS_IRC_PATH` | `sdmc:/Skyrim/ir/tesv.irc` | IR container path |
| `N3DS_IRC_ENABLE` | `1` | enable the IR container tier |
| `N3DS_IRC_REQUIRED` | `0` | `1` makes a missing/bad container a named guest fault |
| `N3DS_IRC_CACHE_BYTES` | `4194304` | decoded-record cache byte budget |
| `N3DS_IRC_CACHE_RECORDS` | `256` | decoded-record cache entry limit |
| `N3DS_MEM_EVERY` | `300` | periodic memory-snapshot interval in frames; `0` disables the periodic line |
| `TESV_MEMPROBE` | unset/off | **measurement only**: reports a fictional 512 MiB commit limit to *every* query; never enable for a normal run (the startup check no longer needs it) |

The allowlist deliberately excludes `TESV_D3D9_CAPS_PROBE`,
`TESV_D3D9_SM3_GATE_PROBE`, `TESV_D3D9_SHADER_SKIP_PROBE`, and all other
probe/capability flags. This config cannot change `DevCaps`; it stays
`0x00082000` without the separate probe flag files below.

`N3DS_TEXT_WINDOW_MB` and `N3DS_TARGET_VRAM_LIMIT` are new in this build. Both
were environment-only before, which is unreachable on a CIA; they are now in the
allowlist (`source/n3ds_env_cfg.c`) and covered by `tests/device_cfg_smoke.c`.

### Changing limits without rebuilding

Edit `sdmc:/Skyrim_N3DS.cfg` in a text editor and relaunch the CIA. The file is
read once at startup, so no CIA rebuild is needed. Lower texture limits can
reduce cache growth, but they do not add linear heap; the compiled split
(`96x24`) only changes with `-MemProfile` and a rebuild.

```cfg
N3DS_TEX_GPU_LIMIT=6M
N3DS_TEX_GUEST_LIMIT=12M
N3DS_VRAM_LIMIT=6M
N3DS_TARGET_VRAM_LIMIT=4352K
N3DS_TEXT_WINDOW_MB=5
N3DS_STREAM=0
```

**If the run ends in an out-of-memory line**, read `Skyrim_N3DS_mem.log` first
and act on the counter that actually moved:

| log evidence | what ran out | first lever |
|---|---|---|
| `newlib_free` near 0, low `guest_used` | host residents in the 120 MiB newlib heap | `N3DS_IRC_CACHE_BYTES=2097152`, then `N3DS_TEX_GUEST_LIMIT=8M`; drop `Skyrim - Textures.bsa` from card + INI as a last resort |
| `guest_backing_failures` > 0 with `guest_used` near `guest_limit` (116 MiB) | guest page cap | `N3DS_TEXT_WINDOW_MB=4` (then `3`) shrinks the resident read-only image; verify `guest_used` fell in the next snapshot |
| `linear_fail_n` / `linear_fail_bytes` rising | 24 MiB linear heap (arenas, targets, textures) | lower `N3DS_TARGET_VRAM_LIMIT` to `3840K`, then `N3DS_TEX_GPU_LIMIT=4M`; check whether named `[d3d9-vram] … refused` lines appear |
| `[d3d9-vram] … refused: live … > … usable` | the 5.75 MiB device VRAM cap | lower `N3DS_TARGET_VRAM_LIMIT` (targets dominate); `N3DS_VRAM_LIMIT` must stay `6M` |
| `vram_fail_n` > 0 but no `linear_fail_n` | citro3d's own pool probe, not the app budget | usually harmless; compare `vram_live` with the target cap |

Any single change is one relaunch, so change one key at a time. Do not “fix” OOM
by enabling `TESV_MEMPROBE`: its 512 MiB value is intentionally false and only
useful for a controlled measurement.

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
2. Copy `sdmc:/Skyrim` as in section 4 plus `sdmc:/Skyrim_N3DS.cfg` and
   `sdmc:/Skyrim/tesv_d3d9_budget.txt` (contents `32`). FAT32, no junction
   points.
3. Without the cfg the launcher still applies `6M`/`16M`/`6M`/`4352K`/`5`/
   stream-off defaults, but the budget file is needed for the menu path.
   The startup minimum-config check needs no key: it is answered by the
   compiled compat shim (section 5).
4. Launch from the home menu. The top screen shows the diagnostic console.
5. Confirm `[cfg] …` lines in `sdmc:/Skyrim_N3DS_stderr.log`, then wait. The
   host needs ~228 s to the menu load stall; on hardware, allow **15–25
   minutes** before calling it stuck. Watch `sdmc:/Skyrim_N3DS_mem.log` (append,
   flushed every line) rather than the scrolling console.

## 7. Checkpoints and diagnosis

`Skyrim_N3DS_tesv.log` (guest stdout/stderr) must contain, in order:

```
[mem] profile 96x24 MiB (newlib/linear): heap=96 MiB linear=24 MiB reserve=4 MiB guest_budget=92 MiB
App region: 178 MiB, free ...
[d3d9-vram] budget 5888 KiB (N3DS_VRAM_LIMIT, 256 KiB slack)
[probe] TESV_D3D9_BUDGET_MB=32 from flag file
mem tag=init ... guest_budget=96468992 guest_used=... linear_mib=24
[mem] startup minimum-config check: 512 MiB commit reported (compat; the real guest budget is still enforced)
[bink] _BinkOpen@8 …: no Bink decoder on this platform; empty clip ...
[menu] enter 00876f60                 StartMenu create
[menu] enter 00876b90                 ctor
[menu] enter 00a60c80 / 00a60c10      Menu::CreateMovie / path builder
[menu] enter 008779d0                 GFx message dispatch
CreateVertexShader / CreatePixelShader counts > 0
BeginScene=… Present=… Clear=…
DrawIndexedPrimitive=…                menu draw (the current wall)
```

The checkpoint lines before `[menu] enter` are the fix checkpoints:

* `[mem] profile 96x24 … guest_budget=92` proves the profile macros reached
  `tesv_slice_main.o` and the heap arithmetic held.
* `[d3d9-vram] budget 5888 KiB` proves the new VRAM accounting is linked and
  the global cap is active (6 MiB − 256 KiB slack).
* `[probe] TESV_D3D9_BUDGET_MB=32 from flag file` proves
  `sdmc:/Skyrim/tesv_d3d9_budget.txt` was read.
* `mem tag=init … guest_used=…` (the first `init` snapshot in
  `Skyrim_N3DS_mem.log`) proves paging: `guest_used` must be well below the
  pre-paging 57.2 MiB, i.e. in the ~44 MiB range as the image is faulted in.
* `[bink] … no Bink decoder on this platform; empty clip …` proves the intro
  gate fell through: `_BinkOpen@8` returned a valid 0x0 clip instead of NULL, so
  the intro state finishes and `StartMenu` is created. A run that stops before
  `[menu] enter 00876f60` with no such line is using a pre-`b03c33c` CIA.
* `[mem] startup minimum-config check: 512 MiB commit reported (compat…)`
  proves the targeted `GlobalMemoryStatusEx` answer ran for the check at
  `0x00a48a5d`. Its absence, or `exit code=0 from=00f5894a` in the log, means a
  CIA built before `180d102` (or one that answered honestly).

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
| `exit code=0 from=00f5894a` right after startup, no guest work | Skyrim PC minimum-config check (needs 512 MiB commit limit) answered honestly | install `Skyrim_N3DS_tesv_96x24_memgate.cia` (`180d102`); do not enable `TESV_MEMPROBE` |
| Azahar/hardware `svcBreak` at `__system_allocateHeaps` before `main` (PC in libctru, `R3` = heap bytes, no guest log) | heap larger than the 96 MiB area, i.e. a 120x24 build | install `Skyrim_N3DS_tesv_96x24_memgate.cia`; 120x24 is host-suite only |
| Azahar/hardware fault with `SP` just below the stack top (e.g. `push` at `0x07FFFFA4`), no `[mem]` OOM line | 32 KiB main-thread stack, i.e. a pre-`b3915c5` build | rebuild from current main; check `__stacksize__` = `0x20000` in the ELF |
| `newlib_free` near 0 / `malloc` failed, low `guest_used` | host residents in the 96 MiB newlib heap | lower `N3DS_IRC_CACHE_BYTES` (2 MiB), then `N3DS_TEX_GUEST_LIMIT=8M` |
| `guest_backing_failures` > 0, `guest_used` ≈ 92 MiB | guest page cap | lower `N3DS_TEXT_WINDOW_MB` (4, then 3); check `guest_used` in the next snapshot |
| `linear_fail_n>0` / `linear_fail_bytes=…` | 24 MiB linear heap bound | lower `N3DS_TARGET_VRAM_LIMIT`, then `N3DS_TEX_GPU_LIMIT` |
| `[d3d9-vram] … refused` / `vram_fail_n` | 5.75 MiB device VRAM cap | lower `N3DS_TARGET_VRAM_LIMIT`; keep `N3DS_VRAM_LIMIT=6M` |
| `[cfg] … ignored` for a key you set | key not in the allowlist (typo, or probe flag) | use a key from the table in section 5 |
| no `[cfg] KEY=VALUE` lines | config missing or key already present in launcher env | check `sdmc:/Skyrim_N3DS.cfg`, path override and stderr log |
| `pilot basic-block budget exceeded` | 2e9 blocks consumed | expected ceiling; not OOM |
| `Guest fault at <addr>` in the log | real guest bug at that VA | report address + `irc_fn` + last call |

`Skyrim_N3DS_mem.log` snapshots (`boot`, `arenas`, `start`, `init`, `exit`,
`alloc_fail`) carry `heap_mib`, `linear_mib`, `guest_used/peak/limit`,
`newlib_free`, `linear_free/linear_min_free`, `linear_fail_n/bytes`,
`vram_*`, `irc_*`, `bsa_*`, `stream_*`, `tex_*` and `os_region_*`. The first
`boot` and `arenas` lines prove whether both heaps were created before any guest
code ran; `init` is the first line that can show the paged `guest_used`.

## 8. Limits

- StartMenu creation and `Present` are the measured frontier; a GFx menu draw
  and a visible menu are not claimed.
- The 2e9 step budget does not complete the menu on the host.
- `96x24` is chosen from the post-paging 44.4 MiB host guest working set and the
  96 MiB device heap ceiling, not from a hardware run. Both memory fixes are
  verified as linked and active, not as pixels: read `guest_used` and
  `linear_fail_*` on the device.
- The application heap cannot exceed 96 MiB on hardware regardless of the
  178 MiB region: libctru's `OS_HEAP_AREA` ends at `0x0E000000`. 120x24 builds
  exist for the host suite only.
- The menu call chain needs the raised main-thread stack. The CIA gets 256 KiB
  from the exheader (`StackSize: 0x40000`); a `.3dsx` gets 128 KiB from the
  `__stacksize__` symbol. A build without either overflows at the first deep
  menu call and looks like a guest fault near the stack top, not OOM.
- No `DevCaps`, pixel or frame claim. Screenshots are only real emulator or
  hardware output.
