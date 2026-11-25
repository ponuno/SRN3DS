#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t image_base, image_size, entry_rva;
    uint16_t machine, sections;
} PeInfo;

/* Returns 0 for a readable 32-bit x86 PE image. Does not execute it. */
int pe_probe(const char *path, PeInfo *info);
int guest_memory_test(void);
#endif
