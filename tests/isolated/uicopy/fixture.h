#pragma once

/* Shared synthetic ui.copy fixture for the host and ARM/Azahar tests.
 *
 * The token stream is the registry's exact structural signature:
 *   dcl_texcoord t0.xy; dcl_2d s0; texld oC0,t0,s0
 * It contains no constants, vertex colour, VPOS, or extra instructions.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_com.h"
#include "d3d9_pipeline_api.h"
#include "d3d9_pica_tev.h"
#include "d3d9_shader.h"
#include "n3ds_lift_runtime.h"

#define UICOPY_TEX_SIZE 8u
#define UICOPY_TEXELS (UICOPY_TEX_SIZE * UICOPY_TEX_SIZE)
#define UICOPY_SCRATCH 0x10010000u
#define UICOPY_CODE (UICOPY_SCRATCH + 0x000u)
#define UICOPY_OUT (UICOPY_SCRATCH + 0x100u)
#define UICOPY_DATA (UICOPY_SCRATCH + 0x200u)
#define UICOPY_PP (UICOPY_SCRATCH + 0x300u)
#define UICOPY_PROBE 0x00401000u

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);

static inline uint32_t uicopy_instruction(unsigned op, unsigned operands) {
    return (uint32_t)op | ((uint32_t)operands << 24);
}

static inline uint32_t uicopy_dst(unsigned type, unsigned reg, unsigned mask) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((mask & 0xFu) << 16);
}

static inline uint32_t uicopy_src(unsigned type, unsigned reg, unsigned swizzle) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) |
           (reg & 0x7FFu) | ((swizzle & 0xFFu) << 16);
}

static inline size_t uicopy_tokens(uint32_t code[16]) {
    size_t n = 0;
    code[n++] = 0xFFFF0300u;
    code[n++] = uicopy_instruction(0x1Fu, 2u);
    code[n++] = 0x80000005u; /* D3DDECLUSAGE_TEXCOORD, index 0 */
    code[n++] = uicopy_dst(1u, 0u, 0x3u); /* v0.xy */
    code[n++] = uicopy_instruction(0x1Fu, 2u);
    code[n++] = 0x90000000u; /* 2D sampler */
    code[n++] = uicopy_dst(10u, 0u, 0xFu); /* s0 */
    code[n++] = uicopy_instruction(0x42u, 3u);
    code[n++] = uicopy_dst(8u, 0u, 0xFu); /* oC0 */
    code[n++] = uicopy_src(1u, 0u, 0xE4u); /* v0.xyzw */
    code[n++] = uicopy_src(10u, 0u, 0xE4u); /* s0 */
    code[n++] = 0x0000FFFFu;
    return n;
}

