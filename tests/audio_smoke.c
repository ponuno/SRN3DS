/* Host smoke test for source/win32_audio.c.
 *
 * Links guest.c + win32_audio.c only.  The two runtime hooks below stand in for
 * n3ds_lift_runtime.c so M32()/rt_unimpl() work without win32_shims.c.  The
 * audio device is the null sink defined here and time comes from a synthetic
 * clock, so every completion assertion is deterministic and needs no hardware.
 * The same assertions are run once with no backend installed to prove that
 * format validation does not depend on the sink.
 */
#include "win32_audio.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define TEB 0x00300000u
#define PCM 0x00400000u
#define X3D 0x00500000u

#define HWO (SCRATCH + 0x100u)
#define WFX (SCRATCH + 0x140u)
#define HDR0 (SCRATCH + 0x200u)
#define HDR1 (SCRATCH + 0x260u)
#define CAPS (SCRATCH + 0x300u)
#define MMT (SCRATCH + 0x400u)

#define LISTENER (X3D + 0x000u)
#define EMITTER (X3D + 0x100u)
#define SETTINGS (X3D + 0x200u)
#define MATRIX (X3D + 0x280u)
#define X3DHANDLE (X3D + 0x300u)
#define CONE (X3D + 0x340u)

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_ALLOCATED 4u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define MMSYSERR_NOTSUPPORTED 8u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u

#define WHDR_DONE 0x00000001u
#define WHDR_PREPARED 0x00000002u
#define WHDR_INQUEUE 0x00000010u

#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u

