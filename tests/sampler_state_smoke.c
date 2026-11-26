/* IDirect3DDevice9 SetSamplerState/GetSamplerState/SetTexture/GetTexture.
 *
 * The fresh TESV census (E:\Skyrim_N3DS_build\analysis\depthpair_census2.log)
 * showed SetSamplerState(stage 0, type 8 = D3DSAMP_MIPMAPLODBIAS, 0x00000000
 * = 0.0f) returning D3DERR_INVALIDCALL, and SetTexture(stage 2..15, NULL)
 * returning D3DERR_NOTAVAILABLE.  Both are valid D3D9 calls.  This test pins:
 *   - every int/float/dword D3DSAMPLERSTATETYPE the PICA sampler can express
 *     (ADDRESS U/V/W, MAG/MIN/MIP filters, MAXMIPLEVEL, MIPMAPLODBIAS as float
 *     bits, MAXANISOTROPY, BORDERCOLOR, SRGBTEXTURE=0);
 *   - named refusals (exact type/value) for the states PICA cannot express,
 *     with the previous value left intact;
 *   - stage 0..15 texture clears and D3D9 defaults for unbacked stages;
 *   - unchanged caps: DevCaps 0x00082000, no anisotropy/border address, SM2.
 * No frame is claimed; the null backend has no pixels.
 */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_citro3d.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 0x10010600u
#define OUT (BUF + 0x700u)
#define ERR_INVALID 0x8876086cu
#define ERR_UNAVAILABLE 0x8876086au

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "demo call trap object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
        abort();
    }
    return result;
}
static uint32_t device_call(D9ComDemo *s, unsigned slot, const uint32_t *args,
                            unsigned count) {
    return call(s, s->device, slot, args, count);
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&s->memory, address, &value));
    return value;
}
static uint32_t sset(D9ComDemo *s, unsigned stage, unsigned type, uint32_t value) {
    uint32_t args[] = {stage, type, value};
    return device_call(s, 69, args, 3);
}
/* Returns the HRESULT and stores the queried value through *out. */
static uint32_t sget(D9ComDemo *s, unsigned stage, unsigned type, uint32_t *out) {
    uint32_t args[] = {stage, type, OUT};
    uint32_t rc = device_call(s, 68, args, 3);
    if (out) *out = (rc == 0) ? read32(s, OUT) : 0xdeadbeefu;
    return rc;
}
/* A D3DSAMP_MIPMAPLODBIAS argument is the float's IEEE-754 bit pattern. */
static uint32_t bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static int diag_has(const char *needle) {
    const char *d = d9_com_last_diagnostic();
    return d && strstr(d, needle) != NULL;
}
/* Refusal must not change the previous value. */
static void expect_refusal(D9ComDemo *s, unsigned stage, unsigned type,
                           uint32_t value, uint32_t want_rc, const char *needle,
                           uint32_t keep) {
    uint32_t v = 0xdeadbeefu;
    uint32_t rc = sset(s, stage, type, value);
    if (rc != want_rc) {
        fprintf(stderr, "sset(%u,%u,0x%08x)=0x%08x want 0x%08x diag=[%s]\n",
                stage, type, value, rc, want_rc, d9_com_last_diagnostic());
        abort();
    }
    assert(diag_has(needle));
    assert(!sget(s, stage, type, &v) && v == keep);
}

static void lod_bias_projection(void) {
    /* Decoding and PICA clamping are pure state math shared with the backend. */
    assert(d9_sampler_lod_bias(0x00000000u) == 0.0f);
    assert(d9_sampler_lod_bias(bits(-1.5f)) == -1.5f);
    assert(d9_sampler_lod_bias(bits(3.25f)) == 3.25f);
    assert(d9_sampler_lod_bias(bits(1.0e6f)) == D9_SAMP_LOD_BIAS_LIMIT);
    assert(d9_sampler_lod_bias(bits(-1.0e6f)) == -D9_SAMP_LOD_BIAS_LIMIT);
    assert(d9_sampler_lod_bias(0x7fc00000u) == 0.0f); /* NaN never reaches a draw */
    D9DrawState st = {0};
    st.resident_mips = 4;
    st.mag_filter = 2;
    st.min_filter = 2;
    st.mip_filter = 2;
    st.max_mip = 1;
    st.lod_bias = bits(-1.5f);
    D9CitroTextureConfig cfg = d9_citro_texture_config(4, &st);
    assert(cfg.min_level == 1 && cfg.max_level == 3 && cfg.lod_bias == -1.5f);
    st.lod_bias = bits(40.0f);
    cfg = d9_citro_texture_config(4, &st);
    assert(cfg.lod_bias == D9_SAMP_LOD_BIAS_LIMIT);
}

