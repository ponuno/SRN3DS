/* The COM object table is a finite address table.  It must be large enough for
 * the measured engine (hundreds of shader variants), keep stable 16-byte
 * guest handles, and fail with a named diagnostic when it is really full. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT 0x10010900u
#define QUERY_EVENT 8u
#define OUTMEM 0x8876017cu

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "demo call trap object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
        abort();
    }
    return result;
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&s->memory, address, &value));
    return value;
}
static void write32(D9ComDemo *s, uint32_t address, uint32_t value) {
    assert(!guest_write32(&s->memory, address, value));
}
static uint32_t refs(D9ComDemo *s, uint32_t object) {
    uint32_t after_add = call(s, object, 1, NULL, 0);
    assert(call(s, object, 2, NULL, 0) == after_add - 1);
    return after_add - 1;
}

int main(void) {
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(),
                                   128u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }

    uint32_t args[] = {QUERY_EVENT, OUT};
    uint32_t handles[2048];
    unsigned n = 0, exhausted = 0;
    for (unsigned i = 0; i < 2048; ++i) {
        write32(&demo, OUT, 0xdeadbeefu);
        uint32_t rc = call(&demo, demo.device, 118, args, 2);
        if (rc == OUTMEM) { exhausted = 1; break; }
        assert(rc == 0);
        uint32_t q = read32(&demo, OUT);
        assert(q && (q & 15u) == 0);
        handles[n++] = q;
    }
    assert(exhausted);
    assert(n > 300); /* the old 128-object ceiling is gone */
    assert(n < 2048); /* the demo already owns a few objects */
    assert(strstr(d9_com_last_diagnostic(), "COM object table exhausted")
           != NULL);

    for (unsigned i = 0; i < n; ++i) {
        assert((handles[i] & 15u) == 0);
        for (unsigned j = 0; j < i; ++j)
            assert(handles[i] != handles[j]);
    }
    assert(refs(&demo, handles[0]) == 1);
    assert(refs(&demo, handles[n / 2]) == 1);
    assert(refs(&demo, handles[n - 1]) == 1);

    for (unsigned i = 0; i < n; ++i)
        (void)call(&demo, handles[i], 2, NULL, 0);
    assert(!d9_com_demo_destroy(&demo));
    assert(d9_memory_used(d) == 0);
    d9_destroy_device(d);
    printf("PASS com_object_table: %u handles, named exhaustion, teardown clean\n",
           n);
    return 0;
}
