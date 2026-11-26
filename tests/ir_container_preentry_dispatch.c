/* Dispatch regression: a slice-IR function with a container-declared pre-entry
 * cold block must use the container frame, not fault in the slice-IR frame. */
#include "n3ds_lift_runtime.h"
#include "ir_container.h"
#include "ir_core.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE  0x00001000u
#define ENTRY      0x00001010u
#define FUNC_END   0x00001015u
#define STACK_TOP  0x00003000u

static const uint8_t code[] = {
    0x8d, 0x42, 0xff, 0x5b, 0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x53, 0x31, 0xc0, 0x74, 0xeb,
};

/* Strong definitions for the runtime's weak generated-slice table. */
const IrFnEntry g_ir_table[] = {{ENTRY, FUNC_END}};
const unsigned g_ir_count = 1;

static GuestMemory mem;

const FnEntry *win32_lookup(uint32_t address) { (void)address; return NULL; }

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s CONTAINER.IRC\n", argv[0]);
        return 2;
    }
    guest_init(&mem, 1u << 20);
    if (guest_map(&mem, CODE_BASE, 0x1000) ||
        guest_map(&mem, STACK_TOP - 0x1000, 0x1000)) {
        fprintf(stderr, "guest map failed\n");
        return 2;
    }
    memcpy(guest_ptr(&mem, CODE_BASE, sizeof code), code, sizeof code);
    if (n3ds_lift_set_container_path(argv[1]) ||
        n3ds_lift_bind_modules(&mem, NULL, 0)) {
        fprintf(stderr, "bind failed: %s\n", n3ds_lift_fault());
        return 2;
    }

    CPU c;
    n3ds_lift_init_cpu(&c, STACK_TOP, 0);
    c.r[2] = 0;
    c.r[3] = 0x13579bdfu;
    IrCacheStats before, after;
    n3ds_ir_cache_stats(&before);
    uint32_t result = 0;
    int rc = n3ds_lift_invoke(&c, ENTRY, NULL, 0, &result);
    n3ds_ir_cache_stats(&after);
    if (rc != N3DS_INVOKE_OK) {
        fprintf(stderr, "FAIL: pre-entry dispatch did not reach the container: %s\n",
                n3ds_lift_fault());
        return 1;
    }
    if (result != 0xffffffffu || c.r[3] != 0x13579bdfu) {
        fprintf(stderr, "FAIL: rc=%d result=%08lx ebx=%08lx\n", rc,
                (unsigned long)result, (unsigned long)c.r[3]);
        return 1;
    }
    if (before.blocks != after.blocks) {
        fprintf(stderr, "FAIL: slice decoder ran (%llu -> %llu)\n",
                (unsigned long long)before.blocks,
                (unsigned long long)after.blocks);
        return 1;
    }
    N3dsIrcStats st;
    if (n3ds_lift_container_stats(&st) || st.execs < 1 || st.block_hits < 2) {
        fprintf(stderr, "FAIL: container did not execute both predecoded blocks\n");
        return 1;
    }
    printf("IR container pre-entry dispatch PASS (execs %llu, hits %llu, slice blocks %llu)\n",
           (unsigned long long)st.execs, (unsigned long long)st.block_hits,
           (unsigned long long)after.blocks);
    n3ds_lift_release();
    guest_destroy(&mem);
    return 0;
}
