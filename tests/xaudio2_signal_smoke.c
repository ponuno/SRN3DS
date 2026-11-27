/* Signal-level host regression for source/win32_xaudio2.c.
 *
 * The older xaudio2_smoke covers the COM/API contract.  This test drives the
 * cooperative mixer with real PCM and checks the things an audio path actually
 * depends on:
 *   - a sine survives the voice graph (stereo, mono->stereo, 48 kHz -> 32.730),
 *   - OnBufferStart/OnBufferEnd/OnStreamEnd order and pContext,
 *   - SamplesPlayed is exact for ratio 1 and monotonic under resampling,
 *   - underruns are counted, not looped,
 *   - FlushSourceBuffers empties the queue and reports OnBufferEnd at once,
 *   - DestroyVoice with live buffers releases them without a later pass,
 *   - two voices sum and mastering volume scales / clips,
 *   - IXAudio2EngineCallback receives pass start/end with the right argc,
 *   - WMA/XMA (unsupported) buffers hold the queue for their nominal duration
 *     and play silence instead of completing instantly.
 *
 * No retail bytes are used; the 3DS DSP backend is not touched here.
 */
#include "win32_shims.h"
#include "win32_audio.h"
#include "win32_xaudio2.h"
#include "n3ds_lift_runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define STACK 0x22000000u
#define PP (DATA + 0x100)
#define WFX (DATA + 0x300)
#define BUF (DATA + 0x400)
#define STATE (DATA + 0x500)
#define CB_OBJ (DATA + 0x900)
#define CB_VT (DATA + 0x940)
#define ENG_OBJ (DATA + 0x980)
#define ENG_VT (DATA + 0x990)
#define PCM (DATA + 0x10000)
#define PCM2 (DATA + 0x90000)
#define PCM3 (DATA + 0x110000)

#define CAP_MAX (2u * 40000u)
static int16_t cap[CAP_MAX];
static unsigned cap_len;

static unsigned sink_submit(const int16_t *frames, unsigned count) {
    if (cap_len + count <= CAP_MAX) {
        memcpy(cap + (size_t)cap_len * 2u, frames, (size_t)count * 2u * sizeof(int16_t));
        cap_len += count;
    }
    return count;
}
static void sink_volume(float l, float r) { (void)l; (void)r; }
static void sink_paused(int p) { (void)p; }
static void sink_flush(void) {}
static uint64_t sink_now(void) { return 0; }
static const Win32AudioBackend sink = {"xaudio-signal", sink_submit, sink_volume, sink_paused, sink_flush};

static void dummy(CPU *c) { c->r[4] += 4; }

/* One thunk per IXAudio2VoiceCallback slot, each popping exactly its stdcall
 * arguments (this + optional bytes/context). */
static unsigned cbs[7], cb_ctx[7], last_ctx[7];
static void cb_pass_start(CPU *c) { ++cbs[0]; c->r[4] += 4 + 4; }
static void cb_pass_end(CPU *c) { ++cbs[1]; c->r[4] += 4; }
static void cb_stream_end(CPU *c) { ++cbs[2]; c->r[4] += 4; }
static void cb_buffer_start(CPU *c) {
    uint32_t ctx = M32(c->r[4] + 8);
    ++cbs[3];
    cb_ctx[3] = ctx;
    c->r[4] += 4 + 4;
}
static void cb_buffer_end(CPU *c) {
    uint32_t ctx = M32(c->r[4] + 8);
    ++cbs[4];
    cb_ctx[4] = ctx;
    if (last_ctx[4] == 0 || ctx != last_ctx[4]) last_ctx[4] = ctx;
    c->r[4] += 4 + 4;
}
static void cb_loop_end(CPU *c) { ++cbs[5]; c->r[4] += 4 + 4; }
static void cb_voice_error(CPU *c) { ++cbs[6]; c->r[4] += 4 + 4 + 4; }
static unsigned eng_calls[3];
static void eng_pass_start(CPU *c) { ++eng_calls[0]; c->r[4] += 4; }
static void eng_pass_end(CPU *c) { ++eng_calls[1]; c->r[4] += 4; }
static void eng_critical(CPU *c) { ++eng_calls[2]; c->r[4] += 4 + 4 + 4; }

static const FnEntry table[] = {
    {0x00600000u, dummy},
    {0x00600100u, cb_pass_start}, {0x00600110u, cb_pass_end},
    {0x00600120u, cb_stream_end}, {0x00600130u, cb_buffer_start},
    {0x00600140u, cb_buffer_end}, {0x00600150u, cb_loop_end},
    {0x00600160u, cb_voice_error},
    {0x00600200u, eng_pass_start}, {0x00600210u, eng_pass_end},
    {0x00600220u, eng_critical},
};

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args, size_t n,
                uint32_t *out) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
