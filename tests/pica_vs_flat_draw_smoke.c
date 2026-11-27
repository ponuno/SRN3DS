/* Host test: verified PICA VS non-skinned (plain-fetch) draw path.
 *
 * The core used to route every active PICA VS program through fetch_skin(),
 * which requires BLENDWEIGHT0/BLENDINDICES0.  This test drives the real
 * emitted blit.vs and lit.vs programs through d3d9_core with declarations that
 * carry no skin inputs:
 *
 *   - the declaration is the program's own (position/texcoord0/color0);
 *   - only the program's identity uniform rows are written, so no bone
 *     palette or WVP row is required or read;
 *   - the captured vertices carry the decoded position/texcoord/colour and
 *     zero weights/indices;
 *   - the null backend records the same submission.
 *
 * The skinned contract is still covered by pica_vs_draw_smoke.c.
 */

#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_pica_vs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BLIT_31D368CE_SHA \
    "31d368cecad5b4d215c4c1fe1580d9ab0606e09af4ee3d2fa4ce8c4fcae9359f"
#define LIT_0C1D7DF0_SHA \
    "0c1d7df008b311289b96b319070a30d8469d3d795aa0c6c6fe637be57234ec84"

static int checks, failures;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { \
    ++failures; printf("FAIL: %s (%s:%d)\n", text, __FILE__, __LINE__); \
} } while (0)

typedef struct { float x, y, z, u, v; } BlitVertex;
typedef struct { float x, y, z, u, v; uint32_t color; } LitVertex;

typedef struct {
    int begin_calls, legacy_draws, pica_calls, pica_reject;
    size_t count;
    D9SkinVertex vertex[8];
    D9PicaVsDraw draw;
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
    ++((TestCtx *)ctx)->legacy_draws;
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
    ops->draw_pica_vs = t_pica_vs;
}

static D9VertexElement entry(unsigned offset, unsigned type, unsigned usage) {
    D9VertexElement e;
    e.stream = 0; e.offset = (uint16_t)offset; e.type = (uint8_t)type;
    e.method = 0; e.usage = (uint8_t)usage; e.usage_index = 0;
    return e;
}

static void set_contract(D9Device *d, const D9PicaVsProgram *program,
                         const D9Decl *decl) {
    D9PicaVsDecl loaders;
    D9PicaVsBank bank;
    char why[192] = "";
    CHECK(d9_pica_vs_declare(program, decl, &loaders, why, sizeof why) == 0, why);
    CHECK(d9_pica_vs_bank(program, &bank, why, sizeof why) == 0, why);
    CHECK(d9_set_vertex_declaration(d, decl) == D9_OK, "declaration binds");
    CHECK(d9_set_pica_vs(d, program, &loaders, &bank) == D9_OK,
          "plain contract records");
    CHECK(loaders.count == program->attr_count, "one loader per emitted input");
    CHECK(bank.rows == program->uniform_rows, "identity bank row count follows the program");
}

static void fill_blit(BlitVertex v[3]) {
    for (unsigned i = 0; i < 3; ++i) {
        v[i].x = 10.0f + (float)i;
        v[i].y = 20.0f + 2.0f * (float)i;
        v[i].z = 30.0f + 3.0f * (float)i;
        v[i].u = 0.125f + 0.25f * (float)i;
        v[i].v = 0.5f + 0.125f * (float)i;
    }
}