static inline void uicopy_vertices(D9Vertex vertices[3]) {
    /* Oversized triangle covers the full target with UV [0,1] inside it. */
    vertices[0] = (D9Vertex){-1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    vertices[1] = (D9Vertex){ 3.0f, -1.0f, 0.0f, 2.0f, 0.0f};
    vertices[2] = (D9Vertex){-1.0f,  3.0f, 0.0f, 0.0f, 2.0f};
}

static inline uint32_t uicopy_texel_argb(unsigned x, unsigned y) {
    unsigned r = 32u + x * 16u;
    unsigned g = 32u + y * 16u;
    unsigned b = 96u + x * 4u + y * 12u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t uicopy_texel_pica(uint32_t argb) {
    /* D3D A8R8G8B8 -> PICA RGBA8 memory word (R,G,B,A high to low). */
    return (argb << 8) | (argb >> 24);
}

static inline void uicopy_texels(uint32_t argb[UICOPY_TEXELS]) {
    for (unsigned y = 0; y < UICOPY_TEX_SIZE; ++y)
        for (unsigned x = 0; x < UICOPY_TEX_SIZE; ++x)
            argb[y * UICOPY_TEX_SIZE + x] = uicopy_texel_argb(x, y);
}

typedef struct {
    unsigned count, min_x, max_x, min_y, max_y;
} UICopyPixelBlock;

/* Shared deterministic histogram/block proof. pica_words=1 expects the raw
 * 0xAABBGGRR words returned by a PICA display transfer; pica_words=0 expects
 * top-first D3D A8R8G8B8 words returned by render-target LockRect readback. */
static inline int uicopy_pixel_proof(const uint32_t *pixels, unsigned width,
                                     unsigned height, int pica_words,
                                     char *detail, size_t detail_size) {
    uint32_t argb[UICOPY_TEXELS], expected[UICOPY_TEXELS];
    UICopyPixelBlock blocks[UICOPY_TEXELS];
    uicopy_texels(argb);
    memset(blocks, 0, sizeof blocks);
    for (unsigned i = 0; i < UICOPY_TEXELS; ++i) {
        expected[i] = pica_words ? uicopy_texel_pica(argb[i]) : argb[i];
        blocks[i].min_x = width;
        blocks[i].min_y = height;
    }
    for (unsigned i = 0; i < UICOPY_TEXELS; ++i)
        for (unsigned j = i + 1; j < UICOPY_TEXELS; ++j)
            if (expected[i] == expected[j]) {
                snprintf(detail, detail_size, "fixture texels are not distinct");
                return -1;
            }

    unsigned unknown = 0;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint32_t value = pixels[y * width + x];
            unsigned index;
            for (index = 0; index < UICOPY_TEXELS; ++index)
                if (expected[index] == value) break;
            if (index == UICOPY_TEXELS) {
                ++unknown;
                continue;
            }
            UICopyPixelBlock *b = &blocks[index];
            ++b->count;
            if (x < b->min_x) b->min_x = x;
            if (x > b->max_x) b->max_x = x;
            if (y < b->min_y) b->min_y = y;
            if (y > b->max_y) b->max_y = y;
        }
    }

    unsigned bad_count = 0, bad_shape = 0;
    const unsigned expected_count = width * height / UICOPY_TEXELS;
    const unsigned cell = width / UICOPY_TEX_SIZE;
    for (unsigned i = 0; i < UICOPY_TEXELS; ++i) {
        UICopyPixelBlock *b = &blocks[i];
        unsigned w = b->max_x - b->min_x + 1u;
        unsigned h = b->max_y - b->min_y + 1u;
        if (b->count != expected_count) ++bad_count;
        if (width != height || !cell || b->count != w * h ||
            w != cell || h != cell)
            ++bad_shape;
    }
    snprintf(detail, detail_size,
             "unknown=%u bad_counts=%u bad_shapes=%u expected_each=%u",
             unknown, bad_count, bad_shape, expected_count);
    return unknown || bad_count || bad_shape ? -1 : 1;
}

static inline int uicopy_validate(const uint32_t *code, size_t words,
                                  D9TevProgram *out, char *why, size_t why_size) {
    if (!code || !out) return -1;
    if (why && why_size) why[0] = 0;

    D9ShaderInfo info;
    int result = d9_shader_analyze_tev(code, words, &info, out);
    if (result != D9_SHADER_TEV) {
        if (why && why_size)
            snprintf(why, why_size, "d9_shader_analyze_tev returned %d", result);
        return -1;
    }
    if (info.instructions != 3 || out->stages != 1 || out->textures != 1 ||
        out->inputs != 1 || out->constants != 0 || out->uses_vertex_color) {
        if (why && why_size)
            snprintf(why, why_size, "unexpected ui.copy analysis shape");
        return -1;
    }

    D9PicaTevProgram descriptor;
    if (d9_pica_tev_validate(out, &descriptor, NULL, why, why_size)) return -1;
    if (descriptor.stage_count != 1 || descriptor.texture_count != 1 ||
        descriptor.stage[0].func_rgb != D9TEV_FUNC_REPLACE ||
        descriptor.stage[0].func_a != D9TEV_FUNC_REPLACE ||
        descriptor.stage[0].src_rgb[0] != D9TEV_SRC_TEX0 ||
        descriptor.stage[0].src_a[0] != D9TEV_SRC_TEX0) {
        if (why && why_size)
            snprintf(why, why_size, "ui.copy did not validate as REPLACE(TEXTURE0)");
        return -1;
    }

    D9PipelineShaderView view;
    D9PipelineMatch match;
    D9PipelineRequest request;
    memset(&request, 0, sizeof request);
    request.source = D9_PIPELINE_SOURCE_FXP_PS3;
    request.quality = D9_PIPELINE_QUALITY_LDR;
    if (d9_pipeline_parse_ps3(code, words, &view, why, why_size)) return -1;
    const D9PipelineDescriptor *selected = d9_pipeline_select(
        D9_PIPELINE_INTERFACE_TEXT, &view, &request, &match, why, why_size);
    if (!selected || strcmp(selected->id, "ui.copy")) {
        if (why && why_size)
            snprintf(why, why_size, "registry did not select ui.copy");
        return -1;
    }
    D9TevProgram plan;
    if (d9_pipeline_build(selected, &view, &request, &match, &plan, why, why_size))
        return -1;
    if (memcmp(&plan, out, sizeof plan)) {
        if (why && why_size)
            snprintf(why, why_size, "registry plan differs from analyzed TEV");
        return -1;
    }
    return 0;
}

