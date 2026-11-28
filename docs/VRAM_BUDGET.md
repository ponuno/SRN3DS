# Render-target VRAM budget (6 MiB PICA VRAM)

Worktree `vram-budget` (base `c87149d`), branch `vram-budget`. This note is
about the four 512x512 colour targets that killed the Azahar menu path:
`CreateRenderTarget: 512x512 VRAM texture allocation failed (vram free 274
KiB)`, then 77 KiB, then a NULL target and a guest fault at `0x30` in
`0x00c8fc90`.

`DevCaps` is unchanged, no frame is claimed, and the `-ForceMemProbe` /
caps-probe build is still measurement-only. Everything below is enforced in
`source/d3d9_core.c` with backend support in `source/d3d9_citro3d.c`; the
Azahar numbers are from the emulator, not from hardware.

## The budget

New 3DS exposes **10 MiB physical VRAM but only 6 MiB usable**; the OS keeps
the rest. `vramSpaceFree()` reports the whole pool, so it is **not** a budget.
The PICA backend therefore accounts for every on-VRAM allocation itself:

- LCD render target (`240x400` colour + `D24S8`),
- 2D targets (colour, plus depth when attached),
- PICA-SR scene/scratch colour and the SR D16 attachment,
- cube-face depth views.

Textures, cube colour, vertex/index arenas and linear-heap targets are FCRAM
(`onVram=false`) and are not charged.

`N3DS_VRAM_LIMIT` (bytes or `K`/`M`, also a `Skyrim_N3DS.cfg` key) sets the
budget; default **6 MiB**. The allocator keeps **256 KiB slack** for citro3d's
rounding, so the enforced cap is 5.75 MiB. Every reservation logs
`[d3d9-vram] <what> N KiB ... live X/Y KiB`; a refusal increments
`vram_failures` and returns an error, never a silent NULL.

The core keeps its own optional target sub-budget (`N3DS_TARGET_VRAM_LIMIT`).
It is **unlimited by default** (0), because the backend's global counter is the
authority; the device main sets **4.25 MiB** so the four 512x512 menu targets,
the retained back buffer/depth, the LCD target and the smaller transient
targets the menu creates afterwards all fit.

| value | meaning |
|---|---|
| `N3DS_VRAM_LIMIT` unset | **6 MiB** total (5.75 MiB enforced) |
| `N3DS_TARGET_VRAM_LIMIT` unset | **0 = unlimited** API default; **4.25 MiB** device main |
| `*=0` | unlimited (backend/library is then the only authority) |

## Policy when a target does not fit

In this order, each step with a named `[d3d9-target]` stderr line:

1. **Reuse pool.** After a 2D target is released its native (`C3D_Tex` +
   `C3D_RenderTarget`) is kept in a small pool (default 0 entries; 4 on the
   device build, `N3DS_TARGET_POOL_ENTRIES=0..4`). A same `(w,h,format)` create
   takes the pooled native instead of allocating. Pooled bytes stay charged
   against the budget and are the first thing dropped under pressure.
2. **Evict an inactive target.** Candidates are targets that are not the
   current render target, not the retained back buffer, were not bound or
   sampled in the current frame generation, and have no depth attachment.
   Creation alone is not a use (`last_use_gen=0`), so a target the guest
   created but has not bound/sampled is evictable in the same frame.
   Sampleable RT-textures are included: `target_suspend` frees the colour
   storage and the render-target view but keeps the D3D handle and the sample
   view valid, and `target_resume` re-creates the storage on the next bind,
   `SetTexture` or draw (the draw path rehydrates a still-bound texture whose
   target was evicted; draw state persists across frames).  Suspension is
   refused inside `BeginScene/EndScene`, so no queued command can reference
   freed storage.  A resume that no longer fits is a named refusal, not a
   silent NULL.
3. **Downscale PO2 storage.** Halve the native storage (512 -> 256 -> 128 ...,
   floor 64) and keep the logical surface through `target_viewport_scaled`.
   The guest still renders "512x512"; the image loses resolution. Counted as
   `downscales`. On the device build this is on by default
   (`N3DS_TARGET_DOWNSCALE=1`); host/diagnostic builds keep the historical
   exact refusal unless the variable is set, because a silently smaller target
   changes what the guest samples.
