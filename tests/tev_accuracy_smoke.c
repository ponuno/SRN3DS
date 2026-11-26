/* Host test: arithmetic accuracy of the D3D9 ps_3_0 -> PICA200 TEV
 * translation.
 *
 * For each hand-assembled shader this test runs two independent evaluators:
 *
 *   1. a straightforward D3D9 interpreter over the original token stream (the
 *      reference), and
 *   2. d9_tev_eval_pixel over the translated TEV program.
 *
 * A deterministic xorshift PRNG feeds thousands of LDR input vectors.  The
 * reported max error is the measured translation accuracy.  Constants are
 * quantised to RGBA8 in both paths because the PICA200 stage constant is an
 * RGBA8 colour.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_tev.h"

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define DST_R(n) (0x80000000u | (uint32_t)(n) | (0xFu << 16))
#define DST_C(n) (0xA0000000u | (uint32_t)(n) | (0xFu << 16))
#define DST_C0 (0x80000000u | 0x0800u | (0xFu << 16))
#define SRC_R(n) (0x80000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_C(n) (0xA0000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_CX(n) (0xA0000000u | (uint32_t)(n))
#define SRC_T(n) (0x90000000u | (uint32_t)(n) | (0xE4u << 16))
#define SRC_V(n) (0x90000000u | (uint32_t)(n) | (0xE4u << 16))
#define DCL_TEXCOORD(n) (0x80000005u | ((uint32_t)(n) << 16))
#define DCL_COLOR(n) (0x8000000Au | ((uint32_t)(n) << 16))
#define DCL_SAMPLER2D (0x90000000u)
#define SAMP(n) (0xA00F0800u | (uint32_t)(n))
#define TEXD(n) (0x90000000u | (uint32_t)(n) | (0x03u << 16))
#define VER_PS30 0xFFFF0300u
#define END_TOKEN 0xFFFFu

typedef struct {
    float temp[8][4];
    float konst[4][4];
    float input[4][4];
    float out[4];
    int out_seen;
} RefState;

static unsigned regtype(uint32_t t) {
    return ((t >> 28) & 7u) | ((t & 0x1800u) >> 8);
}
static unsigned regnum(uint32_t t) { return t & 0x7FFu; }
static unsigned maskof(uint32_t t) { return ((t >> 16) & 0xFu); }
static unsigned swzof(uint32_t t) { return (t >> 16) & 0xFFu; }
static unsigned modof(uint32_t t) { return (t >> 24) & 0xFu; }

static uint32_t fbits(float v) {
    union { float f; uint32_t u; } x;
    x.f = v;
    return x.u;
}

static void src_value(const RefState *st, uint32_t token, float out[4]) {
    unsigned type = regtype(token), index = regnum(token);
    const float *v;
    if (type == 2) v = st->konst[index & 3];
    else if (type == 1) v = st->input[index & 3];
    else v = st->temp[index & 7];
    unsigned sw = swzof(token);
    for (int i = 0; i < 4; ++i) out[i] = v[(sw >> (2 * i)) & 3u];
    if (modof(token) & 1u) for (int i = 0; i < 4; ++i) out[i] = -out[i];
}

static void write_dst(RefState *st, uint32_t token, const float value[4]) {
    unsigned type = regtype(token), index = regnum(token);
    float *dst = (type == 8) ? st->out : st->temp[index & 7];
    unsigned mask = maskof(token);
    if (!mask) mask = 0xF;
    for (int i = 0; i < 4; ++i)
        if (mask & (1u << i)) dst[i] = value[i];
    if (type == 8) st->out_seen = 1;
}

static int d3d_run(const uint32_t *code, size_t words,
                   const float tex[3][4], const float primary[4],
                   float out[4]) {
    RefState st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 4; ++i) memcpy(st.input[i], primary, sizeof st.input[i]);
    size_t pos = 1;
    while (pos < words) {
        uint32_t token = code[pos];
        unsigned op = token & 0xFFFFu;
        if (op == 0xFFFFu) break;
        if (op == 0xFFFEu) { pos += 1 + ((token >> 16) & 0x7FFFu); continue; }
        size_t total = 1 + ((token >> 24) & 0xFu);
        if (total < 2) return -1;
        const uint32_t *arg = code + pos + 1;
        if (op == 0x1F) { pos += total; continue; }
        if (op == 0x51) {
            unsigned index = regnum(arg[0]);
            memcpy(st.konst[index & 3], &arg[1], sizeof st.konst[0]);
            pos += total; continue;
        }
        if (op == 0x42) {
            unsigned dst = arg[0], smp = arg[2];
            unsigned unit = regnum(smp);
            if (unit > 2) return -1;
            write_dst(&st, dst, tex[unit]);
            pos += total; continue;
        }
        float a[4], b[4], c[4], result[4];
        src_value(&st, arg[1], a);
        if (total > 3) src_value(&st, arg[2], b);
        if (total > 4) src_value(&st, arg[3], c);
        switch (op) {
        case 0x01: memcpy(result, a, sizeof result); break;
        case 0x02: for (int i = 0; i < 4; ++i) result[i] = a[i] + b[i]; break;
        case 0x03: for (int i = 0; i < 4; ++i) result[i] = a[i] - b[i]; break;
        case 0x04: for (int i = 0; i < 4; ++i) result[i] = a[i] * b[i] + c[i]; break;
        case 0x05: for (int i = 0; i < 4; ++i) result[i] = a[i] * b[i]; break;
        case 0x08: {
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            result[0] = result[1] = result[2] = result[3] = d;
            break;
        }
        case 0x12: for (int i = 0; i < 4; ++i) result[i] = a[i] * c[i] + b[i] * (1.0f - c[i]); break;
        default: return -2;
        }
        write_dst(&st, arg[0], result);
        pos += total;
    }
    if (!st.out_seen) return -1;
    for (int i = 0; i < 4; ++i) {
        float v = st.out[i];
        out[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
    return 0;
}

static unsigned rng_state = 0x12345678u;
static float rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)(rng_state & 0xFFFFu) / 65535.0f;
}

/* ------------------------------------------------------------------------- */
/* Tiny assembler helpers                                                    */
/* ------------------------------------------------------------------------- */

