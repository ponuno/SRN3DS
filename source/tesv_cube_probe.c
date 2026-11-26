#include "tesv_cube_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact retail window in 0x00c91540:
 *
 *   00c9163e: F6 C2 10              test dl,0x10        ; cube branch flag
 *   00c9164d: 52 51 50              push edx/ecx/eax    ; three call args
 *   00c91650: E8 CB 45 32 00        call 0x00fb5c20     ; NiRenderedCubeMap
 *   00c916d1: 83 7C 24 38 00        cmp [esp+0x38],0    ; checked-NULL epilogue
 *
 * Patch 0x00c91650 (ten bytes) with:
 *   83 C4 0C        add esp,0x0c       ; discard the three pushed args
 *   33 F6           xor esi,esi        ; returned texture = NULL
 *   E9 77 00 00 00  jmp 0x00c916d1     ; existing checked-NULL branch
 *
 * The AOT/IR path never re-fetches patched instruction bytes; the companion
 * slice wrapper tesv_cube_probe_fb5c20() returns eax=0 for the same call site.
 * Both paths end at 0x00c916d1 with esi=0, so 0x00c91ba0 stores a NULL group
 * and the renderer's null-target diagnostic handles it. */
#define CUBE_FLAG_ADDR     0x00c9163eu
#define CUBE_PUSH_ADDR     0x00c9164du
#define CUBE_CALL_ADDR     0x00c91650u
#define CUBE_EPILOGUE_ADDR 0x00c916d1u
#define CUBE_CALL_TARGET   0x00fb5c20u
#define CUBE_STORE_ADDR    0x00c92698u
#define CUBE_INC_ADDR      0x00c926a6u
/* NiRenderTargetGroup vtable 0x011b019c slot 0x88 (GetBufferData) points at
 * 0x00fb6090.  The function is not in the AOT table or the generated IR table;
 * the IR container executes it and can bypass both guest-byte and runtime
 * shadow hooks.  Point the vtable slot at a five-byte in-text stub instead:
 *   00fb60a7: 31 C0 C2 04 00   xor eax,eax; ret 4
 * The slot is read from guest memory by every caller, including the container,
 * so this is the only hook that reliably turns the sentinel into a checked
 * NULL.  Measurement only. */
#define GETBUFFER_VTABLE_ADDR 0x011b0224u
#define GETBUFFER_VTABLE2_ADDR 0x011b0220u
#define GETBUFFER_STUB_ADDR   0x00fb60a7u
#define GETBUFFER_ENTRY       0x00fb6090u
#define GETBUFFER_ENTRY2      0x00fb6070u

static const uint8_t flag_expected[3]     = {0xf6, 0xc2, 0x10};
static const uint8_t push_expected[3]     = {0x52, 0x51, 0x50};
static const uint8_t call_expected[5]     = {0xe8, 0xcb, 0x45, 0x32, 0x00};
static const uint8_t epilogue_expected[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};
/* 00c92698: 89 44 B4 40   mov [esp+esi*4+0x40],eax  (store the cube group)
 * 00c926a6: 46            inc esi                   (count the cube slot)
 * Skipping the cube slot means both must be dropped so the next group reuses
 * the same index and the caller's count stays consistent. */
static const uint8_t store_expected[4]    = {0x89, 0x44, 0xb4, 0x40};
static const uint8_t inc_expected[1]      = {0x46};
static const uint8_t vtable_expected[4]   = {0x90, 0x60, 0xfb, 0x00};
static const uint8_t vtable2_expected[4]  = {0x70, 0x60, 0xfb, 0x00};
static const uint8_t stub_site_expected[5] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
static const uint8_t stub_patch[5]        = {0x31, 0xc0, 0xc2, 0x04, 0x00};
static const uint8_t vtable_patch[4]      = {0x00, 0x00, 0x00, 0x30};
static const uint8_t vtable2_patch[4]     = {0x00, 0x00, 0x00, 0x30};

static const uint8_t patch[10] = {
    0x83, 0xc4, 0x0c,        /* add esp,0x0c */
    0x33, 0xf6,              /* xor esi,esi */
    0xe9, 0x77, 0x00, 0x00, 0x00 /* jmp 0x00c916d1 */
};
static const uint8_t store_patch[4] = {0x90, 0x90, 0x90, 0x90};
static const uint8_t inc_patch[1]   = {0x90};

static int cube_skip_active;
static GuestMemory *cube_memory;
static uint32_t cube_zero;
static int cube_shadow_logged;

int tesv_cube_skip_active(void) {
    return cube_skip_active;
}

GuestMemory *tesv_cube_probe_memory(void) {
    return cube_memory;
}

