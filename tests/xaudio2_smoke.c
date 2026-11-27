/* Host regression for the ported XAudio2 shim (source/win32_xaudio2.c).
 *
 * Exercises the guest-visible COM path end to end: XAudio2Create, mastering and
 * source voices, WAVEFORMATEX parsing, SubmitSourceBuffer/Start/GetState,
 * volume/frequency ratio, FlushSourceBuffers, the 10 ms cooperative mixer and
 * the win32_audio sink bridge.  No guest callbacks are registered here (their
 * OnBufferEnd dispatch is covered by the module's event path and is not faked).
 */
#include "win32_shims.h"
#include "win32_audio.h"
#include "win32_xaudio2.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define STACK 0x22000000u
#define PP (DATA + 0x100)
#define WFX (DATA + 0x300)
#define BUF (DATA + 0x400)
#define STATE (DATA + 0x500)
#define PCM (DATA + 0x1000)

static unsigned sink_submits, sink_frames;
static unsigned sink_submit(const int16_t *frames, unsigned count) {
    (void)frames;
    ++sink_submits;
    sink_frames += count;
    return count;
}
static void sink_volume(float l, float r) { (void)l; (void)r; }
static void sink_paused(int p) { (void)p; }
static void sink_flush(void) {}
static uint64_t sink_now(void) { return 0; }
static const Win32AudioBackend sink = {"xaudio-test", sink_submit, sink_volume, sink_paused, sink_flush};

static void dummy(CPU *c) { c->r[4] += 4; }
static unsigned cb_events, cb_last_ctx;
/* One host thunk per IXAudio2VoiceCallback slot: each pops exactly the
 * stdcall arguments of its method, so the lift runtime's ESP check passes. */
