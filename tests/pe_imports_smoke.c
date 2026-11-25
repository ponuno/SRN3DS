#include "pe_imports.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "PE import test failed at line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t image_base = 0x00010000u;
static uint32_t dll_base = 0x00020000u;

static void put32(GuestMemory *m, uint32_t address, uint32_t value) {
    (void)guest_write32(m, address, value);
}

static void put16(GuestMemory *m, uint32_t address, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    (void)guest_write(m, address, bytes, sizeof bytes);
}

static void put_string(GuestMemory *m, uint32_t address, const char *value) {
    (void)guest_write(m, address, value, strlen(value) + 1);
}

/* Models a diagnostic fallback: unknown imports get a named trap thunk rather
 * than zero. Such a fallback must not hide an export in an already loaded PE. */
static uint32_t diagnostic_fallback(const PeImport *entry, void *ctx) {
    (void)entry;
    ++*(unsigned *)ctx;
    return 0xff000000u;
}

static uint32_t strict_builtin(const PeImport *entry, void *ctx) {
    (void)entry;
    (void)ctx;
    return 0;
}

static uint32_t shim_builtin(const PeImport *entry, void *ctx) {
    (void)entry;
    ++*(unsigned *)ctx;
    return 0xff100000u;
}

int main(void) {
    GuestMemory memory;
    guest_init(&memory, 0x10000u);
    CHECK(guest_map(&memory, image_base, 0x1000u) == 0);
    CHECK(guest_map(&memory, dll_base, 0x1000u) == 0);

    GuestImage importer = {
        .base = image_base, .size = 0x1000u,
        .import_rva = 0x300u, .import_size = 40u,
    };
    GuestImage exporter = {
        .base = dll_base, .size = 0x1000u,
        .export_rva = 0x100u, .export_size = 0x100u,
    };

    /* Two exports: Alpha is local, Forwarded points into the export directory
     * and must remain explicitly unsupported. The name table is sorted. */
    put32(&memory, dll_base + 0x110u, 1);       /* ordinal base */
    put32(&memory, dll_base + 0x114u, 2);       /* function count */
    put32(&memory, dll_base + 0x118u, 2);       /* name count */
    put32(&memory, dll_base + 0x11cu, 0x140u);  /* address table */
    put32(&memory, dll_base + 0x120u, 0x148u);  /* name table */
    put32(&memory, dll_base + 0x124u, 0x150u);  /* ordinal table */
    put32(&memory, dll_base + 0x140u, 0x400u);
    put32(&memory, dll_base + 0x144u, 0x180u);
    put32(&memory, dll_base + 0x148u, 0x1a0u);
    put32(&memory, dll_base + 0x14cu, 0x1c0u);
    put16(&memory, dll_base + 0x150u, 0);
    put16(&memory, dll_base + 0x152u, 1);
    put_string(&memory, dll_base + 0x180u, "other.dll!Beta");
    put_string(&memory, dll_base + 0x1a0u, "Alpha");
    put_string(&memory, dll_base + 0x1c0u, "Forwarded");

    CHECK(pe_export_find(&memory, &exporter, "Alpha", 0) == dll_base + 0x400u);
    CHECK(pe_export_find(&memory, &exporter, NULL, 1) == dll_base + 0x400u);
    CHECK(pe_export_find(&memory, &exporter, "Forwarded", 0) == 0);
    CHECK(pe_export_find(&memory, &exporter, "Missing", 0) == 0);
    CHECK(pe_export_find(&memory, &exporter, NULL, 2) == 0);
    CHECK(pe_export_find(&memory, &exporter, NULL, 3) == 0);

    /* One descriptor with named, ordinal, forwarded, and missing imports. */
    put32(&memory, image_base + 0x300u, 0x500u); /* ILT */
    put32(&memory, image_base + 0x30cu, 0x540u); /* DLL name */
    put32(&memory, image_base + 0x310u, 0x600u); /* IAT */
    put_string(&memory, image_base + 0x540u, "fixture.dll");
    put32(&memory, image_base + 0x500u, 0x580u);
    put32(&memory, image_base + 0x504u, 0x80000001u);
    put32(&memory, image_base + 0x508u, 0x590u);
    put32(&memory, image_base + 0x50cu, 0x5c0u);
    put32(&memory, image_base + 0x600u, 0x580u);
    put32(&memory, image_base + 0x604u, 0x80000001u);
    put32(&memory, image_base + 0x608u, 0x590u);
    put32(&memory, image_base + 0x60cu, 0x5c0u);
    put16(&memory, image_base + 0x580u, 0);
    put_string(&memory, image_base + 0x582u, "Alpha");
    put16(&memory, image_base + 0x590u, 0);
    put_string(&memory, image_base + 0x592u, "Forwarded");
    put16(&memory, image_base + 0x5c0u, 0);
    put_string(&memory, image_base + 0x5c2u, "Missing");

    PeModule module = {"FIXTURE.DLL", &exporter};
    PeLinker strict = {&memory, &module, 1, strict_builtin, NULL, NULL, NULL};
    PeImport alpha = {.dll = "fixture.dll", .name = "Alpha", .iat = image_base + 0x600u};
    PeImport ordinal = {.dll = "fixture.dll", .ordinal = 1, .iat = image_base + 0x604u};
    PeImport forwarded = {.dll = "fixture.dll", .name = "Forwarded"};
    PeImport missing = {.dll = "fixture.dll", .name = "Missing"};
    PeImport absent = {.dll = "absent.dll", .name = "Missing"};

    CHECK(pe_link_resolve(&alpha, &strict) == dll_base + 0x400u);
    CHECK(pe_link_resolve(&ordinal, &strict) == dll_base + 0x400u);
    CHECK(pe_link_resolve(&forwarded, &strict) == 0);
    CHECK(pe_link_resolve(&missing, &strict) == 0);
    CHECK(pe_link_resolve(&absent, &strict) == 0);

    /* A strict resolver rejects the missing export before any IAT cell changes. */
    size_t bound = 99;
    uint32_t cell = 0;
    CHECK(pe_import_bind(&memory, &importer, pe_link_resolve, &strict, &bound) == PE_IMPORT_UNRESOLVED);
    CHECK(bound == 0);
    CHECK(guest_read32(&memory, image_base + 0x600u, &cell) == 0 && cell == 0x580u);
    CHECK(guest_read32(&memory, image_base + 0x604u, &cell) == 0 && cell == 0x80000001u);
    CHECK(guest_read32(&memory, image_base + 0x608u, &cell) == 0 && cell == 0x590u);
    CHECK(guest_read32(&memory, image_base + 0x60cu, &cell) == 0 && cell == 0x5c0u);

    /* Implemented builtins retain priority (for example, an X3DAudio shim can
     * intentionally replace a staged DLL export). */
    unsigned calls = 0;
    PeLinker shimmed = {&memory, &module, 1, shim_builtin, &calls, NULL, NULL};
    CHECK(pe_link_resolve(&alpha, &shimmed) == 0xff100000u);
    CHECK(calls == 1);

    /* A diagnostic fallback still handles genuinely unsupported imports, but
     * it must not replace the two exports that really exist in the loaded PE. */
    calls = 0;
    PeLinker diagnostic = {&memory, &module, 1, strict_builtin, NULL, diagnostic_fallback, &calls};
    CHECK(pe_link_resolve(&alpha, &diagnostic) == dll_base + 0x400u);
    CHECK(pe_link_resolve(&ordinal, &diagnostic) == dll_base + 0x400u);
    CHECK(calls == 0);
    CHECK(pe_link_resolve(&forwarded, &diagnostic) == 0xff000000u);
    CHECK(pe_link_resolve(&missing, &diagnostic) == 0xff000000u);
    CHECK(pe_link_resolve(&absent, &diagnostic) == 0xff000000u);
    CHECK(calls == 3);

    CHECK(pe_import_bind(&memory, &importer, pe_link_resolve, &diagnostic, &bound) == PE_IMPORT_OK);
    CHECK(bound == 4);
    CHECK(guest_read32(&memory, image_base + 0x600u, &cell) == 0 && cell == dll_base + 0x400u);
    CHECK(guest_read32(&memory, image_base + 0x604u, &cell) == 0 && cell == dll_base + 0x400u);
    CHECK(guest_read32(&memory, image_base + 0x608u, &cell) == 0 && cell == 0xff000000u);
    CHECK(guest_read32(&memory, image_base + 0x60cu, &cell) == 0 && cell == 0xff000000u);

    guest_destroy(&memory);
    puts("PE import/export linker tests passed");
    return 0;
}
