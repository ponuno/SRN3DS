/* Measurement-only TESV_FAULT_REGS output contract. */
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

#define FAULT_ADDR 0x00401000u
#define OBJECT     0x00500000u
#define INNER      0x00501000u
#define STACK      0x00600000u
#define STACK_TOP  (STACK+0x800u)

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_fault_regs: FAIL line %d: %s\n", __LINE__, #cond); \
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

static void fault_entry(CPU *c) {
    (void)c;
    (void)M32(0xdeadbeefu);
}

int main(void) {
    GuestMemory memory;
    CPU cpu;
    uint32_t result = 0;
    static const FnEntry table[] = {{FAULT_ADDR, fault_entry}};
    FILE *capture = NULL;
    int saved = -1;
    char *text = NULL;

    CHECK(!set_env("N3DS_WATCH_ADDR", ""));
    CHECK(!set_env("N3DS_WATCH_VALUE", ""));
    guest_init(&memory, 0x10000);
    CHECK(!guest_map(&memory, OBJECT, 0x1000));
    CHECK(!guest_map(&memory, INNER, 0x1000));
    CHECK(!guest_map(&memory, STACK, 0x1000));
    CHECK(!guest_write32(&memory, OBJECT + 0x40, INNER));
    CHECK(!guest_write32(&memory, INNER, 0x12345678u));
    CHECK(!guest_write32(&memory, OBJECT + 0x8, 0x90abcdefu));
    CHECK(!n3ds_lift_bind(&memory, table, 1));

    n3ds_lift_init_cpu(&cpu, STACK_TOP, 0);
    cpu.r[0] = 0x11111111u;
    cpu.r[1] = OBJECT;
    cpu.r[2] = 0x22222222u;
    cpu.r[3] = 0x33333333u;
    cpu.r[5] = 0x44444444u;
    cpu.r[6] = 0x55555555u;
    cpu.r[7] = 0x66666666u;

    /* Default output remains the original unmapped/esp/stack diagnostic. */
    CHECK(!set_env("TESV_FAULT_REGS", "0"));
    CHECK(!capture_begin(&capture, &saved));
    CHECK(n3ds_lift_invoke(&cpu, FAULT_ADDR, NULL, 0, &result) == N3DS_INVOKE_TRAP);
    text = capture_end(capture, saved);
    capture = NULL;
    CHECK(text && strstr(text, "[guest] esp="));
    CHECK(!strstr(text, "[guest] regs"));
    CHECK(!strstr(text, "[guest] ecx_deref"));
    free(text); text = NULL;

    /* Enabled output includes all requested registers and both object reads. */
    CHECK(!set_env("TESV_FAULT_REGS", "1"));
    CHECK(!capture_begin(&capture, &saved));
    CHECK(n3ds_lift_invoke(&cpu, FAULT_ADDR, NULL, 0, &result) == N3DS_INVOKE_TRAP);
    text = capture_end(capture, saved);
    capture = NULL;
    CHECK(text);
    CHECK(strstr(text,
        "[guest] regs eax=11111111 ebx=33333333 ecx=00500000 edx=22222222 "
        "esi=55555555 edi=66666666 ebp=44444444 esp=006007fc eip=00401000"));
    CHECK(strstr(text,
        "[guest] ecx_deref: [ecx+0x40]=00501000 "
        "[[ecx+0x40]]=12345678 [ecx+0x8]=90abcdef"));
    free(text); text = NULL;

    /* An unreadable ECX is diagnosed without introducing a second fault. */
    cpu.r[1] = 0xdeadbeefu;
    CHECK(!capture_begin(&capture, &saved));
    CHECK(n3ds_lift_invoke(&cpu, FAULT_ADDR, NULL, 0, &result) == N3DS_INVOKE_TRAP);
    text = capture_end(capture, saved);
    capture = NULL;
    CHECK(text && strstr(text, "[guest] regs"));
    CHECK(strstr(text, "[guest] ecx_deref: unreadable"));
    free(text); text = NULL;

    CHECK(!set_env("TESV_FAULT_REGS", ""));
    n3ds_lift_release();
    guest_destroy(&memory);
    puts("PASS tesv_fault_regs");
    return 0;
}
