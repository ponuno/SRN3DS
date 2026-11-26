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

    /* A validated MSVC /hotpatch export must bind to the real body, not to the
     * forwarder thunk (the TESV D3DXMatrixMultiply case). */
    const uint32_t hp_base = 0x00030000u;
    CHECK(guest_map(&memory, hp_base, 0x1000u) == 0);
    GuestImage hp = {.base = hp_base, .size = 0x1000u,
                     .export_rva = 0x100u, .export_size = 0x100u};
    put32(&memory, hp_base + 0x110u, 1);      /* ordinal base */
    put32(&memory, hp_base + 0x114u, 1);      /* function count */
    put32(&memory, hp_base + 0x118u, 1);      /* name count */
    put32(&memory, hp_base + 0x11cu, 0x140u); /* address table */
    put32(&memory, hp_base + 0x120u, 0x148u); /* name table */
    put32(&memory, hp_base + 0x124u, 0x150u); /* ordinal table */
    put32(&memory, hp_base + 0x140u, 0x400u); /* export entry rva */
    put32(&memory, hp_base + 0x148u, 0x1a0u);
    put16(&memory, hp_base + 0x150u, 0);
    put_string(&memory, hp_base + 0x1a0u, "Gamma");
    /* entry: FF 25 <slot 0x340> + 5x CC; wrapper at 0x3e0; init at 0x360;
     * real body at entry+11 (0x40b). */
    {
        uint8_t entry[11] = {0xff, 0x25, 0x40, 0x03, 0x03, 0x00,
                             0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        CHECK(guest_write(&memory, hp_base + 0x400u, entry, sizeof entry) == 0);
    }
    put32(&memory, hp_base + 0x340u, hp_base + 0x3e0u); /* slot -> wrapper */
    {
        uint8_t wrapper[19] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0x6a, 0x01, 0xe8,
                               0x74, 0xff, 0xff, 0xff, /* call 0x360 */
                               0x5d, 0xff, 0x25, 0x40, 0x03, 0x03, 0x00};
        CHECK(guest_write(&memory, hp_base + 0x3e0u, wrapper, sizeof wrapper) == 0);
    }
    put32(&memory, hp_base + 0x360u, 0x000000c3u); /* one-time initializer */
    {
        uint8_t body[6] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0xc3};
        CHECK(guest_write(&memory, hp_base + 0x40bu, body, sizeof body) == 0);
    }
    PeModule hp_module = {"HP.DLL", &hp};
    PeLinker hp_link = {&memory, &hp_module, 1, NULL, NULL, NULL, NULL};
    PeImport gamma = {.dll = "hp.dll", .name = "Gamma"};
    CHECK(pe_export_find(&memory, &hp, "Gamma", 0) == hp_base + 0x400u);
    CHECK(pe_link_resolve(&gamma, &hp_link) == hp_base + 0x40bu);
    /* One differing padding byte must leave the raw export address. */
    {
        uint8_t pad = 0x90;
        CHECK(guest_write(&memory, hp_base + 0x406u, &pad, 1) == 0);
        CHECK(pe_link_resolve(&gamma, &hp_link) == hp_base + 0x400u);
        pad = 0xcc;
        CHECK(guest_write(&memory, hp_base + 0x406u, &pad, 1) == 0);
        CHECK(pe_link_resolve(&gamma, &hp_link) == hp_base + 0x40bu);
    }
    /* The one-time initializer call is guarded by push 1, not any imm8. */
    {
        uint8_t imm = 0x02;
        CHECK(guest_write(&memory, hp_base + 0x3e6u, &imm, 1) == 0);
        CHECK(pe_link_resolve(&gamma, &hp_link) == hp_base + 0x400u);
        imm = 0x01;
        CHECK(guest_write(&memory, hp_base + 0x3e6u, &imm, 1) == 0);
        CHECK(pe_link_resolve(&gamma, &hp_link) == hp_base + 0x40bu);
    }

    /* A second hotpatch layout: the export slot points to a wrapper that
     * calls an initializer which copies a real-body table.  entry+11 is
     * another wrapper, so the resolver must use the table, not entry+11. */
    const uint32_t hp2_base = 0x00050000u;
    CHECK(guest_map(&memory, hp2_base, 0x2000u) == 0);
    GuestImage hp2 = {.base = hp2_base, .size = 0x2000u,
                      .export_rva = 0x100u, .export_size = 0x100u};
    put32(&memory, hp2_base + 0x110u, 1);
    put32(&memory, hp2_base + 0x114u, 1);
    put32(&memory, hp2_base + 0x118u, 1);
    put32(&memory, hp2_base + 0x11cu, 0x140u);
    put32(&memory, hp2_base + 0x120u, 0x148u);
    put32(&memory, hp2_base + 0x124u, 0x150u);
    put32(&memory, hp2_base + 0x140u, 0x500u);
    put32(&memory, hp2_base + 0x148u, 0x1a0u);
    put16(&memory, hp2_base + 0x150u, 0);
    put_string(&memory, hp2_base + 0x1a0u, "Epsilon");
    {
        uint8_t entry[11] = {0xff, 0x25, 0x00, 0x06, 0x05, 0x00,
                             0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        CHECK(guest_write(&memory, hp2_base + 0x500u, entry, sizeof entry) == 0);
    }
    put32(&memory, hp2_base + 0x600u, hp2_base + 0x4e0u); /* slot -> wrapper */
    {
        uint8_t wrapper[19] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0x6a, 0x01, 0xe8,
                               0x14, 0x02, 0x00, 0x00, /* call 0x700 */
                               0x5d, 0xff, 0x25, 0x00, 0x06, 0x05, 0x00};
        CHECK(guest_write(&memory, hp2_base + 0x4e0u, wrapper, sizeof wrapper) == 0);
    }
    /* Initializer: mov esi,0x50800; mov edi,0x50600; nops; rep movsd. */
    {
        uint8_t init[17] = {0xbe, 0x00, 0x08, 0x05, 0x00,
                            0xbf, 0x00, 0x06, 0x05, 0x00,
                            0x90, 0x90, 0x90, 0x90, 0x90, 0xf3, 0xa5};
        CHECK(guest_write(&memory, hp2_base + 0x700u, init, sizeof init) == 0);
    }
    /* entry+11 is a second wrapper, not the body. */
    {
        uint8_t wrapper2[19] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0x6a, 0x01, 0xe8,
                                0x49, 0x02, 0x00, 0x00, /* call 0x760 */
                                0x5d, 0xff, 0x25, 0x04, 0x06, 0x05, 0x00};
        CHECK(guest_write(&memory, hp2_base + 0x50bu, wrapper2, sizeof wrapper2) == 0);
    }
    put32(&memory, hp2_base + 0x604u, hp2_base + 0x50bu);
    put32(&memory, hp2_base + 0x800u, hp2_base + 0x520u); /* source table[0] */
    {
        uint8_t body[6] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0xc3};
        CHECK(guest_write(&memory, hp2_base + 0x520u, body, sizeof body) == 0);
    }
    PeModule hp2_module = {"HP2.DLL", &hp2};
    PeLinker hp2_link = {&memory, &hp2_module, 1, NULL, NULL, NULL, NULL};
    PeImport epsilon = {.dll = "hp2.dll", .name = "Epsilon"};
    CHECK(pe_export_find(&memory, &hp2, "Epsilon", 0) == hp2_base + 0x500u);
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x520u);
    /* An invalid table entry must leave the raw export, not entry+11. */
    put32(&memory, hp2_base + 0x800u, 0);
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x500u);
    put32(&memory, hp2_base + 0x800u, hp2_base + 0x520u);
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x520u);

    /* A table entry that points at the guard wrapper must not be accepted just
     * because the wrapper shares the five-byte MSVC frame prologue. */
    put32(&memory, hp2_base + 0x800u, hp2_base + 0x4e0u);
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x500u);
    put32(&memory, hp2_base + 0x800u, hp2_base + 0x520u);

    /* `mov ecx,1` bounds the copy: an index past it must not read table[3]
     * even when that dword is a readable function body. */
    {
        uint8_t init[17] = {0xbe, 0x00, 0x08, 0x05, 0x00,
                            0xbf, 0xf4, 0x05, 0x05, 0x00, /* dst = slot - 12 */
                            0xb9, 0x01, 0x00, 0x00, 0x00, /* mov ecx,1 */
                            0xf3, 0xa5};
        CHECK(guest_write(&memory, hp2_base + 0x700u, init, sizeof init) == 0);
    }
    put32(&memory, hp2_base + 0x800u, 0);
    put32(&memory, hp2_base + 0x80cu, hp2_base + 0x520u); /* table[3] */
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x500u);

    /* The first memcpy pattern may be some other table; keep scanning. */
    {
        uint8_t init[128] = {0};
        uint8_t first[12] = {0xbe, 0x00, 0x09, 0x05, 0x00,
                             0xbf, 0xfc, 0x05, 0x05, 0x00, 0xf3, 0xa5};
        uint8_t second[17] = {0xbe, 0x00, 0x08, 0x05, 0x00,
                              0xbf, 0x00, 0x06, 0x05, 0x00,
                              0xb9, 0x01, 0x00, 0x00, 0x00, 0xf3, 0xa5};
        memcpy(init, first, sizeof first);
        memcpy(init + 0x20, second, sizeof second);
        CHECK(guest_write(&memory, hp2_base + 0x700u, init, sizeof init) == 0);
    }
    put32(&memory, hp2_base + 0x800u, hp2_base + 0x520u);
    put32(&memory, hp2_base + 0x904u, 0); /* first pattern's table[1] */
    CHECK(pe_link_resolve(&epsilon, &hp2_link) == hp2_base + 0x520u);

    /* An entry at size-11 has its body one past the image.  A readable prologue
     * in the next mapped page must not be mistaken for that body. */
    const uint32_t hp3_base = 0x00090000u;
    CHECK(guest_map(&memory, hp3_base, 0x1000u) == 0);
    CHECK(guest_map(&memory, hp3_base + 0x1000u, 0x1000u) == 0);
    GuestImage hp3 = {.base = hp3_base, .size = 0x1000u,
                      .export_rva = 0x100u, .export_size = 0x100u};
    put32(&memory, hp3_base + 0x110u, 1);
    put32(&memory, hp3_base + 0x114u, 1);
    put32(&memory, hp3_base + 0x118u, 1);
    put32(&memory, hp3_base + 0x11cu, 0x140u);
    put32(&memory, hp3_base + 0x120u, 0x148u);
    put32(&memory, hp3_base + 0x124u, 0x150u);
    put32(&memory, hp3_base + 0x140u, 0xff5u);
    put32(&memory, hp3_base + 0x148u, 0x1a0u);
    put16(&memory, hp3_base + 0x150u, 0);
    put_string(&memory, hp3_base + 0x1a0u, "Zeta");
    {
        uint8_t entry[11] = {0xff, 0x25, 0x00, 0x06, 0x09, 0x00,
                             0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        CHECK(guest_write(&memory, hp3_base + 0xff5u, entry, sizeof entry) == 0);
    }
    put32(&memory, hp3_base + 0x600u, hp3_base + 0x800u);
    {
        uint8_t wrapper[19] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0x6a, 0x01, 0xe8,
                               0x74, 0xff, 0xff, 0xff, /* call 0x700 */
                               0x5d, 0xff, 0x25, 0x00, 0x06, 0x09, 0x00};
        CHECK(guest_write(&memory, hp3_base + 0x800u, wrapper, sizeof wrapper) == 0);
    }
    {
        uint8_t body[6] = {0x8b, 0xff, 0x55, 0x8b, 0xec, 0xc3};
        CHECK(guest_write(&memory, hp3_base + 0x1000u, body, sizeof body) == 0);
    }
    PeModule hp3_module = {"HP3.DLL", &hp3};
    PeLinker hp3_link = {&memory, &hp3_module, 1, NULL, NULL, NULL, NULL};
    PeImport zeta = {.dll = "hp3.dll", .name = "Zeta"};
    CHECK(pe_export_find(&memory, &hp3, "Zeta", 0) == hp3_base + 0xff5u);
    CHECK(pe_link_resolve(&zeta, &hp3_link) == hp3_base + 0xff5u);

    guest_destroy(&memory);
    puts("PE import/export linker tests passed");
    return 0;
}
