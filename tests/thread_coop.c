/* Host tests for cooperative guest threads.
 *
 * Covers, against the real win32_shims/win32_sync/win32_thread runtime:
 *   1  the TESV launcher shape: a blocked launcher resumes suspended workers
 *   2  auto-reset event: exactly one waiter wakes, FIFO
 *   3  deterministic wake order over repeated runs
 *   4  TEB / TLS / last-error isolation between threads
 *   5  clean ExitThread / normal return / faulting thread
 *   6  all threads blocked -> explicit fault (inside a real invoke frame)
 *   7  real guest x86 through the IR interpreter: flags and continuation
 *      survive a suspend+resume (the test a CPU snapshot cannot pass)
 */
#include "win32_shims.h"
#include "win32_thread.h"
#include "win32_hostctx.h"
#include "win32_sync.h"
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA   0x21000000u
#define STACK  0x22000000u
#define CODE   0x00510000u
#define ENTRY_DEADLOCK 0x00520000u
#define ENTRY_IR       0x00530000u
#define CODE2          0x00510100u   /* pure-compute loop, no imports */
static int failures;
static int checks;
static GuestMemory m;
static CPU *cpu;

#define CHECK(cond, what) do { ++checks; if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); ++failures; } } while (0)

/* ---- shim calls from a C guest-body stand-in --------------------------- */
static uint32_t resolve(const char *name) {
    PeImport e = {.dll = "kernel32.dll", .name = name};
    return win32_resolve(&e, NULL);
}
static uint32_t call_shim(CPU *c, uint32_t thunk, const uint32_t *args, unsigned n) {
    uint32_t sp = c->r[4], base = sp - 4 * (n + 1);
    M32(base) = 0xffffffffu;
    for (unsigned i = 0; i < n; ++i) M32(base + 4 + 4 * i) = args[i];
    c->r[4] = base;
    const FnEntry *e = win32_lookup(thunk);
    if (!e) { c->r[4] = sp; rt_unimpl(c, thunk, "test: shim not found"); }
    e->fn(c);
    c->r[4] = sp;
    return c->r[0];
}
static uint32_t W1(CPU *c, const char *name, uint32_t a) {
    uint32_t args[1] = {a};
    return call_shim(c, resolve(name), args, 1);
}
static uint32_t W2(CPU *c, const char *name, uint32_t a, uint32_t b) {
    uint32_t args[2] = {a, b};
    return call_shim(c, resolve(name), args, 2);
}
static uint32_t W3(CPU *c, const char *name, uint32_t a, uint32_t b, uint32_t cc) {
    uint32_t args[3] = {a, b, cc};
    return call_shim(c, resolve(name), args, 3);
}
static void ret4(CPU *c, uint32_t v) { c->r[0] = v; c->r[4] += 8; }
static uint32_t param_of(CPU *c) { return M32(c->r[4] + 4); }

/* ---- shared test state in guest memory --------------------------------- */
enum {
    S_EVENT = 0x00, S_DONE = 0x04, S_COUNTER = 0x08, S_PENDING = 0x0c,
    S_TLSIDX = 0x10,
    S_HANDLE0 = 0x20, S_LOGN = 0x50, S_LOG = 0x60,
    S_TEB = 0x100, S_TLSSEEN = 0x120, S_LASTERR = 0x1a0,
};
static void log_push(uint32_t st, uint32_t v) {
    uint32_t n = M32(st + S_LOGN);
    if (n < 16) M32(st + S_LOG + 4 * n) = v;
    M32(st + S_LOGN) = n + 1;
}

/* ---- worker that records its id --------------------------------------- */
static void body_worker(CPU *c) {
    uint32_t st = param_of(c);
    W1(c, "InterlockedIncrement", st + S_COUNTER);
    log_push(st, 100u + win32_thread_current_id());
    M32(st + S_TLSSEEN) = (uint32_t)c->fs_base;
    ret4(c, 0);
}
/* suspends until the event, then runs the worker list (TESV 0xa5ac30 shape) */
static void body_launcher(CPU *c) {
    uint32_t st = param_of(c);
    uint32_t r = W2(c, "WaitForSingleObject", M32(st + S_EVENT), 0xffffffffu);
    if (r == 0) W1(c, "InterlockedDecrement", st + S_PENDING);
    log_push(st, 2);
    uint32_t n = M32(st + 0x1c);
    for (uint32_t i = 0; i < n; ++i)
        W1(c, "ResumeThread", M32(st + S_HANDLE0 + 4 * i));
    log_push(st, 3);
    ret4(c, 0);
}
/* waits for `event`, then records; used for FIFO / auto-reset ordering */
static void body_waiter(CPU *c) {
    uint32_t st = param_of(c);
    uint32_t r = W2(c, "WaitForSingleObject", M32(st + S_EVENT), 0xffffffffu);
    if (r == 0) log_push(st, 1000u + win32_thread_current_id());
    ret4(c, 0);
}
/* TLS / TEB / last-error isolation: set, block, read back */
/* Blocking critical section: param layout {cs, entered, seen, finished}.  The
 * counter is bumped only while this thread actually owns the section. */
