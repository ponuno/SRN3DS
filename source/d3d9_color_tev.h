#pragma once
#include "d3d9_core.h"

/* Closed D3D9 stage-0 fixed-function state -> one PICA TEV stage.
 *
 * Kept free of <3ds.h>/citro3d headers so host tests can prove the mapping
 * against a CPU reference.  Only the subset admitted by
 * d9_set_texture_stage_state() is expressible; any combination that would
 * require more than one TEV stage, a second texture unit, or a semantics the
 * colour vertex shader cannot produce fails with a named reason rather than
 * drawing a different blend than the guest asked for.
 *
 * D3D9 defines CURRENT at texture stage 0 as the diffuse (vertex) colour, so
 * it maps to the same PICA source as DIFFUSE.  It is kept as a distinct enum
 * so the stage-0 rule is visible and testable. */

enum {
    D9_CTEV_SRC_TEXTURE = 0, /* GPU_TEXTURE0   */
    D9_CTEV_SRC_PRIMARY = 1, /* GPU_PRIMARY_COLOR (interpolated vertex RGBA) */
    D9_CTEV_SRC_CURRENT = 2  /* D3D9 CURRENT at stage 0 == DIFFUSE */
};
enum { D9_CTEV_REPLACE = 0, D9_CTEV_MODULATE = 1 };

typedef struct {
    unsigned color_func, color_src[3];
    unsigned alpha_func, alpha_src[3];
} D9ColorTevStage;

/* Returns D9_OK on success.  D9_UNSUPPORTED (with *why naming the exact
 * rejected operation/argument) when the stage-0 tuple cannot be expressed as
 * a single TEV stage; D9_INVALID for null arguments.  has_texture selects
 * whether a source of TEXTURE is legal for this draw. */
int d9_color_tev_stage0(const D9DrawState *state, int has_texture,
                        D9ColorTevStage *out, const char **why);
