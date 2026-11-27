/* Host regression for the msvcrt entry points the Papyrus VM and the script
 * console reach: strtod/atof, _itoa/_ultoa, rand/srand, _errno, the is*
 * classification table, the double-returning math shims (x87 ST0 result) and
 * the _CI* float-argument helpers.  These were previously unresolved imports
 * (an explicit trap) and are now real shims with msvcrt semantics.
 */
#include "win32_shims.h"
#include "n3ds_lift_runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define STACK 0x22000000u

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args,
                size_t n, uint32_t *out) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
static double st0(CPU *c) { return c->st[c->top]; }
static void noop(CPU *c) { (void)c; }
static const FnEntry table[] = {{0x00500000u, noop}};
static uint32_t f32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void dargs(uint32_t *a, double d) { memcpy(a, &d, 8); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "papyrus crt test failed at line %d: %s\n", __LINE__, #x); goto done; } } while (0)

int main(void) {
    int rc = -1, mapped = 0;
    GuestMemory m;
    CPU *cpu = NULL;
    uint32_t r, a[4];
    guest_init(&m, 16u << 20);
    if (guest_map(&m, DATA, 0x8000) || guest_map(&m, STACK, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x200000, ".")) goto done;
    cpu = calloc(1, sizeof *cpu);
    if (!cpu) goto done;
    n3ds_lift_init_cpu(cpu, STACK + 4096 - 64, 0);

    /* strtod: sign, fraction, exponent, endptr; then inf/nan and no-conversion. */
    strcpy((char *)n3ds_guest_ptr(DATA + 0x100, 32), "  -12.5e2xyz");
    a[0] = DATA + 0x100; a[1] = DATA + 0x180;
    CHECK(!call(cpu, "msvcrt.dll", "strtod", a, 2, &r));
    CHECK(st0(cpu) == -1250.0);
    CHECK(M32(DATA + 0x180) == DATA + 0x100 + 9);
    strcpy((char *)n3ds_guest_ptr(DATA + 0x120, 32), "abc");
    a[0] = DATA + 0x120; a[1] = DATA + 0x1a0;
    CHECK(!call(cpu, "msvcrt.dll", "strtod", a, 2, &r));
    CHECK(st0(cpu) == 0.0);
    CHECK(M32(DATA + 0x1a0) == DATA + 0x120);   /* no conversion: original endptr */
    strcpy((char *)n3ds_guest_ptr(DATA + 0x140, 32), "INF");
    a[0] = DATA + 0x140; a[1] = 0;
    CHECK(!call(cpu, "msvcrt.dll", "strtod", a, 2, &r));
    CHECK(isinf(st0(cpu)));
    strcpy((char *)n3ds_guest_ptr(DATA + 0x160, 32), "3.5");
    a[0] = DATA + 0x160;
    CHECK(!call(cpu, "msvcrt.dll", "atof", a, 1, &r));
    CHECK(st0(cpu) == 3.5);
    /* "0x10" is not a hex float in the engine's 2011 msvcrt: 0, endptr at 'x'. */
    strcpy((char *)n3ds_guest_ptr(DATA + 0x1c0, 16), "0x10");
    a[0] = DATA + 0x1c0; a[1] = DATA + 0x1e0;
    CHECK(!call(cpu, "msvcrt.dll", "strtod", a, 2, &r));
    CHECK(st0(cpu) == 0.0 && M32(DATA + 0x1e0) == DATA + 0x1c0 + 1);

    /* _itoa/_ultoa: return the buffer pointer and NUL-terminate in guest memory. */
    a[0] = (uint32_t)-12345; a[1] = DATA + 0x200; a[2] = 10;
    CHECK(!call(cpu, "msvcrt.dll", "_itoa", a, 3, &r) && r == DATA + 0x200);
    CHECK(!strcmp(guest_cstr(&m, DATA + 0x200, 16), "-12345"));
    a[0] = 255; a[1] = DATA + 0x220; a[2] = 16;
    CHECK(!call(cpu, "msvcrt.dll", "_itoa", a, 3, &r) && r == DATA + 0x220);
    CHECK(!strcmp(guest_cstr(&m, DATA + 0x220, 16), "ff"));
    a[0] = 0xffffffffu; a[1] = DATA + 0x240; a[2] = 16;
    CHECK(!call(cpu, "msvcrt.dll", "_ultoa", a, 3, &r) && r == DATA + 0x240);
    CHECK(!strcmp(guest_cstr(&m, DATA + 0x240, 16), "ffffffff"));
    a[0] = 5; a[1] = DATA + 0x260; a[2] = 1;    /* radix < 2: explicit failure */
    CHECK(!call(cpu, "msvcrt.dll", "_itoa", a, 3, &r) && r == 0);

    /* rand/srand: msvcrt LCG, srand(1) -> first value 41. */
    a[0] = 1;
    CHECK(!call(cpu, "msvcrt.dll", "srand", a, 1, &r));
    CHECK(!call(cpu, "msvcrt.dll", "rand", a, 0, &r) && r == 41);
    CHECK(!call(cpu, "msvcrt.dll", "rand", a, 0, &r) && r <= 0x7fffu);

    /* _errno: a stable guest int that a callee can write and the caller rereads. */
    CHECK(!call(cpu, "msvcrt.dll", "_errno", a, 0, &r) && r);
    uint32_t errno_p = r;
    M32(errno_p) = 34;
    CHECK(!call(cpu, "msvcrt.dll", "_errno", a, 0, &r) && r == errno_p && M32(r) == 34);

    /* is* classification: ASCII only, non-ASCII is never a letter. */
    a[0] = '7';
    CHECK(!call(cpu, "msvcrt.dll", "isdigit", a, 1, &r) && r);
    CHECK(!call(cpu, "msvcrt.dll", "isalnum", a, 1, &r) && r);
    CHECK(!call(cpu, "msvcrt.dll", "isalpha", a, 1, &r) && !r);
    a[0] = '\t';
    CHECK(!call(cpu, "msvcrt.dll", "isspace", a, 1, &r) && r);
    a[0] = 'g';
    CHECK(!call(cpu, "msvcrt.dll", "isxdigit", a, 1, &r) && !r);
    a[0] = 0xE9;   /* 'e' with an acute accent in CP1252, not a lead byte */
    CHECK(!call(cpu, "msvcrt.dll", "isalpha", a, 1, &r) && !r);
    CHECK(!call(cpu, "msvcrt.dll", "isleadbyte", a, 1, &r) && !r);

    /* Double-returning math: result in ST0, like the d3d9 float shims.  A
     * double occupies two 4-byte cdecl slots, so x/y need four slots. */
    dargs(a, -1.2);
    CHECK(!call(cpu, "msvcrt.dll", "ceil", a, 2, &r) && st0(cpu) == -1.0);
    CHECK(!call(cpu, "msvcrt.dll", "floor", a, 2, &r) && st0(cpu) == -2.0);
    dargs(a, 81.0);
    CHECK(!call(cpu, "msvcrt.dll", "sqrt", a, 2, &r) && st0(cpu) == 9.0);
    dargs(a, 9.0); dargs(a + 2, 4.0);
    CHECK(!call(cpu, "msvcrt.dll", "fmod", a, 4, &r) && st0(cpu) == 1.0);
    dargs(a, 2.0); a[2] = DATA + 0x338; a[3] = 0;
    CHECK(!call(cpu, "msvcrt.dll", "modf", a, 3, &r) && st0(cpu) == 0.0);
    double ipart = 0.0;
    memcpy(&ipart, n3ds_guest_ptr(DATA + 0x338, 8), 8);
    CHECK(ipart == 2.0);

    /* _finite/_isnan and _CI* take/return by value, not by pointer. */
    dargs(a, 1.0);
    CHECK(!call(cpu, "msvcrt.dll", "_finite", a, 2, &r) && r == 1);
    double nanv = nan("");
    dargs(a, nanv);
    CHECK(!call(cpu, "msvcrt.dll", "_isnan", a, 2, &r) && r == 1);
    a[0] = f32(0.5f);
    CHECK(!call(cpu, "msvcrt.dll", "_CIsin", a, 1, &r) && st0(cpu) == sin(0.5));
    CHECK(!call(cpu, "msvcrt.dll", "_CIsqrt", a, 1, &r) && st0(cpu) == sqrt(0.5));
    a[0] = f32(2.0f); a[1] = f32(3.0f);
    CHECK(!call(cpu, "msvcrt.dll", "_CIpow", a, 2, &r) && st0(cpu) == 8.0);

    /* Resolver-level: every name above resolves, so none of them traps. */
    static const char *names[] = {"strtod", "atof", "_itoa", "_ultoa", "_ltoa",
                                  "rand", "srand", "_errno", "_lock", "_unlock",
                                  "isalnum", "isalpha", "isdigit", "isspace",
                                  "isxdigit", "isleadbyte", "iswalpha", "iswdigit",
                                  "iswspace", "iswpunct", "ceil", "floor", "sqrt",
                                  "fmod", "pow", "frexp", "ldexp", "modf", "_finite",
                                  "_isnan", "_CIacos", "_CIasin", "_CIatan", "_CIatan2",
                                  "_CIcos", "_CIexp", "_CIfmod", "_CIlog", "_CIpow",
                                  "_CIsin", "_CIsqrt"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        PeImport e = {.dll = "msvcrt.dll", .name = names[i]};
        CHECK(win32_resolve(&e, NULL) != 0);
    }
    rc = 0;
done:
    free(cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
