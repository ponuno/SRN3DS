/* Host smoke test for source/win32_sync.c.
 *
 * Links guest.c + win32_sync.c only. The two runtime hooks below stand in for
 * n3ds_lift_runtime.c so M32()/rt_unimpl() work without win32_shims.c; the
 * guest stack and object names are ordinary mapped guest pages. */
#include "win32_sync.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BASE 0x00100000u
#define STACK_TOP (STACK_BASE + 0x1000u)
#define SCRATCH 0x00200000u
#define TEB 0x00300000u

static GuestMemory memory;
static CPU cpu;
static unsigned checks, failures;
static jmp_buf trap;
static int trap_armed;
static char trap_text[160];

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&memory, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    (void)addr;
    snprintf(trap_text, sizeof trap_text, "%s", what ? what : "unknown");
    if (!trap_armed) {
        fprintf(stderr, "FAIL: unexpected guest fault: %s\n", trap_text);
        exit(2);
    }
    longjmp(trap, 1);
}

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(got,want) do { ++checks; uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { ++failures; fprintf(stderr, "FAIL line %d: %s = 0x%08lx, want 0x%08lx\n", \
        __LINE__, #got, (unsigned long)g_, (unsigned long)w_); } } while (0)

static uint32_t last_error(void) {
    uint32_t v = 0xdeadbeefu;
    if (guest_read32(&memory, TEB + 0x34, &v)) return 0xffffffffu;
    return v;
}
static uint32_t put_name(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    memcpy(n3ds_guest_ptr(addr, n), s, n);
    return addr;
}
static GuestFn fn_of(const char *name) {
    /* resolve is module-name case-insensitive; export names are exact. */
    uint32_t a = win32_sync_resolve("KERNEL32.DLL", name);
    const FnEntry *e = a ? win32_sync_lookup(a) : NULL;
    if (!e || !e->fn) {
        fprintf(stderr, "FAIL: no thunk for %s\n", name);
        exit(2);
    }
    return e->fn;
}
/* Blocker stub for the hand-off test: report "a handle may be ready" a fixed
 * number of times, then time out.  This models the scheduler releasing a waiter
 * that must still re-check (and consume) the object itself. */
static int fake_wakes_left;
static int fake_block(const uint32_t *handles, unsigned count, int all, uint32_t ms) {
    (void)handles; (void)count; (void)all; (void)ms;
    if (fake_wakes_left) { --fake_wakes_left; return 0; }
    return 1;
}

/* push return address + args on the guest stack and call the shim directly */static uint32_t call(GuestFn fn, const uint32_t *args, unsigned n) {
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

int main(void) {
    uint32_t args[8];

    /* --- registry: defs, resolve, lookup --- */
    unsigned count = 0;
    const Win32SyncDef *defs = win32_sync_defs(&count);
    CHECK_EQ(count, 12);
    CHECK(defs != NULL);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t want = WIN32_SYNC_BASE + 4 * i;
        CHECK_EQ(win32_sync_resolve(defs[i].dll, defs[i].name), want);
        const FnEntry *e = win32_sync_lookup(want);
        CHECK(e != NULL && e->addr == want && e->fn == defs[i].fn);
    }
    CHECK(win32_sync_resolve("kernel32", "CreateEventA") != 0);
    CHECK_EQ(win32_sync_resolve("kernel32.dll", "createeventa"), 0);
    CHECK_EQ(win32_sync_resolve("user32.dll", "CreateEventA"), 0);
    CHECK_EQ(win32_sync_resolve("kernel32.dll", "NoSuchFunction"), 0);
    CHECK(win32_sync_lookup(WIN32_SYNC_BASE - 4) == NULL);
    CHECK(win32_sync_lookup(WIN32_SYNC_BASE + 4 * count) == NULL);
    CHECK(win32_sync_lookup(WIN32_SYNC_BASE + 2) == NULL);

    /* --- guest memory: stack page, scratch page, TEB page --- */
    guest_init(&memory, 0x8000);
    CHECK_EQ(guest_map(&memory, STACK_BASE, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, SCRATCH, 0x1000), 0);
    CHECK_EQ(guest_map(&memory, TEB, 0x1000), 0);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;
    cpu.fs_base = TEB;
    M32(TEB + 0x34) = 0xdeadbeefu;

    GuestFn create_mutex = fn_of("CreateMutexA"), open_mutex = fn_of("OpenMutexA");
    GuestFn release_mutex = fn_of("ReleaseMutex"), wait_one = fn_of("WaitForSingleObject");
    GuestFn create_sem = fn_of("CreateSemaphoreA"), open_sem = fn_of("OpenSemaphoreA");
    GuestFn release_sem = fn_of("ReleaseSemaphore");
    GuestFn create_event = fn_of("CreateEventA"), open_event = fn_of("OpenEventA");
    GuestFn set_event = fn_of("SetEvent"), reset_event = fn_of("ResetEvent");
    GuestFn wait_many = fn_of("WaitForMultipleObjects");

    /* --- mutex: unowned acquire, recursion, release, not-owner --- */
    args[0] = 0; args[1] = 0; args[2] = 0;
    uint32_t m = call(create_mutex, args, 3);
    CHECK(m >= 0x500 && m < 0x600);
    CHECK_EQ(last_error(), 0);
    args[0] = m; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = m; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);             /* recursive acquire */
    args[0] = m; CHECK_EQ(call(release_mutex, args, 1), 1);
    args[0] = m; CHECK_EQ(call(release_mutex, args, 1), 1);
    args[0] = m; CHECK_EQ(call(release_mutex, args, 1), 0);
    CHECK_EQ(last_error(), 288);                     /* ERROR_NOT_OWNER */

    /* initial owner starts at recursion 1 */
    args[0] = 0; args[1] = 1; args[2] = 0;
    uint32_t owned = call(create_mutex, args, 3);
    args[0] = owned; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);             /* recursion 2 */
    args[0] = owned; CHECK_EQ(call(release_mutex, args, 1), 1);
    args[0] = owned; CHECK_EQ(call(release_mutex, args, 1), 1);
    args[0] = owned; CHECK_EQ(call(release_mutex, args, 1), 0);
    CHECK_EQ(last_error(), 288);

    /* named mutex: case-insensitive open, create-existing, missing name */
    uint32_t nm = put_name(SCRATCH + 0x100, "SyncMutex");
    args[0] = 0; args[1] = 0; args[2] = nm;
    uint32_t named = call(create_mutex, args, 3);
    args[0] = 0; args[1] = 0; args[2] = put_name(SCRATCH + 0x120, "syncmutex");
    uint32_t opened = call(open_mutex, args, 3);
    CHECK_EQ(opened, named);
    CHECK_EQ(last_error(), 0);
    args[0] = 0; args[1] = 0; args[2] = nm;
    CHECK_EQ(call(create_mutex, args, 3), named);     /* same slot, error 183 */
    CHECK_EQ(last_error(), 183);
    args[0] = 0; args[1] = 0; args[2] = put_name(SCRATCH + 0x140, "NoSuchMutex");
    CHECK_EQ(call(open_mutex, args, 3), 0);
    CHECK_EQ(last_error(), 2);
    args[0] = 0; args[1] = 0; args[2] = 0;
    CHECK_EQ(call(open_mutex, args, 3), 0);
    CHECK_EQ(last_error(), 87);
    args[0] = opened; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);             /* alias shares ownership */
    args[0] = opened; CHECK_EQ(call(release_mutex, args, 1), 1);

    /* --- semaphore: counts, timeout 0, previous count, saturation --- */
    args[0] = 0; args[1] = 2; args[2] = 2; args[3] = 0;
    uint32_t s = call(create_sem, args, 4);
    CHECK(s >= 0x500 && s < 0x600 && s != m && s != owned && s != named);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    uint32_t previous = SCRATCH + 0x200;
    args[0] = s; args[1] = 1; args[2] = previous;
    CHECK_EQ(call(release_sem, args, 3), 1);
    CHECK_EQ(M32(previous), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = s; args[1] = 2; args[2] = previous;
    CHECK_EQ(call(release_sem, args, 3), 1);
    CHECK_EQ(M32(previous), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = s; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    args[0] = s; args[1] = 0xffffffffu; args[2] = 0;
    CHECK_EQ(call(release_sem, args, 3), 0);
    CHECK_EQ(last_error(), 87);

    /* count saturates at lMaximumCount instead of overflowing */
    args[0] = 0; args[1] = 0; args[2] = 1; args[3] = 0;
    uint32_t capped = call(create_sem, args, 4);
    args[0] = capped; args[1] = 5; args[2] = previous;
    CHECK_EQ(call(release_sem, args, 3), 1);
    CHECK_EQ(M32(previous), 0);
    args[0] = capped; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = capped; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);

    /* lMaximumCount 0 means 0x7fffffff */
    args[0] = 0; args[1] = 0; args[2] = 0; args[3] = 0;
    uint32_t unbounded = call(create_sem, args, 4);
    args[0] = unbounded; args[1] = 5; args[2] = previous;
    CHECK_EQ(call(release_sem, args, 3), 1);
    CHECK_EQ(M32(previous), 0);
    args[0] = unbounded; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);

    /* named semaphore, reused through OpenSemaphoreA */
    args[0] = 0; args[1] = 3; args[2] = 3; args[3] = put_name(SCRATCH + 0x160, "SyncSem");
    uint32_t ns = call(create_sem, args, 4);
    args[0] = 0; args[1] = 0; args[2] = put_name(SCRATCH + 0x180, "SYNCSEM");
    CHECK_EQ(call(open_sem, args, 3), ns);
    args[0] = ns; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = ns; args[1] = 1; args[2] = previous;
    CHECK_EQ(call(release_sem, args, 3), 1);
    CHECK_EQ(M32(previous), 2);

    /* infinite wait on an empty semaphore must trap, not fake success */
    trap_armed = 1;
    trap_text[0] = 0;
    if (!setjmp(trap)) {
        args[0] = s; args[1] = 0xffffffffu;
        (void)call(wait_one, args, 2);
        CHECK(!"infinite wait on an empty semaphore did not trap");
    } else {
        CHECK(strstr(trap_text, "blocking wait") != NULL);
    }
    trap_armed = 0;
    cpu.r[4] = STACK_TOP;

    /* --- events: manual vs auto reset --- */
    args[0] = 0; args[1] = 1; args[2] = 0; args[3] = 0;
    uint32_t manual = call(create_event, args, 4);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    args[0] = manual; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0); /* stays signaled */
    args[0] = manual; CHECK_EQ(call(reset_event, args, 1), 1);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);

    args[0] = 0; args[1] = 0; args[2] = 1; args[3] = 0; /* auto-reset, initially signaled */
    uint32_t auto_ev = call(create_event, args, 4);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258); /* cleared by wait */
    args[0] = auto_ev; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = auto_ev; CHECK_EQ(call(reset_event, args, 1), 1);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    args[0] = s; CHECK_EQ(call(set_event, args, 1), 0);  /* wrong object kind */
    CHECK_EQ(last_error(), 6);

    /* named event, reused through OpenEventA */
    args[0] = 0; args[1] = 1; args[2] = 0; args[3] = put_name(SCRATCH + 0x1a0, "SyncEvent");
    uint32_t ne = call(create_event, args, 4);
    args[0] = 0; args[1] = 0; args[2] = put_name(SCRATCH + 0x1c0, "syncevent");
    CHECK_EQ(call(open_event, args, 3), ne);
    args[0] = 0; args[1] = 0; args[2] = put_name(SCRATCH + 0x1e0, "NoSuchEvent");
    CHECK_EQ(call(open_event, args, 3), 0);
    CHECK_EQ(last_error(), 2);

    /* --- WaitForMultipleObjects: any, all, no partial consumption --- */
    uint32_t list = SCRATCH + 0x300;
    M32(list) = manual;
    M32(list + 4) = auto_ev;
    args[0] = 2; args[1] = list; args[2] = 0; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 258);
    args[0] = auto_ev; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = 2; args[1] = list; args[2] = 0; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 1);             /* WAIT_OBJECT_0 + 1 */
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);

    args[0] = auto_ev; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = 2; args[1] = list; args[2] = 1; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 258);           /* waitAll fails ... */
    args[0] = auto_ev; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);              /* ... without consuming */

    args[0] = manual; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = auto_ev; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = 2; args[1] = list; args[2] = 1; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 0);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0); /* manual kept */
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258); /* auto consumed */
    args[0] = manual; CHECK_EQ(call(reset_event, args, 1), 1);

    /* invalid handle fails before any object is touched */
    args[0] = manual; CHECK_EQ(call(set_event, args, 1), 1);
    M32(list) = manual;
    M32(list + 4) = 0x1234;
    args[0] = 2; args[1] = list; args[2] = 0; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 0xffffffffu);
    CHECK_EQ(last_error(), 6);
    args[0] = manual; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0);              /* still signaled */
    args[0] = manual; CHECK_EQ(call(reset_event, args, 1), 1);
    args[0] = 0; args[1] = list; args[2] = 0; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 0xffffffffu);
    CHECK_EQ(last_error(), 87);
    args[0] = 9; args[1] = list; args[2] = 0; args[3] = 0;
    CHECK_EQ(call(wait_many, args, 4), 0xffffffffu);
    CHECK_EQ(last_error(), 87);

    /* --- hand-off: a woke thread re-checks and consumes one token -------- */
    /* Windows satisfies a wait by consuming the object.  The cooperative
     * scheduler only makes the waiter runnable; the wait loop re-tests the
     * object, so a release ("woken" but nothing signaled) must go back to sleep
     * instead of reporting WAIT_OBJECT_0, and a real token must be consumed
     * exactly once -- not satisfy the waiter twice. */
    args[0] = 0; args[1] = 0; args[2] = 1; args[3] = 0;
    uint32_t hand = call(create_sem, args, 4);        /* empty semaphore */
    win32_sync_set_blocker(fake_block, NULL);
    fake_wakes_left = 1;                              /* one spurious wake */
    args[0] = hand; args[1] = 0xffffffffu;
    CHECK_EQ(call(wait_one, args, 2), 258);           /* re-checks, no fake success */
    args[0] = hand; args[1] = 1; args[2] = 0;
    CHECK_EQ(call(release_sem, args, 3), 1);
    args[0] = hand; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = hand; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258); /* consumed once */
    win32_sync_set_blocker(NULL, NULL);
    args[0] = auto_ev; CHECK_EQ(call(reset_event, args, 1), 1);
    args[0] = auto_ev; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = auto_ev; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    /* manual-reset stays signaled for every wait until ResetEvent */
    args[0] = manual; CHECK_EQ(call(set_event, args, 1), 1);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 0);
    args[0] = manual; CHECK_EQ(call(reset_event, args, 1), 1);
    args[0] = manual; args[1] = 0; CHECK_EQ(call(wait_one, args, 2), 258);
    CHECK_EQ(win32_sync_close(hand), 1);

    /* --- invalid handles and close --- */
    args[0] = 0x1234; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0xffffffffu);
    CHECK_EQ(last_error(), 6);
    args[0] = 0; args[1] = 0;
    CHECK_EQ(call(wait_one, args, 2), 0xffffffffu);
    CHECK_EQ(last_error(), 6);
    args[0] = 0x1234; CHECK_EQ(call(release_mutex, args, 1), 0);
    CHECK_EQ(last_error(), 6);
    args[0] = 0x1234; args[1] = 1; args[2] = 0;
    CHECK_EQ(call(release_sem, args, 3), 0);
    CHECK_EQ(last_error(), 6);

    CHECK_EQ(win32_sync_close(named), 1);
    CHECK_EQ(win32_sync_close(named), 0);
    CHECK_EQ(win32_sync_close(0x1234), 0);
    CHECK_EQ(win32_sync_close(0), 0);
    CHECK_EQ(win32_sync_close(WIN32_SYNC_HANDLE_BASE + WIN32_SYNC_HANDLE_COUNT), 0);
    args[0] = 0; args[1] = 0; args[2] = nm;
    CHECK_EQ(call(open_mutex, args, 3), 0);            /* close dropped the name */
    CHECK_EQ(last_error(), 2);
    CHECK_EQ(win32_sync_close(manual), 1);
    CHECK_EQ(win32_sync_close(ne), 1);

    guest_destroy(&memory);
    if (failures) {
        fprintf(stderr, "FAIL: win32_sync smoke (%u checks, %u failures)\n", checks, failures);
        return 1;
    }
    printf("PASS: win32_sync smoke (%u checks)\n", checks);
    return 0;
}