4. **Linear heap.** `target_create_linear` allocates the chosen (possibly
   downscaled) `C3D_Tex` with `onVram=false`. Sampling works, but PICA
   framebuffers must live in VRAM: for a render target the RT-view creation
   fails, and the backend reports `linear storage cannot back a PICA render
   target (framebuffer must live in VRAM)`. The core then refuses by name
   instead of handing out a NULL. In the 96x8 `.3dsx` profile the linear heap
   is also nearly full, so this is not a usable render-target path there.
5. **Named refusal.** If no step applies the create fails with
   `CreateRenderTarget: backend could not allocate a WxH native target; target
   VRAM budget ... KiB (live ..., peak ...), evictions ..., linear fallbacks
   ..., downscales ...; allocation refused`. The core never returns a
   half-created target and never evicts the current RT, a just-bound target or
   a target with pending commands.

## Metrics

`d9_target_stats()` (`D9TargetStats`) and the exit line
`[d3d9-target] vram limit=... live=... peak=... alloc_failures=... evictions=...
linear_fallbacks=... downscales=... pool_hits=... pool_flushes=...
pool_entries=... suspended=...`:

| field | meaning |
|---|---|
| `vram_limit` / `vram_live` / `vram_peak` | configured budget, charged bytes, high-water mark |
| `alloc_failures` | target creations no policy step could satisfy |
| `evictions` | inactive targets suspended to free VRAM |
| `linear_fallbacks` | targets allocated in the linear heap |
| `downscales` | PO2 shrink attempts (including attempts that still did not fit) |
| `pool_hits` / `pool_flushes` / `pool_entries` / `pool_bytes` | reuse pool |
| `suspended_targets` | live handles currently without GPU storage |

The existing `mem_stats` VRAM live/free lines are now fed from this backend
accounting on the device (`mem_stats_set_vram`): they no longer count a
suspend/resume or release/re-create pair twice.  The `[d3d9-vram]` live/peak
pair and the exit summary remain the primary evidence.

## Tests

`tests/vram_budget_smoke.c` (registered as `vram_budget`):

* **A** four 512x512 render-target textures + 512x256 retained back buffer +
  depth under a 5 MiB target budget with 512 KiB of non-target VRAM: three fit
  in VRAM, the fourth is downscaled to 256x256 and binds/renders, counters
  (`downscales=1`, `vram_live=peak=4.25 MiB`) match, release leaves only
  the back buffer;
* **B** an inactive plain target is evicted (generation-based), the current RT
  is never chosen, the evicted target is rehydrated on bind (`resumes=1`);
* **C** destroy/recreate reuses the pooled native without a backend allocation
  (`pool_hits=1`), pooled bytes stay charged, teardown flushes the pool;
* **D** with no eviction/linear/downscale available the refusal is named and
  counted (`alloc_failures=1`);
* **E** downscale 512 -> 256 -> 128 is the last fallback and is counted;
* **F** a sampleable RT-texture target is evicted when inactive, rehydrated
  when its texture is bound again, and eight evict/rehydrate cycles leave
  `vram_live`/`vram_peak` unchanged (no counter drift).

Host regressions re-run green: `render_target`, `render_target_bounded`,
`render_target_budget`, `render_texture`, `render_texture_sample`,
`rt_readback`, `depth_rt`, `cube_rt`, `n3ds_sr`, `com`, `menu_pipeline_e2e`,
`device_reset`, `null_backend`, `scaled_readback`, `scaled_target_blit`,
`stretchrect_com`, `budget_trim`, `texcache_pinning`,
`texcache_limits_core` (20/20).

## Azahar

Build (worktree; the `.3dsx` is ~2.85 MB):

```powershell
tools\build.ps1 -Mode tesv -MemProfile 96x8 `
  -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen `
  -Isolated vrambudget4 -TesvArmSteps 2000000000 -ForceMemProbe -NoClean
```

Run with the same inputs as `docs/AZAHAR_MENU.md`: virtual SD junction to
`E:\azahar_sd\Skyrim`, caps probe flag, prefs 400x240, `d3dx9_42.dll`,
`tesv_d3d9_budget.txt=32`, `tesv_trace.flag`.

