/* Diagnostic host renderer: one StartMenu.swf frame -> RGBA8.
 *
 * This is NOT an engine frame and makes no parity claim.  It runs the existing
 * bounded SWF modules end to end on the CPU:
 *
 *   menu_swf_summary -> menu_swf_display_list_build -> menu_swf_shape_summary
 *   -> menu_swf_tess_shape -> menu_swf_batch_shape
 *   -> menu_swf_text_layout_edit_resolved -> menu_swf_raster_mesh
 *   -> menu_swf_atlas_pack -> menu_swf_batch_text
 *
 * and composites the resulting vertex/index batches with a small host
 * triangle rasteriser into an RGBA8 buffer.  Shape and text batches sample one
 * A8 atlas (text coverage + one reserved white texel for solid/gradient
 * fills).  Anything the current path cannot draw (bitmap fills, stroked
 * lines, clip masks, sprite-local edit-text matrices, AVM) is counted and
 * named instead of substituted.
 *
 * Usage:
 *   menu_swf_frame_probe --swf FILE --out-raw FILE.rgba --out-json FILE.json \
 *       [--font-source FILE] [--fontconfig FILE] \
 *       [--width W] [--height H]
 *   menu_swf_frame_probe --self-test
 */

#include "bsa_reader.h"
#include "menu_swf.h"
#include "menu_swf_atlas.h"
#include "menu_swf_batch.h"
#include "menu_swf_display.h"
#include "menu_swf_display_list.h"
#include "menu_swf_font.h"
#include "menu_swf_raster.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf_text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_MAX_DRAW_ITEMS 8192u
#define PROBE_ATLAS_SIZE 512u
#define PROBE_MAX_FONT_ITEMS 32u
#define PROBE_MAX_IMPORT_ITEMS 4u

typedef struct {
    uint32_t swf_bytes;
    uint32_t declared_bytes;
    uint32_t format;
    uint32_t tags;
    uint32_t top_tags;
    uint32_t define_shape;
    uint32_t define_font2;
    uint32_t define_font3;
    uint32_t define_sprite;
    uint32_t show_frame;
    uint32_t do_action;
    uint32_t bits_lossless;
    uint32_t bits_jpeg;
    int32_t scene_w_twips, scene_h_twips;

    uint32_t records;
    uint32_t sprite_records;
    uint32_t mask_records;
    uint32_t shape_records;
    uint32_t text_records;
    uint32_t unsupported_records;
    uint32_t empty_shapes;
    uint32_t degenerate_shapes;
    uint32_t shape_triangles;
    uint32_t shape_vertices;
    uint32_t stroke_segments;
    uint32_t bitmap_fill_records;
    uint32_t bitmap_fill_styles;
    uint32_t gradient_fill_styles;
    uint32_t focal_fill_styles;
    uint32_t solid_fill_styles;

    uint32_t text_definitions;
    uint32_t text_nonempty;
    uint32_t text_resolved;
    uint32_t text_refused;
    uint32_t text_glyphs;
    uint32_t text_missing_glyphs;
    uint32_t text_triangles;
    uint32_t text_quads;

    uint32_t atlas_glyphs;
    uint32_t atlas_used_height;
    uint32_t atlas_occupied;

    uint32_t render_draw_items;
    uint32_t render_triangles;
    uint32_t rgba_w, rgba_h;
    uint64_t opaque_pixels;
    uint32_t bbox_x0, bbox_y0, bbox_x1, bbox_y1;
    uint8_t has_bbox;

    uint32_t avm_tags;
    uint32_t font_source_fallback;
} ProbeStats;

typedef struct {
    uint8_t *pixels;
    uint32_t w, h;
} Rgba;

typedef struct {
    MenuSwfBatchVertex *vertices;
    uint32_t vertex_count;
    uint32_t *indices;
    uint32_t index_count;
} BatchPool;

typedef struct {
    uint8_t is_text;
    uint32_t vbase, vcount, ibase, icount;
} DrawItem;

#include <stdarg.h>

static void diag(char *list, size_t cap, const char *fmt, ...) {
    size_t used = strlen(list);
    if (used + 2 >= cap) return;
    int n = snprintf(list + used, cap - used, "%s", used ? " | " : "");
    if (n < 0) return;
    used += (size_t)n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(list + used, cap - used, fmt, ap);
    va_end(ap);
}

static void tess_scratch(MenuSwfTessMesh *mesh, MenuSwfTessVertex *v,
                         uint32_t *i, uint16_t *f, MenuSwfTessLine *lines,
                         MenuSwfTessEdge *edges, uint32_t *active,
                         int32_t *bands, MenuSwfTessFill *fills) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = v;
    mesh->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
    mesh->indices = i;
    mesh->index_capacity = MENU_SWF_TESS_MAX_INDICES;
    mesh->triangle_fills = f;
    mesh->triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
    mesh->lines = lines;
    mesh->line_capacity = MENU_SWF_TESS_MAX_LINES;
    mesh->edges = edges;
    mesh->edge_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->active = active;
    mesh->active_capacity = MENU_SWF_TESS_MAX_EDGES;
    mesh->bands = bands;
    mesh->band_capacity = MENU_SWF_TESS_MAX_BANDS;
    mesh->fills = fills;
    mesh->fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long end = ftell(f);
    if (end < 0 || (unsigned long)end > MENU_SWF_MAX_DECLARED ||
        fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    uint8_t *bytes = malloc((size_t)end ? (size_t)end : 1);
    if (!bytes) { fclose(f); return NULL; }
    size_t got = fread(bytes, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) { free(bytes); return NULL; }
    *out_size = got;
    return bytes;
}

/* ---------------------------------------------------------------- RECT --- */

typedef struct { const uint8_t *p; size_t bits; } BitReader;

