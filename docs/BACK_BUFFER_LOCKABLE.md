# Lockable back buffer and explicit depth-stencil: implementation record

Date: 2026-11-26. Status: **implemented and host-tested; PICA path built for ARM,
not yet run on Azahar or hardware.**

Supersedes the 2026-11-25 design note. The measurement in `docs/HWTL_DECISION.md`
shows the only presentation parameter TESV asked for that the bridge rejected was
`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` (`0x1`). Everything else - 400x240,
`X8R8G8B8`, one back buffer, `DISCARD`, no MSAA, windowed, interval ONE - was
already accepted. After `CreateDevice` succeeds the engine requests an explicit
depth-stencil surface (`EnableAutoDepthStencil` is 0).

## 1. What is implemented

### 1.1 Retained lockable back buffer

`d9_set_back_buffer_info_ex(device, w, h, format, lockable)` in `d3d9_core.c`:

- `lockable == 0` keeps the previous implicit-screen back buffer exactly as
  before (no new targets, no new pixels).
- `lockable != 0` requires the backend to provide `target_create`,
  `target_delete`, `target_bind`, `target_clear`, `target_read`, `target_write`
  and `target_blit`. If any is missing the call returns `D9_UNSUPPORTED`; COM
  maps that to `D3DERR_NOTAVAILABLE` with a named reason. There is no silent
  promise and no shadow buffer.
- The retained colour target is the next power of two at or above the display
  mode: **512x256 RGBA8 for 400x240**. It is a normal core `Target` (one of the
  four slots), costs 512 KiB of the device budget, and the logical surface stays
  400x240.
- `BeginScene` binds the retained target instead of the screen. The existing
  screen-oriented clip transform still maps the 400x240 viewport into its
  upper-left region, so guest geometry is unchanged.
- `d9_present()` binds the screen target inside the active frame and blits the
  full 400x240 region of the retained target to the screen (one textured quad,
  nearest sampling, no format conversion). A rejected blit fails `Present`; it
  is never reported as success.
- **Orientation contract.** The LCD framebuffer is stored rotated 240x400, so
  every core clip matrix is screen-oriented. An offscreen target is a normal D3D
  surface: `LockRect` must return a D3D top-left image and `Present` samples it
  with direct UVs. The PICA backend therefore post-multiplies the clip output by
  `clip'_x = -clip_y, clip'_y = -clip_x` while an explicit/retained target is
  bound (and uses the same permutation for a blit whose destination is
  offscreen). Combined with the existing `d9_swizzle`/`d9_unswizzle` vertical
  convention this stores the frame unrotated, so lock readback is a D3D image
  and the present quad's UV `(x/400, y/240)` samples the same logical pixel it
  rasterises. The host null-backend tests cannot prove GPU orientation; this is
  reasoned from the existing screen mapping and must be confirmed on Azahar. It
  also means explicit render targets are now unrotated (previously they
  inherited the screen rotation; the old pixel proofs used solid colours or
  orientation-insensitive counts, so this is not a regression in those proofs).
- `D3DSWAPEFFECT_DISCARD` semantics: the retained target is cleared at every
  `BeginScene` - with the queued `Clear` colour when the guest asked for one,
  zero otherwise. Its contents are therefore guaranteed from the last
  `BeginScene`/`Clear` or `LockRect`/`UnlockRect` until `Present`. Reading it
  after `Present` and before the next `BeginScene` returns the previous frame,
  which DISCARD explicitly permits ("undefined").
- `LockRect`/`UnlockRect` on the back buffer are real operations:
  - a read-only lock reads through `d9_render_target_read` (PICA
    `C3D_SyncTextureCopy` + unswizzle);
  - a writable lock is a read-modify-write: the rectangle is copied into the
    guest lock pointer, `UnlockRect` calls `d9_render_target_write`, which reads
    the full target, patches the rectangle and re-uploads the whole level
    (`C3D_TexLoadImage` after `d9_swizzle_rgba8`). Nothing is only recorded.
- Because the back buffer is now a sampleable texture, `StretchRect` **from**
  the back buffer is implemented: the core resolves handle `0` to the retained
  target and passes its logical (400x240) and storage (512x256) dimensions
  separately in `D9BlitSurface.storage_width/storage_height`, so the PICA
  source UVs stay correct. A non-lockable device keeps the old
  `D9_BLIT_BACK_BUFFER_SRC` rejection.

### 1.2 Explicit depth-stencil surface

`CreateDepthStencilSurface` / `SetDepthStencilSurface` /
`GetDepthStencilSurface` / `Clear(Z|STENCIL)` / `CheckDepthStencilMatch`:

- Accepted: `Width=400, Height=240`, `D3DFMT_D24S8 (75)` or `D24X8 (77)`,
  `MultiSampleType=0`, `Quality=0`, `Lockable=FALSE`, pool DEFAULT. Both formats
  are the same 32-bit PICA `DEPTH24_STENCIL8` attachment.
- Rejected with named reasons: any other size/format/MSAA/quality, a lockable
  depth surface, a shared handle, creation inside `BeginScene`/`EndScene`, and a
  backend without `depth_clear`.
- `SetDepthStencilSurface(NULL)` detaches; then `Clear` with
  `D3DCLEAR_ZBUFFER`/`D3DCLEAR_STENCIL` is `D3DERR_INVALIDCALL` (the previous
  behaviour), and `GetDepthStencilSurface` returns `D3DERR_NOTFOUND`.
