# DEBUGGING — build, run, and first-cause workflow

## 1. One-command loops

```powershell
# Native tests (clang + MSVC headers), includes the real-DLL pilot when a
# pilot_out*/…/pilot_manifest.json is present.
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1

# If the system disk is low on space, keep objects/logs/temp on another drive:
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -OutDir E:\Skyrim_N3DS_build\host

# With AddressSanitizer (clang runtime directory is added automatically)
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 -Sanitize

# One test only
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 -Only com

# Pick a specific generated pilot explicitly
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only pilot_real -PilotGen pilot_out_mult\D3DXMatrixMultiply\gen

# ARM builds (clean, archived with hashes under dist/)
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode default
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode graphics
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode com
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode boot
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode lift
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode pilot `
  -PilotGen pilot_out_mult\D3DXMatrixMultiply\gen

# Azahar run with bounded timeout, screenshot and log archive
powershell -ExecutionPolicy Bypass -File tools/run_azahar.ps1 `
  -ThreeDsx dist\Skyrim_N3DS_pilot\Skyrim_N3DS_pilot.3dsx `
  -Name Skyrim_N3DS_pilot_v012 -TimeoutSec 20
```

`run_azahar.ps1` copies the `.3dsx` into the virtual SD
(`%APPDATA%\Azahar\sdmc\3ds\<name>\`), starts a **separate** Azahar instance, captures a window screenshot, copies `sdmc:/Skyrim_N3DS*.log` and the emulator
log to `build_azahar/<stamp>-<name>/`, then closes only the instance it
started. It never touches other Azahar windows.

### Test-harness notes

`run_host_tests.ps1` imports the MSVC environment **once** at start
(`cmd /c call vcvars64.bat && set`) instead of calling `vcvars64.bat` for every
test. Two concurrent runs of the old form deadlocked inside vcvars; that is why
host runs are started one at a time.

TESV host slices compile translation units in parallel (`HOST_CLANG_JOBS`,
default 6, safe up to 8 on the 16-thread/31 GiB workstation) and link once.
Other tests keep the single-command clang path. Parallel output is deterministic;
measure wall time for throughput changes rather than assuming single-core is the
CPU limit.

## 2. Azahar environment facts

- Installed: `C:\Program Files\Azahar\azahar.exe`, version 2126.2.
- `azahar.exe --help` does not print CLI usage; it opens the GUI and exits 1.
  The verified way to boot a file is a positional path argument.
- Profile in use: `System_IsNew3ds=true`, LLE applets on, GDB stub off,
  virtual SD on. `APT_CheckNew3DS` returns `New 3DS: yes` in the diagnostic app.
- **Emulator memory is not device memory.** The diagnostic build reported
  `App memory region: 96 MiB` and `App region free: 4068 MiB`. The last value is
  emulator host RAM; use only `osGetMemRegionSize` and the 48 MiB self-imposed
  guest cap for planning.
- Typical harmless emulator messages for `.3dsx`: `Failed to find title id for
  ROM`, `Could not read program id … 3dsx`, `ClCertA file missing`.
  `ConfigureNew3DSCPU` is reported as unimplemented by the emulator.

## 3. Where the logs are

| Source | Location |
|---|---|
| Diagnostic app | `sdmc:/Skyrim_N3DS.log` → `%APPDATA%\Azahar\sdmc\Skyrim_N3DS.log` |
| Pilot app | `sdmc:/Skyrim_N3DS_pilot.log` |
| Azahar | `%APPDATA%\Azahar\log\azahar_log.txt` |
| Archived evidence | `build_azahar/<stamp>-<name>/` (screen.png, run.log, logs) |
| Host tests | `build_host/logs/<stamp>_<test>.log` |
| ARM builds | `dist/<target>/build.log`, `build_manifest.json` |

### Luma3DS crash dumps

Hardware crashes are saved as `sdmc:/luma/dumps/arm11/crash_dump_*.dmp`.
Decode with:

```powershell
.venv\Scripts\python.exe tools/parse_luma_dump.py <dump> `
  --elf dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.elf
```

