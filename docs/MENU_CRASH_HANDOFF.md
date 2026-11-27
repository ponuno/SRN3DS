# Menu crash handoff (deterministic `0x60aa70` fault)

Worktree: `C:\Users\vasya\Documents\Skyrim_N3DS_wt\menu-crash`
Branch: `menu-crash` (based on `cbe2f9b`; do not merge/rewrite)
This note is self-contained: it records what was measured, what was changed, what
was ruled out, and what was not reached. No frame, `Draw*` or `StartMenu` was
reached.

## Symptom (deterministic)

A host slice run of TESV stops with:

```
[guest] unmapped addr=62783049 bytes=4 last_call=0060aa70 irc_fn=0060aa70 irc_block=0060aa70
[guest] regs eax=012b6634 ebx=00000000 ecx=62783041 edx=64556898 esi=00000000 edi=425ceb50 ebp=554f7e90 ...
Guest fault at 62783049: unmapped guest memory
[thread] id=13 start=00a4b4a0 terminated without returning
```

`ecx=0x62783041` is the faulting `this`: the faulting block is `0x60aa70` and its
first instruction is `mov eax,[ecx+8]`, so the reported address is `ecx+8`.
`edx=0x64556898` is a heap pointer (a shader/stream heap), `esi=0`.

The old stack scan reports:

```
0x699350 -> 0x44376d -> 0x43bdad -> 0x43f17f -> 0x56905c -> CRT/SEH
```

The live saved-this value in the crashing frame is `0x554f7e40`, i.e. the
`ecx`/`ebp` chain is on guest thread 13 (`esp=0x1d61fe3c`, stack top
`0x1d620000`). The relevant current frames are:

```
0x699960 TESInitThread
  0x699100 (call at 0x69934b -> return 0x699350)
    0x4434a0 (call at 0x443776 -> return 0x44377b)
      0x43efe0
        0x43f162: call 0x432610            ; -> NavMeshInfoMap* ([0x1b2e864+0xe0])
        0x43f176: edx = [eax]              ; vtable
        0x43f17a: eax = [edx + 0x4c]       ; vtable slot 19
        0x43f17d: call eax                  ; -> 0x5fc640
          0x5fc640 -> 0x60aa70 (call at 0x5fc863)
```

`0x43bdad`/`0x56905c` in the scan are stale stack words, not the caller chain.
The immediately preceding `BSMemory` block group (`0xa48d60..0xa48f11`) is
thread 13 allocating/freeing, not the direct caller of `0x60aa70`.

## 0x60aa70 semantics

`0x60aa70` is a small getter on a 16-byte object (`ecx` = object):

```
0060aa70: mov eax,[ecx+8]     ; primary char*
0060aa73: test eax,eax
0060aa75: je 0x60aa7a
0060aa77: push eax
0060aa78: jmp 0x60aa7e
0060aa7a: mov ecx,[ecx+0xc]   ; fallback char* when +0x08 is NULL
0060aa7d: push ecx
0060aa7e: mov ecx,[0x1310460] ; registry/manager
0060aa84: call 0x604750
0060aa89: test eax,eax
0060aa8b: je 0x60aa9c
0060aa8d: mov cl,[eax+0x12]   ; registry entry tag
0060aa90: cmp cl,0x3c         ; '<'
0060aa93: je 0x60aa9c
0060aa95: cmp cl,0x47         ; 'G'
0060aa98: je 0x60aa9c
0060aa9a: xor eax,eax
0060aa9c: ret
```

So the faulting object is `[entry+0x28]`; that object is expected to be a
16-byte record with a primary name string at `+0x08` and a fallback at `+0x0c`.
The registry lookup is `0x604750` (which reaches `0x451a30`); entries carry a
byte tag at `+0x12`; accepted tags here are `0x3c` (`<`) and `0x47` (`G`).
`0x60a988` is the write side of the same 16-byte record family (named in the
task context; not re-measured in this session).

## Root evidence for the garbage `ecx`

