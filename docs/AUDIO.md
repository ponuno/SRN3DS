# Audio: XAudio2, waveOut and the 3DS sink

Status: **host-verified mixer and COM semantics; ARM compile-only; no physical
3DS/NDSP listening evidence.** This document separates what the code does, what
the tests prove, and what is still a stub or unreachable. DevCaps and frame
claims are deliberately unchanged.

## 1. What the guest actually imports

`E:\skyr\TESV.exe` (the binary this worktree audits) has these audio imports:

| DLL | Imports |
| --- | --- |
| `X3DAudio1_7.dll` | `X3DAudioInitialize`, `X3DAudioCalculate` |
| `WINMM.dll` | `timeGetTime` (no `waveOut*`) |
| `binkw32.dll` | `_BinkSetSoundSystem@8`, `_BinkOpenDirectSound@8`, ... |

There is **no `XAudio2Create` in the IAT and no XAudio2 DLL import**. A raw
string scan also finds no `XAudio2Create`, `xaudio2_6`, `xaudio2_7` or
`xaudio2_8`; the only `XAudio2` string is the value of the `sAudioAPI:Audio`
setting. `CoCreateInstance` is imported, but XAudio2 is not a COM class and the
runtime's `CoCreateInstance` is a `REGDB_E_CLASSNOTREG` stub.

Consequences, stated plainly:

- The XAudio2 shim is registered for `xaudio2_6/7/8/dll!XAudio2Create`
  (`source/win32_xaudio2.c`), but **this exe never reaches it through the IAT**.