static void body_cs(CPU *c) {
    uint32_t st = param_of(c);
    uint32_t cs = M32(st + 0);
    W1(c, "EnterCriticalSection", cs);
    uint32_t seen = M32(st + 4);
    M32(st + 8) = seen;
    M32(st + 4) = seen + 1;
    W1(c, "LeaveCriticalSection", cs);
    M32(st + 12) = 1;
    ret4(c, 0);
}

static void body_tls(CPU *c) {
    uint32_t st = param_of(c);
    uint32_t me = win32_thread_current_id();
    W2(c, "TlsSetValue", M32(st + S_TLSIDX), me * 100u);
    W1(c, "SetLastError", me * 10u);
    uint32_t pre = W1(c, "GetLastError", 0);
    if (getenv("THREAD_TRACE")) fprintf(stderr, "[tls] id=%u set=%u pre=%u\n", me, me * 10u, pre);
    uint32_t base = (uint32_t)c->fs_base;
    M32(st + S_TEB + 4 * (me - 2)) = base;
    (void)W2(c, "WaitForSingleObject", M32(st + S_EVENT), 0xffffffffu);
    uint32_t err = W1(c, "GetLastError", 0);
    uint32_t tls = W1(c, "TlsGetValue", M32(st + S_TLSIDX));
    M32(st + S_TLSSEEN + 8 * (me - 2)) = tls;
    M32(st + S_TLSSEEN + 8 * (me - 2) + 4) = err;
    /* the TEB must still be this thread's: self pointer, own id, own stack */
    /* NT TEB layout: +0x00 ExceptionList (SEH chain head), +0x18 Self,
     * +0x24/+0x28 ClientId.  The thread id must NOT live at +0x00, or every
     * newly created guest thread starts with a garbage SEH chain. */
    int ok = M32(base + 0x18) == base && M32(base + 0x28) == me &&
             M32(base + 0x00) == 0xffffffffu;
    ret4(c, ok ? 1u : 0u);
}
static void body_exit(CPU *c) { W1(c, "ExitThread", 9); ret4(c, 0); }
static void body_return(CPU *c) { ret4(c, 7); }
static void body_fault(CPU *c) { (void)c; rt_unimpl(NULL, 0, "thread_coop fault body"); }
static void body_wait_forever(CPU *c) {
    uint32_t st = param_of(c);
    W2(c, "WaitForSingleObject", M32(st + S_EVENT), 0xffffffffu);
    ret4(c, 0);
}
static void body_main_deadlock(CPU *c) {
    uint32_t st = param_of(c);
    /* create a worker that blocks forever, then block forever ourselves */
    uint32_t a[6] = {0, 0x10000u, 0x00500070u /* body_wait_forever */, st, 0, 0};
    a[5] = st + 0x1e0;
    uint32_t h = call_shim(c, resolve("CreateThread"), a, 6);
    M32(st + 0x1e4) = h; /* real handle, for the harness to wake it */
    W2(c, "WaitForSingleObject", M32(st + S_DONE), 0xffffffffu);
    ret4(c, 0);
}

/* the table must not shadow the IR body at ENTRY_IR */
static const FnEntry table[] = {
    {0x00500000u, body_worker},
    {0x00500010u, body_launcher},
    {0x00500020u, body_waiter},
    {0x00500030u, body_tls},
    {0x00500040u, body_exit},
    {0x00500050u, body_return},
    {0x00500060u, body_fault},
    {0x00500070u, body_wait_forever},
    {0x00500080u, body_cs},
    {ENTRY_DEADLOCK, body_main_deadlock},
};