The bad pointer is not random: the crashing call is in `0x5fc640`, which treats
the `+0x50` container of the NavMeshInfoMap object as a hash map of 12-byte
buckets `{key, value, next}`:

```
5fc859: mov edx,[esp+0x74]   ; temporary pointer vector built by the first loop
5fc85d: mov eax,[edx+esi*4]
5fc860: mov ecx,[eax+0x28]   ; eax = 0x012b6634, ecx = 0x62783041
5fc863: call 0x60aa70
```

`0x012b6634` is a static `.data` address, not a guest allocation. It sits four
bytes after two `0xdeadbeef` dwords; the MSVC `TypeDescriptor` for
`.?AVFindExactObjectFunctor@?A0xb7a685a0@SkyrimScript@@` begins at `0x12b6638`,
its name string at `0x12b6640`. Consequently:

```
[0x12b6634+0x28] = [0x12b665c] = 0x62783041  ; bytes "A0xb" inside that name
```

`0x62783041` is therefore fully explained by reading a 4-byte string fragment as
an object pointer. It is not a TEB/TLS value and not a plausible `NavMeshInfo*`.

At fault the map object (`this=0x554f7e40`) is intact:

```
[+00]=010c59dc  [+04]=56c7cca0  [+08]=00000001
[+54]=00004000  [+58]=0000039a  [+60]=01269b1c
[+68]=4259eb50
```

`0x10c59dc` is the NavMeshInfoMap vtable; `+0x54` looks like the hash capacity
(0x4000), `+0x58` the entry count (922), `+0x68` the bucket-array pointer. The
bucket array at `0x4259eb50` at dump time starts with
`00100a27 55805430 01269b1c 00085ee2`, so the bad value is not the first bucket
after the intervening rehash; it was collected by the first loop into the
temporary vector at `[esp+0x74]` and survived there.

## Mapping of the container and the loader

* `map+0x50` is a `BSTHashMap`-style container; `0x8bf970` is its rehash and
  `0x8bf800`/`0x5015f0` are the insert fast/collision paths.
* `0x7b0480` is the insert wrapper used by the navmesh loader; it hashes the key
  and calls `0x5015f0`, then `0x8bf970` on a full map.
* `0x5fb220` is the NAVI record loader. It dispatches on the ESM subrecord tags
  `NVMI`, `NVMD`, `VMAD`, `EDID`, `OBND`, `NVSI`, `NVPP`, `NVER`, and calls
  `0x7b0480` for the `NVMI` path (`0x5fb413`).
* Relevant rehash detail: `0x8bf9c1` writes zero only at bucket `+0x08` for every
  new bucket (`new_array + 8 + 12*i`); `+0x00` and `+0x04` are intentionally left
  as they were. Occupancy is the `+0x08` marker. The insert fast path
  (`0x501625..0x50162c`) writes `[bucket+0] = key`, `[bucket+4] = value`,
  `[bucket+8] = sentinel`, so `+0x04` is only valid when the value pointer the
  caller passes is valid.

## What was changed in this branch and why

