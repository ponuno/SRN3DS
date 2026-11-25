#pragma once
#include "d3d9_core.h"

/* Headless backend for the d3d9_core call stream. It validates every call,
 * records counters and produces no pixels. It advertises SM2 templates, one
 * active LDR render target (implicit or explicit), no depth-stencil surface
 * formats, and a validated no-pixel StretchRect sink. */
#define D9_NULL_RESOURCE_LIMIT (4u<<20)

typedef struct {
    unsigned begins,ends,draws,triangles,clears,clear_targets,presents;
    unsigned texture_creates,texture_uploads,texture_deletes;
    unsigned target_creates,target_deletes,target_binds,target_clears,target_draws;
    unsigned blits,blit_stretched,blit_linear;
    unsigned long long blit_pixels;
    uint32_t last_blit_filter;
    int last_blit_src_back,last_blit_dst_back;
    int target_active;
    unsigned long long vertices;
    uint32_t last_clear_argb,last_clear_flags;
    int scene;
    /* Colour-capable draw recording.  The null backend owns no pixels; these
     * fields prove which RGBA the core handed it, never that a pixel changed. */
    unsigned color_draws,color_vertices;
    D9ColorVertex last_color_vertices[3];
    unsigned last_color_count;
    D9DrawState last_color_state;
} D9NullStats;

const D9Backend *d9_null_backend(void);
void *d9_null_context(void);
const D9NullStats *d9_null_stats(void);
void d9_null_reset(void);
/* Count an explicit, validated IDirect3DDevice9::Clear. The backend has no
 * pixels; this records intent without claiming that a framebuffer changed. */
void d9_null_note_clear(unsigned flags);

/* Install the null backend into the current d3d9_com session. Call after
 * d9_com_init(&memory,NULL) and before the first successful CreateDevice.
 * Defined in d3d9_com.c so this GPU-free file needs no guest/import headers.
 * Returns 0 on success, -1 when there is no session or a device already exists. */
int d9_null_register(void);