static unsigned quant8(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    unsigned q = (unsigned)(v * 255.0f + 0.5f);
    return q > 255u ? 255u : q;
}

static void put_version(uint32_t *code, size_t *n) {
    code[(*n)++] = VER_PS30;
}
static void put_sampler_dcl(uint32_t *code, size_t *n, unsigned s) {
    code[(*n)++] = DI(0x1F, 2);
    code[(*n)++] = DCL_SAMPLER2D;
    code[(*n)++] = SAMP(s);
}
static void put_texcoord_dcl(uint32_t *code, size_t *n, unsigned t) {
    code[(*n)++] = DI(0x1F, 2);
    code[(*n)++] = DCL_TEXCOORD(t);
    code[(*n)++] = TEXD(t);
}
static void put_def(uint32_t *code, size_t *n, unsigned reg, const unsigned q[4]) {
    code[(*n)++] = DI(0x51, 5);
    code[(*n)++] = DST_C(reg);
    for (int i = 0; i < 4; ++i) code[(*n)++] = fbits((float)q[i] / 255.0f);
}
static void put_texld(uint32_t *code, size_t *n, unsigned dst, unsigned uv, unsigned s) {
    code[(*n)++] = DI(0x42, 3);
    code[(*n)++] = DST_R(dst);
    code[(*n)++] = SRC_T(uv);
    code[(*n)++] = SAMP(s);
}
static void put_end(uint32_t *code, size_t *n) { code[(*n)++] = END_TOKEN; }

/* ------------------------------------------------------------------------- */

