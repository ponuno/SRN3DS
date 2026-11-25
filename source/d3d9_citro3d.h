#pragma once
#include "d3d9_core.h"
/* Pure sampler-state projection shared by the backend and host state tests.
 * Filter fields retain D3D9 values: point=1, linear=2; mip_filter also allows
 * none=0.  MIPFILTER_NONE keeps the full resident range and selects one mip. */
typedef struct {
    unsigned min_level,max_level,mag_filter,min_filter,mip_filter;
} D9CitroTextureConfig;
static inline D9CitroTextureConfig d9_citro_texture_config(unsigned levels,const D9DrawState *state){
    D9CitroTextureConfig config={0};
    if(!levels||!state)return config;
    unsigned max=state->resident_mips?state->resident_mips-1:0;
    if(max>=levels)max=levels-1;
    config.min_level=state->max_mip<max?state->max_mip:max;
    config.max_level=max;
    config.mag_filter=state->mag_filter;
    config.min_filter=state->min_filter;
    config.mip_filter=state->mip_filter;
    return config;
}
/* Requires gfxInitDefault() before opening. Destroy device before closing. */
typedef struct D9Citro D9Citro;
D9Citro *d9_citro_open(void);
void d9_citro_close(D9Citro *c);
const D9Backend *d9_citro_backend(void);
