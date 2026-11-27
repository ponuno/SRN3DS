/* A straight-line block longer than the uint8 IR block count.
 *
 * The IR block header and the container block entry store the x86 instruction
 * count in one byte, so the decoder stops a block at 255 instructions.  Some
 * compiler output has no branch for longer than that: TESV's game loop at
 * 0x429872 is a 333-instruction x87 run between a call and a jmp, which used to
 * fail as `IR: block has no terminator at 00429872` even though the code is
 * perfectly decodable.
 *
 * The decoder now closes such a block with a synthetic fall-through jump to
 * end_ip and decodes the remainder as the next block.  This test pins both
 * halves: the decode contract (split flag, end_ip, next block) and the
 * interpreter running the whole run to the same result as the straight-line
 * code.  All code is synthetic; no game files are involved.  */
#include "n3ds_lift_runtime.h"
#include "ir_core.h"
#include "guest.h"
#include <stdio.h>
#include <string.h>

#define BASE        0x50000000u
#define STACK_TOP   0x50010000u
#define ADDS        300u
#define RET_AT      (BASE + ADDS * 3u)     /* `ret` past the adds */

static GuestMemory mem;

static void dummy_fn(CPU *c) { (void)c; }

static int fail(const char *what) {
    printf("ir long block smoke: FAIL: %s\n", what);
    return 1;
}

/* Encode [start,end) and return the block info.  buf must hold 4096 words. */
static size_t encode(CPU *c, uint32_t start, uint32_t end, IrBlockInfo *info) {
    static uint32_t buf[4096];
    memset(info, 0, sizeof *info);
    return n3ds_ir_encode_block(c, start, end, buf, 4096, info, 0);
}

int main(void) {
    uint8_t img[ADDS * 3u + 1];
    for (unsigned i = 0; i < ADDS; ++i) {
        img[i * 3 + 0] = 0x83;   /* add eax, 1 */
        img[i * 3 + 1] = 0xc0;
        img[i * 3 + 2] = 0x01;
    }
    img[ADDS * 3] = 0xc3;

    /* A 254-instruction run plus ret fits one block and must not split. */
    uint8_t img2[254 * 3 + 1];
    for (unsigned i = 0; i < 254; ++i) {
        img2[i * 3 + 0] = 0x83;
        img2[i * 3 + 1] = 0xc0;
        img2[i * 3 + 2] = 0x01;
    }
    img2[254 * 3] = 0xc3;

    guest_init(&mem, 64u << 20);
    if (guest_map(&mem, BASE, 0x2000) ||
        guest_map(&mem, STACK_TOP - 0x1000, 0x1000))
        return fail("map");
    if (guest_write(&mem, BASE, img, sizeof img)) return fail("write image");
    if (guest_write(&mem, BASE + 0x1000, img2, sizeof img2)) return fail("write short image");

    static const FnEntry aot[] = {{0x50008000u, dummy_fn}};
    if (n3ds_lift_bind(&mem, aot, 1)) return fail("bind");
    static const IrFnEntry irtab[] = {{BASE, RET_AT + 1}};
    if (n3ds_ir_bind(irtab, 1)) return fail("ir bind");

    CPU c;
    n3ds_lift_init_cpu(&c, STACK_TOP, 0);

    /* --- decoder contract: cap split, then continuation block --- */
    IrBlockInfo info;
    if (!encode(&c, BASE, RET_AT + 1, &info)) return fail("encode first block");
    if (info.insn_count != 255 || !info.split || info.term_op != IR_OP_JMP)
        return fail("block was not closed by the cap split");
    if (info.end_ip != BASE + 255u * 3u) return fail("split end_ip");
    uint32_t mid = info.end_ip;
    if (!encode(&c, mid, RET_AT + 1, &info)) return fail("encode continuation");
    if (info.split || info.term_op != IR_OP_RET) return fail("continuation terminator");
    if (info.end_ip != RET_AT + 1) return fail("continuation end_ip");
    if (info.insn_count != ADDS - 255u + 1u) return fail("continuation count");

    /* A 254-instruction run plus ret fits one block and must not split. */
    if (!encode(&c, BASE + 0x1000, BASE + 0x1000 + 254u * 3u + 1u, &info))
        return fail("encode short block");
    if (info.split || info.insn_count != 255u || info.term_op != IR_OP_RET)
        return fail("254 adds + ret was split");

    /* --- interpreter: the same bytes must run to the same result --- */
    CPU r;
    n3ds_lift_init_cpu(&r, STACK_TOP, 0);
    uint32_t result = 0;
    if (n3ds_ir_invoke(&r, BASE, RET_AT + 1, NULL, 0, &result))
        return fail("invoke");
    if (result != ADDS) return fail("result");

    printf("ir long block smoke: PASS (%u adds through a %u+%u block split)\n",
           ADDS, 255u, ADDS - 255u + 1u);
    return 0;
}
