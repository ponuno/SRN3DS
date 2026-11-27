#pragma once
#include <stdio.h>
#include "guest.h"
#include "rt_table.h"
#include "ir_core.h"
#include "ir_container.h"

/* Bind exactly one CPU's guest address space and a sorted function table.
 * The table must remain valid for the entire session, including call caches.
 * AOT entries stay in FnEntry; the interpreter's ranges are bound separately
 * with n3ds_ir_bind() (the hybrid build also provides the weak g_ir_table
 * symbol so binding is automatic). */
int n3ds_lift_bind(GuestMemory *m,const FnEntry *table,unsigned count);
int n3ds_lift_bind_modules(GuestMemory *m,const FnModule *modules,unsigned count);
int n3ds_lift_has_function(uint32_t address);
/* One session per executable launch; caches are not reset on release. */
void n3ds_lift_release(void);
void n3ds_lift_init_cpu(CPU *c,uint32_t stack_top,uint32_t teb);
int n3ds_lift_invoke(CPU *c,uint32_t address,const uint32_t *args,size_t argc,uint32_t *result);
/* Invoke a function through the interpreter even when the address is also an
 * AOT entry. `end` is exclusive; 0 asks the bound IR table. Used by the
 * equivalence test and by callers that want a specific tier. */
int n3ds_ir_invoke(CPU *c,uint32_t address,uint32_t end,const uint32_t *args,size_t argc,uint32_t *result);
/* Nested invocation for cooperative guest threads and Win32 callbacks: legal
 * while an outer invoke is active. `c` must be a private CPU with its own
 * mapped stack and TEB. Returns the same status codes as n3ds_lift_invoke. */
int n3ds_lift_invoke_reentrant(CPU *c,uint32_t address,const uint32_t *args,size_t argc,uint32_t *result);
enum { N3DS_INVOKE_OK=0,N3DS_INVOKE_BAD=-1,N3DS_INVOKE_TRAP=-2,N3DS_INVOKE_EXIT=1 };
const char *n3ds_lift_fault(void);
/* Bounded guest tracing helpers used only by diagnostic generated slices. */
void n3ds_trace_cstr(const char *tag, uint32_t ptr);
void n3ds_trace_u32(const char *tag, uint32_t value);
/* Hang-watchdog evidence: current IR block, last dispatched entry, and the
 * recent guest-entry ring. AOT functions keep their PC in host locals, so the
 * ring plus the guest stack scan is the available location evidence. */
uint32_t n3ds_lift_current_ip(void);
uint32_t n3ds_lift_last_call(void);
void n3ds_lift_dump_recent_calls(FILE *f);
void n3ds_lift_dump_stream(FILE *f);
/* Measurement-only dump for the NiDX9TextureBufferData descriptor slot. */
void n3ds_cube_desc_probe(uint32_t arg, uint32_t index, uint32_t base, uint32_t slot);
/* Measurement-only fill for f_CECDC0's uninitialised base local.
 * 1: skip the original lookup and keep the real ebx+0x14/[esp+0x14] value. */
int n3ds_cecdc0_source_fill(uint32_t frame, uint32_t index, uint32_t ebx);
void n3ds_lift_exit(uint32_t code) __attribute__((noreturn));
int n3ds_lift_self_test(void);

/* ---- cooperative guest-thread runtime state -------------------------------
 * `active` (the invoke-frame stack) and the fault text are per guest thread:
 * a fault must longjmp into the *running* thread's frame, never into a
 * suspended thread's frame on another host stack. `irc_base` is the IR
 * container frame depth at the thread root, so a fault unwinds only the frames
 * that thread pushed. The executor frame stack itself is global, so it is
 * snapshotted here on every host context switch: a thread that blocks inside a
 * container call resumes with its own pinned frames. win32_thread.c owns one
 * N3dsThreadRtState per thread and calls n3ds_lift_thread_switch() around
 * every host context switch. */
typedef struct {
    void *active;
    uint32_t irc_base;
    uint32_t irc_depth;
    unsigned char irc_frames[N3DS_IRC_FRAME_SNAPSHOT_BYTES];
    char fault[256];
} N3dsThreadRtState;
void n3ds_lift_thread_state_init(N3dsThreadRtState *s);
/* Root the frame stack for a newly entered guest thread. Called by the thread
 * trampoline before n3ds_lift_invoke_reentrant(). */
void n3ds_lift_thread_begin(N3dsThreadRtState *s);
/* Save the running thread's state, restore `to`'s. Called with the outgoing
 * thread still on the CPU, immediately before the host context switch. */
void n3ds_lift_thread_switch(N3dsThreadRtState *from, N3dsThreadRtState *to);


/* Optional indexed IR container. AOT still wins; the container is used only
 * for addresses absent from AOT (then the slice IR table is the last fallback).
 * Path/env contract:
 *   N3DS_IRC_PATH       override path (default: host "tesv.irc",
 *                       ARM "sdmc:/Skyrim/ir/tesv.irc")
 *   N3DS_IRC_ENABLE=0   disable the tier entirely
 *   N3DS_IRC_REQUIRED=1 fail bind/dispatch explicitly when absent/corrupt
 *   N3DS_IRC_CACHE_BYTES / N3DS_IRC_CACHE_RECORDS  override bounded cache
 * Call n3ds_lift_set_container_path() before the first non-AOT dispatch to
 * override the path programmatically (tests). */
int n3ds_lift_set_container_path(const char *path);
int n3ds_lift_container_state(void);   /* 0 untried, 1 open, -1 unavailable */
const char *n3ds_lift_container_error(void);
/* Returns 0 and fills stats when the container is open, -1 otherwise. */
int n3ds_lift_container_stats(N3dsIrcStats *out);
