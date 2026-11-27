# PICA-SR: internal-resolution upscaling

`source/d3d9_citro3d.c` can render the guest frame at a reduced internal
resolution and upscale it onto the real top-LCD framebuffer with the vendored
**PICA-SR / libFSR3DS v0.3** TEV kernels.  The D3D9 contract does not change:
the guest still owns a 400x240 back buffer, its clip matrices, its depth clears
and its `Present` semantics.

Attribution and the exact upstream revision are in
`third_party/pica_sr/README.md` (author `ponuno`,
<https://github.com/ponuno/PICASR-FSR-for-3DS>, version 0.3, copied
2026-11-27).  Only `fsr3ds.c`/`fsr3ds.h` are vendored; the FurMark scene, UI,
benchmark and its blit shader are not.

## Enabling

| Variable | Values | Default |
| --- | --- | --- |
| `N3DS_SR` | `off`, `nearest`, `bilinear`, `tev2d`, `tev2dq` | `off` |
| `N3DS_SR_SCALE` | `0.5` .. `1.0` (percent `50`..`100` also accepted) | `0.53` |
| `N3DS_SR_DRS` | `0` / `1` | `0` |

The default `0.53` is the POT-fit optimum for the 240x400 portrait target: it
is the integer percentage that leaves the fewest unused texels in the rounded
texture (`n3ds_sr_pick_scale`). Example:
`N3DS_SR=tev2dq N3DS_SR_SCALE=0.53`.

On a real 3DS (or in an emulator whose homebrew loader does not forward the
host environment) the same keys are read from `sdmc:/N3DS_SR.cfg`, one
`key=value` per line, but only when `N3DS_SR` is absent:

```
# sdmc:/N3DS_SR.cfg
mode=tev2dq
scale=0.53
drs=0
```

Startup writes one line to `sdmc:/N3DS_SR.log` and one line to stderr:

```
PICA_SR_ACTIVE mode=tev2dq scale=53% texture=128x256 passes=4 steps=1
```

Mode names are case-insensitive.  An unknown name, an out-of-range scale or an
allocation failure logs a named reason (`[d3d9-citro] PICA-SR: ...`) and keeps
`mode=off`; nothing crashes and every frame still presents.

## What happens to a frame

1. `begin()` binds the **scaled scene target** instead of the LCD framebuffer
   when SR is active.  Its storage keeps the framebuffer's portrait 240x400
   orientation, scaled by `N3DS_SR_SCALE` (default 0.53 -> 127x212 in a
   128x256 POT texture), so the guest's screen clip matrices are used
   unmodified.
2. `target_viewport(400, 240)` becomes
   `C3D_SetViewport(0, 0, 400*scale, 240*scale)` (0.53 -> 212x127).  A guest
   viewport is expressed in 400x240 units, so multiplying it by the scale keeps
   the same relative framing inside the 192x320 storage.  Scissor state is not
   touched (the COM layer does not implement a scissor stage yet).
3. Guest draws (`draw`, `draw_colored`, `draw_menu*`, `draw_cube`,
   `draw_pica_vs`) go to that target and set `sr_draws`.  Depth clear uses the
   same target; the SR scene target carries a **D16** attachment (the guest
   never enables a stencil test in this backend), so only the implicit screen
   frame pays 2 bytes/px instead of 4.
4. `end()` runs the kernel chain from the scene texture to `c->target` and then
   `C3D_FrameEnd` performs the display transfer, exactly like the direct path.

`Present`/`EndScene` therefore still look identical to the guest.  The retained
lockable back buffer path is untouched: while an explicit or retained target is
bound, `sr_redirect` is 0 and SR is bypassed for that frame.

### Kernel chains

| Mode | Passes | Notes |
| --- | --- | --- |
| `nearest` | 1 | `REPLACE(T0)`, point-sampled |
| `bilinear` | 1 | `REPLACE(T0)`, linear |
| `tev2d` | 2 | LITE (H) into scratch, LITE (V) to the LCD; tap = 1 texel |
| `tev2dq` | 4 | UP_LOW(H, d=0.8) -> scratch, UP_SHARP(H, d=1.0) -> scene, UP_LOW(V) -> scratch, UP_SHARP(V) + stretch -> LCD |

