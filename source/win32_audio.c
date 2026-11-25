/* Win32 waveOut + X3DAudio1_7 subset with an external audio sink.
 *
 * One guest device at a time.  Guest buffers are converted (linear resampling
 * to WIN32_AUDIO_OUTPUT_HZ, mono->stereo, PCM8 unsigned -> signed) into a ring
 * buffer that the backend drains; WAVEHDR completion follows a sample counter
 * advanced by an injected monotonic clock, never by the call that queued it.
 */
#include "win32_audio.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUE 16u
#define RING_FRAMES 4096u
#define AUDIO_MIN_RATE 4000u
#define AUDIO_MAX_RATE 192000u

/* MMRESULT / WAVERR values (mmsystem.h). */
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_ALLOCATED 4u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_NOTSUPPORTED 8u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u

#define WHDR_DONE 0x00000001u
#define WHDR_PREPARED 0x00000002u
#define WHDR_BEGINLOOP 0x00000004u
#define WHDR_ENDLOOP 0x00000008u
#define WHDR_INQUEUE 0x00000010u

#define WAVEHDR_SIZE 32u
#define WHDR_DATA 0u
#define WHDR_LENGTH 4u
#define WHDR_FLAGS 16u
#define WHDR_LOOPS 20u

#define WFX_SIZE 18u
#define WFX_TAG 0u
#define WFX_CHANNELS 2u
#define WFX_RATE 4u
#define WFX_AVGBYTES 8u
#define WFX_ALIGN 12u
#define WFX_BITS 14u
#define WFX_CBSIZE 16u
#define WAVE_FORMAT_PCM 1u

#define WAVE_MAPPER 0xffffffffu
#define WAVE_FORMAT_QUERY 0x00000001u
#define WAVE_ALLOWSYNC 0x00000002u
#define WAVE_MAPPED 0x00000004u
#define WAVE_FORMAT_DIRECT 0x00000008u
#define WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE 0x00000010u
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_NULL 0x00000000u
#define CALLBACK_FUNCTION 0x00030000u
#define CALLBACK_EVENT 0x00050000u

#define TIME_MS 0x0001u
#define TIME_SAMPLES 0x0002u
#define TIME_BYTES 0x0004u
#define TIME_SMPTE 0x0008u
#define TIME_MIDI 0x0010u
#define TIME_TICKS 0x0020u

#define WAVE_FORMAT_1M08 0x00000001u
#define WAVE_FORMAT_1M16 0x00000002u
#define WAVE_FORMAT_1S08 0x00000004u
#define WAVE_FORMAT_1S16 0x00000008u
#define WAVE_FORMAT_2M08 0x00000010u
#define WAVE_FORMAT_2M16 0x00000020u
#define WAVE_FORMAT_2S08 0x00000040u
#define WAVE_FORMAT_2S16 0x00000080u
#define WAVE_FORMAT_4M08 0x00000100u
#define WAVE_FORMAT_4M16 0x00000200u
#define WAVE_FORMAT_4S08 0x00000400u
#define WAVE_FORMAT_4S16 0x00000800u
#define WAVE_FORMAT_48M08 0x00001000u
#define WAVE_FORMAT_48M16 0x00002000u
#define WAVE_FORMAT_48S08 0x00004000u
#define WAVE_FORMAT_48S16 0x00008000u
#define WAVECAPS_VOLUME 0x0004u
#define WAVECAPS_LRVOLUME 0x0008u
#define WAVEOUTCAPS_SIZE 52u

#define X3D_HANDLE_SIZE 20u
#define X3D_HANDLE_MAGIC 0x31443358u /* "X3D1" */
#define SPEAKER_STEREO 0x00000003u
#define X3DC_MATRIX 0x00000001u
#define X3DC_DELAY 0x00000002u
#define X3DC_LPF_DIRECT 0x00000004u
#define X3DC_LPF_REVERB 0x00000008u
#define X3DC_REVERB 0x00000010u
#define X3DC_DOPPLER 0x00000020u
#define X3DC_EMITTER_ANGLE 0x00000040u
#define X3DC_ZEROCENTER 0x00010000u
#define X3DC_REDIRECT_TO_LFE 0x00020000u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u

/* X3DAUDIO_LISTENER */
#define LISTENER_FRONT 0u
#define LISTENER_TOP 12u
#define LISTENER_POS 24u
#define LISTENER_VEL 36u
/* X3DAUDIO_EMITTER (DirectX SDK / XAudio 2.7 layout, 100 bytes) */
#define EMITTER_CONE 0u
#define EMITTER_FRONT 4u
#define EMITTER_TOP 16u
#define EMITTER_POS 28u
#define EMITTER_VEL 40u
#define EMITTER_INNER_RADIUS 52u
#define EMITTER_INNER_ANGLE 56u
#define EMITTER_CHANNELS 60u
#define EMITTER_AZIMUTHS 68u
#define EMITTER_VOLUME_CURVE 72u
#define EMITTER_LPF_CURVE 80u
#define EMITTER_CURVE_SCALER 92u
#define EMITTER_DOPPLER_SCALER 96u
/* X3DAUDIO_CONE (eight floats) */
#define CONE_INNER_ANGLE 0u
#define CONE_OUTER_ANGLE 4u
#define CONE_INNER_VOLUME 8u
#define CONE_OUTER_VOLUME 12u
#define CONE_INNER_LPF 16u
#define CONE_OUTER_LPF 20u
/* X3DAUDIO_DISTANCE_CURVE */
#define CURVE_POINTS 0u
#define CURVE_COUNT 4u
/* X3DAUDIO_DSP_SETTINGS (48 bytes, documented 1.7/2.7 layout) */
#define SET_MATRIX 0u
#define SET_DELAY_TIMES 4u
#define SET_SRC 8u
#define SET_DST 12u
#define SET_LPF_DIRECT 16u
#define SET_DOPPLER 28u
#define SET_EMITTER_ANGLE 32u
#define SET_DISTANCE 36u
#define SET_EMITTER_VEL 40u
#define SET_LISTENER_VEL 44u

typedef struct {
    uint32_t hdr;    /* guest WAVEHDR address */
    uint32_t data;   /* guest lpData */
    uint32_t frames; /* guest frames in this buffer */
    uint64_t start;  /* first guest frame index of this buffer */
} QueueHdr;

