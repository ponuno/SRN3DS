# LOADING_MAP - static map of `loading -> main menu` (retail image)

Status: **static disassembly only**, no run claimed. Image
`E:\census_ir2\ir_container\_work\ir_container_input.bin` (raw PE image, so
`off = 16 + VA - 0x401000`), function bounds from
`E:\Skyrim_N3DS_build\analysis\tesv\functions.json`. Tools used:
`tools/loading_map.py` (function-level disassembly / owning-function lookup) and
`tools/loading_xref.py` (one-pass xref index: direct calls, immediates, memory
references, writes). Worktree `loading-map`, base `4abb745`.

Context from the dynamic runs (`docs/MENU_STALL_TRACE.md`, `docs/HANDOFF.md`):
`LoadingMenu` `0x86f010` + `FaderMenu` `0x85b020` exist, `StartMenu` is never
created, `0x6982c0` is never called, the main thread never reaches `0x69dd9f`,
thread 12 blocks on semaphore `0x537`, thread 8 blocks on `0x504`, thread 11
sleeps 28 ms in `0x6f4000`, and the only geometry is the loading-scene
`DrawIndexedPrimitive` from `0xcd39fa`.

## 1. What the disassembly actually shows

### 1.1 Render/loader task system (`0x6f3xxx`, `0x6f44xx`)

| VA | Role | Evidence |
|---|---|---|
| `0x6f3ce0` | stream-object ctor `(this, name?)` | `CreateSemaphoreA` x2: `[this+0x30]` = job token (`0x537`), `[this+0x34]` = done token (`0x538`); `[this+0x38]=0` stop flag; `0x10d0cc0` then `0x10d0cdc` vtable; `0xd20090(&this+0x40)` queue/container init; `[this+0x3c]=arg` |
| `0x6f3d70` | create one stream and register its thread proc | alloc `0xa48d60(0x1b418b0, 0x80)`, ctor call, store into `[0x1b39144 + i*4]`, `0x5849b0(obj, 0x32000)`, `0xa4b500(name, entry)` |
| `0x6f44d0` | init loader manager: 1 or 2 streams | `[0x123e304] > 2` -> 2 streams (`0x6f452a`), else 1 (`0x6f4590`); `[0x1b39118] = count`; thread procs `0x6f44a0` (stream 0) and `0x6f3a60` (stream 1); sets `[threadctx+0x348] = 1` |
| `0x6f44a0` | stream-0 thread entry | `0xa58e20(0)`; dispatch on `[0x1b39120]`: `1 -> 0x6f3a10`, `2 -> 0x6f4000`, else return |
| `0x6f3a60` | stream-1 thread entry | same dispatch, `1 -> 0x6f3a10`, otherwise return |
| `0x6f3a10` | background/idle pump (mode 1) | not detailed here |
| `0x6f4000` | **foreground/loading pump** (mode 2) | `cmp [0x1b39124],2; jne epilogue`; `0xa5b620` (menu manager) `-> [mgr+0xa1]; jne epilogue`; `GetTickCount` frame pacing; `0xa5d370` (menu update), `0xa5cd90`, input poll `[0x1b913f0]+0x38` vtable+0xc, `0xfc2d50`, `0xa68f20` (present); `Sleep(remaining)` at `0x6f40c9`, loop `jmp 0x6f4010` |
| `0xa4e2f0` | **stream worker body** (thread 12) | `call [vt+8]`; `if (![this+0x38]) do { WaitForSingleObject([this+0x30],INF) @0xa4e30d; if ([this+0x38]) break; call [vt+0x14]; call [vt+0x10]; } while (![this+0x38]); call [vt+0xc]` |
| `0xa4e2d0` / `0xa4e2e0` | vtable slots that `ReleaseSemaphore([this+0x34]=0x538,1,0)` | return address `0xa4e2ee` is the observed release site |
| `0xa4b580` | drop the stream's file/blob object | `if ([this+0x1c]) { EnterCS(this+4); free([this+0x1c]); [this+0x1c]=0; LeaveCS }` |

### 1.2 The loader mode word (the actual "am I loading" switch)

