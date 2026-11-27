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

## 5. GPU-visible memory (linear heap), 8 MiB

`C3D_TexInitWithParams` and `C3D_RenderTargetCreate*` allocate through
`linearAlloc`, i.e. the **linear heap**, not the 6 MiB VRAM pool.
`vramSpaceFree()` is only logged (`source/d3d9_citro3d.c:242,254,268,297,972,…`).
Treat **8 MiB as the effective GPU ceiling** until measured on hardware
**[unverified]**; on N3DS the linear heap can be enlarged, the VRAM split cannot.

Measured/derived consumers of those 8 MiB:

| Consumer | Size | Basis |
|---|---:|---|
| 9 explicit LDR targets, 256×256 colour + depth | **4.5 MiB** | `source/d3d9_core.c:7-12` (5×1024 + 4×512 logical → nine 256×256; 9 × (256 KiB + 256 KiB)) |
| Retained back buffer 240×400 + D24S8 | ~0.6 MiB | `d3d9_citro3d.c:310` |
| PICA-SR scene (128×256 RGBA8) | 128 KiB | `docs/PICA_SR.md` (scale 0.53) |
| PICA-SR depth (D16) | 64 KiB | same |
| PICA-SR scratch | 128 KiB | same |
| Vertex/index arenas (`FRAME_*`) | `linearAlloc`, sizes in `d3d9_citro3d.c:317-330` | — |
| **All game textures** | remainder | ETC1/ETC1A4 at 4 bpp (`d3d9_texture_data.c`, `d3d9_citro3d.c:445-449`) |

Consequence: after targets, ~3.5 MiB is left for every texture in the frame.
Texture uploads are already 4 bpp (DXT1/3/5 and BC5/BC4 → ETC1/ETC1A4); RGBA8
uploads (A8R8G8B8, R8G8B8 once shipped, L8) cost 8× per pixel-mip.

**There is no texture budget today**: `d9_texcache_init(...,0,0)` at
`source/d3d9_core.c:320` passes `guest_limit=0, gpu_limit=0`, i.e. unlimited.
`d9_texcache_set_limits()` exists (`d3d9_texcache.c:162`) and the cache has LRU
trim/`budget_trim_evictions`/`evictions_gpu` accounting (`d3d9_texcache.h:118-124`),
but nothing sets a limit in the slice.

## 6. Levers, with trade-offs

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
