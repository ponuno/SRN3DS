# Vendored MojoShader (host only)

This directory is a **host-only, build-time vendor** of
[MojoShader](https://github.com/icculus/mojoshader), used by `tools/sm3_dump`
to produce a canonical offline SM3 IR for the shader census.

| Field | Value |
|---|---|
| Upstream | `https://github.com/icculus/mojoshader` |
| Commit | `ad5dff84830c` (2026-09-03) |
| License | zlib — see `LICENSE.txt` (kept verbatim) |
| Files vendored | `mojoshader.c`, `mojoshader.h`, `mojoshader_internal.h`, `mojoshader_common.c`, `profiles/mojoshader_profile.h`, `profiles/mojoshader_profile_common.c`, `profiles/mojoshader_profile_d3d.c` |
| Modifications | none; the two profile files are unmodified upstream copies |

Rules for this vendor:

* **Never** add these files to the target build (`source/`, Makefile, any
  `*.3dsx`/`*.elf` link). The 3DS runtime has no SM3 interpreter and must not
  gain one through this directory.
* Only `SUPPORT_PROFILE_D3D=1` is compiled; every other profile stays `0`.
* MojoShader is zlib-licensed: the copyright notice stays in `LICENSE.txt` and
  in the file headers. Nothing here is claimed as original work.
* The census result is **parser coverage**, not translation coverage. A chunk
  that MojoShader parses is a chunk that is *understood*, not a chunk that is
  *drawn*.

Build (host, Clang; also see `tools/build_sm3_dump.ps1`):

```powershell
clang -std=c99 -O1 -DMOJOSHADER_NO_VERSION_INCLUDE=1 `
  -DSUPPORT_PROFILE_D3D=1 -DSUPPORT_PROFILE_BYTECODE=0 `
  -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_GLSL=0 `
  -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=0 `
  -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_ARB1=0 `
  -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_METAL=0 `
  -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 `
  -Itools/mojoshader -Itools/mojoshader/profiles `
  tools/mojoshader/mojoshader.c tools/mojoshader/mojoshader_common.c `
  tools/mojoshader/profiles/mojoshader_profile_common.c `
  tools/mojoshader/profiles/mojoshader_profile_d3d.c `
  tools/sm3_dump.c -o build_host/sm3_dump.exe
```