static int run_case(const char *name, const uint32_t *code, size_t words,
                    const float constants[4][4], int constant_count,
                    int check_primary) {
    D9TevProgram program;
    char why[256];
    if (d9_tev_translate_pixel(code, words, &program, why, sizeof why) != 0) {
        printf("FAIL %s: translate: %s\n", name, why);
        return 1;
    }
    float stage_const[D9TEV_MAX_STAGES][4];
    memset(stage_const, 0, sizeof stage_const);
    for (unsigned s = 0; s < program.stages; ++s) {
        if (!program.stage[s].const_is_literal && program.stage[s].const_reg != 0xFF) {
            unsigned reg = program.stage[s].const_reg;
            if (reg < (unsigned)constant_count)
                memcpy(stage_const[s], constants[reg], sizeof stage_const[s]);
        }
    }

    double worst = 0.0;
    const int vectors = 4000;
    for (int n = 0; n < vectors; ++n) {
        float tex[3][4];
        for (int u = 0; u < 3; ++u)
            for (int c = 0; c < 4; ++c) tex[u][c] = rnd();
        float primary[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (check_primary) for (int c = 0; c < 4; ++c) primary[c] = rnd();
        float expect[4], got[4];
        if (d3d_run(code, words, tex, primary, expect) != 0) {
            printf("FAIL %s: reference interpreter failed\n", name);
            return 1;
        }
        d9_tev_eval_pixel(&program, tex, stage_const, primary, got);
        for (int c = 0; c < 4; ++c) {
            double error = fabs((double)expect[c] - (double)got[c]);
            if (error > worst) worst = error;
        }
    }
    printf("  %-16s stages=%u textures=%u vectors=%d max_abs_error=%.9f\n",
           name, program.stages, program.textures, vectors, worst);
    if (worst > 1e-5) {
        printf("FAIL %s: max error %.9f exceeds 1e-5\n", name, worst);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    printf("tev accuracy (translated TEV vs D3D9 reference interpreter)\n");
    uint32_t code[64];

    { /* blit */
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        code[n++] = DI(0x42, 3); code[n++] = DST_C0; code[n++] = SRC_T(0); code[n++] = SAMP(0);
        put_end(code, &n);
        failures += run_case("blit", code, n, NULL, 0, 0);
    }
    { /* tint: tex * c0 */
        unsigned q[4] = {quant8(0.5f), quant8(0.25f), 255, 255};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_def(code, &n, 0, q);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x05, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_C(0);
        put_end(code, &n);
        float constants[4][4] = {{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f}};
        failures += run_case("tint", code, n, constants, 1, 0);
    }
    { /* channel carry: RGB and alpha output writes keep the other side live */
        unsigned q[4] = {128, 64, 255, 255};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        code[n++] = DI(0x1F, 2); code[n++] = DCL_COLOR(0); code[n++] = SRC_V(1);
        put_sampler_dcl(code, &n, 0);
        put_def(code, &n, 0, q);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x05, 3); code[n++] = DST_R(0); code[n++] = SRC_R(0); code[n++] = SRC_V(1);
        code[n++] = DI(0x05, 3); code[n++] = 0x80070800u; code[n++] = SRC_R(0); code[n++] = SRC_CX(0);
        code[n++] = DI(0x01, 2); code[n++] = 0x80080800u; code[n++] = SRC_R(0);
        put_end(code, &n);
        float constants[4][4] = {{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f}};
        failures += run_case("channel-carry", code, n, constants, 1, 1);
    }
    { /* split source selectors: RGB from texture0, alpha from texture1 */
        unsigned q[4] = {128, 64, 191, 32};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 1);
        put_def(code, &n, 0, q);
        put_texld(code, &n, 0, 0, 0);
        put_texld(code, &n, 1, 0, 1);
        code[n++] = DI(0x01, 2); code[n++] = 0x80080000u; code[n++] = 0x80FF0001u;
        code[n++] = DI(0x05, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_C(0);
        put_end(code, &n);
        float constants[4][4] = {{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f}};
        failures += run_case("split-channels", code, n, constants, 1, 0);
    }
    { /* DAG fusion: two independent MUL results consumed by ADD */
        unsigned q0[4] = {128, 128, 128, 128};
        unsigned q1[4] = {64, 64, 64, 64};
        size_t n = 0;
        put_version(code, &n);
        code[n++] = DI(0x1F, 2); code[n++] = DCL_COLOR(0); code[n++] = SRC_V(1);
        put_def(code, &n, 0, q0);
        put_def(code, &n, 1, q1);
        code[n++] = DI(0x05, 3); code[n++] = DST_R(0); code[n++] = SRC_C(0); code[n++] = SRC_V(1);
        code[n++] = DI(0x05, 3); code[n++] = DST_R(1); code[n++] = SRC_C(1); code[n++] = SRC_V(1);
        code[n++] = DI(0x02, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_R(1);
        put_end(code, &n);
        float constants[4][4] = {
            {q0[0] / 255.0f, q0[1] / 255.0f, q0[2] / 255.0f, q0[3] / 255.0f},
            {q1[0] / 255.0f, q1[1] / 255.0f, q1[2] / 255.0f, q1[3] / 255.0f},
        };
        failures += run_case("dag-fusion", code, n, constants, 2, 1);
    }
    { /* two textures, lrp */
        unsigned q[4] = {192, 192, 192, 192};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 1);
        put_def(code, &n, 0, q);
        put_texld(code, &n, 0, 0, 0);
        put_texld(code, &n, 1, 0, 1);
        code[n++] = DI(0x12, 4); code[n++] = DST_C0;
        code[n++] = SRC_CX(0); code[n++] = SRC_R(0); code[n++] = SRC_R(1);
        put_end(code, &n);
        float constants[4][4] = {{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f}};
        failures += run_case("lrp-2tex", code, n, constants, 1, 0);
    }
    { /* mad with two constants */
        unsigned q0[4] = {128, 128, 128, 128};
        unsigned q1[4] = {64, 64, 64, 64};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_def(code, &n, 0, q0);
        put_def(code, &n, 1, q1);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x04, 4); code[n++] = DST_C0;
        code[n++] = SRC_R(0); code[n++] = SRC_C(0); code[n++] = SRC_C(1);
        put_end(code, &n);
        float constants[4][4] = {
            {q0[0] / 255.0f, q0[1] / 255.0f, q0[2] / 255.0f, q0[3] / 255.0f},
            {q1[0] / 255.0f, q1[1] / 255.0f, q1[2] / 255.0f, q1[3] / 255.0f},
        };
        failures += run_case("mad-2const", code, n, constants, 2, 0);
    }
    { /* three-stage chain: (tex * c0 + c1) * c2 */
        unsigned q0[4] = {128, 128, 128, 128};
        unsigned q1[4] = {26, 26, 26, 26};
        unsigned q2[4] = {128, 128, 128, 128};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_def(code, &n, 0, q0);
        put_def(code, &n, 1, q1);
        put_def(code, &n, 2, q2);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x05, 3); code[n++] = DST_R(0); code[n++] = SRC_R(0); code[n++] = SRC_C(0);
        code[n++] = DI(0x02, 3); code[n++] = DST_R(0); code[n++] = SRC_R(0); code[n++] = SRC_C(1);
        code[n++] = DI(0x05, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_C(2);
        put_end(code, &n);
        float constants[4][4] = {
            {q0[0] / 255.0f, q0[1] / 255.0f, q0[2] / 255.0f, q0[3] / 255.0f},
            {q1[0] / 255.0f, q1[1] / 255.0f, q1[2] / 255.0f, q1[3] / 255.0f},
            {q2[0] / 255.0f, q2[1] / 255.0f, q2[2] / 255.0f, q2[3] / 255.0f},
        };
        failures += run_case("chain-3stage", code, n, constants, 3, 0);
    }
    { /* dp3 lighting against a constant */
        unsigned q[4] = {128, 128, 128, 0};
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        put_sampler_dcl(code, &n, 0);
        put_def(code, &n, 0, q);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x08, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_C(0);
        put_end(code, &n);
        float constants[4][4] = {{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, 0.0f}};
        failures += run_case("dp3", code, n, constants, 1, 0);
    }
    { /* vertex colour tint: tex * v1 */
        size_t n = 0;
        put_version(code, &n);
        put_texcoord_dcl(code, &n, 0);
        code[n++] = DI(0x1F, 2); code[n++] = DCL_COLOR(0); code[n++] = SRC_V(1);
        put_sampler_dcl(code, &n, 0);
        put_texld(code, &n, 0, 0, 0);
        code[n++] = DI(0x05, 3); code[n++] = DST_C0; code[n++] = SRC_R(0); code[n++] = SRC_V(1);
        put_end(code, &n);
        failures += run_case("vertex-color", code, n, NULL, 0, 1);
    }

    printf("tev accuracy: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
