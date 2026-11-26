/* Vertex-colour TEV mapping and null-backend colour draw smoke test.
 *
 * The PICA backend cannot be executed on the host, so this test proves the two
 * host-decidable halves of the colour path:
 *   1. d9_color_tev_stage0() maps the closed D3D9 stage-0 subset to a single
 *      TEV stage whose semantics are checked against an independent CPU
 *      reference written from the D3D9 combiners (SELECTARG1/SELECTARG2/
 *      MODULATE, colour and alpha separately);
 *   2. the null backend receives the exact per-vertex RGBA the guest supplied
 *      and records the stage state, without claiming any GPU output.
 *
 * It does not prove a PICA pixel.  See REPORT.md. */
#include "d3d9_core.h"
#include "d3d9_color_tev.h"
#include "d3d9_null.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- independent CPU reference for the mapped TEV stage ------------------ */
static float ref_src(unsigned src, unsigned ch, const float tex[4],
                     const float primary[4]) {
    if (src == D9_CTEV_SRC_TEXTURE) return tex[ch];
    assert(src == D9_CTEV_SRC_PRIMARY || src == D9_CTEV_SRC_CURRENT);
    /* D3D9 CURRENT at stage 0 is the diffuse (vertex) colour. */
    return primary[ch];
}
static float ref_eval(unsigned func, const unsigned src[3], unsigned ch,
                      const float tex[4], const float primary[4]) {
    if (func == D9_CTEV_REPLACE) return ref_src(src[0], ch, tex, primary);
    assert(func == D9_CTEV_MODULATE);
    return ref_src(src[0], ch, tex, primary) * ref_src(src[1], ch, tex, primary);
}
static D9DrawState stage_state(unsigned cop, unsigned c1, unsigned c2,
                               unsigned aop, unsigned a1, unsigned a2,
                               unsigned texcoord) {
    D9DrawState s;
    memset(&s, 0, sizeof s);
    s.stage_colorop = cop;
    s.stage_colorarg1 = c1;
    s.stage_colorarg2 = c2;
    s.stage_alphaop = aop;
    s.stage_alphaarg1 = a1;
    s.stage_alphaarg2 = a2;
    s.stage_texcoordindex = texcoord;
    return s;
}
static void expect_stage(const D9DrawState *s, int has_texture,
                         const D9ColorTevStage *want,
                         const float want_rgba[4]) {
    static const float tex[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    static const float primary[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    D9ColorTevStage plan;
    const char *why = NULL;
    int rc = d9_color_tev_stage0(s, has_texture, &plan, &why);
    assert(rc == D9_OK);
    assert(plan.color_func == want->color_func);
    assert(plan.alpha_func == want->alpha_func);
    for (unsigned i = 0; i < 3; ++i) {
        assert(plan.color_src[i] == want->color_src[i]);
        assert(plan.alpha_src[i] == want->alpha_src[i]);
    }
    for (unsigned ch = 0; ch < 4; ++ch) {
        float v = ch < 3
            ? ref_eval(plan.color_func, plan.color_src, ch, tex, primary)
            : ref_eval(plan.alpha_func, plan.alpha_src, ch, tex, primary);
        assert(fabsf(v - want_rgba[ch]) < 1e-6f);
    }
    (void)why;
}
static void expect_reject(const D9DrawState *s, int has_texture,
                          const char *needle) {
    D9ColorTevStage plan;
    const char *why = NULL;
    int rc = d9_color_tev_stage0(s, has_texture, &plan, &why);
    assert(rc == D9_UNSUPPORTED);
    assert(why && strstr(why, needle));
}

/* --- null-backend capture ------------------------------------------------- */
typedef struct {float x,y,z;uint32_t color;float u,v;} XyzColorVertex;
typedef struct {float x,y,z;float r,g,b,a;float u,v;} XyzFloatColorVertex;

static D9Handle upload_vb(D9Device *d, const void *data, size_t bytes) {
    D9Handle h; void *p;
    assert(!d9_create_vertex_buffer(d, bytes, &h));
    assert(!d9_buffer_lock(d, h, 0, bytes, &p));
    memcpy(p, data, bytes);
    assert(!d9_buffer_unlock(d, h));
    return h;
}
static void bind_colour(D9Device *d, D9Handle vb, size_t stride) {
    assert(!d9_set_stream_source(d, vb, 0, stride));
    assert(!d9_set_vertex_declaration(d, NULL));
}

int main(void) {
    /* --- 1. stage-0 -> one TEV stage, checked against the reference ------ */
    D9ColorTevStage want;

    /* MODULATE(TEXTURE,DIFFUSE) for both colour and alpha (the GFx tuple). */
    want = (D9ColorTevStage){
        D9_CTEV_MODULATE, {D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY},
        D9_CTEV_MODULATE, {D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY}};
    D9DrawState s = stage_state(D9_TOP_MODULATE, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                                D9_TOP_MODULATE, D9_TA_TEXTURE, D9_TA_DIFFUSE, 0);
    {
        const float t[4] = {0.2f, 0.4f, 0.6f, 0.8f};
        const float p[4] = {1.0f, 0.5f, 0.25f, 1.0f};
        const float want_rgba[4] = {0.2f * 1.0f, 0.4f * 0.5f, 0.6f * 0.25f, 0.8f * 1.0f};
        expect_stage(&s, 1, &want, want_rgba);
        (void)t; (void)p;
    }

    /* SELECTARG1(TEXTURE). */
    want = (D9ColorTevStage){
        D9_CTEV_REPLACE, {D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_TEXTURE},
        D9_CTEV_REPLACE, {D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_TEXTURE}};
    s = stage_state(D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT,
                    D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT, 0);
    expect_stage(&s, 1, &want, (const float[4]){0.2f, 0.4f, 0.6f, 0.8f});

    /* SELECTARG2(DIFFUSE). */
    want = (D9ColorTevStage){
        D9_CTEV_REPLACE, {D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY},
        D9_CTEV_REPLACE, {D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY}};
    s = stage_state(D9_TOP_SELECTARG2, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                    D9_TOP_SELECTARG2, D9_TA_TEXTURE, D9_TA_DIFFUSE, 0);
    expect_stage(&s, 1, &want, (const float[4]){1.0f, 0.5f, 0.25f, 1.0f});

    /* SELECTARG2(CURRENT): stage 0 CURRENT is the diffuse colour. */
    want = (D9ColorTevStage){
        D9_CTEV_REPLACE, {D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT},
        D9_CTEV_REPLACE, {D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT}};
    s = stage_state(D9_TOP_SELECTARG2, D9_TA_TEXTURE, D9_TA_CURRENT,
                    D9_TOP_SELECTARG2, D9_TA_TEXTURE, D9_TA_CURRENT, 0);
    expect_stage(&s, 0, &want, (const float[4]){1.0f, 0.5f, 0.25f, 1.0f});

    /* Colour and alpha stages are independent: colour modulates, alpha selects
     * the vertex alpha. */
    want = (D9ColorTevStage){
        D9_CTEV_MODULATE, {D9_CTEV_SRC_TEXTURE, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY},
        D9_CTEV_REPLACE, {D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_PRIMARY}};
    s = stage_state(D9_TOP_MODULATE, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                    D9_TOP_SELECTARG1, D9_TA_DIFFUSE, D9_TA_CURRENT, 0);
    {
        const float want_rgba[4] = {0.2f * 1.0f, 0.4f * 0.5f, 0.6f * 0.25f, 1.0f};
        expect_stage(&s, 1, &want, want_rgba);
    }

    /* MODULATE(DIFFUSE,CURRENT) == diffuse * diffuse. */
    want = (D9ColorTevStage){
        D9_CTEV_MODULATE, {D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT},
        D9_CTEV_MODULATE, {D9_CTEV_SRC_PRIMARY, D9_CTEV_SRC_CURRENT, D9_CTEV_SRC_CURRENT}};
    s = stage_state(D9_TOP_MODULATE, D9_TA_DIFFUSE, D9_TA_CURRENT,
                    D9_TOP_MODULATE, D9_TA_DIFFUSE, D9_TA_CURRENT, 0);
    expect_stage(&s, 0, &want, (const float[4]){1.0f, 0.25f, 0.0625f, 1.0f});

    /* --- 2. unexpressible tuples fail by name --------------------------- */
    s = stage_state(D9_TOP_MODULATE, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                    D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT, 0);
    expect_reject(&s, 0, "TEXTURE");           /* no texture bound */
    s = stage_state(D9_TOP_DISABLE, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                    D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT, 0);
    expect_reject(&s, 1, "DISABLE");
    s = stage_state(D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT,
                    D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT, 1);
    expect_reject(&s, 1, "TEXCOORDINDEX");
    s = stage_state(22u, D9_TA_TEXTURE, D9_TA_DIFFUSE,
                    D9_TOP_SELECTARG1, D9_TA_TEXTURE, D9_TA_CURRENT, 0);
    expect_reject(&s, 1, "operation");
    {
        D9ColorTevStage plan; const char *why = "x";
        assert(d9_color_tev_stage0(NULL, 1, &plan, &why) == D9_INVALID);
        assert(why && *why);
    }
    /* CURRENT is only admitted in ARG2 by the core; the mapper itself would
     * still resolve it, so prove the core gate rejects ARG1=CURRENT. */
    {
        D9Device *probe = d9_create_device(d9_null_backend(), d9_null_context(), D9_NULL_RESOURCE_LIMIT);
        assert(probe);
        assert(d9_set_texture_stage_state(probe, 0, D9_TSS_COLORARG1, D9_TA_CURRENT) == D9_UNSUPPORTED);
        assert(strstr(d9_last_draw_diagnostic(probe), "CURRENT"));
        d9_destroy_device(probe); d9_null_reset();
    }

    /* --- 3. null backend receives the exact guest RGBA ------------------ */
    d9_null_reset();
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), D9_NULL_RESOURCE_LIMIT);
    assert(d);
    /* Fix the stage tuple to something the real backend can also express. */
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLOROP, D9_TOP_SELECTARG1));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG1, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAOP, D9_TOP_SELECTARG1));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG1, D9_TA_DIFFUSE));

    const XyzColorVertex packed[3] = {
        {0.0f, 0.0f, 0.5f, 0x80ff0000u, 0.0f, 0.0f},
        {64.0f, 0.0f, 0.5f, 0xff00ff00u, 1.0f, 0.0f},
        {0.0f, 64.0f, 0.5f, 0xff0000ffu, 0.0f, 1.0f}};
    D9Decl packed_decl; const char *why = NULL;
    assert(!d9_decl_from_fvf(0x142,0, &packed_decl, &why));
    D9Handle vb = upload_vb(d, packed, sizeof packed);
    bind_colour(d, vb, sizeof packed[0]);
    assert(!d9_set_vertex_declaration(d, &packed_decl));
    assert(!d9_begin_scene(d, 0xff000000));
    assert(!d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1));
    assert(!d9_present(d));

    const D9NullStats *st = d9_null_stats();
    assert(st->color_draws == 1 && st->last_color_count == 3);
    assert(st->last_color_vertices[0].r == 1.0f);
    assert(st->last_color_vertices[0].g == 0.0f);
    assert(st->last_color_vertices[0].b == 0.0f);
    assert(st->last_color_vertices[0].a == 128.0f / 255.0f);
    assert(st->last_color_vertices[1].g == 1.0f);
    assert(st->last_color_vertices[2].b == 1.0f);
    assert(st->last_color_vertices[2].u == 0.0f && st->last_color_vertices[2].v == 1.0f);
    assert(st->last_color_state.stage_colorarg1 == D9_TA_DIFFUSE);

    /* Unpacked float RGBA is copied verbatim. */
    const XyzFloatColorVertex floats[3] = {
        {0.0f, 0.0f, 0.25f, 0.25f, 0.5f, 0.75f, 1.0f, 0.125f, 0.0f},
        {1.0f, 0.0f, 0.25f, -0.5f, 0.125f, 0.0f, 0.25f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.25f, 2.0f, 0.0f, -1.0f, 0.5f, 0.5f, 1.0f}};
    D9VertexElement elems[3] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT4, 0, D9_USAGE_COLOR, 0},
        {0, 28, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0}};
    D9Decl float_decl;
    assert(!d9_decl_init(&float_decl, elems, 3));
    D9Handle vb2 = upload_vb(d, floats, sizeof floats);
    bind_colour(d, vb2, sizeof floats[0]);
    assert(!d9_set_vertex_declaration(d, &float_decl));
    assert(!d9_begin_scene(d, 0xff000000));
    assert(!d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1));
    assert(!d9_present(d));
    st = d9_null_stats();
    assert(st->color_draws == 2 && st->last_color_count == 3);
    for (unsigned i = 0; i < 3; ++i) {
        assert(st->last_color_vertices[i].r == floats[i].r);
        assert(st->last_color_vertices[i].g == floats[i].g);
        assert(st->last_color_vertices[i].b == floats[i].b);
        assert(st->last_color_vertices[i].a == floats[i].a);
    }

    /* --- 4. the non-colour stream is unchanged -------------------------- */
    const D9Vertex plain[3] = {{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 1, 0, 0, 1}};
    D9Handle vb3 = upload_vb(d, plain, sizeof plain);
    D9Decl plain_decl;
    assert(!d9_decl_from_fvf(0x102,0, &plain_decl, &why));
    assert(!d9_set_stream_source(d, vb3, 0, sizeof plain[0]));
    bind_colour(d, vb3, sizeof plain[0]);
    assert(!d9_begin_scene(d, 0xff000000));
    unsigned colour_before = d9_null_stats()->color_draws;
    assert(!d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1));
    assert(!d9_present(d));
    st = d9_null_stats();
    assert(st->color_draws == colour_before);
    assert(st->draws == 3);

    d9_destroy_device(d);
    puts("d3d9 colour TEV/null smoke: PASS");
    return 0;
}
