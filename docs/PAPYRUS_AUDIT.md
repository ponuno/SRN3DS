# Papyrus VM audit: what the script layer needs from the Win32 shims

Scope: `E:\skyr\TESV.exe` (retail 1.9.32.0, SHA-256
`d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`, image base
`0x400000`, 27.89 MiB of PE images). Static, read-only analysis with
`tools/audit_game.py` and two ad-hoc scanners (string/RTTI census and IAT
cross-reference). No game bytes are copied into this tree.

Honesty notes up front:

* Static imports only. "Import present" does not prove the Papyrus code path
  executes it, and "shim present" does not prove the shim is correct.
* The VM lives inside `TESV.exe`; there is no separate Papyrus DLL to audit.
  Class names below come from MSVC RTTI strings, code anchors from `push imm32`
  references to VM-specific strings.
* No frame, no device, no gameplay claim. `DevCaps` untouched.

## 1. Where the VM is (evidence)

RTTI/class cluster (`rdata`, `0x12b8xxx-0x12bdxxx`):

| Family | Classes seen |
| --- | --- |
| VM core | `BSScript::VirtualMachine` (implied by `SkyrimVM`), `BSScript::Object`, `ObjectTypeInfo`, `Stack`, `StackFrame`, `IFunctionArguments`, `ZeroFunctionArguments`, `IFunction`, `NativeFunction*`, `NativeLatentFunction0..10<...>`, `IStackCallbackFunctor`, `IObjectHandlePolicy`, `IStore`, `ILoader`, `ISavePatcherInterface`, `ErrorLogger`, `IProfilePolicy`, `IForEachScriptObjectFunctor` |
| Game binding | `SkyrimVM`, `SkyrimVM::EventRelayFunctor`, `EffectEventRelayFunctor`, `SleepStartEventFunctor`, `SleepStopEventFunctor`, `InventoryEventFilter`, `ISendEventFilter`, `TESObjectREFR`, `TESForm`, `ActiveMagicEffect`, `PapyrusPersistenceForm`, `BSTEventSink<TESInitScriptEvent>` |

Strings that pin the subsystem (VA, not file offset):

| VA | String | Meaning |
| --- | --- | --- |
| `0x10ea814` | `.pex` | compiled script extension, loader anchor |
| `0x10ebb68` | `SkyrimVM save file version number is too new. Expected %u or lower, found %u` | VM save/co-save writer |
| `0x114b3a8` | `error: Unable to call member function OnInit on object %s due to stack creation failure or queue full` | event dispatch entry |
| `0x1101fa8` | `Error: CallFunction - attempt to call invalid function` | `CallFunction`/`CallMethod` path |
| `0x114960c` | `CallMethod` | native call dispatcher |
| `0x1095558` | `--- Papyrus ---------------------------------` | log/profiler dump |
| `0x1097220` | `Papyrus profiling is disabled - profile request ignored` | profiler gate |
| `0x10954dc` | `No papyrus scripts attached` | `TESObjectREFR` script attach |
| `0x109863c` | `GetVMScriptVariable` | console/native inspection |
| `0x10a0964`, `0x10a0a4c`, `0x114c284` | `OnLoad`, `OnEquip`, `OnInit` | lifecycle events |
| `0x10eb870`, `0x10eb884` | `OnUpdateGameTime`, `OnUpdate` | timer-driven events |
| `0x10ec560`, `0x10ec57c`, `0x10ec65c`, `0x10ec678`, `0x10ec6c0`, `0x10ec6e0` | `UnregisterForUpdateGameTime`, `UnregisterForUpdate`, `RegisterForUpdateGameTime`, `RegisterForUpdate`, `RegisterForSingleUpdateGameTime`, `RegisterForSingleUpdate` | the whole script-timer surface |
| `0x1091af0` | `fQuestScriptDelayTime:MAIN` | quest script pacing INI knob |

Call chain (as the imports and strings allow us to state it):

```
main loop tick
  -> SkyrimVM::Update            (per-frame budget, QPC/GetTickCount deltas)
     -> process queued calls      (BSTaskPool worker threads)
     -> run RegisterForUpdate timers whose game-time deadline elapsed
     -> raise OnUpdate / OnUpdateGameTime / animation, equip, sleep events
  -> event source (BSTEventSource<...>) -> SkyrimVM::EventRelayFunctor
     -> stack frame -> native function (Actor/TESObjectREFR/...)
  -> .pex load: Data\Scripts\*.pex through the BSA/file layer, then
     CompiledScriptLoader / ILoader -> ObjectTypeInfo cache
```

