#pragma once
/* Read-only SWF inspection for the main menu.
 *
 * This module validates the SWF container and walks the tag stream.  It does
 * not build a display list, execute AVM/AS2, decode glyphs, decode images or
 * draw anything.  The output is a bounded summary (counts, external names,
 * first ShowFrame, tag byte totals) for the future GFx path.
 *
 * FWS is parsed in place.  CWS is decompressed to a caller-owned temporary
 * buffer with the declared size as a hard cap and an exact single-zlib-stream
 * check.  ZWS (LZMA) is a named E_NOTIMPL.
 */

#include <stddef.h>
#include <stdint.h>

#include "bsa_reader.h"

#define MENU_SWF_MAX_DECLARED (64u * 1024u * 1024u)
#define MENU_SWF_MAX_TAGS 1000000u
#define MENU_SWF_MAX_SPRITE_DEPTH 16u
#define MENU_SWF_MAX_NAMES 64u
#define MENU_SWF_NAME_MAX 96u
#define MENU_SWF_MAX_IMPORT_ASSETS 16u
#define MENU_SWF_MAX_IMPORT_SYMBOLS 64u
#define MENU_SWF_IMPORT_URL_MAX 256u
#define MENU_SWF_IMPORT_NAME_MAX 96u

typedef enum {
    MENU_SWF_FWS = 0,       /* uncompressed */
    MENU_SWF_CWS = 1,       /* zlib body */
    MENU_SWF_ZWS = 2,       /* LZMA body: named E_NOTIMPL */
    MENU_SWF_UNKNOWN = 3
} MenuSwfFormat;

typedef struct {
    MenuSwfFormat format;
    uint8_t signature[3];
    uint8_t version;
    uint32_t declared_bytes; /* SWF's own uncompressed total size */
    uint32_t file_bytes;     /* size of the SWF file in the BSA entry, before
                                any internal CWS/ZWS decompression */
} MenuSwfHeader;

typedef enum {
    MENU_SWF_OK = 0,
    MENU_SWF_ERR_ARGUMENT,
    MENU_SWF_ERR_TRUNCATED,
    MENU_SWF_ERR_FORMAT,
    MENU_SWF_ERR_VERSION,
    MENU_SWF_ERR_ZLIB,
    MENU_SWF_ERR_TAG,
    MENU_SWF_ERR_END,
    MENU_SWF_ERR_BUDGET,
    MENU_SWF_ERR_NOTIMPL
} MenuSwfStatus;

typedef enum {
    MENU_SWF_NAME_EXPORT = 0,
    MENU_SWF_NAME_IMPORT,
    MENU_SWF_NAME_IMPORT_URL,
    MENU_SWF_NAME_FONT
} MenuSwfNameKind;

typedef struct {
    uint16_t id;             /* 0 for an ImportAssets URL */
    uint8_t kind;            /* MenuSwfNameKind */
    char name[MENU_SWF_NAME_MAX];
} MenuSwfName;

typedef struct {
    uint16_t id;
    char name[MENU_SWF_IMPORT_NAME_MAX];
} MenuSwfImportSymbol;

typedef struct {
    char url[MENU_SWF_IMPORT_URL_MAX];
    MenuSwfImportSymbol symbols[MENU_SWF_MAX_IMPORT_SYMBOLS];
    uint32_t symbol_count;
    uint32_t symbol_total;
    int truncated;
} MenuSwfImportAssets;

typedef struct {
    MenuSwfImportAssets *items; /* caller-owned */
    uint32_t capacity;
    uint32_t count;
    uint32_t total;
    int truncated;
} MenuSwfImportList;

