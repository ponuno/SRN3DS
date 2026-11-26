/* Pixel-shader constant registers, slots 109..114.
 *
 * SetPixelShaderConstantF writes the c0..c7 bank that the menu TEV draw
 * actually reads.  SetPixelShaderConstantI/B are bounded API state: the
 * accepted ps_3_0 subset has no integer/predicate/flow-control opcode, so they
 * round-trip through Get but cannot affect a draw.  The test covers the exact
 * round-trip, partial ranges, the c8 boundary, n==0, double Set, named
 * refusals that leave the old values intact, bool normalisation and the
 * runtime bank visible to d9_get_tev_constants().
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_menu_shader_runtime.h"
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
    return device_call(s, 109, args, 3);
}
static uint32_t get_f(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 110, args, 3);
}
static uint32_t set_i(D9ComDemo *s, unsigned start, const int32_t *values,
                      unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 16),
                       count};
    return device_call(s, 111, args, 3);
}
static uint32_t set_b(D9ComDemo *s, unsigned start, const int32_t *values,
                      unsigned count) {
    uint32_t args[] = {start, address_of(s, DATA, values, (size_t)count * 4),
                       count};
    return device_call(s, 113, args, 3);
}
static uint32_t get_i(D9ComDemo *s, unsigned start, unsigned count) {
    uint32_t args[] = {start, OUT, count};
    return device_call(s, 112, args, 3);
}
static uint32_t get_b(D9ComDemo *s, unsigned start, unsigned count) {
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

    /* Fresh registers are zero, and a zero-length write is a no-op. */
    float zero[8][4] = {{0}};
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, zero, sizeof zero);
    uint32_t zero_args[] = {8, 0, 0};
    assert(device_call(&demo, 109, zero_args, 3) == 0);
    zero_args[0] = 9;
    assert(device_call(&demo, 109, zero_args, 3) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "9") != NULL);

    /* Partial range leaves c0/c1 untouched and writes exactly c2/c3. */
    float partial[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    assert(set_f(&demo, 2, (const float *)partial, 2) == 0);
    assert(get_f(&demo, 0, 4) == 0);
    float got[8][4] = {{0}};
    if (guest_read(&demo.memory, OUT, got, 4 * 16)) abort();
    assert(!memcmp(got[0], zero[0], 16) && !memcmp(got[1], zero[1], 16));
    assert(!memcmp(got[2], partial[0], 16));
    assert(!memcmp(got[3], partial[1], 16));

    /* Full bank, then double Set of one row. */
    float full[8][4];
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j)
            full[i][j] = (float)(i * 4 + j) + 0.25f;
    assert(set_f(&demo, 0, (const float *)full, 8) == 0);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, full, sizeof full);

    float twice[2][4] = {{9, 10, 11, 12}, {13, 14, 15, 16}};
    assert(set_f(&demo, 2, (const float *)twice, 2) == 0);
    memcpy(full[2], twice[0], 16);
    memcpy(full[3], twice[1], 16);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, full, sizeof full);

    /* Boundary: c7 is the last register; c7+2 and c8 are refused without a
     * partial write.  start==8 with n==0 is an empty range and is accepted. */
    float last[4] = {21, 22, 23, 24};
    assert(set_f(&demo, 7, last, 1) == 0);
    memcpy(full[7], last, 16);
    float over[2][4] = {{31, 32, 33, 34}, {35, 36, 37, 38}};
    assert(set_f(&demo, 7, (const float *)over, 2) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "c7") != NULL);
    assert(set_f(&demo, 8, (const float *)over, 1) == ERR_INVALID);
    float nan_value[4] = {NAN, 0, 0, 1};
    assert(set_f(&demo, 5, nan_value, 1) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "finite") != NULL ||
           strstr(d9_com_last_diagnostic(), "NaN") != NULL);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, full, sizeof full);

    /* Invalid pointer and a refused write must not change state. */
    uint32_t bad_ptr[] = {0, 0, 1};
    assert(device_call(&demo, 109, bad_ptr, 3) == ERR_INVALID);
    assert(get_f(&demo, 0, 8) == 0);
    expect_block(&demo, OUT, full, sizeof full);

    /* Integer registers: exact 32-bit round-trip, partial range, boundary. */
    int32_t ints[8][4];
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j)
            ints[i][j] = (int32_t)(i * 4 + j) - 13;
    assert(set_i(&demo, 0, &ints[0][0], 8) == 0);
    uint32_t get_i_args[] = {0, OUT, 8};
    assert(device_call(&demo, 112, get_i_args, 3) == 0);
    expect_block(&demo, OUT, ints, sizeof ints);
    int32_t int_partial[2][4] = {{100, -101, 102, -103}, {104, -105, 106, -107}};
    assert(set_i(&demo, 6, &int_partial[0][0], 2) == 0);
    memcpy(ints[6], int_partial[0], 16);
    memcpy(ints[7], int_partial[1], 16);
    assert(set_i(&demo, 0, &int_partial[0][0], 2) == 0); /* double Set */
    memcpy(ints[0], int_partial[0], 16);
    memcpy(ints[1], int_partial[1], 16);
    assert(device_call(&demo, 112, get_i_args, 3) == 0);
    expect_block(&demo, OUT, ints, sizeof ints);
    assert(set_i(&demo, 8, &int_partial[0][0], 1) == ERR_INVALID);
    assert(set_i(&demo, 7, &int_partial[0][0], 2) == ERR_INVALID);
    assert(device_call(&demo, 112, get_i_args, 3) == 0);
    expect_block(&demo, OUT, ints, sizeof ints); /* refusals changed nothing */
    get_i_args[0] = 8; get_i_args[2] = 0;
    assert(device_call(&demo, 112, get_i_args, 3) == 0);
    get_i_args[0] = 9;
    assert(device_call(&demo, 112, get_i_args, 3) == ERR_INVALID);

    /* Bool registers normalise every nonzero value to 1. */
    int32_t bools[8] = {0, -1, 2, 0x7fffffff, 1, 0, 5, -3};
    int32_t bool_expected[8] = {0, 1, 1, 1, 1, 0, 1, 1};
    assert(set_b(&demo, 0, bools, 8) == 0);
    uint32_t get_b_args[] = {0, OUT, 8};
    assert(device_call(&demo, 114, get_b_args, 3) == 0);
    expect_block(&demo, OUT, bool_expected, sizeof bool_expected);
    int32_t bool_partial[2] = {7, 0};
    int32_t bool_partial_expected[2] = {1, 0};
    assert(set_b(&demo, 6, bool_partial, 2) == 0);
    memcpy(bool_expected + 6, bool_partial_expected, 2 * 4);
    assert(set_b(&demo, 0, bool_partial, 2) == 0); /* double Set */
    memcpy(bool_expected + 0, bool_partial_expected, 2 * 4);
    assert(device_call(&demo, 114, get_b_args, 3) == 0);
    expect_block(&demo, OUT, bool_expected, sizeof bool_expected);
    assert(set_b(&demo, 8, bool_partial, 1) == ERR_INVALID);
    assert(set_b(&demo, 7, bool_partial, 2) == ERR_INVALID);
    assert(device_call(&demo, 114, get_b_args, 3) == 0);
    expect_block(&demo, OUT, bool_expected, sizeof bool_expected);
    get_b_args[0] = 8; get_b_args[2] = 0;
    assert(device_call(&demo, 114, get_b_args, 3) == 0);
    get_b_args[0] = 9;
    assert(device_call(&demo, 114, get_b_args, 3) == ERR_INVALID);
    uint32_t zero_i[] = {8, 0, 0};
    assert(device_call(&demo, 111, zero_i, 3) == 0);
    assert(device_call(&demo, 113, zero_i, 3) == 0);
    zero_i[0] = 9;
    assert(device_call(&demo, 111, zero_i, 3) == ERR_INVALID);
    assert(device_call(&demo, 113, zero_i, 3) == ERR_INVALID);

    /* Runtime consistency: the float rows land in the same nine-row bank the
     * menu draw consumes, and are not clobbered by a later VS/ambient upload. */
    float runtime[9][4];
    assert(d9_get_tev_constants(d, runtime) == 0);
    assert(!memcmp(runtime[0], full[0], 16));
    assert(!memcmp(runtime[7], full[7], 16));

    float vs_rows[9][4] = {{0}};
    vs_rows[3][3] = 0.75f;
    uint32_t vs_args[] = {0, address_of(&demo, DATA, vs_rows, sizeof vs_rows), 9};
    assert(device_call(&demo, 94, vs_args, 3) == 0);
    assert(d9_get_tev_constants(d, runtime) == 0);
    assert(!memcmp(runtime[3], full[3], 16)); /* PS c3 wins over VS c3 */

    float alpha_row[4] = {0.1f, 0.2f, 0.3f, 0.5f};
    assert(set_f(&demo, 4, alpha_row, 1) == 0);
    memcpy(full[4], alpha_row, 16);
    uint32_t observed = d9_com_menu_observed_preconditions();
    assert((observed & D9_MENU_SHADER_PRE_MATERIAL_CONSTANTS_C0_C7) != 0);
    assert((observed & D9_MENU_SHADER_PRE_MATERIAL_ALPHA_C4) != 0);

    /* Set is allowed inside a scene and refused after EndScene; Get stays
     * readable and the refused write changes nothing. */
    assert(device_call(&demo, 41, NULL, 0) == 0);
    float in_scene[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    assert(set_f(&demo, 5, in_scene, 1) == 0);
    assert(device_call(&demo, 42, NULL, 0) == 0);
    float after_end[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    assert(set_f(&demo, 5, after_end, 1) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "EndScene") != NULL);
    assert(get_f(&demo, 5, 1) == 0);
    expect_block(&demo, OUT, in_scene, sizeof in_scene);
    uint32_t present_args[] = {0, 0, 0, 0};
    assert(device_call(&demo, 17, present_args, 4) == 0);

    assert(d9_memory_used(d) == used);
    uint32_t refs_after = call(&demo, demo.device, 1, NULL, 0);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_after - 1);
    assert(refs_after == refs_before);

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("device PS constants: F/I/B roundtrip, bounds, n=0, refusals and TEV bank PASS");
    return 0;
}