/* Real x86 COM/stdcall bridge fixture.  The caller owns the adopted D9Device. */
typedef struct {
    GuestMemory memory;
    CPU cpu;
    uint32_t factory, device, declaration, vb, texture, shader;
    FnEntry probe[1];
    FnModule modules[2];
    int memory_ready, com_ready, bound, resources_ready;
    char error[256];
} UICopyCom;

static void uicopy_guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "ui.copy probe argument count");
    for (unsigned i = n; i > 0; --i) {
        c->r[4] -= 4;
        M32(c->r[4]) = M32(sp + 8 + i * 4);
    }
    c->r[4] -= 4;
    M32(c->r[4]) = UICOPY_PROBE + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c);
    else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "ui.copy COM stdcall stack mismatch");
    c->r[4] = sp + 4;
}

static int uicopy_com_error(UICopyCom *h, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->error, sizeof h->error, fmt, ap);
    va_end(ap);
    return -1;
}

static int uicopy_invoke(UICopyCom *h, uint32_t target, const uint32_t *args,
                         unsigned n, uint32_t *result) {
    if (n > 16) return uicopy_com_error(h, "too many COM arguments");
    uint32_t frame[18] = {target, n};
    for (unsigned i = 0; i < n; ++i) frame[i + 2] = args[i];
    int rc = n3ds_lift_invoke(&h->cpu, UICOPY_PROBE, frame, n + 2, result);
    if (rc) return uicopy_com_error(h, "guest invoke 0x%08lx: %s",
                                   (unsigned long)target, n3ds_lift_fault());
    return 0;
}

static int uicopy_com_call_result(UICopyCom *h, uint32_t object, unsigned slot,
                                  const uint32_t *args, unsigned n,
                                  uint32_t *result) {
    uint32_t vt, fn;
    if (!object || guest_read32(&h->memory, object, &vt) || !vt ||
        guest_read32(&h->memory, vt + slot * 4, &fn) || !fn)
        return uicopy_com_error(h, "bad COM vtable object=0x%08lx slot=%u",
                                (unsigned long)object, slot);
    uint32_t frame[16] = {object};
    for (unsigned i = 0; i < n; ++i) frame[i + 1] = args[i];
    uint32_t value = 0;
    if (uicopy_invoke(h, fn, frame, n + 1, &value)) return -1;
    if (result) *result = value;
    if (value) return uicopy_com_error(h, "COM slot %u returned 0x%08lx", slot,
                                      (unsigned long)value);
    return 0;
}

static int uicopy_com_call(UICopyCom *h, uint32_t object, unsigned slot,
                           const uint32_t *args, unsigned n) {
    return uicopy_com_call_result(h, object, slot, args, n, NULL);
}

/* IUnknown::Release returns the remaining reference count, not an HRESULT. */
static int uicopy_com_release_result(UICopyCom *h, uint32_t object,
                                     uint32_t *refs) {
    uint32_t vt, fn;
    if (!object || guest_read32(&h->memory, object, &vt) || !vt ||
        guest_read32(&h->memory, vt + 2u * 4u, &fn) || !fn)
        return uicopy_com_error(h, "bad COM Release vtable");
    uint32_t frame[1] = {object}, value = 0;
    if (uicopy_invoke(h, fn, frame, 1, &value)) return -1;
    if (refs) *refs = value;
    return 0;
}