Measured with `vrambudget4` (sha256 `382C0A08вЂ¦`, 96x8, `-ForceMemProbe`, caps
probe, budget file 32):

* `[d3d9-vram] closed: live 0 KiB peak 5102 KiB` вЂ” the backend's own global
  counter never exceeded the 5888 KiB enforced cap. The fourth 512x512 menu
  target was **downscaled to 256x256** (`[d3d9-target] вЂ¦ downscaled`,
  `vram 5102/5888 KiB`), saving 768 KiB against a full 512 target. Before the
  change the same set reached `vram free 274 KiB` inside the 10-MiB pool and
  the fourth target failed.
* `[menu] enter 00876f60`: **not reached** (0 occurrences). The run still dies
  at `Guest fault at 00000030`, `last_call=00c8fc90`, after the menu shader
  submissions.
* `linear_fail_n=11` (down from 26), `linear_min_free=1.71 MiB`,
  `newlib_free=48,712`, `guest_used=56,617,152`.
* Remaining blocker: after the four menu targets the guest asks for more
  small targets (512/128/32/8) and the 4.25-MiB target sub-budget is full.
  `evictions=0` because every remaining 512 target is sampleable and therefore
  protected; the core then downscales but its own sub-budget check refuses
  even 64x64 (16 KiB), so the guest gets NULL and faults at `0x30`.
