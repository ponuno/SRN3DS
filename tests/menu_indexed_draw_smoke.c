/* Indexed menu-text draw contract.
 *
 * A 16-bit DrawIndexedPrimitiveUP batch with the certified A8 menu-text state
 * reaches draw_menu_indexed with the compact vertex stream and the exact
 * indices, including batches far above the 4,095-index UP expansion limit.
 * 32-bit indices and a backend without the callback keep the expanded
 * draw_menu path; a 16-bit source range above 65,536 or an expanded draw above
 * 4,096 output vertices is a named refusal.  DevCaps stay 0x00082000. */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_com_demo.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x,y,z; uint32_t color; float u,v; } MenuVertex;
_Static_assert(sizeof(MenuVertex) == 24, "menu vertex size");

static unsigned indexed_calls, menu_calls;static size_t seen_vertex_count, seen_index_count;
static uint16_t seen_indices[8];
static MenuVertex seen_source[8];
static D9ColorVertex seen_vertices[4];

static int record_indexed(void *ctx, const D9ColorVertex *vertices, size_t count,
                          const uint16_t *indices, size_t index_count,
                          const D9Matrix *clip, void *tex0, void *tex1,
                          const D9DrawState *state, const float (*constants)[4]) {
    (void)ctx; (void)constants;
    assert(vertices && indices && clip && tex0 && !tex1 && state);
    assert(state->pixel_shader &&
           (state->pixel_shader->backend_flags & D9TEV_PLAN_MENU_TEXT));
    assert(index_count % 3 == 0);
    for (size_t i = 0; i < index_count; ++i) assert(indices[i] < count);
    seen_vertex_count = count;
    seen_index_count = index_count;
    for (unsigned i = 0; i < 8 && i < index_count; ++i) seen_indices[i] = indices[i];
    for (unsigned i = 0; i < 4 && i < count; ++i) seen_vertices[i] = vertices[i];
    ++indexed_calls;
    return 0;
}

static int record_menu(void *ctx, const D9ColorVertex *vertices, size_t count,
                       const D9Matrix *clip, void *tex0, void *tex1,
                       const D9DrawState *state, const float (*constants)[4]) {
    (void)ctx; (void)clip; (void)tex0; (void)tex1; (void)constants;
    assert(vertices && state);
    seen_vertex_count = count;
    ++menu_calls;
    return 0;
}

static void setup_state(D9Device *d) {
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLOROP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAOP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_TEXCOORDINDEX, 0));
    D9VertexElement elems[3] = {
        {0, 0,  D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_COLOR,  0, D9_USAGE_COLOR, 0},
        {0, 16, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, elems, 3));
    assert(!d9_set_vertex_declaration(d, &decl));
    D9Handle tex = 0;
    assert(!d9_create_texture_ex(d, 8, 8, D9_A8, 1, D9_POOL_MANAGED, 0, 0, &tex));
    void *p = NULL; size_t pitch = 0;
    assert(!d9_texture_lock_level(d, tex, 0, &p, &pitch));
    memset(p, 0x80, pitch * 8);
    assert(!d9_texture_unlock_level(d, tex, 0));
    assert(!d9_bind_texture(d, 0, tex));
}

static void fill_vertices(MenuVertex *v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        v[i].x = (float)i;
        v[i].y = (float)(i * 2);
        v[i].z = 0.0f;
        v[i].color = 0xff000000u | i;
        v[i].u = (float)(i & 7) / 8.0f;
        v[i].v = (float)((i >> 3) & 7) / 8.0f;
    }
}