static int uicopy_read_out(UICopyCom *h, uint32_t *value) {
    if (guest_read32(&h->memory, UICOPY_OUT, value) || !*value)
        return uicopy_com_error(h, "missing guest output");
    return 0;
}

static int uicopy_com_attach(UICopyCom *h, D9Device *core) {
    memset(h, 0, sizeof *h);
    h->error[0] = 0;
    guest_init(&h->memory, 2u << 20);
    h->memory_ready = 1;
    if (guest_map(&h->memory, 0x10000000u, 0x4000u) ||
        guest_map(&h->memory, UICOPY_SCRATCH, 0x1000u))
        return uicopy_com_error(h, "guest mapping failed");
    if (d9_com_init(&h->memory, core))
        return uicopy_com_error(h, "d9_com_init failed");
    h->com_ready = 1;

    h->probe[0] = (FnEntry){UICOPY_PROBE, uicopy_guest_dispatch_probe};
    h->modules[0] = (FnModule){"ui.copy-probe", h->probe, 1};
    h->modules[1] = d9_com_module();
    if (n3ds_lift_bind_modules(&h->memory, h->modules, 2))
        return uicopy_com_error(h, "lift module bind failed");
    h->bound = 1;
    n3ds_lift_init_cpu(&h->cpu, 0x10004000u, 0);

    PeImport import = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    uint32_t create9 = d9_com_resolve(&import, NULL);
    uint32_t sdk = 32;
    if (!create9 || uicopy_invoke(h, create9, &sdk, 1, &h->factory) || !h->factory)
        return uicopy_com_error(h, "Direct3DCreate9 failed");

    uint32_t pp[14] = {400, 240, D9_X8R8G8B8, 1, 0, 0, 1, 0,
                       1, 0, 0, 0, 0, 1};
    if (guest_write(&h->memory, UICOPY_PP, pp, sizeof pp))
        return uicopy_com_error(h, "present-parameter write failed");
    uint32_t create[6] = {0, 1, 0, 0x20, UICOPY_PP, UICOPY_OUT};
    if (uicopy_com_call(h, h->factory, 16, create, 6) ||
        uicopy_read_out(h, &h->device))
        return -1;
    return 0;
}

