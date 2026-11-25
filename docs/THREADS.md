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
