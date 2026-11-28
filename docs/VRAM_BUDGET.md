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
   current render target, not the retained back buffer, were not bound in the
   current frame generation, are not referenced by a live sampleable
   RT-texture, and have no depth attachment. `target_suspend` frees the
   colour storage and the render-target view but keeps the D3D handle and the
   sample view valid; `target_resume` re-creates them on the next
   bind/read/write. Suspension is refused inside `BeginScene/EndScene`, so no
   queued command can reference freed storage.
3. **Downscale PO2 storage.** Halve the native storage (512 -> 256 -> 128 ...,
   floor 64) and keep the logical surface through `target_viewport_scaled`.
   The guest still renders "512x512"; the image loses resolution. Counted as
   `downscales`. On the device build this is on by default
   (`N3DS_TARGET_DOWNSCALE=1`); host/diagnostic builds keep the historical
   exact refusal unless the variable is set, because a silently smaller target
   changes what the guest samples.
4. **Linear heap.** `target_create_linear` allocates the chosen (possibly
   downscaled) `C3D_Tex` with `onVram=false`. Sampling and rendering still
   work; the cost moves from VRAM to the linear heap. In the 96x8 `.3dsx`
   profile the linear heap is nearly full (`linear_fail_n>0`), so this is a
   last resort there.
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

The existing `mem_stats` VRAM live/free lines still describe the backend
accounting; these counters describe the policy decisions.

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
* **E** downscale 512 -> 256 -> 128 is the last fallback and is counted.

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

## What is left

* **Evict sampleable targets safely.** The guest needs more targets than
  4.25 MiB holds. The suspend/resume pair already keeps the `NativeTarget` and
  the sample view valid, but sampling a suspended target must resume it first;
  until the draw paths do that, all RT-textures stay non-evictable and the
  budget is simply full after the menu target set. That is the next fix, not a
  larger number: the four 512 targets alone already consume the budget.
* **Reconcile `mem_stats` VRAM.** Its `vram_live` is a cumulative allocation
  counter (8.87 MiB at exit while the backend peak was 5.10 MiB). Feed it from
  the backend's `g_vram` counters instead of the free-pool deltas.
* **Audit citro3d `vramFree` accounting.** At the final refusal the pool still
  reported 1042 KiB free yet a 64x64 colour-only render-target view failed
  with no reservation charged. Either `C3D_RenderTargetCreateFromTex` needs
  storage the model does not know about, or a deleted VRAM texture is not
  returned to the pool. This is instrumented (named failure + `accounted`)
  but not yet explained.
* Linear-heap targets are validated in Azahar for allocation and binding; a
  physical New 3DS run is still needed to see the real failure order and the
  actual linear-heap headroom next to the texture cache.
* Downscale changes sampled resolution; the current device set is 3x512 + 1x256
  for the four menu targets.
