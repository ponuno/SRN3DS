#include "d3d9_core.h"
#include "d3d9_renderer_requirements.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The module repeats D3D9 numbers so it has no link dependency; keep the LDR
 * values pinned to the values the real core uses. */
_Static_assert(D9REQ_FMT_A8R8G8B8 == D9_A8R8G8B8, "A8R8G8B8 must match d3d9_core");
_Static_assert(D9REQ_FMT_X8R8G8B8 == D9_X8R8G8B8, "X8R8G8B8 must match d3d9_core");
_Static_assert(D9REQ_FMT_DXT1 == D9_DXT1, "DXT1 must match d3d9_core");
_Static_assert(D9REQ_FMT_DXT3 == D9_DXT3, "DXT3 must match d3d9_core");
_Static_assert(D9REQ_FMT_DXT5 == D9_DXT5, "DXT5 must match d3d9_core");

static D9ReqVerdict eval(const D9ReqRequest *request) {
    D9ReqVerdict verdict;
    assert(d9_req_evaluate(request, &verdict) == 0);
    assert(verdict.reason[0]);
    if (verdict.decision == D9REQ_FALLBACK) assert(verdict.contract[0]);
    return verdict;
}

static void expect(const D9ReqRequest *request, uint32_t decision, uint32_t answer,
                   uint32_t requirement) {
    D9ReqVerdict verdict = eval(request);
    if(verdict.decision!=decision||verdict.answer!=answer||verdict.requirement!=requirement)
        fprintf(stderr,"expect mismatch: got decision=%u answer=0x%x req=%s (%s)\n",
                verdict.decision,verdict.answer,d9_req_requirement_name(verdict.requirement),verdict.reason);
    assert(verdict.decision == decision);
    assert(verdict.answer == answer);
    assert(verdict.requirement == requirement);
}

static size_t probe(const D9ReqRequest *request, const D9ReqVerdict *verdict,
                    char *buf, size_t cap) {
    return d9_req_format_probe(request, verdict, buf, cap);
}

static void matrix(void) {
    char why[128];
    unsigned i;
    assert(d9_req_matrix_validate(why, sizeof why) == 0);
    assert(d9_req_rule_count() >= 25);
    for (i = 0; i < D9REQ_ID_COUNT; ++i) {
        const char *name = d9_req_requirement_name(i);
        assert(name && name[0]);
        assert(strcmp(name, "unknown.requirement") != 0);
    }
    for (i = 0; i <= D9REQ_INVALID; ++i) {
        assert(d9_req_decision_name(i));
    }
    for (i = 0; i <= D9REQ_ANSWER_INVALIDCALL; ++i) {
        assert(d9_req_answer_name(i));
    }
    for (i = D9REQ_CAP_SHADER_MODEL; i <= D9REQ_CAP_SM3_SLOTS; ++i) {
        assert(strcmp(d9_req_cap_name(i), "unknown") != 0);
    }
}

