# Upstream shims ported from Skyrim_Android

Base: `upstream-shims` branch at `c6dec33`; source reference
`C:\Users\vasya\Documents\Skyrim_Android\runtime` (identical for these files).
Scope is limited to mature Win32 shims: XAudio2/DirectInput (step 1) and
setjmp/msvcrt/heap behaviour (step 2). Shaders, menus, IR, streaming and
PICA-SR were not touched, and `DevCaps`/frame claims are unchanged.

## XAudio2 2.6/2.7 — `source/win32_xaudio2.c|.h`

Ported from upstream `xaudio2.c` (748 lines) into this runtime's guest-memory
COM model:

- `IXAudio2`: `XAudio2Create`, `GetDeviceCount/Details`, `Initialize`,
  `Register/UnregisterForCallbacks`, `CreateSourceVoice`, `CreateSubmixVoice`,
  `CreateMasteringVoice`, `Start/StopEngine`, `CommitChanges`,
  `GetPerformanceData`, `SetDebugConfiguration`.
- `IXAudio2SourceVoice` / `SubmixVoice` / `MasteringVoice`: voice details,
  output sends (`SetOutputVoices`), effect/filter stubs, volume and channel
  volumes, output matrix (`Set/GetOutputMatrix`), `DestroyVoice`, and the
  source methods `Start`, `Stop`, `SubmitSourceBuffer`,
  `FlushSourceBuffers`, `Discontinuity`, `ExitLoop`, `GetState`,
  `Set/GetFrequencyRatio`, `SetSourceSampleRate`.
- Formats: PCM8/16/24/32, IEEE float32, WAVE_FORMAT_EXTENSIBLE and MS ADPCM
  decoding (same tables/block walk as upstream). Unknown tags play silence and
  are logged, not guessed.
- Buffer queue: play region, loop begin/len/count (255 = forever), buffer
  context, `XAUDIO2_END_OF_STREAM`, and `OnBufferStart`/`OnBufferEnd`/
  `OnLoopEnd`/`OnStreamEnd` callbacks. `GetState` reports
  `pCurrentBufferContext`, `BuffersQueued`, `SamplesPlayed` in the upstream
  layout.
- Mixing preserves upstream behaviour: linear interpolation resampling,
  default speaker matrices, submix processing stages ordered by `stage`, and a
  stereo downmix at the mastering voice.
- `XAPOFX1_5!CreateFX` returns a live IUnknown-shaped stub object.

Changes required by the 3DS port:

- No SDL and no pthread engine thread. `win32_xaudio2_tick(now_us)` runs the
  same 10 ms pass on the calling (single guest) thread; every shim entry
  advances the clock first, matching `win32_audio`'s completion rule. The TESV
  slice loop calls it once per frame; host tests drive it with an explicit
  clock.
- Output is `win32_audio_submit_stereo()`: the same NDSP/OpenDSP sink that
  `waveOut` uses. `N3DS_AUDIO=off` (or a missing backend) mixes normally but
  accepts no frames; `win32_xaudio2_host_stats()` reports submitted frames so
  this is visible instead of hidden.
- No mutexes: there is exactly one guest thread and callbacks run after a pass
  with the queue in a consistent state, so a guest may submit/flush from
  `OnBufferEnd`.
- No engine thread means a stopped voice cannot be drained by a pass. Unlike
  upstream, `FlushSourceBuffers` on a stopped voice drops its queue
  immediately (closer to real XAudio2); a running voice keeps the playing
  buffer and marks the rest finished at the next pass.
- Object cells and vtables live at `0x1e300000..0x1e310000`, callback stack at
  `0x1e310000`, thunk window `0xff0a0000` (checked by
  `tools/check_shim_bases.py`).

Still absent: filters/effects are accepted and ignored (upstream too), no
X3DAudio integration into XAudio2 voices, no hardware-accelerated mixing, no
device enumeration beyond the single default device.

## DirectInput 8 — `source/win32_input.c`

Our module already implemented `DirectInput8Create`,
`IDirectInputDevice8` state/format/cooperative level/properties and the 3DS pad
profile. Upstream `dinput.c` semantics added:

- Buffered `GetDeviceData`: a 256-entry transition ring records key up/down,
  mouse axis deltas and mouse buttons when the state changes, so a tap between
  polls yields both elements. Elements use `DIDEVICEOBJECTDATA`
  (`dwOfs`, `dwData`, `dwTimeStamp`, `dwSequence`); keyboard uses DIK offsets,
  mouse uses `DIMOFS_X/Y/Z` (0/4/8) and `DIMOFS_BUTTON0..` (12+). Each device
  drains only offsets that belong to it; `DIGDD_PEEK` copies without consuming.
