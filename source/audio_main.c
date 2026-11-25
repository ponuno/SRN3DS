/* ARM demo: Win32 waveOut through source/win32_audio.c into OpenDSP/ndsp.
 *
 * The guest side is real: a GuestMemory page holds WAVEFORMATEX, two WAVEHDRs
 * and two PCM buffers, and every call below goes through
 * win32_audio_resolve() -> win32_audio_lookup() -> GuestFn, exactly as the
 * lifted TESV code will call it.  A 440 Hz stereo tone is refilled into a
 * completed header, and the on-screen counter is waveOutGetPosition's
 * TIME_SAMPLES value.
 *
 * Build: powershell -File tools/build.ps1 -Mode audio -Isolated audio
 * Exit with START.
 */
#include <3ds.h>
#include <opendsp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "guest.h"
#include "win32_audio.h"

/* Mirror the console text into the standard app log so tools/run_azahar.ps1
 * can find the PASS/FAIL marker it expects. */
static FILE *log_file;
static void report(const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    vprintf(fmt, args);
    if (log_file) { vfprintf(log_file, fmt, copy); fflush(log_file); }
    va_end(copy);
    va_end(args);
}
#define printf report

#define DEMO_RATE 44100u
#define DEMO_FRAMES 8192u /* 186 ms per WAVEHDR */
#define DEMO_HEADERS 2u
#define STAGE_FRAMES 1024u
#define STAGE_COUNT 4u

#define G_STACK 0x00100000u
#define G_PCM 0x00200000u
#define G_FMT 0x00400000u
#define G_HDR 0x00500000u
#define G_STACK_TOP (G_STACK + 0x1000u)
#define PCM_BYTES (DEMO_FRAMES * 4u)
#define DSP_RAM_VADDR 0x1FF40000u

static GuestMemory memory;
static CPU cpu;
static int dsp_up;
static int dsp_kind; /* 0 none, 1 OpenDSP, 2 libctru ndsp */
static float dsp_volume_l = 1.0f, dsp_volume_r = 1.0f;
static int dsp_paused;
static int verdict; /* 0 pending, 1 PASS (counter advanced), -1 FAIL */
static s16 *stage;
static ndspWaveBuf stage_wave[STAGE_COUNT];
static GuestFn fn_open, fn_close, fn_prepare, fn_write, fn_reset;
static GuestFn fn_pause, fn_restart, fn_position, fn_volume;

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    printf("Guest fault at %08lx: %s\n", (unsigned long)addr, what ? what : "unknown");
    /* Never continue with a fabricated address. */
    svcBreak(USERBREAK_PANIC);
    for (;;) svcSleepThread(1000000000LL);
}

static uint64_t clock_us(void) {
    return (uint64_t)svcGetSystemTick() * 1000000ull / SYSCLOCK_ARM11;
}

/* Do not dereference OpenDSP's mailbox when the emulator leaves DSP DRAM
 * unmapped.  The capability bit is only consulted when that memory is usable. */
static int dsp_mailbox_visible(void) {
    MemInfo info;
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info, &page, DSP_RAM_VADDR))) return 0;
    return info.state != MEMSTATE_FREE && (info.perm & MEMPERM_READ) && info.size >= 0x2000u;
}

/* ------------------------------------------------------- OpenDSP backend */

static void dsp_channel_setup(void) {
    ndspChnReset(0);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, (float)WIN32_AUDIO_OUTPUT_HZ);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    {
        float mix[12];
        memset(mix, 0, sizeof mix);
        mix[0] = dsp_volume_l;
        mix[1] = dsp_volume_r;
        ndspChnSetMix(0, mix);
        /* ndspChnReset clears pause and mix; restore the shim's requested state. */
        ndspChnSetPaused(0, dsp_paused != 0);
    }
}