static uint32_t new_event(int manual, int init) {
    uint32_t a[4] = {0, (uint32_t)manual, (uint32_t)init, 0};
    uint32_t h = 0;
    CPU tmp = *cpu;
    h = call_shim(&tmp, resolve("CreateEventA"), a, 4);
    *cpu = tmp;
    return h;
}
static uint32_t spawn(uint32_t entry, uint32_t param, uint32_t flags) {
    uint32_t a[6] = {0, 0x10000u, entry, param, flags, DATA + 0x1f0};
    CPU tmp = *cpu;
    uint32_t h = call_shim(&tmp, resolve("CreateThread"), a, 6);
    *cpu = tmp;
    return h;
}
static uint32_t wait1(uint32_t h, uint32_t ms) {
    CPU tmp = *cpu;
    uint32_t r = W2(&tmp, "WaitForSingleObject", h, ms);
    *cpu = tmp;
    return r;
}
static uint32_t exit_code(uint32_t h) {
    CPU tmp = *cpu;
    W2(&tmp, "GetExitCodeThread", h, DATA + 0x1f4);
    *cpu = tmp;
    return M32(DATA + 0x1f4);
}
static void close_h(uint32_t h) {
    CPU tmp = *cpu;
    W1(&tmp, "CloseHandle", h);
    *cpu = tmp;
}
static void reset_state(void) {
    for (uint32_t i = 0; i < 0x200; i += 4) M32(DATA + i) = 0;
}

/* ---------------------------------------------------------------- 1 + 3 */
static void test_launcher_and_order(const char *tag) {
    reset_state();
    uint32_t st = DATA;
    M32(st + S_EVENT) = new_event(1, 0);
    M32(st + S_DONE) = new_event(1, 0);
    M32(st + S_PENDING) = 2;
    M32(st + 0x1c) = 2;
    /* two suspended workers */
    uint32_t w0 = spawn(0x00500000u, st, 4);
    uint32_t w1 = spawn(0x00500000u, st, 4);
    M32(st + S_HANDLE0) = w0;
    M32(st + S_HANDLE0 + 4) = w1;
    /* launcher: runs now, blocks on the event, returns to the creator */
    uint32_t L = spawn(0x00500010u, st, 0);
    CHECK(win32_thread_signaled(L) == 0, "launcher is alive and blocked");
    CHECK(M32(st + S_PENDING) == 2, "launcher has not decremented before the event");
    CHECK(M32(st + S_COUNTER) == 0, "workers are still suspended (counter must be 0)");
    if (M32(st + S_COUNTER) != 0)
        fprintf(stderr, "  counter=%u pending=%u logn=%u log0=%u\n",
                M32(st + S_COUNTER), M32(st + S_PENDING), M32(st + S_LOGN), M32(st + S_LOG));
    /* signal; the wait on the two thread handles pumps the launcher */
    CPU tmp = *cpu;
    W1(&tmp, "SetEvent", M32(st + S_EVENT));
    *cpu = tmp;
    uint32_t hs[2] = {w0, w1};
    uint32_t list = DATA + 0x1e8;
    M32(list) = hs[0]; M32(list + 4) = hs[1];
    uint32_t a[4] = {2, list, 1, 0xffffffffu};
    CPU tmp2 = *cpu;
    uint32_t r = call_shim(&tmp2, resolve("WaitForMultipleObjects"), a, 4);
    *cpu = tmp2;
    CHECK(r == 0, "WaitForMultipleObjects(run both workers) == WAIT_OBJECT_0");
    /* A woken waiter may run before the launcher's final statement (Windows
     * makes no ordering promise here). Pump the launcher explicitly. */
    CHECK(wait1(L, 0xffffffffu) == 0, "launcher finishes after resuming both workers");
    CHECK(M32(st + S_COUNTER) == 2, "both workers ran");
    CHECK(M32(st + S_PENDING) == 1, "launcher decremented exactly once");
    CHECK(M32(st + S_LOGN) == 4, "four log entries (launcher start, 2 workers, launcher end)");
    CHECK(M32(st + S_LOG) == 2, "launcher logged before resuming workers");
    uint32_t id0 = w0 - WIN32_THREAD_HANDLE_BASE + 2, id1 = w1 - WIN32_THREAD_HANDLE_BASE + 2;
    CHECK(M32(st + S_LOG + 4) == 100u + id0, "worker 0 logged after resume (FIFO)");
    CHECK(M32(st + S_LOG + 8) == 100u + id1, "worker 1 logged after worker 0");
    CHECK(M32(st + S_LOG + 12) == 3, "launcher logged after resuming both");
    CHECK(exit_code(L) == 0 && exit_code(w0) == 0 && exit_code(w1) == 0, "all exit codes 0");
    if (exit_code(L) != 0 || exit_code(w0) != 0 || exit_code(w1) != 0)
        fprintf(stderr, "  exitcodes L=%u w0=%u w1=%u logn=%u\n",
                exit_code(L), exit_code(w0), exit_code(w1), M32(st + S_LOGN));
    CHECK(win32_thread_live_count() == 0, "no live cooperative threads left");
    printf("PASS launcher/%s order=%u,%u,%u,%u\n", tag,
           M32(st + S_LOG), M32(st + S_LOG + 4), M32(st + S_LOG + 8), M32(st + S_LOG + 12));
    close_h(L); close_h(w0); close_h(w1);
    close_h(M32(st + S_EVENT)); close_h(M32(st + S_DONE));
}

