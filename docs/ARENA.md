# ARENA — the engine's 85 MiB `Default Heap` pre-commit, measured

Status: **measured, analysed, and fixed in the VA layer** (2026-11-24). The
baseline runtime numbers below are from the host x86_64 slice
`E:\Skyrim_N3DS_build\host\run_tesv_slice_14384\tesv_slice.exe`
(`E:\skyr\TESV.exe`, `TESV_MEMPROBE=1`, guest budget 169,000,000 B) at the old
first fault `Guest fault at 00000014`. The fix keeps Win32 reserve/commit
semantics, uses non-contiguous demand-zero pages on 3DS, and advances the host
frontier beyond the allocator; post-fix evidence is in `docs/MEMORY_BUDGET.md`
and `docs/PORT_LOG.md`. Helpers are `tools/arena_*.py` and
`tools/arena_measure.ps1`.

## 0. Answers first

| Question | Answer | Decisive evidence |
|---|---|---|
| Fixed requirement or growable pool? | **Growable pool with a large *initial commit*.** `0x5500000` is the `Default Heap` constructor's pre-commit into a **256 MiB reservation**; the allocator commits more on demand (§5). | one `push 0x5500000` in the whole image (0x687e9f); pool `+0x20 = 0x10000000`, `+0x24 = +0x28 = 0x5500000`; growth path `0xa4f734…0xa4f759` and per-block commit `0xa4f870`; with the initial commit forced to 0 the engine commits **4 KiB** and allocates normally (§6.2). |
| Can it be split into several pools without guest changes? | **Host-side yes and already happens (49 host chunks). Guest-side no.** The guest sees one contiguous range and the allocator's arithmetic assumes it. The engine already has three independent arenas, so splitting adds nothing to RAM. | §1.1 (three ctors), §3.1 (region table), §7.2. |
| Serve it with less real memory honestly? | **Yes — by giving the engine a smaller initial commit and letting its own allocator grow**, not by pretending 85 MiB is committed. Measured: same fault, guest footprint 30.6 MiB instead of 115.6 MiB. Two shim divergences must be fixed first (§6.1). | §6; 121,249,792 B floor today vs 32,124,928 B with a lazy Default Heap. |
| Honest smaller budget / what breaks first | Guest **~32 MB** (image 25.5 + 2.4 DLLs + 2.7 heap/TEB/stack + 1 page arena) instead of 121 MB. First breakages: the shim's recommit/alignment rejection, then the **64-entry host-region table**, then the 3DS newlib heap that cannot `calloc` 85 MiB. | §6.2, §7.4, §8. |

The single most important result is negative: **the 85 MiB is not what the run
is blocked on.** At the fault it contains **0 allocated bytes and 0 written
pages**; the run dies earlier, in the shim, on the allocator's first
re-commit of pages it already owns (§6.1). The 85 MiB is a budget problem, not
a correctness problem.

## 1. Where `0x5500000` comes from — static

`0x5500000 = 89,128,960 B = 85.000 MiB` appears exactly once in the image, as
an immediate:

```
00687e96  6a01              push 1                 ; arg5: commit now
00687e98  6a00              push 0                 ; arg4: alignment 0
00687e9a  68c8d30c01        push 0x10cd3c8         ; arg3: "Default Heap"
00687e9f  6800005005        push 0x5500000         ; arg2: INITIAL COMMIT = 85 MiB
00687ea4  c1e714            shl edi, 0x14          ; 0x100 or 0x200 << 20
00687ea7  57                push edi               ; arg1: RESERVATION 256/512 MiB
00687ea8  8bc8              mov ecx, eax
00687eaa  e8415e3c00        call 0xa4dcf0          ; RegionAllocator::RegionAllocator
```

`tools/arena_xref.py 0x5500000 --imm` finds no other site. The reservation
argument is `0x100 << 20 = 256 MiB` in this run (512 MiB when
`[0x1289834] == 0`); the runtime pool confirms `+0x20 = 0x10000000`.

### 1.1 The constructor chain

`0x687e20` is the heap-setup half of the engine's global memory manager. It is
called from the manager constructor `0xa489b0`, immediately after the 512 MiB
`GlobalMemoryStatusEx` gate at `0xa48a5d`:

