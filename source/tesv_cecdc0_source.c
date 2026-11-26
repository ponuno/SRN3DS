#include "tesv_cecdc0_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The exact retail block that reads the uninitialised base local. */
#define BLOCK_ADDR 0x00ced041u
#define BLOCK_LEN  0x15u

static const uint8_t block_expected[BLOCK_LEN] = {
    0x8b, 0x54, 0x24, 0x54,             /* mov edx,[esp+0x54] */
    0x8b, 0x8a, 0x24, 0x06, 0x00, 0x00, /* mov ecx,[edx+0x624] */
    0x8b, 0x54, 0x24, 0x18,             /* mov edx,[esp+0x18] */
    0x8b, 0x0c, 0x8a,                   /* mov ecx,[edx+ecx*4] */
    0x89, 0x4c, 0x24, 0x14              /* mov [esp+0x14],ecx */
};

static int probe_active;
static unsigned refused_logged;

int tesv_cecdc0_source_active(void) {
    return probe_active;
}

void tesv_cecdc0_source_reset_for_test(void) {
    probe_active = 0;
    refused_logged = 0;
}

static void dump_block(const uint8_t *got, int readable) {
    fprintf(stderr, "[cecdc0-source] block 0x%08lx expected ", (unsigned long)BLOCK_ADDR);
    for (size_t i = 0; i < BLOCK_LEN; ++i)
        fprintf(stderr, "%s%02X", i ? " " : "", block_expected[i]);
    fprintf(stderr, " got ");
    if (!readable) {
        fprintf(stderr, "<unreadable>");
    } else {
        for (size_t i = 0; i < BLOCK_LEN; ++i)
            fprintf(stderr, "%s%02X", i ? " " : "", got[i]);
    }
    fputc('\n', stderr);
}

int tesv_cecdc0_source_probe(GuestMemory *m) {
    if (probe_active) {
        fprintf(stderr, "[cecdc0-source] REFUSED: already active; no write\n");
        return -1;
    }

    const char *env = getenv("TESV_CECDC0_SOURCE_FILL");
    if (!env || strcmp(env, "1")) return 0; /* default off */

    if (!m) {
        fprintf(stderr, "[cecdc0-source] REFUSED: no guest memory; no write\n");
        return -1;
    }

    uint8_t got[BLOCK_LEN];
    int readable = guest_read(m, BLOCK_ADDR, got, sizeof got) == 0;
    if (!readable || memcmp(got, block_expected, sizeof got)) {
        fprintf(stderr, "[cecdc0-source] REFUSED: base-local block mismatch; "
                        "no fill\n");
        dump_block(got, readable);
        return -1;
    }

    probe_active = 1;
    fprintf(stderr, "[cecdc0-source] MEASUREMENT ONLY: 0x00ced041 base local "
                    "[esp+0x18] is uninitialised; NULL slot -> real source "
                    "object, original 68-byte copy kept; not shippable\n");
    return 0;
}

static int refuse(const char *why, uint32_t frame, uint32_t index,
                  uint32_t arg, uint32_t base, uint32_t source) {
    if (refused_logged < 16u) {
        ++refused_logged;
        fprintf(stderr, "[cecdc0-source] REFUSED: %s frame=0x%08lx index=%lu "
                        "arg=0x%08lx base=0x%08lx source=0x%08lx; no fill\n",
                why, (unsigned long)frame, (unsigned long)index,
                (unsigned long)arg, (unsigned long)base,
                (unsigned long)source);
    }
    return -1;
}

