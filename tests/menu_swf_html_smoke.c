/* Host test: GFx HTML runs, run layout and per-run text batch colours. */

#include "menu_swf_text.h"
#include "menu_swf_display.h"
#include "menu_swf_batch.h"
#include "menu_swf_shape.h"
#include "menu_swf_tess.h"
#include "menu_swf.h"
#include "bsa_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_s16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct { uint8_t *p; size_t cap, bit; int ok; } BitW;

static void bw_put(BitW *w, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        size_t bit = w->bit + i;
        if ((bit >> 3) >= w->cap) { w->ok = 0; return; }
        unsigned value = (v >> (n - 1 - i)) & 1u;
        uint8_t mask = (uint8_t)(0x80u >> (bit & 7));
        if (value) w->p[bit >> 3] |= mask;
        else w->p[bit >> 3] &= (uint8_t)~mask;
    }
    w->bit += n;
}

static void bw_signed(BitW *w, int32_t v, unsigned n) {
    uint32_t mask = n == 32 ? 0xffffffffu : ((1u << n) - 1u);
    bw_put(w, (uint32_t)v & mask, n);
}

static void bw_align(BitW *w) { while (w->bit & 7u) bw_put(w, 0, 1); }

static size_t rect_bytes(uint8_t *out, size_t cap, int32_t xmin, int32_t xmax,
                         int32_t ymin, int32_t ymax) {
    memset(out, 0, cap < 32 ? cap : 32);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 12, 5);
    bw_signed(&w, xmin, 12);
    bw_signed(&w, xmax, 12);
    bw_signed(&w, ymin, 12);
    bw_signed(&w, ymax, 12);
    bw_align(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static void rec_move(BitW *w, int32_t x, int32_t y) {
    bw_put(w, 0, 1); bw_put(w, 0, 1); bw_put(w, 0, 1); bw_put(w, 0, 1);
    bw_put(w, 1, 1); bw_put(w, 1, 1);
    bw_put(w, 10, 5); bw_signed(w, x, 10); bw_signed(w, y, 10);
    bw_put(w, 1, 1);
}

static void rec_line(BitW *w, int32_t dx, int32_t dy) {
    bw_put(w, 1, 1); bw_put(w, 1, 1); bw_put(w, 10, 4); bw_put(w, 1, 1);
    bw_signed(w, dx, 12); bw_signed(w, dy, 12);
}

static void rec_end(BitW *w) { bw_put(w, 0, 6); bw_align(w); }

static size_t glyph_square(uint8_t *out, size_t cap) {
    memset(out, 0, cap);
    BitW w = {out, cap, 0, 1};
    bw_put(&w, 1, 4); bw_put(&w, 0, 4);
    rec_move(&w, 0, 0);
    rec_line(&w, 100, 0); rec_line(&w, 0, 100);
    rec_line(&w, -100, 0); rec_line(&w, 0, -100);
    rec_end(&w);
    return w.ok ? (w.bit + 7) / 8 : 0;
}

static size_t build_font_tag(uint8_t *out, size_t cap) {
    uint8_t g[256], rb[32];
    size_t gn = glyph_square(g, sizeof g);
    memset(out, 0, cap);
    size_t at = 0;
    put_u16(out + at, 1); at += 2;
    out[at++] = 0x85; out[at++] = 0;
    const char *name = "HtmlFont";
    out[at++] = 8; memcpy(out + at, name, 8); at += 8;
    put_u16(out + at, 3); at += 2;
    size_t off0 = 8, off1 = off0 + gn, off2 = off1 + gn;
    size_t code_off = off2 + gn;
    put_u16(out + at, (uint16_t)off0); at += 2;
    put_u16(out + at, (uint16_t)off1); at += 2;
    put_u16(out + at, (uint16_t)off2); at += 2;
    put_u16(out + at, (uint16_t)code_off); at += 2;
    for (unsigned i = 0; i < 3; ++i) { memcpy(out + at, g, gn); at += gn; }
    put_u16(out + at, 'A'); at += 2;
    put_u16(out + at, 'V'); at += 2;
    put_u16(out + at, ' '); at += 2;
    put_u16(out + at, 800); at += 2;
    put_u16(out + at, 200); at += 2;
    put_s16(out + at, 0); at += 2;
    put_s16(out + at, 600); at += 2;
    put_s16(out + at, 600); at += 2;
    put_s16(out + at, 300); at += 2;
    for (unsigned i = 0; i < 3; ++i) {
        size_t rn = rect_bytes(rb, sizeof rb, 0, 100, 0, 100);
        memcpy(out + at, rb, rn); at += rn;
    }
    put_u16(out + at, 1); at += 2;
    put_u16(out + at, 'A'); at += 2;
    put_u16(out + at, 'V'); at += 2;
    put_s16(out + at, -100); at += 2;
    return at;
}

static size_t build_edit_payload(uint8_t *out, size_t cap, uint16_t id,
                                 uint16_t flags, uint16_t font_id,
                                 uint16_t font_height, uint8_t align,
                                 const char *variable) {
    uint8_t rect[32];
    size_t rn = rect_bytes(rect, sizeof rect, -2000, 2000, -200, 800);
    size_t at = 0;
    put_u16(out + at, id); at += 2;
    memcpy(out + at, rect, rn); at += rn;
    put_u16(out + at, flags); at += 2;
    if (flags & 0x0080u) {
        put_u16(out + at, font_id); at += 2;
        put_u16(out + at, font_height); at += 2;
    }
    if (flags & 0x0020u) {
        out[at++] = 0x11; out[at++] = 0x22; out[at++] = 0x33; out[at++] = 0x44;
    }
    if (flags & 0x0100u) {
        out[at++] = align;
        put_u16(out + at, 0); at += 2;
        put_u16(out + at, 0); at += 2;
        put_s16(out + at, 0); at += 2;
        put_s16(out + at, 0); at += 2;
    }
    size_t vn = strlen(variable) + 1;
    memcpy(out + at, variable, vn); at += vn;
    (void)cap;
    return at;
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) { put_u32(out + at, len); at += 4; }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t make_edit_swf(uint8_t *out, size_t cap, const uint8_t *font,
                            uint32_t font_len, const uint8_t *edit,
                            uint32_t edit_len) {
    uint8_t body[8192];
    size_t at = 0;
    body[at++] = 0; put_u16(body + at, 0x1e00); at += 2;
    put_u16(body + at, 1); at += 2;
    at = put_tag(body, sizeof body, at, 75, font, font_len);
    at = put_tag(body, sizeof body, at, 37, edit, edit_len);
    at = put_tag(body, sizeof body, at, 0, NULL, 0);
    if (!at || cap < 8 + at) return 0;
    memcpy(out, "FWS", 3); out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + at));
    memcpy(out + 8, body, at);
    return 8 + at;
}

