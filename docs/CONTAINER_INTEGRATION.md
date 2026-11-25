# Indexed IR container: runtime integration landing

Status: **landed in the live tree** on 2026-11-24/25. The long-form scratch
record (tests, exact hunks, rollback) is at
`E:\Skyrim_N3DS_container_pending\CONTAINER_INTEGRATION.md`.

## What landed

| File | Change |
| --- | --- |
| `source/n3ds_lift_runtime.c` | container tier, lazy/required open, env overrides, named faults, release/recover |
| `source/n3ds_lift_runtime.h` | container state/path/stats accessors |
| `source/ir_container.c` | two dead helpers removed; integration fixes |
| `source/ir_core.h` | `IrBlockInfo.term_op` (terminator opcode) |
| `source/ir_decode.c` | records the terminating IR opcode |
| `tests/ir_container_pack.c` | splits a span after a call; sorts/dedupes block starts |
| `Makefile` | `ir_container.o` in irbench, lift and TESV-slice objects |
| `tools/run_host_tests.ps1` | container source in legacy list, Python tests |
| `tests/ir_container_test.py` | integration, required-fault and short-read cases |
| `tests/ir_container_integration.c` | new |
| `tests/ir_container_growth.c` | new |
| `tests/ir_container_growth_test.py` | new |
| `tools/arm_container_compile.ps1` | new |

## Dispatch order

```
AOT FnEntry -> slice IR table -> container record -> explicit unknown fault
```

The slice moved ahead of the container: an invalid container entry would
otherwise fault before the slice IR could serve an address growth had already
lifted. `n3ds_lift_has_function()` and `rt_call_ic()` follow the same order.

Environment:

| Variable | Meaning |
| --- | --- |
| `N3DS_IRC_PATH` | container path; host default `tesv.irc`, ARM default `sdmc:/Skyrim/ir/tesv.irc` |
| `N3DS_IRC_ENABLE=0` | disable the tier entirely |
| `N3DS_IRC_REQUIRED=1` | missing/corrupt container is a named fault |
| `N3DS_IRC_CACHE_BYTES` | cache byte budget (default 4 MiB) |
| `N3DS_IRC_CACHE_RECORDS` | cache record cap (default 256) |

## Packer fix: blocks after a call

`n3ds_ir_encode_block()` terminates a block at a call, but discovery keeps the
call and the following code in one span. The container therefore lacked the
return-address block and failed at `0x00402dcf` with
`block ... is not in the predecoded record`. The packer now re-encodes from
`info.end_ip` while the span continues and the terminator is a call, sorts the
block table and merges duplicate starts.

Result: blocks 722,343 -> 1,035,798; decoded 90,063 -> 90,027; invalid
837 -> 873. The 36 newly invalid functions were previously silent incomplete
records; they are now explicit `not decoded` entries.

## Verification

- Full host suite: **43 passed, 0 diagnostic, 0 failed** (twice: initial
  landing and after the dispatch reorder).
- Container growth test on the real image: target `0x401860` executes 200,000
  times, `loads 1`, `redecodes 0`, `0.065 us/invoke`.
- ARM object compile: 19 objects, 0 failed.
- TESV probe with the container: passes `0x00402dcf`, spawns guest thread
  `0x00a5ac30`, then stops at `0x00ce1b30: unknown function/import` (an address
  that is not in `functions.json`, so it needs a growth root).
- The first probe attempt, before the packer fix, stopped at `0x00402dcf`.

## Current container

```
E:\Skyrim_N3DS_build\ir_container\tesv.irc
64,227,440 bytes (61.25 MiB)
SHA256 21678d3227268b9062cc25abcddfc21a232c6622b333b73479fd29c2d449a9e2
F:\Skyrim\ir\tesv.irc  (same hash)
```

## Not done / open

- No ARM link, `.3dsx`/CIA or physical run of the container build yet.
- Guest threads: TESV spawns thread `0x00a5ac30`; the single-thread runtime
  reports `blocking wait would deadlock the single guest thread` and terminates
  it. Execution continues, but a real scheduler/cooperative wait is needed
  before any thread-dependent phase. The body is a thread launcher:
  `WaitForSingleObject([param+8], INFINITE)` -> loop over `[param+0x1c]` handles
  calling `ResumeThread` -> `ExitThread(0)`. It only needs parking until the
  engine signals that event; it is not a compute thread.
- Growth now keeps host build artifacts on `C:` and prunes old
  `run_tesv_slice_*` directories: each one holds a ~230 MB exe, pdb and objects,
  and 50+ of them filled `E:` (the first growth28 attempt failed with clang
  `IO failure on output stream: no space on device`, not a code error).
- 873 container entries are invalid (823 unsupported opcode, 43 no-terminator,
  7 unknown). Growth must lift them (or stop explicitly).
- Container is 61.25 MiB; the bounded cache still loads one record at a time.
