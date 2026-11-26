/* Real StartMenu CPU -> COM integration smoke (no GPU/frame claim).
 *
 * The first flattened display-list frame is tessellated record by record.  An
 * unmasked shape becomes transformed triangle-list geometry.  DefineEditText
 * records are parsed and their plain sources counted; unavailable imported
 * fonts are named refusals rather than substituted.  One local DefineFont3
 * glyph (!) remains as a raster/atlas/batch/COM control beside a white A8
 * texel.  The combined indexed batch is submitted through d3d9_menu_text in
 * bounded DrawPrimitiveUP chunks.  Fill styles/gradients, masks, AVM and visual
 * equivalence are deliberately outside this smoke.
 */

#include "bsa_reader.h"
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_menu_text.h"
#include "menu_swf_atlas.h"
#include "menu_swf_batch.h"
#include "menu_swf_display.h"
#include "menu_swf_display_list.h"
#include "menu_swf_raster.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define E2E_MAX_BATCH_VERTICES 262144u
#define E2E_MAX_BATCH_INDICES 786432u
#define E2E_DRAW_MAX_INDICES 4095u /* 1,365 triangles, d3d9_menu_text limit */
#define E2E_SCRATCH_ADDR 0x10100000u
#define E2E_SCRATCH_BYTES 0x300000u
#define E2E_OUT_ADDR 0x10010400u
#define E2E_ATLAS_SIZE 64u
#define E2E_RASTER_SIZE 64u
#define E2E_GLYPH_CODE 33u
#define E2E_WHITE_GLYPH 0xffffu

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

typedef struct {
    MenuSwfTessVertex vertices[MENU_SWF_TESS_MAX_VERTICES];
    uint32_t indices[MENU_SWF_TESS_MAX_INDICES];
    uint16_t fills[MENU_SWF_TESS_MAX_TRIANGLES];
    MenuSwfTessLine lines[MENU_SWF_TESS_MAX_LINES];
    MenuSwfTessEdge edges[MENU_SWF_TESS_MAX_EDGES];
    uint32_t active[MENU_SWF_TESS_MAX_EDGES];
    int32_t bands[MENU_SWF_TESS_MAX_BANDS];
    MenuSwfTessFill fill_styles[MENU_SWF_SHAPE_MAX_STYLES];
} TessFixture;

static TessFixture tess_fixture;

static void tess_fixture_init(MenuSwfTessMesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = tess_fixture.vertices;
    mesh->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
    mesh->indices = tess_fixture.indices;
    mesh->index_capacity = MENU_SWF_TESS_MAX_INDICES;
    mesh->triangle_fills = tess_fixture.fills;
    mesh->triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
    mesh->lines = tess_fixture.lines;
    mesh->line_capacity = MENU_SWF_TESS_MAX_LINES;
    mesh->edges = tess_fixture.edges;
    mesh->edge_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->active = tess_fixture.active;
    mesh->active_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->bands = tess_fixture.bands;
    mesh->band_capacity = MENU_SWF_TESS_MAX_BANDS;
    mesh->fills = tess_fixture.fill_styles;
    mesh->fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
}

typedef struct {
    MenuSwfTextVertex vertices[8192];
    uint32_t indices[24576];
    MenuSwfTextGlyph glyphs[4];
} TextFixture;

static TextFixture text_fixture;

typedef struct {
    uint32_t records;
    uint32_t sprites;
    uint32_t sprite_records;
    uint32_t shape_definitions;
    uint32_t shape_records;
    uint32_t text_records;
    uint32_t mask_records;
    uint32_t unsupported_records;
    uint32_t empty_shape_records;
    uint32_t degenerate_shape_records;
    uint32_t tess_vertices;
    uint32_t tess_triangles;
    uint32_t stroke_segments;
    uint32_t shape_batch_vertices;
    uint32_t shape_batch_indices;
    uint32_t text_definitions;
    uint32_t text_ids;
    uint32_t text_nonempty_definitions;
    uint32_t text_source_records;
    uint32_t text_nonempty_records;
    uint32_t text_source_lines;
    uint32_t text_source_chars;
    uint32_t text_refused_records;
    uint32_t text_refused_font_mask;
    uint32_t avm_tags;
    uint32_t glyph_tess_vertices;
    uint32_t glyph_tess_triangles;
    uint32_t text_quads;
    uint32_t text_batch_vertices;
    uint32_t text_batch_indices;
    uint32_t batch_vertices;
    uint32_t batch_indices;
    uint32_t batch_triangles;
} PipelineStats;


static const MenuSwfFontSummary *find_font(const MenuSwfDisplaySummary *display,
                                          uint16_t font_id) {
    for (uint32_t i = 0; i < display->stored_fonts; ++i)
        if (display->fonts[i].font_id == font_id) return &display->fonts[i];
    return NULL;
}

