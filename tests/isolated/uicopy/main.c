/* Isolated ARM/Azahar end-to-end ui.copy draw proof.
 *
 * Real path:
 *   exact ps_3_0 tokens -> d9_shader_analyze_tev -> guest COM stdcall
 *   CreatePixelShader/SetPixelShader -> d3d9_core draw state ->
 *   d3d9_citro3d TEV apply -> C3D_DrawArrays -> Present
 *
 * The 8x8 source has 64 known distinct texels.  A fullscreen triangle draws it
 * to a small VRAM colour texture (128x128 in the recorded run).  After Present,
 * a synchronized display transfer attempts deterministic pixel readback.  If
 * render-to-texture or the transfer is unavailable, the log explicitly says
 * PIXEL_PROOF=NO and retains state/counter evidence plus the screenshot.
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_pica_citro3d.h"
#include "fixture.h"

static int counted_apply_tev(const D9TevProgram *program, C3D_Tex *texture,
                             char *why, size_t why_size);
static void counted_draw_arrays(GPU_Primitive_t primitive, int first, int size);

/* Include the real backend so this isolated probe can replace only its render
 * target with a readable VRAM texture and count the two real submission calls.
 * The bodies below still call the production implementations. */
#define d9_pica_citro3d_apply_tev counted_apply_tev
#define C3D_DrawArrays counted_draw_arrays
#include "../../../source/d3d9_citro3d.c"
#undef C3D_DrawArrays
#undef d9_pica_citro3d_apply_tev

static int checks, errors;
static unsigned apply_calls, draw_calls;
static GPU_Primitive_t last_primitive;
static int last_first, last_size;
static const D9TevProgram *last_program;
static C3D_Tex *last_texture;
static uint32_t last_env_sources;
static GPU_COMBINEFUNC last_env_rgb, last_env_alpha;

#define CHECK(condition, what) do { \
    ++checks; \
    if (!(condition)) { ++errors; printf("FAIL: %s\n", what); } \
} while (0)

static int counted_apply_tev(const D9TevProgram *program, C3D_Tex *texture,
                             char *why, size_t why_size) {
    ++apply_calls;
    last_program = program;
    last_texture = texture;
    int rc = d9_pica_citro3d_apply_tev(program, texture, why, why_size);
    if (!rc) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        if (env) {
            last_env_sources = (uint32_t)env->srcRgb;
            last_env_rgb = (GPU_COMBINEFUNC)env->funcRgb;
            last_env_alpha = (GPU_COMBINEFUNC)env->funcAlpha;
        }
    }
    return rc;
}

static void counted_draw_arrays(GPU_Primitive_t primitive, int first, int size) {
    ++draw_calls;
    last_primitive = primitive;
    last_first = first;
    last_size = size;
    C3D_DrawArrays(primitive, first, size);
}