/* ---------------------------------------------------------------- 2 */
static void test_auto_reset_one_waiter(void) {
    reset_state();
    uint32_t st = DATA;
    M32(st + S_EVENT) = new_event(0, 0);
    uint32_t w0 = spawn(0x00500020u, st, 0); /* blocks */
    uint32_t w1 = spawn(0x00500020u, st, 0); /* blocks */
    CHECK(!win32_thread_signaled(w0) && !win32_thread_signaled(w1), "both waiters blocked");
    CPU tmp = *cpu;
    W1(&tmp, "SetEvent", M32(st + S_EVENT));
    *cpu = tmp;
    CHECK(wait1(w0, 0xffffffffu) == 0, "auto-reset wakes the oldest waiter");
    CHECK(win32_thread_signaled(w0) == 1, "first waiter ran");
    CHECK(win32_thread_signaled(w1) == 0, "second waiter still blocked");
    tmp = *cpu; W1(&tmp, "SetEvent", M32(st + S_EVENT)); *cpu = tmp;
    CHECK(wait1(w1, 0xffffffffu) == 0 && win32_thread_signaled(w1) == 1, "second event wakes it");
    CHECK(M32(st + S_LOG) == 1000u + 2u && M32(st + S_LOG + 4) == 1000u + 3u,
          "FIFO id order 2 then 3");
    printf("PASS auto-reset FIFO\n");
    close_h(w0); close_h(w1); close_h(M32(st + S_EVENT));
}

/* ---------------------------------------------------------------- 3 */
/* One signal, one waiter.  The cooperative scheduler releases a waiter from the
 * signaller's context; the waiter's own wait loop must then consume the object
 * (semaphore count down / auto-reset event cleared).  These pin that contract
 * end to end: TESV's renderer handshake (ReleaseSemaphore -> palette worker ->
 * SetEvent -> video worker) relies on one release satisfying exactly one wait,
 * otherwise the count never drops and a later Wait succeeds with no new
 * signal. */
static void test_one_token_per_signal(void) {
    reset_state();
    uint32_t st = DATA;
    uint32_t a[4] = {0, 0, 1, 0};                    /* initial 0, maximum 1 */
    CPU tmp = *cpu;
    uint32_t sem = call_shim(&tmp, resolve("CreateSemaphoreA"), a, 4);
    *cpu = tmp;
    M32(st + S_EVENT) = sem;
    uint32_t w0 = spawn(0x00500020u, st, 0);         /* body_waiter blocks */
    uint32_t w1 = spawn(0x00500020u, st, 0);
    CHECK(!win32_thread_signaled(w0) && !win32_thread_signaled(w1), "both waiters blocked");
    tmp = *cpu; W3(&tmp, "ReleaseSemaphore", sem, 1, 0); *cpu = tmp;
    CHECK(wait1(w0, 0xffffffffu) == 0, "one release wakes the oldest waiter");
    CHECK(win32_thread_signaled(w0) == 1, "first waiter ran");
    CHECK(win32_thread_signaled(w1) == 0, "second waiter did not run from one token");
    CHECK(wait1(sem, 0) == 258, "released token was consumed (count is 0)");
    tmp = *cpu; W3(&tmp, "ReleaseSemaphore", sem, 1, 0); *cpu = tmp;
    CHECK(wait1(w1, 0xffffffffu) == 0, "second release wakes the second waiter");
    CHECK(wait1(sem, 0) == 258, "no leftover token after both waits");
    printf("PASS one token per signal (semaphore)\n");
    close_h(w0); close_h(w1); close_h(sem);
}

/* Auto-reset event: the waiter that woke must have cleared the signal, so an
 * immediate 0-timeout wait reports WAIT_TIMEOUT instead of taking the same
 * signal a second time. */
