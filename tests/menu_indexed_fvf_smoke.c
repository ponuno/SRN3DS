/* Fixed-function indexed draw contract (DrawIndexedPrimitive with a bound
 * index buffer).
 *
 * Before this test the core refused ANY indexed draw above 1,365 triangles in
 * its first guard and returned D9_INVALID without writing a diagnosis.  GFx
 * menus batch thousands of fixed-function (FVF/D3DDECLUSAGE) triangles into one
 * call, so the real slice showed `refusal=DrawIndexedPrimitive: ` (empty) and
 * no geometry.  The contract now:
 *   - a 16/32-bit bound index buffer with the certified A8 menu-text state goes
 *     through draw_menu_indexed with the decoded vertex window and exact
 *     indices (base vertex folded in);
 *   - every refusal names its reason, so a real slice never again prints an
 *     empty diagnostic. */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_com_demo.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z, w; uint32_t color; float u, v; } FvfVertex;
_Static_assert(sizeof(FvfVertex) == 28, "XYZRHW|DIFFUSE|TEX1 vertex size");

static unsigned indexed_calls, menu_calls, colored_calls;
static size_t seen_vertex_count, seen_index_count;
static uint16_t seen_indices[16];
static D9ColorVertex seen_vertices[8];
static D9TevStage seen_stage0;

static int record_indexed(void *ctx, const D9ColorVertex *vertices, size_t count,
                          const uint16_t *indices, size_t index_count,
                          const D9Matrix *clip, void *tex0, void *tex1,
                          const D9DrawState *state, const float (*constants)[4]) {
    (void)ctx; (void)constants;
    assert(vertices && indices && clip && tex0 && !tex1 && state);
    /* Either the certified A8 text plan or the fixed-function
     * MODULATE(TEXTURE0,DIFFUSE) plan for a non-A8 atlas. */
    assert(state->pixel_shader);
    assert((state->pixel_shader->backend_flags & D9TEV_PLAN_MENU_TEXT) ||
           state->pixel_shader->backend_flags == 0);
    seen_stage0 = state->pixel_shader->stage[0];
    assert(index_count % 3 == 0);
    for (size_t i = 0; i < index_count; ++i) assert(indices[i] < count);
    seen_vertex_count = count;
    seen_index_count = index_count;
    for (unsigned i = 0; i < 16 && i < index_count; ++i) seen_indices[i] = indices[i];
    for (unsigned i = 0; i < 8 && i < count; ++i) seen_vertices[i] = vertices[i];
    ++indexed_calls;
    return 0;
}

static int record_menu(void *ctx, const D9ColorVertex *vertices, size_t count,
                       const D9Matrix *clip, void *tex0, void *tex1,
                       const D9DrawState *state, const float (*constants)[4]) {
    (void)ctx; (void)clip; (void)tex0; (void)tex1; (void)constants; (void)state;
    assert(vertices);
    seen_vertex_count = count;
    ++menu_calls;
    return 0;
}

static int record_colored(void *ctx, const D9ColorVertex *vertices, size_t count,
                          const D9Matrix *clip, void *tex0,
                          const D9DrawState *state) {
    (void)ctx; (void)clip; (void)tex0; (void)state;
    assert(vertices);
    seen_vertex_count = count;
    ++colored_calls;
    return 0;
}

static void setup_state(D9Device *d, unsigned texture_format) {
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLOROP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAOP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_TEXCOORDINDEX, 0));
    /* GFx 2D path: XYZRHW | DIFFUSE | TEX1 (FVF 0x144) through the same FVF
     * decoder the COM bridge uses. */
    D9Decl decl; const char *why = NULL;
    assert(!d9_decl_from_fvf(D9_FVF_XYZRHW_DIFFUSE_TEX1, 0, &decl, &why));
    assert(!d9_set_vertex_declaration(d, &decl));
    assert(!d9_set_viewport_size(d, 640, 480));
    D9Handle tex = 0;
    assert(!d9_create_texture_ex(d, 16, 16, texture_format, 1,
                                 D9_POOL_MANAGED, 0, 0, &tex));
    void *p = NULL; size_t pitch = 0;
    assert(!d9_texture_lock_level(d, tex, 0, &p, &pitch));
    memset(p, 0x80, pitch * 16);
    assert(!d9_texture_unlock_level(d, tex, 0));
    assert(!d9_bind_texture(d, 0, tex));
}

static void fill_vertices(FvfVertex *v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        v[i].x = 16.0f + (float)(i & 63);
        v[i].y = 16.0f + (float)((i >> 6) & 63);
        v[i].z = 0.5f;
        v[i].w = 1.0f;
        v[i].color = 0xff000000u | (i & 0xffu);
        v[i].u = (float)(i & 15) / 16.0f;
        v[i].v = (float)((i >> 4) & 15) / 16.0f;
    }
}

