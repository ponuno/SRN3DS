/* XAudio2 2.6/2.7 shim, ported from Skyrim_Android's runtime/xaudio2.c.
 *
 * The upstream mixer is a host COM object set driven by an SDL/pthread engine
 * thread.  This port keeps the voice model, format decoding, buffer queue and
 * callback semantics, and replaces the thread + SDL device with:
 *   - a cooperative 10 ms pass driven by win32_xaudio2_tick() (every shim entry
 *     advances first, exactly like win32_audio);
 *   - output through win32_audio_submit_stereo(), so N3DS_AUDIO=ndsp/opendsp
 *     uses the same NDSP sink as waveOut and N3DS_AUDIO=off simply has no
 *     device while mixing continues.
 * Callbacks run after a pass with the mixer state consistent, so a guest may
 * submit/flush voices from OnBufferEnd.  There is one guest thread, so no
 * locks are needed and none are held across a guest callback.
 */
#include "win32_xaudio2.h"
#include "win32_audio.h"
#include "n3ds_lift_runtime.h"
#include "guest_process.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#include <time.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define PASS_FRAMES_MS 10u
#define MAX_VOICES 64
#define MAX_OBJECTS 96
#define MAX_QUEUE 32
#define MAX_CH 8
#define MAX_SENDS 8
#define MAX_EVENTS 2048
#define XA_MEM_BASE 0x1e300000u
#define XA_MEM_BYTES 0x2000u
#define XA_CB_STACK 0x1e310000u
#define XA_CB_STACK_BYTES 0x10000u
#define OBJ_BASE (XA_MEM_BASE + 0x1000u)

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define E_NOTIMPL 0x80004001u
#define E_OUTOFMEMORY 0x8007000Eu
#define XAUDIO2_E_INVALID_CALL 0x88960001u
#define XAUDIO2_E_DEVICE_INVALIDATED 0x88960004u

enum { V_SOURCE, V_SUBMIX, V_MASTER };
enum { FMT_PCM16, FMT_PCM8, FMT_F32, FMT_PCM24, FMT_PCM32, FMT_ADPCM, FMT_NONE };

typedef struct {
    uint32_t flags, bytes, data, play_begin, play_len, loop_begin, loop_len, loop_count, ctx;
    int16_t *decoded;
    uint32_t nframes, pos, end;
    int loops_left, started;
} XBuf;

typedef struct Voice Voice;
struct Voice {
    int kind, dead, used;
    uint32_t guest, callback;
    int in_ch, rate;
    int fmt, bpf, block_align, spb;
    uint8_t adpcm_coef[64];
    int ncoef;
    float ratio, max_ratio;
    int running;
    XBuf q[MAX_QUEUE];
    int qhead, qn;
    uint64_t played;
    double frac;
    float prev[MAX_CH], cur[MAX_CH];
    int primed;
    float volume, chvol[MAX_CH];
    int nsends;
    Voice *send[MAX_SENDS];
    float matrix[MAX_SENDS][MAX_CH * MAX_CH];
    int matrix_set[MAX_SENDS];
    float *acc;
    int stage;
};

/* Method indices, in vtable order. */
enum {
    I_query, I_addref, I_release,
    I_xa_getdevcount, I_xa_getdevdetails, I_xa_initialize, I_xa_regcb, I_xa_unregcb,
    I_xa_createsource, I_xa_createsubmix, I_xa_createmaster, I_xa_startengine,
    I_xa_stopengine, I_xa_commit, I_xa_perfdata, I_xa_debug,
    I_v_details, I_v_setoutputvoices, I_v_seteffectchain, I_v_enableeffect, I_v_disableeffect,
    I_v_geteffectstate, I_v_seteffectparams, I_v_geteffectparams, I_v_setfilter, I_v_getfilter,
    I_v_setoutfilter, I_v_getoutfilter, I_v_setvolume, I_v_getvolume, I_v_setchvol,
    I_v_getchvol, I_v_setmatrix, I_v_getmatrix, I_v_destroy,
    I_s_start, I_s_stop, I_s_submit, I_s_flush, I_s_discontinuity, I_s_exitloop,
    I_s_getstate, I_s_setratio, I_s_getratio, I_s_setsamplerate,
    I_fx_stub, I_unsupported,
    I_export_xacreate, I_export_createsfx,
    FN_COUNT
};
#define TH(i) (WIN32_XAUDIO2_BASE + 4u * (i))

#define XA_VT (XA_MEM_BASE + 0x000u)
#define VOICE_VT (XA_MEM_BASE + 0x100u)
#define SOURCE_VT (XA_MEM_BASE + 0x200u)
#define FX_VT (XA_MEM_BASE + 0x300u)

static GuestMemory *g_mem;
static Voice *g_voices[MAX_VOICES];
static Voice *g_master;
static uint32_t g_xa_guest;
static uint32_t g_engine_cb[8];
static unsigned g_nengine_cb;
static int g_running;
static uint64_t g_now_us, g_last_pass_us;
static uint32_t g_out_rate = WIN32_AUDIO_OUTPUT_HZ;
static int g_in_tick;
static CPU g_cb_cpu;
static int g_cb_ready;
static Win32XAudio2Stats g_stats;
static uint32_t g_host_cell[MAX_OBJECTS];

typedef struct { uint32_t cb; int method; uint32_t arg; Voice *v; } Event;
static Event g_ev[MAX_EVENTS];
static unsigned g_nev;

