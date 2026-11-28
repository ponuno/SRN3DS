/* Cooperative Win32 guest threads with real (host) stack switching.
 * The contract is documented in win32_thread.h; read that first.
 *
 * One guest thread executes at a time. A thread gives up the CPU only inside a
 * blocking shim (win32_thread_block / win32_thread_sleep) or when it terminates.
 * The scheduler keeps a FIFO of runnable threads and switches host stacks, which
 * preserves the lifted code's continuation and its local `Flags`. See
 * THREAD_MODEL.md for why a CPU snapshot is not enough.
 */
#include "win32_thread.h"
#include "win32_hostctx.h"
#include "win32_sync.h"
#include "guest_process.h" /* GP_TEB/GP_PEB/GP_TLS layout constants */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "guest.h"
#include "guest_swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef STILL_ACTIVE
#undef CREATE_SUSPENDED
#elif defined(__3DS__)
#include <3ds.h>
#endif

#define MAX_THREADS ((unsigned)WIN32_THREAD_HANDLE_COUNT)
#define THREAD_REGION_BASE 0x1c000000u
#define THREAD_REGION_STRIDE 0x00200000u /* 2 MiB apart: disjoint at max stack */
#define TEB_BYTES 0x2000u
#define STILL_ACTIVE 259u
#define ERR_INVALID_HANDLE 6u
#define ERR_NOT_ENOUGH_MEMORY 8u
#define ERR_INVALID_PARAMETER 87u
#define ERR_CALL_NOT_IMPLEMENTED 120u
#define MAIN_THREAD_ID 1u
#define THREAD_ID_BASE 2u
#define MAX_SUSPEND 0x7fffffffu
#define CREATE_SUSPENDED 0x00000004u
#define STACK_SIZE_IS_RESERVATION 0x00010000u
#define INFINITE_MS 0xffffffffu
#define MAX_WAIT 8u

enum { ST_FREE = 0, ST_READY, ST_RUNNING, ST_BLOCKED, ST_EXITED };
enum { WAIT_SIGNALED = 0, WAIT_TIMED_OUT = 1, WAIT_DEADLOCK = 2, WAIT_NO_SCHED = 3 };

typedef struct {
    uint8_t state, in_ready, close_pending, reaped, timed;
    uint32_t id, start, param, exit_code, slot, handle;
    uint32_t region_base, region_size, stack_top, teb;
    uint32_t suspends, affinity;
    int32_t priority;
    CPU cpu;
    N3dsThreadRtState rt;
    Win32HostCtx *ctx;
    uint32_t wait_h[MAX_WAIT];
    unsigned wait_n;
    int wait_all;
    int wait_status;
    uint64_t deadline, ticket, ready_seq;
} W32Thread;

static GuestMemory *mem;
static W32Thread threads[MAX_THREADS];
static W32Thread main_thread;
static W32Thread *cur;
static uint64_t g_seq, g_ticket;
static W32Thread *reap_pending;
static W32Thread *deadlock_self;
static int scheduler_up;
static char deadlock[256];
static uint8_t teb_template[TEB_BYTES];
static uint32_t tls_base, tls_size;
static int teb_ready;

/* ---- scheduler counters (N3DS_SCHED_TRACE / hang dump) ------------------ */
/* These answer the starvation question directly: how often the executor's
 * yield hook fired, how often it actually switched, and whether a READY main
 * lost pick_ready.  Without them a hang dump can only show the final state. */
static unsigned long long g_yield_enter;      /* win32_thread_yield entries */
static unsigned long long g_yield_switch;     /* ... that switched away */
static unsigned long long g_pick_calls;       /* pick_ready invocations */
static unsigned long long g_pick_main_skipped;/* main READY, another chosen */
static unsigned long long g_ready_orphans;    /* ST_READY with in_ready==0 */
static unsigned long long g_switch_count;
static unsigned g_trace_on = 2;               /* 2 = unread */
static volatile uint64_t g_activity_ms;        /* last scheduler event (ms) */
static int sched_trace_on(void) {
    if (g_trace_on == 2) {
        const char *v = getenv("N3DS_SCHED_TRACE");
        g_trace_on = (v && *v && strcmp(v, "0")) ? 1 : 0;
    }
    return (int)g_trace_on;
}
static void dump_ready_queue(FILE *f) {
    fprintf(f, "[hang] ready queue:");
    unsigned n = 0;
    if (scheduler_up && main_thread.state == ST_READY) {
        fprintf(f, " main(seq=%llu,in=%d,susp=%u)",
                (unsigned long long)main_thread.ready_seq, main_thread.in_ready,
                main_thread.suspends);
        ++n;
    }
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        W32Thread *t = &threads[i];
        if (t->state != ST_READY) continue;
        fprintf(f, " %lu(seq=%llu,in=%d,susp=%u)", (unsigned long)t->id,
                (unsigned long long)t->ready_seq, t->in_ready, t->suspends);
        ++n;
    }
    fprintf(f, " count=%u g_seq=%llu\n", n, (unsigned long long)g_seq);
}
static void dump_sched(FILE *f) {
    uint64_t now_ms = win32_thread_now_ms();
    int64_t gap = (int64_t)(now_ms - g_activity_ms);
    if (gap < 0) gap = 0;
    fprintf(f, "[hang] sched: hook=%d yields=%llu switched=%llu picks=%llu "
               "main_skipped=%llu ready_orphans=%llu switches=%llu gap_ms=%llu "
               "ir_blocks=%llu ir_yields=%llu poll_yields=%llu last_block=%08x "
               "block_age_ms=%u longest_block_ms=%u\n",
            n3ds_ir_yield_hook ? 1 : 0, g_yield_enter, g_yield_switch,
            g_pick_calls, g_pick_main_skipped, g_ready_orphans, g_switch_count,
            (unsigned long long)gap,
            n3ds_ir_blocks_run, n3ds_ir_yield_calls, n3ds_ir_poll_yields,
            (unsigned)n3ds_ir_current_block_ip(), n3ds_ir_block_age_ms(),
            n3ds_ir_longest_block_ms);
    dump_ready_queue(f);
}

