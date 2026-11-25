#pragma once
#include "d3d9_com.h"
#include "n3ds_lift_runtime.h"
typedef struct {
    GuestMemory memory;CPU cpu;FnModule tables[2];
    uint32_t factory,device,vb,ib,texture,bc[3],vs,ps,decl,decl_pos,decl_uv;
    int bridge,shims,bound,ready,shaders,declaration;
    char message[160];
} D9ComDemo;
int d9_com_demo_init(D9ComDemo *s,D9Device *device);
int d9_com_demo_frame(D9ComDemo *s,float angle);
int d9_com_demo_enable_bc(D9ComDemo *s);
int d9_com_demo_select_texture(D9ComDemo *s,unsigned mode); /* 0=BGRA, 1=DXT1,2=DXT3,3=DXT5 */
int d9_com_demo_enable_shaders(D9ComDemo *s);
int d9_com_demo_select_shaders(D9ComDemo *s,int enabled);
int d9_com_demo_mip(D9ComDemo *s,unsigned level);
int d9_com_demo_blend(D9ComDemo *s,int enabled);
int d9_com_demo_enable_decl(D9ComDemo *s);
int d9_com_demo_select_decl(D9ComDemo *s,int enabled);
int d9_com_demo_destroy(D9ComDemo *s);
/* Expose host-side harness calls for regression tests. Args omit this. */
int d9_com_demo_call(D9ComDemo *s,uint32_t object,unsigned slot,const uint32_t *args,unsigned count,uint32_t *result);
