/* Isolated ARM/Azahar render-target switching proof.
 *
 * Real path: guest COM -> d3d9_core -> d3d9_citro3d VRAM render targets ->
 * C3D_DrawArrays -> texture-copy readback.  Two independent targets are
 * cleared, drawn, restored to the implicit back buffer and read back through
 * IDirect3DSurface9::LockRect(READONLY).
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "fixture.h"

static void counted_draw_arrays(GPU_Primitive_t primitive, int first, int size);
#define C3D_DrawArrays counted_draw_arrays
#include "../../../source/d3d9_citro3d.c"
#undef C3D_DrawArrays

static int checks, errors, draw_calls;
#define CHECK(condition, what) do { \
    ++checks; \
    if (!(condition)) { ++errors; printf("FAIL: %s\n", what); } \
} while (0)

static void counted_draw_arrays(GPU_Primitive_t primitive, int first, int size) {
    ++draw_calls;
    C3D_DrawArrays(primitive, first, size);
}

static int create_target(UICopyCom *h, unsigned size, uint32_t *out) {
    uint32_t args[8] = {size, size, D9_A8R8G8B8, 0, 0, 1, UICOPY_OUT, 0};
    return uicopy_com_call(h, h->device, 28, args, 8) || uicopy_read_out(h, out) ? -1 : 0;
}

static int select_target(UICopyCom *h, uint32_t target) {
    uint32_t args[2] = {0, target};
    return uicopy_com_call(h, h->device, 37, args, 2);
}

static int clear_present(UICopyCom *h, uint32_t argb) {
    uint32_t clear[6] = {0, 0, 1, argb, 0, 0};
    uint32_t present[4] = {0, 0, 0, 0};
    return uicopy_com_call(h, h->device, 43, clear, 6) ||
           uicopy_com_call(h, h->device, 41, NULL, 0) ||
           uicopy_com_call(h, h->device, 42, NULL, 0) ||
           uicopy_com_call(h, h->device, 17, present, 4) ? -1 : 0;
}

static int read_target(UICopyCom *h, uint32_t target, const uint32_t **pixels,
                       unsigned *width) {
    uint32_t args[3] = {UICOPY_OUT, 0, 0x10}; /* D3DLOCK_READONLY */
    if (uicopy_com_call_result(h, target, 13, args, 3, NULL)) return -1;
    uint32_t pitch, pointer, w, height;
    if (guest_read32(&h->memory, UICOPY_OUT, &pitch) ||
        guest_read32(&h->memory, UICOPY_OUT + 4, &pointer)) return -1;
    uint32_t desc[1] = {UICOPY_DATA};
    if (uicopy_com_call(h, target, 12, desc, 1) ||
        guest_read32(&h->memory, UICOPY_DATA + 24, &w) ||
        guest_read32(&h->memory, UICOPY_DATA + 28, &height)) return -1;
    const uint32_t *data = guest_ptr(&h->memory, pointer, (size_t)pitch * height);
    if (!data || pitch != w * 4u || w != height) return -1;
    *pixels = data;
    *width = w;
    return 0;
}

static int unlock_target(UICopyCom *h, uint32_t target) {
    return uicopy_com_call(h, target, 14, NULL, 0);
}

static int solid(const uint32_t *pixels, unsigned width, uint32_t argb) {
    for (unsigned i = 0; i < width * width; ++i)
        if (pixels[i] != argb) return 0;
    return 1;
}

