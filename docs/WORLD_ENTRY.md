# WORLD_ENTRY - bypassing StartMenu and entering a cell directly

Status: **static disassembly only, no run claimed.** Worktree `world-entry`
(base `9a3deed`). Scope: docs + tools only, no C changes, so this does not
collide with the pre-menu crash / loading work on `0xa48930`.

Image: `E:\census_ir2\ir_container\_work\ir_container_input.bin`
(`off = 16 + VA - 0x401000`; header `<IIII` = magic `0x49524D31`, lo
`0x401000`, code size `0xc6931a`). Strings and `.data` are **not** in the
container, so string/settings work uses the on-disk PE
`E:\SteamLibrary\steamapps\common\Skyrim\TESV.exe` (same addresses, same
`.text` base `0x400000`).

Tools added in this worktree:

| tool | purpose |
|---|---|
| `tools/world_dis.py` | disassemble a VA range from the IR container image |
| `tools/world_xref.py` | find direct `call`/`jmp` xrefs to a VA inside the image |
| `tools/world_pestr.py` | locate engine string constants in the PE and dword refs to them |
| `tools/world_esm_cells.py` | list `CELL` editor IDs + interior flag from a TES5 ESM (handles compressed records) |

---

## 1. The startup decision block (`0x69dd40`) - the menu bypass that already exists

`0x69dd40`..`0x69de5f` is a one-shot block in WinMain's main function, executed
**before** the message pump (`0x69ded0`). It is where the engine decides
"show StartMenu" vs "go straight to the game".

```
0x69dd6d  lea  ecx,[esp+0x90]; call 0x691410   ; load INI settings ([General] strings/ints)
0x69dd85  call 0x699b20                        ; pre-start prep
0x69dd96  push &cell_out (zeroed)
0x69dd9f  call 0x6982c0                        ; ecx = [0x1b2e864] (TES singleton)
0x69ddaa  mov  [esp+0x17], al                  ; same byte as [esp+0x13] later
0x69ddd3  cmp  al, 0 ; jne 0x69dde6            ; al != 0 -> skip 0x693860
0x69ddf5  cmp  al, 0 ; je  0x69de54            ; al == 0 -> SKIP StartMenu entirely
0x69de01  push -1 ; call 0x43bd60              ; cell flush/reset
0x69de08  push 1  ; call 0x876f60              ; StartMenu create (0x876fc0 -> 0xa60c80 CreateMovie)
0x69de22  push 1,0,0,0 ; call 0x4352d0         ; start the menu-scene load (loader mode 2)
0x69de54  (al == 0 path) call 0xf4f290          ; window/game-mode flip only
```

`al` is the return of `0x6982c0`, and the tail at `0x698544` makes the polarity
explicit:

```
0x698544  mov ecx,[0x1b2e864]
0x69854a  call 0x432e70
0x69854f  xor  eax,eax
0x698551  cmp  dword [ebx], 0        ; ebx = &cell_out
0x698556  sete al                    ; al = 1  <=> cell_out == 0
0x69855d  ret
```

So:

* `0x6982c0` returns **1** when *no* starting cell was resolved -> normal
  launch -> StartMenu is created (`0x876f60(1)`) and the menu-scene load is
  kicked (`0x4352d0(1,0,0,0)`).
* `0x6982c0` returns **0** when it *did* resolve a cell -> the whole StartMenu
  block is skipped and execution goes straight back to the pump.

This is the legal, engine-intended menu bypass: **set a starting cell in
`Skyrim.INI` and StartMenu is never created.**

## 2. `0x6982c0` - what it resolves and how