static uint32_t tid_of(unsigned slot) { return THREAD_ID_BASE + slot; }
static int slot_of(uint32_t handle, unsigned *slot) {
    if (handle < WIN32_THREAD_HANDLE_BASE) return 0;
    handle -= WIN32_THREAD_HANDLE_BASE;
    if (handle >= MAX_THREADS) return 0;
    *slot = handle;
    return 1;
}
static W32Thread *thread_for(uint32_t handle) {
    unsigned slot;
    return slot_of(handle, &slot) && threads[slot].state != ST_FREE ? &threads[slot] : NULL;
}
static W32Thread *thread_for_id(uint32_t id) {
    if (id == MAIN_THREAD_ID) return scheduler_up ? &main_thread : NULL;
    for (unsigned i = 0; i < MAX_THREADS; ++i)
        if (threads[i].state != ST_FREE && threads[i].id == id) return &threads[i];
    return NULL;
}
static W32Thread *created_thread_for_id(uint32_t id) {
    for (unsigned i = 0; i < MAX_THREADS; ++i)
        if (threads[i].state != ST_FREE && threads[i].id == id) return &threads[i];
    return NULL;
}
static void set_error(CPU *c, uint32_t err) {
    if (c && c->fs_base) M32(c->fs_base + 0x34) = err;
    win32_set_last_error(err);
}
#define A(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)
#define FAIL(err, v, n) do { set_error(c, (err)); RET(v, n); } while (0)

/* ---- clock ------------------------------------------------------------- */
uint64_t win32_thread_now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000) / freq.QuadPart);
#elif defined(__3DS__)
    return (uint64_t)svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000u);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}
static void host_sleep_ms(uint32_t ms) {
    if (!ms) return;
#ifdef _WIN32
    Sleep(ms);
#elif defined(__3DS__)
    svcSleepThread((int64_t)ms * 1000000);
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
#endif
}

/* ---- hang watchdog (diagnostic, off by default) ------------------------- */
/* Progress is a scheduler event, not a guest instruction: a cooperative spin
 * inside one lifted function keeps executing code but never yields, sleeps or
 * calls a shim, so wall-clock since the last switch is the honest signal. */
/* last scheduler event (ms); defined with the counters above */
static volatile int g_watchdog_running;
static volatile int g_watchdog_armed;
static unsigned g_watchdog_ms;
static unsigned g_dump_after_ms;
static void activity(void) { g_activity_ms = win32_thread_now_ms(); }

/* ---- last blocking shim call site (diagnostics only) -------------------- */
/* A timed block with an empty handle set (Sleep, the critical-section spin)
 * leaves no trace in the wait object table.  Record the caller so the hang
 * dump can name it instead of printing "wait_n=0". */
#define WAIT_NOTE_MAX (1 + MAX_THREADS)
typedef struct {
    uint32_t id;
    uint32_t caller, a0, a1, ms;
    char what[20];
} WaitNote;
static WaitNote wait_notes[WAIT_NOTE_MAX];
static unsigned wait_note_n;
int n3ds_frame_trace_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("N3DS_FRAME_TRACE"); v = e && *e && strcmp(e, "0"); }
    return v;
}
static WaitNote *wait_note_for(uint32_t id, int create) {
    for (unsigned i = 0; i < wait_note_n; ++i)
        if (wait_notes[i].id == id) return &wait_notes[i];
    if (!create || wait_note_n >= WAIT_NOTE_MAX) return NULL;
    WaitNote *n = &wait_notes[wait_note_n++];
    memset(n, 0, sizeof *n);
    n->id = id;
    return n;
}
void win32_thread_wait_note(const char *what, uint32_t caller, uint32_t a0,
                            uint32_t a1, uint32_t ms) {
    uint32_t id = cur ? cur->id : MAIN_THREAD_ID;
    WaitNote *n = wait_note_for(id, 1);
    if (!n) return;
    n->caller = caller; n->a0 = a0; n->a1 = a1; n->ms = ms;
    snprintf(n->what, sizeof n->what, "%s", what ? what : "?");
}
static const char *wait_note_text(uint32_t id) {
    static char buf[160];
    WaitNote *n = wait_note_for(id, 0);
    if (!n) return "";
    snprintf(buf, sizeof buf, " note=%s caller=%08x a0=%08x a1=%08x ms=%lu",
             n->what, (unsigned)n->caller, (unsigned)n->a0, (unsigned)n->a1,
             (unsigned long)n->ms);
    return buf;
}
/* Everything the watchdog is willing to call progress, sampled cheaply enough
 * for the 50 ms monitor loop. See N3dsWatchdogSample in win32_thread.h. */
static N3dsWatchdogSample watchdog_sample(void) {
    N3dsWatchdogSample s;
    s.blocks = n3ds_ir_blocks_run;
    s.io_bytes = n3ds_io_bytes_total();
    s.ir_ip = n3ds_lift_current_ip();
    s.last_call = n3ds_lift_last_call();
    return s;
}

