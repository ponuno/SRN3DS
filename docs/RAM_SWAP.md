# Compressed guest-page swap (host RAM only)

Status: **prototype implemented and host-tested**; device build enabled by
default, not yet measured on hardware. Design + measured compression ratios +
the exact safety rule. Read with `docs/MEMORY_BUDGET_178.md`.

## 1. Why

The 178 MiB region holds the newlib application heap (96 MiB by default), and
every anonymous guest page is a 4 KiB `calloc()` block from that same heap. The
engine asks the allocator for hundreds of MiB (581 MiB of VA reservations,
100 MiB commit promise) while only a fraction of the pages are live at any one
moment. Until now the only defence was to refuse backing (`GUEST_FAULT_NO_MEMORY`).

Swap turns cold anonymous pages into their **LZ4-compressed** form in host RAM
and gives the 4 KiB block back. A resident page costs 4096 B; an evicted one
costs its compressed size (typically 1.3–4x smaller). The page is restored
byte-for-byte on the next touch.

## 2. Design

New module `source/guest_swap.c` + `source/guest_swap.h`; `guest.c` calls it at
four points:

| Point | Call | What happens |
|---|---|---|
| `page_back()` | `guest_swap_fetch()` / `guest_swap_block()` | restore bytes from the store, or zero a recycled block |
| `slab_prepare()` | `guest_swap_touch()` | set the page's CLOCK bit |
| `guest_slab_decommit/release` | `guest_swap_discard()` / `guest_swap_forget_slab()` | free compressed blobs |
| scheduler preempt tick | `guest_swap_safe_point()` | compress and release cold pages |

### 2.1 The only eviction point

Eviction runs **only** from `guest_preempt_tick()` in `win32_thread.c`, i.e.
between IR/AOT blocks (`ir_exec.c:1633`, `ir_container.c:775`,
`n3ds_ir_yield_poll()`). At that point `exec_block()` has returned: the CPU
holds guest *addresses*, never host pointers, and the decoded block `b` is a
host struct. Lifted AOT bodies call `guest_ptr()` per memory access, so no raw
pointer survives a block label.

`guest_ptr()` itself never evicts. If a page must be backed and the recycled
block pool is empty, it grows within `GuestMemory.limit`; when that also fails
the access returns `GUEST_FAULT_NO_MEMORY`. A pointer returned by an earlier
`guest_ptr()` therefore stays valid for as long as the caller keeps it. The
safety rule is enforced by construction, not by a flag: there is no eviction
call on the `guest_ptr` path.

### 2.2 Compressed store

* Vendored **LZ4 1.9.4 block format** (`third_party/lz4/`, BSD-2-Clause), fast
  compressor with a heap-allocated `LZ4_stream_t` (no 16 KiB stack frame).
* One blob per page, indexed by an open-addressing table keyed by
  `(slab slot << 28) | page`. Tombstones; table doubles at 70 % load.
* A page whose compressed size is `>= 4096` is **not** swapped
  (`fail_incompressible`): it would cost host bytes without saving any.
* Compressed pool bounded by `N3DS_SWAP_BUDGET_MB` (default 32 MiB). When the
  pool is full eviction stops (`fail_budget`) and pages stay resident; the
  guest keeps running with its normal limit.
* Freed 4 KiB blocks go to a 64-block (256 KiB) recycle pool, the rest returns
  to `malloc()` so the savings are real for the rest of the process.

### 2.3 Policy

* Only **anonymous slabs** are swapped. File-backed pages (PE `.text`/`.rdata`)
  already page through the provider window (`f43a537`) and duplicating them
  would double the work.
* CLOCK approximation of LRU: each access sets `age[p]=1`; a safe-point pass
  clears it and evicts on the next visit. `target = N3DS_GUEST_TARGET_MB`
  (default 32 MiB) bounds resident anonymous pages.
* Pinned pages and pages sharing a cross-page bridge allocation are skipped.
  Bridges exist only for a multi-page `guest_ptr()` request; the page-aware
  copy paths use single pages, so the normal heap traffic is fully evictable.
