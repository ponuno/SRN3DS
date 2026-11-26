/* IDirect3DResource9 private-data contract.
 *
 * D3D9 shaders do not inherit IDirect3DResource9 and have no private-data
 * methods.  The shader is therefore exercised as a D3DSPD_IUNKNOWN payload on
 * a texture, which verifies the same COM retain/release ownership path. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF   0x10010800u
#define OUT   (BUF + 0x000u)
#define DATA  (BUF + 0x100u)
#define GET   (BUF + 0x200u)
#define SIZE  (BUF + 0x300u)
#define GUID1 (BUF + 0x400u)
#define GUID2 (BUF + 0x500u)
#define LIMIT 0x10020000u
#define PRIVATE_MAX (64u * 1024u)
#define PRIVATE_ENTRIES 16u
#define D3DSPD_IUNKNOWN 1u
#define ERR_INVALID 0x8876086cu
#define ERR_NOTFOUND 0x88760866u
#define ERR_MOREDATA 0x88760867u

static const uint8_t guid1[16] = {
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xf0,0x01
};
static const uint8_t guid2[16] = {
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f
};

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "private-data: object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
        abort();
    }
    return result;
}
static uint32_t device_call(D9ComDemo *s, unsigned slot,
                            const uint32_t *args, unsigned count) {
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
static void write_bytes(D9ComDemo *s, uint32_t address,
                        const void *data, size_t bytes) {
    assert(!guest_write(&s->memory, address, data, bytes));
}
static void read_bytes(D9ComDemo *s, uint32_t address,
                       void *data, size_t bytes) {
    assert(!guest_read(&s->memory, address, data, bytes));
}
static uint32_t refs(D9ComDemo *s, uint32_t object) {
    uint32_t added = call(s, object, 1, NULL, 0);
    uint32_t remaining = call(s, object, 2, NULL, 0);
    assert(remaining == added - 1);
    return remaining;
}
static uint32_t set_data(D9ComDemo *s, uint32_t object, uint32_t guid,
                         uint32_t data, uint32_t size, uint32_t flags) {
    uint32_t args[] = {guid, data, size, flags};
    return call(s, object, 4, args, 4);
}
static uint32_t get_data(D9ComDemo *s, uint32_t object, uint32_t guid,
                         uint32_t data, uint32_t size_address) {
    uint32_t args[] = {guid, data, size_address};
    return call(s, object, 5, args, 3);
}
static uint32_t free_data(D9ComDemo *s, uint32_t object, uint32_t guid) {
    uint32_t args[] = {guid};
    return call(s, object, 6, args, 1);
}
static void expect_diagnostic(const char *needle) {
    const char *diagnostic = d9_com_last_diagnostic();
    if (!strstr(diagnostic, needle)) {
        fprintf(stderr, "private-data: missing diagnostic '%s' in '%s'\n",
                needle, diagnostic);
        abort();
    }
}

int main(void) {
    D9Device *core = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(core);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, core)) {
        fprintf(stderr, "private-data: demo init failed: %s\n", demo.message);
        return 1;
    }
    assert(!d9_com_demo_enable_shaders(&demo));
    write_bytes(&demo, GUID1, guid1, sizeof guid1);
    write_bytes(&demo, GUID2, guid2, sizeof guid2);

    uint32_t texture_refs = refs(&demo, demo.texture);
    uint32_t shader_refs = refs(&demo, demo.vs);
    const uint32_t original[2] = {0x89abcdefu, 0x01234567u};

    /* Set copies bytes; later guest writes do not mutate stored data. */
    write_bytes(&demo, DATA, original, sizeof original);
    assert(set_data(&demo, demo.texture, GUID1, DATA, sizeof original, 0) == 0);
    write32(&demo, DATA, 0xdeadbeefu);
    write32(&demo, SIZE, sizeof original);
    assert(get_data(&demo, demo.texture, GUID1, GET, SIZE) == 0);
    assert(read32(&demo, SIZE) == sizeof original);
    uint32_t got[2];
    read_bytes(&demo, GET, got, sizeof got);
    assert(!memcmp(got, original, sizeof got));
    assert(refs(&demo, demo.texture) == texture_refs);

    /* MOREDATA reports the exact required size and leaves the buffer alone. */
    memset(got, 0xcd, sizeof got);
    write_bytes(&demo, GET, got, sizeof got);
    write32(&demo, SIZE, sizeof original - 1);
    assert(get_data(&demo, demo.texture, GUID1, GET, SIZE) == ERR_MOREDATA);
    assert(read32(&demo, SIZE) == sizeof original);
    read_bytes(&demo, GET, got, sizeof got);
    assert(got[0] == 0xcdcdcdcdu && got[1] == 0xcdcdcdcdu);
    write32(&demo, SIZE, 0);
    assert(get_data(&demo, demo.texture, GUID1, 0, SIZE) == ERR_MOREDATA);
    assert(read32(&demo, SIZE) == sizeof original);

    /* Missing GUID is NOTFOUND; Free remains idempotent. */
    write32(&demo, SIZE, 0x12345678u);
    assert(get_data(&demo, demo.texture, GUID2, GET, SIZE) == ERR_NOTFOUND);
    assert(read32(&demo, SIZE) == 0x12345678u);
    expect_diagnostic("not found");
    assert(free_data(&demo, demo.texture, GUID2) == 0);
    assert(free_data(&demo, demo.texture, GUID2) == 0);

    /* Overwrite replaces the old bytes; Free removes the entry. */
    const uint32_t replacement = 0xa5a5a5a5u;
    write32(&demo, DATA, replacement);
    assert(set_data(&demo, demo.texture, GUID1, DATA, sizeof replacement, 0) == 0);
    write32(&demo, SIZE, sizeof replacement);
    assert(get_data(&demo, demo.texture, GUID1, GET, SIZE) == 0);
    assert(read32(&demo, GET) == replacement && read32(&demo, SIZE) == sizeof replacement);
    assert(free_data(&demo, demo.texture, GUID1) == 0);
    assert(free_data(&demo, demo.texture, GUID1) == 0);
    assert(get_data(&demo, demo.texture, GUID1, GET, SIZE) == ERR_NOTFOUND);

    /* IUNKNOWN stores a COM pointer and owns one reference until overwrite,
     * Free, or resource destruction.  Get does not add another reference. */
    write32(&demo, DATA, demo.vs);
    assert(set_data(&demo, demo.texture, GUID2, DATA, 4, D3DSPD_IUNKNOWN) == 0);
    assert(refs(&demo, demo.vs) == shader_refs + 1);
    write32(&demo, SIZE, 4);
    assert(get_data(&demo, demo.texture, GUID2, GET, SIZE) == 0);
    assert(read32(&demo, GET) == demo.vs && read32(&demo, SIZE) == 4);
    assert(refs(&demo, demo.vs) == shader_refs + 1);
    write32(&demo, DATA, 0x13579bdfu);
    assert(set_data(&demo, demo.texture, GUID2, DATA, 4, 0) == 0);
    assert(refs(&demo, demo.vs) == shader_refs);
    assert(free_data(&demo, demo.texture, GUID2) == 0);

    /* Resource teardown releases an outstanding IUNKNOWN payload. */
    uint32_t create[] = {8, 8, 1, 0, D9_A8R8G8B8, 0, OUT, 0};
    assert(device_call(&demo, 23, create, 8) == 0);
    uint32_t temporary = read32(&demo, OUT);
    assert(temporary);
    write32(&demo, DATA, demo.vs);
    assert(set_data(&demo, temporary, GUID2, DATA, 4, D3DSPD_IUNKNOWN) == 0);
    assert(refs(&demo, demo.vs) == shader_refs + 1);
    assert(call(&demo, temporary, 2, NULL, 0) == 0);
    assert(refs(&demo, demo.vs) == shader_refs);

    /* Documented bounds: 64 KiB per write and 16 GUID entries per object. */
    assert(!guest_map(&demo.memory, LIMIT, PRIVATE_MAX + 16));
    memset(guest_ptr(&demo.memory, LIMIT, PRIVATE_MAX), 0x5a, PRIVATE_MAX);
    assert(set_data(&demo, demo.texture, GUID1, LIMIT, PRIVATE_MAX, 0) == 0);
    write32(&demo, SIZE, PRIVATE_MAX);
    assert(get_data(&demo, demo.texture, GUID1, LIMIT, SIZE) == 0);
    uint8_t *limit_bytes = guest_ptr(&demo.memory, LIMIT, PRIVATE_MAX);
    assert(limit_bytes && limit_bytes[0] == 0x5a && limit_bytes[PRIVATE_MAX - 1] == 0x5a);
    assert(free_data(&demo, demo.texture, GUID1) == 0);
    assert(set_data(&demo, demo.texture, GUID1, LIMIT, PRIVATE_MAX + 1, 0) == ERR_INVALID);
    expect_diagnostic("64 KiB");
    assert(get_data(&demo, demo.texture, GUID1, GET, SIZE) == ERR_NOTFOUND);

    for (unsigned i = 0; i < PRIVATE_ENTRIES; ++i) {
        uint8_t guid[16] = {0};
        guid[0] = 0x40u + (uint8_t)i;
        write_bytes(&demo, GUID1, guid, sizeof guid);
        write32(&demo, DATA, 0x100u + i);
        assert(set_data(&demo, demo.texture, GUID1, DATA, 4, 0) == 0);
    }
    uint8_t extra_guid[16] = {0};
    extra_guid[0] = 0x50;
    write_bytes(&demo, GUID1, extra_guid, sizeof extra_guid);
    write32(&demo, DATA, 0x200);
    assert(set_data(&demo, demo.texture, GUID1, DATA, 4, 0) == ERR_INVALID);
    expect_diagnostic("entry limit (16)");
    for (unsigned i = 0; i < PRIVATE_ENTRIES; ++i) {
        uint8_t guid[16] = {0};
        guid[0] = 0x40u + (uint8_t)i;
        write_bytes(&demo, GUID1, guid, sizeof guid);
        assert(free_data(&demo, demo.texture, GUID1) == 0);
    }
    write_bytes(&demo, GUID1, extra_guid, sizeof extra_guid);
    assert(set_data(&demo, demo.texture, GUID1, DATA, 4, 0) == 0);
    assert(free_data(&demo, demo.texture, GUID1) == 0);

    /* Zero-length entries, invalid flags and malformed IUNKNOWN are named. */
    write_bytes(&demo, GUID1, guid1, sizeof guid1);
    assert(set_data(&demo, demo.texture, GUID1, 0, 0, 0) == 0);
    write32(&demo, SIZE, 0);
    assert(get_data(&demo, demo.texture, GUID1, 0, SIZE) == 0);
    assert(read32(&demo, SIZE) == 0);
    assert(free_data(&demo, demo.texture, GUID1) == 0);
    assert(set_data(&demo, demo.texture, GUID1, DATA, 4, 2) == ERR_INVALID);
    expect_diagnostic("D3DSPD_IUNKNOWN");
    write32(&demo, DATA, demo.vs);
    assert(set_data(&demo, demo.texture, GUID1, DATA, 8, D3DSPD_IUNKNOWN) == ERR_INVALID);
    expect_diagnostic("one 32-bit COM pointer");
    write32(&demo, DATA, 0);
    assert(set_data(&demo, demo.texture, GUID1, DATA, 4, D3DSPD_IUNKNOWN) == ERR_INVALID);
    expect_diagnostic("not a live COM object");

    assert(refs(&demo, demo.texture) == texture_refs);
    assert(refs(&demo, demo.vs) == shader_refs);
    assert(!d9_com_demo_destroy(&demo));
    assert(d9_destroy_device_ex(core) == 0);
    puts("private data: resource roundtrip, MOREDATA/NOTFOUND, overwrite/free, IUNKNOWN ownership, limits and teardown PASS");
    return 0;
}
