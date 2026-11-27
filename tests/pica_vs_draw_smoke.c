/* Host test: verified PICA VS skinned draw path.
 *
 * Uses the embedded emitted .shbin, a synthetic vertex buffer and a D3D9
 * declaration with POSITION0/BLENDWEIGHT0/BLENDINDICES0.  Checks the 256-row
 * uniform bank and dirty mask, the packed vertex layout, the decode into
 * D9SkinVertex, the one-call backend submit, and that the gate refuses every
 * missing part by name (no program/.shbin, no declaration, missing bank rows,
 * no backend callback).  No device or GPU is opened.
 */

#include "d3d9_core.h"
#include "d3d9_pica_vs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SKIN_F8FFAECD_SHA \
    "f8ffaecd431d4416a4503b2e8ca80b6dad57a128dc1782c941f97db346072efa"

static int checks, failures;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { \
    ++failures; printf("FAIL: %s (%s:%d)\n", text, __FILE__, __LINE__); \
} } while (0)

typedef struct { float x, y, z; float weight[4]; uint8_t index[4]; } SkinVertex;
_Static_assert(sizeof(SkinVertex) == 32, "synthetic skinned vertex is 32 bytes");

typedef struct {
    int begin_calls, draw_calls, pica_calls, pica_reject;
    size_t count;
    D9SkinVertex vertex[8];
    D9PicaVsDraw draw;
    char why[256];
} TestCtx;

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels, void **native) {
    (void)ctx; (void)w; (void)h; (void)levels;
    if (native) *native = NULL;
    return -1;
}
static int tex_upload(void *ctx, void *native, unsigned level, const void *data, size_t bytes) {
    (void)ctx; (void)native; (void)level; (void)data; (void)bytes;
    return -1;
}
static void tex_delete(void *ctx, void *native) { (void)ctx; (void)native; }
static int t_begin(void *ctx, uint32_t argb) { (void)argb; ++((TestCtx *)ctx)->begin_calls; return 0; }
static int t_draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *clip,
                  void *texture, const D9DrawState *state) {
    (void)v; (void)n; (void)clip; (void)texture; (void)state;
    ++((TestCtx *)ctx)->draw_calls;
    return 0;
}
static void t_end(void *ctx) { (void)ctx; }
static int t_pica_vs(void *ctx, const D9SkinVertex *v, size_t n,
                     const D9PicaVsDraw *draw) {
    TestCtx *t = ctx;
    ++t->pica_calls;
    t->count = n;
    if (n > 8 || !draw) { t->pica_reject = 1; return -1; }
    memcpy(t->vertex, v, n * sizeof *v);
    t->draw = *draw;
    return 0;
}

static void base_backend(D9Backend *ops) {
    memset(ops, 0, sizeof *ops);
    ops->texture_create = tex_create;
    ops->texture_upload = tex_upload;
    ops->texture_delete = tex_delete;
    ops->begin = t_begin;
    ops->draw = t_draw;
    ops->end = t_end;
}

static void skin_decl(D9Decl *decl, unsigned index_type, int with_weight) {
    D9VertexElement e[3];
    unsigned n = 0;
    memset(e, 0, sizeof e);
    e[n++] = (D9VertexElement){0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0};
    if (with_weight)
        e[n++] = (D9VertexElement){0, 12, D9_DECL_FLOAT4, 0, D9_USAGE_BLENDWEIGHT, 0};
    e[n++] = (D9VertexElement){0, 28, (uint8_t)index_type, 0, D9_USAGE_BLENDINDICES, 0};
    char why[160] = "";
    CHECK(d9_decl_init_why(decl, e, n, why, sizeof why) == 0, why);
}

static void fill_triangle(SkinVertex v[3]) {
    for (unsigned i = 0; i < 3; ++i) {
        v[i].x = 10.0f + (float)i;
        v[i].y = 20.0f + (float)i;
        v[i].z = 30.0f + (float)i;
        for (unsigned j = 0; j < 4; ++j) v[i].weight[j] = 0.1f * (float)(i + 1) + 0.01f * (float)j;
        v[i].index[0] = (uint8_t)(i % 4);
        v[i].index[1] = (uint8_t)((i + 1) % 4);
        v[i].index[2] = (uint8_t)((i + 2) % 4);
        v[i].index[3] = (uint8_t)((i + 3) % 4);
    }
}

