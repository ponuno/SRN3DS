#include "tesv_sm3_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BASE 0x00c6e000u
#define PAGE_SIZE 0x1000u
#define LEVEL_PAGE 0x01ba7000u
#define LEVEL_ADDR 0x01ba749cu
#define GATE_ADDR 0x00c6e22du
#define JGE_ADDR  0x00c6e23bu

static const uint8_t gate_expected[7] = {0x83, 0x3d, 0x9c, 0x74, 0xba, 0x01, 0x07};
static const uint8_t jge_expected[2] = {0x7d, 0x0a};

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_sm3_probe: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int seed_page(GuestMemory *m) {
    uint8_t page[PAGE_SIZE];
    memset(page, 0xcc, sizeof page);
    memcpy(page + (GATE_ADDR - PAGE_BASE), gate_expected, sizeof gate_expected);
    memcpy(page + (JGE_ADDR - PAGE_BASE), jge_expected, sizeof jge_expected);
    CHECK(!guest_write(m, PAGE_BASE, page, sizeof page));
    return 0;
}

static __attribute__((noinline)) void *probe_write_site(void) {
    return tesv_sm3_probe_level_ptr(LEVEL_ADDR, 4);
}

static __attribute__((noinline)) void *probe_read_site(void) {
    return tesv_sm3_probe_level_ptr(LEVEL_ADDR, 4);
}

int main(void) {
    GuestMemory m;
    guest_init(&m, PAGE_SIZE * 2u);
    CHECK(!guest_map(&m, PAGE_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, LEVEL_PAGE, PAGE_SIZE));
    CHECK(!seed_page(&m));
    CHECK(!guest_write32(&m, LEVEL_ADDR, 2));

    /* Disabled means no read and no write, even with a null memory object. */
    CHECK(!_putenv_s("TESV_D3D9_SM3_GATE_PROBE", "0"));
    CHECK(!tesv_sm3_probe_level_ptr(LEVEL_ADDR, 4));
    CHECK(!tesv_sm3_gate_probe(NULL));
    uint8_t before[PAGE_SIZE], after[PAGE_SIZE];
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    CHECK(!tesv_sm3_gate_probe(&m));
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(before, after, sizeof before));

    /* Enabled: verify both ranges and change exactly the JGE opcode byte. */
    CHECK(!_putenv_s("TESV_D3D9_SM3_GATE_PROBE", "1"));
    CHECK(!tesv_sm3_gate_probe(&m));
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(after + (GATE_ADDR - PAGE_BASE), gate_expected, sizeof gate_expected));
    CHECK(after[JGE_ADDR - PAGE_BASE] == 0xeb);
    CHECK(after[JGE_ADDR - PAGE_BASE + 1] == 0x0a);
    unsigned changed = 0;
    for (size_t i = 0; i < sizeof before; ++i)
        if (before[i] != after[i]) {
            ++changed;
            CHECK(i == JGE_ADDR - PAGE_BASE);
        }
    CHECK(changed == 1);

    /* Predecoded AOT/IR sends c71080's store to a sink and serves the compare's
     * load from an immutable shadow.  The guest data word itself remains 2. */
    uint32_t *sink = probe_read_site();
    CHECK(sink && *sink == 0);
    CHECK(probe_write_site() == sink);
    *sink = 2;
    uint32_t *shadow = probe_read_site();
    CHECK(shadow && *shadow == 7);
    uint32_t guest_level = 0;
    CHECK(!guest_read32(&m, LEVEL_ADDR, &guest_level));
    CHECK(guest_level == 2);

    /* A second call sees EB 0A, refuses against the required 7D 0A, and must
     * leave the already-patched page untouched. */
    uint8_t patched[PAGE_SIZE];
    memcpy(patched, after, sizeof patched);
    CHECK(tesv_sm3_gate_probe(&m) == -1);
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(after, patched, sizeof after));

    /* Wrong cmp byte: refuse without writing anything. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    after[GATE_ADDR - PAGE_BASE + 3] ^= 1;
    CHECK(!guest_write(&m, GATE_ADDR + 3, after + GATE_ADDR - PAGE_BASE + 3, 1));
    memcpy(patched, after, sizeof patched);
    CHECK(tesv_sm3_gate_probe(&m) == -1);
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(after, patched, sizeof after));

    /* Wrong branch byte: refuse without writing anything. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    after[JGE_ADDR - PAGE_BASE] = 0x75;
    CHECK(!guest_write(&m, JGE_ADDR, after + JGE_ADDR - PAGE_BASE, 1));
    memcpy(patched, after, sizeof patched);
    CHECK(tesv_sm3_gate_probe(&m) == -1);
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(after, patched, sizeof after));

    guest_destroy(&m);

    /* Unreadable guest memory: named refusal, still no write. */
    GuestMemory unmapped;
    guest_init(&unmapped, PAGE_SIZE);
    CHECK(tesv_sm3_gate_probe(&unmapped) == -1);
    CHECK(unmapped.count == 0);
    guest_destroy(&unmapped);

    puts("PASS tesv_sm3_probe");
    return 0;
}