The parser reads Luma's `ExceptionDumpHeader` (0x28 bytes), the register block
(R0–R12, SP, LR, PC, CPSR, FPEXC and ARM11 extras), code bytes, stack bytes and
the process name/title id. Type 2 with a trailing `svc 0x3c` means a `svcBreak`
panic, not a data abort. `addr2line` maps PC/LR; remember ARM vs Thumb from
CPSR bit 5. Observed incident: PC → libctru `svcBreak`, LR →
`__system_allocateHeaps`, caused by the default heap split in 178 MiB mode.

## 4. First-cause rule

Record, in order: last successful stage → first failing guest address →
whether the address is a lifted function, an import thunk, or unmapped memory →
the ARM fault PC/LR if the runtime trapped. Do not chase follow-on errors.

Current diagnostic classes are surfaced by the runtime as text:
`UNMAPPED guest memory`, `unsupported D3D9 COM method`, `pilot basic-block
budget exceeded`, load/identity errors with module SHA/FNV, and the upstream
lifter's `pilot_rejected.json`/`lift.log`. A structured ring-buffer logger with
levels/build IDs is planned; until then, keep logs per run and never overwrite a
previous failure archive.

For renderer startup, set `TESV_D3D9_TRACE=1`. It logs factory/COM slot calls,
key arguments/results and named unsupported reasons while normal successful runs
stay quiet. Use it to distinguish "renderer not reached yet" from a specific
D3D9 method failure; the null backend trace is diagnostic, not proof of PICA
rendering.

### Native backtrace for a silent guest exit

`ExitProcess`/`ExitCodeThread` failures are easiest to attribute with the host
`lldb`, because every lifted function is a native C function named `f_XXXXXX`.

```powershell
$d = (Get-ChildItem E:\Skyrim_N3DS_build\host -Directory -Filter 'run_tesv_slice_*' |
      Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
& 'C:\Program Files\LLVM\bin\lldb.exe' --batch `
  -o "settings set target.run-args E:\skyr\TESV.exe . 200000" `
  -o "breakpoint set -n s_ExitProcess" -o "run" -o "bt 12" -o "quit" `
  (Join-Path $d 'tesv_slice.exe')
```

Guest addresses print as `f_F5894A`; the guest caller is `M32(c->r[4])`, which
the shims now log directly as `[guest] ExitProcess(code) from XXXXXXXX stack: …`.
Use `breakpoint set -f tesv_NN.c -l <line>` to stop at a specific lifted block
when the caller is not obvious.

## 5. TESV discovery and slicing

### Table roots: do not walk a pointer table one function per run

When a fault lands on an entry of a function-pointer table (observed with the
7-entry `.rdata` table at `0x1076aa0` and the 98-entry table at `0x106b468`), the
growth loop would otherwise spend one full slice/lift/run cycle per entry.
Expand the table once instead:

```powershell
.venv\Scripts\python.exe tools/table_roots.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json `
  --seed 0x103b5a2 --seed 0xf68a51 --out E:\Skyrim_N3DS_build\analysis\growth_roots.txt
```

then pass the file:

```powershell
powershell -File tools/grow_tesv_slice.ps1 -WorkRoot E:\Skyrim_N3DS_build\analysis\growth9 `
  -StartFunctions 30000 -ExtraRootFile E:\Skyrim_N3DS_build\analysis\growth_roots.txt