static int uicopy_com_create_resources(UICopyCom *h, const uint32_t *code,
                                       size_t words) {
    D9Vertex vertices[3];
    uicopy_vertices(vertices);
    D9VertexElement decl[] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0},
        {0, 12, D9_DECL_FLOAT2, 0, D9_USAGE_TEXCOORD, 0},
        {0xFF, 0, D9_DECL_UNUSED, 0, 0, 0},
    };
    uint32_t args[8], pointer, pitch, level = 0;
    if (guest_write(&h->memory, UICOPY_DATA, decl, sizeof decl))
        return uicopy_com_error(h, "declaration write failed");
    args[0] = UICOPY_DATA; args[1] = UICOPY_OUT;
    if (uicopy_com_call(h, h->device, 86, args, 2) ||
        uicopy_read_out(h, &h->declaration))
        return -1;

    args[0] = sizeof vertices; args[1] = 0; args[2] = 0; args[3] = 1;
    args[4] = UICOPY_OUT; args[5] = 0;
    if (uicopy_com_call(h, h->device, 26, args, 6) || uicopy_read_out(h, &h->vb))
        return -1;
    args[0] = 0; args[1] = 0; args[2] = UICOPY_OUT; args[3] = 0;
    if (uicopy_com_call(h, h->vb, 11, args, 4) || uicopy_read_out(h, &pointer))
        return -1;
    if (guest_write(&h->memory, pointer, vertices, sizeof vertices) ||
        uicopy_com_call(h, h->vb, 12, NULL, 0))
        return uicopy_com_error(h, "vertex upload failed");
    args[0] = 0; args[1] = h->vb; args[2] = 0; args[3] = sizeof(D9Vertex);
    if (uicopy_com_call(h, h->device, 100, args, 4)) return -1;
    args[0] = h->declaration;
    if (uicopy_com_call(h, h->device, 87, args, 1)) return -1;

    args[0] = UICOPY_TEX_SIZE; args[1] = UICOPY_TEX_SIZE; args[2] = 1;
    args[3] = 0; args[4] = D9_A8R8G8B8; args[5] = D9_POOL_MANAGED;
    args[6] = UICOPY_OUT; args[7] = 0;
    if (uicopy_com_call(h, h->device, 23, args, 8) || uicopy_read_out(h, &h->texture))
        return -1;
    args[0] = 0; args[1] = UICOPY_OUT; args[2] = 0; args[3] = 0;
    if (uicopy_com_call(h, h->texture, 19, args, 4) ||
        guest_read32(&h->memory, UICOPY_OUT, &pitch) ||
        guest_read32(&h->memory, UICOPY_OUT + 4, &pointer))
        return uicopy_com_error(h, "texture lock failed");
    if (pitch != UICOPY_TEX_SIZE * 4u)
        return uicopy_com_error(h, "unexpected texture pitch %lu", (unsigned long)pitch);
    uint32_t texels[UICOPY_TEXELS];
    uicopy_texels(texels);
    if (guest_write(&h->memory, pointer, texels, sizeof texels) ||
        uicopy_com_call(h, h->texture, 20, &level, 1))
        return uicopy_com_error(h, "texture upload failed");
    args[0] = 0; args[1] = h->texture;
    if (uicopy_com_call(h, h->device, 65, args, 2)) return -1;

    /* Point sampling, clamp addressing, no mip interpolation. */
    static const unsigned sampler[][2] = {{1, 1}, {2, 1}, {5, 1}, {6, 1}, {7, 0}, {9, 0}};
    for (unsigned i = 0; i < sizeof sampler / sizeof sampler[0]; ++i) {
        args[0] = 0; args[1] = sampler[i][0]; args[2] = sampler[i][1];
        if (uicopy_com_call(h, h->device, 69, args, 3)) return -1;
    }
    static const unsigned render[][2] = {{7, 0}, {22, 1}, {27, 0}, {168, 15}};
    for (unsigned i = 0; i < sizeof render / sizeof render[0]; ++i) {
        args[0] = render[i][0]; args[1] = render[i][1];
        if (uicopy_com_call(h, h->device, 57, args, 2)) return -1;
    }

    if (guest_write(&h->memory, UICOPY_CODE, code, words * sizeof *code))
        return uicopy_com_error(h, "shader token write failed");
    args[0] = UICOPY_CODE; args[1] = UICOPY_OUT;
    if (uicopy_com_call(h, h->device, 106, args, 2) || uicopy_read_out(h, &h->shader))
        return -1;
    args[0] = h->shader;
    if (uicopy_com_call(h, h->device, 107, args, 1)) return -1;
    h->resources_ready = 1;
    return 0;
}

static int uicopy_com_frame(UICopyCom *h) {
    uint32_t draw[3] = {D9_TRIANGLELIST, 0, 1};
    uint32_t present[4] = {0, 0, 0, 0};
    if (uicopy_com_call(h, h->device, 41, NULL, 0) ||
        uicopy_com_call(h, h->device, 81, draw, 3) ||
        uicopy_com_call(h, h->device, 42, NULL, 0) ||
        uicopy_com_call(h, h->device, 17, present, 4))
        return -1;
    return 0;
}

static inline int uicopy_com_set_pixel_shader(UICopyCom *h, uint32_t shader) {
    return uicopy_com_call(h, h->device, 107, &shader, 1);
}

static inline int uicopy_com_set_texture(UICopyCom *h, uint32_t texture) {
    uint32_t args[2] = {0, texture};
    return uicopy_com_call(h, h->device, 65, args, 2);
}

static inline int uicopy_com_release_shader(UICopyCom *h, uint32_t *refs) {
    if (uicopy_com_release_result(h, h->shader, refs)) return -1;
    h->shader = 0;
    return 0;
}

static void uicopy_com_detach(UICopyCom *h) {
    if (h->bound) n3ds_lift_release();
    if (h->com_ready) d9_com_shutdown();
    if (h->memory_ready) guest_destroy(&h->memory);
    memset(h, 0, sizeof *h);
}
