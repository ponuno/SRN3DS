/* Isolated real StartMenu -> COM -> core -> PICA200 diagnostic.
 *
 * Input is the winning Interface/StartMenu.swf from sdmc:/Skyrim/Data/Update.bsa.
 * The CPU pipeline is the same bounded display-list/tessellation/solid-fill/A8
 * path used by menu_pipeline_e2e.  Imported font ids 34/41 are deliberately not
 * substituted: retail text stays empty.  One local Font3 '!' remains the A8
 * sampling control.  The combined 57,484-vertex / 86,160-index batch is sent as
 * one 16-bit indexed COM draw through the real Citro3D backend and Presented.
 *
 * This file includes the production Citro3D adapter only to count the real
 * C3D_TexBind/TEV/C3D_DrawElements calls.  Every wrapper calls the production
 * implementation; there is no mock draw or CPU raster substitute.
 */
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsa_reader.h"
#include "d3d9_citro3d.h"
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_menu_text.h"
#include "d3d9_pica_citro3d.h"
#include "menu_swf_atlas.h"
#include "menu_swf_batch.h"
#include "menu_swf_display.h"
#include "menu_swf_display_list.h"
#include "menu_swf_raster.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"

static int counted_apply_tev_ex(const D9TevProgram *program, C3D_Tex *texture0,
                                C3D_Tex *texture1,
                                const float (*constants)[4], unsigned count,
                                char *why, size_t why_size);
static void counted_tex_bind(int unit, C3D_Tex *texture);
static void counted_draw_elements(GPU_Primitive_t primitive, int count,
                                  int type, const void *indices);

#define d9_pica_citro3d_apply_tev_ex counted_apply_tev_ex
#define C3D_TexBind counted_tex_bind
#define C3D_DrawElements counted_draw_elements
#include "d3d9_citro3d.c"
#undef C3D_DrawElements
#undef C3D_TexBind
#undef d9_pica_citro3d_apply_tev_ex

#define CHECK(condition, what) do { \
    ++g_checks; \
    if (!(condition)) { ++g_errors; fprintf(stderr, "FAIL: %s\n", what); } \
    fflush(stderr); \
} while (0)

/* A crash must not hide the last reached phase: the stage file is written
 * with fopen/fclose on every step and survives whatever terminates the app. */
static void stage(const char *name) {
    fprintf(stderr, "STAGE %s\n", name);
    fflush(stderr);
    FILE *f = fopen("sdmc:/menu_frame_stage.txt", "w");
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

#define PHASE(name) do { fprintf(stderr, "PHASE %s\n", name); fflush(stderr); } while (0)

#define BATCH_VERTEX_CAP 160000u
#define BATCH_INDEX_CAP 100000u
#define TEXT_VERTEX_CAP 4096u
#define TEXT_INDEX_CAP 12288u
#define ATLAS_SIZE 64u
#define RASTER_SIZE 64u
#define CONTROL_CODE 33u
#define WHITE_GLYPH 0xffffu
#define SCRATCH_ADDR 0x10100000u
#define SCRATCH_BYTES 0x300000u
#define CAPS_OUT 0x10010400u

static int g_checks, g_errors;
static unsigned g_apply_calls, g_draw_calls, g_bind_calls;
static C3D_Tex *g_last_bound;
static uint32_t g_env_sources;
static GPU_COMBINEFUNC g_env_rgb, g_env_alpha;
static unsigned g_draw_index_count, g_draw_index_type;
static uint32_t g_draw_index_hash;
static D9TevProgram g_last_program;
static int g_last_program_valid;

static uint32_t hash_indices16(const uint16_t *indices, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= indices[i] & 0xffu;
        hash *= 16777619u;
        hash ^= indices[i] >> 8;
        hash *= 16777619u;
    }
    return hash;
}

static int counted_apply_tev_ex(const D9TevProgram *program, C3D_Tex *texture0,
                                C3D_Tex *texture1,
                                const float (*constants)[4], unsigned count,
                                char *why, size_t why_size) {
    ++g_apply_calls;
    if (program) {
        g_last_program = *program;
        g_last_program_valid = 1;
    } else {
        g_last_program_valid = 0;
    }
    int rc = d9_pica_citro3d_apply_tev_ex(program, texture0, texture1,
                                         constants, count, why, why_size);
    if (!rc) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        if (env) {
            g_env_sources = (uint32_t)env->srcRgb;
            g_env_rgb = (GPU_COMBINEFUNC)env->funcRgb;
            g_env_alpha = (GPU_COMBINEFUNC)env->funcAlpha;
        }
    }
    return rc;
}

