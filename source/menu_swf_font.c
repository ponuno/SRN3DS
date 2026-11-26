#include "menu_swf_font.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void font_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static int ascii_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ci_equal(const char *a, const char *b) {
    for (;;) {
        while (*a == ' ' || *a == '\t') ++a;
        while (*b == ' ' || *b == '\t') ++b;
        int ca = ascii_lower((unsigned char)*a);
        int cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return 0;
        if (!ca) return 1;
        ++a;
        ++b;
    }
}

static int name_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    while (la && (a[la - 1] == 0 || a[la - 1] == ' ' || a[la - 1] == '\t'))
        --la;
    while (lb && (b[lb - 1] == 0 || b[lb - 1] == ' ' || b[lb - 1] == '\t'))
        --lb;
    if (la != lb) return 0;
    for (size_t i = 0; i < la; ++i)
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int copy_field(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) return -1;
    if (len) memcpy(dst, src, len);
    dst[len] = 0;
    return 0;
}

static void parse_style(const char *src, size_t len, MenuSwfFontMap *map) {
    map->bold = 0;
    map->italic = 0;
    map->style[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < len && used + 1 < sizeof map->style; ++i) {
        char ch = src[i];
        if (ch == '_') ch = ' ';
        map->style[used++] = ch;
    }
    map->style[used] = 0;
    for (size_t i = 0; i + 4 <= used; ++i) {
        if (ascii_lower((unsigned char)src[i]) == 'b' &&
            ascii_lower((unsigned char)src[i + 1]) == 'o' &&
            ascii_lower((unsigned char)src[i + 2]) == 'l' &&
            ascii_lower((unsigned char)src[i + 3]) == 'd')
            map->bold = 1;
        if (i + 6 <= used &&
            ascii_lower((unsigned char)src[i]) == 'i' &&
            ascii_lower((unsigned char)src[i + 1]) == 't' &&
            ascii_lower((unsigned char)src[i + 2]) == 'a' &&
            ascii_lower((unsigned char)src[i + 3]) == 'l' &&
            ascii_lower((unsigned char)src[i + 4]) == 'i' &&
            ascii_lower((unsigned char)src[i + 5]) == 'c')
            map->italic = 1;
    }
}

/* Parse `map "$alias" = "face" Style` from one trimmed line. */
static int parse_map_line(const char *line, size_t len, MenuSwfFontMap *map,
                          char *reason, size_t reason_cap) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 3 > len || ascii_lower((unsigned char)line[i]) != 'm' ||
        ascii_lower((unsigned char)line[i + 1]) != 'a' ||
        ascii_lower((unsigned char)line[i + 2]) != 'p')
        return 1; /* not a map line */
    i += 3;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= len || line[i] != '"') {
        font_reason(reason, reason_cap, "fontconfig: map alias is not quoted");
        return -1;
    }
    size_t alias_start = ++i;
    while (i < len && line[i] != '"') ++i;
    if (i >= len) {
        font_reason(reason, reason_cap, "fontconfig: unterminated map alias");
        return -1;
    }
    size_t alias_len = i - alias_start;
    ++i;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= len || line[i] != '=') {
        font_reason(reason, reason_cap, "fontconfig: map alias is missing '='");
        return -1;
    }
    ++i;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= len || line[i] != '"') {
        font_reason(reason, reason_cap, "fontconfig: map face is not quoted");
        return -1;
    }
    size_t face_start = ++i;
    while (i < len && line[i] != '"') ++i;
    if (i >= len) {
        font_reason(reason, reason_cap, "fontconfig: unterminated map face");
        return -1;
    }
    size_t face_len = i - face_start;
    ++i;
    if (copy_field(map->alias, sizeof map->alias, line + alias_start,
                   alias_len) ||
        copy_field(map->face, sizeof map->face, line + face_start, face_len)) {
        font_reason(reason, reason_cap,
                    "fontconfig: alias/face exceeds %u/%u bytes",
                    (unsigned)MENU_SWF_FONTCONFIG_ALIAS_MAX - 1,
                    (unsigned)MENU_SWF_FONTCONFIG_FACE_MAX - 1);
        return -1;
    }
    parse_style(line + i, len - i, map);
    return 0;
}

