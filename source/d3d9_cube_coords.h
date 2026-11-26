#pragma once
/* D3D9 face index + face-local (u,v) -> the PICA cube-map direction vector.
 *
 * faces are 0..5 in +X,-X,+Y,-Y,+Z,-Z order (the same order as
 * D3DCUBEMAP_FACES and GPU_TEXFACE).  The core selects one face per vertex and
 * keeps the D3D face-local coordinates (u,v) with v=0 at the top row.  PICA's
 * face t axis starts at the last stored row, so the inverse mapping uses
 * t = 1 - v (the same vertical flip the 2D colour shader applies).
 *
 * The function only produces the standard cube direction; it does not sample
 * anything.  It is kept free of citro3d so the table can be host-tested. */
void d9_cube_direction(unsigned face,float u,float v,float out[3]);
