#pragma once
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE */

/* Win32 waveOut + X3DAudio subset, backed by an external audio backend.
 *
 * The module is split exactly like win32_sync: a private thunk window, a
 * definition table and a resolve/lookup pair.  A dispatcher that walks guest
 * addresses must chain win32_audio_lookup() after the other module lookups;
 * win32_resolve already chains win32_audio_resolve() for imports.
 *
 * The backend is not part of this module.  source/audio_main.c installs an
 * OpenDSP/ndsp sink; tests install a null sink.  With no backend installed
 * waveOutOpen() returns MMSYSERR_NODRIVER and waveOutGetNumDevs() returns 0 -
 * there is no silent success and no fake device.
 */

#define WIN32_AUDIO_BASE (WIN32_THUNK_BASE + 0x00050000u) /* 0xff050000 */
#define WIN32_AUDIO_HANDLE_BASE 0x700u
#define WIN32_AUDIO_HANDLE_COUNT 8u

/* Output rate fed to the backend.  libctru's NDSP_SAMPLE_RATE is
 * SYSCLOCK_SOC/512 = 16756991/512 = 32728.498 Hz.  The shim uses the
 * conventional rounded figure 32730 Hz for the resampler step and in logs, so
 * its pitch error against the real DAC clock is +0.005 % (1.5 Hz) - measured
 * and accepted, not hidden.  Every samplerate the guest asks for other than
 * this one is resampled linearly in this module. */
#define WIN32_AUDIO_OUTPUT_HZ 32730u

typedef struct Win32AudioBackend {
    /* Short name used in log lines and waveOutGetDevCaps ("opendsp", "ndsp",
     * "null"), never NULL. */
    const char *name;
    /* Copy up to `count` interleaved stereo s16 frames, already at
     * WIN32_AUDIO_OUTPUT_HZ, into the device.  Return how many frames were
     * accepted (0 = device full, backpressure).  Must not block or allocate. */
    unsigned (*submit)(const int16_t *frames, unsigned count);
    /* Per-channel volume, linear 0..1.  Applied by the backend exactly once;
     * the shim does not pre-scale samples, so guest data reaches submit()
     * unchanged. */
    void (*set_volume)(float left, float right);
    /* Freeze/resume consumption.  Frames already submitted stay queued. */
    void (*set_paused)(int paused);
    /* Discard everything the backend still holds (waveOutReset/close). */
    void (*flush)(void);
} Win32AudioBackend;

typedef struct { const char *dll, *name; GuestFn fn; } Win32AudioDef;

/* Thunks are WIN32_AUDIO_BASE + 4*i. */
const Win32AudioDef *win32_audio_defs(unsigned *count);
uint32_t win32_audio_resolve(const char *dll, const char *name);
const FnEntry *win32_audio_lookup(uint32_t addr);

/* Install/remove the sink.  `now_us` is the monotonic clock used to advance
 * playback; it must never go backwards and is converted to frames internally.
 * Both arguments are required.  Returns 0 on success, -1 if devices are still
 * open or an argument is NULL.  Installing replaces a previous backend only
 * when no device is open. */
int win32_audio_install(const Win32AudioBackend *backend, uint64_t (*now_us)(void));
void win32_audio_uninstall(void);

/* Advance playback (clock -> sample counter -> WAVEHDR completion).  Call it
 * from the frame loop; every shim entry also advances first, so a guest that
 * only polls waveOutGetPosition() still observes progress.  Guests must not
 * see a difference between "the demo ticks every frame" and "somebody ticks
 * before each call": completion is a pure function of the clock. */
void win32_audio_tick(void);

/* Number of open devices and the guest-frame position of one device; -1 for an
 * unknown index.  Diagnostics only (the demo prints the counter). */
unsigned win32_audio_device_count(void);
int64_t win32_audio_position_frames(unsigned index);