int main(void) {
    const D9PicaVsProgram *blit = d9_pica_vs_lookup(BLIT_31D368CE_SHA);
    const D9PicaVsProgram *lit = d9_pica_vs_lookup(LIT_0C1D7DF0_SHA);
    CHECK(blit != NULL, "blit.vs.31d368ce is in the catalog");
    CHECK(lit != NULL, "lit.vs.0c1d7df0 is in the catalog");
    if (!blit || !lit) return 1;
    CHECK(blit->bone_rows == 0 && lit->bone_rows == 0,
          "both programs are non-skinned");
    CHECK(blit->uniform_rows == 1, "blit uses one identity row");
    CHECK(lit->uniform_rows == 8, "lit uses eight identity rows");

    TestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    D9Backend ops;
    base_backend(&ops);
    D9Device *d = d9_create_device(&ops, &ctx, 1u << 20);
    CHECK(d != NULL, "device creates");
    if (!d) return 1;
    CHECK(d9_set_back_buffer_info(d, 400, 240, D9_A8R8G8B8) == D9_OK,
          "back buffer is described");
    CHECK(d9_begin_scene(d, 0xff000000u) == D9_OK, "scene opens");
    CHECK(ctx.begin_calls == 1, "backend begin called once");

    /* --- blit.vs: position + texcoord0, one identity row ------------------ */
    {
        D9VertexElement e[2];
        e[0] = entry(0, D9_DECL_FLOAT3, D9_USAGE_POSITION);
        e[1] = entry(12, D9_DECL_FLOAT2, D9_USAGE_TEXCOORD);
        D9Decl decl;
        char why[192] = "";
        CHECK(d9_decl_init_why(&decl, e, 2, why, sizeof why) == 0, why);
        CHECK(decl.weight == UINT32_MAX && decl.index == UINT32_MAX,
              "the declaration carries no skin inputs");
        set_contract(d, blit, &decl);
        float row0[1][4] = {{1.0f, 0.0f, 0.0f, 0.0f}};
        CHECK(d9_set_vs_constants_f(d, 0, row0, 1) == D9_OK,
              "only c0 is written");
        const uint32_t *mask = d9_vs_constants_f_mask(d);
        CHECK(((mask[180u >> 5] >> (180u & 31u)) & 1u) == 0u,
              "no WVP row is set");
        BlitVertex tri[3];
        fill_blit(tri);
        CHECK(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, tri, sizeof tri[0]) == D9_OK,
              d9_last_draw_diagnostic(d));
        CHECK(ctx.pica_calls == 1 && ctx.legacy_draws == 0,
              "one draw_pica_vs submit, no legacy fallback");
        CHECK(ctx.count == 3, "three decoded vertices");
        CHECK(ctx.draw.program == blit || ctx.draw.program->shbin == blit->shbin,
              "callback receives the blit program");
        CHECK(ctx.draw.decl->count == 2u, "callback receives the two-input declaration");
        CHECK(ctx.draw.bank->rows == 1u, "callback receives the one-row identity bank");
        if (ctx.count == 3) {
            CHECK(ctx.vertex[1].x == 11.0f && ctx.vertex[1].y == 22.0f &&
                  ctx.vertex[1].z == 33.0f, "positions decode");
            CHECK(fabsf(ctx.vertex[2].u - 0.625f) < 1e-6f &&
                  fabsf(ctx.vertex[2].v - 0.75f) < 1e-6f,
                  "texcoord0 decodes");
            CHECK(ctx.vertex[0].r == 1.0f && ctx.vertex[0].a == 1.0f,
                  "missing colour defaults to white");
            for (unsigned i = 0; i < 3; ++i) {
                CHECK(ctx.vertex[i].weight[0] == 0.0f &&
                      ctx.vertex[i].weight[3] == 0.0f,
                      "skin weights are never read");
                CHECK(ctx.vertex[i].index[0] == 0 && ctx.vertex[i].index[3] == 0,
                      "skin indices are never read");
                CHECK(ctx.vertex[i].tx == 0.0f && ctx.vertex[i].bx == 0.0f,
                      "missing tangent/binormal stay zero");
            }
        }
    }

    /* --- lit.vs: position + texcoord0 + color0, eight identity rows ------ */
    {
        D9VertexElement e[3];
        e[0] = entry(0, D9_DECL_FLOAT3, D9_USAGE_POSITION);
        e[1] = entry(12, D9_DECL_FLOAT2, D9_USAGE_TEXCOORD);
        e[2] = entry(20, D9_DECL_COLOR, D9_USAGE_COLOR);
        D9Decl decl;
        char why[192] = "";
        CHECK(d9_decl_init_why(&decl, e, 3, why, sizeof why) == 0, why);
        set_contract(d, lit, &decl);
        d9_clear_vs_constants_f(d);
        float rows[8][4];
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned j = 0; j < 4; ++j) rows[i][j] = (float)i + 0.5f * (float)j;
        CHECK(d9_set_vs_constants_f(d, 0, rows, 8) == D9_OK,
              "only the eight identity rows are written");
        LitVertex tri[3] = {
            {1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0xFFC08040u},
            {4.0f, 5.0f, 6.0f, 0.5f, 0.5f, 0xFF00FF00u},
            {7.0f, 8.0f, 9.0f, 1.0f, 1.0f, 0xFFFFFFFFu},
        };
        CHECK(d9_draw_primitive_up(d, D9_TRIANGLELIST, 1, tri, sizeof tri[0]) == D9_OK,
              d9_last_draw_diagnostic(d));
        CHECK(ctx.pica_calls == 2 && ctx.legacy_draws == 0,
              "lit submits through draw_pica_vs");
        CHECK(ctx.count == 3, "three lit vertices");
        CHECK(ctx.draw.decl->count == 3u, "callback receives position/texcoord/colour");
        CHECK(ctx.draw.bank->rows == 8u, "callback receives the eight-row identity bank");
        if (ctx.count == 3) {
            CHECK(ctx.vertex[0].x == 1.0f && ctx.vertex[2].z == 9.0f,
                  "lit positions decode");
            CHECK(fabsf(ctx.vertex[0].r - 192.0f / 255.0f) < 1e-6f &&
                  fabsf(ctx.vertex[0].g - 128.0f / 255.0f) < 1e-6f &&
                  fabsf(ctx.vertex[0].b - 64.0f / 255.0f) < 1e-6f,
                  "D3DCOLOR decodes as RGBA");
            CHECK(fabsf(ctx.vertex[1].g - 1.0f) < 1e-6f,
                  "second colour decodes");
            CHECK(ctx.vertex[2].weight[0] == 0.0f && ctx.vertex[2].index[0] == 0,
                  "lit vertices carry no skin data");
        }
        /* Missing c8..c11 and c180..c183 must not block a non-skinned draw. */
        {
            const uint32_t *mask = d9_vs_constants_f_mask(d);
            for (unsigned r = 8; r < 12; ++r)
                CHECK(((mask[r >> 5] >> (r & 31u)) & 1u) == 0u,
                      "palette rows above the identity window stay unwritten");
            CHECK(((mask[180u >> 5] >> (180u & 31u)) & 1u) == 0u,
                  "WVP row c180 stays unwritten");
        }
    }

    d9_destroy_device(d);

    /* --- null backend records the non-skinned submission ----------------- */
    d9_null_reset();
    D9Device *n = d9_create_device(d9_null_backend(), d9_null_context(), 1u << 20);
    CHECK(n != NULL, "null device creates");
    if (n) {
        CHECK(d9_set_back_buffer_info(n, 400, 240, D9_A8R8G8B8) == D9_OK,
              "null back buffer is described");
        CHECK(d9_begin_scene(n, 0xff000000u) == D9_OK, "null scene opens");
        D9VertexElement e[2];
        e[0] = entry(0, D9_DECL_FLOAT3, D9_USAGE_POSITION);
        e[1] = entry(12, D9_DECL_FLOAT2, D9_USAGE_TEXCOORD);
        D9Decl decl;
        char why[192] = "";
        CHECK(d9_decl_init_why(&decl, e, 2, why, sizeof why) == 0, why);
        set_contract(n, blit, &decl);
        float row0[1][4] = {{1.0f, 0.0f, 0.0f, 0.0f}};
        CHECK(d9_set_vs_constants_f(n, 0, row0, 1) == D9_OK, "null c0 writes");
        BlitVertex tri[3];
        fill_blit(tri);
        CHECK(d9_draw_primitive_up(n, D9_TRIANGLELIST, 1, tri, sizeof tri[0]) == D9_OK,
              d9_last_draw_diagnostic(n));
        const D9NullStats *stats = d9_null_stats();
        CHECK(stats->pica_vs_draws == 1u, "null backend records one PICA VS draw");
        CHECK(stats->pica_vs_vertices == 3u, "null backend records three vertices");
        CHECK(stats->last_pica_vs_id &&
              strcmp(stats->last_pica_vs_id, blit->id) == 0,
              "null backend records the blit program id");
        d9_destroy_device(n);
    }

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("pica_vs_flat_draw: %d checks passed\n", checks);
    return 0;
}
