/* PICA-SR host checks: environment parsing, DRS ladder, target sizes, UV
 * rectangle and the 2DQ pass plan.  No GPU, no citro3d: source/n3ds_sr.c is
 * plain C by design (see docs/PICA_SR.md). */
#include "n3ds_sr.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    /* --- mode names ------------------------------------------------------ */
    assert(n3ds_sr_mode_from_name("off") == N3DS_SR_OFF);
    assert(n3ds_sr_mode_from_name("nearest") == N3DS_SR_NEAREST);
    assert(n3ds_sr_mode_from_name("BILINEAR") == N3DS_SR_BILINEAR);
    assert(n3ds_sr_mode_from_name("  TeV2D\t") == N3DS_SR_TEV2D);
    assert(n3ds_sr_mode_from_name("tev2dq") == N3DS_SR_TEV2DQ);
    assert(n3ds_sr_mode_from_name("tev2dqq") == -1);
    assert(n3ds_sr_mode_from_name("") == -1);
    assert(n3ds_sr_mode_from_name(NULL) == -1);
    assert(!strcmp(n3ds_sr_mode_name(N3DS_SR_TEV2DQ), "tev2dq"));

    /* --- scale ----------------------------------------------------------- */
    unsigned pct = 0;
    assert(n3ds_sr_parse_scale("0.8", &pct) == 0 && pct == 80);
    assert(n3ds_sr_parse_scale("80", &pct) == 0 && pct == 80);
    assert(n3ds_sr_parse_scale("80%", &pct) == 0 && pct == 80);
    assert(n3ds_sr_parse_scale("1", &pct) == 0 && pct == 100);
    assert(n3ds_sr_parse_scale("0.64", &pct) == 0 && pct == 64);
    assert(n3ds_sr_parse_scale("0.53", &pct) == 0 && pct == 53);
    assert(n3ds_sr_parse_scale("0.5", &pct) == 0 && pct == 50);
    assert(n3ds_sr_parse_scale("0.49", &pct) != 0);
    assert(n3ds_sr_parse_scale("120", &pct) != 0);
    assert(n3ds_sr_parse_scale("abc", &pct) != 0);
    assert(n3ds_sr_parse_scale("0.8x", &pct) != 0);
    assert(n3ds_sr_parse_scale("", &pct) != 0);

    /* --- defaults and key/value parsing ---------------------------------- */
    N3dsSrConfig cfg;
    n3ds_sr_config_defaults(&cfg);
    /* 53%: best POT fill for 240x400 -> 127x212 in 128x256 (82%). */
    assert(cfg.mode == N3DS_SR_OFF && cfg.scale_pct == 53 && cfg.drs == 0);

    assert(n3ds_sr_config_key(&cfg, "mode", "tev2dq") == 0);
    assert(cfg.mode == N3DS_SR_TEV2DQ);
    assert(n3ds_sr_config_key(&cfg, "MODE", "bogus") == -2);
    assert(cfg.mode == N3DS_SR_TEV2DQ); /* rejected value keeps the old one */
    assert(n3ds_sr_config_key(&cfg, "scale", "0.65") == 0);
    assert(cfg.scale_pct == 65);
    assert(n3ds_sr_config_key(&cfg, "scale", "0.2") == -2);
    assert(n3ds_sr_config_key(&cfg, "drs", "1") == 0 && cfg.drs == 1);
    assert(n3ds_sr_config_key(&cfg, "drs", "maybe") == -2 && cfg.drs == 1);
    assert(n3ds_sr_config_key(&cfg, "drs", "off") == 0 && cfg.drs == 0);
    assert(n3ds_sr_config_key(&cfg, "speed", "7") == -1);
    assert(n3ds_sr_config_key(&cfg, "mode", "") == -2);

    n3ds_sr_config_defaults(&cfg);
    assert(n3ds_sr_config_line(&cfg, "# comment") == 0);
    assert(n3ds_sr_config_line(&cfg, "") == 0);
    assert(n3ds_sr_config_line(&cfg, "  mode = tev2d  ") == 0);
    assert(cfg.mode == N3DS_SR_TEV2D);
    assert(n3ds_sr_config_line(&cfg, "scale=0.7\r") == 0 && cfg.scale_pct == 70);
    assert(n3ds_sr_config_line(&cfg, "bilinear") == 0);
    assert(cfg.mode == N3DS_SR_BILINEAR);
    assert(n3ds_sr_config_line(&cfg, "nonsense") == -1);
    assert(n3ds_sr_config_line(&cfg, "=5") == -1);
    assert(n3ds_sr_config_line(&cfg, "scale=") == -2);

    /* --- size arithmetic ------------------------------------------------- */
    assert(n3ds_sr_scaled(400, 80) == 320);
    assert(n3ds_sr_scaled(240, 80) == 192);
    assert(n3ds_sr_scaled(400, 64) == 256);
    assert(n3ds_sr_scaled(240, 64) == 154);
    assert(n3ds_sr_scaled(400, 50) == 200);
    assert(n3ds_sr_scaled(240, 50) == 120);
    assert(n3ds_sr_scaled(400, 100) == 400);
    assert(n3ds_sr_scaled(1, 50) == 1);

    assert(n3ds_sr_pow2(192) == 256);
    assert(n3ds_sr_pow2(320) == 512);
    assert(n3ds_sr_pow2(256) == 256);
    assert(n3ds_sr_pow2(400) == 512);
    assert(n3ds_sr_pow2(154) == 256);
    assert(n3ds_sr_pow2(120) == 128);
    assert(n3ds_sr_pow2(3) == 8);
    assert(n3ds_sr_pow2(2000) == 1024);

    /* --- POT-fit scale selection ---------------------------------------- */
    /* 53% fills 128x256 with 127x212 content; this beats 50/64/80 and all
     * other integer percentages for a 240x400 portrait target. */
    assert(n3ds_sr_pick_scale(240, 400) == 53);
    assert(n3ds_sr_pick_scale(0, 400) == 50);
    assert(n3ds_sr_pick_scale(240, 400) == n3ds_sr_pick_scale(400, 240));

    /* --- storage plan (RGBA8 scene + D16 depth + RGBA8 scratch) ---------- */
    N3dsSrStorage st;
    n3ds_sr_storage_size(240, 400, 50, 2, &st);
    assert(st.w == 120 && st.h == 200 && st.tex_w == 128 && st.tex_h == 256);
    assert(st.scene_bytes == 128u << 10);
    assert(st.depth_bytes == 64u << 10);   /* D16 */
    assert(st.scratch_bytes == 128u << 10);
    assert(st.total_bytes == 320u << 10);

    n3ds_sr_storage_size(240, 400, 53, 2, &st);
    assert(st.w == 127 && st.h == 212 && st.tex_w == 128 && st.tex_h == 256);
    assert(st.total_bytes == 320u << 10);  /* same cost, more pixels than 50 */

    n3ds_sr_storage_size(240, 400, 64, 2, &st);
    assert(st.w == 154 && st.h == 256 && st.tex_w == 256 && st.tex_h == 256);
    assert(st.scene_bytes == 256u << 10 && st.depth_bytes == 128u << 10);
    assert(st.scratch_bytes == 256u << 10 && st.total_bytes == 640u << 10);

    n3ds_sr_storage_size(240, 400, 80, 4, &st);
    assert(st.w == 192 && st.h == 320 && st.tex_w == 256 && st.tex_h == 512);
    assert(st.scene_bytes == 512u << 10 && st.depth_bytes == 256u << 10);
    assert(st.scratch_bytes == 512u << 10 && st.total_bytes == 1280u << 10);

    /* Single-pass modes allocate no scratch target. */
    n3ds_sr_storage_size(240, 400, 53, 1, &st);
    assert(st.scratch_bytes == 0 && st.total_bytes == 192u << 10);
    /* D16 is exactly half the RGBA8 scene storage. */
    assert(st.depth_bytes == st.scene_bytes / 2);
    n3ds_sr_storage_size(0, 400, 50, 2, &st);
    assert(st.tex_w == 0 && st.total_bytes == 0);
    n3ds_sr_storage_size(240, 400, 50, 2, NULL); /* must not crash */

    unsigned w, h;
    n3ds_sr_scene_size(240, 400, 80, &w, &h);
    assert(w == 192 && h == 320);
    n3ds_sr_scene_size(240, 400, 50, &w, &h);
    assert(w == 120 && h == 200);

    N3dsSrRect r;
    n3ds_sr_uv_region(512, 256, 320, 192, &r);
    assert(near(r.u0, 0.0f) && near(r.v0, 0.0f));
    assert(near(r.u1, 320.0f / 512.0f) && near(r.v1, 192.0f / 256.0f));
    n3ds_sr_uv_region(512, 256, 900, 900, &r);
    assert(near(r.u1, 1.0f) && near(r.v1, 1.0f));
    n3ds_sr_uv_region(0, 0, 320, 192, &r);
    assert(near(r.u0, 0.0f) && near(r.u1, 1.0f) && near(r.v1, 1.0f));

    /* --- DRS ladder ------------------------------------------------------ */
    n3ds_sr_config_defaults(&cfg);
    cfg.mode = N3DS_SR_TEV2D;
    cfg.scale_pct = 80;
    assert(n3ds_sr_drs_steps(&cfg) == 1);
    assert(n3ds_sr_drs_scale_pct(&cfg, 3) == 80); /* clamps at the only step */
    cfg.drs = 1;
    assert(n3ds_sr_drs_steps(&cfg) == 5);
    assert(n3ds_sr_drs_scale_pct(&cfg, 0) == 80);
    assert(n3ds_sr_drs_scale_pct(&cfg, 1) == 72);
    assert(n3ds_sr_drs_scale_pct(&cfg, 4) == 50);
    assert(n3ds_sr_drs_scale_pct(&cfg, 9) == 50);
    cfg.scale_pct = 50;
    assert(n3ds_sr_drs_steps(&cfg) == 1);
    cfg.scale_pct = 56;
    assert(n3ds_sr_drs_steps(&cfg) == 2);
    assert(n3ds_sr_drs_scale_pct(&cfg, 1) == 50);

    /* --- pass plan (192x320 scene in a 256x512 POT texture at 80%) -------- */
    N3dsSrPass p[4];
    assert(n3ds_sr_plan(N3DS_SR_OFF, 256, 512, p, 4) == 0);
    assert(n3ds_sr_plan(N3DS_SR_NEAREST, 256, 512, p, 4) == 1);
    assert(p[0].op == N3DS_SR_OP_COPY && p[0].du == 0.0f && p[0].dv == 0.0f);
    assert(n3ds_sr_plan(N3DS_SR_BILINEAR, 256, 512, p, 4) == 1);
    assert(n3ds_sr_plan(N3DS_SR_TEV2D, 256, 512, p, 4) == 2);
    assert(p[0].op == N3DS_SR_OP_LITE && near(p[0].du, 1.0f / 256.0f) && p[0].dv == 0.0f);
    assert(p[1].op == N3DS_SR_OP_LITE && p[1].du == 0.0f && near(p[1].dv, 1.0f / 512.0f));
    assert(n3ds_sr_plan(N3DS_SR_TEV2D, 256, 512, p, 1) == -1);
    assert(n3ds_sr_plan(N3DS_SR_TEV2DQ, 256, 512, p, 4) == 4);
    assert(p[0].op == N3DS_SR_OP_UP_LOW && near(p[0].du, 0.8f / 256.0f) && p[0].dv == 0.0f);
    assert(p[1].op == N3DS_SR_OP_UP_SHARP && near(p[1].du, 1.0f / 256.0f));
    assert(p[2].op == N3DS_SR_OP_UP_LOW && p[2].du == 0.0f && near(p[2].dv, 0.8f / 512.0f));
    assert(p[3].op == N3DS_SR_OP_UP_SHARP && near(p[3].dv, 1.0f / 512.0f));
    assert(n3ds_sr_plan(N3DS_SR_TEV2DQ, 256, 512, p, 2) == -1);
    assert(n3ds_sr_plan(N3DS_SR_TEV2DQ, 0, 512, p, 4) == -1);
    assert(n3ds_sr_plan(99, 256, 512, p, 4) == -1);

    /* --- summary --------------------------------------------------------- */
    char text[128];
    n3ds_sr_config_defaults(&cfg);
    cfg.mode = N3DS_SR_TEV2DQ;
    cfg.scale_pct = 80;
    cfg.drs = 1;
    unsigned n = n3ds_sr_describe(&cfg, text, sizeof text);
    assert(n == strlen(text));
    assert(!strcmp(text, "N3DS_SR=tev2dq N3DS_SR_SCALE=80% N3DS_SR_DRS=1"));
    n3ds_sr_describe(&cfg, text, 8);
    assert(strlen(text) < 8);

    printf("n3ds_sr: PASS (modes, scale, env, sizes, pow2, UV, plan, DRS)\n");
    return 0;
}
