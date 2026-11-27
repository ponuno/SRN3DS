/* Host test for the cooperative guest thread module: a real body runs on its
 * own CPU/stack/TEB, ExitThread and faults unwind only that thread, and the
 * handle becomes waitable with the real exit code. */
#include "win32_shims.h"
#include "win32_thread.h"
#include "win32_sync.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define CONSOLE 0x40000000u
#define STEP_RUNS (DATA + 16)
#define STEP_MAIN (DATA + 20)
#define STEP_ORDER (DATA + 24)

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args, size_t n, uint32_t *out) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
#define K(name, args, n) call(cpu, "kernel32.dll", #name, args, n, &r)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "thread test failed at line %d\n", __LINE__); goto done; } } while (0)

/* sp -> [ret][arg], like a stdcall body. */
static void ret_stdcall(CPU *c, uint32_t value) { c->r[0] = value; c->r[4] += 8; }
static void body_return(CPU *c) {
    uint32_t param = M32(c->r[4] + 4);
    M32(DATA) = win32_thread_current_id();
    M32(DATA + 4) = c->fs_base;
    M32(DATA + 8) = (M32(c->fs_base + 0x18) == c->fs_base &&
                     M32(c->fs_base + 0x20) == win32_thread_current_id()) ? 1u : 0u;
    ret_stdcall(c, param + 1);
}
static void body_exit(CPU *c) { (void)c; n3ds_lift_exit(42); }
static void body_fault(CPU *c) { (void)c; rt_unimpl(NULL, 0, "thread smoke fault"); }
/* A non-suspended worker that must get the CPU between main's Sleep(0) calls:
 * it records each run and yields four times. */
static void body_stepper(CPU *c) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t n = M32(STEP_RUNS);
        M32(STEP_RUNS) = n + 1;
        if (n < 8) {
            M32(STEP_ORDER + 4 * n) = win32_thread_current_id();
            M32(STEP_ORDER + 0x40 + 4 * n) = M32(STEP_MAIN);
        }
        win32_thread_sleep(0); /* cooperative yield */
    }
    ret_stdcall(c, 0);
}
/* WaitForSingleObject(handle, 0) from inside a guest thread body. */
static void body_mutex(CPU *c) {
    uint32_t sp = c->r[4], handle = M32(DATA);
    uint32_t base = sp - 12;
    M32(base) = 0xffffffffu;
    M32(base + 4) = handle;
    M32(base + 8) = 0;
    uint32_t addr = win32_sync_resolve("kernel32.dll", "WaitForSingleObject");
    const FnEntry *e = addr ? win32_sync_lookup(addr) : NULL;
    if (!e) ret_stdcall(c, 0xdead);
    else {
        c->r[4] = base;
        e->fn(c);
        M32(DATA + 12) = c->r[0];
        c->r[4] = sp;
    }
    ret_stdcall(c, 0);
}
static const FnEntry table[] = {
    {0x00500000u, body_return}, {0x00501000u, body_exit},
    {0x00502000u, body_fault}, {0x00503000u, body_mutex},
    {0x00504000u, body_stepper},
};

