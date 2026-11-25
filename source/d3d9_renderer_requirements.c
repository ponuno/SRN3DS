#include "d3d9_renderer_requirements.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COUNT(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

typedef struct {
    uint32_t id;
    uint32_t kind;
    const uint32_t *device_types;
    unsigned device_type_count;
    int windowed_check;
    int32_t windowed;
    const uint32_t *display_formats;
    unsigned display_format_count;
    const uint32_t *backbuffer_formats;
    unsigned backbuffer_format_count;
    const uint32_t *adapter_formats;
    unsigned adapter_format_count;
    int usage_check;
    uint32_t usage_required;
    uint32_t usage_allowed;
    const uint32_t *resource_types;
    unsigned resource_type_count;
    const uint32_t *formats;
    unsigned format_count;
    uint32_t caps;
    uint32_t value_min;
    uint32_t value_max;
    uint32_t decision;
    uint32_t answer;
    const char *reason;
    const char *contract;
} D9ReqRule;

static const uint32_t L_DEVICE_HAL[] = { D9REQ_DEVICE_HAL };
static const uint32_t L_DISPLAY_X8[] = { D9REQ_FMT_X8R8G8B8 };
static const uint32_t L_BACKBUFFER_LDR[] = {
    D9REQ_FMT_UNKNOWN, D9REQ_FMT_X8R8G8B8, D9REQ_FMT_A8R8G8B8
};
static const uint32_t L_ADAPTER_X8[] = { D9REQ_FMT_X8R8G8B8 };
static const uint32_t L_SURFACE_LDR[] = { D9REQ_FMT_X8R8G8B8, D9REQ_FMT_A8R8G8B8 };
static const uint32_t L_TEXTURE_LDR[] = {
    D9REQ_FMT_A8R8G8B8, D9REQ_FMT_X8R8G8B8,
    D9REQ_FMT_DXT1, D9REQ_FMT_DXT3, D9REQ_FMT_DXT5
};
static const uint32_t L_RESOURCE_SURFACE[] = { D9REQ_RESOURCE_SURFACE };
static const uint32_t L_RESOURCE_TEXTURE[] = { D9REQ_RESOURCE_TEXTURE };
static const uint32_t L_RESOURCE_LDR[] = {
    D9REQ_RESOURCE_SURFACE, D9REQ_RESOURCE_TEXTURE
};
static const uint32_t L_FLOAT16[] = { D9REQ_FMT_A16B16G16R16F };
static const uint32_t L_ATOC[] = { D9REQ_FMT_ATOC };
static const uint32_t L_RESZ[] = { D9REQ_FMT_RESZ };
static const uint32_t L_DEPTH[] = {
    D9REQ_FMT_D15S1, D9REQ_FMT_D16, D9REQ_FMT_D16_LOCKABLE,
    D9REQ_FMT_D24S8, D9REQ_FMT_D24X8,
    D9REQ_FMT_D24X4S4, D9REQ_FMT_D24FS8, D9REQ_FMT_D32,
    D9REQ_FMT_D32F_LOCKABLE, D9REQ_FMT_D32_LOCKABLE, D9REQ_FMT_S8_LOCKABLE
};

/* The order is significant: specific measured probes first, LDR acceptance
 * next, then truthful catch-alls.  No rule with a non-supported decision is
 * allowed to answer OK. */