```
push ebp/esi/edi, sub esp,0x18
ecx = [0x1b2e864]                      ; TES
call 0x432e50                          ; reset cell/player placement state
[cell_out] = 0
edx = [0x1272fb4]                      ; sStartingCell (char*)
if (strlen(edx)) {                     ; 0x698304..0x698310
    if (0x451b60(edx)) {               ; TESForm::LookupByEditorID(name)
        [cell_out] = form;             ; 0x69831f/0x69848e
        goto place;
    }
}
if (strlen([0x1272fd8]) && strlen([0x1272fe4])) {   ; 0x6983e0..0x698410
    x = to_int([0x1272fd8]);           ; 0x698416 -> 0x4f5330(world, x, y)
    y = to_int([0x1272fe4]);
    world = 0x43bcb0(0x12e2cb8, 0x47, 0);           ; worldspace resolve
    cell  = 0x4f5330(world, x, y);                  ; grid cell
    if (!cell) cell = 0x43ef60(x, y, ...);          ; fallback cell lookup
    [cell_out] = cell;
}
place:                                  ; 0x69848e
[0x1b2e885] = 1                         ; "started from INI cell"
if (cell->byte[0x2c] & 1) {             ; interior
    0x43a120(cell, &pos);               ; cell -> player position
} else {
    world = cell->[0x84];               ; exterior: parent worldspace
    0x433a70(world, 1);
    0x4c0510(cell, &x, &y);             ; grid coords
    0x437c00(player, &pos); 0x4749b0(...);   ; 0x69852c/0x69853f placement
}
0x43bd60(-1);                           ; flush/reset old cell (0x698518)
```

`0x451b60` is `TESForm::LookupByEditorID`: it takes a `const char*`, locks the
form table CS `0x12e59ec`, builds a `BSFixedString` key (`0x43ba00`), looks up
the map head `0x12e59d8` (`0x451ab0`) and returns the form or 0:

```
0x451b74  cmp dword [0x12e59d8], 0 ; je return-0
0x451b7c  mov ecx,0x12e59ec ; call 0xa4aee0       ; lock
0x451b8b  call 0x43ba00                            ; BSFixedString(name)
0x451b9e  call 0x451ab0                            ; map lookup
```

### Settings slots consumed

The `[General]` setting records live in `.data` as 12-byte records
`{value, name, type}` (name string constants are DXE-style
`"sStartingCell:General"` in the block `0x10ce580..0x10ce650`). The values
`0x6982c0` dereferences are the four slots immediately *before* each name slot:

| setting | value slot read by `0x6982c0` | name slot | form |
|---|---|---|---|
| `sStartingCell` | `0x1272fb4` | `0x1272fb8` -> `0x10ce5a8` | `char*`, default `""` (`0x10d1068`) |
| `sStartingWorld` | `0x1272fcc` | `0x1272fd0` -> `0x10ce5e0` | resolved as worldspace id via `0x43bcb0(..,0x47,0)` |
| `sStartingCellX` | `0x1272fd8` | `0x1272fdc` -> `0x10ce5f8` | numeric string, `to_int` |
| `sStartingCellY` | `0x1272fe4` | `0x1272fe8` -> `0x10ce610` | numeric string, `to_int` |

Both `sStartingCell` (editor ID, interior cells) and
`sStartingWorld`+`sStartingCellX/Y` (exterior grid) are live code paths in this
build. The editor-ID path is the simplest: it needs no worldspace/grid math and
no `uGrids` streaming of neighbours.

## 3. Menu path (for contrast) - `0x699530`

```
0x699530  [esi+8] = 0; 0x4352d0(1,0,0,1)      ; enter loading screen (mode 2)
0x699772  cmp byte [esi+6], 0
   == 0 -> 0x876f60(0) ; 0x4352d0(1,0,0,0)     ; "New"  -> StartMenu scene load
   != 0 -> ecx=[0x1b38308]; call 0x6a0ae0      ; "Continue" -> Saves manager
```

`0x6a0ae0` (and its sibling `0x6a0b30`) is a thin selection-commit wrapper on
the singleton at `0x1b38308`:

```
0x6a0ae0  esi = ecx
          call 0x6f3ab0                      ; lock helper
          [esi+8] = [esi+4]                  ; commit cursor
          call 0x6a0870([esi], 0)            ; load the selected entry
          call 0x6f3b40                      ; unlock
```

**There is no path argument.** `0x6a0870` loads whatever the Saves manager
already enumerated/selected. A direct "load this file" call therefore has to
first drive the manager's list (`[0x1b38308]+4` / `+8`) - i.e. reproduce menu
state, not just pass a filename. See option B.

## 4. Three ways into the world

### A. `sStartingCell` in `Skyrim.INI`  (recommended, zero code)

Insert into the guest `Documents\My Games\Skyrim\Skyrim.INI`:

```ini
[General]
sStartingCell=RiverwoodSleepingGiantInn
```

Verified editor IDs from `Skyrim.esm` (tool output, `interior=1` means the
`DATA` flags bit 0 is set):

