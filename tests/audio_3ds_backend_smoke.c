/* Focused host regression for the real source/win32_audio_3ds.c backend.
 *
 * The fake libctru models the relevant lifetime bug: opendspExit() unloads the
 * DSP but leaves ndspUseComponent()'s pinned image selected.  It also models
 * ndspChnReset() clearing pause and channel mix, so waveOutReset preservation is
 * checked against the actual backend implementation rather than a test sink.
 */
#include "win32_audio.h"
#include "win32_audio_3ds.h"
#include <3ds.h>
#include <opendsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define HWO (SCRATCH + 0x100u)
#define WFX (SCRATCH + 0x140u)
#define CAPS (SCRATCH + 0x300u)
#define HDR (SCRATCH + 0x400u)
#define PCM (SCRATCH + 0x500u)

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_NODRIVER 6u
#define WHDR_DONE 0x00000001u

static void set_audio_env(const char *value) {
#ifdef _WIN32
    _putenv_s("N3DS_AUDIO", value ? value : "");
#else
    if (value) setenv("N3DS_AUDIO", value, 1); else unsetenv("N3DS_AUDIO");
#endif
}

static GuestMemory memory;
static CPU cpu;
static unsigned checks, failures;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; fprintf(stderr, "FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)
#define CHECK_NEAR(got, want, tol) do { ++checks; double g_ = (double)(got), w_ = (double)(want); \
    double d_ = g_ > w_ ? g_ - w_ : w_ - g_; \
    if (!(d_ <= (tol))) { ++failures; fprintf(stderr, "FAIL line %d: %s = %.6f, want %.6f +- %.6f\n", \
        __LINE__, #got, g_, w_, (double)(tol)); } } while (0)

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; (void)addr;
    fprintf(stderr, "FAIL: unexpected guest fault: %s\n", what ? what : "unknown");
    exit(2);
}

/* ---- fake DSP service, deliberately retaining OpenDSP's pinned component ---- */

static const unsigned char fake_image[16] = { 1 };
static const void *selected_component;
static u32 selected_size;
static Result opendsp_result, ndsp_result;
static u32 fake_caps;
static unsigned clear_calls, ndsp_init_calls, ndsp_exit_calls, opendsp_exit_calls, opendsp_init_calls;
static int ndsp_init_saw_pinned;
static int fake_dsp_mapped = 1;
static unsigned exit_device_count;
static uint64_t fake_us;
static float fake_mix[12];
static bool fake_paused;
static unsigned reset_calls, wave_clear_calls, wave_add_calls, wave_add_samples;

uint64_t svcGetSystemTick(void) { return fake_us; }
Result svcQueryMemory(MemInfo *info, PageInfo *page, u32 address) {
    (void)page;
    if (address != DSP_RAM_VADDR) return 1u;
    info->base_addr = DSP_RAM_VADDR;
    info->size = fake_dsp_mapped ? 0x40000u : 0u;
    info->perm = fake_dsp_mapped ? MEMPERM_READ | 2u : 0u;
    info->state = fake_dsp_mapped ? 6u : MEMSTATE_FREE;
    return 0;
}
void DSP_FlushDataCache(const void *address, u32 size) { (void)address; (void)size; }
void *linearMemAlign(size_t size, size_t alignment) { (void)alignment; return malloc(size); }
void linearFree(void *pointer) { free(pointer); }

void ndspUseComponent(const void *binary, u32 size, u16 progMask, u16 dataMask) {
    (void)progMask; (void)dataMask;
    selected_component = binary;
    selected_size = size;
    if (!binary) ++clear_calls;
}
Result ndspInit(void) {
    ++ndsp_init_calls;
    ndsp_init_saw_pinned = selected_component != NULL;
    fake_paused = false;
    for (unsigned i = 0; i < 12; ++i) fake_mix[i] = i < 2 ? 1.0f : 0.0f;
    return ndsp_result;
}
void ndspExit(void) {
    ++ndsp_exit_calls;
    exit_device_count = win32_audio_device_count();
    /* Real libctru only frees componentBin when it owns it.  OpenDSP's pinned
     * image is not owned, so the selection intentionally survives this call. */
}
void ndspSetOutputMode(int mode) { (void)mode; }
void ndspSetClippingMode(int mode) { (void)mode; }
void ndspSetMasterVol(float volume) { (void)volume; }
void ndspChnReset(int channel) {
    (void)channel;
    ++reset_calls;
    fake_paused = false;
    for (unsigned i = 0; i < 12; ++i) fake_mix[i] = i < 2 ? 1.0f : 0.0f;
}
void ndspChnSetFormat(int channel, u16 format) { (void)channel; (void)format; }
void ndspChnSetRate(int channel, float rate) { (void)channel; (void)rate; }
void ndspChnSetInterp(int channel, int interpolation) { (void)channel; (void)interpolation; }
void ndspChnSetMix(int channel, float mix[12]) {
    (void)channel;
    memcpy(fake_mix, mix, sizeof fake_mix);
}
void ndspChnGetMix(int channel, float out_mix[12]) {
    (void)channel;
    memcpy(out_mix, fake_mix, sizeof fake_mix);
}
void ndspChnSetPaused(int channel, bool paused) { (void)channel; fake_paused = paused; }
bool ndspChnIsPaused(int channel) { (void)channel; return fake_paused; }
void ndspChnWaveBufClear(int channel) { (void)channel; ++wave_clear_calls; }
void ndspChnWaveBufAdd(int channel, ndspWaveBuf *buffer) {
    (void)channel;
    ++wave_add_calls;
    wave_add_samples += buffer->nsamples;
    buffer->status = NDSP_WBUF_QUEUED;
}

Result opendspInit(void) {
    ++opendsp_init_calls;
    /* Model both a clean init and a partial failure after pinning the image. */
    ndspUseComponent(fake_image, sizeof fake_image, 0xff, 0xff);
    return opendsp_result;
}
void opendspExit(void) {
    ++opendsp_exit_calls;
    ndspExit();
    /* Intentionally does not clear selected_component, matching installed
     * OpenDSP 1.3.1 and libctru's ndspExit(). */
}
Result opendspGetCapabilities(u32 *caps) { *caps = fake_caps; return 0; }

/* ---- guest shim helpers ---- */

static GuestFn g_open, g_close, g_reset, g_pause, g_restart, g_volume;
static GuestFn g_numdevs, g_devcaps, g_prepare, g_write;

static GuestFn fn_of(const char *name) {
    uint32_t addr = win32_audio_resolve("winmm.dll", name);
    const FnEntry *entry = addr ? win32_audio_lookup(addr) : NULL;
    if (!entry || !entry->fn) {
        fprintf(stderr, "FAIL: no thunk for %s\n", name);
        exit(2);
    }
    return entry->fn;
}
static uint32_t call(GuestFn fn, const uint32_t *args, unsigned count) {
    uint32_t frame = STACK_TOP - 4u - 4u * count;
    cpu.r[4] = frame;
    M32(frame) = 0;
    for (unsigned i = 0; i < count; ++i) M32(frame + 4u + 4u * i) = args[i];
    cpu.r[0] = 0;
    fn(&cpu);
    CHECK_EQ(cpu.r[4], frame + 4u + 4u * count);
    return cpu.r[0];
}
static uint32_t open_dev(void) {
    uint32_t args[6];
    M16(WFX + 0) = 1;
    M16(WFX + 2) = 2;
    M32(WFX + 4) = 44100;
    M32(WFX + 8) = 44100 * 4u;
    M16(WFX + 12) = 4;
    M16(WFX + 14) = 16;
    M16(WFX + 16) = 0;
    M32(HWO) = 0xdeadbeefu;
    args[0] = HWO; args[1] = 0xffffffffu; args[2] = WFX;
    args[3] = 0; args[4] = 0; args[5] = 0;
    return call(g_open, args, 6);
}
static uint32_t device_call(GuestFn fn) {
    uint32_t arg = M32(HWO);
    return call(fn, &arg, 1);
}
static void set_volume(uint32_t volume) {
    uint32_t args[2] = { M32(HWO), volume };
    CHECK_EQ(call(g_volume, args, 2), MMSYSERR_NOERROR);
}
static void check_caps_name(const char *want) {
    uint32_t args[3] = { 0xffffffffu, CAPS, 52 };
    CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_NOERROR);
    CHECK(!strcmp((const char *)n3ds_guest_ptr(CAPS + 8, 32), want));
}
static void check_reset_preserves_volume_and_pause(void) {
    float mix[12];

    set_volume(0u);
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    ndspChnGetMix(0, mix);
    CHECK_NEAR(mix[0], 0.0, 0.000001);
    CHECK_NEAR(mix[1], 0.0, 0.000001);

    set_volume(0x80004000u); /* left 25 %, right 50 % */
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    ndspChnGetMix(0, mix);
    CHECK_NEAR(mix[0], 0x4000 / 65535.0, 0.000001);
    CHECK_NEAR(mix[1], 0x8000 / 65535.0, 0.000001);

    CHECK_EQ(device_call(g_pause), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    CHECK(ndspChnIsPaused(0));
    ndspChnGetMix(0, mix);
    CHECK_NEAR(mix[0], 0x4000 / 65535.0, 0.000001);
    CHECK_NEAR(mix[1], 0x8000 / 65535.0, 0.000001);

    CHECK_EQ(device_call(g_restart), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    CHECK(!ndspChnIsPaused(0));
    CHECK(wave_clear_calls >= 4);
}

int main(void) {
    guest_init(&memory, 0x40000);
    CHECK_EQ(guest_map(&memory, STACK_BASE, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, SCRATCH, 0x1000), 0);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;

    g_open = fn_of("waveOutOpen");
    g_close = fn_of("waveOutClose");
    g_reset = fn_of("waveOutReset");
    g_pause = fn_of("waveOutPause");
    g_restart = fn_of("waveOutRestart");
    g_volume = fn_of("waveOutSetVolume");
    g_numdevs = fn_of("waveOutGetNumDevs");
    g_devcaps = fn_of("waveOutGetDevCaps");
    g_prepare = fn_of("waveOutPrepareHeader");
    g_write = fn_of("waveOutWrite");

    /* Explicit off keeps the old NODRIVER result, for bisecting a regression. */
    set_audio_env("off");
    CHECK_EQ(win32_audio_3ds_install(), 0);
    CHECK(!strcmp(win32_audio_3ds_name(), "none"));
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);
    win32_audio_3ds_shutdown();

    /* OpenDSP available: no fallback and an honest device name. */
    set_audio_env("opendsp");
    opendsp_result = 0;
    ndsp_result = 0;
    fake_caps = OPENDSP_CAP_FIRMWARE | OPENDSP_CAP_AAC_ARM11;
    CHECK_EQ(win32_audio_3ds_install(), 1);
    CHECK(!strcmp(win32_audio_3ds_name(), "opendsp"));
    CHECK_EQ(clear_calls, 0);
    CHECK_EQ(call(g_numdevs, NULL, 0), 1);
    check_caps_name("Skyrim N3DS opendsp");
    CHECK_EQ(open_dev(), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_close), MMSYSERR_NOERROR);
    win32_audio_3ds_shutdown();
    CHECK(!strcmp(win32_audio_3ds_name(), "none"));
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(exit_device_count, 0);

    /* Default device mode (N3DS_AUDIO unset): ndsp directly, no OpenDSP probe. */
    set_audio_env(NULL);
    clear_calls = 0;
    ndsp_init_calls = 0;
    opendsp_init_calls = 0;
    ndsp_init_saw_pinned = -1;
    fake_caps = OPENDSP_CAP_AAC_ARM11 | OPENDSP_CAP_MP3_ARM11;
    CHECK_EQ(win32_audio_3ds_install(), 2);
    CHECK(!strcmp(win32_audio_3ds_name(), "ndsp"));
    CHECK_EQ(opendsp_init_calls, 0);
    CHECK_EQ(clear_calls, 1);
    CHECK_EQ(ndsp_init_calls, 1);
    CHECK_EQ(ndsp_init_saw_pinned, 0);
    CHECK_EQ(call(g_numdevs, NULL, 0), 1);
    check_caps_name("Skyrim N3DS ndsp");
    CHECK_EQ(open_dev(), MMSYSERR_NOERROR);

    /* waveOutPrepareHeader + waveOutWrite reach the DSP ring as real frames. */
    memset(n3ds_guest_ptr(HDR, 32), 0, 32);
    memset(n3ds_guest_ptr(PCM, 16), 0, 16);
    M32(HDR + 0) = PCM;
    M32(HDR + 4) = 16; /* 4 stereo s16 frames at 44.1 kHz */
    {
        uint32_t args[3] = { M32(HWO), HDR, 32 };
        CHECK_EQ(call(g_prepare, args, 3), MMSYSERR_NOERROR);
        CHECK_EQ(call(g_write, args, 3), MMSYSERR_NOERROR);
    }
    CHECK(wave_add_calls > 0);
    CHECK(wave_add_samples > 0);
    fake_us += 500000;
    win32_audio_tick();
    CHECK((M32(HDR + 16) & WHDR_DONE) != 0);

    check_reset_preserves_volume_and_pause();
    CHECK_EQ(device_call(g_close), MMSYSERR_NOERROR);
    win32_audio_3ds_shutdown();
    CHECK_EQ(exit_device_count, 0);

    /* If libctru cannot find a component either, preserve the explicit
     * no-driver result instead of pretending a device exists. */
    set_audio_env("ndsp");
    clear_calls = 0;
    ndsp_init_calls = 0;
    ndsp_init_saw_pinned = -1;
    opendsp_result = OPENDSP_ERR_NO_IMAGE;
    ndsp_result = 1u;
    CHECK_EQ(win32_audio_3ds_install(), 0);
    CHECK(!strcmp(win32_audio_3ds_name(), "none"));
    CHECK_EQ(clear_calls, 1);
    CHECK_EQ(ndsp_init_saw_pinned, 0);
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);
    {
        uint32_t args[3] = { 0xffffffffu, CAPS, 52 };
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_NODRIVER);
    }
    win32_audio_3ds_shutdown();

    /* An unmapped mailbox (Azahar LLE) must skip the OpenDSP probe entirely and
     * still select libctru's installed component without a pinned image. */
    clear_calls = 0;
    ndsp_init_calls = 0;
    opendsp_init_calls = 0;
    ndsp_init_saw_pinned = -1;
    fake_dsp_mapped = 0;
    opendsp_result = 0;
    ndsp_result = 0;
    CHECK_EQ(win32_audio_3ds_install(), 2);
    CHECK_EQ(opendsp_init_calls, 0);
    CHECK_EQ(clear_calls, 1);
    CHECK_EQ(ndsp_init_saw_pinned, 0);
    CHECK(!strcmp(win32_audio_3ds_name(), "ndsp"));
    win32_audio_3ds_shutdown();

    guest_destroy(&memory);
    if (failures) {
        fprintf(stderr, "FAIL: win32_audio_3ds backend (%u checks, %u failures)\n", checks, failures);
        return 1;
    }
    printf("PASS: win32_audio_3ds backend (%u checks)\n", checks);
    return 0;
}