static void xa_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[xaudio2] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- format */
static int16_t adpcm_clamp(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static const int k_adpcm_adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

static void adpcm_block(Voice *v, const uint8_t *b, int16_t *out) {
    int ch = v->in_ch, spb = v->spb;
    int pred[2], delta[2], s1[2], s2[2];
    const int16_t *coef = (const int16_t *)v->adpcm_coef;
    for (int c = 0; c < ch; c++) pred[c] = b[c] < v->ncoef ? b[c] : 0;
    b += ch;
    for (int c = 0; c < ch; c++, b += 2) delta[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++, b += 2) s1[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++, b += 2) s2[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++) { out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    int n = 2 * ch, total = spb * ch, hi = 1;
    while (n < total) {
        int c = n % ch;
        int nib = hi ? (*b >> 4) : (*b & 15);
        if (!hi) b++;
        hi = !hi;
        int sn = nib & 8 ? nib - 16 : nib;
        int p = (s1[c] * coef[2 * pred[c]] + s2[c] * coef[2 * pred[c] + 1]) / 256 + sn * delta[c];
        int16_t s = adpcm_clamp(p);
        s2[c] = s1[c];
        s1[c] = s;
        out[n++] = s;
        delta[c] = k_adpcm_adapt[nib] * delta[c] / 256;
        if (delta[c] < 16) delta[c] = 16;
    }
}

static void set_format(Voice *v, uint32_t wf) {
    uint16_t tag = M16(wf);
    int bits = M16(wf + 14);
    v->in_ch = M16(wf + 2);
    if (v->in_ch < 1) v->in_ch = 1;
    if (v->in_ch > MAX_CH) v->in_ch = MAX_CH;
    v->rate = (int)M32(wf + 4);
    if (v->rate < 1000) v->rate = 44100;
    v->block_align = M16(wf + 12);
    if (tag == 0xFFFE) tag = M16(wf + 24);
    v->fmt = FMT_NONE;
    if (tag == 1) v->fmt = bits == 8 ? FMT_PCM8 : bits == 24 ? FMT_PCM24 : bits == 32 ? FMT_PCM32 : FMT_PCM16;
    else if (tag == 3) v->fmt = FMT_F32;
    else if (tag == 2) {
        v->fmt = FMT_ADPCM;
        v->spb = M16(wf + 18);
        v->ncoef = M16(wf + 20);
        if (v->ncoef > 16) v->ncoef = 16;
        if (v->ncoef > 0) memcpy(v->adpcm_coef, n3ds_guest_ptr(wf + 22, (size_t)v->ncoef * 4), (size_t)v->ncoef * 4);
    }
    if (v->fmt == FMT_NONE) {
        static unsigned silent;
        if (++silent % 100 == 1)
            xa_log("unsupported source format tag %04x (%d ch, %d Hz) plays silent (%u so far)",
                   tag, v->in_ch, v->rate, silent);
    }
    v->bpf = v->fmt == FMT_ADPCM ? 2 * v->in_ch
                                 : v->block_align ? v->block_align : v->in_ch * bits / 8;
}

static void read_frame(Voice *v, XBuf *b, uint32_t f, float *out) {
    int ch = v->in_ch;
    if (b->decoded) { for (int c = 0; c < ch; c++) out[c] = b->decoded[f * ch + c] / 32768.0f; return; }
    uint32_t a = b->data + f * (uint32_t)v->bpf;
    switch (v->fmt) {
    case FMT_PCM16: for (int c = 0; c < ch; c++) out[c] = (int16_t)M16(a + 2 * c) / 32768.0f; break;
    case FMT_PCM8: for (int c = 0; c < ch; c++) out[c] = ((float)M8(a + c) - 128.0f) / 128.0f; break;
    case FMT_F32: for (int c = 0; c < ch; c++) out[c] = MF32(a + 4 * c); break;
    case FMT_PCM24:
        for (int c = 0; c < ch; c++) {
            int32_t s = (int32_t)(M8(a + 3 * c) | M8(a + 3 * c + 1) << 8 | M8(a + 3 * c + 2) << 16) << 8;
            out[c] = s / 2147483648.0f;
        }
        break;
    case FMT_PCM32: for (int c = 0; c < ch; c++) out[c] = (int32_t)M32(a + 4 * c) / 2147483648.0f; break;
    default: for (int c = 0; c < ch; c++) out[c] = 0; break;
    }
}

/* ---------------------------------------------------------------- events */
static void push_event(Voice *v, int method, uint32_t arg) {
    if (!v->callback || g_nev >= MAX_EVENTS) return;
    g_ev[g_nev++] = (Event){v->callback, method, arg, v};
}

static int next_frame(Voice *v, float *out) {
    while (v->qn) {
        XBuf *b = &v->q[v->qhead];
        if (!b->started) {
            b->started = 1;
            push_event(v, 3, b->ctx); /* OnBufferStart */
        }
        if (b->pos < b->end) {
            read_frame(v, b, b->pos++, out);
            v->played++;
            if (b->pos >= b->end && b->loops_left && b->loop_len) {
                if (b->loops_left != 255) b->loops_left--;
                b->pos = b->loop_begin;
                b->end = b->loop_begin + b->loop_len;
                if (b->end > b->nframes) b->end = b->nframes;
                push_event(v, 5, b->ctx); /* OnLoopEnd */
            }
            return 1;
        }
        push_event(v, 4, b->ctx); /* OnBufferEnd */
        if (b->flags & 0x40) push_event(v, 2, 0); /* XAUDIO2_END_OF_STREAM */
        free(b->decoded);
        b->decoded = NULL;
        v->qhead = (v->qhead + 1) % MAX_QUEUE;
        v->qn--;
        g_stats.buffers_completed++;
    }
    for (int c = 0; c < v->in_ch; c++) out[c] = 0;
    return 0;
}

/* ---------------------------------------------------------------- mixing */
static void default_matrix(int sc, int dc, float *m) {
    memset(m, 0, sizeof(float) * MAX_CH * MAX_CH);
    for (int d = 0; d < dc; d++)
        for (int s = 0; s < sc; s++) {
            float g = 0;
            if (sc == 1) g = 1.0f;
            else if (sc == dc) g = s == d ? 1.0f : 0.0f;
            else if (dc == 2) g = s == 0 ? (d == 0) : s == 1 ? (d == 1) : (s == 2 || s == 3) ? 0.707f : (s % 2 == d) ? 0.707f : 0.0f;
            else if (dc == 1) g = 1.0f / (float)sc;
            else g = s % dc == d ? 1.0f : 0.0f;
            m[d * sc + s] = g;
        }
}

static void route(Voice *v, const float *in, int frames) {
    Voice *dests[MAX_SENDS];
    int n = v->nsends;
    for (int i = 0; i < n; i++) dests[i] = v->send[i];
    if (!n && g_master && v != g_master) { dests[0] = g_master; n = 1; }
    for (int i = 0; i < n; i++) {
        Voice *d = dests[i];
        if (!d || d->dead || !d->acc) continue;
        float m[MAX_CH * MAX_CH];
        if (i < v->nsends && v->matrix_set[i]) memcpy(m, v->matrix[i], sizeof m);
        else default_matrix(v->in_ch, d->in_ch, m);
        for (int s = 0; s < v->in_ch; s++) {
            float g = v->volume * v->chvol[s];
            for (int dc = 0; dc < d->in_ch; dc++) m[dc * v->in_ch + s] *= g;
        }
        for (int f = 0; f < frames; f++)
            for (int dc = 0; dc < d->in_ch; dc++) {
                float sum = 0;
                for (int s = 0; s < v->in_ch; s++) sum += m[dc * v->in_ch + s] * in[f * v->in_ch + s];
                d->acc[f * d->in_ch + dc] += sum;
            }
    }
}

static void mix_source(Voice *v, int frames, float *tmp) {
    float step = v->ratio * (float)v->rate / (float)g_out_rate;
    int ch = v->in_ch;
    if (!v->primed) {
        next_frame(v, v->cur);
        memcpy(v->prev, v->cur, sizeof v->prev);
        v->primed = 1;
        v->frac = 0;
    }
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < ch; c++) tmp[f * ch + c] = v->prev[c] + (v->cur[c] - v->prev[c]) * (float)v->frac;
        v->frac += step;
        while (v->frac >= 1.0) {
            memcpy(v->prev, v->cur, sizeof v->prev);
            next_frame(v, v->cur);
            v->frac -= 1.0;
        }
    }
    route(v, tmp, frames);
}

