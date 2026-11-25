/* Single-locale (en-US) Win32 NLS shims.
 *
 * Only LCID 0x0409 and ANSI 1252 exist.  Locale data is a fixed table, so
 * every other locale, LCTYPE or flag is an honest error instead of a host
 * locale lookup.  Comparison is ordinal, not Windows-linguistic; see
 * win32_locale.h for the exact contract and the "a" vs "A" difference. */
#include "win32_locale.h"
#include <stdio.h>
#include <string.h>

#define LCID_EN_US 0x0409u
#define LCID_SYSTEM_DEFAULT 0x0400u
#define LCID_USER_DEFAULT 0x0800u

#define LOCALE_RETURN_NUMBER 0x20000000u
#define LOCALE_INSTALLED 0x00000001u
#define LOCALE_SUPPORTED 0x00000002u

#define NORM_IGNORECASE 0x00000001u
#define SORT_STRINGSORT 0x00001000u

#define CSTR_LESS_THAN 1u
#define CSTR_EQUAL 2u
#define CSTR_GREATER_THAN 3u

#define DATE_SHORTDATE 0x00000001u
#define DATE_LONGDATE 0x00000002u

#define ERR_INVALID_PARAMETER 87u
#define ERR_INSUFFICIENT_BUFFER 122u

/* Stop scanning an unterminated guest string instead of running away. */
#define MAX_LOCALE_STRING 0x100000u

/* LCTYPE values from winnls.h. */
enum {
    LOCALE_IDIGITS = 0x00000001,
    LOCALE_SABBREVLANGNAME = 0x00000003,
    LOCALE_SABBREVCTRYNAME = 0x00000007,
    LOCALE_IDEFAULTCODEPAGE = 0x0000000B,
    LOCALE_SLIST = 0x0000000C,
    LOCALE_SDECIMAL = 0x0000000E,
    LOCALE_STHOUSAND = 0x0000000F,
    LOCALE_SGROUPING = 0x00000010,
    LOCALE_ILZERO = 0x00000012,
    LOCALE_SSHORTDATE = 0x0000001F,
    LOCALE_SLONGDATE = 0x00000020,
    LOCALE_S1159 = 0x00000028,
    LOCALE_S2359 = 0x00000029,
    LOCALE_SISO639LANGNAME = 0x00000059,
    LOCALE_SISO3166CTRYNAME = 0x0000005A,
    LOCALE_SNAME = 0x0000005C,
    LOCALE_SENGLANGUAGE = 0x00001001,
    LOCALE_SENGCOUNTRY = 0x00001002,
    LOCALE_STIMEFORMAT = 0x00001003,
    LOCALE_IDEFAULTANSICODEPAGE = 0x00001004
};

typedef struct {
    uint32_t type;
    const char *text;   /* form used without LOCALE_RETURN_NUMBER */
    uint32_t number;    /* form used with LOCALE_RETURN_NUMBER */
    uint8_t numeric;
} LocaleInfo;

static const LocaleInfo locale_info[] = {
    { LOCALE_IDIGITS, "2", 2, 1 },
    { LOCALE_SABBREVLANGNAME, "ENU", 0, 0 },
    { LOCALE_SABBREVCTRYNAME, "USA", 0, 0 },
    { LOCALE_IDEFAULTCODEPAGE, "437", 437, 1 },
    { LOCALE_SLIST, ",", 0, 0 },
    { LOCALE_SDECIMAL, ".", 0, 0 },
    { LOCALE_STHOUSAND, ",", 0, 0 },
    { LOCALE_SGROUPING, "3;0", 0, 0 },
    { LOCALE_ILZERO, "1", 1, 1 },
    { LOCALE_SSHORTDATE, "M/d/yyyy", 0, 0 },
    { LOCALE_SLONGDATE, "dddd, MMMM d, yyyy", 0, 0 },
    { LOCALE_S1159, "AM", 0, 0 },
    { LOCALE_S2359, "PM", 0, 0 },
    { LOCALE_SISO639LANGNAME, "en", 0, 0 },
    { LOCALE_SISO3166CTRYNAME, "US", 0, 0 },
    { LOCALE_SNAME, "en-US", 0, 0 },
    { LOCALE_SENGLANGUAGE, "English", 0, 0 },
    { LOCALE_SENGCOUNTRY, "United States", 0, 0 },
    { LOCALE_STIMEFORMAT, "h:mm:ss tt", 0, 0 },
    { LOCALE_IDEFAULTANSICODEPAGE, "1252", 1252, 1 }
};