static int install_readback_target(D9Citro *gpu, C3D_Tex *texture,
                                   C3D_RenderTarget **target,
                                   unsigned *capture_size,
                                   char *reason, size_t reason_size) {
    /* The default target owns colour+depth VRAM.  Release it first so a capture
     * texture and its depth buffer can be allocated.  Try a visible 128x128
     * target, then smaller deterministic targets if VRAM is fragmented. */
    C3D_RenderTargetDetachOutput(gpu->target);
    C3D_RenderTargetDelete(gpu->target);
    gpu->target = NULL;

    static const unsigned sizes[] = {128, 64, 8};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        unsigned size = sizes[i];
        if (!C3D_TexInitVRAM(texture, size, size, GPU_RGBA8)) {
            snprintf(reason, reason_size, "%ux%u VRAM texture allocation unavailable", size, size);
            continue;
        }
        *target = C3D_RenderTargetCreateFromTex(
            texture, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (!*target) {
            C3D_TexDelete(texture);
            snprintf(reason, reason_size, "%ux%u capture target allocation unavailable", size, size);
            continue;
        }
        C3D_RenderTargetSetOutput(*target, GFX_TOP, GFX_LEFT, TRANSFER);
        gpu->target = *target;
        *capture_size = size;
        return 1;
    }
    goto fallback;

fallback:
    *target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8,
                                     GPU_RB_DEPTH24_STENCIL8);
    if (*target) {
        C3D_RenderTargetSetOutput(*target, GFX_TOP, GFX_LEFT, TRANSFER);
        gpu->target = *target;
    }
    return 0;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    printf("ui.copy ARM: integrated COM -> core -> PICA200 draw\n");

    uint32_t code[16];
    size_t words = uicopy_tokens(code);
    D9TevProgram plan;
    char why[256] = {0};
    CHECK(uicopy_validate(code, words, &plan, why, sizeof why) == 0, why);
    CHECK(plan.stages == 1 && plan.textures == 1 && plan.constants == 0 &&
          plan.inputs == 1 && !plan.uses_vertex_color,
          "validated ui.copy plan shape");

    D9Citro *gpu = d9_citro_open();
    CHECK(gpu != NULL, "real d9_citro_open");
    D9Device *core = gpu ? d9_create_device(d9_citro_backend(), gpu, 4u << 20) : NULL;
    CHECK(core != NULL, "d3d9_core adopts real Citro3D backend");

    C3D_Tex capture;
    C3D_RenderTarget *capture_target = NULL;
    unsigned capture_size = 0;
    char capture_reason[160] = {0};
    int capture_ready = gpu && install_readback_target(
        gpu, &capture, &capture_target, &capture_size,
        capture_reason, sizeof capture_reason);
    if (!capture_ready)
        printf("PIXEL_PROOF=NO reason=%s; using screen target\n", capture_reason);

    UICopyCom com;
    memset(&com, 0, sizeof com);
    int attached = core && uicopy_com_attach(&com, core) == 0;
    CHECK(attached, attached ? "guest COM/stdcall attach" : com.error);
    int resources = attached && gpu->target &&
                    uicopy_com_create_resources(&com, code, words) == 0;
    CHECK(resources, resources ? "COM CreatePixelShader/SetPixelShader and resources" : com.error);

    int submitted = resources && uicopy_com_frame(&com) == 0;
    CHECK(submitted, submitted ? "COM DrawPrimitive/EndScene/Present" : com.error);
    CHECK(apply_calls == 1 && draw_calls == 1,
          "one TEV apply and one C3D_DrawArrays");
    CHECK(last_program != NULL && last_program->stages == 1 &&
          last_program->textures == 1 &&
          last_program->stage[0].func_rgb == D9TEV_FUNC_REPLACE,
          "real backend received ui.copy program");
    CHECK(last_texture != NULL, "real TEV apply received unit-0 texture");
    CHECK(last_primitive == GPU_TRIANGLES && last_first == 0 && last_size == 3,
          "one fullscreen triangle submitted");
    CHECK(last_env_sources == GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PREVIOUS, GPU_PREVIOUS) &&
          last_env_rgb == GPU_REPLACE && last_env_alpha == GPU_REPLACE,
          "C3D_TexEnv stage 0 is REPLACE(TEXTURE0)");
    CHECK(gpu && !gpu->scene && gpu->cursor == 3,
          "Present closed the frame after three vertices");

    D9TevProgram retained;
    CHECK(resources && d9_com_pixel_shader_tev(com.shader, &retained),
          "COM retained analyzed TEV");
    CHECK(memcmp(&retained, &plan, sizeof plan) == 0,
          "COM retained program equals ui.copy plan");

    int pixel_status = 0; /* 0 unavailable, 1 pass, -1 mismatch */
    uint32_t pixel_hash = 0;
    char pixel_detail[256] = "not attempted";
    void *readback = NULL;
    if (submitted && capture_ready) {
        C3D_FrameSync();
        const unsigned readback_words = capture_size * capture_size;
        readback = linearAlloc(readback_words * sizeof(uint32_t));
        if (!readback) {
            snprintf(pixel_detail, sizeof pixel_detail, "linear readback allocation unavailable");
        } else {
            uint32_t *words_out = readback;
            for (unsigned i = 0; i < readback_words; ++i) words_out[i] = 0xDEADBEEFu;
            C3D_SyncDisplayTransfer(
                (u32 *)capture.data, GX_BUFFER_DIM(capture_size, capture_size),
                (u32 *)readback, GX_BUFFER_DIM(capture_size, capture_size),
                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                GX_TRANSFER_RAW_COPY(0) |
                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
            GSPGPU_InvalidateDataCache(readback, readback_words * sizeof(uint32_t));
            int untouched = 1;
            for (unsigned i = 0; i < readback_words; ++i)
                if (words_out[i] != 0xDEADBEEFu) { untouched = 0; break; }
            if (untouched) {
                pixel_status = 0;
                snprintf(pixel_detail, sizeof pixel_detail,
                         "display transfer left readback untouched; no pixel proof");
            } else {
                pixel_status = uicopy_pixel_proof((const uint32_t *)readback,
                                                  capture_size, capture_size, 1,
                                                  pixel_detail, sizeof pixel_detail);
                if (pixel_status == 1) {
                    const uint8_t *bytes = readback;
                    pixel_hash = 2166136261u;
                    for (unsigned i = 0; i < readback_words * sizeof(uint32_t); ++i) {
                        pixel_hash ^= bytes[i];
                        pixel_hash *= 16777619u;
                    }
                }
                CHECK(pixel_status == 1, pixel_detail);
            }
        }
    } else if (!capture_ready) {
        snprintf(pixel_detail, sizeof pixel_detail, "%s; state/counter evidence only",
                 capture_reason[0] ? capture_reason : "render-to-texture unavailable");
    }

    FILE *log = fopen("sdmc:/Skyrim_N3DS.log", "w");
    if (log) {
        fprintf(log, "ui.copy ARM: %s (%d checks, %d errors)\n",
                errors ? "FAIL" : "PASS", checks, errors);
        fprintf(log, "path=ps_3_0->d9_shader_analyze_tev->COM CreatePixelShader/SetPixelShader"
                     "->d3d9_core->d3d9_citro3d->C3D_DrawArrays->Present\n");
        fprintf(log, "pipeline=ui.copy stages=%u textures=%u inputs=%u constants=%u\n",
                plan.stages, plan.textures, plan.inputs, plan.constants);
        fprintf(log, "com_create_ps=%d com_set_ps=%d com_draw=%d com_present=%d\n",
                resources, resources, submitted, submitted);
        fprintf(log, "tev_apply_calls=%u draw_calls=%u primitive=%d first=%d size=%d cursor=%u scene=%d\n",
                apply_calls, draw_calls, (int)last_primitive, last_first, last_size,
                gpu ? (unsigned)gpu->cursor : 0u, gpu ? gpu->scene : -1);
        fprintf(log, "texture_to_tev=%d env_sources=0x%08x func_rgb=%d func_a=%d\n",
                last_texture != NULL, (unsigned)last_env_sources,
                (int)last_env_rgb, (int)last_env_alpha);
        fprintf(log, "PIXEL_PROOF=%s capture=%ux%u detail=%s\n",
                pixel_status > 0 ? "PASS" : "NO",
                capture_size, capture_size, pixel_detail);
        if (pixel_status > 0) {
            fprintf(log, "pixel_fnv1a32=%08lx\n", (unsigned long)pixel_hash);
            fprintf(log, "pixel_block_centers=");
            unsigned cell = capture_size / UICOPY_TEX_SIZE;
            for (unsigned y = 0; y < UICOPY_TEX_SIZE; ++y)
                for (unsigned x = 0; x < UICOPY_TEX_SIZE; ++x) {
                    uint32_t value = ((const uint32_t *)readback)[
                        (y * cell + cell / 2u) * capture_size + x * cell + cell / 2u];
                    fprintf(log, "%08lx%s", (unsigned long)value,
                            x + 1u == UICOPY_TEX_SIZE && y + 1u == UICOPY_TEX_SIZE ? "\n" : " ");
                }
        }
        fprintf(log, "scope=synthetic ui.copy only; NOT a Skyrim menu or game frame\n");
        fclose(log);
    }
    printf("ui.copy ARM: %d checks, %d errors; PIXEL_PROOF=%s\n",
           checks, errors, pixel_status > 0 ? "PASS" : "NO");

    /* Keep the presented target alive for the runner's supplemental screenshot.
     * The readback histogram remains the authoritative pixel evidence. */
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

    if (attached) uicopy_com_detach(&com);
    d9_destroy_device(core);
    if (capture_ready) {
        gpu->target = NULL;
        C3D_RenderTargetDelete(capture_target);
        C3D_TexDelete(&capture);
    }
    d9_citro_close(gpu);
    if (readback) linearFree(readback);
    gfxExit();
    return errors ? 1 : 0;
}
