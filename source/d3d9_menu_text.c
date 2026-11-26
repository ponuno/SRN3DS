#include "d3d9_menu_text.h"

#include "d3d9_com.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define D3DFVF_MENU_TEXT (0x002u | 0x040u | 0x100u)
#define D3DSAMP_ADDRESSU 1u
#define D3DSAMP_ADDRESSV 2u
#define D3DSAMP_MINFILTER 5u
#define D3DSAMP_MAGFILTER 6u
#define D3DSAMP_MIPFILTER 7u
#define D3DTEXF_NONE 0u
#define D3DTEXF_POINT 1u
#define D3DTADDRESS_CLAMP 3u
#define D3DTSS_COLOROP 1u
#define D3DTSS_COLORARG1 2u
#define D3DTSS_COLORARG2 3u
#define D3DTSS_ALPHAOP 4u
#define D3DTSS_ALPHAARG1 5u
#define D3DTSS_ALPHAARG2 6u
#define D3DTOP_MODULATE 4u
#define D3DTA_TEXTURE 2u
#define D3DTA_DIFFUSE 1u

typedef struct {
    float x, y, z;
    uint32_t color;
    float u, v;
} MenuTextVertex;

_Static_assert(sizeof(MenuTextVertex) == 24, "menu text FVF vertex size");

static void text_reason(MenuTextDraw *ctx, const char *fmt, ...) {
    if (!ctx) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->reason, sizeof ctx->reason, fmt, ap);
    va_end(ap);
    ctx->reason[sizeof ctx->reason - 1] = 0;
}

const char *menu_text_draw_status_name(MenuTextDrawStatus status) {
    switch (status) {
    case MENU_TEXT_DRAW_OK: return "ok";
    case MENU_TEXT_DRAW_ERR_ARGUMENT: return "bad menu-text argument";
    case MENU_TEXT_DRAW_ERR_DEVICE: return "menu-text device failure";
    case MENU_TEXT_DRAW_ERR_A8: return "A8 texture unsupported";
    case MENU_TEXT_DRAW_ERR_UPLOAD: return "A8 upload failed";
    case MENU_TEXT_DRAW_ERR_EMPTY: return "empty text batch";
    case MENU_TEXT_DRAW_ERR_GEOMETRY: return "bad text geometry";
    case MENU_TEXT_DRAW_ERR_OVERFLOW: return "text vertex overflow";
    case MENU_TEXT_DRAW_ERR_STATE: return "text render state failure";
    case MENU_TEXT_DRAW_ERR_FVF: return "text FVF failure";
    case MENU_TEXT_DRAW_ERR_DRAW: return "text draw failure";
    default: return "unknown menu-text status";
    }
}

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    if (d9_com_demo_call(s, object, slot, args, count, &result)) return 1;
    return result;
}

