#pragma once
#include "d3d9_core.h"

/* Headless backend for the d3d9_core call stream. It validates every call,
 * keeps real BGRA pixels for every render target and for the private 400x240
 * "presented screen" image, and records counters.  This makes lockable
 * back-buffer read/write and the Present blit host-testable pixel by pixel.
 * It advertises SM2 templates, one active LDR render target (implicit or
 * explicit), D24S8 depth clears, a validated StretchRect copy and six-face
 * cube-map creation/upload counters. */
#define D9_NULL_RESOURCE_LIMIT (4u<<20)

typedef struct {
    unsigned begins,ends,draws,triangles,clears,clear_targets,presents;
    unsigned texture_creates,texture_uploads,texture_deletes;
    unsigned target_creates,target_deletes,target_binds,target_clears,target_draws;
    unsigned target_reads,target_writes;
    unsigned depth_clears;
    uint32_t last_depth_clear,last_depth_flags;
    unsigned viewport_calls,viewport_w,viewport_h;
    unsigned blits,blit_stretched,blit_linear;
    unsigned long long blit_pixels;
    uint32_t last_blit_filter;
    int last_blit_src_back,last_blit_dst_back;
    int target_active;
    unsigned long long vertices;
    uint32_t last_clear_argb,last_clear_flags;
    int scene;
    /* Screen image written by the last blit whose destination was the implicit
     * back buffer (Present).  present_valid is the only claim that it exists. */
    int present_valid;
    unsigned present_w,present_h;
    /* Colour-capable draw recording.  These fields prove which RGBA the core
     * handed it, never that a pixel changed. */
    unsigned color_draws,color_vertices;
    D9ColorVertex last_color_vertices[3];
    unsigned last_color_count;
    D9DrawState last_color_state;
    unsigned menu_draws,menu_textures;
    float last_menu_constants[9][4];
    /* Cube-map creation/upload accounting.  The core owns the six face stores;
     * these counters prove the null backend accepted every face/level. */
    unsigned cube_creates,cube_uploads;
    unsigned last_cube_format;
    /* Cube-face render-target accounting.  A face view is a separate recording
     * object; these counters prove lazy per-face/level target creation and that
     * the cube owns the colour storage. */
    unsigned cube_face_creates,cube_face_deletes,cube_face_binds,cube_face_clears;
    unsigned last_cube_face,last_cube_level,last_cube_depth_flags;
    unsigned volume_creates,volume_uploads;
    /* Cube sampling: the last draw's selected faces and sampled ARGB texels.
     * cube_face_counts accumulates every sampled vertex by face. */
    unsigned cube_draws,cube_vertices;
    unsigned cube_face_counts[6];
    unsigned last_cube_count;
    unsigned last_cube_faces[3];
    uint32_t last_cube_samples[3];
} D9NullStats;

const D9Backend *d9_null_backend(void);
void *d9_null_context(void);
const D9NullStats *d9_null_stats(void);
void d9_null_reset(void);
/* Top-first BGRA bytes of the last Present blit (400x240), or NULL before the
 * first one.  The bytes are a host-side copy, not a hardware framebuffer. */
const void *d9_null_present_pixels(unsigned *w,unsigned *h);
/* Count an explicit, validated IDirect3DDevice9::Clear. This records intent for
 * the colour target; the private present image is only written by a blit. */
void d9_null_note_clear(unsigned flags);

/* Install the null backend into the current d3d9_com session. Call after
 * d9_com_init(&memory,NULL) and before the first successful CreateDevice.
 * Defined in d3d9_com.c so this GPU-free file needs no guest/import headers.
 * Returns 0 on success, -1 when there is no session or a device already exists. */
int d9_null_register(void);