- Mouse axes are reported in `GetDeviceState` as raw deltas and reset, as
  upstream; 3DS circle/touch still feed the same `g.dx/g.dy`.
- POV: `win32_input_pov(pad_mask)` returns the DirectInput hat encoding
  (hundredths of a degree, clockwise from up, `0xFFFF` centred), including the
  four diagonals. Keyboard/mouse continue to express the D-pad as
  WASD/arrows; the helper is the joystick-shaped equivalent and is covered by
  `tests/win32_input_smoke.c`.
- No separate virtual joystick object is created: upstream also only exposes
  keyboard/mouse here and lists pads so XInput can find them. XInput
  (`xinput1_3` ordinals) remains our existing implementation.

Preserved: `TESV_INPUT_PROFILE=menu|legacy`, D-pad→WASD/arrows, A→Enter,
B/Start→Escape, Select→Tab, X→Space, Y→E, touch→left mouse, and the existing
`EnumDevices`/callback contract.

## setjmp / longjmp — `source/setjmp.c`

Upstream `setjmp.c` (43 lines) saves the MSVC `jmp_buf` layout
(`ebp, ebx, edi, esi, esp`, `eip`, SEH cookie) and maps the guest buffer to a
host `jmp_buf` slot. Our lifter/IR does not rewrite the caller, so the port
adds an interpreter resume path:

- `_setjmp3`/`_setjmp` write the guest buffer and return 0. The active IR
  function end is recorded in the slot.
- `longjmp` restores the guest GPRs/ESP/EAX and calls
  `n3ds_lift_guest_resume()`, which unwinds to the innermost invoke frame with
  a code-3 longjmp. `invoke_common` then restarts `n3ds_ir_run()` at the saved
  EIP with the saved function range. No host frame that already returned is
  re-entered, and the IR fault/exception path (`rt_unimpl` → invoke frame) is
  unchanged.
- `_setjmp3`/`_setjmp`/`longjmp` are registered on `msvcrt.dll`.

Limit: the resume needs the saved function range, i.e. a real IR invocation;
a longjmp from outside an active guest invocation faults by name.

## msvcrt / heap

Our `source/guest_heap.c` is a host-metadata first-fit heap; upstream
`heap.c` is a guest-memory boundary-tag allocator. Functional semantics were
compared and aligned where the guest can observe them:

- `realloc(p, 0)` now frees `p` and returns NULL (was a 1-byte allocation).
  `realloc(NULL, n)` allocates, failure preserves the old block, `calloc`
  overflow returns NULL, `malloc(0)` still returns a unique block.
- `qsort` added with a guest cdecl comparator (stable insertion sort, same
  algorithm as upstream `msvcrt.c`), using a reentrant private CPU/stack.
- `strtol`/`strtoul`/`atoi`/`atol` added with C base/sign/whitespace rules and
  guest `endptr`.
- `setlocale`/`_setlocale`/`localeconv` added: `"C"`, `""`, `"POSIX"`,
  `"English"`/`"en-US"` return a guest `"C"` string and a valid `lconv`
  (decimal point `.`); other locales are a named refusal returning NULL.
- `_beginthread`/`_beginthreadex` registered on `msvcrt.dll` and implemented on
  the cooperative guest-thread scheduler (`win32_thread_begin`), not as barren
  stubs.
- No change to IR fault handling; `_setjmp3` exceptions are not SEH.

Not ported: upstream's guest-memory heap layout/tuning (`GUEST_HEAP_BASE`
sizing, segregated bins inside guest memory), the full upstream `msvcrt.c`
printf/scanf/math surface (our runtime has its own), and `_initterm`/CRT
startup beyond what this port already uses.

## Tests

Host tests added/changed:

- `xaudio2` — `tests/xaudio2_smoke.c`: COM creation, mastering/source voices,
  format parsing, submit/start/get-state, volume/ratio, flush, the 10 ms mixer
  and the `win32_audio` sink bridge.
- `win32_crt` — `tests/win32_crt_smoke.c`: `realloc(p,0)`, guest `qsort`,
  `strtol/strtoul`, `setlocale/localeconv`, `_setjmp3` `jmp_buf` layout and
  `msvcrt` export resolution.
- `input` — `tests/win32_input_smoke.c` extended with buffered events, peek,
  mouse axes and POV encoding.
- Existing `audio`, `audio_ndsp_host`, `audio_3ds_backend`, `thread`,
  `save_paths`, `com` and `lift` (which includes `tests/win32_smoke.c`) pass
  unchanged.
- ARM compile-only: `tools/build.ps1 -Mode com -Isolated upshims` builds
  `Skyrim_N3DS_graphics_com_upshims.3dsx` with `win32_xaudio2.c` and
  `setjmp.c` linked.