static void cb_pass_start(CPU *c) { ++cb_events; c->r[4] += 4 + 4; }
static void cb_pass_end(CPU *c) { ++cb_events; c->r[4] += 4; }
static void cb_stream_end(CPU *c) { ++cb_events; c->r[4] += 4; }
static void cb_buffer_start(CPU *c) { ++cb_events; c->r[4] += 4 + 4; }
static void cb_buffer_end(CPU *c) {
    ++cb_events;
    uint32_t ctx = M32(c->r[4] + 8);
    if (ctx) cb_last_ctx = ctx;
    c->r[4] += 4 + 4;
}
static void cb_loop_end(CPU *c) { ++cb_events; c->r[4] += 4 + 4; }
static void cb_voice_error(CPU *c) { ++cb_events; c->r[4] += 4 + 4 + 4; }
static const FnEntry table[] = {
    {0x00500000u, dummy},
    {0x00501000u, cb_pass_start}, {0x00501010u, cb_pass_end},
    {0x00501020u, cb_stream_end}, {0x00501030u, cb_buffer_start},
    {0x00501040u, cb_buffer_end}, {0x00501050u, cb_loop_end},
    {0x00501060u, cb_voice_error},
};

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args, size_t n, uint32_t *out) {    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
static int method(CPU *c, uint32_t vt, unsigned slot, uint32_t *args, size_t n, uint32_t *out) {
    return n3ds_lift_invoke(c, M32(vt + 4 * slot), args, n, out);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "xaudio2 test failed at line %d: %s\n", __LINE__, #x); goto done; } } while (0)

int main(void) {
    int rc = -1, mapped = 0, sink_on = 0;
    GuestMemory m;
    CPU *cpu = NULL;
    guest_init(&m, 16u << 20);
    if (guest_map(&m, DATA, 0x4000) || guest_map(&m, STACK, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x200000, ".")) goto done;
    cpu = calloc(1, sizeof *cpu);
    if (!cpu) goto done;
    n3ds_lift_init_cpu(cpu, STACK + 4096 - 64, 0);
    if (win32_audio_install(&sink, sink_now)) goto done;
    sink_on = 1;
    uint32_t r, a[8];

    /* XAudio2Create from the 2.6 module name Skyrim links. */
    a[0] = PP; a[1] = 0; a[2] = 0;
    CHECK(!call(cpu, "xaudio2_6.dll", "XAudio2Create", a, 3, &r) && r == 0);
    uint32_t xa = M32(PP), xavt = M32(xa);
    CHECK(xa != 0 && xavt != 0);
    a[0] = xa; a[1] = DATA + 0x140;
    CHECK(!method(cpu, xavt, 3, a, 2, &r) && r == 0 && M32(DATA + 0x140) == 1);

    /* Mastering voice: 2 ch, guest rate 48000, stereo sink at 32730 Hz. */
    a[0] = xa; a[1] = PP; a[2] = 2; a[3] = 48000; a[4] = 0; a[5] = 0; a[6] = 0;
    CHECK(!method(cpu, xavt, 10, a, 7, &r) && r == 0);
    uint32_t master = M32(PP), mvt = M32(master);
    CHECK(master && mvt);
    a[0] = master; a[1] = DATA + 0x160;
    CHECK(!method(cpu, mvt, 3, a, 2, &r) && r == 0);
    CHECK(M32(DATA + 0x164) == 2 && M32(DATA + 0x168) == 48000);
    a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = 0;
    /* (XAudio2 layout: CreateSourceVoice(this, ppVoice, fmt, flags, maxRatio, cb, sends)) */
    M16(WFX) = 1; M16(WFX + 2) = 2; M32(WFX + 4) = 48000;
    M32(WFX + 8) = 192000; M16(WFX + 12) = 4; M16(WFX + 14) = 16; M16(WFX + 16) = 0;
    a[0] = xa; a[1] = PP; a[2] = WFX; a[3] = 0; a[4] = 0x40000000u; a[5] = DATA + 0x900; a[6] = 0; a[7] = 0;
    /* COM callback object: [0x900] -> vtable at 0x940, one slot per method. */
    M32(DATA + 0x900) = DATA + 0x940;
    for (unsigned i = 0; i < 7; ++i) M32(DATA + 0x940 + 4 * i) = 0x00501000u + 0x10u * i;
    CHECK(!method(cpu, xavt, 8, a, 8, &r) && r == 0);
    uint32_t src = M32(PP), svt = M32(src);
    CHECK(src && svt);

    /* 480 frames of a constant 1000 PCM16 stereo. */
    for (unsigned i = 0; i < 480; ++i) { M16(PCM + 4 * i) = 1000; M16(PCM + 4 * i + 2) = 1000; }
    memset(n3ds_guest_ptr(BUF, 36), 0, 36);
    M32(BUF + 4) = 1920; M32(BUF + 8) = PCM; M32(BUF + 32) = 0xCAFE;
    a[0] = src; a[1] = BUF; a[2] = 0;
    CHECK(!method(cpu, svt, 24, a, 3, &r) && r == 0);
    a[0] = src; a[1] = 0; a[2] = 0;
    CHECK(!method(cpu, svt, 22, a, 3, &r) && r == 0);
    a[0] = src; a[1] = 0x3F000000u; /* 0.5f */ a[2] = 0;
    CHECK(!method(cpu, svt, 15, a, 3, &r) && r == 0);
    a[0] = src; a[1] = DATA + 0x180;
    CHECK(!method(cpu, svt, 16, a, 2, &r) && r == 0 && MF32(DATA + 0x180) == 0.5f);
    a[0] = src; a[1] = DATA + 0x184;
    CHECK(!method(cpu, svt, 28, a, 2, &r) && r == 0);
    CHECK(M32(DATA + 0x188) == 1 && M32(DATA + 0x184) == 0xCAFE);

    /* Drive the mixer deterministically: two 10 ms advances mix one pass. */
    win32_xaudio2_host_reset();
    sink_submits = sink_frames = 0;
    win32_xaudio2_host_advance_us(10000);
    win32_xaudio2_host_advance_us(10000);
    win32_xaudio2_host_advance_us(10000);
    win32_xaudio2_host_advance_us(10000);
    win32_xaudio2_host_advance_us(10000);
    Win32XAudio2Stats st;
    win32_xaudio2_host_stats(&st);
    CHECK(st.passes >= 1 && st.frames_mixed >= 327);
    CHECK(st.buffers_completed == 1);
    CHECK(sink_submits >= 1 && sink_frames >= 327);
    CHECK(st.frames_submitted == sink_frames);
    CHECK(cb_events >= 2 && cb_last_ctx == 0xCAFE);
    CHECK(st.callbacks >= 2);
    a[0] = src; a[1] = STATE;
    CHECK(!method(cpu, svt, 28, a, 2, &r) && r == 0);
    CHECK(M32(STATE + 4) == 0);                 /* BuffersQueued */
    CHECK(M32(STATE + 8) == 480);               /* SamplesPlayed */
    CHECK(M32(STATE) == 0);

    /* Flush while stopped drops the whole queue immediately (no engine pass). */
    a[0] = src; a[1] = 0; a[2] = 0;
    CHECK(!method(cpu, svt, 23, a, 3, &r) && r == 0);
    a[0] = src; a[1] = BUF; a[2] = 0;
    CHECK(!method(cpu, svt, 24, a, 3, &r) && r == 0);
    a[0] = src;
    CHECK(!method(cpu, svt, 25, a, 1, &r) && r == 0);
    win32_xaudio2_host_advance_us(10000);
    win32_xaudio2_host_advance_us(10000);
    a[0] = src; a[1] = STATE;
    CHECK(!method(cpu, svt, 28, a, 2, &r) && r == 0 && M32(STATE + 4) == 0);

    /* Frequency ratio is clamped to the CreateSourceVoice maximum. */
    a[0] = src; a[1] = 0x42C60000u; /* 99.0f */ a[2] = 0;
    CHECK(!method(cpu, svt, 29, a, 3, &r) && r == 0);
    a[0] = src; a[1] = DATA + 0x190;
    CHECK(!method(cpu, svt, 30, a, 2, &r) && r == 0 && MF32(DATA + 0x190) <= 2.0f);

    /* XAPOFX1_5!CreateFX produces a live IUnknown cell. */
    a[0] = 0; a[1] = PP; a[2] = 0; a[3] = 0;
    CHECK(!call(cpu, "xapofx1_5.dll", "CreateFX", a, 4, &r) && r == 0 && M32(PP) != 0);

    a[0] = src; CHECK(!method(cpu, svt, 21, a, 1, &r) && r == 0);
    a[0] = master; CHECK(!method(cpu, mvt, 21, a, 1, &r) && r == 0);
    win32_xaudio2_host_stats(&st);
    CHECK(st.voices == 0);

    rc = 0;
done:
    if (sink_on) win32_audio_uninstall();
    free(cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
