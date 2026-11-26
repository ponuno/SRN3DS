#pragma once
/* 3DS-only citro3d adapter for the validated PICA200 TEV subset. */

#ifndef __3DS__
#error "d3d9_pica_citro3d.h is for the 3DS backend only"
#endif

#include <stddef.h>

#include <citro3d.h>

#include "d3d9_pica_tev.h"

/* Validate program, initialise all six TEV stages, copy the active descriptors
 * into C3D_TexEnv, and bind texture to unit 0 when the program samples it.
 * Unused stages are deterministic PREVIOUS passthroughs.  Returns 0 or -1.
 *
 * C3D has no public texture-unbind operation.  When texture_count is zero this
 * function deliberately leaves unit 0's old binding untouched but unreferenced;
 * call restore() before any fixed-function draw. */
int d9_pica_citro3d_apply_tev(const D9TevProgram *program, C3D_Tex *texture,
                              char *why, size_t why_size);

/* Menu-capable variant: up to two 2D texture units, color0 as PRIMARY, and a
 * c0..c8 float bank for external stage constants.  `constants` may be NULL only
 * when the validated descriptor has no external constant.  c8 is the internal
 * ambient+emissive fold slot, not a D3D9 cap. */
int d9_pica_citro3d_apply_tev_ex(const D9TevProgram *program,
                                 C3D_Tex *texture0, C3D_Tex *texture1,
                                 const float (*constants)[4],
                                 unsigned constant_count,
                                 char *why, size_t why_size);

/* Restore deterministic fixed-function TEV state: stage 0 replaces PRIMARY,
 * stages 1..5 pass PREVIOUS through.  Safe to call after every draw. */
void d9_pica_citro3d_restore_tev(void);