static const char *state_name(unsigned s) {
    switch (s) {
    case ST_FREE: return "free";
    case ST_READY: return "ready";
    case ST_RUNNING: return "running";
    case ST_BLOCKED: return "blocked";
    case ST_EXITED: return "exited";
    default: return "?";
    }
}
static void dump_thread(const char *role, W32Thread *t) {
    if (!t || t->state == ST_FREE) return;
    fprintf(stderr, "[hang] %s id=%lu state=%s handle=%03x start=%08x "
                    "esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x "
                    "esi=%08x edi=%08x irc_depth=%lu",
            role, (unsigned long)t->id, state_name(t->state), (unsigned)t->handle,
            (unsigned)t->start, (unsigned)t->cpu.r[4], (unsigned)t->cpu.r[5],
            (unsigned)t->cpu.r[0], (unsigned)t->cpu.r[3], (unsigned)t->cpu.r[1],
            (unsigned)t->cpu.r[2], (unsigned)t->cpu.r[6], (unsigned)t->cpu.r[7],
            (unsigned long)t->rt.irc_depth);
    fprintf(stderr, " suspends=%u in_ready=%d seq=%llu", t->suspends, t->in_ready,
            (unsigned long long)t->ready_seq);
    if (t->state == ST_BLOCKED) {
        fprintf(stderr, " wait_n=%u all=%d timed=%u status=%d wait=",
                t->wait_n, t->wait_all, (unsigned)t->timed, t->wait_status);
        for (unsigned k = 0; k < t->wait_n; ++k)
            fprintf(stderr, "%s%03x", k ? "," : "", (unsigned)t->wait_h[k]);
        if (t->timed) {
            uint64_t now = win32_thread_now_ms();
            fprintf(stderr, " deadline=%llu left=%lld",
                    (unsigned long long)t->deadline,
                    (long long)((int64_t)t->deadline - (int64_t)now));
        }
    }
    fprintf(stderr, "%s", wait_note_text(t->id));
    fputc('\n', stderr);
    uint32_t esp = t->cpu.r[4];
    if (!mem || !esp) return;
    uint32_t raw[8] = {0};
    for (unsigned k = 0; k < 8; ++k) (void)guest_read32(mem, esp + 4 * k, &raw[k]);
    fprintf(stderr, "[hang]   stack[%08x..]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            (unsigned)esp, (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2],
            (unsigned)raw[3], (unsigned)raw[4], (unsigned)raw[5], (unsigned)raw[6],
            (unsigned)raw[7]);
    unsigned shown = 0;
    fprintf(stderr, "[hang]   image ret:");
    for (unsigned k = 0; k < 128 && shown < 16; ++k) {
        uint32_t v = 0;
        if (guest_read32(mem, esp + 4 * k, &v)) break;
        if (v >= 0x00400000u && v < 0x0106a310u) {
            fprintf(stderr, " [%08x]=%08x", (unsigned)(esp + 4 * k), (unsigned)v);
            ++shown;
        }
    }
    fputc('\n', stderr);
}
static void hang_dump(unsigned elapsed, const char *why, int is_hang) {
    fprintf(stderr, "[hang] %s: %s for %lu ms "
                    "(TESV_HANG_WATCHDOG_MS=%u); dumping every guest thread\n",
            is_hang ? "Guest hang" : "Diagnostic dump",
            why ? why : "no scheduling progress",
            (unsigned long)elapsed, g_watchdog_ms);
    fprintf(stderr, "[hang] current=%lu last_call=%08x ir_ip=%08x live_threads=%u\n",
            (unsigned long)(cur ? cur->id : 0), (unsigned)n3ds_lift_last_call(),
            (unsigned)n3ds_lift_current_ip(), win32_thread_live_count());
    dump_sched(stderr);
    n3ds_lift_dump_tiers(stderr);
    if (scheduler_up) dump_thread("main", &main_thread);
    for (unsigned i = 0; i < MAX_THREADS; ++i)
        if (threads[i].state != ST_FREE) dump_thread("thread", &threads[i]);
    n3ds_lift_dump_recent_calls(stderr);
    n3ds_lift_dump_stream(stderr);
    n3ds_io_dump(stderr);
    win32_sync_dump(stderr);
    win32_cs_dump(stderr);
    win32_icx_dump(stderr);
    fflush(stderr);
}
#ifdef _WIN32
static DWORD WINAPI watchdog_proc(LPVOID unused) {
    (void)unused;
    uint64_t start = win32_thread_now_ms();
    uint64_t last_trace = start;
    N3dsWatchdogSample last_sample = watchdog_sample();
    for (;;) {
        Sleep(50);
        uint64_t now = win32_thread_now_ms();
        int64_t gap = (int64_t)(now - g_activity_ms);
        if (gap < 0) gap = 0;
        /* Scheduling events are not the only progress: a lone runnable thread
         * yields to itself and a host-side asset job can run for minutes.  Any
         * completed IR block, moved PC, or new ReadFile byte is progress and
         * refreshes the activity clock, so only a thread frozen inside ONE
         * block still trips the watchdog. */
        N3dsWatchdogSample sample = watchdog_sample();
        int progressed = n3ds_watchdog_progressed(&last_sample, &sample);
        if (progressed) {
            g_activity_ms = now;
            last_sample = sample;
        }
        if (sched_trace_on() && now - last_trace >= 1000) {
            last_trace = now;
            fprintf(stderr, "[sched] t=%llums cur=%lu activity_age=%llums "
                            "blocks=%llu ir_yields=%llu poll_yields=%llu "
                            "hook_yields=%llu switches=%llu via_yield=%llu "
                            "picks=%llu main_skip=%llu block=%08x age_ms=%u "
                            "longest=%u\n",
                    (unsigned long long)(now - start),
                    (unsigned long)(cur ? cur->id : 0),
                    (unsigned long long)(gap > 0 ? (uint64_t)gap : 0),
                    n3ds_ir_blocks_run, n3ds_ir_yield_calls, n3ds_ir_poll_yields,
                    g_yield_enter, g_switch_count, g_yield_switch, g_pick_calls,
                    g_pick_main_skipped, (unsigned)n3ds_ir_current_block_ip(),
                    n3ds_ir_block_age_ms(), n3ds_ir_longest_block_ms);
            fflush(stderr);
        }
        if (g_dump_after_ms && now - start > g_dump_after_ms) {
            fprintf(stderr, "[hang] timed diagnostic dump after %llu ms "
                            "(TESV_DUMP_AFTER_MS=%u, progress=%s)\n",
                    (unsigned long long)(now - start), g_dump_after_ms,
                    progressed ? "yes" : "no");
            hang_dump((unsigned)(now - start), "timed diagnostic dump", 0);
            fprintf(stderr, "[hang] timed dump aborting the run (diagnostic only)\n");
            fflush(stderr);
            ExitProcess(70);
        }
        uint64_t last = g_activity_ms;
        /* A guest thread can stamp activity() at a later millisecond than the
         * `now` sampled above (the clock advances between the two reads), which
         * makes `now - last` an enormous unsigned value and fires a bogus
         * "hang". Compare signed and never report a negative gap. */
        int64_t hang_gap = (int64_t)(now - last);
        if (g_watchdog_armed && hang_gap > (int64_t)g_watchdog_ms) {
            hang_dump((unsigned)hang_gap, "no guest progress", 1);
            fprintf(stderr, "[hang] watchdog aborting the run (diagnostic only)\n");
            fflush(stderr);
            ExitProcess(70);
        }
    }
}
#endif
static void watchdog_start(void) {
    if (g_watchdog_running) return;
    const char *v = getenv("TESV_HANG_WATCHDOG_MS");
    const char *d = getenv("TESV_DUMP_AFTER_MS");
    unsigned long ms = v && *v ? strtoul(v, NULL, 0) : 0;
    unsigned long dms = d && *d ? strtoul(d, NULL, 0) : 0;
    if (!ms && !dms) return;
    if (ms) g_watchdog_ms = ms > 3600000ul ? 3600000u : (unsigned)ms;
    if (dms) g_dump_after_ms = dms > 3600000ul ? 3600000u : (unsigned)dms;
    g_watchdog_running = 1;
    activity();
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    fprintf(stderr, "[hang] TESV_HANG_WATCHDOG_MS/TESV_DUMP_AFTER_MS are host-only; not started\n");
#endif
}
/* Arm after the first real Present: loading has legitimate long gaps between
 * scheduler events, while the menu loop after Present must keep presenting. */
void n3ds_hang_watchdog_arm(void) {
    if (!g_watchdog_running || g_watchdog_armed || !g_watchdog_ms) return;
    activity();
    g_watchdog_armed = 1;
}

/* ---- ready queue ------------------------------------------------------- */
/* The single place a thread becomes runnable.  Every ST_READY transition --
 * main's Sleep(0), a wake from block_current(), ResumeThread/activate(), a new
 * created thread -- is stamped here with the next FIFO sequence.  A thread that
 * is already runnable keeps its place rather than being pushed to the back. */