static const D9ReqRule RULES[] = {
    /* CheckDeviceType: the only accepted window is the 400x240 LDR mode. */
    {
        .id = D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR,
        .kind = D9REQ_KIND_CHECK_DEVICE_TYPE,
        .device_types = L_DEVICE_HAL,
        .device_type_count = COUNT(L_DEVICE_HAL),
        .windowed_check = 1,
        .windowed = 1,
        .display_formats = L_DISPLAY_X8,
        .display_format_count = COUNT(L_DISPLAY_X8),
        .backbuffer_formats = L_BACKBUFFER_LDR,
        .backbuffer_format_count = COUNT(L_BACKBUFFER_LDR),
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "windowed HAL device with the single 400x240 X8R8G8B8 display mode",
        .contract = "",
    },
    {
        .id = D9REQ_ID_DEVICE_TYPE_EXCLUSIVE,
        .kind = D9REQ_KIND_CHECK_DEVICE_TYPE,
        .device_types = L_DEVICE_HAL,
        .device_type_count = COUNT(L_DEVICE_HAL),
        .windowed_check = 1,
        .windowed = 0,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "exclusive fullscreen and display-mode ownership are not implemented",
        .contract = "use the windowed 400x240 mode; there is no mode switch",
    },
    {
        .id = D9REQ_ID_DEVICE_TYPE_BACKBUFFER_FORMAT,
        .kind = D9REQ_KIND_CHECK_DEVICE_TYPE,
        .device_types = L_DEVICE_HAL,
        .device_type_count = COUNT(L_DEVICE_HAL),
        .windowed_check = 1,
        .windowed = 1,
        .display_formats = L_DISPLAY_X8,
        .display_format_count = COUNT(L_DISPLAY_X8),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "back buffer is not X8R8G8B8, A8R8G8B8 or UNKNOWN",
        .contract = "request X8R8G8B8 or A8R8G8B8; no HDR or exotic back buffer",
    },
    {
        .id = D9REQ_ID_DEVICE_TYPE_DISPLAY_FORMAT,
        .kind = D9REQ_KIND_CHECK_DEVICE_TYPE,
        .device_types = L_DEVICE_HAL,
        .device_type_count = COUNT(L_DEVICE_HAL),
        .windowed_check = 1,
        .windowed = 1,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "display format is not the implemented X8R8G8B8 mode",
        .contract = "use X8R8G8B8 at 400x240",
    },

    /* CheckDeviceFormat: measured Skyrim probes first. */
    {
        .id = D9REQ_ID_FORMAT_FLOAT16_HDR,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .formats = L_FLOAT16,
        .format_count = COUNT(L_FLOAT16),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "A16B16G16R16F is not a PICA200 texture or render-target format in this bridge",
        .contract = "LDR X8R8G8B8/A8R8G8B8 or DXT/ETC1 only; this is a downgrade, not A16 support",
    },
    {
        .id = D9REQ_ID_FORMAT_SRGB_WRITE,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .usage_check = 1,
        .usage_required = D9REQ_USAGE_QUERY_SRGBWRITE,
        .usage_allowed = D9REQ_USAGE_QUERY_SRGBWRITE | D9REQ_USAGE_QUERY_SRGBREAD |
                         D9REQ_USAGE_QUERY_FILTER | D9REQ_USAGE_QUERY_WRAPANDMIP,
        .resource_types = L_RESOURCE_TEXTURE,
        .resource_type_count = COUNT(L_RESOURCE_TEXTURE),
        .formats = L_TEXTURE_LDR,
        .format_count = COUNT(L_TEXTURE_LDR),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "no hardware sRGB framebuffer encode for QUERY_SRGBWRITE",
        .contract = "write LDR RGBA8; output stays in the display gamma space",
    },
    {
        .id = D9REQ_ID_FORMAT_SRGB_READ,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .usage_check = 1,
        .usage_required = D9REQ_USAGE_QUERY_SRGBREAD,
        .usage_allowed = D9REQ_USAGE_QUERY_SRGBREAD | D9REQ_USAGE_QUERY_FILTER |
                         D9REQ_USAGE_QUERY_WRAPANDMIP,
        .resource_types = L_RESOURCE_TEXTURE,
        .resource_type_count = COUNT(L_RESOURCE_TEXTURE),
        .formats = L_TEXTURE_LDR,
        .format_count = COUNT(L_TEXTURE_LDR),
        .decision = D9REQ_FALLBACK,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "no per-sample sRGB decode for QUERY_SRGBREAD",
        .contract = "proposed CPU sRGB-to-linear conversion at texture import/upload; sampling sees LDR data",
    },
    {
        .id = D9REQ_ID_FORMAT_ATOC,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .formats = L_ATOC,
        .format_count = COUNT(L_ATOC),
        .decision = D9REQ_FALLBACK,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "D3DFMT_ATOC surface/coverage is not implemented",
        .contract = "binary PICA alpha test at a threshold; no multisample coverage or per-sample resolve",
    },
    {
        .id = D9REQ_ID_FORMAT_RESZ,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .formats = L_RESZ,
        .format_count = COUNT(L_RESZ),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "D3DFMT_RESZ depth resolve is not implemented",
        .contract = "none; no depth texture, multisample depth or resolve path exists",
    },
    {
        .id = D9REQ_ID_FORMAT_DEPTH,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .formats = L_DEPTH,
        .format_count = COUNT(L_DEPTH),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "D3D9 depth-stencil and depth-texture formats are not implemented",
        .contract = "none; the PICA depth attachment is not exposed through the D3D9 bridge",
    },
    {
        .id = D9REQ_ID_FORMAT_RENDER_TARGET_LDR,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .adapter_formats = L_ADAPTER_X8,
        .adapter_format_count = COUNT(L_ADAPTER_X8),
        .usage_check = 1,
        .usage_required = D9REQ_USAGE_RENDERTARGET,
        .usage_allowed = D9REQ_USAGE_RENDERTARGET,
        .resource_types = L_RESOURCE_SURFACE,
        .resource_type_count = COUNT(L_RESOURCE_SURFACE),
        .formats = L_SURFACE_LDR,
        .format_count = COUNT(L_SURFACE_LDR),
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "single LDR X8R8G8B8/A8R8G8B8 render-target surface with usage D3DUSAGE_RENDERTARGET",
        .contract = "",
    },
    {
        .id = D9REQ_ID_FORMAT_SURFACE_LDR,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .adapter_formats = L_ADAPTER_X8,
        .adapter_format_count = COUNT(L_ADAPTER_X8),
        .usage_check = 1,
        .usage_required = 0,
        .usage_allowed = 0,
        .resource_types = L_RESOURCE_SURFACE,
        .resource_type_count = COUNT(L_RESOURCE_SURFACE),
        .formats = L_SURFACE_LDR,
        .format_count = COUNT(L_SURFACE_LDR),
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "plain X8R8G8B8/A8R8G8B8 off-screen surface with usage 0",
        .contract = "",
    },
    {
        .id = D9REQ_ID_FORMAT_TEXTURE_LDR,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .adapter_formats = L_ADAPTER_X8,
        .adapter_format_count = COUNT(L_ADAPTER_X8),
        .usage_check = 1,
        .usage_required = 0,
        .usage_allowed = D9REQ_USAGE_QUERY_FILTER | D9REQ_USAGE_QUERY_WRAPANDMIP,
        .resource_types = L_RESOURCE_TEXTURE,
        .resource_type_count = COUNT(L_RESOURCE_TEXTURE),
        .formats = L_TEXTURE_LDR,
        .format_count = COUNT(L_TEXTURE_LDR),
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "LDR RGBA8/BC texture with only QUERY_FILTER/QUERY_WRAPANDMIP",
        .contract = "",
    },
    {
        .id = D9REQ_ID_FORMAT_USAGE,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .resource_types = L_RESOURCE_LDR,
        .resource_type_count = COUNT(L_RESOURCE_LDR),
        .formats = L_TEXTURE_LDR,
        .format_count = COUNT(L_TEXTURE_LDR),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "usage is not a plain surface, a single render-target surface or a QUERY_FILTER/QUERY_WRAPANDMIP texture",
        .contract = "use usage 0, RENDERTARGET on one LDR surface, or a static LDR texture; no texture render target, combined usage bits, MSAA, depth, auto-mip, dynamic or sRGB usage",
    },
    {
        .id = D9REQ_ID_FORMAT_UNKNOWN,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .resource_types = L_RESOURCE_LDR,
        .resource_type_count = COUNT(L_RESOURCE_LDR),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "format has no PICA200 bridge implementation",
        .contract = "use A8R8G8B8, X8R8G8B8, DXT1, DXT3, DXT5 or ETC1/ETC1A4",
    },
    {
        .id = D9REQ_ID_FORMAT_RESOURCE,
        .kind = D9REQ_KIND_CHECK_DEVICE_FORMAT,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "resource type is not D3DRTYPE_SURFACE or D3DRTYPE_TEXTURE",
        .contract = "use a supported surface or texture capability query",
    },

    /* CheckDepthStencilMatch. */
    {
        .id = D9REQ_ID_DEPTH_STENCIL_UNSUPPORTED,
        .kind = D9REQ_KIND_CHECK_DEPTH_STENCIL,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "no D3D9 depth-stencil surfaces; CheckDepthStencilMatch cannot succeed",
        .contract = "none; requires a native PICA depth attachment and a D3D9 surface object",
    },

    /* GetDeviceCaps has no capability argument.  `value` is the requirement
     * (shader model, target count, ...) compared with the caps this bridge
     * actually reports; adapter_smoke pins those caps. */
    {
        .id = D9REQ_ID_CAPS_SM2_TEMPLATES,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SHADER_MODEL,
        .value_min = D9REQ_SHADER_MODEL(2, 0),
        .value_max = D9REQ_SHADER_MODEL(2, 0),
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "VS/PS 2.0 exact templates are advertised; no SM3 slots",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_SM3_FAMILY,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SHADER_MODEL,
        .value_min = D9REQ_SHADER_MODEL(3, 0),
        .value_max = D9REQ_SHADER_MODEL(3, 255),
        .decision = D9REQ_FALLBACK,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "GetDeviceCaps reports VS/PS 2.0; SM3 is not advertised",
        .contract = "specialized shader-family replacement only (TEV/menu-light); no universal SM3 translator. The first-menu closure is a separate non-D3D9 predicate (d9_menu_closure_predicate in d3d9_menu_closure.h), never a caps success",
    },
    {
        .id = D9REQ_ID_CAPS_SHADER_OTHER,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SHADER_MODEL,
        .value_min = 0,
        .value_max = D9REQ_SHADER_MODEL(1, 255),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "only exact VS/PS 2.0 templates are implemented",
        .contract = "no SM1 or pre-2.0 shader path",
    },
    {
        .id = D9REQ_ID_CAPS_SHADER_OTHER,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SHADER_MODEL,
        .value_min = D9REQ_SHADER_MODEL(2, 1),
        .value_max = D9REQ_SHADER_MODEL(2, 255),
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "only exact VS/PS 2.0 templates are implemented",
        .contract = "no VS/PS 2.x variants",
    },
    {
        .id = D9REQ_ID_CAPS_SHADER_OTHER,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SHADER_MODEL,
        .value_min = D9REQ_SHADER_MODEL(4, 0),
        .value_max = 0xffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "only exact VS/PS 2.0 templates are implemented",
        .contract = "no shader model above the specialized SM3 family replacement",
    },
    {
        .id = D9REQ_ID_CAPS_MRT_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_RENDER_TARGETS,
        .value_min = 0,
        .value_max = 1,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "one render target: the back buffer",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_MRT_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_RENDER_TARGETS,
        .value_min = 2,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "multiple simultaneous render targets are not implemented",
        .contract = "render to one LDR target",
    },
    {
        .id = D9REQ_ID_CAPS_STAGES_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_TEXTURE_STAGES,
        .value_min = 0,
        .value_max = 1,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "one texture stage",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_STAGES_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_TEXTURE_STAGES,
        .value_min = 2,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "multiple simultaneous texture stages are not implemented",
        .contract = "combine materials in TEV or on the CPU",
    },
    {
        .id = D9REQ_ID_CAPS_TEXTURE_SIZE_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_TEXTURE_SIZE,
        .value_min = 0,
        .value_max = 1024,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "power-of-two textures up to 1024x1024",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_TEXTURE_SIZE_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_TEXTURE_SIZE,
        .value_min = 1025,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "texture dimension exceeds 1024",
        .contract = "resize or split the texture; 1024x1024 is the maximum",
    },
    {
        .id = D9REQ_ID_CAPS_VS_CONSTANTS_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_VS_CONSTANTS,
        .value_min = 0,
        .value_max = 4,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "four vertex-shader float constants c0..c3",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_VS_CONSTANTS_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_VS_CONSTANTS,
        .value_min = 5,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "only four vertex-shader float constants are implemented",
        .contract = "fold extra constants into the vertex stream",
    },
    {
        .id = D9REQ_ID_CAPS_DEPTH_TEST_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_DEPTH_TEST,
        .value_min = 0,
        .value_max = 1,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "PICA z-test and z-write on the attached depth buffer",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_DEPTH_TEST_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_DEPTH_TEST,
        .value_min = 2,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "depth-test capability is a boolean request, not a count",
        .contract = "request depth test as 0 or 1",
    },
    {
        .id = D9REQ_ID_CAPS_SM3_SLOTS_SUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SM3_SLOTS,
        .value_min = 0,
        .value_max = 0,
        .decision = D9REQ_SUPPORTED,
        .answer = D9REQ_ANSWER_OK,
        .reason = "MaxPixelShader30InstructionSlots is honestly 0",
        .contract = "",
    },
    {
        .id = D9REQ_ID_CAPS_SM3_SLOTS_UNSUPPORTED,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .caps = D9REQ_CAP_SM3_SLOTS,
        .value_min = 1,
        .value_max = 0xffffffffu,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "no SM3 instruction slots are advertised",
        .contract = "specialized family replacement only; no universal SM3 translation",
    },
    {
        .id = D9REQ_ID_CAPS_UNKNOWN,
        .kind = D9REQ_KIND_DEVICE_CAPS,
        .decision = D9REQ_UNSUPPORTED,
        .answer = D9REQ_ANSWER_NOTAVAILABLE,
        .reason = "capability is not part of the implemented GetDeviceCaps contract",
        .contract = "read the actual D3DCAPS9 fields instead of assuming a capability",
    },
};

