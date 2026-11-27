#pragma once
/* Verified PICA200 vertex-shader catalog and upload/bind contract.
 *
 * Entries come from the `skinning` shader pack: a vs_3_0 bytecode SHA-256 whose
 * picasso assembly was emitted and numerically verified (`codegen ==
 * "emitted-verified"`).  The `.shbin` is committed next to the assembly and is
 * embedded here by `pack_plans.py emit-runtime`; no game bytes and no other
 * pack's rows are touched.
 *
 * Selection is by exact bytecode hash only.  `d9_pica_vs_declare()` refuses by
 * name unless the active D3D9 vertex declaration supplies exactly the three
 * inputs the emitted assembly reads, and `d9_pica_vs_bank()` refuses unless the
 * 4-bone palette window and the WVP rows the assembly addresses are present.
 *
 * This module never reports a D3D9 capability: GetDeviceCaps stays VS/PS 2.0
 * and DevCaps 0x00082000.  A loaded program is not a drawn frame.
 */

#include <stddef.h>
#include <stdint.h>

#include "d3d9_vertex.h"

#define D9_PICA_VS_SHA256 65u
#define D9_PICA_VS_MAX_ATTRS 5u
#define D9_PICA_VS_MAX_ROWS 32u
#define D9_PICA_VS_BONES 4u
#define D9_PICA_VS_BONE_ROWS (D9_PICA_VS_BONES * 3u)

enum {
    D9_PICA_VS_ATTR_POSITION = 0,
    D9_PICA_VS_ATTR_BLENDWEIGHT = 1,
    D9_PICA_VS_ATTR_BLENDINDICES = 2,
    D9_PICA_VS_ATTR_TEXCOORD = 3,
    D9_PICA_VS_ATTR_COLOR = 4
};

/* PICA loader component format, kept device-neutral for the host tests. */
enum {
    D9_PICA_VS_FMT_FLOAT = 0,
    D9_PICA_VS_FMT_UBYTE_NORM = 1
};

typedef struct {
    uint8_t attr;         /* D9_PICA_VS_ATTR_* */
    uint8_t decl_type;    /* required D9_DECL_* */
    uint8_t components;   /* PICA loader count */
    uint8_t format;       /* D9_PICA_VS_FMT_* */
    uint8_t reg;          /* PICA input register v0..v2 */
} D9PicaVsAttr;

typedef struct {
    const char *id;
    const char *sha256;        /* vs_3_0 bytecode hash (selection key) */
    const char *asm_name;
    const char *shbin_name;
    const char *shbin_sha256;
    const uint8_t *shbin;
    unsigned shbin_size;
    unsigned instructions;     /* emitted main-function word count */
    unsigned uniform_rows;     /* c0..c15 (16) or c0..c16 (17) */
    unsigned bone_rows;        /* 12: c0..c11, four 3x4 bone matrices */
    unsigned wvp_first_row;    /* 12: D3D c180..c183 -> PICA c12..c15 */
    unsigned wvp_rows;
    unsigned attr_count;
    D9PicaVsAttr attrs[D9_PICA_VS_MAX_ATTRS];
} D9PicaVsProgram;

unsigned d9_pica_vs_count(void);
const D9PicaVsProgram *d9_pica_vs_at(unsigned index);
/* Exact-hash lookup.  Returns the program or NULL. */
const D9PicaVsProgram *d9_pica_vs_lookup(const char *sha256_hex);

int d9_pica_vs_sha256_equal(const char *a, const char *b);

/* Device-free structural check: DVLB magic, DVLE count/offset, main word count,
 * and the embedded bytes' SHA-256 against the plan's `emitter.shbin_sha256`.
 * Returns 0 or -1 with a named reason in `why`. */
int d9_pica_vs_validate(const D9PicaVsProgram *program,
                        char *why, size_t why_size);

typedef struct {
    uint8_t attr;        /* D9_PICA_VS_ATTR_* */
    uint8_t stream;
    uint16_t offset;
    uint8_t components;
    uint8_t format;      /* D9_PICA_VS_FMT_* */
    uint8_t reg;
} D9PicaVsLoader;

typedef struct {
    unsigned count;
    uint32_t mask;   /* BufInfo attribute mask: bit vN per loader */
    D9PicaVsLoader loader[D9_PICA_VS_MAX_ATTRS];
} D9PicaVsDecl;

/* Full declaration contract for the emitted assembly: one stream, POSITION
 * FLOAT3, BLENDWEIGHT FLOAT4, BLENDINDICES UBYTE4N (normalised, because the
 * assembly multiplies indices by 765 = 3*255 and floors).  Anything else is a
 * named refusal; there is no partial loader set. */
int d9_pica_vs_declare(const D9PicaVsProgram *program, const D9Decl *decl,
                       D9PicaVsDecl *out, char *why, size_t why_size);

/* Uniform-bank layout for the 4-bone window: PICA row i is filled from D3D
 * register d3d_reg[i].  Rows 0..11 are the palette (D3D c0..c11), rows 12..15
 * the WVP (D3D c180..c183); a 17-row program also maps D3D c184 -> PICA c16. */
typedef struct {
    unsigned rows;
    unsigned d3d_reg[D9_PICA_VS_MAX_ROWS];
} D9PicaVsBank;

int d9_pica_vs_bank(const D9PicaVsProgram *program, D9PicaVsBank *out,
                    char *why, size_t why_size);

/* True only when every D3D register the bank reads was written by
 * SetVertexShaderConstantF.  `mask` has `words` 32-bit words; bit (reg & 31)
 * of word (reg >> 5) is set when c<reg> was written.  Eight words cover the
 * whole 256-register D3D9 bank. */
int d9_pica_vs_bank_ready(const D9PicaVsBank *bank, const uint32_t *mask,
                          unsigned words, char *why, size_t why_size);