typedef struct {
    MenuSwfTextVertex vertices[8192];
    uint32_t indices[24576];
    MenuSwfTextGlyph glyphs[32];
    MenuSwfTextLine lines[32];
    MenuSwfTextRun runs[MENU_SWF_TEXT_MAX_RUNS];
    MenuSwfTessVertex tvert[8192];
    uint32_t tindices[24576];
    uint16_t tfills[8192];
    MenuSwfTessLine tlines[2048];
    MenuSwfTessEdge tedges[4096];
    uint32_t tactive[4096];
    int32_t tbands[8192];
} TextFixture;

static void fixture_init(TextFixture *f, MenuSwfTextMesh *m) {
    memset(m, 0, sizeof *m);
    m->vertices = f->vertices; m->vertex_capacity = 8192;
    m->indices = f->indices; m->index_capacity = 24576;
    m->glyphs = f->glyphs; m->glyph_capacity = 32;
    m->lines = f->lines; m->line_capacity = 32;
    m->runs = f->runs; m->run_capacity = MENU_SWF_TEXT_MAX_RUNS;
    m->glyph_tess.vertices = f->tvert; m->glyph_tess.vertex_capacity = 8192;
    m->glyph_tess.indices = f->tindices; m->glyph_tess.index_capacity = 24576;
    m->glyph_tess.triangle_fills = f->tfills;
    m->glyph_tess.triangle_capacity = 8192;
    m->glyph_tess.lines = f->tlines; m->glyph_tess.line_capacity = 2048;
    m->glyph_tess.edges = f->tedges; m->glyph_tess.edge_capacity = 4096;
    m->glyph_tess.active = f->tactive; m->glyph_tess.active_capacity = 4096;
    m->glyph_tess.bands = f->tbands; m->glyph_tess.band_capacity = 8192;
}