static int append_shape_batch(const MenuSwfDisplayListRecord *record,
                              const MenuSwfTessMesh *mesh,
                              MenuSwfBatchVertex *vertices,
                              uint32_t *vertex_count, uint32_t *indices,
                              uint32_t *index_count, PipelineStats *stats,
                              char *reason, size_t reason_cap) {
    if ((mesh->index_count % 3u) != 0 ||
        mesh->triangle_count != mesh->index_count / 3u) {
        snprintf(reason, reason_cap, "shape %u did not produce triangles",
                 record->character_id);
        return -1;
    }
    if (!mesh->triangle_count) {
        ++stats->empty_shape_records;
        return 0;
    }
    int64_t determinant = (int64_t)record->matrix.m00 * record->matrix.m11 -
                          (int64_t)record->matrix.m01 * record->matrix.m10;
    if (!determinant) {
        ++stats->degenerate_shape_records;
        return 0;
    }
    uint32_t need = mesh->triangle_count * 3u;
    if (need > E2E_MAX_BATCH_VERTICES - *vertex_count ||
        need > E2E_MAX_BATCH_INDICES - *index_count) {
        snprintf(reason, reason_cap,
                 "combined batch limit %u vertices/%u indices exceeded",
                 E2E_MAX_BATCH_VERTICES, E2E_MAX_BATCH_INDICES);
        return -1;
    }

    MenuSwfBatchShapeStyle style;
    memset(&style, 0, sizeof style);
    style.m00 = record->matrix.m00;
    style.m01 = record->matrix.m01;
    style.m10 = record->matrix.m10;
    style.m11 = record->matrix.m11;
    style.tx = record->matrix.tx;
    style.ty = record->matrix.ty;
    memcpy(style.mult, record->color.mult, sizeof style.mult);
    memcpy(style.add, record->color.add, sizeof style.add);
    style.u = 0;
    style.v = 0;

    MenuSwfBatchResult br;
    MenuSwfBatchStatus bs = menu_swf_batch_shape(
        mesh, &style, NULL, vertices + *vertex_count,
        E2E_MAX_BATCH_VERTICES - *vertex_count, indices + *index_count,
        E2E_MAX_BATCH_INDICES - *index_count, &br, reason, reason_cap);
    if (bs != MENU_SWF_BATCH_OK) {
        if (reason && !reason[0])
            snprintf(reason, reason_cap, "shape %u batch refused: %s",
                     record->character_id, menu_swf_batch_status_name(bs));
        return -1;
    }
    uint32_t base = *vertex_count;
    for (uint32_t i = 0; i < br.index_count; ++i)
        indices[*index_count + i] += base;
    *vertex_count += br.vertex_count;
    *index_count += br.index_count;
    stats->shape_batch_vertices += br.vertex_count;
    stats->shape_batch_indices += br.index_count;
    return 0;
}

typedef struct {
    unsigned w, h, levels, format;
    uint8_t *data;
    size_t bytes;
} MockTex;

typedef struct {
    unsigned created, uploaded, deleted, begin, draw, colored, end;
    unsigned a8_creates, a8_uploads, a8_bytes, a8_deletes;
    unsigned draw_calls, expanded_vertices, max_draw_vertices, bad_draws;
    unsigned indexed_draw_calls, indexed_vertices, indexed_indices;
    uint16_t first_index;
    int active;
    size_t count;
    void *texture;
    D9DrawState state;
    D9ColorVertex first_vertex;
} Mock;

static int tex_create_format(void *ctx, unsigned w, unsigned h,
                             unsigned levels, unsigned format, void **native) {
    Mock *mock = ctx;
    MockTex *texture = calloc(1, sizeof *texture);
    if (!texture) return -1;
    texture->w = w;
    texture->h = h;
    texture->levels = levels;
    texture->format = format;
    size_t pixels = (size_t)w * h;
    texture->bytes = format == D9_A8 ? pixels : pixels * 4;
    texture->data = calloc(1, texture->bytes);
    if (!texture->data) {
        free(texture);
        return -1;
    }
    *native = texture;
    ++mock->created;
    if (format == D9_A8) ++mock->a8_creates;
    return 0;
}

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels,
                      void **native) {
    return tex_create_format(ctx, w, h, levels, D9_A8R8G8B8, native);
}

static int tex_upload_format(void *ctx, void *native, unsigned level,
                             const void *data, size_t bytes) {
    Mock *mock = ctx;
    MockTex *texture = native;
    if (!texture || level >= texture->levels || !data || !bytes) return -1;
    size_t pixels = (size_t)(texture->w >> level) * (texture->h >> level);
    size_t want = texture->format == D9_A8 ? pixels : pixels * 4;
    if (bytes != want || bytes > texture->bytes) return -1;
    if (!level) memcpy(texture->data, data, bytes);
    ++mock->uploaded;
    if (texture->format == D9_A8) {
        ++mock->a8_uploads;
        mock->a8_bytes += (unsigned)bytes;
    }
    return 0;
}

static int tex_upload(void *ctx, void *native, unsigned level,
                      const void *data, size_t bytes) {
    return tex_upload_format(ctx, native, level, data, bytes);
}

static void tex_delete(void *ctx, void *native) {
    Mock *mock = ctx;
    MockTex *texture = native;
    if (!texture) return;
    if (texture->format == D9_A8) ++mock->a8_deletes;
    free(texture->data);
    free(texture);
    ++mock->deleted;
}

static int backend_begin(void *ctx, uint32_t color) {
    Mock *mock = ctx;
    (void)color;
    if (mock->active) return -1;
    mock->active = 1;
    ++mock->begin;
    return 0;
}

static int backend_draw(void *ctx, const D9Vertex *vertices, size_t count,
                        const D9Matrix *matrix, void *texture,
                        const D9DrawState *state) {
    Mock *mock = ctx;
    (void)vertices;
    (void)matrix;
    if (!mock->active || !count) return -1;
    mock->count = count;
    mock->texture = texture;
    if (state) mock->state = *state;
    ++mock->draw;
    return 0;
}

