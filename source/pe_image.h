#pragma once
#include "guest.h"

typedef struct {
    uint32_t base, preferred_base, size, entry, import_rva, import_size, export_rva, export_size;
    uint32_t tls_rva,tls_size;
    uint16_t sections;
} GuestImage;

enum { PE_OK=0, PE_IO=-1, PE_FORMAT=-2, PE_MEMORY=-3, PE_RELOCATION=-4 };
/* Map file headers and sections into non-executable guest data. */
int pe_image_load(GuestMemory *m, const char *path, uint32_t want_base, GuestImage *image);
const char *pe_error(int code);