static void make_edit(MenuSwfEditText *e, const char *html) {
    memset(e, 0, sizeof *e);
    e->html = 1;
    e->multiline = 1;
    e->has_text_color = 1;
    e->color[0] = 0x11; e->color[1] = 0x22;
    e->color[2] = 0x33; e->color[3] = 0xff;
    e->alignment = MENU_SWF_EDIT_TEXT_ALIGN_LEFT;
    e->font_height = 240;
    e->bounds[0] = -2000; e->bounds[1] = 2000;
    e->bounds[2] = -200; e->bounds[3] = 800;
    snprintf(e->variable_name, sizeof e->variable_name, "%s", html);
}

static void test_runs_parse(void) {
    const char *html =
        "<p align=\"center\"><font color=\"#ff0000\" size=\"14\" "
        "face=\"Test\"><b>A</b><i>V</i></font><u>U</u><br>B &amp; C "
        "&lt; D &gt; E &quot; F &apos; G&nbsp;H</p><unknown>Z</unknown>";
    MenuSwfEditText edit;
    make_edit(&edit, html);
    char plain[512], reason[BSA_REASON_MAX];
    size_t plain_len = 0;
    MenuSwfTextRun runs[MENU_SWF_TEXT_MAX_RUNS];
    uint32_t run_count = 0;
    MenuSwfTextHtmlSummary summary;
    MenuSwfStatus st = menu_swf_text_edit_runs(
        &edit, plain, sizeof plain, &plain_len, runs, MENU_SWF_TEXT_MAX_RUNS,
        &run_count, &summary, reason, sizeof reason);
    check(st == MENU_SWF_OK, "HTML runs parse");
    if (st != MENU_SWF_OK) {
        printf("  reason: %s\n", reason);
        return;
    }
    const char *want = "AVU\nB & C < D > E \" F ' G H\nZ";
    check(plain_len == strlen(want) && !memcmp(plain, want, plain_len),
          "HTML entities/unknown text preserved");
    check(run_count == 7, "HTML run count");
    check(summary.tags == 13 && summary.start_tags == 7 &&
              summary.end_tags == 6 && summary.unknown_tags == 2,
          "HTML tag census");
    check(summary.font_tags == 2 && summary.b_tags == 2 &&
              summary.i_tags == 2 && summary.u_tags == 2 &&
              summary.br_tags == 1 && summary.p_tags == 2,
          "HTML tag kinds");
    check(summary.entities == 6 && summary.color_attrs == 1 &&
              summary.size_attrs == 1 && summary.face_attrs == 1 &&
              summary.align_attrs == 1,
          "HTML entity/attribute census");
    if (run_count == 7) {
        check(runs[0].text_first == 0 && runs[0].text_length == 1 &&
                  runs[0].color == 0xffff0000u && runs[0].has_color &&
                  runs[0].size == 14 && runs[0].has_size &&
                  runs[0].has_face && !strcmp(runs[0].face, "Test") &&
                  runs[0].bold && !runs[0].italic && !runs[0].underline &&
                  runs[0].align == MENU_SWF_EDIT_TEXT_ALIGN_CENTER,
              "HTML run 0 colour/size/face/bold");
        check(runs[1].text_first == 1 && runs[1].text_length == 1 &&
                  runs[1].italic && !runs[1].bold,
              "HTML run 1 italic");
        check(runs[2].text_first == 2 && runs[2].text_length == 1 &&
                  runs[2].underline && runs[2].has_color &&
                  runs[2].color == 0xff112233u,
              "HTML run 2 underline/default colour");
        check(runs[3].text_first == 3 && runs[3].text_length == 1 &&
                  plain[3] == '\n' && !runs[3].bold &&
              runs[4].text_first == 4 &&
                  runs[4].text_length ==
                      runs[5].text_first - runs[4].text_first &&
                  runs[4].align == MENU_SWF_EDIT_TEXT_ALIGN_CENTER &&
              runs[5].text_length == 1 &&
                  plain[runs[5].text_first] == '\n' &&
              runs[6].text_first + runs[6].text_length == plain_len &&
                  plain[runs[6].text_first] == 'Z',
              "HTML newline and default runs");
    }

    make_edit(&edit, "<font color=\"#00ff00\"><b>X</b>Y</font>");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 MENU_SWF_TEXT_MAX_RUNS, &run_count, &summary,
                                 reason, sizeof reason);
    check(st == MENU_SWF_OK && run_count == 2 && plain_len == 2 &&
              runs[0].bold && runs[0].color == 0xff00ff00u &&
              !runs[1].bold && runs[1].color == 0xff00ff00u,
          "HTML nested style restore");
}

