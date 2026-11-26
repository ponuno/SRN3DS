/* sm3_dump: host-only canonical SM3 IR dumper.
 *
 * Reads the project's chunk-dump format (little endian):
 *     u32 count; count * { u32 size; u8 bytes[size]; }
 * parses every bytecode blob with the vendored MojoShader `d3d` profile and
 * writes a framed binary stream:
 *     "SM3IR1\0" then count * { u32 index; u32 status; u32 instructions;
 *                              u32 text_len; u8 text[text_len]; }
 * where status is 0 ok / 1 parse error and `text` is the canonical d3d
 * disassembly (or the MojoShader error string).
 *
 * This tool never runs on the target. It stores derived text only; the
 * retail bytecode is read from the local install and is not copied into the
 * repository.
 *
 * Build: tools/build_sm3_dump.ps1 (see tools/mojoshader/README.md).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mojoshader.h"

static uint32_t read_u32(FILE *f, int *ok) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) { *ok = 0; return 0; }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static void write_u32(FILE *f, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <chunks.bin> <ir.bin> [--limit N]\n", argv[0]);
        return 2;
    }
    unsigned limit = 0;
    if (argc >= 5 && strcmp(argv[3], "--limit") == 0)
        limit = (unsigned)strtoul(argv[4], NULL, 10);

    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot open %s\n", argv[2]); fclose(in); return 2; }

    int ok = 1;
    uint32_t count = read_u32(in, &ok);
    if (!ok) { fprintf(stderr, "short chunk header\n"); return 2; }
    if (limit && limit < count) count = limit;

    fwrite("SM3IR1\0", 1, 7, out);
    write_u32(out, count);

    unsigned parsed = 0, failed = 0;
    unsigned long long total_instructions = 0;
    unsigned char *blob = NULL;
    size_t blob_cap = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = read_u32(in, &ok);
        if (!ok) { fprintf(stderr, "short record at %u\n", i); break; }
        if (size > blob_cap) {
            unsigned char *grown = (unsigned char *)realloc(blob, size ? size : 1);
            if (!grown) { fprintf(stderr, "out of memory\n"); break; }
            blob = grown;
            blob_cap = size;
        }
        if (size && fread(blob, 1, size, in) != size) {
            fprintf(stderr, "short payload at %u\n", i);
            break;
        }

        const MOJOSHADER_parseData *pd =
            MOJOSHADER_parse("d3d", NULL, blob, (unsigned)size, NULL, 0,
                             NULL, 0, NULL, NULL, NULL);
        uint32_t status = 0;
        unsigned instructions = 0;
        const char *text = NULL;
        size_t text_len = 0;
        if (!pd) {
            status = 1;
            text = "MOJOSHADER_parse returned NULL";
        } else if (pd->error_count > 0 || pd->output == NULL) {
            status = 1;
            text = pd->error_count > 0 && pd->errors ? pd->errors[0].error
                                                     : "no output";
        } else {
            instructions = (unsigned)pd->instruction_count;
            text = pd->output;
            total_instructions += instructions;
            ++parsed;
        }
        if (status) ++failed;
        if (!text) text = "";
        text_len = strlen(text);

        write_u32(out, i);
        write_u32(out, status);
        write_u32(out, instructions);
        write_u32(out, (uint32_t)text_len);
        fwrite(text, 1, text_len, out);

        if (pd) MOJOSHADER_freeParseData(pd);
    }

    fprintf(stderr, "chunks=%u parsed_ok=%u parse_fail=%u total_instructions=%llu\n",
            count, parsed, failed, total_instructions);
    free(blob);
    fclose(in);
    fclose(out);
    return failed ? 1 : 0;
}