```

A budget at or above the closed closure (~24.5k functions) removes the
budget-cut class of faults entirely; what remains is a genuine missing function,
an unresolved import, or an indirect target outside the static closure.

### Executed-set profile

The runner counts every lifted function entry (`RT_PROF`) and writes
`tesv_profile.csv` in the run directory. Then:

```powershell
.venv\Scripts\python.exe tools\profile_executed.py tesv_profile.csv `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json --out E:\Skyrim_N3DS_build\analysis\prof --hot 512
.venv\Scripts\python.exe tools\slice_pe.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json --root 0x0 `
  --extra-root-file E:\Skyrim_N3DS_build\analysis\prof\executed.txt `
  --no-entry-root --allow-open --out E:\Skyrim_N3DS_build\analysis\slice_dynamic --emit
```

`--no-entry-root` measures the closure of what actually executed, not the whole
static startup closure.

```powershell
# Full discovery (hours; already done)
python C:\Users\vasya\Documents\Skyrim_Android\tools\discover.py `
  "E:\skyr\TESV.exe" E:\Skyrim_N3DS_build\analysis\tesv\functions.json

# Direct-call closure from entry + report/subset for lift.py
.venv\Scripts\python.exe tools/slice_pe.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json --root entry `
  --max-functions 40000 --max-insns 4000000 `
  --out E:\Skyrim_N3DS_build\analysis\slice_entry --emit

# Real PE map + IAT order with the current shims (no guest code executed)
.venv\Scripts\python.exe tools/run_host_tests.ps1 -Only tesv_probe `
  -OutDir E:\Skyrim_N3DS_build\host
```

`slice_functions.json` can be lifted with `staged/lift.py`; the report contains
projected ARM size, import call sites and dynamic indirect sites. Treat the
projection as an estimate until a 20–50 function sample is lifted and measured.

Keep all intermediate outputs (`functions.json`, `gen/`, host build dirs) on a
host disk (`C:`/`E:`). `F:` is the console SD card, may be removed at any time,
and must only hold final `.3dsx`/CIA files and game inputs.

`slice_pe.py` now locally decodes direct targets missing from the discovery
manifest (recursive descent, padding stop, jump-table heuristic) and supports
`--extra-root-file` (one hex address per line). The automated fault-driven loop:

```powershell
powershell -File tools/grow_tesv_slice.ps1 -StartFunctions 500 -StepFunctions 400 `
  -StartInsns 150000 -StepInsns 100000 -MaxIterations 10 `
  -WorkRoot E:\Skyrim_N3DS_build\analysis\growth
```

It writes `growth_journal.json` (fault address/reason per iteration, roots,
EXE/functions SHA256) and exits 2 when it needs an import or hits a rejected/
repeated fault, 1 on infrastructure failure.

Bounded TESV execution (diagnostic):

```powershell
python staged/lift.py "E:\skyr\TESV.exe" <slice>/slice_functions.json <gen> tesv 400000
python tools/instrument_generated.py <gen>
powershell -File tools/build.ps1 -Mode tesv -TesvGen <gen>
powershell -File tools/run_host_tests.ps1 -Only tesv_slice -TesvSliceGen <gen>
powershell -File tools/run_azahar.ps1 -ThreeDsx dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.3dsx `
  -Name tesv_slice -AllowFaultReport
```

The app prints/records `first fault: Guest fault at <addr>: <reason>`. Unlifted
addresses are grown via `--extra-root`; unknown imports must be implemented (or
left to trap). This is a diagnostic, never a PASS.

CIA packaging (178 MiB / 804 MHz / L2, local build only):

```powershell
powershell -File tools/build_cia.ps1 `
  -Elf dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.elf
# -> build_cia\Skyrim_N3DS_tesv.cia + cia_manifest.json
```

Install via Azahar GUI (File → Install CIA). `azahar -i <cia>` exists but did
not complete unattended in this environment. After launch, verify
`osGetMemRegionSize(MEMREGION_APPLICATION)` before trusting a 178 MiB budget.

## 6. Reproduce the 2026-11-24 deep-slice result