static unsigned dsp_submit(const int16_t *frames, unsigned count) {
    int slot = -1;
    s16 *dst;
    if (!dsp_up) return 0;
    for (unsigned i = 0; i < STAGE_COUNT; ++i)
        if (stage_wave[i].status == NDSP_WBUF_DONE || stage_wave[i].status == NDSP_WBUF_FREE) {
            slot = (int)i;
            break;
        }
    if (slot < 0) return 0; /* DSP busy: the shim ring holds the rest */
    if (count > STAGE_FRAMES) count = STAGE_FRAMES;
    dst = stage + (size_t)slot * STAGE_FRAMES * 2u;
    memcpy(dst, frames, (size_t)count * 4u);
    DSP_FlushDataCache(dst, (count * 4u + 31u) & ~31u);
    memset(&stage_wave[slot], 0, sizeof stage_wave[slot]);
    stage_wave[slot].data_vaddr = dst;
    stage_wave[slot].nsamples = count; /* frames, as libctru/OpenDSP use it */
    stage_wave[slot].looping = false;
    stage_wave[slot].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(0, &stage_wave[slot]);
    return count;
}

static void dsp_set_volume(float left, float right) {
    float mix[12];
    dsp_volume_l = left;
    dsp_volume_r = right;
    if (!dsp_up) return;
    memset(mix, 0, sizeof mix);
    mix[0] = left;
    mix[1] = right;
    ndspChnSetMix(0, mix);
}

static void dsp_set_paused(int paused) {
    dsp_paused = paused != 0;
    if (!dsp_up) return;
    ndspChnSetPaused(0, dsp_paused);
}

static void dsp_flush(void) {
    if (!dsp_up) return;
    ndspChnWaveBufClear(0);
    for (unsigned i = 0; i < STAGE_COUNT; ++i) {
        memset(&stage_wave[i], 0, sizeof stage_wave[i]);
        stage_wave[i].status = NDSP_WBUF_FREE;
    }
    dsp_channel_setup();
}

static Win32AudioBackend dsp_backend = {
    "opendsp", dsp_submit, dsp_set_volume, dsp_set_paused, dsp_flush
};

/* -------------------------------------------------------------- guest side */

static GuestFn audio_fn(const char *name) {
    uint32_t addr = win32_audio_resolve("winmm.dll", name);
    const FnEntry *entry = addr ? win32_audio_lookup(addr) : NULL;
    return entry ? entry->fn : NULL;
}

static uint32_t guest_call(GuestFn fn, const uint32_t *args, unsigned count, uint32_t *pop) {
    uint32_t frame = G_STACK_TOP - 4u - 4u * count;
    cpu.r[4] = frame;
    M32(frame) = 0;
    for (unsigned i = 0; i < count; ++i) M32(frame + 4u + 4u * i) = args[i];
    cpu.r[0] = 0;
    fn(&cpu);
    *pop = cpu.r[4];
    return cpu.r[0];
}

static void fill_tone(uint32_t addr, unsigned frames, double *phase) {
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / (double)DEMO_RATE;
    for (unsigned i = 0; i < frames; ++i) {
        int16_t s;
        *phase += step;
        if (*phase >= 2.0 * 3.14159265358979323846) *phase -= 2.0 * 3.14159265358979323846;
        s = (int16_t)(sin(*phase) * 12000.0);
        M16(addr + i * 4u) = (uint16_t)s;
        M16(addr + i * 4u + 2u) = (uint16_t)s;
    }
}

/* Exercise the duplicated demo sink through the same guest waveOut calls as
 * the real runner.  ndspChnReset() clears both fields, so reading them after
 * waveOutReset proves the sink restored the shim's requested state. */
