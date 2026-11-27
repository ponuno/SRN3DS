/* Full adapter capability matrix: every answer and its reason.  The matrix is
 * the shippable contract of d3d9_adapter.c, not a fake "S_OK for everything"
 * table: unsupported combinations must be D3DERR_NOTAVAILABLE with a reason,
 * and DevCaps must stay 0x00082000. */
#include "d3d9_adapter.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RT_SURFACE=1, RT_TEXTURE=3, RT_VOLUME=4, RT_CUBE=5 };
enum { USAGE_RT=0x00000001u, USAGE_DS=0x00000002u, USAGE_DYNAMIC=0x00000200u,
       USAGE_QF=0x00020000u, USAGE_QW=0x00200000u, USAGE_SRGBREAD=0x00010000u,
       USAGE_POSTPIXEL=0x00080000u };
enum { FMT_A8R8G8B8=21u, FMT_X8R8G8B8=22u, FMT_A8=28u,
       FMT_DXT1=0x31545844u, FMT_DXT3=0x33545844u, FMT_DXT5=0x35545844u,
       FMT_ATI1=0x31495441u, FMT_BC4U=0x55344342u,
       FMT_ATI2=0x32495441u, FMT_BC5U=0x55354342u,
       FMT_D24S8=75u, FMT_D24X8=77u, FMT_D16=80u,
       FMT_R16F=0x6fu, FMT_F16=0x71u, FMT_F32=0x72u };

static unsigned cases, failures;