static void ready_stamp(W32Thread *t) {
    t->state = ST_READY;
    t->in_ready = 1;
    t->ready_seq = ++g_seq;
}
static void enqueue(W32Thread *t) {
    if (!t || t->state == ST_FREE || t->state == ST_EXITED) return;
    if (t->state == ST_READY && t->in_ready) return;
    ready_stamp(t);
}
/* One FIFO over threads[] and main_thread, ordered by ready_seq.  main has no
 * permanent priority: it competes like every worker.  On an exact tie main is
 * chosen first (deterministic, and it is the thread that owns process control),
 * but a tie cannot recur because each enqueue stamps a fresh sequence. */
static W32Thread *pick_ready(void) {
    W32Thread *cand[1 + MAX_THREADS];
    unsigned n = 0;
    ++g_pick_calls;
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        W32Thread *t = &threads[i];
        if (t->state == ST_READY && !t->in_ready) ++g_ready_orphans;
        if (t->state != ST_READY || t->suspends) continue;
        if (!t->ready_seq) t->ready_seq = ++g_seq; /* never let 0 win */
        cand[n++] = t;
    }
    if (scheduler_up && main_thread.state == ST_READY && main_thread.suspends == 0) {
        if (!main_thread.in_ready) ++g_ready_orphans;
        if (!main_thread.ready_seq) main_thread.ready_seq = ++g_seq;
        cand[n++] = &main_thread;
    }
    W32Thread *best = NULL;
    for (unsigned i = 0; i < n; ++i) {
        W32Thread *t = cand[i];
        if (!best || t->ready_seq < best->ready_seq ||
            (t->ready_seq == best->ready_seq && t == &main_thread))
            best = t;
    }
    if (scheduler_up && main_thread.state == ST_READY && best && best != &main_thread)
        ++g_pick_main_skipped;
    return best;
}
static void reap_now(void) {
    W32Thread *t = reap_pending;
    if (!t || t == cur) return;
    reap_pending = NULL;
    if (t->ctx) { win32_hostctx_destroy(t->ctx); t->ctx = NULL; }
    if (mem && t->region_base) { guest_unmap(mem, t->region_base); t->region_base = 0; }
    t->reaped = 1;
    if (t->close_pending) memset(t, 0, sizeof *t);
}

static int trace_on(void) { static int v = -1; if (v < 0) v = getenv("THREAD_TRACE") != NULL; return v; }
#define SCHED_TR(...) do { if (trace_on()) fprintf(stderr, __VA_ARGS__); } while (0)

static void switch_to(W32Thread *n) {
    W32Thread *prev = cur;
    if (!n || !prev) abort();
    activity();
    ++g_switch_count;
    SCHED_TR("[sched] %u(%d,%llu) -> %u(%d,%llu)\n", prev->id, prev->state,
             (unsigned long long)prev->ready_seq, n->id, n->state,
             (unsigned long long)n->ready_seq);
    n->state = ST_RUNNING;
    n->in_ready = 0;
    cur = n;
    win32_sync_set_current_thread(n->id);
    n3ds_lift_thread_switch(&prev->rt, &n->rt);
    win32_hostctx_switch(prev->ctx, n->ctx);
    /* Resumed: another thread switched back to us. Free a stack whose thread
     * terminated before that switch. Safe here: it is not our own stack. */
    reap_now();
}

/* Wake timers / idle the host when nothing is runnable. Returns 1 when a
 * deadline exists (even if it has not expired yet), 0 when every blocked thread
 * waits forever. */
static int idle_until_deadline(void) {
    uint64_t best = UINT64_MAX;
    int any = 0;
    if (scheduler_up && main_thread.state == ST_BLOCKED && main_thread.timed) {
        best = main_thread.deadline;
        any = 1;
    }
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        W32Thread *t = &threads[i];
        if (t->state == ST_BLOCKED && t->timed && t->deadline < best) { best = t->deadline; any = 1; }
    }
    if (!any) return 0;
    uint64_t now = win32_thread_now_ms();
    if (best > now) {
        uint64_t wait = best - now;
        if (wait > 50) wait = 50;
        host_sleep_ms((uint32_t)wait);
        now = win32_thread_now_ms();
    }
    if (scheduler_up && main_thread.state == ST_BLOCKED && main_thread.timed &&
        main_thread.deadline <= now) {
        main_thread.wait_status = WAIT_TIMED_OUT;
        enqueue(&main_thread);
    }
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        W32Thread *t = &threads[i];
        if (t->state == ST_BLOCKED && t->timed && t->deadline <= now) {
            t->wait_status = WAIT_TIMED_OUT;
            enqueue(t);
        }
    }
    return 1;
}

static void describe_deadlock(void) {
    size_t at = 0;
    at += (size_t)snprintf(deadlock + at, sizeof deadlock - at,
                           "all guest threads blocked (no runnable thread)");
    W32Thread *all[2 + MAX_THREADS];
    unsigned n = 0;
    if (deadlock_self) all[n++] = deadlock_self;
    if (scheduler_up && main_thread.state == ST_BLOCKED) all[n++] = &main_thread;
    for (unsigned i = 0; i < MAX_THREADS; ++i)
        if (threads[i].state == ST_BLOCKED) all[n++] = &threads[i];
    for (unsigned i = 0; i < n && at + 24 < sizeof deadlock; ++i)
        at += (size_t)snprintf(deadlock + at, sizeof deadlock - at, "%sthread %lu on handle 0x%lx",
                               i ? ", " : ": ", (unsigned long)all[i]->id,
                               all[i]->wait_n ? (unsigned long)all[i]->wait_h[0] : 0ul);
}

/* Block the current thread until `handles` may be ready / the deadline. */
static int block_current(const uint32_t *handles, unsigned count, int wait_all,
                         uint32_t timeout_ms) {
    W32Thread *t = cur;
    if (!t) return WAIT_NO_SCHED;
    activity();
    if (n3ds_frame_trace_on() && t == &main_thread) {
        static unsigned logged;
        if (logged < 200) {
            ++logged;
            fprintf(stderr, "[frame] main block n=%u ms=%lu", count,
                    (unsigned long)timeout_ms);
            for (unsigned i = 0; i < count; ++i) fprintf(stderr, " h=%03x", (unsigned)handles[i]);
            fputc('\n', stderr);
        }
    }
    if (t->suspends) return WAIT_DEADLOCK; /* cannot be resumed while blocked */
    if (count > MAX_WAIT) count = MAX_WAIT;
    memset(t->wait_h, 0, sizeof t->wait_h);
    for (unsigned i = 0; i < count; ++i) t->wait_h[i] = handles[i];
    t->wait_n = count;
    t->wait_all = wait_all;
    t->ticket = ++g_ticket;
    t->wait_status = -1;
    t->timed = timeout_ms != INFINITE_MS;
    t->deadline = t->timed ? win32_thread_now_ms() + timeout_ms : 0;
    t->state = ST_BLOCKED;
    t->in_ready = 0;
    deadlock_self = NULL;
    for (;;) {
        W32Thread *n = pick_ready();
        if (n) {
            if (n == t) { t->state = ST_RUNNING; break; } /* own timeout fired */
            switch_to(n);
            if (t->state == ST_RUNNING) break;
            continue;
        }
        if (idle_until_deadline()) continue;
        if (t != &main_thread && scheduler_up && main_thread.state == ST_BLOCKED) {
            /* Make the main thread runnable so its wait reports the deadlock in
             * the main frame; a worker fault could otherwise leave no thread
             * able to report it. */
            main_thread.wait_status = WAIT_DEADLOCK;
            enqueue(&main_thread);
            switch_to(&main_thread);
            continue;
        }
        t->state = ST_RUNNING;
        t->wait_n = count;
        deadlock_self = t;
        return WAIT_DEADLOCK;
    }
    int st = t->wait_status;
    t->wait_status = -1;
    t->wait_n = count; /* keep the wait set for the deadlock message */
    if (st == -1) st = WAIT_SIGNALED;
    if (st == WAIT_DEADLOCK) deadlock_self = t;
    else t->wait_n = 0;
    activity();
    return st;
}