typedef struct {
    uint8_t used;
    uint16_t channels, bits, block_align;
    uint32_t rate;
    uint32_t step_q16; /* guest frames per output frame, Q16 */
    int16_t *ring;
    uint32_t cap, head, tail, count;
    QueueHdr qh[MAX_QUEUE];
    unsigned qh_head, qh_count;
    uint64_t guest_queued; /* guest frames appended by waveOutWrite */
    uint64_t guest_written; /* guest frames converted into the ring */
    uint64_t rs_pos_q16;    /* stream position on the guest timeline, Q16 */
    uint64_t out_written;   /* output frames written into the ring */
    uint64_t out_read;      /* output frames handed to the backend */
    uint64_t out_played;    /* output frames played according to the clock */
    uint64_t acc;           /* clock remainder, in microseconds * OUTPUT_HZ */
    uint64_t last_us;
    uint8_t paused;
    uint8_t stalled;
    float vol_l, vol_r;
} AudioDevice;

static AudioDevice devices[WIN32_AUDIO_HANDLE_COUNT];
static const Win32AudioBackend *backend;
static uint64_t (*now_us)(void);
static unsigned open_count;

/* ------------------------------------------------------------------ utils */

static void alog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[audio] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static unsigned lower(unsigned ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int icmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { ++a; ++b; }
    return (int)lower((unsigned char)*a) - (int)lower((unsigned char)*b);
}
static int icmp_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}
static int dll_match(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (!icmp(a, b)) return 1;
    size_t n = strlen(b);
    return n > 4 && strlen(a) == n - 4 && !icmp(b + n - 4, ".dll") && icmp_prefix(a, b, n - 4);
}

static AudioDevice *device_of(uint32_t h) {
    if (h < WIN32_AUDIO_HANDLE_BASE || h - WIN32_AUDIO_HANDLE_BASE >= WIN32_AUDIO_HANDLE_COUNT) return NULL;
    AudioDevice *d = &devices[h - WIN32_AUDIO_HANDLE_BASE];
    return d->used ? d : NULL;
}
static uint32_t handle_of(const AudioDevice *d) {
    return WIN32_AUDIO_HANDLE_BASE + (uint32_t)(d - devices);
}

/* ------------------------------------------------------------------ timing */

static uint64_t device_guest_played(const AudioDevice *d) {
    uint64_t played = d->out_played * (uint64_t)d->rate / WIN32_AUDIO_OUTPUT_HZ;
    return played > d->guest_written ? d->guest_written : played;
}

static void submit_available(AudioDevice *d) {
    if (!backend || !backend->submit) return;
    while (d->count) {
        unsigned contiguous = d->cap - d->tail;
        unsigned got;
        if (contiguous > d->count) contiguous = d->count;
        got = backend->submit(d->ring + (size_t)d->tail * 2u, contiguous);
        if (!got) break;
        if (got > d->count) got = d->count; /* defensive: never trust a sink */
        d->tail = (d->tail + got) % d->cap;
        d->count -= got;
        d->out_read += got;
    }
}

static int16_t guest_sample(const AudioDevice *d, const QueueHdr *q, uint32_t frame, unsigned ch) {
    uint32_t addr = q->data + frame * d->block_align + ch * (d->bits / 8u);
    if (d->bits == 16u) return (int16_t)(uint16_t)M16(addr);
    /* WAVE_FORMAT_PCM 8-bit is unsigned; the DSP wants signed. */
    return (int16_t)(((int)M8(addr) - 128) << 8);
}

/* Linear resampler over the concatenated guest timeline.  `rs_pos_q16` is a
 * single stream position, so the interpolation phase continues across
 * waveOutWrite buffers instead of restarting (a restart would deliver slightly
 * more frames than the device consumes, and the queue would drift).
 *
 * Documented limits: no anti-alias filter, so material above ~16.3 kHz aliases
 * when downsampling; at a buffer boundary the right-hand neighbour is the last
 * sample of the buffer (one-sample hold) instead of the next buffer's first. */
static QueueHdr *header_at(AudioDevice *d, uint64_t frame) {
    for (unsigned i = 0; i < d->qh_count; ++i) {
        QueueHdr *q = &d->qh[(d->qh_head + i) % MAX_QUEUE];
        if (frame >= q->start && frame < q->start + q->frames) return q;
    }
    return NULL;
}

static void pump(AudioDevice *d) {
    while (d->count < d->cap) {
        uint64_t idx = d->rs_pos_q16 >> 16;
        QueueHdr *q = header_at(d, idx);
        int32_t out[2];
        if (!q) break; /* the guest has not supplied that part of the timeline */
        {
            uint32_t local = (uint32_t)(idx - q->start);
            uint32_t frac = (uint32_t)(d->rs_pos_q16 & 0xFFFFu);
            for (unsigned ch = 0; ch < 2u; ++ch) {
                unsigned src = d->channels == 1u ? 0u : ch;
                int32_t s0 = guest_sample(d, q, local, src);
                int32_t s1 = local + 1u < q->frames ? guest_sample(d, q, local + 1u, src) : s0;
                int32_t delta = (int32_t)(((int64_t)(s1 - s0) * (int32_t)frac) >> 16);
                out[ch] = s0 + delta;
            }
        }
        {
            uint32_t at = (d->head + d->count) % d->cap;
            d->ring[(size_t)at * 2u] = (int16_t)out[0];
            d->ring[(size_t)at * 2u + 1u] = (int16_t)out[1];
            ++d->count;
            ++d->out_written;
        }
        d->rs_pos_q16 += d->step_q16;
        d->guest_written = d->rs_pos_q16 >> 16;
        if (d->guest_written > d->guest_queued) d->guest_written = d->guest_queued;
    }
}

static void complete_headers(AudioDevice *d) {
    uint64_t played = device_guest_played(d);
    while (d->qh_count) {
        QueueHdr *q = &d->qh[d->qh_head];
        if (q->start + q->frames > played) break;
        {
            uint32_t flags = M32(q->hdr + WHDR_FLAGS);
            flags &= ~WHDR_INQUEUE;
            flags |= WHDR_DONE;
            M32(q->hdr + WHDR_FLAGS) = flags;
        }
        d->qh_head = (d->qh_head + 1u) % MAX_QUEUE;
        --d->qh_count;
    }
}

