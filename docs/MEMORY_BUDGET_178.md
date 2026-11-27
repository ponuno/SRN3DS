# 178 MiB device budget — current ledger (2026-11-27)

This is a **ledger of measured facts**, not a plan. It exists so another agent can
do the budgeting without re-deriving numbers. Read it together with
`docs/MEMORY_BUDGET.md`, which investigates the engine's 512 MiB `AvailPageFile`
gate and is still accurate about the gate itself.

Everything below is either read from the tree (`file:line`) or measured on this
machine. Values that are unverified on hardware are marked **[unverified]**.

## 1. Device envelope

| Fact | Value | Source |
|---|---|---|
| Application region (CIA, extended memory) | **178 MiB** | `tools/check_hw_log.py:20` asserts `app_region == 178` |
| CPU / clock flags | 804 MHz, 178 MB mode | `tools/deploy_device.py` (CIA packaging, `f155720`) |
| VRAM on the PICA side | 6 MiB nominal | comment in `source/d3d9_core.c:8` |

## 2. What we pin in the ARM slice today

`source/tesv_slice_main.c`:

| Region | Size | Line |
|---|---:|---|
| newlib application heap `__ctru_heap_size` | **96 MiB** | `:48` |
| linear heap `__ctru_linear_heap_size` | **8 MiB** | `:49` |
| libctru/runtime reserve | **4 MiB** | `:268` comment |
| guest budget requested (`n3ds_boot_init`) | **92 MiB** | `:269-270` |

Important: the guest budget is **not** an additional region. `guest_map()` uses
`calloc()` (`source/guest.c:179`) from the *newlib heap*, so the 92 MiB figure is
a **cap**, and real free heap is

```
96 MiB − mapped guest pages − host indices − libctru allocations
```

The 4 MiB reserve is what keeps the cap from being hit by malloc traffic.

## 3. What the engine actually needs

From `docs/MEMORY_BUDGET.md` (67 logged runs, `E:\Skyrim_N3DS_membudget\REPORT.md`):

| Item | Value |
|---|---:|
| Minimum guest pool that passes the 512 MiB gate with `TESV_MEMPROBE=1` | **44,165,120 B = 42.12 MiB** (44,164,096 B fails) |
| VA reservations requested by the engine | 5 regions, **581.25 MiB** |
| Commit promise | **100.25 MiB** |
| Physically backed at the startup frontier | **4.63 MiB** |
| Budget with probe off | no value passes; arithmetic minimum ≈ **543 MiB** |

So: startup fits in ~42 MiB of real guest pages; sustained play promises 100 MiB
and the engine believes it may commit 512 MiB. That fiction is the core tension.

## 4. Host-side residents in the same 96 MiB heap

These are `malloc`/`calloc` allocations in the slice process, competing with
guest pages:

| Consumer | Size | Source |
|---|---:|---|
| IRC index (90,900 functions × 40 B) | **3.5 MiB** | `tesv.irc` header: `nfun=90900`, `entry_bytes=40` |
| IRC record cache | **4 MiB** (64 records) | `source/ir_container.h`: `N3DS_IRC_DEFAULT_CACHE_BYTES (4u<<20)`, `..._RECORDS 64`; slice calls `n3ds_ir_cache_init(4u<<20)` at `tesv_slice_main.c:176` |
| BSA overlay index (13 archives, measured on `E:\harness_skyrim`) | **~17.5 MiB** | `8a8f743` report; cap is **96 MiB** |
| Stream staged cache | **2 MiB** default | `source/asset_stream.h:46` |
| Stream per-descriptor window | **256 KiB** default | `asset_stream.h:200` |
| Stream quantum | **64 KiB** default | `asset_stream.h:199` |
| Stream rate | **7,000,000 B/s** default | `asset_stream.h:47` |
| XAudio2 guest memory | 8 KiB (`XA_MEM_BYTES 0x2000`) + 64 KiB callback stack | `source/win32_xaudio2.c` |
| `menu_frame` harness guest budget | 4 MiB (host harness only) | `source/menu_frame_main.c:280` |

On-disk (SD, not RAM) for reference: `tesv.irc` is **64,445,696 B = 61.5 MiB**
(index 3.5 MiB + records 58.0 MiB).

Nothing in the tree shrinks or evicts the BSA index today, and the IRC record
cache is a fixed 4 MiB.