static void test_refusals(void) {
    MenuSwfEditText edit;
    char plain[64], reason[BSA_REASON_MAX];
    size_t plain_len = 0;
    MenuSwfTextRun runs[4];
    uint32_t run_count = 0;
    MenuSwfTextHtmlSummary summary;

    make_edit(&edit, "<b");
    MenuSwfStatus st = menu_swf_text_edit_runs(
        &edit, plain, sizeof plain, &plain_len, runs, 4, &run_count, &summary,
        reason, sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "unterminated HTML tag"),
          "unterminated tag refused");

    make_edit(&edit, "<b>X</b>&bogus;");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 4, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_NOTIMPL && strstr(reason, "entity"),
          "unknown entity refused");

    make_edit(&edit, "<font color=\"red\">X</font>");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 4, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "colour"),
          "bad colour refused");

    make_edit(&edit, "<font size=\"0\">X</font>");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 4, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "size"),
          "bad size refused");

    make_edit(&edit, "<font face=\"012345678901234567890123456789012345\">X</font>");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 4, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_FORMAT && strstr(reason, "face"),
          "long face refused");

    make_edit(&edit, "<b>X</b><i>Y</i>");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 1, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && strstr(reason, "runs"),
          "run capacity refused");

    make_edit(&edit, "<b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b>X");
    st = menu_swf_text_edit_runs(&edit, plain, sizeof plain, &plain_len, runs,
                                 4, &run_count, &summary, reason,
                                 sizeof reason);
    check(st == MENU_SWF_ERR_BUDGET && strstr(reason, "nesting"),
          "nesting limit refused");
}