static int expect_named_refusal(D9Device *d, const char *what) {
    const char *why = d9_last_draw_diagnostic(d);
    if (!why || !*why) {
        fprintf(stderr, "FAIL: %s produced an empty draw diagnostic\n", what);
        return 1;
    }
    return 0;
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.draw_menu = record_menu;
    ops.draw_menu_indexed = record_indexed;
    ops.draw_colored = record_colored;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 16u << 20);
    assert(d);
    setup_state(d, D9_A8);

    /* GFx-sized batch: 6,000 vertices / 2,000 triangles in one bound 16-bit
     * index buffer.  Far above the old silent 1,365-primitive guard. */
    enum { VERTS = 6000, TRIS = 2000, INDICES = TRIS * 3 };
    FvfVertex *vertices = malloc(VERTS * sizeof *vertices);
    uint16_t *indices = malloc(INDICES * sizeof *indices);
    assert(vertices && indices);
    fill_vertices(vertices, VERTS);
    for (unsigned i = 0; i < TRIS; ++i) {
        indices[i * 3] = (uint16_t)(i % VERTS);
        indices[i * 3 + 1] = (uint16_t)((i + 7) % VERTS);
        indices[i * 3 + 2] = (uint16_t)((i + 11) % VERTS);
    }
    /* A in-range run for the BaseVertexIndex/MinVertexIndex fold below. */
    for (unsigned i = 0; i < 6; ++i) indices[30 + i] = (uint16_t)(200 + i);
    D9Handle vb = 0, ib = 0;
    assert(!d9_create_vertex_buffer(d, VERTS * sizeof *vertices, &vb));
    assert(!d9_create_index_buffer(d, INDICES * sizeof *indices, D9_INDEX16, &ib));
    void *p = NULL;
    assert(!d9_buffer_lock(d, vb, 0, VERTS * sizeof *vertices, &p));
    memcpy(p, vertices, VERTS * sizeof *vertices);
    assert(!d9_buffer_unlock(d, vb));
    assert(!d9_buffer_lock(d, ib, 0, INDICES * sizeof *indices, &p));
    memcpy(p, indices, INDICES * sizeof *indices);
    assert(!d9_buffer_unlock(d, ib));
    assert(!d9_set_stream_source(d, vb, 0, sizeof *vertices));
    assert(!d9_set_indices(d, ib));
    assert(!d9_begin_scene(d, 0));
    assert(!d9_draw_indexed_range(d, D9_TRIANGLELIST, 0, 0, VERTS, 0, TRIS));
    assert(indexed_calls == 1 && menu_calls == 0);
    assert(seen_vertex_count == VERTS && seen_index_count == INDICES);
    for (unsigned i = 0; i < 16; ++i) assert(seen_indices[i] == indices[i]);
    assert(seen_vertices[0].x == vertices[0].x);
    assert(seen_vertices[3].u == vertices[3].u);
    printf("indexed FVF: %u vertices / %u indices reach draw_menu_indexed\n",
           VERTS, INDICES);

    /* BaseVertexIndex and MinVertexIndex fold into the compact callback: the
     * decoded window is [base+min, base+min+count) and callback indices stay
     * relative to it. */
    assert(!d9_draw_indexed_range(d, D9_TRIANGLELIST, 100, 200, 600, 30, 2));
    assert(indexed_calls == 2);
    assert(seen_vertex_count == 600 && seen_index_count == 6);
    assert(seen_vertices[0].x == vertices[300].x);
    assert(seen_indices[0] == 0 && seen_indices[5] == 5);

    /* Real refusals are named, never empty. */
    int bad = 0;
    /* index outside MinVertexIndex/NumVertices */
    assert(d9_draw_indexed_range(d, D9_TRIANGLELIST, 0, 0, 10, 0, 2) == D9_INVALID);
    bad += expect_named_refusal(d, "index outside MinVertexIndex/NumVertices");
    /* primitive count above the indexed draw bound */
    assert(d9_draw_indexed_range(d, D9_TRIANGLELIST, 0, 0, 4, 0, 90000u) == D9_INVALID);
    bad += expect_named_refusal(d, "primitive count above the draw bound");
    /* an index range past the end of the bound index buffer */
    assert(d9_draw_indexed_range(d, D9_TRIANGLELIST, 0, 0, 4, INDICES - 3, 2) == D9_INVALID);
    bad += expect_named_refusal(d, "index range past the bound index buffer");
    assert(!d9_end_scene(d));
    d9_destroy_device(d);
    free(indices); free(vertices);

    /* A non-A8 atlas takes the fixed-function MODULATE plan, so a GFx-sized
     * DXT menu batch still compacts.  A stage shape with no certified plan is
     * refused by name instead (see below). */
    D9Backend plain = *d9_null_backend();
    plain.draw_menu = record_menu;
    plain.draw_colored = record_colored;
    plain.draw_menu_indexed = record_indexed;
    D9Device *r = d9_create_device(&plain, d9_null_context(), 16u << 20);
    assert(r);
    setup_state(r, D9_A8R8G8B8);
    enum { RV = 6000, RT = 2000 };
    FvfVertex *rv = malloc(RV * sizeof *rv);
    uint16_t *ri = malloc(RT * 3u * sizeof *ri);
    assert(rv && ri);
    fill_vertices(rv, RV);
    for (unsigned i = 0; i < RT * 3u; ++i) ri[i] = (uint16_t)(i % RV);
    D9Handle rvb = 0, rib = 0;
    assert(!d9_create_vertex_buffer(r, RV * sizeof *rv, &rvb));
    assert(!d9_create_index_buffer(r, RT * 3u * sizeof *ri, D9_INDEX16, &rib));
    assert(!d9_buffer_lock(r, rvb, 0, RV * sizeof *rv, &p));
    memcpy(p, rv, RV * sizeof *rv);
    assert(!d9_buffer_unlock(r, rvb));
    assert(!d9_buffer_lock(r, rib, 0, RT * 3u * sizeof *ri, &p));
    memcpy(p, ri, RT * 3u * sizeof *ri);
    assert(!d9_buffer_unlock(r, rib));
    assert(!d9_set_stream_source(r, rvb, 0, sizeof *rv));
    assert(!d9_set_indices(r, rib));
    assert(!d9_begin_scene(r, 0));
    indexed_calls = 0;
    assert(!d9_draw_indexed_range(r, D9_TRIANGLELIST, 0, 0, RV, 0, RT));
    assert(indexed_calls == 1 && seen_vertex_count == RV &&
           seen_index_count == RT * 3u);
    assert(!d9_end_scene(r));
    /* The GFx mask shape seen on the real slice: stage-0 COLOROP=DISABLE with
     * ALPHAOP=SELECTARG1(TEXTURE).  It compacts with the fixed-function plan
     * (diffuse colour, texture alpha). */
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_TEXCOORDINDEX, 0));
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_COLOROP, D9_TOP_DISABLE));
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_ALPHAOP, D9_TOP_SELECTARG1));
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_ALPHAARG1, D9_TA_TEXTURE));
    assert(!d9_begin_scene(r, 0));
    indexed_calls = 0;
    assert(!d9_draw_indexed_range(r, D9_TRIANGLELIST, 0, 0, RV, 0, RT));
    assert(indexed_calls == 1);
    assert(seen_stage0.func_rgb == D9TEV_FUNC_REPLACE &&
           seen_stage0.src_rgb[0] == D9TEV_SRC_PRIMARY);
    assert(seen_stage0.func_a == D9TEV_FUNC_REPLACE &&
           seen_stage0.src_a[0] == D9TEV_SRC_TEX0);
    assert(!d9_end_scene(r));
    /* TEXCOORDINDEX 1 has no single-TEV texcoord in this port: the expanded
     * path is bounded and the refusal names the 4,096-vertex limit. */
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_TEXCOORDINDEX, 1));
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_COLOROP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(r, 0, D9_TSS_ALPHAOP, D9_TOP_MODULATE));
    assert(!d9_begin_scene(r, 0));
    indexed_calls = 0;
    int rc = d9_draw_indexed_range(r, D9_TRIANGLELIST, 0, 0, RV, 0, RT);
    assert(rc == D9_UNSUPPORTED);
    assert(indexed_calls == 0);
    if (!strstr(d9_last_draw_diagnostic(r), "4,096-vertex limit")) {
        fprintf(stderr, "FAIL: uncertified stage refusal did not name the expansion limit: %s\n",
                d9_last_draw_diagnostic(r));
        ++bad;
    }
    assert(!d9_end_scene(r));
    /* No index buffer bound: named, not empty. */
    assert(!d9_begin_scene(r, 0));
    assert(!d9_set_indices(r, 0));
    assert(d9_draw_indexed_range(r, D9_TRIANGLELIST, 0, 0, 4, 0, 2) == D9_INVALID);
    bad += expect_named_refusal(r, "unbound index buffer");
    /* No stream zero: named, not empty. */
    assert(!d9_set_indices(r, rib));
    assert(!d9_set_stream_source(r, 0, 0, 0));
    assert(d9_draw_indexed_range(r, D9_TRIANGLELIST, 0, 0, 4, 0, 2) == D9_INVALID);
    bad += expect_named_refusal(r, "unbound stream zero");
    assert(!d9_end_scene(r));
    d9_destroy_device(r);
    free(ri); free(rv);

    if (bad) {
        printf("menu indexed fvf: %d failures\n", bad);
        return 1;
    }
    puts("menu indexed fvf: 6,000-vertex bound-IB draw compacts, base/min folded, "
         "every refusal named PASS");
    return 0;
}
