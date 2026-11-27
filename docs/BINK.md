# Bink video: real frames from `BGS_Logo.bik`

Status: **host-decoded video frame, verified.**  The Bink shim no longer
returns NULL for `_BinkOpen@8`.  On the Windows host it decodes the retail
`Data\Video\BGS_Logo.bik` with FFmpeg's real `binkvideo` decoder and copies the
decoded pixels into guest memory, honouring pitch and rectangle.  This is a
**video-frame** result only; it is not a menu frame and no in-engine frame is
claimed here.

## 1. What `TESV.exe` imports (IAT, measured with `tools/audit_game.py`)

`binkw32.dll`, nine `__stdcall` imports (the `@N` suffix is the argument byte
count):

| import | args | shim status |
|---|---|---|
| `_BinkOpen@8` | path, flags | real: parse header + decode to BGRA store, guest `BINK*` |
| `_BinkOpenDirectSound@4` | window | named skip (sound not needed for video) |
| `_BinkSetSoundSystem@8` | open fn, param | named skip, returns 0 |
| `_BinkClose@4` | handle | real: release store + guest struct |
| `_BinkDoFrame@4` | handle | real: pin current frame |
| `_BinkNextFrame@4` | handle | real: advance `FrameNum` |
| `_BinkWait@4` | handle | real: returns 0 (frame store is ready) |
| `_BinkPause@8` | handle, pause | real: toggles, returns previous |
| `_BinkCopyToBufferRect@44` | handle, dest, pitch, destheight, dx, dy, sx, sy, sw, sh, flags | real: rectangle copy into guest memory |

There is no `_BinkCopyToBuffer`, `_BinkGetSummary` or `_BinkGetRects` in the
IAT, so those were not needed.  The guest `BINK` struct receives
`Width, Height, Frames, FrameNum, FrameRate, FrameRateDiv` at offsets
0/4/8/12/16/20; the struct is allocated from the guest heap.

Real intro path (from `docs/MENU_HARNESS.md`): WinMain `0x69dadf` reads
`sIntroSequence:General`; a non-empty value plays `0xF500F0`
(`BGS_LOGO.BIK`).  `E:\harness_skyrim\Skyrim.ini` currently has
`sIntroSequence=` and `bPreloadIntroSequence=0`, so the call is skipped.

## 2. Decoder

There is no Bink decoder in this tree (`source/`, `tools/`, `third_party/`
contain none; `binkw32.dll` is mapped at `0x09000000` but not lifted).  The
honest host route is FFmpeg's `binkvideo` decoder:

* `source/bink_decode.c` parses the Bink 1 header (`BIKi`; frames at `0x08`,
  width `0x14`, height `0x18`, fps `0x1c/0x20`) and runs `ffmpeg -f rawvideo
  -pix_fmt bgra` once into `%TEMP%\srn3ds_bink_<pid>\bink_<pid>_<n>.bgra`.
* `bink_decode_copy_rect()` seeks to the requested frame and copies the
  requested rectangle, converting BGRA/RGBA/ABGR/ARGB/RGB/BGR/565 and honouring
  `BINK_COPY_FLIP`; pitch is bytes per destination row.
* No synthetic pixels exist.  If FFmpeg is missing or fails, `_BinkOpen@8`
  returns NULL with a `[bink]` reason.  On `__3DS__` there is no decoder, so the
  device keeps the documented NULL/named refusal.

FFmpeg is located via `TESV_BINK_FFMPEG`, then `PATH`, then
`%LOCALAPPDATA%\ffmpeg\*\bin\ffmpeg.exe`.

## 3. Host test and artifacts

`tests/bink_host_smoke.c` (registered as `bink_host`) opens the real
`E:\harness_skyrim\Data\Video\BGS_Logo.bik` and checks:

* header: 1280x720, 166 frames, 24000/1001 fps;
* frame 80: 56,355 / 921,600 lit pixels, 462 distinct sampled colours;
* frame 80 decoded twice is byte-identical;
* frame 80 vs frame 0 differs in 56,355 pixels;
* 64x32 rectangle at (100,50) copied to (16,8) with pitch 5184 matches source
  and leaves strided/out-of-rect bytes untouched;
* RGBA conversion swaps R/B and keeps alpha 255;
* a full frame copied into a mapped `GuestMemory` region is byte-identical.

Run: `tools/run_host_tests.ps1 -Only bink_host -SkipPython` -> PASS.

Artifacts from the last direct run:

```
C:\Users\vasya\AppData\Local\Temp\srn3ds_host_bink_frame\bink_frame.raw      (1280*720*4 BGRA)
C:\Users\vasya\AppData\Local\Temp\srn3ds_host_bink_frame\bink_frame.ppm      (frame 80)
C:\Users\vasya\AppData\Local\Temp\srn3ds_host_bink_frame\bink_frame_first.raw(frame 0)
C:\Users\vasya\AppData\Local\Temp\srn3ds_host_bink_frame\bink_frame_info.txt
C:\Users\vasya\AppData\Local\Temp\srn3ds_host_bink_frame\bink_frame_preview.png
```

The preview is the real Bethesda Game Studios logo.

## 4. Engine slice: not reached

The shim honours `TESV_BINK_DUMP=<dir>`: `_BinkCopyToBufferRect@44` writes the
exact guest rectangle as `bink_guest_<frame>.raw`, a `.ppm` for 32-bit
orders, and `bink_guest_info.txt`.

This worktree cannot build `tesv_slice`: `staged/` and the generated
`TESV_GEN` tables are absent, and `run_host_tests.ps1 -Only tesv_slice`
requires both.  Even with a slice, the shipped ini skips the intro.  So no
in-engine guest frame was captured and none is claimed.  To do it later:
provide `staged/` + `TESV_GEN`, set `sIntroSequence=BGS_Logo.bik` (or use
`TESV_INI_OVERLAY`), build with `-Mode tesv`, and run with
`TESV_FILE_TRACE=1 TESV_BINK_DUMP=<dir>`.
