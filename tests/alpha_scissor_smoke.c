/* Host test: alpha-0 draws are not dropped, scissor state reaches the backend,
 * and D3D9 separate-alpha blend/alpha-test/colour-write state is preserved.
 *
 * The port has no alpha-value draw gate: D3D9 submits a draw regardless of a
 * vertex/texture alpha of zero; blending, not the host, decides visibility.
 * This test proves a fully transparent colour vertex still reaches the backend
 * and that the blend/scissor contracts survive the core. */

#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { float x, y, z; uint32_t color; } ColorVertex;

static void set_decl(D9Device *d) {
    D9VertexElement e[2] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_COLOR, 0, D9_USAGE_COLOR, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, e, 2));
    assert(!d9_set_vertex_declaration(d, &decl));
}

static void expect_rect(int offscreen, int l, int t, int r, int b,
                        unsigned vw, unsigned vh, unsigned sw, unsigned sh,
                        unsigned wl, unsigned wt, unsigned wr, unsigned wb) {
    unsigned a = 0, c = 0, e = 0, f = 0;
    assert(!d9_scissor_window_rect(offscreen, l, t, r, b, vw, vh, sw, sh,
                                   &a, &c, &e, &f));
    assert(a == wl && c == wt && e == wr && f == wb);
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    D9Device *d = d9_create_device(&ops, NULL, 1u << 20);
    assert(d);
    assert(!d9_set_back_buffer_info_ex(d, 400, 240, D9_X8R8G8B8, 0));
    const D9NullStats *s = d9_null_stats();
    assert(!d9_begin_scene(d, 0xff000000u));
    set_decl(d);

    /* --- scissor ---------------------------------------------------------- */
    assert(d9_scissor_supported(d));
    assert(!d9_set_scissor(d, 0, 0, 400, 240));      /* full: pushed disabled */
    assert(s->scissor_enable == 0);
    assert(!d9_set_scissor_enable(d, 1));            /* full rect: still disabled */
    assert(s->scissor_enable == 0);
    assert(!d9_set_scissor(d, 100, 50, 300, 200));   /* enabled + real clip */
    assert(s->scissor_calls > 0);
    assert(s->scissor_enable == 1);
    assert(s->scissor_rect[0] == 100 && s->scissor_rect[1] == 50 &&
           s->scissor_rect[2] == 300 && s->scissor_rect[3] == 200);
    assert(s->scissor_surface[0] == 400 && s->scissor_surface[1] == 240);
    /* Window mapping is the inverse of the geometry permutation.  On the
     * rotated screen a D3D rect becomes the transposed/flipped window rect. */
    expect_rect(0, 100, 50, 300, 200, 400, 240, 400, 240, 67, 60, 317, 180);
    /* Offscreen targets use the orient_clip permutation (no flip). */
    expect_rect(1, 100, 50, 300, 200, 512, 512, 400, 240, 107, 128, 427, 384);
    /* A point outside the D3D rect maps outside the mapped window rect, so the
     * PICA scissor rejects every fragment it would produce. */
    {
        unsigned pl = 0, pt = 0, pr = 0, pb = 0, wl = 0, wt = 0, wr = 0, wb = 0;
        /* d3d (10,10) on screen -> window (383,234) */
        assert(!d9_scissor_window_rect(0, 100, 50, 300, 200, 400, 240, 400, 240,
                                       &wl, &wt, &wr, &wb));
        pl = 383; pt = 234;
        assert(pl < wl || pl >= wr || pt < wt || pt >= wb);
        (void)pr; (void)pb;
    }
    assert(!d9_set_scissor_enable(d, 0));
    assert(s->scissor_enable == 0);

    /* --- blend / alpha-test / colour-write -------------------------------- */
    assert(!d9_set_render_state(d, 27, 1));    /* ALPHABLENDENABLE */
    assert(!d9_set_render_state(d, 19, 5));    /* SRCBLEND = SRCALPHA */
    assert(!d9_set_render_state(d, 20, 6));    /* DESTBLEND = INVSRCALPHA */
    assert(!d9_set_render_state(d, 206, 1));   /* SEPARATEALPHABLENDENABLE */
    assert(!d9_set_render_state(d, 207, 2));   /* SRCBLENDALPHA = ONE */
    assert(!d9_set_render_state(d, 208, 1));   /* DESTBLENDALPHA = ZERO */
    assert(!d9_set_render_state(d, 209, 2));   /* BLENDOPALPHA = SUBTRACT */
    assert(!d9_set_render_state(d, 15, 1));    /* ALPHATESTENABLE */
    assert(!d9_set_render_state(d, 25, 5));    /* ALPHAFUNC = GREATER */
    assert(!d9_set_render_state(d, 24, 8));    /* ALPHAREF */
    assert(!d9_set_render_state(d, 168, 0x5)); /* COLORWRITEENABLE = RGBA */
    unsigned v = 0;
    assert(!d9_get_render_state(d, 206, &v) && v == 1);
    assert(!d9_get_render_state(d, 207, &v) && v == 2);
    assert(!d9_get_render_state(d, 208, &v) && v == 1);
    assert(!d9_get_render_state(d, 209, &v) && v == 2);
    D9DrawState st;
    assert(!d9_get_draw_state(d, &st));
    assert(st.separate_alpha_blend == 1 && st.src_blend_alpha == 2 &&
           st.dst_blend_alpha == 1 && st.blend_op_alpha == 2);
    unsigned opr, sr, dr, opa, sa, da;
    d9_resolve_blend(&st, &opr, &sr, &dr, &opa, &sa, &da);
    assert(opr == 1 && sr == 5 && dr == 6);
    assert(opa == 2 && sa == 2 && da == 1);
    /* Disabling separate alpha replicates the RGB pair, exactly like D3D9. */
    st.separate_alpha_blend = 0;
    d9_resolve_blend(&st, &opr, &sr, &dr, &opa, &sa, &da);
    assert(opa == 1 && sa == 5 && da == 6);

    /* --- alpha-0 draw is submitted ---------------------------------------- */
    ColorVertex quad[3] = {
        {-1, -1, 0, 0x00000000u}, /* ARGB alpha 0, fully transparent */
        { 1, -1, 0, 0x00ffffffu},
        { 0,  1, 0, 0x00ffffffu},
    };
    unsigned before = s->color_draws;
    assert(!d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, quad, sizeof quad[0]));
    assert(s->color_draws == before + 1);
    assert(s->last_color_count >= 1);
    assert(s->last_color_vertices[0].a == 0.0f);
    assert(s->last_color_state.alpha_test == 1);
    assert(s->last_color_state.alpha_func == 5);
    assert(s->last_color_state.alpha_ref == 8);
    assert(s->last_color_state.color_write == 5);
    assert(s->last_color_state.separate_alpha_blend == 1);

    assert(!d9_end_scene(d));
    d9_destroy_device(d);
    puts("alpha_scissor: alpha-0 draw submitted; scissor and blend state verified");
    return 0;
}
