#include "tesv_cube_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BASE 0x00c91000u
#define CODE_SIZE 0x2000u
#define GROUP_BASE 0x55100000u
#define BUFFER_BASE 0x55101000u
#define STACK_BASE 0x55102000u
#define VTABLE_BASE 0x011b0000u
#define PAGE_SIZE 0x1000u
#define VTABLE_ADDR 0x011b0224u
#define VTABLE2_ADDR 0x011b0220u
#define FLAG_ADDR 0x00c9163eu
#define PUSH_ADDR 0x00c9164du
#define CALL_ADDR 0x00c91650u
#define EPILOGUE_ADDR 0x00c916d1u
#define STORE_ADDR 0x00c92698u
#define INC_ADDR 0x00c926a6u

static const uint8_t flag_expected[3]     = {0xf6, 0xc2, 0x10};
static const uint8_t push_expected[3]     = {0x52, 0x51, 0x50};
static const uint8_t call_expected[5]     = {0xe8, 0xcb, 0x45, 0x32, 0x00};
static const uint8_t epilogue_expected[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};
static const uint8_t store_expected[4]    = {0x89, 0x44, 0xb4, 0x40};
static const uint8_t inc_expected[1]      = {0x46};
static const uint8_t patch_expected[10] = {
    0x83, 0xc4, 0x0c, 0x33, 0xf6, 0xe9, 0x77, 0x00, 0x00, 0x00
};
static const uint8_t store_patch[4] = {0x90, 0x90, 0x90, 0x90};
static const uint8_t inc_patch[1]   = {0x90};
static const uint8_t vtable_expected[4] = {0x90, 0x60, 0xfb, 0x00};
static const uint8_t vtable2_expected[4] = {0x70, 0x60, 0xfb, 0x00};
static const uint8_t vtable_patch[4]    = {0x00, 0x00, 0x00, 0x30};
static const uint8_t vtable2_patch[4]   = {0x10, 0x00, 0x00, 0x30};

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "tesv_cube_probe: FAIL line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int seed_page(GuestMemory *m) {
    uint8_t page[CODE_SIZE];
    memset(page, 0xcc, sizeof page);
    memcpy(page + (FLAG_ADDR - PAGE_BASE), flag_expected, sizeof flag_expected);
    memcpy(page + (PUSH_ADDR - PAGE_BASE), push_expected, sizeof push_expected);
    memcpy(page + (CALL_ADDR - PAGE_BASE), call_expected, sizeof call_expected);
    memcpy(page + (EPILOGUE_ADDR - PAGE_BASE), epilogue_expected,
           sizeof epilogue_expected);
    memcpy(page + (STORE_ADDR - PAGE_BASE), store_expected, sizeof store_expected);
    memcpy(page + (INC_ADDR - PAGE_BASE), inc_expected, sizeof inc_expected);
    CHECK(!guest_write(m, PAGE_BASE, page, sizeof page));
    return 0;
}

static int seed_extra(GuestMemory *m) {
    CHECK(!guest_write(m, VTABLE_ADDR, vtable_expected, sizeof vtable_expected));
    CHECK(!guest_write(m, VTABLE2_ADDR, vtable2_expected,
                       sizeof vtable2_expected));
    return 0;
}

static int expect_unchanged(GuestMemory *m, const uint8_t *before) {
    uint8_t after[CODE_SIZE];
    CHECK(!guest_read(m, PAGE_BASE, after, sizeof after));
    CHECK(!memcmp(before, after, sizeof after));
    return 0;
}