int win32_thread_scheduler_ready(void) { return scheduler_up; }
int win32_thread_block(const uint32_t *handles, unsigned count, int wait_all,
                       uint32_t timeout_ms) {
    if (!scheduler_up) return WAIT_NO_SCHED;
    return block_current(handles, count, wait_all, timeout_ms);
}
const char *win32_thread_deadlock_text(void) {
    describe_deadlock();
    return deadlock;
}

/* ---- public scheduler hooks ------------------------------------------- */
void win32_thread_wake(uint32_t handle) {
    activity();
    W32Thread *hit[1 + MAX_THREADS];
    unsigned n = 0;
    if (scheduler_up && main_thread.state == ST_BLOCKED) {
        for (unsigned k = 0; k < main_thread.wait_n; ++k)
            if (main_thread.wait_h[k] == handle) { hit[n++] = &main_thread; break; }
    }
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        W32Thread *t = &threads[i];
        if (t->state != ST_BLOCKED) continue;
        for (unsigned k = 0; k < t->wait_n; ++k)
            if (t->wait_h[k] == handle) { hit[n++] = t; break; }
    }
    /* FIFO: the oldest waiter is scheduled first. */
    for (unsigned i = 1; i < n; ++i) {
        W32Thread *v = hit[i];
        unsigned j = i;
        while (j && hit[j - 1]->ticket > v->ticket) { hit[j] = hit[j - 1]; --j; }
        hit[j] = v;
    }
    for (unsigned i = 0; i < n; ++i) {
        hit[i]->wait_status = WAIT_SIGNALED;
        enqueue(hit[i]);
    }
    if (n3ds_frame_trace_on()) {
        static unsigned logged;
        for (unsigned i = 0; i < n; ++i)
            if (hit[i] == &main_thread && logged < 200) {
                ++logged;
                fprintf(stderr, "[frame] main wake h=%03x thread=%lu\n", (unsigned)handle,
                        (unsigned long)(cur ? cur->id : 0));
            }
    }
    if (n || handle == 0x537u || handle == 0x504u)
        fprintf(stderr, "[wake] h=%03x waiters=%u thread=%lu\n", (unsigned)handle, n,
                (unsigned long)(cur ? cur->id : 0));
}

void win32_thread_yield(void) {
    if (!scheduler_up) return;
    activity();
    ++g_yield_enter;
    W32Thread *self = cur;
    if (!self) return;
    enqueue(self);
    W32Thread *n = pick_ready();
    if (!n || n == self) { self->state = ST_RUNNING; self->in_ready = 0; return; }
    ++g_yield_switch;
    switch_to(n);
}

void win32_thread_sleep(uint32_t ms) {
    activity();
    if (!scheduler_up) { host_sleep_ms(ms); return; }
    if (ms == 0) { win32_thread_yield(); return; }
    block_current(NULL, 0, 0, ms);
}

unsigned win32_thread_live_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_THREADS; ++i)
        if (threads[i].state == ST_READY || threads[i].state == ST_RUNNING ||
            threads[i].state == ST_BLOCKED) ++n;
    return n;
}
int win32_thread_memory(unsigned slot, uint32_t *guest_bytes, size_t *host_bytes) {
    if (slot >= MAX_THREADS || threads[slot].state == ST_FREE) return -1;
    if (guest_bytes) *guest_bytes = threads[slot].region_size;
    if (host_bytes) *host_bytes = WIN32_THREAD_HOST_STACK;
    return 0;
}
size_t win32_thread_struct_size(void) { return sizeof(W32Thread); }

uint64_t win32_thread_ready_seq(uint32_t id) {
    W32Thread *t = thread_for_id(id);
    return t ? t->ready_seq : 0;
}

/* ---- thread body trampoline ------------------------------------------- */
static void thread_trampoline(void *arg) {
    W32Thread *t = arg;
    n3ds_lift_thread_begin(&t->rt);
    uint32_t parameter = t->param, result = 0;
    int rc = n3ds_lift_invoke_reentrant(&t->cpu, t->start, &parameter, 1, &result);
    if (rc == 0 || rc == N3DS_INVOKE_EXIT) {
        t->exit_code = result;
    } else {
        t->exit_code = WIN32_THREAD_EXIT_FAULT;
        fprintf(stderr, "[thread] id=%lu start=%08lx terminated without returning (%s)\n",
                (unsigned long)t->id, (unsigned long)t->start,
                rc == N3DS_INVOKE_TRAP ? n3ds_lift_fault() : "invalid call frame");
    }
    t->state = ST_EXITED;
    t->in_ready = 0;
    if (n3ds_frame_trace_on()) {
        fprintf(stderr, "[frame] thread id=%lu exited code=%08x\n",
                (unsigned long)t->id, (unsigned)t->exit_code);
        win32_cs_thread_exit(t->id);
    }
    win32_thread_wake(t->handle);
    reap_pending = t;
    for (;;) {
        W32Thread *n = pick_ready();
        if (n) { switch_to(n); continue; }
        if (idle_until_deadline()) continue;
        if (scheduler_up && main_thread.state == ST_BLOCKED) {
            main_thread.wait_status = WAIT_DEADLOCK;
            enqueue(&main_thread);
            switch_to(&main_thread);
            continue;
        }
        fprintf(stderr, "[thread] no runnable guest thread and the main thread is not "
                        "blocked; the process cannot continue\n");
        abort();
    }
}

