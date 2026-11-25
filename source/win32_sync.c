/* Single-thread Win32 synchronization shims.
 *
 * There is exactly one guest thread, so a wait that cannot be satisfied
 * immediately must not pretend to succeed: a zero/finite timeout reports
 * WAIT_TIMEOUT without sleeping, and an infinite wait traps. Objects live in
 * one fixed table, handles are table indices in 0x500..0x5ff, and named
 * objects resolve to the same slot so state is shared by construction. */
#include "win32_sync.h"
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__3DS__)
#include <3ds.h>
#else
#include <time.h>
#endif

#define MAX_OBJECTS WIN32_SYNC_HANDLE_COUNT
#define MAX_NAME 64u
#define MAX_WAIT_HANDLES 8u

#ifdef _WIN32
/* windows.h defines the same wait constants; keep the shim's values local. */
#undef WAIT_OBJECT_0
#undef WAIT_TIMEOUT
#undef WAIT_FAILED
#undef INFINITE
#endif

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xffffffffu
#define INFINITE_MS 0xffffffffu

#define ERR_INVALID_HANDLE 6u
#define ERR_NOT_ENOUGH_MEMORY 8u
#define ERR_FILE_NOT_FOUND 2u
#define ERR_INVALID_PARAMETER 87u
#define ERR_ALREADY_EXISTS 183u
#define ERR_NOT_OWNER 288u
#define SEM_MAX_DEFAULT 0x7fffffff

enum { OBJ_FREE = 0, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_EVENT };

typedef struct {
    uint8_t kind;
    char name[MAX_NAME];            /* empty = unnamed */
    uint32_t owner, recursion;      /* mutex: owner thread id, recursion depth */
    int32_t count, maximum;         /* semaphore */
    uint8_t manual, signaled;       /* event */
} SyncObject;

static SyncObject objects[MAX_OBJECTS];
static uint32_t current_thread = 1u;
static Win32SyncThreadQuery thread_query;
static Win32SyncBlock blocker;
static Win32SyncBlockText blocker_text;
static Win32SyncWaker waker;

uint32_t win32_sync_set_current_thread(uint32_t id) {
    uint32_t previous = current_thread;
    if (id) current_thread = id;
    return previous;
}
uint32_t win32_sync_current_thread(void) { return current_thread; }
void win32_sync_set_thread_query(Win32SyncThreadQuery query) { thread_query = query; }
void win32_sync_set_blocker(Win32SyncBlock block, Win32SyncBlockText text) {
    blocker = block;
    blocker_text = text;
}
void win32_sync_set_waker(Win32SyncWaker wake) { waker = wake; }
void win32_sync_notify(uint32_t handle) { if (waker) waker(handle); }
static void guest_sleep(uint32_t ms) {
    if (!ms) return;
#ifdef __3DS__
    svcSleepThread((int64_t)ms * 1000000);
#elif defined(_WIN32)
    Sleep(ms);
#else
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
#endif
}

static void set_error(CPU *c, uint32_t err) { if (c->fs_base) M32(c->fs_base + 0x34) = err; }
static int obj_index(uint32_t h) {
    return h >= WIN32_SYNC_HANDLE_BASE && h - WIN32_SYNC_HANDLE_BASE < MAX_OBJECTS
        ? (int)(h - WIN32_SYNC_HANDLE_BASE) : -1;
}
static SyncObject *obj(uint32_t h) {
    int i = obj_index(h);
    return i >= 0 && objects[i].kind != OBJ_FREE ? &objects[i] : NULL;
}
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
    return n > 4 && strlen(a) == n - 4 && !icmp(b + n - 4, ".dll") && icmp_prefix(a, b, n - 4);
}

/* Copy a guest object name into out (MAX_NAME bytes). Returns -1 when the
 * string is longer than 63 characters; the caller reports error 87. An empty
 * string means an unnamed object, as does a NULL pointer. */
