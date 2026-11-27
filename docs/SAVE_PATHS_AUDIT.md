# SAVE PATHS / REGISTRY AUDIT

Worktree `save-audit`, base `80006da`. Scope: startup file/registry paths,
profile roots, registry value types, and the save read/write surface. No
shader/menu/IR/DevCaps change.

## 1. What the engine asks for at startup

Static caller VAs from the retail image (`F:\Skyrim\TESV.exe`,
`functions.json` block map) plus the host trace in
`E:\Skyrim_N3DS_build\host_gen_full3\run_tesv_slice_11404\run_2e8*.log`.

| Surface | Value / path | Caller VA |
|---|---|---|
| Registry key | `HKLM\Software\Bethesda Softworks\Skyrim` | `0x69d625` -> helper `0xb1fe5e` (`RegOpenKeyExA`) |
| Registry value | `sBasePath` | referenced at `0x69d719` (object `0xa529f0`) |
| Registry value | `Installed Path` (real installer value, REG_SZ) | same key |
| Profile root | `SHGetFolderPathA(CSIDL_LOCAL_APPDATA)` + `\Skyrim\` | `0x6912a7` (wrapper `0x6912a0`), from WinMain `0x69d238` |
| Profile root | `SHGetFolderPathA(CSIDL_PERSONAL)` + `\My Games\` + `Skyrim\` | `0x6912a7`, same wrapper |
| INI | `<Documents>\My Games\Skyrim\Skyrim.INI` | `0x691371` (append `0x1272e68`), fallback `CopyFileA` `0x6913a5` (`Skyrim_default.ini`) |
| INI | `<Documents>\My Games\Skyrim\SkyrimPrefs.ini` | WinMain `0x69d660` (append `0x1272e70`) |
| Plugins | `<LocalAppData>\Skyrim\.\Plugins.txt` | literal `0x1272e74` (`.\Plugins.txt`), appended in WinMain `0x69d6e8` |
| Saves | `<Documents>\My Games\Skyrim\Saves\...` | literals `Saves\` `0x107bef0`, `Saves\%s` `0x10cc8b8`, `Saves\*` `0x10cc8d0`, `Saves` `0x10cca14` |
| Renderer info | `<Documents>\My Games\Skyrim\RendererInfo.txt` | literal `0x114e122`; written by the launcher, no direct TESV code xref found |
| Install root | `C:\Skyrim\<module>` | `GetModuleFileNameA` callers `0xf5d410`, `0xf68ec3`, `0xa45119`, `0xb2b805`, `0xe13492` |
| Data / archives | `C:\Skyrim\DATA\*.bsa`, `C:\Skyrim\Update.bsa`, `ControlMap*.txt` | observed in the same host trace; all map through `path_for()` |

Real PC layout on this machine:

```
C:\Users\vasya\Documents\My Games\Skyrim\{Saves, SKSE, RendererInfo.txt, Skyrim.ini, SkyrimPrefs.ini}
C:\Users\vasya\AppData\Local\Skyrim\{DLCList.txt, plugins.txt, SteamModList.txt}
```

sdmc profile (`F:\Skyrim`, mounted as `sdmc:/Skyrim`):

```
F:\Skyrim\Documents\My Games\Skyrim\{RendererInfo.txt, SkyrimPrefs.ini, Skyrim.INI}
F:\Skyrim\LocalAppData\Skyrim\Skyrim.ini
```

There is no `Saves\` on the SD profile yet; the profile builder creates
`Documents`, `My Games`, `Skyrim` and `LocalAppData\Skyrim`, and `Saves` is
created on first save. Absence of `Saves\` or `RendererInfo.txt` does not gate
the startup branch (see section 4).

## 2. Discrepancies found

1. **`.`` components were rejected.** TESV really emits
   `LocalAppData\Skyrim\.\Plugins.txt`; `path_for()` returned -1 on the `.`
   segment, so `CreateFileA` returned `ERROR_INVALID_PARAMETER` and Plugins.txt
   was never opened. Same for `.\DATA\...` on the first probe (it only worked
   because a second probe stripped the `C:\Skyrim\` root).
2. **`Installed Path` was missing from the seeded registry key.** The host
   registry's 32-bit view has
   `HKLM\Software\WOW6432Node\Bethesda Softworks\Skyrim\installed path =
   E:\SteamLibrary\steamapps\common\Skyrim\` (REG_SZ). The shim only seeded
   `sBasePath`, so a guest query for `Installed Path` was logged as a miss.
3. **No language value in the real key.** Language is `sLanguage` in
   `SkyrimPrefs.ini`; no `Language` registry value exists. Nothing is seeded
   for it, and a query would be reported as a genuine miss.
4. `SetCurrentDirectoryA` had the same `.` rejection and did not normalise the
   stored cwd; it now drops `.` and still rejects `..`.
5. `sBasePath`/`Installed Path` must stay the **virtual** `C:\Skyrim\`, not a
   host path. Returning `E:\SteamLibrary\...` would be rejected by the sandbox
   (only the configured root, `C:\Skyrim\` and `F:\Skyrim\`/`sdmc:/Skyrim` are
   aliases of the one root).

## 3. Fixed in this worktree

* `source/win32_shims.c`
  * `path_for_impl()` drops `.` components (Windows semantics) while keeping
    `..`, empty segments and escaping drive/absolute paths rejected.
  * `s_SetCurrentDirectoryA()` applies the same normalization and stores a
    NUL-terminated cwd.
* `source/win32_registry.c/.h`
  * install key now seeds `sBasePath` **and** `Installed Path`, both `REG_SZ`,
    exact size `strlen("C:\\Skyrim\\") + 1`, case-insensitive lookup.
* `docs/REGISTRY_SHIM.md` updated to match.

Registry query semantics were already correct: `REG_SZ`/`REG_DWORD` type and
required size are reported, short buffers return `ERROR_MORE_DATA` with the
required size and untouched data, `NULL` data performs a size/type probe.

## 4. Absence of `Saves\` / `RendererInfo.txt`

* `0x6982c0` is an INI-driven startup probe (`bUseMyGamesDirectory:General`,
  `sStartingCell`, `sStartingWorld`, `sStartingCellX/Y`, `bRunVTuneTest`,
  `iFPSClamp`). It performs no save-file or RendererInfo I/O, so a missing
  `Saves\` or `RendererInfo.txt` cannot change its return value.
* `0x690f90` is a spin-wait: while `[0x1b2e868] && [obj+0x2c]` it calls
  `Sleep(0)`, then invokes `[obj]->vtable[0]` and clears the global. No file
  or registry dependency.
* The save list is built later from the profile root; a missing `Saves\`
  directory yields an empty enumeration, not a startup branch change.

## 5. Regression tests

* `tests/win32_save_paths_smoke.c` (new, registered as `save_paths`):
  * maps `LocalAppData\Skyrim\.\Plugins.txt`, `.\DATA\...`,
    `C:\Skyrim\...`, `F:\Skyrim\...`, `sdmc:/Skyrim\...`,
    `Documents\My Games\Skyrim\...`;
  * rejects `..`, `Documents\..\..\escape`, `C:\Windows\...`;
  * creates the profile directories through `CreateDirectoryA`;
  * writes/reads `Saves\autosave.ess` and `Saves\autosave.skse`, then
    enumerates `Saves\*.ess`;
  * writes and stats `LocalAppData\Skyrim\.\Plugins.txt`.
* `tests/win32_registry_smoke.c` now checks `Installed Path` (`REG_SZ`,
  exact size, case-insensitive `Sbasepath` alias) in addition to
  `sBasePath`.
* Verified: `lift` (includes `win32_smoke`), `registry`, `save_paths` all pass
  with `-SkipPython`.

## 6. Save list ordering (Continue)

`tests/win32_save_paths_smoke.c` also creates three `zz_*.ess` files with
distinct mtimes, enumerates `Saves\zz_*.ess`, reads `ftLastWriteTime` from the
320-byte `WIN32_FIND_DATAA` and sorts newest-first with the guest
`CompareFileTime` shim, then checks that a `*.ESS` pattern matches
case-insensitively and does not return the `.skse` sidecar. The engine itself
sorts the list (`0xa5a9df`); the shim only has to return a truthful
`st_mtime`. Details and the full save-loader checklist are in
`docs/SAVE_LOADER.md`.