## 5. GPU-visible memory: VRAM (6 MiB) and linear heap (8 MiB)

citro3d splits GPU memory explicitly. `C3D_TexInitParams` ends with `onVram`
(`C:\devkitPro\libctru\include\c3d\texture.h:44-51`):

| Call site | `onVram` | Lives in |
|---|---|---|
| `d3d9_citro3d.c:177` (A8 texture) | `true` | **VRAM** |
| `d3d9_citro3d.c:969` (explicit target colour) | `true` | **VRAM** |
| `d3d9_citro3d.c:376,405,447` (game textures, cube, ETC1) | `false` | **linear heap** |
| `C3D_RenderTargetCreate*` framebuffers/depth | — | allocator is inside citro3d, source not in this tree **[unverified]** |

So the 6 MiB VRAM and the 8 MiB linear heap are **two separate ceilings**, and
`vramSpaceFree()` only describes the first. Today `vramSpaceFree()` is logged
(`d3d9_citro3d.c:242,254,268,297,972,…`) but there is no `linearSpaceFree()`
accounting anywhere.

Linear-heap consumers:

| Consumer | Size | Basis |
|---|---:|---|
| Vertex arena `FRAME_VERTICES` × 20 B | **320 KiB** | `d3d9_citro3d.c:317` |
| Colour arena `FRAME_MENU_VERTICES` × 40 B | **2.5 MiB** | `:322` (`D9ColorVertex` = 40 B) |
| Cube arena `FRAME_VERTICES` × 40 B | **640 KiB** | `:327` |
| Index arena `FRAME_INDICES` × 2 B | **512 KiB** | `:328` |
| Skin arena `FRAME_SKIN_VERTICES` × 80 B | **320 KiB** | `:329` |
| **Arenas subtotal** | **≈ 4.35 MiB** | derived from the four constants above |
| All game textures | remainder | ETC1 4 bpp, **ETC1A4 8 bpp** (`d3d9_citro3d.c:445-449`) |
| Depth attachments of explicit targets | — | citro3d-internal **[unverified]** |

The 4.35 MiB of arenas alone consume over half of the 8 MiB linear heap before a
single texture is uploaded. That is the strongest argument for the 24 MiB linear
target below. Note ETC1A4 is **8 bpp**, not 4: a 512×512 alpha texture is 256 KiB,
not 128 KiB, and every mip level counts.

The nine-target 4.5 MiB figure (`d3d9_core.c:7-12`) is an estimate of
colour+depth pairs that are *simultaneously live*; exact placement must be
measured per target. There is **no texture budget today**:
`d9_texcache_init(...,0,0)` at `source/d3d9_core.c:320` passes
`guest_limit=0, gpu_limit=0`, i.e. unlimited. `d9_texcache_set_limits()` exists
(`d3d9_texcache.c:162`) with LRU trim/`budget_trim_evictions`/`evictions_gpu`
accounting (`d3d9_texcache.h:118-124`), but nothing sets a limit in the slice.

## 6. Target profile (2026-11-27 review): 120 MiB newlib + 24 MiB linear

Proposed experiment profile. **96/8 stays the default** until a device route
proves 120/24. The remaining **34 MiB** of the 178 MiB region must hold code,
static data, stacks and every other non-heap cost; that is a precondition, not a
measured result.

CPU budget inside newlib:

| Item | Target MiB | Basis |
|---|---:|---|
| Guest physical memory | **92** | keep the current cap (engine pages + guest-side asset allocations) |
| IRC index | 3.5 | 90,900 × 40 B |
| IRC record cache | 4 | keep until miss rate is measured |
| Decoded-IR cache | 4 | `n3ds_ir_cache_init(4u << 20)` |
| Resident BSA index | **2** | target after paging the index to SD; measured today ≈ 17.5 |
| Stream staged cache | 2 | current default |
| Other host allocations and temporary copies | 8.5 | D3D CPU copies, decode, metadata, load peaks — needs measurement |
| Untouchable newlib reserve | **4** | check at the cell-transition peak |
| **Total** | **120** | |