static void test_auto_reset_consumed_on_wake(void) {
    reset_state();
    uint32_t st = DATA;
    M32(st + S_EVENT) = new_event(0, 0);
    uint32_t w = spawn(0x00500020u, st, 0);
    CHECK(win32_thread_signaled(w) == 0, "waiter blocked on the event");
    CPU tmp = *cpu;
    W1(&tmp, "SetEvent", M32(st + S_EVENT)); *cpu = tmp;
    CHECK(wait1(w, 0xffffffffu) == 0 && win32_thread_signaled(w) == 1, "waiter ran");
    CHECK(wait1(M32(st + S_EVENT), 0) == 258,
          "the woken waiter consumed the auto-reset signal");
    /* set-before-wait: a signal with no waiter is kept, not lost */
    tmp = *cpu; W1(&tmp, "SetEvent", M32(st + S_EVENT)); *cpu = tmp;
    uint32_t w2 = spawn(0x00500020u, st, 0);
    CHECK(win32_thread_signaled(w2) == 1, "a signal set before the wait is kept");
    CHECK(wait1(M32(st + S_EVENT), 0) == 258, "and consumed by exactly one wait");
    /* manual-reset: every waiter and a later wait all see it until reset */
    uint32_t manual = new_event(1, 0);
    M32(st + S_EVENT) = manual;
    uint32_t m0 = spawn(0x00500020u, st, 0), m1 = spawn(0x00500020u, st, 0);
    tmp = *cpu; W1(&tmp, "SetEvent", manual); *cpu = tmp;
    CHECK(wait1(m0, 0xffffffffu) == 0 && wait1(m1, 0xffffffffu) == 0,
          "a manual-reset event wakes every waiter");
    CHECK(wait1(manual, 0) == 0, "manual-reset stays signaled after a wait");
    tmp = *cpu; W1(&tmp, "ResetEvent", manual); *cpu = tmp;
    CHECK(wait1(manual, 0) == 258, "ResetEvent clears it");
    printf("PASS auto/manual reset consumption and set-before-wait\n");
    close_h(w); close_h(w2); close_h(m0); close_h(m1); close_h(manual);
}

/* ---------------------------------------------------------------- 4 */
static void test_tls_teb_isolation(void) {
    reset_state();
    uint32_t st = DATA;
    CPU tmp = *cpu;
    uint32_t idx = W1(&tmp, "TlsAlloc", 0);
    *cpu = tmp;
    CHECK(idx != 0xffffffffu, "TlsAlloc");
    M32(st + S_TLSIDX) = idx;
    M32(st + S_EVENT) = new_event(1, 0);
    uint32_t t0 = spawn(0x00500030u, st, 0);
    uint32_t t1 = spawn(0x00500030u, st, 0);
    CHECK(M32(st + S_TEB) != 0 && M32(st + S_TEB + 4) != 0, "both threads recorded a TEB");
    CHECK(M32(st + S_TEB) != M32(st + S_TEB + 4), "distinct TEBs");
    tmp = *cpu; W1(&tmp, "SetEvent", M32(st + S_EVENT)); *cpu = tmp;
    CHECK(wait1(t0, 0xffffffffu) == 0 && wait1(t1, 0xffffffffu) == 0, "both threads finish");
    uint32_t id0 = exit_code(t0), id1 = exit_code(t1);
    CHECK(id0 == 1 && id1 == 1, "TEB self/id check passed in both threads");
    /* tls slot 0/1 hold per-thread values; which thread got which id is dynamic */
    uint32_t a = M32(st + S_TLSSEEN), ea = M32(st + S_TLSSEEN + 4);
    uint32_t b = M32(st + S_TLSSEEN + 8), eb = M32(st + S_TLSSEEN + 12);
    CHECK(a != b && a >= 200 && b >= 200, "TlsGetValue returned two different thread values");
    CHECK((a == 200 && ea == 20) || (a == 300 && ea == 30),
          "thread's TLS value and last-error belong together (a)");
    CHECK((b == 200 && eb == 20) || (b == 300 && eb == 30), "last-error is per-thread (b)");
    printf("PASS TLS/TEB isolation: tls=%u/%u lasterr=%u/%u\n", a, b, ea, eb);
    close_h(t0); close_h(t1); close_h(M32(st + S_EVENT));
}

/* ---------------------------------------------------------------- 5 */
static void test_exit_paths(void) {
    reset_state();
    uint32_t st = DATA;
    uint32_t t_exit = spawn(0x00500040u, st, 0);
    uint32_t t_ret = spawn(0x00500050u, st, 0);
    uint32_t t_fault = spawn(0x00500060u, st, 0);
    CHECK(exit_code(t_exit) == 9, "ExitThread(9) is the exit code");
    CHECK(exit_code(t_ret) == 7, "normal return value is the exit code");
    CHECK(exit_code(t_fault) == WIN32_THREAD_EXIT_FAULT, "faulting body -> 0xC0000001");
    CHECK(wait1(t_fault, 0) == 0, "faulted thread handle is signaled");
    uint32_t after = spawn(0x00500050u, st, 0);
    CHECK(exit_code(after) == 7, "a later thread still runs after a fault");
    printf("PASS exit paths (ExitThread/return/fault/continue)\n");
    close_h(t_exit); close_h(t_ret); close_h(t_fault); close_h(after);
}

