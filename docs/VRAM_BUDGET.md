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

Measured with `vrambudget4` (sha256 `382C0A08…`, 96x8, `-ForceMemProbe`, caps
probe, budget file 32):

* `[d3d9-vram] closed: live 0 KiB peak 5102 KiB` — the backend's own global
  counter never exceeded the 5888 KiB enforced cap. The fourth 512x512 menu
  target was **downscaled to 256x256** (`[d3d9-target] … downscaled`,
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

Measured with `vrambudget6` (sha256 `BB078875…`, 96x8, `-ForceMemProbe`, caps
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

Measured with `vrampage` (sha256 `E735655A…`, 96x8 + pageable `.text`
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

* **Compressed swap is active and removes the OOM.** The previous 32 KiB-stack
  binary predated `8b533c0`, so swap was not in it, and the cfg keys were not
  in the allowlist. Both are fixed; `vramswap` was built from current main and
  ran with `N3DS_SWAP=1`, `N3DS_GUEST_TARGET_MB=48`,
  `N3DS_SWAP_BUDGET_MB=32`, `N3DS_TEXT_WINDOW_MB=4`,
  `N3DS_IRC_CACHE_BYTES=2M`, `N3DS_IR_CACHE_MB=2`.
  Measured: `swap=1 swap_out=920 swap_in=2 swap_resident=12288
  swap_target=12288 swap_compressed=279,789 swap_ratio_pct=1345
  swap_fail_*=0` (48 MiB anonymous resident exactly at target, 13.45x live
  ratio, no swap failures). `guest_used=66,706,539`, `newlib_free=27,529,108`
  (was 183 KiB and OOM): the anonymous resident cap is 48 MiB, the remaining
  ~15 MiB is file-backed image/paging and slab overhead, not swappable.
* **New blocker after swap:** `Guest fault at 003ba588: IR: unsupported opcode
  in decoded stream` on thread id 9 (`start=00a4b4a0`), then the same on the
  main thread at `000000f9`. Azahar also shows `_free_r` touching guest
  addresses (`0x0E428D9C`, `0x0E428E82`) during teardown, i.e. `free()` on a
  value that looks like a guest pointer. Both are pointer/EIP corruption
  symptoms after a long 3D path (many
  `SetDepthStencilSurface: ... size does not match` lines), not OOM.
  `[menu] enter 00876f60` is still 0; last entries are `00892da0`,
  `00898430`, then `00a5eaa0 from=00a60c3e` twice.
* Next: log the last indirect call target / free argument in the allowed
  runtime files, and A/B with `N3DS_GUEST_TARGET_MB=32` plus
  `N3DS_TEXT_WINDOW_MB=2` to see whether added eviction changes the corrupt
  pointer. Do not change `DevCaps`.

* **PC 0 was main-stack exhaustion, not paging.** A/B with the rebuilt binary:
  `N3DS_TEXT_WINDOW_MB=0` (paging off) ends at the same `[menu] enter
  00a5eaa0 from=00a60c3e` and the same first fault as the default 5 MiB
  window: `unmapped Write32 ... @ 0x07FFFF94 at PC 0x0010F8C0`, Thread ID 1,
  R1=0x1007F148. The later `Read32 @ 0x07FFFFB8 at PC 0` is the secondary
  report. The write is the `push {r4,r5,r6,lr}` prologue of `n3ds_guest_ptr`
  at `SP=0x07FFFFA4`, i.e. below the `0x08000000` stack base.
* The cause was libctru's weak `__stacksize__ = 0x8000` (32 KiB): `initSystem`
  does `SP = base + __stacksize__`. `source/n3ds_boot.c` now defines a strong
  128 KiB `__stacksize__` (3DS only). The `stack128` run has **no PC 0** and
  goes further: `logo02_d.dds`/`logo02_n.dds`, menu entries from
  `0x0087b597` and `0x0089775e`, then a clean **guest backing OOM** at
  `0x640625d0` (`guest_used=71,162,696`, `newlib_free=183,272`,
  `guest_backing_failures=1`, `linear_fail_n=0`, VRAM peak 5042/5888 KiB,
  `[menu] enter 00876f60` still 0).
* Next deficit: the guest backing budget is a few MiB short just before the
  logo texture. Proposals without touching `DevCaps`: `N3DS_IRC_CACHE_BYTES=2M`
  (or `1M`) frees 2–3 MiB of the newlib heap, `N3DS_TEXT_WINDOW_MB=3` frees
  2 MiB of the page window, stream is already `N3DS_STREAM=0`.

* **Resume at the previous native size only.** A suspended target is resumed in
  VRAM; if the budget is full at that moment the core returns a named
  `D9_MEMORY` instead of downscaling the resumed storage.  Downscaling on
  resume would change the sample UV sub-rect and is a separate change.
* **Linear-heap render targets are impossible** (`framebuffer must live in
  VRAM`), so the linear step is only a named diagnostic for targets; the real
  recovery is eviction + downscale.  Sampling linear textures still works.
* **Audit citro3d `vramFree` accounting.** At the pre-fix refusal the pool
  reported 1042 KiB free while the RT-view failure was actually a linear
  storage attempt (no VRAM reservation charged). That is now named, but a
  physical New 3DS run is still needed to confirm allocation order and
  fragmentation.
* Downscale changes sampled resolution; with eviction available the menu set
  should stay at 512 where the budget allows.
* Physical New 3DS run still needed for real VRAM/linear headroom next to the
  texture cache.