#define CHECKM(cond, ...) do { \
    if (!(cond)) { \
        ++failures; \
        fprintf(stderr, "check_format_matrix: FAIL line %d: ", __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)
#define CHECK(cond) do { \
    if (!(cond)) { \
        ++failures; \
        fprintf(stderr, "check_format_matrix: FAIL line %d: %s\n", \
                __LINE__, #cond); \
    } \
} while (0)

static void expect_format(const char *name, unsigned adapter, unsigned type,
                          unsigned adapter_format, unsigned usage,
                          unsigned resource_type, unsigned format,
                          int result, const char *needle) {
    ++cases;
    D9AdapterAnswer a = d9_adapter_answer_device_format(adapter, type,
        adapter_format, usage, resource_type, format);
    CHECKM(a.result == result, "%s: result %d expected %d (reason %s)", name,
          a.result, result, a.reason ? a.reason : "<null>");
    CHECKM(a.reason && *a.reason, "%s: empty reason", name);
    if (needle)
        CHECKM(a.reason && strstr(a.reason, needle),
              "%s: reason '%s' lacks '%s'", name,
              a.reason ? a.reason : "<null>", needle);
}

static void expect_depth(const char *name, unsigned display, unsigned back,
                         unsigned depth, int result, const char *needle) {
    ++cases;
    D9AdapterAnswer a = d9_adapter_answer_depth_stencil(0, 1, display, back,
                                                        depth);
    CHECKM(a.result == result, "%s: result %d expected %d (reason %s)", name,
          a.result, result, a.reason ? a.reason : "<null>");
    CHECKM(a.reason && *a.reason, "%s: empty reason", name);
    if (needle)
        CHECKM(a.reason && strstr(a.reason, needle), "%s: reason '%s' lacks '%s'",
              name, a.reason ? a.reason : "<null>", needle);
}

static void expect_type(const char *name, unsigned adapter, unsigned type,
                        unsigned display, unsigned back, int windowed,
                        int result, const char *needle) {
    ++cases;
    D9AdapterAnswer a = d9_adapter_answer_device_type(adapter, type, display,
                                                      back, windowed);
    CHECKM(a.result == result, "%s: result %d expected %d (reason %s)", name,
          a.result, result, a.reason ? a.reason : "<null>");
    CHECKM(a.reason && *a.reason, "%s: empty reason", name);
    if (needle)
        CHECKM(a.reason && strstr(a.reason, needle), "%s: reason '%s' lacks '%s'",
              name, a.reason ? a.reason : "<null>", needle);
}

static void expect_conversion(const char *name, unsigned source,
                              unsigned target, int result, const char *needle) {
    ++cases;
    D9AdapterAnswer a = d9_adapter_answer_format_conversion(0, 1, source,
                                                            target);
    CHECKM(a.result == result, "%s: result %d expected %d (reason %s)", name,
          a.result, result, a.reason ? a.reason : "<null>");
    CHECKM(a.reason && *a.reason, "%s: empty reason", name);
    if (needle)
        CHECKM(a.reason && strstr(a.reason, needle), "%s: reason '%s' lacks '%s'",
              name, a.reason ? a.reason : "<null>", needle);
}

int main(void) {
    CHECK(!_putenv_s("TESV_D3D9_CAPS_PROBE", "0"));
    CHECK(!_putenv_s("TESV_D3D9_HDR_LDR_FALLBACK", "0"));

    D9Caps9 caps_before, caps_after;
    CHECK(d9_adapter_caps(0, 1, &caps_before) == D9_ADAPTER_OK);
    CHECK(caps_before.dev_caps == 0x00082000u);
    CHECK(caps_before.vertex_shader_version == 0xfffe0200u);
    CHECK(caps_before.pixel_shader_version == 0xffff0200u);
    CHECK(caps_before.number_of_adapters_in_group == 1u);

    /* Adapter/device/adapter-format validation. */
    expect_format("bad adapter", 1, 1, FMT_X8R8G8B8, 0, RT_TEXTURE, FMT_X8R8G8B8,
                  D9_ADAPTER_INVALID, "adapter");
    expect_format("bad device type", 0, 2, FMT_X8R8G8B8, 0, RT_TEXTURE,
                  FMT_X8R8G8B8, D9_ADAPTER_INVALID, "device type");
    expect_format("bad adapter format", 0, 1, FMT_A8R8G8B8, 0, RT_TEXTURE,
                  FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "adapter format");
    expect_format("unknown resource type", 0, 1, FMT_X8R8G8B8, 0, 2,
                  FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "resource type");

    /* Surfaces: plain, depth-stencil usage, render target. */
    expect_format("surface X8 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_X8R8G8B8, D9_ADAPTER_OK, "surface");
    expect_format("surface A8 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_A8R8G8B8, D9_ADAPTER_OK, "surface");
    expect_format("surface D24S8 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_D24S8, D9_ADAPTER_OK, "depth-stencil");
    expect_format("surface D24X8 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_D24X8, D9_ADAPTER_OK, "depth-stencil");
    expect_format("surface D16 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_D16, D9_ADAPTER_UNAVAILABLE, "surface format");
    expect_format("surface A8 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_A8, D9_ADAPTER_UNAVAILABLE, "surface format");
    expect_format("surface DXT1 plain", 0, 1, FMT_X8R8G8B8, 0, RT_SURFACE,
                  FMT_DXT1, D9_ADAPTER_UNAVAILABLE, "surface format");
    expect_format("surface X8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_SURFACE,
                  FMT_X8R8G8B8, D9_ADAPTER_OK, "render-target surface");
    expect_format("surface A8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_SURFACE,
                  FMT_A8R8G8B8, D9_ADAPTER_OK, "render-target surface");
    expect_format("surface D24S8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_SURFACE,
                  FMT_D24S8, D9_ADAPTER_UNAVAILABLE, "no RENDERTARGET");
    expect_format("surface DXT1 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_SURFACE,
                  FMT_DXT1, D9_ADAPTER_UNAVAILABLE, "render-target surfaces");
    expect_format("surface RT+DYNAMIC", 0, 1, FMT_X8R8G8B8,
                  USAGE_RT | USAGE_DYNAMIC, RT_SURFACE, FMT_X8R8G8B8,
                  D9_ADAPTER_UNAVAILABLE, "surface usage");
    expect_format("surface D24S8 DS", 0, 1, FMT_X8R8G8B8, USAGE_DS, RT_SURFACE,
                  FMT_D24S8, D9_ADAPTER_OK, "DEPTHSTENCIL");
    expect_format("surface D24X8 DS", 0, 1, FMT_X8R8G8B8, USAGE_DS, RT_SURFACE,
                  FMT_D24X8, D9_ADAPTER_OK, "DEPTHSTENCIL");
    expect_format("surface D16 DS", 0, 1, FMT_X8R8G8B8, USAGE_DS, RT_SURFACE,
                  FMT_D16, D9_ADAPTER_UNAVAILABLE, "DEPTHSTENCIL");
    expect_format("surface X8 DS", 0, 1, FMT_X8R8G8B8, USAGE_DS, RT_SURFACE,
                  FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "DEPTHSTENCIL");

    /* 2D textures: sampler formats and usages. */
    const unsigned tex2d[] = {FMT_A8R8G8B8,FMT_X8R8G8B8,FMT_A8,FMT_DXT1,FMT_DXT3,FMT_DXT5,
                              FMT_ATI1,FMT_BC4U,FMT_ATI2,FMT_BC5U};
    for (unsigned i=0;i<sizeof tex2d/sizeof *tex2d;++i)
        expect_format("texture 2D plain", 0, 1, FMT_X8R8G8B8, 0, RT_TEXTURE,
                      tex2d[i], D9_ADAPTER_OK, "2D texture");
    expect_format("texture D24S8", 0, 1, FMT_X8R8G8B8, 0, RT_TEXTURE,
                  FMT_D24S8, D9_ADAPTER_UNAVAILABLE, "2D texture format");
    expect_format("texture 0x71 no-hdr", 0, 1, FMT_X8R8G8B8, 0, RT_TEXTURE,
                  FMT_F16, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("texture 0x72 no-hdr", 0, 1, FMT_X8R8G8B8, 0, RT_TEXTURE,
                  FMT_F32, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("texture R16F no-hdr", 0, 1, FMT_X8R8G8B8, 0, RT_TEXTURE,
                  FMT_R16F, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("texture X8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_TEXTURE,
                  FMT_X8R8G8B8, D9_ADAPTER_OK, "render-target texture");
    expect_format("texture A8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_TEXTURE,
                  FMT_A8R8G8B8, D9_ADAPTER_OK, "render-target texture");
    expect_format("texture 0x71 RT no-hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT,
                  RT_TEXTURE, FMT_F16, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("texture R16F RT no-hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT,
                  RT_TEXTURE, FMT_R16F, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("texture DXT1 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_TEXTURE,
                  FMT_DXT1, D9_ADAPTER_UNAVAILABLE, "render-target textures");
    expect_format("texture A8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_TEXTURE,
                  FMT_A8, D9_ADAPTER_UNAVAILABLE, "render-target textures");
    expect_format("texture X8 DYNAMIC", 0, 1, FMT_X8R8G8B8, USAGE_DYNAMIC,
                  RT_TEXTURE, FMT_X8R8G8B8, D9_ADAPTER_OK, "DYNAMIC");
    expect_format("texture DXT1 DYNAMIC", 0, 1, FMT_X8R8G8B8, USAGE_DYNAMIC,
                  RT_TEXTURE, FMT_DXT1, D9_ADAPTER_OK, "DYNAMIC");
    expect_format("texture X8 QF+QW", 0, 1, FMT_X8R8G8B8,
                  USAGE_QF | USAGE_QW, RT_TEXTURE, FMT_X8R8G8B8,
                  D9_ADAPTER_OK, "QUERY");
    expect_format("texture 0x71 QF", 0, 1, FMT_X8R8G8B8, USAGE_QF, RT_TEXTURE,
                  FMT_F16, D9_ADAPTER_UNAVAILABLE, "sampled float");
    expect_format("texture SRGBREAD", 0, 1, FMT_X8R8G8B8, USAGE_SRGBREAD,
                  RT_TEXTURE, FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "usage");
    expect_format("texture POSTPIXEL", 0, 1, FMT_X8R8G8B8, USAGE_POSTPIXEL,
                  RT_TEXTURE, FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "usage");
    expect_format("texture RT+DYNAMIC", 0, 1, FMT_X8R8G8B8,
                  USAGE_RT | USAGE_DYNAMIC, RT_TEXTURE, FMT_X8R8G8B8,
                  D9_ADAPTER_UNAVAILABLE, "usage");

    /* Cubes: non-RT formats, RT formats, dynamic/query, A8 refusal. */
    const unsigned cube_nonrt[] = {FMT_A8R8G8B8,FMT_X8R8G8B8,FMT_DXT1,FMT_DXT3,FMT_DXT5,
                                   FMT_ATI2,FMT_BC5U};
    for (unsigned i=0;i<sizeof cube_nonrt/sizeof *cube_nonrt;++i)
        expect_format("cube plain", 0, 1, FMT_X8R8G8B8, 0, RT_CUBE,
                      cube_nonrt[i], D9_ADAPTER_OK, "cube");
    expect_format("cube A8", 0, 1, FMT_X8R8G8B8, 0, RT_CUBE, FMT_A8,
                  D9_ADAPTER_UNAVAILABLE, "A8 cube");
    expect_format("cube 0x71 no-hdr", 0, 1, FMT_X8R8G8B8, 0, RT_CUBE,
                  FMT_F16, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("cube X8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_X8R8G8B8, D9_ADAPTER_OK, "cube render-target");
    expect_format("cube A8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_A8R8G8B8, D9_ADAPTER_OK, "cube render-target");
    expect_format("cube 0x71 RT no-hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_F16, D9_ADAPTER_UNAVAILABLE, "HDR_LDR_FALLBACK");
    expect_format("cube DXT1 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_DXT1, D9_ADAPTER_UNAVAILABLE, "cube render targets");
    expect_format("cube X8 DYNAMIC", 0, 1, FMT_X8R8G8B8, USAGE_DYNAMIC,
                  RT_CUBE, FMT_X8R8G8B8, D9_ADAPTER_OK, "cube");

    /* Volumes: non-RT only. */
    const unsigned vol_formats[] = {FMT_A8R8G8B8,FMT_X8R8G8B8,FMT_DXT1,FMT_DXT3,FMT_DXT5,
                                    FMT_ATI2,FMT_BC5U};
    for (unsigned i=0;i<sizeof vol_formats/sizeof *vol_formats;++i)
        expect_format("volume plain", 0, 1, FMT_X8R8G8B8, 0, RT_VOLUME,
                      vol_formats[i], D9_ADAPTER_OK, "volume");
    expect_format("volume A8", 0, 1, FMT_X8R8G8B8, 0, RT_VOLUME, FMT_A8,
                  D9_ADAPTER_UNAVAILABLE, "A8 volume");
    expect_format("volume D24S8", 0, 1, FMT_X8R8G8B8, 0, RT_VOLUME,
                  FMT_D24S8, D9_ADAPTER_UNAVAILABLE, "volume format");
    expect_format("volume 0x71", 0, 1, FMT_X8R8G8B8, 0, RT_VOLUME,
                  FMT_F16, D9_ADAPTER_UNAVAILABLE, "volume format");
    expect_format("volume X8 DYNAMIC", 0, 1, FMT_X8R8G8B8, USAGE_DYNAMIC,
                  RT_VOLUME, FMT_X8R8G8B8, D9_ADAPTER_OK, "volume");
    expect_format("volume X8 RT", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_VOLUME,
                  FMT_X8R8G8B8, D9_ADAPTER_UNAVAILABLE, "volume");

    /* HDR LDR fallback: cube/surface/texture descriptors and render targets. */
    CHECK(!_putenv_s("TESV_D3D9_HDR_LDR_FALLBACK", "1"));
    expect_format("surface 0x71 descriptor hdr", 0, 1, FMT_X8R8G8B8, 0,
                  RT_SURFACE, FMT_F16, D9_ADAPTER_OK, "LDR fallback");
    expect_format("texture 0x71 descriptor hdr", 0, 1, FMT_X8R8G8B8, 0,
                  RT_TEXTURE, FMT_F16, D9_ADAPTER_OK, "capability descriptor");
    expect_format("cube 0x71 descriptor hdr", 0, 1, FMT_X8R8G8B8, 0, RT_CUBE,
                  FMT_F16, D9_ADAPTER_OK, "capability descriptor");
    expect_format("surface 0x71 RT hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT,
                  RT_SURFACE, FMT_F16, D9_ADAPTER_OK, "LDR fallback");
    expect_format("texture 0x71 RT hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT,
                  RT_TEXTURE, FMT_F16, D9_ADAPTER_OK, "LDR fallback");
    expect_format("cube 0x71 RT hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_F16, D9_ADAPTER_OK, "LDR fallback");
    expect_format("cube 0x72 RT hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT, RT_CUBE,
                  FMT_F32, D9_ADAPTER_OK, "LDR fallback");
    expect_format("texture R16F RT hdr", 0, 1, FMT_X8R8G8B8, USAGE_RT,
                  RT_TEXTURE, FMT_R16F, D9_ADAPTER_OK, "LDR fallback");
    expect_format("cube BC5U plain", 0, 1, FMT_X8R8G8B8, 0, RT_CUBE,
                  FMT_BC5U, D9_ADAPTER_OK, "cube");

    /* Depth-stencil match. */
    expect_depth("depth match X8/X8/D24S8", FMT_X8R8G8B8, FMT_X8R8G8B8,
                 FMT_D24S8, D9_ADAPTER_OK, "DEPTH24_STENCIL8");
    expect_depth("depth match X8/A8/D24X8", FMT_X8R8G8B8, FMT_A8R8G8B8,
                 FMT_D24X8, D9_ADAPTER_OK, "DEPTH24_STENCIL8");
    expect_depth("depth match bad display", FMT_A8R8G8B8, FMT_X8R8G8B8,
                 FMT_D24S8, D9_ADAPTER_UNAVAILABLE, "display format");
    expect_depth("depth match D16", FMT_X8R8G8B8, FMT_X8R8G8B8, FMT_D16,
                 D9_ADAPTER_UNAVAILABLE, "D24S8/D24X8");
    expect_depth("depth match bad backbuffer", FMT_X8R8G8B8, FMT_D16,
                 FMT_D24S8, D9_ADAPTER_UNAVAILABLE, "back buffer");

    /* Device type. */
    expect_type("windowed X8/X8", 0, 1, FMT_X8R8G8B8, FMT_X8R8G8B8, 1,
                D9_ADAPTER_OK, "windowed");
    expect_type("windowed X8/A8", 0, 1, FMT_X8R8G8B8, FMT_A8R8G8B8, 1,
                D9_ADAPTER_OK, "windowed");
    expect_type("windowed X8/UNKNOWN", 0, 1, FMT_X8R8G8B8, 0, 1,
                D9_ADAPTER_OK, "windowed");
    expect_type("exclusive", 0, 1, FMT_X8R8G8B8, FMT_X8R8G8B8, 0,
                D9_ADAPTER_UNAVAILABLE, "exclusive");
    expect_type("bad display", 0, 1, FMT_A8R8G8B8, FMT_X8R8G8B8, 1,
                D9_ADAPTER_UNAVAILABLE, "display format");
    expect_type("bad backbuffer", 0, 1, FMT_X8R8G8B8, 23, 1,
                D9_ADAPTER_UNAVAILABLE, "back buffer");

    /* Format conversion: only same-format X8/A8 copies. */
    expect_conversion("X8->X8", FMT_X8R8G8B8, FMT_X8R8G8B8, D9_ADAPTER_OK,
                      "same-format");
    expect_conversion("A8->A8", FMT_A8R8G8B8, FMT_A8R8G8B8, D9_ADAPTER_OK,
                      "same-format");
    expect_conversion("X8->A8", FMT_X8R8G8B8, FMT_A8R8G8B8,
                      D9_ADAPTER_UNAVAILABLE, "conversion");
    expect_conversion("A8->X8", FMT_A8R8G8B8, FMT_X8R8G8B8,
                      D9_ADAPTER_UNAVAILABLE, "conversion");
    expect_conversion("DXT1->X8", FMT_DXT1, FMT_X8R8G8B8,
                      D9_ADAPTER_UNAVAILABLE, "conversion");

    /* DevCaps must be byte-identical after the whole matrix. */
    CHECK(d9_adapter_caps(0, 1, &caps_after) == D9_ADAPTER_OK);
    CHECK(!memcmp(&caps_before, &caps_after, sizeof caps_before));
    CHECK(caps_after.dev_caps == 0x00082000u);

    CHECK(!_putenv_s("TESV_D3D9_HDR_LDR_FALLBACK", "0"));
    if (failures) {
        fprintf(stderr, "check_format_matrix: %u/%u cases failed\n", failures,
                cases);
        return 1;
    }
    printf("PASS check_format_matrix (%u cases)\n", cases);
    return 0;
}
