#include "tesv_sm3_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SM3_GATE_ADDR 0x00c6e22du
#define SM3_JGE_ADDR  0x00c6e23bu
#define SM3_LEVEL_ADDR 0x01ba749cu

static const uint8_t sm3_gate_expected[7] = {
    0x83, 0x3d, 0x9c, 0x74, 0xba, 0x01, 0x07
};
static const uint8_t sm3_jge_expected[2] = {0x7d, 0x0a};
static uint32_t sm3_level_shadow = 7;
static uint32_t sm3_level_sink;
static uint32_t sm3_level_sink_seen;
static int sm3_level_write_seen;
static int sm3_patch_active;
static int sm3_shadow_logged;

static void dump_bytes(const char *label, uint32_t address,
                       const uint8_t *bytes, size_t count) {
    fprintf(stderr, "[sm3-probe] %s 0x%08lx expected ", label,
            (unsigned long)address);
    for (size_t i = 0; i < count; ++i)
        fprintf(stderr, "%s%02X", i ? " " : "", sm3_gate_expected[i]);
    fprintf(stderr, " got ");
    if (!bytes) {
        fprintf(stderr, "<unreadable>");
    } else {
        for (size_t i = 0; i < count; ++i)
            fprintf(stderr, "%s%02X", i ? " " : "", bytes[i]);
    }
    fputc('\n', stderr);
}

void *tesv_sm3_probe_level_ptr(uint32_t address, size_t bytes) {
    if (!sm3_patch_active || address != SM3_LEVEL_ADDR || bytes != 4) return NULL;

    /* Until c71080's first store, serve a writable sink.  Once that store has
     * changed the sink, every subsequent predecoded load sees invariant 7. */
    if (!sm3_level_write_seen && sm3_level_sink != sm3_level_sink_seen) {
        sm3_level_write_seen = 1;
        sm3_level_sink_seen = sm3_level_sink;
    }
    if (sm3_level_write_seen) {
        if (!sm3_shadow_logged) {
            sm3_shadow_logged = 1;
            fprintf(stderr, "[sm3-probe] AOT/IR level shadow active; guest "
                            "[0x01ba749c] unchanged\n");
        }
        return &sm3_level_shadow;
    }
    return &sm3_level_sink;
}

int tesv_sm3_gate_probe(GuestMemory *m) {
    const char *enabled = getenv("TESV_D3D9_SM3_GATE_PROBE");
    if (!enabled || strcmp(enabled, "1")) {
        sm3_patch_active = 0;
        sm3_level_sink = 0;
        sm3_level_sink_seen = 0;
        sm3_level_write_seen = 0;
        sm3_shadow_logged = 0;
        return 0;
    }
    if (!m) {
        fprintf(stderr, "[sm3-probe] REFUSED: null GuestMemory; no patch\n");
        return -1;
    }

    uint8_t gate[sizeof sm3_gate_expected];
    uint8_t jge[sizeof sm3_jge_expected];
    int gate_ok = guest_read(m, SM3_GATE_ADDR, gate, sizeof gate) == 0;
    int jge_ok = guest_read(m, SM3_JGE_ADDR, jge, sizeof jge) == 0;

    if (!gate_ok || !jge_ok ||
        memcmp(gate, sm3_gate_expected, sizeof gate) ||
        memcmp(jge, sm3_jge_expected, sizeof jge)) {
        fprintf(stderr, "[sm3-probe] REFUSED: byte verification failed; no patch\n");
        dump_bytes("cmp", SM3_GATE_ADDR, gate_ok ? gate : NULL,
                   sizeof sm3_gate_expected);
        /* The branch has a different expected sequence from the cmp. */
        fprintf(stderr, "[sm3-probe] jge 0x%08lx expected 7D 0A got ",
                (unsigned long)SM3_JGE_ADDR);
        if (!jge_ok) fprintf(stderr, "<unreadable>");
        else fprintf(stderr, "%02X %02X", jge[0], jge[1]);
        fputc('\n', stderr);
        return -1;
    }

    const uint8_t unconditional = 0xeb;
    if (guest_write(m, SM3_JGE_ADDR, &unconditional, 1)) {
        fprintf(stderr, "[sm3-probe] REFUSED: one-byte patch write failed; no patch\n");
        return -1;
    }
    sm3_patch_active = 1;
    sm3_level_sink = 0;
    sm3_level_sink_seen = 0;
    sm3_level_write_seen = 0;
    sm3_shadow_logged = 0;
    fprintf(stderr, "[sm3-probe] MEASUREMENT ONLY: 0x00c6e23b 7D 0A -> EB 0A; "
                    "caps unchanged; not shippable\n");
    return 0;
}