static int br_bits(BitReader *b, unsigned n, uint32_t *out) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        size_t bit = b->bits + i;
        v = (v << 1) | ((b->p[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    b->bits += n;
    *out = v;
    return 0;
}

static int br_signed(BitReader *b, unsigned n, int32_t *out) {
    uint32_t v;
    br_bits(b, n, &v);
    if (n && (v & (1u << (n - 1)))) v |= ~((1u << n) - 1u);
    *out = (int32_t)v;
    return 0;
}

static int swf_rect_twips(const uint8_t *body, size_t len, int32_t *w,
                          int32_t *h) {
    BitReader b = {body, 0};
    uint32_t nbits;
    int32_t xmin, xmax, ymin, ymax;
    if (len < 1) return -1;
    br_bits(&b, 5, &nbits);
    if (nbits > 31) return -1;
    br_signed(&b, nbits, &xmin);
    br_signed(&b, nbits, &xmax);
    br_signed(&b, nbits, &ymin);
    br_signed(&b, nbits, &ymax);
    *w = xmax - xmin;
    *h = ymax - ymin;
    return 0;
}

/* ------------------------------------------------------------ raster ----- */

static void blend_px(Rgba *img, uint32_t x, uint32_t y, uint32_t argb,
                     uint8_t coverage) {
    if (x >= img->w || y >= img->h) return;
    uint32_t sa = (argb >> 24) & 0xffu;
    if (coverage != 255) sa = sa * coverage / 255u;
    if (!sa) return;
    uint8_t *p = img->pixels + ((size_t)y * img->w + x) * 4u;
    uint32_t sr = (argb >> 16) & 0xffu, sg = (argb >> 8) & 0xffu,
             sb = argb & 0xffu;
    /* Premultiplied source-over into straight-alpha destination. */
    uint32_t da = p[3];
    uint32_t outa = sa * 255u + da * (255u - sa);
    if (!outa) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    uint32_t r = (sr * sa * 255u + p[0] * da * (255u - sa)) / outa;
    uint32_t g = (sg * sa * 255u + p[1] * da * (255u - sa)) / outa;
    uint32_t bl = (sb * sa * 255u + p[2] * da * (255u - sa)) / outa;
    p[0] = (uint8_t)(r > 255 ? 255 : r);
    p[1] = (uint8_t)(g > 255 ? 255 : g);
    p[2] = (uint8_t)(bl > 255 ? 255 : bl);
    p[3] = (uint8_t)(outa / 255u);
}

static void draw_triangles(Rgba *img, const MenuSwfBatchVertex *v,
                           const uint32_t *idx, uint32_t index_count,
                           const uint8_t *atlas, uint32_t atlas_w,
                           uint32_t atlas_h, double scale) {
    for (uint32_t t = 0; t + 2 < index_count; t += 3) {
        const MenuSwfBatchVertex *a = &v[idx[t]];
        const MenuSwfBatchVertex *b = &v[idx[t + 1]];
        const MenuSwfBatchVertex *c = &v[idx[t + 2]];
        double ax = a->x * scale, ay = a->y * scale;
        double bx = b->x * scale, by = b->y * scale;
        double cx = c->x * scale, cy = c->y * scale;
        double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (area == 0.0) continue;
        int32_t x0 = (int32_t)(ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx));
        int32_t x1 = (int32_t)(ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx)) + 2;
        int32_t y0 = (int32_t)(ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy));
        int32_t y1 = (int32_t)(ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy)) + 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)img->w) x1 = (int32_t)img->w;
        if (y1 > (int32_t)img->h) y1 = (int32_t)img->h;
        for (int32_t py = y0; py < y1; ++py) {
            for (int32_t px = x0; px < x1; ++px) {
                double sx = px + 0.5, sy = py + 0.5;
                double w0 = ((bx - sx) * (cy - sy) - (by - sy) * (cx - sx)) / area;
                double w1 = ((cx - sx) * (ay - sy) - (cy - sy) * (ax - sx)) / area;
                double w2 = 1.0 - w0 - w1;
                if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                double u = a->u * w0 + b->u * w1 + c->u * w2;
                double vv = a->v * w0 + b->v * w1 + c->v * w2;
                uint32_t tx = (uint32_t)(u / 65536.0 * atlas_w);
                uint32_t ty = (uint32_t)(vv / 65536.0 * atlas_h);
                if (tx >= atlas_w) tx = atlas_w - 1;
                if (ty >= atlas_h) ty = atlas_h - 1;
                uint8_t cov = atlas[ty * atlas_w + tx];
                if (!cov) continue;
                double ca[4], cb[4], cc[4], out[4];
                for (int i = 0; i < 4; ++i) {
                    ca[i] = (a->color >> (24 - 8 * i)) & 0xff;
                    cb[i] = (b->color >> (24 - 8 * i)) & 0xff;
                    cc[i] = (c->color >> (24 - 8 * i)) & 0xff;
                    out[i] = ca[i] * w0 + cb[i] * w1 + cc[i] * w2;
                }
                uint32_t argb = ((uint32_t)(out[0] + 0.5) << 24) |
                                ((uint32_t)(out[1] + 0.5) << 16) |
                                ((uint32_t)(out[2] + 0.5) << 8) |
                                (uint32_t)(out[3] + 0.5);
                blend_px(img, (uint32_t)px, (uint32_t)py, argb, cov);
            }
        }
    }
}

/* ------------------------------------------------------------- glyphs ---- */

typedef struct {
    uint8_t *bitmap;
    uint32_t w, h;
} GlyphBitmap;

typedef struct {
    MenuSwfTextMesh mesh;
    MenuSwfTextVertex *vertices;
    uint32_t *indices;
    MenuSwfTextGlyph *glyphs;
    GlyphBitmap *bitmaps;
    MenuSwfAtlasEntry *entries;
    uint32_t glyph_count;
} TextMeshEx;

static int rasterize_text_mesh(TextMeshEx *t, MenuSwfTessVertex *scratch_v,
                               uint32_t *scratch_i, MenuSwfTessMesh *scratch,
                               char *reason, size_t reason_cap) {
    for (uint32_t g = 0; g < t->glyph_count; ++g) {
        MenuSwfTextGlyph *glyph = &t->glyphs[g];
        if (!glyph->has_geometry || !glyph->vertex_count) continue;
        uint32_t w = (uint32_t)(glyph->bounds.xmax - glyph->bounds.xmin);
        uint32_t h = (uint32_t)(glyph->bounds.ymax - glyph->bounds.ymin);
        if (!w || !h) continue;
        uint8_t *buf = malloc((size_t)w * h);
        if (!buf) return -1;
        if (glyph->vertex_count > MENU_SWF_TESS_MAX_VERTICES ||
            glyph->triangle_count * 3u > MENU_SWF_TESS_MAX_INDICES ||
            glyph->triangle_count > MENU_SWF_RASTER_MAX_TRIANGLES) {
            free(buf);
            return -1;
        }
        for (uint32_t k = 0; k < glyph->vertex_count; ++k) {
            scratch_v[k].x = t->vertices[glyph->vertex_first + k].x;
            scratch_v[k].y = t->vertices[glyph->vertex_first + k].y;
        }
        for (uint32_t k = 0; k < glyph->triangle_count * 3u; ++k)
            scratch_i[k] = t->indices[glyph->triangle_first * 3u + k] -
                           glyph->vertex_first;
        memset(scratch, 0, sizeof *scratch);
        scratch->vertices = scratch_v;
        scratch->vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
        scratch->indices = scratch_i;
        scratch->index_capacity = MENU_SWF_TESS_MAX_INDICES;
        scratch->vertex_count = glyph->vertex_count;
        scratch->index_count = glyph->triangle_count * 3u;
        scratch->triangle_count = glyph->triangle_count;
        MenuSwfRasterTransform xf;
        memset(&xf, 0, sizeof xf);
        xf.m00 = xf.m11 = 1.0;
        xf.tx = -(double)glyph->bounds.xmin;
        xf.ty = -(double)glyph->bounds.ymin;
        MenuSwfRasterTarget target = {w, h, w, buf, 0, 0};
        MenuSwfRasterResult result;
        MenuSwfRasterStatus rs = menu_swf_raster_mesh(
            scratch, &xf, &target, &result, reason, reason_cap);
        if (rs != MENU_SWF_RASTER_OK || !result.has_coverage) {
            free(buf);
            if (rs != MENU_SWF_RASTER_OK) return -1;
            continue;
        }
        t->bitmaps[g].bitmap = buf;
        t->bitmaps[g].w = w;
        t->bitmaps[g].h = h;
    }
    return 0;
}

