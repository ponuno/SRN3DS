/* Deterministic host repro for the menu crash at 0x60aa70.
 *
 * The real run faulted with
 *     addr=0x62783049 last_call=0x60aa70 ecx=0x62783041
 * i.e. `mov eax,[ecx+8]` inside the tiny retail function 0x60aa70, where
 * `ecx` is a `this` reached as `[[record+4]+0x28]` by the menu code.
 *
 * This test executes the *real* retail bytes of 0x60aa70 and 0x604750 through
 * the real IR container tier (mini IRCNT001 built by tools/menu_repro_pack.py
 * with the real C decoder).  Only two pieces are synthetic:
 *
 *   0x451a30  the registry hash-map lookup behind 0x604750, replaced by a
 *             deterministic key/record pair in guest memory (0x705000/4);
 *   0x451a60  a caller mirroring the real call site
 *             `mov ecx,[record+0x28]; call 0x60aa70`.
 *
 * Scenarios:
 *   A  valid `this`, +8 string, registry hit, tag '<'/'G'  -> returns the record
 *   A' +8 == 0 but +0xc valid                              -> still returns it
 *      (so "+8 == 0" alone is NOT a fault; this corrects the older wording)
 *   B  `this` = 0x62783041 (unmapped)                      -> fault addr 0x62783049
 *   C  `this` = 0 (NULL)                                   -> fault addr 0x8
 *
 * The fault text is the runtime's own, so `last_call`/`addr` are produced by
 * the real dispatch path, not by this file.
 */
#include "n3ds_lift_runtime.h"
#include "menu_repro_irc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#define getpid _getpid
#else
#include <unistd.h>
#endif

#define STACK      0x00600000u
#define STACK_TOP  0x00601800u
#define RECORD     0x00500000u
#define OBJECT     0x00501000u
#define STRING     0x00502000u
#define REG        0x00503000u
#define GLOBAL_PG  0x01310000u   /* covers [0x1310460] */
#define REGPAGE    0x00705000u   /* stub key/record pair */

#define BAD_THIS   0x62783041u

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "menu_crash_repro: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static void dummy_aot(CPU *c) { c->r[0] = 0; }

static int w32(GuestMemory *m, uint32_t addr, uint32_t v) {
    return guest_write32(m, addr, v);
}
static int w8(GuestMemory *m, uint32_t addr, unsigned char v) {
    return guest_write(m, addr, &v, 1);
}

static int set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static int capture_begin(FILE **file, int *saved) {
    fflush(stderr);
    *file = tmpfile();
    if (!*file) return -1;
    *saved = dup(fileno(stderr));
    if (*saved < 0) return -1;
    if (dup2(fileno(*file), fileno(stderr)) < 0) return -1;
    return 0;
}

static char *capture_end(FILE *file, int saved) {
    fflush(stderr);
    if (dup2(saved, fileno(stderr)) < 0) return NULL;
    close(saved);
    if (fseek(file, 0, SEEK_END)) return NULL;
    long length = ftell(file);
    if (length < 0 || length > 1024 * 1024) return NULL;
    rewind(file);
    char *text = (char *)malloc((size_t)length + 1u);
    if (!text) return NULL;
    size_t got = fread(text, 1, (size_t)length, file);
    fclose(file);
    text[got] = 0;
    return text;
}

static int run(CPU *cpu, uint32_t record, uint32_t *result) {
    uint32_t arg = record;
    *result = 0xdeadbeefu;
    n3ds_lift_init_cpu(cpu, STACK_TOP, 0);
    return n3ds_lift_invoke(cpu, MENU_REPRO_ENTRY, &arg, 1, result);
}

/* Fault runs must be the only stderr the invocation produces (the container
 * open message was flushed before the first capture). */
static int expect_fault(CPU *cpu, GuestMemory *m, uint32_t this_ptr,
                        const char *want_addr, const char *want_ecx,
                        const char *what) {
    uint32_t result = 0;
    FILE *capture = NULL;
    int saved = -1;
    char *text = NULL;

    CHECK(!w32(m, RECORD + 0x28, this_ptr));
    /* Ask the runtime for the full register dump so the faulting ECX is
     * asserted, not inferred. */
    CHECK(!set_env("TESV_FAULT_REGS", "1"));
    CHECK(!capture_begin(&capture, &saved));
    int rc = run(cpu, RECORD, &result);
    text = capture_end(capture, saved);
    CHECK(!set_env("TESV_FAULT_REGS", ""));
    if (rc != N3DS_INVOKE_TRAP) {
        fprintf(stderr, "menu_crash_repro: %s: expected TRAP, got %d\n", what, rc);
        fprintf(stderr, "%s\n", text ? text : "(no stderr)");
        free(text);
        return 1;
    }
    if (!text) {
        fprintf(stderr, "menu_crash_repro: %s: no stderr captured\n", what);
        return 1;
    }
    char want[64];
    snprintf(want, sizeof want, "addr=%s", want_addr);
    if (!strstr(text, want) || !strstr(text, "last_call=0060aa70") ||
        !strstr(text, "irc_fn=0060aa70")) {
        fprintf(stderr, "menu_crash_repro: %s: fault text lacks %s/last_call "
                        "irc_fn=0060aa70\n%s\n", what, want, text);
        free(text);
        return 1;
    }
    if (!strstr(text, want_ecx)) {
        fprintf(stderr, "menu_crash_repro: %s: fault text lacks %s\n%s\n",
                what, want_ecx, text);
        free(text);
        return 1;
    }
    if (n3ds_lift_last_call() != MENU_REPRO_CHECK) {
        fprintf(stderr, "menu_crash_repro: %s: last_call=%08x\n", what,
                n3ds_lift_last_call());
        free(text);
        return 1;
    }
    if (!strstr(n3ds_lift_fault(), want_addr)) {
        fprintf(stderr, "menu_crash_repro: %s: fault text '%s'\n", what,
                n3ds_lift_fault());
        free(text);
        return 1;
    }
    /* Show the runtime's own unmapped-line so the repro is visible. */
    {
        const char *line = strstr(text, "[guest] unmapped");
        if (line) {
            size_t n = strcspn(line, "\r\n");
            printf("FAULT %s: %.*s\n", what, (int)n, line);
        }
        printf("TRAP  %s: %s\n", what, n3ds_lift_fault());
    }
    free(text);
    return 0;
}