static void check_caps(D9Device *device) {
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, device)) {
        fprintf(stderr, "caps COM init failed: %s\n", demo.message);
        assert(0);
    }
    uint32_t caps_addr = 0x10010000u + 0x800u;
    uint32_t args[1] = { caps_addr };
    uint32_t result = 0;
    assert(!d9_com_demo_call(&demo, demo.device, 7, args, 1, &result));
    assert(result == 0);
    uint32_t caps = 0;
    assert(!guest_read32(&demo.memory, caps_addr + 28, &caps));
    assert(caps == 0x00082000u);
    assert(!d9_com_demo_destroy(&demo));
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.draw_menu = record_menu;
    ops.draw_menu_indexed = record_indexed;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 8u << 20);
    assert(d);
    setup_state(d);
    assert(!d9_begin_scene(d, 0));

    MenuVertex small[4];
    uint16_t small_idx[6] = { 0, 1, 2, 0, 2, 3 };
    fill_vertices(small, 4);
    assert(!d9_draw_indexed_primitive_up(d, D9_TRIANGLELIST, 0, 4, 2,
                                         small_idx, D9_INDEX16, small, 24));
    assert(indexed_calls == 1 && menu_calls == 0);
    assert(seen_vertex_count == 4 && seen_index_count == 6);
    assert(seen_indices[0] == 0 && seen_indices[1] == 1 && seen_indices[2] == 2 &&
           seen_indices[3] == 0 && seen_indices[4] == 2 && seen_indices[5] == 3);
    assert(seen_vertices[0].x == 0.0f && seen_vertices[1].x == 1.0f &&
           seen_vertices[3].x == 3.0f);
    assert(seen_vertices[0].r == 0.0f && seen_vertices[0].a == 1.0f);

    /* Far above the 4,095-index UP expansion: one compact call. */
    enum { BIG_VERTS = 6000, BIG_TRIS = 2000 };
    MenuVertex *big = malloc(BIG_VERTS * sizeof *big);
    uint16_t *big_idx = malloc(BIG_TRIS * 3u * sizeof *big_idx);
    assert(big && big_idx);
    fill_vertices(big, BIG_VERTS);
    for (unsigned i = 0; i < BIG_TRIS; ++i) {
        big_idx[i * 3] = (uint16_t)(i % BIG_VERTS);
        big_idx[i * 3 + 1] = (uint16_t)((i + 1) % BIG_VERTS);
        big_idx[i * 3 + 2] = (uint16_t)((i + 2) % BIG_VERTS);
    }
    assert(!d9_draw_indexed_primitive_up(d, D9_TRIANGLELIST, 0, BIG_VERTS,
                                         BIG_TRIS, big_idx, D9_INDEX16,
                                         big, 24));
    assert(indexed_calls == 2 && menu_calls == 0);
    assert(seen_vertex_count == BIG_VERTS);
    assert(seen_index_count == BIG_TRIS * 3u);

    /* 32-bit indices do not use the 16-bit callback; the expanded menu path
     * sees the six output vertices. */
    uint32_t idx32[6] = { 0, 1, 2, 0, 2, 3 };
    assert(!d9_draw_indexed_primitive_up(d, D9_TRIANGLELIST, 0, 4, 2,
                                         idx32, D9_INDEX32, small, 24));
    assert(indexed_calls == 2 && menu_calls == 1);
    assert(seen_vertex_count == 6);

    /* A 16-bit source range above 65,536 cannot be compacted, and expanding
     * 4,200 output vertices is refused by name. */
    enum { HUGE_VERTS = 65537, HUGE_TRIS = 1400 };
    MenuVertex *huge = malloc(HUGE_VERTS * sizeof *huge);
    uint16_t *huge_idx = malloc(HUGE_TRIS * 3u * sizeof *huge_idx);
    assert(huge && huge_idx);
    fill_vertices(huge, HUGE_VERTS);
    for (unsigned i = 0; i < HUGE_TRIS * 3u; ++i) huge_idx[i] = (uint16_t)(i % 65536u);
    int rc = d9_draw_indexed_primitive_up(d, D9_TRIANGLELIST, 0, HUGE_VERTS,
                                          HUGE_TRIS, huge_idx, D9_INDEX16,
                                          huge, 24);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "4,096-vertex limit") != NULL);
    assert(indexed_calls == 2 && menu_calls == 1);
    free(huge_idx); free(huge);
    free(big_idx); free(big);
    assert(!d9_end_scene(d));

    check_caps(d);
    d9_destroy_device(d);

    /* A backend without draw_menu_indexed keeps the expanded path for a
     * small 16-bit batch. */
    D9Backend plain = *d9_null_backend();
    plain.draw_menu = record_menu;
    plain.draw_menu_indexed = NULL;
    D9Device *p = d9_create_device(&plain, d9_null_context(), 8u << 20);
    assert(p);
    setup_state(p);
    assert(!d9_begin_scene(p, 0));
    indexed_calls = 0; menu_calls = 0;
    assert(!d9_draw_indexed_primitive_up(p, D9_TRIANGLELIST, 0, 4, 2,
                                         small_idx, D9_INDEX16, small, 24));
    assert(indexed_calls == 0 && menu_calls == 1 && seen_vertex_count == 6);
    assert(!d9_end_scene(p));
    d9_destroy_device(p);

    puts("menu indexed draw: exact 16-bit vertices/indices, >4095 indices, "
         "32-bit/64k fallbacks, named expansion limit, caps 0x00082000 PASS");
    return 0;
}
