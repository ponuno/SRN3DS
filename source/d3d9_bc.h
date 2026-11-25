#pragma once
#include <stdint.h>
#include <stddef.h>
unsigned d9_bc_block_bytes(unsigned format);
/* One 4x4 block, row-major RGBA8888 output. */
int d9_bc_decode_block(unsigned format,const uint8_t *block,size_t bytes,uint8_t rgba[64]);
/* Decode directly to PICA tiled ABGR storage, without a full linear RGBA intermediate. */
int d9_bc_to_pica(unsigned format,void *dst,size_t dst_bytes,const void *src,size_t src_bytes,unsigned w,unsigned h,size_t pitch);
