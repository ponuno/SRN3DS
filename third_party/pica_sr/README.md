# PICA-SR / libFSR3DS (vendored)

Third-party source copy used by the Citro3D D3D9 backend for spatial
upcaling between the internal render target and the LCD framebuffer.

| | |
| --- | --- |
| Project | PICA-SR (library name `libFSR3DS`) |
| Author | ponuno |
| Upstream URL | <https://github.com/ponuno/PICASR-FSR-for-3DS> |
| Version | 0.3 (`FSR3DS_VERSION "0.3"`) |
| Copied on | 2026-11-27 |
| Licence | none declared upstream; this repository is the same owner's code, so the copy carries no third-party licence obligation. |

## What was copied

Only the reusable post-processing library, verbatim and unmodified:

- `fsr3ds.c` — TEV kernel configuration (REPLACE/LITE/ADAPT/AA/TEV 2DQ),
  DRS controller, no I/O, no shaders.
- `fsr3ds.h` — public API and the fitted `FSR3DS_2DQ_*` constants.

Nothing else from upstream (`app.c`, `gfx.c`, `ui.c`, `bench.c`, geometry,
FurMark scene, PICA blit shader, assets, build files) is vendored. This repo
already owns an equivalent fullscreen blit vertex shader
(`source/d9shader.v.pica`) and its own render-target plumbing.

## Upstream files not copied and why

`gfx.c` is the reference integration (POT scene textures, viewport placement,
UV offsets, pass ordering for TEV 2DQ). Its behaviour is documented in
`docs/PICA_SR.md` here; the code itself is FurMark-specific (geometry, UI,
benchmark) and is not part of the library.

## Local use

`source/d3d9_citro3d.c` includes `fsr3ds.h` and links `fsr3ds.o`
(`Makefile`: `VPATH`/`INCLUDE` point at this directory). The host-side
configuration/geometry helper `source/n3ds_sr.h` is ours and does not depend
on this library.