/* Switch into a READY thread and let it run. Returns when it blocks or exits. */
static void activate(W32Thread *t) {
    if (!t || t->state != ST_READY) return;
    W32Thread *self = cur;
    if (!self) return;
    if (t == self) { self->state = ST_RUNNING; return; }
    if (!t->in_ready) enqueue(t);
    enqueue(self); /* keep the creator runnable; it is scheduled after t */
    switch_to(t);
}

/* ---- lifecycle --------------------------------------------------------- */
/* Executor time-slice tick (ir_core.h).  Only switches when the scheduler is
 * up and a running guest thread exists, so an isolated test or a pre-init
 * caller is unaffected. */
static void guest_preempt_tick(void) {
    if (scheduler_up && cur) {
        /* Only place guest pages are evicted: between IR/AOT blocks, where the
         * lifted code holds no raw guest pointer.  Never from guest_ptr(). */
        guest_swap_safe_point(mem);
        win32_thread_yield();
    }
}

int win32_thread_init(GuestMemory *m) {
    if (!m || mem) return -1;
    mem = m;
    memset(threads, 0, sizeof threads);
    memset(&main_thread, 0, sizeof main_thread);
    memset(teb_template, 0, sizeof teb_template);
    tls_base = tls_size = 0;
    teb_ready = 0;
    void *teb = guest_ptr(m, GP_TEB, TEB_BYTES);
    if (teb) {
        memcpy(teb_template, teb, TEB_BYTES);
        teb_ready = 1;
    }
    for (size_t i = 0; i < m->count; ++i)
        if (m->regions[i].base == GP_TLS) { tls_base = GP_TLS; tls_size = m->regions[i].size; }
    if (win32_hostctx_init()) return -1;
    main_thread.id = MAIN_THREAD_ID;
    main_thread.state = ST_RUNNING;
    main_thread.slot = ~0u;
    if (win32_hostctx_main(&main_thread.ctx)) return -1;
    n3ds_lift_thread_state_init(&main_thread.rt);
    cur = &main_thread;
    scheduler_up = 1;
    g_seq = g_ticket = 0;
    /* main is RUNNING now, but it takes a real FIFO stamp (not 0) so its first
     * ST_READY transition cannot masquerade as an infinite-priority thread. */
    main_thread.ready_seq = ++g_seq;
    reap_pending = NULL;
    win32_sync_set_current_thread(MAIN_THREAD_ID);
    win32_sync_set_waker(win32_thread_wake);
    win32_sync_set_note(win32_thread_wait_note);
    /* Diagnostic PC ring (N3DS_PC_RING=1).  Installed here so the IR executors
     * keep linking without guest.c/win32_thread.c in standalone tools. */
    n3ds_ir_pc_note_hook = guest_pc_ring_note;
    n3ds_ir_thread_id_hook = win32_thread_current_id;
    /* Time-slice guest IR execution.  The executors call this between blocks so
     * a guest loop with no blocking call cannot own the CPU: TESV's stream/menu
     * worker spins in a hash walk and otherwise starves the init thread and
     * main's pump forever.  Installed here because only the scheduler knows
     * when yielding is meaningful (and the containers also run without it). */
    n3ds_ir_yield_hook = guest_preempt_tick;
    watchdog_start();
    return 0;
}
void win32_thread_shutdown(void) {
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].state != ST_FREE && mem && threads[i].region_base)
            guest_unmap(mem, threads[i].region_base);
        if (threads[i].ctx) win32_hostctx_destroy(threads[i].ctx);
        memset(&threads[i], 0, sizeof threads[i]);
    }
    if (main_thread.ctx) win32_hostctx_destroy(main_thread.ctx);
    memset(&main_thread, 0, sizeof main_thread);
    reap_pending = NULL;
    scheduler_up = 0;
    cur = NULL;
    win32_sync_set_waker(NULL);
    win32_sync_set_note(NULL);
    mem = NULL;
    teb_ready = 0;
}
uint32_t win32_thread_current_id(void) { return cur ? cur->id : MAIN_THREAD_ID; }
int win32_thread_is_handle(uint32_t handle) { return thread_for(handle) != NULL; }
int win32_thread_signaled(uint32_t handle) {
    W32Thread *t = thread_for(handle);
    return t && t->state == ST_EXITED;
}
uint32_t win32_thread_exit_code(uint32_t handle) {
    W32Thread *t = thread_for(handle);
    return t && t->state == ST_EXITED ? t->exit_code : STILL_ACTIVE;
}
int win32_thread_query(uint32_t handle, uint32_t *exit_code) {
    W32Thread *t = thread_for(handle);
    if (!t) return -1;
    if (t->state != ST_EXITED) return 0;
    if (exit_code) *exit_code = t->exit_code;
    return 1;
}
int win32_thread_close(uint32_t handle) {
    W32Thread *t = thread_for(handle);
    if (!t) return 0;
    if (t == cur || t->state == ST_READY || t->state == ST_BLOCKED) {
        /* Windows keeps a live thread alive after its handle is closed. */
        t->close_pending = 1;
        return 1;
    }
    if (t->ctx) { win32_hostctx_destroy(t->ctx); t->ctx = NULL; }
    if (mem && t->region_base) guest_unmap(mem, t->region_base);
    if (reap_pending == t) reap_pending = NULL;
    memset(t, 0, sizeof *t);
    return 1;
}

/* Map stack | TEB | private TLS copy as one guest region and build the TEB.
 * Returns 0 on success. */
