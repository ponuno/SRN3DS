/* Honest refusals for the remaining device patch/process-vertices slots. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPS 0x10010c00u
#define CAPS_BYTES 512u
#define ERR_UNAVAILABLE 0x8876086au

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "patch/process: object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
        abort();
    }
    return result;
}
static uint32_t device_call(D9ComDemo *s, unsigned slot,
                            const uint32_t *args, unsigned count) {
    return call(s, s->device, slot, args, count);
}
static void expect_diagnostic(const char *needle) {
    const char *diagnostic = d9_com_last_diagnostic();
    if (!strstr(diagnostic, needle)) {
        fprintf(stderr, "patch/process: missing diagnostic '%s' in '%s'\n",
                needle, diagnostic);
        abort();
    }
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&s->memory, address, &value));
    return value;
}
static uint8_t *buffer_snapshot(D9ComDemo *s, uint32_t object, size_t bytes) {
    uint32_t args[] = {0, 0, CAPS, 0};
    assert(call(s, object, 11, args, 4) == 0);
    uint32_t address = read32(s, CAPS);
    const uint8_t *data = guest_ptr(&s->memory, address, bytes);
    assert(data);
    uint8_t *copy = malloc(bytes);
    assert(copy);
    memcpy(copy, data, bytes);
    assert(call(s, object, 12, NULL, 0) == 0);
    return copy;
}
static void assert_buffer_unchanged(D9ComDemo *s, uint32_t object,
                                    const uint8_t *snapshot, size_t bytes) {
    uint8_t *current = buffer_snapshot(s, object, bytes);
    assert(!memcmp(current, snapshot, bytes));
    free(current);
}

int main(void) {
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "patch/process: demo init failed: %s\n", demo.message);
        return 1;
    }

    size_t vb_bytes = 24u * sizeof(D9Vertex), ib_bytes = 36u * sizeof(uint16_t);
    uint8_t *vb_before = buffer_snapshot(&demo, demo.vb, vb_bytes);
    uint8_t *ib_before = buffer_snapshot(&demo, demo.ib, ib_bytes);
    uint8_t caps_before[CAPS_BYTES], caps_after[CAPS_BYTES];
    memset(caps_before, 0xcd, sizeof caps_before);
    assert(device_call(&demo, 7, (uint32_t[]){CAPS}, 1) == 0);
    assert(!guest_read(&demo.memory, CAPS, caps_before, sizeof caps_before));
    size_t memory_before = d9_memory_used(d);
    uint32_t refs_before = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_before - 1);

    /* ProcessVertices never reads or writes either buffer and never submits
     * software work, regardless of the supplied destination/declaration. */
    uint32_t process[] = {0, 0, 3, demo.vb, 0, 0};
    assert(device_call(&demo, 85, process, 6) == ERR_UNAVAILABLE);
    expect_diagnostic("ProcessVertices: software vertex processing is not provided; use hardware vertex shaders");
    assert_buffer_unchanged(&demo, demo.vb, vb_before, vb_bytes);
    assert_buffer_unchanged(&demo, demo.ib, ib_before, ib_bytes);

    uint32_t rect_patch[] = {0x1234u, 0};
    assert(device_call(&demo, 115, rect_patch, 2) == ERR_UNAVAILABLE);
    expect_diagnostic("DrawRectPatch: N-patch drawing is not implemented");
    uint32_t tri_patch[] = {0x1234u, 0, 0};
    assert(device_call(&demo, 116, tri_patch, 3) == ERR_UNAVAILABLE);
    expect_diagnostic("DrawTriPatch: N-patch drawing is not implemented");
    assert_buffer_unchanged(&demo, demo.vb, vb_before, vb_bytes);
    assert_buffer_unchanged(&demo, demo.ib, ib_before, ib_bytes);

    /* D3D9 permits deleting an unknown patch handle; it is a no-op. */
    assert(device_call(&demo, 117, (uint32_t[]){0x1234u}, 1) == 0);
    assert(device_call(&demo, 117, (uint32_t[]){0x1234u}, 1) == 0);
    assert(device_call(&demo, 117, (uint32_t[]){0}, 1) == 0);

    assert(device_call(&demo, 7, (uint32_t[]){CAPS}, 1) == 0);
    assert(!guest_read(&demo.memory, CAPS, caps_after, sizeof caps_after));
    assert(!memcmp(caps_before, caps_after, sizeof caps_before));
    assert(d9_memory_used(d) == memory_before);
    uint32_t refs_after = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_after - 1);
    assert(refs_after == refs_before);

    free(vb_before); free(ib_before);
    assert(!d9_com_demo_destroy(&demo));
    assert(d9_destroy_device_ex(d) == 0);
    puts("patch/process refusals: named NOTAVAILABLE, idempotent DeletePatch, buffers/caps unchanged PASS");
    return 0;
}