static int dsp_check_reset_state(uint32_t hwo) {
    uint32_t pop;
    uint32_t args[2];
    float mix[12];
    int failures = 0;

    args[0] = hwo; args[1] = 0u;
    if (guest_call(fn_volume, args, 2, &pop) != 0) return 1;
    if (guest_call(fn_reset, args, 1, &pop) != 0) return 1;
    ndspChnGetMix(0, mix);
    if (mix[0] != 0.0f || mix[1] != 0.0f) ++failures;
    printf("audio demo: mute + reset -> L %.6f R %.6f\n", (double)mix[0], (double)mix[1]);

    args[1] = 0x80004000u; /* left 25 %, right 50 % */
    if (guest_call(fn_volume, args, 2, &pop) != 0) return 1;
    if (guest_call(fn_reset, args, 1, &pop) != 0) return 1;
    ndspChnGetMix(0, mix);
    if (fabsf(mix[0] - 0x4000 / 65535.0f) > 0.000001f ||
        fabsf(mix[1] - 0x8000 / 65535.0f) > 0.000001f) ++failures;
    printf("audio demo: lower volume + reset -> L %.6f R %.6f\n", (double)mix[0], (double)mix[1]);

    if (guest_call(fn_pause, args, 1, &pop) != 0) return 1;
    if (guest_call(fn_reset, args, 1, &pop) != 0) return 1;
    if (!ndspChnIsPaused(0)) ++failures;
    ndspChnGetMix(0, mix);
    if (fabsf(mix[0] - 0x4000 / 65535.0f) > 0.000001f ||
        fabsf(mix[1] - 0x8000 / 65535.0f) > 0.000001f) ++failures;
    printf("audio demo: pause + reset -> paused %d, L %.6f R %.6f\n",
           ndspChnIsPaused(0) ? 1 : 0, (double)mix[0], (double)mix[1]);

    args[1] = 0xffffffffu;
    if (guest_call(fn_volume, args, 2, &pop) != 0) return 1;
    if (guest_call(fn_restart, args, 1, &pop) != 0) return 1;
    if (guest_call(fn_reset, args, 1, &pop) != 0) return 1;
    if (ndspChnIsPaused(0)) ++failures;
    if (failures) printf("audio demo: FAIL reset state (%d failures)\n", failures);
    else printf("audio demo: PASS reset preserves mute, volume and pause\n");
    return failures;
}