The fitted constants `FSR3DS_2DQ_*` are used unchanged
(`LOW_GAIN255=115`, `LOW_RADIUS=0.8`, `SHARP_GAIN255=255`,
`SHARP_RADIUS=1.0`); the reference app's neutral sharpness knob (1.0x) is
fixed.  Gains below 255 always multiply the *centre-minus-neighbour* term, so a
sum over the 3 taps still equals 1.

`texcoord.y = 1 - v` is emitted by `source/d9shader_sr.v.pica`, matching
`source/d9shader.v.pica`; the tap offsets therefore negate `dv` on the CPU
side.  The chain never reads and writes one texture in the same pass: the
source target alternates scene/scratch.

### Dynamic resolution

With `N3DS_SR_DRS=1` the frame time (EMA, `svcGetSystemTick`) is fed to
`fsr3dsDrsUpdate(frameMs, &index, steps, 60, 1/60)` once per presented frame.
The ladder starts at `N3DS_SR_SCALE` and steps down by 8 points to a floor of
50%.  The POT texture is allocated once at the configured maximum; only the
viewport and the UV region change, so no per-step reallocation happens.

## Limits

- **Only the implicit back buffer is scaled.**  Explicit render targets,
  `CreateRenderTarget` surfaces, cube faces and any shadow-map/RT work are
  untouched: they keep their own resolution and are sampled by the guest at
  full size.  A frame that ends with an explicit target bound is not upscaled.
- The UI/menu is part of the same back buffer and is therefore rendered at the
  internal resolution too (and upscaled with the scene).
- A mid-scene `StretchRect` to the back buffer writes straight to the LCD in
  the current implementation; the SR present at `EndScene` then covers it.
  Skyrim's normal frame path does not do this.
- `N3DS_SR_SCALE=1.0` still costs one blit and one extra target.  `off` is the
  only zero-cost path.
- VRAM: scene target (RGBA8) + **D16** depth + one scratch RGBA8, all POT.
  `nearest`/`bilinear` allocate no scratch (single pass).  Sizes for the
  240x400 portrait target (`n3ds_sr_storage_size`, KiB):

  | scale | content | POT | scene | depth D16 | scratch | total |
  | --- | --- | --- | ---: | ---: | ---: | ---: |
  | 0.50 | 120x200 | 128x256 | 128 | 64 | 128 | 320 |
  | **0.53 (default)** | **127x212** | **128x256** | **128** | **64** | **128** | **320** |
  | 0.64 | 154x256 | 256x256 | 256 | 128 | 256 | 640 |
  | 0.80 | 192x320 | 256x512 | 512 | 256 | 512 | 1280 |

  The same 0.8 plan previously cost 1536 KiB (scene 512 + DEPTH24_STENCIL8 512
  + scratch 512), so D16 alone saves 256 KiB there and the 0.53 default saves
  1216 KiB versus the old 0.8/D24S8 combination.  Allocation failure disables
  SR.

  The **scratch target is the only ping-pong buffer**: pass *i* reads one of
  scene/scratch and writes the other, so `tev2d` and `tev2dq` never hold a
  third colour target.  The scratch render target has no depth attachment.

- **NPOT is not used.**  citro3d's `C3D_TexInitWithParams` rejects any
  dimension that is not a power of two in [8,1024] (`checkTexSize` in
  `source/texture.c`), and render targets are created from that texture.  A
  hand-rolled NPOT `C3D_Tex` would have to bypass the public API and assume an
  unverified 8x8-tile layout for both rendering and sampling; that cannot be
  validated without hardware and is intentionally not enabled.  The supported
  equivalent is the POT allocation plus the UV crop used here: unused texels
  cost memory but never affect sampling.
- The library is a **spatial** upscaler: no motion vectors, no temporal
  accumulation, no jitter.  It improves edges and mid-band detail over plain
  bilinear but cannot recover sub-pixel information.
- PICA TEV has no `abs`/`min`/`max`; `UP_SHARP_GATE` (the contrast gate) is
  intentionally not used, matching upstream's default pipeline.

## Verification status