static void free_text_mesh(TextMeshEx *t) {
    for (uint32_t g = 0; g < t->glyph_count; ++g)
        free(t->bitmaps[g].bitmap);
    free(t->bitmaps);
    free(t->entries);
    free(t->glyphs);
    free(t->indices);
    free(t->vertices);
}

/* ------------------------------------------------------------ render ----- */

static int render_swf(const uint8_t *swf, size_t swf_bytes,
                      const uint8_t *font_source, size_t font_source_bytes,
                      const char *font_source_name,
                      const uint8_t *config_source, size_t config_source_bytes,
                      int width_override, int height_override,
                      Rgba *out, ProbeStats *st, char *miss, size_t miss_cap,
                      char *reason, size_t reason_cap) {
    memset(st, 0, sizeof *st);
    memset(out, 0, sizeof *out);
    st->swf_bytes = (uint32_t)swf_bytes;
    st->font_source_fallback = font_source != NULL;

    MenuSwfSummary summary;
    if (menu_swf_summary(swf, swf_bytes, &summary, reason, reason_cap) !=
        MENU_SWF_OK) {
        snprintf(reason, reason_cap, "swf summary: %s", reason);
        return -1;
    }
    st->declared_bytes = summary.header.declared_bytes;
    st->format = summary.header.format;
    st->tags = summary.tag_count;
    st->top_tags = summary.top_level_tag_count;
    st->define_shape = summary.define_shape;
    st->define_font2 = summary.define_font2;
    st->define_font3 = summary.define_font3;
    st->define_sprite = summary.define_sprite;
    st->show_frame = summary.show_frame;
    st->do_action = summary.do_action;
    st->avm_tags = summary.do_action;
    st->bits_lossless = summary.define_bits_lossless;
    st->bits_jpeg = summary.define_bits_jpeg;

    MenuSwfView view;
    if (menu_swf_view_open(swf, swf_bytes, &view, reason, reason_cap) !=
        MENU_SWF_OK) {
        return -1;
    }
    int32_t scene_w = 0, scene_h = 0;
    swf_rect_twips(view.body, view.body_len, &scene_w, &scene_h);
    menu_swf_view_close(&view);
    st->scene_w_twips = scene_w;
    st->scene_h_twips = scene_h;
    uint32_t out_w = width_override > 0 ? (uint32_t)width_override
                                        : (uint32_t)(scene_w / 20);
    uint32_t out_h = height_override > 0 ? (uint32_t)height_override
                                         : (uint32_t)(scene_h / 20);
    if (!out_w || !out_h) { out_w = 1280; out_h = 720; }
    double scale = (double)out_w / (scene_w ? scene_w / 20.0 : 1280.0);

    MenuSwfDisplayListRecord *records =
        calloc(MENU_SWF_DISPLAY_LIST_MAX_RECORDS, sizeof *records);
    MenuSwfDisplayListResult list_result;
    if (!records) { snprintf(reason, reason_cap, "oom records"); return -1; }
    MenuSwfStatus s = menu_swf_display_list_build(
        swf, swf_bytes, records, MENU_SWF_DISPLAY_LIST_MAX_RECORDS,
        &list_result, reason, reason_cap);
    if (s != MENU_SWF_OK) {
        snprintf(reason, reason_cap, "display list: %s", reason);
        free(records);
        return -1;
    }
    st->records = list_result.record_count;

    MenuSwfShapeResult *shapes = calloc(1, sizeof *shapes);
    if (!shapes) { free(records); return -1; }
    s = menu_swf_shape_summary(swf, swf_bytes, shapes, reason, reason_cap);
    if (s != MENU_SWF_OK) {
        snprintf(reason, reason_cap, "shape summary: %s", reason);
        free(shapes);
        free(records);
        return -1;
    }
    for (uint32_t i = 0; i < shapes->shape_total && i < MENU_SWF_SHAPE_MAX_SHAPES; ++i) {
        const MenuSwfShapeSummary *sh = &shapes->shapes[i];
        st->solid_fill_styles += sh->solid_fills;
        st->gradient_fill_styles += sh->gradient_fills;
        st->focal_fill_styles += sh->focal_fills;
        st->bitmap_fill_styles += sh->bitmap_fills;
    }

    MenuSwfEditText *edits = calloc(MENU_SWF_DISPLAY_MAX_EDIT_TEXTS, sizeof *edits);
    MenuSwfEditTextList edit_list = {
        edits, MENU_SWF_DISPLAY_MAX_EDIT_TEXTS, 0, 0, 0};
    if (edits) {
        s = menu_swf_display_collect_edit_texts(swf, swf_bytes, &edit_list,
                                                reason, reason_cap);
    }
    st->text_definitions = edit_list.total;

    MenuSwfImportAssets *imports = calloc(PROBE_MAX_IMPORT_ITEMS, sizeof *imports);
    MenuSwfImportList import_list = {imports, PROBE_MAX_IMPORT_ITEMS, 0, 0, 0};
    if (imports)
        menu_swf_collect_imports(swf, swf_bytes, &import_list, reason,
                                 reason_cap);

    MenuSwfFontSummary *font_items = calloc(PROBE_MAX_FONT_ITEMS,
                                            sizeof *font_items);
    MenuSwfFontList font_list = {font_items, PROBE_MAX_FONT_ITEMS, 0, 0, 0, "", 0};
    MenuSwfFontConfig config;
    memset(&config, 0, sizeof config);
    if (config_source && config_source_bytes) {
        menu_swf_fontconfig_parse((const char *)config_source,
                                  config_source_bytes, &config, reason,
                                  reason_cap);
    }
    if (font_source && font_source_bytes)
        menu_swf_display_collect_fonts(font_source, font_source_bytes,
                                       &font_list, reason, reason_cap);
    if (!font_source)
        diag(miss, miss_cap, "no font source: imported font ids resolve to nothing (%s)",
             "Interface/fonts_en.swf not supplied");

    /* Buffers. */
    MenuSwfTessVertex *tv = malloc(MENU_SWF_TESS_MAX_VERTICES * sizeof *tv);
    uint32_t *ti = malloc(MENU_SWF_TESS_MAX_INDICES * sizeof *ti);
    uint16_t *tf = malloc(MENU_SWF_TESS_MAX_TRIANGLES * sizeof *tf);
    MenuSwfTessEdge *te = malloc(MENU_SWF_TESS_MAX_EDGES * sizeof *te);
    uint32_t *ta = malloc(MENU_SWF_TESS_MAX_EDGES * sizeof *ta);
    int32_t *tb = malloc(MENU_SWF_TESS_MAX_BANDS * sizeof *tb);
    MenuSwfTessFill *tfill = malloc(MENU_SWF_SHAPE_MAX_STYLES * sizeof *tfill);
    MenuSwfTessLine *tl = malloc(MENU_SWF_TESS_MAX_LINES * sizeof *tl);
    uint32_t max_batch_v = MENU_SWF_BATCH_MAX_SHAPE_VERTICES;
    uint32_t max_batch_i = MENU_SWF_BATCH_MAX_SHAPE_INDICES;
    MenuSwfBatchVertex *pool_v = malloc((size_t)max_batch_v * sizeof *pool_v);
    uint32_t *pool_i = malloc((size_t)max_batch_i * sizeof *pool_i);
    DrawItem *items = calloc(PROBE_MAX_DRAW_ITEMS, sizeof *items);
    uint8_t *atlas = calloc(1, (size_t)PROBE_ATLAS_SIZE * PROBE_ATLAS_SIZE);
    TextMeshEx *texts = calloc(MENU_SWF_DISPLAY_MAX_EDIT_TEXTS, sizeof *texts);
    uint32_t text_used = 0;
    int ok = tv && ti && tf && te && ta && tb && tfill && tl && pool_v &&
             pool_i && items && atlas && texts;
    BatchPool pool = {pool_v, 0, pool_i, 0};
    uint32_t item_count = 0;
    if (!ok) {
        snprintf(reason, reason_cap, "oom probe buffers");
        goto done;
    }

    for (uint32_t i = 0; i < list_result.record_count && i < PROBE_MAX_DRAW_ITEMS; ++i) {
        const MenuSwfDisplayListRecord *record = &records[i];
        DrawItem item;
        memset(&item, 0, sizeof item);
        if (record->has_clip_depth) {
            ++st->mask_records;
            diag(miss, miss_cap, "clip mask depth %s", "not composited");
            continue;
        }
        if (record->is_sprite) {
            ++st->sprite_records;
            continue;
        }
        item.vbase = pool.vertex_count;
        item.ibase = pool.index_count;
        if (menu_swf_shape_find(shapes, record->character_id)) {
            MenuSwfTessMesh mesh;
            memset(&mesh, 0, sizeof mesh);
            mesh.vertices = tv;
            mesh.vertex_capacity = MENU_SWF_TESS_MAX_VERTICES;
            mesh.indices = ti;
            mesh.index_capacity = MENU_SWF_TESS_MAX_INDICES;
            mesh.triangle_fills = tf;
            mesh.triangle_capacity = MENU_SWF_TESS_MAX_TRIANGLES;
            mesh.lines = tl;
            mesh.line_capacity = MENU_SWF_TESS_MAX_LINES;
            mesh.edges = te;
            mesh.edge_capacity = MENU_SWF_TESS_MAX_EDGES;
            mesh.active = ta;
            mesh.active_capacity = MENU_SWF_TESS_MAX_EDGES;
            mesh.bands = tb;
            mesh.band_capacity = MENU_SWF_TESS_MAX_BANDS;
            mesh.fills = tfill;
            mesh.fill_capacity = MENU_SWF_SHAPE_MAX_STYLES;
            s = menu_swf_tess_shape(swf, swf_bytes, record->character_id,
                                    MENU_SWF_TESS_EVEN_ODD, &mesh, reason,
                                    reason_cap);
            if (s != MENU_SWF_OK) {
                ++st->unsupported_records;
                diag(miss, miss_cap, "shape tess refused: %s", reason);
                continue;
            }
            st->stroke_segments += mesh.line_count;
            if (!mesh.triangle_count) {
                ++st->empty_shapes;
                continue;
            }
            int64_t det = (int64_t)record->matrix.m00 * record->matrix.m11 -
                          (int64_t)record->matrix.m01 * record->matrix.m10;
            if (!det) {
                ++st->degenerate_shapes;
                continue;
            }
            st->shape_triangles += mesh.triangle_count;
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
            /* White texel UVs are patched after the atlas exists. */
            MenuSwfBatchResult br;
            MenuSwfBatchStatus bs = menu_swf_batch_shape(
                &mesh, &style, NULL, pool.vertices + pool.vertex_count,
                max_batch_v - pool.vertex_count,
                pool.indices + pool.index_count,
                max_batch_i - pool.index_count, &br, reason, reason_cap);
            if (bs != MENU_SWF_BATCH_OK) {
                ++st->unsupported_records;
                ++st->bitmap_fill_records;
                diag(miss, miss_cap,
                     "shape %u fill refused (bitmap/focal fill needs a decoder): %s",
                     record->character_id, reason);
                continue;
            }
            uint32_t base = pool.vertex_count;
            for (uint32_t k = 0; k < br.index_count; ++k)
                pool.indices[pool.index_count + k] += base;
            st->shape_vertices += br.vertex_count;
            pool.vertex_count += br.vertex_count;
            pool.index_count += br.index_count;
            ++st->shape_records;
            if (getenv("PROBE_DEBUG") && st->shape_records <= 12)
                printf("debug: record shape=%u id=%u matrix=%d,%d,%d,%d t=%d,%d "
                       "verts=%u bounds=%d,%d-%d,%d color(mult/add)=%d,%d,%d,%d/"
                       "%d,%d,%d,%d\n",
                       i, record->character_id, record->matrix.m00,
                       record->matrix.m01, record->matrix.m10,
                       record->matrix.m11, record->matrix.tx, record->matrix.ty,
                       br.vertex_count, br.bounds.xmin, br.bounds.ymin,
                       br.bounds.xmax, br.bounds.ymax, record->color.mult[0],
                       record->color.mult[1], record->color.mult[2],
                       record->color.mult[3], record->color.add[0],
                       record->color.add[1], record->color.add[2],
                       record->color.add[3]);
            item.vcount = br.vertex_count;
            item.icount = br.index_count;
            ++st->render_draw_items;
            items[item_count++] = item;
            continue;
        }
        const MenuSwfEditText *edit =
            edits ? menu_swf_display_find_edit_text(&edit_list,
                                                    record->character_id)
                  : NULL;
        if (edit) {
            char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
            size_t plain_len = 0;
            if (menu_swf_text_edit_plain(edit, plain, sizeof plain, &plain_len,
                                         reason, reason_cap) == MENU_SWF_OK &&
                plain_len) {
                ++st->text_nonempty;
                MenuSwfFontResolution resolution;
                MenuSwfStatus rs = menu_swf_font_resolve(
                    &import_list, &config, &font_list, edit->font_id,
                    font_source_name ? font_source_name : "",
                    &resolution, reason, reason_cap);
                if (rs != MENU_SWF_OK || !resolution.font) {
                    ++st->text_refused;
                    diag(miss, miss_cap, "edit %u font: %s", reason);
                    continue;
                }
                ++st->text_resolved;
                TextMeshEx *t = &texts[text_used];
                t->vertices = malloc(MENU_SWF_TEXT_MAX_VERTICES *
                                     sizeof *t->vertices);
                t->indices = malloc(MENU_SWF_TEXT_MAX_INDICES * sizeof *t->indices);
                t->glyphs = calloc(MENU_SWF_TEXT_MAX_GLYPHS, sizeof *t->glyphs);
                t->bitmaps = calloc(MENU_SWF_TEXT_MAX_GLYPHS,
                                    sizeof *t->bitmaps);
                t->entries = calloc(MENU_SWF_TEXT_MAX_GLYPHS,
                                    sizeof *t->entries);
                if (!t->vertices || !t->indices || !t->glyphs ||
                    !t->bitmaps || !t->entries) {
                    snprintf(reason, reason_cap, "oom text mesh");
                    goto done;
                }
                MenuSwfTextStyle style;
                memset(&style, 0, sizeof style);
                int64_t style_scale;
                if (edit->has_font && edit->font_height) {
                    int64_t denom = 20LL * resolution.font->em_square *
                                    resolution.font->shape_scale;
                    style_scale = ((int64_t)edit->font_height * 65536LL +
                                   denom / 2) / denom;
                } else {
                    style_scale = 65536LL / resolution.font->shape_scale;
                }
                style.m00 = style.m11 = (int32_t)style_scale;
                style.color = edit->has_text_color
                                  ? ((uint32_t)edit->color[3] << 24) |
                                        ((uint32_t)edit->color[0] << 16) |
                                        ((uint32_t)edit->color[1] << 8) |
                                        edit->color[2]
                                  : 0xffffffffu;
                MenuSwfTextRun runs[MENU_SWF_TEXT_MAX_RUNS];
                char html[MENU_SWF_TEXT_MAX_CHARS + 1];
                size_t html_len = 0;
                uint32_t run_count = 0;
                MenuSwfTextHtmlSummary html_summary;
                const MenuSwfFontSummary *run_fonts[MENU_SWF_TEXT_MAX_RUNS];
                memset(run_fonts, 0, sizeof run_fonts);
                if (menu_swf_text_edit_runs(edit, html, sizeof html, &html_len,
                                            runs, MENU_SWF_TEXT_MAX_RUNS,
                                            &run_count, &html_summary, reason,
                                            reason_cap) == MENU_SWF_OK) {
                    for (uint32_t r = 0; r < run_count; ++r) {
                        if (!runs[r].has_face) continue;
                        MenuSwfFontResolution rr;
                        if (menu_swf_font_resolve_face(
                                &config, &font_list, runs[r].face,
                                runs[r].bold, runs[r].italic,
                                font_source_name ? font_source_name : "",
                                &rr, reason, reason_cap) == MENU_SWF_OK)
                            run_fonts[r] = rr.font;
                    }
                }
                memset(&t->mesh, 0, sizeof t->mesh);
                t->mesh.vertices = t->vertices;
                t->mesh.vertex_capacity = MENU_SWF_TEXT_MAX_VERTICES;
                t->mesh.indices = t->indices;
                t->mesh.index_capacity = MENU_SWF_TEXT_MAX_INDICES;
                t->mesh.glyphs = t->glyphs;
                t->mesh.glyph_capacity = MENU_SWF_TEXT_MAX_GLYPHS;
                tess_scratch(&t->mesh.glyph_tess, tv, ti, tf, tl, te, ta, tb,
                             tfill);
                MenuSwfStatus ts = menu_swf_text_layout_edit_resolved(
                    font_source, font_source_bytes, resolution.font,
                    run_fonts, edit, &style, &t->mesh, reason, reason_cap);
                if (ts != MENU_SWF_OK) {
                    ++st->text_refused;
                    diag(miss, miss_cap, "edit %u layout: %s", reason);
                    free(t->vertices); free(t->indices); free(t->glyphs);
                    free(t->bitmaps); free(t->entries);
                    memset(t, 0, sizeof *t);
                    continue;
                }
                t->glyph_count = t->mesh.glyph_count;
                st->text_glyphs += t->mesh.glyph_count;
                st->text_missing_glyphs += t->mesh.missing_glyphs;
                st->text_triangles += t->mesh.triangle_count;
                if (rasterize_text_mesh(t, tv, ti,
                                        &(MenuSwfTessMesh){0}, reason,
                                        reason_cap)) {
                    ++st->text_refused;
                    diag(miss, miss_cap, "edit %u glyph raster: %s", reason);
                    free_text_mesh(t);
                    memset(t, 0, sizeof *t);
                    continue;
                }
                item.is_text = 1;
                items[item_count++] = item;
                ++text_used;
                ++st->text_records;
            }
            continue;
        }
        ++st->unsupported_records;
    }

    /* Pack every text glyph plus one opaque white texel into one A8 atlas. */
    {
        MenuSwfAtlasGlyph *glyphs = calloc(MENU_SWF_ATLAS_MAX_GLYPHS,
                                           sizeof *glyphs);
        uint32_t *map_t = calloc(MENU_SWF_ATLAS_MAX_GLYPHS, sizeof *map_t);
        uint32_t *map_g = calloc(MENU_SWF_ATLAS_MAX_GLYPHS, sizeof *map_g);
        uint32_t count = 0;
        static uint8_t white = 255;
        if (!glyphs || !map_t || !map_g) {
            snprintf(reason, reason_cap, "oom atlas plan");
            free(map_g); free(map_t); free(glyphs);
            goto done;
        }
        for (uint32_t t = 0; t < text_used; ++t) {
            TextMeshEx *tm = &texts[t];
            for (uint32_t g = 0; g < tm->glyph_count; ++g) {
                tm->entries[g].glyph_index = (uint16_t)g;
                if (!tm->bitmaps[g].bitmap) continue;
                if (count >= MENU_SWF_ATLAS_MAX_GLYPHS - 1u) break;
                glyphs[count].pixels = tm->bitmaps[g].bitmap;
                glyphs[count].stride = tm->bitmaps[g].w;
                glyphs[count].width = tm->bitmaps[g].w;
                glyphs[count].height = tm->bitmaps[g].h;
                glyphs[count].glyph_index = (uint16_t)count;
                map_t[count] = t;
                map_g[count] = g;
                ++count;
            }
        }
        uint32_t white_slot = count;
        glyphs[white_slot].pixels = &white;
        glyphs[white_slot].stride = 1;
        glyphs[white_slot].width = glyphs[white_slot].height = 1;
        glyphs[white_slot].glyph_index = 0xffffu;
        uint32_t total_glyphs = count + 1u;

        MenuSwfAtlasTarget atlas_target = {PROBE_ATLAS_SIZE, PROBE_ATLAS_SIZE,
                                           PROBE_ATLAS_SIZE, atlas};
        MenuSwfAtlasEntry *entries = calloc(total_glyphs, sizeof *entries);
        MenuSwfAtlasResult atlas_result;
        MenuSwfAtlasStatus as = entries ? menu_swf_atlas_pack(
            glyphs, total_glyphs, &atlas_target, entries, total_glyphs,
            &atlas_result, reason, reason_cap)
                                       : MENU_SWF_ATLAS_ERR_ARGUMENT;
        if (as != MENU_SWF_ATLAS_OK) {
            snprintf(reason, reason_cap, "atlas: %s", reason);
            free(entries);
            free(map_g); free(map_t); free(glyphs);
            goto done;
        }
        st->atlas_glyphs = atlas_result.placed_glyphs;
        st->atlas_used_height = atlas_result.used_height;
        st->atlas_occupied = atlas_result.occupied_pixels;
        for (uint32_t k = 0; k < count; ++k)
            texts[map_t[k]].entries[map_g[k]] = entries[k];
        const MenuSwfAtlasEntry *white_entry = &entries[white_slot];
        uint32_t wu = white_entry->u + white_entry->uw / 2u;
        uint32_t wv = white_entry->v + white_entry->vh / 2u;
        for (uint32_t i = 0; i < st->shape_vertices; ++i) {
            pool.vertices[i].u = wu;
            pool.vertices[i].v = wv;
        }

        /* Text batches need the packed atlas, so they are appended now while
         * each draw item keeps its own range. */
        for (uint32_t t = 0; t < text_used; ++t) {
            TextMeshEx *tm = &texts[t];
            uint32_t slot = item_count;
            for (uint32_t k = 0; k < item_count; ++k)
                if (items[k].is_text && !items[k].icount) { slot = k; break; }
            if (slot >= item_count) continue;
            MenuSwfBatchStyle style;
            memset(&style, 0, sizeof style);
            style.m00 = style.m11 = 1.0;
            style.color = 0xffffffffu;
            MenuSwfBatchResult br;
            uint32_t vb = pool.vertex_count, ib = pool.index_count;
            MenuSwfBatchStatus bs = menu_swf_batch_text(
                &tm->mesh, tm->entries, tm->glyph_count, &style, NULL,
                pool.vertices + vb, max_batch_v - vb,
                pool.indices + ib, max_batch_i - ib, &br, reason, reason_cap);
            if (bs != MENU_SWF_BATCH_OK) {
                diag(miss, miss_cap, "text batch: %s", reason);
                continue;
            }
            for (uint32_t k = 0; k < br.index_count; ++k)
                pool.indices[ib + k] += vb;
            pool.vertex_count += br.vertex_count;
            pool.index_count += br.index_count;
            items[slot].vbase = vb;
            items[slot].vcount = br.vertex_count;
            items[slot].ibase = ib;
            items[slot].icount = br.index_count;
            st->text_quads += br.quad_count;
        }
        free(entries);
        free(map_g);
        free(map_t);
        free(glyphs);
    }

    /* Composite in display order. */
    out->w = out_w;
    out->h = out_h;
    out->pixels = calloc((size_t)out_w * out_h, 4);
    if (!out->pixels) { snprintf(reason, reason_cap, "oom rgba"); goto done; }
    for (uint32_t k = 0; k < item_count; ++k) {
        DrawItem *item = &items[k];
        if (!item->icount) continue;
        draw_triangles(out, pool.vertices + item->vbase,
                       pool.indices + item->ibase, item->icount, atlas,
                       PROBE_ATLAS_SIZE, PROBE_ATLAS_SIZE, scale);
        st->render_triangles += item->icount / 3u;
    }
    for (uint32_t i = 0; i < out->w * out->h; ++i) {
        if (!out->pixels[i * 4u + 3]) continue;
        ++st->opaque_pixels;
        uint32_t x = i % out->w, y = i / out->w;
        if (!st->has_bbox) {
            st->bbox_x0 = st->bbox_x1 = x;
            st->bbox_y0 = st->bbox_y1 = y;
            st->has_bbox = 1;
        } else {
            if (x < st->bbox_x0) st->bbox_x0 = x;
            if (x > st->bbox_x1) st->bbox_x1 = x;
            if (y < st->bbox_y0) st->bbox_y0 = y;
            if (y > st->bbox_y1) st->bbox_y1 = y;
        }
    }
    st->rgba_w = out_w;
    st->rgba_h = out_h;
    ok = 1;

done:
    if (st->stroke_segments)
        diag(miss, miss_cap, "stroked segments: %s",
             "line styles have no host stroke rasteriser");
    if (st->sprite_records)
        diag(miss, miss_cap, "sprite records: %s",
             "expanded, but sprite-local masks/AVM state are not simulated");
    if (st->mask_records)
        diag(miss, miss_cap, "mask/clip records: %s",
             "clip-depth compositing is not implemented");
    if (st->bitmap_fill_styles)
        diag(miss, miss_cap, "bitmap FILLSTYLE: %s",
             "DefineBitsLossless/JPEG decode is not wired to the batch");
    if (st->avm_tags)
        diag(miss, miss_cap, "DoAction tags: %s",
             "AVM/AS2 is not executed, so dynamic labels are empty");
    for (uint32_t t = 0; t < text_used; ++t) free_text_mesh(&texts[t]);
    free(texts);
    free(atlas);
    free(items);
    free(pool_i);
    free(pool_v);
    free(tl);
    free(tfill);
    free(tb);
    free(ta);
    free(te);
    free(tf);
    free(ti);
    free(tv);
    free(font_items);
    free(imports);
    free(edits);
    free(shapes);
    free(records);
    if (!ok && out->pixels) { free(out->pixels); out->pixels = NULL; }
    return ok ? 0 : -1;
}