```powershell
# 1. Current startup slice with every verified table root found so far
.venv\Scripts\python.exe tools/slice_pe.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json --root entry `
  --max-functions 32000 --max-insns 5000000 `
  --extra-root-file E:\Skyrim_N3DS_build\analysis\growth_roots14_all.txt `
  --allow-open --emit --out E:\Skyrim_N3DS_build\analysis\repro_slice

# 2. Lift + instrument (global-register codegen is the default)
.venv\Scripts\python.exe staged/lift.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\repro_slice\slice_functions.json `
  E:\Skyrim_N3DS_build\analysis\repro_gen tesv 400000
.venv\Scripts\python.exe tools/instrument_generated.py E:\Skyrim_N3DS_build\analysis\repro_gen --profile

# 3. Host run with the memory probe (measurement only)
powershell -File tools/run_tesv_probe.ps1 `
  -TesvGen E:\Skyrim_N3DS_build\analysis\repro_gen -BudgetMB 160

# For an isolated ARM/Azahar run that does not require holding X:
powershell -File tools/build.ps1 -Mode tesv `
  -TesvGen E:\Skyrim_N3DS_build\irfull\gen -Isolated irprobe -ForceMemProbe

# 4. ARM build + CIA (isolated so it cannot clobber another agent's artifact)
powershell -File tools/build.ps1 -Mode tesv -TesvGen E:\Skyrim_N3DS_build\analysis\repro_gen -Isolated repro
# If a long generated-code compile times out, resume the same isolated objects:
powershell -File tools/build.ps1 -Mode tesv -TesvGen E:\Skyrim_N3DS_build\analysis\repro_gen -Isolated repro -NoClean
powershell -File tools/build_cia.ps1 -Elf Skyrim_N3DS_tesv_repro.elf
```

Expected at this checkpoint: the guest passes the allocator's legal re-commit
path. Lazy VA reports about 590 MiB reserved, 95 MiB commit promise and only
68–3,608 KiB physically backed across host startup stages. The stable hardware
slice growth13/gen01 is at `0x00403780`; supervised growth26 is running from
`0x00a94b10` with 11,827 verified roots, 42k+ functions, 8 parallel lift workers, 6-way clang
and a 25M-step diagnostic budget. This is a diagnostic
checkpoint, not a successful game launch.

### Hardware CIA recipe (local, not distributable)

```powershell
# build a deep-slice CIA (178 MB extheader, 804 MHz, L2)
powershell -File tools/build.ps1 -Mode tesv -TesvGen <gen> -Isolated deep
powershell -File tools/build_cia.ps1 -Elf Skyrim_N3DS_tesv_deep.elf
```

If a large CIA crashes in system `loader` before main (process `loader`, PC in
`0x1400xxxx`, undefined instruction), build a no-compression control by copying
`tools/cia/template.rsf`, changing `EnableCompress` to `false`, and passing that
RSF to `build_cia.ps1 -Rsf`. This tests the compressed-ExeFS hypothesis without
changing the ELF. A small hybrid CIA is the preferred fallback.

For the next slice, build/package the physical hybrid in one reproducible step:

```powershell
powershell -File tools/build_hw_hybrid.ps1 `
  -Functions <slice_functions.json> `
  -OutRoot E:\Skyrim_N3DS_build\hw_next `
  -Isolated hwnext -RunAzahar -AzaharTimeoutSec 300 `
  -CiaOut F:\cias\Skyrim_N3DS_tesv_hwnext.cia -CopyToCard
```

The script performs ranked AOT selection, emits/validates the exact IR table,
lifts only AOT functions, builds with `-ForceMemProbe`, optionally runs Azahar,
packages the CIA, verifies hashes and writes `hw_manifest.json`. Regenerate the
forced-AOT list with `tools/ir_measure.py --fail-list` for each new slice and
exclude proven false roots before passing `-ForceAot`.

