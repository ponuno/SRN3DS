/* Cube-map draw routing with a recording backend (no citro3d required).
 *
 * The core selects one D3D face per vertex and hands the backend the cube
 * native, the face array and a level-0 D9CubeView.  This test checks that
 * contract for all six faces, the single-stage MODULATE plan, the direction
 * table used by the PICA shader, and the named refusals for mip filtering
 * with incomplete levels, a second texture, a pixel shader and a backend
 * without draw_cube.  The PICA-shaped refusal is checked on a second device
 * whose ops have draw_cube == NULL.  DevCaps are covered by cube_rt_sampling;
 * this test changes no cap state. */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_cube_coords.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { float x,y,z,nx,ny,nz,u,v; uint32_t color; } CubeVertex;

#define EDGE 16u
#define PITCH (EDGE * 4u)

static unsigned draws, seen_faces[6];
static void *expected_native;
static int saw_expected_native;

static int record_cube(void *ctx, const D9ColorVertex *v, size_t n,
                       const unsigned *faces, const D9Matrix *clip,
                       const D9CubeView *cube, const D9DrawState *state) {
    (void)ctx; (void)clip;
    assert(v && n == 3 && faces && cube && state);
    assert(cube->native == expected_native);
    assert(cube->edge == EDGE && cube->levels >= 1);
    assert(cube->format == D9_A8R8G8B8);
    assert(cube->level_edge[0] == EDGE && cube->level_pitch[0] == PITCH);
    assert(state->stage_colorop == D9_TOP_MODULATE);
    assert(state->stage_colorarg1 == D9_TA_TEXTURE);
    assert(state->stage_colorarg2 == D9_TA_DIFFUSE);
    assert(state->stage_alphaop == D9_TOP_MODULATE);
    assert(state->stage_alphaarg1 == D9_TA_TEXTURE);
    assert(state->stage_alphaarg2 == D9_TA_DIFFUSE);
    assert(state->mip_filter == 0 && state->resident_mips == 1);
    assert(draws < 7);
    for (unsigned i = 0; i < 3; ++i) if (draws < 6) assert(faces[i] == draws);
    if (draws < 6) seen_faces[draws] = faces[0];
    ++draws;
    saw_expected_native = 1;
    return 0;
}

static int record_create_cube(void *ctx, unsigned edge, unsigned levels,
                              unsigned format, void **native) {
    int rc = d9_null_backend()->texture_create_cube(ctx, edge, levels, format, native);
    if (!rc) expected_native = *native;
    return rc;
}

static uint32_t make_cube(D9Device *d, unsigned edge, unsigned levels) {
    D9Handle h = 0;
    assert(!d9_create_cube_texture_ex(d, edge, levels, D9_A8R8G8B8,
                                      D9_POOL_MANAGED, 0, &h));
    uint32_t face[EDGE * EDGE];
    for (unsigned f = 0; f < 6; ++f) {
        for (unsigned i = 0; i < EDGE * EDGE; ++i) face[i] = 0xff000000u | (f << 16) | i;
        assert(!d9_cube_upload_face_level(d, h, f, 0, face, edge * 4u,
                                          (size_t)edge * edge * 4u));
    }
    assert(!d9_bind_cube_texture(d, 0, h));
    return h;
}

/* Independent inverse of the documented standard face mapping
 * (PICA t = 1 - v), used to prove the direction table is self-consistent. */
static void inverse(const float d[3], unsigned *face, float *u, float *v) {
    float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]);
    float ma, s, t;
    if (ax >= ay && ax >= az) {
        *face = d[0] >= 0 ? 0 : 1; ma = ax;
        s = *face == 0 ? -d[2] : d[2]; t = -d[1];
    } else if (ay >= az) {
        *face = d[1] >= 0 ? 2 : 3; ma = ay;
        s = d[0]; t = *face == 2 ? d[2] : -d[2];
    } else {
        *face = d[2] >= 0 ? 4 : 5; ma = az;
        s = *face == 4 ? d[0] : -d[0]; t = -d[1];
    }
    s /= ma; t /= ma;
    *u = (s + 1.0f) * 0.5f;
    *v = (1.0f - t) * 0.5f;
}

static void check_directions(void) {
    const float uvs[2][2] = {{0.25f,0.75f}, {0.9f,0.1f}};
    for (unsigned f = 0; f < 6; ++f) {
        for (unsigned i = 0; i < 2; ++i) {
            float d[3], u, v; unsigned back;
            d9_cube_direction(f, uvs[i][0], uvs[i][1], d);
            inverse(d, &back, &u, &v);
            assert(back == f);
            assert(fabsf(u - uvs[i][0]) < 1e-5f);
            assert(fabsf(v - uvs[i][1]) < 1e-5f);
        }
    }
}

