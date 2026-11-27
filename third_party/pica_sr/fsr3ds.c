/*
 * libFSR3DS v0.3 - PICA200 TEV post-processing kernels.
 */
#include "fsr3ds.h"

#include <math.h>

uint32_t fsr3dsColor(int r, int g, int b, int a)
{
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	if (a < 0) a = 0;
	if (a > 255) a = 255;
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

void fsr3dsTevReset(void)
{
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	C3D_TexEnvBufColor(0xFFFFFFFF);
}

static void kernel_replace(void)
{
	C3D_TexEnv *e = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
}

void fsr3dsTevModulate(void)
{
	fsr3dsTevReset();
	C3D_TexEnv *e = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
}

/*
 * v0.1 3-sample kernel:
 *   B = (L+R)/2, D = C-B+0.5, out = C+D-0.5 = 2C-B
 */
static void kernel_lite(void)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);
}

/* Horizontal pass: RGB = 2C-B, Alpha = 4*P*(1-P) = 1 - 4*(C-B)^2 */
static void kernel_adapt_h(void)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, 0);
	C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvOpAlpha(e2, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvScale(e2, C3D_Alpha, GPU_TEVSCALE_4);
	C3D_TexEnvColor(e2, 0xFFFFFFFF);
}

/* Vertical pass: out = lerp(2C-B, C, weight in T0.alpha) */
static void kernel_adapt_v(void)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, 0);
	C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e3 = C3D_GetTexEnv(3);
	C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_TEXTURE0);
	C3D_TexEnvOpRgb(e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(e3, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e3, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e3, 0xFFFFFFFF);
}

/*
 * Simple 3-tap blur along the axis selected by the UV offsets:
 *   B = (L+R)/2, blur = (C+B)/2, out = lerp(C, blur, strength)
 */
static void kernel_aa_blur(int strength255)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvColor(e1, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_CONSTANT);
	C3D_TexEnvColor(e2, fsr3dsColor(255, 255, 255, strength255));
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);
}

/* Edge-aware AA, horizontal: RGB = blur, Alpha = 1-4*(C-B)^2 */
static void kernel_aa_edge(void)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	/* B2 in RGB, edge weight in Alpha */
	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e2, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvOpAlpha(e2, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvScale(e2, C3D_Alpha, GPU_TEVSCALE_4);

	/* blur = (C + B2)/2, keep edge weight in Alpha */
	C3D_TexEnv *e3 = C3D_GetTexEnv(3);
	C3D_TexEnvSrc(e3, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvColor(e3, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e3, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e3, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);
}

/* Second pass of the edge-aware AA: use weight from T0.alpha */
static void kernel_aa_apply(void)
{
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvColor(e1, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_TEXTURE0);
	C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e2, 0xFFFFFFFF);
}

