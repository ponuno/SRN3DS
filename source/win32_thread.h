#pragma once
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE only; no shim internals */
#include "n3ds_lift_runtime.h"

/* Cooperative Win32 guest threads.
 *
 * Model: each guest thread owns a guest stack/TEB/TLS copy, a CPU struct and a
 * *host* stack. Windows makes a created thread runnable; this port runs exactly
 * one guest thread at a time, and a thread gives up the CPU only inside a
 * blocking shim (WaitForSingleObject, WaitForMultipleObjects, Sleep) or when it
 * terminates. The scheduler then switches host stacks to the next runnable guest
 * thread. A wait that cannot ever be satisfied is a hard fault naming every
 * blocked thread; it is never reported as success.
 *
 * Why a host stack per thread: the lifted AOT code keeps the guest GPRs in
 * locals and the guest arithmetic flags in a local `Flags`; the IR interpreter
 * does the same. `CPU` is only the call-clobber spill area. Suspending a guest
 * thread therefore requires preserving the host C stack, not just `CPU`. See
 * THREAD_MODEL.md.
 *
 * Consequences, stated instead of hidden:
 *  - No preemption. A thread that computes forever starves the others. Every
 *    blocking shim is a scheduling point; Sleep(0) is an explicit yield.
 *  - CreateThread (without CREATE_SUSPENDED) and the ResumeThread that drops
 *    the suspend count to zero switch to the new thread immediately, exactly as
 *    Windows is allowed to; if it blocks, control returns to the creator.
 *  - If no thread is runnable and no blocked thread has a finite deadline, the
 *    scheduler faults explicitly ("all guest threads blocked"). If a deadline
 *    exists the scheduler idles the host until it expires and returns
 *    WAIT_TIMEOUT to that waiter.
 *  - Guest thread handles are waitable (signaled on termination). Closing a
 *    live thread handle is deferred until the thread exits.
 *  - DLL_THREAD_ATTACH/DETACH are not delivered. Fls* data stays process-wide
 *    (TEB TLS slots *are* per thread).
 *  - SuspendThread on the calling thread is refused (ERROR_CALL_NOT_IMPLEMENTED).
 *  - One logical CPU: SetThreadAffinityMask accepts only bit 0.
 *
 * Handle space: 0x600 + slot, 16 slots. Thread ids: main = 1, created = 2+slot.
 * Each handle owns one guest mapping (stack + TEB + TLS copy) and one host
 * stack (win32_hostctx_default_stack()). CloseHandle releases both once the
 * thread has terminated.
 */
#define WIN32_THREAD_BASE (WIN32_THUNK_BASE + 0x00070000u)
#define WIN32_THREAD_HANDLE_BASE 0x600u
#define WIN32_THREAD_HANDLE_COUNT 16u
#define WIN32_THREAD_DEFAULT_STACK (256u * 1024u)
#define WIN32_THREAD_MAX_STACK (4u * 1024u * 1024u)
#define WIN32_THREAD_HOST_STACK (256u * 1024u)
#define WIN32_THREAD_EXIT_FAULT 0xC0000001u

typedef struct { const char *dll, *name; GuestFn fn; } Win32ThreadDef;

/* Binds guest memory, snapshots the process TEB/TLS template and registers the
 * main guest thread with the scheduler. No thread is created until CreateThread. */
int win32_thread_init(GuestMemory *m);
void win32_thread_shutdown(void);
const Win32ThreadDef *win32_thread_defs(unsigned *count);
uint32_t win32_thread_resolve(const char *dll, const char *name);
const FnEntry *win32_thread_lookup(uint32_t addr);

/* Id of the guest thread currently executing (main = 1). Used by mutexes,
 * critical sections and the wait functions. */
uint32_t win32_thread_current_id(void);
/* 1 when handle belongs to this module (including an exited thread). */
int win32_thread_is_handle(uint32_t handle);
/* Sync-module hook: 1 = terminated thread (exit code filled), 0 = live thread,
 * -1 = not a thread handle. */
int win32_thread_query(uint32_t handle, uint32_t *exit_code);
/* 1 when the handle names a terminated thread (a thread handle is never
 * acquired by a wait, unlike a mutex). */
int win32_thread_signaled(uint32_t handle);
uint32_t win32_thread_exit_code(uint32_t handle);
/* Frees the slot, its guest mapping and its host stack. 1 if the handle was
 * ours. A live thread is marked close-pending instead. */
int win32_thread_close(uint32_t handle);

/* ---- scheduler interface used by win32_sync.c / win32_shims.c ------------- */
/* 1 once win32_thread_init() has registered the main thread. */
int win32_thread_scheduler_ready(void);
/* Suspend the current guest thread until one of `handles` is signaled or the
 * timeout expires. Must be called only after the caller has checked that no
 * handle in the set is ready. Returns:
 *   0  a listed handle became signaled (caller must re-check and acquire)
 *   1  timeout
 *   2  deadlock: no thread can ever run; caller must call rt_unimpl with
 *      win32_thread_deadlock_text()
 *   3  no scheduler (single-threaded build); caller keeps the legacy behaviour
 * A thread that is woken but cannot acquire (another waiter won an auto-reset
 * object) simply calls this again. Waiter order is FIFO by block ticket. */
int win32_thread_block(const uint32_t *handles, unsigned count, int wait_all,
                       uint32_t timeout_ms);
/* Broadcast: mark every blocked thread whose wait set contains `handle`
 * runnable, oldest ticket first. Never switches by itself. */
void win32_thread_wake(uint32_t handle);
const char *win32_thread_deadlock_text(void);
/* Monotonic milliseconds for Sleep/deadline bookkeeping. */
uint64_t win32_thread_now_ms(void);
/* Blocking sleep that schedules other guest threads; INFINITE faults. */
void win32_thread_sleep(uint32_t ms);
/* Give other runnable guest threads a turn without blocking (Sleep(0)). */
void win32_thread_yield(void);
/* Number of live (READY/RUNNING/BLOCKED) created threads; diagnostics. */
unsigned win32_thread_live_count(void);
/* Test/diagnostic: last FIFO sequence stamped for a guest thread id (0 = never
 * stamped).  Main id 1 must be non-zero as soon as the scheduler is up. */
uint64_t win32_thread_ready_seq(uint32_t id);
/* Diagnostics: guest mapping and host stack reserved for a thread slot. */
int win32_thread_memory(unsigned slot, uint32_t *guest_bytes, size_t *host_bytes);
size_t win32_thread_struct_size(void);
/* Measurement-only hang watchdog: start counting only once the first real
 * Present has been submitted. No-op unless TESV_HANG_WATCHDOG_MS is set. */
void n3ds_hang_watchdog_arm(void);
