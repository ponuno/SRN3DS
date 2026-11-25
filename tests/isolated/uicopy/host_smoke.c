/* Host mock-backend regression for the isolated ui.copy submission path. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

static int checks, errors;
#define CHECK(condition, what) do { \
    ++checks; \
    if (!(condition)) { ++errors; printf("FAIL: %s\n", what); } \
} while (0)

typedef struct {
    unsigned width, height, levels, uploads;
    uint8_t tiled[UICOPY_TEX_SIZE * UICOPY_TEX_SIZE * 4u];
} MockTex;

typedef struct {
    int active;
    unsigned begins, ends, draws;
    void *native;
    D9Vertex vertices[3];
    D9Matrix clip;
    D9DrawState state;
    void *bound_texture;
    const D9TevProgram *shader;
} Mock;

static int mock_tex_create(void *ctx, unsigned w, unsigned h, unsigned levels,
                           void **native) {
    Mock *m = ctx;
    MockTex *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->width = w; t->height = h; t->levels = levels;
    m->native = t;
    *native = t;
    return 0;
}

static int mock_tex_upload(void *ctx, void *native, unsigned level,
                           const void *data, size_t bytes) {
    (void)ctx;
    MockTex *t = native;
    if (!t || level || bytes != sizeof t->tiled) return -1;
    memcpy(t->tiled, data, bytes);
    ++t->uploads;
    return 0;
}

static void mock_tex_delete(void *ctx, void *native) {
    Mock *m = ctx;
    assert(!m->active);
    if (m->native == native) m->native = NULL;
    free(native);
}

static int mock_begin(void *ctx, uint32_t argb) {
    Mock *m = ctx;
    (void)argb;
    assert(!m->active);
    m->active = 1;
    ++m->begins;
    return 0;
}

static int mock_draw(void *ctx, const D9Vertex *vertices, size_t count,
                     const D9Matrix *clip, void *texture,
                     const D9DrawState *state) {
    Mock *m = ctx;
    assert(m->active && vertices && count == 3 && clip && state);
    memcpy(m->vertices, vertices, sizeof m->vertices);
    m->clip = *clip;
    m->state = *state;
    m->bound_texture = texture;
    m->shader = state->pixel_shader;
    ++m->draws;
    return 0;
}

static void mock_end(void *ctx) {
    Mock *m = ctx;
    assert(m->active);
    m->active = 0;
    ++m->ends;
}

static const D9Backend mock_backend = {
    mock_tex_create, mock_tex_upload, mock_tex_delete,
    mock_begin, mock_draw, mock_end,
    NULL, NULL, NULL
};

static int same_float(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    uint32_t code[16];
    size_t words = uicopy_tokens(code);
    D9TevProgram plan;
    char why[256];
    CHECK(uicopy_validate(code, words, &plan, why, sizeof why) == 0, why);
    CHECK(plan.version == 0xFFFF0300u && plan.instructions == 3,
          "exact ps_3_0 three-instruction copy");
    CHECK(plan.stages == 1 && plan.textures == 1 && plan.inputs == 1 &&
          plan.constants == 0 && !plan.uses_vertex_color,
          "one texture, texcoord0, no constants or vertex colour");
    CHECK(plan.stage[0].func_rgb == D9TEV_FUNC_REPLACE &&
          plan.stage[0].func_a == D9TEV_FUNC_REPLACE &&
          plan.stage[0].src_rgb[0] == D9TEV_SRC_TEX0 &&
          plan.stage[0].src_a[0] == D9TEV_SRC_TEX0,
          "one REPLACE(TEXTURE0) stage");

    Mock mock;
    memset(&mock, 0, sizeof mock);
    D9Device *core = d9_create_device(&mock_backend, &mock, 4u << 20);
    CHECK(core != NULL, "core device creation");
    UICopyCom com;
    memset(&com, 0, sizeof com);
    int attached = core && uicopy_com_attach(&com, core) == 0;
    CHECK(attached, attached ? "COM attach" : com.error);
    int resources = attached && uicopy_com_create_resources(&com, code, words) == 0;
    CHECK(resources, resources ? "COM resources and CreatePixelShader" : com.error);
    if (!resources) {
        uicopy_com_detach(&com);
        d9_destroy_device(core);
        return 1;
    }

    D9TevProgram retained;
    CHECK(d9_com_pixel_shader_tev(com.shader, &retained), "COM retained TEV program");
    CHECK(memcmp(&retained, &plan, sizeof plan) == 0,
          "COM TEV equals analyzed ui.copy plan");
    CHECK(d9_com_last_shader_rejection()[0] == 0, "no shader rejection");

    uint32_t queried = 0, out_address = UICOPY_OUT;
    CHECK(guest_write32(&com.memory, UICOPY_OUT, 0) == 0, "clear GetPixelShader output");
    CHECK(uicopy_com_call(&com, com.device, 108, &out_address, 1) == 0,
          "GetPixelShader");
    CHECK(guest_read32(&com.memory, UICOPY_OUT, &queried) == 0 &&
          queried == com.shader, "GetPixelShader returns submitted object");
    uint32_t queried_refs = 99;
    CHECK(uicopy_com_release_result(&com, queried, &queried_refs) == 0 &&
          queried_refs == 2, "release GetPixelShader reference");

    CHECK(uicopy_com_frame(&com) == 0, com.error);
    CHECK(mock.draws == 1 && mock.begins == 1 && mock.ends == 1 && !mock.active,
          "one draw and one Present");
    CHECK(mock.shader != NULL, "pixel shader pointer reached backend");
    CHECK(mock.shader->stages == 1 && mock.shader->textures == 1 &&
          mock.shader->stage[0].func_rgb == D9TEV_FUNC_REPLACE &&
          mock.shader->stage[0].src_rgb[0] == D9TEV_SRC_TEX0,
          "backend received ui.copy TEV descriptor");
    const D9TevProgram *first_pointer = mock.shader;
    CHECK(mock.bound_texture != NULL && mock.bound_texture == mock.native,
          "unit-0 native texture reached backend");
    CHECK(mock.state.resident_mips == 1 && mock.state.mag_filter == 1 &&
          mock.state.min_filter == 1 && mock.state.mip_filter == 0 &&
          mock.state.address_u == 1 && mock.state.address_v == 1,
          "point/clamp sampler state propagated");
    CHECK(mock.state.z_enable == 0 && mock.state.cull == 1 &&
          mock.state.blend == 0 && mock.state.color_write == 15,
          "draw render state propagated");

    D9Vertex expected_vertices[3];
    uicopy_vertices(expected_vertices);
    CHECK(memcmp(mock.vertices, expected_vertices, sizeof expected_vertices) == 0,
          "position+texcoord0 fullscreen triangle propagated");
    D9Matrix identity, expected_clip;
    d9_matrix_identity(&identity);
    d9_pica_transform(&expected_clip, &identity, &identity, &identity);
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            CHECK(same_float(mock.clip.m[i][j], expected_clip.m[i][j]),
                  "PICA clip matrix propagated");

    uint32_t argb[UICOPY_TEXELS];
    uint8_t expected_tiled[UICOPY_TEX_SIZE * UICOPY_TEX_SIZE * 4u];
    uicopy_texels(argb);
    CHECK(d9_swizzle_rgba8(expected_tiled, sizeof expected_tiled, argb,
                           UICOPY_TEX_SIZE * 4u, UICOPY_TEX_SIZE,
                           UICOPY_TEX_SIZE, D9_A8R8G8B8) == 0,
          "expected PICA swizzle generated");
    MockTex *native = mock.native;
    CHECK(native && native->width == 8 && native->height == 8 &&
          native->levels == 1 && native->uploads == 1,
          "one 8x8 texture upload");
    CHECK(native && memcmp(native->tiled, expected_tiled, sizeof expected_tiled) == 0,
          "known distinct texels uploaded in PICA layout");

    CHECK(uicopy_com_frame(&com) == 0, com.error);
    CHECK(mock.draws == 2 && mock.begins == 2 && mock.ends == 2,
          "second draw/Present counted");
    CHECK(mock.shader == first_pointer, "borrowed shader pointer is stable");

    CHECK(uicopy_com_set_pixel_shader(&com, 0) == 0, "SetPixelShader(NULL)");
    CHECK(uicopy_com_frame(&com) == 0, com.error);
    CHECK(mock.draws == 3 && mock.shader == NULL && mock.bound_texture == mock.native,
          "NULL shader restores fixed path without losing texture");
    uint32_t refs = 99;
    CHECK(uicopy_com_release_shader(&com, &refs) == 0 && refs == 0,
          "released shader object reaches zero references");
    CHECK(uicopy_com_set_texture(&com, 0) == 0, "SetTexture(NULL)");
    CHECK(uicopy_com_frame(&com) == 0, com.error);
    CHECK(mock.draws == 4 && mock.begins == 4 && mock.ends == 4 &&
          mock.shader == NULL && mock.bound_texture == NULL && !mock.active,
          "final unbound draw state and counters");

    uicopy_com_detach(&com);
    d9_destroy_device(core);
    CHECK(mock.native == NULL, "native texture released");

    printf("ui.copy host: %d checks, %d errors\n", checks, errors);
    return errors ? 1 : 0;
}