- **Host**: `tests/n3ds_sr_smoke.c` (run with
  `tools/run_host_tests.ps1 -Only n3ds_sr`).  Covers mode/scale/DRS parsing,
  rejection of bad values, scaled sizes, POT rounding, the POT-fit scale
  picker, the scene/depth/scratch byte plan (including the D16 depth size),
  the UV region and the 2D/2DQ pass plans.  No GPU involved.
- **ARM**: the changed translation units (`source/n3ds_sr.c`,
  `source/d3d9_citro3d.c`) compile cleanly with devkitARM in
  `-Mode graphics` / `-Mode com` (objects archived in the isolated build).
  The **link** of both modes fails on base `8a8f743` for unrelated OFILES
  omissions (`bsa_reader.o`, `menu_bsa_runtime.o` for com; `d3d9_pica_vs.o`
  for graphics); that is not caused by this change.
- **Azahar**: see `docs/PICA_SR.md` §Evidence below / the archived
  `build_azahar/<stamp>-*` directories.  The emulator does not emulate PICA SR
  performance; frame numbers from Azahar are wall-clock of the host, so they
  are not device measurements.

### Evidence

Azahar 2126.2, `-Mode com` demo, `build_azahar/` (each directory has
`screen.png`, `run.log`, `azahar_log.txt`):

| Run | cfg / env actually parsed | Result |
| --- | --- | --- |
| `20261127-063004-picasr_off_base` | none (pre-change build) | baseline frame, direct path |
| `20261127-064127-picasr_off_new` | none | identical to the baseline |
| `20261127-064425-picasr_bilinear` | `mode=bilinear scale=0.8` | upscaled, framing matches `off` |
| `20261127-064447-picasr_tev2d` | `mode=tev2d scale=0.8` | 2-pass LITE, framing matches `off` |
| `20261127-064509-picasr_tev2dq` | `mode=tev2dq scale=0.8` | 4-pass 2DQ chain |
| `20261127-064550-picasr_tev2dq_064` | `mode=tev2dq scale=0.64` | 4-pass at 64% |
| `20261127-094000-picasr_05_prev` | `mode=tev2dq scale=0.5` (pre-change 06:53 binary) | 4-pass at 50%; `N3DS_SR.log` last line `SCALE=50%`, screenshot archived.  The strict app-log check failed because that pre-change binary writes no `Skyrim_N3DS.log`; the visual frame is present. |
| `20261127-064623-picasr_tev2d_drs` | `mode=tev2d drs=1` | DRS frame, no crash |
| `20261127-064949-picasr_final_tev2dq` | `mode=tev2dq scale=0.8` | blank capture (emulator window not yet painted) - not evidence |
| `20261127-065022-picasr_final_retry` | `mode=tev2dq scale=0.8` | final build, same frame as above |

`sdmc:/N3DS_SR.log` in each SR run contains exactly the parsed line, e.g.
`N3DS_SR=tev2dq N3DS_SR_SCALE=64% N3DS_SR_DRS=0`, so the screenshot mode is
not inferred from the filename.

The `0.53` default is covered by the host size/POT tests; no fresh Azahar run
of the new default exists yet because base `8a8f743` does not link a com
`.3dsx` (see the ARM note above), and the archived `0.5`/`0.64` screenshots
come from the pre-change binary.  D16 changes only the SR depth attachment,
not the visible colour path.

Azahar is vsync-locked at 59.8-60.0 FPS in every run (including `off`), so
these runs prove correctness and framing, **not** frame cost.  No device
timing was taken; the native/0.8/0.64 frame-time comparison remains open.

### Known interaction with the existing screen viewport

The direct (`off`) path programs `C3D_SetViewport(0, 0, 400, 240)` for a
240x400 portrait framebuffer.  A 400-pixel-wide viewport on a 240-pixel-wide
buffer clips the right 40% of the NDC range and leaves the 240..400 row band
unused, which is why the `off` demo frame is vertically stretched and cut at
the screen edge.  SR reproduces the same relative mapping by scaling that
viewport, so enabling SR does not change the framing - it only changes the
sampling resolution and the upscale kernel.  Fixing the screen viewport itself
would change `off` and is deliberately out of scope for this change; it is a
separate, guest-visible fix.

