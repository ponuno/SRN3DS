#include "probe.h"
#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int pe_probe(const char *path, PeInfo *info) {
    unsigned char dos[64], pe[24], opt[60];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int rc = -2;
    if (fread(dos, 1, sizeof dos, f) != sizeof dos || le16(dos) != 0x5a4d) goto done;
    uint32_t offset = le32(dos + 60);
    if (offset < 64 || offset > INT32_MAX || fseek(f, (long)offset, SEEK_SET) != 0) goto done;
    if (fread(pe, 1, sizeof pe, f) != sizeof pe || memcmp(pe, "PE\0\0", 4)) goto done;
    if (le16(pe + 4) != 0x14c || le16(pe + 20) < sizeof opt) goto done;
    if (fread(opt, 1, sizeof opt, f) != sizeof opt || le16(opt) != 0x10b) goto done;
    *info = (PeInfo){.machine = le16(pe+4), .sections = le16(pe+6),
        .image_base = le32(opt+28), .image_size = le32(opt+56), .entry_rva = le32(opt+16)};
    if (!info->sections || !info->image_size || info->entry_rva >= info->image_size ||
        (uint64_t)info->image_base + info->image_size > (UINT64_C(1)<<32)) goto done;
    rc = 0;
done:
    fclose(f);
    return rc;
}

int guest_memory_test(void) {
    GuestMemory m;
    guest_init(&m,8192);
    int result = -1;
    if (guest_map(&m,0x00400000,4096) || guest_map(&m,0x20000000,4096)) goto done;
    unsigned char *a = guest_ptr(&m,0x00400000,4);
    unsigned char *b = guest_ptr(&m,0x20000001,4);
    if (!a || !b) goto done;
    memcpy(a, "TESV", 4);
    memcpy(b, "HEAP", 4);
    if (memcmp(a,"TESV",4) || memcmp(b,"HEAP",4)) goto done;
    if (guest_ptr(&m,0x00400fff,2) || guest_ptr(&m,0xffffffff,4) ||
        guest_ptr(&m,0x10000000,1) || guest_ptr(&m,0x00400000,0)) goto done;
    if (!guest_map(&m,0x10000000,1) || guest_unmap(&m,0x00400000) || m.used!=4096 ||
        !guest_map(&m,0x20000001,2) || !guest_map(&m,0xffffffff,2) ||
        guest_map(&m,0x10000000,4096)) goto done;
    result = 0;
done:
    guest_destroy(&m);
    return result;
}
