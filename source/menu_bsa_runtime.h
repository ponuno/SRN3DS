#pragma once
/* Runtime path from the shipped BSA archives to Interface/StartMenu.swf.
 *
 * This is an asset-loading path only: it validates the SWF container header and
 * returns the stored bytes in guest memory.  It does not decompress the GFx
 * body, build a movie, create fonts or draw a frame.  A caller that later
 * implements the GFx draw path owns the CWS/ZWS body decompression.
 *
 * Archive precedence follows the mount order: the sources are listed in
 * Skyrim.ini `[Archive]` (`sResourceArchiveList` then
 * `sResourceArchiveList2`) and a later archive overrides a same-path file in an
 * earlier one.  When the INI is absent or the lists are empty, the measured
 * fallback is Skyrim - Interface.bsa followed by Update.bsa.  The resolver
 * searches every source, remembers the last archive containing the path, then
 * reads only that winner.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bsa_reader.h"
#include "guest_heap.h"
#include "menu_swf.h"

#define MENU_BSA_PATH_MAX 512u
#define MENU_BSA_REASON_MAX 224u
#define MENU_BSA_ARCHIVE_NAME_MAX 64u
#define MENU_BSA_DATA_PATH_MAX 1024u
#define MENU_BSA_MAX_SOURCES 32u
#define MENU_BSA_ARCHIVE_COUNT 2u
/* Sum of every mounted archive's parsed index.  The reader's per-archive cap is
 * 24 MiB; four large texture archives plus the small ones still fit.  Exceeding
 * this is a named refusal, never a silent truncation of the INI list. */
#define MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES (96u * 1024u * 1024u)

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
    char storage[MENU_BSA_MAX_SOURCES][MENU_BSA_DATA_PATH_MAX];
    MenuBsaSource source[MENU_BSA_MAX_SOURCES];
    size_t count;
    uint64_t total_index_budget; /* 0 means MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES */
    int skip_unusable;           /* INI mounts log and skip absent/corrupt archives */
} MenuBsaSources;

const char *menu_bsa_status_name(MenuBsaStatus status);

/* Fill `out` with <data_dir>/Skyrim - Interface.bsa and <data_dir>/Update.bsa
 * in the measured order.  Returns 1 on success, 0 with a reason on failure. */
int menu_bsa_sources_init(MenuBsaSources *out, const char *data_dir,
                          char *reason, size_t reason_cap);

/* Read [Archive] sResourceArchiveList and sResourceArchiveList2 from
 * `ini_path` and fill `out` in listed order: List first, then List2.  Names
 * keep their embedded spaces, are split on commas, accept a case-insensitive
 * `.bsa` suffix and skip empty elements.  A missing/unreadable INI or an empty
 * list falls back to `menu_bsa_sources_init` (Interface then Update).
 *
 * The list is capped at MENU_BSA_MAX_SOURCES.  A longer list is a named
 * refusal (return 0); it is never quietly truncated.  The resolver enforces the
 * summed index budget and, because `skip_unusable` is 1, logs and skips an
 * absent or corrupt archive instead of aborting the mount. */
int menu_bsa_sources_from_ini(MenuBsaSources *out, const char *data_dir,
                              const char *ini_path,
                              char *reason, size_t reason_cap);

/* Boot helper.  `root` is the device game root ("sdmc:/Skyrim"): archives are
 * read from <root>/Data, and the INI is tried at <root>/Skyrim.ini first, then
 * <root>/Data/Skyrim.ini (where `deploy_device --pack cooked` writes it).
 * With no readable non-empty list the Interface/Update fallback is used. */
int menu_bsa_sources_from_boot(MenuBsaSources *out, const char *root,
                               char *reason, size_t reason_cap);

/* Startup mount line for the device log:
 *   [menu-bsa] N archive(s) from Skyrim.ini: name1, name2
 *   [menu-bsa] fallback: Interface+Update
 * `log` is normally stderr. */
void menu_bsa_sources_log(const MenuBsaSources *set, FILE *log);

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

/* As menu_bsa_resolve, plus the summed-index cap and the lenient mount policy.
 * `total_index_budget` 0 means MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES.  With
 * `skip_unusable` set, an archive that is missing or fails to open is named on
 * stderr and skipped; only an over-budget index sum is fatal. */
MenuBsaStatus menu_bsa_resolve_ex(GuestHeap *heap,
                                  const MenuBsaSource *sources,
                                  size_t source_count,
                                  const char *virtual_path,
                                  uint32_t budget_bytes,
                                  const BsaOptions *options,
                                  uint64_t total_index_budget,
                                  int skip_unusable, MenuBsaResult *out);

/* Resolve through a source set built by menu_bsa_sources_init/from_ini,
 * applying its count, index budget and skip policy. */
MenuBsaStatus menu_bsa_resolve_sources(GuestHeap *heap,
                                       const MenuBsaSources *set,
                                       const char *virtual_path,
                                       uint32_t budget_bytes,
                                       const BsaOptions *options,
                                       MenuBsaResult *out);

/* Convenience wrapper for "Interface\\StartMenu.swf". */
MenuBsaStatus menu_bsa_resolve_startmenu(GuestHeap *heap,
                                         const MenuBsaSource *sources,
                                         size_t source_count,
                                         uint32_t budget_bytes,
                                         MenuBsaResult *out);

MenuBsaStatus menu_bsa_resolve_startmenu_sources(GuestHeap *heap,
                                                 const MenuBsaSources *set,
                                                 uint32_t budget_bytes,
                                                 MenuBsaResult *out);
