#include "tesv_cecdc0_source.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE 0x00ced000u
#define CODE_SIZE 0x1000u
#define BLOCK_ADDR 0x00ced041u
#define BLOCK_LEN 0x15u
#define FRAME_BASE 0x60000000u
#define SOURCE_BASE 0x60001000u
#define ARG_BASE 0x60002000u
#define BASE_BASE 0x60003000u
#define PAGE_SIZE 0x1000u
#define INDEX 2u

static const uint8_t block_expected[BLOCK_LEN] = {
    0x8b, 0x54, 0x24, 0x54,
    0x8b, 0x8a, 0x24, 0x06, 0x00, 0x00,
    0x8b, 0x54, 0x24, 0x18,
    0x8b, 0x0c, 0x8a,
    0x89, 0x4c, 0x24, 0x14
};

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_cecdc0_source: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int seed_code(GuestMemory *m) {
    uint8_t page[CODE_SIZE];
    memset(page, 0xcc, sizeof page);
    memcpy(page + (BLOCK_ADDR - CODE_BASE), block_expected,
           sizeof block_expected);
    CHECK(!guest_write(m, CODE_BASE, page, sizeof page));
    return 0;
}

static int expect_code(GuestMemory *m, const uint8_t *before) {
    uint8_t after[CODE_SIZE];
    CHECK(!guest_read(m, CODE_BASE, after, sizeof after));
    CHECK(!memcmp(before, after, sizeof after));
    return 0;
}

static int seed_source(GuestMemory *m) {
    CHECK(!guest_write32(m, FRAME_BASE + 0x54u, ARG_BASE));
    CHECK(!guest_write32(m, FRAME_BASE + 0x18u, 0u));
    CHECK(!guest_write32(m, FRAME_BASE + 0x10u, 0u));
    CHECK(!guest_write32(m, FRAME_BASE + 0x24u, 0u));
    CHECK(!guest_write32(m, ARG_BASE + 0x604u, SOURCE_BASE));
    CHECK(!guest_write32(m, SOURCE_BASE + 0x10u, INDEX));
    CHECK(!guest_write32(m, SOURCE_BASE + INDEX * 4u, 0x11223344u));
    CHECK(!guest_write32(m, BASE_BASE + INDEX * 4u, 0u));
    return 0;
}

