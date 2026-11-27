/*
 * PICA-SR configuration and geometry helpers for the Citro3D D3D9 backend.
 *
 * This header is deliberately free of 3DS/citro3d includes: it is compiled on
 * the host by tests/n3ds_sr_smoke.c, so the environment parsing, the size
 * arithmetic and the pass/UV planning are verifiable without a GPU.
 *
 * The kernels themselves live in third_party/pica_sr (libFSR3DS v0.3).
 */
#pragma once

#include <stddef.h>

#define N3DS_SR_LIB_VERSION "0.3"

typedef enum
{
    N3DS_SR_OFF = 0,   /* direct screen rendering, previous behaviour */
    N3DS_SR_NEAREST,   /* one point-sampled blit */
    N3DS_SR_BILINEAR,  /* one linear blit */
    N3DS_SR_TEV2D,     /* two LITE passes: H then V */
    N3DS_SR_TEV2DQ     /* four passes: LOW/SHARP per axis (libFSR3DS 2DQ) */
} N3dsSrMode;

typedef enum
{
    N3DS_SR_OP_COPY = 0, /* REPLACE(T0); filter chosen by the caller */
    N3DS_SR_OP_LITE,     /* FSR3DS_KERNEL_LITE */
    N3DS_SR_OP_UP_LOW,   /* FSR3DS_KERNEL_UP_LOW */
    N3DS_SR_OP_UP_SHARP  /* FSR3DS_KERNEL_UP_SHARP */
} N3dsSrOp;

typedef struct
{
    unsigned mode;      /* N3dsSrMode */
    unsigned scale_pct; /* 50..100, percent of the 400x240 back buffer */
    unsigned drs;       /* 0/1: dynamic resolution scaling inside [50,scale] */
} N3dsSrConfig;

/* POT storage plan for the SR scene target.  The scene colour texture and the
 * scratch colour texture are RGBA8; the scene target's depth attachment is
 * D16 (2 bytes/px) because the guest never tests stencil into the SR target.
 * scratch_bytes is 0 for single-pass modes (nearest/bilinear), which do not
 * allocate a scratch target at all. */
typedef struct
{
    unsigned w, h;          /* scaled content, portrait storage orientation */
    unsigned tex_w, tex_h;  /* POT storage actually allocated */
    unsigned scene_bytes;   /* RGBA8 colour storage */
    unsigned depth_bytes;   /* D16 depth attachment */
    unsigned scratch_bytes; /* RGBA8 ping-pong target, 0 when passes <= 1 */
    unsigned total_bytes;   /* scene + depth + scratch */
} N3dsSrStorage;

typedef struct
{
    unsigned op;   /* N3dsSrOp */
    float du, dv;  /* tap offsets in UV units (POT texture space) */
} N3dsSrPass;

/* Fitted TEV 2DQ constants (see third_party/pica_sr/fsr3ds.h). */
#define N3DS_SR_2DQ_LOW_GAIN255   115
#define N3DS_SR_2DQ_SHARP_GAIN255 255
#define N3DS_SR_2DQ_LOW_RADIUS     0.800f
#define N3DS_SR_2DQ_SHARP_RADIUS   1.000f

/* Defaults: N3DS_SR=off, N3DS_SR_SCALE=0.53, N3DS_SR_DRS=0.  The scale is the
 * POT-fit optimum for a 240x400 portrait target (see n3ds_sr_pick_scale). */
void n3ds_sr_config_defaults(N3dsSrConfig *cfg);

/* "off" | "nearest" | "bilinear" | "tev2d" | "tev2dq" -> N3dsSrMode,
 * -1 for an unknown name.  Case-insensitive, surrounding blanks ignored. */
int n3ds_sr_mode_from_name(const char *name);
const char *n3ds_sr_mode_name(unsigned mode);

/* "0.8", "80", "0.65" -> 65.  Accepts 0.5..1.0 (percent 50..100).
 * "off"/"1" also work: 1 -> 100.  Returns 0 on success, -1 on bad input. */
int n3ds_sr_parse_scale(const char *text, unsigned *pct);

/* key = "mode" | "scale" | "drs" (case-insensitive).  0 ok, -1 unknown key,
 * -2 bad value.  A NULL/empty value is an error, so a typo never silently
 * leaves the previous value. */
int n3ds_sr_config_key(N3dsSrConfig *cfg, const char *key, const char *value);

/* One "key=value" line (or a bare mode name).  Blank lines and '#' comments
 * are ignored.  0 ok (including ignored lines), -1 malformed, -2 bad value. */
int n3ds_sr_config_line(N3dsSrConfig *cfg, const char *line);

/* round(base * pct / 100), minimum 1. */
unsigned n3ds_sr_scaled(unsigned base, unsigned pct);

/* Smallest power of two >= n, clamped to [8, 1024] (PICA texture limit). */
unsigned n3ds_sr_pow2(unsigned n);

/* POT storage plan for a scaled 240x400-style target.  `passes` is the plan
 * length from n3ds_sr_plan (1 -> no scratch, >1 -> one ping-pong scratch).
 * All values are bytes; dimensions are clamped by n3ds_sr_pow2. */
void n3ds_sr_storage_size(unsigned store_w, unsigned store_h, unsigned pct,
                          unsigned passes, N3dsSrStorage *out);

/* Pick the scale in [50,100] that wastes the fewest POT texels for
 * store_w x store_h, i.e. maximises (scaled_w*scaled_h)/(tex_w*tex_h).
 * Ties keep the lower percentage (cheaper memory).  For 240x400 this is 53,
 * which fills 128x256 with 127x212 content (82% of the storage). */
unsigned n3ds_sr_pick_scale(unsigned store_w, unsigned store_h);

/* DRS ladder: index 0 == scale_pct, each next step 8 points lower, floor 50.
 * Returns 1 when DRS is off. */
unsigned n3ds_sr_drs_steps(const N3dsSrConfig *cfg);
unsigned n3ds_sr_drs_scale_pct(const N3dsSrConfig *cfg, unsigned index);

/* Scene target storage size for a scale.  The arguments are the LCD storage
 * dimensions (240x400 for the top screen: the framebuffer is portrait, the
 * scan-out rotates it), so the scaled target keeps the *same* orientation as
 * the real framebuffer and the guest clip matrices stay untouched. */
void n3ds_sr_scene_size(unsigned store_w, unsigned store_h, unsigned pct,
                        unsigned *w, unsigned *h);

/* Source rectangle of the POT texture that holds the rendered scene.  The
 * content sits at the PICA origin (u/v 0), so only the upper corner shrinks
 * with the scale: u1 = w/tw, v1 = h/th. */
typedef struct
{
    float u0, v0, u1, v1;
} N3dsSrRect;
void n3ds_sr_uv_region(unsigned tex_w, unsigned tex_h, unsigned w, unsigned h,
                       N3dsSrRect *rect);

/* Build the post chain for one frame.
 *   tex_w/tex_h   POT storage of the scene texture (tap spacing)
 * Returns the number of passes (0 for off), or -1 when the mode is unknown or
 * the buffer cannot hold the chain.  The caller renders pass i into an
 * alternating scratch/scene target and the last pass onto the screen. */
int n3ds_sr_plan(unsigned mode, unsigned tex_w, unsigned tex_h,
                 N3dsSrPass *passes, unsigned max_passes);

/* "N3DS_SR=tev2dq N3DS_SR_SCALE=0.8 N3DS_SR_DRS=1" one-line summary used by
 * the named startup log.  Always NUL-terminates; returns strlen(). */
unsigned n3ds_sr_describe(const N3dsSrConfig *cfg, char *out, size_t cap);
