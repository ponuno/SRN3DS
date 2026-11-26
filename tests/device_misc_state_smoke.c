/* Miscellaneous IDirect3DDevice9 state: slots 55/56, 62/63 and 77/78.
 *
 * Six user clip planes are stored exactly but never applied: PICA has no user
 * clip-plane stage, so a draw with D3DRS_CLIPPLANEENABLE != 0 must be a named
 * D3DERR_NOTAVAILABLE refusal.  ClipStatus is legacy CPU state with no draw
 * effect.  Software vertex processing is unavailable; the bridge always
 * reports hardware processing.
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define DATA (SCRATCH + 0x800u)
#define OUT (SCRATCH + 0x900u)
#define OUT2 (SCRATCH + 0x908u)
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
static void expect_block(D9ComDemo *s, uint32_t address, const void *data,
                         size_t bytes) {
    uint8_t got[64];
    assert(bytes <= sizeof got);
    assert(!guest_read(&s->memory, address, got, bytes));
    assert(!memcmp(got, data, bytes));
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
    uint32_t refs_before = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_before - 1);

    /* Six independent float4 clip planes; unset planes are zero. */
    float zero[4] = {0, 0, 0, 0};
    uint32_t get_plane[] = {0, OUT};
    assert(device_call(&demo, 56, get_plane, 2) == 0);
    expect_block(&demo, OUT, zero, sizeof zero);
    for (unsigned i = 0; i < 6; ++i) {
        float plane[4] = {(float)i, (float)i + 0.25f, (float)i + 0.5f, 1.0f};
        assert(!guest_write(&demo.memory, DATA, plane, sizeof plane));
        uint32_t set_plane[] = {i, DATA};
        assert(device_call(&demo, 55, set_plane, 2) == 0);
        get_plane[0] = i;
        assert(device_call(&demo, 56, get_plane, 2) == 0);
        expect_block(&demo, OUT, plane, sizeof plane);
    }
    uint32_t bad_plane[] = {6, DATA};
    assert(device_call(&demo, 55, bad_plane, 2) == ERR_INVALID);
    assert(device_call(&demo, 56, bad_plane, 2) == ERR_INVALID);
    float plane5[4];
    assert(!guest_read(&demo.memory, DATA, plane5, sizeof plane5));
    float nan_plane[4] = {0, 0, NAN, 1};
    assert(!guest_write(&demo.memory, DATA, nan_plane, sizeof nan_plane));
    uint32_t nan_set[] = {5, DATA};
    assert(device_call(&demo, 55, nan_set, 2) == ERR_INVALID);
    assert(device_call(&demo, 56, (uint32_t[]){5, OUT}, 2) == 0);
    expect_block(&demo, OUT, plane5, sizeof plane5);
    uint32_t bad_ptr[] = {0, 0};
    assert(device_call(&demo, 55, bad_ptr, 2) == ERR_INVALID);
    assert(device_call(&demo, 56, bad_ptr, 2) == ERR_INVALID);

    /* Legacy ClipStatus round-trips and does not affect a draw. */
    uint32_t status[2] = {0x12345678u, 0x9abcdef0u};
    assert(!guest_write(&demo.memory, DATA, status, sizeof status));
    assert(device_call(&demo, 62, (uint32_t[]){DATA}, 1) == 0);
    assert(device_call(&demo, 63, (uint32_t[]){OUT}, 1) == 0);
    expect_block(&demo, OUT, status, sizeof status);
    status[0] = 0xdeadbeefu; status[1] = 0x00c0ffeeu;
    assert(!guest_write(&demo.memory, DATA, status, sizeof status));
    assert(device_call(&demo, 62, (uint32_t[]){DATA}, 1) == 0);
    assert(device_call(&demo, 63, (uint32_t[]){OUT}, 1) == 0);
    expect_block(&demo, OUT, status, sizeof status);
    assert(device_call(&demo, 62, (uint32_t[]){0}, 1) == ERR_INVALID);
    assert(device_call(&demo, 63, (uint32_t[]){0}, 1) == ERR_INVALID);

    /* The bridge is hardware-only: FALSE is accepted, TRUE is named. */
    assert(device_call(&demo, 78, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 0);
    assert(device_call(&demo, 77, (uint32_t[]){0}, 1) == 0);
    assert(device_call(&demo, 78, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 0);
    assert(device_call(&demo, 77, (uint32_t[]){1}, 1) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "software") != NULL);
    assert(device_call(&demo, 78, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 0);

    /* Draw: disabled planes pass; any enabled bit is refused by name and the
     * backend receives no draw. */
    assert(device_call(&demo, 41, NULL, 0) == 0);
    uint32_t set_rs[] = {152, 0}; /* D3DRS_CLIPPLANEENABLE */
    assert(device_call(&demo, 57, set_rs, 2) == 0);
    uint32_t draw[] = {D9_TRIANGLELIST, 0, 1};
    unsigned draws = d9_null_stats()->draws;
    uint32_t draw_rc = device_call(&demo, 81, draw, 3);
    if (draw_rc) fprintf(stderr, "disabled-plane draw rc=0x%08x diag=[%s]\n",
                         draw_rc, d9_com_last_diagnostic());
    assert(draw_rc == 0);
    assert(d9_null_stats()->draws == draws + 1);
    set_rs[1] = 1;
    assert(device_call(&demo, 57, set_rs, 2) == 0);
    draws = d9_null_stats()->draws;
    assert(device_call(&demo, 81, draw, 3) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "clip plane") != NULL);
    assert(d9_null_stats()->draws == draws); /* nothing was submitted */
    assert(device_call(&demo, 58, (uint32_t[]){152, OUT2}, 2) == 0);
    assert(read32(&demo, OUT2) == 1);
    set_rs[1] = 0;
    assert(device_call(&demo, 57, set_rs, 2) == 0);
    draws = d9_null_stats()->draws;
    assert(device_call(&demo, 81, draw, 3) == 0);
    assert(d9_null_stats()->draws == draws + 1);
    set_rs[1] = 0x40; /* bit 6 is outside the six planes */
    assert(device_call(&demo, 57, set_rs, 2) == ERR_INVALID);
    set_rs[1] = 0;
    assert(device_call(&demo, 57, set_rs, 2) == 0);
    assert(device_call(&demo, 42, NULL, 0) == 0);
    assert(device_call(&demo, 17, (uint32_t[]){0, 0, 0, 0}, 4) == 0);

    assert(d9_memory_used(d) == used);
    uint32_t refs_after = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_after - 1);
    assert(refs_after == refs_before);

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("device misc state: clip planes/status, software VP and draw refusal PASS");
    return 0;
}
