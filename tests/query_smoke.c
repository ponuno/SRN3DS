/* IDirect3DDevice9::CreateQuery and IDirect3DQuery9.
 *
 * The bridge is synchronous, so only D3DQUERYTYPE_EVENT is implemented.
 * EVENT has a 4-byte BOOL payload: GetData refuses before Issue and reports
 * TRUE after Issue, including repeated calls and D3DGETDATA_FLUSH.  Every GPU
 * counter/timing query is a named D3DERR_NOTAVAILABLE refusal with no object.
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
#define QUERY_EVENT 8u
#define D3DGETDATA_FLUSH 1u

static const uint8_t iid_unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_query[16] = {
    0x60,0x14,0x77,0xd9,0x95,0xa6,0x26,0x4f,0xbb,0xd3,0x27,0xb8,0x40,0xb5,0x41,0xcc
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
static void write32(D9ComDemo *s, uint32_t address, uint32_t value) {
    assert(!guest_write32(&s->memory, address, value));
}
static uint32_t refs(D9ComDemo *s, uint32_t object) {
    uint32_t after_add = call(s, object, 1, NULL, 0);
    assert(call(s, object, 2, NULL, 0) == after_add - 1);
    return after_add - 1; /* stable count before the probe */
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
    uint32_t create_args[] = {QUERY_EVENT, OUT};

    /* EVENT query: create, type/size, Issue, TRUE result. */
    write32(&demo, OUT, 0xdeadbeefu);
    assert(device_call(&demo, 118, create_args, 2) == 0);
    uint32_t query = read32(&demo, OUT);
    assert(query);
    assert(refs(&demo, demo.device) == device_refs + 1);
    assert(refs(&demo, query) == 1);

    assert(call(&demo, query, 4, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == QUERY_EVENT);
    assert(call(&demo, query, 5, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == 4);

    /* Before Issue there is no completed work to report. */
    write32(&demo, OUT2, 0xdeadbeefu);
    uint32_t data_args[] = {OUT2, 4, 0};
    assert(call(&demo, query, 7, data_args, 3) == ERR_INVALID);
    assert(strstr(d9_com_last_diagnostic(), "issued") != NULL);
    assert(read32(&demo, OUT2) == 0xdeadbeefu);

    uint32_t issue_end[] = {1}; /* D3DISSUE_END */
    assert(call(&demo, query, 6, issue_end, 1) == 0);
    assert(call(&demo, query, 6, issue_end, 1) == 0); /* repeat Issue is allowed */
    assert(call(&demo, query, 6, (uint32_t[]){2}, 1) == ERR_INVALID); /* BEGIN */
    assert(call(&demo, query, 7, data_args, 3) == 0);
    assert(read32(&demo, OUT2) == 1);
    write32(&demo, OUT2, 0);
    assert(call(&demo, query, 7, data_args, 3) == 0);
    assert(read32(&demo, OUT2) == 1);
    data_args[2] = D3DGETDATA_FLUSH; /* accepted, same completed semantics */
    write32(&demo, OUT2, 0);
    assert(call(&demo, query, 7, data_args, 3) == 0);
    assert(read32(&demo, OUT2) == 1);

    /* Bad flags and payload sizes are named INVALIDCALL, not silent writes. */
    data_args[2] = 2;
    write32(&demo, OUT2, 0xdeadbeefu);
    assert(call(&demo, query, 7, data_args, 3) == ERR_INVALID);
    assert(read32(&demo, OUT2) == 0xdeadbeefu);
    data_args[2] = 0; data_args[1] = 3;
    assert(call(&demo, query, 7, data_args, 3) == ERR_INVALID);
    data_args[1] = 4; data_args[0] = 0;
    assert(call(&demo, query, 7, data_args, 3) == ERR_INVALID);

    /* GetDevice returns the creating device and takes a reference. */
    uint32_t refs_before_get = refs(&demo, demo.device);
    assert(call(&demo, query, 3, (uint32_t[]){OUT2}, 1) == 0);
    assert(read32(&demo, OUT2) == demo.device);
    assert(refs(&demo, demo.device) == refs_before_get + 1);
    assert(refs(&demo, demo.device) == device_refs + 2);
    assert(call(&demo, demo.device, 2, NULL, 0) == device_refs + 1); /* drop GetDevice ref */

    /* QI accepts IUnknown and IID_IDirect3DQuery9, and rejects others. */
    assert(!guest_write(&demo.memory, DATA, iid_unknown, sizeof iid_unknown));
    assert(call(&demo, query, 0, (uint32_t[]){DATA, OUT2}, 2) == 0);
    assert(read32(&demo, OUT2) == query);
    assert(refs(&demo, query) == 2);
    assert(!guest_write(&demo.memory, DATA, iid_query, sizeof iid_query));
    assert(call(&demo, query, 0, (uint32_t[]){DATA, OUT2}, 2) == 0);
    assert(read32(&demo, OUT2) == query);
    assert(refs(&demo, query) == 3);
    uint8_t wrong_iid[16];
    memset(wrong_iid, 0xaa, sizeof wrong_iid);
    assert(!guest_write(&demo.memory, DATA, wrong_iid, sizeof wrong_iid));
    write32(&demo, OUT2, query);
    assert(call(&demo, query, 0, (uint32_t[]){DATA, OUT2}, 2) == NOINTERFACE);
    assert(read32(&demo, OUT2) == 0);
    assert(call(&demo, query, 2, NULL, 0) == 2); /* first QI reference */

    /* Every non-EVENT type is a named refusal with a zero output object. */
    static const unsigned refused[] = {0, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    static const char *const refused_names[] = {
        "unknown", "VCACHE", "RESOURCEMANAGER", "VERTEXSTATS", "OCCLUSION",
        "TIMESTAMP", "TIMESTAMPDISJOINT", "TIMESTAMPFREQ", "PIPELINETIMINGS",
        "INTERFACETIMINGS", "VERTEXTIMINGS", "PIXELTIMINGS", "BANDWIDTHTIMINGS",
        "CACHEUTILIZATION", "MEMORYPRESSURE"
    };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        write32(&demo, OUT, 0xdeadbeefu);
        create_args[0] = refused[i];
        assert(device_call(&demo, 118, create_args, 2) == ERR_UNAVAILABLE);
        assert(read32(&demo, OUT) == 0);
        assert(strstr(d9_com_last_diagnostic(), refused_names[i]) != NULL);
    }

    /* Two consecutive Release calls: the first drops the remaining QI
     * reference, the second destroys the object. */
    assert(call(&demo, query, 2, NULL, 0) == 1);
    assert(call(&demo, query, 2, NULL, 0) == 0);
    assert(read32(&demo, query) == 0);
    assert(refs(&demo, demo.device) == device_refs);

    /* A second query can be destroyed independently of the device. */
    create_args[0] = QUERY_EVENT;
    assert(device_call(&demo, 118, create_args, 2) == 0);
    uint32_t query2 = read32(&demo, OUT);
    assert(query2 && query2 != query);
    assert(call(&demo, query2, 6, issue_end, 1) == 0);
    assert(call(&demo, query2, 2, NULL, 0) == 0);
    assert(read32(&demo, query2) == 0);
    assert(refs(&demo, demo.device) == device_refs);

    assert(d9_memory_used(d) == used);
    d9_com_demo_destroy(&demo);
    assert(d9_destroy_device_ex(d) == 0);
    puts("query smoke: EVENT lifecycle, named refusals, QI and teardown PASS");
    return 0;
}
