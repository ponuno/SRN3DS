/* Vertex-shader constant registers, slots 94..99.
 *
 * F remains the legacy c0..c8 menu material bank.  The new I/B registers use
 * the same bounded 8-register API-state window as PS I/B: exact 32-bit
 * round-trip for I, nonzero->1 normalisation for B, n==0 no-op, start==limit
 * with n==0 accepted, and named refusals beyond the limit without a partial
 * write.  The accepted VS subset has no integer/predicate/flow-control opcode,
 * so I/B never change a draw.  The test also proves F/I/B and VS/PS banks do
 * not overwrite each other.
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_menu_shader_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define DATA (SCRATCH + 0x800u)
#define OUT (SCRATCH + 0x900u)
#define ERR_INVALID 0x8876086cu

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
static uint32_t address_of(D9ComDemo *s, uint32_t address,
                           const void *data, size_t bytes) {
    if (guest_write(&s->memory, address, data, bytes)) abort();
    return address;
}
static void expect_block(D9ComDemo *s, uint32_t address, const void *data,
                         size_t bytes) {
    assert(bytes <= 256);
    uint8_t got[256];
    if (guest_read(&s->memory, address, got, bytes)) abort();
    assert(!memcmp(got, data, bytes));
}
static uint32_t set_f(D9ComDemo *s, unsigned start, const float *values,
                      unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 16),
                       count};
    return device_call(s, 94, args, 3);
}
static uint32_t get_f(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 95, args, 3);
}
static uint32_t set_i(D9ComDemo *s, unsigned start, const int32_t *values,
                      unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 16),
                       count};
    return device_call(s, 96, args, 3);
}
static uint32_t get_i(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 97, args, 3);
}
static uint32_t set_b(D9ComDemo *s, unsigned start, const int32_t *values,
                      unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 4),
                       count};
    return device_call(s, 98, args, 3);
}
static uint32_t get_b(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 99, args, 3);
}
static uint32_t set_ps_f(D9ComDemo *s, unsigned start, const float *values,
                         unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 16),
                       count};
    return device_call(s, 109, args, 3);
}
static uint32_t set_ps_i(D9ComDemo *s, unsigned start, const int32_t *values,
                         unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 16),
                       count};
    return device_call(s, 111, args, 3);
}
static uint32_t get_ps_i(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 112, args, 3);
}
static uint32_t set_ps_b(D9ComDemo *s, unsigned start, const int32_t *values,
                         unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 4),
                       count};
    return device_call(s, 113, args, 3);
}
static uint32_t get_ps_b(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 114, args, 3);
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

    /* Fresh F bank is zero; c8 is the existing ninth material row. */
    float zero_f[9][4] = {{0}};
    assert(get_f(&demo, 0, 9) == 0);
    expect_block(&demo, OUT, zero_f, sizeof zero_f);

    float vf[9][4];
    for (unsigned i = 0; i < 9; ++i)
        for (unsigned j = 0; j < 4; ++j)
            vf[i][j] = (float)(i * 4 + j) + 0.5f;
    assert(set_f(&demo, 0, (const float *)vf, 8) == 0);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vf, 8 * 16);

    float c8[4] = {91, 92, 93, 94};
    assert(set_f(&demo, 8, c8, 1) == 0);
    memcpy(vf[8], c8, 16);
    assert(get_f(&demo, 8, 1) == 0);
    expect_block(&demo, OUT, c8, sizeof c8);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vf, 8 * 16);

    /* Integer window: exact 32-bit values, partial range, double Set. */
    int32_t vi[8][4];
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j)
            vi[i][j] = (int32_t)(i * 4 + j) - 29;
    assert(set_i(&demo, 0, &vi[0][0], 8) == 0);
    assert(get_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vi, sizeof vi);
    int32_t vi_partial[2][4] = {{500, -501, 502, -503}, {504, -505, 506, -507}};
    assert(set_i(&demo, 0, &vi_partial[0][0], 2) == 0);
    memcpy(vi[0], vi_partial[0], 16);
    memcpy(vi[1], vi_partial[1], 16);
    assert(get_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vi, sizeof vi);

    /* Bool window normalises nonzero and keeps the other banks untouched. */
    int32_t vb[8] = {0, -1, 2, 0x7fffffff, 1, 0, 9, -7};
    int32_t vb_expected[8] = {0, 1, 1, 1, 1, 0, 1, 1};
    assert(set_b(&demo, 0, vb, 8) == 0);
    assert(get_b(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vb_expected, sizeof vb_expected);

    /* F, I and B are separate banks in both shader stages. */
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vf, 8 * 16);
    assert(get_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vi, sizeof vi);

    float vf_c3[4] = {10, 11, 12, 13};
    assert(set_f(&demo, 3, vf_c3, 1) == 0);
    memcpy(vf[3], vf_c3, 16);
    float ps_c3[4] = {20, 21, 22, 23};
    assert(set_ps_f(&demo, 3, ps_c3, 1) == 0);
    assert(get_f(&demo, 3, 1) == 0);
    expect_block(&demo, OUT, vf_c3, sizeof vf_c3);
    float runtime[9][4];
    assert(d9_get_tev_constants(d, runtime) == 0);
    assert(!memcmp(runtime[3], ps_c3, 16));

    int32_t ps_i[8][4];
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j)
            ps_i[i][j] = (int32_t)(i * 4 + j) + 1000;
    assert(set_ps_i(&demo, 0, &ps_i[0][0], 8) == 0);
    assert(get_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vi, sizeof vi);

    int32_t ps_b[8] = {1, 0, 1, 0, 1, 0, 1, 0};
    assert(set_ps_b(&demo, 0, ps_b, 8) == 0);
    assert(get_b(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vb_expected, sizeof vb_expected);

    /* The reverse direction: writing VS I/B must not change PS I/B. */
    int32_t vi_c3[4] = {-71, -72, -73, -74};
    assert(set_i(&demo, 3, vi_c3, 1) == 0);
    memcpy(vi[3], vi_c3, 16);
    assert(get_ps_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, ps_i, sizeof ps_i);
    int32_t vb_c3[1] = {0};
    assert(set_b(&demo, 3, vb_c3, 1) == 0);
    vb_expected[3] = 0;
    assert(get_ps_b(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, ps_b, sizeof ps_b);

    /* Boundary refusals leave every bank exactly as it was. */
    uint32_t bad_zero[] = {8, 0, 0};
    assert(device_call(&demo, 96, bad_zero, 3) == 0);
    assert(device_call(&demo, 98, bad_zero, 3) == 0);
    bad_zero[0] = 9;
    assert(device_call(&demo, 96, bad_zero, 3) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "VS window") != NULL);
    assert(device_call(&demo, 98, bad_zero, 3) == ERR_INVALID);

    int32_t vi_over[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    assert(set_i(&demo, 7, &vi_over[0][0], 2) == ERR_INVALID);
    int32_t vb_over[2] = {1, 0};
    assert(set_b(&demo, 7, vb_over, 2) == ERR_INVALID);
    uint32_t bad_ptr_i[] = {0, 0, 1};
    assert(device_call(&demo, 96, bad_ptr_i, 3) == ERR_INVALID);
    uint32_t bad_ptr_b[] = {0, 0, 1};
    assert(device_call(&demo, 98, bad_ptr_b, 3) == ERR_INVALID);
    float vf_over[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    assert(set_f(&demo, 8, (const float *)vf_over, 2) == ERR_INVALID);
    assert(device_call(&demo, 95, (uint32_t[]){9, OUT, 0}, 3) == ERR_INVALID);

    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vf, 8 * 16);
    assert(get_f(&demo, 8, 1) == 0);
    expect_block(&demo, OUT, c8, sizeof c8);
    assert(get_i(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vi, sizeof vi);
    assert(get_b(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, vb_expected, sizeof vb_expected);

    /* Set works inside a scene and is refused after EndScene; Get remains. */
    assert(device_call(&demo, 41, NULL, 0) == 0);
    int32_t phase_i[4] = {31, 32, 33, 34};
    int32_t phase_b[1] = {1};
    assert(set_i(&demo, 5, phase_i, 1) == 0);
    assert(set_b(&demo, 5, phase_b, 1) == 0);
    assert(device_call(&demo, 42, NULL, 0) == 0);
    int32_t after_i[4] = {41, 42, 43, 44};
    int32_t after_b[1] = {0};
    assert(set_i(&demo, 5, after_i, 1) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "EndScene") != NULL);
    assert(set_b(&demo, 5, after_b, 1) == ERR_INVALID);
    assert(get_i(&demo, 5, 1) == 0);
    expect_block(&demo, OUT, phase_i, sizeof phase_i);
    assert(get_b(&demo, 5, 1) == 0);
    expect_block(&demo, OUT, phase_b, sizeof phase_b);
    uint32_t present_args[] = {0, 0, 0, 0};
    assert(device_call(&demo, 17, present_args, 4) == 0);

    assert(d9_memory_used(d) == used);
    uint32_t refs_after = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_after - 1);
    assert(refs_after == refs_before);

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("device VS constants: F/I/B roundtrip, bounds, cross-bank isolation PASS");
    return 0;
}