/* ------------------------------------------------------- synthetic SWF --- */

typedef struct { uint8_t *p; size_t len, cap; int bits, bitbuf; } Writer;

static int w_reserve(Writer *w, size_t n) {
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 256;
        while (cap < w->len + n) cap *= 2;
        uint8_t *p = realloc(w->p, cap);
        if (!p) return -1;
        w->p = p;
        w->cap = cap;
    }
    return 0;
}
static void w_u8(Writer *w, uint8_t v) { if (!w_reserve(w, 1)) w->p[w->len++] = v; }
static void w_u16(Writer *w, uint16_t v) {
    w_u8(w, (uint8_t)v); w_u8(w, (uint8_t)(v >> 8));
}
static void w_tag(Writer *w, uint16_t code, const uint8_t *payload, size_t len) {
    w_u16(w, (uint16_t)((code << 6) | (len < 0x3f ? len : 0x3f)));
    if (len >= 0x3f) {
        w_u8(w, (uint8_t)len);
        w_u8(w, (uint8_t)(len >> 8));
        w_u8(w, (uint8_t)(len >> 16));
        w_u8(w, (uint8_t)(len >> 24));
    }
    for (size_t i = 0; i < len; ++i) w_u8(w, payload[i]);
}
static void w_bits(Writer *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) {
        w->bitbuf = (w->bitbuf << 1) | ((v >> i) & 1u);
        if (++w->bits == 8) { w_u8(w, (uint8_t)w->bitbuf); w->bits = 0; w->bitbuf = 0; }
    }
}
static void w_align(Writer *w) {
    if (w->bits) { w->bitbuf <<= (8 - w->bits); w_u8(w, (uint8_t)w->bitbuf); w->bits = 0; w->bitbuf = 0; }
}
static void w_sbits(Writer *w, int32_t v, int n) {
    uint32_t mask = n >= 32 ? 0xffffffffu : ((1u << n) - 1u);
    w_bits(w, (uint32_t)v & mask, n);
}