static int name_arg(uint32_t addr, char *out) {
    out[0] = 0;
    if (!addr) return 0;
    for (unsigned i = 0; i < MAX_NAME; ++i) {
        char ch = (char)M8(addr + i);
        out[i] = ch;
        if (!ch) return 0;
    }
    out[0] = 0;
    return -1;
}
static int find_named(unsigned kind, const char *name) {
    for (unsigned i = 0; i < MAX_OBJECTS; ++i)
        if (objects[i].kind == kind && objects[i].name[0] && !icmp(objects[i].name, name))
            return (int)i;
    return -1;
}
static int new_object(unsigned kind, const char *name) {
    for (unsigned i = 0; i < MAX_OBJECTS; ++i) if (objects[i].kind == OBJ_FREE) {
        SyncObject *o = &objects[i];
        memset(o, 0, sizeof *o);
        o->kind = (uint8_t)kind;
        if (name) memcpy(o->name, name, strlen(name) + 1);
        return (int)i;
    }
    return -1;
}
static int can_acquire(const SyncObject *o) {
    switch (o->kind) {
    case OBJ_MUTEX: return o->owner == 0 || o->owner == current_thread;
    case OBJ_SEMAPHORE: return o->count > 0;
    case OBJ_EVENT: return o->signaled != 0;
    default: return 0;
    }
}
static void acquire(SyncObject *o) {
    switch (o->kind) {
    case OBJ_MUTEX: o->owner = current_thread; ++o->recursion; break;
    case OBJ_SEMAPHORE: --o->count; break;
    case OBJ_EVENT: if (!o->manual) o->signaled = 0; break;
    default: break;
    }
}
/* With no cooperative scheduler an unsatisfied wait cannot be completed by
 * another guest thread, so the honest outcome is a real timed wait; INFINITE
 * would deadlock and traps. */
static uint32_t wait_unavailable(CPU *c, uint32_t handle, uint32_t ms) {
    if (ms == INFINITE_MS)
        rt_unimpl(c, handle, "blocking wait would deadlock the single guest thread");
    guest_sleep(ms);
    return WAIT_TIMEOUT;
}
/* Thread handles are waitable: signaled exactly when the body has terminated.
 * Returns 1 for a terminated thread, 0 for a live one, -1 for a non-thread. */
static int thread_state(uint32_t handle, uint32_t *code) {
    uint32_t exit_code = 0;
    if (!thread_query) return -1;
    int rc = thread_query(handle, &exit_code);
    if (rc == 1 && code) *code = exit_code;
    return rc;
}

/* 1 when this handle can be observed signaled without consuming it (or can be
 * acquired, which the caller does). The process pseudo-handle is never
 * signaled while alive. */
static int waitable_ready(uint32_t h) {
    SyncObject *o = obj(h);
    if (o) return can_acquire(o);
    if (h == 0xffffffffu) return 0;
    return thread_state(h, NULL) == 1;
}
/* Check a whole wait set. Returns -1 when not satisfiable, otherwise the
 * WAIT_OBJECT_0 index. Consumes the acquired object(s) only on success. */
static int wait_set_ready(const uint32_t *h, unsigned n, int all) {
    if (all) {
        for (unsigned i = 0; i < n; ++i) if (!waitable_ready(h[i])) return -1;
        for (unsigned i = 0; i < n; ++i) { SyncObject *o = obj(h[i]); if (o) acquire(o); }
        return 0;
    }
    for (unsigned i = 0; i < n; ++i) {
        SyncObject *o = obj(h[i]);
        if (o) { if (can_acquire(o)) { acquire(o); return (int)i; } }
        else if (thread_state(h[i], NULL) == 1) return (int)i;
    }
    return -1;
}
/* Validate a wait set the way the old single-thread code did. Returns 0 on
 * success, 1 when some handle is invalid (WAIT_FAILED was written). */