Install the CIA with FBI/GodMode9, put `TESV.exe`, `d3dx9_42.dll`, `binkw32.dll`,
`X3DAudio1_7.dll`, `steam_api.dll`, `Skyrim_default.ini`, the two profile INIs
(400×240), and the base `Data\` ESM/BSA resources under `sdmc:/Skyrim/`. The card
copy includes hash manifests (`INPUT_SHA256.txt`, `DATA_SHA256.txt`) and
`README_N3DS_TEST.txt`. On start, **hold X** to run with the memory
probe (measurement only); the SD log `sdmc:/Skyrim_N3DS_tesv.log` then contains
the fault, the trap list, the executed-function count and the `[mem]` report.

Expect at this checkpoint: without X the game refuses at the 512 MiB
minimum-config check; with X it creates four lazy VA arenas (about 590 MiB
reserved / 95 MiB commit promise) and physically backs only the pages touched so
far. The 3DS implementation uses non-contiguous page backing because
`MEMOP_RESERVE` is unavailable. The next failure should be the same diagnosed
guest frontier as the host, not an allocator-capacity failure.

Hardware acceptance evidence for this checkpoint (record, do not assume):

1. CIA/RSF reports `SystemModeExt: 178MB`, 804 MHz and L2 enabled.
2. Runtime reports `App memory region: 178 MiB` (not the 96 MiB `.3dsx` value).
3. With X held, startup prints the same `[mem] va reserved/committed/backed`
   classes as the host and reaches the same guest fault address/reason.
4. Without X, the minimum-configuration refusal remains explicit; the 512 MiB
   probe is never used as a shippable capability claim.
5. No `svcBreak` before `main`, no corrupt-texture/VA fault, and no SD-card build
   intermediates. Only the final local CIA and owner-provided game inputs belong
   on the card.

Azahar CIA execution is useful evidence for packaging and the 178 MiB mode, but
it is **not** a physical New 3DS measurement. A hardware run must include the
console model/system version and the archived SD log.

Validate the copied log on the host:

```powershell
.venv\Scripts\python.exe tools\check_hw_log.py <log> --expected-fault 00b2a970 --require-ir
# hwhybridprobe: --expected-fault 00403780 --require-ir
# hwtest: --expected-fault 00403780 (without --require-ir)
```

Physical `hwhybridprobe` passed this check on New 3DS: 178 MiB region, guest
35,653,056 B, fault `0x00403780`, IR cache 956 blocks / 23,193 hits, no
`svcBreak`. The SD log is `sdmc:/Skyrim_N3DS_tesv.log`; the profile is
`sdmc:/Skyrim_N3DS_tesv_prof.csv`.

### Optional guest-memory watchpoint

Set `N3DS_WATCH_ADDR` to a guest address and optionally `N3DS_WATCH_VALUE` to a
specific 32-bit value. The watch is disabled unless the address is set. It keeps
a bounded 128-record ring with guest block PC, cooperative thread id, old/new
value, source path and the containing guest region/slab boundaries.

Diagnostic generated slices should be instrumented with
`tools/instrument_generated.py` (PC-aware block ticks) and built with
`N3DS_WATCH_STORE_CHECKS=1`. That enables checks after AOT `TS8/TS16/TS32`
stores, overlapping `rep movsd`, atomic read-modify-write and IR stores. Win32
`memcpy`/`memset`/Interlocked shims, CRT heap realloc and IR SSE stores are
checked in all builds; an inactive watch returns immediately.

```powershell
$env:N3DS_WATCH_ADDR = '0x30000d18'
$env:N3DS_WATCH_VALUE = '0x30000c90'
$env:N3DS_WATCH_STORE_CHECKS = '1'
```

The first matching record is printed immediately; the ring is printed when it
fills or the guest memory is destroyed. Addresses are placement-specific: if the
heap layout changes, re-arm with the newly measured object/field addresses.
PC-instrumented slices also print the last 64 visited block addresses for the
thread that exhausts the step budget.

## 7. Guest vs ARM address spaces

- ARM PC/LR/SP: the real `.3dsx` execution in Azahar/3DS, symbolized by
  `Skyrim_N3DS*.elf` and the linker map.
- Guest EIP/ESP/EAX: x86 state inside the recompiled runtime. Symbols are
  `f_XXXXXXXX` (guest virtual address, e.g. `f_802FA80` for the multiply body).
- The `.3dsx` may relocate sections; verify load addresses before trusting
  cross-references between an ELF symbol and a runtime PC.

Azahar GDB stub is available in settings (`Debugging_UseGdbstub`,
port 24689 default) but was **not enabled or validated** in this session.
Do not expose it beyond loopback. If stepping misbehaves, retry with CPU JIT
disabled. Without a stub, use `arm-none-eabi-addr2line`/`nm` with the archived
ELF and the runtime's own fault reports.

## 8. Pilot generation diagnostics

```powershell
.venv\Scripts\python.exe tools/prepare_pilot.py `
  "C:\Windows\SysWOW64\D3DX9_42.dll" staged pilot_out_new
```

