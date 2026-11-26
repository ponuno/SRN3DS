/* Cheap post-renderer-init IDirect3DDevice9 methods.
 *
 * GetAvailableTextureMem reports the remaining device budget, EvictManaged-
 * Resources is a no-op only while MANAGED resources still have guest-backed
 * copies, ValidateDevice accepts the supported fixed-function/TEV state,
 * GetRasterStatus refuses because PICA exposes no scanline/vblank status, and
 * SetDialogBoxMode accepts only FALSE.  The test checks the supported calls,
 * named refusals, the budget value and that the methods do not allocate or
 * change the device refcount.
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 0x10010600u
#define OUT (BUF + 0x700u)
#define OUT2 (BUF + 0x708u)
#define ERR_INVALID 0x8876086cu
#define ERR_UNAVAILABLE 0x8876086au

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
static uint32_t device_call(D9ComDemo *s, unsigned slot, const uint32_t *args,
                            unsigned count) {
    return call(s, s->device, slot, args, count);
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&s->memory, address, &value));
    return value;
}

int main(void) {
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }

    size_t used = d9_memory_used(d);
    size_t remaining = used < (8u << 20) ? (8u << 20) - used : 0;
    uint32_t refs_before = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_before - 1);

    /* GetAvailableTextureMem is the exact remaining budget, capped to DWORD. */
    uint32_t mem = device_call(&demo, 4, NULL, 0);
    assert(mem == (remaining > 0xffffffffu ? 0xffffffffu : (uint32_t)remaining));
    assert(d9_memory_used(d) == used);

    /* EvictManagedResources: the demo texture is MANAGED with a guest copy. */
    assert(device_call(&demo, 5, NULL, 0) == 0);
    assert(d9_memory_used(d) == used);

    /* GetRasterStatus has no PICA scanline/vblank source. */
    uint32_t status[] = {OUT2};
    assert(device_call(&demo, 19, status, 1) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "raster") != NULL ||
           strstr(d9_com_last_diagnostic(), "scanline") != NULL);
    assert(read32(&demo, OUT2) == 0 && read32(&demo, OUT2 + 4) == 0);
    assert(device_call(&demo, 19, (uint32_t[]){0}, 1) == ERR_INVALID);

    /* Dialog box mode only accepts FALSE. */
    assert(device_call(&demo, 20, (uint32_t[]){0}, 1) == 0);
    assert(device_call(&demo, 20, (uint32_t[]){1}, 1) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "TRUE") != NULL);
    assert(device_call(&demo, 20, (uint32_t[]){0}, 1) == 0);

    /* ValidateDevice: default fixed-function state is supported, one pass. */
    uint32_t passes[] = {OUT2};
    uint32_t vrc = device_call(&demo, 70, passes, 1);
    if (vrc != 0) fprintf(stderr, "ValidateDevice initial rc=0x%08x diag=[%s]\n",
                          vrc, d9_com_last_diagnostic());
    assert(vrc == 0);
    assert(read32(&demo, OUT2) == 1);
    assert(device_call(&demo, 70, (uint32_t[]){0}, 1) == ERR_INVALID);

    /* A pixel shader needing three texture units is outside this backend. */
    D9TevProgram bad;
    memset(&bad, 0, sizeof bad);
    bad.stage_limit = 1;
    bad.textures = 3;
    assert(d9_set_pixel_shader(d, &bad) == D9_OK);
    assert(device_call(&demo, 70, passes, 1) == ERR_UNAVAILABLE);
    assert(read32(&demo, OUT2) == 0);
    assert(strstr(d9_com_last_diagnostic(), "3 texture") != NULL);
    assert(d9_memory_used(d) == used);
    assert(d9_set_pixel_shader(d, NULL) == D9_OK);
    assert(device_call(&demo, 70, passes, 1) == 0 && read32(&demo, OUT2) == 1);

    /* No method should retain, release or allocate anything. */
    uint32_t refs_after = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_after - 1);
    assert(refs_after == refs_before);
    assert(d9_memory_used(d) == used);

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("device extra methods: budget, managed no-op, raster/dialog refusals and ValidateDevice PASS");
    return 0;
}