* Thread stacks, TEB/TLS, the `GuestHeap` CRT region and every other
  `GuestRegion` are **not** swapped: they are the live execution context. This
  is deliberate: they cannot be moved without a page-table rewrite, and they
  are also the pages most likely to be hot.

### 2.4 Configuration

| Variable | Meaning | Default |
|---|---|---|
| `N3DS_SWAP` | `1` enables, `0`/unset disables | off on host, **on** on device |
| `N3DS_SWAP_BUDGET_MB` | compressed pool ceiling | 32 |
| `N3DS_GUEST_TARGET_MB` | resident anonymous-page ceiling | 32 |

Device: `tesv_slice_main.c:swap_start()` enables swap before `n3ds_boot_prepare`
so every slab allocated during prepare is covered; `N3DS_SWAP=0` in
`sdmc:/Skyrim_N3DS.cfg` turns it off for A/B runs. The run logs
`[swap] enabled: ...` and a final `[swap] out=... in=... ratio=...` line;
`mem_stats` carries `swap= out= in= resident= target= compressed= ratio_pct=
fail_nomem= fail_budget= fail_incompressible=`.

## 3. Measured compression (LZ4 1.9.4, 4 KiB pages)

### 3.1 Real image pages (file bytes of `E:\harness_skyrim\TESV.exe`)

| Section | Pages | Compressed / raw | Ratio | Best page | Worst page |
|---|---:|---:|---:|---:|---:|
| `.text` | 3177 | 9,515,710 / 13,004,592 | **73.1 %** (1.37x) | 621 | 3721 |
| `.rdata` | 463 | 996,867 / 1,896,448 | **52.6 %** (1.90x) | 701 | 4114 |
| `.data` | 167 | 227,791 / 684,032 | **33.3 %** (3.00x) | 400 | 3963 |
| `.rsrc` | 183 | 72,354 / 749,568 | **9.7 %** (10.4x) | 81 | 1991 |
| `.reloc` | 267 | 744,956 / 1,093,632 | **68.1 %** (1.47x) | 26 | 4114 |

The guest's *runtime* `.data`/BSS pages are mostly zero (touch data:
9.52 MiB virtual `.data`, 9.18 MiB touched), and zero pages compress to ~24 B,
so the anonymous mix is much better than the raw `.data` row suggests. The
synthetic smoke profile (structured heap pages) measures **25.3 %** (3.95x).

### 3.2 Host slice run

Command (from the task): `tools\fast_slice.ps1 -Reslice -NoShaderSkipProbe -OutDir
E:\ram_swap -Irc E:\census_ir2\ir_container\tesv.irc -HangMs 2000 -TimeoutSec 900`
with `N3DS_SWAP=1 N3DS_GUEST_TARGET_MB=16 N3DS_SWAP_BUDGET_MB=32`. The run stops at
the same pre-existing fault as the baseline (`0x0802fa80`, missing lifted D3DX
body table), so the comparison is like-for-like.

| Metric | swap off | swap on | Delta |
|---|---:|---:|---:|
| `guest_used` at fault | 44,374,252 | 35,284,239 | **-9,090,013 B (-8.67 MiB, -20.5 %)** |
| `guest_peak` | 44,374,252 | 40,047,887 | **-4,326,365 B (-4.13 MiB, -9.7 %)** |
| `va_backed_live` | 33,271,808 | 24,023,040 | **-9,248,768 B (-8.82 MiB, -27.8 %)** |
| pages evicted (`swap_out`) | 0 | 2,413 | |
| pages restored (`swap_in`) | 0 | 155 | |
| resident anon pages / target | – | 4,097 / 4,096 | |
| compressed live | 0 | 3,076,446 B | |
| compressed / evicted bytes | – | 3,076,446 / 9,883,648 = **31.1 %** (3.21x) | |
| wall time to the same fault | 76.2 s | 77.4 s | **+1.6 %** (limit +30 %) |

Both runs stop at the same pre-existing fault (`0x0802fa80`, missing lifted D3DX
body table) and log the same 49,317 IR blocks, so the comparison is
like-for-like.