typedef struct {
    MenuSwfHeader header;

    uint32_t tag_count;      /* all tags, including tags inside DefineSprite */
    uint32_t top_level_tag_count;
    uint32_t nested_tag_count;
    uint32_t define_shape;   /* codes 2, 22, 32, 83 */
    uint32_t define_font2;   /* code 48 */
    uint32_t define_font3;   /* code 75 */
    uint32_t define_text;    /* codes 11, 33 (DefineText/DefineText2) */
    uint32_t define_edit_text; /* code 37 */
    uint32_t define_sprite;  /* code 39 (nested tags are not walked) */
    uint32_t place_object2;  /* code 26 */
    uint32_t place_object3;  /* code 70 */
    uint32_t show_frame;     /* code 1 */
    uint32_t do_action;      /* code 12 (payload is not executed) */
    uint32_t export_assets;  /* code 56 */
    uint32_t import_assets;  /* code 57 */
    uint32_t define_font_name; /* code 88 */
    uint32_t define_bits_lossless; /* codes 20, 36 */
    uint32_t define_bits_jpeg;     /* codes 6, 21, 35, 90 */

    uint64_t tag_stream_bytes; /* top-level stream, body offset 0 through End */
    uint64_t tag_payload_bytes; /* sum of top-level tag payload lengths */

    uint32_t frame_header_bytes; /* RECT + frame rate/count before the tags */
    int32_t first_show_frame;         /* tag index in encounter order, -1 */
    uint32_t first_show_frame_offset; /* tag-stream-relative byte offset */
    uint32_t trailing_bytes;          /* bytes after End (recorded, not parsed) */

    uint32_t name_count;     /* stored names, <= MENU_SWF_MAX_NAMES */
    uint32_t name_total;     /* all names encountered, including dropped */
    int names_truncated;
    MenuSwfName names[MENU_SWF_MAX_NAMES];
} MenuSwfSummary;

typedef struct {
    MenuSwfHeader header;
    const uint8_t *body;  /* SWF body after the 8-byte file header */
    size_t body_len;
    size_t tag_offset;    /* offset of the first tag inside body */
    uint8_t *owned;       /* non-NULL only for CWS */
} MenuSwfView;

const char *menu_swf_format_name(MenuSwfFormat format);
const char *menu_swf_status_name(MenuSwfStatus status);

/* Parse and validate only the 8-byte SWF header plus the CWS zlib prefix.
 * `file_bytes` is the complete SWF file size inside the BSA entry.  FWS and
 * CWS pass; ZWS and unknown signatures return BSA_ERR_UNSUPPORTED with a named
 * reason.  This helper is shared with the BSA runtime resolver. */
BsaStatus menu_swf_header_parse(const uint8_t *data, size_t available,
                                uint32_t file_bytes, MenuSwfHeader *out,
                                char *reason, size_t reason_cap);

/* Open the container and validate the frame header.  FWS points into `data`;
 * CWS owns a temporary expanded body that must be released with
 * menu_swf_view_close().  `view->tag_offset` is the first tag inside
 * `view->body`. */
MenuSwfStatus menu_swf_view_open(const uint8_t *data, size_t size,
                                 MenuSwfView *view, char *reason,
                                 size_t reason_cap);
void menu_swf_view_close(MenuSwfView *view);

/* Validate the container and walk the top-level tag stream.  CWS is expanded
 * with a hard cap of `declared_bytes - 8`; the stream must be one complete zlib
 * stream with no trailing compressed bytes.  Stops at End; a missing End,
 * truncated tag, tag length past the body or declared size over
 * MENU_SWF_MAX_DECLARED is a named refusal.  `out` is filled on success and
 * cleared on failure. */
MenuSwfStatus menu_swf_summary(const uint8_t *data, size_t size,
                               MenuSwfSummary *out, char *reason,
                               size_t reason_cap);

/* Collect structured ImportAssets/ImportAssets2 records, preserving each URL
 * with its id->symbol list.  Caller-owned storage; malformed tags are FORMAT,
 * missing/truncated fields are TRUNCATED/FORMAT and capacity/string overflow is
 * a named BUDGET refusal. */
MenuSwfStatus menu_swf_collect_imports(const uint8_t *data, size_t size,
                                       MenuSwfImportList *list,
                                       char *reason, size_t reason_cap);

/* Find the import record/symbol that declares `id`. */
const MenuSwfImportAssets *menu_swf_find_import(
    const MenuSwfImportList *list, uint16_t id,
    const MenuSwfImportSymbol **symbol);