void win32_audio_tick(void) {
    if (!backend || !now_us) return;
    {
        uint64_t now = now_us();
        for (unsigned i = 0; i < WIN32_AUDIO_HANDLE_COUNT; ++i) {
            AudioDevice *d = &devices[i];
            uint64_t elapsed;
            if (!d->used) continue;
            if (now <= d->last_us) { d->last_us = now; continue; }
            elapsed = now - d->last_us;
            d->last_us = now;
            pump(d);
            submit_available(d);
            if (!d->paused) {
                uint64_t want, avail, take;
                d->acc += elapsed * (uint64_t)WIN32_AUDIO_OUTPUT_HZ;
                /* One second of unpaid clock is a stalled backend, not a
                 * reason to jump the sample counter forward by minutes. */
                if (d->acc > 1000000ull * WIN32_AUDIO_OUTPUT_HZ) {
                    d->acc = 1000000ull * WIN32_AUDIO_OUTPUT_HZ;
                    if (!d->stalled) {
                        alog("playback stalled: backend accepted no frames for 1 s");
                        d->stalled = 1;
                    }
                } else {
                    d->stalled = 0;
                }
                want = d->acc / 1000000u;
                avail = d->out_read - d->out_played;
                take = want < avail ? want : avail;
                d->out_played += take;
                d->acc -= take * 1000000u;
            }
            complete_headers(d);
        }
    }
}

/* -------------------------------------------------------------- registry */

#define A(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)

static uint32_t read32(uint32_t addr) { return M32(addr); }
static void write32(uint32_t addr, uint32_t v) { M32(addr) = v; }
static float readf(uint32_t addr) { return MF32(addr); }
static void writef(uint32_t addr, float v) { MF32(addr) = v; }

/* --------------------------------------------------------------- waveOut */

typedef struct {
    uint16_t tag, channels, bits, block_align;
    uint32_t rate;
} Format;

/* Validate WAVEFORMATEX before the backend is consulted so that results do
 * not depend on whether hardware is present. */
static uint32_t check_format(uint32_t wfx, Format *out) {
    Format f;
    uint32_t avg;
    if (!wfx) { alog("waveOutOpen: E_INVALPARAM lpWaveFormat is NULL"); return MMSYSERR_INVALPARAM; }
    f.tag = (uint16_t)M16(wfx + WFX_TAG);
    f.channels = (uint16_t)M16(wfx + WFX_CHANNELS);
    f.rate = read32(wfx + WFX_RATE);
    avg = read32(wfx + WFX_AVGBYTES);
    f.block_align = (uint16_t)M16(wfx + WFX_ALIGN);
    f.bits = (uint16_t)M16(wfx + WFX_BITS);
    if (f.tag != WAVE_FORMAT_PCM) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED wFormatTag %u (PCM only)", (unsigned)f.tag);
        return MMSYSERR_NOTSUPPORTED;
    }
    if (f.channels < 1u || f.channels > 2u) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED nChannels %u (mono/stereo only)", (unsigned)f.channels);
        return MMSYSERR_NOTSUPPORTED;
    }
    if (f.bits != 8u && f.bits != 16u) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED wBitsPerSample %u (8/16 only)", (unsigned)f.bits);
        return MMSYSERR_NOTSUPPORTED;
    }
    if (f.rate < AUDIO_MIN_RATE || f.rate > AUDIO_MAX_RATE) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED nSamplesPerSec %lu (4000..192000)",
             (unsigned long)f.rate);
        return MMSYSERR_NOTSUPPORTED;
    }
    if (f.block_align != (uint16_t)(f.channels * (f.bits / 8u))) {
        alog("waveOutOpen: WAVERR_BADFORMAT nBlockAlign %u, expected %u",
             (unsigned)f.block_align, (unsigned)(f.channels * (f.bits / 8u)));
        return WAVERR_BADFORMAT;
    }
    if (avg != f.rate * (uint32_t)f.block_align) {
        alog("waveOutOpen: WAVERR_BADFORMAT nAvgBytesPerSec %lu, expected %lu",
             (unsigned long)avg, (unsigned long)(f.rate * (uint32_t)f.block_align));
        return WAVERR_BADFORMAT;
    }
    if ((uint16_t)M16(wfx + WFX_CBSIZE) != 0u) {
        alog("waveOutOpen: WAVERR_BADFORMAT cbSize %u for PCM (expected 0)",
             (unsigned)(uint16_t)M16(wfx + WFX_CBSIZE));
        return WAVERR_BADFORMAT;
    }
    if (out) *out = f;
    return MMSYSERR_NOERROR;
}