`0x6f3ea0(mode)` (`0x6f3ea0..0x6f3ef0`):

```
if (!mode) return;
[0x1b39124] = mode;          ; 0x6f3eab   copy for the pump loop
[0x1b39120] = mode;          ; 0x6f3eb0   authoritative mode
[0x1b39128] = 0;             ; 0x6f3eb5   "progress"/timer, reset
for (i = 0; i < [0x1b39118]; ++i)
    if ([0x1b39144 + i*4])
        ReleaseSemaphore(stream->[+0x30] /*0x537*/, 1, 0);   ; 0x6f3ee3
```

`0x6f3f00(mode)` (`0x6f3f00..0x6f3f68`):

```
if (mode == 0) match = ([0x1b39120] != 0); else match = ([0x1b39120] == mode);
if (!match) return;
[0x1b39124] = 0;                                    ; 0x6f3f25
for each stream: WaitForSingleObject(stream->[+0x34] /*0x538*/, INFINITE);  ; 0x6f3f51
[0x1b39120] = 0;                                    ; 0x6f3f5d
```

`0x6f3f80(enter)` = enter/leave wrapper: `enter != 0` and mode != 2 -> `0x6f3ea0(2)`
(`0x6f3f98`); `enter == 0` -> CS dance on `[0x12e3548]+0xc0` and `0x6f3f00(2)`.

Queries: `0x6f3570(mode)` -> `[0x1b39120] == mode` (`== !=0` when mode 0),
`0x6f3590()` -> `[0x1b39120] == 1` plus a lazy "start the render thread" path
(`[threadctx+0x348]`, `InterlockedIncrement`, `0xf54600`).

### 1.3 Callers (complete, from the xref index)

| callee | call sites |
|---|---|
| `0x6f3ea0` | `0x6f3f98` (tail of `0x6f3f80`, arg 2) and **`0x69ce15`** (game frame `0x69cbd0`, arg 1) |
| `0x6f3f80` | `0x4353ba` with **1** (enter loading), `0x43548b` with **0** (leave loading) |
| `0x6f3f00` | `0x6f3fda` (inside `0x6f3f80(0)`, arg 2) and `0x69d0c3` (game frame, arg 1) |
| `0x6f3e20` | `0x69e09e` (WinMain **teardown** block) and `0x696745` (renderer teardown) |
| `0x6f4000` | `0x6f44b9` (`jmp` from the stream-0 thread entry) |
| `0x6f3ce0` | `0x6f3d91` (`0x6f3d70`) |
| `0x86f010` (LoadingMenu ctor) | `0x86f2c8` only (`0x86f290` helper); the ctor itself is reached through the menu factory `0xa60c80` + name string `0x10e5ffc`, so there are no other static call sites |
| `0x85b020` (FaderMenu ctor) | none direct (same factory path) |

`0x435345` and `0x435455` have **no direct reference anywhere**; they are the two
halves of one state-machine function (`0x4352d0`), entered through
`0x4352d0`'s own branches (`0x435344` falls through into `0x435345`;
`0x4353ec` starts the arg1==0 half). `0x4352d0` itself is called from
`0x69de22` (WinMain), `0x69db05`, `0x699552/0x699781/0x6997db` (`0x699530`),
`0x746e61` (game task state machine, many sites), `0x681xxx`, `0x694a68`,
`0x74b825`, `0x4397e0`, `0x43a120`, `0x43a530`, `0x641b30`, `0x745860`.

## 2. The loading entry and exit paths (what "finished loading" means)

### Enter (`0x4352d0` arg1 != 0, `0x435345` block)

```
[0x12e3562] != 0        -> 0x435600 (already in the load state machine)
[0x12e3561] != 0        -> 0x898430(0,..) and return 0   ; a load is already running
0x1b2d4c0 = 1
0xa662a0, 0x892da0      ; system/save init
[threadctx+0x4ac] = 0x43
0xa5d250                ; menu manager "hide all / enter loading"
0x898430(1, a, b)       ; sound/event #1
0xa5d370                ; menu manager update
0x898430(0, a, b)       ; only if arg4 != 0
0x6f3f80(1)             ; *** 0x4353ba: mode := 2 -> loading pump owns the frame ***
[0x12e3561] = 1         ; 0x4353ca: "loading in progress"
0xc7be60(0), [0x1ba75f8] = 0, 0x688040
```

