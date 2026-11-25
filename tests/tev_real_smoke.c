/* Host measurement tool: run the TEV translator over extracted shader chunks.
 *
 * Input format (little endian, written by tools/shaders/report.py --dump-chunks):
 *   u32 count
 *   count * { u32 size; u8 code[size] }
 *
 * This tool never embeds game bytes; the file is produced from the local,
 * read-only install into E:\Skyrim_N3DS_build and can be regenerated or
 * deleted at any time.
 *
 * Output: accepted/rejected counts, family histogram, and the top rejection
 * reasons (with the opcode name).  Exit code 0 as long as at least one chunk
 * was read; the numbers are the deliverable, not a pass/fail threshold.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9_tev.h"
#include "d3d9_menu_light.h"

typedef struct {
    char reason[160];
    unsigned count;
} ReasonRow;

static unsigned u32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <chunk-dump.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    unsigned char header[4];
    if (fread(header, 1, 4, f) != 4) { fclose(f); return 2; }
    unsigned count = u32le(header);
    unsigned accepted = 0, rejected = 0;
    unsigned menu_p5 = 0, menu_p120 = 0;
    unsigned fam_blit = 0, fam_tint = 0, fam_textured = 0, fam_constant = 0;
    unsigned stage_hist[8] = {0};
    unsigned tex_hist[5] = {0};
    ReasonRow reasons[24];
    unsigned reason_count = 0;
    memset(reasons, 0, sizeof reasons);

    for (unsigned i = 0; i < count; ++i) {
        unsigned char sizebuf[4];
        if (fread(sizebuf, 1, 4, f) != 4) break;
        unsigned size = u32le(sizebuf);
        if (size < 8 || size > 65536) break;
        unsigned char *bytes = malloc(size);
        if (!bytes) break;
        if (fread(bytes, 1, size, f) != size) { free(bytes); break; }
        /* The translator reads 32-bit words; size is always a multiple of 4. */
        size_t words = size / 4;
        uint32_t *code = malloc(words * sizeof *code);
        if (!code) { free(bytes); break; }
        for (size_t w = 0; w < words; ++w) code[w] = u32le(bytes + 4 * w);
        D9TevProgram program;
        char why[256];
        D9MenuLightMatch menu;
        if (d9_menu_light_match(code, words, &menu, NULL, 0)) {
            if (menu.signature == D9_MENU_LIGHT_P5_MAD_NORMAL) ++menu_p5;
            else if (menu.signature == D9_MENU_LIGHT_P1200005_ADD_NORMAL) ++menu_p120;
        }
        if (d9_tev_translate_pixel(code, words, &program, why, sizeof why) == 0) {
            ++accepted;
            const char *family = d9_tev_family_name(&program);
            if (!strcmp(family, "blit")) ++fam_blit;
            else if (!strcmp(family, "tint")) ++fam_tint;
            else if (!strcmp(family, "constant")) ++fam_constant;
            else ++fam_textured;
            if (program.stages < 8) ++stage_hist[program.stages];
            if (program.textures < 5) ++tex_hist[program.textures];
        } else {
            ++rejected;
            /* Normalise "at word N" so the histogram groups by cause. */
            char key[160];
            snprintf(key, sizeof key, "%s", why);
            char *at = strstr(key, " at word ");
            if (at) at[0] = 0;
            unsigned found = 0;
            for (unsigned r = 0; r < reason_count; ++r) {
                if (!strcmp(reasons[r].reason, key)) { ++reasons[r].count; found = 1; break; }
            }
            if (!found && reason_count < sizeof reasons / sizeof reasons[0]) {
                snprintf(reasons[reason_count].reason, sizeof reasons[reason_count].reason, "%s", key);
                reasons[reason_count].count = 1;
                ++reason_count;
            }
        }
        free(code);
        free(bytes);
    }
    fclose(f);

    printf("TEV translator over %u extracted chunks\n", count);
    printf("  accepted %u (%.1f%%), rejected %u\n", accepted,
           100.0 * (double)accepted / (count ? count : 1), rejected);
    printf("  accepted families: blit=%u tint=%u textured=%u constant=%u\n",
           fam_blit, fam_tint, fam_textured, fam_constant);
    printf("  menu-light signatures: p5_MAD=%u p1200005_ADD=%u\n", menu_p5, menu_p120);
    printf("  stages:");
    for (unsigned s = 0; s < 8; ++s) if (stage_hist[s]) printf(" %u:%u", s, stage_hist[s]);
    printf("\n  textures:");
    for (unsigned t = 0; t < 5; ++t) if (tex_hist[t]) printf(" %u:%u", t, tex_hist[t]);
    printf("\n  rejection reasons:\n");
    for (unsigned r = 0; r < reason_count; ++r)
        printf("    %6u  %s\n", reasons[r].count, reasons[r].reason);
    return count ? 0 : 1;
}