static void s_waveOutOpen(CPU *c) {
    uint32_t phwo = A(0), id = A(1), wfx = A(2), flags = A(5);
    Format f;
    uint32_t rc = check_format(wfx, &f);
    AudioDevice *d;
    win32_audio_tick();
    if (rc != MMSYSERR_NOERROR) RET(rc, 6);
    if (flags & (WAVE_MAPPED | WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE)) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED fdwOpen flag WAVE_MAPPED (device GUID selection)");
        RET(MMSYSERR_NOTSUPPORTED, 6);
    }
    if (flags & ~(WAVE_FORMAT_QUERY | WAVE_ALLOWSYNC | WAVE_FORMAT_DIRECT)) {
        alog("waveOutOpen: MMSYSERR_INVALFLAG fdwOpen 0x%08lx", (unsigned long)flags);
        RET(MMSYSERR_INVALFLAG, 6);
    }
    if (id != WAVE_MAPPER && id != 0u) {
        alog("waveOutOpen: MMSYSERR_BADDEVICEID %lu", (unsigned long)id);
        RET(MMSYSERR_BADDEVICEID, 6);
    }
    /* No sink means no device: a WAVE_FORMAT_QUERY must not answer "supported"
     * when there is nothing to play on. */
    if (!backend || !backend->submit) {
        alog("waveOutOpen: MMSYSERR_NODRIVER (no audio backend installed)");
        RET(MMSYSERR_NODRIVER, 6);
    }
    if (flags & WAVE_FORMAT_QUERY) RET(MMSYSERR_NOERROR, 6);
    if (!phwo) { alog("waveOutOpen: MMSYSERR_INVALPARAM phwo is NULL"); RET(MMSYSERR_INVALPARAM, 6); }
    if ((flags & CALLBACK_TYPEMASK) != CALLBACK_NULL &&
        (flags & CALLBACK_TYPEMASK) != CALLBACK_FUNCTION &&
        (flags & CALLBACK_TYPEMASK) != CALLBACK_EVENT) {
        alog("waveOutOpen: MMSYSERR_NOTSUPPORTED callback type 0x%lx",
             (unsigned long)(flags & CALLBACK_TYPEMASK));
        RET(MMSYSERR_NOTSUPPORTED, 6);
    }
    if (open_count) {
        /* One sink, no mixer: a second handle would have to share it. */
        alog("waveOutOpen: MMSYSERR_ALLOCATED (one waveOut device at a time)");
        RET(MMSYSERR_ALLOCATED, 6);
    }
    for (unsigned i = 0; i < WIN32_AUDIO_HANDLE_COUNT; ++i) {
        d = &devices[i];
        if (d->used) continue;
        memset(d, 0, sizeof *d);
        d->ring = (int16_t *)calloc((size_t)RING_FRAMES * 2u, sizeof(int16_t));
        if (!d->ring) { alog("waveOutOpen: MMSYSERR_NOMEM (%u frame ring)", RING_FRAMES); RET(MMSYSERR_NOMEM, 6); }
        d->used = 1;
        d->cap = RING_FRAMES;
        d->channels = f.channels;
        d->bits = f.bits;
        d->block_align = f.block_align;
        d->rate = f.rate;
        d->step_q16 = (uint32_t)(((uint64_t)f.rate << 16) / WIN32_AUDIO_OUTPUT_HZ);
        if (!d->step_q16) d->step_q16 = 1;
        d->vol_l = d->vol_r = 1.0f;
        d->last_us = now_us ? now_us() : 0;
        ++open_count;
        if (backend->set_volume) backend->set_volume(1.0f, 1.0f);
        write32(phwo, handle_of(d));
        if (f.rate != WIN32_AUDIO_OUTPUT_HZ)
            alog("resampling %lu -> %u", (unsigned long)f.rate, (unsigned)WIN32_AUDIO_OUTPUT_HZ);
        if ((flags & CALLBACK_TYPEMASK) != CALLBACK_NULL)
            alog("waveOutOpen: callback type 0x%lx accepted but never invoked; "
                 "completion is polled via WHDR_DONE/waveOutGetPosition (no audio thread)",
                 (unsigned long)(flags & CALLBACK_TYPEMASK));
        RET(MMSYSERR_NOERROR, 6);
    }
    alog("waveOutOpen: MMSYSERR_NOMEM (device table full)");
    RET(MMSYSERR_NOMEM, 6);
}