### Exit (`0x4352d0` arg1 == 0, `0x435455` block)

```
... optional save/quickload glue (0x4f0960/0x6293a0/0x629fb0) ...
0x1b2d4c0 = 0
0x6f3f80(0)             ; *** 0x43548b: -> 0x6f3f00(2): state 0, wait 0x538 per stream, mode 0 ***
0x431b00([0x12e35e0]+0x74, 3, 0)   ; queue a menu message (fade-out family)
... 0xa5ce90([0x12e35e0]+0x54) etc. ...
[0x12e3561] = 0         ; 0x4355a6 / 0x4355f9: loading finished
```

### The per-frame game half (`0x69cbd0`)

`0x69cbd0` is the frame function called from the WinMain loop at `0x69dfe7`.
Relevant only for this map:

```
0x69cc12  0x6986c0
0x69cc17  0x460580
0x69cc49  0xa5b620 -> 0xa5b3c0        ; menu manager tick
0x69cc6e  al = ([0x12e3548]+0xc8 > 0); [0x1b2e85f] = al   ; "has player/world"
0x69cc7c  paused_byte = (al == 0 && [0x1b2e860+0xa] == 0) ; [esp+0xf]
0x69cc8e  0xa5c7d0 -> [0x1ba75f8] = 3
0x69cca3  if (!has_player) 0xa5ce90(menu+0x48); [0x1ba75f8] = 2
0x69ccce  else [0x1ba75f8] = 1
0x69cdfc  if (!paused_byte) goto 0x69ce1d
0x69ce02  [0x1b39264] += [0x1b4ade0]      ; accumulate loading time (float)
0x69ce15  0x6f3ea0(1)                     ; *** mode := 1, one job token per stream ***
0x69d05f  0x6f3570(1) -> 0xa59070(1,0)
0x69d0c3  0x6f3f00(1)                     ; flush: wait 0x538 per stream
```

**Completion condition (static):** the loading screen ends only when the loader
mode `[0x1b39120]` / `[0x1b39124]` stops being `2`. There are exactly three
writers of that word, and only two produce a transition out of loading:

1. `0x43548b` -> `0x6f3f80(0)` -> `0x6f3f00(2)` -> mode `0` (world-load state
   machine decided the load is over; also clears `[0x12e3561]`);
2. `0x69ce15` -> `0x6f3ea0(1)` -> mode `1` (the game frame ran, i.e. WinMain
   reached its frame loop and the "no player yet" test passed).

There is **no progress/file/job/timer completion test**: `[0x1b39128]` is only
ever written (reset at `0x6f3eb5`) and never read; `[0x1b39264]` only
accumulates. `[0x1b39118]`/`[0x1b39144]` are only a stream table. So loading
completion is a **control-flow event in the load state machine**, not a
data-driven condition - if that code path does not run, the screen never ends no
matter how much I/O completes.

## 3. Job handoff: who is supposed to feed the streams

* The **only** static `ReleaseSemaphore(stream->[+0x30])` sites are `0x6f3ee3`
  (inside `0x6f3ea0`) and `0x6f3e57` (inside `0x6f3e20`).
* `0x6f3e20` also sets `[stream+0x38] = 1` *before* releasing the token and then
  `WaitForSingleObject(stream->[+0x34], INFINITE)` per stream, and finally
  destroys the stream objects. It is the **stop/flush-all** routine: it is
  called only from the WinMain teardown block (`0x69e09e`, after
  `[0x1b2e860+4] != 0` sets the quit path) and from renderer teardown
  (`0x696745`). It is *not* the loading handoff, and it correctly never runs
  while the process is alive.
* The real handoff is `0x6f3ea0`: each call grants exactly **one** work token per
  stream and resets `[0x1b39128]`. Static callers: `0x4353ba` (enter loading,
  arg 2 - this is the single release the dynamic run observes, return address
  `0x4353bf`) and `0x69ce15` (game frame, arg 1). A third grant path does not
  exist in this binary.
