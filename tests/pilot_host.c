/* Host runner for a generated, real-DLL pilot. Mirrors pilot_main.c on 3DS. */
#include "n3ds_pilot.h"
#include "pilot_config.h"
#include <stdio.h>

extern const FnEntry g_fn_table_pilot[];
extern const unsigned g_fn_count_pilot;
static const PilotSpec spec = {PILOT_EXPORT, PILOT_CASE, PILOT_ENTRY, PILOT_BASE,
                               PILOT_FILE_BYTES, PILOT_FNV64};

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s DLL\n", argv[0]); return 2; }
    PilotResult r;
    int rc = n3ds_pilot_run(argv[1], &spec, g_fn_table_pilot, g_fn_count_pilot, 48u << 20, &r);
#if PILOT_ILT_BYPASS
    printf("Entry: lazy ILT thunk resolved to body 0x%08lx; wrapper 0x%08lx and one-time initializer 0x%08lx are not executed\n",
           (unsigned long)PILOT_ENTRY, (unsigned long)PILOT_ILT_WRAPPER, (unsigned long)PILOT_ILT_INIT);
#else
    printf("Entry: direct export body (no thunk)\n");
#endif
    printf("%s\nGuest memory: %lu / %lu KiB\n", r.message,
           (unsigned long)(r.memory / 1024), (unsigned long)(r.limit / 1024));
    return rc ? 1 : 0;
}