```
00a48a92  mov ecx, esi
00a48a94  call 0x687e20        ; <-- creates the three arenas
00a48a99  mov ecx, esi
00a48a9b  mov byte [esi], 1
00a48a9e  call 0x687b00
```

`0x687e20` allocates a 12-byte table from itself (the manager), then three
allocator objects and stores them in that table:

| Slot | Object | Ctor | Size | Reserve | Initial commit | Name string |
|---|---|---|---:|---:|---:|---|
| `cell[0]` | Default Heap | `0xa4dcf0` (RegionAllocator) | 0x158 | **0x10000000 (256 MiB)** | **0x5500000 (85 MiB)** | `0x10cd3c8 "Default Heap"` |
| `cell[1]` | Static Heap | `0xa4db10` (FixedHeap) | 0x24 | 0x40000 (256 KiB) | = reserve (fixed) | `0x10cd3bc "Static Heap"` |
| `cell[2]` | File Heap | `0xa4dcf0` (RegionAllocator) | 0x158 | 0x4000000 (64 MiB) | 0x200000 (2 MiB) | `0x10cd3b0 "File Heap"` |

That the *same* class gets **85 MiB** for `Default Heap` and **2 MiB** for
`File Heap` is the first proof that the number is a heuristic warm-up size,
not a requirement of the allocator: both grow to their reservation the same
way. `0xa4db10` is a fixed arena (`VirtualAlloc(RESERVE)` then
`VirtualAlloc(COMMIT)` of the whole size, no growth), which is why 256 KiB is
enough there.

### 1.2 `RegionAllocator` (0x158 bytes, vtable `0x110d730`)

`0xa4dcf0` calls `0xa4f000` (field init) and then `0xa4f070`, which performs
the first reserve+commit through `vtbl+0x3c`:

```
00a4f070  mov ecx, [esi+0x28]      ; committed size
00a4f073  mov eax, [esi]           ; vtable
00a4f078  mov edx, [esi+0x20]      ; reservation size
00a4f07b  mov eax, [eax+0x3c]      ; = 0xa4de80 (reserve + commit)
00a4f07e  push ecx
00a4f07f  push edx
00a4f082  call eax                 ; VirtualAlloc(NULL,reserve,RESERVE);
                                   ; VirtualAlloc(base,commit,COMMIT)
00a4f084  mov [esi+0x3c], eax      ; base, NULL => fatal exit
```

Fields (all confirmed at runtime, §3.2):

| Off | Meaning | Value at the fault |
|---:|---|---:|
| `+0x00` | vtable | `0x110d730` |
| `+0x04` | back-pointer/owner tag | `0x012de5a8` |
| `+0x10` | name (ASCII) | `"Default Heap"` |
| `+0x14` | small-bin count | `0x20` (32) |
| `+0x18` | commit granularity (offset, shift basis) | `0x10000` |
| `+0x1c` | zero | 0 |
| `+0x20` | **reservation size** | `0x10000000` (256 MiB) |
| `+0x24` | configured initial commit | `0x05500000` (85 MiB) |
| `+0x28` | **committed watermark** (grows) | `0x05500000` (85 MiB) |
| `+0x2c…+0x38` | counters/watermarks | 0 |
| `+0x3c` | **base VA** | `0x40000000` |
| `+0x40` | region/block count | 0 |
| `+0x44` / `+0x48` | first / last block | 0 |
| `+0x4c` | free-block count | 0 |
| `+0x50` / `+0x51` | commit-now / init flags | 1 / 0 |
| `+0x54…+0xd3` | 32 small-bin free-list heads | all 0 |
| `+0xd4…+0x153` | 32 large-bin free-list heads | all 0 |
| `+0x154` | destroyed flag | 0 |

Relevant vtable slots (dumped from `0x110d730`):

| Slot | Function | Role |
|---|---|---|
| `+0x10` | `0xa4fc70` | allocate(size), flag 1 |
| `+0x18` | `0xa4fc90` | allocate(size), flag 0 |
| `+0x34` | `0xa4eff0` | "can serve?" → `true` |
| `+0x3c` | `0xa4de80` | **reserve + initial commit** |
| `+0x40` | `0xa4ded0` | `VirtualFree(base, 0, MEM_RELEASE)` |
| `+0x44` | `0xa4def0` | **commit inside the reservation** (`base`, `old`, `delta`) |
| `+0x48` | `0xa4df30` | decommit with preserve |

