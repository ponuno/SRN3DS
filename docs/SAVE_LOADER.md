# SAVE LOADER (.ess) - host-layer audit

Worktree `save-loader`, base `2969b62`. Scope: the "Continue" path into the
world (`[0x1b38308]+4 = index; call 0x6a0ae0` -> `0x6a0870`), the `.ess`
container, and the Win32/file surfaces that path needs from the host. No
shader, texture, `ir_*`, `guest*`, `DevCaps` or `sr` change.

Real saves were read only, from
`C:\Users\vasya\Documents\My Games\Skyrim\Saves\*.ess`.

## 1. Container format actually observed (LE, save version 9)

Reference: UESP "Skyrim Mod:Save File Format". Two details differ from the
common summary and were confirmed on all 27 local saves:

* strings are `uint16 byteCount` + **one byte per character** (ASCII/ANSI),
  not UTF-16;
* the LE screenshot is **3 bytes/pixel (RGB)**, and on this machine it is
  `320x192` (184,320 bytes), not the often-quoted `128x128 BGRA`.

```
offset  field
0x000   char[13]  "TESV_SAVEGAME"
0x00d   u32       headerSize              (observed 74..97)
0x011   u32       version                 (9 = LE 1.9; 12 = SE)
0x015   u32       saveNumber
        string    playerName
        u32       playerLevel
        string    playerLocation
        string    gameDate
        string    playerRace
        u16       playerSex
        f32       playerCurExp
        f32       playerLvlUpExp
        u64       filetime (FILETIME)
        u32       shotWidth
        u32       shotHeight
        u8[]      screenshot: LE w*h*3 RGB, SE w*h*4 BGRA
SE only u32       uncompressedLen / compressedLen, u16 compressionType
        u8        formVersion             (74 for LE 1.9)
        u32       pluginInfoSize          (includes the count byte)
        u8        pluginCount
        repeat    u16 len + name bytes
SE only lightPluginInfo: u16 count + strings
        FileLocationTable: 10*u32 + 15*u32 unused = 100 bytes
        globalDataTable1[count1]          type u32, length u32, data
        globalDataTable2[count2]
        changeForms[count]
        globalDataTable3[count3]
        u32 formIDArrayCount + u32 formIDs[]
        u32 visitedWorldspaceCount + u32 formIDs[]
        u32 unknown3TableSize + data
```

FileLocationTable fields, in order:
`formIDArrayCountOffset, unknownTable3Offset, globalDataTable1Offset,
globalDataTable2Offset, changeFormsOffset, globalDataTable3Offset,
globalDataTable1Count, globalDataTable2Count, globalDataTable3Count,
changeFormCount`. (The 9th count is `globalDataTable3Count`; omitting it
misaligns everything by 4 bytes.)

Change form (11-byte prefix for the normal 1-byte length case):

```
u8        refID[3]        lower 22 bits = formID, upper 2 = id type
u32       changeFlags
u8        type            upper 2 bits select length width (1/2/4),
                          lower 6 bits are the form type (0=REFR, 1=ACHR, ...)
u8        version         (74)
lenWidth  length1         bytes of payload that follow
lenWidth  length2         0 = plain, else zlib and this is the inflated size
u8[]      payload[length1]
```

`globalDataTable3` type 1001 is Papyrus; it is plain (not zlib) in LE and is
by far the largest single block.

## 2. Validated on the real saves

`tools/save_ess.py` parses the header, plugin block, all three global tables,
every change form (inflating zlib where `length2 != 0`), the formID array and
the unknown3 table. Results for all 27 `*.ess`:

| property | value |
|---|---|
| files | 27 (25 named saves + `autosave1/2/3`, `quicksave`) |
| version / formVersion | 9 / 74 on every file |
| size | 4,493,279 .. 8,269,582 B |
| screenshot | 320x192x3 on every file |
| plugins | 5 .. 134 (first is always `Skyrim.esm`) |
| global tables | t1=9, t2=14..15, t3=5 on every file |
| change forms | 9,519 .. 28,967 per file |
| compressed change forms | 4,412 .. 10,955 per file |
| inflate result | **all** compressed records inflate to exactly `length2`; 0 errors |
| formID array | 9,966 .. 30k+ (parsed, count in range) |

Structural checks (`validate()`): no problems on any file. The parser also
rejects bad magic, truncated headers and corrupt zlib deterministically.

Largest single blocks: Papyrus (type 1001) reaches **5,313,831 B** in
`quicksave.ess`; most saves are 1.7-3.4 MB. Compressed change-form input is
0.58-1.99 MB, expanding to 1.27-4.25 MB. That is guest-heap pressure once the
loader runs, not a host path problem.

