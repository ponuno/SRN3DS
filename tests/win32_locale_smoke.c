/* Host smoke test for source/win32_locale.c.
 *
 * Links guest.c + win32_locale.c only.  n3ds_guest_ptr/rt_unimpl stand in for
 * the runtime so the M8/M16/M32 accessors work without win32_shims.c.  The
 * guest stack, data buffers and TEB are ordinary mapped guest pages. */
#include "win32_locale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define TEB 0x00300000u

#define LCID_EN_US 0x0409u
#define LOCALE_RETURN_NUMBER 0x20000000u

#define LOCALE_IDIGITS 0x00000001u
#define LOCALE_SABBREVLANGNAME 0x00000003u
#define LOCALE_SABBREVCTRYNAME 0x00000007u
#define LOCALE_IDEFAULTCODEPAGE 0x0000000Bu
#define LOCALE_SLIST 0x0000000Cu
#define LOCALE_SDECIMAL 0x0000000Eu
#define LOCALE_STHOUSAND 0x0000000Fu
#define LOCALE_SGROUPING 0x00000010u
#define LOCALE_ILZERO 0x00000012u
#define LOCALE_SSHORTDATE 0x0000001Fu
#define LOCALE_SLONGDATE 0x00000020u
#define LOCALE_S1159 0x00000028u
#define LOCALE_S2359 0x00000029u
#define LOCALE_SISO639LANGNAME 0x00000059u
#define LOCALE_SISO3166CTRYNAME 0x0000005Au
#define LOCALE_SNAME 0x0000005Cu
#define LOCALE_SENGLANGUAGE 0x00001001u
#define LOCALE_SENGCOUNTRY 0x00001002u
#define LOCALE_STIMEFORMAT 0x00001003u
#define LOCALE_IDEFAULTANSICODEPAGE 0x00001004u

#define BUF_A (SCRATCH + 0x0000u)
#define BUF_W (SCRATCH + 0x0200u)
#define NUM_BUF (SCRATCH + 0x0400u)
#define STR_A (SCRATCH + 0x0800u)
#define STR_B (SCRATCH + 0x0a00u)
#define STR_EMBED (SCRATCH + 0x0c00u)
#define DATE_BUF (SCRATCH + 0x1000u)
#define SYSTEMTIME_BUF (SCRATCH + 0x2000u)

