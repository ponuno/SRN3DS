/* Regression test for the TESV_INI_OVERLAY read overlay.
 *
 * The overlay is a world-entry experiment aid: it injects [General] settings
 * into INI *reads* so the engine's settings loader (0x691410 -> the sStarting*
 * slots) sees them without editing the profile on the SD card or in the slice
 * run directory.  The test drives the same parser/merge the read path uses,
 * through win32_ini_overlay_merge_text(), so it needs no guest memory, no
 * profile root and no engine image.
 *
 * Covered:
 *   - file overlay: replace an existing value, add missing ones;
 *   - inline "key=value;key=value" form, last duplicate wins;
 *   - unsupported keys are ignored (sLanguage stays as in the file);
 *   - quoted values, CRLF and spaces around '=';
 *   - a file without a [General] section gets one appended;
 *   - [Display] values are never touched;
 *   - only Skyrim.INI/Skyrim.ini is a target;
 *   - the no-overlay case leaves the buffer byte-for-byte unchanged.
 */
#include "win32_shims.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFCAP (1024u * 1024u)
#define NAME "Documents\\My Games\\Skyrim\\Skyrim.INI"

static unsigned failures;
static char work[BUFCAP];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static const char *SRC =
    "[General]\n"
    "sLanguage=ENGLISH\n"
    "sStartingCell=FromFile\n"
    "bPreloadIntroSequence=1\n"
    "\n"
    "[Display]\n"
    "sStartingCell=NotGeneral\n";

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
}

static int merge(const char *spec, const char *name) {
    size_t n = strlen(SRC);
    memcpy(work, SRC, n + 1);
    return win32_ini_overlay_merge_text(spec, name, work, sizeof work, &n);
}

/* Whole-line match: the line must start at a line boundary and end there. */
static int has_line(const char *text, const char *line) {
    const char *p = text;
    size_t n = strlen(line);
    while ((p = strstr(p, line)) != NULL) {
        if ((p == text || p[-1] == '\n') && (p[n] == '\n' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

int main(void) {
    /* --- no overlay: buffer unchanged ---------------------------------- */
    {
        size_t n = strlen(SRC);
        memcpy(work, SRC, n + 1);
        CHECK(win32_ini_overlay_merge_text(NULL, NAME, work, sizeof work, &n) == 0);
        CHECK(!memcmp(work, SRC, n));
        CHECK(win32_ini_overlay_merge_text("", NAME, work, sizeof work, &n) == 0);
        CHECK(!memcmp(work, SRC, n));
    }

    /* --- file form: replace one value, add two ------------------------- */
    write_file("overlay.ini",
               "; comment line\n"
               "sStartingCell=RiverwoodSleepingGiantInn\n"
               "sStartingCellX=6\n"
               "sStartingCellY=-3\n");
    CHECK(merge("overlay.ini", NAME) > 0);
    CHECK(has_line(work, "sStartingCell=RiverwoodSleepingGiantInn"));
    CHECK(has_line(work, "sStartingCellX=6"));
    CHECK(has_line(work, "sStartingCellY=-3"));
    CHECK(has_line(work, "sLanguage=ENGLISH"));
    CHECK(has_line(work, "sStartingCell=NotGeneral"));   /* [Display] intact */
    {
        /* The overlay block is appended last, so the profile parser (which is
         * last-match-wins) returns the overlay value for General/sStartingCell. */
        const char *d = strstr(work, "[Display]");
        const char *c = strstr(work, "sStartingCell=RiverwoodSleepingGiantInn");
        CHECK(d && c && c > d);
        const char *last = strstr(work, "sStartingCell=");
        while (last && strstr(last + 1, "sStartingCell="))
            last = strstr(last + 1, "sStartingCell=");
        CHECK(last && !strncmp(last, "sStartingCell=Riverwood", 22));
    }

    /* --- inline form: last duplicate wins, unsupported key ignored ----- */
    CHECK(merge("sStartingCell=Inline; sStartingCell=Inline2; "
                "bPreloadIntroSequence=0; sLanguage=FRENCH", NAME) > 0);
    CHECK(has_line(work, "sStartingCell=Inline2"));
    CHECK(!has_line(work, "sStartingCell=Inline"));
    CHECK(has_line(work, "bPreloadIntroSequence=0"));
    CHECK(has_line(work, "sLanguage=ENGLISH"));      /* unsupported: ignored */
    CHECK(!strstr(work, "FRENCH"));

    /* --- quoted value -------------------------------------------------- */
    CHECK(merge("sIntroSequence=\"logo.bik\"", NAME) > 0);
    CHECK(has_line(work, "sIntroSequence=logo.bik"));

    /* --- no [General] section: one is appended ------------------------- */
    {
        static const char *NOGEN = "[Display]\nfBrightness=1\n";
        size_t n = strlen(NOGEN);
        memcpy(work, NOGEN, n + 1);
        CHECK(win32_ini_overlay_merge_text("sStartingCell=RiverwoodSleepingGiantInn",
                                           NAME, work, sizeof work, &n) > 0);
        CHECK(has_line(work, "[General]"));
        CHECK(has_line(work, "sStartingCell=RiverwoodSleepingGiantInn"));
        CHECK(has_line(work, "fBrightness=1"));
    }

    /* --- non-target INI names are untouched ---------------------------- */
    CHECK(merge("sStartingCell=RiverwoodSleepingGiantInn",
                "Documents\\My Games\\Skyrim\\SkyrimPrefs.ini") == 0);
    CHECK(!memcmp(work, SRC, strlen(SRC)));
    CHECK(merge("sStartingCell=RiverwoodSleepingGiantInn", "Skyrim.ini") > 0);
    CHECK(has_line(work, "sStartingCell=RiverwoodSleepingGiantInn"));

    /* --- whitespace / CRLF inline form --------------------------------- */
    CHECK(merge(" sStartingCell = RiverwoodSleepingGiantInn \r\n"
                "sStartingCellX=1\r\n", NAME) > 0);
    CHECK(has_line(work, "sStartingCell=RiverwoodSleepingGiantInn"));
    CHECK(has_line(work, "sStartingCellX=1"));

    /* --- section headers: [Display] keys skipped, [General] applied ---- */
    CHECK(merge("[Display]\nsStartingCell=Ignored\n[General]\nsStartingWorld=Tamriel",
                NAME) > 0);
    CHECK(has_line(work, "sStartingWorld=Tamriel"));
    CHECK(has_line(work, "sStartingCell=FromFile"));   /* not overridden */
    CHECK(!has_line(work, "sStartingCell=Ignored"));

    /* --- argument guards ----------------------------------------------- */
    {
        size_t n = strlen(SRC);
        memcpy(work, SRC, n + 1);
        CHECK(win32_ini_overlay_merge_text("sStartingCell=X", NAME, work, 4096, &n) == -1);
    }

    /* Evidence: the effective [General] block the engine's settings loader
     * (0x691410) would parse for the world-entry experiment. */
    merge("sStartingCell=RiverwoodSleepingGiantInn", NAME);
    printf("[ini] effective tail after TESV_INI_OVERLAY:\n%s", work + (strlen(work) > 120 ? strlen(work) - 120 : 0));
    fflush(stdout);

    if (failures) fprintf(stderr, "%u failure(s)\n", failures);
    else fprintf(stderr, "ini_overlay: all checks passed\n");
    remove("overlay.ini");
    return failures ? 1 : 0;
}