static uint8_t *build_synthetic_swf(size_t *out_len) {
    Writer body = {0}, file = {0};
    /* Frame 100x100 px = 2000x2000 twips. RECT: nbits=15, 0,2000,0,2000. */
    w_bits(&body, 15, 5);
    w_sbits(&body, 0, 15); w_sbits(&body, 2000, 15);
    w_sbits(&body, 0, 15); w_sbits(&body, 2000, 15);
    w_align(&body);
    w_u16(&body, 0x1800); /* 24 fps in 8.8 */
    w_u16(&body, 1);

    /* DefineShape (code 2): solid opaque red rectangle over the frame. */
    Writer shape = {0};
    w_u16(&shape, 1);                 /* shape id */
    w_bits(&shape, 15, 5);
    w_sbits(&shape, 0, 15); w_sbits(&shape, 2000, 15);
    w_sbits(&shape, 0, 15); w_sbits(&shape, 2000, 15);
    w_align(&shape);
    w_u8(&shape, 1);                  /* one fill style */
    w_u8(&shape, 0x00);               /* solid */
    w_u8(&shape, 0xd0); w_u8(&shape, 0x20); w_u8(&shape, 0x20);
    w_u8(&shape, 0);                  /* no line styles */
    w_bits(&shape, 4, 4);             /* fill bits */
    w_bits(&shape, 0, 4);             /* line bits */
    /* StyleChange: fill1=1, move to (0,0) with 15-bit deltas. */
    w_bits(&shape, 0, 1);             /* style change */
    w_bits(&shape, 0, 1);             /* state new styles */
    w_bits(&shape, 0, 1);             /* state line style */
    w_bits(&shape, 1, 1);             /* state fill style 1 */
    w_bits(&shape, 0, 1);             /* state fill style 0 */
    w_bits(&shape, 1, 1);             /* move to */
    w_bits(&shape, 15, 5);            /* move bits */
    w_sbits(&shape, 0, 15); w_sbits(&shape, 0, 15);
    w_bits(&shape, 1, 4);             /* fill1 = 1 (f0 is absent) */
    /* Four straight edges. NumBits=12 means the stored deltas are 12+2 bits. */
    w_bits(&shape, 1, 1); w_bits(&shape, 1, 1); w_bits(&shape, 12, 4);
    w_bits(&shape, 1, 1); w_sbits(&shape, 2000, 14); w_sbits(&shape, 0, 14);
    w_bits(&shape, 1, 1); w_bits(&shape, 1, 1); w_bits(&shape, 12, 4);
    w_bits(&shape, 1, 1); w_sbits(&shape, 0, 14); w_sbits(&shape, 2000, 14);
    w_bits(&shape, 1, 1); w_bits(&shape, 1, 1); w_bits(&shape, 12, 4);
    w_bits(&shape, 1, 1); w_sbits(&shape, -2000, 14); w_sbits(&shape, 0, 14);
    w_bits(&shape, 1, 1); w_bits(&shape, 1, 1); w_bits(&shape, 12, 4);
    w_bits(&shape, 1, 1); w_sbits(&shape, 0, 14); w_sbits(&shape, -2000, 14);
    w_bits(&shape, 0, 1); w_bits(&shape, 0, 1); /* EndShapeRecord */
    w_align(&shape);
    w_tag(&body, 2, shape.p, shape.len);

    /* PlaceObject2: has_character, depth 1. */
    uint8_t place[5] = {0x02, 0x01, 0x00, 0x01, 0x00};
    w_tag(&body, 26, place, sizeof place);
    w_tag(&body, 1, NULL, 0);  /* ShowFrame */
    w_tag(&body, 0, NULL, 0);  /* End */

    size_t total = 8 + body.len;
    w_u8(&file, 'F'); w_u8(&file, 'W'); w_u8(&file, 'S');
    w_u8(&file, 6);
    w_u8(&file, (uint8_t)total); w_u8(&file, (uint8_t)(total >> 8));
    w_u8(&file, (uint8_t)(total >> 16)); w_u8(&file, (uint8_t)(total >> 24));
    for (size_t i = 0; i < body.len; ++i) w_u8(&file, body.p[i]);
    free(shape.p);
    free(body.p);
    *out_len = file.len;
    return file.p;
}

