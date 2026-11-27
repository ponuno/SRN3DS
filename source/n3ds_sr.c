/*
 * PICA-SR configuration/geometry helpers.  No GPU calls, no 3DS headers:
 * tests/n3ds_sr_smoke.c compiles this file on the host.
 */
#include "n3ds_sr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_names[] = {
    "off", "nearest", "bilinear", "tev2d", "tev2dq",
};

static int equals_ci(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static const char *skip_blanks(const char *s)
{
    if (!s)
        return "";
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

static size_t trimmed_len(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n'))
        --n;
    return n;
}

static int name_matches(const char *text, const char *name)
{
    size_t n = trimmed_len(text);
    size_t m = strlen(name);
    if (n != m)
        return 0;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)name[i]))
            return 0;
    return 1;
}

void n3ds_sr_config_defaults(N3dsSrConfig *cfg)
{
    if (!cfg)
        return;
    cfg->mode = N3DS_SR_OFF;
    cfg->scale_pct = 80;
    cfg->drs = 0;
}

int n3ds_sr_mode_from_name(const char *name)
{
    const char *s = skip_blanks(name);
    for (unsigned i = 0; i < sizeof k_names / sizeof k_names[0]; ++i)
        if (name_matches(s, k_names[i]))
            return (int)i;
    return -1;
}

const char *n3ds_sr_mode_name(unsigned mode)
{
    return mode < sizeof k_names / sizeof k_names[0] ? k_names[mode] : "?";
}

int n3ds_sr_parse_scale(const char *text, unsigned *pct)
{
    const char *s = skip_blanks(text);
    char *end = NULL;
    if (!s || !*s || *s == '-' || *s == '+')
        return -1;
    double v = strtod(s, &end);
    if (!end || end == s)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end == '%')
        ++end;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end)
        return -1; /* trailing junk */
    /* A bare 1 means native scale; 50..100 is already a percentage.  Values
     * below 1 are fractions (0.5..0.99). */
    double percent = v <= 1.0 ? v * 100.0 : v;
    if (percent < 49.5 || percent > 100.5)
        return -1;
    unsigned p = (unsigned)(percent + 0.5);
    if (p < 50)
        p = 50;
    if (p > 100)
        p = 100;
    if (pct)
        *pct = p;
    return 0;
}

int n3ds_sr_config_key(N3dsSrConfig *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value)
        return -2;
    const char *v = skip_blanks(value);
    if (!*v)
        return -2;

    if (name_matches(key, "mode"))
    {
        int m = n3ds_sr_mode_from_name(v);
        if (m < 0)
            return -2;
        cfg->mode = (unsigned)m;
        return 0;
    }
    if (name_matches(key, "scale"))
    {
        unsigned pct;
        if (n3ds_sr_parse_scale(v, &pct))
            return -2;
        cfg->scale_pct = pct;
        return 0;
    }
    if (name_matches(key, "drs"))
    {
        if (name_matches(v, "1") || name_matches(v, "on") ||
            name_matches(v, "true") || name_matches(v, "yes"))
            cfg->drs = 1;
        else if (name_matches(v, "0") || name_matches(v, "off") ||
                 name_matches(v, "false") || name_matches(v, "no"))
            cfg->drs = 0;
        else
            return -2;
        return 0;
    }
    return -1;
}

int n3ds_sr_config_line(N3dsSrConfig *cfg, const char *line)
{
    const char *s = skip_blanks(line);
    if (!cfg || !s)
        return -1;
    if (!*s || *s == '#' || *s == ';')
        return 0; /* blank/comment */

    const char *eq = strchr(s, '=');
    if (!eq)
    {
        /* Bare mode name is accepted for a one-token config file. */
        int m = n3ds_sr_mode_from_name(s);
        if (m < 0)
            return -1;
        cfg->mode = (unsigned)m;
        return 0;
    }

    char key[16];
    size_t klen = (size_t)(eq - s);
    while (klen > 0 && (s[klen - 1] == ' ' || s[klen - 1] == '\t'))
        --klen;
    if (klen == 0 || klen >= sizeof key)
        return -1;
    memcpy(key, s, klen);
    key[klen] = 0;
    return n3ds_sr_config_key(cfg, key, eq + 1) == 0 ? 0 : -2;
}

unsigned n3ds_sr_scaled(unsigned base, unsigned pct)
{
    if (pct > 100)
        pct = 100;
    unsigned n = (unsigned)(((unsigned long long)base * pct + 50ull) / 100ull);
    return n ? n : 1;
}