int main(void) {
    static double tone_phase;
    uint32_t rc, pop;
    uint32_t hwo = 0;
    uint32_t hdr_addr[DEMO_HEADERS];
    uint32_t pcm_addr[DEMO_HEADERS];
    unsigned frame = 0;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    log_file = fopen("sdmc:/Skyrim_N3DS.log", "w");
    printf("Skyrim N3DS audio demo (win32 waveOut -> OpenDSP/ndsp)\n");
    printf("START: exit\n\n");

    guest_init(&memory, 0x40000);
    if (guest_map(&memory, G_STACK, 0x1000) || guest_map(&memory, G_PCM, 0x10000) ||
        guest_map(&memory, G_FMT, 0x1000) || guest_map(&memory, G_HDR, 0x1000)) {
        printf("FAIL: guest memory mapping failed\n");
        goto idle;
    }
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = G_STACK_TOP;

    {
        /* Primary path: OpenDSP replaces the DSP firmware.  Its capability bit
         * is the only honest proof that the firmware is running. */
        if (dsp_mailbox_visible()) {
            Result r = opendspInit();
            if (R_FAILED(r)) {
                printf("opendspInit failed 0x%08lx\n", (unsigned long)r);
            } else {
                u32 caps = 0;
                Result c = opendspGetCapabilities(&caps);
                if (R_SUCCEEDED(c) && (caps & OPENDSP_CAP_FIRMWARE)) {
                    printf("OpenDSP firmware running, caps 0x%08lx\n", (unsigned long)caps);
                    dsp_kind = 1;
                } else {
                    printf("OpenDSP firmware is not running (caps 0x%08lx)\n", (unsigned long)caps);
                    /* Every later OpenDSP command would block: stop the service. */
                    opendspExit();
                }
            }
        } else {
            printf("DSP mailbox memory is not mapped; skipping OpenDSP\n");
        }
        if (!dsp_kind) {
            /* opendspExit() leaves libctru's pinned component selection intact;
             * a failed opendspInit() can pin it too.  Clear it so ndspInit()
             * loads the installed system component instead of OpenDSP again. */
            ndspUseComponent(NULL, 0, 0, 0);
            /* Emulators that only implement the HLE DSP cannot run an uploaded
             * firmware image.  libctru's own ndspInit is a different, real
             * backend; using it is logged and changes no shim behaviour. */
            Result h = ndspInit();
            if (R_SUCCEEDED(h)) {
                printf("falling back to libctru ndsp (system DSP component), backend \"ndsp\"\n");
                dsp_kind = 2;
            } else {
                printf("ndspInit failed 0x%08lx\n", (unsigned long)h);
                printf("waveOutOpen will return MMSYSERR_NODRIVER\n");
            }
        }
        if (dsp_kind) {
            stage = (s16 *)linearMemAlign(STAGE_COUNT * STAGE_FRAMES * 2u * sizeof(s16), 128);
            if (!stage) {
                printf("linearMemAlign for %u staging frames failed\n",
                       (unsigned)(STAGE_COUNT * STAGE_FRAMES));
                if (dsp_kind == 1) opendspExit(); else ndspExit();
                dsp_kind = 0;
            } else {
                ndspSetOutputMode(NDSP_OUTPUT_STEREO);
                ndspSetClippingMode(NDSP_CLIP_SOFT);
                ndspSetMasterVol(1.0f);
                dsp_backend.name = dsp_kind == 1 ? "opendsp" : "ndsp";
                dsp_up = dsp_kind;
                dsp_channel_setup();
            }
        }
    }

    fn_open = audio_fn("waveOutOpen");
    fn_close = audio_fn("waveOutClose");
    fn_prepare = audio_fn("waveOutPrepareHeader");
    fn_write = audio_fn("waveOutWrite");
    fn_reset = audio_fn("waveOutReset");
    fn_pause = audio_fn("waveOutPause");
    fn_restart = audio_fn("waveOutRestart");
    fn_position = audio_fn("waveOutGetPosition");
    fn_volume = audio_fn("waveOutSetVolume");
    if (!fn_open || !fn_close || !fn_prepare || !fn_write || !fn_reset || !fn_pause ||
        !fn_restart || !fn_position || !fn_volume) {
        printf("win32_audio registry incomplete\n");
        goto idle;
    }

    if (dsp_up && win32_audio_install(&dsp_backend, clock_us) != 0) {
        printf("win32_audio_install failed\n");
        goto idle;
    }

    /* WAVEFORMATEX: PCM, 2 ch, 44100 Hz, 16 bit -> the shim resamples. */
    M16(G_FMT + 0) = 1;
    M16(G_FMT + 2) = 2;
    M32(G_FMT + 4) = DEMO_RATE;
    M32(G_FMT + 8) = DEMO_RATE * 4u;
    M16(G_FMT + 12) = 4;
    M16(G_FMT + 14) = 16;
    M16(G_FMT + 16) = 0;

    for (unsigned i = 0; i < DEMO_HEADERS; ++i) {
        hdr_addr[i] = G_HDR + i * 32u;
        pcm_addr[i] = G_PCM + i * PCM_BYTES;
        memset(n3ds_guest_ptr(hdr_addr[i], 32), 0, 32);
        M32(hdr_addr[i] + 0) = pcm_addr[i];
        M32(hdr_addr[i] + 4) = PCM_BYTES;
        fill_tone(pcm_addr[i], DEMO_FRAMES, &tone_phase);
    }

    {
        uint32_t args[6];
        args[0] = G_HDR + 0x800u; /* separate phwo cell */
        args[1] = 0xffffffffu;    /* WAVE_MAPPER */
        args[2] = G_FMT;
        args[3] = 0;
        args[4] = 0;
        args[5] = 0;
        rc = guest_call(fn_open, args, 6, &pop);
        printf("waveOutOpen -> %lu\n", (unsigned long)rc);
        if (rc != 0) {
            printf("FAIL: waveOutOpen %s\n", rc == 6u ? "returned MMSYSERR_NODRIVER (6): no usable audio backend"
                                                      : "was refused (see the [audio] log line)");
            goto idle;
        }
        hwo = M32(G_HDR + 0x800u);
        printf("handle 0x%03lx\n", (unsigned long)hwo);
        if (dsp_check_reset_state(hwo)) goto idle;
        for (unsigned i = 0; i < DEMO_HEADERS; ++i) {
            uint32_t a[3];
            a[0] = hwo; a[1] = hdr_addr[i]; a[2] = 32;
            if (guest_call(fn_prepare, a, 3, &pop) != 0) { printf("FAIL: prepare %u\n", i); goto idle; }
            if (guest_call(fn_write, a, 3, &pop) != 0) { printf("FAIL: write %u\n", i); goto idle; }
        }
    }

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        win32_audio_tick();
        for (unsigned i = 0; i < DEMO_HEADERS; ++i) {
            if (M32(hdr_addr[i] + 16) & 0x00000001u /* WHDR_DONE */) {
                uint32_t a[3];
                fill_tone(pcm_addr[i], DEMO_FRAMES, &tone_phase);
                a[0] = hwo; a[1] = hdr_addr[i]; a[2] = 32;
                if (guest_call(fn_write, a, 3, &pop) != 0) {
                    printf("\x1b[6;1HwaveOutWrite failed, header not requeued\n");
                }
            }
        }
        if ((frame++ % 30u) == 0u) {
            uint32_t samples = 0;
            M32(G_HDR + 0x840u) = 0x0002u; /* TIME_SAMPLES */
            M32(G_HDR + 0x844u) = 0;
            {
                uint32_t a[3];
                a[0] = hwo; a[1] = G_HDR + 0x840u; a[2] = 12;
                (void)guest_call(fn_position, a, 3, &pop);
                samples = M32(G_HDR + 0x844u);
            }
            printf("\x1b[9;1Hsample counter: %lu\n", (unsigned long)samples);
            printf("\x1b[10;1HwaveOut: %u Hz guest -> %u Hz DSP (linear resample)\n", DEMO_RATE,
                   (unsigned)WIN32_AUDIO_OUTPUT_HZ);
            printf("\x1b[11;1Hseconds played: %lu.%03lu   DSP staging: %u x %u frames\n",
                   (unsigned long)(samples / DEMO_RATE),
                   (unsigned long)((samples % DEMO_RATE) * 1000u / DEMO_RATE),
                   (unsigned)STAGE_COUNT, (unsigned)STAGE_FRAMES);
            /* Self-check against the guest-visible counter, not against a hope. */
            if (!verdict && samples >= DEMO_RATE) {
                printf("audio: PASS waveOut position reached %lu samples (>1 s of audio)\n",
                       (unsigned long)samples);
                verdict = 1;
            } else if (!verdict && frame > 300u) {
                printf("audio: FAIL waveOut position never reached one second (%lu samples)\n",
                       (unsigned long)samples);
                verdict = -1;
            }
        }
        gspWaitForVBlank();
    }

    {
        uint32_t a[1];
        a[0] = hwo;
        (void)guest_call(fn_reset, a, 1, &pop);
        (void)guest_call(fn_close, a, 1, &pop);
    }
    win32_audio_uninstall();
    printf("\x1b[13;1HwaveOut closed, exit\n");

idle:
    win32_audio_uninstall();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    if (dsp_kind == 1) opendspExit(); else if (dsp_kind == 2) ndspExit();
    if (stage) linearFree(stage);
    dsp_volume_l = dsp_volume_r = 1.0f;
    dsp_paused = 0;
    guest_destroy(&memory);
    if (log_file) fclose(log_file);
    gfxExit();
    return 0;
}