static int validate_wait_set(CPU *c, const uint32_t *h, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (obj(h[i]) || h[i] == 0xffffffffu) continue;
        if (thread_state(h[i], NULL) < 0) { set_error(c, ERR_INVALID_HANDLE); return 1; }
    }
    return 0;
}
/* One wait loop shared by WaitForSingleObject and WaitForMultipleObjects. */
static uint32_t wait_common(CPU *c, const uint32_t *h, unsigned n, int all,
                            uint32_t ms) {
    for (;;) {
        int idx = wait_set_ready(h, n, all);
        if (idx >= 0) return WAIT_OBJECT_0 + (uint32_t)idx;
        if (ms == 0) return WAIT_TIMEOUT;
        if (!blocker) return wait_unavailable(c, h[0], ms);
        int rc = blocker(h, n, all, ms);
        if (rc == 1) return WAIT_TIMEOUT;
        if (rc == 2) {
            const char *why = blocker_text ? blocker_text() : "all guest threads blocked";
            rt_unimpl(c, h[0], why);
        }
        if (rc == 3) return wait_unavailable(c, h[0], ms);
    }
}

#define A(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v,n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)
#define FAIL(err,v,n) do { set_error(c, (err)); RET(v, n); } while (0)

static void s_CreateMutexA(CPU *c) {
    char name[MAX_NAME];
    if (name_arg(A(2), name)) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    if (name[0]) {
        int i = find_named(OBJ_MUTEX, name);
        if (i >= 0) { set_error(c, ERR_ALREADY_EXISTS); RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 3); }
    }
    int i = new_object(OBJ_MUTEX, name[0] ? name : NULL);
    if (i < 0) FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 3);
    if (A(1)) { objects[i].owner = current_thread; objects[i].recursion = 1; }
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 3);
}
static void s_OpenMutexA(CPU *c) {
    char name[MAX_NAME];
    if (name_arg(A(2), name) || !name[0]) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    int i = find_named(OBJ_MUTEX, name);
    if (i < 0) FAIL(ERR_FILE_NOT_FOUND, 0, 3);
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 3);
}
static void s_ReleaseMutex(CPU *c) {
    SyncObject *o = obj(A(0));
    if (!o || o->kind != OBJ_MUTEX) FAIL(ERR_INVALID_HANDLE, 0, 1);
    if (o->owner != current_thread) FAIL(ERR_NOT_OWNER, 0, 1);
    if (!--o->recursion) { o->owner = 0; win32_sync_notify(A(0)); }
    RET(1, 1);
}

static void s_CreateSemaphoreA(CPU *c) {
    char name[MAX_NAME];
    int32_t initial = (int32_t)A(1), maximum = (int32_t)A(2);
    if (name_arg(A(3), name)) FAIL(ERR_INVALID_PARAMETER, 0, 4);
    if (initial < 0 || maximum < 0 || (maximum && initial > maximum)) FAIL(ERR_INVALID_PARAMETER, 0, 4);
    if (name[0]) {
        int i = find_named(OBJ_SEMAPHORE, name);
        if (i >= 0) { set_error(c, ERR_ALREADY_EXISTS); RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 4); }
    }
    int i = new_object(OBJ_SEMAPHORE, name[0] ? name : NULL);
    if (i < 0) FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 4);
    objects[i].count = initial;
    objects[i].maximum = maximum ? maximum : SEM_MAX_DEFAULT;
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 4);
}
static void s_OpenSemaphoreA(CPU *c) {
    char name[MAX_NAME];
    if (name_arg(A(2), name) || !name[0]) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    int i = find_named(OBJ_SEMAPHORE, name);
    if (i < 0) FAIL(ERR_FILE_NOT_FOUND, 0, 3);
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 3);
}
static void s_ReleaseSemaphore(CPU *c) {
    SyncObject *o = obj(A(0));
    int32_t release = (int32_t)A(1), previous = A(2);
    if (!o || o->kind != OBJ_SEMAPHORE) FAIL(ERR_INVALID_HANDLE, 0, 3);
    if (release <= 0) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    if (previous) M32(previous) = (uint32_t)o->count;
    int64_t next = (int64_t)o->count + release;
    o->count = next > (int64_t)o->maximum ? o->maximum : (int32_t)next; /* saturate, see header */
    win32_sync_notify(A(0));
    RET(1, 3);
}

