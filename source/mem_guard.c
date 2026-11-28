/* Host-heap ownership guard for the 3DS build.
 *
 * A guest virtual address is just a number; passing one to free() corrupts the
 * newlib heap (the Azahar crash walked a chunk whose bytes were the string
 * "Data..." and faulted on an unmapped address).  Wrap free/realloc and refuse
 * anything outside the application heap range, not 8-byte aligned, or whose
 * malloc_usable_size() would run past the heap, naming the caller and dumping
 * the last free/realloc calls instead of corrupting the heap.
 *
 * Enabled only for the TESV device link (Makefile adds --wrap=free/realloc and
 * this object).  The host build does not compile this file. */
#ifdef __3DS__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* libctru's crt0 exposes the application heap bounds as these pointers. */
extern char *fake_heap_start;
extern char *fake_heap_end;

static struct { const void *ptr; void *caller; size_t size; int kind; } g_ring[16];
static unsigned g_ring_pos, g_ring_n;
static unsigned g_refused;

static void note(const void *p, void *caller, size_t size, int kind) {
    unsigned i = g_ring_pos++ & 15u;
    g_ring[i].ptr = p;
    g_ring[i].caller = caller;
    g_ring[i].size = size;
    g_ring[i].kind = kind;
    if (g_ring_n < 16u) ++g_ring_n;
}

void n3ds_mem_guard_dump(void) {
    fprintf(stderr, "[heap] last %u free/realloc calls, %u refused:\n",
            g_ring_n, g_refused);
    for (unsigned k = 0; k < g_ring_n; ++k) {
        unsigned i = (g_ring_pos - g_ring_n + k) & 15u;
        unsigned long sp = 0;
        __asm__ volatile ("mov %0, sp" : "=r"(sp));
        fprintf(stderr, "[heap]   %s ptr=%p size=%lu caller=%p sp=%08lx\n",
                g_ring[i].kind ? "realloc" : "free",
                g_ring[i].ptr, (unsigned long)g_ring[i].size,
                g_ring[i].caller, sp);
    }
}

/* 0 = definitely not ours, 1 = in range.  newlib's nano-malloc and standard
 * malloc_usable_size disagree about the chunk layout (1261 valid internal
 * frees were misjudged when this used it), so ownership is the heap range and
 * 8-byte alignment only; the corruption case we must stop is a guest address
 * (~0x00xxxxxx) or an unaligned pointer reaching free(). */
static int heap_owner(const void *p) {
    uintptr_t v = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)fake_heap_start;
    uintptr_t hi = (uintptr_t)fake_heap_end;
    if (!p) return 0;
    if (v & 7u) return 0;
    if (!lo || !hi || hi <= lo) return 1; /* unknown bounds: alignment only */
    return v >= lo && v < hi;
}

static void refuse(const char *op, const void *p, size_t n, void *caller, int owner) {
    ++g_refused;
    fprintf(stderr, "[heap] %s REFUSED: ptr=%p size=%lu caller=%p heap=[%p,%p) "
            "%s\n", op, p, (unsigned long)n, caller,
            (void *)fake_heap_start, (void *)fake_heap_end,
            owner ? "not the start of a block" : "outside the host heap");
    n3ds_mem_guard_dump();
}

void __real_free(void *p);
void *__real_realloc(void *p, size_t n);

void __wrap_free(void *p) {
    void *caller = __builtin_return_address(0);
    note(p, caller, 0, 0);
    if (!p) return;
    int owner = heap_owner(p);
    if (owner < 1) { refuse("free", p, 0, caller, owner); return; }
    __real_free(p);
}

void *__wrap_realloc(void *p, size_t n) {
    void *caller = __builtin_return_address(0);
    note(p, caller, n, 1);
    if (p) {
        int owner = heap_owner(p);
        if (owner < 1) { refuse("realloc", p, n, caller, owner); return NULL; }
    }
    return __real_realloc(p, n);
}

#endif /* __3DS__ */