- Candidates: Transpose, Scaling, Translation, Multiply, MultiplyTranspose,
  Vec3TransformCoord, Vec3TransformNormal, Vec3Normalize, MatrixInverse,
  PlaneNormalize, PlaneTransform (see `--help`).
- Rejections per export in `pilot_out_new/rejections.json`, detailed lifter
  output in `<export>/lift.log`, manifest in `<export>/gen/pilot_manifest.json`.
- D3DX9_42 exports are MSVC lazy-ILT thunks. The tool verifies the exact layout
  (`FF 25 slot` + `CC*5`, guard wrapper, one-time initializer call) and resolves
  the real body from the initializer's copied table. It falls back to
  `export+0xB` only when that address is a real body, not another wrapper. If a
  DLL does not match, generation fails with a specific message instead of
  guessing. The current generated table is in `docs/D3DX_PILOT_TABLE.md`.
- A bypassed initializer is always printed and stored in the log/manifest.
  Never remove that note to make a run look cleaner.

## 9. Preparing a separate worktree for builds

Large generated inputs are not committed: a fresh worktree is missing
`staged`, `pilot_out_mult` and `.venv`. A build there can then fail with a
**false** guest fault at `0x802fa80` (missing staging/pilot code), not a real
CPU/lifter bug. Link them from the main tree before building:

```powershell
# From the worktree (defaults: -Worktree = current dir,
# -Main = C:\Users\vasya\Documents\Skyrim_N3DS)
powershell -ExecutionPolicy Bypass -File tools\prepare_worktree.ps1

# Explicit paths
powershell -File tools\prepare_worktree.ps1 `
  -Worktree C:\Users\vasya\Documents\Skyrim_N3DS_wt\wt-prep `
  -Main C:\Users\vasya\Documents\Skyrim_N3DS
```

The script creates junctions (falls back to a copy if a junction fails) only
when the target is absent, then checks and reports `staged\fpu.c`,
`pilot_out_mult\D3DXMatrixMultiply\gen` and `.venv\Scripts\python.exe`.
Exit code is `0` when all three are present, `1` when an input is missing
(warning), `2` for a bad path. Re-running is safe: existing directories and
junctions pointing elsewhere are left untouched. It never edits source,
tests, `.git`, DevCaps or `F:`.

```powershell
# Remove only the junctions this tool created; real directories are kept.
powershell -File tools\prepare_worktree.ps1 -Worktree <wt> -Clean
```

`-Clean` verifies the reparse point and its recorded target, then deletes the
junction itself non-recursively (it does not follow the link into the main
tree). Copies are never removed automatically. The creation record lives in
`%TEMP%\prepare_worktree_state\`, not in the repo. Git lists junctions as
untracked symlinks because the ignore rules for these directories have a
trailing slash; run `-Clean` before committing, and do not stage them.