* `mem_stats` still prints `vram_live=8.87 MiB` at exit: that counter is
  cumulative allocation deltas (it counts releases and re-creations
  separately and does not see the backend's reservations). The authoritative
  number is the `[d3d9-vram]` live/peak pair. The pool free at the refusal was
  1042 KiB while the backend still failed a 64x64 render-target view; that
  gap (citro3d `vramFree` versus the modelled reservation) is the next thing
  to audit.

The device main therefore sets the target sub-budget to 4.25 MiB: the four
512 targets become 3x512 + 1x256, the target set peaks at ~4.25 MiB and ~1.5
MiB of the global cap stays for the LCD and smaller targets. That removed the
original target #4 failure but not the later small-target refusals.

Measured with `vrambudget6` (sha256 `BB078875вЂ¦`, 96x8, `-ForceMemProbe`, caps
probe, budget file 32):

* `[d3d9-vram] closed: live 0 KiB peak **5042 KiB**` against the 5888 KiB
  enforced cap; the reconciled `mem_stats` line reports
  `vram_live=5,097,984` / `vram_peak=5,163,776` (same accounting, no double
  count). `refusals=0`, `linear_fail_n=0`.
* **8 evictions** and **17 target creations**: the four 512x512 menu targets
  all exist at full size (one is evicted to admit the next and resumed later);
  the later 512/128/32/8 targets are admitted instead of refused. The linear
  "cannot back a PICA render target" diagnostic is no longer reached.
* `[menu] enter 00876f60`: **0**, draw probe 0. The VRAM wall is gone but the
  guest now dies of **guest RAM**, not VRAM:
  `Guest fault at 41cee000: guest memory backing failed: out of memory`,
  `guest_used=78,121,152`, `newlib_free=46,560`, `va backed=39,188 KiB`,
  `pages failures=1`. That is the 92 MiB `.3dsx` guest budget, not the 6 MiB
  VRAM budget.
* Before eviction was enabled the same path stopped at 56.6 MiB guest and a
  NULL 64x64 target; eviction moved it to 78.1 MiB and a clean guest-RAM
  exhaustion.

Measured with `vrampage` (sha256 `E735655AвЂ¦`, 96x8 + pageable `.text`
`f43a537`, `-ForceMemProbe`, caps probe, budget file 32):

* VRAM is no longer the wall: peak `vram 5042/5888 KiB`, **8 evictions**,
  17 target creations, `linear_fail_n=0`; no `[d3d9-vram]` refusal and no
  linear-target diagnostic. The four 512x512 menu targets are created at full
  size (evicted/resumed as needed).
* The pageable `.text` window (5 MiB default) carries the run much further:
  `Translation_ENGLISH.txt` loaded, semaphores/threads created, and the menu
  entries `0x00892da0`, `0x00898430`, `0x0086f067` -> `0x00a60c80` /
  `0x00a60c10` / `0x00a5eaa0` were reached.
* `[menu] enter 00876f60`: **0**; draw probe **0**.
* The run ends with a raw CPU jump to null, not a named guest fault and not
  OOM: `unmapped Read32 @ 0x07FFFFB8 at PC 0x00000000` (Azahar stack dump,
  no `Skyrim_N3DS_tesv.log`, so no atexit snapshot). Last memory snapshot is
  `tag=init` (`guest_used=5,168,456`, `newlib_free=2,853,664`); the earlier
  78.1 MiB guest-RAM wall is gone with paging.
* `N3DS_TEXT_WINDOW_MB` is now a `Skyrim_N3DS.cfg` key, so the paging window
  can be raised/lowered (`0` disables it) without a rebuild.

## What is left

Latest Azahar runs:

* `depthdefer3` (2 MiB IR cache, sha256
  `FDF60D166FFD5178327ABF1BB57A12B858961CA768C9FCDD05ABD1FADCA7D92F`)
  reached the 3D path and stopped at the IR decode arena.
* `depthdefer4` (same binary plus `N3DS_IR_CACHE_MB=6`, sha256
  `AB5E195814E0735A67425940AA4A035016873FE2ADD9DA3B5FD2A308BE6D3305`)
  ran the full 950 s wall budget with **no guest fault**: 452 menu entries,
  4 `SetRenderTarget` calls (all back buffer 512x256), one queued depth, no
  `00876f60`, no null call. The larger arena removed the decode-cache fault;
  the run is simply slower and did not reach the target before the timeout.

* **The depth mismatch is gone.** `SetDepthStencilSurface(512x1024)` is
  accepted once while the 400x240 back buffer is current
  (`queued while 400x240 is active`), and no
  `does not match the ... active target` line appears. The repeated
  INVALIDCALL failure the guest used to ignore is no longer produced.
* **The null call is gone.** Up to the new blocker there is no
  `[target] target=...` and no `Guest fault at 00000000`. `0x003ba588` and
  the 3dsx `.data` decode are also absent.
* **New blocker: the IR decode arena.** With 2 MiB, thread 9
  (`start=00a4b4a0`) faults at `009e03f6` with
  `IR: block does not fit the decode cache`, then `00644fc7`; a flush cannot
  preserve every suspended block. At 6 MiB the fault is gone and the run
  survives to the wall-clock timeout.
* `[menu] enter 00876f60` is still 0; the 6 MiB run reaches 452 menu entries.
  Last entries: `00892da0`, `00898430`, `00431b00`, `00a5d370`.
* Swap: `out=921 in=3 live=279697/33554432 ratio=13.37`,
  `resident=12288/12288`, `dropped=0`; every `fail_*` counter is 0.
* Host `free` guard: 0 refusals once the check was limited to the heap range
  and 8-byte alignment. `malloc_usable_size()` misjudged 1261 valid newlib
  internal frees and stalled boot before any menu, so it is not used.

What is fixed:

* `d9_release_render_target` retires the current target instead of destroying
  it; the native is freed on the next `SetRenderTarget` (test `vram_budget`
  G).
* `d9_set_depth_surface` accepts a size mismatch, remembers the request
  (`GetDepthStencilSurface` returns it) and applies it when
  `SetRenderTarget` selects the matching target. Clears/draws before that
  use the active target's embedded depth instead of failing.
* `d9_set_render_target` no longer refuses a size-mismatched bound depth; it
  keeps the request, uses the target's embedded depth and logs once per
  `(depth,size)` pair. `N3DS_RT_TRACE=1` logs every request and successful
  selection (`[rt] SetRenderTarget h=... / ok current=...`).
* `ir_exec` rejects call targets outside the guest code and thunk ranges and
  names a block that leaves them with `from_block`, `ret`, `esp`, all
  registers and 8 dwords of each register that points into guest memory.
* `source/mem_guard.c` (TESV device link only) wraps `free`/`realloc`,
  refuses pointers outside `fake_heap_start..fake_heap_end` or unaligned,
  and keeps the last 16 calls (pointer, size, caller, sp). `rt_unimpl` dumps
  that ring before unwinding.
* `guest_swap` counts duplicate pool recycles (`reissued` in the swap
  report). The first version used a 128-entry live ring; with 12288 resident
  pages it wrapped and produced 1705 false positives. The pool-duplicate
  check replaces it, so a non-zero value is now real aliasing.

Earlier findings kept for context:

* 128 KiB main stack (`source/n3ds_boot.c`) removed the PC 0 fault; the
  guest then reached the logo/menu path (`0087b597`, `0089775e`).
* `N3DS_SWAP=0` cannot reach this point: without swap the same path exhausts
  guest backing at 71.16 MiB.
* PICA colour targets own one embedded depth-stencil attachment; a D3D9
  depth surface is a size/format descriptor, not extra VRAM.
* Host suite: **185 passed, 0 failed** (`-SkipPython`, fresh
  `HOST_OBJ_CACHE`). The first full run reused a stale shared object cache
  and reported false target/IR failures; a fresh cache is green. Python
  tests are unavailable in this environment (missing `capstone`, LLVM
  `clang.exe`, `tex3ds.exe`).

## Device menu gate after b03c33c

`b03c33c` makes `BinkOpen` return a valid one-frame empty clip when the
platform has no decoder.  On Azahar the device still does not reach StartMenu:

* `_BinkOpen` is never called.  The only Bink line is
  `_BinkSetSoundSystem@8: Bink sound is not implemented`; no `empty clip`.
* `[menu] enter 00876f60/00876fc0/00876b90` = 0; `[menustate] singleton=0`,
  `view=0`; the run advances to `frame=114360+` in `mode=2/2 inload=1`
  (`00a5d370 from=006f4045`, the loading pump), then the wall timeout ends it.
* `sIntroSequence` was tried empty and `BGS_Logo.bik`, with
  `bPreloadIntroSequence=0/1`; the `[ini]` trace confirms the values reach the
  engine, but the branch does not change.  `sStartingCell/World/X/Y` are all
  empty (`""` default).
* Renaming the guest `Skyrim/binkw32.dll` did not change it: the engine still
  never calls the shim's `_BinkOpen`.

So the startup probe `0x6982c0` still returns 0 and WinMain takes
`0x69de54` (load path) instead of `0x69de08` (menu).  Singleton 0 is a
consequence: its only writer `0x876b90` at `0x876bd7` is behind the menu call.
The empty-clip fix cannot help until the guest actually opens the movie.

Next: log at `0x69dda4`/`0x69ddf5` the three probe settings
(`[0x1272fb4]`, `[0x1272fd8]`, `[0x1272fe4]`), `AL` and the out object; and
check whether the engine resolves `binkw32.dll!_BinkOpen@8` to the guest DLL
body instead of the host shim.  The host success requires the probe to return
1 first.

## Gateprobe (1000 s): the 0x6982c0 gate is not a function entry

`menu_trace` now has a dump for `addr==0x006982c0` (ecx, out, arg, the three
`0x1272fb4/0x1272fd8/0x1272fe4` strings) and AL logs for
`0x0069de08`/`0x0069de54`.  In the 1000 s Azahar run **none of them fired**:

* `[menu] enter 006982c0` = 0; `winmain menu/load` = 0; `00876f60` = 0.
* The run still reaches the load path: `release h=50c` by thread 10
  (`0xb9da1c`), `[bink] _BinkSetSoundSystem`, then
  `00a5d370 from=006f4045` (loading pump), `[menustate] frame=480`,
  `singleton=0`, `view=0`.
* `release h=504` = 0: the GFx worker remains at `0xa5ac42`.
* No guest faults, 0 `[heap]` refusals, no swap report before the wall timeout.

So `0x6982c0` is executed as a **block inside the 0x6981f0 function** (and
`0x69de08/0x69de54` inside WinMain), not as a traced function entry.  The
device does pass the gate and takes `0x69de54` (load path); the AL/out values
need a block-IP probe in `n3ds_ir_run` for the block covering `0x69ddf5`.

## Block-IP probe (900 s): no `[ip]` line fired

The block-entry probe in both executors (`ir_exec.c` and the unity copy used by
`ir_container.c`) checks the block byte range (`b->start .. b->start+b->x86_len`)
for `0x6982c0`, `0x69dda4`, `0x69ddf5`.  In the 900 s Azahar run **none fired**,
although the same binary reaches the loading pump:

* last traced call: `00a4b500 from=006982ae` (VMInitThread);
* `release h=50c` by thread 10 (`0xb9da1c`);
* `[bink] _BinkSetSoundSystem`;
* `00a5d370 from=006f4045` (loading pump), `[menustate] frame=600`,
  `singleton=0`, `view=0`; `00876f60` = 0.

The `[ip]` format strings are present in the `.elf`, so the probe is built in.
Either the gate is not executed on the device path (the loading pump is entered
from a different caller) or the gate runs in a tier that bypasses both
executors.  No positive-control probe was run in this timebox; the next check
is to add a probe for a known-executed block (`0xa4b500` or `0x6f4045`) and a
16-entry last-block ring dumped at `0x6f4045`, to prove the hook fires and to
name the block immediately before the gate.

## Ringprobe: 0x6982c0 is never called on the device

Positive control passed: `[ipc] a4b500 from=00698299` and
`[ip] a4b500 ... blk=00a4b500+15 tid=1`, so the block hook is active in the
executed tier.  The loading-pump block then produced:

```
[ring] 6f4045 eax=1ce12000 esp=1ce0ff88 tid=9 n=16:
  006f44a7 006f44b4 006f44b9 006f4000 006f401d 006f4022 006f402f 006f4031
  008c71e0 008c71f7 008c71fb 00a5e180 00a5e1c4 008c7209 008c7212 006f4045
[ip] 6f4045 eax=1ce12000 esp=1ce0ff88 blk=006f4045+11 tid=9
```

There is **no `[ipc] 6982c0`**, no `[ip] 6982c0`, and no `6982c0` in the ring.
So the device never calls the startup gate at all; the loading pump is driven
from `0x8c71e0..0x8c7212` (main loop) and returns to `0x6f4045`, not through
WinMain's `0x69de54`.  Singleton 0 follows because `0x876f60` is never called.

Next: identify the caller of `0x8c71e0`/`0x8c7212` and why the pump is started
directly, and confirm whether `0x69de08`/`0x69de54` are ever reached (both have
`[ipc]` probes and neither fired).

## Threadprobe: the main thread runs 0x8c71e0; the WinMain gate is absent

At the loading pump:

```
[ring] 8c71e0 eax=008c71e0 esp=1007fc48 ret=00a5d46c tid=1 n=16:
  00a4baf9 00a4e2f0 00a4e2fa 00a4e300 008972f0 00897300 00897304 008c71a0
  008c71b7 008c71bb 00a5e130 00a5e140 00a5e14e 008c71c9 008c71d2 008c71e0
[ring] 6f4045 eax=1ce12000 esp=1ce0ff88 ret=ff010028 tid=9:
  006f44a7 .. 006f4031 008c71e0 008c71f7 008c71fb 00a5e180 00a5e1c4
  008c7209 008c7212 006f4045
[threads] 6f4045 live=10 current=9
```

* **tid=1 (main) is ready and is the one executing `0x8c71e0`**, return
  `0xa5d46c`; its ring is the `0xa4baf9 -> 0xa4e2f0.. -> 0x8972f0.. ->
  0x8c71a0.. -> 0xa5e130.. -> 0x8c71c9..` loop.  The main CPU snapshot is
  empty (`start=0 esp=0`), so the ring is the only PC evidence.
* tid=9 is running `0xa4b4a0`/`0x6f4045` (`irc_depth=3`, esp `1ce0ff88`).
* tid=8 is blocked on `0x504` (`0xa5ac42`); tid=10 sleeps 1 ms at
  `0xb9ef30`; tid=11 is ready waiting on `0x537` (`0xa4e30f`); tid=2..7 are
  GFx pool workers at `0xa5b000`.
* No `[ipc]`/`[menu]` for `0x6982c0`, `0x69de08` or `0x69de54`.

So the device main thread is not blocked: it runs an alternate loop that
enters the pump through `0x8c71e0` and never calls WinMain's menu gate.
Next: identify `0xa5d46c` (return from `0x8c71e0`) and the entry that starts
this loop; that will show how to route it through `0x876f60`.