/* ------------------------------------------------------------ JSON ------- */

static void write_json(FILE *f, const ProbeStats *s, const char *miss,
                       int ok, const char *reason) {
    fprintf(f, "{\n");
    fprintf(f, "  \"ok\": %d,\n", ok);
    fprintf(f, "  \"diagnostic_only\": true,\n");
    fprintf(f, "  \"reason\": \"%s\",\n", ok ? "" : reason);
    fprintf(f, "  \"swf_bytes\": %u,\n", s->swf_bytes);
    fprintf(f, "  \"declared_bytes\": %u,\n", s->declared_bytes);
    fprintf(f, "  \"format\": %u,\n", s->format);
    fprintf(f, "  \"tags\": %u, \"top_level_tags\": %u,\n", s->tags, s->top_tags);
    fprintf(f, "  \"define_shape\": %u, \"define_font2\": %u, \"define_font3\": %u,\n",
            s->define_shape, s->define_font2, s->define_font3);
    fprintf(f, "  \"define_sprite\": %u, \"show_frame\": %u, \"do_action\": %u,\n",
            s->define_sprite, s->show_frame, s->do_action);
    fprintf(f, "  \"define_bits_lossless\": %u, \"define_bits_jpeg\": %u,\n",
            s->bits_lossless, s->bits_jpeg);
    fprintf(f, "  \"scene_twips\": [%d, %d],\n", s->scene_w_twips,
            s->scene_h_twips);
    fprintf(f, "  \"records\": %u, \"sprite_records\": %u, \"mask_records\": %u,\n",
            s->records, s->sprite_records, s->mask_records);
    fprintf(f, "  \"shape_records\": %u, \"text_records\": %u, \"unsupported_records\": %u,\n",
            s->shape_records, s->text_records, s->unsupported_records);
    fprintf(f, "  \"empty_shapes\": %u, \"degenerate_shapes\": %u,\n",
            s->empty_shapes, s->degenerate_shapes);
    fprintf(f, "  \"shape_triangles\": %u, \"shape_vertices\": %u, \"stroke_segments\": %u,\n",
            s->shape_triangles, s->shape_vertices, s->stroke_segments);
    fprintf(f, "  \"fill_styles\": {\"solid\": %u, \"gradient\": %u, \"focal\": %u, \"bitmap\": %u},\n",
            s->solid_fill_styles, s->gradient_fill_styles, s->focal_fill_styles,
            s->bitmap_fill_styles);
    fprintf(f, "  \"bitmap_fill_records\": %u,\n", s->bitmap_fill_records);
    fprintf(f, "  \"text_definitions\": %u, \"text_nonempty\": %u, \"text_resolved\": %u,\n",
            s->text_definitions, s->text_nonempty, s->text_resolved);
    fprintf(f, "  \"text_refused\": %u, \"text_glyphs\": %u, \"text_missing_glyphs\": %u,\n",
            s->text_refused, s->text_glyphs, s->text_missing_glyphs);
    fprintf(f, "  \"text_triangles\": %u, \"text_quads\": %u,\n",
            s->text_triangles, s->text_quads);
    fprintf(f, "  \"atlas\": {\"size\": %u, \"placed_glyphs\": %u, \"used_height\": %u, \"occupied_pixels\": %u},\n",
            PROBE_ATLAS_SIZE, s->atlas_glyphs, s->atlas_used_height,
            s->atlas_occupied);
    fprintf(f, "  \"rgba\": {\"width\": %u, \"height\": %u, \"opaque_pixels\": %llu,\n",
            s->rgba_w, s->rgba_h, (unsigned long long)s->opaque_pixels);
    if (s->has_bbox)
        fprintf(f, "           \"bbox\": [%u, %u, %u, %u]},\n", s->bbox_x0,
                s->bbox_y0, s->bbox_x1 + 1, s->bbox_y1 + 1);
    else
        fprintf(f, "           \"bbox\": null},\n");
    fprintf(f, "  \"render_triangles\": %u, \"render_draw_items\": %u,\n",
            s->render_triangles, s->render_draw_items);
    fprintf(f, "  \"font_source_fallback\": %u,\n", s->font_source_fallback);
    fprintf(f, "  \"missing\": \"%s\"\n", miss);
    fprintf(f, "}\n");
}

