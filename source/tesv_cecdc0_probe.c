#include "tesv_cecdc0_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* f_CD4E40's call to NiDX9TextureBufferData::Create (0x00cecdc0).  The
 * caller pushes two arguments, calls, then does `add esp,8`; EAX is copied to
 * EDI and tested before the function returns `al = (eax != 0)`. */
#define SITE1_CALL        0x00cd4e63u
#define SITE1_RET         0x00cd4e68u
#define CECDC0_ENTRY      0x00cecdc0u
#define SITE1_WINDOW      0x26u

/* The second direct caller: `call 0xcecdc0; add esp,8; pop esi; add esp,0x48;
 * ret` with no EAX test.  It is named in the log but never patched. */
#define SITE2_CALL        0x00ce77f8u
#define SITE2_WINDOW      0x0du

static const uint8_t site1_expected[SITE1_WINDOW] = {
    0xe8, 0x58, 0x7f, 0x01, 0x00, /* call 0x00cecdc0 */
    0x83, 0xc4, 0x08,             /* add esp,8 */
    0x83, 0x46, 0x04, 0xff,       /* add [esi+4],-1 */
    0x8b, 0xf8,                   /* mov edi,eax */
    0x75, 0x0c,                   /* jne 0x00cd4e7f */
    0x8b, 0x06,                   /* mov eax,[esi] */
    0x50, 0x6a, 0x00, 0x56,       /* push eax; push 0; push esi */
    0xff, 0x15, 0x4c, 0xb1, 0x06, 0x01, /* call [0x0106b14c] */
    0x85, 0xff,                   /* test edi,edi */
    0x5f,                         /* pop edi */
    0x0f, 0x95, 0xc0,             /* setne al */
    0x5e,                         /* pop esi */
    0xc2, 0x04, 0x00              /* ret 4 */
};

static const uint8_t site2_expected[SITE2_WINDOW] = {
    0xe8, 0xc3, 0x55, 0x00, 0x00, /* call 0x00cecdc0 */
    0x83, 0xc4, 0x08,             /* add esp,8 */
    0x5e,                         /* pop esi */
    0x83, 0xc4, 0x48,             /* add esp,0x48 */
    0xc3                          /* ret */
};

static const uint8_t site1_patch[5] = {0x33, 0xc0, 0x90, 0x90, 0x90};

static int probe_active;

int tesv_cecdc0_probe_active(void) {
    return probe_active;
}

int tesv_cecdc0_is_primary_return(uint32_t ret) {
    return ret == SITE1_RET;
}

void tesv_cecdc0_probe_reset_for_test(void) {
    probe_active = 0;
}

static void dump_bytes(const char *label, uint32_t address,
                       const uint8_t *expected, const uint8_t *got,
                       size_t count, int readable) {
    fprintf(stderr, "[cecdc0-probe] %s 0x%08lx expected ", label,
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

int tesv_cecdc0_skip_probe(GuestMemory *m) {
    if (probe_active) {
        fprintf(stderr, "[cecdc0-probe] REFUSED: already active; no patch\n");
        return -1;
    }

    const char *env = getenv("TESV_CECDC0_SKIP");
    if (!env || strcmp(env, "1")) return 0; /* default off */

    if (!m) {
        fprintf(stderr, "[cecdc0-probe] REFUSED: no guest memory; no patch\n");
        return -1;
    }

    uint8_t site1[SITE1_WINDOW], site2[SITE2_WINDOW];
    int site1_ok = guest_read(m, SITE1_CALL, site1, sizeof site1) == 0;
    int site2_ok = guest_read(m, SITE2_CALL, site2, sizeof site2) == 0;

    int target_ok = 0;
    uint32_t target = 0;
    if (site1_ok) {
        int32_t rel;
        memcpy(&rel, site1 + 1, sizeof rel);
        target = SITE1_CALL + 5u + (uint32_t)rel;
        target_ok = target == CECDC0_ENTRY;
    }

    /* The caller must test the return value.  EAX is copied to EDI at +0x0c,
     * tested at +0x1c and converted to `setne al` at +0x1f. */
    int null_path_ok = site1_ok &&
        site1[0x0c] == 0x8b && site1[0x0d] == 0xf8 &&
        site1[0x1c] == 0x85 && site1[0x1d] == 0xff &&
        site1[0x1f] == 0x0f && site1[0x20] == 0x95 && site1[0x21] == 0xc0;

    if (!site1_ok || !target_ok || !null_path_ok ||
        memcmp(site1, site1_expected, sizeof site1)) {
        fprintf(stderr, "[cecdc0-probe] REFUSED: call-site bytes/target/NULL "
                        "path mismatch; no patch\n");
        dump_bytes("site1", SITE1_CALL, site1_expected, site1, sizeof site1,
                   site1_ok);
        if (site1_ok)
            fprintf(stderr, "[cecdc0-probe] target 0x%08lx expected 0x%08lx; "
                            "null_path_ok=%d\n",
                    (unsigned long)target, (unsigned long)CECDC0_ENTRY,
                    null_path_ok);
        else
            fprintf(stderr, "[cecdc0-probe] site1 unreadable\n");
        return -1;
    }

    /* Document the second caller without touching it: it discards EAX. */
    if (site2_ok && !memcmp(site2, site2_expected, sizeof site2)) {
        fprintf(stderr, "[cecdc0-probe] NOT PATCHED 0x%08lx: caller discards "
                        "eax (add esp,8; pop esi; add esp,0x48; ret); "
                        "measurement only\n", (unsigned long)SITE2_CALL);
    } else {
        fprintf(stderr, "[cecdc0-probe] NOT PATCHED 0x%08lx: bytes unreadable "
                        "or unexpected; no write\n", (unsigned long)SITE2_CALL);
    }

    if (guest_write(m, SITE1_CALL, site1_patch, sizeof site1_patch)) {
        fprintf(stderr, "[cecdc0-probe] REFUSED: patch write failed; no "
                        "patch\n");
        return -1;
    }

    probe_active = 1;
    fprintf(stderr, "[cecdc0-probe] MEASUREMENT ONLY: 0x00cd4e63 "
                    "E8 58 7F 01 00 -> 33 C0 90 90 90 (xor eax,eax); "
                    "caller add esp,8 keeps the stack balanced; eax=NULL "
                    "reaches the tested failure path; 0x00ce77f8 not patched; "
                    "not shippable\n");
    return 0;
}