```
RiverwoodSleepingGiantInn        form=000133c6 interior=1
RiverwoodGerdursHouse            form=000133c7 interior=1
RiverwoodAlvorsHouse             form=000133c8 interior=1
RiverwoodRiverwoodTrader         form=000133c9 interior=1
RiverwoodFaendalsHouse           form=000133ca interior=1
WhiterunBreezehome               form=000165a8 interior=1
WhiterunDragonsreach             form=000165a3 interior=1
WhiterunBanneredMare             form=0001605e interior=1
WhiterunDrunkenHuntsman          form=000165b7 interior=1
```

Reproduce with:

```
python tools/world_esm_cells.py "F:\Skyrim\Data\Skyrim.esm" Riverwood Whiterun
```

Risk: **low-to-medium**. No code patch, no host change, and a bad editor ID
degrades safely (`LookupByEditorID` returns 0 -> fallback path -> return 1 ->
normal StartMenu). Unknowns are (a) whether the world-load kick is done from
the frame pump instead of `0x4352d0(1,0,0,0)` on this branch, and (b) asset
coverage for the cell (see section 5).

### B. Drive "Continue" programmatically

Requires the Saves manager (`0x1b38308`) to exist with a populated list, then
write the wanted index to `[0x1b38308]+4` and call `0x6a0ae0` (ecx = manager).
Alternatively call `0x6a0870(entry, 0)` directly.

Risk: **high**. The manager is created later in init, its entry objects are
engine-allocated, and the save itself must be fully loadable (compressed
records, Papyrus, plugin list from `Plugins.txt`, `Saves\*.ess` enumeration).
This exercises the entire save loader at once - a much bigger surface than a
cell load, and it will look like a port bug even when it is a save-version
mismatch.

### C. `-loadgame` / command line

**Not supported by this build.** There is no `loadgame`, `-startgame`,
`quickstart` or `-save` string anywhere in `TESV.exe`, and the host hardcodes
the guest command line:

```
source/guest_process.c:  snprintf(cmd, ..., "\"%s\"", "C:\\Skyrim\\<module>");
```

so even a host-side switch would be accepted by WinMain and ignored. Risk:
**high effort, zero payoff**; do not pursue.

## 5. Host layer requirements

Nothing in C has to change for option A:

* **INI read path is already real.** `source/win32_shims.c` implements
  `GetPrivateProfileStringA/IntA` over the guest-visible file system with
  sections, case-insensitive keys, quotes stripped. The engine's settings
  loader (`0x691410`, called from `0x69dd74`) goes through it, so an entry in
  the guest INI reaches `0x1272fb4` before `0x6982c0` runs.
* **File to edit**
  * host slice: `<run dir>\Documents\My Games\Skyrim\Skyrim.INI`
    (the slice already carries one; it is copied from
    `run_tesv_slice_*` `Skyrim_default.ini` on first run);
  * device/deploy: `tools/deploy_device.py` writes `<out_data>/Skyrim.ini`
    from `Skyrim_default.ini` (`INI_NAMES`, `stage_cooked`), so the key must be
    added there or to the overlay INI, not to the SD card by hand.
* **Assets** for an interior cell: `Data\Skyrim.esm` (the shim already
  accounts for the 249 MiB mapping), plus `Skyrim - Meshes.bsa`,
  `Skyrim - Textures.bsa`, `Skyrim - Misc.bsa`, `Skyrim - Shaders.bsa`,
  `Skyrim - Animations.bsa`, `Skyrim - Interface.bsa` from the existing
  `sResourceArchiveList`. No LOD/`uGrids` work is needed for an interior; the
  current INI already has `uExterior Cell Buffer=36` for exteriors.
* **Timers**: the world path runs inside the normal frame pump, so
  `TESV_TIME_OFFSET_MS` / `TESV_TIME_SCALE` stay untouched. Expect the loading
  screen for one or more frames; do not shorten it with a host hack.