static int backend_draw_colored(void *ctx, const D9ColorVertex *vertices,
                                size_t count, const D9Matrix *matrix,
                                void *texture, const D9DrawState *state) {
    Mock *mock = ctx;
    (void)matrix;
    if (!mock->active || !count || count > E2E_DRAW_MAX_INDICES ||
        (count % 3u) != 0 || !texture || !state ||
        state->address_u != 3 || state->address_v != 3 ||
        state->min_filter != 1 || state->mag_filter != 1 ||
        state->mip_filter != 0 || state->stage_colorop != 4 ||
        state->stage_colorarg1 != 2 || state->stage_colorarg2 != 1 ||
        state->stage_alphaop != 4 || state->stage_alphaarg1 != 2 ||
        state->stage_alphaarg2 != 1) {
        ++mock->bad_draws;
        return -1;
    }
    mock->count = count;
    mock->texture = texture;
    mock->state = *state;
    mock->first_vertex = vertices[0];
    ++mock->colored;
    ++mock->draw_calls;
    mock->expanded_vertices += (unsigned)count;
    if (count > mock->max_draw_vertices)
        mock->max_draw_vertices = (unsigned)count;
    return 0;
}

static int backend_draw_menu_indexed(void *ctx, const D9ColorVertex *vertices,
                                     size_t count, const uint16_t *indices,
                                     size_t index_count, const D9Matrix *matrix,
                                     void *texture, void *texture1,
                                     const D9DrawState *state,
                                     const float (*constants)[4]) {
    Mock *mock = ctx;
    (void)matrix; (void)constants;
    if (!mock->active || !vertices || !count || !indices || !index_count ||
        (index_count % 3u) != 0 || !texture || texture1 || !state ||
        state->address_u != 3 || state->address_v != 3 ||
        state->min_filter != 1 || state->mag_filter != 1 ||
        state->mip_filter != 0 || state->stage_colorop != 4 ||
        state->stage_colorarg1 != 2 || state->stage_colorarg2 != 1 ||
        state->stage_alphaop != 4 || state->stage_alphaarg1 != 2 ||
        state->stage_alphaarg2 != 1) {
        ++mock->bad_draws;
        return -1;
    }
    for (size_t i = 0; i < index_count; ++i)
        if (indices[i] >= count) { ++mock->bad_draws; return -1; }
    mock->count = count;
    mock->texture = texture;
    mock->state = *state;
    mock->first_vertex = vertices[0];
    mock->first_index = indices[0];
    ++mock->indexed_draw_calls;
    mock->indexed_vertices += (unsigned)count;
    mock->indexed_indices += (unsigned)index_count;
    ++mock->draw_calls;
    return 0;
}

static void backend_end(void *ctx) {
    Mock *mock = ctx;
    assert(mock->active);
    mock->active = 0;
    ++mock->end;
}

static const D9Backend backend_ops = {
    .texture_create = tex_create,
    .texture_upload = tex_upload,
    .texture_delete = tex_delete,
    .begin = backend_begin,
    .draw = backend_draw,
    .end = backend_end,
    .draw_colored = backend_draw_colored,
    .texture_create_format = tex_create_format,
    .texture_upload_format = tex_upload_format,
    .draw_menu_indexed = backend_draw_menu_indexed,
};

static uint32_t com_call(D9ComDemo *demo, uint32_t object, unsigned slot,
                         const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(demo, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "menu pipeline: COM slot=%u rc=%d: %s\n",
                slot, rc, demo->message);
        abort();
    }
    return result;
}

static uint32_t guest_u32(D9ComDemo *demo, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&demo->memory, address, &value));
    return value;
}

static uint32_t sampler_value(D9ComDemo *demo, unsigned type) {
    uint32_t args[3] = {0, type, E2E_OUT_ADDR};
    assert(com_call(demo, demo->device, 68, args, 3) == 0);
    return guest_u32(demo, E2E_OUT_ADDR);
}

static uint32_t stage_value(D9ComDemo *demo, unsigned type) {
    uint32_t args[3] = {0, type, E2E_OUT_ADDR};
    assert(com_call(demo, demo->device, 66, args, 3) == 0);
    return guest_u32(demo, E2E_OUT_ADDR);
}