- If some build resolves it at runtime with `LoadLibraryA`/`GetProcAddress`, the
  current `module_handle()` in `source/win32_shims.c` does **not** recognise
  `xaudio2_*.dll` or `xapofx1_5.dll`, so `LoadLibraryA` fails with error 126 and
  `GetProcAddress` cannot reach the shim. This is a known, unclosed gap (fixing
  it belongs in `win32_shims.c`, outside this task's allowed files).
- The only audio code this binary can be shown to call directly is X3DAudio,
  which is a stereo-only shim (`X3DAudio1_7`) and does not feed XAudio2 voices
  here.

Therefore: **menu/world audio through XAudio2 is not proven reachable from this
TESV.exe.** Everything below proves the shim itself is correct when called; it
does not claim the game currently calls it.

## 2. Supported formats and conversion

`set_format()` parses `WAVEFORMATEX`/`WAVEFORMATEXTENSIBLE` (`tag 0xFFFE` reads
the sub-format GUID tag at `+24`):

| tag | decoding |
| --- | --- |
| 1 PCM | 8 (unsigned), 16, 24, 32-bit signed |
| 3 IEEE float | 32-bit float |
| 2 MS ADPCM | block walk with the guest coefficient table; `spb` samples/block |
| 0x0161 WMA / 0x0166 XMA / anything else | **not decoded**: silence, logged, but the buffer holds its nominal duration (`bytes / nAvgBytesPerSec`) so the queue does not spin |

Output is always **interleaved stereo PCM16 at `WIN32_AUDIO_OUTPUT_HZ` =
32730 Hz**, the rounded NDSP DAC rate (`16756991/512 = 32728.498`; +0.005 %
pitch error, documented in `win32_audio.h`). Source rates other than 32730 are
resampled with linear interpolation (`step = ratio * src_rate / out_rate`);
there is no anti-alias filter, so material above ~16.3 kHz aliases. Mono is
duplicated to both channels by the default matrix; the master downmix handles
1/2/3/4+ channels and clips each channel to [-1, 1] before the s16 conversion.

## 3. Voice graph and buffer semantics

Implemented objects live in guest memory (`0x1e300000..`, vtables at
`0x1e300000`, callback stack `0x1e310000`, thunks `0xff0a0000`):

- `IXAudio2`: create/initialize/start/stop engine, device count/details,
  `RegisterForCallbacks`/`UnregisterForCallbacks`, create source/submix/master.
- Voices: details, sends/output matrix, volume + channel volumes, filters and
  effects accepted and ignored, `DestroyVoice`.
- Source: `Start`, `Stop`, `SubmitSourceBuffer`, `FlushSourceBuffers`,
  `Discontinuity`, `ExitLoop`, `GetState`, `Set/GetFrequencyRatio`,
  `SetSourceSampleRate`.

Semantics now covered by `tests/xaudio2_smoke.c` and
`tests/xaudio2_signal_smoke.c`:

- **Queue order and context**: FIFO, `pContext` reaches `OnBufferStart`,
  `OnBufferEnd`, `OnLoopEnd`; `GetState.pCurrentBufferContext` is the playing
  buffer and becomes NULL when the queue drains. `XAUDIO2_END_OF_STREAM` raises
  `OnStreamEnd` after the final `OnBufferEnd`.
- **Completion is a function of the clock**: every shim entry calls
  `win32_xaudio2_tick(0)` first; the mixer runs 10 ms passes. No audio thread,
  no locks, callbacks run after the pass with a consistent queue.
- **Callbacks are COM objects now**: the shim dereferences
  `pCallback -> vtable -> method` (this was a regression from upstream, which
  did `M32(M32(cb) + 4*method)`; the old code treated the object pointer as a
  vtable and would crash on a real C++ callback). Argument counts are exact:
  pass-start gets `(this, bytesRequired)`, pass-end/stream-end get `(this)`,
  buffer/loop callbacks get `(this, pBufferContext)`, voice-error gets
  `(this, pBufferContext, HRESULT)`.
- **`SamplesPlayed`** is a 64-bit source-frame counter, monotonic and exact at
  ratio 1; it advances with consumed source frames under resampling. `GetState`
  returns low/high dwords as XAudio2 2.6 does.
- **Frequency ratio** is clamped to `[1/1024, maxRatio]`; NaN/Inf and <= 0 are
  rejected with `XAUDIO2_E_INVALID_CALL` and leave the old ratio in place. The
  resampler step is bounded to 64 source frames per output frame, so a hostile
  ratio cannot spin the mixer.
- **Underrun** is counted per starved source frame (`underruns` in
  `Win32XAudio2Stats`), never looped. The voice keeps running and resumes when
  a buffer arrives.
- **`FlushSourceBuffers`** removes the whole queue immediately (including the
  playing buffer), sets `BuffersQueued` to 0 before returning and reports
  `OnBufferEnd` for every removed buffer. This matches real XAudio2 and was
  previously a documented deviation that kept the playing buffer.
- **`DestroyVoice` with live buffers** frees decoded buffers and the mix
  accumulator immediately; if the engine is stopped there is no later pass to
  leak into. Pending callback events for a dead voice are skipped.
- **Mixing**: source volume is applied once in `route()`, mastering volume once
  at the master, default speaker matrices follow XAudio2's mono/stereo rules,
  submixes run in processing-stage order. Two sources sum; the master clips.
- **Unsupported WMA/XMA**: silent but duration-accurate (see §2).
- **`SetSourceSampleRate`** rejects 0 or > 384000 Hz with `E_INVALIDARG`.

Known differences that remain:

- Effects/filters (`SetEffectChain`, `EnableEffect`, `SetFilter`, ...) return
  `S_OK` and do nothing (same as upstream), including XAPOFX `CreateFX`, which
  returns a live IUnknown-shaped stub.
- `OnVoiceProcessingPassEnd` is delivered for running source voices; real
  XAudio2 also calls it for stopped voices in some states, not modelled.
- No voice reverb/sends to effect chains, no 5.1 speaker matrix beyond the
  simple downmix, no hardware mixing.
- Only one `IXAudio2`/master per guest cell space; device enumeration reports a
  single default stereo device at 32730 Hz.

## 4. Output sink: NDSP/OpenDSP, host null sink

`source/win32_audio_3ds.c`:

- On `__3DS__` it initialises **libctru ndsp** (default) or OpenDSP. ndsp uses
  one stereo PCM16 channel at 32730 Hz, four 1024-frame staging buffers,
  linear interpolation, soft clipping and a 12-entry mix register. `submit()`
  copies into linear memory, flushes the DSP cache and queues the wave buffer;
  when no slot is free it returns 0 (backpressure, counted by XAudio2 as
  frames not accepted).
- `N3DS_AUDIO=off` (or a missing DSP) installs no backend: XAudio2 still mixes
  and advances callbacks, but `win32_audio_submit_stereo()` accepts nothing.
  `N3DS_AUDIO=opendsp|auto` probes the DSP mailbox and falls back to ndsp.
- On the host (`#else`) the same file is a **counting null sink**, not a
  device. It accepts every frame and records submits/frames/volume/pause/flush
  calls. This is why host tests can verify mixing and callbacks but cannot
  verify sound.

The real DSP path is **not a stub**: it calls `ndspInit`, `ndspChnSetFormat`,
`ndspChnWaveBufAdd`, `ndspChnSetPaused`, etc. It has only been compiled for
ARM (`arm-none-eabi-gcc -D__3DS__`, see below), not run on a New 3DS or Azahar
with audio in this task. Do not cite it as audible until a device run is
archived.

## 5. Verification performed

Host (Windows, clang, `tools/run_host_tests.ps1`):

```
xaudio2            PASS  COM/API contract
xaudio2_signal     PASS  signal-level mixer test
audio              PASS  waveOut + X3DAudio
audio_ndsp_host    PASS  ndsp host (null) contract
audio_3ds_backend  PASS  win32_audio_3ds.c against fake libctru
com, lift          PASS
```

`xaudio2_signal` synthesises PCM and asserts:

- 1 kHz stereo sine at 0.5 full scale: peak 16382, RMS 11586 (expected
  0.5/sqrt(2) * 32767), 3270 source frames -> exact `SamplesPlayed == 3270`,
  one `OnBufferEnd` with the submitted context and one `OnStreamEnd`;
- underrun frames are counted after the queue drains;
- two mono voices sum to 3000, mastering 0.5 gives 1500, mastering 2.0 on two
  full-scale voices clips to 32767 without wrapping;
- mono -> stereo duplicate; 48 kHz -> 32730 consumes exactly 4800 source
  frames; ratio 2 consumes 654 frames in one 327-frame pass; NaN ratio rejected;
- flush while running: queue 0, two `OnBufferEnd`, `SamplesPlayed` frozen;
- destroy with a live buffer leaves only the master voice and survives further
  passes;
- engine pass-start/pass-end callbacks arrive with the correct argument count;
- a WMA (`0x0161`) buffer plays silence but stays queued for ~1 second instead
  of completing instantly.

ARM compile-only (devkitARM, `-march=armv6k -mfloat-abi=hard -D__3DS__`):
`win32_xaudio2.c`, `win32_audio.c` and `win32_audio_3ds.c` compile with exit 0
(warnings only from the pre-existing `staged/x86.h` MMX macros).

## 6. Remaining work

1. Decide how the game is supposed to load XAudio2. This exe has no import and
   no export-name string; if runtime loading is intended, add
   `xaudio2_6/7/8/dll` and `xapofx1_5.dll` to `win32_shims.c`'s
   `module_handle`/`module_name` (not done here to respect file ownership).
2. Real WMA/XMA decode for music and voice (`.xwm`/`.fuz`); today they are
   duration-accurate silence.
3. A device run (New 3DS or Azahar with audio capture) to confirm ndsp output,
   pitch against the 32730 Hz approximation, and underrun behaviour when the
   game thread stalls.
4. Filters/effects and X3DAudio-to-XAudio2 integration remain unimplemented.