* The worker side (`0xa4e2f0`) consumes one token, runs `[vt+0x14]`/`[vt+0x10]`,
  releases `0x538`, and blocks again. That matches the trace exactly
  (`release h=537 once -> block h=537 -> release h=538`).

**Consequence:** while the main thread does not execute `0x69cbd0`, the loaders
get exactly one token in total and the queue is never pumped. Nothing else in
the image can feed `0x537`.

## 4. `0x504` - what that wait actually is

`0x504` is created by `0xa5b06f` inside `0xa5b050`: the engine's own thread
wrapper ctor (`BSThread`-like, vtable `0x110dc44` then `0x110dd1c`):

```
[this+8]  = CreateSemaphoreA(NULL,1,0,NULL)     ; 0x504
[this+0xc] = CRITICAL_SECTION (InitializeCriticalSection at 0xa5b0a3)
[this+0x18] = min(priority, 6)
[this+0x1c] = number of sub-threads actually spawned
for i < [this+0x18]: CreateThread(..., 0xa5b000, this, 0, &[edi+0x18])   ; 0xa5b0f2..0xa5b100
if ([this+0x1c]) CreateThread(..., 0xa5ac30, this, 0, &tid)               ; 0x5b126
```

`0xa5ac30` is the **builder/watcher** proc, not a worker that consumes work:
it does `WaitForSingleObject([this+8], INFINITE)`, on success
`InterlockedDecrement([this+0xc])`, then releases each sub-thread handle
(`0x106b0d8`) and `ExitThread(0)`. The only `ReleaseSemaphore([this+8])` /
`CloseHandle([this+8])` sites are the destructor paths `0xa5b232` /
`0xa5b326`, i.e. shutdown.

So thread 8 blocked on `0x504` forever is **by design for a running process**.
It is a red herring: it is the "wait for the thread pool to be torn down"
watcher. Treating it as the missing producer (as `docs/PORT_LOG.md` rows 250-252
suspected) leads in the wrong direction.

## 5. Call graph (condensed)

```
WinMain 0x69d1d0
 |- ... 0x699b20 / 0x691410 / 0x691030 -> renderer gate, MessageBox+ExitProcess on failure
 |- 0x6982c0 (startup-save probe; never reached in the stuck run)
 |- 0x69dd9f -> 0x6982c0
 |- branch [esp+0x13] != 0:
 |   0x876f60(1)  StartMenu create -> 0x876fc0 -> 0x876b90 -> 0xa60c80 CreateMovie
 |   0x4352d0(1,..)  start world load
 |- loop 0x69e000: PeekMessage/DispatchMessage
      0x69dfe7 -> 0x69cbd0   game frame
                   0x69ce15 -> 0x6f3ea0(1)     (mode 1, one token/stream)
                   0x69d0c3 -> 0x6f3f00(1)     (flush, waits 0x538)
      quit -> 0x69e033 teardown -> 0x6f3e20 (stop all streams)

0x4352d0(1) -> 0x435345
        -> 0x6f3f80(1) -> 0x6f3ea0(2) -> ReleaseSemaphore(0x537) per stream, mode 2
        -> loading pump thread: 0x6f44a0 -> [mode 2] -> 0x6f4000 loop
             (menu tick 0xa5d370 / input / present 0xa68f20 / Sleep to frame time)
        -> stream worker: 0xa4e2f0 wait 0x537 -> job -> release 0x538 -> wait 0x537
0x4352d0(0) -> 0x435455 -> 0x6f3f80(0) -> 0x6f3f00(2) -> mode 0, waits 0x538
                         -> 0x431b00(menu+0x74, 3, 0), [0x12e3561] = 0
```

## 6. Fields worth watching

