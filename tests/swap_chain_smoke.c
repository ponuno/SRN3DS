/* Implicit IDirect3DSwapChain9 and CreateAdditionalSwapChain.
 *
 * The bridge has exactly one swap chain, backed by the same g.back surface as
 * Device::GetBackBuffer(0,0).  Present delegates to the device scene state
 * machine.  GetPresentParameters returns the real creation values, and second
 * swap chains are a named D3DERR_NOTAVAILABLE refusal with a zero output.
 */
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
#define NOINTERFACE 0x80004002u

static const uint8_t iid_unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_swapchain[16] = {
    0xf2,0x50,0x49,0x79,0xfc,0xad,0x8a,0x45,0x90,0x5e,0x10,0xa1,0x0b,0x0b,0x50,0x3b
};
static const uint32_t expected_pp[14] = {
    400, 240, D9_X8R8G8B8, 1, 0, 0, 1, 0, 1, 0, 75, 0, 0, 1
};

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
    uint32_t device_refs = refs(&demo, demo.device);
    uint32_t get_sc[] = {0, OUT};
    assert(device_call(&demo, 14, get_sc, 2) == 0);
    uint32_t chain = read32(&demo, OUT);
    assert(chain);
    assert(refs(&demo, chain) == 1);
    assert(refs(&demo, demo.device) == device_refs + 1);

    /* GetSwapChain(0) is stable and returns another reference to the same
     * object; another index is a named INVALIDCALL. */
    assert(device_call(&demo, 14, get_sc, 2) == 0);
    assert(read32(&demo, OUT) == chain);
    assert(refs(&demo, chain) == 2);
    get_sc[0] = 1;
    assert(!guest_write32(&demo.memory, OUT, 0xdeadbeefu));
    assert(device_call(&demo, 14, get_sc, 2) == ERR_INVALID);
    assert(read32(&demo, OUT) == 0);
    assert(strstr(d9_com_last_diagnostic(), "swap chain 0") != NULL);

    /* QI accepts IUnknown and IID_IDirect3DSwapChain9 and rejects others. */
    assert(!guest_write(&demo.memory, DATA, iid_unknown, sizeof iid_unknown));
    assert(call(&demo, chain, 0, (uint32_t[]){DATA, OUT2}, 2) == 0);
    assert(read32(&demo, OUT2) == chain);
    assert(refs(&demo, chain) == 3);
    assert(!guest_write(&demo.memory, DATA, iid_swapchain, sizeof iid_swapchain));
    assert(call(&demo, chain, 0, (uint32_t[]){DATA, OUT2}, 2) == 0);
    assert(read32(&demo, OUT2) == chain);
    assert(refs(&demo, chain) == 4);
    uint8_t wrong_iid[16];
    memset(wrong_iid, 0xaa, sizeof wrong_iid);
    assert(!guest_write(&demo.memory, DATA, wrong_iid, sizeof wrong_iid));
    assert(!guest_write32(&demo.memory, OUT2, chain));
    assert(call(&demo, chain, 0, (uint32_t[]){DATA, OUT2}, 2) == NOINTERFACE);
    assert(read32(&demo, OUT2) == 0);

    /* GetDevice retains the device; the caller releases that reference. */
    uint32_t refs_before_get = refs(&demo, demo.device);
    assert(call(&demo, chain, 7, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == demo.device);
    assert(refs(&demo, demo.device) == refs_before_get + 1);
    assert(call(&demo, demo.device, 2, NULL, 0) == refs_before_get);

    /* GetDisplayMode matches the single adapter mode. */
    assert(call(&demo, chain, 6, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 400 && read32(&demo, OUT2 + 4) == 240);
    assert(read32(&demo, OUT2 + 8) == 60 && read32(&demo, OUT2 + 12) == D9_X8R8G8B8);

    /* GetPresentParameters returns the actual creation values. */
    assert(call(&demo, chain, 8, (uint32_t[]){OUT2}, 1) == 0);
    expect_block(&demo, OUT2, expected_pp, sizeof expected_pp);

    /* Swap-chain GetBackBuffer and Device::GetBackBuffer(0,0) are the same. */
    assert(call(&demo, chain, 4, (uint32_t[]){0, 0, OUT2}, 3) == 0);
    uint32_t chain_back = read32(&demo, OUT2);
    assert(chain_back);
    uint32_t device_back[] = {0, 0, 0, OUT2};
    assert(device_call(&demo, 18, device_back, 4) == 0);
    assert(read32(&demo, OUT2) == chain_back);
    assert(refs(&demo, chain_back) == 3);
    assert(call(&demo, chain_back, 2, NULL, 0) == 2);
    assert(call(&demo, chain_back, 2, NULL, 0) == 1);
    assert(call(&demo, chain, 4, (uint32_t[]){1, 0, OUT2}, 3) == ERR_INVALID);
    assert(read32(&demo, OUT2) == 0);
    assert(call(&demo, chain, 4, (uint32_t[]){0, 1, OUT2}, 3) == ERR_INVALID);
    assert(read32(&demo, OUT2) == 0);

    /* Raster status is the same named refusal as the device's. */
    assert(!guest_write32(&demo.memory, OUT2, 0xdeadbeefu));
    assert(!guest_write32(&demo.memory, OUT2 + 4, 0xdeadbeefu));
    assert(call(&demo, chain, 5, (uint32_t[]){OUT2}, 1) == ERR_UNAVAILABLE);
    assert(read32(&demo, OUT2) == 0 && read32(&demo, OUT2 + 4) == 0);
    assert(strstr(d9_com_last_diagnostic(), "raster") != NULL);

    /* Present through the swap chain uses the device scene state machine. */
    assert(device_call(&demo, 41, NULL, 0) == 0);
    assert(device_call(&demo, 42, NULL, 0) == 0);
    uint32_t present_args[] = {0, 0, 0, 0};
    assert(call(&demo, chain, 3, present_args, 4) == 0);
    assert(call(&demo, chain, 3, present_args, 4) == ERR_INVALID); /* no active scene */

    /* A second swap chain is refused without an object. */
    assert(!guest_write(&demo.memory, DATA, expected_pp, sizeof expected_pp));
    assert(!guest_write32(&demo.memory, OUT2, 0xdeadbeefu));
    assert(device_call(&demo, 13, (uint32_t[]){DATA, OUT2}, 2) == ERR_UNAVAILABLE);
    assert(read32(&demo, OUT2) == 0);
    assert(strstr(d9_com_last_diagnostic(), "second swap chains") != NULL);
    assert(refs(&demo, chain) == 4);

    /* Drop the two GetSwapChain references and two QI references. */
    assert(call(&demo, chain, 2, NULL, 0) == 3);
    assert(call(&demo, chain, 2, NULL, 0) == 2);
    assert(call(&demo, chain, 2, NULL, 0) == 1);
    assert(call(&demo, chain, 2, NULL, 0) == 0);
    assert(read32(&demo, chain) == 0);
    assert(refs(&demo, demo.device) == device_refs);
    assert(d9_memory_used(d) == used);

    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("swap chain smoke: implicit chain, QI, present parameters, Present and refusal PASS");
    return 0;
}
