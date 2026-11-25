#pragma once
#include <stdint.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* only for WIN32_THUNK_BASE; no shim internals */

/* Single-locale Win32 NLS subset: en-US (LCID 0x0409), ANSI code page 1252.
 *
 * There is no host locale behind these thunks.  en-US is implemented from a
 * fixed table; every other locale, LCTYPE, flag or encoding is either absent
 * from the registry or fails with a Win32 error.  Nothing falls back to the
 * host's locale, code page or time zone.
 *
 * Documented limitations:
 *  - GetUserDefaultLCID/GetSystemDefaultLCID return 0x0409 and
 *    GetUserDefaultLangID/GetSystemDefaultLangID return 0x0409.  They do not
 *    touch last-error (matching the Windows contract).
 *  - IsValidLocale accepts only LCID 0x0409.  Flags 0, LCID_INSTALLED (1),
 *    LCID_SUPPORTED (2) and their union are accepted; any other LCID or flag
 *    bit returns FALSE and sets ERROR_INVALID_PARAMETER (87).
 *  - GetLocaleInfoA/W supports exactly the en-US values listed below.  Only
 *    LCID 0x0409 is accepted.  The only permitted high LCTYPE bit is
 *    LOCALE_RETURN_NUMBER (0x20000000); LOCALE_NOUSEROVERRIDE and
 *    LOCALE_USE_CP_ACP are rejected rather than silently ignored.
 *      LOCALE_SISO639LANGNAME       "en"
 *      LOCALE_SISO3166CTRYNAME      "US"
 *      LOCALE_SNAME                 "en-US"
 *      LOCALE_SENGLANGUAGE          "English"
 *      LOCALE_SENGCOUNTRY           "United States"
 *      LOCALE_SABBREVLANGNAME       "ENU"
 *      LOCALE_SABBREVCTRYNAME       "USA"
 *      LOCALE_IDEFAULTANSICODEPAGE  "1252"
 *      LOCALE_IDEFAULTCODEPAGE      "437"
 *      LOCALE_SDECIMAL              "."
 *      LOCALE_STHOUSAND             ","
 *      LOCALE_SGROUPING             "3;0"
 *      LOCALE_IDIGITS               "2"
 *      LOCALE_ILZERO                "1"
 *      LOCALE_SLIST                 ","
 *      LOCALE_SLONGDATE             "dddd, MMMM d, yyyy"
 *      LOCALE_SSHORTDATE            "M/d/yyyy"
 *      LOCALE_STIMEFORMAT           "h:mm:ss tt"
 *      LOCALE_S1159                 "AM"
 *      LOCALE_S2359                 "PM"
 *    With LOCALE_RETURN_NUMBER, the four numeric types IDEFAULTANSICODEPAGE,
 *    IDEFAULTCODEPAGE, IDIGITS and ILZERO write a DWORD (1252, 437, 2, 1).
 *    The buffer must hold 2 WCHARs (4 bytes) for both the A and W versions;
 *    the return value is 2 in both cases.  RETURN_NUMBER on a string type
 *    fails with 87.
 *    cchData==0 returns the required count including the NUL.  A buffer that
 *    is too small returns 0, sets ERROR_INSUFFICIENT_BUFFER (122) and writes
 *    nothing.  A negative cchData or an unsupported LCID/LCTYPE returns 0 and
 *    sets ERROR_INVALID_PARAMETER (87).  A and W differ only in output
 *    encoding; all supported text is ASCII, so W output is one UTF-16 unit
 *    per character.
 *  - CompareStringA/W accepts LCIDs 0x0409, 0x0400, 0x0800 and 0.  Supported
 *    flags are 0, NORM_IGNORECASE (1) and SORT_STRINGSORT (0x1000); the last
 *    is an accepted no-op.  Other flags return 0 and set 87.  cchCount==-1
 *    includes the NUL terminator in the comparison; cchCount>=0 compares
 *    exactly that many units and permits embedded NULs; cchCount==0 is the
 *    empty string.  Case folding covers ASCII and Latin-1 letters (C0-DE
 *    except D7, E0-FE except F7).
 *    IMPORTANT: flags 0 is an ordinal code-unit comparison, unlike Windows'
 *    linguistic order.  Consequently "a" vs "A" returns CSTR_GREATER_THAN
 *    (3) here, whereas Windows returns CSTR_LESS_THAN (1).  Use
 *    NORM_IGNORECASE for a case-insensitive result.
 *  - GetDateFormatA supports lpFormat==NULL with DATE_SHORTDATE (1) or 0 for
 *    "M/d/yyyy", and DATE_LONGDATE (2) for "dddd, MMMM d, yyyy".  It reads
 *    the guest SYSTEMTIME, uses the supplied wDayOfWeek for the long name and
 *    validates the calendar day.  Custom formats, other flags, a NULL
 *    SYSTEMTIME and a bad LCID fail with 87.
 *  - GetTimeFormatA supports lpFormat==NULL and flags 0 only, producing
 *    "h:mm:ss tt" from the guest SYSTEMTIME.  Custom formats, other flags, a
 *    NULL SYSTEMTIME, an invalid clock field or a bad LCID fail with 87.
 *  - There is no GetDateFormatW/GetTimeFormatW, no locale enumeration and no
 *    calendar beyond the proleptic Gregorian month lengths.
 *  - Last-error is written to the guest TEB at fs_base+0x34 only when fs_base
 *    is nonzero.  There is no host-side fallback.
 *
 * The thunk window is private and disjoint from win32_shims (WIN32_THUNK_BASE
 * + 4*i, small i) and from win32_sync/win32_user.  A dispatcher that resolves
 * an address must chain win32_locale_lookup() after the other lookups. */
#define WIN32_LOCALE_BASE (WIN32_THUNK_BASE + 0x00030000u)

typedef struct { const char *dll, *name; GuestFn fn; } Win32LocaleDef;

/* Thunks are WIN32_LOCALE_BASE + 4*i; every entry lives in kernel32.dll. */
const Win32LocaleDef *win32_locale_defs(unsigned *count);
/* DLL names are case-insensitive and may omit ".dll"; the export name is
 * exact, as in win32_resolve().  Returns 0 when not implemented here. */
uint32_t win32_locale_resolve(const char *dll, const char *name);
/* FnEntry for one of our thunk addresses, or NULL. */
const FnEntry *win32_locale_lookup(uint32_t addr);
