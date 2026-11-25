# SteamAPI compatibility (offline)

## Status and scope

`source/win32_steam.c` / `source/win32_steam.h` are an **integrated, offline**
compatibility module. They are wired into the shared dispatcher, Makefile and
host test lists. The integration was validated in a separate scratch tree before
landing at a growth stage boundary.

There is no Steam client, Steam service, IPC, socket, or network code in this
module. It does not execute unlifted code from `steam_api.dll`, create a fake
Steam interface, report a successful initialization, or synthesize events.

## How TESV actually calls it

Static import analysis of `TESV.exe` (image base `0x400000`):

- `steam_api.dll` is a **static** import; the data directory 13 (delay import)
  is empty, so there is no runtime `LoadLibrary`/`GetProcAddress` path for it.
- `SteamAPI_Init` is called at `0x00a5bd33`, inside the wrapper at
  `0x00a5bd30` (`ret 0x14`). The wrapper stores the `bool` result at
  `this+0xb8` and returns without branching on it.
- `0x00a5bd60` is the only immediate consumer: it tests `[ecx+0xb8]` and,
  when zero, simply returns; when nonzero it jumps to
  `steam_api.dll!SteamAPI_RunCallbacks`.
- The wrapper is virtual (slot at vtable `0x110ded8+8`), so callers must be
  resolved dynamically; no direct `call` sites exist.
- `TESV.exe` contains no "Steam is required"/"Steam must be running" message
  string. A failed `SteamAPI_Init` therefore looks non-fatal in the binary:
  it disables callback dispatch instead of aborting startup.

This is binary evidence, not yet an executed result. Whether the offline
`false` contract is sufficient for the single-player menu path will be verified
when growth reaches the real `WinMain`/renderer code.

Evidence used for the ABI and callback layout (re-checked against the local
`steam_api.dll`, image base `0x3b400000`, plus an import-table scan of the
unprotected `TESV.exe`):

- The `SteamAPI_RegisterCallback` export entry jumps to a body that reads
  `[ebp+8]`/`[ebp+0xc]` (that is `[esp+4]`/`[esp+8]` on entry), executes
  `or byte [edx+4],1` and then `mov [edx+8],ecx`, and ends in a plain `ret`.
  So the callback comes from `[esp+4]`, the id from `[esp+8]`, bit 0 at
  `callback+4` is set before the id is stored at `callback+8`, and the caller
  removes both arguments. The shim reproduces exactly this order and cleanup.
- `SteamAPI_UnregisterCallback` reads its single `[ebp+8]` argument, clears
  that bit and unregisters, then ends in a plain `ret` (cdecl as well).
- `SteamAPI_Init`, `SteamAPI_RunCallbacks` and `SteamAPI_Shutdown` take no
  arguments and end in a plain `ret`; `SteamAPI_Init` is a wrapper shaped
  `push 0 / call / add esp,4 / ret`, returning its boolean in `EAX`.
- The TESV import table contains exactly five Steam imports —
  `SteamAPI_Init`, `SteamAPI_RunCallbacks`, `SteamAPI_RegisterCallback`,
  `SteamUserStats`, `SteamAPI_UnregisterCallback` — all served by this module.
  `SteamAPI_Shutdown` and the service/interface accessors are included because
  they are known `steam_api.dll` exports, not because TESV calls them.

## Thunk window

```c
#define WIN32_STEAM_BASE (WIN32_THUNK_BASE + 0x00080000u) /* 0xff080000 */
```

Thunks occupy `WIN32_STEAM_BASE + 4*i`. The dedicated 64 KiB window is
`[0xff080000, 0xff090000)`. Current windows verified before choosing it:

| Window | Base |
|---|---:|
| core `win32_shims` | `0xff000000` |
| sync | `0xff010000` |
| user | `0xff020000` |
| locale | `0xff030000` |
| shell | `0xff040000` |
| audio | `0xff050000` |
| input | `0xff060000` |
| thread | `0xff070000` |
| **Steam** | **`0xff080000`** |

No existing `WIN32_*_BASE` uses `0xff080000`. The smoke test includes all
existing module headers and checks every base for equality with
`WIN32_STEAM_BASE`; it also checks lookup boundaries and unaligned addresses.

## Guest-visible behavior

### Lifecycle

