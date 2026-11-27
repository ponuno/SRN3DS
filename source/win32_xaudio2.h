#pragma once
#include <stdint.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE only */

/* XAudio2 2.6/2.7 subset, ported from Skyrim_Android's runtime/xaudio2.c.
 *
 * Objects are guest-memory COM cells whose vtables point into this module's
 * thunk window; the host state (voices, queues, mixer accumulators) lives in
 * host memory.  There is no engine thread on the 3DS: the mixer is driven by
 * win32_xaudio2_tick(), and every shim entry advances it first, following the
 * same "completion is a pure function of the clock" rule as win32_audio.
 *
 * Output goes through win32_audio_submit_stereo() to whatever backend
 * N3DS_AUDIO installed (opendsp/ndsp on hardware, counting null sink on the
 * host, none for N3DS_AUDIO=off).  Mixing continues with no backend so
 * OnBufferEnd/GetState keep advancing; the only difference is that the sink
 * accepts nothing, which is reported by host stats instead of hidden.
 *
 * Implemented: IXAudio2, IXAudio2MasteringVoice, IXAudio2SubmixVoice,
 * IXAudio2SourceVoice (PCM8/16/24/32, F32, MS ADPCM; play region/loop count;
 * volume/channel volumes/output matrix; frequency ratio; OnBufferStart/
 * OnBufferEnd/OnLoopEnd/OnStreamEnd callbacks), XAPOFX1_5!CreateFX stubs.
 * Effects and filters are accepted and ignored, as upstream.
 */
#define WIN32_XAUDIO2_BASE (WIN32_THUNK_BASE + 0x000A0000u) /* 0xff0a0000 */

typedef struct { const char *dll, *name; GuestFn fn; } Win32XAudio2Def;

int win32_xaudio2_init(GuestMemory *m);
void win32_xaudio2_shutdown(void);
const Win32XAudio2Def *win32_xaudio2_defs(unsigned *count);
uint32_t win32_xaudio2_resolve(const char *dll, const char *name);
const FnEntry *win32_xaudio2_lookup(uint32_t addr);

/* Advance the software mixer to `now_us` (absolute monotonic microseconds; 0
 * means "read the host clock").  Mixes in 10 ms passes and submits stereo
 * PCM16 at WIN32_AUDIO_OUTPUT_HZ.  Safe to call from every frame. */
void win32_xaudio2_tick(uint64_t now_us);

/* Host regression surface: counters and a deterministic clock. */
typedef struct {
    unsigned voices;          /* live guest objects (source/submix/master) */
    unsigned source_voices;
    unsigned buffers_submitted;
    unsigned buffers_completed;
    unsigned frames_mixed;    /* output frames produced */
    unsigned frames_submitted;/* frames accepted by the sink */
    unsigned passes;
    unsigned callbacks;       /* guest callbacks invoked */
    unsigned last_error;      /* last XAUDIO2_E_* returned, 0 = none */
    unsigned underrun_frames; /* source frames requested with an empty queue */
    unsigned buffers_flushed; /* buffers removed by FlushSourceBuffers */
} Win32XAudio2Stats;

void win32_xaudio2_host_stats(Win32XAudio2Stats *out);
/* Host clock used by the shims; returns the current fake/host time in us.
 * win32_xaudio2_tick() with an explicit value moves it forward. */
uint64_t win32_xaudio2_host_advance_us(uint64_t delta);
void win32_xaudio2_host_reset(void);