/* The skipped water-reflection group is not the only sentinel in the path.
 * The renderer's default group at [0x01b913f0]+0x628 is created before the
 * cube group and can hold a 0xffffffff buffer slot when the back buffer is not
 * a Ni2DBuffer.  GetBufferData (0x00fb6090) reads [group+index*4+0xc] and then
 * [slot+0xc]; 0xffffffff faults at address 0xb.  The measurement probe
 * rewrites only that -1 slot to a checked NULL and returns a zero shadow, so
 * the getter takes its existing test/je NULL path.  It does not create or
 * substitute a cube. */
void *tesv_cube_probe_ptr(uint32_t address, size_t bytes) {
    if (!cube_skip_active || !cube_memory || bytes != 4) return NULL;

    uint32_t value = 0;
    if (guest_read32(cube_memory, address, &value) || value != 0xffffffffu)
        return NULL;

    /* The -1 sentinel lives in a NiRenderTargetGroup: either the engine-side
     * vtable 0x011b019c (0x28-byte object) or the renderer-side group vtable
     * 0x011539d8 (0x40-byte object).  Walk back at most one object to find the
     * base and zero only the field that returned -1.  This covers the default
     * group and every cube face sub-group without touching unrelated data. */
    for (uint32_t off = 0; off <= 0x3cu; off += 4u) {
        if (address < off) break;
        uint32_t base = address - off;
        uint32_t vtable = 0;
        if (guest_read32(cube_memory, base, &vtable)) break;
        if (vtable != 0x011b019cu && vtable != 0x011539d8u) continue;
        if (guest_write32(cube_memory, address, 0u)) return NULL;
        if (!cube_shadow_logged) {
            cube_shadow_logged = 1;
            fprintf(stderr, "[cube-probe] group 0x%08lx field +0x%lx "
                            "0xffffffff -> NULL; measurement only\n",
                    (unsigned long)base, (unsigned long)off);
        }
        if (getenv("TESV_CUBE_PROBE_DEBUG")) {
            fprintf(stderr, "[cube-probe] ptr addr=0x%08lx group=0x%08lx "
                            "field=+0x%lx\n", (unsigned long)address,
                    (unsigned long)base, (unsigned long)off);
        }
        return &cube_zero;
    }
    return NULL;
}

static void dump_bytes(const char *label, uint32_t address,
                       const uint8_t *expected, const uint8_t *got,
                       size_t count, int readable) {
    fprintf(stderr, "[cube-probe] %s 0x%08lx expected ", label,
            (unsigned long)address);
    for (size_t i = 0; i < count; ++i)
        fprintf(stderr, "%s%02X", i ? " " : "", expected[i]);
    fprintf(stderr, " got ");
    if (!readable) {
        fprintf(stderr, "<unreadable>");
    } else {
        for (size_t i = 0; i < count; ++i)
            fprintf(stderr, "%s%02X", i ? " " : "", got[i]);
    }
    fputc('\n', stderr);
}