static void accepted_ldr_menu(void) {
    D9ReqRequest q;
    unsigned i;
    static const uint32_t formats[] = {
        D9REQ_FMT_A8R8G8B8, D9REQ_FMT_X8R8G8B8,
        D9REQ_FMT_DXT1, D9REQ_FMT_DXT3, D9REQ_FMT_DXT5
    };
    static const uint32_t usages[] = {
        0,
        D9REQ_USAGE_QUERY_FILTER,
        D9REQ_USAGE_QUERY_WRAPANDMIP,
        D9REQ_USAGE_QUERY_FILTER | D9REQ_USAGE_QUERY_WRAPANDMIP,
    };

    q = d9_req_check_device_type(D9REQ_DEVICE_HAL, D9REQ_FMT_X8R8G8B8,
                                 D9REQ_FMT_X8R8G8B8, 1);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR);
    q.backbuffer_format = D9REQ_FMT_A8R8G8B8;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR);
    q.backbuffer_format = D9REQ_FMT_UNKNOWN;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR);
    q.windowed = 2; /* D3D BOOL semantics: non-zero is TRUE */
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR);

    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_X8R8G8B8);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_SURFACE_LDR);
    q.format = D9REQ_FMT_A8R8G8B8;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_SURFACE_LDR);

    /* The one usage-bearing LDR surface in the contract: exactly
     * D3DUSAGE_RENDERTARGET on an X8/A8 surface. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_X8R8G8B8);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_RENDER_TARGET_LDR);
    q.format = D9REQ_FMT_A8R8G8B8;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_RENDER_TARGET_LDR);

    {
        D9ReqVerdict v;
        char line[D9REQ_LOG_MAX];
        v = eval(&q);
        assert(probe(&q, &v, line, sizeof line) < sizeof line);
        assert(strstr(line, "req=check_device_format") != NULL);
        assert(strstr(line, "usage=0x00000001(RENDERTARGET)") != NULL);
        assert(strstr(line, "resource=surface") != NULL);
        assert(strstr(line, "format=A8R8G8B8") != NULL);
        assert(strstr(line, "claim=supported") != NULL);
        assert(strstr(line, "d3d9=OK") != NULL);
        assert(strstr(line, "id=format.render_target_ldr") != NULL);
        assert(strstr(line, "contract=") == NULL);
        assert(!strcmp(d9_req_requirement_name(D9REQ_ID_FORMAT_RENDER_TARGET_LDR),
                       "format.render_target_ldr"));
    }

    for (i = 0; i < sizeof formats / sizeof formats[0]; ++i) {
        unsigned u;
        for (u = 0; u < sizeof usages / sizeof usages[0]; ++u) {
            q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, usages[u],
                                           D9REQ_RESOURCE_TEXTURE, formats[i]);
            expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_TEXTURE_LDR);
        }
    }

    q = d9_req_device_caps(D9REQ_CAP_SHADER_MODEL, D9REQ_SHADER_MODEL(2, 0));
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_SM2_TEMPLATES);
    q = d9_req_device_caps(D9REQ_CAP_RENDER_TARGETS, 1);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_MRT_SUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_TEXTURE_STAGES, 1);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_STAGES_SUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_TEXTURE_SIZE, 1024);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_TEXTURE_SIZE_SUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_VS_CONSTANTS, 4);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_VS_CONSTANTS_SUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_DEPTH_TEST, 1);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_DEPTH_TEST_SUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_SM3_SLOTS, 0);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_CAPS_SM3_SLOTS_SUPPORTED);

    {
        D9ReqVerdict v;
        char line[D9REQ_LOG_MAX];
        q = d9_req_check_device_type(D9REQ_DEVICE_HAL, D9REQ_FMT_X8R8G8B8,
                                     D9REQ_FMT_X8R8G8B8, 1);
        v = eval(&q);
        assert(probe(&q, &v, line, sizeof line) < sizeof line);
        assert(strstr(line, "req=check_device_type") != NULL);
        assert(strstr(line, "claim=supported") != NULL);
        assert(strstr(line, "d3d9=OK") != NULL);
        assert(strstr(line, "id=device_type.windowed_ldr") != NULL);
        assert(strstr(line, "contract=") == NULL);
    }
}

static void rejected_hdr_and_formats(void) {
    D9ReqRequest q;
    D9ReqVerdict v;
    unsigned i;
    static const uint32_t depths[] = {
        D9REQ_FMT_D15S1, D9REQ_FMT_D16, D9REQ_FMT_D16_LOCKABLE,
        D9REQ_FMT_D24S8, D9REQ_FMT_D24X8, D9REQ_FMT_D24X4S4,
        D9REQ_FMT_D24FS8, D9REQ_FMT_D32, D9REQ_FMT_D32F_LOCKABLE,
        D9REQ_FMT_D32_LOCKABLE, D9REQ_FMT_S8_LOCKABLE
    };

    /* The recorded init probe: format 0x71, usage 0x00080000.  The docs label
     * 0x80000 as SRGBREAD, but the SDK defines it as post-pixel-shader
     * blending; either way A16B16G16R16F itself is not implemented. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A16B16G16R16F);
    v = eval(&q);
    assert(v.decision == D9REQ_UNSUPPORTED);
    assert(v.answer == D9REQ_ANSWER_NOTAVAILABLE);
    assert(v.requirement == D9REQ_ID_FORMAT_FLOAT16_HDR);
    assert(strstr(v.reason, "A16B16G16R16F") != NULL);
    assert(strstr(v.contract, "LDR") != NULL);

    {
        char log[D9REQ_LOG_MAX];
        size_t n = probe(&q, &v, log, sizeof log);
        assert(n == strlen(log));
        assert(n > 0 && n < sizeof log);
        assert(strstr(log, "format=A16B16G16R16F") != NULL);
        assert(strstr(log, "usage=0x00080000(QUERY_POSTPIXELSHADER_BLENDING)") != NULL);
        assert(strstr(log, "claim=unsupported") != NULL);
        assert(strstr(log, "d3d9=NOTAVAILABLE") != NULL);
        assert(strstr(log, "id=format.a16b16g16r16f") != NULL);
        assert(strstr(log, "contract=") != NULL);
    }

    /* Float loses even when the query bit is the real SRGBREAD bit. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_QUERY_SRGBREAD,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A16B16G16R16F);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_FLOAT16_HDR);
    q.resource_type = D9REQ_RESOURCE_SURFACE;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_FLOAT16_HDR);

    /* sRGB read is a documented CPU fallback, never a fake D3D9 success. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_QUERY_SRGBREAD,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    v = eval(&q);
    assert(v.decision == D9REQ_FALLBACK);
    assert(v.answer == D9REQ_ANSWER_NOTAVAILABLE);
    assert(v.requirement == D9REQ_ID_FORMAT_SRGB_READ);
    assert(strstr(v.reason, "sRGB") != NULL);
    assert(strstr(v.contract, "CPU") != NULL);
    assert(strstr(v.contract, "upload") != NULL);
    q.usage = D9REQ_USAGE_QUERY_SRGBREAD | D9REQ_USAGE_QUERY_FILTER;
    expect(&q, D9REQ_FALLBACK, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_SRGB_READ);
    q.usage = D9REQ_USAGE_QUERY_SRGBREAD | D9REQ_USAGE_RENDERTARGET;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);

    q.usage = D9REQ_USAGE_QUERY_SRGBWRITE;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_SRGB_WRITE);

    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_ATOC);
    v = eval(&q);
    assert(v.decision == D9REQ_FALLBACK);
    assert(v.answer == D9REQ_ANSWER_NOTAVAILABLE);
    assert(v.requirement == D9REQ_ID_FORMAT_ATOC);
    assert(strstr(v.contract, "alpha test") != NULL);
    assert(strstr(v.contract, "coverage") != NULL);

    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_RESZ);
    v = eval(&q);
    assert(v.decision == D9REQ_UNSUPPORTED);
    assert(v.answer == D9REQ_ANSWER_NOTAVAILABLE);
    assert(v.requirement == D9REQ_ID_FORMAT_RESZ);
    assert(strstr(v.reason, "resolve") != NULL);

    for (i = 0; i < sizeof depths / sizeof depths[0]; ++i) {
        q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                       D9REQ_RESOURCE_TEXTURE, depths[i]);
        expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_DEPTH);
        q.usage = D9REQ_USAGE_DEPTHSTENCIL;
        expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_DEPTH);
    }

    q = d9_req_check_depth_stencil(D9REQ_FMT_X8R8G8B8, D9REQ_FMT_X8R8G8B8,
                                   D9REQ_FMT_D24S8);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEPTH_STENCIL_SUPPORTED);
    q = d9_req_check_depth_stencil(D9REQ_FMT_X8R8G8B8, D9REQ_FMT_A8R8G8B8,
                                   D9REQ_FMT_D24X8);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEPTH_STENCIL_SUPPORTED);
    q = d9_req_check_depth_stencil(D9REQ_FMT_X8R8G8B8, D9REQ_FMT_X8R8G8B8,
                                   D9REQ_FMT_D16);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_DEPTH_STENCIL_UNSUPPORTED);
    q = d9_req_check_depth_stencil(D9REQ_FMT_A8R8G8B8, D9REQ_FMT_X8R8G8B8,
                                   D9REQ_FMT_D24S8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_DEPTH_STENCIL_UNSUPPORTED);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_D24S8);
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_DEPTH_SURFACE);

    /* The render-target surface rule is exact: one usage bit, an LDR surface,
     * the single adapter format and HAL.  Everything else keeps its old
     * rejection.  MSAA is not a request kind in this matrix, so this contract
     * adds no multisample claim either. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_RENDERTARGET | D9REQ_USAGE_DEPTHSTENCIL,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_RENDERTARGET | D9REQ_USAGE_QUERY_FILTER,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_X8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_A16B16G16R16F);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_FLOAT16_HDR);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   2, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_RESOURCE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_SURFACE, 0x12345678u);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_UNKNOWN);
    q = d9_req_check_device_format(D9REQ_FMT_A8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_X8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_ADAPTER_FORMAT);
}

static void rejected_caps_and_edges(void) {
    D9ReqRequest q;
    D9ReqVerdict v;

    q = d9_req_device_caps(D9REQ_CAP_SHADER_MODEL, D9REQ_SHADER_MODEL(3, 0));
    v = eval(&q);
    assert(v.decision == D9REQ_FALLBACK);
    assert(v.answer == D9REQ_ANSWER_NOTAVAILABLE);
    assert(v.requirement == D9REQ_ID_CAPS_SM3_FAMILY);
    assert(strstr(v.reason, "2.0") != NULL);
    assert(strstr(v.contract, "specialized") != NULL);
    assert(strstr(v.contract, "no universal") != NULL);

    q.value = D9REQ_SHADER_MODEL(3, 255);
    expect(&q, D9REQ_FALLBACK, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_SM3_FAMILY);
    q.value = D9REQ_SHADER_MODEL(1, 255);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_SHADER_OTHER);
    q.value = D9REQ_SHADER_MODEL(2, 1);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_SHADER_OTHER);
    q.value = D9REQ_SHADER_MODEL(4, 0);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_SHADER_OTHER);

    q = d9_req_device_caps(D9REQ_CAP_RENDER_TARGETS, 2);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_MRT_UNSUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_TEXTURE_STAGES, 2);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_STAGES_UNSUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_TEXTURE_SIZE, 2048);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_CAPS_TEXTURE_SIZE_UNSUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_VS_CONSTANTS, 5);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_CAPS_VS_CONSTANTS_UNSUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_DEPTH_TEST, 2);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_CAPS_DEPTH_TEST_UNSUPPORTED);
    q = d9_req_device_caps(D9REQ_CAP_SM3_SLOTS, 1);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_CAPS_SM3_SLOTS_UNSUPPORTED);
    q = d9_req_device_caps(99, 1);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_CAPS_UNKNOWN);

    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, 0x12345678u);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_UNKNOWN);

    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_RENDERTARGET,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_QUERY_FILTER,
                                   D9REQ_RESOURCE_SURFACE, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_USAGE);
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    q.resource_type = 2;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_FORMAT_RESOURCE);

    q = d9_req_check_device_type(D9REQ_DEVICE_HAL, D9REQ_FMT_X8R8G8B8,
                                 D9REQ_FMT_X8R8G8B8, 0);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_DEVICE_TYPE_EXCLUSIVE);
    q.windowed = 1;
    q.display_format = D9REQ_FMT_A8R8G8B8;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_DEVICE_TYPE_DISPLAY_FORMAT);
    q.display_format = D9REQ_FMT_X8R8G8B8;
    q.backbuffer_format = D9REQ_FMT_A16B16G16R16F;
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE,
           D9REQ_ID_DEVICE_TYPE_BACKBUFFER_FORMAT);

    q = d9_req_check_device_format(D9REQ_FMT_A8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    expect(&q, D9REQ_UNSUPPORTED, D9REQ_ANSWER_NOTAVAILABLE, D9REQ_ID_ADAPTER_FORMAT);

    q = d9_req_check_device_type(2, D9REQ_FMT_X8R8G8B8, D9REQ_FMT_X8R8G8B8, 1);
    expect(&q, D9REQ_INVALID, D9REQ_ANSWER_INVALIDCALL, D9REQ_ID_INVALID_DEVICE_TYPE);
    q = d9_req_check_device_type(D9REQ_DEVICE_HAL, D9REQ_FMT_X8R8G8B8,
                                 D9REQ_FMT_X8R8G8B8, 1);
    q.adapter = 1;
    expect(&q, D9REQ_INVALID, D9REQ_ANSWER_INVALIDCALL, D9REQ_ID_INVALID_ADAPTER);
    q = d9_req_device_caps(D9REQ_CAP_SHADER_MODEL, D9REQ_SHADER_MODEL(2, 0));
    q.device_type = 2;
    expect(&q, D9REQ_INVALID, D9REQ_ANSWER_INVALIDCALL, D9REQ_ID_INVALID_DEVICE_TYPE);

    memset(&q, 0, sizeof q);
    q.kind = 99;
    expect(&q, D9REQ_INVALID, D9REQ_ANSWER_INVALIDCALL, D9REQ_ID_INVALID_REQUEST);

    /* Same usage bit, LDR format: this is the post-pixel-shader blending query
     * that the docs mislabel; it must not be reported as an sRGB fallback. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8,
                                   D9REQ_USAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    v = eval(&q);
    assert(v.decision == D9REQ_UNSUPPORTED);
    assert(v.requirement == D9REQ_ID_FORMAT_USAGE);
    assert(strstr(v.reason, "usage") != NULL);

    /* Kind-specific inputs only: unrelated fields cannot turn a decision off. */
    q = d9_req_check_device_format(D9REQ_FMT_X8R8G8B8, 0,
                                   D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    q.depth_stencil_format = D9REQ_FMT_D24S8;
    q.windowed = 1;
    q.display_format = D9REQ_FMT_A16B16G16R16F;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_FORMAT_TEXTURE_LDR);

    q = d9_req_check_device_type(D9REQ_DEVICE_HAL, D9REQ_FMT_X8R8G8B8,
                                 D9REQ_FMT_X8R8G8B8, 1);
    q.adapter_format = D9REQ_FMT_A8R8G8B8;
    q.format = D9REQ_FMT_A16B16G16R16F;
    expect(&q, D9REQ_SUPPORTED, D9REQ_ANSWER_OK, D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR);
}

