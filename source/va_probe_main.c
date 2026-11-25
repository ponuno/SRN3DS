/* Isolated ARM diagnostic: can the 3DS reserve the guest's 256 MiB VA arena?
 *
 * The guest's first VirtualAlloc is a 256 MiB MEM_RESERVE, larger than the
 * application memory region. This build calls svcControlMemory directly so the
 * answer is measured on the device/emulator, not assumed. It also exercises
 * the lazy slab's first-page backing path (source/guest.c).
 *
 * Results go to the console and sdmc:/Skyrim_N3DS_va_probe.log.
 * Not part of the game build. */
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include "guest.h"

u32 __ctru_heap_size = 40u << 20;
u32 __ctru_linear_heap_size = 8u << 20;

static FILE *log_file;
static void report(const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    vprintf(fmt, args);
    if (log_file) { vfprintf(log_file, fmt, copy); fflush(log_file); }
    va_end(copy);
    va_end(args);
}

static void try_reserve(const char *label, u32 size, MemOp op) {
    u32 out = 0;
    Result r = svcControlMemory(&out, 0, 0, size, op,
                                (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    report("%-28s size=%8lu result=%08lx desc=%lu %s addr=%08lx\n",
           label, (unsigned long)size, (unsigned long)r,
           (unsigned long)R_DESCRIPTION(r), R_SUCCEEDED(r) ? "OK" : "FAIL",
           (unsigned long)out);
    if (R_SUCCEEDED(r) && out) {
        u32 page = 0;
        Result rc = svcControlMemory(&page, out, 0, 0x1000,
                                     (MemOp)(MEMOP_ALLOC | MEMOP_REGION_APP),
                                     (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
        report("%-28s commit first page: result=%08lx desc=%lu %s addr=%08lx\n",
               "  commit", (unsigned long)rc, (unsigned long)R_DESCRIPTION(rc),
               R_SUCCEEDED(rc) ? "OK" : "FAIL", (unsigned long)page);
        if (R_SUCCEEDED(rc)) *(volatile u32 *)out = 0x5a5a5a5a;
        u32 freed = 0;
        Result rf = svcControlMemory(&freed, out, 0, size, MEMOP_FREE,
                                     (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
        report("%-28s free: result=%08lx desc=%lu %s\n", "  free",
               (unsigned long)rf, (unsigned long)R_DESCRIPTION(rf),
               R_SUCCEEDED(rf) ? "OK" : "FAIL");
    }
}

static void slab_probe(void) {
    GuestMemory m;
    guest_init(&m, 32u << 20);
    GuestSlab *s = guest_slab_reserve(&m, 0x40000000u, 256u << 20);
    if (!s) {
        report("guest slab: reserve failed (page tables > 32 MiB)\n");
        return;
    }
    guest_slab_commit(&m, 0x40000000u, 0x4000u, 4);
    void *p = guest_ptr(&m, 0x40000000u, 4);
    report("guest slab: first page %s, meta=%lu KiB used=%lu KiB backed=%lu KiB "
           "failures=%lu\n",
           p ? "BACKED" : "FAILED", (unsigned long)(m.slab_meta >> 10),
           (unsigned long)(m.used >> 10), (unsigned long)(m.slab_backed >> 10),
           (unsigned long)m.backing_failures);
    if (p) {
        *(volatile u32 *)p = 0x12345678u;
        report("guest slab: first page readback=%08lx\n",
               (unsigned long)*(volatile u32 *)p);
        /* 0x40000ffe crosses the 4 KiB page boundary; the non-contiguous
         * page table must bridge it and preserve both bytes. */
        volatile u8 *x = (volatile u8 *)guest_ptr(&m, 0x40000ffeu, 4);
        if (!x) {
            report("guest slab: cross-page access FAILED fault=%s\n",
                   guest_fault_text(&m));
        } else {
            x[0] = 0x11; x[1] = 0x22; x[2] = 0x33; x[3] = 0x44;
            u8 back[4] = {0};
            int rc = guest_read(&m, 0x40000ffeu, back, 4);
            report("guest slab: cross-page %s readback=%02x %02x %02x %02x\n",
                   rc ? "FAILED" : "OK", back[0], back[1], back[2], back[3]);
        }
        GuestPageStats ps;
        guest_page_stats(&m, &ps);
        report("guest slab: pages first-touch=%lu (anon=%lu file=%lu), failures=%lu\n",
               (unsigned long)ps.first_touched, (unsigned long)ps.anon,
               (unsigned long)ps.file, (unsigned long)ps.failures);
        size_t old_limit=m.limit;
        m.limit=m.used;
        void *oom=guest_ptr(&m,0x40002000u,4);
        report("guest slab: exhausted backing %s fault=%s\n",
               oom ? "unexpected success" : "deterministic failure",
               guest_fault_text(&m));
        m.limit=old_limit;
    }
    guest_destroy(&m);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    log_file = fopen("sdmc:/Skyrim_N3DS_va_probe.log", "w");
    report("Skyrim N3DS - VA reservation probe\n");
    report("app region: %lu MiB, free %lu MiB\n",
           (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20),
           (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20));
    report("heap pin: %lu MiB, linear pin: %lu MiB\n",
           (unsigned long)(__ctru_heap_size >> 20),
           (unsigned long)(__ctru_linear_heap_size >> 20));

    try_reserve("APP reserve 256 MiB", 256u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve 178 MiB", 178u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve 128 MiB", 128u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve  96 MiB",  96u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve  85 MiB",  85u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve  64 MiB",  64u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("APP reserve  32 MiB",  32u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_APP));
    try_reserve("BASE reserve 256 MiB", 256u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_BASE));
    try_reserve("SYSTEM reserve 256 MiB", 256u << 20, (MemOp)(MEMOP_RESERVE | MEMOP_REGION_SYSTEM));

    slab_probe();

    report("\nSTART: exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    if (log_file) fclose(log_file);
    return 0;
}