int tesv_cube_skip_probe(GuestMemory *m) {
    const char *enabled = getenv("TESV_D3D9_CUBE_SKIP_PROBE");
    if (!enabled || strcmp(enabled, "1")) {
        cube_skip_active = 0;
        cube_memory = NULL;
        cube_shadow_logged = 0;
        return 0;
    }
    if (!m) {
        fprintf(stderr, "[cube-probe] REFUSED: null GuestMemory; no patch\n");
        return -1;
    }

    const char *vtable_env = getenv("TESV_CUBE_PROBE_VTABLE");
    int vtable_on = vtable_env && !strcmp(vtable_env, "1");

    uint8_t flag[3], push[3], call[5], epilogue[5], store[4], inc[1];
    uint8_t vtable[4], vtable2[4], stub_site[5];
    int flag_ok = guest_read(m, CUBE_FLAG_ADDR, flag, sizeof flag) == 0;
    int push_ok = guest_read(m, CUBE_PUSH_ADDR, push, sizeof push) == 0;
    int call_ok = guest_read(m, CUBE_CALL_ADDR, call, sizeof call) == 0;
    int epilogue_ok = guest_read(m, CUBE_EPILOGUE_ADDR, epilogue,
                                 sizeof epilogue) == 0;
    int store_ok = guest_read(m, CUBE_STORE_ADDR, store, sizeof store) == 0;
    int inc_ok = guest_read(m, CUBE_INC_ADDR, inc, sizeof inc) == 0;
    int vtable_ok = !vtable_on ||
        guest_read(m, GETBUFFER_VTABLE_ADDR, vtable, sizeof vtable) == 0;
    int vtable2_ok = !vtable_on ||
        guest_read(m, GETBUFFER_VTABLE2_ADDR, vtable2, sizeof vtable2) == 0;
    int stub_ok = !vtable_on ||
        guest_read(m, GETBUFFER_STUB_ADDR, stub_site, sizeof stub_site) == 0;

    int target_ok = 0;
    uint32_t target = 0;
    if (call_ok) {
        int32_t rel;
        memcpy(&rel, call + 1, sizeof rel);
        target = CUBE_CALL_ADDR + 5u + (uint32_t)rel;
        target_ok = target == CUBE_CALL_TARGET;
    }

    if (!flag_ok || !push_ok || !call_ok || !epilogue_ok || !store_ok ||
        !inc_ok || !target_ok ||
        memcmp(flag, flag_expected, sizeof flag) ||
        memcmp(push, push_expected, sizeof push) ||
        memcmp(call, call_expected, sizeof call) ||
        memcmp(epilogue, epilogue_expected, sizeof epilogue) ||
        memcmp(store, store_expected, sizeof store) ||
        memcmp(inc, inc_expected, sizeof inc) ||
        (vtable_on && (!vtable_ok || !vtable2_ok || !stub_ok ||
         memcmp(vtable, vtable_expected, sizeof vtable) ||
         memcmp(vtable2, vtable2_expected, sizeof vtable2) ||
         memcmp(stub_site, stub_site_expected, sizeof stub_site)))) {
        fprintf(stderr, "[cube-probe] REFUSED: byte verification failed; "
                        "no patch\n");
        dump_bytes("flag", CUBE_FLAG_ADDR, flag_expected, flag, sizeof flag,
                   flag_ok);
        dump_bytes("push", CUBE_PUSH_ADDR, push_expected, push, sizeof push,
                   push_ok);
        dump_bytes("call", CUBE_CALL_ADDR, call_expected, call, sizeof call,
                   call_ok);
        dump_bytes("epilogue", CUBE_EPILOGUE_ADDR, epilogue_expected, epilogue,
                   sizeof epilogue, epilogue_ok);
        dump_bytes("store", CUBE_STORE_ADDR, store_expected, store, sizeof store,
                   store_ok);
        dump_bytes("inc", CUBE_INC_ADDR, inc_expected, inc, sizeof inc, inc_ok);
        if (vtable_on) {
            dump_bytes("vtable", GETBUFFER_VTABLE_ADDR, vtable_expected, vtable,
                       sizeof vtable, vtable_ok);
            dump_bytes("vtable2", GETBUFFER_VTABLE2_ADDR, vtable2_expected,
                       vtable2, sizeof vtable2, vtable2_ok);
            dump_bytes("stub", GETBUFFER_STUB_ADDR, stub_site_expected, stub_site,
                       sizeof stub_site, stub_ok);
        }
        if (call_ok) {
            fprintf(stderr, "[cube-probe] call target 0x%08lx expected "
                            "0x%08lx\n", (unsigned long)target,
                    (unsigned long)CUBE_CALL_TARGET);
        } else {
            fprintf(stderr, "[cube-probe] call target <unreadable> expected "
                            "0x%08lx\n", (unsigned long)CUBE_CALL_TARGET);
        }
        return -1;
    }

    if (guest_write(m, CUBE_CALL_ADDR, patch, sizeof patch) ||
        guest_write(m, CUBE_STORE_ADDR, store_patch, sizeof store_patch) ||
        guest_write(m, CUBE_INC_ADDR, inc_patch, sizeof inc_patch) ||
        (vtable_on &&
         (guest_write(m, GETBUFFER_STUB_ADDR, stub_patch, sizeof stub_patch) ||
          guest_write(m, GETBUFFER_VTABLE_ADDR, vtable_patch,
                      sizeof vtable_patch) ||
          guest_write(m, GETBUFFER_VTABLE2_ADDR, vtable2_patch,
                      sizeof vtable2_patch)))) {
        fprintf(stderr, "[cube-probe] REFUSED: patch write failed; no patch\n");
        return -1;
    }

    cube_skip_active = 1;
    cube_memory = m;
    cube_shadow_logged = 0;
    fprintf(stderr, "[cube-probe] MEASUREMENT ONLY: 0x00c91650 "
                    "E8 CB 45 32 00 -> 83 C4 0C 33 F6 E9 77 00 00 00; "
                    "0x00c92698 store and 0x00c926a6 inc NOPed so the cube slot "
                    "is skipped with the caller's count; 0x00c916d1 "
                    "checked-NULL path; caps/SM3/T&L unchanged; not "
                    "shippable\n");
    if (vtable_on)
        fprintf(stderr, "[cube-probe] MEASUREMENT ONLY: GetBufferData vtable "
                        "0x011b0220/0x011b0224 -> 0x30000000 host stub "
                        "(eax=0; ret 4); not shippable\n");
    return 0;
}
