# Guest memory budget and the 178 MiB target

Investigation: `E:\Skyrim_N3DS_membudget\REPORT.md`, 67 logged runs. Host
binary with the container tier, `TESV_MEMPROBE=1`.

## The gate

TESV calls `GlobalMemoryStatusEx` at `0x00a48a5d` (IAT `0x0106b274`) and
continues only when `ullAvailPageFile >= 0x20000000` (512 MiB). The test is at
`0x00a48a67..0x00a48a77`. Nothing else decides it: not `HeapCreate` sizes, not
a `VirtualAlloc` failure, not a CRT limit.

On failure it shows the `Not enough memory to run application.` message box
(`0x0110d0ec`), calls `0xf58b4f`, and exits; `0xf5894a` is that call's return
address.

`source/win32_shims.c` reports `AvailPageFile = budget - used` and, with
`TESV_MEMPROBE=1`, a fictional 512 MiB. That probe value is the only reason a
160-256 MiB host budget passes today.

## Measured thresholds

| Mode | Threshold | Notes |
|---|---:|---|
| `TESV_MEMPROBE=1` | **44,165,120 B = 42.119 MiB** | deterministic, 3x each; 44,164,096 B fails |
| probe off | no budget passes | the runner clamps at 256 MiB; arithmetic minimum is ~543 MiB, impossible on 178 MiB |

At the threshold the failing allocation is one 4 KiB anonymous backing page in
the third reservation (256 MiB at `0x54050000`): `page_back()`'s
`used + 4096 > limit` check in `source/guest.c:175`. Commit failures are zero.
At 32 MiB the 256 MiB reservation's metadata does not fit; at 28 MiB PEB/TEB
setup fails.

## What the engine actually reserves

```
595,200 KiB = 581.25 MiB  VA reservations (5)
102,656 KiB = 100.25 MiB  commit promise
  4,744 KiB =   4.63 MiB  physically backed at the current frontier
```

The 178 MiB CIA region (~127 MiB free) therefore **can** host this stage:
42.12 MiB of guest pool is enough, and 160 MiB behaves identically to 178 MiB.
No reservation or the 100 MiB commit promise has to shrink for startup.

## The honest problem

The engine only accepts a *reported* 512 MiB. Today that number is fictional
under `TESV_MEMPROBE=1`, which is explicitly a diagnostic-only value. Two real
options:

1. **Real backing store.** Implement eviction/reload for dirty anonymous pages
   (SD swap). `page_flush()` currently handles only file-backed pages, so lazy
   backing is safe for startup (4.63 MiB touched) but not for sustained play
   (100 MiB promise vs a 60 MiB device pool, and dirty anon pages cannot be
   evicted). This is the honest route to reporting 512 MiB.
2. **Native engine patch.** Replace the 512 MiB gate with the real device
   budget and keep the working set under it. Explicit, but it changes guest
   behaviour and must be documented as a port patch, not a shim value.

Do not ship the probe value. It reports capacity the port cannot back and the
project rule is to fail explicitly rather than fake success.

## Runner detail

The host runner clamps the requested budget at 256 MiB, so a 1 GiB argument is
not what made the earlier run pass; `TESV_MEMPROBE=1` was. The growth script
always sets `TESV_MEMPROBE=1`, so growth results are unaffected by this clamp.