static int list_has(const uint32_t *values, unsigned count, uint32_t value) {
    unsigned i;
    for (i = 0; i < count; ++i) {
        if (values[i] == value) return 1;
    }
    return 0;
}

static int rule_matches(const D9ReqRule *rule, const D9ReqRequest *request) {
    if (rule->kind != request->kind) return 0;
    if (rule->device_types &&
        !list_has(rule->device_types, rule->device_type_count, request->device_type)) return 0;
    if (rule->windowed_check &&
        ((request->windowed != 0) != (rule->windowed != 0))) return 0;
    if (rule->display_formats &&
        !list_has(rule->display_formats, rule->display_format_count, request->display_format)) return 0;
    if (rule->backbuffer_formats &&
        !list_has(rule->backbuffer_formats, rule->backbuffer_format_count,
                  request->backbuffer_format)) return 0;
    if (rule->adapter_formats &&
        !list_has(rule->adapter_formats, rule->adapter_format_count, request->adapter_format)) return 0;
    if (rule->usage_check) {
        if ((request->usage & rule->usage_required) != rule->usage_required) return 0;
        if (request->usage & ~rule->usage_allowed) return 0;
    }
    if (rule->resource_types &&
        !list_has(rule->resource_types, rule->resource_type_count, request->resource_type)) return 0;
    if (rule->formats &&
        !list_has(rule->formats, rule->format_count, request->format)) return 0;
    if (rule->caps) {
        if (request->caps != rule->caps) return 0;
        if (request->value < rule->value_min || request->value > rule->value_max) return 0;
    }
    return 1;
}

