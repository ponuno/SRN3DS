#pragma once
/* Host/menu text draw demo: CPU A8 atlas + quad batch -> D3D9 COM calls.
 *
 * This is a host-side menu layer, not the TESV renderer.  It creates a real
 * D3DFMT_A8 texture through the COM device, uploads the caller atlas with
 * LockRect/UnlockRect, expands the indexed batch to DrawPrimitiveUP vertices,
 * and sets stage-0 MODULATE(TEXTURE,DIFFUSE), POINT/CLAMP sampler and
 * XYZ|DIFFUSE|TEX1 FVF.  A8 is never silently replaced by A8R8G8B8; if the
 * device/backend cannot create or upload A8, the operation fails by name.
 *
 * The caller must have an active BeginScene for draw.  No frame, pixel result,
 * texture cache, GPU sample or TESV integration is claimed.
 */

#include <stddef.h>
#include <stdint.h>

#include "d3d9_com_demo.h"
#include "menu_swf_atlas.h"
#include "menu_swf_batch.h"

typedef enum {
    MENU_TEXT_DRAW_OK = 0,
    MENU_TEXT_DRAW_ERR_ARGUMENT,
    MENU_TEXT_DRAW_ERR_DEVICE,
    MENU_TEXT_DRAW_ERR_A8,
    MENU_TEXT_DRAW_ERR_UPLOAD,
    MENU_TEXT_DRAW_ERR_EMPTY,
    MENU_TEXT_DRAW_ERR_GEOMETRY,
    MENU_TEXT_DRAW_ERR_OVERFLOW,
    MENU_TEXT_DRAW_ERR_STATE,
    MENU_TEXT_DRAW_ERR_FVF,
    MENU_TEXT_DRAW_ERR_DRAW
} MenuTextDrawStatus;

typedef struct {
    D9ComDemo *demo;
    uint32_t texture; /* guest IDirect3DTexture9* */
    uint32_t scratch; /* guest vertex upload buffer */
    uint32_t scratch_bytes;
    uint32_t width, height;
    uint8_t mapped_scratch;
    char reason[192];
} MenuTextDraw;

const char *menu_text_draw_status_name(MenuTextDrawStatus status);

/* Create the A8 texture and upload the atlas.  `scratch`/`scratch_bytes` is a
 * guest region used for expanded DrawPrimitiveUP vertices; it is mapped here
 * when not already present.  The atlas must be width/height/stride/pixels with
 * a supported power-of-two size (8..1024 in this bridge). */
MenuTextDrawStatus menu_text_draw_init(MenuTextDraw *ctx, D9ComDemo *demo,
                                       const MenuSwfAtlasTarget *atlas,
                                       uint32_t scratch,
                                       uint32_t scratch_bytes);

/* Expand the indexed batch and issue exactly one DrawPrimitiveUP.  indices
 * must be triangles and refer to vertices.  Empty batches are a named refusal. */
MenuTextDrawStatus menu_text_draw_batch(MenuTextDraw *ctx,
                                        const MenuSwfBatchVertex *vertices,
                                        uint32_t vertex_count,
                                        const uint32_t *indices,
                                        uint32_t index_count);

/* Release the texture and any scratch mapping created by init. */
MenuTextDrawStatus menu_text_draw_destroy(MenuTextDraw *ctx);