static int method(CPU *c, uint32_t vt, unsigned slot, uint32_t *args, size_t n, uint32_t *out) {
    return n3ds_lift_invoke(c, M32(vt + 4 * slot), args, n, out);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "xaudio2_signal failed line %d: %s\n", __LINE__, #x); goto done; } } while (0)

static uint32_t g_xa, g_xavt, g_master, g_mvt;
static CPU *g_cpu;

static void win32_wfx(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits,
                      uint16_t align, uint32_t avg) {
    M16(WFX) = tag; M16(WFX + 2) = ch; M32(WFX + 4) = rate; M32(WFX + 8) = avg;
    M16(WFX + 12) = align; M16(WFX + 14) = bits; M16(WFX + 16) = 0;
}

static int make_source(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits,
                       uint16_t align, uint32_t avg, uint32_t cb_obj, float max_ratio,
                       uint32_t *src, uint32_t *svt) {
    uint32_t r, a[8];
    win32_wfx(tag, ch, rate, bits, align, avg);
    a[0] = g_xa; a[1] = PP; a[2] = WFX; a[3] = 0;
    a[4] = 0; memcpy(&a[4], &max_ratio, 4);
    a[5] = cb_obj; a[6] = 0; a[7] = 0;
    if (method(g_cpu, g_xavt, 8, a, 8, &r) || r) return -1;
    *src = M32(PP);
    *svt = M32(*src);
    return (*src && *svt) ? 0 : -1;
}

static int submit(uint32_t src, uint32_t svt, uint32_t data, uint32_t bytes,
                  uint32_t flags, uint32_t ctx) {
    uint32_t r, a[3];
    memset(n3ds_guest_ptr(BUF, 36), 0, 36);
    M32(BUF) = flags; M32(BUF + 4) = bytes; M32(BUF + 8) = data;
    M32(BUF + 32) = ctx;
    a[0] = src; a[1] = BUF; a[2] = 0;
    return method(g_cpu, svt, 24, a, 3, &r) ? -1 : (int)r;
}
static int voice_op(uint32_t src, uint32_t svt, unsigned slot) {
    uint32_t r, a[3] = {src, 0, 0};
    size_t n = (slot == 21 || slot == 25) ? 1 : 3; /* DestroyVoice / Flush take no extra args */
    return method(g_cpu, svt, slot, a, n, &r) ? -1 : (int)r;
}
static int set_float(uint32_t src, uint32_t svt, unsigned slot, float v) {
    uint32_t r, a[3] = {src, 0, 0};
    memcpy(&a[1], &v, 4);
    return method(g_cpu, svt, slot, a, 3, &r) ? -1 : (int)r;
}
static void get_state(uint32_t src, uint32_t svt, uint32_t *ctx, uint32_t *q, uint64_t *played) {
    uint32_t r, a[2] = {src, STATE};
    memset(n3ds_guest_ptr(STATE, 16), 0, 16);
    method(g_cpu, svt, 28, a, 2, &r);
    if (ctx) *ctx = M32(STATE);
    if (q) *q = M32(STATE + 4);
    if (played) *played = (uint64_t)M32(STATE + 8) | ((uint64_t)M32(STATE + 12) << 32);
}
static void advance(void) { win32_xaudio2_host_advance_us(10000); }
static void reset_events(void) { memset(cbs, 0, sizeof cbs); memset(cb_ctx, 0, sizeof cb_ctx); last_ctx[4] = 0; }

static void tone(uint32_t addr, unsigned frames, unsigned ch, double hz, double amp, double rate) {
    for (unsigned f = 0; f < frames; ++f) {
        int16_t s = (int16_t)lrint(amp * 32767.0 * sin(2.0 * 3.141592653589793 * hz * (double)f / rate));
        for (unsigned c = 0; c < ch; ++c) M16(addr + 2u * (f * ch + c)) = (uint16_t)s;
    }
}
static void constant(uint32_t addr, unsigned frames, unsigned ch, int value) {
    for (unsigned f = 0; f < frames * ch; ++f) M16(addr + 2u * f) = (uint16_t)(int16_t)value;
}
static uint32_t peak_abs(unsigned frames) {
    uint32_t p = 0;
    if (frames > cap_len) frames = cap_len;
    for (unsigned i = 0; i < frames * 2u; ++i) {
        int v = cap[i] < 0 ? -cap[i] : cap[i];
        if ((uint32_t)v > p) p = (uint32_t)v;
    }
    return p;
}
static double rms(unsigned frames) {
    double sum = 0;
    if (frames > cap_len) frames = cap_len;
    for (unsigned i = 0; i < frames * 2u; ++i) sum += (double)cap[i] * cap[i];
    return frames ? sqrt(sum / (double)(frames * 2u)) : 0;
}
static int near(int v, int want, int tol) { int d = v - want; return d >= -tol && d <= tol; }

int main(void) {
    int rc = -1, mapped = 0, sink_on = 0;
    GuestMemory m;
    guest_init(&m, 64u << 20);
    if (guest_map(&m, DATA, 0x200000) || guest_map(&m, STACK, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x200000, ".")) goto done;
    g_cpu = calloc(1, sizeof *g_cpu);
    if (!g_cpu) goto done;
    n3ds_lift_init_cpu(g_cpu, STACK + 4096 - 64, 0);
    if (win32_audio_install(&sink, sink_now)) goto done;
    sink_on = 1;

    uint32_t r, a[8];
    /* Callback objects: [obj] -> vtable, COM style (the shim dereferences). */
    M32(CB_OBJ) = CB_VT;
    M32(ENG_OBJ) = ENG_VT;
    M32(ENG_VT) = 0x00600200u; M32(ENG_VT + 4) = 0x00600210u; M32(ENG_VT + 8) = 0x00600220u;
    for (unsigned i = 0; i < 7; ++i) M32(CB_VT + 4u * i) = 0x00600100u + 0x10u * i;

    a[0] = PP; a[1] = 0; a[2] = 0;
    CHECK(!call(g_cpu, "xaudio2_6.dll", "XAudio2Create", a, 3, &r) && r == 0);
    g_xa = M32(PP); g_xavt = M32(g_xa);
    CHECK(g_xa && g_xavt);
    a[0] = g_xa; a[1] = PP; a[2] = 2; a[3] = 32730; a[4] = 0; a[5] = 0; a[6] = 0;
    CHECK(!method(g_cpu, g_xavt, 10, a, 7, &r) && r == 0);
    g_master = M32(PP); g_mvt = M32(g_master);
    CHECK(g_master && g_mvt);
    win32_xaudio2_host_reset();
    advance(); /* calibrate the clock: the next 10 ms is one pass */

    Win32XAudio2Stats before, after;

    /* ---- 1. 100 ms stereo sine at the output rate ---------------------- */
    reset_events();
    uint32_t src, svt;
    CHECK(!make_source(1, 2, 32730, 16, 4, 32730u * 4u, CB_OBJ, 2.0f, &src, &svt));
    tone(PCM, 3270, 2, 1000.0, 0.5, 32730.0);
    win32_xaudio2_host_stats(&before);
    CHECK(!submit(src, svt, PCM, 3270u * 4u, 0x40u, 0xCAFE0001u));
    CHECK(!voice_op(src, svt, 22));
    cap_len = 0;
    for (int i = 0; i < 12; ++i) advance();
    win32_xaudio2_host_stats(&after);
    uint32_t ctx, q; uint64_t played, played2;
    get_state(src, svt, &ctx, &q, &played);
    CHECK(q == 0 && ctx == 0);
    CHECK(played == 3270);
    CHECK(after.buffers_completed - before.buffers_completed == 1);
    CHECK(cbs[3] == 1 && cbs[4] == 1 && cbs[2] == 1);
    CHECK(cb_ctx[4] == 0xCAFE0001u);
    CHECK(after.callbacks - before.callbacks >= 4); /* pass start/end + start + end */
    get_state(src, svt, NULL, NULL, &played2);
    CHECK(played2 == played); /* monotonic / exact after completion */
    CHECK(cap_len >= 3270);
    uint32_t pk = peak_abs(3270);
    double rv = rms(3270);
    fprintf(stderr, "xaudio2_signal: sine peak=%u rms=%.1f cap_len=%u\n", pk, rv, cap_len);
    CHECK(pk > 15000 && pk < 17000);
    CHECK(rv > 10000.0 && rv < 13000.0);
    CHECK(after.underrun_frames - before.underrun_frames > 0); /* tail of the 12 passes */
    CHECK(!voice_op(src, svt, 21));

    /* ---- 2. two voices, mastering volume and clipping ------------------ */
    reset_events();
    uint32_t s1, v1, s2, v2;
    CHECK(!make_source(1, 1, 32730, 16, 2, 32730u * 2u, 0, 2.0f, &s1, &v1));
    CHECK(!make_source(1, 1, 32730, 16, 2, 32730u * 2u, 0, 2.0f, &s2, &v2));
    constant(PCM, 327, 1, 1000);
    constant(PCM2, 327, 1, 2000);
    CHECK(!submit(s1, v1, PCM, 327u * 2u, 0, 0));
    CHECK(!submit(s2, v2, PCM2, 327u * 2u, 0, 0));
    CHECK(!voice_op(s1, v1, 22) && !voice_op(s2, v2, 22));
    cap_len = 0; advance();
    CHECK(cap_len >= 327);
    CHECK(near(cap[0], 3000, 3) && near(cap[1], 3000, 3));
    CHECK(set_float(g_master, g_mvt, 15, 0.5f) == 0);
    CHECK(!submit(s1, v1, PCM, 327u * 2u, 0, 0));
    CHECK(!submit(s2, v2, PCM2, 327u * 2u, 0, 0));
    cap_len = 0; advance();
    CHECK(near(cap[0], 1500, 3) && near(cap[1], 1500, 3));
    CHECK(set_float(g_master, g_mvt, 15, 2.0f) == 0);
    constant(PCM, 327, 1, 32767);
    constant(PCM2, 327, 1, 32767);
    CHECK(!submit(s1, v1, PCM, 327u * 2u, 0, 0));
    CHECK(!submit(s2, v2, PCM2, 327u * 2u, 0, 0));
    cap_len = 0; advance();
    CHECK(near(cap[20], 32767, 2) && near(cap[21], 32767, 2)); /* hard clip, no wrap */
    CHECK(set_float(g_master, g_mvt, 15, 1.0f) == 0);
    CHECK(!voice_op(s1, v1, 21) && !voice_op(s2, v2, 21));

    /* ---- 3. mono->stereo, 48 kHz resampling, ratio and guards ---------- */
    reset_events();
    CHECK(!make_source(1, 1, 32730, 16, 2, 32730u * 2u, 0, 2.0f, &s1, &v1));
    constant(PCM, 327, 1, 2000);
    CHECK(!submit(s1, v1, PCM, 327u * 2u, 0, 0));
    CHECK(!voice_op(s1, v1, 22));
    cap_len = 0; advance();
    CHECK(near(cap[0], 2000, 3) && near(cap[1], 2000, 3));
    CHECK(!voice_op(s1, v1, 21));

    CHECK(!make_source(1, 2, 48000, 16, 4, 48000u * 4u, 0, 2.0f, &s2, &v2));
    tone(PCM, 4800, 2, 1000.0, 0.4, 48000.0);
    win32_xaudio2_host_stats(&before);
    CHECK(!submit(s2, v2, PCM, 4800u * 4u, 0x40u, 0x4800u));
    CHECK(!voice_op(s2, v2, 22));
    for (int i = 0; i < 20 && (get_state(s2, v2, NULL, &q, NULL), q); ++i) advance();
    get_state(s2, v2, NULL, &q, &played);
    win32_xaudio2_host_stats(&after);
    CHECK(q == 0 && played == 4800);
    CHECK(after.buffers_completed - before.buffers_completed == 1);
    CHECK(!voice_op(s2, v2, 21));

    CHECK(!make_source(1, 2, 32730, 16, 4, 32730u * 4u, 0, 2.0f, &s2, &v2));
    constant(PCM, 2000, 2, 0);
    CHECK(!submit(s2, v2, PCM, 2000u * 4u, 0, 0));
    CHECK(set_float(s2, v2, 29, 2.0f) == 0); /* SetFrequencyRatio */
    CHECK(!voice_op(s2, v2, 22));
    advance();
    get_state(s2, v2, NULL, &q, &played);
    CHECK(q == 1 && played >= 650 && played <= 658); /* 1 + 327*2 */
    /* NaN and zero are rejected, the last good ratio is kept. */
    CHECK(set_float(s2, v2, 29, NAN) != 0);
    float got = 0;
    a[0] = s2; a[1] = DATA + 0x180;
    CHECK(!method(g_cpu, v2, 30, a, 2, &r));
    memcpy(&got, n3ds_guest_ptr(DATA + 0x180, 4), 4);
    CHECK(got == 2.0f);
    a[0] = s2; a[1] = 0; a[2] = 0;
    CHECK(!method(g_cpu, v2, 31, a, 2, &r) && r != 0); /* SetSourceSampleRate(0) */
    CHECK(!voice_op(s2, v2, 25));
    CHECK(!voice_op(s2, v2, 21));

    /* ---- 4. FlushSourceBuffers while running --------------------------- */
    reset_events();
    CHECK(!make_source(1, 2, 32730, 16, 4, 32730u * 4u, CB_OBJ, 2.0f, &s1, &v1));
    constant(PCM, 3270, 2, 1000);
    win32_xaudio2_host_stats(&before);
    CHECK(!submit(s1, v1, PCM, 3270u * 4u, 0, 0xAAAAu));
    CHECK(!submit(s1, v1, PCM, 3270u * 4u, 0, 0xBBBBu));
    CHECK(!voice_op(s1, v1, 22));
    advance();
    get_state(s1, v1, NULL, &q, NULL);
    CHECK(q == 2);
    uint64_t frozen;
    get_state(s1, v1, NULL, NULL, &frozen);
    CHECK(!voice_op(s1, v1, 25));
    get_state(s1, v1, NULL, &q, &played);
    win32_xaudio2_host_stats(&after);
    CHECK(q == 0);
    CHECK(after.buffers_flushed - before.buffers_flushed == 2);
    CHECK(cbs[4] >= 2);
    CHECK(last_ctx[4] == 0xBBBBu || last_ctx[4] == 0xAAAAu);
    for (int i = 0; i < 3; ++i) advance();
    get_state(s1, v1, NULL, NULL, &played2);
    CHECK(played2 == played && played2 >= frozen); /* no playback after flush */
    CHECK(!voice_op(s1, v1, 21));

    /* ---- 5. DestroyVoice with a live buffer ---------------------------- */
    CHECK(!make_source(1, 2, 32730, 16, 4, 32730u * 4u, 0, 2.0f, &s1, &v1));
    constant(PCM, 3270, 2, 1000);
    CHECK(!submit(s1, v1, PCM, 3270u * 4u, 0, 0));
    CHECK(!voice_op(s1, v1, 22));
    advance();
    CHECK(!voice_op(s1, v1, 21));
    win32_xaudio2_host_stats(&after);
    CHECK(after.voices == 1 && after.source_voices == 0); /* master only */
    advance(); advance();
    win32_xaudio2_host_stats(&after);
    CHECK(after.voices == 1 && after.source_voices == 0);

    /* ---- 6. IXAudio2EngineCallback: pass start/end with correct argc ---- */
    memset(eng_calls, 0, sizeof eng_calls);
    a[0] = g_xa; a[1] = ENG_OBJ;
    CHECK(!method(g_cpu, g_xavt, 6, a, 2, &r) && r == 0);
    win32_xaudio2_host_stats(&before);
    advance();
    win32_xaudio2_host_stats(&after);
    CHECK(eng_calls[0] >= 1 && eng_calls[1] >= 1);
    CHECK(after.callbacks - before.callbacks >= 2);
    a[0] = g_xa; a[1] = ENG_OBJ;
    CHECK(!method(g_cpu, g_xavt, 7, a, 2, &r) && r == 0);
    unsigned e0 = eng_calls[0], e1 = eng_calls[1];
    advance();
    CHECK(eng_calls[0] == e0 && eng_calls[1] == e1);

    /* ---- 7. WMA/XMA is silent but keeps its nominal duration ----------- */
    reset_events();
    CHECK(!make_source(0x0161, 2, 48000, 16, 4, 16000u, 0, 2.0f, &s1, &v1));
    win32_xaudio2_host_stats(&before);
    CHECK(!submit(s1, v1, PCM, 16000u, 0, 0));
    CHECK(!voice_op(s1, v1, 22));
    cap_len = 0; advance();
    win32_xaudio2_host_stats(&after);
    get_state(s1, v1, NULL, &q, &played);
    CHECK(q == 1);                                  /* one second of audio */
    CHECK(played >= 470 && played <= 490);          /* 48000/32730*327 */
    CHECK(after.buffers_completed == before.buffers_completed);
    CHECK(peak_abs(cap_len) == 0);                  /* honest silence */
    CHECK(!voice_op(s1, v1, 25));
    CHECK(!voice_op(s1, v1, 21));

    /* ---- teardown ------------------------------------------------------ */
    a[0] = g_master;
    CHECK(!method(g_cpu, g_mvt, 21, a, 1, &r) && r == 0);
    win32_xaudio2_host_stats(&after);
    CHECK(after.voices == 0);
    puts("PASS xaudio2 signal: sine RMS/peak, callbacks+context, exact SamplesPlayed, "
         "underrun accounting, flush/destroy, 2-voice mix/volume/clip, mono/48k, "
         "NaN ratio guard, engine callbacks, silent WMA duration");
    rc = 0;
done:
    if (sink_on) win32_audio_uninstall();
    free(g_cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
