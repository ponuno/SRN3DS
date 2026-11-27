# Guest thread model

Status: **implemented and host-tested** (cooperative, one guest thread at a
time). The design, the exact state that must be saved, memory cost, 3DS
feasibility and the test evidence are in [THREAD_MODEL.md](THREAD_MODEL.md).

## What changed

`source/win32_thread.c` no longer runs a thread body to completion inside
`CreateThread`/`ResumeThread`. Each created thread owns

- a guest stack + TEB + private TLS copy (unchanged mapping layout),
- a `CPU` that is live for the whole thread (not just during one call),
- a **host stack** (`source/win32_hostctx.c`) so a blocking shim can suspend the
  thread and resume it later at the same instruction.

`CreateThread` (without `CREATE_SUSPENDED`) and the `ResumeThread` that drops the
suspend count to zero still switch to the new thread immediately; if it blocks,
control returns to the creator. `WaitForSingleObject`, `WaitForMultipleObjects`
and `Sleep` are scheduling points. A wait that no thread can ever satisfy fails
with an explicit "all guest threads blocked" fault naming every waiter.

Two host backends are compiled from the same code:

| backend | used for | status |
|---|---|---|
| Windows fibers (`WIN32_HOSTCTX_BACKEND=1`) | host tests | verified |
| manual x86-64 stack switch (`=3`) | host tests, portable mechanism | verified |
| manual ARMv6K switch (`=2`) | 3DS | compile-guarded, **not run on hardware** |

The 3DS build must add `source/win32_hostctx.c` to `OFILES` (done in the
Makefile); the ARM backend publishes no OS thread and only swaps r4-r11/sp/lr
and d8-d15.

## Invariants (enforced by tests)

- No thread is reported as running if its body was never entered.
- A wait on an unsignaled object with `INFINITE` never returns success.
- A blocked thread resumes with its continuation and arithmetic flags intact.
- Every new thread has its own TEB, TLS block, last-error slot and stack.
- If every thread is blocked forever the runtime faults; it never hangs.
- **READY means runnable**: every `ST_READY` transition goes through the single
  `ready_stamp()`/`enqueue()` pair and carries a fresh FIFO `ready_seq`, and
  `pick_ready()` scans the state (not a separate list), so a `READY` thread is
  always a scheduling candidate. `main` competes with the same sequence; only
  an exact tie (which cannot recur) prefers it.
- **No tier may own the CPU**: slice IR and container IR poll after every
  `N3DS_IR_YIELD_INTERVAL` (4096) blocks, native AOT polls after every 4096
  lifted block labels, and a single `rep` polls every 65536 elements.

## Starvation fix: AOT back-edge yield (base `23d06dd`)

Observed: thread 13 ran guest code for 165 031 ms with no scheduler event at
all; main was `state=ready seq=381891`, thread 13 `state=running seq=381890`,
the other workers blocked. The dump's stack contained the lifted **AOT** body
`0xa48d60` (AOT table entry `{0xA48D60, f_A48D60}` from `gen/tesv_table.c`) and
`ir_ip`/`last_call` pointed at the last tiny *leaf* calls (`0x575c46`,
`0x4658c0`), not at a hot IR block.

Root cause: the cooperative slice only existed *between IR blocks*
(`ir_exec.c`/`ir_container.c`). Lifted AOT bodies are ordinary host C and run
their loops to completion inside one `n3ds_rt_dispatch` call, so the tick never
ran there. `f_A48D60` walks an object list and calls a virtual method per
element: each iteration returns to native code, and once the walked structures
are IR-only leaves the outer native loop can run for minutes without a single
scheduling point. Nothing was wrong with the ready queue - yields simply were
not requested (`ir_yield_calls`/`hook_yields` flat while wall time grew).

Fix (no generated file edited by hand):

- `source/n3ds_pilot.c`: `n3ds_pilot_tick_pc()`, which
  `tools/instrument_generated.py` already emits after **every** lifted block
  label, now calls `n3ds_ir_yield_poll()`.
- `source/ir_core.h`: `n3ds_ir_yield_poll()` is a static-inline counter test
  (one increment + compare, NULL hook = one predictable branch) that fires the
  same scheduler hook every `N3DS_IR_YIELD_INTERVAL` blocks, so an AOT loop
  gives the CPU up within a few thousand blocks (well under 100 ms).
- `source/ir_exec.c`: a single `rep movs/stos/lods/cmps/scas` is one IR block,
  so its count is walked in 65536-element chunks with a poll between chunks.
  Chunking is semantically identical (ascending order is exact for the forward
  forms; `rep movsd` already processes dwords in ascending order).
- `source/win32_thread.c`: the hang watchdog compares the activity gap with a
  **signed** difference. Sampling `now` before reading `g_activity_ms` can see
  a stamp 1 ms in the future, and the old unsigned compare turned that into a
  bogus 4 294 967 295 ms "hang".

Diagnostics (`N3DS_SCHED_TRACE=1`, plus the hang dump) now print yield-hook
entries, actual switches, `pick_ready` calls, how often READY main lost the
pick, ready/queue orphans, IR blocks executed, yield-hook firings, the current
IR block and its age, the longest single block, the queue contents and the
dispatch count per tier (AOT / slice IR / container). That is what
distinguishes "one block never returns" (blocks and yields flat) from
"interpreter runs but hook missing" (blocks climb, yields flat) from "AOT runs
natively" (AOT tier climbing, IR counters flat).

Regression: `tests/thread_coop.c::test_aot_loop_yields` spawns a native
(lifted-AOT-shaped) body that loops 3 000 000 times calling
`n3ds_pilot_tick_pc()`, and requires the creator to regain the CPU with fewer
than `budget` iterations done and in **under 100 ms** (measured: first yield
after 4096 blocks, 0 ms).