static void set_contract(D9Device *d, const D9PicaVsProgram *program,
                         const D9Decl *decl, D9PicaVsDecl *out_decl,
                         D9PicaVsBank *out_bank) {
    char why[192] = "";
    CHECK(d9_pica_vs_declare(program, decl, out_decl, why, sizeof why) == 0, why);
    CHECK(d9_pica_vs_bank(program, out_bank, why, sizeof why) == 0, why);
    CHECK(d9_set_vertex_declaration(d, decl) == D9_OK, "declaration binds");
    CHECK(d9_set_pica_vs(d, program, out_decl, out_bank) == D9_OK, "contract records");
}

static void upload_bank(D9Device *d) {
    float rows[16][4];
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 4; ++j) rows[i][j] = (float)i + 0.25f * (float)j;
    CHECK(d9_set_vs_constants_f(d, 0, rows, 12) == D9_OK, "palette c0..c11 writes");
    CHECK(d9_set_vs_constants_f(d, 180, rows + 12, 4) == D9_OK, "WVP c180..c183 writes");
}

int main(void) {
    const D9PicaVsProgram *program = d9_pica_vs_lookup(SKIN_F8FFAECD_SHA);
    CHECK(program != NULL, "embedded emitted program is present");

    /* Declaration stores the skin inputs with type/stride. */
    D9Decl decl;
    skin_decl(&decl, D9_DECL_UBYTE4N, 1);
    CHECK(decl.weight == 1u, "BLENDWEIGHT0 element index is recorded");
    CHECK(decl.index == 2u, "BLENDINDICES0 element index is recorded");
    CHECK(decl.required[0] == 32u, "skinned declaration stride is 32 bytes");

    /* --- 256-row uniform bank + dirty mask ------------------------------- */
    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops;
    base_backend(&ops);
    ops.draw_pica_vs = t_pica_vs;
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    CHECK(d != NULL, "device creates");
    if (!d) return 1;
    CHECK(d9_set_back_buffer_info(d, 400, 240, D9_A8R8G8B8) == D9_OK, "back buffer is described");
    /* Buffers are created outside an open scene. */
    SkinVertex tri[3];
    fill_triangle(tri);
    D9Handle vb = 0;
    CHECK(d9_create_vertex_buffer(d, sizeof tri, &vb) == D9_OK, "vertex buffer creates");
    void *vdata = NULL;
    CHECK(d9_buffer_lock(d, vb, 0, sizeof tri, &vdata) == D9_OK, "vertex buffer locks");
    if (vdata) memcpy(vdata, tri, sizeof tri);
    CHECK(d9_buffer_unlock(d, vb) == D9_OK, "vertex buffer unlocks");
    CHECK(d9_set_stream_source(d, vb, 0, sizeof tri[0]) == D9_OK, "stream zero binds");
    CHECK(d9_begin_scene(d, 0xff000000u) == D9_OK, "scene opens");
    CHECK(ctx.begin_calls == 1, "backend begin called once");
    upload_bank(d);
    const uint32_t *mask = d9_vs_constants_f_mask(d);
    CHECK(mask != NULL, "mask is readable");
    CHECK((mask[0] & 0xfffu) == 0xfffu, "dirty mask has c0..c11");
    CHECK(((mask[180u >> 5] >> (180u & 31u)) & 1u) == 1u, "dirty mask has c180");
    CHECK(((mask[183u >> 5] >> (183u & 31u)) & 1u) == 1u, "dirty mask has c183");
    CHECK(((mask[181u >> 5] >> (181u & 31u)) & 1u) == 1u, "dirty mask has c181");
    CHECK(((mask[50u >> 5] >> (50u & 31u)) & 1u) == 0u, "unwritten c50 stays clean");
    float probe[2][4];
    CHECK(d9_get_vs_constants_f(d, 180, probe, 2) == D9_OK, "bank reads back");
    CHECK(probe[0][0] == 12.0f && probe[1][3] == 13.75f, "WVP rows are the written values");
    {
        float high[1][4] = {{7.0f, 8.0f, 9.0f, 10.0f}};
        CHECK(d9_set_vs_constants_f(d, 255, high, 1) == D9_OK, "c255 is addressable");
        CHECK(((d9_vs_constants_f_mask(d)[255u >> 5] >> (255u & 31u)) & 1u) == 1u,
              "c255 sets the dirty bit");
        CHECK(d9_set_vs_constants_f(d, 256, high, 1) == D9_INVALID, "c256 is outside the bank");
        CHECK(d9_set_vs_constants_f(d, 255, high, 2) == D9_INVALID, "count cannot cross c255");
    }

    /* --- declaration/bank gate ------------------------------------------- */
    D9PicaVsDecl pdecl;
    D9PicaVsBank bank;
    char why[192] = "";
    CHECK(d9_pica_vs_declare(program, &decl, &pdecl, why, sizeof why) == 0, why);
    CHECK(d9_pica_vs_bank(program, &bank, why, sizeof why) == 0, why);
    D9PicaVsDecl gdecl;
    D9PicaVsBank gbank;
    CHECK(d9_pica_vs_gate(program, &decl, 1, mask, 8, &gdecl, &gbank,
                          why, sizeof why) == 0, why);
    CHECK(gdecl.count == 3u && gbank.rows == 16u, "gate fills declaration and bank");
    why[0] = 0;
    CHECK(d9_pica_vs_gate(NULL, &decl, 1, mask, 8, &gdecl, &gbank,
                          why, sizeof why) != 0, "no program is refused");
    CHECK(strstr(why, "program") != NULL || strstr(why, "SHA") != NULL, why);
    {
        D9PicaVsProgram bad = *program;
        bad.shbin = NULL;
        why[0] = 0;
        CHECK(d9_pica_vs_gate(&bad, &decl, 1, mask, 8, &gdecl, &gbank,
                              why, sizeof why) != 0, "missing .shbin is refused");
        CHECK(strstr(why, "shbin") != NULL, why);
    }
    why[0] = 0;
    CHECK(d9_pica_vs_gate(program, NULL, 1, mask, 8, &gdecl, &gbank,
                          why, sizeof why) != 0, "no declaration is refused");
    CHECK(strstr(why, "declaration") != NULL, why);
    why[0] = 0;
    CHECK(d9_pica_vs_gate(program, &decl, 1, NULL, 8, &gdecl, &gbank,
                          why, sizeof why) != 0, "no constant mask is refused");
    CHECK(strstr(why, "constant mask") != NULL, why);
    {
        uint32_t hole[8];
        memcpy(hole, mask, sizeof hole);
        hole[183u >> 5] &= ~(1u << (183u & 31u));
        why[0] = 0;
        CHECK(d9_pica_vs_gate(program, &decl, 1, hole, 8, &gdecl, &gbank,
                              why, sizeof why) != 0, "missing WVP row is refused");
        CHECK(strstr(why, "c183") != NULL, why);
    }
    why[0] = 0;
    CHECK(d9_pica_vs_gate(program, &decl, 0, mask, 8, &gdecl, &gbank,
                          why, sizeof why) != 0, "no backend callback is refused");
    CHECK(strstr(why, "draw_pica_vs") != NULL, why);
    {
        D9Decl raw;
        skin_decl(&raw, D9_DECL_UBYTE4, 1);
        why[0] = 0;
        CHECK(d9_pica_vs_gate(program, &raw, 1, mask, 8, &gdecl, &gbank,
                              why, sizeof why) != 0, "raw UBYTE4 indices are refused");
        CHECK(strstr(why, "blendindices") != NULL, why);
    }

    /* --- packed vertex layout -------------------------------------------- */
    CHECK(d9_pica_vs_vertex_stride(&pdecl) == 44u, "packed stride is 44 bytes");
    CHECK(d9_pica_vs_bufinfo_permutation(&pdecl) == 0x210u, "attribute permutation is 0x210");
    {
        D9SkinVertex sv;
        memset(&sv, 0, sizeof sv);
        sv.x = 1.5f; sv.y = 2.5f; sv.z = 3.5f;
        for (unsigned i = 0; i < 4; ++i) sv.weight[i] = 0.25f * (float)(i + 1);
        sv.index[0] = 0; sv.index[1] = 1; sv.index[2] = 2; sv.index[3] = 3;
        uint8_t packed[44];
        memset(packed, 0xa5, sizeof packed);
        CHECK(d9_pica_vs_pack_vertex(&pdecl, &sv, packed) == 0, "pack succeeds");
        float pos[3], weight[4], index[4];
        memcpy(pos, packed, sizeof pos);
        memcpy(weight, packed + 12, sizeof weight);
        memcpy(index, packed + 28, sizeof index);
        CHECK(pos[0] == 1.5f && pos[1] == 2.5f && pos[2] == 3.5f, "packed position matches");
        CHECK(weight[0] == 0.25f && weight[3] == 1.0f, "packed weights match");
        CHECK(index[0] == 0.0f && index[1] == 1.0f / 255.0f &&
              index[2] == 2.0f / 255.0f && index[3] == 3.0f / 255.0f,
              "UBYTE4N indices are packed as normalised floats [0,1,2,3]");
    }

    /* A declaration with padding and a different element order still maps by
     * usage; the native pack stays in emitted-program loader order. */
    {
        D9VertexElement e[4] = {{0}};
        e[0] = (D9VertexElement){0, 0, D9_DECL_UBYTE4N, 0, D9_USAGE_BLENDINDICES, 0};
        e[1] = (D9VertexElement){0, 8, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0};
        e[2] = (D9VertexElement){0, 24, D9_DECL_FLOAT4, 0, D9_USAGE_BLENDWEIGHT, 0};
        D9Decl padded;
        CHECK(d9_decl_init_why(&padded, e, 3, why, sizeof why) == 0, why);
        D9PicaVsDecl pdecl2;
        CHECK(d9_pica_vs_declare(program, &padded, &pdecl2, why, sizeof why) == 0, why);
        CHECK(pdecl2.count == 3u, "padded reordered declaration maps three loaders");
        CHECK(pdecl2.loader[0].offset == 8u && pdecl2.loader[1].offset == 24u &&
              pdecl2.loader[2].offset == 0u,
              "loaders carry the declaration's real offsets");
        CHECK(d9_pica_vs_vertex_stride(&pdecl2) == 44u,
              "native packed stride is independent of declaration padding");
    }

    /* --- decode + backend submit ----------------------------------------- */
    set_contract(d, program, &decl, &pdecl, &bank);
    CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_OK,
          d9_last_draw_diagnostic(d));
    CHECK(ctx.pica_calls == 1 && ctx.draw_calls == 0, "one draw_pica_vs call, no legacy draw");
    CHECK(ctx.count == 3, "three decoded vertices");
    if (ctx.count == 3) {
        CHECK(ctx.vertex[1].x == 11.0f && ctx.vertex[1].z == 31.0f, "positions decode");
        CHECK(ctx.vertex[0].u == 0.0f && ctx.vertex[0].v == 0.0f, "missing UV defaults to zero");
        CHECK(ctx.vertex[0].r == 1.0f && ctx.vertex[0].a == 1.0f, "missing colour defaults to white");
        CHECK(ctx.vertex[2].weight[0] == 0.3f || fabsf(ctx.vertex[2].weight[0] - 0.3f) < 1e-6f,
              "BLENDWEIGHT0 decodes as float4");
        CHECK(ctx.vertex[2].index[0] == 2 && ctx.vertex[2].index[3] == 1,
              "BLENDINDICES0 is quantised back to bytes");
        CHECK(ctx.draw.program == program || ctx.draw.program->shbin == program->shbin,
              "callback receives the program");
        CHECK(ctx.draw.decl->count == 3u && ctx.draw.bank->rows == 16u,
              "callback receives declaration and bank");
        CHECK(ctx.draw.row_mask[180u >> 5] != 0u, "callback receives the dirty mask");
    }

    /* A BLENDINDICES byte outside the 4-bone window is a named refusal before
     * the backend sees the stream. */
    {
        SkinVertex bad[3];
        memcpy(bad, tri, sizeof bad);
        bad[1].index[2] = D9_PICA_VS_BONES;
        CHECK(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, bad, sizeof bad[0]) == D9_UNSUPPORTED,
              "bone index 4 is refused");
        CHECK(strstr(d9_last_draw_diagnostic(d), "4-bone") != NULL,
              d9_last_draw_diagnostic(d));
        CHECK(ctx.pica_calls == 1, "refused bone index does not reach the backend");
    }

    /* The last vertex is valid; one past it is rejected. */
    CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_OK,
          "triangle starting at vertex 0 reaches the last triangle");
    CHECK(ctx.pica_calls == 2, "last in-range triangle submits");
    CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 1, 1) == D9_INVALID,
          "one vertex past the buffer is rejected");
    CHECK(ctx.pica_calls == 2, "out-of-range triangle does not submit");

    /* A bank row that was wiped must stop the draw before any submit. */
    d9_clear_vs_constants_f(d);
    {
        float rows[12][4];
        for (unsigned i = 0; i < 12; ++i)
            for (unsigned j = 0; j < 4; ++j) rows[i][j] = 1.0f;
        CHECK(d9_set_vs_constants_f(d, 0, rows, 12) == D9_OK, "palette only");
    }
    CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_UNSUPPORTED,
          "missing WVP rows refuse the draw");
    CHECK(strstr(d9_last_draw_diagnostic(d), "c180") != NULL,
          d9_last_draw_diagnostic(d));
    CHECK(ctx.pica_calls == 2, "refused draw does not reach the backend");

    /* Missing BLENDWEIGHT0 is a named refusal. */
    upload_bank(d);
    D9Decl pos_only;
    skin_decl(&pos_only, D9_DECL_UBYTE4N, 0);
    {
        CHECK(d9_set_vertex_declaration(d, &pos_only) == D9_OK, "position-only declaration binds");
        CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_UNSUPPORTED,
              "missing BLENDWEIGHT0 refuses the draw");
        CHECK(strstr(d9_last_draw_diagnostic(d), "BLENDWEIGHT") != NULL,
              d9_last_draw_diagnostic(d));
    }

    /* No callback: the core refuses instead of falling back to the fixed path. */
    TestCtx ctx2;
    memset(&ctx2, 0, sizeof ctx2);
    D9Backend ops2;
    base_backend(&ops2);
    D9Device *d2 = d9_create_device(&ops2, &ctx2, 1u << 20);
    CHECK(d2 != NULL, "second device creates");
    if (d2) {
        CHECK(d9_set_back_buffer_info(d2, 400, 240, D9_A8R8G8B8) == D9_OK, "second back buffer");
        CHECK(d9_begin_scene(d2, 0) == D9_OK, "second scene opens");
        CHECK(d9_pica_vs_backend(d2) == 0, "second backend reports no draw_pica_vs");
        set_contract(d2, program, &decl, &pdecl, &bank);
        upload_bank(d2);
        CHECK(d9_draw_primitive_up(d2, D9_TRIANGLELIST, 1, tri, sizeof tri[0]) == D9_UNSUPPORTED,
              "no backend callback refuses the draw");
        CHECK(strstr(d9_last_draw_diagnostic(d2), "draw_pica_vs") != NULL,
              d9_last_draw_diagnostic(d2));
        CHECK(ctx2.draw_calls == 0, "no legacy draw fallback");
        d9_destroy_device(d2);
    }

    /* A bound fragment stage has no contract on the VS-only submit. */
    {
        CHECK(d9_set_vertex_declaration(d, &decl) == D9_OK, "full skin declaration rebinds");
        CHECK(d9_set_pixel_shader(d, (const D9TevProgram *)(const void *)program) == D9_OK,
              "unsupported PS records");
        CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_UNSUPPORTED,
              "PICA VS with an unsupported PS is refused");
        CHECK(strstr(d9_last_draw_diagnostic(d), "fragment-stage") != NULL,
              d9_last_draw_diagnostic(d));
        CHECK(d9_set_pixel_shader(d, NULL) == D9_OK, "PS unbinds");
    }

    /* Switching between two emitted programs and back to the ordinary VS. */
    {
        const D9PicaVsProgram *q = d9_pica_vs_lookup(
            "60dbd11a23ffbf74517f1256edaadadb62441fe1ed120d413540b220b9833407");
        CHECK(q != NULL, "second emitted program is present");
        if (q) {
            D9VertexElement e[5] = {{0}};
            e[0] = (D9VertexElement){0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0};
            e[1] = (D9VertexElement){0, 12, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0};
            e[2] = (D9VertexElement){0, 20, D9_DECL_COLOR, 0, D9_USAGE_COLOR, 0};
            e[3] = (D9VertexElement){0, 24, D9_DECL_FLOAT4, 0, D9_USAGE_BLENDWEIGHT, 0};
            e[4] = (D9VertexElement){0, 40, D9_DECL_UBYTE4N, 0, D9_USAGE_BLENDINDICES, 0};
            D9Decl qdecl;
            D9PicaVsDecl qloaders;
            D9PicaVsBank qbank;
            CHECK(d9_decl_init_why(&qdecl, e, 5, why, sizeof why) == 0, why);
            CHECK(d9_pica_vs_declare(q, &qdecl, &qloaders, why, sizeof why) == 0, why);
            CHECK(d9_pica_vs_bank(q, &qbank, why, sizeof why) == 0, why);
            CHECK(d9_set_pica_vs(d, q, &qloaders, &qbank) == D9_OK,
                  "second emitted program records");
            CHECK(d9_set_pica_vs(d, program, &pdecl, &bank) == D9_OK,
                  "first emitted program records again");
        }
        CHECK(d9_set_pica_vs(d, NULL, NULL, NULL) == D9_OK, "ordinary VS records");
        CHECK(d9_pica_vs_active(d) == 0, "ordinary VS is not the PICA VS path");
        CHECK(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_OK,
              "ordinary VS draw succeeds");
        CHECK(ctx.draw_calls == 1 && ctx.pica_calls == 2,
              "ordinary VS goes through the legacy draw path");
    }

    d9_destroy_device(d);

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("pica_vs_draw: %d checks passed\n", checks);
    return 0;
}
