#include "d3d9_bc.h"
#include "d3d9_core.h"
#include "d3d9_shader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void alpha_indices(uint8_t *b){uint64_t bits=0;for(unsigned i=0;i<16;++i)bits|=(uint64_t)(i%8)<<(3*i);for(unsigned i=0;i<6;++i)b[2+i]=(uint8_t)(bits>>(8*i));}
static void decoder(void){uint8_t rgba[64],b[16]={0};
    const uint8_t opaque[]={0x00,0xf8,0x1f,0x00,0xe4,0xe4,0xe4,0xe4};
    assert(!d9_bc_decode_block(D9_DXT1,opaque,8,rgba));
    assert(rgba[0]==255&&rgba[1]==0&&rgba[2]==0&&rgba[3]==255);
    assert(rgba[4]==0&&rgba[6]==255&&rgba[7]==255);
    assert(rgba[8]==170&&rgba[10]==85&&rgba[12]==85&&rgba[14]==170);
    const uint8_t transparent[]={0x1f,0x00,0x00,0xf8,0xe4,0,0,0};
    assert(!d9_bc_decode_block(D9_DXT1,transparent,8,rgba)&&rgba[15]==0&&rgba[8]==127&&rgba[10]==127);
    b[0]=0xa3;memcpy(b+8,transparent,8);
    assert(!d9_bc_decode_block(D9_DXT3,b,16,rgba)&&rgba[3]==51&&rgba[7]==170&&rgba[12]==170&&rgba[14]==85);
    b[0]=255;b[1]=0;alpha_indices(b);memcpy(b+8,opaque,8);
    assert(!d9_bc_decode_block(D9_DXT5,b,16,rgba)&&rgba[3]==255&&rgba[7]==0&&rgba[11]==218&&rgba[31]==36);
    b[0]=0;b[1]=100;alpha_indices(b);
    assert(!d9_bc_decode_block(D9_DXT5,b,16,rgba)&&rgba[11]==20&&rgba[27]==0&&rgba[31]==255);
    assert(d9_bc_decode_block(D9_DXT5,b,15,rgba)==D9_INVALID);
    uint8_t blocks[32],pica[256];for(unsigned i=0;i<4;++i)memcpy(blocks+i*8,opaque,8);
    assert(!d9_bc_to_pica(D9_DXT1,pica,sizeof pica,blocks,sizeof blocks,8,8,16));
    /* Top-left pixel has RGBA red; PICA Morton(0,7)=42, stored ABGR. */
    assert(pica[42*4]==255&&pica[42*4+1]==0&&pica[42*4+2]==0&&pica[42*4+3]==255);
    assert(d9_bc_to_pica(D9_DXT1,pica,sizeof pica,blocks,sizeof blocks-1,8,8,16)==D9_INVALID);
    size_t pitch,bytes;assert(!d9_texture_layout(64,64,D9_DXT1,&pitch,&bytes)&&pitch==128&&bytes==2048);
    assert(!d9_texture_layout(64,64,D9_DXT3,&pitch,&bytes)&&pitch==256&&bytes==4096);
    assert(!d9_texture_layout(64,64,D9_DXT5,&pitch,&bytes)&&pitch==256&&bytes==4096);
}

static void show(const char *text){puts(text);}

/* Hand-built SM3/edge-case token streams. Offsets are bytes from the start of
 * the version token, matching the diagnostic strings. */
