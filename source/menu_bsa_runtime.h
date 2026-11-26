#pragma once
/* Runtime path from the shipped BSA archives to Interface/StartMenu.swf.
 *
 * This is an asset-loading path only: it validates the SWF container header and
 * returns the stored bytes in guest memory.  It does not decompress the GFx
 * body, build a movie, create fonts or draw a frame.  A caller that later
 * implements the GFx draw path owns the CWS/ZWS body decompression.
 *
 * Archive precedence is the measured TESV order: the base resource archive is
 * mounted first and Update.bsa is mounted last by a separate loader call, so a
 * same-path file in Update.bsa overrides the base archive.  For the main menu
 * the two relevant archives are:
 *
 *   Skyrim - Interface.bsa   (base)
 *   Update.bsa               (last, override)
 *
 * The resolver searches every source, remembers the last archive containing
 * the path, then reads only that winner.
 */

#include <stddef.h>
#include <stdint.h>

#include "bsa_reader.h"
#include "guest_heap.h"

#define MENU_BSA_PATH_MAX 512u
#define MENU_BSA_REASON_MAX 224u
#define MENU_BSA_ARCHIVE_NAME_MAX 64u
#define MENU_BSA_DATA_PATH_MAX 1024u
#define MENU_BSA_MAX_SOURCES 8u
#define MENU_BSA_ARCHIVE_COUNT 2u

typedef enum {
    MENU_BSA_OK = 0,
    MENU_BSA_MISSING,
    MENU_BSA_ACCESS,
    MENU_BSA_CORRUPT,
    MENU_BSA_UNSUPPORTED,
    MENU_BSA_BUDGET,
    MENU_BSA_GUEST_MEMORY,
    MENU_BSA_BAD_ARGUMENT
} MenuBsaStatus;

typedef enum {
    MENU_SWF_FWS = 0,       /* uncompressed */
    MENU_SWF_CWS = 1,       /* zlib body, header readable without decompression */
    MENU_SWF_ZWS = 2,       /* LZMA body: named refusal */
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

typedef struct {
    const char *path;        /* host path of the archive */
    const char *name;        /* short name for diagnostics, optional */
} MenuBsaSource;

typedef struct {
    MenuBsaStatus status;
    uint32_t guest_address;  /* 0 on failure */
    uint32_t size;           /* stored SWF bytes copied into guest memory */
    uint32_t archive_index;  /* winning source index */
    uint32_t stored_size;    /* archive record's stored bytes */
    uint32_t crc32;          /* CRC-32 of the copied bytes */
    char archive_name[MENU_BSA_ARCHIVE_NAME_MAX];
    char virtual_path[MENU_BSA_PATH_MAX];
    char reason[MENU_BSA_REASON_MAX];
    MenuSwfHeader swf;
} MenuBsaResult;

/* Default archive order, index 0 first and index 1 last (override). */
extern const char *const menu_bsa_archive_names[MENU_BSA_ARCHIVE_COUNT];

typedef struct {
    char storage[MENU_BSA_ARCHIVE_COUNT][MENU_BSA_DATA_PATH_MAX];
    MenuBsaSource source[MENU_BSA_ARCHIVE_COUNT];
} MenuBsaSources;

const char *menu_bsa_status_name(MenuBsaStatus status);
const char *menu_swf_format_name(MenuSwfFormat format);

/* Fill `out` with <data_dir>/Skyrim - Interface.bsa and <data_dir>/Update.bsa
 * in the measured order.  Returns 1 on success, 0 with a reason on failure. */
int menu_bsa_sources_init(MenuBsaSources *out, const char *data_dir,
                          char *reason, size_t reason_cap);

/* Parse and validate only the 8-byte SWF header plus the CWS zlib prefix.
 * `file_bytes` is the complete SWF file size inside the BSA entry (the BSA
 * record's unpacked size, still CWS-compressed internally).  FWS and CWS pass;
 * ZWS and unknown signatures return BSA_ERR_UNSUPPORTED with a named reason. */
BsaStatus menu_swf_header_parse(const uint8_t *data, size_t available,
                                uint32_t file_bytes, MenuSwfHeader *out,
                                char *reason, size_t reason_cap);

/* Resolve `virtual_path` through `sources` in order and copy the winning
 * stored bytes into `heap`.  The returned guest block is owned by the caller
 * and must be released with heap_free(heap, result->guest_address).
 *
 * Refusals are explicit:
 *   MENU_BSA_MISSING        no source contains the path
 *   MENU_BSA_ACCESS         a present archive could not be opened or read
 *                           (EACCES/EBUSY/IO); never silently skipped
 *   MENU_BSA_CORRUPT        a searched archive is present but fails its format,
 *                           bounds or size checks, or the winning entry fails
 *   MENU_BSA_UNSUPPORTED    SWF container compression/signature unsupported
 *   MENU_BSA_BUDGET         entry or SWF exceeds `budget_bytes`
 *   MENU_BSA_GUEST_MEMORY   the guest heap cannot provide the block
 *
 * `budget_bytes` bounds the archive entry's unpacked size, its stored
 * (compressed) size and the guest allocation.  A compressed entry whose stored
 * bytes exceed the caller budget is refused before any large temporary
 * allocation.  It is not a GFx decoded-movie budget. */
MenuBsaStatus menu_bsa_resolve(GuestHeap *heap,
                               const MenuBsaSource *sources, size_t source_count,
                               const char *virtual_path, uint32_t budget_bytes,
                               const BsaOptions *options, MenuBsaResult *out);

/* Convenience wrapper for "Interface\\StartMenu.swf". */
MenuBsaStatus menu_bsa_resolve_startmenu(GuestHeap *heap,
                                         const MenuBsaSource *sources,
                                         size_t source_count,
                                         uint32_t budget_bytes,
                                         MenuBsaResult *out);
