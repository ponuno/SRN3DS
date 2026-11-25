/* Host stacks and context switching for cooperative guest threads.
 *
 * A guest thread's *continuation* is a host C call stack: the lifted AOT code
 * keeps guest registers in locals and arithmetic flags in a local `Flags`, and
 * the interpreter does the same. Only the host stack can preserve that. A CPU
 * struct alone cannot (see THREAD_MODEL.md, "What must be saved").
 *
 * Backends:
 *   1  Windows fibers (ConvertThreadToFiber/SwitchToFiber)
 *   3  manual x86-64 switch (Win64 ABI)
 *   2  manual ARMv6K switch (AAPCS hard-float) - compile-guarded, NOT run on
 *      hardware by this work; see THREAD_MODEL.md.
 */
#include "win32_hostctx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if WIN32_HOSTCTX_BACKEND == 1
/* ------------------------------------------------------------- fibers */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct Win32HostCtx {
    void *fiber;
    int converted;
};

static void hostctx_entry_fiber(void *arg) {
    struct Call { void (*fn)(void *); void *arg; } *call = arg;
    call->fn(call->arg);
    /* A guest thread trampoline must never return: returning terminates the
     * host thread that owns every guest thread's fiber. */
    abort();
}

int win32_hostctx_init(void) { return 0; }

int win32_hostctx_create(Win32HostCtx **out, void (*fn)(void *), void *arg,
                         size_t stack_bytes) {
    if (!out || !fn) return -1;
    Win32HostCtx *ctx = calloc(1, sizeof *ctx);
    struct Call { void (*fn)(void *); void *arg; } *call = malloc(sizeof *call);
    if (!ctx || !call) { free(ctx); free(call); return -1; }
    call->fn = fn;
    call->arg = arg;
    ctx->fiber = CreateFiber(stack_bytes, (LPFIBER_START_ROUTINE)hostctx_entry_fiber, call);
    if (!ctx->fiber) { free(ctx); free(call); return -1; }
    *out = ctx;
    return 0;
}

void win32_hostctx_switch(Win32HostCtx *from, Win32HostCtx *to) {
    if (!to || !to->fiber) abort();
    if (from && !from->converted) {
        void *self = ConvertThreadToFiber(NULL);
        if (!self) self = GetCurrentFiber();
        from->fiber = self;
        from->converted = 1;
    }
    SwitchToFiber(to->fiber);
}

void win32_hostctx_destroy(Win32HostCtx *ctx) {
    if (!ctx) return;
    if (ctx->fiber && !ctx->converted) DeleteFiber(ctx->fiber);
    free(ctx);
}
size_t win32_hostctx_default_stack(void) { return 256u * 1024u; }
const char *win32_hostctx_name(void) { return "win32-fibers"; }

#elif WIN32_HOSTCTX_BACKEND == 3
/* --------------------------------------------------------- x86-64 switch */
struct Win32HostCtx {
    void *sp;
    void *stack;
    size_t stack_size;
    void *stack_base;   /* TEB StackBase while this context runs */
    void *stack_limit;  /* TEB StackLimit */
};

/* MSVC's x64 longjmp restores the context through RtlRestoreContext, which
 * validates RSP against the TEB stack limits. A manually switched stack must
 * therefore publish its own limits, exactly like a Windows fiber does. */
static void teb_get_stack(void **base, void **limit) {
    void *b, *l;
    __asm__ volatile("movq %%gs:0x30, %%rax\n\t"
                     "movq 0x08(%%rax), %0\n\t"
                     "movq 0x10(%%rax), %1\n\t"
                     : "=r"(b), "=r"(l) : : "rax", "memory");
    *base = b; *limit = l;
}
static void teb_set_stack(void *base, void *limit) {
    if (!base || !limit) return;
    __asm__ volatile("movq %%gs:0x30, %%rax\n\t"
                     "movq %0, 0x08(%%rax)\n\t"
                     "movq %1, 0x10(%%rax)\n\t"
                     :: "r"(base), "r"(limit) : "rax", "memory");
}

