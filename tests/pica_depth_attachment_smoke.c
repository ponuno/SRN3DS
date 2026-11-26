/* PICA embedded-depth policy, exercised through the backend shape only.
 *
 * PICA colour targets are created with a DEPTH24_STENCIL8 attachment, so a D3D9
 * depth-stencil surface is a size descriptor: a matching SetDepthStencilSurface
 * is a no-op S_OK and a mismatch is D3DERR_INVALIDCALL naming the embedded
 * attachment.  Clear(Z) must still reach the backend depth clear with the
 * packed 24-bit depth/8-bit stencil value.  The null-backend shape without the
 * embedded-depth callback keeps the old separate-object semantics: match S_OK,
 * mismatch D3DERR_NOTAVAILABLE.  DevCaps stay 0x00082000. */
#include "d3d9_core.h"
#include "d3d9_null.h"
#include "d3d9_adapter.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned embedded_queries, clear_calls;
static unsigned last_clear_flags;
static uint32_t last_clear_value;

static int pica_embedded(void *ctx) {
    (void)ctx;
    ++embedded_queries;
    return 1;
}

static int record_clear(void *ctx, unsigned flags, uint32_t value) {
    ++clear_calls;
    last_clear_flags = flags;
    last_clear_value = value;
    return d9_null_backend()->depth_clear(ctx, flags, value);
}

static uint32_t packed_z(float z, uint32_t stencil) {
    uint32_t q = (uint32_t)(z * 16777215.0f + 0.5f);
    if (q > 0xffffffu) q = 0xffffffu;
    return q | ((stencil & 0xffu) << 24);
}

static void setup(D9Device *d, D9Handle *target, D9Handle *depth16,
                  D9Handle *depth8) {
    assert(!d9_set_back_buffer_info(d, 400, 240, D9_X8R8G8B8));
    assert(!d9_create_render_target(d, 16, 16, D9_A8R8G8B8, 0, target));
    assert(!d9_create_depth_surface(d, 16, 16, D9_D24S8, 0, 0, 0, depth16));
    assert(!d9_create_depth_surface(d, 8, 8, D9_D24S8, 0, 0, 0, depth8));
    assert(!d9_begin_scene(d, 0));
    assert(!d9_set_render_target(d, *target));
}

int main(void) {
    D9Backend ops = *d9_null_backend();
    ops.depth_attachment_embedded = pica_embedded;
    ops.depth_clear = record_clear;
    D9Device *d = d9_create_device(&ops, d9_null_context(), 4u << 20);
    assert(d);

    D9Handle target = 0, depth16 = 0, depth8 = 0;
    setup(d, &target, &depth16, &depth8);
    assert(d9_null_stats()->target_creates >= 1);

    /* Matching descriptor: the active target already owns the attachment. */
    assert(!d9_set_depth_surface(d, depth16));
    assert(d9_get_depth_surface(d) == depth16);
    assert(!d9_clear_depth(d, 2, 0.5f, 0));
    assert(clear_calls == 1 && last_clear_flags == 2);
    assert(last_clear_value == packed_z(0.5f, 0));

    /* Mismatch is a named INVALIDCALL and leaves the binding unchanged. */
    int rc = d9_set_depth_surface(d, depth8);
    assert(rc == D9_INVALID);
    assert(embedded_queries > 0);
    assert(strstr(d9_last_draw_diagnostic(d), "embedded depth") != NULL);
    assert(d9_get_depth_surface(d) == depth16);

    D9Caps9 caps;
    assert(!d9_adapter_caps(0, 1, &caps));
    assert(caps.dev_caps == 0x00082000u);
    assert(!d9_end_scene(d));
    d9_destroy_device(d);

    /* The null shape has no embedded callback: separate-depth semantics. */
    D9Backend plain = *d9_null_backend();
    D9Device *p = d9_create_device(&plain, d9_null_context(), 4u << 20);
    assert(p);
    D9Handle ptarget = 0, p16 = 0, p8 = 0;
    setup(p, &ptarget, &p16, &p8);
    assert(!d9_set_depth_surface(p, p16));
    assert(d9_set_depth_surface(p, p8) == D9_INVALID);
    assert(strstr(d9_last_draw_diagnostic(p), "does not match") != NULL);
    assert(d9_get_depth_surface(p) == p16);
    assert(!d9_end_scene(p));
    d9_destroy_device(p);

    puts("PICA embedded depth: matching descriptor S_OK, mismatch INVALIDCALL, "
         "Clear(Z) reaches backend, null shape keeps NOTAVAILABLE, caps unchanged PASS");
    return 0;
}
