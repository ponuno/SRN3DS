#pragma once
/* Truthful renderer capability contract for the D3D9 bridge.
 *
 * This file is intentionally not wired into d3d9_adapter.c/d3d9_core.c yet.
 * It turns the recorded/decoded Skyrim renderer probes into a data-driven
 * requirement matrix and gives one bounded decision line that can be attached
 * to the real
 * renderer trace.  It never turns a fallback into a fake D3D9 success:
 *
 *   D9REQ_SUPPORTED    -> the D3D9 query may answer OK.
 *   D9REQ_FALLBACK     -> the D3D9 query must answer NOTAVAILABLE, but a named
 *                         alternative contract exists outside the D3D9 surface.
 *   D9REQ_UNSUPPORTED  -> the D3D9 query must answer NOTAVAILABLE and no path
 *                         exists; the caller must reject or rewrite the path.
 *   D9REQ_INVALID      -> malformed request; answer INVALIDCALL.
 *
 * Numeric formats/usages are the D3D9 SDK values.  They are repeated here so
 * the module has no link dependency on d3d9_core.c; the smoke test cross-checks
 * the LDR values against source/d3d9_core.h.
 */

#include <stddef.h>
#include <stdint.h>

#define D9REQ_REASON_MAX   160u
#define D9REQ_CONTRACT_MAX 192u
#define D9REQ_LOG_MAX      640u

enum {
    D9REQ_SUPPORTED = 0,
    D9REQ_FALLBACK = 1,
    D9REQ_UNSUPPORTED = 2,
    D9REQ_INVALID = 3,
};

enum {
    D9REQ_ANSWER_OK = 0,
    D9REQ_ANSWER_NOTAVAILABLE = 1,
    D9REQ_ANSWER_INVALIDCALL = 2,
};

enum {
    D9REQ_KIND_CHECK_DEVICE_TYPE = 1,
    D9REQ_KIND_CHECK_DEVICE_FORMAT = 2,
    D9REQ_KIND_CHECK_DEPTH_STENCIL = 3,
    D9REQ_KIND_DEVICE_CAPS = 4,
};

enum {
    D9REQ_CAP_SHADER_MODEL = 1,
    D9REQ_CAP_RENDER_TARGETS = 2,
    D9REQ_CAP_TEXTURE_STAGES = 3,
    D9REQ_CAP_TEXTURE_SIZE = 4,
    D9REQ_CAP_VS_CONSTANTS = 5,
    D9REQ_CAP_DEPTH_TEST = 6,
    D9REQ_CAP_SM3_SLOTS = 7,
};

/* D3DFORMAT values. */
enum {
    D9REQ_FMT_UNKNOWN = 0,
    D9REQ_FMT_A8R8G8B8 = 21,
    D9REQ_FMT_X8R8G8B8 = 22,
    D9REQ_FMT_D16_LOCKABLE = 70,
    D9REQ_FMT_D32 = 71,
    D9REQ_FMT_D15S1 = 73,
    D9REQ_FMT_D24S8 = 75,
    D9REQ_FMT_D24X8 = 77,
    D9REQ_FMT_D24X4S4 = 79,
    D9REQ_FMT_D16 = 80,
    D9REQ_FMT_D32F_LOCKABLE = 82,
    D9REQ_FMT_D24FS8 = 83,
    D9REQ_FMT_D32_LOCKABLE = 84,
    D9REQ_FMT_S8_LOCKABLE = 85,
    D9REQ_FMT_A16B16G16R16F = 113,
    D9REQ_FMT_DXT1 = 0x31545844u,
    D9REQ_FMT_DXT3 = 0x33545844u,
    D9REQ_FMT_DXT5 = 0x35545844u,
    D9REQ_FMT_ATOC = 0x434f5441u, /* MAKEFOURCC('A','T','O','C') */
    D9REQ_FMT_RESZ = 0x5a534552u, /* MAKEFOURCC('R','E','S','Z') */
};

enum {
    D9REQ_DEVICE_HAL = 1,
};

enum {
    D9REQ_RESOURCE_SURFACE = 1,
    D9REQ_RESOURCE_TEXTURE = 3,
};

/* The only usage-bearing LDR surface accepted by the contract is one
 * D3DUSAGE_RENDERTARGET surface.  Textures cannot be render targets and no
 * combined usage bits, multisampling or non-LDR format are claimed. */

/* D3DUSAGE values.  The measured 0x00080000 probe is
 * QUERY_POSTPIXELSHADER_BLENDING, not QUERY_SRGBREAD (0x00010000); docs/
 * INIT_TRACE.md uses the wrong label.  Both bits are evaluated separately. */
#define D9REQ_USAGE_RENDERTARGET                    0x00000001u
#define D9REQ_USAGE_DEPTHSTENCIL                    0x00000002u
#define D9REQ_USAGE_WRITEONLY                       0x00000008u
#define D9REQ_USAGE_DYNAMIC                         0x00000200u
#define D9REQ_USAGE_AUTOGENMIPMAP                   0x00000400u
#define D9REQ_USAGE_QUERY_SRGBREAD                  0x00010000u
#define D9REQ_USAGE_QUERY_FILTER                    0x00020000u
#define D9REQ_USAGE_QUERY_SRGBWRITE                 0x00040000u
#define D9REQ_USAGE_QUERY_POSTPIXELSHADER_BLENDING  0x00080000u
#define D9REQ_USAGE_QUERY_WRAPANDMIP                0x00200000u

/* Shader model requests use D3D's (major << 8) | minor encoding. */
#define D9REQ_SHADER_MODEL(major, minor) (((uint32_t)(major) << 8) | (uint32_t)(minor))