static void counted_tex_bind(int unit, C3D_Tex *texture) {
    ++g_bind_calls;
    if (unit == 0) g_last_bound = texture;
    C3D_TexBind(unit, texture);
}

static void counted_draw_elements(GPU_Primitive_t primitive, int count,
                                  int type, const void *indices) {
    ++g_draw_calls;
    g_draw_index_count = (unsigned)count;
    g_draw_index_type = (unsigned)type;
    g_draw_index_hash = count > 0 && indices
                            ? hash_indices16((const uint16_t *)indices,
                                             (size_t)count)
                            : 0;
    C3D_DrawElements(primitive, count, type, indices);
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
static TessFixture g_tess;

typedef struct {
    MenuSwfTextVertex vertices[TEXT_VERTEX_CAP];
    uint32_t indices[TEXT_INDEX_CAP];
    MenuSwfTextGlyph glyphs[MENU_SWF_TEXT_MAX_GLYPHS];
} TextFixture;
static TextFixture g_text;

static MenuSwfBatchVertex g_batch_vertices[BATCH_VERTEX_CAP];
static uint32_t g_batch_indices[BATCH_INDEX_CAP];
static MenuSwfDisplayListRecord g_records[256];
static MenuSwfEditText g_edits[MENU_SWF_DISPLAY_MAX_EDIT_TEXTS];
static MenuSwfImportAssets g_imports[4];
static uint8_t g_raster[RASTER_SIZE * RASTER_SIZE];
static uint8_t g_atlas_pixels[ATLAS_SIZE * ATLAS_SIZE];

typedef struct {
    uint32_t records, sprites, sprite_records, shape_records, text_records;
    uint32_t mask_records, unsupported_records, text_nonempty;
    uint32_t text_refused, text_font_mask;
    uint32_t tess_vertices, tess_triangles, strokes;
    uint32_t shape_vertices, shape_indices;
    uint32_t batch_vertices, batch_indices, batch_triangles;
    uint32_t glyph_tess_vertices, glyph_tess_triangles;
} MenuFrameStats;

static void tess_init(MenuSwfTessMesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = g_tess.vertices;
    mesh->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
    mesh->indices = g_tess.indices;
    mesh->index_capacity = MENU_SWF_TESS_MAX_INDICES;
    mesh->triangle_fills = g_tess.fills;
    mesh->triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
    mesh->lines = g_tess.lines;
    mesh->line_capacity = MENU_SWF_TESS_MAX_LINES;
    mesh->edges = g_tess.edges;
    mesh->edge_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->active = g_tess.active;
    mesh->active_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->bands = g_tess.bands;
    mesh->band_capacity = MENU_SWF_TESS_MAX_BANDS;
    mesh->fills = g_tess.fill_styles;
    mesh->fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
}

static void text_init(MenuSwfTextMesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = g_text.vertices;
    mesh->vertex_capacity = TEXT_VERTEX_CAP;
    mesh->indices = g_text.indices;
    mesh->index_capacity = TEXT_INDEX_CAP;
    mesh->glyphs = g_text.glyphs;
    mesh->glyph_capacity = MENU_SWF_TEXT_MAX_GLYPHS;
    tess_init(&mesh->glyph_tess);
}

static int append_shape(const MenuSwfDisplayListRecord *record,
                        const MenuSwfTessMesh *mesh,
                        uint32_t *vertex_count, uint32_t *index_count,
                        MenuFrameStats *stats, char *reason, size_t reason_cap) {
    if (!mesh->triangle_count) return 0;
    int64_t det = (int64_t)record->matrix.m00 * record->matrix.m11 -
                  (int64_t)record->matrix.m01 * record->matrix.m10;
    if (!det) return 0;
    uint32_t need = mesh->triangle_count * 3u;
    if (need > BATCH_VERTEX_CAP - *vertex_count ||
        need > BATCH_INDEX_CAP - *index_count) {
        snprintf(reason, reason_cap, "combined batch capacity exceeded");
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
    MenuSwfBatchResult result;
    MenuSwfBatchStatus status = menu_swf_batch_shape(
        mesh, &style, NULL, g_batch_vertices + *vertex_count,
        BATCH_VERTEX_CAP - *vertex_count, g_batch_indices + *index_count,
        BATCH_INDEX_CAP - *index_count, &result, reason, reason_cap);
    if (status != MENU_SWF_BATCH_OK) {
        if (!reason[0])
            snprintf(reason, reason_cap, "shape %u: %s", record->character_id,
                     menu_swf_batch_status_name(status));
        return -1;
    }
    uint32_t base = *vertex_count;
    for (uint32_t i = 0; i < result.index_count; ++i)
        g_batch_indices[*index_count + i] += base;
    *vertex_count += result.vertex_count;
    *index_count += result.index_count;
    stats->shape_vertices += result.vertex_count;
    stats->shape_indices += result.index_count;
    return 0;
}

static uint8_t *load_startmenu(size_t *out_size, uint32_t *out_crc,
                               char *reason, size_t reason_cap) {
    const char *path = "sdmc:/Skyrim/Data/Update.bsa";
    BsaArchive *archive = NULL;
    BsaStatus status = bsa_open_path(path, NULL, &archive, reason, reason_cap);
    if (status != BSA_OK) return NULL;
    BsaMatch match;
    status = bsa_find(archive, "interface/startmenu.swf", &match);
    if (status != BSA_OK) {
        snprintf(reason, reason_cap, "%s: %s", path, bsa_status_name(status));
        bsa_close(archive);
        return NULL;
    }
    uint32_t size = 0;
    status = bsa_unpacked_size(archive, &match.entry, &size, reason, reason_cap);
    if (status != BSA_OK || !size || size > MENU_SWF_MAX_DECLARED) {
        if (status == BSA_OK)
            snprintf(reason, reason_cap, "StartMenu size %lu is outside the SWF bound",
                     (unsigned long)size);
        bsa_close(archive);
        return NULL;
    }
    uint8_t *bytes = malloc(size);
    size_t got = 0;
    if (!bytes) {
        snprintf(reason, reason_cap, "StartMenu allocation failed");
        bsa_close(archive);
        return NULL;
    }
    status = bsa_read(archive, &match.entry, bytes, size, &got, reason, reason_cap);
    bsa_close(archive);
    if (status != BSA_OK || got != size) {
        free(bytes);
        return NULL;
    }
    *out_size = size;
    *out_crc = bsa_crc32(bytes, size);
    return bytes;
}

static int build_menu(const uint8_t *swf, size_t swf_size, MenuSwfAtlasTarget *atlas,
                      uint32_t *out_vertices, uint32_t *out_indices,
                      MenuFrameStats *stats, char *reason, size_t reason_cap) {
    MenuSwfSummary summary;
    PHASE("summary");
    MenuSwfStatus status = menu_swf_summary(swf, swf_size, &summary, reason, reason_cap);
    CHECK(status == MENU_SWF_OK, "real StartMenu container parses");
    if (status != MENU_SWF_OK) return -1;
    PHASE("summary_done");

    MenuSwfDisplayListResult list;
    PHASE("display_list");
    status = menu_swf_display_list_build(swf, swf_size, g_records,
                                         sizeof g_records / sizeof g_records[0],
                                         &list, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && !list.truncated, "real display list flattens");
    if (status != MENU_SWF_OK || list.truncated) return -1;
    PHASE("display_list_done");

    MenuSwfShapeResult *shapes = calloc(1, sizeof *shapes);
    MenuSwfDisplaySummary *display = calloc(1, sizeof *display);
    if (!shapes || !display) {
        free(shapes); free(display);
        snprintf(reason, reason_cap, "shape/display allocation failed");
        return -1;
    }
    PHASE("shape_summary");
    status = menu_swf_shape_summary(swf, swf_size, shapes, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && !shapes->truncated, "real shape/font geometry parses");
    if (status != MENU_SWF_OK || shapes->truncated) {
        free(shapes); free(display); return -1;
    }
    PHASE("shape_summary_done");
    PHASE("display_summary");
    status = menu_swf_display_summary(swf, swf_size, display, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && !display->truncated, "real display/font metrics parse");
    if (status != MENU_SWF_OK || display->truncated) {
        free(shapes); free(display); return -1;
    }
    PHASE("display_summary_done");

    MenuSwfEditTextList edits = {g_edits, MENU_SWF_DISPLAY_MAX_EDIT_TEXTS, 0, 0, 0};
    status = menu_swf_display_collect_edit_texts(swf, swf_size, &edits, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && edits.count == 22, "22 real DefineEditText definitions");
    if (status != MENU_SWF_OK) { free(shapes); free(display); return -1; }
    MenuSwfImportList imports = {g_imports, 4, 0, 0, 0};
    status = menu_swf_collect_imports(swf, swf_size, &imports, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && imports.count == 2, "two structured font imports");
    if (status != MENU_SWF_OK) { free(shapes); free(display); return -1; }
    PHASE("metadata_done");

    memset(stats, 0, sizeof *stats);
    stats->records = list.record_count;
    stats->sprites = list.sprite_count;
    uint32_t vertex_count = 0, index_count = 0;
    char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
    PHASE("shape_loop");
    for (uint32_t i = 0; i < list.record_count; ++i) {
        const MenuSwfDisplayListRecord *record = &g_records[i];
        fprintf(stderr, "REC %u/%u id=%u sprite=%u clip=%u\n", i, list.record_count,
                record->character_id, record->is_sprite, record->has_clip_depth);
        fflush(stderr);
        if (record->is_sprite) { ++stats->sprite_records; continue; }
        if (record->has_clip_depth) { ++stats->mask_records; continue; }
        const MenuSwfShapeSummary *shape = menu_swf_shape_find(shapes, record->character_id);
        if (shape) {
            ++stats->shape_records;
            MenuSwfTessMesh mesh;
            tess_init(&mesh);
            fprintf(stderr, "TESS %u begin\n", record->character_id);
            fflush(stderr);
            status = menu_swf_tess_shape(swf, swf_size, record->character_id,
                                         MENU_SWF_TESS_EVEN_ODD, &mesh,
                                         reason, reason_cap);
            if (status != MENU_SWF_OK) {
                fprintf(stderr, "shape %u tessellation: %s\n", record->character_id, reason);
                free(shapes); free(display); return -1;
            }
            fprintf(stderr, "TESS %u done v=%u t=%u\n", record->character_id,
                    mesh.vertex_count, mesh.triangle_count);
            fflush(stderr);
            stats->tess_vertices += mesh.vertex_count;
            stats->tess_triangles += mesh.triangle_count;
            stats->strokes += mesh.line_count;
            fprintf(stderr, "APPEND %u begin\n", record->character_id);
            fflush(stderr);
            if (append_shape(record, &mesh, &vertex_count, &index_count,
                             stats, reason, reason_cap)) {
                fprintf(stderr, "shape %u batch: %s\n", record->character_id, reason);
                free(shapes); free(display); return -1;
            }
            fprintf(stderr, "APPEND %u done total=%u/%u\n", record->character_id,
                    vertex_count, index_count);
            fflush(stderr);
        } else if (menu_swf_display_find_edit_text(&edits, record->character_id)) {
            ++stats->text_records;
            const MenuSwfEditText *edit = menu_swf_display_find_edit_text(&edits,
                                                                          record->character_id);
            size_t len = 0;
            status = menu_swf_text_edit_plain(edit, plain, sizeof plain, &len,
                                              reason, reason_cap);
            if (status != MENU_SWF_OK) {
                free(shapes); free(display); return -1;
            }
            if (len) ++stats->text_nonempty;
            ++stats->text_refused;
            stats->text_font_mask |= 1u << (edit->font_id & 31u);
        } else {
            ++stats->unsupported_records;
        }
    }
    CHECK(stats->records == 216 && stats->sprites == 71 &&
              stats->sprite_records == 106 && stats->shape_records == 53 &&
              stats->text_records == 57 && stats->text_nonempty == 3 &&
              stats->text_refused == 57 && stats->mask_records == 0,
          "real display-list census is unchanged");
    CHECK(stats->tess_vertices == 57480 && stats->tess_triangles == 28718 &&
              stats->shape_vertices == 57480 && stats->shape_indices == 86154,
          "real shape tessellation/batch counts are unchanged");
    PHASE("shape_loop_done");

    const MenuSwfFontSummary *font = NULL;
    for (uint32_t i = 0; i < display->stored_fonts; ++i)
        if (display->fonts[i].glyph_count == 243) { font = &display->fonts[i]; break; }
    CHECK(font != NULL, "local 243-glyph Font3 control is present");
    if (!font) { free(shapes); free(display); return -1; }

    MenuSwfTextMesh text;
    text_init(&text);
    MenuSwfTextStyle text_style;
    memset(&text_style, 0, sizeof text_style);
    text_style.m00 = text_style.m11 = 65536;
    text_style.color = 0xffffffffu;
    status = menu_swf_text_layout(swf, swf_size, font, "!", 1, &text_style,
                                  &text, reason, reason_cap);
    CHECK(status == MENU_SWF_OK && text.glyph_count == 1 &&
              !text.missing_glyphs && text.triangle_count == 159,
          "local '!' control lays out with 159 triangles");
    PHASE("text_done");
    if (status != MENU_SWF_OK || text.glyph_count != 1 || text.missing_glyphs ||
        text.triangle_count != 159) { free(shapes); free(display); return -1; }
    stats->glyph_tess_vertices = text.glyph_tess.vertex_count;
    stats->glyph_tess_triangles = text.glyph_tess.triangle_count;

    double width = (double)text.glyphs[0].bounds.xmax - text.glyphs[0].bounds.xmin;
    double height = (double)text.glyphs[0].bounds.ymax - text.glyphs[0].bounds.ymin;
    double scale = (RASTER_SIZE - 2.0) / width;
    double scale_y = (RASTER_SIZE - 2.0) / height;
    if (scale_y < scale) scale = scale_y;
    CHECK(scale > 0.0, "control glyph has a positive raster scale");
    if (!(scale > 0.0)) { free(shapes); free(display); return -1; }

    MenuSwfRasterTransform raster_transform;
    memset(&raster_transform, 0, sizeof raster_transform);
    raster_transform.m00 = raster_transform.m11 = scale;
    raster_transform.tx = (RASTER_SIZE - width * scale) / 2.0 -
                          text.glyphs[0].bounds.xmin * scale;
    raster_transform.ty = (RASTER_SIZE - height * scale) / 2.0 -
                          text.glyphs[0].bounds.ymin * scale;
    MenuSwfRasterTarget raster_target = {
        RASTER_SIZE, RASTER_SIZE, RASTER_SIZE, g_raster, 0, 0};
    MenuSwfRasterResult raster_result;
    MenuSwfRasterStatus raster_status = menu_swf_raster_mesh(
        &text.glyph_tess, &raster_transform, &raster_target, &raster_result,
        reason, reason_cap);
    CHECK(raster_status == MENU_SWF_RASTER_OK && raster_result.has_coverage,
          "local '!' control rasterises to A8");
    PHASE("raster_done");
    if (raster_status != MENU_SWF_RASTER_OK || !raster_result.has_coverage) {
        free(shapes); free(display); return -1;
    }

    static const uint8_t white_pixel = 255;
    MenuSwfAtlasGlyph atlas_glyphs[2];
    memset(atlas_glyphs, 0, sizeof atlas_glyphs);
    atlas_glyphs[0].pixels = g_raster + raster_result.y0 * RASTER_SIZE + raster_result.x0;
    atlas_glyphs[0].stride = RASTER_SIZE;
    atlas_glyphs[0].width = raster_result.x1 - raster_result.x0;
    atlas_glyphs[0].height = raster_result.y1 - raster_result.y0;
    atlas_glyphs[0].glyph_index = CONTROL_CODE;
    atlas_glyphs[1].pixels = &white_pixel;
    atlas_glyphs[1].stride = 1;
    atlas_glyphs[1].width = atlas_glyphs[1].height = 1;
    atlas_glyphs[1].glyph_index = WHITE_GLYPH;
    *atlas = (MenuSwfAtlasTarget){ATLAS_SIZE, ATLAS_SIZE, ATLAS_SIZE, g_atlas_pixels};
    MenuSwfAtlasEntry entries[2];
    MenuSwfAtlasResult atlas_result;
    MenuSwfAtlasStatus atlas_status = menu_swf_atlas_pack(
        atlas_glyphs, 2, atlas, entries, 2, &atlas_result, reason, reason_cap);
    CHECK(atlas_status == MENU_SWF_ATLAS_OK && atlas_result.placed_glyphs == 2,
          "control glyph and white texel pack into 64x64 A8");
    PHASE("atlas_done");
    if (atlas_status != MENU_SWF_ATLAS_OK) { free(shapes); free(display); return -1; }
    const MenuSwfAtlasEntry *white = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (entries[i].glyph_index == WHITE_GLYPH) white = &entries[i];
    if (!white || !white->has_rect) { free(shapes); free(display); return -1; }
    uint32_t white_u = white->u + white->uw / 2u;
    uint32_t white_v = white->v + white->vh / 2u;
    for (uint32_t i = 0; i < stats->shape_vertices; ++i) {
        g_batch_vertices[i].u = white_u;
        g_batch_vertices[i].v = white_v;
    }

    MenuSwfBatchStyle batch_style;
    memset(&batch_style, 0, sizeof batch_style);
    batch_style.m00 = batch_style.m11 = scale;
    batch_style.tx = 300.0 - text.glyphs[0].bounds.xmin * scale;
    batch_style.ty = 200.0 - text.glyphs[0].bounds.ymin * scale;
    batch_style.color = 0xffffffffu;
    MenuSwfBatchVertex text_vertices[4];
    uint32_t text_indices[6];
    MenuSwfBatchResult text_result;
    MenuSwfBatchStatus batch_status = menu_swf_batch_text(
        &text, entries, 1, &batch_style, NULL, text_vertices, 4,
        text_indices, 6, &text_result, reason, reason_cap);
    CHECK(batch_status == MENU_SWF_BATCH_OK && text_result.vertex_count == 4 &&
              text_result.index_count == 6,
          "control glyph becomes one atlas quad");
    if (batch_status != MENU_SWF_BATCH_OK || text_result.vertex_count != 4 ||
        text_result.index_count != 6) { free(shapes); free(display); return -1; }
    if (vertex_count + 4 > BATCH_VERTEX_CAP || index_count + 6 > BATCH_INDEX_CAP) {
        free(shapes); free(display); return -1;
    }
    uint32_t base = vertex_count;
    memcpy(g_batch_vertices + vertex_count, text_vertices, sizeof text_vertices);
    for (unsigned i = 0; i < 6; ++i)
        g_batch_indices[index_count + i] = base + text_indices[i];
    vertex_count += 4;
    index_count += 6;
    stats->batch_vertices = vertex_count;
    stats->batch_indices = index_count;
    stats->batch_triangles = index_count / 3u;
    CHECK(vertex_count == 57484 && index_count == 86160 && stats->batch_triangles == 28720,
          "combined real batch is 57,484 vertices / 86,160 indices / 28,720 triangles");
    *out_vertices = vertex_count;
    *out_indices = index_count;
    PHASE("build_menu_done");
    fprintf(stderr, "MENU_CPU records=%u sprites=%u shapes=%u text=%u text_glyphs=0 "
                    "tess_vertices=%u triangles=%u shape_batch=%u/%u control=%u/%u "
                    "batch=%u/%u/%u atlas=%ux%u tags=%u\n",
            (unsigned)stats->records, (unsigned)stats->sprites,
            (unsigned)stats->shape_records, (unsigned)stats->text_records,
            (unsigned)stats->tess_vertices, (unsigned)stats->tess_triangles,
            (unsigned)stats->shape_vertices, (unsigned)stats->shape_indices,
            (unsigned)stats->glyph_tess_vertices, (unsigned)stats->glyph_tess_triangles,
            (unsigned)stats->batch_vertices, (unsigned)stats->batch_indices,
            (unsigned)stats->batch_triangles, (unsigned)ATLAS_SIZE,
            (unsigned)ATLAS_SIZE, (unsigned)summary.tag_count);
    free(shapes);
    free(display);
    return g_errors ? -1 : 0;
}

static int set_menu_transforms(D9Device *device) {
    D9Matrix world, view, projection;
    d9_matrix_identity(&world);
    d9_matrix_identity(&view);
    d9_matrix_identity(&projection);
    projection.m[0][0] = 2.0f / 400.0f;
    projection.m[0][3] = -1.0f;
    projection.m[1][1] = -2.0f / 240.0f;
    projection.m[1][3] = 1.0f;
    projection.m[2][2] = 1.0f;
    return d9_set_transform(device, D9_WORLD, &world) ||
           d9_set_transform(device, D9_VIEW, &view) ||
           d9_set_transform(device, D9_PROJECTION, &projection);
}

static int set_menu_2d_state(D9Device *device) {
    return d9_set_render_state(device, 7, 0) ||   /* ZENABLE = FALSE */
           d9_set_render_state(device, 14, 0) ||  /* ZWRITEENABLE = FALSE */
           d9_set_render_state(device, 22, 1) ||  /* CULLMODE = NONE */
           d9_set_render_state(device, 19, 5) ||  /* SRCBLEND = SRCALPHA */
           d9_set_render_state(device, 20, 6) ||  /* DESTBLEND = INVSRCALPHA */
           d9_set_render_state(device, 27, 1);    /* ALPHABLENDENABLE = TRUE */
}

static uint32_t com_call(D9ComDemo *demo, uint32_t object, unsigned slot,
                         const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    if (d9_com_demo_call(demo, object, slot, args, count, &result) || result) {
        fprintf(stderr, "COM slot=%u failed: %s hr=%08lx\n", slot,
                demo->message[0] ? demo->message : "invoke failure",
                (unsigned long)result);
        return 1;
    }
    return 0;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    FILE *stderr_log = freopen("sdmc:/Skyrim_N3DS_stderr.log", "w", stderr);
    /* freopen makes stderr fully buffered; a crash or a block before the final
     * fflush() would otherwise leave an empty log. */
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "StartMenu PICA frame runner: build-only until Azahar pixels are archived\n");
    fprintf(stderr, "input=sdmc:/Skyrim/Data/Update.bsa!interface/startmenu.swf\n");
    if (!stderr_log) fprintf(stderr, "stderr redirect unavailable\n");
    stage("start");

    char reason[BSA_REASON_MAX] = {0};
    size_t swf_size = 0;
    uint32_t swf_crc = 0;
    stage("load_startmenu");
    uint8_t *swf = load_startmenu(&swf_size, &swf_crc, reason, sizeof reason);
    CHECK(swf != NULL, "real Update.bsa StartMenu loads");
    stage("load_startmenu_done");
    if (!swf) fprintf(stderr, "MENU_INPUT_FAIL: %s\n", reason);
    if (swf)
        fprintf(stderr, "MENU_INPUT bytes=%u crc32=%08lx\n", (unsigned)swf_size,
                (unsigned long)swf_crc);

    MenuSwfAtlasTarget atlas;
    uint32_t vertex_count = 0, index_count = 0;
    MenuFrameStats stats;
    stage("build_menu");
    int cpu_ok = swf && !build_menu(swf, swf_size, &atlas, &vertex_count,
                                    &index_count, &stats, reason, sizeof reason);
    stage(cpu_ok ? "build_menu_done" : "build_menu_failed");
    fprintf(stderr, "MENU_BUILD vertices=%u indices=%u\n", vertex_count, index_count);
    free(swf);

    D9Citro *gpu = NULL;
    D9Device *device = NULL;
    D9ComDemo demo;
    memset(&demo, 0, sizeof demo);
    MenuTextDraw text;
    memset(&text, 0, sizeof text);
    int text_ready = 0, submitted = 0, presented = 0;
    int com_attempted = 0;
    uint32_t expected_index_hash = 0;
    if (cpu_ok) {
        stage("gpu_open");
        gpu = d9_citro_open();
        CHECK(gpu != NULL, "real Citro3D/PICA backend opens");
        stage("device_create");
        device = gpu ? d9_create_device(d9_citro_backend(), gpu, 8u << 20) : NULL;
        CHECK(device != NULL, "core adopts the real PICA backend");
        if (device) {
            com_attempted = 1;
            stage("com_init");
            if (!d9_com_demo_init(&demo, device)) {
                CHECK(demo.menu_indexed, "COM device exposes indexed menu callback");
                uint32_t caps_ptr = CAPS_OUT;
                if (!com_call(&demo, demo.device, 7, &caps_ptr, 1)) {
                    uint32_t caps = 0;
                    CHECK(!guest_read32(&demo.memory, CAPS_OUT + 28, &caps) &&
                              caps == 0x00082000u,
                          "DevCaps remain exactly 0x00082000");
                    fprintf(stderr, "DEVCAPS=%08lx\n", (unsigned long)caps);
                }
                CHECK(!set_menu_transforms(device), "400x240 orthographic transforms");
                CHECK(!set_menu_2d_state(device), "2D no-depth alpha-blended state");
                MenuTextDrawStatus ts = menu_text_draw_init(
                    &text, &demo, &atlas, SCRATCH_ADDR, SCRATCH_BYTES);
                CHECK(ts == MENU_TEXT_DRAW_OK, "A8 atlas creates/uploads through COM");
                if (ts != MENU_TEXT_DRAW_OK)
                    fprintf(stderr, "MENU_A8_FAIL: %s (%s)\n",
                            menu_text_draw_status_name(ts), text.reason);
                text_ready = ts == MENU_TEXT_DRAW_OK;
                stage(text_ready ? "atlas_ready" : "atlas_failed");
            } else {
                fprintf(stderr, "MENU_COM_FAIL: %s\n", demo.message);
            }
        }
    }

    if (text_ready) {
        uint16_t *index16 = malloc((size_t)index_count * sizeof *index16);
        CHECK(index16 != NULL, "index hash allocation");
        if (index16) {
            int indices_ok = 1;
            for (uint32_t i = 0; i < index_count; ++i) {
                if (g_batch_indices[i] > 0xffffu) { indices_ok = 0; break; }
                index16[i] = (uint16_t)g_batch_indices[i];
            }
            CHECK(indices_ok, "batch indices fit 16 bits");
            if (indices_ok) expected_index_hash = hash_indices16(index16, index_count);
            free(index16);
        }
        uint32_t clear = 0xff101820u;
        uint32_t clear_args[6] = {0, 0, 1, clear, 0x3f800000u, 0};
        stage("clear_begin");
        if (!com_call(&demo, demo.device, 43, clear_args, 6) &&
            !com_call(&demo, demo.device, 41, NULL, 0)) {
            stage("draw");
            MenuTextDrawStatus ts = menu_text_draw_batch(
                &text, g_batch_vertices, vertex_count, g_batch_indices, index_count);
            stage(ts == MENU_TEXT_DRAW_OK ? "draw_ok" : "draw_failed");
            CHECK(ts == MENU_TEXT_DRAW_OK, "one real indexed menu draw submits");
            if (ts != MENU_TEXT_DRAW_OK)
                fprintf(stderr, "MENU_DRAW_FAIL: %s (%s)\n",
                        menu_text_draw_status_name(ts), text.reason);
            submitted = ts == MENU_TEXT_DRAW_OK;
            uint32_t present_args[4] = {0, 0, 0, 0};
            stage("present");
            presented = !com_call(&demo, demo.device, 42, NULL, 0) &&
                        !com_call(&demo, demo.device, 17, present_args, 4);
            stage(presented ? "present_ok" : "present_failed");
            CHECK(presented, "COM EndScene and PICA Present complete");
        } else {
            fprintf(stderr, "MENU_BEGIN_FAIL: %s\n", demo.message);
        }
    }

    if (submitted) {
        CHECK(g_apply_calls == 1, "one real PICA TEV apply");
        CHECK(g_draw_calls == 1, "one real C3D_DrawElements");
        CHECK(g_bind_calls >= 1 && g_last_bound != NULL, "A8 texture reaches C3D_TexBind(0)");
        CHECK(g_draw_index_count == 86160 && g_draw_index_type == C3D_UNSIGNED_SHORT,
              "PICA receives 86,160 unsigned-short indices");
        CHECK(g_draw_index_hash == expected_index_hash, "PICA index stream hash matches CPU batch");
        CHECK(gpu && gpu->cursor_color == 57484 && gpu->cursor_index == 86160,
              "PICA keeps 57,484 compact vertices and 86,160 indices");
        CHECK(g_last_program_valid && g_last_program.stages == 1 &&
                  g_last_program.textures == 1 &&
                  (g_last_program.backend_flags & D9TEV_PLAN_MENU_TEXT) &&
                  g_last_program.stage[0].func_rgb == D9TEV_FUNC_MODULATE &&
                  g_last_program.stage[0].func_a == D9TEV_FUNC_MODULATE &&
                  g_last_program.stage[0].src_rgb[0] == D9TEV_SRC_TEX0 &&
                  g_last_program.stage[0].src_rgb[1] == D9TEV_SRC_PRIMARY,
              "TEV plan is one-stage MODULATE(TEXTURE0,PRIMARY)");
        CHECK(g_env_sources == GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PREVIOUS, GPU_PREVIOUS) &&
                  g_env_rgb == GPU_MODULATE && g_env_alpha == GPU_MODULATE,
              "C3D_TexEnv stage 0 is MODULATE(TEXTURE0,PREVIOUS,PREVIOUS)");
        CHECK(g_last_bound && g_last_bound->fmt == GPU_A8 &&
                  g_last_bound->width == 64 && g_last_bound->height == 64 &&
                  C3D_TexGetType(g_last_bound) == GPU_TEX_2D,
              "bound PICA texture is the 64x64 GPU_A8 atlas");
        fprintf(stderr, "PICA_DRAW apply=%u draw_elements=%u bind=%u indices=%u type=%u "
                        "index_hash=%08lx expected_hash=%08lx vertices=%u cursor_indices=%u "
                        "plan=menu_text stages=1 textures=1 env_rgb=%u env_a=%u "
                        "texture=%ux%u format=%u\n",
                g_apply_calls, g_draw_calls, g_bind_calls, g_draw_index_count,
                g_draw_index_type, (unsigned long)g_draw_index_hash,
                (unsigned long)expected_index_hash,
                gpu ? (unsigned)gpu->cursor_color : 0u,
                gpu ? (unsigned)gpu->cursor_index : 0u,
                (unsigned)g_env_rgb, (unsigned)g_env_alpha,
                g_last_bound ? g_last_bound->width : 0u,
                g_last_bound ? g_last_bound->height : 0u,
                g_last_bound ? (unsigned)g_last_bound->fmt : 0u);
    }
    fprintf(stderr, "MENU_SUBMISSION: %s\n", g_errors ? "FAIL" : "PASS");
    fprintf(stderr, "MENU_FRAME_STATUS=%s cpu=%d submitted=%d present=%d checks=%d errors=%d\n",
            g_errors ? "FAIL" : presented ? "SUBMISSION_PASS_FRAME_NOT_CLAIMED" : "BUILD_ONLY",
            cpu_ok, submitted, presented, g_checks, g_errors);
    fprintf(stderr, "scope=real StartMenu shapes + local '!' A8 control; imported text glyphs=0; "
                    "no visual/frame claim without an archived Azahar screenshot\n");
    fflush(stderr);
    stage("waiting_start");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

    if (text_ready) {
        MenuTextDrawStatus ts = menu_text_draw_destroy(&text);
        CHECK(ts == MENU_TEXT_DRAW_OK, "A8 atlas COM destroy");
    }
    if (com_attempted) CHECK(!d9_com_demo_destroy(&demo), "COM demo teardown");
    d9_destroy_device(device);
    d9_citro_close(gpu);
    gfxExit();
    return g_errors ? 1 : 0;
}