static void test_layout_and_batch(void) {
    uint8_t font[2048], edit_tag[2048], file[8192];
    size_t font_len = build_font_tag(font, sizeof font);
    uint16_t flags = 0x0004u | 0x0020u | 0x0080u | 0x0100u | 0x1000u;
    size_t edit_len = build_edit_payload(
        edit_tag, sizeof edit_tag, 9, flags, 1, 240,
        MENU_SWF_EDIT_TEXT_ALIGN_LEFT,
        "<font color=\"#ff0000\" size=\"24\">AA</font>"
        "<font color=\"#0000ff\">V</font>");
    size_t file_len = make_edit_swf(file, sizeof file, font,
                                    (uint32_t)font_len, edit_tag,
                                    (uint32_t)edit_len);
    check(font_len && edit_len && file_len, "HTML layout fixture builds");
    if (!file_len) return;

    MenuSwfDisplaySummary *display = malloc(sizeof *display);
    check(display != NULL, "HTML display allocation");
    if (!display) return;
    char reason[BSA_REASON_MAX];
    MenuSwfStatus st = menu_swf_display_summary(file, file_len, display, reason,
                                                sizeof reason);
    check(st == MENU_SWF_OK && display->stored_fonts == 1,
          "HTML font summary");
    if (st != MENU_SWF_OK) { free(display); return; }

    MenuSwfEditText items[2];
    MenuSwfEditTextList list = {items, 2, 0, 0, 0};
    st = menu_swf_display_collect_edit_texts(file, file_len, &list, reason,
                                             sizeof reason);
    check(st == MENU_SWF_OK && list.count == 1, "HTML edit collection");
    if (st != MENU_SWF_OK || list.count != 1) { free(display); return; }

    static TextFixture fixture;
    MenuSwfTextMesh mesh;
    MenuSwfTextStyle style;
    memset(&style, 0, sizeof style);
    style.m00 = style.m11 = 8192;
    style.color = 0xffffffffu;
    fixture_init(&fixture, &mesh);
    st = menu_swf_text_layout_edit(file, file_len, &display->fonts[0],
                                   &items[0], &style, &mesh, reason,
                                   sizeof reason);
    check(st == MENU_SWF_OK, "HTML run layout");
    if (st != MENU_SWF_OK) {
        printf("  reason: %s\n", reason);
        free(display);
        return;
    }
    check(mesh.glyph_count == 3 && mesh.run_count == 2 &&
              mesh.line_count == 1,
          "HTML layout runs/glyphs");
    check(mesh.runs[0].glyph_first == 0 && mesh.runs[0].glyph_count == 2 &&
              mesh.runs[1].glyph_first == 2 && mesh.runs[1].glyph_count == 1,
          "HTML run glyph ranges");
    check(mesh.glyphs[0].has_color && mesh.glyphs[0].color == 0xffff0000u &&
              mesh.glyphs[1].color == 0xffff0000u &&
              mesh.glyphs[2].has_color && mesh.glyphs[2].color == 0xff0000ffu,
          "HTML glyph run colours");
    check(mesh.glyphs[1].x - mesh.glyphs[0].x == 150,
          "HTML size 24 doubles the 12 px advance");
    unsigned reds = 0, blues = 0;
    for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
        if (mesh.vertices[i].color == 0xffff0000u) ++reds;
        if (mesh.vertices[i].color == 0xff0000ffu) ++blues;
    }
    check(reds > 0 && blues > 0 && reds + blues == mesh.vertex_count,
          "HTML layout vertices carry run colours");

    MenuSwfAtlasEntry atlas[3];
    for (unsigned i = 0; i < 3; ++i) {
        memset(&atlas[i], 0, sizeof atlas[i]);
        atlas[i].has_rect = 1; atlas[i].w = 10; atlas[i].h = 10;
        atlas[i].u = i * 0x10000u; atlas[i].uw = 0x10000u;
        atlas[i].vh = 0x10000u;
    }
    MenuSwfBatchStyle bstyle;
    memset(&bstyle, 0, sizeof bstyle);
    bstyle.m00 = bstyle.m11 = 1.0;
    bstyle.color = 0xffffffffu;
    MenuSwfBatchVertex vertices[12];
    uint32_t indices[18];
    MenuSwfBatchResult br;
    MenuSwfBatchStatus bs = menu_swf_batch_text(
        &mesh, atlas, 3, &bstyle, NULL, vertices, 12, indices, 18, &br,
        reason, sizeof reason);
    check(bs == MENU_SWF_BATCH_OK && br.quad_count == 3,
          "HTML per-run text batch");
    if (bs == MENU_SWF_BATCH_OK)
        check(vertices[0].color == 0xffff0000u &&
                  vertices[7].color == 0xffff0000u &&
                  vertices[8].color == 0xff0000ffu,
              "HTML batch keeps per-run colours");

    bstyle.has_color_transform = 1;
    bstyle.color_mult[0] = 128;
    bstyle.color_mult[1] = bstyle.color_mult[3] = 256;
    bstyle.color_mult[2] = 128;
    bs = menu_swf_batch_text(&mesh, atlas, 3, &bstyle, NULL, vertices, 12,
                             indices, 18, &br, reason, sizeof reason);
    check(bs == MENU_SWF_BATCH_OK && vertices[0].color == 0xff7f0000u &&
              vertices[8].color == 0xff00007fu,
          "HTML batch applies record CXFORM to run colours");
    free(display);
}