typedef struct {
    uint32_t kind;
    uint32_t adapter; /* the bridge has exactly one adapter, ordinal 0 */
    /* CheckDeviceType. */
    uint32_t device_type;
    uint32_t windowed;
    uint32_t display_format;
    uint32_t backbuffer_format;
    /* CheckDeviceFormat. */
    uint32_t adapter_format;
    uint32_t usage;
    uint32_t resource_type;
    uint32_t format;
    /* CheckDepthStencilMatch. */
    uint32_t depth_stencil_format;
    /* GetDeviceCaps scalar probe. */
    uint32_t caps;
    uint32_t value;
} D9ReqRequest;

typedef struct {
    uint32_t decision;
    uint32_t answer;
    uint32_t requirement;
    char reason[D9REQ_REASON_MAX];
    char contract[D9REQ_CONTRACT_MAX];
} D9ReqVerdict;

/* Requirement ids are stable labels for the matrix and the trace line. */
enum {
    D9REQ_ID_NONE = 0,
    D9REQ_ID_DEVICE_TYPE_WINDOWED_LDR,
    D9REQ_ID_DEVICE_TYPE_EXCLUSIVE,
    D9REQ_ID_DEVICE_TYPE_DISPLAY_FORMAT,
    D9REQ_ID_DEVICE_TYPE_BACKBUFFER_FORMAT,
    D9REQ_ID_ADAPTER_FORMAT,
    D9REQ_ID_FORMAT_FLOAT16_HDR,
    D9REQ_ID_FORMAT_SRGB_WRITE,
    D9REQ_ID_FORMAT_SRGB_READ,
    D9REQ_ID_FORMAT_ATOC,
    D9REQ_ID_FORMAT_RESZ,
    D9REQ_ID_FORMAT_DEPTH,
    D9REQ_ID_FORMAT_SURFACE_LDR,
    D9REQ_ID_FORMAT_TEXTURE_LDR,
    D9REQ_ID_FORMAT_USAGE,
    D9REQ_ID_FORMAT_RESOURCE,
    D9REQ_ID_FORMAT_UNKNOWN,
    D9REQ_ID_DEPTH_STENCIL_UNSUPPORTED,
    D9REQ_ID_CAPS_SM2_TEMPLATES,
    D9REQ_ID_CAPS_SM3_FAMILY,
    D9REQ_ID_CAPS_SHADER_OTHER,
    D9REQ_ID_CAPS_MRT_SUPPORTED,
    D9REQ_ID_CAPS_MRT_UNSUPPORTED,
    D9REQ_ID_CAPS_STAGES_SUPPORTED,
    D9REQ_ID_CAPS_STAGES_UNSUPPORTED,
    D9REQ_ID_CAPS_TEXTURE_SIZE_SUPPORTED,
    D9REQ_ID_CAPS_TEXTURE_SIZE_UNSUPPORTED,
    D9REQ_ID_CAPS_VS_CONSTANTS_SUPPORTED,
    D9REQ_ID_CAPS_VS_CONSTANTS_UNSUPPORTED,
    D9REQ_ID_CAPS_DEPTH_TEST_SUPPORTED,
    D9REQ_ID_CAPS_DEPTH_TEST_UNSUPPORTED,
    D9REQ_ID_CAPS_SM3_SLOTS_SUPPORTED,
    D9REQ_ID_CAPS_SM3_SLOTS_UNSUPPORTED,
    D9REQ_ID_CAPS_UNKNOWN,
    D9REQ_ID_INVALID_ADAPTER,
    D9REQ_ID_INVALID_DEVICE_TYPE,
    D9REQ_ID_INVALID_REQUEST,
    D9REQ_ID_UNMATCHED,
    /* Appended after the original ids so their numeric values stay stable. */
    D9REQ_ID_FORMAT_RENDER_TARGET_LDR,
    D9REQ_ID_COUNT
};

/* Convenience constructors.  Unused fields are zero. */
D9ReqRequest d9_req_check_device_type(uint32_t device_type, uint32_t display_format,
                                      uint32_t backbuffer_format, int windowed);
D9ReqRequest d9_req_check_device_format(uint32_t adapter_format, uint32_t usage,
                                        uint32_t resource_type, uint32_t format);
D9ReqRequest d9_req_check_depth_stencil(uint32_t display_format,
                                        uint32_t backbuffer_format,
                                        uint32_t depth_stencil_format);
D9ReqRequest d9_req_device_caps(uint32_t caps, uint32_t value);

/* Evaluate one actual query.  Returns 0 when a verdict was produced (including
 * an unsupported/invalid verdict), -1 for a null request/verdict. */
int d9_req_evaluate(const D9ReqRequest *request, D9ReqVerdict *verdict);

/* Validate the static matrix and its truthfulness invariants.  Returns 0 on
 * success; on failure writes a bounded reason and returns -1. */
int d9_req_matrix_validate(char *why, size_t why_size);
unsigned d9_req_rule_count(void);

/* Names for trace output.  Unknown numeric values return NULL for formats and
 * usages; requirement/decision/answer/cap names always return a string. */
const char *d9_req_decision_name(uint32_t decision);
const char *d9_req_answer_name(uint32_t answer);
const char *d9_req_requirement_name(uint32_t requirement);
const char *d9_req_cap_name(uint32_t caps);
const char *d9_req_format_name(uint32_t format);
const char *d9_req_usage_name(uint32_t usage);

/* One bounded, NUL-terminated trace line.  Returns the number of bytes the
 * complete line needs, excluding the terminator (snprintf semantics), even
 * when it is larger than cap.  buf may be NULL only when cap is 0. */
size_t d9_req_format_probe(const D9ReqRequest *request, const D9ReqVerdict *verdict,
                           char *buf, size_t cap);
