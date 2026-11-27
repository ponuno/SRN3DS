/* 3DS sink for the Win32 waveOut shim. See win32_audio_3ds.h. */
#include "win32_audio_3ds.h"
#include "win32_audio.h"

#ifdef __3DS__
#include <3ds.h>
#include <opendsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* N3DS_AUDIO modes.  ndsp is the default on the device: it needs no uploaded
 * firmware and cannot leave libctru's component selection pinned the way a
 * failed OpenDSP probe can. */
enum { AUDIO_MODE_DEFAULT, AUDIO_MODE_NDSP, AUDIO_MODE_OPENDSB, AUDIO_MODE_OFF };

static int audio_mode(void) {
    const char *v = getenv("N3DS_AUDIO");
    if (!v || !*v) return AUDIO_MODE_DEFAULT;
    if (!strcmp(v, "ndsp")) return AUDIO_MODE_NDSP;
    if (!strcmp(v, "opendsp") || !strcmp(v, "auto")) return AUDIO_MODE_OPENDSB;
    if (!strcmp(v, "off")) return AUDIO_MODE_OFF;
    printf("[audio] unknown N3DS_AUDIO=\"%s\"; using the default\n", v);
    return AUDIO_MODE_DEFAULT;
}

#define STAGE_FRAMES 1024u
#define STAGE_COUNT 4u
#define DSP_RAM_VADDR 0x1FF40000u

static int dsp_up;
static int dsp_kind; /* 0 none, 1 OpenDSP, 2 libctru ndsp */
static float channel_volume_l = 1.0f, channel_volume_r = 1.0f;
static int channel_paused;
static s16 *stage;
static ndspWaveBuf stage_wave[STAGE_COUNT];

static uint64_t clock_us(void) {
    return (uint64_t)svcGetSystemTick() * 1000000ull / SYSCLOCK_ARM11;
}

/* OpenDSP's capability probe dereferences its DSP DRAM mailbox.  Some emulator
 * configurations report a converted address but leave that page unmapped;
 * querying first avoids an unmapped read and a false firmware claim. */
static int dsp_mailbox_visible(void) {
    MemInfo info;
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info, &page, DSP_RAM_VADDR))) return 0;
    return info.state != MEMSTATE_FREE && (info.perm & MEMPERM_READ) && info.size >= 0x2000u;
}

static void channel_setup(void) {
    ndspChnReset(0);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, (float)WIN32_AUDIO_OUTPUT_HZ);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    float mix[12];
    memset(mix, 0, sizeof mix);
    mix[0] = channel_volume_l;
    mix[1] = channel_volume_r;
    ndspChnSetMix(0, mix);
    /* ndspChnReset clears pause and mix; restore the shim's requested state. */
    ndspChnSetPaused(0, channel_paused != 0);
}

static unsigned submit(const int16_t *frames, unsigned count) {
    if (!dsp_up) return 0;
    int slot = -1;
    for (unsigned i = 0; i < STAGE_COUNT; ++i)
        if (stage_wave[i].status == NDSP_WBUF_DONE || stage_wave[i].status == NDSP_WBUF_FREE) {
            slot = (int)i;
            break;
        }
    if (slot < 0) return 0; /* device busy: the shim ring holds the rest */
    if (count > STAGE_FRAMES) count = STAGE_FRAMES;
    s16 *dst = stage + (size_t)slot * STAGE_FRAMES * 2u;
    memcpy(dst, frames, (size_t)count * 4u);
    DSP_FlushDataCache(dst, (count * 4u + 31u) & ~31u);
    memset(&stage_wave[slot], 0, sizeof stage_wave[slot]);
    stage_wave[slot].data_vaddr = dst;
    stage_wave[slot].nsamples = count;
    stage_wave[slot].looping = false;
    stage_wave[slot].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(0, &stage_wave[slot]);
    return count;
}

static void set_volume(float left, float right) {
    channel_volume_l = left;
    channel_volume_r = right;
    if (!dsp_up) return;
    float mix[12];
    memset(mix, 0, sizeof mix);
    mix[0] = left;
    mix[1] = right;
    ndspChnSetMix(0, mix);
}

static void set_paused(int paused) {
    channel_paused = paused != 0;
    if (!dsp_up) return;
    ndspChnSetPaused(0, channel_paused);
}

static void flush(void) {
    if (!dsp_up) return;
    ndspChnWaveBufClear(0);
    for (unsigned i = 0; i < STAGE_COUNT; ++i) {
        memset(&stage_wave[i], 0, sizeof stage_wave[i]);
        stage_wave[i].status = NDSP_WBUF_FREE;
    }
    channel_setup();
}

static Win32AudioBackend backend = {"ndsp", submit, set_volume, set_paused, flush};