static void invoke_cb(uint32_t cb, int method, uint32_t arg) {
    if (!g_cb_ready || !cb) return;
    uint32_t fn = M32(cb + 4u * (uint32_t)method);
    if (!fn) return;
    uint32_t args[2] = {cb, arg};
    uint32_t result = 0;
    if (n3ds_lift_invoke_reentrant(&g_cb_cpu, fn, args, method == 2 ? 1u : 2u, &result) == 0)
        g_stats.callbacks++;
}

static void engine_pass(int frames, float *out, int16_t *pcm) {
    static float tmp[MAX_CH * 512];
    if (frames > 512) frames = 512;
    for (unsigned i = 0; i < g_nengine_cb; i++) invoke_cb(g_engine_cb[i], 0, 0);
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (v && !v->dead && v->kind == V_SOURCE && v->callback && v->running)
            invoke_cb(v->callback, 0, 0); /* OnVoiceProcessingPassStart(0 bytes needed) */
    }
    g_nev = 0;
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (v && !v->dead && v->acc) memset(v->acc, 0, sizeof(float) * (size_t)(v->in_ch * frames));
    }
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (v && !v->dead && v->kind == V_SOURCE && v->running) mix_source(v, frames, tmp);
    }
    for (int stage = 0; stage < 32; stage++)
        for (int i = 0; i < (int)MAX_VOICES; i++) {
            Voice *v = g_voices[i];
            if (!v || v->dead || v->kind != V_SUBMIX || v->stage != stage) continue;
            route(v, v->acc, frames);
        }
    if (g_master && g_master->acc) {
        Voice *m = g_master;
        for (int f = 0; f < frames; f++) {
            float l = 0, r = 0;
            for (int ch = 0; ch < m->in_ch; ch++) {
                float s = m->acc[f * m->in_ch + ch] * m->volume * m->chvol[ch];
                if (m->in_ch == 1) l = r = s;
                else if (ch == 0) l += s;
                else if (ch == 1) r += s;
                else if (ch == 2) { l += 0.707f * s; r += 0.707f * s; }
                else if (ch >= 4) { if (ch % 2 == 0) l += 0.707f * s; else r += 0.707f * s; }
            }
            if (l > 1) l = 1; else if (l < -1) l = -1;
            if (r > 1) r = 1; else if (r < -1) r = -1;
            out[2 * f] = l;
            out[2 * f + 1] = r;
            pcm[2 * f] = (int16_t)(l * 32767.0f);
            pcm[2 * f + 1] = (int16_t)(r * 32767.0f);
        }
    } else {
        memset(out, 0, sizeof(float) * 2 * (size_t)frames);
        memset(pcm, 0, sizeof(int16_t) * 2 * (size_t)frames);
    }
    unsigned nev = g_nev;
    static Event ev[MAX_EVENTS];
    memcpy(ev, g_ev, sizeof(Event) * (size_t)nev);
    for (unsigned i = 0; i < nev; i++)
        if (!ev[i].v->dead) invoke_cb(ev[i].cb, ev[i].method, ev[i].arg);
    for (unsigned i = 0; i < g_nengine_cb; i++) invoke_cb(g_engine_cb[i], 1, 0);
    g_stats.passes++;
    g_stats.frames_mixed += (unsigned)frames;
    /* Free voices destroyed during the pass. */
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (!v || !v->dead) continue;
        for (int k = 0; k < v->qn; k++) free(v->q[(v->qhead + k) % MAX_QUEUE].decoded);
        free(v->acc);
        free(v);
        g_voices[i] = NULL;
    }
}

