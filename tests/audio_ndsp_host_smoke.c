/* Host regression for the N3DS_AUDIO=ndsp null sink.
 *
 * Proves that a host build can install the 3DS backend, that waveOutOpen no
 * longer returns MMSYSERR_NODRIVER while it is installed, and that
 * PrepareHeader/Write/tick/Reset/Close keep their host-visible contract.
 * Default (unset) and N3DS_AUDIO=off preserve the old NODRIVER behaviour.
 */
#include "win32_audio.h"
#include "win32_audio_3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define HWO (SCRATCH + 0x100u)
#define WFX (SCRATCH + 0x140u)
#define HDR (SCRATCH + 0x400u)
#define PCM (SCRATCH + 0x500u)

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_NODRIVER 6u
#define WHDR_DONE 0x00000001u

static GuestMemory memory;
static CPU cpu;
static unsigned checks, failures;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; fprintf(stderr, "FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)

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

static void set_audio_env(const char *value) {
#ifdef _WIN32
    _putenv_s("N3DS_AUDIO", value ? value : "");
#else
    if (value) setenv("N3DS_AUDIO", value, 1); else unsetenv("N3DS_AUDIO");
#endif
}

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
    M16(WFX + 0) = 1;   /* WAVE_FORMAT_PCM */
    M16(WFX + 2) = 2;
    M32(WFX + 4) = 44100;
    M32(WFX + 8) = 44100 * 4u;
    M16(WFX + 12) = 4;
    M16(WFX + 14) = 16;
    M16(WFX + 16) = 0;
    M32(HWO) = 0xdeadbeefu;
    args[0] = HWO; args[1] = 0xffffffffu; args[2] = WFX;
    args[3] = 0; args[4] = 0; args[5] = 0;
    return call(win32_audio_lookup(win32_audio_resolve("winmm.dll", "waveOutOpen"))->fn, args, 6);
}

int main(void) {
    GuestFn open = fn_of("waveOutOpen"), close = fn_of("waveOutClose");
    GuestFn prepare = fn_of("waveOutPrepareHeader"), write = fn_of("waveOutWrite");
    GuestFn reset = fn_of("waveOutReset"), pause = fn_of("waveOutPause");
    GuestFn restart = fn_of("waveOutRestart"), volume = fn_of("waveOutSetVolume");
    GuestFn numdevs = fn_of("waveOutGetNumDevs"), devcaps = fn_of("waveOutGetDevCaps");
    Win32Audio3dsHostStats stats;
    uint32_t hwo, args[3];

    guest_init(&memory, 0x40000);
    CHECK_EQ(guest_map(&memory, STACK_BASE, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, SCRATCH, 0x1000), 0);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;

    /* Host tests default to off: no fake device, the old NODRIVER stands. */
    set_audio_env(NULL);
    CHECK_EQ(win32_audio_3ds_install(), 0);
    CHECK_EQ(call(numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);
    set_audio_env("off");
    CHECK_EQ(win32_audio_3ds_install(), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);

    /* Opt in: one device, open no longer fails with NODRIVER. */
    set_audio_env("ndsp");
    CHECK_EQ(win32_audio_3ds_install(), 2);
    CHECK(!strcmp(win32_audio_3ds_name(), "null"));
    CHECK_EQ(call(numdevs, NULL, 0), 1);
    CHECK_EQ(open_dev(), MMSYSERR_NOERROR);
    hwo = M32(HWO);
    CHECK(hwo >= WIN32_AUDIO_HANDLE_BASE);
    {
        uint32_t caps_args[3] = { 0xffffffffu, SCRATCH + 0x600u, 52 };
        CHECK_EQ(call(devcaps, caps_args, 3), MMSYSERR_NOERROR);
        CHECK(!strcmp((const char *)n3ds_guest_ptr(SCRATCH + 0x608u, 32), "Skyrim N3DS null"));
    }

    memset(n3ds_guest_ptr(HDR, 32), 0, 32);
    memset(n3ds_guest_ptr(PCM, 16), 0, 16);
    M32(HDR + 0) = PCM;
    M32(HDR + 4) = 16; /* 4 stereo s16 frames */
    args[0] = hwo; args[1] = HDR; args[2] = 32;
    CHECK_EQ(call(prepare, args, 3), MMSYSERR_NOERROR);
    CHECK_EQ(call(write, args, 3), MMSYSERR_NOERROR);
    win32_audio_3ds_host_stats(&stats);
    CHECK(stats.submits > 0);
    CHECK(stats.frames > 0);
    CHECK(stats.pause_calls == 0);

    CHECK_EQ(call(pause, args, 1), MMSYSERR_NOERROR);
    CHECK_EQ(call(restart, args, 1), MMSYSERR_NOERROR);
    {
        uint32_t v[2] = { hwo, 0x80004000u };
        CHECK_EQ(call(volume, v, 2), MMSYSERR_NOERROR);
    }
    win32_audio_3ds_host_stats(&stats);
    CHECK(stats.pause_calls >= 2);
    CHECK(stats.volume_calls >= 1);

    /* The injected clock advances the shared counter, not a wall-clock sleep. */
    win32_audio_3ds_host_advance_us(500000);
    win32_audio_tick();
    CHECK((M32(HDR + 16) & WHDR_DONE) != 0);

    CHECK_EQ(call(reset, args, 1), MMSYSERR_NOERROR);
    win32_audio_3ds_host_stats(&stats);
    CHECK(stats.flush_calls >= 1);
    CHECK_EQ(call(close, args, 1), MMSYSERR_NOERROR);
    win32_audio_3ds_shutdown();
    CHECK(!strcmp(win32_audio_3ds_name(), "none"));
    CHECK_EQ(call(numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(), MMSYSERR_NODRIVER);

    guest_destroy(&memory);
    if (failures) {
        fprintf(stderr, "FAIL: audio ndsp host (%u checks, %u failures)\n", checks, failures);
        return 1;
    }
    printf("PASS: audio ndsp host (%u checks)\n", checks);
    return 0;
}