int tesv_cecdc0_source_fill_slot(GuestMemory *m, uint32_t frame,
                                 uint32_t index, uint32_t *new_base) {
    if (!probe_active) return -1;
    if (!m || !frame || !new_base) return -1;
    if (index >= 0x16u)
        return refuse("index out of range", frame, index, 0, 0, 0);

    uint32_t arg = 0, base = 0, source = 0, source_index = 0;
    if (guest_read32(m, frame + 0x54u, &arg) || !arg)
        return refuse("bad arg local", frame, index, arg, base, source);
    if (guest_read32(m, frame + 0x18u, &base))
        return refuse("unreadable base local", frame, index, arg, base, source);

    uint32_t slot = 0;
    if (base) {
        if (guest_read32(m, base + index * 4u, &slot))
            return refuse("unreadable base slot", frame, index, arg, base,
                          source);
        if (slot) return 0; /* already populated */
    }

    uint32_t value = 0;
    if (!guest_read32(m, arg + 0x604u, &source) && source &&
        !guest_read32(m, source + 0x10u, &source_index) &&
        source_index == index &&
        !guest_read32(m, source + index * 4u, &value) && value) {
        *new_base = source;
        fprintf(stderr, "[cecdc0-source] MEASUREMENT ONLY: frame=0x%08lx "
                        "index=%lu base 0x%08lx slot=0 -> source 0x%08lx "
                        "(source+index*4=0x%08lx); not shippable\n",
                (unsigned long)frame, (unsigned long)index,
                (unsigned long)base, (unsigned long)source,
                (unsigned long)value);
        return 1;
    }

    /* Nothing real in the source object: keep the diagnostics. */
    if (!guest_read32(m, arg + 0x604u, &source) && source) {
        uint32_t dump[16];
        if (!guest_read(m, source, dump, sizeof dump)) {
            fprintf(stderr, "[cecdc0-source] source dump 0x%08lx:",
                    (unsigned long)source);
            for (unsigned i = 0; i < 16; ++i)
                fprintf(stderr, " %08lx", (unsigned long)dump[i]);
            fputc('\n', stderr);
        }
    }
    return refuse("source slot is NULL/unreadable", frame, index, arg, base,
                  source);
}

int tesv_cecdc0_source_decide(GuestMemory *m, uint32_t frame, uint32_t index,
                              uint32_t ebx, uint32_t *new_base,
                              uint32_t *new_desc) {
    uint32_t base = 0;
    int rc = tesv_cecdc0_source_fill_slot(m, frame, index, &base);
    if (rc == 1) {
        *new_base = base;
        return 0; /* fill the base local, then let the original lookup run */
    }
    if (rc == 0) return 0; /* already populated */

    /* The real alternate branch ([ebp+0x70]=0, 0x00ced067) copies the 68-byte
     * descriptor from the existing buffer at [esp+0x10]+0x14 (the same object
     * L_CED05E later loads into ebx).  Keep that engine buffer instead of
     * overwriting [esp+0x14] with the NULL table slot. */
    uint32_t buffer = 0;
    if (m && !guest_read32(m, frame + 0x10u, &buffer) && buffer) {
        uint32_t desc = buffer + 0x14u;
        uint32_t probe = 0;
        if (!guest_read32(m, desc, &probe)) {
            *new_desc = desc;
            fprintf(stderr, "[cecdc0-source] MEASUREMENT ONLY: source slot "
                            "NULL; using existing [esp+0x10]+0x14 buffer "
                            "0x%08lx at frame=0x%08lx index=%lu; original "
                            "68-byte copy kept; not shippable\n",
                    (unsigned long)desc, (unsigned long)frame,
                    (unsigned long)index);
            return 1; /* skip the base[index] overwrite */
        }
    }

    /* The generated call also passes ebx; keep it as a second real buffer. */
    if (ebx) {
        uint32_t desc = ebx + 0x14u;
        uint32_t probe = 0;
        if (m && !guest_read32(m, desc, &probe)) {
            *new_desc = desc;
            fprintf(stderr, "[cecdc0-source] MEASUREMENT ONLY: source slot "
                            "NULL; using existing ebx+0x14 buffer 0x%08lx "
                            "at frame=0x%08lx index=%lu; original 68-byte "
                            "copy kept; not shippable\n",
                    (unsigned long)desc, (unsigned long)frame,
                    (unsigned long)index);
            return 1; /* skip the base[index] overwrite */
        }
    }

    /* Last real value: the f_CE9E00 result already in [esp+0x14]. */
    uint32_t existing = 0;
    if (m && !guest_read32(m, frame + 0x14u, &existing) && existing) {
        *new_desc = existing;
        fprintf(stderr, "[cecdc0-source] MEASUREMENT ONLY: source slot NULL; "
                        "keep existing descriptor 0x%08lx at frame=0x%08lx "
                        "index=%lu; original 68-byte copy kept; not shippable\n",
                (unsigned long)existing, (unsigned long)frame,
                (unsigned long)index);
        return 1; /* skip the base[index] overwrite */
    }
    return -1;
}