static uint64_t host_now_us(void) {
#ifdef __3DS__
    return (uint64_t)svcGetSystemTick() * 1000000u / SYSCLOCK_ARM11;
#elif defined(_WIN32)
    LARGE_INTEGER f, n;
    if (!QueryPerformanceFrequency(&f) || !f.QuadPart) return 0;
    QueryPerformanceCounter(&n);
    return (uint64_t)n.QuadPart * 1000000u / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

void win32_xaudio2_tick(uint64_t now_us) {
    if (g_in_tick) return;
    if (!now_us) now_us = host_now_us();
    if (now_us > g_now_us) g_now_us = now_us;
    if (!g_last_pass_us) { g_last_pass_us = g_now_us; return; }
    if (g_now_us - g_last_pass_us < PASS_FRAMES_MS * 1000u) return;
    g_in_tick = 1;
    int frames = (int)(g_out_rate * PASS_FRAMES_MS / 1000u);
    if (frames < 16) frames = 16;
    if (frames > 512) frames = 512;
    static float out[2 * 512];
    static int16_t pcm[2 * 512];
    /* Catch up on the clock, bounded so a long stall cannot spin forever. */
    for (unsigned n = 0; n < 8 && g_now_us - g_last_pass_us >= PASS_FRAMES_MS * 1000u; n++) {
        g_last_pass_us += PASS_FRAMES_MS * 1000u;
        if (!g_running) continue;
        engine_pass(frames, out, pcm);
        unsigned got = win32_audio_submit_stereo(pcm, (unsigned)frames);
        g_stats.frames_submitted += got;
    }
    if (g_now_us - g_last_pass_us > 1000000u) g_last_pass_us = g_now_us;
    g_in_tick = 0;
}

void win32_xaudio2_host_stats(Win32XAudio2Stats *out) {
    if (!out) return;
    *out = g_stats;
    out->voices = 0;
    out->source_voices = 0;
    for (int i = 0; i < (int)MAX_VOICES; i++)
        if (g_voices[i] && !g_voices[i]->dead) {
            out->voices++;
            if (g_voices[i]->kind == V_SOURCE) out->source_voices++;
        }
}

uint64_t win32_xaudio2_host_advance_us(uint64_t delta) {
    g_now_us += delta;
    win32_xaudio2_tick(g_now_us);
    return g_now_us;
}

void win32_xaudio2_host_reset(void) {
    g_stats = (Win32XAudio2Stats){0};
    g_now_us = 0;
    g_last_pass_us = 0;
    g_nev = 0;
}

/* ---------------------------------------------------------------- objects */
static Voice *voice_of(uint32_t addr) {
    if (addr < OBJ_BASE || (addr - OBJ_BASE) % 16) return NULL;
    unsigned i = (addr - OBJ_BASE) / 16;
    if (i >= MAX_OBJECTS) return NULL;
    if (g_host_cell[i] != addr) return NULL;
    Voice *v = NULL;
    for (int k = 0; k < (int)MAX_VOICES; k++)
        if (g_voices[k] && g_voices[k]->guest == addr && !g_voices[k]->dead) { v = g_voices[k]; break; }
    return v;
}

static uint32_t new_guest_cell(void) {
    for (unsigned i = 0; i < MAX_OBJECTS; i++)
        if (!g_host_cell[i]) {
            g_host_cell[i] = OBJ_BASE + 16 * i;
            return g_host_cell[i];
        }
    return 0;
}
static void free_guest_cell(uint32_t addr) {
    if (addr < OBJ_BASE || (addr - OBJ_BASE) % 16) return;
    unsigned i = (addr - OBJ_BASE) / 16;
    if (i < MAX_OBJECTS && g_host_cell[i] == addr) g_host_cell[i] = 0;
}

static Voice *voice_new(int kind, int ch, int rate, uint32_t flags) {
    (void)flags;
    Voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->kind = kind;
    v->in_ch = ch < 1 ? 2 : ch > MAX_CH ? MAX_CH : ch;
    v->rate = rate > 0 ? rate : (int)g_out_rate;
    v->volume = 1.0f;
    v->ratio = 1.0f;
    for (int i = 0; i < MAX_CH; i++) v->chvol[i] = 1.0f;
    if (kind != V_SOURCE) {
        v->acc = calloc((size_t)(v->in_ch * 512), sizeof(float));
        if (!v->acc) { free(v); return NULL; }
    }
    for (int i = 0; i < (int)MAX_VOICES; i++)
        if (!g_voices[i]) { g_voices[i] = v; break; }
    v->used = 1;
    return v;
}

static void set_sends(Voice *v, uint32_t sl) {
    v->nsends = 0;
    memset(v->matrix_set, 0, sizeof v->matrix_set);
    if (!sl) return;
    uint32_t n = M32(sl), p = M32(sl + 4);
    for (uint32_t i = 0; i < n && i < MAX_SENDS; i++) {
        Voice *d = voice_of(M32(p + 8 * i + 4));
        if (d) v->send[v->nsends++] = d;
    }
}

#define A(n) M32(c->r[4] + 4 + 4 * (n))
static float argf(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); win32_xaudio2_tick(0); return; } while (0)
#define V voice_of(A(0))

