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

static const uint8_t flag_expected[3]     = {0xf6, 0xc2, 0x10};
static const uint8_t push_expected[3]     = {0x52, 0x51, 0x50};
static const uint8_t call_expected[5]     = {0xe8, 0xcb, 0x45, 0x32, 0x00};
static const uint8_t epilogue_expected[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};

static const uint8_t patch[10] = {
    0x83, 0xc4, 0x0c,        /* add esp,0x0c */
    0x33, 0xf6,              /* xor esi,esi */
    0xe9, 0x77, 0x00, 0x00, 0x00 /* jmp 0x00c916d1 */
};

static int cube_skip_active;

int tesv_cube_skip_active(void) {
    return cube_skip_active;
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
        return 0;
    }
    if (!m) {
        fprintf(stderr, "[cube-probe] REFUSED: null GuestMemory; no patch\n");
        return -1;
    }

    uint8_t flag[3], push[3], call[5], epilogue[5];
    int flag_ok = guest_read(m, CUBE_FLAG_ADDR, flag, sizeof flag) == 0;
    int push_ok = guest_read(m, CUBE_PUSH_ADDR, push, sizeof push) == 0;
    int call_ok = guest_read(m, CUBE_CALL_ADDR, call, sizeof call) == 0;
    int epilogue_ok = guest_read(m, CUBE_EPILOGUE_ADDR, epilogue,
                                 sizeof epilogue) == 0;

    int target_ok = 0;
    uint32_t target = 0;
    if (call_ok) {
        int32_t rel;
        memcpy(&rel, call + 1, sizeof rel);
        target = CUBE_CALL_ADDR + 5u + (uint32_t)rel;
        target_ok = target == CUBE_CALL_TARGET;
    }

    if (!flag_ok || !push_ok || !call_ok || !epilogue_ok || !target_ok ||
        memcmp(flag, flag_expected, sizeof flag) ||
        memcmp(push, push_expected, sizeof push) ||
        memcmp(call, call_expected, sizeof call) ||
        memcmp(epilogue, epilogue_expected, sizeof epilogue)) {
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

    if (guest_write(m, CUBE_CALL_ADDR, patch, sizeof patch)) {
        fprintf(stderr, "[cube-probe] REFUSED: ten-byte patch write failed; "
                        "no patch\n");
        return -1;
    }

    cube_skip_active = 1;
    fprintf(stderr, "[cube-probe] MEASUREMENT ONLY: 0x00c91650 "
                    "E8 CB 45 32 00 -> 83 C4 0C 33 F6 E9 77 00 00 00; "
                    "NiRenderedCubeMap creation skipped through the existing "
                    "0x00c916d1 checked-NULL path; caps/SM3/T&L unchanged; "
                    "not shippable\n");
    return 0;
}