static GuestMemory memory;
static CPU cpu;
static unsigned checks, failures;
static jmp_buf trap;
static int trap_armed;
static char trap_text[160];

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    (void)addr;
    snprintf(trap_text, sizeof trap_text, "%s", what ? what : "unknown");
    if (!trap_armed) {
        fprintf(stderr, "FAIL: unexpected guest fault: %s\n", trap_text);
        exit(2);
    }
    longjmp(trap, 1);
}

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; fprintf(stderr, "FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)
#define CHECK_NEAR(got, want, tol) do { ++checks; double g_ = (double)(got), w_ = (double)(want); \
    double d_ = g_ > w_ ? g_ - w_ : w_ - g_; \
    if (!(d_ <= (tol))) { ++failures; fprintf(stderr, "FAIL line %d: %s = %.6f, want %.6f +- %.6f\n", \
        __LINE__, #got, g_, w_, (double)(tol)); } } while (0)

/* ---- synthetic clock and null sink ---- */

static uint64_t fake_us;
static unsigned sink_frames, sink_calls, sink_flushes;
static float sink_vol_l = 1.0f, sink_vol_r = 1.0f;
static int sink_paused;
static int16_t sink_l, sink_r;
static int sink_captured;

static uint64_t clock_now(void) { return fake_us; }
static unsigned null_submit(const int16_t *frames, unsigned count) {
    if (!sink_captured && count && frames) {
        sink_l = frames[0];
        sink_r = frames[1];
        sink_captured = 1;
    }
    sink_frames += count;
    ++sink_calls;
    return count;
}
static void null_volume(float l, float r) { sink_vol_l = l; sink_vol_r = r; }
static void null_paused(int paused) { sink_paused = paused; }
static void null_flush(void) { ++sink_flushes; }

static GuestFn g_open, g_close, g_prepare, g_unprepare, g_write, g_reset;
static GuestFn g_pause, g_restart, g_volume, g_position, g_numdevs, g_devcaps;
static GuestFn g_x3d_init, g_x3d_calc;
static uint32_t call(GuestFn fn, const uint32_t *args, unsigned n);

static const Win32AudioBackend null_sink = {
    "null", null_submit, null_volume, null_paused, null_flush
};
static const Win32AudioBackend opendsp_sink = {
    "opendsp", null_submit, null_volume, null_paused, null_flush
};
static const Win32AudioBackend ndsp_sink = {
    "ndsp", null_submit, null_volume, null_paused, null_flush
};

static uint32_t device_caps_name(const char *want) {
    uint32_t args[3];
    memset(n3ds_guest_ptr(CAPS, 64), 0xcc, 64);
    args[0] = 0xffffffffu; args[1] = CAPS; args[2] = 52;
    uint32_t rc = call(g_devcaps, args, 3);
    CHECK_EQ(rc, MMSYSERR_NOERROR);
    CHECK(!strcmp((const char *)n3ds_guest_ptr(CAPS + 8, 32), want));
    return rc;
}

static void advance_ms(unsigned ms) {
    fake_us += (uint64_t)ms * 1000u;
    win32_audio_tick();
}

/* ---- guest helpers ---- */

static GuestFn fn_of(const char *dll, const char *name) {
    uint32_t a = win32_audio_resolve(dll, name);
    const FnEntry *e = a ? win32_audio_lookup(a) : NULL;
    if (!e || !e->fn) {
        fprintf(stderr, "FAIL: no thunk for %s!%s\n", dll, name);
        exit(2);
    }
    return e->fn;
}
static uint32_t call(GuestFn fn, const uint32_t *args, unsigned n) {
    uint32_t frame = STACK_TOP - 4 - 4 * n;
    cpu.r[4] = frame;
    M32(frame) = 0;
    for (unsigned i = 0; i < n; ++i) M32(frame + 4 + 4 * i) = args[i];
    cpu.r[0] = 0;
    fn(&cpu);
    if (cpu.r[4] != frame + 4 + 4 * n) {
        fprintf(stderr, "FAIL: shim did not pop %u args (esp 0x%08lx, want 0x%08lx)\n",
            n, (unsigned long)cpu.r[4], (unsigned long)(frame + 4 + 4 * n));
        exit(2);
    }
    return cpu.r[0];
}

static uint32_t put_wfx(uint32_t at, uint16_t tag, uint16_t channels, uint32_t rate,
                        uint16_t bits, uint16_t cb, uint32_t avg_override,
                        uint16_t align_override) {
    uint16_t align = (uint16_t)(channels * (bits / 8u));
    uint32_t avg = rate * (uint32_t)align;
    memset(n3ds_guest_ptr(at, 18), 0, 18);
    M16(at + 0) = tag;
    M16(at + 2) = channels;
    M32(at + 4) = rate;
    M32(at + 8) = avg_override ? avg_override : avg;
    M16(at + 12) = align_override ? align_override : align;
    M16(at + 14) = bits;
    M16(at + 16) = cb;
    return at;
}
static uint32_t good_wfx(uint32_t at, uint32_t rate) {
    return put_wfx(at, 1, 2, rate, 16, 0, 0, 0);
}
static uint32_t put_hdr(uint32_t at, uint32_t data, uint32_t length) {
    memset(n3ds_guest_ptr(at, 32), 0, 32);
    M32(at + 0) = data;   /* lpData */
    M32(at + 4) = length; /* dwBufferLength */
    M32(at + 8) = 0;      /* dwBytesRecorded */
    M32(at + 12) = 0x1111;/* dwUser */
    M32(at + 16) = 0;     /* dwFlags */
    M32(at + 20) = 0;     /* dwLoops */
    return at;
}
static uint32_t hdr_flags(uint32_t at) { return M32(at + 16); }

static uint32_t open_dev(uint32_t wfx, uint32_t flags) {
    uint32_t args[6];
    M32(HWO) = 0xdeadbeefu;
    args[0] = HWO; args[1] = 0xffffffffu; args[2] = wfx;
    args[3] = 0; args[4] = 0; args[5] = flags;
    return call(g_open, args, 6);
}
static uint32_t prepare(uint32_t hdr, uint32_t cb) {
    uint32_t args[3];
    args[0] = M32(HWO); args[1] = hdr; args[2] = cb;
    return call(g_prepare, args, 3);
}
static uint32_t unprepare(uint32_t hdr, uint32_t cb) {
    uint32_t args[3];
    args[0] = M32(HWO); args[1] = hdr; args[2] = cb;
    return call(g_unprepare, args, 3);
}
static uint32_t write_hdr(uint32_t hdr, uint32_t cb) {
    uint32_t args[3];
    args[0] = M32(HWO); args[1] = hdr; args[2] = cb;
    return call(g_write, args, 3);
}
static uint32_t device_call(GuestFn fn, unsigned args_n) {
    uint32_t args[3];
    args[0] = M32(HWO); args[1] = 0; args[2] = 0;
    return call(fn, args, args_n);
}
static uint32_t position(uint32_t type, uint32_t *rc) {
    uint32_t args[3];
    M32(MMT + 0) = type;
    M32(MMT + 4) = 0xaaaaaaaau;
    args[0] = M32(HWO); args[1] = MMT; args[2] = 12;
    *rc = call(g_position, args, 3);
    return M32(MMT + 4);
}
static void fill_pcm16(uint32_t at, uint32_t frames, int16_t value) {
    for (uint32_t i = 0; i < frames * 2u; ++i) M16(at + 2u * i) = (uint16_t)value;
}
static int all_same_codes(unsigned count, const uint32_t *a, const uint32_t *b) {
    for (unsigned i = 0; i < count; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

/* X3DAudio structures */
static void x3d_listener(float px, float py, float pz) {
    memset(n3ds_guest_ptr(LISTENER, 52), 0, 52);
    MF32(LISTENER + 0) = 0.0f; MF32(LISTENER + 4) = 0.0f; MF32(LISTENER + 8) = 1.0f;
    MF32(LISTENER + 12) = 0.0f; MF32(LISTENER + 16) = 1.0f; MF32(LISTENER + 20) = 0.0f;
    MF32(LISTENER + 24) = px; MF32(LISTENER + 28) = py; MF32(LISTENER + 32) = pz;
}
static void x3d_emitter(float px, float py, float pz, float vx, float vy, float vz) {
    memset(n3ds_guest_ptr(EMITTER, 100), 0, 100);
    MF32(EMITTER + 4) = 0.0f; MF32(EMITTER + 8) = 0.0f; MF32(EMITTER + 12) = 1.0f;
    MF32(EMITTER + 16) = 0.0f; MF32(EMITTER + 20) = 1.0f; MF32(EMITTER + 24) = 0.0f;
    MF32(EMITTER + 28) = px; MF32(EMITTER + 32) = py; MF32(EMITTER + 36) = pz;
    MF32(EMITTER + 40) = vx; MF32(EMITTER + 44) = vy; MF32(EMITTER + 48) = vz;
    M32(EMITTER + 60) = 1;  /* ChannelCount */
    MF32(EMITTER + 92) = 1.0f; /* CurveDistanceScaler */
    MF32(EMITTER + 96) = 1.0f; /* DopplerScaler */
}
static void x3d_settings(uint32_t src, uint32_t dst, uint32_t matrix) {
    memset(n3ds_guest_ptr(SETTINGS, 48), 0, 48);
    M32(SETTINGS + 0) = matrix;
    M32(SETTINGS + 4) = 0;
    M32(SETTINGS + 8) = src;
    M32(SETTINGS + 12) = dst;
}
static uint32_t x3d_calculate(uint32_t flags) {
    uint32_t args[5];
    args[0] = X3DHANDLE; args[1] = LISTENER; args[2] = EMITTER;
    args[3] = flags; args[4] = SETTINGS;
    return call(g_x3d_calc, args, 5);
}
static uint32_t x3d_init(uint32_t mask, float speed) {
    uint32_t args[3];
    memset(n3ds_guest_ptr(X3DHANDLE, 20), 0, 20);
    MF32(SCRATCH + 0x500u) = speed;
    args[0] = mask; args[1] = SCRATCH + 0x500u; args[2] = X3DHANDLE;
    return call(g_x3d_init, args, 3);
}

int main(void) {
    uint32_t wfx, hdr, rc, pos;

    /* --- registry: defs, resolve, lookup, disjointness --- */
    {
        unsigned count = 0;
        const Win32AudioDef *defs = win32_audio_defs(&count);
        CHECK_EQ(count, 14);
        CHECK(defs != NULL);
        for (unsigned i = 0; i < count; ++i) {
            uint32_t want = WIN32_AUDIO_BASE + 4 * i;
            CHECK_EQ(win32_audio_resolve(defs[i].dll, defs[i].name), want);
            {
                const FnEntry *e = win32_audio_lookup(want);
                CHECK(e != NULL && e->addr == want && e->fn == defs[i].fn);
            }
        }
        CHECK_EQ(win32_audio_resolve("winmm", "waveOutOpen"), WIN32_AUDIO_BASE);
        CHECK_EQ(win32_audio_resolve("WINMM.DLL", "waveOutWrite"), win32_audio_resolve("winmm.dll", "waveOutWrite"));
        CHECK_EQ(win32_audio_resolve("winmm.dll", "waveoutopen"), 0);
        CHECK_EQ(win32_audio_resolve("kernel32.dll", "waveOutOpen"), 0);
        CHECK_EQ(win32_audio_resolve("X3DAudio1_7.dll", "X3DAudioCalculate"),
                 win32_audio_resolve("x3daudio1_7", "X3DAudioCalculate"));
        CHECK_EQ(win32_audio_resolve("winmm.dll", "timeGetTime"), 0); /* owned by win32_shims.c */
        CHECK(win32_audio_lookup(WIN32_AUDIO_BASE - 4) == NULL);
        CHECK(win32_audio_lookup(WIN32_AUDIO_BASE + 4 * count) == NULL);
        CHECK(win32_audio_lookup(WIN32_AUDIO_BASE + 2) == NULL);
        CHECK_EQ(WIN32_AUDIO_BASE, 0xff050000u);
        /* Disjoint from every other module window the dispatcher consults. */
        CHECK(WIN32_AUDIO_BASE != 0xff000000u /* win32_shims */);
        CHECK(WIN32_AUDIO_BASE != 0xff010000u /* win32_sync */);
        CHECK(WIN32_AUDIO_BASE != 0xff020000u /* win32_user */);
        CHECK(WIN32_AUDIO_BASE != 0xff030000u /* win32_locale/shell */);
    }

    guest_init(&memory, 0x40000);
    CHECK_EQ(guest_map(&memory, STACK_BASE, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, SCRATCH, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, TEB, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, PCM, 0x10000), 0);
    CHECK_EQ(guest_map(&memory, X3D, 0x1000), 0);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;
    cpu.fs_base = TEB;

    g_open = fn_of("winmm.dll", "waveOutOpen");
    g_close = fn_of("winmm.dll", "waveOutClose");
    g_prepare = fn_of("winmm.dll", "waveOutPrepareHeader");
    g_unprepare = fn_of("winmm.dll", "waveOutUnprepareHeader");
    g_write = fn_of("winmm.dll", "waveOutWrite");
    g_reset = fn_of("winmm.dll", "waveOutReset");
    g_pause = fn_of("winmm.dll", "waveOutPause");
    g_restart = fn_of("winmm.dll", "waveOutRestart");
    g_volume = fn_of("winmm.dll", "waveOutSetVolume");
    g_position = fn_of("winmm.dll", "waveOutGetPosition");
    g_numdevs = fn_of("winmm.dll", "waveOutGetNumDevs");
    g_devcaps = fn_of("winmm.dll", "waveOutGetDevCaps");
    g_x3d_init = fn_of("X3DAudio1_7.dll", "X3DAudioInitialize");
    g_x3d_calc = fn_of("X3DAudio1_7.dll", "X3DAudioCalculate");

    wfx = good_wfx(WFX, 44100);

    /* --- no backend: a real, visible failure --- */
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(wfx, 0), MMSYSERR_NODRIVER);
    CHECK_EQ(open_dev(wfx, 0x00000001u /* WAVE_FORMAT_QUERY */), MMSYSERR_NODRIVER);
    {
        uint32_t args[3];
        args[0] = 0xffffffffu; args[1] = CAPS; args[2] = 52;
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_NODRIVER);
    }
    {
        uint32_t codes_nobackend[7];
        codes_nobackend[0] = open_dev(put_wfx(WFX, 3, 2, 44100, 16, 0, 0, 0), 0); /* float */
        codes_nobackend[1] = open_dev(put_wfx(WFX, 1, 6, 44100, 16, 0, 0, 0), 0); /* 5.1 */
        codes_nobackend[2] = open_dev(put_wfx(WFX, 1, 2, 44100, 24, 0, 0, 0), 0); /* 24 bit */
        codes_nobackend[3] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 0, 0, 3), 0); /* align */
        codes_nobackend[4] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 0, 12345, 0), 0); /* avg */
        codes_nobackend[5] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 22, 0, 0), 0); /* cbSize */
        codes_nobackend[6] = open_dev(0, 0); /* NULL format */
        CHECK_EQ(codes_nobackend[0], MMSYSERR_NOTSUPPORTED);
        CHECK_EQ(codes_nobackend[1], MMSYSERR_NOTSUPPORTED);
        CHECK_EQ(codes_nobackend[2], MMSYSERR_NOTSUPPORTED);
        CHECK_EQ(codes_nobackend[3], WAVERR_BADFORMAT);
        CHECK_EQ(codes_nobackend[4], WAVERR_BADFORMAT);
        CHECK_EQ(codes_nobackend[5], WAVERR_BADFORMAT);
        CHECK_EQ(codes_nobackend[6], MMSYSERR_INVALPARAM);

        /* --- install the null sink --- */
        CHECK_EQ(win32_audio_install(NULL, clock_now), -1);
        CHECK_EQ(win32_audio_install(&null_sink, NULL), -1);
        CHECK_EQ(win32_audio_install(&null_sink, clock_now), 0);
        CHECK_EQ(win32_audio_install(&null_sink, clock_now), 0);
        CHECK_EQ(call(g_numdevs, NULL, 0), 1);

        /* identical failure codes with hardware present */
        {
            uint32_t codes_backend[7];
            codes_backend[0] = open_dev(put_wfx(WFX, 3, 2, 44100, 16, 0, 0, 0), 0);
            codes_backend[1] = open_dev(put_wfx(WFX, 1, 6, 44100, 16, 0, 0, 0), 0);
            codes_backend[2] = open_dev(put_wfx(WFX, 1, 2, 44100, 24, 0, 0, 0), 0);
            codes_backend[3] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 0, 0, 3), 0);
            codes_backend[4] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 0, 12345, 0), 0);
            codes_backend[5] = open_dev(put_wfx(WFX, 1, 2, 44100, 16, 22, 0, 0), 0);
            codes_backend[6] = open_dev(0, 0);
            CHECK(all_same_codes(7, codes_nobackend, codes_backend));
        }
    }

    /* --- device caps --- */
    {
        uint32_t args[3];
        memset(n3ds_guest_ptr(CAPS, 64), 0xcc, 64);
        args[0] = 0xffffffffu; args[1] = CAPS; args[2] = 51;
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_INVALPARAM);
        args[2] = 52;
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_NOERROR);
        CHECK_EQ(M16(CAPS + 0), 0);
        CHECK_EQ(M16(CAPS + 2), 0);
        CHECK_EQ(M32(CAPS + 4), 0x0100u);
        CHECK(!strcmp((const char *)n3ds_guest_ptr(CAPS + 8, 32), "Skyrim N3DS null"));
        CHECK_EQ(M32(CAPS + 40) & 0x00008888u, 0x00008888u); /* 2S16/4S16/48S16/1S16 */
        CHECK_EQ(M16(CAPS + 44), 2);
        CHECK_EQ(M32(CAPS + 48), 0x000cu); /* VOLUME | LRVOLUME */
        args[0] = 5;
        CHECK_EQ(call(g_devcaps, args, 3), MMSYSERR_BADDEVICEID);

        /* szPname identifies the sink that is actually installed. */
        CHECK_EQ(win32_audio_install(&opendsp_sink, clock_now), 0);
        (void)device_caps_name("Skyrim N3DS opendsp");
        CHECK_EQ(win32_audio_install(&ndsp_sink, clock_now), 0);
        (void)device_caps_name("Skyrim N3DS ndsp");
        CHECK_EQ(win32_audio_install(&null_sink, clock_now), 0);
        (void)device_caps_name("Skyrim N3DS null");
    }

    /* --- open: flags, id, query, one device --- */
    wfx = good_wfx(WFX, 44100);
    CHECK_EQ(open_dev(wfx, 0x00000001u /* WAVE_FORMAT_QUERY */), MMSYSERR_NOERROR);
    CHECK_EQ(M32(HWO), 0xdeadbeefu);
    CHECK_EQ(open_dev(wfx, 0x00000004u /* WAVE_MAPPED */), MMSYSERR_NOTSUPPORTED);
    CHECK_EQ(open_dev(wfx, 0x00004000u), MMSYSERR_INVALFLAG);
    CHECK_EQ(open_dev(wfx, 0x00000000u), MMSYSERR_NOERROR);
    {
        uint32_t handle = M32(HWO);
        CHECK(handle >= WIN32_AUDIO_HANDLE_BASE && handle < WIN32_AUDIO_HANDLE_BASE + WIN32_AUDIO_HANDLE_COUNT);
        CHECK_EQ(win32_audio_device_count(), 1);
        CHECK_EQ(win32_audio_install(&null_sink, clock_now), -1); /* busy */
        CHECK_EQ(open_dev(wfx, 0), MMSYSERR_ALLOCATED);
        {
            uint32_t args[6];
            M32(HWO) = 0xdeadbeefu;
            args[0] = HWO; args[1] = 3; args[2] = wfx; args[3] = 0; args[4] = 0; args[5] = 0;
            CHECK_EQ(call(g_open, args, 6), MMSYSERR_BADDEVICEID);
            args[1] = 0; args[0] = 0;
            CHECK_EQ(call(g_open, args, 6), MMSYSERR_INVALPARAM);
            args[0] = HWO;
            CHECK_EQ(call(g_open, args, 6), MMSYSERR_ALLOCATED);
        }
        M32(HWO) = handle;
    }

    /* --- header state machine --- */
    hdr = put_hdr(HDR0, PCM, 4410u * 4u);
    CHECK_EQ(prepare(hdr, 31), MMSYSERR_INVALPARAM);
    CHECK_EQ(hdr_flags(hdr), 0);
    CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(hdr_flags(hdr), WHDR_PREPARED);
    CHECK_EQ(prepare(hdr, 32), MMSYSERR_INVALPARAM);
    CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(hdr_flags(hdr) & WHDR_INQUEUE, WHDR_INQUEUE);
    CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_INVALPARAM); /* already queued */
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, 0);
    CHECK_EQ(device_call(g_reset, 1), MMSYSERR_NOERROR); /* finish it */
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, WHDR_DONE);
    CHECK_EQ(hdr_flags(hdr) & WHDR_INQUEUE, 0);
    CHECK_EQ(unprepare(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(hdr_flags(hdr), 0);
    CHECK_EQ(unprepare(hdr, 32), WAVERR_UNPREPARED);
    CHECK_EQ(write_hdr(hdr, 32), WAVERR_UNPREPARED);
    {
        /* NULL lpData and zero length are parameter errors */
        uint32_t h2 = put_hdr(HDR1, 0, 16);
        CHECK_EQ(prepare(h2, 32), MMSYSERR_INVALPARAM);
        CHECK_EQ(prepare(h2, 33), MMSYSERR_INVALPARAM);
        h2 = put_hdr(HDR1, PCM, 0);
        CHECK_EQ(prepare(h2, 32), MMSYSERR_NOERROR);
        CHECK_EQ(write_hdr(h2, 32), MMSYSERR_INVALPARAM);
        h2 = put_hdr(HDR1, PCM, 4410u * 4u);
        M32(HDR1 + 20) = 3; /* dwLoops */
        CHECK_EQ(prepare(h2, 32), MMSYSERR_NOERROR);
        CHECK_EQ(write_hdr(h2, 32), MMSYSERR_NOTSUPPORTED);
        CHECK_EQ(unprepare(h2, 32), MMSYSERR_NOERROR);
    }

    /* --- sample counter and deferred completion --- */
    fill_pcm16(PCM, 4410u, 0x1234);
    sink_captured = 0;
    sink_frames = 0;
    hdr = put_hdr(HDR0, PCM, 4410u * 4u);
    CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(position(0x0002u /* TIME_SAMPLES */, &rc), 0);
    CHECK_EQ(rc, MMSYSERR_NOERROR);
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, 0);
    /* waveOutWrite converts and submits immediately; the clock only decides
     * when the header is done.  4410 guest frames -> ~3273 output frames. */
    CHECK(sink_frames > 3265u && sink_frames < 3282u);
    advance_ms(20); /* 20 ms = 654 output frames = 881 guest frames at 44100 */
    pos = position(0x0002u /* TIME_SAMPLES */, &rc);
    CHECK(pos > 850u && pos < 910u);
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, 0);
    CHECK(sink_frames > 3265u && sink_frames < 3282u);
    CHECK_EQ(sink_l, 0x1234);
    CHECK_EQ(sink_r, 0x1234);
    advance_ms(200);
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, WHDR_DONE);
    CHECK_EQ(hdr_flags(hdr) & WHDR_INQUEUE, 0);
    CHECK_EQ(position(0x0002u, &rc), 4410u);
    CHECK_EQ(position(0x0004u /* TIME_BYTES */, &rc), 4410u * 4u);
    CHECK_EQ(position(0x0001u /* TIME_MS */, &rc), 100u);
    CHECK_EQ(position(0x0020u /* TIME_TICKS */, &rc), 100u);
    {
        uint32_t got = position(0x0008u /* TIME_SMPTE */, &rc);
        CHECK_EQ(rc, MMSYSERR_NOTSUPPORTED);
        CHECK_EQ(M32(MMT), 0x0004u);
        CHECK_EQ(got, 4410u * 4u);
    }
    CHECK_EQ(unprepare(hdr, 32), MMSYSERR_NOERROR);

    /* --- one second at 44.1 kHz yields exactly 32730 output frames --- */
    {
        unsigned done = 0;
        uint32_t hdrs = SCRATCH + 0x600u;
        CHECK_EQ(device_call(g_reset, 1), MMSYSERR_NOERROR); /* counter back to 0 */
        sink_frames = 0;
        sink_captured = 0;
        for (unsigned i = 0; i < 10u; ++i) {
            hdr = put_hdr(hdrs + i * 32u, PCM, 4410u * 4u); /* 100 ms each */
            CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
            CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
        }
        for (unsigned i = 0; i < 100u; ++i) advance_ms(10);
        for (unsigned i = 0; i < 10u; ++i)
            if (M32(hdrs + i * 32u + 16) & WHDR_DONE) ++done;
        CHECK_EQ(done, 10u);
        CHECK(sink_frames >= 32720u && sink_frames <= 32740u);
        CHECK_NEAR(sink_frames, 32730.0, 8.0);
        CHECK_NEAR(position(0x0002u, &rc), 44100.0, 40.0);
        for (unsigned i = 0; i < 10u; ++i)
            CHECK_EQ(unprepare(hdrs + i * 32u, 32), MMSYSERR_NOERROR);
    }

    /* --- pause / restart / reset --- */
    hdr = put_hdr(HDR0, PCM, 4410u * 4u);
    CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
    advance_ms(20);
    CHECK_EQ(device_call(g_pause, 1), MMSYSERR_NOERROR);
    CHECK_EQ(sink_paused, 1);
    pos = position(0x0002u, &rc);
    advance_ms(500);
    CHECK_EQ(position(0x0002u, &rc), pos);
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, 0);
    CHECK_EQ(device_call(g_restart, 1), MMSYSERR_NOERROR);
    CHECK_EQ(sink_paused, 0);
    advance_ms(20);
    CHECK(position(0x0002u, &rc) > pos);
    CHECK_EQ(device_call(g_reset, 1), MMSYSERR_NOERROR);
    CHECK_EQ(hdr_flags(hdr) & WHDR_DONE, WHDR_DONE);
    CHECK_EQ(position(0x0002u, &rc), 0);
    CHECK(sink_flushes >= 1);
    CHECK_EQ(unprepare(hdr, 32), MMSYSERR_NOERROR);

    /* --- volume --- */
    {
        uint32_t args[2];
        args[0] = M32(HWO); args[1] = 0xffff8000u;
        CHECK_EQ(call(g_volume, args, 2), MMSYSERR_NOERROR);
        CHECK_NEAR(sink_vol_l, 0x8000 / 65535.0, 0.001);
        CHECK_NEAR(sink_vol_r, 1.0, 0.001);
        args[1] = 0u;
        CHECK_EQ(call(g_volume, args, 2), MMSYSERR_NOERROR);
        CHECK_NEAR(sink_vol_l, 0.0, 0.001);
        CHECK_NEAR(sink_vol_r, 0.0, 0.001);
        args[1] = 0xffffffffu;
        CHECK_EQ(call(g_volume, args, 2), MMSYSERR_NOERROR);
    }

    /* --- close: queued header blocks, then the handle dies --- */
    hdr = put_hdr(HDR0, PCM, 4410u * 4u);
    CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_close, 1), WAVERR_STILLPLAYING);
    CHECK_EQ(device_call(g_reset, 1), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_close, 1), MMSYSERR_NOERROR);
    CHECK_EQ(win32_audio_device_count(), 0);
    CHECK_EQ(unprepare(hdr, 32), MMSYSERR_INVALHANDLE);
    CHECK_EQ(position(0x0002u, &rc), 0xaaaaaaaau); /* unchanged */
    CHECK_EQ(rc, MMSYSERR_INVALHANDLE);
    CHECK_EQ(device_call(g_close, 1), MMSYSERR_INVALHANDLE);
    CHECK_EQ(call(g_numdevs, NULL, 0), 1); /* the sink is still installed */

    /* --- mono 8-bit at 8 kHz: unsigned->signed, upsampling, duplication --- */
    {
        wfx = put_wfx(WFX, 1, 1, 8000, 8, 0, 0, 0);
        CHECK_EQ(open_dev(wfx, 0), MMSYSERR_NOERROR);
        memset(n3ds_guest_ptr(PCM, 800), 0xff, 800); /* PCM8 0xff = +127 */
        sink_captured = 0;
        sink_frames = 0;
        hdr = put_hdr(HDR0, PCM, 800);
        CHECK_EQ(prepare(hdr, 32), MMSYSERR_NOERROR);
        CHECK_EQ(write_hdr(hdr, 32), MMSYSERR_NOERROR);
        advance_ms(100);
        CHECK_EQ(sink_l, 32512);
        CHECK_EQ(sink_r, 32512);
        /* 100 ms of 8 kHz = 800 guest frames = 3273 output frames */
        CHECK(sink_frames >= 3250u && sink_frames <= 3290u);
        CHECK(hdr_flags(hdr) & WHDR_DONE);
        CHECK_EQ(unprepare(hdr, 32), MMSYSERR_NOERROR);
        CHECK_EQ(device_call(g_close, 1), MMSYSERR_NOERROR);
    }

    /* --- 48 kHz opens and logs a resample, 4 kHz is refused --- */
    CHECK_EQ(open_dev(good_wfx(WFX, 48000), 0), MMSYSERR_NOERROR);
    CHECK_EQ(device_call(g_close, 1), MMSYSERR_NOERROR);
    CHECK_EQ(open_dev(good_wfx(WFX, 3999), 0), MMSYSERR_NOTSUPPORTED);

    /* --- X3DAudio --- */
    {
        uint32_t args[5];
        CHECK_EQ(x3d_init(0x00000003u /* SPEAKER_STEREO */, 343.5f), 0);
        CHECK_EQ(M32(X3DHANDLE + 0), 0x31443358u);
        CHECK_EQ(M32(X3DHANDLE + 16), M32(X3DHANDLE + 0) ^ M32(X3DHANDLE + 4) ^ M32(X3DHANDLE + 8));
        CHECK_EQ(x3d_init(0x0000003fu /* 5.1 */, 343.5f), E_NOTIMPL);
        CHECK_EQ(x3d_init(0x00000003u, 0.0f), E_INVALIDARG);
        CHECK_EQ(x3d_init(0x00000003u, 343.5f), 0);

        x3d_listener(0.0f, 0.0f, 0.0f);
        x3d_emitter(-2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        x3d_settings(1, 2, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u | 0x4u | 0x20u | 0x40u), 0);
        CHECK_NEAR(MF32(MATRIX + 0), 0.5f, 0.001f);  /* left source, left dst */
        CHECK_NEAR(MF32(MATRIX + 4), 0.0f, 0.001f);
        CHECK_NEAR(MF32(SETTINGS + 16), 0.75f, 0.001f); /* LPF direct default */
        CHECK_NEAR(MF32(SETTINGS + 28), 1.0f, 0.001f);  /* no velocity */
        CHECK_NEAR(MF32(SETTINGS + 32), 1.5707964f, 0.001f); /* emitter front vs +x */
        CHECK_NEAR(MF32(SETTINGS + 36), 2.0f, 0.001f);
        CHECK_NEAR(MF32(SETTINGS + 40), 0.0f, 0.001f);
        CHECK_NEAR(MF32(SETTINGS + 44), 0.0f, 0.001f);

        /* receding emitter at 50 units/s lowers the pitch */
        x3d_emitter(-2.0f, 0.0f, 0.0f, -50.0f, 0.0f, 0.0f);
        CHECK_EQ(x3d_calculate(0x20u), 0);
        CHECK_NEAR(MF32(SETTINGS + 40), -50.0f, 0.001f);
        CHECK_NEAR(MF32(SETTINGS + 28), 343.5f / 393.5f, 0.001f);

        /* right-hand source mirrors the pan */
        x3d_emitter(2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        x3d_settings(1, 2, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u), 0);
        CHECK_NEAR(MF32(MATRIX + 0), 0.0f, 0.001f);
        CHECK_NEAR(MF32(MATRIX + 4), 0.5f, 0.001f);

        /* stereo source passes through with attenuation only */
        x3d_emitter(-2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        M32(EMITTER + 60) = 2;
        x3d_settings(2, 2, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u), 0);
        CHECK_NEAR(MF32(MATRIX + 0), 0.5f, 0.001f);
        CHECK_NEAR(MF32(MATRIX + 4), 0.0f, 0.001f);
        CHECK_NEAR(MF32(MATRIX + 8), 0.0f, 0.001f);
        CHECK_NEAR(MF32(MATRIX + 12), 0.5f, 0.001f);

        /* refusals name the field and leave the settings alone */
        x3d_settings(1, 2, MATRIX);
        MF32(SETTINGS + 16) = -1.0f;
        CHECK_EQ(x3d_calculate(0x2u /* DELAY */), E_NOTIMPL);
        CHECK_NEAR(MF32(SETTINGS + 16), -1.0f, 0.0f);
        CHECK_EQ(x3d_calculate(0x8u /* LPF_REVERB */), E_NOTIMPL);
        CHECK_EQ(x3d_calculate(0x10u /* REVERB */), E_NOTIMPL);
        CHECK_EQ(x3d_calculate(0x10000u /* ZEROCENTER */), E_NOTIMPL);
        CHECK_EQ(x3d_calculate(0x20000u /* REDIRECT_TO_LFE */), E_NOTIMPL);
        CHECK_EQ(x3d_calculate(0x80u /* unknown */), E_NOTIMPL);
        x3d_settings(6, 2, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u), E_NOTIMPL); /* SrcChannelCount */
        x3d_settings(1, 6, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u), E_INVALIDARG); /* DstChannelCount */
        x3d_settings(1, 2, MATRIX);
        M32(EMITTER + 60) = 2;
        CHECK_EQ(x3d_calculate(0x1u), E_INVALIDARG); /* ChannelCount */
        M32(EMITTER + 60) = 1;
        MF32(EMITTER + 52) = 0.5f; /* InnerRadius */
        CHECK_EQ(x3d_calculate(0x1u), E_NOTIMPL);
        MF32(EMITTER + 52) = 0.0f;
        MF32(EMITTER + 56) = 0.1f; /* InnerRadiusAngle */
        CHECK_EQ(x3d_calculate(0x1u), E_NOTIMPL);
        MF32(EMITTER + 56) = 0.0f;
        M32(EMITTER + 60) = 2;
        M32(EMITTER + 68) = PCM; /* pChannelAzimuths with 2 channels */
        x3d_settings(2, 2, MATRIX);
        CHECK_EQ(x3d_calculate(0x1u), E_NOTIMPL);
        M32(EMITTER + 68) = 0;
        M32(EMITTER + 60) = 1;
        x3d_settings(1, 2, 0);
        CHECK_EQ(x3d_calculate(0x1u), E_INVALIDARG); /* NULL matrix */

        /* cone: full volume inside, attenuated outside */
        x3d_settings(1, 2, MATRIX);
        M32(EMITTER + 0) = CONE; /* pCone */
        MF32(CONE + 0) = 3.14159265f; /* inner 180 deg */
        MF32(CONE + 4) = 3.14159265f;
        MF32(CONE + 8) = 1.0f;
        MF32(CONE + 12) = 0.0f;
        MF32(CONE + 16) = 1.0f;
        MF32(CONE + 20) = 1.0f;
        /* emitter due left, front +z: listener is 90 deg off axis = inside */
        CHECK_EQ(x3d_calculate(0x1u), 0);
        CHECK_NEAR(MF32(MATRIX + 0), 0.5f, 0.001f);
        MF32(CONE + 0) = 1.0f; /* inner 57 deg, outer 57 deg */
        MF32(CONE + 4) = 1.0f;
        CHECK_EQ(x3d_calculate(0x1u), 0);
        CHECK_NEAR(MF32(MATRIX + 0), 0.0f, 0.001f); /* outside -> outer volume 0 */
        M32(EMITTER + 0) = 0;

        /* a zeroed handle is rejected, not used */
        memset(n3ds_guest_ptr(X3DHANDLE, 20), 0, 20);
        CHECK_EQ(x3d_calculate(0x1u), E_INVALIDARG);
        args[0] = 0; args[1] = LISTENER; args[2] = EMITTER; args[3] = 0; args[4] = SETTINGS;
        CHECK_EQ(call(g_x3d_calc, args, 5), E_INVALIDARG);
        args[1] = 0;
        CHECK_EQ(call(g_x3d_calc, args, 5), E_INVALIDARG);
    }

    /* --- uninstall is explicit: no backend, no device --- */
    win32_audio_uninstall();
    CHECK_EQ(call(g_numdevs, NULL, 0), 0);
    CHECK_EQ(open_dev(good_wfx(WFX, 44100), 0), MMSYSERR_NODRIVER);

    guest_destroy(&memory);
    if (failures) {
        fprintf(stderr, "FAIL: win32_audio smoke (%u checks, %u failures)\n", checks, failures);
        return 1;
    }
    printf("PASS: win32_audio smoke (%u checks)\n", checks);
    return 0;
}
