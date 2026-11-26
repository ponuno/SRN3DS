/* Cube mip sampling through the core draw_cube contract.
 *
 * A 16x16 three-level cube with every face of every level uploaded may be
 * drawn with LINEAR/LINEAR/LINEAR; the backend sees mip_filter=LINEAR and
 * resident_mips=3.  The same sampler on a cube with only level 0 uploaded is
 * refused by name.  With MIPFILTER_NONE (point sampling) only level 0 is
 * selected (resident_mips=1).  DevCaps are unchanged by this layer; the COM
 * cube tests keep asserting 0x00082000. */
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { float x,y,z,nx,ny,nz,u,v; uint32_t color; } CubeVertex;

#define EDGE 16u
#define LEVELS 3u

static unsigned backend_draws;
static void *expected_native, *last_created_native;
static unsigned expect_mip_filter, expect_resident, expect_levels;

static int record_cube(void *ctx, const D9ColorVertex *v, size_t n,
                       const unsigned *faces, const D9Matrix *clip,
                       const D9CubeView *cube, const D9DrawState *state) {
    (void)ctx; (void)clip;
    assert(v && n == 3 && faces && cube && state);
    assert(cube->native == expected_native);
    assert(cube->edge == EDGE && cube->levels == expect_levels);
    assert(cube->format == D9_A8R8G8B8);
    assert(cube->level_edge[0] == EDGE && cube->level_edge[1] == 8 &&
           cube->level_edge[2] == 4);
    assert(state->mip_filter == expect_mip_filter);
    assert(state->resident_mips == expect_resident);
    assert(state->stage_colorop == D9_TOP_MODULATE);
    assert(state->stage_colorarg1 == D9_TA_TEXTURE);
    assert(state->stage_colorarg2 == D9_TA_DIFFUSE);
    assert(faces[0] == D9_CUBE_FACE_POSITIVE_X);
    ++backend_draws;
    return 0;
}

static int record_create_cube(void *ctx, unsigned edge, unsigned levels,
                              unsigned format, void **native) {
    int rc = d9_null_backend()->texture_create_cube(ctx, edge, levels, format, native);
    if (!rc) last_created_native = *native;
    return rc;
}

/* Upload all six faces of one level; full selects every level. */
static void upload_faces(D9Device *d, D9Handle h, unsigned levels) {
    static uint32_t pixels[EDGE * EDGE];
    for (unsigned level = 0; level < levels; ++level) {
        unsigned dim = EDGE >> level;
        size_t pitch = (size_t)dim * 4u;
        for (unsigned f = 0; f < 6; ++f) {
            for (unsigned i = 0; i < dim * dim; ++i)
                pixels[i] = 0xff000000u | (f << 16) | (level << 8) | i;
            assert(!d9_cube_upload_face_level(d, h, f, level, pixels, pitch,
                                              pitch * dim));
        }
    }
}

static D9Handle make_cube(D9Device *d, unsigned levels, unsigned uploaded_levels,
                          void **native_out) {
    D9Handle h = 0;
    assert(!d9_create_cube_texture_ex(d, EDGE, levels, D9_A8R8G8B8,
                                      D9_POOL_MANAGED, 0, &h));
    if (native_out) *native_out = last_created_native;
    upload_faces(d, h, uploaded_levels);
    return h;
}

static void setup_state(D9Device *d) {
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLOROP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_COLORARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAOP, D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG1, D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_ALPHAARG2, D9_TA_DIFFUSE));
    assert(!d9_set_texture_stage_state(d, 0, D9_TSS_TEXCOORDINDEX, 0));
    D9VertexElement elems[4] = {
        {0, 0,  D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT3, 0, D9_USAGE_NORMAL, 0},
        {0, 24, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
        {0, 32, D9_DECL_COLOR,  0, D9_USAGE_COLOR, 0},
    };
    D9Decl decl;
    assert(!d9_decl_init(&decl, elems, 4));
    assert(!d9_set_vertex_declaration(d, &decl));
}

static int draw_one(D9Device *d) {
    CubeVertex v[3] = {
        {-1,-1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
        { 1,-1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
        { 0, 1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
    };
    return d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]);
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.draw_cube = record_cube;
    ops.texture_create_cube = record_create_cube;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 1u << 20);
    assert(d);
    setup_state(d);

    void *full_native = NULL, *partial_native = NULL;
    D9Handle full = make_cube(d, LEVELS, LEVELS, &full_native);
    D9Handle partial = make_cube(d, LEVELS, 1, &partial_native);
    assert(full_native && partial_native);
    assert(!d9_bind_cube_texture(d, 0, full));
    assert(!d9_begin_scene(d, 0));

    /* Full mip chain + LINEAR_MIPMAP reaches the backend with mips enabled. */
    assert(!d9_set_unit_sampler(d, 0, 6, 2)); /* MIN  LINEAR */
    assert(!d9_set_unit_sampler(d, 0, 5, 2)); /* MAG  LINEAR */
    assert(!d9_set_unit_sampler(d, 0, 7, 2)); /* MIP  LINEAR */
    expected_native = full_native;
    expect_mip_filter = 2; expect_resident = LEVELS; expect_levels = LEVELS;
    unsigned before = backend_draws;
    assert(!draw_one(d));
    assert(backend_draws == before + 1);

    /* A cube with only level 0 uploaded cannot satisfy mip sampling. */
    assert(!d9_bind_cube_texture(d, 0, partial));
    expected_native = partial_native;
    before = backend_draws;
    int rc = draw_one(d);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "all mip levels resident") != NULL);
    assert(backend_draws == before);

    /* MIPFILTER_NONE keeps level 0 even when MIN/MAG are linear. */
    assert(!d9_set_unit_sampler(d, 0, 7, 0));
    expect_mip_filter = 0; expect_resident = 1; expect_levels = LEVELS;
    assert(!draw_one(d));
    assert(backend_draws == before + 1);

    /* POINT sampling also selects level 0 only. */
    assert(!d9_set_unit_sampler(d, 0, 6, 1));
    assert(!d9_set_unit_sampler(d, 0, 5, 1));
    expect_mip_filter = 0; expect_resident = 1;
    assert(!draw_one(d));
    assert(backend_draws == before + 2);

    /* The full cube with mip NONE is level 0 only as well. */
    assert(!d9_bind_cube_texture(d, 0, full));
    expected_native = full_native;
    expect_mip_filter = 0; expect_resident = 1;
    assert(!draw_one(d));
    assert(backend_draws == before + 3);

    assert(!d9_end_scene(d));
    d9_destroy_device(d);
    puts("cube mip sampling: full 3-level chain LINEAR_MIPMAP, level-0 point, "
         "named incomplete-mip refusal PASS");
    return 0;
}