static void set_text(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    (void)snprintf(dst, cap, "%s", src);
}

int d9_req_evaluate(const D9ReqRequest *request, D9ReqVerdict *verdict) {
    unsigned i;

    if (!verdict) return -1;
    memset(verdict, 0, sizeof *verdict);
    verdict->decision = D9REQ_INVALID;
    verdict->answer = D9REQ_ANSWER_INVALIDCALL;
    verdict->requirement = D9REQ_ID_INVALID_REQUEST;
    set_text(verdict->reason, sizeof verdict->reason, "null request");
    set_text(verdict->contract, sizeof verdict->contract, "none");
    if (!request) return -1;

    if (request->kind < D9REQ_KIND_CHECK_DEVICE_TYPE ||
        request->kind > D9REQ_KIND_DEVICE_CAPS) {
        set_text(verdict->reason, sizeof verdict->reason,
                 "request kind is not a supported D3D9 query");
        return 0;
    }

    if (request->adapter != 0) {
        verdict->requirement = D9REQ_ID_INVALID_ADAPTER;
        set_text(verdict->reason, sizeof verdict->reason,
                 "the bridge has exactly one adapter, ordinal 0");
        set_text(verdict->contract, sizeof verdict->contract,
                 "query adapter 0");
        return 0;
    }

    if (request->device_type != D9REQ_DEVICE_HAL) {
        verdict->requirement = D9REQ_ID_INVALID_DEVICE_TYPE;
        set_text(verdict->reason, sizeof verdict->reason,
                 "only D3DDEVTYPE_HAL is implemented");
        set_text(verdict->contract, sizeof verdict->contract,
                 "query the HAL device type");
        return 0;
    }

    if (request->kind == D9REQ_KIND_CHECK_DEVICE_FORMAT &&
        request->adapter_format != D9REQ_FMT_X8R8G8B8) {
        verdict->decision = D9REQ_UNSUPPORTED;
        verdict->answer = D9REQ_ANSWER_NOTAVAILABLE;
        verdict->requirement = D9REQ_ID_ADAPTER_FORMAT;
        set_text(verdict->reason, sizeof verdict->reason,
                 "adapter format is not X8R8G8B8");
        set_text(verdict->contract, sizeof verdict->contract,
                 "query against the single adapter format");
        return 0;
    }

    for (i = 0; i < COUNT(RULES); ++i) {
        if (!rule_matches(&RULES[i], request)) continue;
        verdict->decision = RULES[i].decision;
        verdict->answer = RULES[i].answer;
        verdict->requirement = RULES[i].id;
        set_text(verdict->reason, sizeof verdict->reason, RULES[i].reason);
        set_text(verdict->contract, sizeof verdict->contract, RULES[i].contract);
        return 0;
    }

    verdict->decision = D9REQ_UNSUPPORTED;
    verdict->answer = D9REQ_ANSWER_NOTAVAILABLE;
    verdict->requirement = D9REQ_ID_UNMATCHED;
    set_text(verdict->reason, sizeof verdict->reason,
             "no requirement rule matched the actual query inputs");
    set_text(verdict->contract, sizeof verdict->contract,
             "none; no D3D9 capability is claimed");
    return 0;
}

