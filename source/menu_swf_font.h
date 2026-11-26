#pragma once
/* Face/config driven font resolution for the menu SWF path.
 *
 * Retail StartMenu.swf imports two fonts by id (34/41 in the shipped movie)
 * from gfxfontlib.swf, but the subset that ships inside
 * Interface/fonts_en.swf has different DefineFont ids.  The shipped
 * Interface/fontconfig.txt is the authoritative alias layer:
 *
 *     map "$EverywhereFont" = "Futura CondensedLight" Normal
 *     map "$EverywhereMediumFont" = "Futura Condensed" Normal
 *
 * This module parses that bounded text format, maps an imported symbol name to
 * a face/style, and selects one DefineFont2/3 summary by exact face name plus
 * bold style.  It never invents glyphs: an unresolved id/face/style pair is a
 * named E_NOTIMPL refusal.  A successful non-id match is explicitly marked as
 * a substitution so callers can log it.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf.h"
#include "menu_swf_display.h"

#define MENU_SWF_FONTCONFIG_MAX_MAPS 64u
#define MENU_SWF_FONTCONFIG_ALIAS_MAX 96u
#define MENU_SWF_FONTCONFIG_FACE_MAX 64u
#define MENU_SWF_FONTCONFIG_STYLE_MAX 16u

typedef struct {
    char alias[MENU_SWF_FONTCONFIG_ALIAS_MAX]; /* "$EverywhereFont" */
    char face[MENU_SWF_FONTCONFIG_FACE_MAX];   /* "Futura CondensedLight" */
    char style[MENU_SWF_FONTCONFIG_STYLE_MAX]; /* "Normal" / "Bold" ... */
    uint8_t bold;
    uint8_t italic;
} MenuSwfFontMap;

typedef struct {
    MenuSwfFontMap maps[MENU_SWF_FONTCONFIG_MAX_MAPS];
    uint32_t count;
    uint32_t total; /* map lines seen, including dropped */
    uint32_t fonts; /* fontlib directives seen (recorded, not required) */
    int truncated;
} MenuSwfFontConfig;

typedef enum {
    MENU_SWF_FONT_MATCH_EXACT_ID = 0, /* imported id exists in the library */
    MENU_SWF_FONT_MATCH_FACE = 1      /* id missing; fontconfig face selected */
} MenuSwfFontMatchKind;

typedef struct {
    const MenuSwfFontSummary *font;
    uint8_t kind;      /* MenuSwfFontMatchKind */
    uint8_t bold;
    uint8_t italic;
    uint16_t requested_id;
    uint16_t resolved_id;
    char import_name[MENU_SWF_IMPORT_NAME_MAX];
    char face[MENU_SWF_FONTCONFIG_FACE_MAX];
    char style[MENU_SWF_FONTCONFIG_STYLE_MAX];
} MenuSwfFontResolution;

/* Parse the shipped fontconfig text.  Blank lines and non-map directives
 * (fontlib, validNameChars) are ignored; a malformed `map` line is FORMAT.
 * More than MENU_SWF_FONTCONFIG_MAX_MAPS maps is a named BUDGET refusal and
 * sets `truncated`.  Duplicate aliases are FORMAT. */
MenuSwfStatus menu_swf_fontconfig_parse(const char *text, size_t size,
                                        MenuSwfFontConfig *out, char *reason,
                                        size_t reason_cap);

/* Case-insensitive alias/face lookup (leading/trailing spaces and a trailing
 * NUL are ignored). */
const MenuSwfFontMap *menu_swf_fontconfig_find(const MenuSwfFontConfig *config,
                                               const char *name);

/* Resolve an imported id to a real DefineFont summary.
 *
 * 1. An exact DefineFont id match in `candidates` wins and is marked
 *    MENU_SWF_FONT_MATCH_EXACT_ID.
 * 2. Otherwise the id must be declared by an ImportAssets record; its symbol
 *    name must have a fontconfig map; the mapped face and bold/italic style
 *    must select exactly one candidate.  That is MENU_SWF_FONT_MATCH_FACE.
 * 3. Missing declaration/map/face/style/ambiguous candidates are E_NOTIMPL
 *    with a diagnostic that names the failing id, alias and face. */
MenuSwfStatus menu_swf_font_resolve(const MenuSwfImportList *imports,
                                    const MenuSwfFontConfig *config,
                                    const MenuSwfFontList *candidates,
                                    uint16_t requested_id,
                                    const char *candidate_source,
                                    MenuSwfFontResolution *out, char *reason,
                                    size_t reason_cap);

/* Resolve a face name (or a fontconfig alias such as "$EverywhereFont")
 * directly.  Used for per-run HTML `face=` attributes.  `bold`/`italic` are
 * the caller's requested style and must match the selected DefineFont flags. */
MenuSwfStatus menu_swf_font_resolve_face(const MenuSwfFontConfig *config,
                                         const MenuSwfFontList *candidates,
                                         const char *face_or_alias, uint8_t bold,
                                         uint8_t italic,
                                         const char *candidate_source,
                                         MenuSwfFontResolution *out,
                                         char *reason, size_t reason_cap);