int main(void) {
    int rc = -1, mapped = 0;
    GuestMemory m;
    guest_init(&m, 16u << 20);
    if (guest_map(&m, DATA, 4096) || guest_map(&m, CONSOLE, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x200000, ".")) goto done;
    CPU *cpu = calloc(1, sizeof *cpu);
    if (!cpu) goto done;
    n3ds_lift_init_cpu(cpu, CONSOLE + 4096 - 64, 0);
    uint32_t r, a[8];

    /* 0. The main thread must own a real FIFO sequence from init, never the
     * magic 0 that would let it outrank every created thread forever. */
    CHECK(win32_thread_ready_seq(1) != 0);

    /* 1. Un-suspended thread runs to completion inside CreateThread. */
    a[0] = 0; a[1] = 0x20000; a[2] = 0x00500000u; a[3] = 41; a[4] = 0; a[5] = CONSOLE + 0x100;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    uint32_t thread = r, tid = M32(CONSOLE + 0x100);
    CHECK(tid >= 2);
    CHECK(M32(DATA) == tid);                       /* own id, not the main thread's 1 */
    CHECK(M32(DATA + 4) != 0x18000000u && M32(DATA + 4) != 0); /* own TEB */
    CHECK(M32(DATA + 8) == 1);                     /* own TEB self/id fields */
    a[0] = thread; a[1] = CONSOLE + 0x104;
    CHECK(!K(GetExitCodeThread, a, 2) && r == 1 && M32(CONSOLE + 0x104) == 42);
    a[0] = thread; a[1] = 0; CHECK(!K(WaitForSingleObject, a, 2) && r == 0);
    a[0] = 1; a[1] = CONSOLE + 0x120; M32(CONSOLE + 0x120) = thread; a[2] = 0; a[3] = 0;
    CHECK(!K(WaitForMultipleObjects, a, 4) && r == 0);

    /* 2. CREATE_SUSPENDED defers the body until ResumeThread; ExitThread code. */
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00501000u; a[3] = 0; a[4] = 4; a[5] = CONSOLE + 0x108;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    uint32_t suspended = r;
    a[0] = suspended; a[1] = CONSOLE + 0x10c;
    CHECK(!K(GetExitCodeThread, a, 2) && M32(CONSOLE + 0x10c) == 259);
    a[0] = suspended; CHECK(!K(ResumeThread, a, 1) && r == 1);
    a[0] = suspended; a[1] = CONSOLE + 0x10c;
    CHECK(!K(GetExitCodeThread, a, 2) && M32(CONSOLE + 0x10c) == 42);

    /* 3. A body fault terminates only that thread; the caller continues. */
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00502000u; a[3] = 0; a[4] = 0; a[5] = CONSOLE + 0x110;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    uint32_t faulted = r;
    a[0] = faulted; a[1] = CONSOLE + 0x114;
    CHECK(!K(GetExitCodeThread, a, 2) && M32(CONSOLE + 0x114) == WIN32_THREAD_EXIT_FAULT);
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00500000u; a[3] = 7; a[4] = 0; a[5] = CONSOLE + 0x118;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    uint32_t after = r;
    a[0] = after; a[1] = CONSOLE + 0x11c;
    CHECK(!K(GetExitCodeThread, a, 2) && M32(CONSOLE + 0x11c) == 8);

    /* 4. Mutex ownership is per thread: the body cannot take a mutex held by
     * the main thread, and a zero-timeout wait reports WAIT_TIMEOUT. */
    a[0] = 0; a[1] = 0; a[2] = 0; CHECK(!K(CreateMutexA, a, 3) && r >= WIN32_SYNC_HANDLE_BASE);
    uint32_t mutex = r;
    a[0] = mutex; a[1] = 0; CHECK(!K(WaitForSingleObject, a, 2) && r == 0);
    M32(DATA) = mutex; M32(DATA + 12) = 0xffffffffu;
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00503000u; a[3] = 0; a[4] = 0; a[5] = CONSOLE + 0x120;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    CHECK(M32(DATA + 12) == 258); /* WAIT_TIMEOUT */
    a[0] = mutex; CHECK(!K(ReleaseMutex, a, 1) && r == 1);

    /* 5. Priorities, affinity, OpenThread/OpenProcess/GetExitCodeProcess. */
    a[0] = thread; a[1] = 2; CHECK(!K(SetThreadPriority, a, 2) && r == 1);
    a[0] = thread; CHECK(!K(GetThreadPriority, a, 1) && r == 2);
    a[0] = thread; a[1] = 1; CHECK(!K(SetThreadAffinityMask, a, 2) && r == 1);
    a[0] = thread; a[1] = 2; CHECK(!K(SetThreadAffinityMask, a, 2) && r == 0 &&
                                   (K(GetLastError, a, 0), r == 87));
    a[0] = 0x1f0fff; a[1] = 0; a[2] = tid;
    CHECK(!K(OpenThread, a, 3) && r == thread);
    a[0] = 0x1f0fff; a[1] = 0; a[2] = 1; CHECK(!K(OpenProcess, a, 3) && r == 0xffffffffu);
    a[0] = 0xffffffffu; a[1] = CONSOLE + 0x124;
    CHECK(!K(GetExitCodeProcess, a, 2) && r == 1 && M32(CONSOLE + 0x124) == 259);
    a[0] = 0xffffffffu; a[1] = CONSOLE + 0x124;
    CHECK(!K(GetExitCodeProcess, a, 2) && r == 1);

    /* 6. FIFO fairness: a non-suspended worker and main alternate through
     * Sleep(0).  Neither may be starved: the worker must run all four times and
     * main must advance through all eight yields. */
    M32(STEP_RUNS) = 0;
    M32(STEP_MAIN) = 0;
    for (uint32_t i = 0; i < 0x50; i += 4) M32(STEP_ORDER + i) = 0;
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00504000u; a[3] = 0; a[4] = 0; a[5] = CONSOLE + 0x140;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    uint32_t stepper = r;
    CHECK(win32_thread_ready_seq(M32(CONSOLE + 0x140)) != 0);
    for (uint32_t i = 0; i < 8; ++i) {
        M32(STEP_MAIN) = i + 1;
        win32_thread_sleep(0);
    }
    CHECK(M32(STEP_RUNS) == 4);
    CHECK(M32(STEP_MAIN) == 8);
    CHECK(M32(STEP_ORDER) == M32(CONSOLE + 0x140));
    CHECK(M32(STEP_ORDER + 0x40) < 8);
    a[0] = stepper; CHECK(!K(CloseHandle, a, 1) && r == 1);

    /* 7. CloseHandle releases every thread slot. */
    a[0] = thread; CHECK(!K(CloseHandle, a, 1) && r == 1);
    a[0] = suspended; CHECK(!K(CloseHandle, a, 1) && r == 1);
    a[0] = faulted; CHECK(!K(CloseHandle, a, 1) && r == 1);
    a[0] = after; CHECK(!K(CloseHandle, a, 1) && r == 1);
    a[0] = 0; a[1] = 0x10000u; a[2] = 0x00500000u; a[3] = 1; a[4] = 0; a[5] = CONSOLE + 0x128;
    CHECK(!K(CreateThread, a, 6) && r >= WIN32_THREAD_HANDLE_BASE);
    a[0] = r; CHECK(!K(CloseHandle, a, 1) && r == 1);

    rc = 0;
done:
    if (cpu) free(cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
