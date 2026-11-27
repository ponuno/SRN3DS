#pragma once
#include <stdio.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE */
#include "win32_thread.h" /* win32_thread_wait_note (diagnostics only) */

/* Private thunk window for the single-thread Win32 synchronization subset.
 * It is disjoint from the main win32_shims table, whose win32_lookup returns
 * NULL for this range, so a dispatcher should consult this table too. */
#define WIN32_SYNC_BASE (WIN32_THUNK_BASE + 0x00010000u)
#define WIN32_SYNC_HANDLE_BASE 0x500u
#define WIN32_SYNC_HANDLE_COUNT 256u

typedef struct { const char *dll, *name; GuestFn fn; } Win32SyncDef;

/* Current guest thread id (1 = main). A guest thread body sets its id through
 * win32_thread.c; mutex ownership and waits use it. Returns the previous id. */
uint32_t win32_sync_set_current_thread(uint32_t id);
uint32_t win32_sync_current_thread(void);
/* Thread-handle query installed by win32_thread_init: returns 1 when the
 * handle is a thread and terminated (exit_code filled), 0 when it is a thread
 * that has not terminated, -1 when the handle is not a thread. */
typedef int (*Win32SyncThreadQuery)(uint32_t handle, uint32_t *exit_code);
void win32_sync_set_thread_query(Win32SyncThreadQuery query);
/* Signal an event object from another shim (input state notification).
 * Returns 1 if the handle is one of this module's events. */
int win32_sync_signal(uint32_t handle);

/* ---- cooperative-thread hooks (installed by win32_thread_init) -------------
 * A NULL blocker keeps the historical single-thread behaviour: a finite wait
 * sleeps the host and returns WAIT_TIMEOUT, an INFINITE wait faults.
 * `block` suspends the calling guest thread and returns 0 (a listed handle may
 * be ready), 1 (timeout), 2 (deadlock: `text` explains it) or 3 (no scheduler).
 * `wake` marks waiters of a handle runnable after SetEvent/Release*. */
typedef int (*Win32SyncBlock)(const uint32_t *handles, unsigned count,
                              int wait_all, uint32_t timeout_ms);
typedef const char *(*Win32SyncBlockText)(void);
typedef void (*Win32SyncWaker)(uint32_t handle);
void win32_sync_set_blocker(Win32SyncBlock block, Win32SyncBlockText text);
void win32_sync_set_waker(Win32SyncWaker wake);
/* Called by win32_thread.c when a thread handle becomes signaled. */
void win32_sync_notify(uint32_t handle);

/* Thunks are WIN32_SYNC_BASE + 4*i; every entry lives in kernel32.dll. */
const Win32SyncDef *win32_sync_defs(unsigned *count);
uint32_t win32_sync_resolve(const char *dll, const char *name);
const FnEntry *win32_sync_lookup(uint32_t addr);

/* Release a handle owned by this table. 1 if it was ours and is now free,
 * 0 for an unknown, foreign or already-closed handle. Closing drops the
 * object and its name. No last-error update: there is no CPU argument. */
int win32_sync_close(uint32_t handle);
/* Hang-watchdog diagnostic: dump every live sync object. */
void win32_sync_dump(FILE *f);

/* Semantics (cooperative guest threads, current thread id from
 * win32_thread_current_id()):
 * - Handles are 0x500..0x5ff; object names match ASCII case-insensitively
 *   within this process and within the same object kind. A second create of
 *   the same name, and every Open*, return the same numeric handle (there is
 *   no per-handle reference count, so one close releases the name for all).
 * - Mutex: recursive for the owning thread; Wait acquires/increments,
 *   ReleaseMutex decrements, ERROR_NOT_OWNER (288) when called by a different
 *   thread.
 * - Event: manual-reset stays signaled until ResetEvent; auto-reset clears on
 *   each successful wait.
 * - Semaphore: count>0 decrements; count==0 sleeps the requested finite
 *   timeout and returns WAIT_TIMEOUT (no other guest thread can run while the
 *   caller sleeps in the one-shot thread model); an INFINITE wait returns
 *   rt_unimpl because the wait would deadlock.
 * - WaitForSingleObject also accepts a guest thread handle (signaled exactly
 *   when that thread's body has terminated) and the process pseudo-handle
 *   0xffffffff (alive, therefore never signaled). A non-thread, non-sync
 *   handle is ERROR_INVALID_HANDLE.
 * - Invalid handle -> WAIT_FAILED (0xffffffff) and ERROR_INVALID_HANDLE (6)
 *   written straight to the guest TEB at fs_base+0x34 when fs_base is
 *   nonzero. No TEB means no last-error update.
 * - WaitForMultipleObjects accepts 1..8 handles; a bad count or any bad
 *   handle fails with no side effects. bWaitAll succeeds only when every
 *   object is available, otherwise no object is consumed.
 * - Successful Create/Open clears the guest last-error to 0; create of an
 *   existing name returns the existing handle with ERROR_ALREADY_EXISTS
 *   (183); failed opens set ERROR_FILE_NOT_FOUND (2).
 * - Names longer than 63 characters fail with ERROR_INVALID_PARAMETER (87)
 *   instead of being truncated.
 * - ReleaseSemaphore saturates the count at the creation maximum (0x7fffffff
 *   when lMaximumCount was 0) and still reports success; Windows fails with
 *   ERROR_TOO_MANY_POSTS instead. The previous count is written first.
 */
