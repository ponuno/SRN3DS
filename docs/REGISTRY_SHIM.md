# ADVAPI32 registry shim

Landed from `E:\Skyrim_N3DS_registry` (scratch, 44-test suite + end-to-end
TESV probe). `source/win32_registry.c/.h`, wired through `win32_shims.c`,
`Makefile`, `tools/run_host_tests.ps1` and `tests/win32_registry_smoke.c`.
Thunk window: `WIN32_REGISTRY_BASE = 0xff090000` (checked by
`tools/check_shim_bases.py`).

## Why it exists

WinMain `0x69d1d0` opens `HKLM\Software\Bethesda Softworks\Skyrim` at
`0x0069d625` through the engine helper at `0x00b1fe5e`:

```
RegOpenKeyExA(0x80000002, subkey, 0, 0xf003f KEY_ALL_ACCESS, &hKey)
```

TESV imports exactly five registry functions: `RegOpenKeyExA`,
`RegCreateKeyExA`, `RegQueryValueExA`, `RegSetValueExA`, `RegCloseKey`.
No enumeration, delete or security APIs.

## Behaviour

* Bounded in-memory tree: 64 keys, 16 handles, 8 values/key, 512 B/value,
  192 B paths, case-insensitive. Static arrays, no per-call host allocation.
* **Install state is preloaded** because a real installation writes it:
  `HKLM\Software\Bethesda Softworks\Skyrim` with `sBasePath` (REG_SZ) and
  `Installed Path` (REG_SZ), both `C:\Skyrim\` — the virtual root the guest
  already sees through `GetModuleFileNameA`. `sBasePath` was discovered at
  runtime; `Installed Path` is the documented installer value (observed in the
  host's `WOW6432Node` view of the same key). No language value is invented:
  on a real install the language lives in `SkyrimPrefs.ini` (`sLanguage`).
* Everything else exists only when the guest writes it.
* Missing keys/values return `ERROR_FILE_NOT_FOUND`; each distinct missing
  value is logged once. No fabricated values, no host registry, no filesystem.
* Strict stdcall ABI: return address plus the exact argument count.
* Writes are volatile; no enumeration/security/`REG_EXPAND_SZ`.

## A side finding: stream double-release

After the registry open, TESV reads `LocalAppData\Skyrim\.\Plugins.txt`. Its
CRT enters a stream `CRITICAL_SECTION` once (inside `fopen`, return address
`0x00f68874`) and releases it twice through `_unlock_file`
(`0x00f5b3d0`). Measured: `enter=1, leave=2` for the same section.

Windows' `RtlLeaveCriticalSection` does not validate ownership, so the shipping
game relies on the second release not faulting. `s_LeaveCriticalSection` now
emulates that: a leave on an unheld section is a **no-op with a one-time named
diagnostic** (`[cs] leave on unheld section ... (Windows-compatible no-op)`).
This is not a fabricated success — the section was genuinely not held, and no
state is invented. A leave from a *different owning thread* still faults,
because that is a real concurrency condition the single-thread runtime cannot
honour.

## Verification

* `registry` smoke test covers open/close, unknown key, write/read back, type
  and size handling, short buffer, invalid handles, guest-memory bounds,
  case-insensitivity and all three capacity limits.
* Full host suite: **44 passed, 0 diagnostic, 0 failed**.
* TESV probe with the shim reaches the registry call, opens the key, reads
  `sBasePath`, and continues to the stream double-release.