`0xa4de80` is exactly:

```
00a4de80  VirtualAlloc(NULL,  reserve, MEM_RESERVE, PAGE_READWRITE)
00a4dea6  VirtualAlloc(base,  commit,  MEM_COMMIT,  PAGE_READWRITE)
```

so the engine's own design is already `reserve` then `commit`.

## 2. The memory manager (0x1b418b0) and the 12-byte cell

`0x1b418b0` is the engine-global memory manager (an image global in the
`0x00400000` module region), not the arena itself. Runtime fields:

| Off | Value | Meaning |
|---:|---:|---|
| `+0x00` | `0x00030000` | flag byte + version word 3 (set by `0x687e20`) |
| `+0x04` | 0 | |
| `+0x08` | `0x30000a20` | **12-byte heap table** |
| `+0x10` | 0 | per-thread current-pool array (indexed by `[thread+0x4ac]`) |
| `+0x14` | 0 | Static Heap (not created yet) |
| `+0x18` | `0x30000d10` | **Default Heap** (`= cell[0]`) |
| `+0x19c`, `+0x1c` | 0 | Static Heap aliases |
| `+0x214` | 0 | optional small/large sub-allocator |
| `+0x220` | `0x00000101` | byte `+0x220 = 1` initialised, `+0x221 = 1` "route via pools" |
| `+0x22c` | 4 | |
| `+0x234` | `0x24` | last requested size (set just before the failing grow call) |
| `+0x30` | 0 | File Heap (not created yet) |

The 12-byte cell lives at `0x30000a20` in the CRT heap (a `HeapCreate` guest
heap at `0x30000000`):

```
cell[0] Default Heap = 0x30000d10
cell[1] Static Heap  = 0x6f646e69   <- uninitialised bytes ("indo")
cell[2] File Heap    = 0x4e5f7377   <- uninitialised ("ws_N")
```

The engine does not zero this allocation (`0xa48f1b` → CRT allocation), which
is why the two unfilled slots contain stale heap bytes. The constructor dies
before it fills them, so `cell[1]`/`cell[2]` are *expected* to be garbage at
this fault.

## 3. Runtime state at the fault

`arena_dump` (lldb Python, §9) at `rt_unimpl`, frame `run()` of
`tesv_slice_main.c`, budget 169,000,000.

### 3.1 GuestMemory and the commit

```
GuestMemory: regions=64 used=121,249,792 (115.6 MiB) limit=169,000,000 (161.2 MiB)
```

64 = **the table is full** (`GUEST_MAX_REGIONS`). Region `[15]…[63]` are the
arena:

```
[15] 40000000 + 1,835,008 (1.750 MiB)
[16] 401c0000 + 1,835,008
...
[62] 45240000 + 1,835,008
[63] 45400000 + 1,048,576 (1.000 MiB)
     -------------------------------------------------
     49 regions, 89,128,960 B = 85.000 MiB exactly
```

