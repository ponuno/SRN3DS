#pragma once
#include <stdint.h>
#include <stddef.h>
unsigned d9_bc_block_bytes(unsigned format);
/* DDS stores R8G8B8 rows padded to a 4-byte boundary. */
unsigned d9_rgb24_pitch(unsigned w);
/* Expand one linear R8G8B8 level (memory order B,G,R) to tightly packed
 * A8R8G8B8 (memory order B,G,R,A).  src_pitch is the DDS row pitch. */
int d9_rgb24_to_bgra8(void *dst,size_t dst_bytes,const void *src,size_t src_pitch,
                      size_t src_bytes,unsigned w,unsigned h);
/* Repack one linear A8B8G8R8 level (memory order R,G,B,A) to A8R8G8B8
 * (B,G,R,A), swapping the R and B bytes. */
int d9_abgr8_to_bgra8(void *dst,size_t dst_bytes,const void *src,size_t src_pitch,
                      size_t src_bytes,unsigned w,unsigned h);
/* One 4x4 block, row-major RGBA8888 output. */
int d9_bc_decode_block(unsigned format,const uint8_t *block,size_t bytes,uint8_t rgba[64]);
/* Decode directly to PICA tiled ABGR storage, without a full linear RGBA intermediate. */
int d9_bc_to_pica(unsigned format,void *dst,size_t dst_bytes,const void *src,size_t src_bytes,unsigned w,unsigned h,size_t pitch);