MenuTextDrawStatus menu_text_draw_init(MenuTextDraw *ctx, D9ComDemo *demo,
                                       const MenuSwfAtlasTarget *atlas,
                                       uint32_t scratch,
                                       uint32_t scratch_bytes) {
    if (!ctx || !demo || !demo->ready || !demo->device || !atlas ||
        !atlas->pixels || !atlas->width || !atlas->height ||
        atlas->stride < atlas->width || !scratch || !scratch_bytes) {
        if (ctx) text_reason(ctx, "menu-text init needs a ready COM device, "
                                  "atlas and scratch");
        return MENU_TEXT_DRAW_ERR_ARGUMENT;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->demo = demo;
    ctx->width = atlas->width;
    ctx->height = atlas->height;
    ctx->indexed = demo->menu_indexed;

    GuestMemory *mem = &demo->memory;
    if (!guest_ptr(mem, scratch, scratch_bytes)) {
        if (guest_map(mem, scratch, scratch_bytes)) {
            text_reason(ctx, "menu-text scratch map failed");
            return MENU_TEXT_DRAW_ERR_DEVICE;
        }
        ctx->mapped_scratch = 1;
    }
    ctx->scratch = scratch;
    ctx->scratch_bytes = scratch_bytes;

    uint32_t texture = 0;
    uint32_t hr = d9_com_host_create_texture(
        demo->device, atlas->width, atlas->height, 1, 0, D9_A8, 0, &texture);
    if (hr || !texture) {
        const char *why = d9_com_last_diagnostic();
        text_reason(ctx, "A8 CreateTexture refused (hr %08lx): %s",
                    (unsigned long)hr, why ? why : "");
        if (ctx->mapped_scratch) guest_unmap(mem, scratch);
        ctx->mapped_scratch = 0;
        return MENU_TEXT_DRAW_ERR_A8;
    }
    ctx->texture = texture;

    size_t src_bytes = (size_t)atlas->stride * atlas->height;
    hr = d9_com_host_upload_texture_level(texture, 0, atlas->pixels,
                                          atlas->stride, src_bytes);
    if (hr) {
        const char *why = d9_com_last_diagnostic();
        text_reason(ctx, "A8 coverage upload refused (hr %08lx): %s",
                    (unsigned long)hr, why ? why : "");
        (void)d9_com_host_release(texture);
        ctx->texture = 0;
        if (ctx->mapped_scratch) guest_unmap(mem, scratch);
        ctx->mapped_scratch = 0;
        return MENU_TEXT_DRAW_ERR_UPLOAD;
    }
    return MENU_TEXT_DRAW_OK;
}

MenuTextDrawStatus menu_text_draw_batch(MenuTextDraw *ctx,
                                        const MenuSwfBatchVertex *vertices,
                                        uint32_t vertex_count,
                                        const uint32_t *indices,
                                        uint32_t index_count) {
    if (!ctx || !ctx->texture || !ctx->demo || !vertices || !indices) {
        if (ctx) text_reason(ctx, "menu-text draw needs a texture and vertices");
        return MENU_TEXT_DRAW_ERR_ARGUMENT;
    }
    if (!vertex_count || !index_count) {
        text_reason(ctx, "menu-text batch is empty");
        return MENU_TEXT_DRAW_ERR_EMPTY;
    }
    if ((index_count % 3u) != 0) {
        text_reason(ctx, "menu-text index count is not triangles");
        return MENU_TEXT_DRAW_ERR_GEOMETRY;
    }
    uint32_t triangles = index_count / 3u;
    /* Prefer one indexed submission: expand each source vertex once and keep
     * the caller's 16-bit indices.  The fallback keeps the old expanded UP
     * path and its 4,095-index limit. */
    int use_indexed = ctx->indexed && vertex_count <= 65536u;
    size_t vbytes = (size_t)vertex_count * sizeof(MenuTextVertex);
    size_t ioff = (vbytes + 3u) & ~(size_t)3u;
    size_t ibytes = (size_t)index_count * 2u;
    MenuTextVertex *dst = NULL;
    uint16_t *index16 = NULL;
    if (use_indexed) {
        if (ioff > ctx->scratch_bytes || ibytes > ctx->scratch_bytes - ioff) {
            text_reason(ctx, "indexed menu-text %lu vertices/%lu indices exceed "
                             "the %lu-byte scratch",
                        (unsigned long)vertex_count, (unsigned long)index_count,
                        (unsigned long)ctx->scratch_bytes);
            return MENU_TEXT_DRAW_ERR_OVERFLOW;
        }
        void *base = guest_ptr(&ctx->demo->memory, ctx->scratch, ioff + ibytes);
        if (!base) {
            text_reason(ctx, "menu-text indexed scratch is not mapped");
            return MENU_TEXT_DRAW_ERR_DEVICE;
        }
        dst = (MenuTextVertex *)base;
        index16 = (uint16_t *)((uint8_t *)base + ioff);
        for (uint32_t i = 0; i < vertex_count; ++i) {
            const MenuSwfBatchVertex *v = &vertices[i];
            dst[i].x = (float)v->x;
            dst[i].y = (float)v->y;
            dst[i].z = 0.0f;
            dst[i].color = v->color;
            dst[i].u = (float)v->u / 65536.0f;
            dst[i].v = (float)v->v / 65536.0f;
        }
        for (uint32_t i = 0; i < index_count; ++i) {
            uint32_t index = indices[i];
            if (index >= vertex_count || index > 0xFFFFu) {
                text_reason(ctx, "menu-text index %lu >= %lu vertices or 16-bit range",
                            (unsigned long)index, (unsigned long)vertex_count);
                return MENU_TEXT_DRAW_ERR_GEOMETRY;
            }
            index16[i] = (uint16_t)index;
        }
    } else {
        if (triangles > 4096u / 3u) {
            text_reason(ctx, "menu-text %lu triangles exceed the UP draw limit",
                        (unsigned long)triangles);
            return MENU_TEXT_DRAW_ERR_OVERFLOW;
        }
        size_t bytes = (size_t)index_count * sizeof(MenuTextVertex);
        if (bytes > ctx->scratch_bytes) {
            text_reason(ctx, "menu-text %lu vertices exceed the %lu-byte scratch",
                        (unsigned long)index_count,
                        (unsigned long)ctx->scratch_bytes);
            return MENU_TEXT_DRAW_ERR_OVERFLOW;
        }
        dst = (MenuTextVertex *)guest_ptr(&ctx->demo->memory, ctx->scratch, bytes);
        if (!dst) {
            text_reason(ctx, "menu-text scratch is not mapped");
            return MENU_TEXT_DRAW_ERR_DEVICE;
        }
        for (uint32_t i = 0; i < index_count; ++i) {
            uint32_t index = indices[i];
            if (index >= vertex_count) {
                text_reason(ctx, "menu-text index %lu >= %lu vertices",
                            (unsigned long)index, (unsigned long)vertex_count);
                return MENU_TEXT_DRAW_ERR_GEOMETRY;
            }
            const MenuSwfBatchVertex *v = &vertices[index];
            dst[i].x = (float)v->x;
            dst[i].y = (float)v->y;
            dst[i].z = 0.0f;
            dst[i].color = v->color;
            dst[i].u = (float)v->u / 65536.0f;
            dst[i].v = (float)v->v / 65536.0f;
        }
    }

    D9ComDemo *d = ctx->demo;
    uint32_t args[8];
    uint32_t hr;

    args[0] = 0;
    args[1] = ctx->texture;
    hr = call(d, d->device, 65, args, 2); /* SetTexture */
    if (hr) {
        text_reason(ctx, "SetTexture(0) failed: %s",
                    d9_com_last_diagnostic());
        return MENU_TEXT_DRAW_ERR_STATE;
    }

    static const struct {
        unsigned type, value;
    } sampler[] = {
        {D3DSAMP_MINFILTER, D3DTEXF_POINT},
        {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
        {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
    };
    for (unsigned i = 0; i < sizeof sampler / sizeof sampler[0]; ++i) {
        args[0] = 0;
        args[1] = sampler[i].type;
        args[2] = sampler[i].value;
        hr = call(d, d->device, 69, args, 3); /* SetSamplerState */
        if (hr) {
            text_reason(ctx, "SetSamplerState(%u) failed: %s",
                        sampler[i].type, d9_com_last_diagnostic());
            return MENU_TEXT_DRAW_ERR_STATE;
        }
    }

    static const struct {
        unsigned type, value;
    } stage[] = {
        {D3DTSS_COLOROP, D3DTOP_MODULATE},
        {D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {D3DTSS_COLORARG2, D3DTA_DIFFUSE},
        {D3DTSS_ALPHAOP, D3DTOP_MODULATE},
        {D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
        {D3DTSS_ALPHAARG2, D3DTA_DIFFUSE},
    };
    for (unsigned i = 0; i < sizeof stage / sizeof stage[0]; ++i) {
        args[0] = 0;
        args[1] = stage[i].type;
        args[2] = stage[i].value;
        hr = call(d, d->device, 67, args, 3); /* SetTextureStageState */
        if (hr) {
            text_reason(ctx, "SetTextureStageState(%u) failed: %s",
                        stage[i].type, d9_com_last_diagnostic());
            return MENU_TEXT_DRAW_ERR_STATE;
        }
    }

    args[0] = D3DFVF_MENU_TEXT;
    hr = call(d, d->device, 89, args, 1); /* SetFVF */
    if (hr) {
        text_reason(ctx, "SetFVF failed: %s", d9_com_last_diagnostic());
        return MENU_TEXT_DRAW_ERR_FVF;
    }

    if (use_indexed) {
        args[0] = D9_TRIANGLELIST;
        args[1] = 0; /* MinIndex */
        args[2] = vertex_count;
        args[3] = triangles;
        args[4] = ctx->scratch + (uint32_t)ioff;
        args[5] = D9_INDEX16;
        args[6] = ctx->scratch;
        args[7] = (uint32_t)sizeof(MenuTextVertex);
        hr = call(d, d->device, 84, args, 8); /* DrawIndexedPrimitiveUP */
        if (hr) {
            text_reason(ctx, "DrawIndexedPrimitiveUP failed: %s",
                        d9_com_last_diagnostic());
            return MENU_TEXT_DRAW_ERR_DRAW;
        }
    } else {
        args[0] = D9_TRIANGLELIST;
        args[1] = triangles;
        args[2] = ctx->scratch;
        args[3] = (uint32_t)sizeof(MenuTextVertex);
        hr = call(d, d->device, 83, args, 4); /* DrawPrimitiveUP */
        if (hr) {
            text_reason(ctx, "DrawPrimitiveUP failed: %s",
                        d9_com_last_diagnostic());
            return MENU_TEXT_DRAW_ERR_DRAW;
        }
    }
    return MENU_TEXT_DRAW_OK;
}

MenuTextDrawStatus menu_text_draw_destroy(MenuTextDraw *ctx) {
    if (!ctx) return MENU_TEXT_DRAW_ERR_ARGUMENT;
    MenuTextDrawStatus st = MENU_TEXT_DRAW_OK;
    if (ctx->texture) {
        if (ctx->demo && ctx->demo->device) {
            uint32_t unset[2] = {0, 0};
            (void)call(ctx->demo, ctx->demo->device, 65, unset, 2);
        }
        uint32_t hr = d9_com_host_release(ctx->texture);
        if (hr) st = MENU_TEXT_DRAW_ERR_DEVICE;
        ctx->texture = 0;
    }
    if (ctx->mapped_scratch && ctx->demo) {
        guest_unmap(&ctx->demo->memory, ctx->scratch);
        ctx->mapped_scratch = 0;
    }
    return st;
}