int main(void) {
    GuestMemory m;
    guest_init(&m, CODE_SIZE + PAGE_SIZE * 5u);
    CHECK(!guest_map(&m, PAGE_BASE, CODE_SIZE));
    CHECK(!guest_map(&m, GROUP_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, BUFFER_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, STACK_BASE, PAGE_SIZE));
    CHECK(!guest_map(&m, VTABLE_BASE, PAGE_SIZE));
    CHECK(!seed_page(&m));
    CHECK(!seed_extra(&m));

    uint8_t before[CODE_SIZE], after[CODE_SIZE];
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));

    /* Disabled: no read, no write, even with null memory. */
    CHECK(!_putenv_s("TESV_D3D9_CUBE_SKIP_PROBE", "0"));
    CHECK(!tesv_cube_skip_active());
    CHECK(!tesv_cube_skip_probe(NULL));
    CHECK(!tesv_cube_skip_active());
    CHECK(!tesv_cube_probe_ptr(GROUP_BASE + 0x0cu, 4));
    CHECK(!expect_unchanged(&m, before));

    /* Enabled: verify the exact window and change only the expected bytes. */
    CHECK(!_putenv_s("TESV_CUBE_PROBE_VTABLE", "1"));
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
    CHECK(!memcmp(after + (STORE_ADDR - PAGE_BASE), store_patch,
                  sizeof store_patch));
    CHECK(!memcmp(after + (INC_ADDR - PAGE_BASE), inc_patch, sizeof inc_patch));
    unsigned changed = 0;
    for (size_t i = 0; i < sizeof before; ++i)
        if (before[i] != after[i]) {
            ++changed;
            CHECK((i >= CALL_ADDR - PAGE_BASE &&
                   i < CALL_ADDR - PAGE_BASE + sizeof patch_expected) ||
                  (i >= STORE_ADDR - PAGE_BASE &&
                   i < STORE_ADDR - PAGE_BASE + sizeof store_patch) ||
                  i == INC_ADDR - PAGE_BASE);
        }
    CHECK(changed == sizeof patch_expected + sizeof store_patch +
                      sizeof inc_patch);
    uint8_t vtable[4], vtable2[4];
    CHECK(!guest_read(&m, VTABLE_ADDR, vtable, sizeof vtable));
    CHECK(!memcmp(vtable, vtable_patch, sizeof vtable));
    CHECK(!guest_read(&m, VTABLE2_ADDR, vtable2, sizeof vtable2));
    CHECK(!memcmp(vtable2, vtable2_patch, sizeof vtable2));

    /* Group sentinel shadow: GROUP_BASE carries the NiRenderTargetGroup
     * vtable, and group+0xc is -1.  The probe walks back to the vtable,
     * rewrites only that field to a checked NULL and returns a zero shadow. */
    CHECK(!guest_write32(&m, GROUP_BASE, 0x011b019cu));
    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, 0xffffffffu));
    uint32_t *zero = (uint32_t *)tesv_cube_probe_ptr(GROUP_BASE + 0x0cu, 4);
    CHECK(zero && *zero == 0);
    uint32_t slot = 0xffffffffu;
    CHECK(!guest_read32(&m, GROUP_BASE + 0x0cu, &slot));
    CHECK(slot == 0);
    CHECK(!guest_write32(&m, GROUP_BASE + 0x2cu, 0xffffffffu));
    zero = (uint32_t *)tesv_cube_probe_ptr(GROUP_BASE + 0x2cu, 4);
    CHECK(zero && *zero == 0);
    slot = 0xffffffffu;
    CHECK(!guest_read32(&m, GROUP_BASE + 0x2cu, &slot));
    CHECK(slot == 0);
    CHECK(!tesv_cube_probe_ptr(GROUP_BASE + 0x08u, 4));
    CHECK(!tesv_cube_probe_ptr(GROUP_BASE + 0x0cu, 8));

    /* Conditional getter classification: a real group + readable buffer slot
     * must be allowed to reach the real container getter and yields the real
     * field; sentinel/small/unreadable slots return the checked NULL. */
    CHECK(!guest_write32(&m, GROUP_BASE, 0x011b019cu));
    CHECK(!guest_write32(&m, GROUP_BASE + 0x1cu, 6u));
    CHECK(!guest_write32(&m, BUFFER_BASE + 0x08u, 0x11112222u));
    CHECK(!guest_write32(&m, BUFFER_BASE + 0x0cu, 0x33334444u));
    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, BUFFER_BASE));
    uint32_t cslot = 0;
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 0, &cslot) == 1);
    CHECK(cslot == BUFFER_BASE);
    int field_ok = 0;
    CHECK(tesv_cube_probe_slot_field(&m, cslot, 0x0cu, &field_ok) ==
          0x33334444u);
    CHECK(field_ok);
    CHECK(tesv_cube_probe_slot_field(&m, cslot, 0x08u, &field_ok) ==
          0x11112222u);
    CHECK(field_ok);

    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, 0xffffffffu));
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 0, &cslot) == -1);
    CHECK(cslot == 0xffffffffu);

    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, 0x20u));
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 0, &cslot) == -1);

    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, 0x70000000u));
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 0, &cslot) == 0);

    CHECK(!guest_write32(&m, GROUP_BASE + 0x0cu, BUFFER_BASE));
    CHECK(!guest_write32(&m, GROUP_BASE + 0x20u, BUFFER_BASE));
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 6, &cslot) == 0);
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 5, &cslot) == 1);
    CHECK(tesv_cube_probe_slot_state(&m, 0x70000000u, 0, &cslot) == 0);
    CHECK(!guest_write32(&m, GROUP_BASE, 0x011b0198u));
    CHECK(tesv_cube_probe_slot_state(&m, GROUP_BASE, 0, &cslot) == 0);
    CHECK(!guest_write32(&m, GROUP_BASE, 0x011b019cu));

    /* Stack-copy group (no recognised vtable) is opt-in: a -1/small slot is
     * the checked NULL; a readable Ni2DBuffer still goes to the real getter. */
    CHECK(!_putenv_s("TESV_CUBE_PROBE_STACK", "0"));
    CHECK(!guest_write32(&m, STACK_BASE, 0u));
    CHECK(!guest_write32(&m, STACK_BASE + 0x0cu, 0xffffffffu));
    uint32_t sslot = 0;
    CHECK(tesv_cube_probe_slot_state(&m, STACK_BASE, 0, &sslot) == 0);
    CHECK(!_putenv_s("TESV_CUBE_PROBE_STACK", "1"));
    CHECK(tesv_cube_probe_slot_state(&m, STACK_BASE, 0, &sslot) == -1);
    CHECK(!guest_write32(&m, STACK_BASE + 0x0cu, BUFFER_BASE));
    CHECK(tesv_cube_probe_slot_state(&m, STACK_BASE, 0, &sslot) == 1);
    CHECK(!guest_write32(&m, STACK_BASE + 0x0cu, 0x20u));
    CHECK(tesv_cube_probe_slot_state(&m, STACK_BASE, 0, &sslot) == -1);
    CHECK(!guest_write32(&m, STACK_BASE + 0x0cu, 0x70000000u));
    CHECK(tesv_cube_probe_slot_state(&m, STACK_BASE, 0, &sslot) == 0);
    CHECK(!_putenv_s("TESV_CUBE_PROBE_STACK", "0"));

    /* A second call sees the patch and must refuse without another write. */
    uint8_t patched[CODE_SIZE];
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

    /* Wrong store/inc bytes: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[STORE_ADDR - PAGE_BASE] = 0x8b;
    CHECK(!guest_write(&m, STORE_ADDR, before + STORE_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    CHECK(!seed_page(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    before[INC_ADDR - PAGE_BASE] = 0x47;
    CHECK(!guest_write(&m, INC_ADDR, before + INC_ADDR - PAGE_BASE, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Wrong vtable bytes: refuse without writing. */
    CHECK(!seed_page(&m));
    CHECK(!seed_extra(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    uint8_t bad_vtable = 0x91;
    CHECK(!guest_write(&m, VTABLE_ADDR, &bad_vtable, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    CHECK(!seed_page(&m));
    CHECK(!seed_extra(&m));
    CHECK(!guest_read(&m, PAGE_BASE, before, sizeof before));
    uint8_t bad_vtable2 = 0x71;
    CHECK(!guest_write(&m, VTABLE2_ADDR, &bad_vtable2, 1));
    memcpy(patched, before, sizeof patched);
    CHECK(tesv_cube_skip_probe(&m) == -1);
    CHECK(!expect_unchanged(&m, patched));

    /* Vtable patch is opt-in: without TESV_CUBE_PROBE_VTABLE the code patches
     * apply but the vtable entries remain the retail values. */
    CHECK(!seed_page(&m));
    CHECK(!seed_extra(&m));
    CHECK(!_putenv_s("TESV_CUBE_PROBE_VTABLE", "0"));
    CHECK(!tesv_cube_skip_probe(&m));
    CHECK(!guest_read(&m, VTABLE_ADDR, vtable, sizeof vtable));
    CHECK(!memcmp(vtable, vtable_expected, sizeof vtable));
    CHECK(!guest_read(&m, VTABLE2_ADDR, vtable2, sizeof vtable2));
    CHECK(!memcmp(vtable2, vtable2_expected, sizeof vtable2));

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