static const char *const weekday_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void set_error(CPU *c, uint32_t err) {
    if (c->fs_base) M32(c->fs_base + 0x34) = err;
}
static int fail(CPU *c, uint32_t err) {
    set_error(c, err);
    return 0;
}
static void *checked(CPU *c, uint32_t addr, uint32_t bytes) {
    void *p = n3ds_guest_ptr(addr, bytes);
    if (!p) rt_unimpl(c, addr, "invalid locale shim memory range");
    return p;
}

#define A(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)
#define FAIL(err, v, n) do { set_error(c, (err)); RET(v, n); } while (0)

static const LocaleInfo *info_for(uint32_t type) {
    for (unsigned i = 0; i < sizeof locale_info / sizeof locale_info[0]; ++i)
        if (locale_info[i].type == type) return &locale_info[i];
    return NULL;
}

/* ---- output helpers: return 1 and *rc on success, 0 after setting error ---- */

static int out_ascii(CPU *c, uint32_t dst, int32_t cch, const char *s, uint32_t *rc) {
    size_t need = strlen(s) + 1;
    if (cch < 0) return fail(c, ERR_INVALID_PARAMETER);
    if (cch == 0) { *rc = (uint32_t)need; return 1; }
    if ((size_t)cch < need) return fail(c, ERR_INSUFFICIENT_BUFFER);
    memcpy(checked(c, dst, (uint32_t)need), s, need);
    *rc = (uint32_t)need;
    return 1;
}
static int out_wide(CPU *c, uint32_t dst, int32_t cch, const char *s, uint32_t *rc) {
    size_t need = strlen(s) + 1;
    if (cch < 0) return fail(c, ERR_INVALID_PARAMETER);
    if (cch == 0) { *rc = (uint32_t)need; return 1; }
    if ((size_t)cch < need) return fail(c, ERR_INSUFFICIENT_BUFFER);
    checked(c, dst, (uint32_t)(need * 2));
    for (size_t i = 0; i < need; ++i)
        M16(dst + (uint32_t)(i * 2)) = (uint16_t)(unsigned char)s[i];
    *rc = (uint32_t)need;
    return 1;
}
/* RETURN_NUMBER writes one DWORD.  The A-form count is bytes, the W-form
 * count is WCHARs, but both require 4 bytes and report 2 as Windows does. */
static int out_number(CPU *c, uint32_t dst, int32_t cch, int wide,
                      uint32_t value, uint32_t *rc) {
    uint32_t need = wide ? 2u : 4u;
    if (cch < 0) return fail(c, ERR_INVALID_PARAMETER);
    if (cch == 0) { *rc = 2; return 1; }
    if ((uint32_t)cch < need) return fail(c, ERR_INSUFFICIENT_BUFFER);
    checked(c, dst, 4);
    M32(dst) = value;
    *rc = 2;
    return 1;
}

static int locale_info_impl(CPU *c, int wide, uint32_t *rc) {
    uint32_t lcid = A(0), lctype = A(1), dst = A(2);
    int32_t cch = (int32_t)A(3);
    if (lcid != LCID_EN_US) return fail(c, ERR_INVALID_PARAMETER);
    int numeric = (lctype & LOCALE_RETURN_NUMBER) != 0;
    uint32_t type = lctype & 0xffffu;
    if (lctype & ~((uint32_t)0xffffu | LOCALE_RETURN_NUMBER))
        return fail(c, ERR_INVALID_PARAMETER);
    const LocaleInfo *li = info_for(type);
    if (!li) return fail(c, ERR_INVALID_PARAMETER);
    if (numeric) {
        if (!li->numeric) return fail(c, ERR_INVALID_PARAMETER);
        return out_number(c, dst, cch, wide, li->number, rc);
    }
    return wide ? out_wide(c, dst, cch, li->text, rc)
                : out_ascii(c, dst, cch, li->text, rc);
}

static void s_GetUserDefaultLCID(CPU *c) { RET(LCID_EN_US, 0); }
static void s_GetSystemDefaultLCID(CPU *c) { RET(LCID_EN_US, 0); }
static void s_GetUserDefaultLangID(CPU *c) { RET(LCID_EN_US, 0); }
static void s_GetSystemDefaultLangID(CPU *c) { RET(LCID_EN_US, 0); }