/* ---------------------------------------------------------------- voice API */
static void s_v_details(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 2);
    M32(A(1)) = 0;
    M32(A(1) + 4) = (uint32_t)v->in_ch;
    M32(A(1) + 8) = (uint32_t)v->rate;
    RET(S_OK, 2);
}
static void s_v_setoutputvoices(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 2); set_sends(v, A(1)); RET(S_OK, 2); }
static void s_v_seteffectchain(CPU *c) { RET(S_OK, 2); }
static void s_v_enableeffect(CPU *c) { RET(S_OK, 3); }
static void s_v_disableeffect(CPU *c) { RET(S_OK, 3); }
static void s_v_geteffectstate(CPU *c) { if (A(2)) M32(A(2)) = 1; RET(S_OK, 3); }
static void s_v_seteffectparams(CPU *c) { RET(S_OK, 5); }
static void s_v_geteffectparams(CPU *c) {
    if (A(2) && A(3)) memset(n3ds_guest_ptr(A(2), A(3)), 0, A(3));
    RET(S_OK, 4);
}
static void s_v_setfilter(CPU *c) { RET(S_OK, 3); }
static void s_v_getfilter(CPU *c) { M32(A(1)) = 0; MF32(A(1) + 4) = 1.0f; MF32(A(1) + 8) = 1.0f; RET(S_OK, 2); }
static void s_v_setoutfilter(CPU *c) { RET(S_OK, 4); }
static void s_v_getoutfilter(CPU *c) { M32(A(2)) = 0; MF32(A(2) + 4) = 1.0f; MF32(A(2) + 8) = 1.0f; RET(S_OK, 3); }
static void s_v_setvolume(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 3); v->volume = argf(A(1)); RET(S_OK, 3); }
static void s_v_getvolume(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 2); MF32(A(1)) = v->volume; RET(S_OK, 2); }
static void s_v_setchvol(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 4);
    for (uint32_t i = 0; i < A(1) && i < MAX_CH; i++) v->chvol[i] = MF32(A(2) + 4 * i);
    RET(S_OK, 4);
}
static void s_v_getchvol(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 3);
    for (uint32_t i = 0; i < A(1) && i < MAX_CH; i++) MF32(A(2) + 4 * i) = v->chvol[i];
    RET(S_OK, 3);
}
static void s_v_setmatrix(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 6);
    Voice *d = voice_of(A(1));
    uint32_t sc = A(2), dc = A(3), m = A(4);
    int idx = -1;
    for (int i = 0; i < v->nsends; i++) if (v->send[i] == d) idx = i;
    if (idx < 0 && !v->nsends && v->kind != V_MASTER) {
        v->send[0] = g_master; v->nsends = 1;
        idx = (d == g_master || !d) ? 0 : -1;
    }
    if (idx >= 0 && sc <= MAX_CH && dc <= MAX_CH) {
        memset(v->matrix[idx], 0, sizeof v->matrix[idx]);
        for (uint32_t k = 0; k < sc * dc; k++) v->matrix[idx][k] = MF32(m + 4 * k);
        v->matrix_set[idx] = 1;
    }
    RET(S_OK, 6);
}
static void s_v_getmatrix(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 5);
    Voice *d = voice_of(A(1));
    uint32_t sc = A(2), dc = A(3), m = A(4);
    float def[MAX_CH * MAX_CH];
    const float *src = def;
    int idx = -1;
    for (int i = 0; i < v->nsends; i++) if (v->send[i] == d) idx = i;
    if (idx >= 0 && v->matrix_set[idx]) src = v->matrix[idx];
    else default_matrix((int)sc, (int)dc, def);
    for (uint32_t k = 0; k < sc * dc && k < MAX_CH * MAX_CH; k++) MF32(m + 4 * k) = src[k];
    RET(S_OK, 5);
}
static void s_v_destroy(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 1);
    v->dead = 1;
    if (v == g_master) g_master = NULL;
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *o = g_voices[i];
        if (o) for (int k = 0; k < o->nsends; k++) if (o->send[k] == v) o->send[k] = NULL;
    }
    free_guest_cell(v->guest);
    M32(v->guest) = 0;
    v->guest = 0;
    RET(S_OK, 1);
}

static void s_s_start(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 3); v->running = 1; RET(S_OK, 3); }
static void s_s_stop(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 3); v->running = 0; RET(S_OK, 3); }
static void s_s_submit(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 3);
    uint32_t b = A(1);
    if (v->qn >= MAX_QUEUE) { g_stats.last_error = XAUDIO2_E_INVALID_CALL; RET(XAUDIO2_E_INVALID_CALL, 3); }
    XBuf *x = &v->q[(v->qhead + v->qn) % MAX_QUEUE];
    memset(x, 0, sizeof *x);
    x->flags = M32(b);
    x->bytes = M32(b + 4);
    x->data = M32(b + 8);
    x->play_begin = M32(b + 12);
    x->play_len = M32(b + 16);
    x->loop_begin = M32(b + 20);
    x->loop_len = M32(b + 24);
    x->loop_count = M32(b + 28);
    x->ctx = M32(b + 32);
    if (v->fmt == FMT_ADPCM && v->block_align && v->spb) {
        uint32_t nb = x->bytes / (uint32_t)v->block_align;
        x->nframes = nb * (uint32_t)v->spb;
        x->decoded = malloc(sizeof(int16_t) * (size_t)x->nframes * (size_t)v->in_ch + 2);
        if (x->decoded)
            for (uint32_t i = 0; i < nb; i++)
                adpcm_block(v, n3ds_guest_ptr(x->data + i * (uint32_t)v->block_align, (size_t)v->block_align),
                            x->decoded + (size_t)i * (size_t)v->spb * (size_t)v->in_ch);
    } else if (v->fmt == FMT_NONE) {
        x->nframes = 0;
    } else {
        x->nframes = v->bpf ? x->bytes / (uint32_t)v->bpf : 0;
    }
    x->pos = x->play_begin < x->nframes ? x->play_begin : x->nframes;
    x->end = x->play_len ? x->play_begin + x->play_len : x->nframes;
    if (x->end > x->nframes) x->end = x->nframes;
    if (!x->loop_len && x->loop_count) x->loop_len = x->end - x->loop_begin;
    x->loops_left = x->loop_count == 255 ? 255 : (int)x->loop_count;
    v->qn++;
    g_stats.buffers_submitted++;
    RET(S_OK, 3);
}
static void s_s_flush(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 1);
    for (int k = v->running ? 1 : 0; k < v->qn; k++) {
        XBuf *b = &v->q[(v->qhead + k) % MAX_QUEUE];
        b->pos = b->end;
        b->loops_left = 0;
        b->flags &= ~0x40u;
    }
    if (!v->running && v->qn) { /* stopped voice: drop the whole queue at once */
        for (int k = 0; k < v->qn; k++) free(v->q[(v->qhead + k) % MAX_QUEUE].decoded);
        v->qn = 0;
    }
    RET(S_OK, 1);
}
static void s_s_discontinuity(CPU *c) { RET(S_OK, 1); }
static void s_s_exitloop(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 2); if (v->qn) v->q[v->qhead].loops_left = 0; RET(S_OK, 2); }
static void s_s_getstate(CPU *c) {
    Voice *v = V; uint32_t s = A(1);
    if (!v) RET(E_INVALIDARG, 2);
    M32(s) = v->qn ? v->q[v->qhead].ctx : 0;
    M32(s + 4) = (uint32_t)v->qn;
    M32(s + 8) = (uint32_t)v->played;
    M32(s + 12) = (uint32_t)(v->played >> 32);
    RET(S_OK, 2);
}
static void s_s_setratio(CPU *c) {
    Voice *v = V; if (!v) RET(E_INVALIDARG, 3);
    float r = argf(A(1));
    if (r < 1.0f / 1024) r = 1.0f / 1024;
    if (v->max_ratio > 0 && r > v->max_ratio) r = v->max_ratio;
    v->ratio = r;
    RET(S_OK, 3);
}
static void s_s_getratio(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 2); MF32(A(1)) = v->ratio; RET(S_OK, 2); }
static void s_s_setsamplerate(CPU *c) { Voice *v = V; if (!v) RET(E_INVALIDARG, 2); v->rate = (int)A(1); RET(S_OK, 2); }