The 49 chunks are **an artifact of our shim**, not 49 engine calls. In
`source/win32_shims.c` `va_commit()` sizes each host mapping as
`max(512 KiB, roundup(need / available_regions, 64 KiB))`. At commit time 15
regions were used, so `avail = 49`, `need = 89,128,960`,
`roundup(need/49, 64 KiB) = 0x1C0000 = 1,792 KiB`, and
`48 × 1,835,008 + 1,048,576 = 89,128,960`. The probe's `[mem] va: 49 calls,
87040 KiB` is therefore 49 `mem_track` calls for **one** guest commit.

A native breakpoint on the shim shows exactly three `VirtualAlloc` calls in
the whole run:

```
s_VirtualAlloc(00000000, 10000000, 00002000, 00000004)   reserve 256 MiB
s_VirtualAlloc(40000000, 05500000, 00001000, 00000004)   ONE commit of 85 MiB
s_VirtualAlloc(40000000, 00000040, 00001000, 00000004)   first 0x24 allocation
```

and the lifted pool's reserve+commit entry is called once:
`f_A4DE80(reserve=0x10000000, commit=0x0550000)`. So the answer to "one commit
or incremental" is: **one 85 MiB `MEM_COMMIT`, split into 49 host mappings by
the port's region budget.**

### 3.2 The pool object and its contents

`0x1b418b0+0x18 → 0x30000d10` (the `Default Heap`, 0x158 bytes):

```
+000 0110d730  +010 010cd3c8 "Default Heap"  +014 00000020
+018 00010000  +020 10000000  +024 05500000  +028 05500000
+02c 00000000  +030 00000000  +034 00000000  +038 00000000
+03c 40000000  +040 00000000  +044 00000000  +048 00000000
+04c 00000000  +050 00000001  +051 00000000
+054…0x153  64 free-list heads, all zero
+154 00000000
```

**No allocation has ever been served from this arena**: `+0x40` (block count)
is 0, both block list pointers are 0, and every free-list head is 0. The
0x24-byte `Static Heap` object allocation is the one that dies.

### 3.3 TLS and the null thread object

```
guest fs_base                  = 0x18000000
[0x1bbeb54] (TLS index)        = 0
fs:[0x2c] (TLS pointer array)  = 0x18001000
TLS[0]                         = 0x18200000
[0x1b418a0]                    = 0
[0x1b2e860] (current thread)   = 0
```

`0xa48c80` → `0x687b10` does `GetCurrentThreadId()` then
`cmp eax, [[0x1b2e860]+0x14]`; `[0x1b2e860]` is 0 here, so it faults at
`0x14`. This is the *symptom* only: on Windows the same global is also zero at
this point (PORT_LOG row 33), and the real difference is that on Windows the
earlier pool allocation **succeeds** and the grow path is never taken (§6.1).

## 4. Usage vs reservation at the fault

Two independent measurements agree.

1. **Allocator state**: block count `+0x40 = 0`, free lists empty, so the
   arena has handed out 0 bytes. The engine allocated 0x24 bytes from it only
   in the *lazy* run (§6.2).
2. **Page scan**: every 4 KiB page of all 49 committed chunks was read
   through the debugger (bounded per chunk; 85 MiB total) and checked for
   non-zero bytes. Result: **0 / 21,760 pages non-zero**.
   A stride-16 sample read only 5.31 MiB (1,360 pages) and also found 0; with
   0 hits the 95 % upper bound by the rule of three is `3/1360 = 0.22 %` of
   pages ≈ 0.19 MiB, so the full scan is quoted instead. `calloc` gives zero
   pages and the guest is the only writer, so non-zero ⇔ the guest wrote it.
   (Reads alone are not detected; for a heap that is harmless because block
   metadata is always written before a block is handed out.)

So at this point the 85 MiB is **100 % untouched**. It is not "working set
that the log rounds up"; it is a single eager commit of 85 MiB of zero pages,
73.5 % of the 115.6 MiB guest footprint.

One nuance about "cost": on the host, `calloc` may hand back demand-zero pages
that the OS does not materialise until written, so the host's RSS can be lower
than 85 MiB. That does not make the commit free. The port charges every
committed byte against the guest budget (`mem->used`), which is why a
121,000,000-byte budget fails with "pool has 86796 KiB free" (§6.2), and on the
3DS the same commit is a real allocation from `__ctru_heap_size`. The
measurement above is the cost the guest actually creates, not the host's page
cache behaviour.

## 5. The allocator really grows (static)

`0xa4f490` is the allocation core. Two growth mechanisms exist:

**Watermark growth** — commit the difference between the committed watermark
and the new end, capped by the reservation:

```
00a4f734  cmp dword [edi+0x18], 0     ; commit granularity configured?
00a4f738  je  0xa4f75c
00a4f73a  mov eax, [edi+0x28]         ; committed
00a4f73d  cmp ebp, eax
00a4f73f  jbe 0xa4f75c                ; still inside
00a4f741  cmp ebp, [edi+0x20]         ; end <= reservation?
00a4f744  ja  0xa4f75c
00a4f746  mov edx, [edi]
00a4f748  mov edx, [edx+0x44]         ; = 0xa4def0, commit-at-offset
00a4f74d  sub ecx, eax                ; delta
00a4f757  call edx                    ; VirtualAlloc(base+old, delta, COMMIT)
00a4f759  add [edi+0x28], eax         ; watermark += committed bytes
```

**Per-block growth** — when extending at the end of the used range, commit
exactly the new block:

```
00a4f861  mov ebx, [edi+0x30]         ; used offset
00a4f864  add ebx, [edi+0x3c]         ; base + used
00a4f86e  push esi                    ; block size
00a4f86f  push ebx
00a4f870  call VirtualAlloc           ; MEM_COMMIT, PAGE_READWRITE
```

Both are bounded by `+0x20` (reservation), never by `+0x24` (initial commit).
The initial 85 MiB only decides how many of the first allocations avoid a
syscall. No request is compared against the literal `0x5500000`: the only
bounds the allocator checks are the reservation (`+0x20`) and the current
watermark (`+0x28`).

## 6. Measurement: what happens with a smaller initial commit

The engine's code is lifted C with the immediate baked in
(`E:\Skyrim_N3DS_build\analysis\g12_new\tesv_04.c:323195`,
`v_ = 0x5500000u`), so patching the PE has no effect. The experiment was done
by rewriting the guest-stack argument of `0xa4de80` at its first call
(`tools/arena_dump.py`, `arena_patcharg`). Nothing on disk changed.

### 6.1 First, an independent blocker: the shim rejects legitimate re-commits

`VirtualAlloc` semantics on Windows: a `MEM_COMMIT` address may be only page-
aligned (it is rounded down to a page), and committing pages that are already
committed is legal. The port's shim requires **64 KiB** alignment and refuses
any commit whose range overlaps an existing host mapping
(`source/win32_shims.c:721` and `:727`). Both paths were hit here:

* With the real 85 MiB commit, the first 0x24 allocation asks for
  `VirtualAlloc(0x40000000, 0x40, MEM_COMMIT)`. `va_host_overlaps(0x40000000,
  0x1000)` now sees the 85 MiB host mapping and returns 1 → the shim returns
  NULL, silently (`ERR_INVALID_ADDRESS`). Native trace confirms:
  `va_host_overlaps(40000000, 00001000)`.
* `MemMgr::Alloc` (`0xa48d60`) gets NULL from the pool and falls back to the
  grow path `0xa48c80` → `0x687b10` → `[0x1b2e860] == 0` → **fault 0x14**.

This is the same recommendation as PORT_LOG row 33 but with the exact
mechanism: it is not the *size* of the 85 MiB that kills the run, it is the
re-commit of a range the shim has already mapped.

A second allocation would hit the alignment rule: after the first block the
pool watermark `+0x30 = 0x40`, so the next commit is
`VirtualAlloc(0x40000040, 0x170, MEM_COMMIT)` — legal on Windows (rounded down
to 0x40000000), rejected by `:721` here. Observed in the lazy run below.

### 6.2 Results

| Initial commit | Committed | Guest `used` | Regions | First 0x24 alloc | Fault |
|---:|---:|---:|---:|---|---|
| 85 MiB (real) | 85.00 MiB | 121,249,792 B (115.6 MiB) | 64/64 | fails (recommit rejected) | 0x14 |
| 1 MiB (patched arg) | 1.00 MiB | 33,169,408 B (31.6 MiB) | 17/64 | **fails** (recommit rejected) | 0x14 |
| 0 (patched arg) | **4 KiB** | **32,124,928 B (30.6 MiB)** | **16/64** | **succeeds** (0x40000010) | 0x14 at the *next* alloc |

In the 0-commit run the `Default Heap` still works: the per-block path at
`0xa4f870` commits exactly one 4 KiB page, the `Static Heap` object is placed
at `0x40000010`, `cell[1] = 0x40000010`, and the run proceeds to the next
allocation (the 0x158 `File Heap`), where the unaligned re-commit above kills
it. So **the engine needs no pre-commit at all to make progress**; it needs
the port to honour Windows' commit rules.

Budget floor today, measured by bisection:

```
budget 121,249,792 -> reaches the 0x14 fault (0 KiB free)
budget 121,000,000 -> "VirtualAlloc commit failed: 87040 KiB (pool has 86796 KiB free,
                       15/64 host regions used, 49 chunks of 1792 KiB; limit: pool)"
                      then fault 0x2c (pool constructor aborts)