static void setup_state(D9Device *d) {
    assert(!d9_set_unit_sampler(d, 0, 7, 0)); /* no mip */
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

static void one_face(D9Device *d, unsigned face) {
    CubeVertex v[3];
    float n[3] = {0,0,0};
    n[face / 2] = (face & 1) ? -1.0f : 1.0f;
    for (unsigned i = 0; i < 3; ++i) {
        v[i] = (CubeVertex){ -1.0f + (float)i, -1.0f + (float)(i & 1), 0,
                             n[0], n[1], n[2], 0.5f, 0.5f, 0xffffffffu };
    }
    assert(!d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, v, sizeof v[0]));
}

static uint32_t make_2d(D9Device *d) {
    D9Handle t = 0;
    assert(!d9_create_texture_ex(d, 8, 8, D9_A8R8G8B8, 1, D9_POOL_MANAGED, 0, 0, &t));
    void *p = NULL; size_t pitch = 0;
    assert(!d9_texture_lock_level(d, t, 0, &p, &pitch));
    memset(p, 0x5a, pitch * 8);
    assert(!d9_texture_unlock_level(d, t, 0));
    return t;
}

static int draw_face(D9Device *d) {
    return d9_draw_primitive_up(d, D9_TRIANGLELIST, 1,
        (CubeVertex[]){
            {-1,-1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
            { 1,-1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
            { 0, 1,0, 1,0,0, 0.5f,0.5f, 0xffffffffu},
        }, sizeof(CubeVertex));
}

int main(void) {
    check_directions();

    D9Backend ops = *d9_null_backend();
    ops.draw_cube = record_cube;
    ops.texture_create_cube = record_create_cube;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 1u << 20);
    assert(d);
    setup_state(d);
    make_cube(d, EDGE, 1);
    assert(!d9_begin_scene(d, 0));
    for (unsigned f = 0; f < 6; ++f) one_face(d, f);
    assert(draws == 6 && saw_expected_native);
    for (unsigned f = 0; f < 6; ++f) assert(seen_faces[f] == f);
    assert(!d9_end_scene(d));

    /* A multi-level cube whose mips are not uploaded is named; the same cube
     * with mip filter NONE still samples level 0. */
    make_cube(d, EDGE, 2);
    D9Handle tex = make_2d(d);
    assert(!d9_begin_scene(d, 0));
    assert(!d9_set_unit_sampler(d, 0, 7, 2)); /* LINEAR mip */
    unsigned before = draws;
    int rc = draw_face(d);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "all mip levels resident") != NULL);
    assert(draws == before);
    assert(!d9_set_unit_sampler(d, 0, 7, 0));
    assert(!draw_face(d));
    assert(draws == before + 1);

    /* A second texture unit on the same draw is named. */
    assert(!d9_bind_texture(d, 1, tex));
    before = draws;
    rc = draw_face(d);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "only texture stage 0") != NULL);
    assert(draws == before);
    assert(!d9_bind_texture(d, 1, 0));

    /* A pixel shader owns the TEV stages and is refused by name. */
    D9TevProgram ps;
    memset(&ps, 0, sizeof ps);
    assert(!d9_set_pixel_shader(d, &ps));
    rc = draw_face(d);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(d), "pixel shader") != NULL);
    assert(draws == before);
    assert(!d9_set_pixel_shader(d, 0));
    assert(!d9_end_scene(d));
    d9_destroy_device(d);

    /* PICA shape before this layer: no draw_cube callback -> named refusal,
     * no face-0 substitution and no backend draw. */
    D9Backend pica = *d9_null_backend();
    pica.draw_cube = NULL;
    pica.texture_create_cube = record_create_cube;
    D9Device *p = d9_create_device(&pica, d9_null_context(), 1u << 20);
    assert(p);
    setup_state(p);
    make_cube(p, EDGE, 1);
    assert(!d9_begin_scene(p, 0));
    unsigned pdraws = draws;
    rc = draw_face(p);
    assert(rc == D9_UNSUPPORTED);
    assert(strstr(d9_last_draw_diagnostic(p), "requires a backend draw_cube") != NULL);
    assert(draws == pdraws);
    assert(!d9_end_scene(p));
    d9_destroy_device(p);

    puts("cube draw plan: six D3D faces, MODULATE plan, direction table, "
         "named level/second-texture/pixel-shader/no-callback refusals PASS");
    return 0;
}
