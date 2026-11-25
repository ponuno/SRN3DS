/* Optional real-inventory measurement for the specialised pipeline registry.
 *
 * Input is a local, derived dump written by tools/pipelines/inventory.py. It
 * contains package/meta/offset/trailer metadata and temporary game bytecode;
 * no game bytes are embedded in this source or committed to the repository.
 * Output is TSV consumed by inventory.py. Counts are measurements, not a claim
 * that a menu/world frame renders.
 */

#include "d3d9_pipeline_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned u16le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static int read_exact(FILE *file, void *data, size_t size) {
    return fread(data, 1, size, file) == size;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pipeline-chunks.bin>\n", argv[0]);
        return 2;
    }
    char why[256];
    if (d9_pipeline_registry_validate(d9_pipeline_registry(), why, sizeof why) < 0) {
        fprintf(stderr, "registry validation: %s\n", why);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    unsigned char magic[8];
    unsigned char count_bytes[4];
    if (!read_exact(file, magic, sizeof magic) || memcmp(magic, "D9PIPE01", 8) ||
        !read_exact(file, count_bytes, sizeof count_bytes)) {
        fprintf(stderr, "invalid pipeline dump header\n");
        fclose(file);
        return 2;
    }
    uint32_t count = u32le(count_bytes);
    uint32_t ps_count = 0, vs_count = 0, parse_failures = 0;

    for (uint32_t record = 0; record < count; ++record) {
        unsigned char header[24];
        if (!read_exact(file, header, sizeof header)) {
            fprintf(stderr, "truncated record %u header\n", record);
            fclose(file);
            return 2;
        }
        uint32_t code_size = u32le(header);
        uint32_t meta = u32le(header + 4);
        uint32_t offset = u32le(header + 8);
        uint32_t trailer_size = u32le(header + 12);
        unsigned package_length = u16le(header + 16);
        unsigned family_length = u16le(header + 18);
        unsigned hash_length = header[20];
        if (code_size < 8u || code_size > 65536u || (code_size & 3u) ||
            !package_length || package_length > 255u ||
            !family_length || family_length > 63u ||
            hash_length != 40u || header[21] || header[22] || header[23]) {
            fprintf(stderr, "invalid record %u lengths\n", record);
            fclose(file);
            return 2;
        }
        char package[256];
        char family[64];
        char program_hash[80];
        unsigned char *bytes = malloc(code_size);
        if (!bytes ||
            !read_exact(file, package, package_length) ||
            !read_exact(file, family, family_length) ||
            !read_exact(file, program_hash, hash_length) ||
            !read_exact(file, bytes, code_size)) {
            fprintf(stderr, "truncated record %u payload\n", record);
            free(bytes);
            fclose(file);
            return 2;
        }
        package[package_length] = 0;
        family[family_length] = 0;
        program_hash[hash_length] = 0;
        /* The version's high half distinguishes ps_3_0 (0xFFFF) from vs_3_0
         * (0xFFFE). */
        int pixel = code_size >= 4u && bytes[2] == 0xFFu && bytes[3] == 0xFFu;
        if (pixel) ++ps_count;
        else ++vs_count;
        printf("SOURCE_ROW\t%s\t%u\t%u\t%u\t%u\t%u\t%s\n",
               family, 1u, (unsigned)pixel, (unsigned)!pixel,
               meta, offset, program_hash);
        if (!pixel) {
            free(bytes);
            continue;
        }

        size_t words = code_size / 4u;
        uint32_t *code = malloc(words * sizeof *code);
        if (!code) {
            free(bytes);
            fclose(file);
            return 2;
        }
        for (size_t i = 0; i < words; ++i)
            code[i] = u32le(bytes + 4u * i);
        D9PipelineShaderView view;
        if (d9_pipeline_parse_ps3(code, words, &view, why, sizeof why) < 0) {
            ++parse_failures;
            printf("PARSE_FAIL\t%s\t%u\t%u\t%s\n", family, meta, offset, why);
            free(code);
            free(bytes);
            continue;
        }
        D9PipelineRequest request;
        memset(&request, 0, sizeof request);
        request.source = D9_PIPELINE_SOURCE_FXP_PS3;
        request.quality = D9_PIPELINE_QUALITY_LDR;
        request.preconditions = UINT32_MAX;
        request.meta = meta;
        request.package_path = package;
        D9PipelineMatch match;
        const D9PipelineDescriptor *selected = d9_pipeline_select(
            D9_PIPELINE_FAMILY_ANY, &view, &request, &match, why, sizeof why);
        if (selected) {
            D9TevProgram plan;
            int built = d9_pipeline_build(selected, &view, &request, &match,
                                          &plan, why, sizeof why) == 0;
            request.preconditions = 0;
            int built_without_pre = d9_pipeline_build(selected, &view, &request, &match,
                                                       &plan, why, sizeof why) == 0;
            printf("MATCH\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%s\n",
                   selected->id, d9_pipeline_family_name(selected->family), family,
                   package, meta, offset, code_size, trailer_size, view.instructions,
                   view.textures, view.constant_count, built, built_without_pre,
                   program_hash);
        }
        free(code);
        free(bytes);
    }
    fclose(file);
    /* SOURCE_ROW is authoritative for aggregation; this line is a compact
     * dataset checksum for humans. */
    printf("DATASET_SUMMARY\t%u\t%u\t%u\t%u\n", count, ps_count, vs_count, parse_failures);
    return parse_failures ? 1 : 0;
}