int main(void) {
    GuestMemory memory;
    CPU cpu;
    static const FnEntry table[] = {{0x00010000u, dummy_aot}};
    uint32_t result = 0;
    char path[512];
    const char *tmp = getenv("TEMP");
    FILE *f = NULL;

    if (!tmp || !*tmp) tmp = ".";

    guest_init(&memory, 0x40000);
    CHECK(!guest_map(&memory, STACK, 0x2000));
    CHECK(!guest_map(&memory, RECORD, 0x1000));
    CHECK(!guest_map(&memory, OBJECT, 0x1000));
    CHECK(!guest_map(&memory, STRING, 0x1000));
    CHECK(!guest_map(&memory, REG, 0x1000));
    CHECK(!guest_map(&memory, GLOBAL_PG, 0x1000));
    CHECK(!guest_map(&memory, REGPAGE, 0x1000));

    snprintf(path, sizeof path, "%s/menu_repro_%lu.irc", tmp,
             (unsigned long)getpid());
    f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(menu_repro_irc, 1, sizeof menu_repro_irc, f) ==
          sizeof menu_repro_irc);
    CHECK(!fclose(f));

    CHECK(!n3ds_lift_set_container_path(path));
    CHECK(!n3ds_lift_bind(&memory, table, 1));
    n3ds_lift_init_cpu(&cpu, STACK_TOP, 0);
    /* Open the container eagerly so its stderr banner is outside the capture. */
    CHECK(n3ds_lift_has_function(MENU_REPRO_ENTRY));
    CHECK(n3ds_lift_container_state() == 1);

    /* [0x1310460] is read by 0x60aa70 before the (stubbed) lookup call. */
    CHECK(!w32(&memory, 0x01310460u, 0x00000001u));
    CHECK(!w32(&memory, STRING, 0x656d6147u));   /* "Game" */
    CHECK(!w32(&memory, STRING + 4, 0x756e654du)); /* "Menu" */
    CHECK(!w8(&memory, STRING + 8, 0));

    /* ---------------- scenario A: valid object, registry hit ------------- */
    CHECK(!w32(&memory, RECORD + 0x28, OBJECT));
    CHECK(!w32(&memory, OBJECT + 8, STRING));
    CHECK(!w32(&memory, OBJECT + 0xc, 0));
    CHECK(!w32(&memory, REGPAGE + 0, STRING));
    CHECK(!w32(&memory, REGPAGE + 4, REG));
    CHECK(!w8(&memory, REG + 0x12, 'G'));

    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == REG);

    /* tag '<' is the other accepted tag */
    CHECK(!w8(&memory, REG + 0x12, '<'));
    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == REG);

    /* the tag byte really decides: an unknown tag returns NULL */
    CHECK(!w8(&memory, REG + 0x12, 'X'));
    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == 0);
    CHECK(!w8(&memory, REG + 0x12, 'G'));

    /* a lookup miss (key mismatch) returns NULL, no fault */
    CHECK(!w32(&memory, REGPAGE + 0, STRING + 4));
    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == 0);
    CHECK(!w32(&memory, REGPAGE + 0, STRING));

    /* +8 == 0 falls back to +0xc; that is NOT a fault in this function. */
    CHECK(!w32(&memory, OBJECT + 8, 0));
    CHECK(!w32(&memory, OBJECT + 0xc, STRING));
    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == REG);
    CHECK(!w32(&memory, OBJECT + 8, STRING));
    CHECK(!w32(&memory, OBJECT + 0xc, 0));

    /* ---------------- scenario B: the real crash ----------------------- */
    if (expect_fault(&cpu, &memory, BAD_THIS, "62783049", "ecx=62783041",
                     "scenario B")) return 1;

    /* ---------------- scenario C: NULL this ---------------------------- */
    if (expect_fault(&cpu, &memory, 0, "00000008", "ecx=00000000",
                     "scenario C")) return 1;

    /* Restore a good record and prove the container is still healthy after
     * two traps (pins recovered, no re-decode). */
    CHECK(!w32(&memory, RECORD + 0x28, OBJECT));
    CHECK(run(&cpu, RECORD, &result) == N3DS_INVOKE_OK);
    CHECK(result == REG);

    {
        N3dsIrcStats stats;
        CHECK(!n3ds_lift_container_stats(&stats));
        CHECK(stats.execs >= 8);
        CHECK(stats.redecodes == 0);
        CHECK(stats.opens >= 1);
    }

    n3ds_lift_release();
    guest_destroy(&memory);
    remove(path);
    puts("HOST PASS: menu_crash_repro");
    return 0;
}