static int self_test(void) {
    size_t len = 0;
    uint8_t *swf = build_synthetic_swf(&len);
    if (!swf) { printf("self-test: synthetic SWF build failed\n"); return 1; }
    Rgba img;
    ProbeStats st;
    char miss[2048] = "", reason[256] = "";
    int rc = render_swf(swf, len, NULL, 0, "", NULL, 0, 0, 0, &img, &st, miss,
                        sizeof miss, reason, sizeof reason);
    int fail = 0;
    if (getenv("PROBE_DEBUG")) {
        MenuSwfShapeResult *sh = calloc(1, sizeof *sh);
        char r2[256] = "";
        MenuSwfStatus shs = sh ? menu_swf_shape_summary(swf, len, sh, r2,
                                                        sizeof r2)
                               : MENU_SWF_ERR_ARGUMENT;
        printf("debug: shape summary status=%d reason='%s' total=%u paths=%u "
               "fills=%u\n", (int)shs, r2, sh ? sh->shape_total : 0,
               sh && sh->shape_total ? sh->shapes[0].path_count : 0,
               sh && sh->shape_total ? sh->shapes[0].fill_count : 0);
        free(sh);
    }
    if (rc) { printf("self-test: render failed: %s\n", reason); fail = 1; }
    if (!fail && getenv("PROBE_DEBUG")) {
        MenuSwfShapeResult *sh = calloc(1, sizeof *sh);
        char r2[256] = "";
        if (sh && menu_swf_shape_summary(swf, len, sh, r2, sizeof r2) ==
                      MENU_SWF_OK) {
            printf("debug: shapes total=%u stored=%u paths=%u fills=%u\n",
                   sh->shape_total, sh->stored_shapes,
                   sh->shapes[0].path_count, sh->shapes[0].fill_count);
            for (uint32_t k = 0; k < sh->shapes[0].path_count; ++k)
                printf("debug: path %u fill0=%u fill1=%u segs=%u closed=%u\n",
                       k, sh->shape_paths[k].fill0, sh->shape_paths[k].fill1,
                       sh->shape_paths[k].segment_count,
                       sh->shape_paths[k].closed);
        } else {
            printf("debug: shape summary failed: %s\n", r2);
        }
        free(sh);
    }
    if (!fail && (!st.shape_records || !st.shape_triangles)) {
        printf("self-test: synthetic shape did not tessellate "
               "(bytes=%u tags=%u records=%u shapes=%u unsupported=%u empty=%u "
               "reason=%s)\n",
               st.swf_bytes, st.tags, st.records, st.shape_records,
               st.unsupported_records, st.empty_shapes, reason);
        fail = 1;
    }
    if (!fail && st.opaque_pixels == 0) {
        printf("self-test: synthetic frame is empty\n");
        fail = 1;
    }
    if (!fail && (st.rgba_w != 100 || st.rgba_h != 100)) {
        printf("self-test: unexpected size %ux%u\n", st.rgba_w, st.rgba_h);
        fail = 1;
    }
    if (!fail) {
        printf("self-test: synthetic SWF %u bytes, %u tags, %u shape records, "
               "%llu opaque pixels, bbox %u,%u-%u,%u PASS\n",
               st.swf_bytes, st.tags, st.shape_records,
               (unsigned long long)st.opaque_pixels, st.bbox_x0, st.bbox_y0,
               st.bbox_x1 + 1, st.bbox_y1 + 1);
    }
    free(img.pixels);
    free(swf);
    return fail;
}

