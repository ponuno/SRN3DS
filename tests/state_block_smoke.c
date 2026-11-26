/* Bounded IDirect3DStateBlock9 capture/apply roundtrip. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF   0x10010800u
#define OUT   (BUF + 0x000u)
#define MAT   (BUF + 0x100u)
#define LIGHT (BUF + 0x200u)
#define RECT  (BUF + 0x300u)
#define VIEW  (BUF + 0x320u)
#define GUID  (BUF + 0x360u)
#define ERR_INVALID    0x8876086cu
#define ERR_UNAVAILABLE 0x8876086au

static const uint8_t state_block_iid[16] = {
    0xe5,0x4f,0x7c,0xb0,0x0d,0x31,0xa8,0x4b,
    0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b
};

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "state_block: call object=0x%08x slot=%u rc=%d: %s\n",
                object, slot, rc, s->message);
        abort();
    }
    return result;
}
static uint32_t device_call(D9ComDemo *s, unsigned slot,
                            const uint32_t *args, unsigned count) {
    return call(s, s->device, slot, args, count);
}
static uint32_t read32(D9ComDemo *s, uint32_t address) {
    uint32_t value = 0;
    assert(!guest_read32(&s->memory, address, &value));
    return value;
}
static void write32(D9ComDemo *s, uint32_t address, uint32_t value) {
    assert(!guest_write32(&s->memory, address, value));
}
static uint32_t refs(D9ComDemo *s, uint32_t object) {
    uint32_t added = call(s, object, 1, NULL, 0);
    uint32_t remaining = call(s, object, 2, NULL, 0);
    assert(remaining == added - 1);
    return remaining;
}
static void set_rs(D9ComDemo *s, unsigned id, unsigned value) {
    uint32_t args[] = {id, value};
    assert(device_call(s, 57, args, 2) == 0);
}
static unsigned get_rs(D9ComDemo *s, unsigned id) {
    uint32_t args[] = {id, OUT};
    assert(device_call(s, 58, args, 2) == 0);
    return read32(s, OUT);
}
static void set_sampler(D9ComDemo *s, unsigned stage, unsigned id, unsigned value) {
    uint32_t args[] = {stage, id, value};
    assert(device_call(s, 69, args, 3) == 0);
}
static unsigned get_sampler(D9ComDemo *s, unsigned stage, unsigned id) {
    uint32_t args[] = {stage, id, OUT};
    assert(device_call(s, 68, args, 3) == 0);
    return read32(s, OUT);
}
static void set_tss(D9ComDemo *s, unsigned stage, unsigned id, unsigned value) {
    uint32_t args[] = {stage, id, value};
    assert(device_call(s, 67, args, 3) == 0);
}
static unsigned get_tss(D9ComDemo *s, unsigned stage, unsigned id) {
    uint32_t args[] = {stage, id, OUT};
    assert(device_call(s, 66, args, 3) == 0);
    return read32(s, OUT);
}
static void set_texture(D9ComDemo *s, unsigned stage, uint32_t texture) {
    uint32_t args[] = {stage, texture};
    assert(device_call(s, 65, args, 2) == 0);
}
static uint32_t get_texture(D9ComDemo *s, unsigned stage) {
    uint32_t args[] = {stage, OUT};
    assert(device_call(s, 64, args, 2) == 0);
    uint32_t value = read32(s, OUT);
    if (value) assert(call(s, value, 2, NULL, 0) != 0);
    return value;
}
static void set_fvf(D9ComDemo *s, unsigned value) {
    uint32_t args[] = {value};
    assert(device_call(s, 89, args, 1) == 0);
}
static unsigned get_fvf(D9ComDemo *s) {
    uint32_t args[] = {OUT};
    assert(device_call(s, 90, args, 1) == 0);
    return read32(s, OUT);
}
static void set_stream(D9ComDemo *s, unsigned stage, uint32_t vb,
                       uint32_t offset, uint32_t stride) {
    uint32_t args[] = {stage, vb, offset, stride};
    assert(device_call(s, 100, args, 4) == 0);
}
static void get_stream(D9ComDemo *s, unsigned stage, uint32_t *vb,
                       uint32_t *offset, uint32_t *stride) {
    uint32_t args[] = {stage, OUT, OUT + 4, OUT + 8};
    assert(device_call(s, 101, args, 4) == 0);
    *vb = read32(s, OUT); *offset = read32(s, OUT + 4); *stride = read32(s, OUT + 8);
    if (*vb) assert(call(s, *vb, 2, NULL, 0) != 0);
}
static void set_indices(D9ComDemo *s, uint32_t ib) {
    uint32_t args[] = {ib};
    assert(device_call(s, 104, args, 1) == 0);
}
static uint32_t get_indices(D9ComDemo *s) {
    uint32_t args[] = {OUT};
    assert(device_call(s, 105, args, 1) == 0);
    uint32_t value = read32(s, OUT);
    if (value) assert(call(s, value, 2, NULL, 0) != 0);
    return value;
}
static void set_transform(D9ComDemo *s, unsigned type, const D9Matrix *m) {
    uint32_t args[] = {type, MAT};
    assert(!guest_write(&s->memory, MAT, m, sizeof *m));
    assert(device_call(s, 44, args, 2) == 0);
}
static void get_transform(D9ComDemo *s, unsigned type, D9Matrix *m) {
    uint32_t args[] = {type, OUT};
    assert(device_call(s, 45, args, 2) == 0);
    assert(!guest_read(&s->memory, OUT, m, sizeof *m));
}
static void set_material(D9ComDemo *s, const float values[17]) {
    uint32_t args[] = {MAT};
    assert(!guest_write(&s->memory, MAT, values, 17 * sizeof(float)));
    assert(device_call(s, 49, args, 1) == 0);
}
static void get_material(D9ComDemo *s, float values[17]) {
    uint32_t args[] = {OUT};
    assert(device_call(s, 50, args, 1) == 0);
    assert(!guest_read(&s->memory, OUT, values, 17 * sizeof(float)));
}
static void set_light(D9ComDemo *s, unsigned index, const uint32_t light[26]) {
    uint32_t args[] = {index, LIGHT};
    assert(!guest_write(&s->memory, LIGHT, light, 26 * sizeof(uint32_t)));
    assert(device_call(s, 51, args, 2) == 0);
}
static void get_light(D9ComDemo *s, unsigned index, uint32_t light[26]) {
    uint32_t args[] = {index, OUT};
    assert(device_call(s, 52, args, 2) == 0);
    assert(!guest_read(&s->memory, OUT, light, 26 * sizeof(uint32_t)));
}
static void set_light_enable(D9ComDemo *s, unsigned index, unsigned enabled) {
    uint32_t args[] = {index, enabled};
    assert(device_call(s, 53, args, 2) == 0);
}
static unsigned get_light_enable(D9ComDemo *s, unsigned index) {
    uint32_t args[] = {index, OUT};
    assert(device_call(s, 54, args, 2) == 0);
    return read32(s, OUT);
}
static void set_rect(D9ComDemo *s, uint32_t address, const uint32_t rect[4]) {
    uint32_t args[] = {address};
    assert(!guest_write(&s->memory, address, rect, 4 * sizeof(uint32_t)));
    assert(device_call(s, 75, args, 1) == 0);
}
static void get_rect(D9ComDemo *s, uint32_t address, uint32_t rect[4]) {
    uint32_t args[] = {address};
    assert(device_call(s, 76, args, 1) == 0);
    assert(!guest_read(&s->memory, address, rect, 4 * sizeof(uint32_t)));
}
static void set_viewport(D9ComDemo *s, const uint32_t view[6]) {
    uint32_t args[] = {VIEW};
    assert(!guest_write(&s->memory, VIEW, view, 6 * sizeof(uint32_t)));
    assert(device_call(s, 47, args, 1) == 0);
}
static void get_viewport(D9ComDemo *s, uint32_t view[6]) {
    uint32_t args[] = {VIEW};
    assert(device_call(s, 48, args, 1) == 0);
    assert(!guest_read(&s->memory, VIEW, view, 6 * sizeof(uint32_t)));
}
static uint32_t create_decl(D9ComDemo *s) {
    D9VertexElement elements[3] = {
        {0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
        {0xff,0,D9_DECL_UNUSED,0,0,0}
    };
    uint32_t args[] = {MAT, OUT};
    assert(!guest_write(&s->memory, MAT, elements, sizeof elements));
    assert(device_call(s, 86, args, 2) == 0);
    return read32(s, OUT);
}
static void set_decl(D9ComDemo *s, uint32_t decl) {
    uint32_t args[] = {decl};
    assert(device_call(s, 87, args, 1) == 0);
}
static uint32_t get_decl(D9ComDemo *s) {
    uint32_t args[] = {OUT};
    assert(device_call(s, 88, args, 1) == 0);
    uint32_t value = read32(s, OUT);
    if (value) assert(call(s, value, 2, NULL, 0) != 0);
    return value;
}
static uint32_t create_block(D9ComDemo *s, unsigned type) {
    uint32_t args[] = {type, OUT};
    write32(s, OUT, 0xdeadbeefu);
    uint32_t rc = device_call(s, 59, args, 2);
    if (rc == 0) assert(read32(s, OUT) != 0);
    else assert(read32(s, OUT) == 0);
    return rc == 0 ? read32(s, OUT) : 0;
}

int main(void) {
#ifdef _WIN32
    assert(!_putenv_s("TESV_D3D9_TNL_PROTO", "1"));
#else
    assert(!setenv("TESV_D3D9_TNL_PROTO", "1", 1));
#endif
    D9Device *d = d9_create_device(d9_null_backend(), d9_null_context(), 8u << 20);
    assert(d);
    D9ComDemo demo;
    if (d9_com_demo_init(&demo, d)) {
        fprintf(stderr, "state_block: demo init failed: %s\n", demo.message);
        return 1;
    }

    D9Matrix captured_world, changed_world;
    d9_matrix_identity(&captured_world);
    captured_world.m[0][0] = 2.0f; captured_world.m[1][1] = 3.0f; captured_world.m[2][2] = 4.0f;
    d9_matrix_identity(&changed_world);
    const float captured_material[17] = {
        .2f,.3f,.4f,.5f, .1f,.2f,.3f,.4f, .5f,.4f,.3f,.2f,
        .01f,.02f,.03f,.04f, 8.0f
    };
    const float changed_material[17] = {
        .9f,.8f,.7f,.6f, .6f,.7f,.8f,.9f, .1f,.2f,.3f,.4f,
        .4f,.3f,.2f,.1f, 2.0f
    };
    uint32_t captured_light[26] = {0}, changed_light[26] = {0};
    captured_light[0] = D9_LIGHT_POINT;
    changed_light[0] = D9_LIGHT_DIRECTIONAL;
    float *cl = (float *)(captured_light + 1);
    cl[0] = .2f; cl[1] = .3f; cl[2] = .4f; cl[3] = .5f;
    cl[12] = 1.0f; cl[13] = 2.0f; cl[14] = 3.0f;
    cl[18] = 10.0f; cl[19] = 1.0f; cl[20] = 1.0f;
    float *dl = (float *)(changed_light + 1);
    dl[0] = .8f; dl[1] = .7f; dl[2] = .6f; dl[3] = .5f;
    dl[15] = 0.0f; dl[16] = 1.0f; dl[17] = 0.0f;
    const uint32_t full_rect[4] = {0,0,400,240};
    const uint32_t small_rect[4] = {0,0,200,120};
    const uint32_t full_view[6] = {0,0,400,240,0,0x3f800000u};

    set_rs(&demo, 7, 0); set_rs(&demo, 14, 0);
    set_sampler(&demo, 0, 1, 2); set_sampler(&demo, 0, 6, 2);
    set_sampler(&demo, 1, 1, 3); set_sampler(&demo, 1, 6, 2);
    set_tss(&demo, 0, D9_TSS_COLOROP, D9_TOP_MODULATE);
    set_tss(&demo, 1, D9_TSS_COLOROP, D9_TOP_MODULATE);
    set_texture(&demo, 0, demo.texture);
    set_texture(&demo, 1, demo.texture);
    set_fvf(&demo, 0x102);
    set_stream(&demo, 0, demo.vb, 0, sizeof(D9Vertex));
    set_indices(&demo, demo.ib);
    set_transform(&demo, 256, &captured_world);
    set_material(&demo, captured_material);
    set_light(&demo, 0, captured_light);
    set_light_enable(&demo, 0, 1);
    set_rect(&demo, RECT, full_rect);
    set_viewport(&demo, full_view);

    uint32_t texture_refs = refs(&demo, demo.texture);
    uint32_t vb_refs = refs(&demo, demo.vb), ib_refs = refs(&demo, demo.ib);
    uint32_t block = create_block(&demo, 1); /* D3DSBT_ALL */
    assert(block && refs(&demo, demo.texture) == texture_refs + 2);
    assert(refs(&demo, demo.vb) == vb_refs + 1 && refs(&demo, demo.ib) == ib_refs + 1);
    assert(refs(&demo, block) == 1);

    /* Concrete state-block QI and GetDevice retain/release correctly. */
    assert(!guest_write(&demo.memory, GUID, state_block_iid, sizeof state_block_iid));
    uint32_t qi[] = {GUID, OUT};
    assert(call(&demo, block, 0, qi, 2) == 0 && read32(&demo, OUT) == block);
    assert(call(&demo, block, 2, NULL, 0) == 1);
    uint32_t gd[] = {OUT};
    assert(call(&demo, block, 3, gd, 1) == 0 && read32(&demo, OUT) == demo.device);
    assert(call(&demo, demo.device, 2, NULL, 0) != 0);

    set_rs(&demo, 7, 1); set_sampler(&demo, 0, 1, 1);
    set_sampler(&demo, 1, 1, 1);
    set_tss(&demo, 0, D9_TSS_COLOROP, D9_TOP_SELECTARG1);
    set_tss(&demo, 1, D9_TSS_COLOROP, D9_TOP_SELECTARG1);
    set_texture(&demo, 0, 0); set_texture(&demo, 1, 0); set_fvf(&demo, 0x104);
    set_stream(&demo, 0, 0, 0, 0); set_indices(&demo, 0);
    set_transform(&demo, 256, &changed_world);
    set_material(&demo, changed_material);
    set_light(&demo, 0, changed_light); set_light_enable(&demo, 0, 0);
    set_rect(&demo, RECT, small_rect);

    assert(call(&demo, block, 5, NULL, 0) == 0);
    assert(get_rs(&demo, 7) == 0 && get_rs(&demo, 14) == 0);
    assert(get_sampler(&demo, 0, 1) == 2 && get_sampler(&demo, 0, 6) == 2);
    assert(get_sampler(&demo, 1, 1) == 3 && get_sampler(&demo, 1, 6) == 2);
    assert(get_tss(&demo, 0, D9_TSS_COLOROP) == D9_TOP_MODULATE);
    assert(get_tss(&demo, 1, D9_TSS_COLOROP) == D9_TOP_MODULATE);
    assert(get_texture(&demo, 0) == demo.texture);
    assert(get_texture(&demo, 1) == demo.texture);
    assert(get_fvf(&demo) == 0x102);
    uint32_t vb = 0, offset = 0, stride = 0, ib = 0, rect[4], view[6];
    get_stream(&demo, 0, &vb, &offset, &stride);
    assert(vb == demo.vb && offset == 0 && stride == sizeof(D9Vertex));
    assert(get_indices(&demo) == demo.ib);
    get_transform(&demo, 256, &changed_world);
    assert(!memcmp(&changed_world, &captured_world, sizeof captured_world));
    float material[17]; uint32_t light[26];
    get_material(&demo, material);
    assert(!memcmp(material, captured_material, sizeof material));
    get_light(&demo, 0, light);
    assert(!memcmp(light, captured_light, sizeof light));
    assert(get_light_enable(&demo, 0) == 1);
    get_rect(&demo, RECT, rect); assert(!memcmp(rect, full_rect, sizeof rect));
    get_viewport(&demo, view); assert(!memcmp(view, full_view, sizeof view));
    assert(refs(&demo, demo.texture) == texture_refs + 2);
    assert(refs(&demo, demo.vb) == vb_refs + 1 && refs(&demo, demo.ib) == ib_refs + 1);
    assert(call(&demo, block, 5, NULL, 0) == 0);
    assert(refs(&demo, demo.texture) == texture_refs + 2);

    /* Capture replaces the snapshot atomically and can be applied repeatedly. */
    set_rs(&demo, 7, 1);
    assert(call(&demo, block, 4, NULL, 0) == 0);
    set_rs(&demo, 7, 0);
    assert(call(&demo, block, 5, NULL, 0) == 0 && get_rs(&demo, 7) == 1);
    assert(call(&demo, block, 5, NULL, 0) == 0 && get_rs(&demo, 7) == 1);
    set_rs(&demo, 7, 0);

    /* Pixel/vertex blocks touch only their own half. */
    uint32_t pixel = create_block(&demo, 2); assert(pixel);
    set_fvf(&demo, 0x104); set_rs(&demo, 7, 1);
    assert(call(&demo, pixel, 5, NULL, 0) == 0);
    assert(get_rs(&demo, 7) == 0 && get_fvf(&demo) == 0x104);
    assert(call(&demo, pixel, 2, NULL, 0) == 0);

    uint32_t vertex = create_block(&demo, 3); assert(vertex);
    set_fvf(&demo, 0x102); set_rs(&demo, 7, 1);
    assert(call(&demo, vertex, 5, NULL, 0) == 0);
    assert(get_fvf(&demo) == 0x104 && get_rs(&demo, 7) == 1);
    assert(call(&demo, vertex, 2, NULL, 0) == 0);

    /* A declaration block restores the declaration object, not an FVF alias. */
    uint32_t decl = create_decl(&demo); assert(decl);
    set_decl(&demo, decl);
    uint32_t decl_refs = refs(&demo, decl);
    uint32_t decl_block = create_block(&demo, 3); assert(decl_block);
    assert(refs(&demo, decl) == decl_refs + 1);
    set_fvf(&demo, 0x102);
    assert(call(&demo, decl_block, 5, NULL, 0) == 0);
    assert(get_decl(&demo) == decl && get_fvf(&demo) == 0);
    assert(refs(&demo, decl) == decl_refs + 1);
    assert(call(&demo, decl_block, 2, NULL, 0) == 0);
    assert(refs(&demo, decl) == decl_refs);
    set_fvf(&demo, 0x102);
    assert(call(&demo, decl, 2, NULL, 0) == 0);

    /* Cube and volume bindings use the same retain/release path as 2D textures. */
    uint32_t face_pixels[64], cube = 0;
    for (unsigned i = 0; i < 64; ++i) face_pixels[i] = 0xff102030u + i;
    assert(d9_com_host_create_cube_texture(demo.device, 8, 1, 0,
            D9_A8R8G8B8, 0, &cube) == 0 && cube);
    for (unsigned face = 0; face < 6; ++face)
        assert(d9_com_host_upload_cube_face_level(cube, face, 0, face_pixels,
               8 * sizeof(uint32_t), sizeof face_pixels) == 0);
    set_texture(&demo, 0, cube);
    uint32_t cube_refs = refs(&demo, cube);
    uint32_t cube_block = create_block(&demo, 1); assert(cube_block);
    assert(refs(&demo, cube) == cube_refs + 1);
    set_texture(&demo, 0, 0);
    assert(call(&demo, cube_block, 5, NULL, 0) == 0);
    assert(get_texture(&demo, 0) == cube);
    assert(refs(&demo, cube) == cube_refs + 1);
    assert(call(&demo, cube_block, 2, NULL, 0) == 0);
    assert(refs(&demo, cube) == cube_refs);
    set_texture(&demo, 0, 0);
    assert(call(&demo, cube, 2, NULL, 0) == 0);

    uint32_t volume_pixels[128], volume = 0;
    for (unsigned i = 0; i < 128; ++i) volume_pixels[i] = 0xff405060u + i;
    assert(d9_com_host_create_volume_texture(demo.device, 8, 8, 2, 1, 0,
            D9_A8R8G8B8, 0, &volume) == 0 && volume);
    assert(d9_com_host_upload_volume_level(volume, 0, volume_pixels,
           8 * sizeof(uint32_t), 8 * 8 * sizeof(uint32_t), sizeof volume_pixels) == 0);
    set_texture(&demo, 0, volume);
    uint32_t volume_refs = refs(&demo, volume);
    uint32_t volume_block = create_block(&demo, 1); assert(volume_block);
    assert(refs(&demo, volume) == volume_refs + 1);
    set_texture(&demo, 0, 0);
    assert(call(&demo, volume_block, 5, NULL, 0) == 0);
    assert(get_texture(&demo, 0) == volume);
    assert(refs(&demo, volume) == volume_refs + 1);
    assert(call(&demo, volume_block, 2, NULL, 0) == 0);
    assert(refs(&demo, volume) == volume_refs);
    set_texture(&demo, 0, 0);
    assert(call(&demo, volume, 2, NULL, 0) == 0);
    set_texture(&demo, 0, demo.texture);

    /* Begin/End records every supported state change through End. */
    uint32_t end_args[] = {OUT};
    write32(&demo, OUT, 0xdeadbeefu);
    assert(device_call(&demo, 61, end_args, 1) == ERR_INVALID);
    assert(read32(&demo, OUT) == 0);
    set_rs(&demo, 7, 0);
    assert(device_call(&demo, 60, NULL, 0) == 0);
    assert(device_call(&demo, 60, NULL, 0) == ERR_INVALID);
    set_rs(&demo, 7, 1);
    assert(device_call(&demo, 61, end_args, 1) == 0);
    uint32_t recorded = read32(&demo, OUT); assert(recorded);
    set_rs(&demo, 7, 0);
    assert(call(&demo, recorded, 5, NULL, 0) == 0 && get_rs(&demo, 7) == 1);
    assert(call(&demo, recorded, 2, NULL, 0) == 0);
    set_rs(&demo, 7, 0);

    /* Unsupported type is named and side-effect free. */
    uint32_t before_refs = refs(&demo, demo.texture);
    assert(create_block(&demo, 0) == 0);
    assert(device_call(&demo, 59, (uint32_t[]){0, OUT}, 2) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "D3DSTATEBLOCKTYPE"));
    assert(refs(&demo, demo.texture) == before_refs && get_rs(&demo, 7) == 0);

    /* A locked bound resource refuses Capture without changing state/refcounts. */
    uint32_t locked = create_block(&demo, 1); assert(locked);
    uint32_t lock_args[] = {0, OUT, 0, 0};
    assert(call(&demo, demo.texture, 19, lock_args, 4) == 0);
    before_refs = refs(&demo, demo.texture);
    assert(call(&demo, locked, 4, NULL, 0) == ERR_UNAVAILABLE);
    assert(strstr(d9_com_last_diagnostic(), "locked"));
    assert(refs(&demo, demo.texture) == before_refs && get_rs(&demo, 7) == 0);
    assert(device_call(&demo, 60, NULL, 0) == 0);
    write32(&demo, OUT, 0xdeadbeefu);
    assert(device_call(&demo, 61, end_args, 1) == ERR_UNAVAILABLE);
    assert(read32(&demo, OUT) == 0);
    uint32_t unlock_args[] = {0};
    assert(call(&demo, demo.texture, 20, unlock_args, 1) == 0);
    assert(device_call(&demo, 61, end_args, 1) == ERR_INVALID);
    assert(call(&demo, locked, 2, NULL, 0) == 0);

    assert(call(&demo, block, 2, NULL, 0) == 0);
    assert(refs(&demo, demo.texture) == texture_refs);
    assert(refs(&demo, demo.vb) == vb_refs && refs(&demo, demo.ib) == ib_refs);

    /* Teardown: no block remains, and the demo releases every resource once. */
    set_texture(&demo, 0, 0); set_texture(&demo, 1, 0);
    set_stream(&demo, 0, 0, 0, 0); set_indices(&demo, 0);
    set_fvf(&demo, 0x102);
    assert(!d9_com_demo_destroy(&demo));
    assert(d9_destroy_device_ex(d) == 0);
#ifdef _WIN32
    assert(!_putenv_s("TESV_D3D9_TNL_PROBE", ""));
#else
    assert(!unsetenv("TESV_D3D9_TNL_PROBE"));
#endif
    puts("state blocks: create/capture/apply, type masks, refcounts, Begin/End, refusals and teardown PASS");
    return 0;
}