static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    BsaArchive *archive = NULL;
    BsaMatch match;
    for (unsigned i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof path, "%s/%s", data_dir, archives[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (bsa_open_path(path, NULL, &archive, NULL, 0) == BSA_OK) {
            if (bsa_find(archive, "interface/startmenu.swf", &match) == BSA_OK)
                break;
            bsa_close(archive); archive = NULL;
        }
    }
    if (!archive) {
        printf("real: skip (StartMenu.swf not found under %s)\n", data_dir);
        return;
    }
    uint32_t size = 0;
    char reason[BSA_REASON_MAX];
    check(bsa_unpacked_size(archive, &match.entry, &size, reason,
                            sizeof reason) == BSA_OK,
          "real StartMenu size");
    uint8_t *bytes = (uint8_t *)malloc(size ? size : 1);
    size_t got = 0;
    if (bytes)
        check(bsa_read(archive, &match.entry, bytes, size, &got, reason,
                       sizeof reason) == BSA_OK && got == size,
              "real StartMenu read");
    bsa_close(archive);
    if (!bytes) return;

    MenuSwfEditText items[64];
    MenuSwfEditTextList list = {items, 64, 0, 0, 0};
    MenuSwfStatus st = menu_swf_display_collect_edit_texts(
        bytes, got, &list, reason, sizeof reason);
    check(st == MENU_SWF_OK, "real edit-text collection");
    if (st != MENU_SWF_OK) { free(bytes); return; }

    unsigned html_defs = 0, payloads = 0, nonempty = 0;
    unsigned runs = 0, tags = 0, unknown = 0, colors = 0, sizes = 0;
    unsigned faces = 0, entities = 0;
    for (uint32_t i = 0; i < list.count; ++i) {
        const MenuSwfEditText *edit = &list.items[i];
        if (!edit->html) continue;
        ++html_defs;
        char text[MENU_SWF_TEXT_MAX_CHARS + 1];
        size_t text_len = 0;
        MenuSwfTextRun r[MENU_SWF_TEXT_MAX_RUNS];
        uint32_t run_count = 0;
        MenuSwfTextHtmlSummary summary;
        st = menu_swf_text_edit_runs(
            edit, text, sizeof text, &text_len, r, MENU_SWF_TEXT_MAX_RUNS,
            &run_count, &summary, reason, sizeof reason);
        check(st == MENU_SWF_OK, "real HTML runs parse");
        if (st != MENU_SWF_OK) {
            printf("real: html id=%u refused: %s\n", edit->character_id,
                   reason);
            continue;
        }
        if (text_len || summary.tags) ++payloads;
        if (text_len) ++nonempty;
        runs += run_count; tags += summary.tags;
        unknown += summary.unknown_tags;
        colors += summary.color_attrs; sizes += summary.size_attrs;
        faces += summary.face_attrs; entities += summary.entities;
        if (text_len)
            printf("real: html id=%u len=%zu runs=%u tags=%u text=\"%s\"\n",
                   edit->character_id, text_len, run_count, summary.tags,
                   text);
    }
    printf("real: html_defs=%u payloads=%u nonempty=%u runs=%u tags=%u "
           "unknown=%u colors=%u sizes=%u faces=%u entities=%u\n",
           html_defs, payloads, nonempty, runs, tags, unknown, colors, sizes,
           faces, entities);
    check(html_defs == 22 && payloads == 4 && nonempty == 3,
          "real StartMenu HTML payload counts");
    check(runs == 3 && tags == 14 && unknown == 0 && colors == 3 &&
              sizes == 3 && faces == 3 && entities == 0,
          "real StartMenu HTML run/tag census");
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF HTML tests\n");
    test_runs_parse();
    test_refusals();
    test_layout_and_batch();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF HTML: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