/* ---------------------------------------------------------------- IXAudio2 */
static void s_query(CPU *c) {
    if (A(2)) M32(A(2)) = A(0);
    RET(S_OK, 3);
}
static void s_addref(CPU *c) { RET(1, 1); }
static void s_release(CPU *c) { RET(0, 1); }
static void s_xa_getdevcount(CPU *c) { M32(A(1)) = 1; RET(S_OK, 2); }
static void s_xa_getdevdetails(CPU *c) {
    uint32_t d = A(2);
    memset(n3ds_guest_ptr(d, 1068), 0, 1068);
    const char *id = "N3DS", *name = "Speakers";
    for (int i = 0; id[i]; i++) M16(d + 2 * i) = (uint8_t)id[i];
    for (int i = 0; name[i]; i++) M16(d + 512 + 2 * i) = (uint8_t)name[i];
    M32(d + 1024) = 0xF;
    uint32_t f = d + 1028;
    M16(f) = 0xFFFE;
    M16(f + 2) = 2;
    M32(f + 4) = g_out_rate;
    M32(f + 8) = g_out_rate * 8;
    M16(f + 12) = 8;
    M16(f + 14) = 32;
    M16(f + 16) = 22;
    M16(f + 18) = 32;
    M32(f + 20) = 3;
    static const uint8_t float_guid[16] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    memcpy(n3ds_guest_ptr(f + 24, 16), float_guid, 16);
    RET(S_OK, 3);
}
static void s_xa_initialize(CPU *c) { RET(S_OK, 3); }
static void s_xa_regcb(CPU *c) { if (g_nengine_cb < 8) g_engine_cb[g_nengine_cb++] = A(1); RET(S_OK, 2); }
static void s_xa_unregcb(CPU *c) {
    for (unsigned i = 0; i < g_nengine_cb; i++)
        if (g_engine_cb[i] == A(1)) { g_engine_cb[i] = g_engine_cb[--g_nengine_cb]; break; }
    RET(S_OK, 2);
}
static void s_xa_createsource(CPU *c) {
    uint32_t wf = A(2);
    if (!wf) RET(E_INVALIDARG, 8);
    Voice *v = voice_new(V_SOURCE, M16(wf + 2), (int)M32(wf + 4), A(3));
    if (!v) RET(E_OUTOFMEMORY, 8);
    set_format(v, wf);
    v->max_ratio = argf(A(4));
    v->callback = A(5);
    set_sends(v, A(6));
    v->guest = new_guest_cell();
    if (!v->guest) { v->dead = 1; RET(E_OUTOFMEMORY, 8); }
    M32(v->guest) = SOURCE_VT;
    M32(A(1)) = v->guest;
    RET(S_OK, 8);
}
static void s_xa_createsubmix(CPU *c) {
    Voice *v = voice_new(V_SUBMIX, (int)A(2), (int)A(3), A(4));
    if (!v) RET(E_OUTOFMEMORY, 8);
    v->stage = (int)(A(5) < 32 ? A(5) : 31);
    set_sends(v, A(6));
    v->guest = new_guest_cell();
    if (!v->guest) { v->dead = 1; RET(E_OUTOFMEMORY, 8); }
    M32(v->guest) = VOICE_VT;
    M32(A(1)) = v->guest;
    RET(S_OK, 8);
}
static void s_xa_createmaster(CPU *c) {
    int ch = A(2) ? (int)A(2) : 2;
    Voice *v = voice_new(V_MASTER, ch, (int)A(3), A(4));
    if (!v) RET(E_OUTOFMEMORY, 7);
    g_master = v;
    v->guest = new_guest_cell();
    if (!v->guest) { v->dead = 1; g_master = NULL; RET(E_OUTOFMEMORY, 7); }
    M32(v->guest) = VOICE_VT;
    M32(A(1)) = v->guest;
    g_running = 1;
    xa_log("mastering voice %d ch, output %u Hz stereo (sink %s)", v->in_ch, g_out_rate,
           win32_audio_backend_present() ? "installed" : "absent");
    RET(S_OK, 7);
}
static void s_xa_startengine(CPU *c) { g_running = 1; RET(S_OK, 1); }
static void s_xa_stopengine(CPU *c) { g_running = 0; RET(S_OK, 1); }
static void s_xa_commit(CPU *c) { RET(S_OK, 2); }
static void s_xa_perfdata(CPU *c) { memset(n3ds_guest_ptr(A(1), 64), 0, 64); RET(S_OK, 2); }
static void s_xa_debug(CPU *c) { RET(S_OK, 3); }
static void s_fx_stub(CPU *c) { RET(S_OK, 1); }