static void run_com_batch(MenuSwfBatchVertex *vertices, uint32_t vertex_count,
                          uint32_t *indices, uint32_t index_count,
                          const MenuSwfAtlasTarget *atlas,
                          PipelineStats *stats) {
    Mock mock = {0};
    D9Device *device = d9_create_device(&backend_ops, &mock, 8u << 20);
    check(device != NULL, "COM device creation");
    if (!device) return;

    D9ComDemo demo;
    if (d9_com_demo_init(&demo, device)) {
        fprintf(stderr, "menu pipeline: COM demo init failed: %s\n",
                demo.message);
        ++failures;
        d9_destroy_device(device);
        return;
    }

    unsigned created_before = mock.created;
    unsigned uploaded_before = mock.uploaded;
    MenuTextDraw text;
    MenuTextDrawStatus status = menu_text_draw_init(
        &text, &demo, atlas, E2E_SCRATCH_ADDR, E2E_SCRATCH_BYTES);
    check(status == MENU_TEXT_DRAW_OK, "A8 atlas COM init");
    if (status != MENU_TEXT_DRAW_OK) {
        printf("real: A8 atlas init refused: %s (%s)\n",
               menu_text_draw_status_name(status), text.reason);
        ++failures;
        goto done_demo;
    }
    check(mock.created == created_before + 1 && mock.a8_creates == 1,
          "exactly one A8 texture created");
    check(mock.uploaded == uploaded_before + 1 && mock.a8_uploads == 1,
          "exactly one A8 atlas upload");
    check(mock.a8_bytes == E2E_ATLAS_SIZE * E2E_ATLAS_SIZE,
          "A8 upload byte count");

    uint32_t desc_args[2] = {0, E2E_OUT_ADDR};
    check(com_call(&demo, text.texture, 17, desc_args, 2) == 0,
          "A8 texture GetDesc");
    check(guest_u32(&demo, E2E_OUT_ADDR) == D9_A8 &&
              guest_u32(&demo, E2E_OUT_ADDR + 0x18) == E2E_ATLAS_SIZE &&
              guest_u32(&demo, E2E_OUT_ADDR + 0x1c) == E2E_ATLAS_SIZE,
          "A8 texture format and dimensions");

    uint32_t lock_args[4] = {0, E2E_OUT_ADDR, 0, 0};
    check(com_call(&demo, text.texture, 19, lock_args, 4) == 0,
          "A8 atlas LockRect");
    check(guest_u32(&demo, E2E_OUT_ADDR) == E2E_ATLAS_SIZE,
          "A8 atlas pitch");
    uint32_t locked = guest_u32(&demo, E2E_OUT_ADDR + 4);
    const uint8_t *locked_pixels = guest_ptr(&demo.memory, locked,
                                             E2E_ATLAS_SIZE * E2E_ATLAS_SIZE);
    check(locked_pixels && memcmp(locked_pixels, atlas->pixels,
                                  E2E_ATLAS_SIZE * E2E_ATLAS_SIZE) == 0,
          "A8 atlas lock bytes match CPU atlas");
    uint32_t level = 0;
    check(com_call(&demo, text.texture, 20, &level, 1) == 0,
          "A8 atlas UnlockRect");

    check(com_call(&demo, demo.device, 41, NULL, 0) == 0, "BeginScene");
    if (text.indexed) {
        status = menu_text_draw_batch(&text, vertices, vertex_count,
                                      indices, index_count);
        check(status == MENU_TEXT_DRAW_OK && mock.indexed_draw_calls == 1,
              "one indexed menu draw");
        check(mock.indexed_vertices == vertex_count,
              "indexed draw keeps the compact source vertex count");
        check(mock.indexed_indices == index_count,
              "indexed draw keeps every index");
        check(mock.draw_calls == 1 && mock.expanded_vertices == 0,
              "indexed path replaces the UP chunks");
        if (status != MENU_TEXT_DRAW_OK) {
            printf("real: indexed COM batch refused: %s (%s)\n",
                   menu_text_draw_status_name(status), text.reason);
            ++failures;
        }
    } else {
        uint32_t offset = 0;
        while (offset < index_count) {
            uint32_t remaining = index_count - offset;
            uint32_t chunk = remaining > E2E_DRAW_MAX_INDICES
                                 ? E2E_DRAW_MAX_INDICES
                                 : remaining;
            status = menu_text_draw_batch(&text, vertices, vertex_count,
                                          indices + offset, chunk);
            if (status != MENU_TEXT_DRAW_OK) {
                printf("real: COM batch offset=%u refused: %s (%s)\n", offset,
                       menu_text_draw_status_name(status), text.reason);
                ++failures;
                break;
            }
            offset += chunk;
        }
        check(offset == index_count, "all combined indices submitted");
        check(mock.draw_calls == (index_count + E2E_DRAW_MAX_INDICES - 1) /
                                     E2E_DRAW_MAX_INDICES,
              "DrawPrimitiveUP chunk count");
        check(mock.expanded_vertices == index_count,
              "expanded draw vertices equal batch index count");
        check(mock.max_draw_vertices <= E2E_DRAW_MAX_INDICES,
              "every draw respects the UP vertex limit");
    }
    check(mock.bad_draws == 0, "every backend draw has texture and menu state");

    uint32_t fvf_args[1] = {E2E_OUT_ADDR};
    check(com_call(&demo, demo.device, 90, fvf_args, 1) == 0,
          "GetFVF after draws");
    check(guest_u32(&demo, E2E_OUT_ADDR) == 0x142, "FVF XYZ|DIFFUSE|TEX1");
    check(sampler_value(&demo, 1) == 3 && sampler_value(&demo, 2) == 3 &&
              sampler_value(&demo, 5) == 1 && sampler_value(&demo, 6) == 1 &&
              sampler_value(&demo, 7) == 0,
          "POINT/NONE/CLAMP sampler readback");
    check(stage_value(&demo, 1) == 4 && stage_value(&demo, 2) == 2 &&
              stage_value(&demo, 3) == 1 && stage_value(&demo, 4) == 4 &&
              stage_value(&demo, 5) == 2 && stage_value(&demo, 6) == 1,
          "MODULATE(TEXTURE,DIFFUSE) stage readback");
    uint32_t texture_args[2] = {0, E2E_OUT_ADDR};
    check(com_call(&demo, demo.device, 64, texture_args, 2) == 0,
          "GetTexture after draws");
    uint32_t retained_texture = guest_u32(&demo, E2E_OUT_ADDR);
    check(retained_texture == text.texture, "stage 0 retains the A8 atlas");
    check(com_call(&demo, retained_texture, 2, NULL, 0) == 2,
          "GetTexture reference released");
    check(com_call(&demo, demo.device, 42, NULL, 0) == 0, "EndScene");
    uint32_t present[4] = {0, 0, 0, 0};
    check(com_call(&demo, demo.device, 17, present, 4) == 0,
          "host mock Present closes the COM scene (no frame claim)");

    uint32_t bad_indices[3] = {vertex_count, 0, 0};
    status = menu_text_draw_batch(&text, vertices, vertex_count,
                                  bad_indices, 3);
    check(status == MENU_TEXT_DRAW_ERR_GEOMETRY && text.reason[0] &&
              strstr(text.reason, "index"),
          "bad batch index is a named refusal");
    status = menu_text_draw_batch(&text, vertices, vertex_count, indices, 0);
    check(status == MENU_TEXT_DRAW_ERR_EMPTY && text.reason[0],
          "empty batch is a named refusal");

    check(menu_text_draw_destroy(&text) == MENU_TEXT_DRAW_OK,
          "A8 atlas COM destroy");

    stats->batch_triangles = index_count / 3u;
    printf("real: COM draw_calls=%u expanded_vertices=%u max_draw_vertices=%u "
           "indexed_draws=%u indexed_vertices=%u indexed_indices=%u "
           "FVF=0x142 A8=%ux%u control_text_quads=%u\n",
           mock.draw_calls, mock.expanded_vertices, mock.max_draw_vertices,
           mock.indexed_draw_calls, mock.indexed_vertices, mock.indexed_indices,
           E2E_ATLAS_SIZE, E2E_ATLAS_SIZE, stats->text_quads);

done_demo:
    check(!d9_com_demo_destroy(&demo), "COM demo destroy");
    check(mock.a8_deletes == 1, "A8 texture deleted exactly once");
    d9_destroy_device(device);
}

