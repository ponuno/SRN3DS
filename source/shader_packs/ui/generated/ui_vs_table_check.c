/* Compile-only check for the emitted BSA UI PICA VS table. */
#include <stdint.h>

typedef struct {
    const char *id;
    const char *sha256;
    const char *shbin_sha256;
    const uint8_t *bytes;
    unsigned size;
    unsigned instructions;
    unsigned uniform_rows;
    unsigned input_count;
} D9UiVsProgram;

#include "ui_vs_table.inc"

unsigned d9_ui_vs_program_count(void)
{
    return (unsigned)D9_UI_VS_PROGRAM_COUNT;
}