/* Single-pass adaptive AA: blur * weight + C * (1-weight) */
static void kernel_aa_edge_apply(void)
{
	kernel_aa_edge();

	C3D_TexEnv *e4 = C3D_GetTexEnv(4);
	C3D_TexEnvSrc(e4, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e4, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(e4, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e4, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e4, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e4, 0xFFFFFFFF);
}

/* ------------------------------------------------------------------------- */
/* TEV 2DQ cascade (stage F)                                                 */
/*                                                                           */
/* Semantics (verified against the PICA200 TEV): MODULATE = a*b,             */
/* ADD_SIGNED = a + b - 0.5 (two sources), INTERPOLATE = b*(1-c) + a*c.      */
/* Every stage output is clamped to [0,1].                                   */
/* ------------------------------------------------------------------------- */

/*
 * Cascade pass A ("low"): out = C - |g|*(C - B), |g| = gain255/255,
 * plus the edge weight k = 4*P*(1-P) in alpha (P = red of D).
 *
 *   s0  B  = (L + R)/2                      INTERPOLATE T1, T2, const .5
 *   s1  D  = C - B + 0.5                    ADD_SIGNED T0, 1-PREV
 *   s2  M  = |g|*D                          MODULATE PREV, const |g|
 *       k  = 4*D_r*(1-D_r)                  alpha combiner, red of PREV
 *   s3  X  = 0.5 + 0.5*|g| - M              ADD_SIGNED 1-PREV, const 1-.5|g|
 *   s4  out= C + X - 0.5 = C - |g|*(C-B)    ADD_SIGNED T0, PREV
 *
 * If the red-channel alpha read (s2) is unsupported the weight degrades
 * to 1.0 and the gated sharp pass below behaves as if ungated.
 */
static void kernel_up_low(int gain255)
{
	float g = (float)(gain255 & 0xFF) / 255.0f;
	int gm = (int)(g * 255.0f + 0.5f);
	int c3 = (int)((1.0f - 0.5f * g) * 255.0f + 0.5f);

	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	/* M = |g|*D in RGB, k = 4*D_r*(1-D_r) in alpha */
	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvOpAlpha(e2, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvScale(e2, C3D_Alpha, GPU_TEVSCALE_4);
	C3D_TexEnvColor(e2, fsr3dsColor(gm, gm, gm, 255));

	/* X = (1 - M) + const - 0.5 = 0.5 + 0.5|g| - M, always inside [0,1] */
	C3D_TexEnv *e3 = C3D_GetTexEnv(3);
	C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
	C3D_TexEnvOpRgb(e3, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e3, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvColor(e3, fsr3dsColor(c3, c3, c3, 255));
	C3D_TexEnvSrc(e3, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);

	/* out = C + X - 0.5 = C - |g|*(C-B) */
	C3D_TexEnv *e4 = C3D_GetTexEnv(4);
	C3D_TexEnvSrc(e4, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvFunc(e4, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e4, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e4, C3D_Alpha, GPU_REPLACE);
}

/*
 * Cascade pass B ("sharp"): out = C + g*(C - B), g = gain255/255,
 * optionally gated toward the input centre by T0.alpha.
 *
 *   s0  B  = (L + R)/2
 *   s1  D  = C - B + 0.5
 *   s2  M  = g*D                          MODULATE PREV, const g
 *   s3  X  = M + 0.5 - 0.5*g              ADD_SIGNED PREV, 1-const(.5+.5g)
 *   s4  F  = C + X - 0.5 = C + g*(C-B)    ADD_SIGNED T0, PREV
 *   s5  out= lerp(C, F, T0.a)              INTERPOLATE PREV, T0, T0.a
 */
static void kernel_up_sharp(int gain255, bool gate)
{
	float g = (float)(gain255 & 0xFF) / 255.0f;
	int gm = (int)(g * 255.0f + 0.5f);
	int c2 = (int)((0.5f + 0.5f * g) * 255.0f + 0.5f);

	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, GPU_CONSTANT);
	C3D_TexEnvColor(e0, fsr3dsColor(128, 128, 128, 128));
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv *e1 = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);

	/* M = g*D */
	C3D_TexEnv *e2 = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvColor(e2, fsr3dsColor(gm, gm, gm, 255));
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);

	/* X = M + (1 - const) - 0.5 = M + 0.5 - 0.5g, always inside [0,1] */
	C3D_TexEnv *e3 = C3D_GetTexEnv(3);
	C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
	C3D_TexEnvOpRgb(e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e3, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvColor(e3, fsr3dsColor(c2, c2, c2, 255));
	C3D_TexEnvSrc(e3, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);

	/* F = C + X - 0.5 = C + g*(C-B) */
	C3D_TexEnv *e4 = C3D_GetTexEnv(4);
	C3D_TexEnvSrc(e4, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
	C3D_TexEnvFunc(e4, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(e4, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e4, C3D_Alpha, GPU_REPLACE);

	if (gate)
	{
		/* out = C*(1-k) + F*k, k = T0.alpha from the "low" pass */
		C3D_TexEnv *e5 = C3D_GetTexEnv(5);
		C3D_TexEnvSrc(e5, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvOpRgb(e5, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e5, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(e5, C3D_Alpha, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(e5, C3D_Alpha, GPU_REPLACE);
	}
}

void fsr3dsTevKernel(Fsr3dsKernel kernel, int strength255)
{
	fsr3dsTevReset();

	switch (kernel)
	{
	case FSR3DS_KERNEL_REPLACE:
		kernel_replace();
		break;
	case FSR3DS_KERNEL_LITE:
		kernel_lite();
		break;
	case FSR3DS_KERNEL_ADAPT_H:
		kernel_adapt_h();
		break;
	case FSR3DS_KERNEL_ADAPT_V:
		kernel_adapt_v();
		break;
	case FSR3DS_KERNEL_AA_BLUR:
		kernel_aa_blur(strength255);
		break;
	case FSR3DS_KERNEL_AA_EDGE:
		kernel_aa_edge();
		break;
	case FSR3DS_KERNEL_AA_APPLY:
		kernel_aa_apply();
		break;
	case FSR3DS_KERNEL_AA_EDGE_APPLY:
		kernel_aa_edge_apply();
		break;
	case FSR3DS_KERNEL_UP_LOW:
		kernel_up_low(strength255);
		break;
	case FSR3DS_KERNEL_UP_SHARP:
		kernel_up_sharp(strength255, false);
		break;
	case FSR3DS_KERNEL_UP_SHARP_GATE:
		kernel_up_sharp(strength255, true);
		break;
	default:
		kernel_replace();
		break;
	}
}

void fsr3dsTevFx(void)
{
	/* Stage 5 is free in every kernel used by the final pass. */
	C3D_TexEnv *e = C3D_GetTexEnv(5);
	C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
}

/* ------------------------------------------------------------------------- */
/* Dynamic resolution                                                        */
/* ------------------------------------------------------------------------- */

static double s_drsHold;
static int s_drsDir;

int fsr3dsDrsUpdate(double frameMs, int *resIndex, int resCount, int targetFps, double dt)
{
	if (targetFps <= 0 || !resIndex || resCount <= 1)
		return 0;

	const double target = 1000.0 / (double)targetFps;
	s_drsHold -= dt;

	if (s_drsHold > 0.0)
		return 0;

	if (frameMs > target * 1.10 && *resIndex > 0)
	{
		(*resIndex)--;
		s_drsHold = 0.5;
		s_drsDir = -1;
		return -1;
	}

	if (frameMs < target * 0.90 && *resIndex < resCount - 1)
	{
		(*resIndex)++;
		s_drsHold = 1.0;
		s_drsDir = 1;
		return 1;
	}

	s_drsDir = 0;
	return 0;
}

const char *fsr3dsVersion(void)
{
	return FSR3DS_VERSION;
}
