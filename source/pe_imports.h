#pragma once
#include "pe_image.h"

typedef struct {
    const char *dll, *name; /* name=NULL for ordinal import */
    uint16_t ordinal;
    uint32_t iat; /* guest address, not RVA */
} PeImport;
typedef int (*PeImportVisit)(const PeImport *entry,void *ctx);
typedef uint32_t (*PeImportResolve)(const PeImport *entry,void *ctx);
enum { PE_IMPORT_OK=0,PE_IMPORT_FORMAT=-1,PE_IMPORT_LIMIT=-2,PE_IMPORT_UNRESOLVED=-3 };
int pe_import_walk(GuestMemory *m,const GuestImage *im,PeImportVisit visit,void *ctx);
/* All-or-nothing IAT binding; unknown imports prevent any writes. */
int pe_import_bind(GuestMemory *m,const GuestImage *im,PeImportResolve resolve,void *ctx,size_t *count);

typedef struct {const char *dll;const GuestImage *image;} PeModule;
typedef struct {
    GuestMemory *memory;
    const PeModule *modules;
    size_t count;
    PeImportResolve builtin;
    void *builtin_ctx;
    PeImportResolve fallback;
    void *fallback_ctx;
} PeLinker;
/* Return guest address for a named or ordinal export. Forwarders unsupported. */
uint32_t pe_export_find(GuestMemory *m,const GuestImage *im,const char *name,uint16_t ordinal);
/* Resolution order: implemented builtins, loaded PE exports, then an explicit
 * diagnostic fallback. A fallback may return a trap thunk, but must never hide
 * a real export in a loaded module. */
uint32_t pe_link_resolve(const PeImport *entry,void *ctx);