static void shader_diagnostics(void){
    D9ShaderInfo i;D9ShaderCensus c;char text[192];

    /* vs_3_0: dcl_position v0; mov r0,v0; add r0,r0,c0; mad r0,r0,c1,c2 */
    static const uint32_t vs3[]={0xfffe0300,
        0x0200001f,0x80000000,0x900f0000,
        0x02000001,0xc00f0000,0x90e40000,
        0x03000002,0xc00f0000,0xc00f0000,0xa0e40000,
        0x04000004,0xc00f0000,0xc00f0000,0xa0e40001,0xa0e40002,
        0x0000ffff};
    assert(d9_shader_analyze(vs3,sizeof vs3/sizeof *vs3,&i)==D9_SHADER_UNKNOWN);
    assert(i.vertex&&i.major==3&&i.minor==0&&i.version==0xfffe0300);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.saw_end&&!i.bad_version&&!i.unknown_opcode&&!i.bad_length);
    assert(i.instructions==4&&i.words==17&&i.kind==D9_SHADER_UNKNOWN);
    assert(i.first_opcode==31&&i.first_opcode_offset==4);           /* dcl */
    assert(i.opcode==2&&i.opcode_offset==28);                       /* add */
    assert(!strcmp(d9_shader_opcode_name(i.opcode),"add"));
    assert(d9_shader_version_name(&i,text,sizeof text)==6&&!strcmp(text,"vs_3_0"));
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"vs_3_0: unsupported (stream valid, 4 instructions); first unsupported opcode 'add' at byte 28"));
    show(text);

    /* ps_3_0: dcl t0.xy; dcl_2d s0; texld r0,t0,s0; mov oC0,r0.
     * Every opcode is in the SM2 template subset, only the version differs. */
    static const uint32_t ps3[]={0xffff0300,
        0x0200001f,0x80000000,0xb0030000,
        0x0200001f,0x90000000,0xa00f0800,
        0x03000042,0x800f0000,0xb0e40000,0xa0e40800,
        0x02000001,0x800f0800,0x80e40000,
        0x0000ffff};
    assert(d9_shader_analyze(ps3,sizeof ps3/sizeof *ps3,&i)==D9_SHADER_UNKNOWN);
    assert(!i.vertex&&i.major==3&&i.minor==0&&i.instructions==4&&i.words==15);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.saw_end);
    assert(i.first_opcode==31&&i.first_opcode_offset==4);
    assert(i.opcode==31&&i.opcode_offset==4);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_3_0: unsupported (stream valid, 4 instructions); TEV rejected: dcl of register type 3 is outside the TEV subset"));
    show(text);

    /* if/endif block: vs_3_0 dcl; mov; if c0.x; mov; endif; mov oPos. */
    static const uint32_t vs3_if[]={0xfffe0300,
        0x0200001f,0x80000000,0x900f0000,
        0x02000001,0x800f0000,0x90e40000,
        0x01000028,0xa0e40000,
        0x02000001,0x800f0001,0x80e40000,
        0x0000002b,
        0x02000001,0xc00f0000,0x80e40001,
        0x0000ffff};
    assert(d9_shader_analyze(vs3_if,sizeof vs3_if/sizeof *vs3_if,&i)==D9_SHADER_UNKNOWN);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.instructions==6);
    assert(i.opcode==40&&i.opcode_offset==28&&!strcmp(d9_shader_opcode_name(i.opcode),"if"));
    d9_shader_census_scan(vs3_if,sizeof vs3_if/sizeof *vs3_if,&i,&c);
    assert(c.instructions==6&&c.opcode[31]==1&&c.opcode[1]==3&&c.opcode[40]==1&&c.opcode[43]==1&&c.outside==0);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"vs_3_0: unsupported (stream valid, 6 instructions); first unsupported opcode 'if' at byte 28"));
    show(text);

    /* loop/endloop block: offset 16 is 'loop', 24 'mov', 36 'endloop'. */
    static const uint32_t vs3_loop[]={0xfffe0300,
        0x0200001f,0x80000000,0x900f0000,
        0x0100001b,0xa0e40000,
        0x02000001,0x800f0000,0x90e40000,
        0x0000001d,
        0x02000001,0xc00f0000,0x80e40000,
        0x0000ffff};
    assert(d9_shader_analyze(vs3_loop,sizeof vs3_loop/sizeof *vs3_loop,&i)==D9_SHADER_UNKNOWN);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.instructions==5&&i.opcode==27&&i.opcode_offset==16);
    d9_shader_census_scan(vs3_loop,sizeof vs3_loop/sizeof *vs3_loop,&i,&c);
    assert(c.instructions==5&&c.opcode[31]==1&&c.opcode[27]==1&&c.opcode[29]==1&&c.opcode[1]==2);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"vs_3_0: unsupported (stream valid, 5 instructions); first unsupported opcode 'loop' at byte 16"));
    show(text);

    /* Truncated mid-instruction: mad claims 5 words, only 4 are present. */
    static const uint32_t truncated[]={0xfffe0300,
        0x0200001f,0x80000000,0x900f0000,
        0x04000004,0xc00f0000,0xc00f0000,0xa0e40000};
    assert(d9_shader_analyze(truncated,sizeof truncated/sizeof *truncated,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_TRUNCATED&&!i.saw_end&&i.bad_version==0);
    assert(i.opcode==4&&i.opcode_offset==16&&i.need_words==5&&i.have_words==4);
    assert(i.instructions==1);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"vs_3_0: rejected (stream truncated); opcode 'mad' at byte 16 needs 5 words, 4 present"));
    show(text);

    /* Complete instructions but no END token. */
    assert(d9_shader_analyze(d9_demo_ps,d9_demo_ps_words-1,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_TRUNCATED&&!i.saw_end&&i.instructions==4&&i.opcode_offset==0);
    assert(i.kind==D9_SHADER_INVALID);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: rejected (stream truncated); no END token after 4 instructions"));
    show(text);

    /* Bogus version token. */
    static const uint32_t bogus[]={0x12345678,0x0000ffff};
    assert(d9_shader_analyze(bogus,sizeof bogus/sizeof *bogus,&i)==D9_SHADER_INVALID);
    assert(i.bad_version&&i.stream==D9_SHADER_STREAM_INVALID&&!i.saw_end&&!i.vertex);
    assert(i.version==0x12345678&&i.major==0x56&&i.minor==0x78&&i.instructions==0);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"invalid version token 0x12345678 (unknown); expected vs_2_0, vs_3_0, ps_2_0 or ps_3_0"));
    show(text);

    /* SM1 is recognised but rejected: it is not an SM2/SM3 template. */
    static const uint32_t vs1[]={0xfffe0101,0x0000ffff};
    assert(d9_shader_analyze(vs1,sizeof vs1/sizeof *vs1,&i)==D9_SHADER_INVALID);
    assert(i.bad_version&&i.major==1&&i.minor==1);
    d9_shader_version_name(&i,text,sizeof text);
    assert(!strcmp(text,"vs_1_1"));
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"invalid version token 0xfffe0101 (vs_1_1); expected vs_2_0, vs_3_0, ps_2_0 or ps_3_0"));
    show(text);

    /* Undefined opcode 49 in an otherwise valid ps_2_0 stream. */
    static const uint32_t unknown_op[]={0xffff0200,0x00000031,0x0000ffff};
    assert(d9_shader_analyze(unknown_op,sizeof unknown_op/sizeof *unknown_op,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_INVALID&&i.unknown_opcode&&i.opcode==0x31&&i.opcode_offset==4);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: rejected (stream invalid); unknown opcode 0x0031 at byte 4"));
    show(text);

    /* mov with a 1-word length field (must be 3 words). */
    static const uint32_t bad_len[]={0xffff0200,0x00000001,0x0000ffff};
    assert(d9_shader_analyze(bad_len,sizeof bad_len/sizeof *bad_len,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_INVALID&&i.bad_length&&i.opcode==1&&i.opcode_offset==4);
    assert(i.need_words==3&&i.have_words==1);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: rejected (stream invalid); opcode 'mov' at byte 4 has length 1, expected 3 words"));
    show(text);

    /* A too-long mov (length field 4, only 3 words allowed). */
    static const uint32_t long_len[]={0xffff0200,0x03000001,0x800f0000,0x80e40000,0x80e40001,0x0000ffff};
    assert(d9_shader_analyze(long_len,sizeof long_len/sizeof *long_len,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_INVALID&&i.bad_length&&i.opcode==1);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: rejected (stream invalid); opcode 'mov' at byte 4 has length 4, expected 3 words"));
    show(text);

    /* PHASE (0xfffd) is a known name but not an SM2/SM3 instruction. */
    static const uint32_t phase[]={0xffff0200,0x0000fffd,0x0000ffff};
    assert(d9_shader_analyze(phase,sizeof phase/sizeof *phase,&i)==D9_SHADER_INVALID);
    assert(i.stream==D9_SHADER_STREAM_INVALID&&i.unknown_opcode&&i.opcode==0xfffd);
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: rejected (stream invalid); opcode 'phase' at byte 4 is not an SM2/SM3 instruction"));
    show(text);

    /* Histogram and batch merge. */
    assert(d9_shader_census_scan(vs3,sizeof vs3/sizeof *vs3,&i,&c)==D9_SHADER_UNKNOWN);
    assert(c.instructions==4&&c.opcode[31]==1&&c.opcode[1]==1&&c.opcode[2]==1&&c.opcode[4]==1&&c.outside==0);
    D9ShaderCensus c2;assert(d9_shader_census_scan(ps3,sizeof ps3/sizeof *ps3,&i,&c2)==D9_SHADER_UNKNOWN);
    d9_shader_census_merge(&c,&c2);
    assert(c.instructions==8&&c.opcode[31]==3&&c.opcode[1]==2&&c.opcode[66]==1&&c.opcode[2]==1);
    d9_shader_census_clear(&c2);assert(c2.instructions==0&&c2.opcode[31]==0&&c2.outside==0);

    assert(!strcmp(d9_shader_opcode_name(31),"dcl"));
    assert(!strcmp(d9_shader_opcode_name(66),"texld"));
    assert(!strcmp(d9_shader_opcode_name(4),"mad"));
    assert(!strcmp(d9_shader_opcode_name(49),"unknown"));
    assert(!strcmp(d9_shader_opcode_name(0xfffd),"phase"));
    assert(d9_shader_analyze(NULL,0,&i)==D9_SHADER_INVALID&&i.bad_version);
}

static void shaders(void){D9ShaderInfo i;char text[64];
    /* The two exact SM2 templates must keep translating to their old kinds. */
    assert(d9_shader_analyze(d9_demo_vs,d9_demo_vs_words,&i)==D9_SHADER_VS_MATRIX_UV&&i.vertex&&i.instructions==4&&i.words==d9_demo_vs_words);
    assert(i.major==2&&i.minor==0&&i.stream==D9_SHADER_STREAM_VALID&&i.saw_end&&!i.bad_version);
    d9_shader_version_name(&i,text,sizeof text);assert(!strcmp(text,"vs_2_0"));
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"vs_2_0: supported (stream valid, 4 instructions)"));
    assert(d9_shader_analyze(d9_demo_ps,d9_demo_ps_words,&i)==D9_SHADER_PS_TEXTURE&&!i.vertex&&i.instructions==4);
    assert(i.major==2&&i.minor==0&&i.stream==D9_SHADER_STREAM_VALID&&i.saw_end);
    d9_shader_version_name(&i,text,sizeof text);assert(!strcmp(text,"ps_2_0"));
    d9_shader_diagnostic(&i,text,sizeof text);
    assert(!strcmp(text,"ps_2_0: supported (stream valid, 4 instructions)"));
    uint32_t altered[32]={0};memcpy(altered,d9_demo_vs,d9_demo_vs_words*4);altered[10]=0xa0e40001;
    assert(d9_shader_analyze(altered,d9_demo_vs_words,&i)==D9_SHADER_UNKNOWN);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.saw_end);
    assert(d9_shader_analyze(d9_demo_ps,d9_demo_ps_words-1,&i)==D9_SHADER_INVALID);
    altered[0]=d9_demo_ps[0];altered[1]=0x0002fffe;altered[2]=0x0000ffff;altered[3]=0x12345678;
    memcpy(altered+4,d9_demo_ps+1,(d9_demo_ps_words-1)*4);
    assert(d9_shader_analyze(altered,d9_demo_ps_words+3,&i)==D9_SHADER_PS_TEXTURE&&i.words==d9_demo_ps_words+3);
    assert(i.stream==D9_SHADER_STREAM_VALID&&i.instructions==4);
}
int main(void){decoder();shaders();shader_diagnostics();puts("BC1/BC2/BC3 block alpha/color, tiled output, pitches, SM2 templates and structured SM2/SM3 diagnostics passed");return 0;}
