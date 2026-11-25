/* Host test: core-level texture pool accounting, cache ownership lifecycle,
 * SYSTEMMEM behavior, direct SKTX uploads and retryable backend release.
 */

#include "d3d9_core.h"
#include "d3d9_texture_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void check_size(size_t actual, size_t expected, const char *what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        printf("FAIL: %s (%zu != %zu)\n", what, actual, expected);
    }
}

typedef struct {
    unsigned w, h, levels, format, encoded;
} MockTex;

typedef struct {
    unsigned creates, deletes, uploads, release_calls;
    unsigned encoded_creates, encoded_uploads;
    int fail_upload_level;
    int fail_release;
} Mock;

static MockTex *new_tex(Mock *m, unsigned w, unsigned h, unsigned levels,
                        unsigned format, int encoded) {
    MockTex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->w = w; t->h = h; t->levels = levels; t->format = format; t->encoded = (unsigned)encoded;
    if (encoded) ++m->encoded_creates; else ++m->creates;
    return t;
}

static int tex_create(void *ctx, unsigned w, unsigned h, unsigned levels, void **native) {
    Mock *m = ctx;
    MockTex *t = new_tex(m, w, h, levels, D9_A8R8G8B8, 0);
    if (!t) return -1;
    *native = t;
    return 0;
}

static int tex_upload(void *ctx, void *native, unsigned level, const void *data, size_t bytes) {
    Mock *m = ctx; MockTex *t = native;
    if (!t || level >= t->levels || !data || bytes != (size_t)t->w * t->h * 4) return -1;
    if (m->fail_upload_level >= 0 && (int)level == m->fail_upload_level) return -1;
    ++m->uploads;
    return 0;
}

static void tex_delete(void *ctx, void *native) {
    Mock *m = ctx;
    free(native);
    ++m->deletes;
}

static int tex_release(void *ctx, void *native) {
    Mock *m = ctx;
    ++m->release_calls;
    if (m->fail_release || !native) return -1;
    free(native);
    ++m->deletes;
    return 0;
}

static int tex_create_encoded(void *ctx, unsigned w, unsigned h, unsigned levels,
                              unsigned format, void **native) {
    Mock *m = ctx;
    if (format != D9_TEX_ETC1 && format != D9_TEX_ETC1A4) return -1;
    MockTex *t = new_tex(m, w, h, levels, format, 1);
    if (!t) return -1;
    *native = t;
    return 0;
}

static int tex_upload_encoded(void *ctx, void *native, unsigned level,
                              const void *data, size_t bytes) {
    Mock *m = ctx; MockTex *t = native;
    if (!t || !t->encoded || level >= t->levels || !data || !bytes) return -1;
    if (m->fail_upload_level >= 0 && (int)level == m->fail_upload_level) return -1;
    ++m->encoded_uploads;
    return 0;
}

static int begin(void *ctx, uint32_t argb) { (void)ctx; (void)argb; return 0; }
static void end(void *ctx) { (void)ctx; }
static int draw(void *ctx, const D9Vertex *v, size_t n, const D9Matrix *clip,
                void *texture, const D9DrawState *state) {
    (void)ctx; (void)v; (void)n; (void)clip; (void)texture; (void)state; return 0;
}

static const D9Backend ops = {
    tex_create, tex_upload, tex_delete, begin, draw, end,
    tex_release, tex_create_encoded, tex_upload_encoded
};

static void wr16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static size_t build_sktx(uint8_t *out, unsigned format, unsigned w, unsigned h,
                         unsigned levels) {
    size_t table = 24u + (size_t)levels * 12u;
    size_t data = 0;
    memset(out, 0, 512);
    memcpy(out, "SKTX", 4);
    wr32(out + 4, 1);
    wr16(out + 8, format);
    wr16(out + 10, levels);
    wr16(out + 12, w);
    wr16(out + 14, h);
    for (unsigned i = 0; i < levels; ++i) {
        unsigned lw = w >> i, lh = h >> i;
        size_t size = (size_t)((lw + 3u) / 4u) * ((lh + 3u) / 4u) *
                      (format == D9_TEX_ETC1A4 ? 16u : 8u);
        uint8_t *row = out + 24u + (size_t)i * 12u;
        wr16(row, lw); wr16(row + 2, lh);
        wr32(row + 4, (uint32_t)(table + data)); wr32(row + 8, (uint32_t)size);
        for (size_t k = 0; k < size; ++k) out[table + data + k] = (uint8_t)(k + i);
        data += size;
    }
    wr32(out + 16, (uint32_t)data);
    wr32(out + 20, d9_sktx_crc32(out + table, data));
    return table + data;
}