/* ---------------------------------------------------------------- 6 */
static void test_deadlock_fault(void) {
    reset_state();
    uint32_t st = DATA;
    M32(st + S_EVENT) = new_event(1, 0);
    M32(st + S_DONE) = new_event(1, 0);
    uint32_t quiet = new_event(1, 0);
    (void)quiet;
    /* run the deadlock body inside a real invoke frame, like the game's main */
    CPU c = *cpu;
    uint32_t result = 0;
    int rc = n3ds_lift_invoke(&c, ENTRY_DEADLOCK, &st, 1, &result);
    const char *why = n3ds_lift_fault();
    CHECK(rc == N3DS_INVOKE_TRAP, "all-blocked situation traps instead of hanging");
    CHECK(strstr(why, "all guest threads blocked") != NULL, "fault names the condition");
    CHECK(strstr(why, "thread 1") != NULL, "fault names the main thread");
    printf("PASS deadlock fault: [%s] live=%u\n", why, win32_thread_live_count());
    /* the worker created by the body is still blocked on S_EVENT; wake it and
     * pump the scheduler by waiting on its handle, then close both */
    uint32_t h = M32(st + 0x1e4);
    CPU tmp = *cpu; W1(&tmp, "SetEvent", M32(st + S_EVENT)); *cpu = tmp;
    if (h) { CHECK(wait1(h, 0xffffffffu) == 0, "blocked worker completes after the fault"); close_h(h); }
    close_h(M32(st + S_EVENT)); close_h(M32(st + S_DONE));
}

/* ---------------------------------------------------------------- 7 */
/* Real guest x86 in the IR interpreter:
 *   [param+0] = side effect        must be written exactly once
 *   [param+4] = event handle
 *   [param+8] = WaitForSingleObject thunk
 *   [param+12..13] = sete/setb results (flags from `cmp ecx,1`)
 *   [param+16] = second side effect
 */
static void install_ir_code(void) {
    static const uint8_t code[] = {        0x55,                         /* push ebp */
        0x89, 0xe5,                   /* mov ebp,esp */
        0x8b, 0x75, 0x08,             /* mov esi,[ebp+8] (callee-saved base) */
        0x83, 0x06, 0x01,             /* add dword [esi],1 */
        0x31, 0xc9,                   /* xor ecx,ecx */
        0x83, 0xf9, 0x01,             /* cmp ecx,1  (ZF=0 CF=1) */
        0x6a, 0xff,                   /* push -1 */
        0xff, 0x76, 0x04,             /* push dword [esi+4] */
        0xff, 0x56, 0x08,             /* call dword [esi+8] */
        0x0f, 0x94, 0x46, 0x0c,       /* sete byte [esi+12] */
        0x0f, 0x92, 0x46, 0x0d,       /* setb byte [esi+13] */
        0xc7, 0x46, 0x10, 0x22, 0x22, 0x00, 0x00, /* mov dword [esi+16],0x2222 */
        0x5d,                         /* pop ebp */
        0xc2, 0x04, 0x00,             /* ret 4 */
    };
    memcpy(guest_ptr(&m, CODE, sizeof code), code, sizeof code);
    /* Pure-compute loop: no import call, no blocking point.  Count in [param+0],
     * iteration budget in [param+4], completion marker in [param+8]. */
    static const uint8_t loop_code[] = {
        0x55,                         /* push ebp */
        0x89, 0xe5,                   /* mov ebp,esp */
        0x8b, 0x75, 0x08,             /* mov esi,[ebp+8] */
        0x8b, 0x4e, 0x04,             /* mov ecx,[esi+4] */
        0x83, 0x06, 0x01,             /* l: add dword [esi],1 */
        0x49,                         /*    dec ecx */
        0x75, 0xfa,                   /*    jnz l */
        0xc7, 0x46, 0x08, 0x01, 0x00, 0x00, 0x00, /* mov dword [esi+8],1 */
        0x5d,                         /* pop ebp */
        0xc2, 0x04, 0x00,             /* ret 4 */
    };
    memcpy(guest_ptr(&m, CODE2, sizeof loop_code), loop_code, sizeof loop_code);
    static IrFnEntry fn[2]; /* the runtime keeps this table for the whole session */
    fn[0].addr = CODE;
    fn[0].end = CODE + sizeof code;
    fn[1].addr = CODE2;
    fn[1].end = CODE2 + sizeof loop_code;
    if (n3ds_ir_bind(fn, 2)) { fprintf(stderr, "ir_bind failed\n"); exit(2); }
}
static void test_ir_flags_and_continuation(void) {
    reset_state();
    uint32_t rec = DATA + 0x180;
    M32(rec + 0) = 0;
    M32(rec + 4) = new_event(1, 0);
    M32(rec + 8) = resolve("WaitForSingleObject");
    M32(rec + 12) = 0xdeadbeefu;
    M32(rec + 16) = 0;
    uint32_t t = spawn(CODE, rec, 0); /* runs immediately, blocks in the event */
    CHECK(win32_thread_signaled(t) == 0, "IR body is suspended inside the wait");
    CHECK(M32(rec + 0) == 1, "IR body executed the pre-wait increment once");
    CHECK(M32(rec + 16) == 0, "IR body did not execute past the call while blocked");
    CPU tmp = *cpu; W1(&tmp, "SetEvent", M32(rec + 4)); *cpu = tmp;
    CHECK(wait1(t, 0xffffffffu) == 0, "IR body resumes and finishes");
    CHECK(M32(rec + 0) == 1, "no re-execution of the pre-wait code (continuation, not restart)");
    CHECK((M32(rec + 12) & 0xff) == 0, "ZF=0 from before the wait survived");
    CHECK(((M32(rec + 12) >> 8) & 0xff) == 1, "CF=1 from before the wait survived");
    CHECK(M32(rec + 16) == 0x2222, "IR body executed the post-wait code");
    CHECK(exit_code(t) == 0, "IR thread exit code 0");
    printf("PASS IR flags/continuation after suspend\n");
    close_h(t); close_h(M32(rec + 4));

    /* Negative control: entering the function again is a *restart*, not a
     * resume. A design that saves only the CPU struct and re-enters the entry
     * point does this: the pre-wait side effect happens a second time. */
    M32(rec + 4) = new_event(1, 1); /* already signaled */
    CPU c = *cpu;
    uint32_t result = 0;
    int rc = n3ds_lift_invoke(&c, CODE, &rec, 1, &result);
    CHECK(rc == 0, "re-entry completes");
    CHECK(M32(rec + 0) == 2, "re-entry repeats the pre-wait side effect (restart, not resume)");
    printf("INFO restart control: counter=%u after second entry\n", M32(rec + 0));
    close_h(M32(rec + 4));
}

