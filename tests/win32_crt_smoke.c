/* Host regression for the msvcrt/heap/setjmp work ported from upstream:
 * realloc(p,0), qsort with a guest comparator, strtol endptr/base rules,
 * setlocale/localeconv and the _setjmp3 jmp_buf layout.  _beginthread is
 * checked at the resolver level (creating a guest thread needs a real body).
 */
#include "win32_shims.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define STACK 0x22000000u

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args, size_t n, uint32_t *out) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
static void cmp_words(CPU *c) {
    uint32_t a = M32(c->r[4] + 4), b = M32(c->r[4] + 8);
    uint32_t x = M32(a), y = M32(b);
    c->r[0] = x < y ? (uint32_t)-1 : x > y ? 1u : 0u;
    c->r[4] += 4 + 8;
}
static const FnEntry table[] = {{0x00500000u, cmp_words}};
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "crt test failed at line %d: %s\n", __LINE__, #x); goto done; } } while (0)

int main(void) {
    int rc = -1, mapped = 0;
    GuestMemory m;
    CPU *cpu = NULL;
    uint32_t r, a[8];
    guest_init(&m, 16u << 20);
    if (guest_map(&m, DATA, 0x4000) || guest_map(&m, STACK, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x200000, ".")) goto done;
    cpu = calloc(1, sizeof *cpu);
    if (!cpu) goto done;
    n3ds_lift_init_cpu(cpu, STACK + 4096 - 64, 0);

    /* realloc(p,0) frees and returns NULL; realloc(NULL,n) is malloc(n). */
    a[0] = 16; CHECK(!call(cpu, "msvcrt.dll", "malloc", a, 1, &r) && r != 0);
    uint32_t p = r;
    M32(p) = 0x12345678;
    a[0] = p; a[1] = 0;
    CHECK(!call(cpu, "msvcrt.dll", "realloc", a, 2, &r) && r == 0);
    a[0] = 0; a[1] = 24;
    CHECK(!call(cpu, "msvcrt.dll", "realloc", a, 2, &r) && r != 0);
    uint32_t q = r;
    a[0] = q; CHECK(!call(cpu, "msvcrt.dll", "free", a, 1, &r) && r == 0);

    /* qsort calls the guest comparator and sorts 5 dwords. */
    static const uint32_t vals[5] = {5, 1, 4, 2, 3};
    for (unsigned i = 0; i < 5; ++i) M32(DATA + 0x100 + 4 * i) = vals[i];
    a[0] = DATA + 0x100; a[1] = 5; a[2] = 4; a[3] = 0x00500000u;
    CHECK(!call(cpu, "msvcrt.dll", "qsort", a, 4, &r) && r == 0);
    for (unsigned i = 0; i < 5; ++i) CHECK(M32(DATA + 0x100 + 4 * i) == i + 1);

    /* strtol: whitespace/sign/base 0 prefix, endptr points at the first bad char. */
    strcpy((char *)n3ds_guest_ptr(DATA + 0x200, 32), "  -0x1fZ");
    a[0] = DATA + 0x200; a[1] = DATA + 0x280; a[2] = 0;
    CHECK(!call(cpu, "msvcrt.dll", "strtol", a, 3, &r) && (int32_t)r == -31);
    CHECK(M32(DATA + 0x280) == DATA + 0x200 + 7);
    strcpy((char *)n3ds_guest_ptr(DATA + 0x240, 32), "777");
    a[0] = DATA + 0x240; a[1] = 0; a[2] = 8;
    CHECK(!call(cpu, "msvcrt.dll", "strtol", a, 3, &r) && r == 511);
    a[0] = DATA + 0x240; a[1] = 0; a[2] = 10;
    CHECK(!call(cpu, "msvcrt.dll", "strtoul", a, 3, &r) && r == 777);

    /* setlocale("C") returns a guest "C"; an unknown locale is a named refusal. */
    a[0] = 0; a[1] = 0;
    CHECK(!call(cpu, "msvcrt.dll", "setlocale", a, 2, &r) && r && !strcmp(guest_cstr(&m, r, 8), "C"));
    strcpy((char *)n3ds_guest_ptr(DATA + 0x2c0, 8), "C");
    a[0] = 2; a[1] = DATA + 0x2c0;
    CHECK(!call(cpu, "msvcrt.dll", "setlocale", a, 2, &r) && r);
    a[0] = 0; CHECK(!call(cpu, "msvcrt.dll", "localeconv", a, 1, &r) && r);
    uint32_t dp = M32(r);
    CHECK(dp && !strcmp(guest_cstr(&m, dp, 4), "."));

    /* _setjmp3 writes the MSVC layout and returns 0; ESP pops the jmp_buf arg. */
    uint32_t jb = DATA + 0x300, sp = DATA + 0x380;
    M32(sp) = 0xDEADBEEFu;        /* return address */
    M32(sp + 4) = jb;             /* jmp_buf argument */
    cpu->r[4] = sp;
    cpu->r[5] = 0x11111111u;      /* ebp */
    cpu->r[3] = 0x33333333u;      /* ebx */
    cpu->r[7] = 0x77777777u;      /* edi */
    cpu->r[6] = 0x66666666u;      /* esi */
    cpu->r[0] = 0xAAAAAAAAu;
    rt_setjmp3(cpu);
    CHECK(cpu->r[0] == 0);
    CHECK(cpu->r[4] == sp + 4);
    CHECK(M32(jb + 0) == 0x11111111u && M32(jb + 4) == 0x33333333u);
    CHECK(M32(jb + 8) == 0x77777777u && M32(jb + 12) == 0x66666666u);
    CHECK(M32(jb + 16) == sp + 4 && M32(jb + 20) == 0xDEADBEEFu);

    /* The msvcrt thread entry points are reachable. */
    CHECK(win32_resolve(&(PeImport){.dll = "msvcrt.dll", .name = "_beginthread"}, NULL) != 0);
    CHECK(win32_resolve(&(PeImport){.dll = "msvcrt.dll", .name = "_beginthreadex"}, NULL) != 0);
    CHECK(win32_resolve(&(PeImport){.dll = "msvcrt.dll", .name = "_setjmp3"}, NULL) != 0);
    CHECK(win32_resolve(&(PeImport){.dll = "msvcrt.dll", .name = "longjmp"}, NULL) != 0);

    rc = 0;
done:
    free(cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