The timer part is the important negative result: **the VM has no OS timer of
its own.** `TESV.exe` imports no `CreateWaitableTimer*`, `SetWaitableTimer`,
`CreateTimerQueue*`, `timeSetEvent` or `SetTimer`. `RegisterForUpdate` and
`OnUpdate` are driven by the frame/game-time clock, so the only clock services
the VM can touch are the ones listed in §2.

## 2. External API surface and status

Status legend: **shim** = implemented in `source/win32_*.c`; **new** = added by
this audit; **missing** = imported by `TESV.exe` and still an unresolved trap.

### 2.1 Time / timers (all imported, all shimmed)

| API | Used for | Criticality | Status |
| --- | --- | --- | --- |
| `GetTickCount` | frame budget, `RegisterForUpdate` deltas | high | shim (`vtime_ms`) |
| `QueryPerformanceCounter` / `QueryPerformanceFrequency` | high-resolution profiling, `fUpdateBudgetMS` accounting | high | shim (`vtime_ticks`, `freq`) |
| `timeGetTime` | legacy millisecond clock | medium | shim (`vtime_ms`) |
| `timeBeginPeriod` / `timeEndPeriod` | request 1 ms timer resolution | low | shim, **no-op** (see §3) |
| `Sleep` / `SleepEx` | worker-thread pacing, tasklet back-off | high | shim, scheduling point |
| `GetSystemTimeAsFileTime` | save/log timestamps | medium | shim (host `time()`) |
| `GetLocalTime`, `SystemTimeToFileTime`, `FileTimeToSystemTime`, `CompareFileTime` | save file metadata | medium | shim |

### 2.2 Threads, sync, memory (all shimmed)

`CreateThread`, `ExitThread`, `ResumeThread`, `SuspendThread`,
`SetThreadPriority`, `SetThreadAffinityMask`, `SetThreadIdealProcessor`,
`GetCurrentThread(Id)`, `Sleep`, `SleepEx`, `WaitForSingleObject`,
`WaitForMultipleObjects`, `CreateEventA`, `SetEvent`, `ResetEvent`,
`CreateMutexA`, `ReleaseMutex`, `CreateSemaphoreA`, `ReleaseSemaphore`,
`Enter/Leave/Initialize/DeleteCriticalSection`, `Interlocked*`,
`GetProcessHeap`, `HeapCreate/Alloc/ReAlloc/Free/Size`, `VirtualAlloc/Free`.
No gap here. `operator new/delete` is *not* in this group (see §4).

### 2.3 File I/O / registry / CRT

`.pex` loading uses the engine's BSA/file layer: `CreateFileA`, `ReadFile`,
`GetFileSizeEx`, `SetFilePointerEx`, `CloseHandle`, `FindFirstFileA` — all
shimmed, plus the read-only Data BSA overlay and INI overlay from recent
commits. `RegOpenKeyA` (imported once) is still missing; the rest of the
registry surface goes through `win32_registry.c`.

CRT: `malloc/calloc/realloc/free`, `mem*`, the narrow `str*` core, `qsort`,
`strtol/strtoul`, `atoi/atol`, `setlocale`, `localeconv`, `_setjmp3`,
`longjmp` were already present. The Papyrus-relevant holes were numeric
conversion, classification, randomness, `_errno`, and every floating-point
math entry point — the VM's `Math.*` natives and its arg parser hit these.
Those are now implemented (§3).

## 3. What this audit implemented

`source/win32_shims.c`, new `C()` entries (41 resolver entries, of which 38
were previously unresolved imports; the other three — `_ltoa`, `sqrt`, `pow` —
are aliases the table should have had anyway):

* **Conversions/numbers**: `strtod`, `atof` (sign, fraction, exponent,
  `inf`/`infinity`/`nan`, `endptr`, no hex floats — matching the engine's 2011
  msvcrt), `_itoa`, `_ltoa`, `_ultoa` (radix 2-36, `INT_MIN`, buffer pointer
  return), `_errno` (lazily allocated guest int, stable address).