/* Semantics implemented (single guest thread, no audio thread):
 * - Handles are 0x700..0x707.  waveOutOpen accepts WAVE_MAPPER (0xffffffff)
 *   and device id 0 while a backend is installed; otherwise NODRIVER, for
 *   WAVE_FORMAT_QUERY too.  One device may be open at a time (one sink, no
 *   mixer): a second open returns MMSYSERR_ALLOCATED (4).
 * - WAVEFORMATEX is validated before the backend is consulted, so an invalid
 *   format fails identically with and without hardware:
 *     malformed PCM (block align / average bytes mismatch, cbSize != 18,
 *     zero rate/channels/bits)            -> WAVERR_BADFORMAT (32)
 *     unsupported tag/channels/bits/rate  -> MMSYSERR_NOTSUPPORTED (8) + log
 *   Supported: WAVE_FORMAT_PCM, 1-2 channels, 8 or 16 bits, 4000..192000 Hz.
 * - prepare/write/unprepare enforce WHDR_PREPARED / WHDR_INQUEUE and return
 *   MMSYSERR_INVALPARAM, WAVERR_UNPREPARED or WAVERR_STILLPLAYING as Windows
 *   does.  Completion (WHDR_DONE) is driven by the sample counter, never by
 *   the call that queued the header.
 * - dwBytesRecorded is left untouched: it is an input-device field.
 * - waveOutGetPosition reports TIME_SAMPLES/TIME_BYTES/TIME_MS/TIME_TICKS from
 *   the same counter; other time types return MMSYSERR_NOTSUPPORTED and the
 *   structure is rewritten as TIME_BYTES.
 * - Volume is 16-bit per channel and reaches the backend through
 *   set_volume(); samples are not rescaled here.
 * - waveOutPause stops the counter and the backend; waveOutRestart resumes;
 *   waveOutReset completes every queued header and rewinds the counter to 0.
 * - Callbacks (fdwOpen CALLBACK_*) and dwLoops > 1 are refused (NOTSUPPORTED,
 *   logged) instead of being silently dropped; WAVE_FORMAT_QUERY is honoured.
 * - Resampling is a linear interpolation (no anti-alias filter, so material
 *   above ~16.3 kHz aliases) over one stream position, so the phase continues
 *   across buffers.  At a buffer boundary the right-hand neighbour is the last
 *   sample of the buffer (one-sample hold).  The sample counter is driven by
 *   the injected clock, not by the output-frame count, so it stays wall-clock
 *   accurate; if a backend accepts nothing for a full second the counter stops
 *   (and logs "playback stalled") instead of running minutes ahead.
 *
 * X3DAudio subset (X3DAudio1_7.dll, the exports TESV imports):
 * - X3DAudioInitialize accepts SPEAKER_STEREO only and writes a 20-byte handle.
 *   Any other mask returns E_NOTIMPL and logs "SpeakerChannelMask".
 * - X3DAudioCalculate computes, for 1- or 2-channel emitters and a stereo
 *   destination: distance attenuation (pVolumeCurve or the documented
 *   inverse-law default), LPF direct path (pLPFDirectCurve or default),
 *   cone volume/LPF, Doppler factor, emitter-to-listener angle/distance and the
 *   [dst][src] pan matrix.  Elevation does not move the pan (documented).
 * - Refused with E_NOTIMPL and a log line naming the field: speaker masks other
 *   than stereo, InnerRadius/InnerRadiusAngle > 0, pChannelAzimuths with more
 *   than one channel, X3DAUDIO_CALCULATE_DELAY (pDelayTimes), LPF_REVERB,
 *   REVERB, ZEROCENTER, REDIRECT_TO_LFE, unknown flag bits, dwLoops > 1.
 *   E_INVALIDARG is returned for NULL pointers, a bad handle, ChannelCount
 *   disagreeing with SrcChannelCount, or DstChannelCount != 2.
 *   X3DAudioCalculate is declared void in the real DLL; the shim still leaves
 *   the code in EAX so tests and logs can see it.  A guest cannot.
 * - No 5.1, no reverb, no delay, no HRTF: this is a stereo subset, not a
 *   reimplementation of the proprietary matrixing.
 */