/* ---------------------------------------------------------------- 8 */
/* Cooperative time slicing: a guest thread that runs a long loop with no
 * import call must not own the CPU until it finishes.  Without the executor's
 * yield hook, spawn() only returns after the worker's loop has completed, so
 * the counter below equals the budget and every other guest thread (in TESV:
 * the init thread and main) is starved for the whole loop. */
static void test_ir_compute_loop_yields(void) {
    reset_state();
    uint32_t rec = DATA + 0x1c0;
    const uint32_t budget = 200000u;
    M32(rec + 0) = 0;
    M32(rec + 4) = budget;
    M32(rec + 8) = 0;
    uint32_t t = spawn(CODE2, rec, 0);
    CHECK(M32(rec + 0) > 0, "compute loop started before the spawner resumed");
    CHECK(M32(rec + 0) < budget,
          "compute loop yielded while running (spawner regained the CPU mid-loop)");
    CHECK(wait1(t, 0xffffffffu) == 0, "compute loop thread completes");
    CHECK(M32(rec + 0) == budget, "compute loop ran every iteration exactly once");
    CHECK(M32(rec + 8) == 1, "compute loop reached its tail after the yields");
    CHECK(exit_code(t) == 0, "compute loop thread exit code 0");
    printf("PASS IR compute loop yields to other threads\n");
    close_h(t);
}

/* ---------------------------------------------------------------- 9 */
/* A critical section owned by another guest thread is waited for, not refused
 * and not entered.  Before this the shim faulted (the one-shot model assumed no
 * other thread could ever hold a section), which killed TESV's loader while it
 * waited on a lock held by a thread the executor had just preempted. */
static void test_critical_section_waits(void) {
    reset_state();
    uint32_t rec = DATA + 0x220;
    uint32_t cs = DATA + 0x2c0;
    memset(guest_ptr(&m, cs, 64), 0, 64);
    M32(rec + 0) = cs;
    M32(rec + 4) = 0;
    M32(rec + 8) = 0;
    M32(rec + 12) = 0;
    CPU tmp = *cpu;
    W1(&tmp, "InitializeCriticalSection", cs);
    W1(&tmp, "EnterCriticalSection", cs);
    *cpu = tmp;
    uint32_t t = spawn(0x00500080u, rec, 0);
    CHECK(M32(rec + 4) == 0, "a thread does not enter a section another thread owns");
    CHECK(M32(rec + 12) == 0, "the waiter is still waiting, not finished");
    tmp = *cpu;
    W1(&tmp, "LeaveCriticalSection", cs);
    *cpu = tmp;
    CHECK(wait1(t, 0xffffffffu) == 0, "the waiter finishes once the owner leaves");
    CHECK(M32(rec + 4) == 1, "the waiter entered the section exactly once");
    CHECK(M32(rec + 8) == 0, "the waiter saw a free section, never a shared lock");
    CHECK(M32(rec + 12) == 1, "the waiter left the section");
    tmp = *cpu;
    W1(&tmp, "DeleteCriticalSection", cs);
    *cpu = tmp;
    printf("PASS critical section waits instead of faulting\n");
    close_h(t);
}