| Address | Meaning | Writers | Readers |
|---|---|---|---|
| `0x1b39120` | loader mode (authoritative) | `0x6f3eab` (via `0x6f3ea0`), `0x6f3f5d` (zero) | `0x6f3570`, `0x6f3590`, `0x6f3f87`, `0x6f3f0a/15`, `0x6f44a7`, `0x6f3a67/87` |
| `0x1b39124` | loader mode copy for the pump | `0x6f3eab`, `0x6f3f25` (zero) | `0x6f4010` (`==2` runs the loading pump) |
| `0x1b39128` | "progress"/timer, dead | `0x6f3eb5` only | **nobody** |
| `0x1b39118` | stream count (1 or 2) | `0x6f4548`, `0x6f459d` | `0x6f3ebf/ee6`, `0x6f3e23/93`, `0x6f3f2f/54` |
| `0x1b39144[i]` | stream object ptr | `0x6f3da6` etc. | `0x6f3e40`, `0x6f3ed0`, `0x6f3f40` |
| `0x1b39264` | accumulated loading seconds (float) | `0x69ce0f` | - |
| `0x12e3561` | "load in progress" | `1` at `0x4353ca`, `0` at `0x4355a6/0x4355f9` | `0x4352ea`, many task sites |
| `0x12e3562` | "in load state machine" | `0x746e61` (`1`/`0`) | `0x4352dd` |
| `0x1ba75f8` | menu/game phase: 0 none, 1 game, 2 loading-ish, 3 loading | `0x4353d6`, `0x69cc97/ccc0/ccce` | `0xcca42e`, `0xc9553a` |
| `0x1b2e85f` | "player/world exists" byte | `0x69cc77` | `0x69cca3`, `0x69cdab`, ... |
| `stream+0x30` | job token semaphore (`0x537`) | release `0x6f3ee3`, `0x6f3e57` | wait `0xa4e30d` |
| `stream+0x34` | done semaphore (`0x538`) | release `0xa4e2d8`, `0xa4e2e8` | wait `0x6f3f51`, `0x6f3e66` |
| `stream+0x38` | stop flag | `0x6f3e4d`, `0x6f3ce0`(0) | `0xa4e2fa/0x30f/0x327`, `0x6f3e29` |
| `stream+0x40` | job queue container | `0xd20090` init | worker `[vt+0x14/0x10]` |
| `threadctx+0x348` | "render thread started" | `0x6f452a` | `0x6f35a9` |

## 7. Ranked hypotheses

**H1 (most likely, static-only): the main thread never reaches the frame pump
`0x69cbd0`, so nothing ever grants a second work token or ends the loading
screen.**
The only two exits from mode 2 are `0x43548b` (world-load state machine decides
the load is done) and `0x69ce15` (frame pump). With `0x6982c0` / `0x69dd9f`
never reached, WinMain is stuck in an init/load sub-call and never enters
`0x69e000` -> `0x69dfe7`. That yields exactly the observed picture: one token
release (return address `0x4353bf`), worker blocked on `0x537`, pump thread
sleeping in `0x6f4000`, no menu message dispatch (`0x8779d0` / `0x877a50` never
entered), no GFx geometry.
*Check:* main-thread guest stack; expect a return address inside
`0x69d1d0` between the entry and `0x69dd9f`, i.e. a blocking child of
`0x696810` / `0x699b20` / `0x691410`, or a cooperative wait inside
`0x4352d0`'s `0x898430` / `0x892da0` calls.

**H2: the load state machine entered loading from a partial path
(`0x4352d0(1)` via `0x746e61`/`0x699530` rather than from WinMain's save
branch), and the matching `0x4352d0(0)` "finish" call is gated behind the very
task update (`0x746e61`) that cannot run while mode 2 owns the frame.**
This is a real re-entrancy trap in the retail code: the loading pump
(`0x6f4000`) runs *instead of* the task graph; the task graph is what would call
`0x4352d0(0)`. If the engine's normal order is "task calls `0x4352d0(0)` from its
own update", then a host-side scheduling difference (which thread resumes after
`Sleep(28)`) decides whether that happens.
*Check:* whether `0x746e61` is ever entered; whether `[0x12e3562]` is ever
cleared; `0x4352d0` call-site histogram (arg1=1 vs arg1=0).