All changes are diagnostic only; no guest state was altered (beyond making the
watchpoint's `last_pc` accurate for IR execution).

* `source/guest.c`
  - New per-thread block ring (`N3DS_PC_RING=1`) of 96 records per guest thread
    id, each `{pc, esp, ecx, eax}`, with `guest_pc_ring_note()` and
    `guest_pc_ring_dump_thread()`. This is what showed the real faulting thread's
    path (`0x5fc6df ... 0x5fc859 0x60aa70`) instead of the interleaved global
    ring of `0xa48d60..0xa48f11` AOT allocator blocks.
  - `guest_pc_ring_note()` now also updates `guest_watch.last_pc/last_thread`,
    so a watchpoint hit in IR code is attributed to the IR block that stored,
    not to an unrelated old AOT block.
  - On the first watchpoint target, the per-thread ring is printed.
* `source/guest.h`
  - Prototypes for the two new ring functions.
* `source/ir_exec.c`, `source/ir_container.c`
  - Call `guest_pc_ring_note()` at IR function entry and at every IR block (both
    the slice IR and the container IR; the latter is the tier `0x60aa70` and
    `0x5fc640` run in here). A local `win32_thread_current_id()` prototype keeps
    the executor independent of the thread header.
* `source/n3ds_lift_runtime.c`
  - Note AOT/IR dispatches (`n3ds_rt_dispatch`, `rt_call_ic`) into the ring.
  - Fault dump now prints the faulting guest thread, that thread's 96-entry
    register ring, the first 8 stack words, `[ebp+0]..[ebp+0x68]`, the
    caller-frame slots (`[esp+0x4c]` = saved `this`, `+0x50`, `+0x64`, `+0x78`,
    `+0x80`, `+0x84`) and, from the saved `this`, the map fields
    `+0x00/+0x04/+0x08/+0x54/+0x58/+0x60/+0x68/+0x94` plus the first 4 dwords of
    the bucket array.

Nothing else in the tree was modified; `staged/` and `pilot_out_mult/` were
copied locally because the worktree lacked them, but they are gitignored and not
part of the commit. No experimental file remains in the tracked tree.

## Hypotheses checked and rejected

1. **Wrong TEB/TLS or wrong guest stack for the faulting thread.** Rejected:
   thread 13 has a normal `0x1d62xxxx` stack, its saved `this` is a valid map
   object with vtable `0x10c59dc`, and the faulting `eax`/`ecx` match the
   instruction operands exactly. This is not the earlier TEB bug.
2. **Wrong virtual dispatch / bad `this` from `0x43efe0`.** Rejected for this
   fault: `[0x1b2e864+0xe0]` is the valid NavMeshInfoMap at `0x554f7e40`, its
   vtable is `0x10c59dc`, and `[vtable+0x4c]` is `0x5fc640`. The call is
   intentional.
3. **The map object itself is freed/reused at the fault moment.** Not supported:
   `[this+0x00]=0x10c59dc`, `[this+0x60]=0x1269b1c`, `[this+0x80]=0x1269b20` are
   still the constructor values. It cannot have been freed without clobbering
   the vtable.
4. **The immediate `ecx` value comes from a shim/TLS.** Rejected; `ecx` is
   `[0x12b6634+0x28]` and the bytes are reproduced from the static image.
5. **Watchpoint result is a clean store PC.** Rejected as a technique in the
   current runtime: the watch detects a changed field at the *next* checked
   store, and the global `last_pc` was updated by AOT blocks on other threads.
   Results without the per-thread ring are misleading. Even with the fixed
   attribution, a watch on the final bucket slot `0x4259eb54` with target
   `0x12b6634` did not fire, which is itself evidence that the value may never
   have been written by a checked store.

## Remaining hypotheses, ranked

1. **Uninitialized/stale bucket value.** The rehash clears only `+0x08` per
   bucket. If the insert path (or the loader's local value pointer passed to
   `0x7b0480`/`0x5015f0`) is not initialized on the path that inserts this
   entry, the bucket's `+0x04` keeps stale heap data. `0x12b6634` is exactly the
   kind of value a previous object (a script functor `NiRTTI`) leaves at a
   reused address. This best matches "no checked store ever wrote the slot".
2. **Lifted field/effective-address bug in the navmesh loader.** A wrong
   displacement or source pointer in `0x5fb220` / `0x7b0480` / `0x5015f0` could
   store `[form+0x14]` (an `NiRTTI*`) or another wrong field as the bucket value.
   The insert fast path writes `[bucket+4]` from the pointer at `[esp+0x1c]`;
   verify that local is always the intended `NavMeshInfo*`.
3. **BSMemory double-use / cross-thread corruption.** A watch on `map+0x68`
   (`0x554f7ea8`) fired during `BSMemory::Free(0x42586b40)` and during the hash
   rehash, with free-list-looking values (`0x5578af80 -> ... -> 0x4259eb50`).
   This is suspicious but not proof: the map vtable and constructor fields
   stayed valid. The 12-byte bucket array is freed/reallocated repeatedly
   (`0x42586b40` old → `0x4259eb50` new) by `0x8bf970`; a UAF there is still
   possible.
4. **Cross-thread overwrite of the bucket array by a task thread.** Thread 13 is
   a `BSTaskManagerThread` (thread object vtables `0x111d7a0`/`0x111d7ac`), and
   other loader threads (14/15) are active. Not measured; the cooperative
   scheduler means a dump is single-threaded, but the corruption could predate
   the fault.

## Exact reproduction

Build the host slice (uses `E:`, never `F:`):

```powershell
cd C:\Users\vasya\Documents\Skyrim_N3DS_wt\menu-crash
$env:HOST_CLANG_JOBS='8'
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -TesvIrc E:\census_ir2\ir_container\tesv.irc `
  -TesvSliceGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -TesvExe E:\harness_skyrim\TESV.exe -AllowDiagnosticFault `
  -OutDir E:\menu_crash_host -Only tesv_slice -SkipPython -TestTimeoutSec 600
```

The built exe is `E:\menu_crash_host\run_tesv_slice_<pid>\tesv_slice.exe`.
Run from that directory:

```powershell
$env:N3DS_IRC_PATH='E:\census_ir2\ir_container\tesv.irc'
$env:TESV_MEMPROBE='1'
$env:TESV_D3D9_DEPTH_PAIR_PROBE='1'
$env:TESV_D3D9_HDR_LDR_FALLBACK='1'
$env:TESV_D3D9_SHADER_SKIP_PROBE='1'
$env:TESV_D3D9_BUDGET_MB='92'
$env:TESV_D3D9_CAPS_PROBE='1'
$env:TESV_D3D9_SM3_GATE_PROBE='1'
$env:TESV_MENU_TRACE='1'
$env:TESV_FILE_TRACE='1'
$env:TESV_FAULT_REGS='1'
$env:N3DS_PC_RING='1'
.\tesv_slice.exe E:\harness_skyrim\TESV.exe E:\harness_skyrim 6000000000 167772160 `
  2>&1 | Tee-Object E:\menu_crash_host\run.log
```

The fault appears deterministically (around log line 1690 in earlier runs),
thread 13 dies, and the process then hangs in the remaining threads; set
`TESV_HANG_WATCHDOG_MS` (e.g. `60000`) to get the all-thread dump/exit, or kill
the process after the fault dump. Memory budget is 160 MiB
(`167772160`); `F:` is the user's SD card and must never be read or written.

For the watchpoint experiments used here, additionally build with
`$env:N3DS_WATCH_STORE_CHECKS='1'` and run with, for example:

```powershell
$env:N3DS_WATCH_ADDR='0x554f7ea8'   # map+0x68, bucket-array pointer
$env:N3DS_WATCH_VALUE='0x4259eb50'
```

`N3DS_WATCH_ADDR=0x554f7e94` (map+0x54) and the final bucket slot
`0x4259eb54` were also tried. Remember the attribution caveat above.

## Test status in this branch

* Only the `tesv_slice` target was built and run here. Build: OK (clang, host).
  The scripted 200k-step run reports `0 passed, 1 diagnostic, 0 failed`
  (`ExitProcess(0)` after `Not enough memory to run application.` at 200k).
* The full host suite was **not** run after these edits. Do not claim it green.
* The real 6e9-step run reaches the documented fault; it is a diagnostic, not a
  PASS.
* No ARM/Azahar run was made in this session.

## Not achieved

* `StartMenu`/SWF open, `0x8779d0` type-0 dispatch, GFx `Draw*`: **0**.
* No frame, pixel or Present result is claimed.
* `DevCaps` was not changed (`0x00082000` remains the advertising value).
* The root cause was narrowed to the NavMeshInfoMap `map+0x50` hash bucket value
  `[bucket+0x04] = 0x012b6634` during TES init, but not fixed. The next agent
  should decide between the uninitialized-value path and a loader store bug
  (hypotheses 1 and 2) before touching the allocator.
