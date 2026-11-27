/*
 * libFSR3DS v0.3 - public post-processing API.
 *
 * The library only depends on Citro3D/PICA200: no geometry, textures, input
 * handling or UI of the FurMark application.  The application owns the scene
 * targets and passes the source texture/dimensions to these helpers.
 *
 * All functions configure the fixed-function TEV pipeline; there are no
 * fragment/compute shaders on the PICA200.
 */
#pragma once

#include <3ds.h>
#include <citro3d.h>

#define FSR3DS_VERSION "0.3"

typedef enum
{
	FSR3DS_KERNEL_REPLACE = 0,   /* pass-through */
	FSR3DS_KERNEL_LITE,          /* v0.1 3-tap unsharp (H or V by UV offset) */
	FSR3DS_KERNEL_ADAPT_H,       /* 2C-B + edge weight in alpha (H pass) */
	FSR3DS_KERNEL_ADAPT_V,       /* weighted lerp(2C-B, C, weight) */
	FSR3DS_KERNEL_AA_BLUR,       /* 3-tap blur with constant strength */
	FSR3DS_KERNEL_AA_EDGE,       /* blur + edge weight in alpha (H pass) */
	FSR3DS_KERNEL_AA_APPLY,      /* weighted lerp(blur, C, weight) */
	FSR3DS_KERNEL_AA_EDGE_APPLY, /* single-pass adaptive blur (5 stages) */
	/* TEV 2DQ cascade (see docs/TEV_FILTERS.md, stage F) */
	FSR3DS_KERNEL_UP_LOW,        /* pass A: out = C - |g|*(C-B), weight k in alpha */
	FSR3DS_KERNEL_UP_SHARP,      /* pass B: out = C + g*(C-B), no gate (fx-capable) */
	FSR3DS_KERNEL_UP_SHARP_GATE, /* pass B + lerp toward C by T0.alpha */
} Fsr3dsKernel;

/* TEV 2DQ cascade defaults, fitted offline (tools/kernelfit/fit_upscale.py).
 * Per-axis response (d in source texels, taps via the UV offsets):
 *   H(f) = sinc^2(f) * (1 - |g1|*(1-cos(2*pi*f*d1)))
 *                     * (1 + g2*(1-cos(2*pi*f*d2)))
 * |g1| suppresses the source-Nyquist leak (zipper/crawl), g2 restores the
 * mid-band droop of the bilinear stretch.  Gains are passed to
 * fsr3dsTevKernel() as strength255; the radii are applied by the caller
 * as texel offsets.  The sharpness knob scales both radii around the
 * default (knob 0.75 == 1.0x).
 * Image-domain validation at 200x120 and 267x160 vs a Lanczos-3
 * reference: zone plate +4.6 dB, thin lines +2.8 dB, checker +3.0 dB
 * over a plain bilinear stretch, with halo overshoot roughly halved
 * compared to the v0.1 LITE kernel.  The contrast gate (UP_SHARP_GATE)
 * is NOT used by the default pipeline: it costs ~7 dB on broadband
 * content; the cascade's own notch-before-boost limits halos already. */
#define FSR3DS_2DQ_LOW_GAIN255    115   /* |g1| = 0.451 */
#define FSR3DS_2DQ_LOW_RADIUS     0.800f
#define FSR3DS_2DQ_SHARP_GAIN255  255   /* g2 = 1.000 */
#define FSR3DS_2DQ_SHARP_RADIUS   1.000f

/* TEV constant colour register order: R in bits 0-7. */
uint32_t fsr3dsColor(int r, int g, int b, int a);

/* Reset all six TEV stages to REPLACE(PREVIOUS), scale 1, no buffer update. */
void fsr3dsTevReset(void);

/* Scene/UI pass-through: stage 0 MODULATE(texture0, vertex colour). */
void fsr3dsTevModulate(void);

/* Configure a complete kernel. strength255 is used by the AA kernels
 * (0 = keep centre, 255 = full blur). */
void fsr3dsTevKernel(Fsr3dsKernel kernel, int strength255);

/* Append the effects stage (stage 4): previous * vertex colour.
 * Vertex colours carry the vignette and colour grade. */
void fsr3dsTevFx(void);

/* Dynamic-resolution controller.  frameMs is a smoothed frame time.
 * Returns -1/0/+1 resolution-index delta and updates *resIndex. */
int fsr3dsDrsUpdate(double frameMs, int *resIndex, int resCount, int targetFps, double dt);

const char *fsr3dsVersion(void);
