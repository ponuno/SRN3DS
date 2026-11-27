/* Font face resolution regression (real fonts_en.swf + fontconfig.txt).
 *
 * The shipped StartMenu.swf imports font ids 34 ($EverywhereMediumFont) and
 * 41 ($EverywhereFont) from gfxfontlib.swf.  Those exact ids are absent from
 * Interface/fonts_en.swf, but the shipped Interface/fontconfig.txt maps the
 * symbols to real faces (Futura Condensed / Futura CondensedLight).  This test
 * proves the alias -> face -> DefineFont chain against the retail files,
 * lays out the three authored menu labels with real glyph outlines, rasterises
 * them, and keeps the combined 16-bit batch range valid.
 */

#include "bsa_reader.h"
#include "menu_swf_atlas.h"
#include "menu_swf_batch.h"
#include "menu_swf_display.h"
#include "menu_swf_font.h"
#include "menu_swf_raster.h"
#include "menu_swf_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REAL_ATLAS_SIZE 64u
#define REAL_TEXT_ATLAS_SIZE 512u
#define REAL_RASTER_SIZE 64u
#define REAL_WHITE_GLYPH 0xffffu

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

static void tess_init(MenuSwfTessMesh *mesh) {
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
    MenuSwfTextVertex vertices[262144];
    uint32_t indices[786432];
    MenuSwfTextGlyph glyphs[MENU_SWF_TEXT_MAX_GLYPHS];
} TextFixture;

static TextFixture text_fixture;

static void text_init(MenuSwfTextMesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = text_fixture.vertices;
    mesh->vertex_capacity = 262144u;
    mesh->indices = text_fixture.indices;
    mesh->index_capacity = 786432u;
    mesh->glyphs = text_fixture.glyphs;
    mesh->glyph_capacity = MENU_SWF_TEXT_MAX_GLYPHS;
    tess_init(&mesh->glyph_tess);
}

static uint8_t *read_bsa(const char *archive_path, const char *relative,
                         size_t *out_size) {
    BsaArchive *archive = NULL;
    if (bsa_open_path(archive_path, NULL, &archive, NULL, 0) != BSA_OK)
        return NULL;
    BsaMatch match;
    uint8_t *bytes = NULL;
    if (bsa_find(archive, relative, &match) == BSA_OK) {
        uint32_t size = 0;
        char reason[BSA_REASON_MAX];
        if (bsa_unpacked_size(archive, &match.entry, &size, reason,
                              sizeof reason) == BSA_OK) {
            bytes = malloc(size ? size : 1);
            size_t got = 0;
            if (bytes && (bsa_read(archive, &match.entry, bytes, size, &got,
                                   reason, sizeof reason) != BSA_OK ||
                          got != size)) {
                free(bytes);
                bytes = NULL;
            }
            if (bytes) *out_size = size;
        }
    }
    bsa_close(archive);
    return bytes;
}

static void test_fontconfig_text(void) {
    const char *text =
        "fontlib \"Interface\\fonts_en.swf\"\r\n"
        "map \"$EverywhereFont\" = \"Futura CondensedLight\" Normal\r\n"
        "map \"$EverywhereBoldFont\" = \"Futura Condensed\" Bold\r\n"
        "validNameChars \"abc\"\r\n";
    MenuSwfFontConfig config;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_fontconfig_parse(text, strlen(text), &config,
                                                 reason, sizeof reason);
    check(st == MENU_SWF_OK && config.count == 2 && config.fonts == 1,
          "fontconfig parses two maps and one fontlib");
    const MenuSwfFontMap *light = menu_swf_fontconfig_find(
        &config, "$everywherefont");
    const MenuSwfFontMap *bold = menu_swf_fontconfig_find(
        &config, "$EverywhereBoldFont");
    check(light && !strcmp(light->face, "Futura CondensedLight") &&
              !light->bold && !light->italic,
          "fontconfig alias lookup is case-insensitive");
    check(bold && !strcmp(bold->face, "Futura Condensed") && bold->bold &&
              !bold->italic,
          "fontconfig keeps the Bold style");

    const char *bad = "map $EverywhereFont = \"X\" Normal\n";
    check(menu_swf_fontconfig_parse(bad, strlen(bad), &config, reason,
                                    sizeof reason) == MENU_SWF_ERR_FORMAT &&
              strstr(reason, "alias is not quoted"),
          "unquoted fontconfig alias is a named refusal");
    const char *dup = "map \"$A\" = \"X\" Normal\nmap \"$a\" = \"Y\" Bold\n";
    check(menu_swf_fontconfig_parse(dup, strlen(dup), &config, reason,
                                    sizeof reason) == MENU_SWF_ERR_FORMAT &&
              strstr(reason, "duplicate alias"),
          "duplicate fontconfig alias is a named refusal");
}