```

## 7. The four questions, with evidence

### 7.1 Is the 85 MiB a fixed requirement or a growable pool with a lower real working set?

**A growable pool with a large initial commit.** `0x5500000` is argument 2 of
the `Default Heap` constructor, stored at `+0x24` and copied to the committed
watermark `+0x28`. The reservation is `+0x20 = 256 MiB`. Two growth paths
exist (`0xa4f734`: commit `needed - committed`, `0xa4f861`: commit the new
block), both capped by `+0x20`, none by `+0x24`. The sibling `File Heap` uses
2 MiB for the same class with a 64 MiB reservation. At the fault the arena's
real working set is **0 bytes / 0 pages**; with the initial commit forced to 0
the whole run fits in **30.6 MiB** of guest memory.

### 7.2 Can it be split (several independent pools) without changing guest code?

**Host-side yes, guest-side no, and it does not save RAM.**

* Host-side: the port already splits the single guest commit into 49 host
  allocations (`guest_map` = `calloc` per chunk). That is invisible to the
  guest and already implemented — but it is what exhausts the 64-entry region
  table, so it is the *wrong* kind of splitting (§8).
* Guest-side: the guest sees one contiguous 256 MiB reservation based at
  `0x40000000`. The allocator's arithmetic (`base + [0x30]`,
  `committed <= reserve`, `base + committed`, block headers containing
  region-relative spans) assumes one range. Two reservations at different
  bases would return pointers the pool cannot size or free.
* The engine already has three pools (`Default`, `Static`, `File`) and routes
  between them (`0xa48d60` uses the per-thread slot `+0x10` or `+0x18`, and
  `+0x214` as a sub-allocator). Splitting the `Default` pool further would
  require changing `0x687e20` — guest code.

### 7.3 Can the port serve it with less real memory honestly?

**Yes, but "lazy commit" has one precise meaning here.** Our `VirtualAlloc`
separates reserve (free) from commit (real host `calloc`); it cannot hand the
guest a "committed" page that has no backing, because the guest will write
into it and the 3DS has no MMU to fault it in. The honest lever is that the
*engine already commits on demand*: give it a small initial commit and it
commits the high-water mark itself. Measured: 85 MiB → 0 bytes used;
0 initial commit → 4 KiB for the first block, total 30.6 MiB, same fault.

Two port bugs must be fixed for that to be usable (observation only, left to
their owners):

1. `win32_shims.c:727` — a `MEM_COMMIT` inside an existing allocation must
   not be rejected for overlapping that allocation (and re-committing is a
   no-op success, not a failure). This is what fails today with any non-zero
   initial commit.
2. `win32_shims.c:721` — `MEM_COMMIT` addresses are page-granular on Windows
   and rounded down; only reservations need 64 KiB granularity. This fails
   the second block (`0x40000040`).

### 7.4 Honest smaller budget and what breaks first

Guest floor with a lazily grown `Default Heap` (measured, 0-commit run):

| Item | Bytes | MiB |
|---|---:|---:|
| TESV image | 26,771,456 | 25.53 |
| d3dx9/bink/X3DAudio/steam data | 2,469,888 | 2.36 |
| PEB/TEB/TLS/stack/guest heaps | 2,879,488 | 2.75 |
| Default Heap first page | 4,096 | 0.004 |
| **Guest total** | **32,124,928** | **30.63** |

versus **121,249,792 B (115.63 MiB)** today. A budget with a 2 MiB initial
commit (same as `File Heap`) would be ~34 MiB guest, leaving room to grow to
the reservation under real load.

Budget arithmetic (host current text size, `docs/PORT_LOG.md` row 36):

| Build | ARM text | Guest | Linear | Rodata | Total | 178 MiB? | 96 MiB `.3dsx`? |
|---|---:|---:|---:|---:|---:|---|---|
| today | 69.8 | 115.6 | 8 | 1.1 | **194.5** | no | no |
| honest lazy arena | 69.8 | ~32 | 8 | 1.1 | **110.9** | **yes** | no (≈15 MiB over) |
| lazy arena + IR cold tier (~30 MiB text) | ~30 | ~32 | 8 | 1.1 | **71.1** | yes | yes |

What breaks first if the initial commit is simply lowered (ordered):

1. **The shim's re-commit rejection** (`:727`) — immediate, at the first pool
   allocation, for *any* non-zero commit. This is the current fault.
2. **The shim's 64 KiB alignment of `MEM_COMMIT`** (`:721`) — at the second
   block (`0x40000040`).
3. **The 64-entry host-region table.** 85 MiB consumes 49 of 64 slots by
   construction, and `guest_map` fails after that. The chunking formula always
   tries to fill the remaining slots, so *any* large commit leaves zero
   headroom. The fix is the arena design already prescribed in
   `docs/MEMORY_BUDGET.md` §2: one reservation-backed region plus a committed-
   page bitmap, not one host allocation per chunk. Lowering the initial commit
   without this just trades 49 chunks for one chunk per block.
4. **The 3DS heap.** Guest mappings are `calloc`'d from `__ctru_heap_size`
   (current 64 MiB, diagnostic guest budget 60 MiB). An 85 MiB commit cannot be served on device at
   all; the host only succeeds because it has 85 MiB of real RAM. With a 2 MiB
   initial commit the commit fits the pinned heap, but the guest still needs
   the arena+bitmap rework before the high-water mark can exceed the heap.
5. **The 512 MiB `GlobalMemoryStatusEx` gate** is unaffected; it still needs
   the probe (or a real backing store) regardless of the arena size.

## 8. Recommendation

1. **Treat `0x5500000` as a tuning constant, not an engine requirement.** It is
   one immediate with one reference in the image; the allocator's contract is
   the 256 MiB reservation and the two growth paths.
2. For the device, specialise the `Default Heap` initial commit to **2 MiB**
   (matching `File Heap`) and record it as an explicit guest-image
   specialisation, not a silent change. Keep the 256 MiB reservation — it is
   free.
3. Rework the VA layer to **one arena reservation + committed-page bitmap**
   (MEMORY_BUDGET §2) before any real run, otherwise the 64-region table is
   the next wall no matter what the arena size is.
4. Fix the two `VirtualAlloc` divergences (`:721`, `:727`) so the engine's
   own per-block commits work; that is what unblocks the current `0x14` fault,
   not the arena size.
5. Do not claim "the engine needs 85 MiB". The measured requirement at the
   current fault is **0 bytes**; the measured requirement to make progress is
   **one 4 KiB page**; the 85 MiB is 21,760× that, all zero.

## 9. Reproduction

```powershell
# newest host slice, probe on, 161 MiB budget
powershell -File tools/arena_measure.ps1