Consequence: with today's BSA index the same layout loses 15.5 MiB → 76.5 MiB
left for guest under 120 MiB, 52.5 MiB under the current 96 MiB. That is a
simultaneous-peak estimate, not an observed high-water mark, and it explains why
raising `guest_budget` alone does not help. **The first change to make is the BSA
index**: keep an archive catalog and a bounded page cache in RAM, page the rest
from SD, and verify lookup equivalence against the current overlay including mod
priority.

GPU budget for 24 MiB linear:

| Item | Target MiB |
|---|---:|
| Resident game textures (upper bound) | **8** |
| Minimum free linear reserve | **2** |
| Frame arenas, backend resources, linear targets, upload buffers | 14 |

The texture limit must drop below 8 MiB if measured peaks of the other items
exceed 14 MiB. Enforce it through `d9_texcache_set_limits()`, and keep any
resource referenced by already-submitted GPU commands pinned until those commands
retire.

Removed from the plan: **160 MiB physical guest RAM**. With 24 MiB linear it
already exceeds the region. The engine has three different numbers — 42.12 MiB
(gate pass), 4.63 MiB (backed at the fixed start point), 100.25 MiB (promised
commit) — and none of them is a cell working set. If the engine touches the whole
commit, a 92 MiB cap is short by at least 8.25 MiB; passing `AvailPageFile` does
not remove that deficit.

### Device route to accept or reject the profile

1. Run the same route on **96/8**, **104/16**, **120/24**: cold start → menu →
   first exterior cell → neighbour cell → return. Same archives, settings,
   scenario. Record whether each heap initialised.
2. Sample peaks **at allocation time**, including cell transitions: guest
   backed/peak, live/peak for BSA and both IR caches, minimum free
   newlib/linear/VRAM, largest free block where the allocator reports it, and the
   size of every failed request. For streaming: queued bytes, staged bytes, and
   wait time for a mandatory resource. A periodic post-load snapshot misses the
   moment when the old asset, the new copy and the upload buffer are all live.
3. Accept 120/24 only by route result: both heaps created, mandatory allocations
   pass, ≥4 MiB newlib and ≥2 MiB linear free at the peak, resident set plateaus
   on a repeat run. If linear runs out first, reduce mip levels and measure reload
   time. If newlib runs out first, the log must show what evicts guest: BSA, CPU
   copies, or the guest pages themselves.

## 7. Levers, with trade-offs

1. **Raise the linear heap** 8 → 32/48 MiB and lower the newlib heap to
   64/48 MiB. The engine needs ~42 MiB guest at startup, so this is feasible for
   startup; the open risk is the 100 MiB commit promise during play. This is the
   single highest-value lever for textures.
2. **Bound the texture cache** with `d9_texcache_set_limits()`: pick a GPU limit
   (e.g. 4 MiB) and a guest staging limit (e.g. 2 MiB). Eviction is implemented;
   only policy is missing. Needs a "texture used this frame" pin (`no_evict`).
3. **Make the BSA index file-backed or lazy.** 17.5 MiB is the largest single
   host allocation after the guest. Per-archive lazy index (open on first hit)
   or `MapViewOfFile`-style paging would cut it to <1 MiB resident.
4. **Shrink the IRC cache** to 2 MiB, or make it adaptive. 4 MiB of decoded
   blocks may be cheaper to re-read from SD than to keep.
5. **Reduce the render-target set.** If the 5×1024 + 4×512 logical targets can
   share storage (they are never live simultaneously in the measured set),
   targeting 2 MiB instead of 4.5 frees half the linear heap.
6. **Stream budget vs SD.** 7 MB/s default; a full cell (NIF+DDS) is tens of MiB.
   The 2 MiB staged cache means prefetch can only ever be one cell ahead.
7. **The 512 MiB report.** Two routes are documented in `docs/MEMORY_BUDGET.md`:
   a paging provider for dirty anonymous pages, or a native patch of the
   `0x00a48a67` gate. Neither is implemented; this is a decision, not a task.

## 7. Unknowns that need measurement

- Actual `linearAlloc` high-water in the menu/world slice (no counter exists).
- Guest `used` high-water under a real cell load (the 42.12 MiB figure is a gate
  threshold, not a working set).
- Whether the engine ever calls `CreateFileMapping` on a path we serve from a BSA
  (would change both RAM and IO accounting).
- Whether raising the linear heap above 8 MiB requires a matching `osSetMemMode`
  change for the 178 MiB CIA title.