The live-pool ratio is 3.21x. `swap_ratio_pct=305` in `mem_stats` is the
cumulative `bytes_out / compressed_total` (3.05x), slightly lower because the
first pages evicted were the most compressible. The net host RAM saved is
`evicted bytes - compressed live` = 9,883,648 - 3,076,446 = **6.49 MiB**, while
`guest_used` falls by 8.67 MiB: compressed blobs are host allocations outside
`GuestMemory.used`, so the mem line alone overstates the win by the pool size.
The device profile (32 MiB target, 32 MiB pool) is the same policy applied to a
smaller budget.

## 4. Tests

`tests/guest_swap_smoke.c` (host, `tools/run_host_tests.ps1 -Only guest_swap`):

* LZ4 round trip on 64 random/zero pages, byte-exact;
* evict 12+ of 16 pages, restore every page byte-exact, `pages_in == pages_out`;
* write after page-in survives another eviction (dirty re-compression);
* random (incompressible) pages stay resident and unchanged;
* **held-pointer test**: `guest_ptr()` on 8 pages with a 1-page target does
  zero swap-outs, and the held pointer still reads the written bytes; only
  `guest_swap_safe_point()` evicts;
* compressed-pool exhaustion keeps pages resident, `fail_budget > 0`;
* host-block exhaustion returns `-1` + `GUEST_FAULT_NO_MEMORY` with the
  "out of memory" text and `fail_nomem > 0` (no silent corruption);
* decommit releases the compressed blobs.

Host suite (2026-11-28, worktree `ram-swap`): **208 passed, 1 diagnostic
(`tesv_slice`, expected with `-AllowDiagnosticFault`), 0 failed**. The two
IR-container Python tests were part of the change (their private build lists now
link `guest_swap.c` + `lz4.c`).

Existing suites must stay green: `guest_paging`, `host`, `thread*`, `com`,
`lift` and the full host suite. Every build list that compiles `guest.c` now also
links `source/guest_swap.c` + `third_party/lz4/lz4.c`:

* `Makefile` (`lz4.o`, `guest_swap.o` in every OFILES, VPATH/INCLUDE entry);
* `tools/run_host_tests.ps1` (`$legacySources` and all ad-hoc `source/guest.c`
  entries);
* `tests/ir_container_test.py` and `tests/ir_container_growth_test.py` (their
  private link commands);
* `tools/arm_container_compile.ps1` and `tools/alloc_probe_nod3d.ps1`.

ARMv6K compile check: `guest.c`, `guest_swap.c` and `lz4.c` build clean for
`-march=armv6k` with the production `__3DS__` define (devkitARM gcc).

## 5. Limitations and next steps

* **Host RAM only.** Nothing is written to the SD card. The pool is still app
  memory, so the win is `4096 - compressed_size` per evicted page, not a free
  page. Zero-heavy pages win ~99 %, code-like pages ~27 %.
* **No region swap.** Thread stacks/TEB/TLS/CRT heap are excluded. Converting
  `GuestRegion` to chunked page backing would make them swappable too, at the
  cost of a bridging path on every multi-page `guest_ptr()`.
* **Bridges are not evictable.** A guest call that returns a pointer spanning
  many pages (`rep movs` fast path) pins that allocation until its pages are
  released.
* **No writeback.** There is no dirty-page store to disk; an evicted page is
  the only copy, so `guest_swap_discard()` on decommit destroys it (that is the
  correct decommit semantics).
* **Cost.** One safe-point pass compresses up to 8192 pages; LZ4 runs at
  hundreds of MB/s, so a full 32 MiB target costs ~10–30 ms of host CPU, paid
  between blocks. The host A/B run measures the wall-clock price.
* If the device still OOMs with the pool full, the next step is an SD-spill
  tier behind `guest_swap_fetch()`: the same blob format, `fopen`-backed,
  bounded, with an explicit `swap_io` metric. It is not enabled by default
  because SD latency (and card wear) is unbounded.
