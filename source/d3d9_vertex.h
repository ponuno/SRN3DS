#pragma once
#include <stddef.h>
#include <stdint.h>
#define D9_MAX_STREAMS 16u
#define D9_MAX_DECL 64u
enum {D9_DECL_FLOAT1=0,D9_DECL_FLOAT2,D9_DECL_FLOAT3,D9_DECL_FLOAT4,D9_DECL_COLOR,D9_DECL_UBYTE4,
 D9_DECL_SHORT2,D9_DECL_SHORT4,D9_DECL_UBYTE4N,D9_DECL_SHORT2N,D9_DECL_SHORT4N,D9_DECL_USHORT2N,
 D9_DECL_USHORT4N,D9_DECL_UDEC3,D9_DECL_DEC3N,D9_DECL_FLOAT16_2,D9_DECL_FLOAT16_4,D9_DECL_UNUSED};
enum {D9_USAGE_POSITION=0,D9_USAGE_NORMAL=3,D9_USAGE_TEXCOORD=5,D9_USAGE_POSITIONT=9,D9_USAGE_COLOR=10};
/* D3DFVF_* bits used by the admitted pre-transformed GFx batch. */
enum {D9_OK=0,D9_INVALID=-1,D9_MEMORY=-2,D9_BACKEND=-3,D9_RELEASE_FAILED=-4,D9_UNSUPPORTED=-5};
enum {D9_FVF_XYZ=0x002u,D9_FVF_XYZRHW=0x004u,D9_FVF_NORMAL=0x010u,D9_FVF_DIFFUSE=0x040u,D9_FVF_TEX1=0x100u};
enum {
    D9_FVF_XYZ_TEX1=D9_FVF_XYZ|D9_FVF_TEX1,              /* 0x102 */
    D9_FVF_XYZ_DIFFUSE_TEX1=D9_FVF_XYZ|D9_FVF_DIFFUSE|D9_FVF_TEX1, /* 0x142? no: 0x142 */
    D9_FVF_XYZRHW_TEX1=D9_FVF_XYZRHW|D9_FVF_TEX1,        /* 0x104 */
    D9_FVF_XYZRHW_DIFFUSE_TEX1=D9_FVF_XYZRHW|D9_FVF_DIFFUSE|D9_FVF_TEX1 /* 0x144 */
};
typedef struct {uint16_t stream,offset;uint8_t type,method,usage,usage_index;} D9VertexElement;
enum {D9_USAGE_BLENDWEIGHT=1,D9_USAGE_BLENDINDICES=2};
typedef struct {
    D9VertexElement elements[D9_MAX_DECL];unsigned count,position,uv,color,normal,stream_mask,position_t;
    /* BLENDWEIGHT0/BLENDINDICES0 element indices, or UINT32_MAX.  The verified
     * PICA VS path decodes them beside position/uv/color/normal; the generic
     * draw path ignores them. */
    unsigned weight,index;
    unsigned required[D9_MAX_STREAMS];
} D9Decl;
/* Decoded skinned vertex for the verified PICA VS path.  weight is the
 * BLENDWEIGHT0 float4; index is BLENDINDICES0 after normalisation, quantised
 * back to the 0..255 byte the PICA loader reads.  The other fields are the same
 * decode the fixed-function path uses. */
typedef struct {
    float x,y,z;
    float u,v;
    float r,g,b,a;
    float nx,ny,nz;
    float weight[4];
    uint8_t index[4];
} D9SkinVertex;
unsigned d9_decl_type_size(unsigned type);
unsigned d9_decl_components(unsigned type);
/* Decode an LE element into floats with D3D9 default (0,0,0,1). */
int d9_decl_decode(unsigned type,const void *data,size_t bytes,float out[4]);
/* Admit POSITION/POSITIONT + optional COLOR0 + TEXCOORD0.  Colour is admitted
 * as data here; whether a backend can consume it is a separate draw-time
 * gate (see d9_draw_* and D9Backend.draw_colored). */
int d9_decl_init(D9Decl *decl,const D9VertexElement *elements,unsigned count);
/* Same admission contract as d9_decl_init, but names the first rejected
 * element (stream/type/usage/offset) when why/cap are given. */
int d9_decl_init_why(D9Decl *decl,const D9VertexElement *elements,unsigned count,
                     char *why,size_t why_cap);
/* Map a supported D3D9 FVF to a declaration.  On failure *why names the exact
 * rejected bit or combination.  D9_OK, D9_INVALID or D9_UNSUPPORTED.
 * allow_normal admits D3DFVF_NORMAL for the test-only CPU T&L prototype; the
 * production path passes 0 so its admitted FVF set does not widen. */
int d9_decl_from_fvf(unsigned fvf,int allow_normal,D9Decl *decl,const char **why);
