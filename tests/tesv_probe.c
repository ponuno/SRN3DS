/* Host probe: map the real TESV PE, bind imports with the current shims and
 * report the first unresolved imports in IAT order. No guest code is executed. */
#include "guest.h"
#include "pe_image.h"
#include "pe_imports.h"
#include "win32_shims.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const PeLinker *linker;
    unsigned total, unresolved, shown;
} Report;

static int visit(const PeImport *e, void *ctx) {
    Report *r = ctx;
    ++r->total;
    if (!pe_link_resolve(e, (void *)r->linker)) {
        ++r->unresolved;
        if (e->name && r->shown < 40) {
            printf("  unresolved %s!%s\n", e->dll, e->name);
            ++r->shown;
        }
        if (!e->name && r->shown < 40) {
            printf("  unresolved %s!#%u\n", e->dll, (unsigned)e->ordinal);
            ++r->shown;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s TESV.exe [game_root]\n", argv[0]); return 2; }
    const char *root = argc > 2 ? argv[2] : ".";

    GuestMemory m;
    guest_init(&m, 64u << 20);
    if (win32_init(&m, 8u << 20, root)) { fprintf(stderr, "win32_init failed\n"); return 1; }

    GuestImage image;
    int rc = pe_image_load(&m, argv[1], 0, &image);
    if (rc) { fprintf(stderr, "pe_image_load: %s\n", pe_error(rc)); win32_shutdown(); return 1; }
    printf("TESV image base %08lx entry %08lx size %lu KiB\n",
           (unsigned long)image.base, (unsigned long)image.entry, (unsigned long)(image.size >> 10));

    PeModule modules[1] = {{"TESV.exe", &image}};
    PeLinker linker = {&m, modules, 1, win32_resolve, NULL, NULL, NULL};
    win32_set_linker(&linker);

    Report report = {&linker, 0, 0, 0};
    rc = pe_import_walk(&m, &image, visit, &report);
    printf("imports: %u total, %u unresolved (walk rc %d)\n", report.total, report.unresolved, rc);

    uint32_t first_iat = 0;
    /* Bind only if everything resolved; otherwise the message is the blocker. */
    if (!report.unresolved) {
        size_t bound = 0;
        int br = pe_import_bind(&m, &image, pe_link_resolve, &linker, &bound);
        printf("bind: rc %d, %lu entries\n", br, (unsigned long)bound);
    } else {
        printf("first blocker: an import above has no shim; no IAT was modified\n");
    }
    (void)first_iat;

    win32_shutdown();
    guest_destroy(&m);
    return 0;
}