- A queued depth clear is applied by `BeginScene` after the target bind, through
  `d9_clear_depth()`, which requires an active scene, the bound depth surface and
  a size match with the active render target. `z` must be finite and in `[0,1]`;
  the packed value written to the attachment is `depth24 | (stencil << 24)`.
- The PICA backend creates every colour target (screen and explicit) with
  `C3D_RenderTargetCreateFromTex(..., GPU_RB_DEPTH24_STENCIL8)`, binds depth
  testing for offscreen targets too (previously disabled), and implements
  `depth_clear` with `C3D_RenderTargetClear(target, C3D_CLEAR_DEPTH, 0, packed)`.
- **Known PICA limitation, stated rather than hidden:** depth and stencil live
  in one 24+8 buffer and the GPU clear writes the packed word. A Z-only clear
  therefore also writes the stencil byte (the caller's stencil argument is used
  when supplied, otherwise 0). Exact Z-only/stencil-preserving clears would need
  a separate attachment format that the hardware does not have.
- Depth testing itself stays driven by `D3DRS_ZENABLE/ZWRITEENABLE/ZFUNC`; the
  surface only carries the size/format/binding/clear contract.

### 1.3 Presentation acceptance

`CreateDevice`/`Reset` accept `Flags` 0 or exactly `0x1`. `0x1` is accepted only
when the installed backend advertises the retained-back-buffer capability; the
old `TESV_D3D9_PP_PROBE` measurement env var and its zero-filled shadow are
deleted. Any other flag bit, any back-buffer size other than 400x240 (or 0,0),
non-`DISCARD` swap effect, MSAA, fullscreen, automatic depth-stencil, or an
interval other than immediate/one keeps the previous named
`D3DERR_NOTAVAILABLE` rejection.

## 2. Backend contract

`D9Backend` has two appended optional callbacks (append-only, so every existing
positional initializer is unchanged):

```c
int (*target_write)(void *ctx, void *native, const void *bgra,
                    size_t pitch, unsigned w, unsigned h);
int (*depth_clear)(void *ctx, unsigned flags, uint32_t depth_stencil);
int (*target_viewport)(void *ctx, unsigned w, unsigned h);
```

`target_write` replaces the whole surface (the read-modify-write lives in the
core). `depth_clear` must fail, not ignore, a clear it cannot perform.
`target_viewport` is required for the retained back buffer: `C3D_FrameDrawOn`
sets the GPU viewport to the target *storage* size (512x256), so the core tells
the backend the logical 400x240 surface size after every offscreen bind. Without
this the frame would be stretched into the padded storage and the present blit
would sample the wrong region.

New core API: `d9_set_back_buffer_info_ex`, `d9_back_buffer_target`,
`d9_backend_supports_lockable_back_buffer`, `d9_back_buffer_info_ex`,
`d9_render_target_write`, `d9_create_depth_surface`,
`d9_release_depth_surface`, `d9_set_depth_surface`, `d9_get_depth_surface`,
`d9_depth_surface_info`, `d9_clear_depth`, `d9_end_scene`.

## 3. Test evidence

- `tests/back_buffer_lockable_smoke.c` (new, host, null backend): the exact TESV
  presentation set; 512x256 retained target with a 400x240 logical surface;
  pixel-verified `Present` blit; `LockRect` read of the presented frame;
  writable `LockRect`/`UnlockRect` round-trip; `StretchRect` sampling the back
  buffer into an explicit target; DISCARD clear of the retained target;
  depth-surface create/reject/attach/detach/clear with the packed value checked;
  `Reset` with the depth surface still alive rejected, then accepted after
  release; `Reset` back to `Flags=0` making `LockRect` fail explicitly.
- `tests/null_backend_smoke.c`: the null backend now owns real BGRA pixels for
  targets and a private 400x240 presented image; lockable target read/write is
  exercised, `Reset` with `Flags=1` succeeds, the old "no readback" rejection is
  updated.
- `tests/device_create_smoke.c`: a six-callback backend without retention still
  rejects `Flags=1` and `Flags=2`.
- The full host suite and the TESV host slice results are recorded in
  `docs/PORT_LOG.md`; PICA pixel proof (Azahar) for the retained target, present
  orientation and depth clear is **not** done yet. The ARM build compiles and
  links both shaders; that is build evidence, not a rendered frame.

## 4. Remaining work / integration notes

1. TESV host slice (2026-11-26, caps probe on, 2,000,000,000 steps):
   `CreateDevice` now returns `S_OK` with `Flags=0x1`. The engine then sets
   VIEW/PROJECTION/viewport, creates its 2x1 MANAGED texture and locks its level
   surface, probes MSAA and one optional post-pixel-shader-blending format, and
   aborts at the **SM3 gate `0x00c6e22d`** ("ShaderModel3 required but not
   supported by your video card."). It does not yet call
   `CreateDepthStencilSurface`, `Clear`, `BeginScene`, `Present` or any
   fixed-function lighting entry. No guest fault; `ExitProcess(0)` from
   `0x0069107d`. See `docs/HWTL_IMPLEMENTATION.md` section 5.
2. PICA pixel proof on Azahar: (a) clear+draw into the retained 512x256 target,
   (b) present blit orientation/scale, (c) depth clear + depth test. Until then
   the PICA path is compiled, reviewed and structurally identical to the tested
   null path, but not pixel-proven.
3. VRAM/budget: the retained target costs 512 KiB and its depth attachment
   another 512 KiB. The core budget accounts the colour target only; the depth
   attachment is a fixed per-target PICA cost and is documented, not silently
   ignored.