int main(void) {
    GuestMemory m;
    guest_init(&m, CODE_SIZE + PAGE_SIZE * 4u);
    CHECK(!guest_map(&m, CODE_BASE, CODE_SIZE));
    CHECK(!guest_map(&m, FRAME_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, SOURCE_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, ARG_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, BASE_BASE, PAGE_SIZE));
    CHECK(!seed_code(&m));
    CHECK(!seed_source(&m));

    uint8_t before[CODE_SIZE], after[CODE_SIZE];
    CHECK(!guest_read(&m, CODE_BASE, before, sizeof before));

    /* Disabled: no read, no active latch. */
    CHECK(!_putenv_s("TESV_CECDC0_SOURCE_FILL", "0"));
    CHECK(!tesv_cecdc0_source_active());
    CHECK(!tesv_cecdc0_source_probe(&m));
    CHECK(!tesv_cecdc0_source_active());
    CHECK(!tesv_cecdc0_source_probe(NULL)); /* env off wins over NULL memory */
    CHECK(!expect_code(&m, before));

    /* Enabled: exact block accepted, code bytes untouched. */
    CHECK(!_putenv_s("TESV_CECDC0_SOURCE_FILL", "1"));
    CHECK(!tesv_cecdc0_source_probe(&m));
    CHECK(tesv_cecdc0_source_active());
    CHECK(!expect_code(&m, before));

    uint32_t new_base = 0, new_desc = 0;

    /* NULL base local + valid source/index -> fill with the source object. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == 1);
    CHECK(new_base == SOURCE_BASE);

    /* Non-NULL base with a zero selected slot -> fill too. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, BASE_BASE));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == 1);
    CHECK(new_base == SOURCE_BASE);

    /* Already populated slot -> no-op. */
    CHECK(!guest_write32(&m, BASE_BASE + INDEX * 4u, 0x55667788u));
    CHECK(!tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base));

    /* Source index mismatch -> no fill. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    CHECK(!guest_write32(&m, SOURCE_BASE + 0x10u, INDEX + 1u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);
    CHECK(!guest_write32(&m, SOURCE_BASE + 0x10u, INDEX));

    /* Source descriptor slot is NULL -> no fill (no fabricated descriptor). */
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0x11223344u));

    /* Source slot NULL; the real [esp+0x10]+0x14 buffer is the alternate. */
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0u));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0u));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x10u, BASE_BASE));
    CHECK(!guest_write32(&m, BASE_BASE + 0x14u, 0x0badf00du));
    new_base = 0;
    new_desc = 0;
    CHECK(tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, 0u,
                                    &new_base, &new_desc) == 1);
    CHECK(new_desc == BASE_BASE + 0x14u);
    CHECK(!guest_write32(&m, FRAME_BASE + 0x10u, 0u));
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0x11223344u));

    /* Out-of-range index -> no fill. */
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, 0x16u, &new_base) == -1);

    /* No source object -> no fill. */
    CHECK(!guest_write32(&m, ARG_BASE + 0x604u, 0u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);
    CHECK(!guest_write32(&m, ARG_BASE + 0x604u, SOURCE_BASE));

    /* Unreadable base array / source object -> no fill. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0x70000000u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    CHECK(!guest_write32(&m, ARG_BASE + 0x604u, 0x70000000u));
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);
    CHECK(!guest_write32(&m, ARG_BASE + 0x604u, SOURCE_BASE));

    /* decide(): valid source slot -> fill and run the original lookup. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0xaaaabbbbu));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    new_base = 0;
    new_desc = 0;
    CHECK(!tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, 0u,
                                     &new_base, &new_desc));
    CHECK(new_base == SOURCE_BASE);

    /* Source slot NULL + ebx+0x14 is readable -> skip the overwrite. */
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0u));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0u));
    CHECK(!guest_write32(&m, BASE_BASE + 0x14u, 0x0badf00du));
    new_base = 0;
    new_desc = 0;
    CHECK(tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, BASE_BASE,
                                    &new_base, &new_desc) == 1);
    CHECK(new_desc == BASE_BASE + 0x14u);

    /* Source slot NULL, ebx=0, existing f_CE9E00 descriptor -> keep it. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0x12345678u));
    new_base = 0;
    new_desc = 0;
    CHECK(tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, 0u,
                                    &new_base, &new_desc) == 1);
    CHECK(new_desc == 0x12345678u);

    /* Source slot NULL, ebx=0, no existing descriptor -> no safe change. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0u));
    CHECK(tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, 0u,
                                    &new_base, &new_desc) == -1);

    /* Already populated base slot -> original lookup, no fill. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, BASE_BASE));
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0x11223344u));
    new_base = 0;
    new_desc = 0;
    CHECK(!tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, 0u,
                                     &new_base, &new_desc));
    CHECK(new_base == 0);

    /* Invalid source index + ebx buffer -> skip the overwrite. */
    CHECK(!guest_write32(&m, FRAME_BASE + 0x18u, 0u));
    CHECK(!guest_write32(&m, FRAME_BASE + 0x14u, 0u));
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0u));
    CHECK(!guest_write32(&m, SOURCE_BASE + 0x10u, INDEX + 1u));
    new_base = 0;
    new_desc = 0;
    CHECK(tesv_cecdc0_source_decide(&m, FRAME_BASE, INDEX, BASE_BASE,
                                    &new_base, &new_desc) == 1);
    CHECK(new_desc == BASE_BASE + 0x14u);
    CHECK(!guest_write32(&m, SOURCE_BASE + 0x10u, INDEX));
    CHECK(!guest_write32(&m, SOURCE_BASE + INDEX * 4u, 0x11223344u));

    /* Repeat probe call refuses without changing code. */
    CHECK(!guest_read(&m, CODE_BASE, after, sizeof after));
    CHECK(tesv_cecdc0_source_probe(&m) == -1);
    CHECK(!expect_code(&m, after));

    /* Wrong block byte: refuse and stay inactive. */
    tesv_cecdc0_source_reset_for_test();
    CHECK(!seed_code(&m));
    CHECK(!guest_read(&m, CODE_BASE, before, sizeof before));
    before[BLOCK_ADDR - CODE_BASE + 1] = 0x92; /* mov ecx -> mov edx */
    CHECK(!guest_write(&m, BLOCK_ADDR + 1,
                       before + BLOCK_ADDR - CODE_BASE + 1, 1));
    memcpy(after, before, sizeof after);
    CHECK(tesv_cecdc0_source_probe(&m) == -1);
    CHECK(!tesv_cecdc0_source_active());
    CHECK(!expect_code(&m, after));

    /* Unreadable block: refuse. */
    tesv_cecdc0_source_reset_for_test();
    GuestMemory unmapped;
    guest_init(&unmapped, PAGE_SIZE);
    CHECK(tesv_cecdc0_source_probe(&unmapped) == -1);
    CHECK(unmapped.count == 0);
    guest_destroy(&unmapped);

    /* The fill helper is inert while the probe is off. */
    CHECK(tesv_cecdc0_source_fill_slot(&m, FRAME_BASE, INDEX, &new_base) == -1);

    guest_destroy(&m);
    puts("PASS tesv_cecdc0_source");
    return 0;
}