## 3. How TESV actually reads a save (static, `E:\skyr\TESV.exe`)

Import table of the retail image (IAT VAs):

| IAT VA | import |
|---|---|
| `0x106b03c` | `FindFirstFileA` |
| `0x106b0c8` | `FindNextFileA` |
| `0x106b128` | `FindClose` |
| `0x106b038` | `CompareFileTime` |
| `0x106b048` | `FileTimeToSystemTime` |
| `0x106b280` | `SystemTimeToTzSpecificLocalTime` |
| `0x106b24c` | `CreateFileA` |
| `0x106b284` | `ReadFile` |
| `0x106b288` | `WriteFile` |
| `0x106b140` / `0x106b28c` | `SetFilePointer` / `SetFilePointerEx` |
| `0x106b294` | `GetFileSizeEx` |
| `0x106b238` | `SetFileTime` |
| `0x106b248` | `ReadFileEx` |

**There is no `CreateFileMappingA`, `MapViewOfFile` or `FlushViewOfFile` in
the TESV import table at all.** The save path therefore cannot hit the lazy
file-view layer; it is plain buffered `CreateFileA` + `ReadFile` +
`SetFilePointer`. This is not a blocker.

Relevant code:

* `0x8767c0` builds the save-list path: copies the profile `Saves` directory,
  concatenates `*` and `.ess`, calls `FindFirstFileA` (`0x8768c2`), and closes
  with `FindClose` (`0x876923`). Failed open (`-1`) yields an empty entry.
* `0xa57f70` checks one save file: `FindFirstFileA` on the filename, takes
  `ftLastWriteTime` from `WIN32_FIND_DATAA` (`+0x14`), converts it with
  `FileTimeToSystemTime` + `SystemTimeToTzSpecificLocalTime`. **A wrong mtime
  or an all-zero mtime changes the displayed date and sort order.**
* `0xa5a9df` compares two save entries with `CompareFileTime`: newest first.
* `0xa58490` is the file-open helper. For the normal read object
  (flag bit31 clear) it passes `GENERIC_READ`, **share = 1
  (`FILE_SHARE_READ`)**, `OPEN_EXISTING`, and flags `0x08000000`
  (`FILE_FLAG_SEQUENTIAL_SCAN`). The `OVERLAPPED|NO_BUFFERING|SEQUENTIAL`
  combination (`0x68000000`) and `ReadFileEx` belong to `0xa58920`, which
  opens the literal `testfile.txt` (self-test), not a save.
* `0xa580a0` is the synchronous read wrapper: `ReadFile(handle, buf, len,
  &done, NULL)`. `0xa58180` seeks with `SetFilePointer`.
* zlib is **inside TESV**, not imported: the host must deliver bytes, not
  decompress. The parser still inflates every chunk to prove the container is
  intact.

Conclusion: host coverage is complete for the read path
(`CreateFileA/GetFileSize(Ex)/SetFilePointer(Ex)/ReadFile/CloseHandle`,
`FindFirstFileA/FindNextFileA/FindClose`, `CompareFileTime`,
`FileTimeToSystemTime`, `SystemTimeToTzSpecificLocalTime`). No patch is
strictly required.

### Residual host risks

1. **Share mask.** `create_file()` rejects `share > 3`
   (`source/win32_shims.c:2202`). Observed save open is share=1, so fine. If a
   future save code path uses `FILE_SHARE_DELETE` (4) it will get
   `ERROR_INVALID_PARAMETER`. Minimal hardening, if evidence appears:
   accept `share <= 7` and keep rejecting unknown bits.
2. **OVERLAPPED is rejected.** `flags & 0x40000000` fails. The save path uses
   `0x08000000`; only the `testfile.txt` self-test uses OVERLAPPED. Keep as is
   until a save trace shows otherwise.
3. **`ReadFileEx` is `ERROR_NOT_SUPPORTED`.** Same reasoning: not on the save
   path.
4. **32-bit offsets.** `SetFilePointer` accepts only a signed 32-bit low part
   (high part must be 0/-1). All saves are < 2 GiB, so fine. `SetFilePointerEx`
   already handles 64-bit.
5. **`FlushViewOfFile` is not exported at all.** Irrelevant for reading and
   for saves written through `WriteFile`; add it together with
   `CreateFileMapping` hardening only if a mapped write path ever appears.

## 4. Save paths on the 3DS profile

`SHGetFolderPathA(CSIDL_PERSONAL)` returns the sandbox-relative `Documents`
(`source/win32_shell.c`), and `path_for()` joins it under the configured root.
TESV then appends `\My Games\Skyrim\Saves`. On device that is:

```
sdmc:/Skyrim/Documents/My Games/Skyrim/Saves/*.ess
sdmc:/Skyrim/LocalAppData/Skyrim/Plugins.txt     (or plugins.txt)
```

The profile must literally contain `Saves` with at least one `.ess`, otherwise
Continue has an empty list. `docs/SAVE_PATHS_AUDIT.md` says the profile builder
does not create `Saves` yet - stage it with at least one real save plus the
matching plugin list.

Enumeration semantics in `source/win32_shims.c`:

* `FindFirstFileA`/`FindNextFileA` return a 320-byte `WIN32_FIND_DATAA`,
  `ftLastWriteTime` at `+20`, name at `+44`; `*`/`?` are case-insensitive.
* The shim does **not** sort; TESV collects entries and sorts with
  `CompareFileTime`. The shim only has to report a truthful
  `st_mtime` (converted by `unix_to_ft`).
* Directories and files both map; `.skse` sidecars must not be returned for a
  `*.ess` pattern (covered).
* FAT time granularity is 2 s and the console RTC may start at 1980 if never
  set. Then multiple saves get equal/zero mtimes and the "newest" order is
  arbitrary but stable. Set the RTC before copying saves if ordering matters.

Regression coverage added in `tests/win32_save_paths_smoke.c`:

* creates `zz_old/zz_mid/zz_new.ess` with three known mtimes through
  `SetFileTime`;
* enumerates `Saves\zz_*.ess`, reads `ftLastWriteTime`, sorts newest-first
  with the guest `CompareFileTime` shim, and asserts `new, mid, old`;
* enumerates `Saves\*.ESS` (case-insensitive) and checks `.skse` is excluded.

## 5. Tool and tests

* `tools/save_ess.py` - read-only validator.
  `python tools/save_ess.py --dir <Saves>` prints one summary per file;
  `--json` is machine-readable; `--limit-changes N` stops after N change
  forms (header + first chunks); exit code 1 on structural failure.
* `tests/save_ess_test.py` - synthetic minimal save built in the test
  (header, RGB screenshot, plugin info, global table, one zlib and one plain
  change form, formID array, unknown3) plus a real save (header + first
  change form only) when one is present. Verifies size, plugin count, first
  chunk inflation, corrupt-zlib reporting and truncation handling.
* Registered in `tools/run_host_tests.ps1` (Python list) and the existing
  `save_paths` C smoke.

Run:

```
python tools/save_ess.py --dir "C:/Users/vasya/Documents/My Games/Skyrim/Saves"
python tests/save_ess_test.py
powershell -File tools/run_host_tests.ps1 -Only save_paths -SkipPython
```

## 6. Checklist after the menu crash is fixed

Before pressing Continue:

1. `FindFirstFileA("...\Saves\*.ess")` returns a handle and at least one
   entry; `ftLastWriteTime` matches the host file (not 0/1980).
2. `CompareFileTime` ordering puts the intended save first; the save manager
   list has a non-zero count and `[0x1b38308]+4` can be set.
3. `Plugins.txt` is readable; its master list matches the save's plugin block
   (names and order). A mismatch is a save-loader refusal, not a host bug.

On `0x6a0ae0` / `0x6a0870`:

4. `CreateFileA` for `...\Saves\<name>.ess` succeeds with access
   `GENERIC_READ`, share=1, `OPEN_EXISTING`, flags `0x08000000`. Watch for
   `ERROR_INVALID_PARAMETER` (share/flag rejection) and `ERROR_FILE_NOT_FOUND`
   (name encoding / staging).
5. First `ReadFile` returns 13 bytes and they are `TESV_SAVEGAME`; then the
   header, screenshot skip (`w*h*3`) and plugin block are read without a short
   read.
6. `SetFilePointer`/`GetFileSizeEx` return the real file size (4.5-8.3 MB)
   and the FileLocationTable offsets are inside the file.
7. Change forms: the guest inflates each `length2 != 0` payload; no zlib
   error, no "inflated size mismatch". `tools/save_ess.py` says all real saves
   pass, so a failure here is guest/IR, not the file.
8. Global table 3 type 1001 (Papyrus) is reached; this is the largest block
   (up to 5.3 MB) and the most likely allocation failure.
9. Only then expect the cell/world load. Do not attribute a post-Papyrus fault
   to `.ess` parsing without checking steps 4-8 in the host log.

Success criteria for the host layer: no `[file] FAIL`, no
`[path] reject` for a `Saves` path, no `[mapping]` line on this path, and the
guest reaches the end of `globalDataTable3` / formID array.