/* Frame below the saved rsp:
 *   [0..159]   xmm6..xmm15
 *   [160] r15 [168] r14 [176] r13 [184] r12
 *   [192] rsi [200] rdi [208] rbp [216] rbx
 *   [224] return address
 */
#define HCTX_FRAME (160u + 64u)

__attribute__((naked, noinline))
void win32_hostctx_switch_impl(Win32HostCtx *from, Win32HostCtx *to) {
    __asm__(
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %rdi\n\t"
        "push %rsi\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "sub $160, %rsp\n\t"
        "movups %xmm6, 0(%rsp)\n\t"
        "movups %xmm7, 16(%rsp)\n\t"
        "movups %xmm8, 32(%rsp)\n\t"
        "movups %xmm9, 48(%rsp)\n\t"
        "movups %xmm10, 64(%rsp)\n\t"
        "movups %xmm11, 80(%rsp)\n\t"
        "movups %xmm12, 96(%rsp)\n\t"
        "movups %xmm13, 112(%rsp)\n\t"
        "movups %xmm14, 128(%rsp)\n\t"
        "movups %xmm15, 144(%rsp)\n\t"
        "mov %rsp, (%rcx)\n\t"          /* Win64: from=rcx, to=rdx */
        "mov (%rdx), %rsp\n\t"
        "movups 0(%rsp), %xmm6\n\t"
        "movups 16(%rsp), %xmm7\n\t"
        "movups 32(%rsp), %xmm8\n\t"
        "movups 48(%rsp), %xmm9\n\t"
        "movups 64(%rsp), %xmm10\n\t"
        "movups 80(%rsp), %xmm11\n\t"
        "movups 96(%rsp), %xmm12\n\t"
        "movups 112(%rsp), %xmm13\n\t"
        "movups 128(%rsp), %xmm14\n\t"
        "movups 144(%rsp), %xmm15\n\t"
        "add $160, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rsi\n\t"
        "pop %rdi\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}

__attribute__((used)) void hostctx_entry_c(void *arg) {
    struct Call { void (*fn)(void *); void *arg; } *call = arg;
    call->fn(call->arg);
    abort();
}

/* First activation: r12 holds the call record; Win64 passes arg1 in rcx. */
__attribute__((naked, noinline)) static void hostctx_entry_r12(void) {
    __asm__("mov %r12, %rcx\n\tjmp hostctx_entry_c\n\t");
}

int win32_hostctx_init(void) { return 0; }

int win32_hostctx_create(Win32HostCtx **out, void (*fn)(void *), void *arg,
                         size_t stack_bytes) {
    if (!out || !fn) return -1;
    if (stack_bytes < 32u * 1024u) stack_bytes = 32u * 1024u;
    stack_bytes = (stack_bytes + 0xFFFu) & ~(size_t)0xFFFu;
    Win32HostCtx *ctx = calloc(1, sizeof *ctx);
    uint8_t *stack = malloc(stack_bytes);
    struct Call { void (*fn)(void *); void *arg; } *call = malloc(sizeof *call);
    if (!ctx || !stack || !call) { free(ctx); free(stack); free(call); return -1; }
    call->fn = fn;
    call->arg = arg;
    uintptr_t top = ((uintptr_t)stack + stack_bytes) & ~(uintptr_t)0xF;
    uint8_t *sp = (uint8_t *)(top - HCTX_FRAME - 32u);
    memset(sp, 0, HCTX_FRAME);
    *(void **)(sp + 184) = call;                    /* r12 */
    *(void **)(sp + 224) = (void *)hostctx_entry_r12;
    ctx->sp = sp;
    ctx->stack = stack;
    ctx->stack_size = stack_bytes;
    ctx->stack_limit = stack;
    ctx->stack_base = (void *)((uintptr_t)stack + stack_bytes);
    *out = ctx;
    return 0;
}

void win32_hostctx_switch(Win32HostCtx *from, Win32HostCtx *to) {
    if (!from || !to || !to->sp) abort();
    teb_set_stack(to->stack_base, to->stack_limit);
    win32_hostctx_switch_impl(from, to);
}

void win32_hostctx_destroy(Win32HostCtx *ctx) {
    if (!ctx) return;
    free(ctx->stack);
    free(ctx);
}
size_t win32_hostctx_default_stack(void) { return 256u * 1024u; }
const char *win32_hostctx_name(void) { return "manual-x86_64"; }

#elif WIN32_HOSTCTX_BACKEND == 2
/* ------------------------------------------------------------- ARM switch */
struct Win32HostCtx {
    void *sp;
    void *stack;
    size_t stack_size;
    void (*fn)(void *);
    void *arg;
};
/* low -> high: [d8..d15 64][r4][r5]..[r11][lr] */
#define HCTX_ARM_FRAME 104u
#define HCTX_ARM_LR    96u
#define HCTX_ARM_R4    64u

__attribute__((used)) static void hostctx_arm_entry_c(Win32HostCtx *ctx) { ctx->fn(ctx->arg); abort(); }
__attribute__((naked)) static void hostctx_arm_entry(void) {
    __asm__("mov r0, r4\n\tb hostctx_arm_entry_c\n\t");
}
__attribute__((naked)) static void hostctx_arm_switch(Win32HostCtx *from, Win32HostCtx *to) {
    (void)from; (void)to;
    __asm__(
        "push {r4-r11, lr}\n\t"
        "vpush {d8-d15}\n\t"
        "str sp, [r0]\n\t"
        "ldr sp, [r1]\n\t"
        "vpop {d8-d15}\n\t"
        "pop {r4-r11, pc}\n\t");
}

int win32_hostctx_init(void) { return 0; }

int win32_hostctx_create(Win32HostCtx **out, void (*fn)(void *), void *arg,
                         size_t stack_bytes) {
    if (!out || !fn) return -1;
    if (stack_bytes < 32u * 1024u) stack_bytes = 32u * 1024u;
    stack_bytes = (stack_bytes + 7u) & ~(size_t)7u;
    Win32HostCtx *ctx = calloc(1, sizeof *ctx);
    uint8_t *stack = malloc(stack_bytes);
    if (!ctx || !stack) { free(ctx); free(stack); return -1; }
    ctx->fn = fn;
    ctx->arg = arg;
    uintptr_t top = ((uintptr_t)stack + stack_bytes) & ~(uintptr_t)7;
    uint8_t *sp = (uint8_t *)(top - HCTX_ARM_FRAME);
    memset(sp, 0, HCTX_ARM_FRAME);
    *(void **)(sp + HCTX_ARM_LR) = (void *)hostctx_arm_entry;
    *(void **)(sp + HCTX_ARM_R4) = ctx;
    ctx->sp = sp;
    ctx->stack = stack;
    ctx->stack_size = stack_bytes;
    *out = ctx;
    return 0;
}

void win32_hostctx_switch(Win32HostCtx *from, Win32HostCtx *to) {
    if (!from || !to || !to->sp) abort();
    hostctx_arm_switch(from, to);
}
void win32_hostctx_destroy(Win32HostCtx *ctx) { if (!ctx) return; free(ctx->stack); free(ctx); }
size_t win32_hostctx_default_stack(void) { return 192u * 1024u; }
const char *win32_hostctx_name(void) { return "manual-armv6k"; }

#else
#error "no host context backend for this target"
#endif

/* A context for the already-running host thread (the main guest thread).
 * Defined after the backend blocks because it only needs the struct size. */
int win32_hostctx_main(Win32HostCtx **out) {
    if (!out) return -1;
    Win32HostCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return -1;
#if WIN32_HOSTCTX_BACKEND == 3
    teb_get_stack(&ctx->stack_base, &ctx->stack_limit);
#endif
    *out = ctx;
    return 0;
}