* **Random**: `rand`, `srand` with the msvcrt LCG (`s*214013+2531011`,
  `(s>>16)&0x7fff`; `srand(1)` → 41, covered by the test).
* **Classification**: `isalnum`, `isalpha`, `isdigit`, `isspace`, `isxdigit`,
  `isleadbyte` (0: CP1252 has no lead bytes), `iswalpha`, `iswdigit`,
  `iswspace`, `iswpunct`. ASCII-only by design; a non-ASCII byte is never
  reported as a letter.
* **Locking**: `_lock`, `_unlock` as explicit no-ops with a comment explaining
  that only one guest thread runs at a time, so stdio lock ids cannot be
  contended.
* **Math (x87 ST0 result)**: `ceil`, `floor`, `sqrt`, `fmod`, `pow`, `frexp`,
  `ldexp`, `modf`, `_finite`, `_isnan`, and the compiler helpers `_CIacos`,
  `_CIasin`, `_CIatan`, `_CIatan2`, `_CIcos`, `_CIexp`, `_CIfmod`, `_CIlog`,
  `_CIpow`, `_CIsin`, `_CIsqrt` (float argument, double result). `sqrt(<0)`,
  `fmod(x,0)` and `pow(negative, non-integer)` set `EDOM` and return NaN like
  msvcrt. Double arguments are addressed by **byte offset**, because a `double`
  occupies two 4-byte cdecl slots — this ABI detail is the one that broke the
  first test iteration and is now pinned by assertions.

Measured effect (`tools/audit_game.py` on the same image):

| | before | after |
| --- | --- | --- |
| imports resolved by shims | 486 | **524** |
| unresolved import names | 154 | **116** (109 symbols) |

Tests (host, `tools/run_host_tests.ps1`):

* new `win32_papyrus_crt` (`tests/win32_papyrus_crt_smoke.c`): `strtod`
  sign/exponent/endptr, no-conversion endptr, `inf`, `atof`, `0x` behaviour,
  `_itoa`/`_ultoa`/bad radix, `srand`/`rand` vector, `_errno` stability,
  classification (including `0xE9`), `ceil/floor/sqrt/fmod/modf` through ST0,
  `_finite`/`_isnan`, `_CIsin`/`_CIsqrt`/`_CIpow`, and resolver presence for
  all 41 names. **PASS**
* `win32_crt`: still **PASS**.
* `host`, `lift`: still **PASS**.
* Pre-existing failures unrelated to this change: `tests/slice_pe_test.py`,
  `texture_pipeline_test.py`, `ir_container_test.py`,
  `ir_container_growth_test.py` — they look for `C:\Program Files\LLVM\bin\clang.exe`
  (and `capstone`), which this host does not have.

## 4. Top-10 remaining gaps

Ranked by impact on a script-enabled run, not by reference count.

1. **`operator new`/`operator delete` (`??2@YAPAXI@Z`, `??3@YAXPAX@Z`)** —
   imported, unresolved; `BSScript` is C++ and allocates objects. Patch: map to
   `heap_alloc`/`heap_free`, return NULL on failure and let the caller's
   `_purecall`/null-check path run, **or** implement MSVC's throwing form and
   document that exceptions are not modelled. Needs a decision before code.
2. **`_snprintf` / `_vsnprintf` / `_vsnwprintf`** — the VM profiler and the
   Papyrus log format strings through these. Requires a guest-`va_list`
   formatter: 32-bit cdecl `va_list` is a plain pointer, so walk 4-byte slots,
   align `double`/`__int64` to 8, and copy `%s` from guest memory. Must not hand
   guest format strings to host `vsnprintf` (host would dereference guest
   addresses). Sketch is straightforward; not low-risk enough for this pass.
3. **`sscanf`** — console/script argument parsing (`Script command "%s"`).
   Same guest-varargs machinery as (2), plus `%n`/width handling.
4. **`_initterm` / `_amsg_exit` / `_XcptFilter` / `__CxxFrameHandler`** — CRT
   startup/SEH machinery. Currently explicit traps, which is honest: wiring
   them up without a real SEH model would silently mis-handle access
   violations. Keep the trap until an SEH story exists.
5. **`_onexit` / `__dllonexit`** — static destructors. Low value before
   shutdown works; implement together with (4).