static void test_pools_and_split_accounting(void) {
    Mock m = {.fail_upload_level = -1};
    D9Device *d = d9_create_device(&ops, &m, 1u << 20);
    check(d != NULL, "create device for pool accounting");
    unsigned default_state = 0;
    check(!d9_get_render_state(d, 22, &default_state) && default_state == 2,
          "default cull is CCW");
    check(!d9_get_render_state(d, 25, &default_state) && default_state == 2,
          "default alpha compare is LESS");
    check(!d9_get_sampler_state(d, 1, &default_state) && default_state == 1 &&
          !d9_get_sampler_state(d, 2, &default_state) && default_state == 1,
          "default address mode is WRAP");
    check(!d9_get_sampler_state(d, 5, &default_state) && default_state == 1 &&
          !d9_get_sampler_state(d, 6, &default_state) && default_state == 1,
          "default min/mag filter is POINT");

    D9Handle def = 0, managed = 0, system = 0;
    check(!d9_create_texture_ex(d, 16, 8, D9_A8R8G8B8, 0,
                                D9_POOL_DEFAULT, 0, 0, &def), "create DEFAULT texture");
    check_size(d9_memory_used(d), 1196, "DEFAULT exact guest+GPU accounting");
    unsigned pool = 99, usage = 99, levels = 0;
    size_t guest = 0, gpu = 0; int cpu_only = -1, gpu_only = -1;
    check(!d9_texture_resource_info(d, def, &pool, &usage, NULL, &levels,
                                    &guest, &gpu, &cpu_only, &gpu_only), "DEFAULT resource info");
    check(pool == D9_POOL_DEFAULT && usage == 0 && levels == 5, "DEFAULT pool/usage/levels");
    check_size(guest, 684, "DEFAULT guest bytes"); check_size(gpu, 512, "DEFAULT GPU bytes");
    check(!cpu_only && !gpu_only, "DEFAULT is guest-backed GPU texture");
    check(m.creates == 1 && m.uploads == 0, "DEFAULT allocates native before upload");

    void *pixels = NULL; size_t pitch = 0;
    check(!d9_texture_lock_level(d, def, 0, &pixels, &pitch), "lock DEFAULT level 0");
    check(d9_release_texture(d, def) == D9_INVALID, "locked DEFAULT cannot be released");
    memset(pixels, 0x5a, 16 * 8 * 4);
    check(!d9_texture_unlock_level(d, def, 0), "unlock DEFAULT level 0");
    check(m.uploads == 1, "DEFAULT upload reaches backend");
    check_size(d9_memory_used(d), 1196, "DEFAULT retains guest after upload");
    check(!d9_texture_lock_level(d, def, 0, &pixels, &pitch) && ((uint8_t *)pixels)[0] == 0x5a,
          "DEFAULT CPU readback still works");
    check(!d9_texture_unlock_readonly(d, def, 0), "readonly DEFAULT unlock");
    check(m.uploads == 1, "readonly unlock does not upload");
    check(!d9_set_texture(d, def), "bind uploaded DEFAULT");
    D9VertexElement position_only[] = {
        {0, 0, D9_DECL_FLOAT3, 0, D9_USAGE_POSITION, 0}
    };
    D9Decl no_uv;
    check(!d9_decl_init(&no_uv, position_only, 1), "init position-only declaration");
    check(!d9_set_vertex_declaration(d, &no_uv), "select position-only declaration");
    check(!d9_begin_scene(d, 0), "begin missing-UV draw test");
    check(d9_draw_primitive(d, D9_TRIANGLELIST, 0, 1) == D9_INVALID,
          "textured draw requires declaration UV");
    check(!d9_present(d), "end missing-UV draw test");
    check(!d9_set_vertex_declaration(d, NULL), "restore FVF declaration path");

    check(!d9_create_texture_ex(d, 8, 8, D9_X8R8G8B8, 1,
                                D9_POOL_MANAGED, 0, 1, &managed), "create disposable-marked MANAGED");
    check_size(d9_memory_used(d), 1196 + 256 + 256, "MANAGED exact guest+GPU accounting");
    check(!d9_texture_resource_info(d, managed, &pool, &usage, NULL, NULL,
                                    &guest, &gpu, &cpu_only, &gpu_only), "MANAGED resource info");
    check(pool == D9_POOL_MANAGED && guest == 256 && gpu == 256 && !cpu_only && !gpu_only,
          "MANAGED remains guest-backed");
    check_size(guest, 256, "MANAGED guest bytes");
    check_size(gpu, 256, "MANAGED GPU bytes");
    check(!d9_texture_lock_level(d, managed, 0, &pixels, &pitch), "lock MANAGED");
    check(!d9_texture_unlock_level(d, managed, 0), "upload MANAGED");
    check_size(d9_memory_used(d), 1196 + 256 + 256, "disposable-marked MANAGED still retains guest");
    check(!d9_texture_lock_level(d, managed, 0, &pixels, &pitch) && ((uint8_t *)pixels)[0] == 0,
          "MANAGED restoration source remains lockable");
    check(!d9_texture_unlock_readonly(d, managed, 0), "unlock MANAGED restoration source");

    check(!d9_create_texture_ex(d, 8, 8, D9_A8R8G8B8, 1,
                                D9_POOL_SYSTEMMEM, 0, 0, &system), "create SYSTEMMEM texture");
    check_size(d9_memory_used(d), 1196 + 256 + 256 + 256, "SYSTEMMEM exact CPU-only accounting");
    check(!d9_texture_resource_info(d, system, &pool, &usage, NULL, NULL,
                                    &guest, &gpu, &cpu_only, &gpu_only), "SYSTEMMEM resource info");
    check(pool == D9_POOL_SYSTEMMEM && guest == 256 && gpu == 0 && cpu_only && !gpu_only,
          "SYSTEMMEM pool/usage split");
    unsigned uploads = m.uploads;
    check(!d9_texture_lock_level(d, system, 0, &pixels, &pitch), "lock SYSTEMMEM");
    memset(pixels, 0x33, 256);
    check(!d9_texture_unlock_level(d, system, 0), "unlock SYSTEMMEM");
    check(m.uploads == uploads, "SYSTEMMEM never uploads to GPU");
    check(d9_set_texture(d, system) == D9_INVALID, "SYSTEMMEM cannot be bound");

    check(!d9_release_texture(d, def) && !d9_release_texture(d, managed) &&
          !d9_release_texture(d, system), "release pool fixtures");
    check_size(d9_memory_used(d), 0, "pool fixtures release all bytes");
    check(m.creates == m.deletes, "pool fixtures delete each native once");
    check(!d9_destroy_device_ex(d), "destroy pool device");
}