int win32_audio_3ds_install(void) {
    if (dsp_up) return dsp_kind;
    int mode = audio_mode();
    if (mode == AUDIO_MODE_OFF) {
        printf("[audio] disabled by N3DS_AUDIO=off; waveOutOpen returns MMSYSERR_NODRIVER\n");
        return 0;
    }
    if (mode == AUDIO_MODE_OPENDSB) {
        if (dsp_mailbox_visible()) {
            Result r = opendspInit();
            if (R_SUCCEEDED(r)) {
                u32 caps = 0;
                if (R_SUCCEEDED(opendspGetCapabilities(&caps)) && (caps & OPENDSP_CAP_FIRMWARE)) {
                    dsp_kind = 1;
                    printf("[audio] OpenDSP firmware running (caps 0x%08lx)\n", (unsigned long)caps);
                } else {
                    /* Every later OpenDSP command would block: stop the service. */
                    printf("[audio] OpenDSP firmware is not running (caps 0x%08lx)\n",
                           (unsigned long)caps);
                    opendspExit();
                }
            } else {
                printf("[audio] opendspInit failed 0x%08lx\n", (unsigned long)r);
            }
        } else {
            printf("[audio] DSP mailbox memory is not mapped; skipping OpenDSP\n");
        }
    }
    if (!dsp_kind) {
        /* A previous OpenDSP attempt (or a stale selection) can pin libctru's
         * component.  Clear it so ndspInit() discovers the installed system
         * component.  This is a no-op when nothing is pinned. */
        ndspUseComponent(NULL, 0, 0, 0);
        if (R_SUCCEEDED(ndspInit())) {
            dsp_kind = 2;
            printf("[audio] libctru ndsp ready (system DSP component)\n");
        } else {
            printf("[audio] ndspInit failed; waveOutOpen returns MMSYSERR_NODRIVER\n");
        }
    }
    if (dsp_kind) {
        stage = (s16 *)linearMemAlign(STAGE_COUNT * STAGE_FRAMES * 2u * sizeof(s16), 128);
        if (!stage) {
            printf("[audio] staging allocation failed\n");
            if (dsp_kind == 1) opendspExit(); else ndspExit();
            dsp_kind = 0;
        } else {
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspSetClippingMode(NDSP_CLIP_SOFT);
            ndspSetMasterVol(1.0f);
            backend.name = dsp_kind == 1 ? "opendsp" : "ndsp";
            dsp_up = dsp_kind;
            channel_setup();
        }
    }
    if (dsp_kind) {
        if (win32_audio_install(&backend, clock_us) != 0) {
            printf("[audio] win32_audio_install failed\n");
            win32_audio_3ds_shutdown();
            return 0;
        }
        printf("[audio] backend \"%s\" installed, output %u Hz\n", backend.name,
               (unsigned)WIN32_AUDIO_OUTPUT_HZ);
    }
    return dsp_kind;
}

void win32_audio_3ds_shutdown(void) {
    if (dsp_up) {
        win32_audio_uninstall();
        if (dsp_kind == 1) opendspExit(); else if (dsp_kind == 2) ndspExit();
        dsp_up = 0;
        dsp_kind = 0;
    }
    if (stage) {
        linearFree(stage);
        stage = NULL;
    }
    channel_volume_l = channel_volume_r = 1.0f;
    channel_paused = 0;
}

const char *win32_audio_3ds_name(void) {
    return dsp_kind == 1 ? "opendsp" : dsp_kind == 2 ? "ndsp" : "none";
}

void win32_audio_3ds_host_stats(Win32Audio3dsHostStats *out) {
    if (out) memset(out, 0, sizeof *out);
}
uint64_t win32_audio_3ds_host_advance_us(uint64_t delta) { (void)delta; return 0; }

#else /* host: counting null sink, no real device */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned host_submits, host_frames, host_volume_calls, host_pause_calls, host_flush_calls;
static uint64_t host_us;
static int host_up;

static uint64_t host_clock(void) { return host_us; }

/* Accept every frame and keep silence: the shim already converts and resamples,
 * so this sink only proves the open/write/close contract on a host. */
static unsigned host_submit(const int16_t *frames, unsigned count) {
    (void)frames;
    ++host_submits;
    host_frames += count;
    return count;
}
static void host_set_volume(float left, float right) {
    (void)left; (void)right;
    ++host_volume_calls;
}
static void host_set_paused(int paused) { (void)paused; ++host_pause_calls; }
static void host_flush(void) { ++host_flush_calls; }

static Win32AudioBackend host_backend = {
    "null", host_submit, host_set_volume, host_set_paused, host_flush
};

int win32_audio_3ds_install(void) {
    const char *v;
    if (host_up) return 2;
    v = getenv("N3DS_AUDIO");
    /* Off by default on the host so ordinary unit tests see no device. */
    if (!v || !*v || !strcmp(v, "off")) return 0;
    if (strcmp(v, "ndsp")) {
        if (strcmp(v, "opendsp") && strcmp(v, "auto"))
            printf("[audio] unknown N3DS_AUDIO=\"%s\"; host backend stays off\n", v);
        else
            printf("[audio] N3DS_AUDIO=%s needs a 3DS; host backend stays off\n", v);
        return 0;
    }
    host_submits = host_frames = host_volume_calls = host_pause_calls = host_flush_calls = 0;
    host_us = 0;
    if (win32_audio_install(&host_backend, host_clock) != 0) {
        printf("[audio] host win32_audio_install failed\n");
        return 0;
    }
    host_up = 1;
    printf("[audio] host null sink installed (N3DS_AUDIO=ndsp); no sound\n");
    return 2;
}

void win32_audio_3ds_shutdown(void) {
    if (!host_up) return;
    win32_audio_uninstall();
    host_up = 0;
}

const char *win32_audio_3ds_name(void) { return host_up ? "null" : "none"; }

void win32_audio_3ds_host_stats(Win32Audio3dsHostStats *out) {
    if (!out) return;
    out->submits = host_submits;
    out->frames = host_frames;
    out->volume_calls = host_volume_calls;
    out->pause_calls = host_pause_calls;
    out->flush_calls = host_flush_calls;
}

uint64_t win32_audio_3ds_host_advance_us(uint64_t delta) {
    host_us += delta;
    return host_us;
}

#endif