static GuestMemory memory;
static CPU cpu;
static unsigned checks, failures;

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "FAIL: unexpected guest fault at %08lx: %s\n",
            (unsigned long)addr, what ? what : "unknown");
    exit(2);
}

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; fprintf(stderr, "FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)

static uint32_t last_error(void) {
    uint32_t v = 0;
    if (guest_read32(&memory, TEB + 0x34, &v)) return 0xffffffffu;
    return v;
}
static void set_last_error(uint32_t v) { M32(TEB + 0x34) = v; }
static void put_str(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    memcpy(n3ds_guest_ptr(addr, n), s, n);
}
static void put_wstr(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    for (size_t i = 0; i < n; ++i) M16(addr + (uint32_t)(i * 2)) = (uint16_t)(unsigned char)s[i];
}
static void fill_bytes(uint32_t addr, unsigned char v, size_t n) {
    memset(n3ds_guest_ptr(addr, n), v, n);
}
static int all_bytes(uint32_t addr, unsigned char v, size_t n) {
    const unsigned char *p = n3ds_guest_ptr(addr, n);
    for (size_t i = 0; i < n; ++i) if (p[i] != v) return 0;
    return 1;
}
static int ascii_eq(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    return memcmp(n3ds_guest_ptr(addr, n), s, n) == 0;
}
static int wide_eq(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    for (size_t i = 0; i < n; ++i)
        if (M16(addr + (uint32_t)(i * 2)) != (uint16_t)(unsigned char)s[i]) return 0;
    return 1;
}

static GuestFn fn_of(const char *name) {
    uint32_t a = win32_locale_resolve("KERNEL32.DLL", name);
    const FnEntry *e = a ? win32_locale_lookup(a) : NULL;
    if (!e || !e->fn) {
        fprintf(stderr, "FAIL: no thunk for %s\n", name);
        exit(2);
    }
    return e->fn;
}
/* push return address + args on the guest stack and call the shim directly */
static uint32_t call(GuestFn fn, const uint32_t *args, unsigned n) {
    uint32_t frame = STACK_TOP - 4 - 4 * n;
    cpu.r[4] = frame;
    M32(frame) = 0;
    for (unsigned i = 0; i < n; ++i) M32(frame + 4 + 4 * i) = args[i];
    cpu.r[0] = 0;
    fn(&cpu);
    if (cpu.r[4] != frame + 4 + 4 * n) {
        fprintf(stderr, "FAIL: shim did not pop %u args (esp 0x%08lx, want 0x%08lx)\n",
            n, (unsigned long)cpu.r[4], (unsigned long)(frame + 4 + 4 * n));
        exit(2);
    }
    return cpu.r[0];
}
static uint32_t get_info(GuestFn fn, uint32_t lcid, uint32_t type,
                         uint32_t dst, int32_t cch) {
    uint32_t args[4] = { lcid, type, dst, (uint32_t)cch };
    return call(fn, args, 4);
}
static uint32_t compare(GuestFn fn, uint32_t lcid, uint32_t flags,
                        uint32_t a, int32_t ca, uint32_t b, int32_t cb) {
    uint32_t args[6] = { lcid, flags, a, (uint32_t)ca, b, (uint32_t)cb };
    return call(fn, args, 6);
}
static uint32_t format_date(GuestFn fn, uint32_t lcid, uint32_t flags,
                            uint32_t date, uint32_t fmt, uint32_t dst, int32_t cch) {
    uint32_t args[6] = { lcid, flags, date, fmt, dst, (uint32_t)cch };
    return call(fn, args, 6);
}

static const struct {
    uint32_t type;
    const char *text;
    uint32_t number;
    int numeric;
} info_cases[] = {
    { LOCALE_SISO639LANGNAME, "en", 0, 0 },
    { LOCALE_SISO3166CTRYNAME, "US", 0, 0 },
    { LOCALE_SNAME, "en-US", 0, 0 },
    { LOCALE_SENGLANGUAGE, "English", 0, 0 },
    { LOCALE_SENGCOUNTRY, "United States", 0, 0 },
    { LOCALE_SABBREVLANGNAME, "ENU", 0, 0 },
    { LOCALE_SABBREVCTRYNAME, "USA", 0, 0 },
    { LOCALE_IDEFAULTANSICODEPAGE, "1252", 1252, 1 },
    { LOCALE_IDEFAULTCODEPAGE, "437", 437, 1 },
    { LOCALE_SDECIMAL, ".", 0, 0 },
    { LOCALE_STHOUSAND, ",", 0, 0 },
    { LOCALE_SGROUPING, "3;0", 0, 0 },
    { LOCALE_IDIGITS, "2", 2, 1 },
    { LOCALE_ILZERO, "1", 1, 1 },
    { LOCALE_SLIST, ",", 0, 0 },
    { LOCALE_SLONGDATE, "dddd, MMMM d, yyyy", 0, 0 },
    { LOCALE_SSHORTDATE, "M/d/yyyy", 0, 0 },
    { LOCALE_STIMEFORMAT, "h:mm:ss tt", 0, 0 },
    { LOCALE_S1159, "AM", 0, 0 },
    { LOCALE_S2359, "PM", 0, 0 }
};

int main(void) {
    /* --- registry: defs, resolve, lookup --- */
    unsigned count = 0;
    const Win32LocaleDef *defs = win32_locale_defs(&count);
    CHECK_EQ(count, 11);
    CHECK(defs != NULL);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t want = WIN32_LOCALE_BASE + 4 * i;
        CHECK(defs[i].dll != NULL && !strcmp(defs[i].dll, "kernel32.dll"));
        CHECK_EQ(win32_locale_resolve(defs[i].dll, defs[i].name), want);
        const FnEntry *e = win32_locale_lookup(want);
        CHECK(e != NULL && e->addr == want && e->fn == defs[i].fn);
    }
    CHECK(win32_locale_resolve("KERNEL32.DLL", "CompareStringA") != 0);
    CHECK(win32_locale_resolve("kernel32", "GetLocaleInfoW") != 0);
    CHECK_EQ(win32_locale_resolve("kernel32.dll", "comparestringa"), 0);
    CHECK_EQ(win32_locale_resolve("user32.dll", "CompareStringA"), 0);
    CHECK_EQ(win32_locale_resolve("kernel32.dll", "NoSuchFunction"), 0);
    CHECK_EQ(win32_locale_resolve(NULL, "IsValidLocale"), 0);
    CHECK_EQ(win32_locale_resolve("kernel32.dll", NULL), 0);
    CHECK_EQ(win32_locale_resolve("kernel32.dll", "GetDateFormatW"), 0);
    CHECK_EQ(win32_locale_resolve("kernel32.dll", "GetTimeFormatW"), 0);
    CHECK(win32_locale_lookup(WIN32_LOCALE_BASE - 4) == NULL);
    CHECK(win32_locale_lookup(WIN32_LOCALE_BASE + 4 * count) == NULL);
    CHECK(win32_locale_lookup(WIN32_LOCALE_BASE + 2) == NULL);

    /* --- guest memory: stack, scratch and TEB pages --- */
    guest_init(&memory, 0x20000);
    CHECK_EQ(guest_map(&memory, STACK_BASE, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, SCRATCH, 0x10000), 0);
    CHECK_EQ(guest_map(&memory, TEB, 0x1000), 0);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;
    cpu.fs_base = TEB;
    set_last_error(0xdeadbeefu);

    GuestFn get_user_lcid = fn_of("GetUserDefaultLCID");
    GuestFn get_system_lcid = fn_of("GetSystemDefaultLCID");
    GuestFn get_user_lang = fn_of("GetUserDefaultLangID");
    GuestFn get_system_lang = fn_of("GetSystemDefaultLangID");
    GuestFn is_valid = fn_of("IsValidLocale");
    GuestFn info_a = fn_of("GetLocaleInfoA"), info_w = fn_of("GetLocaleInfoW");
    GuestFn cmp_a = fn_of("CompareStringA"), cmp_w = fn_of("CompareStringW");
    GuestFn date_a = fn_of("GetDateFormatA"), time_a = fn_of("GetTimeFormatA");

    /* --- default LCID/LANGID getters; they leave last-error alone --- */
    set_last_error(0x12345678u);
    CHECK_EQ(call(get_user_lcid, NULL, 0), 0x0409);
    CHECK_EQ(call(get_system_lcid, NULL, 0), 0x0409);
    CHECK_EQ(call(get_user_lang, NULL, 0), 0x0409);
    CHECK_EQ(call(get_system_lang, NULL, 0), 0x0409);
    CHECK_EQ(last_error(), 0x12345678u);
    set_last_error(0xdeadbeefu);

    /* --- IsValidLocale: only 0x0409, flags 0/1/2/3 --- */
    uint32_t args[6];
    args[0] = LCID_EN_US; args[1] = 0;
    CHECK_EQ(call(is_valid, args, 2), 1);
    args[0] = LCID_EN_US; args[1] = 1; CHECK_EQ(call(is_valid, args, 2), 1);
    args[0] = LCID_EN_US; args[1] = 2; CHECK_EQ(call(is_valid, args, 2), 1);
    args[0] = LCID_EN_US; args[1] = 3; CHECK_EQ(call(is_valid, args, 2), 1);
    set_last_error(0xdeadbeefu);
    args[0] = LCID_EN_US; args[1] = 4;
    CHECK_EQ(call(is_valid, args, 2), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    args[0] = 0x0407; args[1] = 0;
    CHECK_EQ(call(is_valid, args, 2), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    args[0] = 0; args[1] = 2;
    CHECK_EQ(call(is_valid, args, 2), 0);
    CHECK_EQ(last_error(), 87);

    /* --- GetLocaleInfoA/W: every documented string --- */
    for (unsigned i = 0; i < sizeof info_cases / sizeof info_cases[0]; ++i) {
        uint32_t need = (uint32_t)strlen(info_cases[i].text) + 1;
        CHECK_EQ(get_info(info_a, LCID_EN_US, info_cases[i].type, 0, 0), need);
        CHECK_EQ(get_info(info_w, LCID_EN_US, info_cases[i].type, 0, 0), need);

        fill_bytes(BUF_A, 0xAA, 256);
        CHECK_EQ(get_info(info_a, LCID_EN_US, info_cases[i].type, BUF_A, 256), need);
        CHECK(ascii_eq(BUF_A, info_cases[i].text));

        fill_bytes(BUF_W, 0xAA, 256);
        CHECK_EQ(get_info(info_w, LCID_EN_US, info_cases[i].type, BUF_W, 128), need);
        CHECK(wide_eq(BUF_W, info_cases[i].text));

        /* one unit too small: error 122, no write */
        set_last_error(0xdeadbeefu);
        fill_bytes(BUF_A, 0xAA, 256);
        CHECK_EQ(get_info(info_a, LCID_EN_US, info_cases[i].type, BUF_A, (int32_t)need - 1), 0);
        CHECK_EQ(last_error(), 122);
        CHECK(all_bytes(BUF_A, 0xAA, 256));
        set_last_error(0xdeadbeefu);
        fill_bytes(BUF_W, 0xAA, 256);
        CHECK_EQ(get_info(info_w, LCID_EN_US, info_cases[i].type, BUF_W, (int32_t)need - 1), 0);
        CHECK_EQ(last_error(), 122);
        CHECK(all_bytes(BUF_W, 0xAA, 256));

        /* negative count: error 87, no write */
        set_last_error(0xdeadbeefu);
        fill_bytes(BUF_A, 0xAA, 256);
        CHECK_EQ(get_info(info_a, LCID_EN_US, info_cases[i].type, BUF_A, -1), 0);
        CHECK_EQ(last_error(), 87);
        CHECK(all_bytes(BUF_A, 0xAA, 256));
    }

    /* unsupported LCID / LCTYPE / LCTYPE flags */
    set_last_error(0xdeadbeefu);
    CHECK_EQ(get_info(info_a, 0x0407, LOCALE_SNAME, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(get_info(info_a, LCID_EN_US, 0x1234, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(get_info(info_w, LCID_EN_US, LOCALE_SNAME | 0x40000000u, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(get_info(info_a, LCID_EN_US, LOCALE_SNAME | LOCALE_RETURN_NUMBER, 0, 0), 0);
    CHECK_EQ(last_error(), 87);

    /* --- LOCALE_RETURN_NUMBER: 2 WCHARs / 4 bytes, return 2 --- */
    for (unsigned i = 0; i < sizeof info_cases / sizeof info_cases[0]; ++i) {
        if (!info_cases[i].numeric) continue;
        uint32_t type = info_cases[i].type | LOCALE_RETURN_NUMBER;
        uint32_t value = info_cases[i].number;

        CHECK_EQ(get_info(info_a, LCID_EN_US, type, 0, 0), 2);
        CHECK_EQ(get_info(info_w, LCID_EN_US, type, 0, 0), 2);

        set_last_error(0xdeadbeefu);
        fill_bytes(NUM_BUF, 0xAA, 16);
        CHECK_EQ(get_info(info_a, LCID_EN_US, type, NUM_BUF, 3), 0);
        CHECK_EQ(last_error(), 122);
        CHECK(all_bytes(NUM_BUF, 0xAA, 16));

        fill_bytes(NUM_BUF, 0xAA, 16);
        CHECK_EQ(get_info(info_a, LCID_EN_US, type, NUM_BUF, 4), 2);
        CHECK_EQ(M32(NUM_BUF), value);
        CHECK(all_bytes(NUM_BUF + 4, 0xAA, 12));

        set_last_error(0xdeadbeefu);
        fill_bytes(NUM_BUF, 0xAA, 16);
        CHECK_EQ(get_info(info_w, LCID_EN_US, type, NUM_BUF, 1), 0);
        CHECK_EQ(last_error(), 122);
        CHECK(all_bytes(NUM_BUF, 0xAA, 16));

        fill_bytes(NUM_BUF, 0xAA, 16);
        CHECK_EQ(get_info(info_w, LCID_EN_US, type, NUM_BUF, 2), 2);
        CHECK_EQ(M32(NUM_BUF), value);
        CHECK(all_bytes(NUM_BUF + 4, 0xAA, 12));

        set_last_error(0xdeadbeefu);
        CHECK_EQ(get_info(info_a, LCID_EN_US, type, NUM_BUF, -1), 0);
        CHECK_EQ(last_error(), 87);
    }

    /* --- CompareStringA --- */
    put_str(STR_A, "abc");
    put_str(STR_B, "abc");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 2);
    put_str(STR_B, "abd");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 1);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_B, -1, STR_A, -1), 3);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, 3, STR_B, 3), 1);

    /* documented difference from Windows: flags 0 is ordinal */
    put_str(STR_A, "a");
    put_str(STR_B, "A");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 3);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_B, -1, STR_A, -1), 1);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 1, STR_A, -1, STR_B, -1), 2);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0x1000, STR_A, -1, STR_B, -1), 3);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0x1001, STR_A, -1, STR_B, -1), 2);

    /* LCID aliases and case folding */
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t lcid = (uint32_t[]){ LCID_EN_US, 0x0400u, 0x0800u, 0u }[i];
        CHECK_EQ(compare(cmp_a, lcid, 0, STR_A, -1, STR_A, -1), 2);
    }
    put_str(STR_A, "abc");
    put_str(STR_B, "ABD");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 1, STR_A, -1, STR_B, -1), 1);
    put_str(STR_A, "ABC");
    put_str(STR_B, "abd");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 1, STR_A, -1, STR_B, -1), 1);
    {
        uint8_t *p = n3ds_guest_ptr(STR_A, 2);
        uint8_t *q = n3ds_guest_ptr(STR_B, 2);
        p[0] = 0xc9; p[1] = 0; /* E-acute upper */
        q[0] = 0xe9; q[1] = 0; /* e-acute lower */
        CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 1);
        CHECK_EQ(compare(cmp_a, LCID_EN_US, 1, STR_A, -1, STR_B, -1), 2);
    }

    /* explicit lengths with an embedded NUL */
    fill_bytes(STR_EMBED, 0, 16);
    {
        const unsigned char x[4] = { 'a', 'b', 0, 'c' };
        const unsigned char y[4] = { 'a', 'b', 0, 'd' };
        memcpy(n3ds_guest_ptr(STR_EMBED, 4), x, 4);
        memcpy(n3ds_guest_ptr(STR_EMBED + 8, 4), y, 4);
    }
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_EMBED, 4, STR_EMBED + 8, 4), 1);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_EMBED + 8, 4, STR_EMBED, 4), 3);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_EMBED, 4, STR_EMBED, 4), 2);

    /* cch==0 is the empty string; -1 includes the terminator */
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, 0, 0, 0, 0), 2);
    put_str(STR_A, "a");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, 0, 0, STR_A, -1), 1);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, 0, 0), 3);
    put_str(STR_B, "ab");
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 1);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_B, -1, STR_A, -1), 3);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, STR_A, 1, STR_B, 2), 1);

    /* bad flags / LCID fail with 87 and no comparison */
    set_last_error(0xdeadbeefu);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 2, STR_A, -1, STR_A, -1), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0x10000, STR_A, -1, STR_A, -1), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(compare(cmp_a, 0x0407, 0, STR_A, -1, STR_A, -1), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(compare(cmp_a, LCID_EN_US, 0, 0, -1, STR_A, -1), 0);
    CHECK_EQ(last_error(), 87);

    /* --- CompareStringW --- */
    put_wstr(STR_A, "a");
    put_wstr(STR_B, "A");
    CHECK_EQ(compare(cmp_w, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 3);
    CHECK_EQ(compare(cmp_w, LCID_EN_US, 1, STR_A, -1, STR_B, -1), 2);
    put_wstr(STR_A, "abc");
    put_wstr(STR_B, "abd");
    CHECK_EQ(compare(cmp_w, LCID_EN_US, 0, STR_A, -1, STR_B, -1), 1);
    CHECK_EQ(compare(cmp_w, 0x0800, 0, STR_B, -1, STR_A, -1), 3);
    for (size_t i = 0; i < 4; ++i) {
        M16(STR_EMBED + (uint32_t)(i * 2)) = (uint16_t)"ab\0c"[i];
        M16(STR_EMBED + 8 + (uint32_t)(i * 2)) = (uint16_t)"ab\0d"[i];
    }
    CHECK_EQ(compare(cmp_w, LCID_EN_US, 0, STR_EMBED, 4, STR_EMBED + 8, 4), 1);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(compare(cmp_w, LCID_EN_US, 2, STR_A, -1, STR_B, -1), 0);
    CHECK_EQ(last_error(), 87);

    /* --- optional GetDateFormatA --- */
    memset(n3ds_guest_ptr(SYSTEMTIME_BUF, 16), 0, 16);
    M16(SYSTEMTIME_BUF + 0) = 2024;  /* wYear */
    M16(SYSTEMTIME_BUF + 2) = 3;     /* wMonth */
    M16(SYSTEMTIME_BUF + 4) = 2;     /* wDayOfWeek: Tuesday */
    M16(SYSTEMTIME_BUF + 6) = 5;     /* wDay */
    M16(SYSTEMTIME_BUF + 8) = 14;    /* wHour */
    M16(SYSTEMTIME_BUF + 10) = 7;    /* wMinute */
    M16(SYSTEMTIME_BUF + 12) = 9;    /* wSecond */

    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, 0, 0), 9);
    fill_bytes(DATE_BUF, 0xAA, 128);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, DATE_BUF, 128), 9);
    CHECK(ascii_eq(DATE_BUF, "3/5/2024"));
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, DATE_BUF, 128), 9);
    CHECK(ascii_eq(DATE_BUF, "3/5/2024"));
    uint32_t long_need = (uint32_t)strlen("Tuesday, March 5, 2024") + 1;
    CHECK_EQ(format_date(date_a, LCID_EN_US, 2, SYSTEMTIME_BUF, 0, 0, 0), long_need);
    fill_bytes(DATE_BUF, 0xAA, 128);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 2, SYSTEMTIME_BUF, 0, DATE_BUF, 128), long_need);
    CHECK(ascii_eq(DATE_BUF, "Tuesday, March 5, 2024"));

    set_last_error(0xdeadbeefu);
    fill_bytes(DATE_BUF, 0xAA, 128);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, DATE_BUF, 8), 0);
    CHECK_EQ(last_error(), 122);
    CHECK(all_bytes(DATE_BUF, 0xAA, 128));
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 4, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, STR_A, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, 0, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, 0x0407, 1, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);

    M16(SYSTEMTIME_BUF + 2) = 13; /* bad month */
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    M16(SYSTEMTIME_BUF + 2) = 2;  /* 2023-02-30 is not a date */
    M16(SYSTEMTIME_BUF + 0) = 2023;
    M16(SYSTEMTIME_BUF + 6) = 30;
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    M16(SYSTEMTIME_BUF + 0) = 2024;
    M16(SYSTEMTIME_BUF + 2) = 3;
    M16(SYSTEMTIME_BUF + 6) = 5;
    M16(SYSTEMTIME_BUF + 4) = 7;  /* no long weekday name */
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(date_a, LCID_EN_US, 2, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    M16(SYSTEMTIME_BUF + 4) = 2;

    /* --- optional GetTimeFormatA --- */
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, 0, 0), 11);
    fill_bytes(DATE_BUF, 0xAA, 128);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, DATE_BUF, 128), 11);
    CHECK(ascii_eq(DATE_BUF, "2:07:09 PM"));
    set_last_error(0xdeadbeefu);
    fill_bytes(DATE_BUF, 0xAA, 128);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, DATE_BUF, 10), 0);
    CHECK_EQ(last_error(), 122);
    CHECK(all_bytes(DATE_BUF, 0xAA, 128));

    M16(SYSTEMTIME_BUF + 8) = 0;
    M16(SYSTEMTIME_BUF + 10) = 5;
    M16(SYSTEMTIME_BUF + 12) = 0;
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, 0, 0), 12);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, DATE_BUF, 128), 12);
    CHECK(ascii_eq(DATE_BUF, "12:05:00 AM"));
    M16(SYSTEMTIME_BUF + 8) = 12;
    M16(SYSTEMTIME_BUF + 10) = 0;
    M16(SYSTEMTIME_BUF + 12) = 0;
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, DATE_BUF, 128), 12);
    CHECK(ascii_eq(DATE_BUF, "12:00:00 PM"));

    M16(SYSTEMTIME_BUF + 8) = 24;
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    M16(SYSTEMTIME_BUF + 8) = 14;
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 1, SYSTEMTIME_BUF, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, SYSTEMTIME_BUF, STR_A, 0, 0), 0);
    CHECK_EQ(last_error(), 87);
    set_last_error(0xdeadbeefu);
    CHECK_EQ(format_date(time_a, LCID_EN_US, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(last_error(), 87);

    guest_destroy(&memory);
    if (failures) {
        fprintf(stderr, "FAIL: win32_locale smoke (%u checks, %u failures)\n", checks, failures);
        return 1;
    }
    printf("PASS: win32_locale smoke (%u checks)\n", checks);
    return 0;
}