static void test_upload_failure_retry(void) {
    Mock m = {.fail_upload_level = -1};
    D9Device *d = d9_create_device(&ops, &m, 1u << 20);
    check(d != NULL, "create upload-retry device");
    D9Handle h = 0;
    check(!d9_create_texture(d, 8, 8, D9_A8R8G8B8, &h), "create upload-retry texture");
    void *pixels = NULL; size_t pitch = 0;
    check(!d9_texture_lock_level(d, h, 0, &pixels, &pitch), "lock upload-retry texture");
    m.fail_upload_level = 0;
    check(d9_texture_unlock_level(d, h, 0) == D9_BACKEND, "backend upload failure reported");
    check(d9_set_texture(d, h) == D9_INVALID, "failed upload is not drawable");
    check(d9_release_texture(d, h) == D9_INVALID, "failed upload keeps lock for retry");
    check_size(d9_memory_used(d), 512, "failed upload retains both copies");
    m.fail_upload_level = -1;
    check(!d9_texture_unlock_level(d, h, 0), "retry same locked upload succeeds");
    check(!d9_set_texture(d, h), "retry makes texture drawable");
    check_size(d9_memory_used(d), 512, "retry still retains guest");
    check(!d9_release_texture(d, h), "release upload-retry texture");
    check_size(d9_memory_used(d), 0, "upload-retry final accounting");
    check(!d9_destroy_device_ex(d), "destroy upload-retry device");
}

