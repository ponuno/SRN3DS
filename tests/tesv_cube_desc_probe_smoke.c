/* Measurement-only TESV_CUBE_DESC_PROBE output contract. */
#include "n3ds_lift_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

#define ARG   0x00500000u
#define BASE  0x00501000u
#define SOURCE 0x00502000u

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_cube_desc_probe: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

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
    return dup2(fileno(*file), fileno(stderr));
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

static void dummy(CPU *c) { (void)c; }

int main(void) {
    GuestMemory memory;
    static const FnEntry table[] = {{0x00401000u, dummy}};
    FILE *capture = NULL;
    int saved = -1;
    char *text = NULL;

    CHECK(!set_env("TESV_CUBE_DESC_PROBE", ""));
    guest_init(&memory, 0x10000);
    CHECK(!guest_map(&memory, ARG, 0x1000));
    CHECK(!guest_map(&memory, BASE, 0x1000));
    CHECK(!guest_map(&memory, SOURCE, 0x1000));
    CHECK(!guest_write32(&memory, ARG + 0x604, SOURCE));
    CHECK(!guest_write32(&memory, ARG + 0x624, 0x16));
    CHECK(!guest_write32(&memory, SOURCE + 0x10, 0x16));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!guest_write32(&memory, BASE + i * 4, 0x11111111u + i));
    CHECK(!n3ds_lift_bind(&memory, table, 1));

    /* Default output is unchanged. */
    CHECK(!capture_begin(&capture, &saved));
    n3ds_cube_desc_probe(ARG, 0x16, BASE, 0);
    text = capture_end(capture, saved);
    capture = NULL;
    CHECK(text && !strstr(text, "[cube-desc]"));
    free(text); text = NULL;

    /* Enabled output names the NULL sentinel slot and its source object. */
    CHECK(!set_env("TESV_CUBE_DESC_PROBE", "1"));
    CHECK(!capture_begin(&capture, &saved));
    n3ds_cube_desc_probe(ARG, 0x16, BASE, 0);
    text = capture_end(capture, saved);
    capture = NULL;
    CHECK(text);
    CHECK(strstr(text,
        "[cube-desc] arg=00500000 index=22 base=00501000 slot=00000000 "
        "slot_null=yes base_null=no index_eq_0x16=yes index_ge_0x16=yes "
        "source_obj=00502000 source_index=00000016 source_ok=yes"));
    CHECK(strstr(text,
        "[cube-desc] base8=11111111 11111112 11111113 11111114 "
        "11111115 11111116 11111117 11111118"));
    free(text); text = NULL;

    CHECK(!set_env("TESV_CUBE_DESC_PROBE", ""));
    n3ds_lift_release();
    guest_destroy(&memory);
    puts("PASS tesv_cube_desc_probe");
    return 0;
}
