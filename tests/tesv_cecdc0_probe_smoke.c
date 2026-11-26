#include "tesv_cecdc0_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_BASE 0x00cd4000u
#define TEXT_SIZE 0x2000u
#define SITE2_BASE 0x00ce7000u
#define SITE2_SIZE 0x1000u
#define SITE1_CALL 0x00cd4e63u
#define SITE2_CALL 0x00ce77f8u
#define SITE1_WINDOW 0x26u
#define SITE2_WINDOW 0x0du

static const uint8_t site1_expected[SITE1_WINDOW] = {
    0xe8, 0x58, 0x7f, 0x01, 0x00,
    0x83, 0xc4, 0x08,
    0x83, 0x46, 0x04, 0xff,
    0x8b, 0xf8,
    0x75, 0x0c,
    0x8b, 0x06,
    0x50, 0x6a, 0x00, 0x56,
    0xff, 0x15, 0x4c, 0xb1, 0x06, 0x01,
    0x85, 0xff,
    0x5f,
    0x0f, 0x95, 0xc0,
    0x5e,
    0xc2, 0x04, 0x00
};

static const uint8_t site2_expected[SITE2_WINDOW] = {
    0xe8, 0xc3, 0x55, 0x00, 0x00,
    0x83, 0xc4, 0x08,
    0x5e,
    0x83, 0xc4, 0x48,
    0xc3
};

static const uint8_t site1_patch[5] = {0x33, 0xc0, 0x90, 0x90, 0x90};

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_cecdc0_probe: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int seed(GuestMemory *m) {
    uint8_t page[TEXT_SIZE];
    memset(page, 0xcc, sizeof page);
    memcpy(page + (SITE1_CALL - TEXT_BASE), site1_expected,
           sizeof site1_expected);
    CHECK(!guest_write(m, TEXT_BASE, page, sizeof page));
    uint8_t page2[SITE2_SIZE];
    memset(page2, 0xcc, sizeof page2);
    memcpy(page2 + (SITE2_CALL - SITE2_BASE), site2_expected,
           sizeof site2_expected);
    CHECK(!guest_write(m, SITE2_BASE, page2, sizeof page2));
    return 0;
}

static int expect_unchanged(GuestMemory *m, const uint8_t *before) {
    uint8_t after[TEXT_SIZE];
    CHECK(!guest_read(m, TEXT_BASE, after, sizeof after));
    CHECK(!memcmp(before, after, sizeof after));
    return 0;
}

int main(void) {
    GuestMemory m;
    guest_init(&m, TEXT_SIZE + SITE2_SIZE);
    CHECK(!guest_map(&m, TEXT_BASE, TEXT_SIZE));
    CHECK(!guest_map(&m, SITE2_BASE, SITE2_SIZE));
    CHECK(!seed(&m));

    uint8_t before[TEXT_SIZE], after[TEXT_SIZE];
    CHECK(!guest_read(&m, TEXT_BASE, before, sizeof before));

    /* Disabled: no write and no active latch, even with null memory. */
    CHECK(!_putenv_s("TESV_CECDC0_SKIP", "0"));
    CHECK(!tesv_cecdc0_probe_active());
    CHECK(!tesv_cecdc0_skip_probe(NULL));
    CHECK(!tesv_cecdc0_probe_active());
    CHECK(!expect_unchanged(&m, before));

    /* Enabled: only the five call bytes change; the second caller is not
     * touched, and only the f_CD4E40 return address is recognised. */
    CHECK(!_putenv_s("TESV_CECDC0_SKIP", "1"));
    CHECK(!tesv_cecdc0_skip_probe(&m));
    CHECK(tesv_cecdc0_probe_active());
    CHECK(!guest_read(&m, TEXT_BASE, after, sizeof after));
    CHECK(!memcmp(after + (SITE1_CALL - TEXT_BASE), site1_patch,
                  sizeof site1_patch));
    unsigned changed = 0;
    for (size_t i = 0; i < sizeof before; ++i)
        if (before[i] != after[i]) {
            ++changed;
            CHECK(i >= SITE1_CALL - TEXT_BASE &&
                  i < SITE1_CALL - TEXT_BASE + sizeof site1_patch);
        }
    CHECK(changed == sizeof site1_patch);
    uint8_t site2[SITE2_WINDOW];
    CHECK(!guest_read(&m, SITE2_CALL, site2, sizeof site2));
    CHECK(!memcmp(site2, site2_expected, sizeof site2));
    CHECK(tesv_cecdc0_is_primary_return(SITE1_CALL + 5u));
    CHECK(!tesv_cecdc0_is_primary_return(SITE2_CALL + 5u));

    /* A second call refuses without another write. */
    uint8_t patched[TEXT_SIZE];
    memcpy(patched, after, sizeof patched);
    CHECK(tesv_cecdc0_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong call opcode: refuse without writing. */
    tesv_cecdc0_probe_reset_for_test();
    CHECK(!seed(&m));
    CHECK(!guest_read(&m, TEXT_BASE, before, sizeof before));
    before[SITE1_CALL - TEXT_BASE] = 0xe9;
    CHECK(!guest_write(&m, SITE1_CALL, before + SITE1_CALL - TEXT_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cecdc0_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong call target: refuse without writing. */
    tesv_cecdc0_probe_reset_for_test();
    CHECK(!seed(&m));
    CHECK(!guest_read(&m, TEXT_BASE, before, sizeof before));
    before[SITE1_CALL - TEXT_BASE + 4] ^= 1u;
    CHECK(!guest_write(&m, SITE1_CALL + 4,
                       before + SITE1_CALL - TEXT_BASE + 4, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cecdc0_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong continuation (`add esp,8`): refuse without writing. */
    tesv_cecdc0_probe_reset_for_test();
    CHECK(!seed(&m));
    CHECK(!guest_read(&m, TEXT_BASE, before, sizeof before));
    before[SITE1_CALL - TEXT_BASE + 5] = 0x81;
    CHECK(!guest_write(&m, SITE1_CALL + 5,
                       before + SITE1_CALL - TEXT_BASE + 5, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cecdc0_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong EAX test byte: refuse without writing. */
    tesv_cecdc0_probe_reset_for_test();
    CHECK(!seed(&m));
    CHECK(!guest_read(&m, TEXT_BASE, before, sizeof before));
    before[SITE1_CALL - TEXT_BASE + 0x1c] = 0x84;
    CHECK(!guest_write(&m, SITE1_CALL + 0x1c,
                       before + SITE1_CALL - TEXT_BASE + 0x1c, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cecdc0_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Unreadable guest memory: named refusal, still no write. */
    tesv_cecdc0_probe_reset_for_test();
    GuestMemory unmapped;
    guest_init(&unmapped, 0x1000);
    CHECK(tesv_cecdc0_skip_probe(&unmapped) == -1);
    CHECK(unmapped.count == 0);
    guest_destroy(&unmapped);

    guest_destroy(&m);
    puts("PASS tesv_cecdc0_probe");
    return 0;
}