static int build_thread_memory(W32Thread *t, uint32_t stack_bytes) {
    uint64_t need = (uint64_t)stack_bytes + TEB_BYTES + tls_size;
    need = (need + 0xfff) & ~UINT64_C(0xfff);
    if (need > 0xffffffffu) return -1;
    unsigned slot = (unsigned)(t - threads);
    uint32_t base = THREAD_REGION_BASE + slot * THREAD_REGION_STRIDE;
    if (guest_map(mem, base, (uint32_t)need)) return -1;
    t->region_base = base;
    t->region_size = (uint32_t)need;
    t->teb = base + stack_bytes;
    t->stack_top = base + stack_bytes - 64;
    uint8_t *teb = guest_ptr(mem, t->teb, TEB_BYTES);
    if (!teb) { guest_unmap(mem, base); t->region_base = 0; return -1; }
    if (teb_ready) memcpy(teb, teb_template, TEB_BYTES);
    else memset(teb, 0, TEB_BYTES);
    uint32_t new_tls = t->teb + TEB_BYTES;
    if (tls_size) {
        void *src = guest_ptr(mem, tls_base, tls_size);
        void *dst = guest_ptr(mem, new_tls, tls_size);
        if (!src || !dst) { guest_unmap(mem, base); t->region_base = 0; return -1; }
        memcpy(dst, src, tls_size);
    }
    /* TEB fields used by the shims and by MSVC SEH/CRT code.  The field
     * layout is the NT one, not a port convenience: +0x00 is ExceptionList
     * (the head of the SEH chain), +0x18 Self, +0x24/+0x28 the ClientId.
     * Writing the thread id at +0x00 (as this used to) left every new guest
     * thread with a garbage SEH chain: the first `__try` prologue linked its
     * registration to a small integer, and the unwinder then decoded garbage
     * frame pointers (TESV died in __CxxFrameHandler with a bogus this). */
    M32(t->teb + 0x00) = 0xffffffffu; /* ExceptionList terminator */
    M32(t->teb + 0x04) = t->stack_top;
    M32(t->teb + 0x08) = t->region_base;
    M32(t->teb + 0x18) = t->teb;
    M32(t->teb + 0x20) = 1;           /* EnvironmentPointer (unused) */
    M32(t->teb + 0x24) = 1;           /* ClientId.UniqueProcess */
    M32(t->teb + 0x28) = t->id;       /* ClientId.UniqueThread */
    if (!teb_ready) M32(t->teb + 0x30) = GP_PEB;
    M32(t->teb + 0x2c) = t->teb + 0x1000;
    M32(t->teb + 0x34) = 0; /* last error */
    M32(t->teb + 0xf94) = t->teb + 0x1800;
    /* Point the TEB TLS array at the private copy instead of the process block. */
    if (tls_size)
        for (uint32_t off = 0x1000; off < TEB_BYTES; off += 4) {
            uint32_t v = M32(t->teb + off);
            if (v >= tls_base && v - tls_base < tls_size)
                M32(t->teb + off) = new_tls + (v - tls_base);
        }
    return 0;
}

