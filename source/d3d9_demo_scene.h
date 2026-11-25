#pragma once
#include "d3d9_core.h"
typedef struct {D9Handle vb,ib,texture;} D9Demo;
int d9_demo_init(D9Device *d,D9Demo *scene);
int d9_demo_frame(D9Device *d,const D9Demo *scene,float angle);
void d9_demo_mesh(D9Vertex vertices[24],uint16_t indices[36]);
void d9_demo_fill_texture(void *pixels,size_t pitch);
int d9_demo_fill_bc(void *pixels,size_t pitch,size_t bytes,unsigned format);
int d9_demo_fill_bc_level(void *pixels,size_t pitch,size_t bytes,unsigned w,unsigned h,unsigned format,unsigned level);
void d9_demo_camera(D9Matrix *view,D9Matrix *projection);
void d9_demo_world(D9Matrix *world,float angle);
