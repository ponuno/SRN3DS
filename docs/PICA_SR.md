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
- **ARM**: `tools/build.ps1 -Mode com -Isolated srev` on base `2969b62`
  now **links** `Skyrim_N3DS_graphics_com_srev.3dsx`
  (1,198,328 bytes, sha256
  `7f93834f4dd06a34c4f8c3a6ae41524428abaf5670aa4b5b189a495ca4180af5`).
  The earlier link failure on `8a8f743` is gone.  That `.3dsx` is the binary
  used for every Azahar run below.
- **Azahar**: see §Evidence below and
  `build_azahar/<stamp>-srev-*` (each has `screen.png`, `run.log`,
  `N3DS_SR.log`, `azahar_log.txt`).  **These are emulator screenshots, not
  hardware measurements.**  Azahar does not model PICA SR cost, so no frame
  time or performance claim is made.

### Evidence

> **Emulator evidence, not hardware.**  Everything in this section was
> captured in desktop Azahar (OpenGL renderer, RX 7900 XTX host), never on a
> 3DS.  It proves that the frame renders and that the configured scale/POT
> plan is accepted by citro3d; it says nothing about device frame time.

Binary: `Skyrim_N3DS_graphics_com_srev.3dsx` built at base `2969b62`
(sha256 `7f93834f4dd06a34c4f8c3a6ae41524428abaf5670aa4b5b189a495ca4180af5`).
Each directory in `build_azahar/<stamp>-srev-*` has `screen.png`, `run.log`
(stdout of the capture script), `N3DS_SR.log` (the app's own decision file)
and `azahar_log.txt` (emulator log).  A missing `N3DS_SR.log` is expected in
the `off` run: `sr_init` returns before writing it.

| Run / screenshot | `sdmc:/N3DS_SR.log` (actually parsed) | Observations |
| --- | --- | --- |
| `20261127-100109-srev-off/screen.png` | none (mode `off`) | Direct path baseline.  Cone silhouette smooth; the known 400-wide viewport on a 240-wide buffer crops/stretches it.  No SR allocation. |
| `20261127-100356-srev-tev2d-053b/screen.png` | `N3DS_SR=tev2d N3DS_SR_SCALE=53% N3DS_SR_DRS=0` (cfg had only `mode=tev2d`, so this is the **default**) | 127x212 content in a 128x256 POT target, 2-pass LITE.  Frame is present and correctly framed; edges are visibly stair-stepped versus `off`, as expected from 2D LITE upscaling.  No depth speckle, no z-fighting, no missing/black geometry. |
| `20261127-100423-srev-tev2d-050b/screen.png` | `...SCALE=50%...` (`mode=tev2d`, `scale=0.5`) | 120x200 content in 128x256 POT, same 2 passes.  Strongest stair-stepping of the three SR scales, consistent with the lowest internal resolution.  No z-fighting. |
| `20261127-100319-srev-tev2d-064/screen.png` | `...SCALE=64%...` (`mode=tev2d`, `scale=0.64`) | 154x256 content in 256x256 POT.  Edges are less stair-stepped than `0.53`/`0.50`; no z-fighting, colour corruption or depth shimmer. |

POT sizes above come from `n3ds_sr_storage_size` and are asserted by the host
test; the runtime file only records mode/scale/DRS.  The guest's stderr
(`PICA_SR_ACTIVE ... texture=...` and the `VRAM: ... depth(D16) ...` line) is
not forwarded into Azahar's log, so it could not be copied.  However,
`sr_log_file()` is called at the **end** of a successful `sr_init()`, after
`C3D_RenderTargetCreateFromTex(..., GPU_RB_DEPTH16)` and the pass plan have
succeeded; a per-run `N3DS_SR.log` therefore proves the D16 scene target and
the pass chain were created, not merely that the config parsed.

Run notes and limits:

- Azahar does not forward host environment variables to the homebrew process:
  launching it with `N3DS_SR`/`N3DS_SR_SCALE` set in the parent shell produced
  no `N3DS_SR.log` and an unscaled frame.  Every archived run therefore used
  `sdmc:/N3DS_SR.cfg`.  The `0.53` row omits `scale`, so the default path is
  what was exercised.
- `tools/run_azahar.ps1` reports `FAIL` for all four runs only because this
  `-Mode com` demo writes no `Skyrim_N3DS.log`/`PASS` marker.  The emulator
  stayed alive until closed, the window painted, and `N3DS_SR.log` was
  written; these are screenshots, not a crash report.
- The demo's rotation angle is `svcGetSystemTick()`-based, so the four
  screenshots land on different rotation phases.  A pixel-exact diff is not
  meaningful; the comparison above is about framing, edge quality and the
  absence of depth artefacts.  `0.53` shows more aliasing than `0.64` (lower
  internal resolution) but **no z-fighting or depth-order artefacts relative
  to `0.64` or `off`**.
- `azahar_log.txt` for each run contains only the known non-renderer errors
  (`Failed to find title id for ROM` for a `.3dsx`, `ClCertA file missing`,
  `ConfigureNew3DSCPU` stub).  There are **no** Render/OpenGL/citro3d errors
  and no fault report.

Azahar is vsync-locked, so these runs prove correctness and framing, **not**
frame cost.  No device timing was taken; the native/0.53/0.64 frame-time
comparison remains open.

Historical 0.8/pre-change captures referenced by earlier revisions of this
document lived in other worktrees and are not present in this one; they are
superseded by the table above.

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