MenuSwfStatus menu_swf_fontconfig_parse(const char *text, size_t size,
                                        MenuSwfFontConfig *out, char *reason,
                                        size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out || (!text && size)) return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    size_t line_start = 0;
    while (line_start < size) {
        size_t line_end = line_start;
        while (line_end < size && text[line_end] != '\n' &&
               text[line_end] != '\r')
            ++line_end;
        size_t len = line_end - line_start;
        while (len && (text[line_start + len - 1] == ' ' ||
                       text[line_start + len - 1] == '\t'))
            --len;
        size_t first = 0;
        while (first < len && (text[line_start + first] == ' ' ||
                               text[line_start + first] == '\t'))
            ++first;
        const char *line = text + line_start + first;
        size_t trimmed = len - first;
        if (trimmed >= 6 && !strncmp(line, "fontlib", 6)) {
            ++out->fonts;
        } else if (trimmed >= 3 && ascii_lower((unsigned char)line[0]) == 'm' &&
                   ascii_lower((unsigned char)line[1]) == 'a' &&
                   ascii_lower((unsigned char)line[2]) == 'p') {
            MenuSwfFontMap map;
            memset(&map, 0, sizeof map);
            int rc = parse_map_line(line, trimmed, &map, reason, reason_cap);
            if (rc < 0) {
                memset(out, 0, sizeof *out);
                return MENU_SWF_ERR_FORMAT;
            }
            if (rc == 0) {
                ++out->total;
                if (out->count >= MENU_SWF_FONTCONFIG_MAX_MAPS) {
                    out->truncated = 1;
                    font_reason(reason, reason_cap,
                                "fontconfig: %u maps exceed %u", out->total,
                                MENU_SWF_FONTCONFIG_MAX_MAPS);
                    memset(out, 0, sizeof *out);
                    return MENU_SWF_ERR_BUDGET;
                }
                for (uint32_t i = 0; i < out->count; ++i) {
                    if (name_equal(out->maps[i].alias, map.alias)) {
                        font_reason(reason, reason_cap,
                                    "fontconfig: duplicate alias '%s'",
                                    map.alias);
                        memset(out, 0, sizeof *out);
                        return MENU_SWF_ERR_FORMAT;
                    }
                }
                out->maps[out->count++] = map;
            }
        }
        line_start = line_end;
        while (line_start < size &&
               (text[line_start] == '\r' || text[line_start] == '\n'))
            ++line_start;
    }
    return MENU_SWF_OK;
}

const MenuSwfFontMap *menu_swf_fontconfig_find(const MenuSwfFontConfig *config,
                                               const char *name) {
    if (!config || !name) return NULL;
    for (uint32_t i = 0; i < config->count; ++i)
        if (name_equal(config->maps[i].alias, name)) return &config->maps[i];
    return NULL;
}

typedef struct {
    const MenuSwfFontSummary *font;
    uint8_t bold;
    uint8_t italic;
} FontStyle;

static int font_style(const MenuSwfFontSummary *font, FontStyle *out) {
    out->font = font;
    /* DefineFont2/3 FontFlagsBold = 0x01, FontFlagsItalic = 0x02. */
    out->bold = (font->flags & 0x01u) != 0;
    out->italic = (font->flags & 0x02u) != 0;
    return 0;
}