/* XAPOFX1_5!CreateFX(REFCLSID, IUnknown **, const void *, UINT32), cdecl. */
static void s_CreateFX(CPU *c) {
    uint32_t cell = new_guest_cell();
    if (!cell) RET(E_OUTOFMEMORY, 4);
    M32(cell) = FX_VT;
    M32(A(1)) = cell;
    RET(S_OK, 4);
}

/* ---------------------------------------------------------------- defs */
static void s_XAudio2Create(CPU *c);
static GuestFn fn_table[FN_COUNT];
static void init_functions(void) {
    fn_table[I_query] = s_query;
    fn_table[I_addref] = s_addref;
    fn_table[I_release] = s_release;
    fn_table[I_xa_getdevcount] = s_xa_getdevcount;
    fn_table[I_xa_getdevdetails] = s_xa_getdevdetails;
    fn_table[I_xa_initialize] = s_xa_initialize;
    fn_table[I_xa_regcb] = s_xa_regcb;
    fn_table[I_xa_unregcb] = s_xa_unregcb;
    fn_table[I_xa_createsource] = s_xa_createsource;
    fn_table[I_xa_createsubmix] = s_xa_createsubmix;
    fn_table[I_xa_createmaster] = s_xa_createmaster;
    fn_table[I_xa_startengine] = s_xa_startengine;
    fn_table[I_xa_stopengine] = s_xa_stopengine;
    fn_table[I_xa_commit] = s_xa_commit;
    fn_table[I_xa_perfdata] = s_xa_perfdata;
    fn_table[I_xa_debug] = s_xa_debug;
    fn_table[I_v_details] = s_v_details;
    fn_table[I_v_setoutputvoices] = s_v_setoutputvoices;
    fn_table[I_v_seteffectchain] = s_v_seteffectchain;
    fn_table[I_v_enableeffect] = s_v_enableeffect;
    fn_table[I_v_disableeffect] = s_v_disableeffect;
    fn_table[I_v_geteffectstate] = s_v_geteffectstate;
    fn_table[I_v_seteffectparams] = s_v_seteffectparams;
    fn_table[I_v_geteffectparams] = s_v_geteffectparams;
    fn_table[I_v_setfilter] = s_v_setfilter;
    fn_table[I_v_getfilter] = s_v_getfilter;
    fn_table[I_v_setoutfilter] = s_v_setoutfilter;
    fn_table[I_v_getoutfilter] = s_v_getoutfilter;
    fn_table[I_v_setvolume] = s_v_setvolume;
    fn_table[I_v_getvolume] = s_v_getvolume;
    fn_table[I_v_setchvol] = s_v_setchvol;
    fn_table[I_v_getchvol] = s_v_getchvol;
    fn_table[I_v_setmatrix] = s_v_setmatrix;
    fn_table[I_v_getmatrix] = s_v_getmatrix;
    fn_table[I_v_destroy] = s_v_destroy;
    fn_table[I_s_start] = s_s_start;
    fn_table[I_s_stop] = s_s_stop;
    fn_table[I_s_submit] = s_s_submit;
    fn_table[I_s_flush] = s_s_flush;
    fn_table[I_s_discontinuity] = s_s_discontinuity;
    fn_table[I_s_exitloop] = s_s_exitloop;
    fn_table[I_s_getstate] = s_s_getstate;
    fn_table[I_s_setratio] = s_s_setratio;
    fn_table[I_s_getratio] = s_s_getratio;
    fn_table[I_s_setsamplerate] = s_s_setsamplerate;
    fn_table[I_fx_stub] = s_fx_stub;
    fn_table[I_unsupported] = s_fx_stub;
    fn_table[I_export_xacreate] = s_XAudio2Create;
    fn_table[I_export_createsfx] = s_CreateFX;
}

static int build_vtables(void) {
    uint32_t xa[16], voice[22], source[32], fx[8];
    for (unsigned i = 0; i < 16; i++) xa[i] = TH(I_unsupported);
    xa[0] = TH(I_query); xa[1] = TH(I_addref); xa[2] = TH(I_release);
    xa[3] = TH(I_xa_getdevcount); xa[4] = TH(I_xa_getdevdetails); xa[5] = TH(I_xa_initialize);
    xa[6] = TH(I_xa_regcb); xa[7] = TH(I_xa_unregcb);
    xa[8] = TH(I_xa_createsource); xa[9] = TH(I_xa_createsubmix); xa[10] = TH(I_xa_createmaster);
    xa[11] = TH(I_xa_startengine); xa[12] = TH(I_xa_stopengine); xa[13] = TH(I_xa_commit);
    xa[14] = TH(I_xa_perfdata); xa[15] = TH(I_xa_debug);
    for (unsigned i = 0; i < 22; i++) voice[i] = TH(I_unsupported);
    voice[0] = TH(I_query); voice[1] = TH(I_addref); voice[2] = TH(I_release);
    voice[3] = TH(I_v_details); voice[4] = TH(I_v_setoutputvoices); voice[5] = TH(I_v_seteffectchain);
    voice[6] = TH(I_v_enableeffect); voice[7] = TH(I_v_disableeffect); voice[8] = TH(I_v_geteffectstate);
    voice[9] = TH(I_v_seteffectparams); voice[10] = TH(I_v_geteffectparams);
    voice[11] = TH(I_v_setfilter); voice[12] = TH(I_v_getfilter);
    voice[13] = TH(I_v_setoutfilter); voice[14] = TH(I_v_getoutfilter);
    voice[15] = TH(I_v_setvolume); voice[16] = TH(I_v_getvolume);
    voice[17] = TH(I_v_setchvol); voice[18] = TH(I_v_getchvol);
    voice[19] = TH(I_v_setmatrix); voice[20] = TH(I_v_getmatrix); voice[21] = TH(I_v_destroy);
    for (unsigned i = 0; i < 32; i++) source[i] = voice[i < 22 ? i : 21];
    source[22] = TH(I_s_start); source[23] = TH(I_s_stop); source[24] = TH(I_s_submit);
    source[25] = TH(I_s_flush); source[26] = TH(I_s_discontinuity); source[27] = TH(I_s_exitloop);
    source[28] = TH(I_s_getstate); source[29] = TH(I_s_setratio); source[30] = TH(I_s_getratio);
    source[31] = TH(I_s_setsamplerate);
    for (unsigned i = 0; i < 8; i++) fx[i] = TH(I_fx_stub);
    fx[0] = TH(I_query); fx[1] = TH(I_addref); fx[2] = TH(I_release);
    uint32_t *tables[4] = {xa, voice, source, fx};
    uint32_t bases[4] = {XA_VT, VOICE_VT, SOURCE_VT, FX_VT};
    unsigned sizes[4] = {16, 22, 32, 8};
    for (int t = 0; t < 4; t++)
        for (unsigned i = 0; i < sizes[t]; i++)
            if (guest_write32(g_mem, bases[t] + 4 * i, tables[t][i])) return -1;
    return 0;
}