unsigned d9_req_rule_count(void) {
    return COUNT(RULES);
}

int d9_req_matrix_validate(char *why, size_t why_size) {
    unsigned i, kind;
    int seen_kind[D9REQ_KIND_DEVICE_CAPS + 1] = { 0 };
    int seen_fallback = 0, seen_unsupported = 0;

    if (why && why_size) why[0] = '\0';
    if (COUNT(RULES) == 0) {
        set_text(why, why_size, "matrix is empty");
        return -1;
    }
    for (i = 0; i < COUNT(RULES); ++i) {
        const D9ReqRule *r = &RULES[i];
        char detail[96];
        if (r->id == D9REQ_ID_NONE || r->id >= D9REQ_ID_COUNT) {
            (void)snprintf(detail, sizeof detail, "rule %u has an out-of-range id", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->kind < D9REQ_KIND_CHECK_DEVICE_TYPE || r->kind > D9REQ_KIND_DEVICE_CAPS) {
            (void)snprintf(detail, sizeof detail, "rule %u has an out-of-range kind", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->decision != D9REQ_SUPPORTED && r->decision != D9REQ_FALLBACK &&
            r->decision != D9REQ_UNSUPPORTED) {
            (void)snprintf(detail, sizeof detail, "rule %u has an invalid decision", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->answer != D9REQ_ANSWER_OK && r->answer != D9REQ_ANSWER_NOTAVAILABLE &&
            r->answer != D9REQ_ANSWER_INVALIDCALL) {
            (void)snprintf(detail, sizeof detail, "rule %u has an invalid answer", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (!r->reason || !r->reason[0]) {
            (void)snprintf(detail, sizeof detail, "rule %u has no reason", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->decision == D9REQ_SUPPORTED && r->answer != D9REQ_ANSWER_OK) {
            (void)snprintf(detail, sizeof detail, "rule %u is supported but not advertised", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->decision != D9REQ_SUPPORTED && r->answer == D9REQ_ANSWER_OK) {
            (void)snprintf(detail, sizeof detail,
                           "rule %u would fake a success for a non-supported claim", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->decision == D9REQ_FALLBACK && (!r->contract || !r->contract[0])) {
            (void)snprintf(detail, sizeof detail, "fallback rule %u has no contract", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->usage_check && (r->usage_required & ~r->usage_allowed)) {
            (void)snprintf(detail, sizeof detail, "rule %u has an impossible usage mask", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if (r->value_min > r->value_max) {
            (void)snprintf(detail, sizeof detail, "rule %u has an inverted value range", i);
            set_text(why, why_size, detail);
            return -1;
        }
        if ((r->device_types == NULL) != (r->device_type_count == 0) ||
            (r->display_formats == NULL) != (r->display_format_count == 0) ||
            (r->backbuffer_formats == NULL) != (r->backbuffer_format_count == 0) ||
            (r->adapter_formats == NULL) != (r->adapter_format_count == 0) ||
            (r->resource_types == NULL) != (r->resource_type_count == 0) ||
            (r->formats == NULL) != (r->format_count == 0)) {
            (void)snprintf(detail, sizeof detail, "rule %u has a null/count mismatch", i);
            set_text(why, why_size, detail);
            return -1;
        }
        seen_kind[r->kind] = 1;
        if (r->decision == D9REQ_FALLBACK) seen_fallback = 1;
        if (r->decision == D9REQ_UNSUPPORTED) seen_unsupported = 1;
    }
    for (kind = D9REQ_KIND_CHECK_DEVICE_TYPE; kind <= D9REQ_KIND_DEVICE_CAPS; ++kind) {
        if (!seen_kind[kind]) {
            char detail[64];
            (void)snprintf(detail, sizeof detail, "kind %u has no rule", kind);
            set_text(why, why_size, detail);
            return -1;
        }
    }
    if (!seen_fallback) {
        set_text(why, why_size, "matrix has no fallback contract");
        return -1;
    }
    if (!seen_unsupported) {
        set_text(why, why_size, "matrix has no unsupported decision");
        return -1;
    }
    return 0;
}

const char *d9_req_decision_name(uint32_t decision) {
    switch (decision) {
    case D9REQ_SUPPORTED: return "supported";
    case D9REQ_FALLBACK: return "fallback";
    case D9REQ_UNSUPPORTED: return "unsupported";
    case D9REQ_INVALID: return "invalid";
    default: return "unknown";
    }
}

const char *d9_req_answer_name(uint32_t answer) {
    switch (answer) {
    case D9REQ_ANSWER_OK: return "OK";
    case D9REQ_ANSWER_NOTAVAILABLE: return "NOTAVAILABLE";
    case D9REQ_ANSWER_INVALIDCALL: return "INVALIDCALL";
    default: return "UNKNOWN";
    }
}

const char *d9_req_requirement_name(uint32_t requirement) {
    switch (requirement) {
    case D9REQ_ID_NONE: return "none";
    case D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR: return "device_type.windowed_ldr";
    case D9REQ_ID_DEVICE_TYPE_EXCLUSIVE: return "device_type.exclusive";
    case D9REQ_ID_DEVICE_TYPE_DISPLAY_FORMAT: return "device_type.display_format";
    case D9REQ_ID_DEVICE_TYPE_BACKBUFFER_FORMAT: return "device_type.backbuffer_format";
    case D9REQ_ID_ADAPTER_FORMAT: return "format.adapter";
    case D9REQ_ID_FORMAT_FLOAT16_HDR: return "format.a16b16g16r16f";
    case D9REQ_ID_FORMAT_SRGB_WRITE: return "format.srgb_write";
    case D9REQ_ID_FORMAT_SRGB_READ: return "format.srgb_read";
    case D9REQ_ID_FORMAT_ATOC: return "format.atoc";
    case D9REQ_ID_FORMAT_RESZ: return "format.resz";
    case D9REQ_ID_FORMAT_DEPTH: return "format.depth";
    case D9REQ_ID_FORMAT_SURFACE_LDR: return "format.surface_ldr";
    case D9REQ_ID_FORMAT_RENDER_TARGET_LDR: return "format.render_target_ldr";
    case D9REQ_ID_FORMAT_TEXTURE_LDR: return "format.texture_ldr";
    case D9REQ_ID_FORMAT_USAGE: return "format.usage";
    case D9REQ_ID_FORMAT_RESOURCE: return "format.resource";
    case D9REQ_ID_FORMAT_UNKNOWN: return "format.unknown";
    case D9REQ_ID_DEPTH_STENCIL_UNSUPPORTED: return "depth_stencil.unsupported";
    case D9REQ_ID_CAPS_SM2_TEMPLATES: return "caps.sm2_templates";
    case D9REQ_ID_CAPS_SM3_FAMILY: return "caps.sm3_family_only";
    case D9REQ_ID_CAPS_SHADER_OTHER: return "caps.shader_other";
    case D9REQ_ID_CAPS_MRT_SUPPORTED: return "caps.mrt_supported";
    case D9REQ_ID_CAPS_MRT_UNSUPPORTED: return "caps.mrt_unsupported";
    case D9REQ_ID_CAPS_STAGES_SUPPORTED: return "caps.stages_supported";
    case D9REQ_ID_CAPS_STAGES_UNSUPPORTED: return "caps.stages_unsupported";
    case D9REQ_ID_CAPS_TEXTURE_SIZE_SUPPORTED: return "caps.texture_size_supported";
    case D9REQ_ID_CAPS_TEXTURE_SIZE_UNSUPPORTED: return "caps.texture_size_unsupported";
    case D9REQ_ID_CAPS_VS_CONSTANTS_SUPPORTED: return "caps.vs_constants_supported";
    case D9REQ_ID_CAPS_VS_CONSTANTS_UNSUPPORTED: return "caps.vs_constants_unsupported";
    case D9REQ_ID_CAPS_DEPTH_TEST_SUPPORTED: return "caps.depth_test_supported";
    case D9REQ_ID_CAPS_DEPTH_TEST_UNSUPPORTED: return "caps.depth_test_unsupported";
    case D9REQ_ID_CAPS_SM3_SLOTS_SUPPORTED: return "caps.sm3_slots_supported";
    case D9REQ_ID_CAPS_SM3_SLOTS_UNSUPPORTED: return "caps.sm3_slots_unsupported";
    case D9REQ_ID_CAPS_UNKNOWN: return "caps.unknown";
    case D9REQ_ID_INVALID_ADAPTER: return "invalid.adapter";
    case D9REQ_ID_INVALID_DEVICE_TYPE: return "invalid.device_type";
    case D9REQ_ID_INVALID_REQUEST: return "invalid.request";
    case D9REQ_ID_UNMATCHED: return "unmatched";
    default: return "unknown.requirement";
    }
}

const char *d9_req_cap_name(uint32_t caps) {
    switch (caps) {
    case D9REQ_CAP_SHADER_MODEL: return "shader_model";
    case D9REQ_CAP_RENDER_TARGETS: return "render_targets";
    case D9REQ_CAP_TEXTURE_STAGES: return "texture_stages";
    case D9REQ_CAP_TEXTURE_SIZE: return "texture_size";
    case D9REQ_CAP_VS_CONSTANTS: return "vs_constants";
    case D9REQ_CAP_DEPTH_TEST: return "depth_test";
    case D9REQ_CAP_SM3_SLOTS: return "sm3_slots";
    default: return "unknown";
    }
}

const char *d9_req_format_name(uint32_t format) {
    switch (format) {
    case D9REQ_FMT_UNKNOWN: return "UNKNOWN";
    case D9REQ_FMT_A8R8G8B8: return "A8R8G8B8";
    case D9REQ_FMT_X8R8G8B8: return "X8R8G8B8";
    case D9REQ_FMT_D16_LOCKABLE: return "D16_LOCKABLE";
    case D9REQ_FMT_D32: return "D32";
    case D9REQ_FMT_D15S1: return "D15S1";
    case D9REQ_FMT_D24S8: return "D24S8";
    case D9REQ_FMT_D24X8: return "D24X8";
    case D9REQ_FMT_D24X4S4: return "D24X4S4";
    case D9REQ_FMT_D16: return "D16";
    case D9REQ_FMT_D32F_LOCKABLE: return "D32F_LOCKABLE";
    case D9REQ_FMT_D24FS8: return "D24FS8";
    case D9REQ_FMT_D32_LOCKABLE: return "D32_LOCKABLE";
    case D9REQ_FMT_S8_LOCKABLE: return "S8_LOCKABLE";
    case D9REQ_FMT_A16B16G16R16F: return "A16B16G16R16F";
    case D9REQ_FMT_DXT1: return "DXT1";
    case D9REQ_FMT_DXT3: return "DXT3";
    case D9REQ_FMT_DXT5: return "DXT5";
    case D9REQ_FMT_ATOC: return "ATOC";
    case D9REQ_FMT_RESZ: return "RESZ";
    default: return NULL;
    }
}

const char *d9_req_usage_name(uint32_t usage) {
    switch (usage) {
    case 0: return "none";
    case D9REQ_USAGE_RENDERTARGET: return "RENDERTARGET";
    case D9REQ_USAGE_DEPTHSTENCIL: return "DEPTHSTENCIL";
    case D9REQ_USAGE_WRITEONLY: return "WRITEONLY";
    case D9REQ_USAGE_DYNAMIC: return "DYNAMIC";
    case D9REQ_USAGE_AUTOGENMIPMAP: return "AUTOGENMIPMAP";
    case D9REQ_USAGE_QUERY_SRGBREAD: return "QUERY_SRGBREAD";
    case D9REQ_USAGE_QUERY_FILTER: return "QUERY_FILTER";
    case D9REQ_USAGE_QUERY_SRGBWRITE: return "QUERY_SRGBWRITE";
    case D9REQ_USAGE_QUERY_POSTPIXELSHADER_BLENDING:
        return "QUERY_POSTPIXELSHADER_BLENDING";
    case D9REQ_USAGE_QUERY_WRAPANDMIP: return "QUERY_WRAPANDMIP";
    case D9REQ_USAGE_QUERY_FILTER | D9REQ_USAGE_QUERY_WRAPANDMIP:
        return "QUERY_FILTER|QUERY_WRAPANDMIP";
    default: return NULL;
    }
}

D9ReqRequest d9_req_check_device_type(uint32_t device_type, uint32_t display_format,
                                      uint32_t backbuffer_format, int windowed) {
    D9ReqRequest r;
    memset(&r, 0, sizeof r);
    r.kind = D9REQ_KIND_CHECK_DEVICE_TYPE;
    r.device_type = device_type;
    r.display_format = display_format;
    r.backbuffer_format = backbuffer_format;
    r.windowed = windowed ? 1u : 0u;
    return r;
}

D9ReqRequest d9_req_check_device_format(uint32_t adapter_format, uint32_t usage,
                                        uint32_t resource_type, uint32_t format) {
    D9ReqRequest r;
    memset(&r, 0, sizeof r);
    r.kind = D9REQ_KIND_CHECK_DEVICE_FORMAT;
    r.device_type = D9REQ_DEVICE_HAL;
    r.adapter_format = adapter_format;
    r.usage = usage;
    r.resource_type = resource_type;
    r.format = format;
    return r;
}

D9ReqRequest d9_req_check_depth_stencil(uint32_t display_format,
                                        uint32_t backbuffer_format,
                                        uint32_t depth_stencil_format) {
    D9ReqRequest r;
    memset(&r, 0, sizeof r);
    r.kind = D9REQ_KIND_CHECK_DEPTH_STENCIL;
    r.device_type = D9REQ_DEVICE_HAL;
    r.display_format = display_format;
    r.backbuffer_format = backbuffer_format;
    r.depth_stencil_format = depth_stencil_format;
    return r;
}

D9ReqRequest d9_req_device_caps(uint32_t caps, uint32_t value) {
    D9ReqRequest r;
    memset(&r, 0, sizeof r);
    r.kind = D9REQ_KIND_DEVICE_CAPS;
    r.device_type = D9REQ_DEVICE_HAL;
    r.caps = caps;
    r.value = value;
    return r;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t used;
} ReqWriter;

static void req_put(ReqWriter *w, const char *text) {
    size_t n = strlen(text);
    if (w->cap && w->buf && w->used < w->cap) {
        size_t room = w->cap - 1 - w->used;
        size_t copy = n < room ? n : room;
        if (copy) memcpy(w->buf + w->used, text, copy);
        w->buf[w->used + copy] = '\0';
    }
    w->used += n;
}

static void req_fmt(ReqWriter *w, const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    int n, old;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    old = (int)w->used;
    req_put(w, tmp);
    if ((size_t)n >= sizeof tmp) w->used = (size_t)old + (size_t)n;
}

static void put_format(ReqWriter *w, uint32_t format) {
    const char *name = d9_req_format_name(format);
    if (name) req_fmt(w, "%s", name);
    else req_fmt(w, "0x%08x", format);
}

static void put_resource(ReqWriter *w, uint32_t type) {
    if (type == D9REQ_RESOURCE_SURFACE) req_put(w, "surface");
    else if (type == D9REQ_RESOURCE_TEXTURE) req_put(w, "texture");
    else req_fmt(w, "type#%u", type);
}

static void put_device(ReqWriter *w, uint32_t type) {
    if (type == D9REQ_DEVICE_HAL) req_put(w, "HAL");
    else req_fmt(w, "type#%u", type);
}

size_t d9_req_format_probe(const D9ReqRequest *request, const D9ReqVerdict *verdict,
                           char *buf, size_t cap) {
    ReqWriter w;
    w.buf = buf;
    w.cap = cap;
    w.used = 0;
    if (buf && cap) buf[0] = '\0';

    if (!request || !verdict) {
        req_put(&w, "probe=invalid(null request or verdict)");
        return w.used;
    }

    switch (request->kind) {
    case D9REQ_KIND_CHECK_DEVICE_TYPE:
        req_put(&w, "req=check_device_type device=");
        put_device(&w, request->device_type);
        req_fmt(&w, " windowed=%u display=", request->windowed);
        put_format(&w, request->display_format);
        req_put(&w, " backbuffer=");
        put_format(&w, request->backbuffer_format);
        break;
    case D9REQ_KIND_CHECK_DEVICE_FORMAT: {
        const char *usage = d9_req_usage_name(request->usage);
        req_put(&w, "req=check_device_format adapter=");
        put_format(&w, request->adapter_format);
        req_fmt(&w, " usage=0x%08x", request->usage);
        if (usage) req_fmt(&w, "(%s)", usage);
        req_put(&w, " resource=");
        put_resource(&w, request->resource_type);
        req_put(&w, " format=");
        put_format(&w, request->format);
        break;
    }
    case D9REQ_KIND_CHECK_DEPTH_STENCIL:
        req_put(&w, "req=check_depth_stencil display=");
        put_format(&w, request->display_format);
        req_put(&w, " backbuffer=");
        put_format(&w, request->backbuffer_format);
        req_put(&w, " depth=");
        put_format(&w, request->depth_stencil_format);
        break;
    case D9REQ_KIND_DEVICE_CAPS:
        req_put(&w, "req=get_device_caps cap=");
        req_put(&w, d9_req_cap_name(request->caps));
        req_fmt(&w, " value=0x%08x", request->value);
        break;
    default:
        req_fmt(&w, "req=kind#%u", request->kind);
        break;
    }

    req_put(&w, " | claim=");
    req_put(&w, d9_req_decision_name(verdict->decision));
    req_put(&w, " | d3d9=");
    req_put(&w, d9_req_answer_name(verdict->answer));
    req_put(&w, " | id=");
    req_put(&w, d9_req_requirement_name(verdict->requirement));
    req_put(&w, " | reason=");
    req_put(&w, verdict->reason);
    if (verdict->contract[0]) {
        req_put(&w, " | contract=");
        req_put(&w, verdict->contract);
    }
    return w.used;
}