static MenuSwfStatus select_face(const MenuSwfFontConfig *config,
                                 const MenuSwfFontList *candidates,
                                 const char *face_or_alias, uint8_t bold,
                                 uint8_t italic, const char *candidate_source,
                                 MenuSwfFontResolution *out, char *reason,
                                 size_t reason_cap) {
    const MenuSwfFontMap *map = menu_swf_fontconfig_find(config, face_or_alias);
    const char *face = map ? map->face : face_or_alias;
    if (map) {
        bold = map->bold;
        italic = map->italic;
    }
    if (!face || !face[0]) {
        font_reason(reason, reason_cap, "fontconfig: empty face for '%s'",
                    face_or_alias);
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (!candidates || !candidates->items) {
        font_reason(reason, reason_cap,
                    "swf fonts: no candidate library for face '%s'", face);
        return MENU_SWF_ERR_ARGUMENT;
    }
    const MenuSwfFontSummary *matches[MENU_SWF_DISPLAY_MAX_FONTS > 1
                                          ? MENU_SWF_DISPLAY_MAX_FONTS
                                          : 2];
    uint32_t match_count = 0;
    uint32_t count = candidates->count < candidates->capacity
                         ? candidates->count
                         : candidates->capacity;
    for (uint32_t i = 0; i < count; ++i) {
        const MenuSwfFontSummary *font = &candidates->items[i];
        if (!name_equal(font->name, face)) continue;
        FontStyle style;
        font_style(font, &style);
        if (style.bold != bold || style.italic != italic) continue;
        if (match_count < sizeof matches / sizeof matches[0])
            matches[match_count] = font;
        ++match_count;
    }
    if (match_count == 0) {
        font_reason(reason, reason_cap,
                    "swf fonts: face '%s' %s%s is not present in %s", face,
                    bold ? "Bold" : "Normal", italic ? " Italic" : "",
                    candidate_source ? candidate_source : "candidate font set");
        return MENU_SWF_ERR_NOTIMPL;
    }
    if (match_count > 1) {
        font_reason(reason, reason_cap,
                    "swf fonts: face '%s' %s%s is ambiguous (%u candidates in "
                    "%s)",
                    face, bold ? "Bold" : "Normal", italic ? " Italic" : "",
                    match_count,
                    candidate_source ? candidate_source : "candidate font set");
        return MENU_SWF_ERR_NOTIMPL;
    }
    memset(out, 0, sizeof *out);
    out->font = matches[0];
    out->kind = MENU_SWF_FONT_MATCH_FACE;
    out->bold = bold;
    out->italic = italic;
    out->resolved_id = matches[0]->font_id;
    copy_field(out->face, sizeof out->face, face, strlen(face));
    if (map) copy_field(out->import_name, sizeof out->import_name,
                        map->alias, strlen(map->alias));
    copy_field(out->style, sizeof out->style, map ? map->style : "Normal",
               strlen(map ? map->style : "Normal"));
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_font_resolve_face(const MenuSwfFontConfig *config,
                                         const MenuSwfFontList *candidates,
                                         const char *face_or_alias, uint8_t bold,
                                         uint8_t italic,
                                         const char *candidate_source,
                                         MenuSwfFontResolution *out,
                                         char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!config || !candidates || !face_or_alias || !out)
        return MENU_SWF_ERR_ARGUMENT;
    return select_face(config, candidates, face_or_alias, bold, italic,
                       candidate_source, out, reason, reason_cap);
}

MenuSwfStatus menu_swf_font_resolve(const MenuSwfImportList *imports,
                                    const MenuSwfFontConfig *config,
                                    const MenuSwfFontList *candidates,
                                    uint16_t requested_id,
                                    const char *candidate_source,
                                    MenuSwfFontResolution *out, char *reason,
                                    size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!imports || !config || !candidates || !out)
        return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);

    const MenuSwfFontSummary *exact = menu_swf_display_find_font(candidates,
                                                                 requested_id);
    if (exact) {
        out->font = exact;
        out->kind = MENU_SWF_FONT_MATCH_EXACT_ID;
        out->requested_id = requested_id;
        out->resolved_id = exact->font_id;
        copy_field(out->face, sizeof out->face, exact->name,
                   strlen(exact->name));
        copy_field(out->style, sizeof out->style, "Normal", 6);
        return MENU_SWF_OK;
    }

    const MenuSwfImportSymbol *symbol = NULL;
    const MenuSwfImportAssets *asset = menu_swf_find_import(imports, requested_id,
                                                            &symbol);
    if (!asset || !symbol) {
        font_reason(reason, reason_cap,
                    "swf fonts: imported font id %u is not declared by this "
                    "movie and is not present in %s",
                    requested_id,
                    candidate_source ? candidate_source : "candidate font set");
        return MENU_SWF_ERR_NOTIMPL;
    }
    const MenuSwfFontMap *map = menu_swf_fontconfig_find(config, symbol->name);
    if (!map) {
        font_reason(reason, reason_cap,
                    "swf fonts: imported font id %u (%s, URL %s) has no "
                    "fontconfig map in the shipped Interface/fontconfig.txt",
                    requested_id, symbol->name, asset->url);
        return MENU_SWF_ERR_NOTIMPL;
    }
    MenuSwfStatus st = select_face(config, candidates, symbol->name, map->bold,
                                   map->italic, candidate_source, out, reason,
                                   reason_cap);
    if (st != MENU_SWF_OK) return st;
    out->kind = MENU_SWF_FONT_MATCH_FACE;
    out->requested_id = requested_id;
    out->resolved_id = out->font->font_id;
    copy_field(out->import_name, sizeof out->import_name, symbol->name,
               strlen(symbol->name));
    return MENU_SWF_OK;
}
