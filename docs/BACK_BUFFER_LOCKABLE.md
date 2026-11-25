# Lockable back buffer and explicit depth-stencil: design contract

Date: 2026-11-25. Status: design; implementation not started.

## Why

The measurement in `docs/HWTL_DECISION.md` shows the only presentation
parameter TESV asks for that this bridge rejects is
`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` (`0x1`). Everything else — 400x240,
`X8R8G8B8`, one back buffer, `DISCARD`, no MSAA, windowed, interval ONE — is
already supported. After `CreateDevice` succeeds, the engine will request an
explicit depth-stencil surface (`EnableAutoDepthStencil` is 0 in its
`D3DPRESENT_PARAMETERS`).

## A. Lockable retained back buffer

`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is a promise that `LockRect` on the back
buffer succeeds and that its contents survive until the next `Present`. The
screen framebuffer on PICA is a transfer target and cannot be sampled, so the
honest implementation is a retained offscreen surface:

1. The backend creates a colour target sized to the next power of two at or
   above the display mode (512x256 for 400x240), format RGBA8, with the depth
   attachment the target already has.
2. `BeginScene` binds that target instead of the screen target. The existing
   screen-oriented clip transform still maps the 400x240 viewport into its
   upper-left region, so geometry is unchanged.
3. `Present` blits the 400x240 region of the retained target to the screen
   (textured quad, same clip mapping, nearest sampling, no format conversion),
   then transfers the screen.
4. Back-buffer `LockRect`/`UnlockRect` read/write the retained target through
   the existing target readback path. A read-only lock and a full write-back
   are both real operations on that surface.
5. `StretchRect` from the back buffer becomes possible because the source is a
   sampleable texture; it stays `NOTAVAILABLE` only if the backend cannot do
   the copy.

Honesty rules:

- `CreateDevice` accepts `Flags==0` always and `Flags==LOCKABLE_BACKBUFFER`
  only when the backend advertises the retained back buffer and readback.
  Any other flag bit is rejected with `D3DERR_NOTAVAILABLE` and a named reason.
- If the backend cannot retain/read the back buffer, `0x1` is rejected — no
  silent promise.
- Present of the retained target must be pixel-verified, not assumed.

Open questions to settle during implementation:

- Exact orientation/scale mapping of the present blit (screen is stored rotated
  240x400; the existing `ui.copy` and StretchRect paths have the same problem).
- Whether the 512x256 target wastes acceptably little VRAM (512 KiB) — yes, but
  the budget accounting must include it.
- Whether `Present` must keep the previous frame (DISCARD semantics) or clear;
  D3D9 with `DISCARD` may discard after Present, but a lockable back buffer is
  usually read before Present. Implement "retained until next Present".

## B. Explicit depth-stencil surface

After `CreateDevice` the engine creates a depth-stencil surface and binds it.

Proposed bounded contract (adjust to the measured request):

- Supported: `Width=400, Height=240`, `Format=D24S8 (75)` or `D24X8 (77)`,
  `MultiSampleType=0`, `Quality=0`, `Lockable=FALSE`, pool DEFAULT.
- The surface maps to the single PICA depth attachment of the bound colour
  target. Creating a second simultaneously bound depth surface, or one with a
  different size, fails with `D3DERR_NOTAVAILABLE` and a named reason.
- `SetDepthStencilSurface` binds/unbinds that state; `NULL` restores the
  implicit attachment.
- `Clear` with `D3DCLEAR_ZBUFFER`/`D3DCLEAR_STENCIL` clears the PICA depth
  attachment for the bound target; clearing without a bound depth surface is
  `D3DERR_INVALIDCALL` (already the current behaviour).
- Depth testing stays driven by `D3DRS_ZENABLE`/`ZWRITEENABLE`/`ZFUNC`, which
  the backend already maps.
- `CheckDepthStencilMatch` must accept exactly the combinations above.

Honesty rules: no "depth surface" handle that cannot be cleared or tested;
dimension/format mismatches fail explicitly.

## Test plan

- Host core/COM: flags acceptance matrix, lockable read/write round-trip,
  Present retention, depth create/bind/clear/reject, refcounts, Reset.
- Null backend: full state machine plus pixel checks through the CPU surfaces.
- PICA: isolated pixel proof for (a) clear + draw into the retained target,
  (b) present blit correctness including orientation, (c) depth clear + depth
  test. Azahar run with FNV evidence, like `rt_switch`/`ui.copy`.
- TESV behind the measurement probe: `CreateDevice` must return `S_OK`, then
  record the depth request and the first post-device calls.
