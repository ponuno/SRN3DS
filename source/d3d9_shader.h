#pragma once
#include <stdint.h>
#include <stddef.h>
#include "d3d9_tev.h"
enum {D9_SHADER_UNKNOWN=0,D9_SHADER_VS_MATRIX_UV=1,D9_SHADER_PS_TEXTURE=2,D9_SHADER_TEV=3,D9_SHADER_INVALID=-1};
#define D9_SHADER_MAX_WORDS 4096u
/* D3DSIO_* opcodes 0..96 are the SM2/SM3 instruction set; comments (0xfffe),
 * phase (0xfffd) and END (0xffff) are handled separately. */
#define D9_SHADER_OPCODE_COUNT 97u
/* D9ShaderInfo.stream: how far the token walk got. A valid stream is one that
 * reached an END token without a framing error. */
enum {D9_SHADER_STREAM_VALID=0,D9_SHADER_STREAM_TRUNCATED=1,D9_SHADER_STREAM_INVALID=2};

typedef struct {
    unsigned vertex,major,minor,instructions,words,first_mismatch;
    uint32_t signature;
    int kind;
    uint32_t version;              /* raw version token, 0 when unavailable */
    unsigned first_opcode;         /* first instruction opcode */
    unsigned first_opcode_offset;  /* byte offset of first instruction token */
    unsigned opcode;               /* opcode explaining the rejection */
    unsigned opcode_offset;        /* byte offset of that token; 0 = none */
    unsigned stream;               /* D9_SHADER_STREAM_* */
    unsigned bad_version;          /* version token is not vs_/ps_ 2_0 or 3_0 */
    unsigned unknown_opcode;       /* token held an opcode outside the SM2/SM3 set */
    unsigned bad_length;           /* token length disagrees with the opcode */
    unsigned saw_end;              /* END token was reached */
    unsigned need_words;           /* truncated/bad length: words required */
    unsigned have_words;           /* truncated/bad length: words present/claimed */
    char tev_reason[256];          /* explicit TEV rejection, or empty */
} D9ShaderInfo;

/* Fixed-size opcode histogram. No allocation; one slot per SM2/SM3 opcode. */
typedef struct {
    uint32_t opcode[D9_SHADER_OPCODE_COUNT];
    unsigned instructions;         /* instructions counted (comments/END excluded) */
    unsigned outside;              /* tokens outside opcode 0..96 (not phase/comment/END) */
} D9ShaderCensus;

/* Bounded SM2/SM3 token scan. The two exact SM2 templates translate to their
 * historical kinds; a fully validated ps_3_0 stream is additionally offered to
 * the real TEV translator. */
int d9_shader_analyze(const uint32_t *code,size_t available,D9ShaderInfo *info);
/* Same scan, retaining a successfully translated TEV program for the COM
 * shader object. tev_out may be NULL. */
int d9_shader_analyze_tev(const uint32_t *code,size_t available,D9ShaderInfo *info,
                          D9TevProgram *tev_out);
/* Same scan, additionally filling a per-opcode histogram. */
int d9_shader_census_scan(const uint32_t *code,size_t available,D9ShaderInfo *info,D9ShaderCensus *census);
void d9_shader_census_clear(D9ShaderCensus *census);
void d9_shader_census_merge(D9ShaderCensus *into,const D9ShaderCensus *from);
/* "mov", "dcl", "texld", ... or "unknown". */
const char *d9_shader_opcode_name(unsigned opcode);
/* "vs_3_0" / "ps_2_0", or "unknown" when the version token is not vs_/ps_. */
int d9_shader_version_name(const D9ShaderInfo *info,char *text,size_t size);
/* Structured one-line rejection text, e.g.
 * "ps_3_0: unsupported (stream valid, 4 instructions); first unsupported opcode 'add' at byte 28"
 * Returns characters written (excluding NUL), or -1 for a bad argument. */
int d9_shader_diagnostic(const D9ShaderInfo *info,char *text,size_t size);
extern const uint32_t d9_demo_vs[],d9_demo_ps[];
extern const size_t d9_demo_vs_words,d9_demo_ps_words;
