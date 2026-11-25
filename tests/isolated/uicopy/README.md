# Isolated `ui.copy` PICA200 draw proof

This fixture proves one synthetic backend-compatible `ui.copy` submission. It
does **not** run Skyrim, the Skyrim menu, or a game frame.

## Path exercised

```text
exact ps_3_0 ui.copy tokens
  -> d9_shader_analyze_tev
  -> guest x86 COM stdcall CreatePixelShader / SetPixelShader
  -> d3d9_core draw state
  -> d3d9_citro3d TEV apply
  -> C3D_DrawArrays
  -> Present
```

The token stream is exactly:

```hlsl
dcl_texcoord t0.xy;
dcl_2d s0;
texld oC0, t0, s0;
```

It has one texture, one texcoord0 input, one `REPLACE(TEXTURE0)` TEV stage, no
external constants, and no vertex colour. The shared fixture also asks the
pipeline registry to select `ui.copy` and checks that its emitted plan equals
the analyzer's TEV program.

Geometry is one oversized fullscreen triangle with position + texcoord0. The
source is an 8x8 A8R8G8B8 texture with 64 distinct known texels. Sampling is
point/clamp, depth and blending are disabled, and colour writes are enabled.

## Files

| File | Purpose |
|---|---|
| `fixture.h` | Exact tokens, texels, validation, and real guest COM/stdcall harness |
| `host_smoke.c` | Mock-backend pointer/texture/state/draw-count regression |
| `main.c` | ARM/Azahar integrated draw and deterministic readback |
| `rt_switch.c` | ARM/Azahar two-target switching, independent readback and back-buffer restore |
| `Makefile` | Isolated devkitARM build; does not alter the main build |
| `shbin_blob.S` | Embeds the production `d9shader.v.pica` blob for the real backend |

`main.c` includes `source/d3d9_citro3d.c` so the test can replace only the
backend's render target with a readable VRAM target and count calls to the real
TEV apply and `C3D_DrawArrays`. The wrappers call the production implementations;
they do not substitute a mock renderer.

## Host regression

From the repository root in a Visual Studio developer command prompt:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only uicopy_host -OutDir build_host_uicopy
```

Expected:

```text
ui.copy host: 52 checks, 0 errors
HOST PASS: uicopy_host
```

The mock verifies:

- COM retains the same TEV plan produced by analysis and the registry;
- `SetPixelShader` propagates a stable borrowed shader pointer to core/backend;
- the native 8x8 texture and exact PICA-swizzled texel bytes reach the backend;
- position/UV vertices, clip matrix, sampler state, and render state propagate;
- each draw/Present increments the expected counters;
- `SetPixelShader(NULL)` restores the fixed path and shader release is safe.

## ARM/Azahar integrated proof

Build from this directory:

```powershell
$env:PATH = 'C:\devkitPro\msys2\usr\bin;' + $env:PATH
make clean
make DEVKITPRO=C:/devkitPro
```

This builds both `uicopy_arm.3dsx` and `rt_switch_arm.3dsx`.

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_azahar.ps1 `
  -ThreeDsx tests\isolated\uicopy\uicopy_arm.3dsx `
  -Name uicopy_arm -TimeoutSec 12
```

The guest holds the rendered frame until START or the bounded runner closes
Azahar. The final 128x128 run recorded:

```text
ui.copy ARM: PASS (16 checks, 0 errors)
tev_apply_calls=1 draw_calls=1 primitive=0 first=0 size=3 cursor=3 scene=0
PIXEL_PROOF=PASS capture=128x128 detail=unknown=0 bad_counts=0 bad_shapes=0 expected_each=256
pixel_fnv1a32=e34b4dc5
scope=synthetic ui.copy only; NOT a Skyrim menu or game frame
```

### Pixel check

The test releases the default render target's VRAM, then tries a 128x128 VRAM
colour texture (falling back to 64x64 and 8x8 if fragmented). The real backend
draws the 8x8 source to it with the one fullscreen triangle. After `Present` and
`C3D_FrameSync`, `C3D_SyncDisplayTransfer` copies the colour buffer to linear
memory.

The check requires:

- all 64 expected PICA-format texel words appear;
- no unknown pixels appear;
- every texel covers exactly `capture_size² / 64` pixels;
- each texel forms one contiguous square block of side `capture_size / 8`.

At 128x128 this is 256 pixels per texel in a 16x16 block. This is deterministic
pixel evidence, not a screenshot-only claim. The FNV-1a32 hash records the
actual captured buffer; it changes if a smaller fallback target is used.

If render-to-texture or transfer is unavailable, the guest keeps the real
screen-target path and logs `PIXEL_PROOF=NO` with the reason. That result is
explicitly state/counter evidence only. A transfer mismatch is a test error,
not an unavailable-readback fallback.

## Render-target switching proof

`rt_switch.c` uses the production backend callbacks rather than replacing the
screen target. It creates two lockable 64x64 A8R8G8B8 surfaces through the real
guest COM vtable, then verifies:

- target A clears and reads back solid red;
- restoring the implicit back buffer and clearing it blue does not touch A;
- target B clears and reads back solid green, while A remains red;
- the exact `ui.copy` triangle draws only into A and passes the same
  histogram/block pixel proof in top-first D3D format;
- B remains green after A's draw;
- `GetRenderTarget` reports A's COM identity and then the implicit back buffer
  after `SetRenderTarget(0, NULL)`.

Recorded Azahar 2126.2 run (`build_azahar/20261125-211721-rt_switch_arm`):

```text
render-target ARM: PASS (38 checks, 0 errors)
target_a=0x50002070 target_b=0x50002080 back=0x50002020 size=64x64
red=1 green=1 a_unchanged=1 b_unchanged=1 restore=1 identity=1
PIXEL_PROOF=PASS detail=unknown=0 bad_counts=0 bad_shapes=0 expected_each=64
pixel_fnv1a32=54d9adc5
```

`rt_switch_arm.3dsx` is 730,412 bytes, SHA-256
`bdc43ec480275cbb1447e2e4fb94e4e851fb344cce64af9384c11a747d1074a5`.
This proves target switching/readback only; it is not a Skyrim frame and does
not prove arbitrary offscreen orientation or depth attachment.

## Recorded evidence

Final run on 2026-11-24 with Azahar 2126.2:

- ARM: `build_azahar/20261124-203413-uicopy_arm_final/`
- `.3dsx` SHA-256:
  `55B13320865573D4B71DD166A6AFF3AB5100D0C20EC1E4832A99E21C6DA2786E`
- captured pixel-buffer FNV-1a32: `e34b4dc5`
- Host: `build_host_uicopy_final/logs/host_tests_20261124-203008-25544.log`
- Host ASan/UBSan: `build_host_uicopy_sanitize/logs/host_tests_20261124-203336-27220.log`
- Existing `tev_com` regression also passed in the same host run.

The archived screenshot is supplemental; the readback histogram and block
shape are the pixel proof.