static void bounded_probe(void) {
    D9ReqRequest q = d9_req_check_device_format(
        D9REQ_FMT_X8R8G8B8, D9REQ_USAGE_QUERY_SRGBREAD,
        D9REQ_RESOURCE_TEXTURE, D9REQ_FMT_A8R8G8B8);
    D9ReqVerdict v = eval(&q);
    char full[D9REQ_LOG_MAX];
    char guarded[96];
    char sentinel[4] = { 'x', 'y', 'z', 'w' };
    size_t full_len, needed, i;

    full_len = probe(&q, &v, full, sizeof full);
    assert(full_len == strlen(full));
    assert(full_len > 0 && full_len < sizeof full);
    assert(strstr(full, "req=check_device_format") != NULL);
    assert(strstr(full, "claim=fallback") != NULL);
    assert(strstr(full, "d3d9=NOTAVAILABLE") != NULL);
    assert(strstr(full, "id=format.srgb_read") != NULL);
    assert(strstr(full, "reason=") != NULL);
    assert(strstr(full, "contract=") != NULL);

    /* cap 0 must not write and must return the complete required length. */
    needed = probe(&q, &v, sentinel, 0);
    assert(needed == full_len);
    assert(!memcmp(sentinel, "xyzw", 4));
    needed = probe(&q, &v, NULL, 0);
    assert(needed == full_len);

    /* A one-byte buffer only receives the terminator. */
    guarded[0] = 'Q';
    needed = probe(&q, &v, guarded, 1);
    assert(needed == full_len);
    assert(guarded[0] == '\0');

    /* A short buffer is NUL-terminated inside the window and never writes past
     * it.  Guard bytes on both sides stay untouched. */
    memset(guarded, 0xA5, sizeof guarded);
    needed = probe(&q, &v, guarded + 8, 16);
    assert(needed == full_len);
    assert(guarded[8 + 15] == '\0');
    assert((unsigned char)guarded[8 + 16] == 0xA5u);
    for (i = 0; i < 8; ++i) assert((unsigned char)guarded[i] == 0xA5u);
    for (i = 8 + 16; i < sizeof guarded; ++i) {
        assert((unsigned char)guarded[i] == 0xA5u);
    }

    /* Deterministic output and no hidden state. */
    {
        char again[D9REQ_LOG_MAX];
        size_t again_len = probe(&q, &v, again, sizeof again);
        assert(again_len == full_len);
        assert(!strcmp(full, again));
        assert(eval(&q).decision == D9REQ_FALLBACK);
    }

    /* Null arguments are described, not dereferenced. */
    needed = probe(NULL, &v, full, sizeof full);
    assert(needed == strlen(full));
    assert(strstr(full, "probe=invalid") != NULL);

    {
        D9ReqVerdict invalid;
        assert(d9_req_evaluate(NULL, &invalid) == -1);
        assert(invalid.decision == D9REQ_INVALID);
        assert(invalid.answer == D9REQ_ANSWER_INVALIDCALL);
        assert(d9_req_evaluate(&q, NULL) == -1);
    }
}

int main(void) {
    matrix();
    accepted_ldr_menu();
    rejected_hdr_and_formats();
    rejected_caps_and_edges();
    bounded_probe();
    puts("renderer requirements: LDR menu paths and one LDR render-target surface accepted; HDR/SRGB/ATOC/RESZ/SM3/depth decisions truthful; bounded probe passed");
    return 0;
}