static void run_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    char reason[BSA_REASON_MAX];
    BsaArchive *archive = NULL;
    BsaMatch match;
    const char *winner = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof path, "%s/%s", data_dir, archives[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (bsa_open_path(path, NULL, &archive, NULL, 0) == BSA_OK) {
            if (bsa_find(archive, "interface/startmenu.swf", &match) == BSA_OK) {
                winner = archives[i];
                break;
            }
            bsa_close(archive);
            archive = NULL;
        }
    }
    if (!archive) {
        printf("real: skip (StartMenu.swf not found under %s)\n", data_dir);
        return;
    }

    uint32_t swf_size = 0;
    check(bsa_unpacked_size(archive, &match.entry, &swf_size, reason,
                            sizeof reason) == BSA_OK,
          "real StartMenu unpacked size");
    uint8_t *swf = malloc(swf_size ? swf_size : 1);
    size_t swf_bytes = 0;
    check(swf && bsa_read(archive, &match.entry, swf, swf_size, &swf_bytes,
                          reason, sizeof reason) == BSA_OK &&
              swf_bytes == swf_size,
          "real StartMenu read");
    bsa_close(archive);
    if (!swf || swf_bytes != swf_size) {
        free(swf);
        ++failures;
        return;
    }

    MenuSwfSummary summary;
    MenuSwfStatus st = menu_swf_summary(swf, swf_bytes, &summary, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK, "real StartMenu container summary");
    if (st != MENU_SWF_OK) {
        printf("real: summary refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        free(swf);
        return;
    }

    MenuSwfDisplayListRecord *records = calloc(
        MENU_SWF_DISPLAY_LIST_MAX_RECORDS, sizeof *records);
    MenuSwfShapeResult *shapes = calloc(1, sizeof *shapes);
    MenuSwfDisplaySummary *display = calloc(1, sizeof *display);
    MenuSwfEditText *edit_items = calloc(MENU_SWF_DISPLAY_MAX_EDIT_TEXTS,
                                         sizeof *edit_items);
    MenuSwfBatchVertex *batch_vertices = malloc(
        E2E_MAX_BATCH_VERTICES * sizeof *batch_vertices);
    uint32_t *batch_indices = malloc(E2E_MAX_BATCH_INDICES * sizeof *batch_indices);
    check(records && shapes && display && edit_items && batch_vertices &&
              batch_indices,
          "real pipeline buffer allocation");
    if (!records || !shapes || !display || !edit_items || !batch_vertices ||
        !batch_indices)
        goto done;

    MenuSwfDisplayListResult list_result;
    st = menu_swf_display_list_build(
        swf, swf_bytes, records, MENU_SWF_DISPLAY_LIST_MAX_RECORDS,
        &list_result, reason, sizeof reason);
    check(st == MENU_SWF_OK && !list_result.truncated,
          "real StartMenu flattened display list");
    if (st != MENU_SWF_OK) {
        printf("real: display list refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        goto done;
    }

    st = menu_swf_shape_summary(swf, swf_bytes, shapes, reason, sizeof reason);
    check(st == MENU_SWF_OK && !shapes->truncated,
          "real StartMenu shape/font geometry summary");
    if (st != MENU_SWF_OK) {
        printf("real: shape summary refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        goto done;
    }

    st = menu_swf_display_summary(swf, swf_bytes, display, reason,
                                  sizeof reason);
    check(st == MENU_SWF_OK && !display->truncated,
          "real StartMenu display/font summary");
    if (st != MENU_SWF_OK) {
        printf("real: display/font summary refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        goto done;
    }

    MenuSwfEditTextList edit_list = {
        edit_items, MENU_SWF_DISPLAY_MAX_EDIT_TEXTS, 0, 0, 0};
    st = menu_swf_display_collect_edit_texts(swf, swf_bytes, &edit_list,
                                             reason, sizeof reason);
    check(st == MENU_SWF_OK && edit_list.count == 22,
          "real 22 DefineEditText definitions");
    if (st != MENU_SWF_OK) {
        printf("real: edit-text collect refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        goto done;
    }

    PipelineStats stats;
    memset(&stats, 0, sizeof stats);
    stats.records = list_result.record_count;
    stats.sprites = list_result.sprite_count;
    stats.shape_definitions = shapes->shape_total;
    stats.text_definitions = edit_list.total;
    stats.text_ids = edit_list.count;
    stats.avm_tags = summary.do_action;

    char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
    for (uint32_t i = 0; i < edit_list.count; ++i) {
        size_t len = 0;
        st = menu_swf_text_edit_plain(&edit_items[i], plain, sizeof plain,
                                      &len, reason, sizeof reason);
        if (st != MENU_SWF_OK) {
            printf("real: edit id=%u plain refused: %s (%s)\n",
                   edit_items[i].character_id, menu_swf_status_name(st),
                   reason);
            ++failures;
            goto done;
        }
        if (len) {
            ++stats.text_nonempty_definitions;
            printf("real: edit_source id=%u chars=%u text='%.*s'\n",
                   edit_items[i].character_id, (unsigned)len, (int)len, plain);
        }
    }

    uint32_t vertex_count = 0, index_count = 0;
    int cpu_ok = 1;
    for (uint32_t i = 0; i < list_result.record_count; ++i) {
        const MenuSwfDisplayListRecord *record = &records[i];
        if (record->is_sprite) {
            ++stats.sprite_records;
            continue;
        }
        if (record->has_clip_depth) {
            ++stats.mask_records;
            continue;
        }
        const MenuSwfShapeSummary *shape = menu_swf_shape_find(
            shapes, record->character_id);
        if (shape) {
            ++stats.shape_records;
            MenuSwfTessMesh mesh;
            tess_fixture_init(&mesh);
            st = menu_swf_tess_shape(swf, swf_bytes, record->character_id,
                                     MENU_SWF_TESS_EVEN_ODD, &mesh, reason,
                                     sizeof reason);
            if (st != MENU_SWF_OK) {
                printf("real: record=%u shape=%u tess refused: %s (%s)\n",
                       i, record->character_id, menu_swf_status_name(st),
                       reason);
                ++failures;
                cpu_ok = 0;
                break;
            }
            stats.tess_vertices += mesh.vertex_count;
            stats.tess_triangles += mesh.triangle_count;
            stats.stroke_segments += mesh.line_count;
            /* Filled in after atlas packing; white texel is reserved below. */
            if (append_shape_batch(record, &mesh, batch_vertices,
                                   &vertex_count, batch_indices, &index_count,
                                   &stats, reason, sizeof reason)) {
                printf("real: record=%u shape=%u append refused: %s\n",
                       i, record->character_id, reason);
                ++failures;
                cpu_ok = 0;
                break;
            }
        } else if (menu_swf_display_find_edit_text(&edit_list,
                                                    record->character_id)) {
            ++stats.text_records;
            ++stats.text_source_records;
            const MenuSwfEditText *edit = menu_swf_display_find_edit_text(
                &edit_list, record->character_id);
            size_t len = 0;
            st = menu_swf_text_edit_plain(edit, plain, sizeof plain, &len,
                                          reason, sizeof reason);
            if (st != MENU_SWF_OK) {
                printf("real: record=%u edit=%u plain refused: %s (%s)\n",
                       i, record->character_id, menu_swf_status_name(st),
                       reason);
                ++failures;
                cpu_ok = 0;
                break;
            }
            if (len) {
                ++stats.text_nonempty_records;
                ++stats.text_source_lines;
                for (size_t j = 0; j < len; ++j)
                    if (plain[j] == '\n') ++stats.text_source_lines;
                stats.text_source_chars += (uint32_t)len;
            }
            if (!find_font(display, edit->font_id)) {
                ++stats.text_refused_records;
                stats.text_refused_font_mask |= 1u << (edit->font_id & 31u);
            }
        } else {
            ++stats.unsupported_records;
        }
    }
    check(stats.records == stats.sprite_records + stats.mask_records +
                                stats.shape_records + stats.text_records +
                                stats.unsupported_records,
          "every display-list record classified exactly once");
    check(stats.shape_records > 0 && stats.tess_triangles > 0,
          "real shape records produced geometry");
    check(stats.text_records == 57 && stats.text_source_records == 57 &&
              stats.text_nonempty_definitions == 3 &&
              stats.text_nonempty_records == 3 &&
              stats.text_source_lines == 3 && stats.text_source_chars == 72 &&
              stats.text_refused_records == 57 &&
              (stats.text_refused_font_mask & (1u << 2)) &&
              (stats.text_refused_font_mask & (1u << 9)),
          "real edit-text sources counted and imported fonts refused by name");
    if (!cpu_ok) goto done;

    const MenuSwfFontSummary *font = NULL;
    for (uint32_t i = 0; i < display->stored_fonts; ++i) {
        if (display->fonts[i].glyph_count == 243) {
            font = &display->fonts[i];
            break;
        }
    }
    check(font != NULL, "real 243-glyph DefineFont3 found");
    if (!font) {
        ++failures;
        goto done;
    }

    MenuSwfTextMesh text;
    memset(&text, 0, sizeof text);
    text.vertices = text_fixture.vertices;
    text.vertex_capacity = sizeof text_fixture.vertices /
                           sizeof text_fixture.vertices[0];
    text.indices = text_fixture.indices;
    text.index_capacity = sizeof text_fixture.indices /
                          sizeof text_fixture.indices[0];
    text.glyphs = text_fixture.glyphs;
    text.glyph_capacity = sizeof text_fixture.glyphs /
                          sizeof text_fixture.glyphs[0];
    MenuSwfTessMesh glyph_scratch;
    tess_fixture_init(&glyph_scratch);
    text.glyph_tess = glyph_scratch;
    MenuSwfTextStyle text_style;
    memset(&text_style, 0, sizeof text_style);
    text_style.m00 = text_style.m11 = 65536;
    text_style.color = 0xffffffffu;
    st = menu_swf_text_layout(swf, swf_bytes, font, "!", 1, &text_style,
                              &text, reason, sizeof reason);
    check(st == MENU_SWF_OK && text.glyph_count == 1 &&
              text.missing_glyphs == 0 && text.triangle_count > 0,
          "one real DefineFont3 glyph laid out");
    if (st != MENU_SWF_OK || text.glyph_count != 1 ||
        text.missing_glyphs || !text.triangle_count) {
        printf("real: glyph layout refused: %s (%s)\n",
               menu_swf_status_name(st), reason);
        ++failures;
        goto done;
    }
    stats.glyph_tess_vertices = text.glyph_tess.vertex_count;
    stats.glyph_tess_triangles = text.glyph_tess.triangle_count;

    double bounds_width = (double)text.glyphs[0].bounds.xmax -
                          text.glyphs[0].bounds.xmin;
    double bounds_height = (double)text.glyphs[0].bounds.ymax -
                           text.glyphs[0].bounds.ymin;
    double scale = (E2E_RASTER_SIZE - 2.0) / bounds_width;
    double scale_y = (E2E_RASTER_SIZE - 2.0) / bounds_height;
    if (scale_y < scale) scale = scale_y;
    check(scale > 0.0, "real glyph has a positive raster scale");
    if (!(scale > 0.0)) {
        ++failures;
        goto done;
    }

    static uint8_t raster[E2E_RASTER_SIZE * E2E_RASTER_SIZE];
    MenuSwfRasterTransform raster_transform;
    memset(&raster_transform, 0, sizeof raster_transform);
    raster_transform.m00 = raster_transform.m11 = scale;
    raster_transform.tx = (E2E_RASTER_SIZE - bounds_width * scale) / 2.0 -
                          text.glyphs[0].bounds.xmin * scale;
    raster_transform.ty = (E2E_RASTER_SIZE - bounds_height * scale) / 2.0 -
                          text.glyphs[0].bounds.ymin * scale;
    MenuSwfRasterTarget raster_target = {
        E2E_RASTER_SIZE, E2E_RASTER_SIZE, E2E_RASTER_SIZE, raster, 0, 0};
    MenuSwfRasterResult raster_result;
    MenuSwfRasterStatus raster_status = menu_swf_raster_mesh(
        &text.glyph_tess, &raster_transform, &raster_target, &raster_result,
        reason, sizeof reason);
    check(raster_status == MENU_SWF_RASTER_OK && raster_result.has_coverage,
          "real glyph A8 rasterisation");
    if (raster_status != MENU_SWF_RASTER_OK || !raster_result.has_coverage) {
        printf("real: glyph raster refused: %s (%s)\n",
               menu_swf_raster_status_name(raster_status), reason);
        ++failures;
        goto done;
    }

    static uint8_t white_pixel = 255;
    MenuSwfAtlasGlyph atlas_glyphs[2];
    memset(atlas_glyphs, 0, sizeof atlas_glyphs);
    atlas_glyphs[0].pixels = raster + raster_result.y0 * E2E_RASTER_SIZE +
                            raster_result.x0;
    atlas_glyphs[0].stride = E2E_RASTER_SIZE;
    atlas_glyphs[0].width = raster_result.x1 - raster_result.x0;
    atlas_glyphs[0].height = raster_result.y1 - raster_result.y0;
    atlas_glyphs[0].glyph_index = E2E_GLYPH_CODE;
    atlas_glyphs[1].pixels = &white_pixel;
    atlas_glyphs[1].stride = 1;
    atlas_glyphs[1].width = atlas_glyphs[1].height = 1;
    atlas_glyphs[1].glyph_index = E2E_WHITE_GLYPH;

    static uint8_t atlas_pixels[E2E_ATLAS_SIZE * E2E_ATLAS_SIZE];
    MenuSwfAtlasTarget atlas = {
        E2E_ATLAS_SIZE, E2E_ATLAS_SIZE, E2E_ATLAS_SIZE, atlas_pixels};
    MenuSwfAtlasEntry atlas_entries[2];
    MenuSwfAtlasResult atlas_result;
    MenuSwfAtlasStatus atlas_status = menu_swf_atlas_pack(
        atlas_glyphs, 2, &atlas, atlas_entries, 2, &atlas_result, reason,
        sizeof reason);
    check(atlas_status == MENU_SWF_ATLAS_OK && atlas_result.placed_glyphs == 2,
          "real glyph plus white texel packed into A8 atlas");
    if (atlas_status != MENU_SWF_ATLAS_OK) {
        printf("real: atlas refused: %s (%s)\n",
               menu_swf_atlas_status_name(atlas_status), reason);
        ++failures;
        goto done;
    }
    const MenuSwfAtlasEntry *white = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (atlas_entries[i].glyph_index == E2E_WHITE_GLYPH) white = &atlas_entries[i];
    check(white && white->has_rect && white->w == 1 && white->h == 1,
          "white A8 texel reserved for shape geometry");
    if (!white || !white->has_rect || white->w != 1 || white->h != 1) {
        ++failures;
        goto done;
    }
    uint32_t white_u = white->u + white->uw / 2u;
    uint32_t white_v = white->v + white->vh / 2u;

    /* Shape vertices were appended before the atlas existed.  Patch only the
     * reserved UVs; positions/colours/indices are already final. */
    for (uint32_t i = 0; i < stats.shape_batch_vertices; ++i) {
        batch_vertices[i].u = white_u;
        batch_vertices[i].v = white_v;
    }
    unsigned shape_coloured = 0;
    for (uint32_t i = 0; i < stats.shape_batch_vertices; ++i)
        if (batch_vertices[i].color != 0xffffffffu) ++shape_coloured;
    printf("real: shape_colour_vertices=%u/%u\n", shape_coloured,
           stats.shape_batch_vertices);
    check(shape_coloured > 0, "shape vertices carry fill colours");

    MenuSwfBatchStyle batch_style;
    memset(&batch_style, 0, sizeof batch_style);
    batch_style.m00 = batch_style.m11 = scale;
    batch_style.tx = 300.0 - text.glyphs[0].bounds.xmin * scale;
    batch_style.ty = 200.0 - text.glyphs[0].bounds.ymin * scale;
    batch_style.color = 0xffffffffu;
    MenuSwfBatchVertex text_vertices[4];
    uint32_t text_indices[6];
    MenuSwfBatchResult text_batch_result;
    MenuSwfBatchStatus batch_status = menu_swf_batch_text(
        &text, atlas_entries, 1, &batch_style, NULL, text_vertices, 4,
        text_indices, 6, &text_batch_result, reason, sizeof reason);
    check(batch_status == MENU_SWF_BATCH_OK &&
              text_batch_result.quad_count == 1 &&
              text_batch_result.vertex_count == 4 &&
              text_batch_result.index_count == 6,
          "real glyph becomes one text quad");
    if (batch_status != MENU_SWF_BATCH_OK) {
        printf("real: text batch refused: %s (%s)\n",
               menu_swf_batch_status_name(batch_status), reason);
        ++failures;
        goto done;
    }
    if (vertex_count + 4 > E2E_MAX_BATCH_VERTICES ||
        index_count + 6 > E2E_MAX_BATCH_INDICES) {
        printf("real: combined batch limit exceeded at text append\n");
        ++failures;
        goto done;
    }
    uint32_t text_base = vertex_count;
    memcpy(batch_vertices + vertex_count, text_vertices, sizeof text_vertices);
    for (unsigned i = 0; i < 6; ++i)
        batch_indices[index_count + i] = text_base + text_indices[i];
    vertex_count += 4;
    index_count += 6;
    stats.text_quads = 1;
    stats.text_batch_vertices = 4;
    stats.text_batch_indices = 6;
    stats.batch_vertices = vertex_count;
    stats.batch_indices = index_count;
    stats.batch_triangles = index_count / 3u;
    check(stats.tess_vertices == 57480 && stats.shape_batch_vertices == 57480 &&
              stats.shape_batch_indices == 86154 &&
              stats.batch_vertices == 57484 && stats.batch_indices == 86160,
          "indexed shape batch keeps mesh vertex sharing");
    check(stats.batch_vertices <= 65536u,
          "combined batch fits the 16-bit indexed vertex range");

    printf("real: source=%s StartMenu.swf bytes=%u tags=%u avm_skipped=%u\n",
           winner, swf_size, summary.tag_count, stats.avm_tags);
    printf("real: records=%u sprites=%u sprite_records=%u "
           "shape_definitions=%u shape_records=%u text_records=%u "
           "mask_records=%u unsupported_records=%u\n",
           stats.records, stats.sprites, stats.sprite_records,
           stats.shape_definitions, stats.shape_records, stats.text_records,
           stats.mask_records, stats.unsupported_records);
    printf("real: edit_text_definitions=%u edit_text_ids=%u "
           "nonempty_definitions=%u source_records=%u nonempty_records=%u "
           "source_lines=%u source_glyph_slots=%u refused_records=%u "
           "refused_font_mask=%08x resolved_text_glyphs=0\n",
           stats.text_definitions, stats.text_ids,
           stats.text_nonempty_definitions, stats.text_source_records,
           stats.text_nonempty_records, stats.text_source_lines,
           stats.text_source_chars, stats.text_refused_records,
           stats.text_refused_font_mask);
    printf("real: empty_shapes=%u degenerate_shapes=%u strokes_skipped=%u\n",
           stats.empty_shape_records, stats.degenerate_shape_records,
           stats.stroke_segments);
    printf("real: tessellated_vertices=%u triangles=%u "
           "shape_batch_vertices=%u shape_batch_indices=%u\n",
           stats.tess_vertices, stats.tess_triangles,
           stats.shape_batch_vertices, stats.shape_batch_indices);
    printf("real: glyph=%u tess_vertices=%u triangles=%u raster=%ux%u "
           "rect=%u,%u,%u,%u sum=%llu atlas_used_height=%u\n",
           E2E_GLYPH_CODE, stats.glyph_tess_vertices,
           stats.glyph_tess_triangles, raster_result.width,
           raster_result.height, raster_result.x0, raster_result.y0,
           raster_result.x1, raster_result.y1,
           (unsigned long long)raster_result.coverage_sum,
           atlas_result.used_height);
    printf("real: batch_vertices=%u indices=%u triangles=%u limits=%u/%u "
           "draw_chunk_indices=%u\n",
           stats.batch_vertices, stats.batch_indices, stats.batch_triangles,
           E2E_MAX_BATCH_VERTICES, E2E_MAX_BATCH_INDICES,
           E2E_DRAW_MAX_INDICES);

    run_com_batch(batch_vertices, vertex_count, batch_indices, index_count,
                  &atlas, &stats);

done:
    free(batch_indices);
    free(batch_vertices);
    free(display);
    free(edit_items);
    free(shapes);
    free(records);
    free(swf);
}

int main(int argc, char **argv) {
    printf("menu StartMenu CPU -> COM pipeline tests\n");
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        run_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu pipeline e2e: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