static void s_waveOutClose(CPU *c) {
    AudioDevice *d;
    win32_audio_tick();
    d = device_of(A(0));
    if (!d) { alog("waveOutClose: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 1); }
    if (d->qh_count) {
        alog("waveOutClose: WAVERR_STILLPLAYING (%u header(s) queued)", d->qh_count);
        RET(WAVERR_STILLPLAYING, 1);
    }
    if (backend && backend->flush) backend->flush();
    free(d->ring);
    memset(d, 0, sizeof *d);
    if (open_count) --open_count;
    RET(MMSYSERR_NOERROR, 1);
}

static void s_waveOutPrepareHeader(CPU *c) {
    uint32_t hwo = A(0), hdr = A(1), cb = A(2);
    AudioDevice *d;
    uint32_t flags;
    win32_audio_tick();
    d = device_of(hwo);
    if (!d) { alog("waveOutPrepareHeader: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)hwo); RET(MMSYSERR_INVALHANDLE, 3); }
    if (cb != WAVEHDR_SIZE) { alog("waveOutPrepareHeader: MMSYSERR_INVALPARAM cbwh %lu", (unsigned long)cb); RET(MMSYSERR_INVALPARAM, 3); }
    if (!hdr) { alog("waveOutPrepareHeader: MMSYSERR_INVALPARAM WAVEHDR is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    flags = read32(hdr + WHDR_FLAGS);
    if (!read32(hdr + WHDR_DATA)) { alog("waveOutPrepareHeader: MMSYSERR_INVALPARAM lpData is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    if (flags & WHDR_PREPARED) { alog("waveOutPrepareHeader: MMSYSERR_INVALPARAM header already prepared"); RET(MMSYSERR_INVALPARAM, 3); }
    write32(hdr + WHDR_FLAGS, (flags & ~(WHDR_DONE | WHDR_INQUEUE)) | WHDR_PREPARED);
    RET(MMSYSERR_NOERROR, 3);
}

static void s_waveOutUnprepareHeader(CPU *c) {
    uint32_t hwo = A(0), hdr = A(1), cb = A(2);
    AudioDevice *d;
    uint32_t flags;
    win32_audio_tick();
    d = device_of(hwo);
    if (!d) { alog("waveOutUnprepareHeader: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)hwo); RET(MMSYSERR_INVALHANDLE, 3); }
    if (cb != WAVEHDR_SIZE) { alog("waveOutUnprepareHeader: MMSYSERR_INVALPARAM cbwh %lu", (unsigned long)cb); RET(MMSYSERR_INVALPARAM, 3); }
    if (!hdr) { alog("waveOutUnprepareHeader: MMSYSERR_INVALPARAM WAVEHDR is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    flags = read32(hdr + WHDR_FLAGS);
    if (flags & WHDR_INQUEUE) { alog("waveOutUnprepareHeader: WAVERR_STILLPLAYING"); RET(WAVERR_STILLPLAYING, 3); }
    if (!(flags & WHDR_PREPARED)) { alog("waveOutUnprepareHeader: WAVERR_UNPREPARED"); RET(WAVERR_UNPREPARED, 3); }
    write32(hdr + WHDR_FLAGS, flags & ~(WHDR_PREPARED | WHDR_DONE | WHDR_INQUEUE));
    RET(MMSYSERR_NOERROR, 3);
}

static void s_waveOutWrite(CPU *c) {
    uint32_t hwo = A(0), hdr = A(1), cb = A(2);
    AudioDevice *d;
    uint32_t flags, data, bytes, loops, frames;
    QueueHdr *q;
    win32_audio_tick();
    d = device_of(hwo);
    if (!d) { alog("waveOutWrite: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)hwo); RET(MMSYSERR_INVALHANDLE, 3); }
    if (cb != WAVEHDR_SIZE) { alog("waveOutWrite: MMSYSERR_INVALPARAM cbwh %lu", (unsigned long)cb); RET(MMSYSERR_INVALPARAM, 3); }
    if (!hdr) { alog("waveOutWrite: MMSYSERR_INVALPARAM WAVEHDR is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    flags = read32(hdr + WHDR_FLAGS);
    data = read32(hdr + WHDR_DATA);
    bytes = read32(hdr + WHDR_LENGTH);
    loops = read32(hdr + WHDR_LOOPS);
    if (!(flags & WHDR_PREPARED)) { alog("waveOutWrite: WAVERR_UNPREPARED"); RET(WAVERR_UNPREPARED, 3); }
    if (flags & WHDR_INQUEUE) { alog("waveOutWrite: MMSYSERR_INVALPARAM header already in the queue"); RET(MMSYSERR_INVALPARAM, 3); }
    if (!data || !bytes) { alog("waveOutWrite: MMSYSERR_INVALPARAM lpData/dwBufferLength is zero"); RET(MMSYSERR_INVALPARAM, 3); }
    if (loops > 1u) {
        alog("waveOutWrite: MMSYSERR_NOTSUPPORTED dwLoops %lu (looping is not implemented)",
             (unsigned long)loops);
        RET(MMSYSERR_NOTSUPPORTED, 3);
    }
    if (d->qh_count >= MAX_QUEUE) {
        alog("waveOutWrite: MMSYSERR_NOMEM queue full (%u headers)", MAX_QUEUE);
        RET(MMSYSERR_NOMEM, 3);
    }
    if (bytes % d->block_align) {
        alog("waveOutWrite: dwBufferLength %lu is not a multiple of nBlockAlign %u; tail dropped",
             (unsigned long)bytes, (unsigned)d->block_align);
        bytes -= bytes % d->block_align;
        if (!bytes) { alog("waveOutWrite: MMSYSERR_INVALPARAM nothing left after alignment"); RET(MMSYSERR_INVALPARAM, 3); }
    }
    frames = bytes / d->block_align;
    if (!frames) { alog("waveOutWrite: MMSYSERR_INVALPARAM zero frames"); RET(MMSYSERR_INVALPARAM, 3); }

    q = &d->qh[(d->qh_head + d->qh_count) % MAX_QUEUE];
    memset(q, 0, sizeof *q);
    q->hdr = hdr;
    q->data = data;
    q->frames = frames;
    q->start = d->guest_queued;
    d->guest_queued += frames;
    ++d->qh_count;
    write32(hdr + WHDR_FLAGS, (flags | WHDR_INQUEUE) & ~WHDR_DONE);
    /* Start converting now; completion still waits for the clock. */
    pump(d);
    submit_available(d);
    RET(MMSYSERR_NOERROR, 3);
}

static void s_waveOutReset(CPU *c) {
    AudioDevice *d = device_of(A(0));
    win32_audio_tick();
    if (!d) { alog("waveOutReset: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 1); }
    while (d->qh_count) {
        QueueHdr *q = &d->qh[d->qh_head];
        write32(q->hdr + WHDR_FLAGS, (read32(q->hdr + WHDR_FLAGS) & ~WHDR_INQUEUE) | WHDR_DONE);
        d->qh_head = (d->qh_head + 1u) % MAX_QUEUE;
        --d->qh_count;
    }
    if (backend && backend->flush) backend->flush();
    d->head = d->tail = d->count = 0;
    d->guest_queued = d->guest_written = 0;
    d->rs_pos_q16 = 0;
    d->out_written = d->out_read = d->out_played = 0;
    d->acc = 0;
    d->last_us = now_us ? now_us() : 0;
    RET(MMSYSERR_NOERROR, 1);
}

static void s_waveOutPause(CPU *c) {
    AudioDevice *d;
    win32_audio_tick();
    d = device_of(A(0));
    if (!d) { alog("waveOutPause: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 1); }
    if (!d->paused) {
        d->paused = 1;
        d->last_us = now_us ? now_us() : 0;
        d->acc = 0;
        if (backend && backend->set_paused) backend->set_paused(1);
    }
    RET(MMSYSERR_NOERROR, 1);
}

static void s_waveOutRestart(CPU *c) {
    AudioDevice *d;
    win32_audio_tick();
    d = device_of(A(0));
    if (!d) { alog("waveOutRestart: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 1); }
    if (d->paused) {
        d->paused = 0;
        d->last_us = now_us ? now_us() : 0;
        d->acc = 0;
        if (backend && backend->set_paused) backend->set_paused(0);
    }
    RET(MMSYSERR_NOERROR, 1);
}

static void s_waveOutSetVolume(CPU *c) {
    AudioDevice *d = device_of(A(0));
    uint32_t vol = A(1);
    win32_audio_tick();
    if (!d) { alog("waveOutSetVolume: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 2); }
    d->vol_l = (float)(vol & 0xffffu) / 65535.0f;
    d->vol_r = (float)((vol >> 16) & 0xffffu) / 65535.0f;
    if (backend && backend->set_volume) backend->set_volume(d->vol_l, d->vol_r);
    RET(MMSYSERR_NOERROR, 2);
}

static void s_waveOutGetPosition(CPU *c) {
    AudioDevice *d;
    uint32_t mmt = A(1), cb = A(2), type;
    uint64_t frames;
    win32_audio_tick();
    d = device_of(A(0));
    if (!d) { alog("waveOutGetPosition: MMSYSERR_INVALHANDLE 0x%lx", (unsigned long)A(0)); RET(MMSYSERR_INVALHANDLE, 3); }
    if (!mmt) { alog("waveOutGetPosition: MMSYSERR_INVALPARAM pmmt is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    if (cb < 8u) { alog("waveOutGetPosition: MMSYSERR_INVALPARAM cbmmt %lu", (unsigned long)cb); RET(MMSYSERR_INVALPARAM, 3); }
    type = read32(mmt);
    frames = device_guest_played(d);
    switch (type) {
    case TIME_SAMPLES:
        write32(mmt + 4, (uint32_t)frames);
        RET(MMSYSERR_NOERROR, 3);
    case TIME_BYTES:
        write32(mmt + 4, (uint32_t)(frames * d->block_align));
        RET(MMSYSERR_NOERROR, 3);
    case TIME_MS:
        write32(mmt + 4, (uint32_t)(frames * 1000u / d->rate));
        RET(MMSYSERR_NOERROR, 3);
    case TIME_TICKS:
        /* No MIDI subsystem: ticks are reported as milliseconds. */
        write32(mmt + 4, (uint32_t)(frames * 1000u / d->rate));
        RET(MMSYSERR_NOERROR, 3);
    default:
        alog("waveOutGetPosition: MMSYSERR_NOTSUPPORTED wType 0x%lx; reporting TIME_BYTES",
             (unsigned long)type);
        write32(mmt, TIME_BYTES);
        write32(mmt + 4, (uint32_t)(frames * d->block_align));
        RET(MMSYSERR_NOTSUPPORTED, 3);
    }
}

static void s_waveOutGetNumDevs(CPU *c) {
    win32_audio_tick();
    RET(backend && backend->submit ? 1u : 0u, 0);
}

static void s_waveOutGetDevCaps(CPU *c) {
    uint32_t id = A(0), caps = A(1), cb = A(2);
    char name[32];
    win32_audio_tick();
    if (!backend || !backend->submit) { alog("waveOutGetDevCaps: MMSYSERR_NODRIVER"); RET(MMSYSERR_NODRIVER, 3); }
    if (id != WAVE_MAPPER && id != 0u) { alog("waveOutGetDevCaps: MMSYSERR_BADDEVICEID %lu", (unsigned long)id); RET(MMSYSERR_BADDEVICEID, 3); }
    if (!caps) { alog("waveOutGetDevCaps: MMSYSERR_INVALPARAM pwoc is NULL"); RET(MMSYSERR_INVALPARAM, 3); }
    if (cb < WAVEOUTCAPS_SIZE) { alog("waveOutGetDevCaps: MMSYSERR_INVALPARAM cbwoc %lu", (unsigned long)cb); RET(MMSYSERR_INVALPARAM, 3); }
    snprintf(name, sizeof name, "Skyrim N3DS %s", backend->name ? backend->name : "unknown");
    M16(caps + 0) = 0; /* wMid */
    M16(caps + 2) = 0; /* wPid */
    write32(caps + 4, 0x0100u); /* vDriverVersion 1.0 */
    memset(n3ds_guest_ptr(caps + 8, 32), 0, 32);
    memcpy(n3ds_guest_ptr(caps + 8, 32), name, sizeof name);
    write32(caps + 40, WAVE_FORMAT_1M08 | WAVE_FORMAT_1M16 | WAVE_FORMAT_1S08 | WAVE_FORMAT_1S16 |
                          WAVE_FORMAT_2M08 | WAVE_FORMAT_2M16 | WAVE_FORMAT_2S08 | WAVE_FORMAT_2S16 |
                          WAVE_FORMAT_4M08 | WAVE_FORMAT_4M16 | WAVE_FORMAT_4S08 | WAVE_FORMAT_4S16 |
                          WAVE_FORMAT_48M08 | WAVE_FORMAT_48M16 | WAVE_FORMAT_48S08 | WAVE_FORMAT_48S16);
    M16(caps + 44) = 2; /* wChannels */
    M16(caps + 46) = 0; /* wReserved1 */
    write32(caps + 48, WAVECAPS_VOLUME | WAVECAPS_LRVOLUME);
    RET(MMSYSERR_NOERROR, 3);
}

/* -------------------------------------------------------------- X3DAudio */

typedef struct { float x, y, z; } Vec3;

static Vec3 vec_read(uint32_t addr) {
    Vec3 v;
    v.x = readf(addr);
    v.y = readf(addr + 4);
    v.z = readf(addr + 8);
    return v;
}
static Vec3 vec_sub(Vec3 a, Vec3 b) { Vec3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static float vec_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float vec_len(Vec3 a) { return sqrtf(vec_dot(a, a)); }
static Vec3 vec_cross(Vec3 a, Vec3 b) {
    Vec3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    return r;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Refs are logged once per field: X3DAudioCalculate can run per voice per
 * frame, so a repeated E_NOTIMPL must not flood the log. */
#define MAX_X3D_FIELDS 24
static const char *x3d_fields[MAX_X3D_FIELDS];
static unsigned x3d_field_count;

static uint32_t x3d_refuse(const char *field, uint32_t code) {
    unsigned i;
    for (i = 0; i < x3d_field_count; ++i)
        if (!strcmp(x3d_fields[i], field)) return code;
    if (x3d_field_count < MAX_X3D_FIELDS) {
        x3d_fields[x3d_field_count++] = field;
        alog("X3DAudioCalculate: 0x%08lx for %s", (unsigned long)code, field);
    }
    return code;
}

/* Piecewise-linear X3DAUDIO_DISTANCE_CURVE over normalised distance t in
 * [0,1]; beyond the last point the last DSPSetting is used (documented). */
static float curve_value(uint32_t curve, float t) {
    uint32_t points = read32(curve + CURVE_POINTS);
    uint32_t count = read32(curve + CURVE_COUNT);
    if (!points || count < 2u) return 1.0f;
    if (!(t > 0.0f)) return readf(points + 4);
    for (uint32_t i = 1; i < count; ++i) {
        float d1 = readf(points + i * 8u);
        if (t <= d1) {
            float d0 = readf(points + (i - 1u) * 8u);
            float v0 = readf(points + (i - 1u) * 8u + 4u);
            float v1 = readf(points + i * 8u + 4u);
            if (d1 <= d0) return v1;
            return v0 + (v1 - v0) * ((t - d0) / (d1 - d0));
        }
    }
    return readf(points + (count - 1u) * 8u + 4u);
}

static void s_X3DAudioInitialize(CPU *c) {
    uint32_t mask = A(0), handle = A(2);
    float speed = readf(A(1));
    uint32_t bits;
    memcpy(&bits, &speed, sizeof bits);
    if (mask != SPEAKER_STEREO) {
        alog("X3DAudioInitialize: E_NOTIMPL SpeakerChannelMask 0x%08lx (stereo 0x3 only)",
             (unsigned long)mask);
        RET(E_NOTIMPL, 3);
    }
    if (!(speed > 0.0f) || speed > 1.0e6f) {
        alog("X3DAudioInitialize: E_INVALIDARG SpeedOfSound %f", (double)speed);
        RET(E_INVALIDARG, 3);
    }
    if (!handle) { alog("X3DAudioInitialize: E_INVALIDARG X3DInstance is NULL"); RET(E_INVALIDARG, 3); }
    write32(handle + 0, X3D_HANDLE_MAGIC);
    write32(handle + 4, mask);
    write32(handle + 8, bits);
    write32(handle + 12, 0u);
    write32(handle + 16, X3D_HANDLE_MAGIC ^ mask ^ bits);
    RET(0, 3);
}

static void s_X3DAudioCalculate(CPU *c) {
    uint32_t inst = A(0), lis = A(1), emi = A(2), flags = A(3), set = A(4);
    uint32_t checksum, src, dst, channels, volume_curve, lpf_curve, cone, azimuths;
    float scaler, norm, distance, volume, lpf, doppler_scaler;
    Vec3 lpos, lfront, ltop, lvel, epos, efront, evel, dir, to_listener, to_emitter;
    win32_audio_tick();

    if (!inst) { alog("X3DAudioCalculate: E_INVALIDARG X3DInstance is NULL"); RET(E_INVALIDARG, 5); }
    if (!lis || !emi || !set) {
        alog("X3DAudioCalculate: E_INVALIDARG %s is NULL", !lis ? "pListener" : !emi ? "pEmitter" : "pDSPSettings");
        RET(E_INVALIDARG, 5);
    }
    if (read32(inst + 0) != X3D_HANDLE_MAGIC) {
        alog("X3DAudioCalculate: E_INVALIDARG X3DInstance was not produced by X3DAudioInitialize");
        RET(E_INVALIDARG, 5);
    }
    checksum = read32(inst + 0) ^ read32(inst + 4) ^ read32(inst + 8);
    if (read32(inst + 16) != checksum) {
        alog("X3DAudioCalculate: E_INVALIDARG X3DInstance handle is corrupt");
        RET(E_INVALIDARG, 5);
    }
    if (read32(inst + 4) != SPEAKER_STEREO) {
        RET(x3d_refuse("SpeakerChannelMask", E_NOTIMPL), 5);
    }
    if (flags & X3DC_DELAY) RET(x3d_refuse("X3DAUDIO_CALCULATE_DELAY (pDelayTimes)", E_NOTIMPL), 5);
    if (flags & X3DC_LPF_REVERB) RET(x3d_refuse("X3DAUDIO_CALCULATE_LPF_REVERB (LPFReverbCoefficient)", E_NOTIMPL), 5);
    if (flags & X3DC_REVERB) RET(x3d_refuse("X3DAUDIO_CALCULATE_REVERB (ReverbLevel)", E_NOTIMPL), 5);
    if (flags & X3DC_ZEROCENTER) RET(x3d_refuse("X3DAUDIO_CALCULATE_ZEROCENTER", E_NOTIMPL), 5);
    if (flags & X3DC_REDIRECT_TO_LFE) RET(x3d_refuse("X3DAUDIO_CALCULATE_REDIRECT_TO_LFE", E_NOTIMPL), 5);
    if (flags & ~(X3DC_MATRIX | X3DC_LPF_DIRECT | X3DC_DOPPLER | X3DC_EMITTER_ANGLE)) {
        alog("X3DAudioCalculate: E_NOTIMPL unknown Flags 0x%08lx", (unsigned long)flags);
        RET(E_NOTIMPL, 5);
    }

    src = read32(set + SET_SRC);
    dst = read32(set + SET_DST);
    if (dst != 2u) {
        alog("X3DAudioCalculate: E_INVALIDARG DstChannelCount %lu (stereo destination only)",
             (unsigned long)dst);
        RET(E_INVALIDARG, 5);
    }
    if (src != 1u && src != 2u) {
        alog("X3DAudioCalculate: E_NOTIMPL SrcChannelCount %lu (1 or 2)", (unsigned long)src);
        RET(E_NOTIMPL, 5);
    }
    channels = read32(emi + EMITTER_CHANNELS);
    if (channels != src) {
        alog("X3DAudioCalculate: E_INVALIDARG ChannelCount %lu != SrcChannelCount %lu",
             (unsigned long)channels, (unsigned long)src);
        RET(E_INVALIDARG, 5);
    }
    azimuths = read32(emi + EMITTER_AZIMUTHS);
    if (src > 1u && azimuths) RET(x3d_refuse("pChannelAzimuths (multi-point emitter)", E_NOTIMPL), 5);
    if (readf(emi + EMITTER_INNER_RADIUS) > 0.0f) RET(x3d_refuse("InnerRadius", E_NOTIMPL), 5);
    if (readf(emi + EMITTER_INNER_ANGLE) > 0.0f) RET(x3d_refuse("InnerRadiusAngle", E_NOTIMPL), 5);
    if ((flags & X3DC_MATRIX) && !read32(set + SET_MATRIX)) {
        alog("X3DAudioCalculate: E_INVALIDARG pMatrixCoefficients is NULL with X3DAUDIO_CALCULATE_MATRIX");
        RET(E_INVALIDARG, 5);
    }

    lpos = vec_read(lis + LISTENER_POS);
    lfront = vec_read(lis + LISTENER_FRONT);
    ltop = vec_read(lis + LISTENER_TOP);
    lvel = vec_read(lis + LISTENER_VEL);
    epos = vec_read(emi + EMITTER_POS);
    efront = vec_read(emi + EMITTER_FRONT);
    evel = vec_read(emi + EMITTER_VEL);

    dir = vec_sub(epos, lpos);
    distance = vec_len(dir);
    if (distance > 1.0e-6f) {
        to_emitter.x = dir.x / distance;
        to_emitter.y = dir.y / distance;
        to_emitter.z = dir.z / distance;
        to_listener.x = -to_emitter.x;
        to_listener.y = -to_emitter.y;
        to_listener.z = -to_emitter.z;
    } else {
        to_listener.x = 0.0f;
        to_listener.y = 0.0f;
        to_listener.z = 1.0f;
        to_emitter.x = 0.0f;
        to_emitter.y = 0.0f;
        to_emitter.z = 1.0f;
        dir = to_emitter;
    }

    scaler = readf(emi + EMITTER_CURVE_SCALER);
    if (!(scaler > 0.0f)) scaler = 1.0f;
    norm = distance / scaler;

    volume_curve = read32(emi + EMITTER_VOLUME_CURVE);
    if (volume_curve) {
        volume = curve_value(volume_curve, clampf(norm, 0.0f, 1.0f));
    } else {
        volume = norm <= 1.0f ? 1.0f : 1.0f / norm;
    }
    lpf_curve = read32(emi + EMITTER_LPF_CURVE);
    if (lpf_curve) {
        lpf = curve_value(lpf_curve, clampf(norm, 0.0f, 1.0f));
    } else {
        /* Documented default: [0,1] -> [1,0.75]. */
        lpf = 1.0f - 0.25f * clampf(norm, 0.0f, 1.0f);
    }

    /* Cone: InnerAngle/OuterAngle are full cone widths in radians (2*pi means
     * omnidirectional).  Only single-channel emitters use a cone. */
    cone = read32(emi + EMITTER_CONE);
    if (cone && src == 1u) {
        float front_len = vec_len(efront);
        float angle;
        if (front_len > 1.0e-6f) {
            float c = clampf(vec_dot(efront, to_listener) / front_len, -1.0f, 1.0f);
            angle = acosf(c);
        } else {
            angle = 0.0f;
        }
        {
            float inner_angle = readf(cone + CONE_INNER_ANGLE) * 0.5f;
            float outer_angle = readf(cone + CONE_OUTER_ANGLE) * 0.5f;
            float t;
            if (outer_angle <= inner_angle) t = angle <= inner_angle ? 0.0f : 1.0f;
            else t = clampf((angle - inner_angle) / (outer_angle - inner_angle), 0.0f, 1.0f);
            volume *= readf(cone + CONE_INNER_VOLUME) +
                      (readf(cone + CONE_OUTER_VOLUME) - readf(cone + CONE_INNER_VOLUME)) * t;
            lpf *= readf(cone + CONE_INNER_LPF) +
                   (readf(cone + CONE_OUTER_LPF) - readf(cone + CONE_INNER_LPF)) * t;
        }
    }

    if (flags & X3DC_MATRIX) {
        uint32_t matrix = read32(set + SET_MATRIX);
        if (src == 1u) {
            Vec3 right = vec_cross(ltop, lfront);
            float right_len = vec_len(right);
            float pan, theta;
            if (right_len > 1.0e-6f) {
                right.x /= right_len;
                right.y /= right_len;
                right.z /= right_len;
            } else {
                right.x = 1.0f;
                right.y = 0.0f;
                right.z = 0.0f;
            }
            pan = clampf(vec_dot(to_emitter, right), -1.0f, 1.0f);
            /* Constant-power pan.  Elevation is deliberately not folded in. */
            theta = (pan + 1.0f) * 3.14159265f * 0.25f;
            writef(matrix + 0u, cosf(theta) * volume); /* dst L, src */
            writef(matrix + 4u, sinf(theta) * volume); /* dst R, src */
        } else {
            /* Stereo source: straight through with distance attenuation. */
            writef(matrix + 0u, volume);
            writef(matrix + 4u, 0.0f);
            writef(matrix + 8u, 0.0f);
            writef(matrix + 12u, volume);
        }
    }

    if (flags & X3DC_LPF_DIRECT) writef(set + SET_LPF_DIRECT, clampf(lpf, 0.0f, 1.0f));

    if (flags & X3DC_DOPPLER) {
        uint32_t speed_bits = read32(inst + 8);
        float speed;
        memcpy(&speed, &speed_bits, sizeof speed);
        doppler_scaler = readf(emi + EMITTER_DOPPLER_SCALER);
        if (!(doppler_scaler > 0.0f)) doppler_scaler = 1.0f;
        {
            float c0 = speed * doppler_scaler;
            float v_emitter = vec_dot(evel, to_listener);
            float v_listener = -vec_dot(lvel, to_listener);
            float factor = (c0 + v_listener) / (c0 - v_emitter);
            if (!(factor == factor) || factor < 0.05f) factor = 0.05f;
            if (factor > 20.0f) factor = 20.0f;
            writef(set + SET_DOPPLER, factor);
            writef(set + SET_EMITTER_VEL, v_emitter);
            writef(set + SET_LISTENER_VEL, v_listener);
        }
    }

    if (flags & X3DC_EMITTER_ANGLE) {
        float front_len = vec_len(efront);
        float angle = 0.0f;
        if (front_len > 1.0e-6f)
            angle = acosf(clampf(vec_dot(efront, to_listener) / front_len, -1.0f, 1.0f));
        writef(set + SET_EMITTER_ANGLE, angle);
    }
    writef(set + SET_DISTANCE, distance);
    RET(0, 5);
}

/* --------------------------------------------------------------- exports */

#define DEF(N) { "winmm.dll", #N, s_##N },
#define XDEF(N) { "X3DAudio1_7.dll", #N, s_##N },

static const Win32AudioDef defs[] = {
    DEF(waveOutOpen) DEF(waveOutClose)
    DEF(waveOutPrepareHeader) DEF(waveOutUnprepareHeader) DEF(waveOutWrite)
    DEF(waveOutReset) DEF(waveOutPause) DEF(waveOutRestart)
    DEF(waveOutSetVolume) DEF(waveOutGetPosition)
    DEF(waveOutGetNumDevs) DEF(waveOutGetDevCaps)
    XDEF(X3DAudioInitialize) XDEF(X3DAudioCalculate)
};
#undef DEF
#undef XDEF
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32AudioDef *win32_audio_defs(unsigned *count) {
    if (count) *count = (unsigned)NDEFS;
    return defs;
}

uint32_t win32_audio_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (dll_match(dll, defs[i].dll) && !strcmp(name, defs[i].name))
            return WIN32_AUDIO_BASE + 4 * i;
    return 0;
}

static FnEntry entries[NDEFS];
const FnEntry *win32_audio_lookup(uint32_t addr) {
    if (addr < WIN32_AUDIO_BASE || (addr & 3)) return NULL;
    {
        unsigned i = (addr - WIN32_AUDIO_BASE) / 4;
        if (i >= NDEFS) return NULL;
        entries[i] = (FnEntry){addr, defs[i].fn};
        return &entries[i];
    }
}

int win32_audio_install(const Win32AudioBackend *sink, uint64_t (*clock_us)(void)) {
    if (!sink || !sink->submit || !clock_us) return -1;
    if (open_count) return -1;
    backend = sink;
    now_us = clock_us;
    alog("backend \"%s\" installed, output %u Hz stereo s16", sink->name ? sink->name : "?",
         (unsigned)WIN32_AUDIO_OUTPUT_HZ);
    return 0;
}

void win32_audio_uninstall(void) {
    if (backend && backend->flush && open_count) backend->flush();
    for (unsigned i = 0; i < WIN32_AUDIO_HANDLE_COUNT; ++i) {
        AudioDevice *d = &devices[i];
        if (!d->used) continue;
        free(d->ring);
        memset(d, 0, sizeof *d);
    }
    open_count = 0;
    x3d_field_count = 0;
    backend = NULL;
    now_us = NULL;
}

unsigned win32_audio_device_count(void) { return open_count; }

int64_t win32_audio_position_frames(unsigned index) {
    if (index >= WIN32_AUDIO_HANDLE_COUNT) return -1;
    if (!devices[index].used) return -1;
    win32_audio_tick();
    return (int64_t)device_guest_played(&devices[index]);
}
