/* Isolated ARM/Azahar regression for the production 3DS waveOut backend.
 *
 * Build without changing the Makefile:
 *   make AUDIO=1 TARGET=audio_3ds_arm BUILD=build_audio_arm \
 *     OFILES="audio_3ds_arm_smoke.o win32_audio_3ds.o win32_audio.o guest.o" \
 *     VPATH="source tests"
 */
#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "win32_audio.h"
#include "win32_audio_3ds.h"

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define HWO (SCRATCH + 0x100u)
#define WFX (SCRATCH + 0x140u)
#define CAPS (SCRATCH + 0x300u)

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_NODRIVER 6u

static GuestMemory memory;
static CPU cpu;
static FILE *log_file;
static unsigned checks, failures;

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

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; printf("FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)
#define CHECK_NEAR(got, want, tol) do { ++checks; double g_ = (double)(got), w_ = (double)(want); \
    double d_ = g_ > w_ ? g_ - w_ : w_ - g_; \
    if (!(d_ <= (tol))) { ++failures; printf("FAIL line %d: %s = %.6f, want %.6f +- %.6f\n", \
        __LINE__, #got, g_, w_, (double)(tol)); } } while (0)

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    printf("Guest fault at %08lx: %s\n", (unsigned long)addr, what ? what : "unknown");
    svcBreak(USERBREAK_PANIC);
    for (;;) svcSleepThread(1000000000LL);
}

static GuestFn g_open, g_close, g_reset, g_pause, g_restart, g_volume;
static GuestFn g_numdevs, g_devcaps;

static GuestFn fn_of(const char *name) {
    uint32_t addr = win32_audio_resolve("winmm.dll", name);
    const FnEntry *entry = addr ? win32_audio_lookup(addr) : NULL;
    if (!entry || !entry->fn) {
        printf("FAIL: no thunk for %s\n", name);
        return NULL;
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
    printf("audio_3ds: mute + reset -> L %.6f R %.6f\n", (double)mix[0], (double)mix[1]);

    set_volume(0x80004000u); /* left 25 %, right 50 % */
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    ndspChnGetMix(0, mix);
    CHECK_NEAR(mix[0], 0x4000 / 65535.0, 0.000001);
    CHECK_NEAR(mix[1], 0x8000 / 65535.0, 0.000001);
    printf("audio_3ds: lower volume + reset -> L %.6f R %.6f\n", (double)mix[0], (double)mix[1]);

    CHECK_EQ(device_call(g_pause), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    CHECK(ndspChnIsPaused(0));
    ndspChnGetMix(0, mix);
    CHECK_NEAR(mix[0], 0x4000 / 65535.0, 0.000001);
    CHECK_NEAR(mix[1], 0x8000 / 65535.0, 0.000001);
    printf("audio_3ds: pause + reset -> paused %d, L %.6f R %.6f\n",
           ndspChnIsPaused(0) ? 1 : 0, (double)mix[0], (double)mix[1]);

    CHECK_EQ(device_call(g_restart), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_reset), MMSYSERR_NOERROR);
    CHECK(!ndspChnIsPaused(0));
}

int main(void) {
    int kind;
    char want_name[32];

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    log_file = fopen("sdmc:/Skyrim_N3DS.log", "w");
    printf("Skyrim N3DS isolated audio_3ds ARM test\n");

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
    if (!g_open || !g_close || !g_reset || !g_pause || !g_restart || !g_volume ||
        !g_numdevs || !g_devcaps) goto idle;

    kind = win32_audio_3ds_install();
    printf("audio_3ds: installed kind %d name \"%s\"\n", kind, win32_audio_3ds_name());
    if (!kind) {
        ++failures;
        printf("FAIL: no usable audio backend (waveOutOpen must stay MMSYSERR_NODRIVER)\n");
        goto idle;
    }
    snprintf(want_name, sizeof want_name, "Skyrim N3DS %s", win32_audio_3ds_name());
    CHECK_EQ(call(g_numdevs, NULL, 0), 1);
    check_caps_name(want_name);
    CHECK_EQ(open_dev(), MMSYSERR_NOERROR);
    check_reset_preserves_volume_and_pause();
    CHECK_EQ(device_call(g_close), MMSYSERR_NOERROR);
    win32_audio_3ds_shutdown();
    CHECK(!strcmp(win32_audio_3ds_name(), "none"));
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);
    {
        uint32_t args[3] = { 0xffffffffu, CAPS, 52 };
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_NODRIVER);
    }

idle:
    if (failures) printf("audio_3ds: FAIL (%u checks, %u failures)\n", checks, failures);
    else printf("audio_3ds: PASS (%u checks)\n", checks);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    win32_audio_3ds_shutdown();
    guest_destroy(&memory);
    if (log_file) fclose(log_file);
    gfxExit();
    return failures ? 1 : 0;
}