**H3: the one grant at `0x4353ba` is consumed by an empty queue, and the SWF
`CreateMovie` continuation (`0xa5ed20` `BSScaleformFileOpener`) is queued behind
a *submit* call that is never made (no static caller of `0xa5ed20` is reached
from the loading path).**
`0x6f3ea0` resets `[0x1b39128]` and grants a token, but the job list at
`[stream+0x40]` is empty because nothing pushed a job. The GFx opener is thus
never called and `Menu::CreateMovie` stays half-done (`[0x1b3fc98]` singleton
still 0 at ctor time in `MENU_STALL_TRACE.md`).
*Check:* `0xa4e31c`/`0xa4e325` (the two vtable job calls) - do they run once and
find an empty container? Dump `[stream+0x40]` head/tail before and after.

**H4 (host-side, lower confidence): the cooperative scheduler resumes the wrong
party after the frame pacer `Sleep(remaining)` at `0x6f40c9`.**
`win32_thread_sleep` -> `block_current(NULL,0,0,ms)`; if the timer wakeup is
delivered but `pick_ready()` keeps choosing the pump thread (because it becomes
ready first), the main thread is starved and H1's symptom appears even though
the guest code is correct. The host already special-cases `0x537`/`0x504` in
`source/win32_thread.c` logging, so these waits are visible but not scheduled
around.

**Rejected:** `0x6f3e20` as "the handoff" (it is teardown-only); `0x504` as "the
missing producer" (it is the thread-pool teardown semaphore, expected to block
for the process lifetime); `[0x1b39128]` as a completion counter (never read).

## 8. What to verify dynamically (in this order)

1. `0x1b39120` / `0x1b39124` **and** `0x1b39128` per present. Expect
   `2, 2, 0` forever while stuck. If `0x1b39128` ever moves, H2/H3 change.
2. Call count and return addresses of `0x6f3ea0`. `1` call -> H1/H2 confirmed;
   `>1` calls with mode 2 -> the grant path exists, look at the queue.
3. `0x69cbd0` entry count and `[esp+0xf]` at `0x69cdfc`; if the function is
   entered, log `[0x12e3548]+0xc8` and `[0x1b2e860+0xa]`.
4. Main-thread guest stack at the stall (expect WinMain below, a blocking child
   of `0x696810`/`0x699b20`/`0x691410` at the top).
5. `0x4352d0` arg1 histogram and `[0x12e3561]` / `[0x12e3562]` transitions; add
   a `TESV_LOADER_TRACE` probe at `0x4353ba`, `0x43548b`, `0x6f3ea0`,
   `0x6f3f00`, `0xa4e307`.
6. `0xa4e31c`/`0xa4e325`: the two vtable calls of the worker - do they run and
   what does the queue at `[stream+0x40]` look like.

## 9. Host-side sketch (not applied here)

If probe 1 shows mode is `2` and the main thread is not in `0x69cbd0`, the
cheapest honest instrumentation is read-only tracing, not simulation:

* `source/win32_thread.c`: extend the existing `h == 0x537 || h == 0x504`
  logging to also print `mode20/mode24/progress` (`0x1b39120`, `0x1b39124`,
  `0x1b39128`) on every `ReleaseSemaphore`/wait, so the mode flip and token
  count are visible without a new probe surface.
* Add `0x6f3ea0`, `0x6f3f00`, `0x4353ba`, `0x43548b`, `0xa4e307` to the
  `TESV_MENU_TRACE`-style entry list (same mechanism as `MENU_STALL_TRACE.md`,
  printing `mode`, `[0x12e3561]`, `[0x12e3562]`, `[0x1ba75f8]`).
* Only if H4 is confirmed (main thread never scheduled while the pump thread
  sleeps): make the timer expiry wakeups fair (oldest-ticket first) instead of
  letting the woken sleeper re-win `pick_ready()`.

No C change is made in this worktree.

## 10. Reproduce

```powershell
$py = 'C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe'
& $py tools\loading_map.py dis 0x6f3ea0 0x6f3f80 0x4353bf 0x69ce15
& $py tools\loading_xref.py --build          # one pass, ~2 min, %TEMP%\lm\xref.json
& $py tools\loading_xref.py 0x6f3ea0 0x6f3f80 0x6f3e20
& $py tools\loading_xref.py --mems 0x1b39120 0x1b39124 0x12e3561
```
