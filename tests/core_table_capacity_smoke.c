/* Core resource tables: capacity, named exhaustion, and release/reuse.  The
 * measured engine keeps ~30 textures plus cube faces, depth and render targets;
 * 32-entry tables were too small.  Every table must name itself on exhaustion. */
#include "d3d9_core.h"
#include "d3d9_null.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEX_LIMIT 128u
#define TARGET_LIMIT 128u
#define CUBE_LIMIT 32u
#define VOLUME_LIMIT 32u
#define DEPTH_LIMIT 32u

static void expect(const D9Device *d, const char *needle) {
    const char *why = d9_last_draw_diagnostic(d);
    if (!why || !strstr(why, needle)) {
        fprintf(stderr, "core table diagnostic '%s' lacks '%s'\n",
                why ? why : "<null>", needle);
        assert(0);
    }
}

int main(void) {
    d9_null_reset();
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(),
                                   8u << 20);
    assert(d);
    assert(d9_set_back_buffer_info_ex(d, 400, 240, D9_X8R8G8B8, 0) == D9_OK);
    D9Handle h[128];

    for (unsigned i = 0; i < TEX_LIMIT; ++i)
        assert(d9_create_texture_ex(d, 8, 8, D9_A8R8G8B8, 1, D9_POOL_MANAGED,
                                    0, 0, &h[i]) == D9_OK);
    assert(d9_create_texture_ex(d, 8, 8, D9_A8R8G8B8, 1, D9_POOL_MANAGED,
                                0, 0, &h[0]) == D9_MEMORY);
    expect(d, "texture table exhausted (128 entries)");
    for (unsigned i = 0; i < TEX_LIMIT; ++i) d9_release_texture(d, h[i]);

    for (unsigned i = 0; i < TARGET_LIMIT; ++i)
        assert(d9_create_render_target(d, 8, 8, D9_A8R8G8B8, 0, &h[i]) == D9_OK);
    assert(d9_create_render_target(d, 8, 8, D9_A8R8G8B8, 0, &h[0]) == D9_MEMORY);
    expect(d, "render-target table exhausted (128 entries)");
    for (unsigned i = 0; i < TARGET_LIMIT; ++i) d9_release_render_target(d, h[i]);

    for (unsigned i = 0; i < CUBE_LIMIT; ++i)
        assert(d9_create_cube_texture_ex(d, 8, 1, D9_A8R8G8B8, D9_POOL_MANAGED,
                                         0, &h[i]) == D9_OK);
    assert(d9_create_cube_texture_ex(d, 8, 1, D9_A8R8G8B8, D9_POOL_MANAGED,
                                     0, &h[0]) == D9_MEMORY);
    expect(d, "cube texture table exhausted (32 entries)");
    for (unsigned i = 0; i < CUBE_LIMIT; ++i) d9_release_cube_texture(d, h[i]);

    for (unsigned i = 0; i < VOLUME_LIMIT; ++i)
        assert(d9_create_volume_texture_ex(d, 8, 8, 2, 1, D9_A8R8G8B8,
                                           D9_POOL_MANAGED, 0, &h[i]) == D9_OK);
    assert(d9_create_volume_texture_ex(d, 8, 8, 2, 1, D9_A8R8G8B8,
                                       D9_POOL_MANAGED, 0, &h[0]) == D9_MEMORY);
    expect(d, "volume texture table exhausted (32 entries)");
    for (unsigned i = 0; i < VOLUME_LIMIT; ++i) d9_release_volume_texture(d, h[i]);

    for (unsigned i = 0; i < DEPTH_LIMIT; ++i)
        assert(d9_create_depth_surface(d, 8, 8, D9_D24S8, 0, 0, 0,
                                       &h[i]) == D9_OK);
    assert(d9_create_depth_surface(d, 8, 8, D9_D24S8, 0, 0, 0, &h[0])
           == D9_MEMORY);
    expect(d, "depth-stencil table exhausted (32 entries)");
    for (unsigned i = 0; i < DEPTH_LIMIT; ++i) d9_release_depth_surface(d, h[i]);

    d9_destroy_device(d);
    puts("PASS core_table_capacity: 128 textures/targets, 32 cubes/volumes/depths, named exhaustion");
    return 0;
}
