#pragma once

#include <stdint.h>

/* 3DS audio sink for the Win32 waveOut shim.
 *
 * N3DS_AUDIO selects the backend:
 *   unset    ndsp on ARM, off on the host build (tests see no device)
 *   ndsp     libctru ndsp only: one stereo PCM16 channel, 4-stage ring, silence
 *            while the guest has nothing queued
 *   opendsp  OpenDSP firmware first, ndsp fallback (legacy behaviour)
 *   off      install nothing; the shim answers MMSYSERR_NODRIVER honestly
 * An unknown value is logged and treated as the default.
 *
 * The host build installs a counting null sink when N3DS_AUDIO=ndsp, so the
 * open/write/close contract can be tested without a DSP. With the default
 * (off) it returns 0 and the shim keeps returning MMSYSERR_NODRIVER.
 *
 * Returns 1 = OpenDSP, 2 = libctru ndsp (or the host null sink), 0 = no
 * backend (logged). Installs the backend into win32_audio with the system tick
 * as its clock, so callers must also call win32_audio_tick() from their main
 * loop. */

int win32_audio_3ds_install(void);
void win32_audio_3ds_shutdown(void);
const char *win32_audio_3ds_name(void);

/* Host-only test surface: counters behind the null sink and a fake clock so a
 * test can drive header completion without sleeping. ARM builds report zeros. */
typedef struct {
    unsigned submits;      /* submit() calls */
    unsigned frames;       /* interleaved stereo frames accepted */
    unsigned volume_calls;
    unsigned pause_calls;
    unsigned flush_calls;
} Win32Audio3dsHostStats;

void win32_audio_3ds_host_stats(Win32Audio3dsHostStats *out);
uint64_t win32_audio_3ds_host_advance_us(uint64_t delta);
