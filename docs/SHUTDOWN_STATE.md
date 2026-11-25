# Shutdown state — 2026-11-25 ~01:50

Saved before shutting the workstation down for the night.

## Milestone

**WinMain `0x69d1d0` is executing.** It reaches
`HKLM\Software\Bethesda Softworks\Skyrim` (call site `0x0069d625`), reads
`sBasePath`, initializes Steam (offline, returns false) and proceeds into the
plugin-list path.

## Landed in the live tree

| Change | Status |
| --- | --- |
| IR container runtime + packer call-split fix | landed, 44-test host suite |
| Parallel lifter (8 workers, ~4.14x) | landed |
| Lifter segment/loop coverage (605 liftable invalid entries) | landed |
| ADVAPI32 registry shim (5 functions, bounded in-memory tree) | landed |
| Install state preload: `HKLM\Software\Bethesda Softworks\Skyrim\sBasePath = C:\Skyrim\` | landed |
| `LeaveCriticalSection` on an unheld section: logged no-op (Windows-compatible) | landed |
| Steam shim + first-draw capture | landed earlier |

Host suite: **44 passed, 0 diagnostic, 0 failed** (last full run 01:15).
Registry test updated for the seeded install key.

## Current blocker (diagnosis in progress)

After `[cs] leave on unheld section 30000cb0 (Windows-compatible no-op)` the
probe stops producing output and the single thread sits in Windows
`Wait ExecutionDelay` — the guest is sleeping/polling, not executing. Likely it
polls for the `TESInitThread` launcher (`0x00a5ac30`) that the single-thread
runtime terminated on `WaitForSingleObject(INFINITE)`.

Diagnostics added to live `source/win32_shims.c` (`s_Sleep`) and
`source/win32_sync.c` (`wait_unavailable`): a bounded `[sync] Sleep ...` /
`[sync] wait ... from <return address>` log for >= 1000 ms, and a trap for a
sleep/wait over 60 s. **These diagnostic log lines are still in the live tree;
review them (keep the 60 s trap, the log is bounded to 16 lines).**

A host build + 15 s probe was running when the machine was shut down
(`C:\Skyrim_N3DS_build\host_probe_sync`). Rebuild and run it, then read the
`[sync]` lines: the return address identifies the polling loop.

## Likely fix

The cooperative thread model is implemented and host-tested in scratch:
`E:\Skyrim_N3DS_threads\threads.patch` (16 files), `THREAD_MODEL.md`.
Verified: real `0x00a5ac30` with two suspended workers passes
(`counter=2, pending=1, launcher_exit=0`), and the whole-game probe no longer
faults at `0x504`. It is **not landed**: ARM backend untested, critical
sections are not yet blocking waits, container frame depth is single-global.
Review before merging; it may be exactly what unblocks the polling loop.

## Growth

Stopped (no supervisor running). Last confirmed frontier: WinMain →
`ADVAPI32!RegOpenKeyExA`. Restart with:

```
E:\Skyrim_N3DS_build\analysis\growth_roots16_all.txt   (12,436 roots)
```

plus `HOST_LIFT_JOBS=8`, `N3DS_IRC_PATH=E:\Skyrim_N3DS_build\ir_container\tesv.irc`.
Host build output now goes to `C:\Skyrim_N3DS_build\host` (auto-pruned, 3 newest).

## Artifacts

```
E:\Skyrim_N3DS_build\ir_container\tesv.irc   64,227,440 B
  sha256 21678d3227268b9062cc25abcddfc21a232c6622b333b73479fd29c2d449a9e2
F:\Skyrim\ir\tesv.irc  (same)
F:\cias\Skyrim_N3DS_tesv_hwbulk.cia   968,640 B
F:\cias\Skyrim_N3DS_tesv_hwnext2.cia  905,664 B
F:\cias\Skyrim_N3DS_tesv_hwhybridprobe.cia 845,248 B
```

## Open subagent

`E:\Skyrim_N3DS_callsite` — traces the origin of computed call targets
(`0x00ce1b30`, `0x00e6e7d0`). It was still running; its files and logs are on
disk, and its `CALLSITE_TRACE.md` may exist by morning.

## Docs updated

`docs/REGISTRY_SHIM.md`, `docs/MEMORY_BUDGET.md`, `docs/CONTAINER_INTEGRATION.md`,
`docs/LIFTER_EXTENSIONS.md`, `docs/PARALLEL_LIFT.md`, `docs/IR_CONTAINER.md`,
`docs/STATUS.md`, `docs/PORT_LOG.md` (rows through 104), `docs/ROADMAP.md`,
`docs/DEBUGGING.md`, `docs/STEAM_API.md`.