static void s_CreateEventA(CPU *c) {
    char name[MAX_NAME];
    if (name_arg(A(3), name)) FAIL(ERR_INVALID_PARAMETER, 0, 4);
    if (name[0]) {
        int i = find_named(OBJ_EVENT, name);
        if (i >= 0) { set_error(c, ERR_ALREADY_EXISTS); RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 4); }
    }
    int i = new_object(OBJ_EVENT, name[0] ? name : NULL);
    if (i < 0) FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 4);
    objects[i].manual = A(1) ? 1 : 0;
    objects[i].signaled = A(2) ? 1 : 0;
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 4);
}
static void s_OpenEventA(CPU *c) {
    char name[MAX_NAME];
    if (name_arg(A(2), name) || !name[0]) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    int i = find_named(OBJ_EVENT, name);
    if (i < 0) FAIL(ERR_FILE_NOT_FOUND, 0, 3);
    set_error(c, 0);
    RET(WIN32_SYNC_HANDLE_BASE + (uint32_t)i, 3);
}
static void s_SetEvent(CPU *c) {
    SyncObject *o = obj(A(0));
    if (!o || o->kind != OBJ_EVENT) FAIL(ERR_INVALID_HANDLE, 0, 1);
    o->signaled = 1;
    win32_sync_notify(A(0));
    RET(1, 1);
}
static void s_ResetEvent(CPU *c) {
    SyncObject *o = obj(A(0));
    if (!o || o->kind != OBJ_EVENT) FAIL(ERR_INVALID_HANDLE, 0, 1);
    o->signaled = 0;
    RET(1, 1);
}
int win32_sync_signal(uint32_t handle) {
    SyncObject *o = obj(handle);
    if (!o || o->kind != OBJ_EVENT) return 0;
    o->signaled = 1;
    win32_sync_notify(handle);
    return 1;
}

static void s_WaitForSingleObject(CPU *c) {
    uint32_t handle = A(0), ms = A(1);
    /* Validate before any block: a bad handle must not become a wait. */
    if (!obj(handle) && handle != 0xffffffffu && thread_state(handle, NULL) < 0)
        FAIL(ERR_INVALID_HANDLE, WAIT_FAILED, 2);
    if (handle == 0xffffffffu && ms == 0) RET(WAIT_TIMEOUT, 2);
    RET(wait_common(c, &handle, 1, 0, ms), 2);
}

static void s_WaitForMultipleObjects(CPU *c) {
    uint32_t count = A(0), list = A(1), wait_all = A(2), ms = A(3);
    uint32_t handles[MAX_WAIT_HANDLES];
    if (count == 0 || count > MAX_WAIT_HANDLES) FAIL(ERR_INVALID_PARAMETER, WAIT_FAILED, 4);
    for (uint32_t i = 0; i < count; ++i) handles[i] = M32(list + 4 * i);
    /* Validate every handle before touching any object. */
    if (validate_wait_set(c, handles, count)) RET(WAIT_FAILED, 4);
    RET(wait_common(c, handles, count, wait_all != 0, ms), 4);
}

#define DEF(X) { "kernel32.dll", #X, s_##X },
static const Win32SyncDef defs[] = {
    DEF(CreateMutexA) DEF(OpenMutexA) DEF(ReleaseMutex)
    DEF(CreateSemaphoreA) DEF(OpenSemaphoreA) DEF(ReleaseSemaphore)
    DEF(CreateEventA) DEF(OpenEventA) DEF(SetEvent) DEF(ResetEvent)
    DEF(WaitForSingleObject) DEF(WaitForMultipleObjects)
};
#undef DEF
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32SyncDef *win32_sync_defs(unsigned *count) {
    if (count) *count = (unsigned)NDEFS;
    return defs;
}
uint32_t win32_sync_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (dll_match(dll, defs[i].dll) && !strcmp(name, defs[i].name))
            return WIN32_SYNC_BASE + 4 * i;
    return 0;
}
static FnEntry entries[NDEFS];
const FnEntry *win32_sync_lookup(uint32_t addr) {
    if (addr < WIN32_SYNC_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_SYNC_BASE) / 4;
    if (i >= NDEFS) return NULL;
    entries[i] = (FnEntry){addr, defs[i].fn};
    return &entries[i];
}
int win32_sync_close(uint32_t handle) {
    int i = obj_index(handle);
    if (i < 0 || objects[i].kind == OBJ_FREE) return 0;
    memset(&objects[i], 0, sizeof objects[i]);
    return 1;
}
