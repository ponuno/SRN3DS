#pragma once

/* 3DS audio sink for the Win32 waveOut shim.
 *
 * Tries OpenDSP first (an open replacement for the DSP firmware, BSD-2), falls
 * back to libctru's own ndsp (HLE DSP in emulators), and reports which one is
 * actually running. With no backend installed the shim returns
 * MMSYSERR_NODRIVER, which is the honest answer.
 *
 * Returns 1 = OpenDSP, 2 = libctru ndsp, 0 = no backend (logged). Installs the
 * backend into win32_audio with the system tick as its clock, so callers must
 * also call win32_audio_tick() from their main loop. */

int win32_audio_3ds_install(void);
void win32_audio_3ds_shutdown(void);
const char *win32_audio_3ds_name(void);