static void test_direct_sktx_path(void) {
    Mock m = {.fail_upload_level = -1};
    D9Device *d = d9_create_device(&ops, &m, 1u << 20);
    check(d != NULL, "create direct-SKTX device");
    uint8_t blob[512];
    size_t bytes = build_sktx(blob, D9_TEX_ETC1, 16, 16, 2);
    check_size(bytes, 24 + 24 + 128 + 32, "direct SKTX fixture size");

    blob[0] = 'X';
    check(d9_create_texture_sktx(d, blob, bytes, D9_POOL_MANAGED, 0, NULL) == D9_INVALID,
          "malformed SKTX rejected");
    check(m.encoded_creates == 0, "malformed SKTX allocates no GPU texture");
    blob[0] = 'S';

    size_t npot = build_sktx(blob, D9_TEX_ETC1, 7, 7, 1);
    D9Handle bad = 0;
    check(d9_create_texture_sktx(d, blob, npot, D9_POOL_DEFAULT, 0, &bad) == D9_INVALID,
          "semantically invalid SKTX rejected");
    check(m.encoded_creates == 0 && bad == 0, "semantic rejection has no side effects");

    bytes = build_sktx(blob, D9_TEX_ETC1, 16, 16, 2);
    m.fail_upload_level = 1;
    check(d9_create_texture_sktx(d, blob, bytes, D9_POOL_MANAGED, 0, &bad) == D9_BACKEND,
          "direct SKTX upload failure reported");
    check(m.encoded_creates == 1 && m.encoded_uploads == 1 && m.deletes == 1,
          "direct SKTX upload failure deletes partial native");
    check_size(d9_memory_used(d), 0, "direct SKTX failure leaves no accounting");
    m.fail_upload_level = -1;

    D9Handle h = 0;
    check(!d9_create_texture_sktx(d, blob, bytes, D9_POOL_MANAGED, 0, &h),
          "create direct GPU-only SKTX texture");
    check(m.encoded_creates == 2 && m.encoded_uploads == 3, "direct SKTX full chain uploaded");
    check_size(d9_memory_used(d), 160, "direct SKTX exact GPU-only bytes");
    unsigned pool = 99, levels = 0; size_t guest = 99, gpu = 0; int cpu_only = -1, gpu_only = -1;
    check(!d9_texture_resource_info(d, h, &pool, NULL, NULL, &levels,
                                    &guest, &gpu, &cpu_only, &gpu_only), "direct SKTX resource info");
    check(pool == D9_POOL_MANAGED && levels == 2 && guest == 0 && gpu == 160 &&
          !cpu_only && gpu_only, "direct SKTX has no retained guest source");
    void *pixels = NULL; size_t pitch = 0;
    check(d9_texture_lock_level(d, h, 0, &pixels, &pitch) == D9_INVALID,
          "GPU-only SKTX texture has no CPU lock source");
    check(!d9_set_texture(d, h), "direct SKTX texture is immediately drawable");
    check(!d9_release_texture(d, h), "release direct SKTX texture");
    check_size(d9_memory_used(d), 0, "direct SKTX release accounting");
    check(m.encoded_creates == m.deletes, "direct SKTX natives deleted once");
    check(!d9_destroy_device_ex(d), "destroy direct-SKTX device");
}

static void test_release_failure_is_retryable(void) {
    Mock m = {.fail_upload_level = -1};
    D9Device *d = d9_create_device(&ops, &m, 1u << 20);
    check(d != NULL, "create release-retry device");
    D9Handle h = 0;
    check(!d9_create_texture(d, 8, 8, D9_A8R8G8B8, &h), "create release-retry texture");
    m.fail_release = 1;
    check(d9_release_texture(d, h) == D9_RELEASE_FAILED, "GPU release failure reported");
    check_size(d9_memory_used(d), 256, "failed release keeps exact GPU accounting");
    check_size(m.release_calls, 1, "failed GPU release attempted once");
    check(d9_texture_lock_level(d, h, 0, &(void *){0}, &(size_t){0}) == D9_INVALID,
          "released guest is not exposed after partial release");
    m.fail_release = 0;
    check(!d9_release_texture(d, h), "release retry succeeds");
    check_size(m.release_calls, 2, "GPU release retried once");
    check_size(m.deletes, 1, "GPU native deleted exactly once");
    check_size(d9_memory_used(d), 0, "release retry final accounting");

    D9Handle h2 = 0;
    check(!d9_create_texture(d, 8, 8, D9_A8R8G8B8, &h2), "create destroy-retry texture");
    m.fail_release = 1;
    check(d9_destroy_device_ex(d) == D9_RELEASE_FAILED, "device destroy reports release failure");
    check_size(d9_memory_used(d), 256, "failed destroy preserves accounted native");
    m.fail_release = 0;
    check(!d9_destroy_device_ex(d), "device destroy retry succeeds");
    check_size(m.deletes, 2, "destroy retry deletes native exactly once");
}

static void test_exact_device_budget(void) {
    Mock m = {.fail_upload_level = -1};
    D9Device *d = d9_create_device(&ops, &m, 512);
    check(d != NULL, "create exact-budget device");
    D9Handle h = 0, extra = 0;
    check(!d9_create_texture(d, 8, 8, D9_A8R8G8B8, &h), "exact-budget texture fits");
    check_size(d9_memory_used(d), 512, "exact-budget current bytes");
    check(d9_create_texture(d, 8, 8, D9_A8R8G8B8, &extra) == D9_MEMORY,
          "one byte over budget fails before allocation");
    check(m.creates == 1, "budget failure allocates no native");
    check(!d9_release_texture(d, h), "release exact-budget texture");
    check(!d9_create_texture_ex(d, 8, 8, D9_A8R8G8B8, 1,
                                D9_POOL_SYSTEMMEM, 0, 0, &extra), "SYSTEMMEM fits CPU-only budget");
    check_size(d9_memory_used(d), 256, "SYSTEMMEM exact budget bytes");
    check(!d9_destroy_device_ex(d), "destroy exact-budget device");
}

int main(void) {
    test_pools_and_split_accounting();
    test_upload_failure_retry();
    test_direct_sktx_path();
    test_release_failure_is_retryable();
    test_exact_device_budget();
    printf("texture runtime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
