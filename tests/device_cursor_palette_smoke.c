/* Cursor and legacy palette device state (slots 10/11/12 and 71..74).
 *
 * The cursor surface is CPU-backed state only; PICA has no hardware cursor and
 * nothing is drawn.  Palettes are bounded 256xPALETTEENTRY CPU arrays; paletted
 * texture formats are not implemented, so the current palette never changes a
 * draw.  ShowCursor must return the previous visibility exactly.
 */
#include "d3d9_adapter.h"
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
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
static uint32_t refs(D9ComDemo *s, uint32_t object) {
    uint32_t after_add = call(s, object, 1, NULL, 0);
    assert(call(s, object, 2, NULL, 0) == after_add - 1);
    return after_add - 1;
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
    uint32_t device_refs = refs(&demo, demo.device);
    D9Caps9 caps_before, caps_after;
    assert(!d9_adapter_caps(0, 1, &caps_before));

    /* CPU-backed 16x16 cursor surface. */
    uint32_t surface_args[] = {16, 16, D9_A8R8G8B8, 2, OUT, 0};
    assert(device_call(&demo, 36, surface_args, 6) == 0);
    uint32_t cursor_surface = read32(&demo, OUT);
    assert(cursor_surface);
    assert(refs(&demo, demo.device) == device_refs + 1);

    uint32_t set_cursor[] = {3, 4, cursor_surface};
    assert(device_call(&demo, 10, set_cursor, 3) == 0);
    set_cursor[0] = 0; set_cursor[1] = 0;
    assert(device_call(&demo, 10, set_cursor, 3) == 0);
    set_cursor[0] = 16;
    assert(device_call(&demo, 10, set_cursor, 3) == ERR_INVALID);
    set_cursor[0] = 0; set_cursor[1] = 16;
    assert(device_call(&demo, 10, set_cursor, 3) == ERR_INVALID);
    set_cursor[0] = 0; set_cursor[1] = 0; set_cursor[2] = 0;
    assert(device_call(&demo, 10, set_cursor, 3) == ERR_INVALID);

    /* The back buffer is not CPU-backed. */
    uint32_t back_args[] = {0, 0, 0, OUT2};
    assert(device_call(&demo, 18, back_args, 4) == 0);
    uint32_t back = read32(&demo, OUT2);
    assert(back);
    set_cursor[2] = back;
    assert(device_call(&demo, 10, set_cursor, 3) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "CPU-backed") != NULL);
    assert(call(&demo, back, 2, NULL, 0) == 1);

    /* Cursor position flags are bounded; X/Y are signed ints. */
    uint32_t pos[] = {10, (uint32_t)-5, 0};
    assert(device_call(&demo, 11, pos, 3) == 0);
    pos[2] = 1;
    assert(device_call(&demo, 11, pos, 3) == 0);
    pos[2] = 2;
    assert(device_call(&demo, 11, pos, 3) == ERR_INVALID);

    /* ShowCursor returns the previous visibility, not the new one. */
    assert(device_call(&demo, 12, (uint32_t[]){1}, 1) == 0);
    assert(device_call(&demo, 12, (uint32_t[]){0}, 1) == 1);
    assert(device_call(&demo, 12, (uint32_t[]){7}, 1) == 0);
    assert(device_call(&demo, 12, (uint32_t[]){0}, 1) == 1);

    /* Two independent 256-entry palettes round-trip byte-exactly. */
    uint8_t pal0[1024], pal1[1024], zero[1024];
    memset(zero, 0, sizeof zero);
    for (unsigned i = 0; i < 256; ++i) {
        pal0[i * 4 + 0] = (uint8_t)i;
        pal0[i * 4 + 1] = (uint8_t)(i ^ 0x55);
        pal0[i * 4 + 2] = (uint8_t)(255 - i);
        pal0[i * 4 + 3] = (uint8_t)(i * 3);
        pal1[i * 4 + 0] = (uint8_t)(i * 7);
        pal1[i * 4 + 1] = (uint8_t)(i * 5);
        pal1[i * 4 + 2] = (uint8_t)(i * 3);
        pal1[i * 4 + 3] = (uint8_t)(255 - i);
    }
    assert(!guest_write(&demo.memory, DATA, pal0, sizeof pal0));
    assert(device_call(&demo, 71, (uint32_t[]){0, DATA}, 2) == 0);
    assert(device_call(&demo, 72, (uint32_t[]){0, OUT}, 2) == 0);
    uint8_t got[1024];
    assert(!guest_read(&demo.memory, OUT, got, sizeof got));
    assert(!memcmp(got, pal0, sizeof pal0));

    assert(!guest_write(&demo.memory, DATA, pal1, sizeof pal1));
    assert(device_call(&demo, 71, (uint32_t[]){255, DATA}, 2) == 0);
    assert(device_call(&demo, 72, (uint32_t[]){255, OUT}, 2) == 0);
    assert(!guest_read(&demo.memory, OUT, got, sizeof got));
    assert(!memcmp(got, pal1, sizeof pal1));

    /* Unset palette defaults to black; out-of-range indices and pointers fail. */
    assert(device_call(&demo, 72, (uint32_t[]){1, OUT}, 2) == 0);
    assert(!guest_read(&demo.memory, OUT, got, sizeof got));
    assert(!memcmp(got, zero, sizeof zero));
    assert(device_call(&demo, 71, (uint32_t[]){256, DATA}, 2) == ERR_INVALID);
    assert(device_call(&demo, 72, (uint32_t[]){256, OUT}, 2) == ERR_INVALID);
    assert(device_call(&demo, 71, (uint32_t[]){0, 0}, 2) == ERR_INVALID);
    assert(device_call(&demo, 72, (uint32_t[]){0, 0}, 2) == ERR_INVALID);

    /* Current palette number is stored exactly. */
    assert(device_call(&demo, 73, (uint32_t[]){7}, 1) == 0);
    assert(device_call(&demo, 74, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 7);
    assert(device_call(&demo, 73, (uint32_t[]){255}, 1) == 0);
    assert(device_call(&demo, 74, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 255);
    assert(device_call(&demo, 73, (uint32_t[]){256}, 1) == ERR_INVALID);
    assert(device_call(&demo, 74, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 255);

    assert(refs(&demo, demo.device) == device_refs + 1);
    assert(call(&demo, cursor_surface, 2, NULL, 0) == 0);
    assert(refs(&demo, demo.device) == device_refs);
    assert(d9_memory_used(d) == used);
    assert(!d9_adapter_caps(0, 1, &caps_after));
    assert(!memcmp(&caps_before, &caps_after, sizeof caps_before));

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("device cursor/palette: visibility, CPU surface, palettes and caps PASS");
    return 0;
}