/* ---------------------------------------------------------------- timeouts */
static void test_timeout_paths(void) {
    reset_state();
    uint32_t e1 = new_event(1, 1), e2 = new_event(1, 0);
    uint32_t list = DATA + 0x1e8;
    M32(list) = e1; M32(list + 4) = e2;
    uint32_t a[4] = {2, list, 1, 20};
    CPU tmp = *cpu;
    uint32_t r = call_shim(&tmp, resolve("WaitForMultipleObjects"), a, 4);
    *cpu = tmp;
    CHECK(r == 258, "WAIT_ALL with one unsignaled object times out");
    CHECK(wait1(e2, 20) == 258, "single finite wait times out without faulting");
    tmp = *cpu; W1(&tmp, "Sleep", 5); *cpu = tmp; /* no other thread: idles, does not fault */
    tmp = *cpu; W1(&tmp, "SetEvent", e2); *cpu = tmp;
    a[3] = 0xffffffffu;
    tmp = *cpu; r = call_shim(&tmp, resolve("WaitForMultipleObjects"), a, 4); *cpu = tmp;
    CHECK(r == 0, "WAIT_ALL succeeds once the second object is signaled");
    printf("PASS finite timeouts and Sleep never fault\n");
    close_h(e1); close_h(e2);
}

/* ---------------------------------------------------------------- memory */
static void report_memory(void) {
    reset_state();
    uint32_t hs[8];
    unsigned n = 0;
    for (unsigned i = 0; i < 8; ++i) {
        hs[i] = spawn(0x00500000u, DATA, 0);
        if (hs[i]) ++n;
    }
    uint32_t guest = 0;
    size_t host = 0;
    unsigned slot = n ? (hs[0] - WIN32_THREAD_HANDLE_BASE) : 0;
    win32_thread_memory(slot, &guest, &host);
    printf("INFO threads=%u sizeof(CPU)=%u sizeof(Thread)=%u sizeof(GuestMemory)=%u "
           "guest/thread=%u host/thread=%u\n", n, (unsigned)sizeof(CPU),
           (unsigned)win32_thread_struct_size(), (unsigned)sizeof(GuestMemory), guest, (unsigned)host);    for (unsigned i = 0; i < n; ++i) close_h(hs[i]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    _putenv("N3DS_IRC_ENABLE=0");
#else
    setenv("N3DS_IRC_ENABLE", "0", 1);
#endif
    guest_init(&m, 64u << 20);
    if (guest_map(&m, DATA, 0x1000) || guest_map(&m, STACK, 0x20000) ||
        guest_map(&m, 0x00500000u, 0x20000)) {
        fprintf(stderr, "guest_map failed\n");
        return 2;
    }
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) {
        fprintf(stderr, "n3ds_lift_bind failed\n");
        return 2;
    }
    if (win32_init(&m, 0x400000, ".")) { fprintf(stderr, "win32_init failed\n"); return 2; }
    install_ir_code();
    cpu = calloc(1, sizeof *cpu);
    if (!cpu) return 2;
    n3ds_lift_init_cpu(cpu, STACK + 0x20000 - 64, 0);
    printf("INFO hostctx backend: %s\n", win32_hostctx_name());

    test_launcher_and_order("tesv-shape");
    test_launcher_and_order("repeat-1");
    test_launcher_and_order("repeat-2");
    test_launcher_and_order("repeat-3");
    test_auto_reset_one_waiter();
    test_one_token_per_signal();
    test_auto_reset_consumed_on_wake();
    test_tls_teb_isolation();
    test_exit_paths();
    test_deadlock_fault();
    test_timeout_paths();
    test_ir_flags_and_continuation();
    test_ir_compute_loop_yields();
    test_critical_section_waits();
    report_memory();

    printf("SUMMARY checks=%d failures=%d\n", checks, failures);
    free(cpu);
    win32_shutdown();
    guest_destroy(&m);
    return failures ? 1 : 0;
}
