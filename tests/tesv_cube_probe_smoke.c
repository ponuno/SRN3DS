#include "tesv_cube_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BASE 0x00c91000u
#define PAGE_SIZE 0x1000u
#define FLAG_ADDR 0x00c9163eu
#define PUSH_ADDR 0x00c9164du
#define CALL_ADDR 0x00c91650u
#define EPILOGUE_ADDR 0x00c916d1u

static const uint8_t flag_expected[3]     = {0xf6, 0xc2, 0x10};
static const uint8_t push_expected[3]     = {0x52, 0x51, 0x50};
static const uint8_t call_expected[5]     = {0xe8, 0xcb, 0x45, 0x32, 0x00};
static const uint8_t epilogue_expected[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};
static const uint8_t patch_expected[10] = {
    0x83, 0xc4, 0x0c, 0x33, 0xf6, 0xe9, 0x77, 0x00, 0x00, 0x00
};

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_cube_probe: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int seed_page(GuestMemory *m) {
    uint8_t page[PAGE_SIZE];
    memset(page, 0xcc, sizeof page);
    memcpy(page + (FLAG_ADDR - PAGE_BASE), flag_expected, sizeof flag_expected);
    memcpy(page + (PUSH_ADDR - PAGE_BASE), push_expected, sizeof push_expected);
    memcpy(page + (CALL_ADDR - PAGE_BASE), call_expected, sizeof call_expected);
    memcpy(page + (EPILOGUE_ADDR - PAGE_BASE), epilogue_expected,
           sizeof epilogue_expected);
    CHECK(!guest_write(m, PAGE_BASE, page, sizeof page));
    return 0;
}

static int expect_unchanged(GuestMemory *m, const uint8_t *before) {
    uint8_t after[PAGE_SIZE];
    CHECK(!guest_read(m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(before, after, sizeof after));
    return 0;
}

int main(void) {
    GuestMemory m;
    guest_init(&m, PAGE_SIZE);
    CHECK(!guest_map(&m, PAGE_BASE, PAGE_SIZE));
    CHECK(!seed_page(&m));

    uint8_t before[PAGE_SIZE], after[PAGE_SIZE];
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));

    /* Disabled: no read, no write, even with null memory. */
    CHECK(!_putenv_s("TESV_D3D9_CUBE_SKIP_PROBE", "0"));
    CHECK(!tesv_cube_skip_active());
    CHECK(!tesv_cube_skip_probe(NULL));
    CHECK(!tesv_cube_skip_active());
    CHECK(!expect_unchanged(&m, before));

    /* Enabled: verify the exact window and change only the ten call bytes. */
    CHECK(!_putenv_s("TESV_D3D9_CUBE_SKIP_PROBE", "1"));
    CHECK(!tesv_cube_skip_probe(&m));
    CHECK(tesv_cube_skip_active());
    CHECK(!guest_read(&m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(after + (FLAG_ADDR - PAGE_BASE), flag_expected,
                  sizeof flag_expected));
    CHECK(!memcmp(after + (PUSH_ADDR - PAGE_BASE), push_expected,
                  sizeof push_expected));
    CHECK(!memcmp(after + (EPILOGUE_ADDR - PAGE_BASE), epilogue_expected,
                  sizeof epilogue_expected));
    CHECK(!memcmp(after + (CALL_ADDR - PAGE_BASE), patch_expected,
                  sizeof patch_expected));
    unsigned changed = 0;
    for (size_t i = 0; i < sizeof before; ++i)
        if (before[i] != after[i]) {
            ++changed;
            CHECK(i >= CALL_ADDR - PAGE_BASE &&
                  i < CALL_ADDR - PAGE_BASE + sizeof patch_expected);
        }
    CHECK(changed == sizeof patch_expected);

    /* A second call sees the patch and must refuse without another write. */
    uint8_t patched[PAGE_SIZE];
    memcpy(patched, after, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong flag byte: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[FLAG_ADDR - PAGE_BASE] ^= 1u;
    CHECK(!guest_write(&m, FLAG_ADDR, before + FLAG_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong push byte: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[PUSH_ADDR - PAGE_BASE] = 0x53;
    CHECK(!guest_write(&m, PUSH_ADDR, before + PUSH_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong call opcode: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[CALL_ADDR - PAGE_BASE] = 0xe9;
    CHECK(!guest_write(&m, CALL_ADDR, before + CALL_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong call target byte: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[CALL_ADDR - PAGE_BASE + 4] ^= 1u;
    CHECK(!guest_write(&m, CALL_ADDR + 4,
                       before + CALL_ADDR - PAGE_BASE + 4, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong epilogue byte: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[EPILOGUE_ADDR - PAGE_BASE] = 0x81;
    CHECK(!guest_write(&m, EPILOGUE_ADDR,
                       before + EPILOGUE_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    guest_destroy(&m);

    /* Unreadable guest memory: named refusal, still no write. */
    GuestMemory unmapped;
    guest_init(&unmapped, PAGE_SIZE);
    CHECK(tesv_cube_skip_probe(&unmapped) == -1);
    CHECK(unmapped.count == 0);
    guest_destroy(&unmapped);

    puts("PASS tesv_cube_probe");
    return 0;
}