static uint32_t pixel_hash(const uint32_t *pixels, unsigned width) {
    const uint8_t *bytes = (const uint8_t *)pixels;
    size_t count = (size_t)width * width * 4u;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    printf("render-target ARM: COM -> core -> PICA switch/readback\n");

    uint32_t code[16];
    size_t words = uicopy_tokens(code);
    D9TevProgram plan;
    char why[256] = {0};
    CHECK(uicopy_validate(code, words, &plan, why, sizeof why) == 0, why);

    D9Citro *gpu = d9_citro_open();
    CHECK(gpu != NULL, "real d9_citro_open");
    D9Device *core = gpu ? d9_create_device(d9_citro_backend(), gpu, 4u << 20) : NULL;
    CHECK(core != NULL, "core adopts real Citro3D backend");
    UICopyCom com;
    memset(&com, 0, sizeof com);
    int attached = core && uicopy_com_attach(&com, core) == 0;
    CHECK(attached, attached ? "guest COM/stdcall attach" : com.error);
    int resources = attached && uicopy_com_create_resources(&com, code, words) == 0;
    CHECK(resources, resources ? "ui.copy resources" : com.error);

    uint32_t target_a = 0, target_b = 0, back = 0, current = 0;
    int targets = resources && create_target(&com, 64, &target_a) == 0 &&
                  create_target(&com, 64, &target_b) == 0;
    CHECK(targets, targets ? "two 128x128 lockable LDR targets" : com.error);
    if (targets) {
        uint32_t args[4] = {0, 0, 0, UICOPY_OUT};
        CHECK(uicopy_com_call(&com, com.device, 18, args, 4) == 0 &&
              uicopy_read_out(&com, &back) == 0 && back != 0,
              "implicit back buffer identity");
    }

    const uint32_t *pixels = NULL;
    unsigned width = 0;
    int red_ok = 0, green_ok = 0, a_unchanged = 0, b_unchanged = 0;
    int restore_ok = 0, identity_ok = 0, pixel_status = 0;
    uint32_t hash = 0;
    char detail[256] = "not attempted";

    if (targets) {
        CHECK(select_target(&com, target_a) == 0 && clear_present(&com, 0xffff0000u) == 0,
              "select A and clear red");
        CHECK(read_target(&com, target_a, &pixels, &width) == 0,
              "LockRect readback A");
        red_ok = pixels && width == 64 && solid(pixels, width, 0xffff0000u);
        CHECK(red_ok, "A readback is solid red");
        if (pixels) CHECK(unlock_target(&com, target_a) == 0, "unlock A");

        CHECK(select_target(&com, 0) == 0 && clear_present(&com, 0xff0000ffu) == 0,
              "restore implicit back buffer and clear blue");
        CHECK(select_target(&com, target_b) == 0 && clear_present(&com, 0xff00ff00u) == 0,
              "select B and clear green");
        CHECK(read_target(&com, target_b, &pixels, &width) == 0,
              "LockRect readback B");
        green_ok = pixels && width == 64 && solid(pixels, width, 0xff00ff00u);
        CHECK(green_ok, "B readback is solid green");
        if (pixels) CHECK(unlock_target(&com, target_b) == 0, "unlock B");
        CHECK(read_target(&com, target_a, &pixels, &width) == 0,
              "independent readback A after B");
        a_unchanged = pixels && solid(pixels, width, 0xffff0000u);
        CHECK(a_unchanged, "B clear/draw did not alias A");
        if (pixels) CHECK(unlock_target(&com, target_a) == 0, "unlock A again");

        CHECK(select_target(&com, target_a) == 0, "reselect A for ui.copy");
        uint32_t clear[6] = {0, 0, 1, 0xff000000u, 0, 0};
        CHECK(uicopy_com_call(&com, com.device, 43, clear, 6) == 0 &&
              uicopy_com_frame(&com) == 0,
              "ui.copy draws only into A");
        CHECK(read_target(&com, target_a, &pixels, &width) == 0,
              "LockRect readback drawn A");
        if (pixels) {
            pixel_status = uicopy_pixel_proof(pixels, width, width, 0,
                                              detail, sizeof detail);
            hash = pixel_hash(pixels, width);
        }
        CHECK(pixel_status == 1, detail);
        if (pixels) CHECK(unlock_target(&com, target_a) == 0, "unlock drawn A");
        CHECK(read_target(&com, target_b, &pixels, &width) == 0,
              "independent readback B after A draw");
        b_unchanged = pixels && solid(pixels, width, 0xff00ff00u);
        CHECK(b_unchanged, "A draw did not alias B");
        if (pixels) CHECK(unlock_target(&com, target_b) == 0, "unlock B again");

        CHECK(select_target(&com, 0) == 0, "SetRenderTarget(NULL) restore");
        uint32_t get[2] = {0, UICOPY_OUT};
        CHECK(uicopy_com_call(&com, com.device, 38, get, 2) == 0 &&
              uicopy_read_out(&com, &current) == 0 && current == back,
              "GetRenderTarget reports implicit back buffer");
        if (current) CHECK(uicopy_com_release_result(&com, current, NULL) == 0,
                           "release GetRenderTarget reference");
        CHECK(select_target(&com, target_a) == 0, "explicit target identity select");
        get[1] = UICOPY_OUT;
        CHECK(uicopy_com_call(&com, com.device, 38, get, 2) == 0 &&
              uicopy_read_out(&com, &current) == 0 && current == target_a,
              "GetRenderTarget reports A identity");
        if (current) CHECK(uicopy_com_release_result(&com, current, NULL) == 0,
                           "release A identity reference");
        CHECK(select_target(&com, 0) == 0, "restore before release");
        CHECK(uicopy_com_release_result(&com, target_a, NULL) == 0 &&
              uicopy_com_release_result(&com, target_b, NULL) == 0,
              "release both targets");
        restore_ok = 1;
        identity_ok = 1;
    }

    CHECK(gpu && !gpu->scene && gpu->active_target == NULL,
          "backend closed the frame and cleared explicit-target bookkeeping");
    CHECK(draw_calls == 1, "one real C3D_DrawArrays submission");
    CHECK(red_ok && green_ok && a_unchanged && b_unchanged && restore_ok && identity_ok,
          "all target-switch invariants");
    printf("RT_SWITCH=%s checks=%d errors=%d draw_calls=%d\n",
           errors ? "FAIL" : "PASS", checks, errors, draw_calls);
    printf("PIXEL_PROOF=%s target=64x64 detail=%s pixel_fnv1a32=%08lx\n",
           pixel_status > 0 ? "PASS" : "NO", detail, (unsigned long)hash);
    printf("scope=synthetic render-target switch only; NOT a Skyrim frame\n");

    FILE *log = fopen("sdmc:/Skyrim_N3DS.log", "w");
    if (log) {
        fprintf(log, "render-target ARM: %s (%d checks, %d errors)\n",
                errors ? "FAIL" : "PASS", checks, errors);
        fprintf(log, "path=COM CreateRenderTarget/SetRenderTarget/Clear/LockRect->d3d9_core"
                     "->d3d9_citro3d VRAM targets->C3D_DrawArrays\n");
        fprintf(log, "target_a=0x%08lx target_b=0x%08lx back=0x%08lx size=64x64\n",
                (unsigned long)target_a, (unsigned long)target_b, (unsigned long)back);
        fprintf(log, "com_error=%s\n", com.error);
        fprintf(log, "red=%d green=%d a_unchanged=%d b_unchanged=%d restore=%d identity=%d\n",
                red_ok, green_ok, a_unchanged, b_unchanged, restore_ok, identity_ok);
        fprintf(log, "draw_calls=%d scene=%d active_target=%d\n", draw_calls,
                gpu ? gpu->scene : -1, gpu && gpu->active_target ? 1 : 0);
        fprintf(log, "PIXEL_PROOF=%s detail=%s pixel_fnv1a32=%08lx\n",
                pixel_status > 0 ? "PASS" : "NO", detail, (unsigned long)hash);
        fprintf(log, "scope=synthetic render-target switch only; NOT a Skyrim frame\n");
        fclose(log);
    }

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

    if (attached) uicopy_com_detach(&com);
    d9_destroy_device(core);
    d9_citro_close(gpu);
    gfxExit();
    return errors ? 1 : 0;
}