static void caps_unchanged(void) {
    D9Caps9 caps;
    memset(&caps, 0xcd, sizeof caps);
    assert(d9_adapter_caps(0, 1 /* DEVTYPE_HAL */, &caps) == 0);
    assert(caps.dev_caps == 0x00082000u); /* DRAWPRIMITIVES2 | HWRASTERIZATION */
    assert(caps.max_anisotropy == 1);     /* no anisotropic filtering claim */
    assert(caps.texture_filter_caps == 0x00030303u); /* point/linear mag/min/mip */
    assert(caps.texture_address_caps == 0x00000017u); /* wrap/mirror/clamp/independent */
    assert(caps.vertex_shader_version == 0xfffe0200u);
    assert(caps.pixel_shader_version == 0xffff0200u);
    assert(caps.max_texture_blend_stages == 1 && caps.max_simultaneous_textures == 1);
    assert(caps.max_vertex_shader_30_instruction_slots == 0);
    assert(caps.max_pixel_shader_30_instruction_slots == 0);
}

int main(void) {
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);
    /* D3D9 defaults for every new sampler field on a fresh device. */
    D9DrawState fresh;
    assert(!d9_get_draw_state(d, &fresh));
    assert(fresh.address_u == 1 && fresh.address_v == 1 && fresh.address_w == 1);
    assert(fresh.mag_filter == 1 && fresh.min_filter == 1 && fresh.mip_filter == 0);
    assert(fresh.max_mip == 0 && fresh.max_anisotropy == 1);
    assert(fresh.lod_bias == 0 && fresh.srgb == 0 && fresh.border_color == 0);

    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "COM initialization failed: %s\n", demo.message);
        return 1;
    }
    lod_bias_projection();
    caps_unchanged();

    /* ---- the census call: MIPMAPLODBIAS(0.0f) must succeed -------------- */
    uint32_t v = 0;
    assert(sset(&demo, 0, 8, 0) == 0);
    assert(d9_com_last_diagnostic()[0] == 0);
    assert(!sget(&demo, 0, 8, &v) && v == 0);

    /* Float bit patterns round-trip exactly; the backend clamps to PICA. */
    assert(sset(&demo, 0, 8, bits(-1.5f)) == 0);
    assert(!sget(&demo, 0, 8, &v) && v == bits(-1.5f));
    assert(sset(&demo, 0, 8, bits(3.25f)) == 0);
    assert(!sget(&demo, 0, 8, &v) && v == bits(3.25f));
    assert(sset(&demo, 0, 8, bits(1.0e6f)) == 0); /* stored; clamped at draw */
    assert(!sget(&demo, 0, 8, &v) && v == bits(1.0e6f));
    /* NaN is not a D3D9 float argument we can honour; previous value stays. */
    expect_refusal(&demo, 0, 8, 0x7fc00000u, ERR_INVALID, "not a finite float",
                   bits(1.0e6f));
    assert(sset(&demo, 0, 8, bits(-1.5f)) == 0);

    /* ---- integer ranges ------------------------------------------------ */
    assert(sset(&demo, 0, 9, 15) == 0); /* MAXMIPLEVEL */
    assert(!sget(&demo, 0, 9, &v) && v == 15);
    expect_refusal(&demo, 0, 9, 16, ERR_INVALID, "MAXMIPLEVEL=16", 15);
    expect_refusal(&demo, 0, 9, 0xffffffffu, ERR_INVALID, "MAXMIPLEVEL=", 15);
    assert(sset(&demo, 0, 9, 0) == 0);

    assert(sset(&demo, 0, 10, 16) == 0); /* MAXANISOTROPY */
    assert(!sget(&demo, 0, 10, &v) && v == 16);
    expect_refusal(&demo, 0, 10, 0, ERR_INVALID, "MAXANISOTROPY=0", 16);
    expect_refusal(&demo, 0, 10, 17, ERR_INVALID, "MAXANISOTROPY=17", 16);
    assert(sset(&demo, 0, 10, 1) == 0);

    /* ---- filters ------------------------------------------------------- */
    assert(sset(&demo, 0, 5, 1) == 0 && sset(&demo, 0, 5, 2) == 0); /* MAG */
    assert(sset(&demo, 0, 6, 1) == 0 && sset(&demo, 0, 6, 2) == 0); /* MIN */
    assert(!sget(&demo, 0, 5, &v) && v == 2);
    expect_refusal(&demo, 0, 5, 3, ERR_UNAVAILABLE, "ANISOTROPIC", 2);
    expect_refusal(&demo, 0, 6, 3, ERR_UNAVAILABLE, "ANISOTROPIC", 2);
    expect_refusal(&demo, 0, 5, 0, ERR_INVALID, "D3DTEXF_POINT/LINEAR/ANISOTROPIC", 2);
    expect_refusal(&demo, 0, 5, 4, ERR_INVALID, "mip-filter mode", 2);
    expect_refusal(&demo, 0, 6, 5, ERR_INVALID, "mip-filter mode", 2);

    assert(sset(&demo, 0, 7, 0) == 0 && sset(&demo, 0, 7, 1) == 0 &&
           sset(&demo, 0, 7, 2) == 0);
    assert(!sget(&demo, 0, 7, &v) && v == 2);
    expect_refusal(&demo, 0, 7, 3, ERR_INVALID, "not a D3D9 mip filter", 2);
    expect_refusal(&demo, 0, 7, 9, ERR_INVALID, "D3DTEXF_NONE..LINEAR", 2);

    /* ---- address modes -------------------------------------------------- */
    assert(sset(&demo, 0, 1, 1) == 0 && sset(&demo, 0, 1, 2) == 0 &&
           sset(&demo, 0, 1, 3) == 0);
    assert(sset(&demo, 0, 2, 3) == 0);
    assert(sset(&demo, 0, 3, 2) == 0); /* ADDRESSW stored; 2D sampling has no W */
    assert(!sget(&demo, 0, 1, &v) && v == 3);
    assert(!sget(&demo, 0, 3, &v) && v == 2);
    expect_refusal(&demo, 0, 1, 4, ERR_UNAVAILABLE, "D3DTADDRESS_BORDER", 3);
    expect_refusal(&demo, 0, 2, 5, ERR_UNAVAILABLE, "D3DTADDRESS_MIRRORONCE", 3);
    expect_refusal(&demo, 0, 3, 0, ERR_INVALID, "outside D3DTADDRESS_WRAP", 2);
    expect_refusal(&demo, 0, 3, 6, ERR_INVALID, "outside D3DTADDRESS_WRAP", 2);

    /* Border colour is a dword and round-trips; it is only observable with
     * BORDER addressing, which is refused above. */
    assert(sset(&demo, 0, 4, 0xff112233u) == 0);
    assert(!sget(&demo, 0, 4, &v) && v == 0xff112233u);

    /* ---- sRGB / vertex-texture / displacement -------------------------- */
    assert(sset(&demo, 0, 11, 0) == 0);
    expect_refusal(&demo, 0, 11, 1, ERR_UNAVAILABLE, "sRGB", 0);
    expect_refusal(&demo, 0, 11, 2, ERR_INVALID, "not a boolean", 0);
    assert(sset(&demo, 0, 12, 0) == 0);
    expect_refusal(&demo, 0, 12, 1, ERR_UNAVAILABLE, "vertex-texture", 0);
    expect_refusal(&demo, 0, 12, 16, ERR_INVALID, "outside 0..15", 0);
    assert(sset(&demo, 0, 13, 0) == 0);
    expect_refusal(&demo, 0, 13, 4, ERR_UNAVAILABLE, "displacement", 0);

    /* ---- unknown types and stages -------------------------------------- */
    /* An unknown type has no GetSamplerState round-trip, so check the refusal
     * and that no known state changed instead. */
    assert(sset(&demo, 0, 14, 0) == ERR_INVALID);
    assert(diag_has("unknown D3DSAMPLERSTATETYPE 14"));
    assert(!sget(&demo, 0, 1, &v) && v == 3);
    assert(sset(&demo, 16, 9, 0) == ERR_INVALID);
    uint32_t gv = 0;
    assert(sget(&demo, 16, 9, &gv) == ERR_INVALID);

    /* Stage 1 is backed (the null backend has draw_two) and independent. */
    assert(sset(&demo, 1, 1, 3) == 0 && sset(&demo, 1, 7, 2) == 0);
    assert(!sget(&demo, 0, 1, &v) && v == 3); /* same value, still stage-0 state */

    /* Unbacked stages 2..15: the D3D9 default is accepted exactly, anything
     * else is a named refusal instead of a silent loss. */
    assert(sset(&demo, 2, 1, 1) == 0 && sset(&demo, 15, 1, 1) == 0);
    assert(sset(&demo, 15, 5, 1) == 0 && sset(&demo, 15, 6, 1) == 0);
    assert(sset(&demo, 15, 7, 0) == 0 && sset(&demo, 15, 8, 0) == 0);
    assert(sset(&demo, 15, 9, 0) == 0 && sset(&demo, 15, 10, 1) == 0);
    assert(sset(&demo, 15, 11, 0) == 0 && sset(&demo, 15, 4, 0) == 0);
    assert(!sget(&demo, 15, 1, &v) && v == 1);
    assert(!sget(&demo, 15, 8, &v) && v == 0);
    assert(!sget(&demo, 15, 10, &v) && v == 1);
    expect_refusal(&demo, 15, 1, 3, ERR_UNAVAILABLE, "stage 15", 1);
    expect_refusal(&demo, 15, 9, 2, ERR_UNAVAILABLE, "stage 15", 0);
    expect_refusal(&demo, 15, 8, bits(1.0f), ERR_UNAVAILABLE, "stage 15", 0);
    expect_refusal(&demo, 15, 4, 0xff00ff00u, ERR_UNAVAILABLE, "stage 15", 0);

    /* ---- SetTexture/GetTexture over the 16 D3D9 stages ------------------ */
    uint32_t tex[2] = {5, 0};
    assert(device_call(&demo, 65, tex, 2) == 0);
    tex[0] = 15;
    assert(device_call(&demo, 65, tex, 2) == 0);
    tex[0] = 16;
    assert(device_call(&demo, 65, tex, 2) == ERR_INVALID);
    /* Clearing a back-end stage releases it. */
    uint32_t bind0[2] = {0, 0};
    assert(device_call(&demo, 65, bind0, 2) == 0);
    uint32_t gt[2] = {0, OUT};
    assert(device_call(&demo, 64, gt, 2) == 0 && read32(&demo, OUT) == 0);
    gt[0] = 5;
    assert(device_call(&demo, 64, gt, 2) == 0 && read32(&demo, OUT) == 0);
    gt[0] = 16;
    assert(device_call(&demo, 64, gt, 2) == ERR_INVALID);
    /* A real texture cannot live on an unbacked stage: named refusal. */
    tex[0] = 2;
    tex[1] = demo.texture;
    assert(device_call(&demo, 65, tex, 2) == ERR_UNAVAILABLE);
    assert(diag_has("stage 2") && diag_has("no PICA texture unit"));
    /* Backed stages still work and report the bound object. */
    bind0[1] = demo.texture;
    assert(device_call(&demo, 65, bind0, 2) == 0);
    gt[0] = 0;
    assert(device_call(&demo, 64, gt, 2) == 0 && read32(&demo, OUT) == demo.texture);

    /* Sampler state does not block the real fixed-function draw path. */
    assert(!d9_com_demo_frame(&demo, 0.7f));
    assert(d9_com_demo_destroy(&demo));

    puts("sampler state: all D3DSAMPLERSTATETYPE int/float/dword values, named "
         "refusals, D3D9 stage defaults, NULL texture clears and unchanged caps PASS");
    return 0;
}