static void s_IsValidLocale(CPU *c) {
    uint32_t lcid = A(0), flags = A(1);
    if (flags & ~(LOCALE_INSTALLED | LOCALE_SUPPORTED))
        FAIL(ERR_INVALID_PARAMETER, 0, 2);
    if (lcid != LCID_EN_US) FAIL(ERR_INVALID_PARAMETER, 0, 2);
    RET(1, 2);
}

static void s_GetLocaleInfoA(CPU *c) {
    uint32_t rc = 0;
    if (!locale_info_impl(c, 0, &rc)) RET(0, 4);
    RET(rc, 4);
}
static void s_GetLocaleInfoW(CPU *c) {
    uint32_t rc = 0;
    if (!locale_info_impl(c, 1, &rc)) RET(0, 4);
    RET(rc, 4);
}

/* ---- CompareStringA/W ---- */

static uint32_t fold_latin1(uint32_t u) {
    if (u >= 'a' && u <= 'z') return u - 32;
    if (u >= 0xe0 && u <= 0xfe && u != 0xf7) return u - 32;
    return u;
}
static size_t scan_a(CPU *c, uint32_t addr) {
    size_t n = 0;
    while (n < MAX_LOCALE_STRING && M8(addr + (uint32_t)n)) ++n;
    if (n >= MAX_LOCALE_STRING) rt_unimpl(c, addr, "locale string is not NUL-terminated");
    return n + 1; /* cchCount -1 includes the terminator */
}
static size_t scan_w(CPU *c, uint32_t addr) {
    size_t n = 0;
    while (n < MAX_LOCALE_STRING && M16(addr + (uint32_t)(n * 2))) ++n;
    if (n >= MAX_LOCALE_STRING) rt_unimpl(c, addr, "locale string is not NUL-terminated");
    return n + 1;
}
static int arg_ok(uint32_t addr, int32_t cch) { return addr != 0 || cch == 0; }

static int compare_impl(CPU *c, uint32_t a, int32_t ca, uint32_t b, int32_t cb,
                        int wide, uint32_t flags) {
    size_t na = ca < 0 ? (wide ? scan_w(c, a) : scan_a(c, a)) : (size_t)ca;
    size_t nb = cb < 0 ? (wide ? scan_w(c, b) : scan_a(c, b)) : (size_t)cb;
    const uint8_t *pa = na ? checked(c, a, wide ? (uint32_t)(na * 2) : (uint32_t)na) : NULL;
    const uint8_t *pb = nb ? checked(c, b, wide ? (uint32_t)(nb * 2) : (uint32_t)nb) : NULL;
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; ++i) {
        uint32_t ua = wide ? ((const u16u *)pa)[i] : pa[i];
        uint32_t ub = wide ? ((const u16u *)pb)[i] : pb[i];
        if (flags & NORM_IGNORECASE) { ua = fold_latin1(ua); ub = fold_latin1(ub); }
        if (ua < ub) return CSTR_LESS_THAN;
        if (ua > ub) return CSTR_GREATER_THAN;
    }
    if (na < nb) return CSTR_LESS_THAN;
    if (na > nb) return CSTR_GREATER_THAN;
    return CSTR_EQUAL;
}
static int compare_lcid_ok(uint32_t lcid) {
    return lcid == LCID_EN_US || lcid == LCID_SYSTEM_DEFAULT ||
           lcid == LCID_USER_DEFAULT || lcid == 0;
}
static int compare_flags_ok(uint32_t flags) {
    return (flags & ~(NORM_IGNORECASE | SORT_STRINGSORT)) == 0;
}

static void s_CompareStringA(CPU *c) {
    uint32_t lcid = A(0), flags = A(1), a = A(2);
    int32_t ca = (int32_t)A(3);
    uint32_t b = A(4);
    int32_t cb = (int32_t)A(5);
    if (!compare_lcid_ok(lcid) || !compare_flags_ok(flags) ||
        !arg_ok(a, ca) || !arg_ok(b, cb))
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    RET(compare_impl(c, a, ca, b, cb, 0, flags), 6);
}
static void s_CompareStringW(CPU *c) {
    uint32_t lcid = A(0), flags = A(1), a = A(2);
    int32_t ca = (int32_t)A(3);
    uint32_t b = A(4);
    int32_t cb = (int32_t)A(5);
    if (!compare_lcid_ok(lcid) || !compare_flags_ok(flags) ||
        !arg_ok(a, ca) || !arg_ok(b, cb))
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    RET(compare_impl(c, a, ca, b, cb, 1, flags), 6);
}