* **No `Saves\` requirement** for option A (the INI probe is independent of
  save enumeration - see `docs/SAVE_PATHS_AUDIT.md` section 4).

## 6. Experiment plan (first thing in the morning)

Order matters: the slice currently dies long before `0x69dd9f`
(backtrace ends in `0x69d1b`/`0x69b1a`), so option A can only be *observed*
once the startup crash is fixed. Two cheap checks can run before that.

### Step 0 - static sanity (5 min, no run)

```
python tools/world_dis.py 0x69dd96 0x69de30     # call site + polarity
python tools/world_dis.py 0x6982c0 0x698560     # resolver + sete al tail
```

Confirm the two `cmp byte [esp+0x13]` sites still read the byte written at
`0x69ddaa`. If a future port patch touches this block, the polarity is the
thing to re-check.

### Step 1 - host slice with the key set (after the crash fix)

1. Copy the slice run dir, add to `Documents\My Games\Skyrim\Skyrim.INI`:

   ```ini
   [General]
   sStartingCell=RiverwoodSleepingGiantInn
   ```

2. Run the same invocation as the recorded baseline
   (`tesv_slice.exe <TESV.exe> . 200000000 167772160 *`) with
   `TESV_FILE_TRACE=1` so the INI open and the BSA/ESM reads are visible.

3. Expected log differences vs the baseline:
   * `CreateFileA`/`GetPrivateProfileStringA` for `...\Skyrim.INI` hits
     `[General] sStartingCell` (the file trace shows the open; the shim has no
     per-key trace, so this is inferred from the subsequent behaviour);
   * **no** `0x876f60` / `0xa60c80 CreateMovie` for `StartMenu` - i.e. no
     StartMenu SWF load in the log;
   * a cell load instead: `Skyrim - Meshes.bsa` reads for the cell's
     `meshes\...` paths and a `DrawIndexedPrimitive` stream from the world
     scene rather than the loading-scene one (`0xcd39fa`).

4. Check points, in order:

   | check | how |
   |---|---|
   | `0x6982c0` reached | breakpoint/log on `0x69dd9f`; if still unreached, the crash fix is not complete |
   | it returned 0 | evaluate `[cell_out]` != 0 at `0x69854f`, or `al` at `0x69de54` |
   | `[0x1b2e885]` == 1 | dump the byte after `0x69848e` |
   | no StartMenu | no `0x876f60` call at `0x69de08`; `[0x1b3fc98]` stays 0 |
   | loader mode | `[0x1b39120]` / `[0x1b39124]` - if they stay 0, nothing kicked the stream and the frame pump must be the one to start it (see open question) |
   | frame pump runs | `0x69cbd0` entered, `0x69ce15 -> 0x6f3ea0(1)` in the call histogram |

### Step 2 - fall back to "no cell" control

Rerun with `sStartingCell=` (empty). `al` must be 1 and StartMenu must be
created again. This isolates any difference to the single key.

### Step 3 - device

Only after the slice shows the world frame: add the key to the deploy INI
source (`tools/deploy_device.py` `stage_cooked` / `Skyrim_default.ini`) and
redeploy. Watch for the upper/lower-case `Skyrim.INI` vs `Skyrim.ini`
mismatch on FAT (deploy currently writes `Skyrim.ini`, the engine appends
`Skyrim.INI`); this is benign on the Windows host only.

## 7. Open questions (and how to settle them)

1. **Who kicks the world load on the `sStartingCell` branch?** The menu branch
   calls `0x4352d0(1,0,0,0)` at `0x69de22`; the `al == 0` branch does not.
   Settlement: log `0x4352d0` call sites and `[0x1b39120]` after the startup
   block.
2. **Does `0x6982c0` also spawn the player?** `0x437c00`/`0x4749b0` at
   `0x69852c`/`0x69853f` look like placement, but the branch at `0x6984b3`
   (`cell->[0x2c] & 1`, test at `0x6984a7`) also feeds `0x43a120`. Settlement: dump the player
   object position after `0x698544` and compare with the cell's north marker.
3. **Are `sStartingCellX/Y` value slots really `0x1272fd8`/`0x1272fe4`?** The
   records are 12 bytes `{value, name, type}` and the name constants sit at
   `value + 4`, which fits all four reads; the alternative (`+8` layout) would
   make the X/Y slot hold the name pointer. Settlement: set only
   `sStartingCellX=1` and inspect the dword at `0x1272fd8` in a debugger.
4. **Interior vs exterior first.** Interior avoids grid streaming/LOD; an
   exterior (`sStartingWorld=Tamriel` + X/Y) is the next step and the first
   real test of `uGrids` / `0x4f5330` cell streaming.

## 8. Safe host-side mechanism (implemented: `TESV_INI_OVERLAY`)

The overlay is implemented in `source/win32_shims.c` (INI read path only) and is
the supported way to run the world-entry experiment without touching the
profile on the SD card or in the slice run directory.

### 8.1 Usage

```bat
:: file form: plain key=value lines, [General] implied
set TESV_INI_OVERLAY=C:\temp\world.ini
:: or inline form
set TESV_INI_OVERLAY=sStartingCell=RiverwoodSleepingGiantInn;bPreloadIntroSequence=0
:: optional diagnostics
set TESV_INI_TRACE=1
set TESV_INI_LOG=C:\temp\ini_trace.log
```

`world.ini`:

```ini
sStartingCell=RiverwoodSleepingGiantInn
```

Supported `[General]` keys: `sStartingCell`, `sStartingWorld`,
`sStartingCellX`, `sStartingCellY`, `sIntroSequence`,
`bPreloadIntroSequence`. Unknown keys are ignored, duplicates are last-wins,
`;` and newlines are both separators, surrounding quotes are stripped.

### 8.2 Semantics

* The overlay is merged into the buffer the Win32 profile parser sees when
  `Skyrim.INI` / `Skyrim.ini` is read, so `GetPrivateProfileStringA/IntA`
  return the overlay value exactly as if the file contained it.
* The block is appended as a fresh `[General]` section at EOF. The parser is
  last-match-wins, so the overlay overrides the file's own value while the file
  is never rewritten. Consequence: section enumeration lists `General` twice;
  key lookups (what the engine's settings loader `0x691410` does) are
  unaffected.
* Only reads are overlaid. `WritePrivateProfileStringA` uses the raw file
  (`ini_read_raw`), so a write never persists overlay values.
* `SkyrimPrefs.ini`, `Skyrim_default.ini` and any other INI name are untouched.
* `TESV_INI_OVERLAY` unset or empty: the reader is byte-for-byte the previous
  implementation.
* No guest memory is patched and `0x1272fb4` is never written by the host: the
  engine's own settings loader stores the string.

### 8.3 Experiment

```
set TESV_INI_OVERLAY=sStartingCell=RiverwoodSleepingGiantInn
set TESV_INI_TRACE=1
set TESV_INI_LOG=%CD%\ini_trace.log
tesv_slice.exe <TESV.irc/exe> . 200000000 167772160 *
```

Expected log (`ini_trace.log`):

```
[ini] overlay applied to Documents\My Games\Skyrim\Skyrim.INI: sStartingCell=RiverwoodSleepingGiantInn
[ini] Get "General" "sStartingCell" -> "RiverwoodSleepingGiantInn" (overlay)
```

The second line is the engine's own settings-load call for that key, i.e. the
value that lands in `0x1272fb4` and is consumed by `0x6982c0`. Checkpoints are
unchanged from section 6: `0x6982c0` must return 0 and StartMenu must not be
created.

### 8.4 Tests

`tests/win32_ini_overlay_smoke.c` (registered as `ini_overlay` in
`tools/run_host_tests.ps1`) drives the same parser/merge through
`win32_ini_overlay_merge_text()` with no guest memory: file and inline forms,
last-wins duplicates, unknown keys ignored, quoted values, CRLF, missing
`[General]` (appended), `[Display]` untouched, non-target INI names, the
no-overlay byte-for-byte case and the argument guards. The test also prints the
effective tail, so the run log shows the appended `sStartingCell` line.

### 8.5 Rejected alternatives

Writing a host-allocated string pointer into `0x1272fb4` remains rejected: the
loader re-allocates its own copy, so a hand-written pointer leaks or is
overwritten. An argv switch is a dead end (the engine never parses extra
arguments).

## 9. Summary

* The engine already contains a first-class bypass: `[General]
  sStartingCell=<editor id>` makes `0x6982c0` resolve a cell, return 0, and
  skip the StartMenu creation at `0x69de08`.
* `sStartingCell` is an **editor ID** looked up by `0x451b60`
  (`TESForm::LookupByEditorID`), not a form ID and not a filename.
* `Continue` is not addressable by path: `0x6a0ae0` commits whatever the Saves
  manager has selected.
* Command-line `-loadgame` does not exist in this build and the host hardcodes
  the guest command line.
* Recommended first test: `RiverwoodSleepingGiantInn`
  (`0x000133C6`, interior) via `TESV_INI_OVERLAY` (section 8), with an
  empty-value control run.