int win32_xaudio2_init(GuestMemory *m) {
    if (!m || g_mem) return -1;
    memset(&g_stats, 0, sizeof g_stats);
    memset(g_voices, 0, sizeof g_voices);
    memset(g_host_cell, 0, sizeof g_host_cell);
    g_master = NULL;
    g_xa_guest = 0;
    g_nengine_cb = 0;
    g_running = 0;
    g_now_us = g_last_pass_us = 0;
    g_in_tick = 0;
    g_mem = m;
    if (guest_map(m, XA_MEM_BASE, XA_MEM_BYTES)) { g_mem = NULL; return -1; }
    if (guest_map(m, XA_CB_STACK, XA_CB_STACK_BYTES)) {
        guest_unmap(m, XA_MEM_BASE); g_mem = NULL; return -1;
    }
    init_functions();
    if (build_vtables()) {
        guest_unmap(m, XA_CB_STACK); guest_unmap(m, XA_MEM_BASE); g_mem = NULL; return -1;
    }
    uint32_t teb = guest_ptr(m, GP_TEB, 0x100) ? GP_TEB : 0;
    n3ds_lift_init_cpu(&g_cb_cpu, XA_CB_STACK + XA_CB_STACK_BYTES - 64, teb);
    g_cb_ready = 1;
    return 0;
}

void win32_xaudio2_shutdown(void) {
    if (!g_mem) return;
    for (int i = 0; i < (int)MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (!v) continue;
        for (int k = 0; k < v->qn; k++) free(v->q[(v->qhead + k) % MAX_QUEUE].decoded);
        free(v->acc);
        free(v);
        g_voices[i] = NULL;
    }
    if (g_xa_guest) free_guest_cell(g_xa_guest);
    g_xa_guest = 0;
    g_master = NULL;
    g_cb_ready = 0;
    guest_unmap(g_mem, XA_CB_STACK);
    guest_unmap(g_mem, XA_MEM_BASE);
    g_mem = NULL;
}

static int dll_is(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

typedef struct { const char *dll, *name; unsigned fn; } Named;
static const Named named[] = {
    {"xaudio2_6.dll", "XAudio2Create", I_xa_initialize},
    {"xaudio2_7.dll", "XAudio2Create", I_xa_initialize},
    {"xaudio2_8.dll", "XAudio2Create", I_xa_initialize},
    {"xaudio2.dll", "XAudio2Create", I_xa_initialize},
    {"xapofx1_5.dll", "CreateFX", I_unsupported},
};

/* XAudio2Create(IXAudio2 **pp, UINT32 flags, XAUDIO2_PROCESSOR proc): allocate
 * a fresh IXAudio2 object (upstream's CoCreateInstance path).  Registered for
 * every XAudio2 module name, so the version DLL does not matter. */
static void s_XAudio2Create(CPU *c) {
    uint32_t cell = new_guest_cell();
    if (!cell || !A(0)) RET(E_INVALIDARG, 3);
    M32(cell) = XA_VT;
    M32(A(0)) = cell;
    g_xa_guest = cell;
    g_running = 1;
    xa_log("created IXAudio2 %08lx", (unsigned long)cell);
    RET(S_OK, 3);
}

const Win32XAudio2Def *win32_xaudio2_defs(unsigned *count) {
    static Win32XAudio2Def defs[sizeof named / sizeof named[0]];
    unsigned n = (unsigned)(sizeof named / sizeof named[0]);
    for (unsigned i = 0; i < n; i++) {
        defs[i].dll = named[i].dll;
        defs[i].name = named[i].name;
        defs[i].fn = dll_is(named[i].dll, "xapofx1_5.dll") ? s_CreateFX : s_XAudio2Create;
    }
    if (count) *count = n;
    return defs;
}

uint32_t win32_xaudio2_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (dll_is(dll, named[i].dll) && !strcmp(name, named[i].name))
            return TH(dll_is(dll, "xapofx1_5.dll") ? I_export_createsfx : I_export_xacreate);
    return 0;
}

static FnEntry entries[1];
const FnEntry *win32_xaudio2_lookup(uint32_t addr) {
    if (addr < WIN32_XAUDIO2_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_XAUDIO2_BASE) / 4;
    if (i >= FN_COUNT) return NULL;
    entries[0] = (FnEntry){addr, fn_table[i]};
    return &entries[0];
}