| Export | Result | Diagnostic / effect |
|---|---|---|
| `SteamAPI_Init` | `false` (`EAX=0`) | `[steam] offline: no Steam client (SteamAPI_Init failed)` |
| `SteamAPI_InitSafe` | `false` (`EAX=0`) | Same named failure for `SteamAPI_InitSafe` |
| `SteamAPI_IsSteamRunning` | `false` (`EAX=0`) | Named offline diagnostic |
| `SteamAPI_RestartAppIfNecessary` | `false` (`EAX=0`) | Named offline diagnostic |
| `SteamAPI_RunCallbacks` | void | Always logs `dispatched 0 events (no Steam client)` |
| `SteamAPI_Shutdown` | void | Clears the host registry and logs the number cleared |

`RunCallbacks` has no queue and no event source. It never invokes a callback,
even when the registry is nonempty.

### Callback registry

The host registry is a fixed array of **32 entries**, keyed by the guest
callback pointer. Each entry also retains the callback id. No host allocation is
performed per registration.

For a valid registration:

1. Reject an unbound module (no guest memory to read arguments from).
2. Read both cdecl arguments through validated guest memory.
3. Reject NULL, duplicates, a full registry, or an invalid callback object.
4. Validate the complete 12-byte callback object before any write.
5. Preserve every byte except bit 0 at `callback+4`, which is set.
6. Write the 32-bit id at `callback+8`.
7. Add the pointer/id pair to the bounded registry.

Explicit refusal rules:

- **NULL:** ignored with `ignored null callback`; no registry or object change.
- **Duplicate pointer:** rejected with `rejected duplicate callback`; the
  original id and object bytes are not rewritten.
- **Full registry:** rejected with `registry full (32)`; no object write.
- **Unmapped/incomplete object:** rejected with `invalid callback memory`; no
  partial flags/id write is made.
- **Unregister NULL or unknown pointer:** ignored with a named diagnostic; no
  object write.
- **Unregister with invalid guest memory:** the registry entry is retained,
  because the guest object could not be updated honestly.
- **Successful unregister:** clears only bit 0 at `callback+4`; the id and all
  other bytes remain unchanged.

When no module is bound (before `win32_steam_init()`, or after
`win32_steam_shutdown()`), `RegisterCallback` and `UnregisterCallback` still
remove only the return address and log `module is not bound to guest memory`;
they read and write no guest memory and create no registry entry.
`SteamAPI_Init` still reports the offline failure, because it needs no guest
memory.

`SteamAPI_Shutdown` clears the host registry even if a registered callback's
guest memory has since disappeared. It deliberately does **not** rewrite guest
callback objects. `win32_steam_shutdown()` additionally drops the guest-memory
binding and clears all host state.

### Known service/interface imports

These names resolve to offline thunks and return zero/NULL in `EAX`, each with
`[steam] offline: <name> unavailable (no Steam client)`:

- Handles/path: `SteamAPI_GetHSteamPipe`, `SteamAPI_GetHSteamUser`,
  `SteamAPI_GetSteamInstallPath`, `Steam_GetHSteamUserCurrent`.
- Interfaces: `SteamApps`, `SteamClient`, `SteamController`, `SteamFriends`,
  `SteamGameServer`, `SteamGameServerApps`, `SteamGameServerHTTP`,
  `SteamGameServerNetworking`, `SteamGameServerStats`, `SteamGameServerUtils`,
  `SteamHTMLSurface`, `SteamHTTP`, `SteamMatchmaking`,
  `SteamMatchmakingServers`, `SteamMusic`, `SteamMusicRemote`,
  `SteamNetworking`, `SteamRemoteStorage`, `SteamScreenshots`, `SteamUGC`,
  `SteamUnifiedMessages`, `SteamUser`, `SteamUserStats`, `SteamUtils`.

In particular, `SteamUserStats` returns NULL; no vtable or statistics object is
invented. Unknown Steam names are not silently mapped to success: they return
zero from `win32_steam_resolve()` and continue through the normal unresolved
import/trap policy.

## cdecl stack contract

Every Steam thunk removes **only the 4-byte return address**:

```c
c->r[4] += 4;
```

Arguments remain at the caller's old `[esp+4...]` slots. The caller performs
its own `add esp, args*4` cleanup after the call. This matches the observed
`ret` without an immediate and is intentionally different from the shared
stdcall-style `result(c, value, slots)` helper.

| Export | Arguments | `EAX` after call | Callee ESP delta |
|---|---:|---|---:|
| `SteamAPI_Init` / `InitSafe` | 0 | `0` | `+4` |
| `SteamAPI_RunCallbacks` | 0 | unchanged | `+4` |
| `SteamAPI_RegisterCallback` | 2 | unchanged | `+4` |
| `SteamAPI_UnregisterCallback` | 1 | unchanged | `+4` |
| `SteamAPI_Shutdown` | 0 | unchanged | `+4` |
| Known offline accessors | 0 | `0` | `+4` |

