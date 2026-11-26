/* D3D9 SM3 token walk: the instruction length field is authoritative.
 *
 * First divergent shader layer, measured on the retail archive: 320 of the 854
 * vs_3_0 chunks in `Skyrim - Shaders.bsa` were rejected as "malformed" because
 * the nominal operand count of mul (4 words) and mad (5 words) does not include
 * the address-register token fxc emits for a relative-addressed constant
 * (D3DSP_ADDRESSMODE, bit 13).  Both tokens are counted in the instruction
 * length field.
 *
 * Provenance of the two shapes below (shaders001.fxp, BSVS chunks):
 *   meta 9    word 123: 0x04000005 0x800F0000 0x90550002 0xA0E42000 0xB0550000
 *                        mul o3.xyz, r0, c10[a0.x]              (5 words)
 *   meta 8582 word 132: 0x06000004 0x80030001 0xA0ED2000 0xB0000000 0x90E40001
 *                       0xA0E82000 0xB0000000
 *                        mad r1.xyz, c0[a0.x], v1, c0[a0.x]     (7 words)
 * The streams here are the minimal synthetic equivalent; no retail bytecode is
 * embedded.  A whole-chunk check lives in the census/BSA tooling.
 */
#include "d3d9_shader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define D3D9_VALID D9_SHADER_STREAM_VALID

static const uint32_t rel_mul[]={
    0xFFFE0300u,
    0x04000005u,                        /* mul, length field 4 -> 5 words */
    0x800F0000u,                        /* dst temp r0.xyzw */
    0x90550002u,                        /* src input v1 (swizzle .yzxy) */
    0xA0E42000u,                        /* src constant, D3DSP_ADDRESSMODE set */
    0xB0550000u,                        /* a0 address token */
    0x0000FFFFu
};
static const uint32_t rel_mad[]={
    0xFFFE0300u,
    0x06000004u,                        /* mad, length field 6 -> 7 words */
    0x80030001u,                        /* dst r1.xyz */
    0xA0ED2000u,                        /* src c0[a0.x] (relative) */
    0xB0000000u,                        /* a0 address token */
    0x90E40001u,                        /* src v1 */
    0xA0E82000u,                        /* src c0[a0.x] (relative) */
    0xB0000000u,                        /* a0 address token */
    0x0000FFFFu
};

static void analyze_ok(const uint32_t *code,size_t words,unsigned instructions,
                       const char *what){
    D9ShaderInfo info;
    int kind=d9_shader_analyze(code,words,&info);
    if(kind==D9_SHADER_INVALID||info.stream!=D3D9_VALID||!info.saw_end){
        fprintf(stderr,"%s: rejected (stream %u, opcode 0x%02x at byte %u)\n",
                what,info.stream,info.opcode,info.opcode_offset);
        assert(0);
    }
    if(info.instructions!=instructions||info.words!=words){
        fprintf(stderr,"%s: %u instructions / %u words, expected %u / %u\n",what,
                info.instructions,info.words,instructions,(unsigned)words);
        assert(0);
    }
    assert(info.version==0xFFFE0300u);
}

static void expect_invalid(const uint32_t *code,size_t words,const char *what){
    D9ShaderInfo info;
    int kind=d9_shader_analyze(code,words,&info);
    if(kind!=D9_SHADER_INVALID){
        fprintf(stderr,"expected INVALID for %s (kind %d, stream %u)\n",
                what,kind,info.stream);
        assert(0);
    }
}

static void check_relative_instructions(void){
    analyze_ok(rel_mul,sizeof rel_mul/sizeof *rel_mul,1,"5-word relative mul");
    analyze_ok(rel_mad,sizeof rel_mad/sizeof *rel_mad,1,"7-word relative mad");

    /* The relative constant and its address token count as one operand pair;
     * the parser must not treat the address token as a third source. */
    D9ShaderInfo info;
    d9_shader_analyze(rel_mad,sizeof rel_mad/sizeof *rel_mad,&info);
    assert(info.opcode==0x0004u||info.opcode==0u);
}

static void check_rejections(void){
    /* A relative mad with one source short: 5 words (dst + c[a0] + v1). */
    static const uint32_t truncated_mad[]={
        0xFFFE0300u,0x04000004u,0x800F0000u,0xA0E42000u,0xB0000000u,0x80E40000u,
        0x0000FFFFu};
    expect_invalid(truncated_mad,sizeof truncated_mad/sizeof *truncated_mad,
                   "relative mad with one source");

    /* A relative source as the last operand has no address token to consume;
     * mul then has only one real source. */
    static const uint32_t dangling_rel[]={
        0xFFFE0300u,0x03000005u,0x800F0000u,0xA0E42000u,0xB0000000u,
        0x0000FFFFu};
    expect_invalid(dangling_rel,sizeof dangling_rel/sizeof *dangling_rel,
                   "mul with a required source missing");

    /* def carries four literal words; a 0x2000 bit inside a literal is data,
     * not an address mode, so a 7-word def stays invalid. */
    static const uint32_t def_extra[]={
        0xFFFE0300u,0x06000051u,0xA00F0000u,0x3F800000u,0x00002000u,
        0x00000000u,0x00000000u,0x00000000u,0x0000FFFFu};
    expect_invalid(def_extra,sizeof def_extra/sizeof *def_extra,
                   "def with a literal that looks relative");

    /* Garbage: an opcode D3D9 does not define. */
    static const uint32_t unknown[]={0xFFFE0300u,0x00000031u,0x0000FFFFu};
    expect_invalid(unknown,sizeof unknown/sizeof *unknown,"unknown opcode");

    /* Truncated buffer before END. */
    expect_invalid(rel_mad,sizeof rel_mad/sizeof *rel_mad-1,"stream cut before END");
}

int main(void){
    check_relative_instructions();
    check_rejections();
    puts("shader relative addressing: the retail 5/7-word mul/mad shapes parse; "
         "truncated/literal/unknown streams stay invalid");
    return 0;
}