int main(int argc, char **argv) {
    const char *swf_path = NULL, *raw_path = NULL, *json_path = NULL;
    const char *font_path = NULL, *config_path = NULL;
    int width = 0, height = 0, do_self = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--self-test")) do_self = 1;
        else if (!strcmp(argv[i], "--swf") && i + 1 < argc) swf_path = argv[++i];
        else if (!strcmp(argv[i], "--out-raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "--out-json") && i + 1 < argc) json_path = argv[++i];
        else if (!strcmp(argv[i], "--font-source") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--fontconfig") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (do_self) return self_test();
    if (!swf_path || !raw_path || !json_path) {
        fprintf(stderr,
                "usage: %s --swf FILE --out-raw FILE.rgba --out-json FILE.json "
                "[--font-source FILE] [--fontconfig FILE]\n",
                argv[0]);
        return 2;
    }

    size_t swf_bytes = 0, font_bytes = 0, config_bytes = 0;
    uint8_t *swf = read_file(swf_path, &swf_bytes);
    uint8_t *font = font_path ? read_file(font_path, &font_bytes) : NULL;
    uint8_t *config = config_path ? read_file(config_path, &config_bytes) : NULL;
    if (!swf) {
        fprintf(stderr, "cannot read %s\n", swf_path);
        return 1;
    }
    Rgba img;
    ProbeStats st;
    char miss[4096] = "", reason[512] = "";
    int rc = render_swf(swf, swf_bytes, font, font_bytes, font_path ? font_path : "",
                        config, config_bytes, width, height, &img, &st, miss,
                        sizeof miss, reason, sizeof reason);
    FILE *jf = fopen(json_path, "wb");
    if (jf) {
        write_json(jf, &st, miss, rc == 0, reason);
        fclose(jf);
    }
    if (rc) {
        fprintf(stderr, "probe failed: %s\n", reason);
        free(img.pixels); free(config); free(font); free(swf);
        return 1;
    }
    FILE *rf = fopen(raw_path, "wb");
    if (!rf) {
        fprintf(stderr, "cannot write %s\n", raw_path);
        free(img.pixels); free(config); free(font); free(swf);
        return 1;
    }
    size_t wrote = fwrite(img.pixels, 4, (size_t)img.w * img.h, rf);
    fclose(rf);
    free(img.pixels);
    free(config);
    free(font);
    free(swf);
    if (wrote != (size_t)st.rgba_w * st.rgba_h) {
        fprintf(stderr, "short raw write\n");
        return 1;
    }
    printf("scene_twips=%dx%d rgba=%ux%u opaque_pixels=%llu\n",
           st.scene_w_twips, st.scene_h_twips, st.rgba_w, st.rgba_h,
           (unsigned long long)st.opaque_pixels);
    printf("diagnostic frame written (not an engine frame): %s\n", raw_path);
    return 0;
}
