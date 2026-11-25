/* Host smoke test for the minimal shell32 module. */
#include "win32_shell.h"
#include "win32_locale.h"   /* header only: compare thunk windows */
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++fails; } } while (0)

static void w32(GuestMemory *m, uint32_t a, uint32_t v) {
    uint8_t *p = guest_ptr(m, a, 4);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(void) {
    /* Thunk windows must be disjoint. They once collided: shell32 sat on
     * WIN32_LOCALE_BASE, so every shell call was dispatched to a locale shim. */
    CHECK(WIN32_SHELL_BASE != WIN32_LOCALE_BASE);
    CHECK(WIN32_SHELL_BASE != WIN32_THUNK_BASE);
    GuestMemory m;
    guest_init(&m, 0x10000);
    CHECK(!guest_map(&m, 0x10000000, 0x1000));
    CHECK(!guest_map(&m, 0x10002000, 0x1000));
    win32_shell_init(&m);

    CPU c;
    memset(&c, 0, sizeof c);
    c.r[4] = 0x10000f00;

    uint8_t *buf = guest_ptr(&m, 0x10002000, 8);
    CHECK(buf != NULL);
    memset(buf, 0xAA, 8);

    uint32_t fnaddr = win32_shell_resolve("SHELL32.DLL", "SHGetFolderPathA");
    CHECK(fnaddr != 0);
    const FnEntry *e = win32_shell_lookup(fnaddr);
    CHECK(e != NULL && e->fn != NULL);

    /* No configured root: honest E_FAIL, buffer terminated empty. */
    w32(&m, c.r[4] + 4, 0);
    w32(&m, c.r[4] + 8, 0x1a);
    w32(&m, c.r[4] + 12, 0);
    w32(&m, c.r[4] + 16, 0);
    w32(&m, c.r[4] + 20, 0x10002000);
    e->fn(&c);
    CHECK(c.r[0] == 0x80004005u);
    CHECK(buf[0] == 0 && buf[1] == 0xAA);
    CHECK(c.r[4] == 0x10000f00 + 24); /* return address + 5 args */

    /* With a root, known CSIDLs answer with a sandbox-relative path. */
    win32_shell_set_root("sandbox_root");
    memset(buf, 0xAA, 8);
    c.r[4] = 0x10000f00;
    w32(&m, c.r[4] + 4, 0);
    w32(&m, c.r[4] + 8, 0x1c);   /* CSIDL_LOCAL_APPDATA */
    w32(&m, c.r[4] + 12, 0);
    w32(&m, c.r[4] + 16, 0);
    w32(&m, c.r[4] + 20, 0x10002000);
    e->fn(&c);
    CHECK(c.r[0] == 0);
    CHECK(!strcmp((const char *)buf, "LocalAppData"));
    CHECK(c.r[4] == 0x10000f00 + 24); /* return address + 5 args */

    /* Unknown CSIDL is still E_FAIL. */
    memset(buf, 0xAA, 8);
    c.r[4] = 0x10000f00;
    w32(&m, c.r[4] + 8, 0x7f);
    w32(&m, c.r[4] + 20, 0x10002000);
    e->fn(&c);
    CHECK(c.r[0] == 0x80004005u);
    CHECK(buf[0] == 0);

    CHECK(win32_shell_resolve("kernel32.dll", "SHGetFolderPathA") == 0);
    CHECK(win32_shell_resolve("shell32.dll", "Nope") == 0);
    CHECK(win32_shell_lookup(WIN32_SHELL_BASE + 0x1000) == NULL);

    uint32_t se = win32_shell_resolve("shell32.dll", "ShellExecuteA");
    CHECK(se != 0);
    const FnEntry *sefn = win32_shell_lookup(se);
    c.r[4] = 0x10000f00;
    for (unsigned i = 0; i < 6; ++i) w32(&m, c.r[4] + 4 + 4 * i, 0);
    sefn->fn(&c);
    CHECK(c.r[0] == 31u);
    CHECK(c.r[4] == 0x10000f00 + 28); /* return address + 6 args */

    win32_shell_shutdown();
    guest_destroy(&m);
    if (fails) { printf("FAIL: win32_shell smoke (%d)\n", fails); return 1; }
    puts("PASS: win32_shell smoke");
    return 0;
}
