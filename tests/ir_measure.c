/* Host measurement harness for the IR decoder.
 *
 * Input (little-endian):
 *   u32 magic 'IRM1', u32 base, u32 blob_bytes, u32 function_count,
 *   u8  blob[blob_bytes],
 *   function_count * {
 *       u32 entry, u32 function_end, u32 range_count,
 *       range_count * { u32 start, u32 end }        (discovery leaders)
 *   }
 *
 * The harness decodes only addresses that the discovery listed, in address
 * order, and lets a block merge across *contiguous* leaders (which is what the
 * interpreter does, since it decodes to a terminator). A leader covered by the
 * previous block is skipped. Unsupported opcodes are counted, not fatal.
 *
 * This is the source of the bytes/instruction numbers in docs/IR.md; it is not
 * part of the interpreter.
 */
#include "ir_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_code;
static uint32_t g_base, g_size;

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    if (!bytes) return NULL;
    if (addr < g_base || (uint64_t)addr - g_base > g_size ||
        bytes > g_size - (addr - g_base)) {
        fprintf(stderr, "harness: unmapped guest memory at %08lx (%lu bytes)\n",
                (unsigned long)addr, (unsigned long)bytes);
        exit(2);
    }
    return g_code + (addr - g_base);
}

void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "harness: fault at %08lx: %s\n", (unsigned long)addr, what);
    exit(2);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

typedef struct { uint32_t start, end; } Range;

static int range_cmp(const void *a, const void *b) {
    const Range *x = (const Range *)a, *y = (const Range *)b;
    return x->start < y->start ? -1 : x->start > y->start;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s input.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 2; }
    fclose(f);

    if (sz < 16 || rd32(buf) != 0x49524d31u) { fprintf(stderr, "bad input\n"); return 2; }
    uint32_t base = rd32(buf + 4), blob = rd32(buf + 8), nfunc = rd32(buf + 12);
    uint32_t off = 16;
    if ((size_t)off + blob > (size_t)sz) { fprintf(stderr, "truncated blob\n"); return 2; }

    g_base = base;
    g_size = blob;
    g_code = (uint8_t *)calloc(1, blob);
    if (!g_code) { fprintf(stderr, "out of memory\n"); return 2; }
    memcpy(g_code, buf + off, blob);
    off += blob;
    uint64_t total_insns = 0, total_bytes = 0, total_blocks = 0, failed_funcs = 0;
    uint64_t failed_insns = 0, skipped_leaders = 0, funcs_ok = 0, failed_blocks = 0;
    char first_error[128] = "";

    for (uint32_t fi = 0; fi < nfunc; ++fi) {
        if ((size_t)off + 12 > (size_t)sz) { fprintf(stderr, "truncated function\n"); return 2; }
        uint32_t entry = rd32(buf + off);
        uint32_t fend = rd32(buf + off + 4);
        uint32_t nr = rd32(buf + off + 8);
        off += 12;
        if ((size_t)off + (size_t)nr * 8 > (size_t)sz) { fprintf(stderr, "truncated ranges\n"); return 2; }
        Range *rs = (Range *)malloc((size_t)nr * sizeof *rs);
        if (!rs) return 2;
        for (uint32_t k = 0; k < nr; ++k) {
            rs[k].start = rd32(buf + off + k * 8);
            rs[k].end = rd32(buf + off + k * 8 + 4);
        }
        off += nr * 8;
        qsort(rs, nr, sizeof *rs, range_cmp);

        uint32_t last_end = 0;
        int failed = 0;
        char ferr[64] = "";
        for (uint32_t k = 0; k < nr; ++k) {
            uint32_t start = rs[k].start;
            if (start < last_end) { skipped_leaders++; continue; }
            /* The interpreter's frame limit is the function end (max of the
             * discovery ranges), not the end of this leader's range. Following
             * that is the only way to measure what it will actually decode;
             * a straight-line split is one block. */
            uint32_t limit = fend;
            if (limit > fend) limit = fend;

            IrBlockInfo info;
            size_t cap = (size_t)(limit - start) * 4 + 32;
            uint32_t *words = (uint32_t *)malloc(cap * 4);
            if (!words) return 2;
            size_t r = n3ds_ir_encode_block(NULL, start, limit, words, cap, &info, 1);
            free(words);
            if (!r || info.end_ip <= start) {
                if (!first_error[0])
                    snprintf(first_error, sizeof first_error, "%s", info.error);
                if (!ferr[0]) snprintf(ferr, sizeof ferr, "%s", info.error);
                failed = 1;
                failed_blocks++;
                last_end = start + 1;   /* keep scanning the rest */
                continue;
            }
            total_insns += info.insn_count;
            total_bytes += info.ir_bytes;
            total_blocks++;
            last_end = info.end_ip;
        }
        free(rs);
        if (failed) {
            failed_funcs++;
            printf("FAILED 0x%08x %s\n", entry, ferr);
        } else {
            funcs_ok++;
        }
    }

    printf("{\"base\":%u,\"blob\":%u,\"functions\":%u,\"functions_ok\":%llu,"
           "\"blocks\":%llu,\"insns\":%llu,\"ir_bytes\":%llu,"
           "\"failed_functions\":%llu,\"failed_blocks\":%llu,"
           "\"skipped_leaders\":%llu,\"first_error\":\"%s\"}\n",
           base, blob, nfunc, (unsigned long long)funcs_ok, (unsigned long long)total_blocks,
           (unsigned long long)total_insns, (unsigned long long)total_bytes,
           (unsigned long long)failed_funcs, (unsigned long long)failed_blocks,
           (unsigned long long)skipped_leaders, first_error);
    free(g_code);
    return 0;
}