unsigned n3ds_sr_pow2(unsigned n)
{
    unsigned p = 8;
    while (p < n && p < 1024)
        p <<= 1;
    return p;
}

unsigned n3ds_sr_drs_steps(const N3dsSrConfig *cfg)
{
    if (!cfg || !cfg->drs)
        return 1;
    unsigned steps = 1;
    unsigned pct = cfg->scale_pct;
    while (pct > 50)
    {
        pct = pct > 58 ? pct - 8 : 50;
        ++steps;
    }
    return steps;
}

unsigned n3ds_sr_drs_scale_pct(const N3dsSrConfig *cfg, unsigned index)
{
    if (!cfg || !cfg->drs)
        return cfg ? cfg->scale_pct : 80;
    unsigned pct = cfg->scale_pct;
    while (index-- && pct > 50)
        pct = pct > 58 ? pct - 8 : 50;
    return pct;
}

void n3ds_sr_scene_size(unsigned store_w, unsigned store_h, unsigned pct,
                        unsigned *w, unsigned *h)
{
    if (w)
        *w = n3ds_sr_scaled(store_w, pct);
    if (h)
        *h = n3ds_sr_scaled(store_h, pct);
}

void n3ds_sr_uv_region(unsigned tex_w, unsigned tex_h, unsigned w, unsigned h,
                       N3dsSrRect *rect)
{
    if (!rect)
        return;
    if (!tex_w || !tex_h)
    {
        rect->u0 = rect->v0 = 0.0f;
        rect->u1 = rect->v1 = 1.0f;
        return;
    }
    if (w > tex_w)
        w = tex_w;
    if (h > tex_h)
        h = tex_h;
    rect->u0 = 0.0f;
    rect->v0 = 0.0f;
    rect->u1 = (float)w / (float)tex_w;
    rect->v1 = (float)h / (float)tex_h;
}

/* The 2DQ chain is fitted in source texels; the sharpness knob of the
 * reference app is fixed at its neutral 1.0x (0.75 knob). */
int n3ds_sr_plan(unsigned mode, unsigned tex_w, unsigned tex_h,
                 N3dsSrPass *passes, unsigned max_passes)
{
    if (!tex_w || !tex_h || !passes)
        return -1;
    float iw = 1.0f / (float)tex_w;
    float ih = 1.0f / (float)tex_h;

    switch (mode)
    {
    case N3DS_SR_OFF:
        return 0;
    case N3DS_SR_NEAREST:
    case N3DS_SR_BILINEAR:
        if (max_passes < 1)
            return -1;
        passes[0].op = N3DS_SR_OP_COPY;
        passes[0].du = passes[0].dv = 0.0f;
        return 1;
    case N3DS_SR_TEV2D:
        if (max_passes < 2)
            return -1;
        passes[0] = (N3dsSrPass){N3DS_SR_OP_LITE, 1.0f * iw, 0.0f};
        passes[1] = (N3dsSrPass){N3DS_SR_OP_LITE, 0.0f, 1.0f * ih};
        return 2;
    case N3DS_SR_TEV2DQ:
        if (max_passes < 4)
            return -1;
        /* H LOW, H SHARP, V LOW, V SHARP; the last pass stretches to the LCD. */
        passes[0] = (N3dsSrPass){N3DS_SR_OP_UP_LOW, N3DS_SR_2DQ_LOW_RADIUS * iw, 0.0f};
        passes[1] = (N3dsSrPass){N3DS_SR_OP_UP_SHARP, N3DS_SR_2DQ_SHARP_RADIUS * iw, 0.0f};
        passes[2] = (N3dsSrPass){N3DS_SR_OP_UP_LOW, 0.0f, N3DS_SR_2DQ_LOW_RADIUS * ih};
        passes[3] = (N3dsSrPass){N3DS_SR_OP_UP_SHARP, 0.0f, N3DS_SR_2DQ_SHARP_RADIUS * ih};
        return 4;
    default:
        return -1;
    }
}

unsigned n3ds_sr_describe(const N3dsSrConfig *cfg, char *out, size_t cap)
{
    if (!out || !cap)
        return 0;
    if (!cfg)
    {
        out[0] = 0;
        return 0;
    }
    int n = snprintf(out, cap, "N3DS_SR=%s N3DS_SR_SCALE=%u%% N3DS_SR_DRS=%u",
                     n3ds_sr_mode_name(cfg->mode), cfg->scale_pct, cfg->drs ? 1u : 0u);
    if (n < 0)
        out[0] = 0;
    return (unsigned)strlen(out);
}