static void s_CreateThread(CPU *c) {
    uint32_t attrs = A(0), stack = A(1), start = A(2), param = A(3), flags = A(4), tid_out = A(5);
    if (attrs && !guest_ptr(mem, attrs, 12)) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (!start) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (flags & ~(CREATE_SUSPENDED | STACK_SIZE_IS_RESERVATION)) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (tid_out && !guest_ptr(mem, tid_out, 4)) FAIL(ERR_INVALID_PARAMETER, 0, 6);
    if (!stack) stack = WIN32_THREAD_DEFAULT_STACK;
    if (stack < 0x10000u) stack = 0x10000u;
    if (stack > WIN32_THREAD_MAX_STACK) FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 6);
    stack = (stack + 0xfff) & ~0xfffu;
    unsigned slot;
    for (slot = 0; slot < MAX_THREADS; ++slot) if (threads[slot].state == ST_FREE) break;
    if (slot == MAX_THREADS) FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 6);
    W32Thread *t = &threads[slot];
    memset(t, 0, sizeof *t);
    t->slot = slot;
    t->id = tid_of(slot);
    t->handle = WIN32_THREAD_HANDLE_BASE + slot;
    t->start = start;
    t->param = param;
    t->priority = 0; /* THREAD_PRIORITY_NORMAL */
    t->affinity = 1;
    ready_stamp(t); /* a concrete FIFO sequence, even while still suspended */
    t->suspends = (flags & CREATE_SUSPENDED) ? 1 : 0;
    if (build_thread_memory(t, stack)) {
        memset(t, 0, sizeof *t);
        FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 6);
    }
    n3ds_lift_init_cpu(&t->cpu, t->stack_top, t->teb);
    t->cpu.thread = t;
    n3ds_lift_thread_state_init(&t->rt);
    if (win32_hostctx_create(&t->ctx, thread_trampoline, t, WIN32_THREAD_HOST_STACK)) {
        if (t->region_base) guest_unmap(mem, t->region_base);
        memset(t, 0, sizeof *t);
        FAIL(ERR_NOT_ENOUGH_MEMORY, 0, 6);
    }
    if (tid_out) M32(tid_out) = t->id;
    set_error(c, 0);
    /* TESV's BSThread::Start (return address 0x00a4b53b) stores the handle and
     * sets its own +0x2c "running" flag *after* CreateThread returns.  Running
     * the new body synchronously here is safe for the existing tests, but for
     * that caller it lets a short-lived VMInitThread run Run->Stop before the
     * handle/flag exist, so Stop sees [+0x1c]==0 and never clears +0x2c; the
     * thread is gone but main spins forever in 0x690f90.  Defer only that
     * Windows-ordered caller; the body runs at the next yield or its
     * ResumeThread. */
    if (!t->suspends) {
        if (M32(c->r[4]) == 0x00a4b53bu) { if (!t->in_ready) enqueue(t); }
        else activate(t);
    }
    RET(t->handle, 6);
}
uint32_t win32_thread_begin(uint32_t start, uint32_t param, uint32_t stack, uint32_t flags) {
    if (!start) return 0;
    if (!stack) stack = WIN32_THREAD_DEFAULT_STACK;
    if (stack < 0x10000u) stack = 0x10000u;
    if (stack > WIN32_THREAD_MAX_STACK) return 0;
    stack = (stack + 0xfff) & ~0xfffu;
    unsigned slot;
    for (slot = 0; slot < MAX_THREADS; ++slot) if (threads[slot].state == ST_FREE) break;
    if (slot == MAX_THREADS) return 0;
    W32Thread *t = &threads[slot];
    memset(t, 0, sizeof *t);
    t->slot = slot;
    t->id = tid_of(slot);
    t->handle = WIN32_THREAD_HANDLE_BASE + slot;
    t->start = start;
    t->param = param;
    t->affinity = 1;
    ready_stamp(t);
    t->suspends = (flags & CREATE_SUSPENDED) ? 1 : 0;
    if (build_thread_memory(t, stack)) { memset(t, 0, sizeof *t); return 0; }
    n3ds_lift_init_cpu(&t->cpu, t->stack_top, t->teb);
    t->cpu.thread = t;
    n3ds_lift_thread_state_init(&t->rt);
    if (win32_hostctx_create(&t->ctx, thread_trampoline, t, WIN32_THREAD_HOST_STACK)) {
        if (t->region_base) guest_unmap(mem, t->region_base);
        memset(t, 0, sizeof *t);
        return 0;
    }
    if (!t->suspends) activate(t);
    return t->handle;
}
static void s__beginthread(CPU *c) {
    uint32_t handle = win32_thread_begin(A(0), A(2), A(1), 0);
    if (!handle) { set_error(c, ERR_NOT_ENOUGH_MEMORY); RET(0xffffffffu, 3); }
    set_error(c, 0);
    RET(handle, 3);
}
static void s__beginthreadex(CPU *c) {
    uint32_t handle = win32_thread_begin(A(2), A(3), A(1), A(4) & (CREATE_SUSPENDED | STACK_SIZE_IS_RESERVATION));
    if (!handle) { set_error(c, ERR_NOT_ENOUGH_MEMORY); RET(0, 6); }
    set_error(c, 0);
    RET(handle, 6);
}
static void s_SuspendThread(CPU *c) {
    uint32_t h = A(0);
    if (h == 0xfffffffeu) {
        fprintf(stderr, "[thread] SuspendThread(current thread) refused: the running "
                        "thread cannot resume itself\n");
        FAIL(ERR_CALL_NOT_IMPLEMENTED, 0xffffffffu, 1);
    }
    W32Thread *t = thread_for(h);
    if (!t) FAIL(ERR_INVALID_HANDLE, 0xffffffffu, 1);
    if (t == cur) FAIL(ERR_CALL_NOT_IMPLEMENTED, 0xffffffffu, 1);
    uint32_t previous = t->suspends;
    if (t->suspends < MAX_SUSPEND) ++t->suspends;
    set_error(c, 0);
    RET(previous, 1);
}
static void s_ResumeThread(CPU *c) {
    uint32_t h = A(0);
    if (h == 0xfffffffeu) RET(0, 1); /* current thread: previous count 0 */
    W32Thread *t = thread_for(h);
    if (!t) FAIL(ERR_INVALID_HANDLE, 0xffffffffu, 1);
    uint32_t previous = t->suspends;
    if (t->suspends) --t->suspends;
    set_error(c, 0);
    /* ResumeThread on an already-runnable thread returns immediately on
     * Windows; it does not switch to it.  Switching here delayed the caller
     * (TESV's BSThread::Start -> main) until the resumed init thread happened
     * to yield, which starved main before its next init call.  Only a real
     * suspend->resume transition needs to activate the thread. */
    if (previous && t->suspends == 0 && t->state == ST_READY) {
        if (!t->in_ready) enqueue(t);
        activate(t);
    }
    RET(previous, 1);
}
static void s_ExitThread(CPU *c) {
    uint32_t code = A(0);
    /* Worker exits are routine (TESV uses them); only a main-thread exit is
     * worth a diagnostic because it terminates the process. */
    if (win32_thread_current_id() == MAIN_THREAD_ID)
        fprintf(stderr, "[thread] ExitThread(%lu) on the main guest thread exits the process\n",
                (unsigned long)code);
    n3ds_lift_exit(code);
}
static void s_GetExitCodeThread(CPU *c) {
    uint32_t h = A(0), out = A(1);
    if (!guest_ptr(mem, out, 4)) FAIL(ERR_INVALID_PARAMETER, 0, 2);
    if (h == 0xfffffffeu) { M32(out) = STILL_ACTIVE; set_error(c, 0); RET(1, 2); }
    W32Thread *t = thread_for(h);
    if (!t) FAIL(ERR_INVALID_HANDLE, 0, 2);
    M32(out) = t->state == ST_EXITED ? t->exit_code : STILL_ACTIVE;
    set_error(c, 0);
    RET(1, 2);
}
static void s_SetThreadPriority(CPU *c) {
    uint32_t h = A(0);
    int32_t prio = (int32_t)A(1);
    if (prio < -15 || prio > 15) FAIL(ERR_INVALID_PARAMETER, 0, 2);
    if (h == 0xfffffffeu) { set_error(c, 0); RET(1, 2); }
    W32Thread *t = thread_for(h);
    if (!t) FAIL(ERR_INVALID_HANDLE, 0, 2);
    t->priority = prio;
    set_error(c, 0);
    RET(1, 2);
}
static void s_GetThreadPriority(CPU *c) {
    uint32_t h = A(0);
    if (h == 0xfffffffeu) { set_error(c, 0); RET(0, 1); }
    W32Thread *t = thread_for(h);
    if (!t) FAIL(ERR_INVALID_HANDLE, 0x7fffffffu, 1);
    set_error(c, 0);
    RET((uint32_t)t->priority, 1);
}
static void s_SetThreadAffinityMask(CPU *c) {
    uint32_t h = A(0), mask = A(1);
    if (!mask || (mask & ~1u)) FAIL(ERR_INVALID_PARAMETER, 0, 2);
    if (h != 0xfffffffeu && !thread_for(h)) FAIL(ERR_INVALID_HANDLE, 0, 2);
    if (h != 0xfffffffeu) thread_for(h)->affinity = 1;
    set_error(c, 0);
    RET(1, 2); /* previous mask: one logical CPU */
}
static void s_OpenThread(CPU *c) {
    uint32_t id = A(2);
    W32Thread *t = created_thread_for_id(id);
    if (!t) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    set_error(c, 0);
    RET(WIN32_THREAD_HANDLE_BASE + (t - threads), 3);
}
static void s_OpenProcess(CPU *c) {
    if (A(2) != 1) FAIL(ERR_INVALID_PARAMETER, 0, 3);
    set_error(c, 0);
    RET(0xffffffffu, 3);
}
static void s_GetExitCodeProcess(CPU *c) {
    uint32_t out = A(1);
    if (A(0) != 0xffffffffu) FAIL(ERR_INVALID_HANDLE, 0, 2);
    if (!guest_ptr(mem, out, 4)) FAIL(ERR_INVALID_PARAMETER, 0, 2);
    M32(out) = STILL_ACTIVE;
    set_error(c, 0);
    RET(1, 2);
}

#define DEF(X) { "kernel32.dll", #X, s_##X },
static const Win32ThreadDef defs[] = {
    DEF(CreateThread) DEF(SuspendThread) DEF(ResumeThread) DEF(ExitThread)
    DEF(GetExitCodeThread) DEF(SetThreadPriority) DEF(GetThreadPriority)
    DEF(SetThreadAffinityMask) DEF(OpenThread) DEF(OpenProcess)
    DEF(GetExitCodeProcess)
    {"msvcrt.dll", "_beginthread", s__beginthread},
    {"msvcrt.dll", "_beginthreadex", s__beginthreadex},
};
#undef DEF
#define NDEFS (sizeof defs / sizeof defs[0])

const Win32ThreadDef *win32_thread_defs(unsigned *count) {
    if (count) *count = (unsigned)NDEFS;
    return defs;
}
static int same(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
uint32_t win32_thread_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    if (!same(dll, "kernel32.dll") && !same(dll, "kernel32") &&
        !same(dll, "msvcrt.dll") && !same(dll, "msvcrt")) return 0;
    for (unsigned i = 0; i < NDEFS; ++i)
        if (!strcmp(name, defs[i].name)) return WIN32_THREAD_BASE + 4 * i;
    return 0;
}
static FnEntry entries[NDEFS];
const FnEntry *win32_thread_lookup(uint32_t addr) {
    if (addr < WIN32_THREAD_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_THREAD_BASE) / 4;
    if (i >= NDEFS) return NULL;
    entries[i] = (FnEntry){addr, defs[i].fn};
    return &entries[i];
}