# individual pieces (run from the run_tesv_slice_* directory)
$env:TESV_MEMPROBE = "1"
.\tesv_slice.exe E:\skyr\TESV.exe . 200000 169000000

lldb --batch `
  -o "breakpoint set -n rt_unimpl" `
  -o "command script import C:\Users\vasya\Documents\Skyrim_N3DS\tools\arena_dump.py" `
  -o "run E:\skyr\TESV.exe . 200000 169000000" `
  -o "arena_dump 16" -o quit .\tesv_slice.exe

# same, with the Default Heap initial commit forced to 0 on the guest stack
# (add -o "arena_patcharg 0" before run)

# static helpers
python tools/arena_dis.py 0xa4dcf0 0x300
python tools/arena_xref.py 0x5500000 --imm
python tools/arena_calls.py 0xa4dcf0
```

Files added by this work: `tools/arena_dis.py` (range disassembler with IAT
names), `tools/arena_xref.py` (immediate/address xrefs), `tools/arena_calls.py`
(argument windows around calls), `tools/arena_dump.py` (lldb: guest-memory
region table, struct dump, page-usage scan, stack-argument patch),
`tools/arena_measure.ps1` (reproducible runs).

## 10. Appendix — decisive instructions per address

Captured with `python tools/arena_dis.py <addr> <len>`; IAT names from
`functions.json`. These are the instructions the conclusions rest on.

**`0x687e20` — heap setup (`this` = 0x1b418b0)**

```
687e24 push 0x1f4 / lea eax,[esi+0x10]           ; memset(fields,0,0x1f4)
687e3c mov word [esi+2], 3
687e59 push eax (12) / mov ecx,0x1b418b0
687e5f call 0xa48d60                              ; 12-byte heap table
687e64 mov [esi+8], eax
687e7d push 0x158 / call 0xa48d60                 ; Default Heap object
687e96 push 1; push 0; push 0x10cd3c8 "Default Heap"
687e9f push 0x5500000                             ; <-- the 85 MiB
687ea4 shl edi, 0x14                              ; 256/512 MiB reservation
687eaa call 0xa4dcf0
687eb6 mov [cell], eax   ; 687ec8 mov [esi+0x18], eax
687ecb call 0xa48d60 (0x24)                       ; <-- dies here
687ee2 call 0xa4db10 (Static Heap, 0x40000)
687f17 call 0xa48d60 (0x158)                      ; File Heap object
687f29 push 0x200000 (2 MiB); 687f2e push 0x4000000 (64 MiB)
687f35 call 0xa4dcf0
```

**`0xa489b0` — memory-manager constructor**

```
a48a08/a48a0f  byte [esi+0x220]=1, [esi+0x221]=1
a48a5d call [0x106b274] GlobalMemoryStatusEx
a48a6f cmp dword [esp+0x30], 0x20000000           ; 512 MiB gate
a48a94 call 0x687e20                              ; arena creation
a48aa9 mov [edi+0x4ac], eax                       ; restore TLS slot
```

**`0xa4dcf0` — `RegionAllocator` ctor `(reserve, commit, name, 0, 1)`**

```
a4dd13 call 0xa4f000                              ; field init
a4dd1a mov dword [esi], 0x110d730                 ; final vtable
a4dd27 call 0xa4f070                              ; reserve + initial commit
a4dd2f ret 0x14
```

**`0xa4f000` — field init**

```
a4f01e mov [esi+0x10], ecx   ; name
a4f02c mov [esi+0x18], edx   ; 0x10000 granularity
a4f025 mov [esi+0x20], ecx   ; reserve
a4f033/036 mov [esi+0x24],[esi+0x28], ecx   ; commit, commit watermark
a4f061 mov dword [esi+0x14], 0x20
```

**`0xa4f070` — first reserve+commit**

```
a4f073 mov ecx,[esi+0x28] ; a4f078 mov edx,[esi+0x20]
a4f07b mov eax,[eax+0x3c] ; = 0xa4de80
a4f084 mov [esi+0x3c], eax ; base
a4f08b push 0 / call 0xf58b4f  ; fatal when the commit failed
```

**`0xa48d60` — allocation entry `(size, a, b)`**

```
a48d63 mov eax,[0x1bbeb54]            ; TLS index
a48d6c mov ecx, fs:[0x2c]             ; TLS array
a48d76 mov edx,[eax+0x4ac]            ; per-thread allocator index
a48d7c mov ecx,[ebp+0x18]             ; default pool
a48d80 mov edi,[ebp+edx*4+0x10]       ; per-thread pool
a48d96 test ecx,ecx ; je 0xa48f1b     ; no pool -> CRT fallback
a48e53 call [edi]->vtbl[0x10]         ; pool allocate
a48e84 test ebx,ebx ; jne return
a48ea9 call 0xa48c80                  ; grow  <-- fault path
```

**`0xa48c80` — grow / per-thread retry**

```
a48cc7 mov ecx,[0x1bbeb54] ; a48ccd mov edx, fs:[0x2c]
a48cd4 mov esi,[edx+ecx*4]            ; thread object
a48cd7 add [esi+0x4a8], 1
a48cfc call 0x687b10                  ; <-- reads [0x1b2e860] and faults
a48d07 mov [esi+0x4a4], eax
```

**`0xa48f1b` — CRT-heap fallback when `[manager+0x18] == 0`**

```
a48f1b cmp byte [esp+0x2c], bl
a48f21/2b call 0xf587f5 (2 args)      ; zeroing variant
a48f40 call 0xf568fa (1 arg)          ; plain allocation
```

**`0xa4de80` — vtable `+0x3c`, reserve + commit**

```
a4de8c push 4, 0x2000, size, 0 ; call VirtualAlloc   ; MEM_RESERVE
a4dea6 push 4, 0x1000, commit, base ; call VirtualAlloc ; MEM_COMMIT
a4deb5 push 0x8000, 0, base ; call VirtualFree        ; rollback on failure
```

## 11. What this does not settle

* No device measurement: the 3DS side is inferred from `calloc`/newlib heap
  sizes, not observed.
* The long-run high-water mark of the `Default Heap` is unknown: the run stops
  at the same shim bug in every configuration, before resource loading.
  Everything after `0xa4dcf0` (menus, world, BSAs) would commit more.
* Whether the engine ever calls `MEM_DECOMMIT`/`Free` on this pool in the
  startup path was not observed (the fault precedes any free).
* The 512 MiB `GlobalMemoryStatusEx` gate is independent of this arena and
  still requires the probe or a real backing store.