static void test_real(const char *data_dir) {
    char path[1024];
    char reason[BSA_REASON_MAX];
    snprintf(path, sizeof path, "%s/Update.bsa", data_dir);
    size_t swf_size = 0;
    uint8_t *swf = read_bsa(path, "interface/startmenu.swf", &swf_size);
    if (!swf) {
        snprintf(path, sizeof path, "%s/Skyrim - Interface.bsa", data_dir);
        swf = read_bsa(path, "interface/startmenu.swf", &swf_size);
    }
    snprintf(path, sizeof path, "%s/Skyrim - Interface.bsa", data_dir);
    size_t font_size = 0, config_size = 0;
    uint8_t *fonts = read_bsa(path, "interface/fonts_en.swf", &font_size);
    uint8_t *config_text = read_bsa(path, "interface/fontconfig.txt",
                                    &config_size);
    check(swf && fonts && config_text,
          "retail StartMenu, fonts_en.swf and fontconfig.txt loaded");
    if (!swf || !fonts || !config_text) goto done;

    MenuSwfImportAssets import_items[4];
    MenuSwfImportList imports = {import_items, 4, 0, 0, 0};
    MenuSwfStatus st = menu_swf_collect_imports(swf, swf_size, &imports,
                                                 reason, sizeof reason);
    check(st == MENU_SWF_OK && imports.count == 2,
          "retail two ImportAssets2 font records");
    uint16_t id34 = 0, id41 = 0;
    for (uint32_t i = 0; i < imports.count; ++i)
        for (uint32_t j = 0; j < import_items[i].symbol_count; ++j) {
            if (!strcmp(import_items[i].symbols[j].name,
                        "$EverywhereMediumFont"))
                id34 = import_items[i].symbols[j].id;
            if (!strcmp(import_items[i].symbols[j].name, "$EverywhereFont"))
                id41 = import_items[i].symbols[j].id;
        }
    check(id34 == 34 && id41 == 41, "retail font import ids 34/41");

    MenuSwfFontSummary *font_items = calloc(32, sizeof *font_items);
    check(font_items != NULL, "font candidate allocation");
    if (!font_items) goto done;
    MenuSwfFontList font_list = {font_items, 32, 0, 0, 0};
    st = menu_swf_display_collect_fonts(fonts, font_size, &font_list, reason,
                                        sizeof reason);
    check(st == MENU_SWF_OK && font_list.count == 10 && font_list.skipped == 2,
          "fonts_en yields 10 bounded fonts with 2 oversized skipped");
    if (st != MENU_SWF_OK) {
        free(font_items);
        goto done;
    }

    MenuSwfFontConfig config;
    st = menu_swf_fontconfig_parse((const char *)config_text, config_size,
                                   &config, reason, sizeof reason);
    check(st == MENU_SWF_OK && config.count == 16 && config.fonts == 2,
          "retail fontconfig has all 16 aliases");
    if (st != MENU_SWF_OK) {
        free(font_items);
        goto done;
    }

    MenuSwfFontResolution res34, res41, exact, missing;
    check(menu_swf_font_resolve(&imports, &config, &font_list, id34,
                                "fonts_en.swf", &res34, reason,
                                sizeof reason) == MENU_SWF_OK &&
              res34.kind == MENU_SWF_FONT_MATCH_FACE &&
              res34.requested_id == 34 && res34.resolved_id == 3 &&
              res34.font->glyph_count == 187 &&
              !strcmp(res34.import_name, "$EverywhereMediumFont") &&
              !strcmp(res34.face, "Futura Condensed"),
          "id 34 -> Futura Condensed -> DefineFont3 id 3");
    check(menu_swf_font_resolve(&imports, &config, &font_list, id41,
                                "fonts_en.swf", &res41, reason,
                                sizeof reason) == MENU_SWF_OK &&
              res41.kind == MENU_SWF_FONT_MATCH_FACE &&
              res41.requested_id == 41 && res41.resolved_id == 1 &&
              res41.font->glyph_count == 187 &&
              !strcmp(res41.import_name, "$EverywhereFont") &&
              !strcmp(res41.face, "Futura CondensedLight"),
          "id 41 -> Futura CondensedLight -> DefineFont3 id 1");
    check(menu_swf_font_resolve(&imports, &config, &font_list, 3,
                                "fonts_en.swf", &exact, reason,
                                sizeof reason) == MENU_SWF_OK &&
              exact.kind == MENU_SWF_FONT_MATCH_EXACT_ID &&
              exact.resolved_id == 3,
          "a library id keeps the exact-id path");
    check(menu_swf_font_resolve(&imports, &config, &font_list, 77,
                                "fonts_en.swf", &missing, reason,
                                sizeof reason) == MENU_SWF_ERR_NOTIMPL &&
              strstr(reason, "id 77 is not declared"),
          "undeclared imported id refuses instead of guessing");
    check(menu_swf_font_resolve_face(&config, &font_list, "Not A Face", 0, 0,
                                     "fonts_en.swf", &missing, reason,
                                     sizeof reason) == MENU_SWF_ERR_NOTIMPL &&
              strstr(reason, "face 'Not A Face' Normal is not present"),
          "unknown face refuses by exact name");

    MenuSwfEditText *edit_items = calloc(MENU_SWF_DISPLAY_MAX_EDIT_TEXTS,
                                         sizeof *edit_items);
    check(edit_items != NULL, "edit-text allocation");
    if (!edit_items) {
        free(font_items);
        goto done;
    }
    MenuSwfEditTextList edits = {edit_items, MENU_SWF_DISPLAY_MAX_EDIT_TEXTS,
                                 0, 0, 0};
    st = menu_swf_display_collect_edit_texts(swf, swf_size, &edits, reason,
                                              sizeof reason);
    check(st == MENU_SWF_OK && edits.count == 22,
          "retail 22 DefineEditText definitions");

    uint32_t nonempty = 0, glyphs = 0, missing_glyphs = 0;
    uint32_t vertices = 0, indices = 0, triangles = 0;
    uint32_t substituted = 0, layout_records = 0;
    uint32_t batch_glyph_count = 0, geometry_slots = 0, empty_slots = 0;
    uint32_t zero_advances = 0, raster_refusals = 0;
    uint32_t letter_spacing_attrs = 0, kerning_attrs = 0, unknown_attrs = 0;
    uint32_t label_mask = 0;
    MenuSwfTextGlyph batch_glyphs[MENU_SWF_TEXT_MAX_GLYPHS];
    MenuSwfAtlasGlyph atlas_glyphs[MENU_SWF_TEXT_MAX_GLYPHS];
    uint8_t *atlas_owned[MENU_SWF_TEXT_MAX_GLYPHS];
    memset(atlas_glyphs, 0, sizeof atlas_glyphs);
    memset(atlas_owned, 0, sizeof atlas_owned);
    if (st == MENU_SWF_OK) {
        char plain[MENU_SWF_TEXT_MAX_CHARS + 1];
        for (uint32_t i = 0; i < edits.count; ++i) {
            const MenuSwfEditText *edit = &edit_items[i];
            size_t len = 0;
            st = menu_swf_text_edit_plain(edit, plain, sizeof plain, &len,
                                          reason, sizeof reason);
            if (st != MENU_SWF_OK || !len) continue;
            ++nonempty;
            if (!strcmp(plain, "$Loading extra content.")) label_mask |= 1u;
            else if (!strcmp(plain, "$Loading downloadable content...")) label_mask |= 2u;
            else if (!strcmp(plain, "Start a New Game?")) label_mask |= 4u;
            printf("real: label id=%u text=\"%s\"\n", edit->character_id, plain);
            MenuSwfFontResolution res;
            st = menu_swf_font_resolve(&imports, &config, &font_list,
                                       edit->font_id, "fonts_en.swf", &res,
                                       reason, sizeof reason);
            check(st == MENU_SWF_OK && res.font != NULL,
                  "every authored edit font resolves");
            if (st != MENU_SWF_OK) continue;
            if (res.kind == MENU_SWF_FONT_MATCH_FACE) ++substituted;

            char run_text[MENU_SWF_TEXT_MAX_CHARS + 1];
            MenuSwfTextRun runs[MENU_SWF_TEXT_MAX_RUNS];
            MenuSwfTextHtmlSummary html;
            size_t run_len = 0;
            uint32_t run_count = 0;
            st = menu_swf_text_edit_runs(
                edit, run_text, sizeof run_text, &run_len, runs,
                MENU_SWF_TEXT_MAX_RUNS, &run_count, &html, reason,
                sizeof reason);
            if (st != MENU_SWF_OK) continue;
            letter_spacing_attrs += html.letter_spacing_attrs;
            kerning_attrs += html.kerning_attrs;
            unknown_attrs += html.unknown_attrs;
            const MenuSwfFontSummary *run_fonts[MENU_SWF_TEXT_MAX_RUNS];
            memset(run_fonts, 0, sizeof run_fonts);
            int run_fonts_ok = 1;
            for (uint32_t r = 0; r < run_count; ++r) {
                if (!runs[r].has_face) continue;
                MenuSwfFontResolution rr;
                st = menu_swf_font_resolve_face(&config, &font_list,
                                                 runs[r].face, runs[r].bold,
                                                 runs[r].italic, "fonts_en.swf",
                                                 &rr, reason, sizeof reason);
                if (st != MENU_SWF_OK) {
                    printf("real: edit=%u run=%u face='%s' refused: %s\n",
                           edit->character_id, r, runs[r].face, reason);
                    run_fonts_ok = 0;
                    break;
                }
                run_fonts[r] = rr.font;
            }
            if (!run_fonts_ok) continue;

            MenuSwfTextMesh mesh;
            text_init(&mesh);
            MenuSwfTextStyle style;
            memset(&style, 0, sizeof style);
            int64_t denom = 20LL * res.font->em_square * res.font->shape_scale;
            int64_t scale = ((int64_t)edit->font_height * 65536LL + denom / 2) /
                            denom;
            if (scale < 1) scale = 1;
            style.m00 = style.m11 = (int32_t)scale;
            style.color = 0xffffffffu;
            st = menu_swf_text_layout_edit_resolved(
                fonts, font_size, res.font, run_fonts, edit, &style, &mesh,
                reason, sizeof reason);
            if (st != MENU_SWF_OK) {
                printf("real: edit=%u layout refused: %s (%s)\n",
                       edit->character_id, menu_swf_status_name(st), reason);
                ++failures;
                continue;
            }
            ++layout_records;
            glyphs += mesh.glyph_count;
            missing_glyphs += mesh.missing_glyphs;
            vertices += mesh.vertex_count;
            indices += mesh.index_count;
            triangles += mesh.triangle_count;
            printf("real: edit=%u font=%u->%u glyphs=%u missing=%u "
                   "vertices=%u indices=%u triangles=%u\n",
                   edit->character_id, res.requested_id, res.resolved_id,
                   mesh.glyph_count, mesh.missing_glyphs, mesh.vertex_count,
                   mesh.index_count, mesh.triangle_count);

            /* Preserve each real placement, then rasterise its resolved outline
             * at the exact layout scale into a local A8 rectangle.  Repeated
             * characters remain separate atlas inputs so the resulting entry
             * array is directly aligned with the combined text mesh. */
            for (uint32_t j = 0; j < mesh.glyph_count; ++j) {
                if (batch_glyph_count >= MENU_SWF_TEXT_MAX_GLYPHS) {
                    ++failures;
                    break;
                }
                uint32_t slot = batch_glyph_count++;
                const MenuSwfTextGlyph *g = &mesh.glyphs[j];
                batch_glyphs[slot] = *g;
                MenuSwfAtlasGlyph *ag = &atlas_glyphs[slot];
                memset(ag, 0, sizeof *ag);
                ag->glyph_index = (uint16_t)slot;
                if (!g->has_geometry) {
                    ++empty_slots;
                    continue;
                }
                ++geometry_slots;
                if (!g->advance && g->code != ' ') ++zero_advances;
                uint32_t r = g->run_index;
                const MenuSwfFontSummary *rf =
                    r < run_count && run_fonts[r] ? run_fonts[r] : res.font;
                MenuSwfTextMesh glyph_mesh;
                text_init(&glyph_mesh);
                st = menu_swf_tess_glyph(
                    fonts, font_size, rf->font_id, g->code,
                    MENU_SWF_TESS_NON_ZERO, &glyph_mesh.glyph_tess, reason,
                    sizeof reason);
                if (st != MENU_SWF_OK) {
                    printf("real: glyph code=%u tess refused: %s\n", g->code,
                           reason);
                    ++raster_refusals;
                    continue;
                }
                int32_t width = g->bounds.xmax - g->bounds.xmin;
                int32_t height = g->bounds.ymax - g->bounds.ymin;
                if (width <= 0 || height <= 0) {
                    ++raster_refusals;
                    continue;
                }
                uint8_t *pixels = malloc((size_t)width * height);
                if (!pixels) {
                    ++raster_refusals;
                    continue;
                }
                atlas_owned[slot] = pixels;
                double run_scale =
                    r < run_count && runs[r].has_size
                        ? (double)runs[r].size / ((double)edit->font_height / 20.0)
                        : 1.0;
                double sx = ((double)style.m00 / 65536.0) * run_scale;
                double sy = ((double)style.m11 / 65536.0) * run_scale;
                MenuSwfRasterTransform transform;
                memset(&transform, 0, sizeof transform);
                transform.m00 = sx;
                transform.m11 = sy;
                transform.tx = (double)g->x - g->bounds.xmin;
                transform.ty = (double)g->y - g->bounds.ymin;
                MenuSwfRasterTarget target = {
                    (uint32_t)width, (uint32_t)height, (uint32_t)width,
                    pixels, g->advance, 0};
                MenuSwfRasterResult raster_result;
                MenuSwfRasterStatus raster_status = menu_swf_raster_mesh(
                    &glyph_mesh.glyph_tess, &transform, &target,
                    &raster_result, reason, sizeof reason);
                if (raster_status != MENU_SWF_RASTER_OK ||
                    !raster_result.has_coverage) {
                    printf("real: glyph code=%u raster refused: %s "
                           "(bounds=%d,%d,%d,%d pen=%d,%d transform=%.6f,%.6f,%.6f,%.6f size=%dx%d tris=%u used=%u,%u,%u,%u)\n",
                           g->code, reason, g->bounds.xmin, g->bounds.xmax,
                           g->bounds.ymin, g->bounds.ymax, g->pen_x, g->pen_y,
                           transform.m00, transform.m11, transform.tx,
                           transform.ty, width, height,
                           glyph_mesh.glyph_tess.triangle_count,
                           raster_result.x0, raster_result.y0,
                           raster_result.x1, raster_result.y1);
                    free(pixels);
                    atlas_owned[slot] = NULL;
                    memset(ag, 0, sizeof *ag);
                    ag->glyph_index = (uint16_t)slot;
                    ++raster_refusals;
                    continue;
                }
                ag->pixels = pixels;
                ag->stride = (uint32_t)width;
                ag->width = (uint32_t)width;
                ag->height = (uint32_t)height;
            }
        }
    }
    printf("real: nonempty=%u layout=%u substituted=%u glyphs=%u missing=%u "
           "vertices=%u indices=%u triangles=%u\n",
           nonempty, layout_records, substituted, glyphs, missing_glyphs,
           vertices, indices, triangles);
    check(nonempty == 3 && layout_records == 3 && substituted == 3,
          "three retail labels resolve through substituted faces");
    check(glyphs == 72 && missing_glyphs == 0,
          "72 real glyph placements, none missing");
    check(vertices == 106836 && indices == 160182 && triangles == 53394,
          "resolved labels produce real Futura outlines, not password-mask glyphs");
    check(vertices > 3024 && indices > 4320,
          "real label outlines exceed the old asterisk-mask geometry");
    check(indices % 3 == 0, "resolved text index count is triangle-complete");
    check(label_mask == 7u,
          "real authored labels are the two loading strings and New Game confirmation");
    check(letter_spacing_attrs == 3 && kerning_attrs == 3 && unknown_attrs == 0,
          "real HTML letterSpacing/kerning attributes are parsed, not ignored");
    check(zero_advances == 0,
          "every non-space real menu glyph has a positive advance");
    check(raster_refusals == 0 &&
              geometry_slots + empty_slots == glyphs,
          "every real text outline rasterises; only empty glyphs use no atlas rect");

    if (batch_glyph_count) {
        static uint8_t atlas_pixels[REAL_TEXT_ATLAS_SIZE * REAL_TEXT_ATLAS_SIZE];
        static MenuSwfAtlasEntry entries[MENU_SWF_TEXT_MAX_GLYPHS];
        static MenuSwfBatchVertex batch_vertices[MENU_SWF_TEXT_MAX_GLYPHS * 4u];
        static uint32_t batch_indices[MENU_SWF_TEXT_MAX_GLYPHS * 6u];
        MenuSwfAtlasTarget atlas = {
            REAL_TEXT_ATLAS_SIZE, REAL_TEXT_ATLAS_SIZE,
            REAL_TEXT_ATLAS_SIZE, atlas_pixels};
        MenuSwfAtlasResult ar;
        MenuSwfAtlasStatus as = menu_swf_atlas_pack(
            atlas_glyphs, batch_glyph_count, &atlas, entries,
            MENU_SWF_TEXT_MAX_GLYPHS, &ar, reason, sizeof reason);
        check(as == MENU_SWF_ATLAS_OK && ar.placed_glyphs == geometry_slots &&
                  ar.empty_glyphs == empty_slots && ar.occupied_pixels > 0,
              "all real label glyph coverage packs into one A8 atlas");
        if (as == MENU_SWF_ATLAS_OK) {
            MenuSwfTextMesh batch_mesh;
            memset(&batch_mesh, 0, sizeof batch_mesh);
            batch_mesh.glyphs = batch_glyphs;
            batch_mesh.glyph_count = batch_glyph_count;
            MenuSwfBatchStyle batch_style;
            memset(&batch_style, 0, sizeof batch_style);
            batch_style.m00 = batch_style.m11 = 1.0;
            batch_style.color = 0xffffffffu;
            MenuSwfBatchResult br;
            MenuSwfBatchStatus bs = menu_swf_batch_text(
                &batch_mesh, entries, batch_glyph_count, &batch_style, NULL,
                batch_vertices, MENU_SWF_TEXT_MAX_GLYPHS * 4u, batch_indices,
                MENU_SWF_TEXT_MAX_GLYPHS * 6u, &br, reason, sizeof reason);
            check(bs == MENU_SWF_BATCH_OK && br.quad_count == geometry_slots &&
                      br.vertex_count == geometry_slots * 4u &&
                      br.index_count == geometry_slots * 6u,
                  "real label atlas entries produce a complete text quad batch");
            if (bs == MENU_SWF_BATCH_OK) {
                int indices_ok = 1, uv_ok = 1;
                for (uint32_t i = 0; i < br.index_count; ++i)
                    if (batch_indices[i] >= br.vertex_count) indices_ok = 0;
                for (uint32_t i = 0; i < batch_glyph_count; ++i)
                    if (entries[i].u + entries[i].uw > 65536u ||
                        entries[i].v + entries[i].vh > 65536u) uv_ok = 0;
                check(indices_ok && uv_ok && br.has_bounds,
                      "real text batch indices and atlas UVs stay in range");
                check(57480u + br.vertex_count <= 65536u,
                      "shape plus real text quads stay inside 16-bit indexed range");
            }
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < REAL_TEXT_ATLAS_SIZE * REAL_TEXT_ATLAS_SIZE;
                 ++i)
                if (atlas_pixels[i]) ++nonzero;
            check(nonzero > 0 && nonzero <= ar.occupied_pixels,
                  "real text atlas contains nonzero coverage within packed rects");
            printf("real: text_atlas=%ux%u placed=%u empty=%u occupied=%u "
                   "used_height=%u occupancy16=%u nonzero=%u batch_quads=%u "
                   "batch_vertices=%u batch_indices=%u\n",
                   REAL_TEXT_ATLAS_SIZE, REAL_TEXT_ATLAS_SIZE, ar.placed_glyphs,
                   ar.empty_glyphs, ar.occupied_pixels, ar.used_height,
                   ar.occupancy_16, nonzero, br.quad_count, br.vertex_count,
                   br.index_count);
        } else {
            printf("real: text atlas refused: %s (%s)\n",
                   menu_swf_atlas_status_name(as), reason);
            ++failures;
        }
    }
    for (uint32_t i = 0; i < MENU_SWF_TEXT_MAX_GLYPHS; ++i)
        free(atlas_owned[i]);

    if (layout_records) {
        const MenuSwfFontSummary *face = menu_swf_display_find_font(&font_list,
                                                                     1);
        MenuSwfTextMesh glyph_mesh;
        text_init(&glyph_mesh);
        MenuSwfTextStyle glyph_style;
        memset(&glyph_style, 0, sizeof glyph_style);
        glyph_style.m00 = glyph_style.m11 = 65536 / 20;
        glyph_style.color = 0xffffffffu;
        st = menu_swf_text_layout(fonts, font_size, face, "S", 1,
                                  &glyph_style, &glyph_mesh, reason,
                                  sizeof reason);
        check(st == MENU_SWF_OK && glyph_mesh.glyph_count == 1 &&
                  glyph_mesh.triangle_count > 0,
              "one resolved Futura glyph lays out for rasterisation");
        if (st == MENU_SWF_OK && glyph_mesh.triangle_count > 0 &&
            glyph_mesh.glyph_tess.has_bounds) {
            MenuSwfShapeRect bounds = glyph_mesh.glyph_tess.bounds;
            double w = (double)bounds.xmax - bounds.xmin;
            double h = (double)bounds.ymax - bounds.ymin;
            check(w > 0.0 && h > 0.0,
                  "resolved glyph has a positive outline box");
            if (w > 0.0 && h > 0.0) {
                double scale = (REAL_RASTER_SIZE - 2.0) / w;
                double sy = (REAL_RASTER_SIZE - 2.0) / h;
                if (sy < scale) scale = sy;
                static uint8_t raster[REAL_RASTER_SIZE * REAL_RASTER_SIZE];
                MenuSwfRasterTransform transform;
                memset(&transform, 0, sizeof transform);
                transform.m00 = transform.m11 = scale;
                transform.tx = (REAL_RASTER_SIZE - w * scale) / 2.0 -
                               bounds.xmin * scale;
                transform.ty = (REAL_RASTER_SIZE - h * scale) / 2.0 -
                               bounds.ymin * scale;
                MenuSwfRasterTarget target = {
                    REAL_RASTER_SIZE, REAL_RASTER_SIZE, REAL_RASTER_SIZE,
                    raster, 0, 0};
                MenuSwfRasterResult rr;
                MenuSwfRasterStatus rs = menu_swf_raster_mesh(
                    &glyph_mesh.glyph_tess, &transform, &target, &rr, reason,
                    sizeof reason);
                check(rs == MENU_SWF_RASTER_OK && rr.has_coverage &&
                          rr.coverage_sum > 0,
                      "resolved retail glyph rasterises with coverage");

                static uint8_t white = 255;
                static uint8_t atlas_pixels[REAL_ATLAS_SIZE * REAL_ATLAS_SIZE];
                MenuSwfAtlasGlyph glyphs_in[2];
                memset(glyphs_in, 0, sizeof glyphs_in);
                glyphs_in[0].pixels = raster + rr.y0 * REAL_RASTER_SIZE +
                                      rr.x0;
                glyphs_in[0].stride = REAL_RASTER_SIZE;
                glyphs_in[0].width = rr.x1 - rr.x0;
                glyphs_in[0].height = rr.y1 - rr.y0;
                glyphs_in[0].glyph_index = 0;
                glyphs_in[1].pixels = &white;
                glyphs_in[1].stride = 1;
                glyphs_in[1].width = glyphs_in[1].height = 1;
                glyphs_in[1].glyph_index = REAL_WHITE_GLYPH;
                MenuSwfAtlasTarget atlas = {REAL_ATLAS_SIZE, REAL_ATLAS_SIZE,
                                            REAL_ATLAS_SIZE, atlas_pixels};
                MenuSwfAtlasEntry entries[2];
                MenuSwfAtlasResult ar;
                MenuSwfAtlasStatus as = menu_swf_atlas_pack(
                    glyphs_in, 2, &atlas, entries, 2, &ar, reason,
                    sizeof reason);
                check(as == MENU_SWF_ATLAS_OK && ar.placed_glyphs == 2,
                      "resolved glyph and white texel pack into the A8 atlas");
                uint32_t nonzero = 0;
                for (uint32_t i = 0; i < REAL_ATLAS_SIZE * REAL_ATLAS_SIZE;
                     ++i)
                    if (atlas_pixels[i]) ++nonzero;
                check(nonzero > 0, "A8 atlas contains real resolved coverage");
            }
        }
    }

    free(edit_items);
    free(font_items);
done:
    free(config_text);
    free(fonts);
    free(swf);
}

int main(int argc, char **argv) {
    printf("menu font resolution tests\n");
    test_fontconfig_text();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu font resolve: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