The host smoke test checks the return-address marker and every argument slot
after each thunk, then models the caller's cleanup.

## Host test

The test is standalone; it links only `source/guest.c` and
`source/win32_steam.c`. From a Visual Studio developer command prompt with
LLVM's `clang` on `PATH`:

```bat
if not exist build_host mkdir build_host
clang -std=gnu11 -O1 -g -Istaged -Isource -Wall -Wextra ^
  source/guest.c source/win32_steam.c tests/win32_steam_smoke.c ^
  -o build_host/win32_steam_smoke.exe
build_host\win32_steam_smoke.exe
```

Expected output, and the result recorded on 2026-11-24 with the Visual Studio
18 x64 environment plus LLVM clang:

```text
PASS: win32_steam smoke
```

The same invocation with `-O2 -fsanitize=undefined -fno-sanitize-recover=all`
also printed `PASS`, and `python tools/check_shim_bases.py` printed
`shim bases: all distinct` — that checker globs `source/win32_*.h`, so it
already covers `WIN32_STEAM_BASE` without a build-list change. The module also
cross-compiles for the device with the Makefile's ARM flags:

```powershell
arm-none-eabi-gcc -g -O2 -Wall -Wextra -std=gnu11 -mword-relocations `
  -ffunction-sections -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft `
  -Istaged -Isource -D__3DS__ -c source/win32_steam.c -o build_host/win32_steam_arm.o
```

(exit 0, no diagnostics from this file; `staged/x86.h` produces the pre-existing
`-Wmisleading-indentation` notes that the Makefile suppresses).

Coverage includes:

- exact base value and collision checks against every existing shim window;
- definition-table uniqueness, resolve/lookup, and range boundaries (including
  `steam_api` without the `.dll` suffix, and NULL arguments);
- binding contract: NULL and double-bind are refused, and a diagnostics sink
  installed before `win32_steam_init()` is preserved;
- ESP `+4` behavior and untouched caller arguments for every entry;
- callback bit/id writes and preservation of all other bytes;
- NULL, duplicate, unknown-pointer, partial-object, and invalid-stack refusals;
- unbound-module refusals for `RegisterCallback`/`UnregisterCallback`;
- bounded 32-entry registry, overflow refusal, and slot reuse;
- unregister after guest memory is removed;
- offline `Init`/`RunCallbacks`, zero fabricated events, and registry-clearing
  `Shutdown`;
- named zero/NULL diagnostics for every known offline accessor.

Diagnostics default to `stderr`. `win32_steam_set_log_stream()` is a host-only
test/front-end hook; passing NULL restores `stderr`. A stream installed before
`win32_steam_init()` survives binding, while `win32_steam_shutdown()` drops it
together with the memory binding (the owner normally closes it next, and a
retained closed `FILE*` would be worse than returning to `stderr`). It is not a
guest import.

## Exact later wiring (not applied)

Apply these changes only after Growth21 releases the shared files.

### 1. `source/win32_shims.c`

Add the include next to the other module headers:

```diff
 #include "win32_input.h"
+#include "win32_steam.h"
```

Bind the module in `win32_init()` after `win32_input_init()` succeeds. It owns
no guest mapping, so failure only means NULL or an already-bound module:

```diff
     if(win32_input_init(m)){win32_thread_shutdown();win32_user_shutdown();mem=NULL;return -1;}
+    if(win32_steam_init(m)){
+        win32_input_shutdown();
+        win32_thread_shutdown();
+        win32_user_shutdown();
+        mem=NULL;
+        return -1;
+    }
     win32_shell_init(m);
```

Release it in `win32_shutdown()`:

```diff
 void win32_shutdown(void) {
     if(!mem)return;
+    win32_steam_shutdown();
     win32_input_shutdown();
```

Chain address lookup in `win32_lookup()` before the core/trap window fallback:

```diff
     const FnEntry *aud=win32_audio_lookup(addr);if(aud)return aud;
+    const FnEntry *steam=win32_steam_lookup(addr);if(steam)return steam;
     unsigned i=(addr-WIN32_THUNK_BASE)/4;
```

Chain named resolution in `win32_resolve()` before the external resolver and
loaded-PE fallback. This precedence is what prevents known Steam imports from
executing unlifted `steam_api.dll` code:

```diff
         uint32_t aud=win32_audio_resolve(e->dll,e->name);if(aud)return aud;
+        uint32_t steam=win32_steam_resolve(e->dll,e->name);if(steam)return steam;
     } else {
```

