/* 3DS sink for the Win32 waveOut shim. See win32_audio_3ds.h. */
#include "win32_audio_3ds.h"
#include "win32_audio.h"

#ifdef __3DS__
#include <3ds.h>
#include <opendsp.h>
#include <stdio.h>
#include <string.h>

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
    if (dsp_mailbox_visible()) {
        Result r = opendspInit();
        if (R_SUCCEEDED(r)) {
            u32 caps = 0;
            if (R_SUCCEEDED(opendspGetCapabilities(&caps)) && (caps & OPENDSP_CAP_FIRMWARE)) {
                dsp_kind = 1;
                printf("[audio] OpenDSP firmware running (caps 0x%08lx)\n", (unsigned long)caps);
            } else {
                /* Every later OpenDSP command would block: stop the service. */
                printf("[audio] OpenDSP firmware is not running (caps 0x%08lx)\n", (unsigned long)caps);
                opendspExit();
            }
        } else {
            printf("[audio] opendspInit failed 0x%08lx\n", (unsigned long)r);
        }
    } else {
        printf("[audio] DSP mailbox memory is not mapped; skipping OpenDSP\n");
    }
    if (!dsp_kind) {
        /* opendspExit() unloads the DSP but does not clear the image selected
         * with ndspUseComponent(..., componentFree=false).  A failed
         * opendspInit() can leave the same selection behind.  Reset it so
         * ndspInit() discovers the installed system component instead of
         * loading the pinned OpenDSP image again. */
        ndspUseComponent(NULL, 0, 0, 0);
        if (R_SUCCEEDED(ndspInit())) {
            dsp_kind = 2;
            printf("[audio] falling back to libctru ndsp (system DSP component)\n");
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

#else /* host: no device, the shim stays without a backend */

int win32_audio_3ds_install(void) { return 0; }
void win32_audio_3ds_shutdown(void) {}
const char *win32_audio_3ds_name(void) { return "none"; }

#endif