/* ---- optional date/time formatting (A only, documented flags) ---- */

static unsigned days_in_month(unsigned year, unsigned month) {
    static const uint8_t days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return days[month - 1];
}

static void s_GetDateFormatA(CPU *c) {
    uint32_t lcid = A(0), flags = A(1), date = A(2), format = A(3), dst = A(4);
    int32_t cch = (int32_t)A(5);
    if (lcid != LCID_EN_US) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (format != 0) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (flags == 0) flags = DATE_SHORTDATE;
    if (flags != DATE_SHORTDATE && flags != DATE_LONGDATE)
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (!date) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    checked(c, date, 16);
    unsigned year = M16(date), month = M16(date + 2);
    unsigned dow = M16(date + 4), day = M16(date + 6);
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month))
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    char buf[128];
    if (flags == DATE_SHORTDATE) {
        snprintf(buf, sizeof buf, "%u/%u/%04u", month, day, year);
    } else {
        if (dow > 6) FAIL(ERR_INVALID_PARAMETER, 0, 6);
        snprintf(buf, sizeof buf, "%s, %s %u, %04u",
                 weekday_names[dow], month_names[month - 1], day, year);
    }
    uint32_t rc = 0;
    if (!out_ascii(c, dst, cch, buf, &rc)) RET(0, 6);
    RET(rc, 6);
}

static void s_GetTimeFormatA(CPU *c) {
    uint32_t lcid = A(0), flags = A(1), date = A(2), format = A(3), dst = A(4);
    int32_t cch = (int32_t)A(5);
    if (lcid != LCID_EN_US || flags != 0 || format != 0 || !date)
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    checked(c, date, 16);
    unsigned hour = M16(date + 8), minute = M16(date + 10), second = M16(date + 12);
    if (hour > 23 || minute > 59 || second > 59)
        FAIL(ERR_INVALID_PARAMETER, 0, 6);
    unsigned hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;
    char buf[64];
    snprintf(buf, sizeof buf, "%u:%02u:%02u %s",
             hour12, minute, second, hour < 12 ? "AM" : "PM");
    uint32_t rc = 0;
    if (!out_ascii(c, dst, cch, buf, &rc)) RET(0, 6);
    RET(rc, 6);
}

/* ---- registry ---- */

#define DEF(X) { "kernel32.dll", #X, s_##X },
static const Win32LocaleDef defs[] = {
    DEF(GetUserDefaultLCID) DEF(GetSystemDefaultLCID)
    DEF(GetUserDefaultLangID) DEF(GetSystemDefaultLangID)
    DEF(IsValidLocale)
    DEF(GetLocaleInfoA) DEF(GetLocaleInfoW)
    DEF(CompareStringA) DEF(CompareStringW)
    DEF(GetDateFormatA) DEF(GetTimeFormatA)
};
#undef DEF
#define NDEFS (sizeof defs / sizeof defs[0])

static unsigned lower(unsigned ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int icmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { ++a; ++b; }
    return (int)lower((unsigned char)*a) - (int)lower((unsigned char)*b);
}
static int icmp_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}
/* "kernel32.dll" and "kernel32" name the same module. */
static int dll_match(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (!icmp(a, b)) return 1;
    size_t n = strlen(b);
    return n > 4 && strlen(a) == n - 4 && !icmp(b + n - 4, ".dll") &&
           icmp_prefix(a, b, n - 4);
}

const Win32LocaleDef *win32_locale_defs(unsigned *count) {
    if (count) *count = (unsigned)NDEFS;
    return defs;
}
uint32_t win32_locale_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (dll_match(dll, defs[i].dll) && !strcmp(name, defs[i].name))
            return WIN32_LOCALE_BASE + 4 * i;
    return 0;
}
static FnEntry entries[NDEFS];
const FnEntry *win32_locale_lookup(uint32_t addr) {
    if (addr < WIN32_LOCALE_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_LOCALE_BASE) / 4;
    if (i >= NDEFS) return NULL;
    entries[i] = (FnEntry){ addr, defs[i].fn };
    return &entries[i];
}