No `n3ds_boot.c` change is needed for lifecycle because `win32_init()` and
`win32_shutdown()` own the binding after this wiring.

### 2. `Makefile`

Add `win32_steam.o` to both existing lists that contain `win32_input.o`:

```diff
-export OFILES += n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_thread.o win32_input.o
+export OFILES += n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_thread.o win32_input.o win32_steam.o
```

```diff
-export OFILES := n3ds_boot.o tesv_slice_main.o n3ds_pilot.o guest.o guest_heap.o pe_image.o pe_imports.o guest_process.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_audio_3ds.o win32_thread.o win32_input.o d3d9_core.o d3d9_texcache.o d3d9_texture_data.o d3d9_bc.o d3d9_vertex.o d3d9_adapter.o d3d9_shader.o d3d9_tev.o d3d9_pica_tev.o d3d9_pica_citro3d.o d3d9_menu_light.o d3d9_com.o d3d9_null.o d3d9_citro3d.o d9shader.shbin.o $(patsubst %.c,%.o,$(notdir $(wildcard $(TESV_GEN)/tesv_*.c)))
+export OFILES := n3ds_boot.o tesv_slice_main.o n3ds_pilot.o guest.o guest_heap.o pe_image.o pe_imports.o guest_process.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_audio_3ds.o win32_thread.o win32_input.o win32_steam.o d3d9_core.o d3d9_texcache.o d3d9_texture_data.o d3d9_bc.o d3d9_vertex.o d3d9_adapter.o d3d9_shader.o d3d9_tev.o d3d9_pica_tev.o d3d9_pica_citro3d.o d3d9_menu_light.o d3d9_com.o d3d9_null.o d3d9_citro3d.o d9shader.shbin.o $(patsubst %.c,%.o,$(notdir $(wildcard $(TESV_GEN)/tesv_*.c)))
```

### 3. `tools/run_host_tests.ps1`

Add the module to `$legacySources` so the existing lift/boot/integration tests
link it:

```diff
-$legacySources = @("source/guest.c", "source/guest_heap.c", "source/pe_image.c", "source/pe_imports.c", "source/win32_shims.c", "source/win32_sync.c", "source/win32_user.c", "source/win32_locale.c", "source/win32_shell.c", "source/win32_audio.c", "source/win32_audio_3ds.c", "source/win32_thread.c", "source/win32_input.c", "source/n3ds_lift_runtime.c", "source/ir_core.c", "source/ir_decode.c", "source/ir_exec.c", "source/rt_prof.c", "staged/fpu.c")
+$legacySources = @("source/guest.c", "source/guest_heap.c", "source/pe_image.c", "source/pe_imports.c", "source/win32_shims.c", "source/win32_sync.c", "source/win32_user.c", "source/win32_locale.c", "source/win32_shell.c", "source/win32_audio.c", "source/win32_audio_3ds.c", "source/win32_thread.c", "source/win32_input.c", "source/win32_steam.c", "source/n3ds_lift_runtime.c", "source/ir_core.c", "source/ir_decode.c", "source/ir_exec.c", "source/rt_prof.c", "staged/fpu.c")
```

Register the standalone smoke test next to the other module tests:

```diff
 Add-Test "audio" $liftCommon @("source/guest.c", "source/win32_audio.c", "tests/audio_smoke.c") @()
+Add-Test "steam" $liftCommon @("source/guest.c", "source/win32_steam.c", "tests/win32_steam_smoke.c") @()
```

After wiring, run at minimum:

```powershell
.\tools\run_host_tests.ps1 -Only steam,lift,boot
```

### 4. Validation of this diff (scratch copy only)

On 2026-11-24 each of the five `win32_shims.c` anchors above matched exactly one
location in the current file, so the snippets are still current. A scratch copy
with all five edits plus `source/win32_steam.c` was compiled with the project's
`$liftCommon` flags and both wired smoke binaries exited 0:

- `lift` source set + `tests/lift_smoke.c` + `tests/win32_smoke.c` →
  `Lift ABI smoke test passed`;
- `boot` source set (the same list plus `source/guest_process.c` and
  `source/n3ds_boot.c`) + `tests/boot_smoke.c` + `tests/win32_smoke.c` →
  `PEB/TEB/TLS, IAT, DLL attach, entry and ExitProcess passed`.

The repository's shared files were not modified; this only proves the wiring
above builds and keeps the existing host smokes green.

Until then, the standalone test can be built directly with the command above.