6. **`fread`/`fwrite`/`fflush`/`_fileno`/`_write`/`_iob`/`_lseeki64`** — stdio
   FILE\* surface. The engine's own file path is Win32 handles, so this is
   mostly third-party/tooling code; a `_iob` table mapping to host FILE\* would
   be a large, low-value layer. Skip unless a concrete caller appears.
7. **`wctomb`/`wcstombs`** — narrow/wide conversion used by localization
   paths. Needs a code-page decision (CP1252 vs UTF-8); do it with the locale
   work, not ad hoc.
8. **`_strtime`/`_strdate`** — crash/log timestamps. Trivial and safe; left out
   only because the exact msvcrt `"HH:MM:SS"` formatting was not needed yet.
9. **`RegOpenKeyA`** — one import, registry shim already owns the rest; add for
   completeness.
10. **`_controlfp`/`_clearfp`/`_adjust_fdiv`/`__pioinfo`/`__badioinfo`** — x87
    control word and CRT internals. `_controlfp` should eventually map to
    `fpu_cw` so a script that changes rounding does not silently diverge;
    `_adjust_fdiv` is a no-op on any modern CRT.

Not needed (verified absent from the import table): waitable timers, timer
queues, `timeSetEvent`, `SetTimer`, `GetTickCount64`. Do not build a waitable
timer layer for the VM — it would be dead code with a misleading success path.

## 5. Timer semantics review (precision, monotonicity, cooperative model)

* **Source.** `tick()` = `QueryPerformanceCounter` (host),
  `svcGetSystemTick` (3DS), `CLOCK_MONOTONIC` (other). All monotonic; no
  wall-clock jumps. `freq()` reports `SYSCLOCK_ARM11` on 3DS, host QPF
  otherwise. `GetTickCount`/`timeGetTime` = `floor(ticks*1000/freq)`, so
  sub-millisecond truncation, and they wrap at 2^32 ms exactly like Windows —
  which is what the engine expects, since `GetTickCount64` is not imported.
* **Epoch.** The guest clock starts at process start, not at host boot. Delta
  users (the VM budget and `RegisterForUpdate`) are unaffected; a hypothetical
  absolute-time comparison against boot uptime would see a smaller value.
  `TESV_TIME_SCALE`/`TESV_TIME_OFFSET_MS` remain measurement-only and are
  logged when active.
* **Minor defect found.** `vtime_ticks()` computes `base = start_tick ?
  start_tick : tick()` and then `return base + (tick() - start_tick)`. If the
  first tick is exactly 0, this returns `2*tick`. `win32_init()` sets
  `start_tick`, so the branch is practically unreachable, but the expression
  should be `return tick() + offset` or `base = start_tick` unconditionally.
  Left as-is (behaviour-neutral on both hosts) and recorded here rather than
  silently "fixed" without a test.
* **Sleep precision.** `Sleep(ms)` blocks the guest thread on a deadline and
  idles the host in chunks of at most 50 ms, re-checking the scheduler between
  chunks, so a wake is never later than `50 ms + host granularity`; other
  guest threads run while one sleeps, and `Sleep(0)` is an explicit yield.
  On Windows the host granularity is the default ~15.6 ms unless someone calls
  `timeBeginPeriod(1)`. Our `timeBeginPeriod` is a no-op that returns success,
  so a guest `Sleep(1)` can oversleep by up to one host tick. Patch sketch:
  call the real `timeBeginPeriod`/`timeEndPeriod` from `winmm` on `_WIN32`
  (refcounted, matching begin/end pairs), keep the no-op on 3DS where
  `svcSleepThread` already takes nanoseconds. This needs a link flag, hence
  not done here.
* **Monotonicity across `Sleep`.** Time only advances while a shim runs, and a
  sleeping guest advances the shared monotonic clock by the whole sleep, so a
  second guest thread observes a jump — identical to a real preemptive host.
* **No APCs.** `SleepEx(INFINITE)` and `Sleep(INFINITE)` fault explicitly
  instead of hanging; `SleepEx` returns 0 (no APC queued), which is the honest
  result because nothing can queue one.

## 6. Reproduction

```
python tools/audit_game.py E:\skyr --output <tmp>\tesv_audit.json
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 -Only win32_crt,win32_papyrus_crt,lift,host
```

`staged/` (x86.h, rt_table.h, fpu.c, lift.py, ...) is gitignored and was copied
from the main tree for the host build; nothing in `staged/` is committed.
