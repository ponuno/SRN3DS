/* Menu text through the core draw_menu route, with a recording backend.
 *
 * An A8 texture + FVF/declaration COLOR0 + stage-0 MODULATE(TEXTURE,DIFFUSE)
 * must reach draw_menu with the certified MENU_TEXT plan, one texture unit,
 * texcoord0 and point/clamped sampler state.  A non-A8 texture or a second
 * texture on the same plan is a named refusal before the backend.
 */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_tev.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned a8_creates, menu_draws;

static int record_create_format(void *ctx, unsigned w, unsigned h,
                                unsigned levels, unsigned format, void **native) {
    if (format == D9_A8) ++a8_creates;
    return d9_null_backend()->texture_create_format(ctx, w, h, levels, format, native);
}
static int record_menu(void *ctx, const D9ColorVertex *vertices, size_t count,
                       const D9Matrix *clip, void *tex0, void *tex1,
                       const D9DrawState *state, const float (*constants)[4]) {
    (void)ctx; (void)clip; (void)constants;
    assert(vertices && count == 3 && tex0 && !tex1 && state && state->pixel_shader);
    assert(state->pixel_shader->backend_flags == D9TEV_PLAN_MENU_TEXT);
    assert(state->pixel_shader->stages == 1);
    assert(state->pixel_shader->textures == 1);
    assert(state->pixel_shader->uses_vertex_color == 1);
    /* A8 carries coverage only: RGB is the vertex colour, alpha is the mask. */
    assert(state->pixel_shader->stage[0].func_rgb == D9TEV_FUNC_REPLACE);
    assert(state->pixel_shader->stage[0].src_rgb[0] == D9TEV_SRC_PRIMARY);
    assert(state->pixel_shader->stage[0].func_a == D9TEV_FUNC_MODULATE);
    assert(state->pixel_shader->stage[0].src_a[0] == D9TEV_SRC_TEX0);
    assert(state->pixel_shader->stage[0].src_a[1] == D9TEV_SRC_PRIMARY);
    assert(state->address_u == 3 && state->address_v == 3);
    assert(state->mag_filter == 1 && state->min_filter == 1 &&
           state->mip_filter == 0);
    ++menu_draws;
    return 0;
}

static uint32_t make_texture(D9Device *d, unsigned format, unsigned w, unsigned h,
                             unsigned a8) {
    D9Handle tex = 0;
    int crc = d9_create_texture_ex(d, w, h, format, 1, D9_POOL_MANAGED, 0, 0, &tex);
    if (crc) fprintf(stderr, "create format=%u rc=%d: %s\n", format, crc,
                     d9_last_draw_diagnostic(d));
    assert(!crc);
    void *p = NULL; size_t pitch = 0;
    assert(!d9_texture_lock_level(d, tex, 0, &p, &pitch));
    uint32_t pixels[64];
    if (a8) {
        for (unsigned i = 0; i < 64; ++i) memset((uint8_t *)p + i, (int)(i * 3), 1);
    } else {
        for (unsigned i = 0; i < 64; ++i) pixels[i] = 0xff000000u | (i * 0x010203u);
        memcpy(p, pixels, sizeof pixels);
    }
    assert(!d9_texture_unlock_level(d, tex, 0));
    assert(pitch >= (a8 ? 8u : 32u));
    assert(!d9_bind_texture(d, 0, tex));
    return tex;
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.texture_create_format = record_create_format;
    ops.draw_menu = record_menu;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 4096);
    assert(d);

    D9Handle a8 = make_texture(d, D9_A8, 8, 8, 1);
    assert(a8_creates == 1);
    assert(!d9_set_unit_sampler(d, 0, 1, 3)); /* CLAMP U */
    assert(!d9_set_unit_sampler(d, 0, 2, 3)); /* CLAMP V */
    assert(!d9_set_unit_sampler(d, 0, 5, 1)); /* POINT min */
    assert(!d9_set_unit_sampler(d, 0, 6, 1)); /* POINT mag */
    assert(!d9_set_unit_sampler(d, 0, 7, 0)); /* no mip */
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLOROP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAOP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_TEXCOORDINDEX, 0));
    D9VertexElement elems[3] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
        {0, 20, D9_DECL_COLOR, 0, D9_USAGE_COLOR, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, elems, 3));
    assert(!d9_set_vertex_declaration(d, &decl));

    D9ColorVertex v[3] = {
        {0, 0, 0, 0, 0, 1, 0, 0, 1},
        {1, 0, 0, 1, 0, 0, 1, 0, 1},
        {0, 1, 0, 0, 1, 0, 0, 1, 1},
    };
    assert(!d9_begin_scene(d, 0));
    int rc = d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]);
    if (rc) fprintf(stderr, "A8 text draw rc=%d: %s\n", rc, d9_last_draw_diagnostic(d));
    assert(!rc && menu_draws == 1);
    assert(!d9_end_scene(d));

    /* A MENU_TEXT plan bound explicitly to a non-A8 texture is named. */
    assert(!d9_bind_texture(d, 0, 0));
    D9Handle rgba = make_texture(d, D9_A8R8G8B8, 8, 8, 0);
    (void)rgba;
    D9TevProgram text;
    char why[192];
    assert(!d9_tev_text_plan(&text, 1, 0, 0, why, sizeof why));
    assert(!d9_set_pixel_shader(d, &text));
    assert(!d9_begin_scene(d, 0));
    unsigned before = menu_draws;
    rc = d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "A8") != NULL);
    assert(menu_draws == before);
    assert(!d9_set_pixel_shader(d, NULL));
    assert(!d9_end_scene(d));

    /* The same text plan is one texture only. */
    assert(!d9_bind_texture(d, 0, a8));
    D9Handle a8b = make_texture(d, D9_A8, 8, 8, 1);
    assert(!d9_bind_texture(d, 1, a8b));
    assert(!d9_set_pixel_shader(d, &text));
    assert(!d9_begin_scene(d, 0));
    rc = d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "one texture") != NULL);
    assert(menu_draws == before);

    assert(!d9_end_scene(d));
    d9_destroy_device(d);
    puts("menu text draw plan: A8 unit, one texture, point/clamp, named refusals PASS");
    return 0;
}
