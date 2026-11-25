#include "d3d9_capture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_BASE 0x100000u
#define GUEST_SIZE 0x100000u
#define VS_ADDR    0x100000u
#define PS_ADDR    0x180000u
#define IB_ADDR    0x1f0000u
#define BAD_ADDR   0x300000u

/* Section ids from source/d3d9_capture.c; the smoke test validates the
 * version 2 additions explicitly. */
enum { TEST_SECTION_TARGET = 16, TEST_SECTION_PRESENT = 17 };

static GuestMemory g_memory;
static D9Vertex g_vertices[300];
static uint8_t g_texture[64];

static void identity(D9Matrix *m) {
    memset(m, 0, sizeof *m);
    for (unsigned i = 0; i < 4; ++i) m->m[i][i] = 1.0f;
}

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static void cleanup(const char *base, unsigned count) {
    char path[512];
    for (unsigned n = 1; n <= count; ++n) {
        char suffix[16] = "";
        if (n > 1) snprintf(suffix, sizeof suffix, "_%u", n);
        snprintf(path, sizeof path, "%s%s.txt", base, suffix);
        remove(path);
        snprintf(path, sizeof path, "%s%s.bin", base, suffix);
        remove(path);
    }
}

static void init_capture(const char *base, unsigned max_records, unsigned flags) {
    D9CaptureOptions options;
    d9_capture_options_init(&options);
    options.directory = ".";
    options.basename = base;
    options.max_records = max_records;
    options.flags = flags;
    assert(d9_capture_init(&options) == D9_CAPTURE_OK);
}

static void begin_capture(const char *base, unsigned max_records, unsigned flags) {
    cleanup(base, 8);
    init_capture(base, max_records, flags);
}

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *data = malloc((size_t)length + 1);
    assert(data);
    if (length) assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    data[length] = 0;
    fclose(file);
    *size = (size_t)length;
    return data;
}

static int file_contains(const char *path, const char *needle) {
    size_t size;
    unsigned char *data = read_file(path, &size);
    int found = strstr((const char *)data, needle) != NULL;
    free(data);
    return found;
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

/* Walk the section table and return the payload, or NULL. */
static const unsigned char *find_section(const unsigned char *data, size_t size,
                                         uint32_t id, uint32_t *payload) {
    if (size < 32) return NULL;
    uint32_t sections = rd32(data + 28);
    size_t off = 32;
    for (uint32_t i = 0; i < sections; ++i) {
        if (off + 8 > size) return NULL;
        uint32_t section = rd32(data + off);
        uint32_t length = rd32(data + off + 4);
        off += 8;
        if (off + length > size) return NULL;
        if (section == id) {
            if (payload) *payload = length;
            return data + off;
        }
        off += length;
    }
    return NULL;
}

static void seed_guest(void) {
    uint8_t vs[32], ps[24], ib[6] = {0, 0, 1, 0, 2, 0};
    for (unsigned i = 0; i < sizeof vs; ++i) vs[i] = (uint8_t)(i * 7 + 3);
    for (unsigned i = 0; i < sizeof ps; ++i) ps[i] = (uint8_t)(i * 11 + 5);
    assert(guest_write(&g_memory, VS_ADDR, vs, sizeof vs) == 0);
    assert(guest_write(&g_memory, PS_ADDR, ps, sizeof ps) == 0);
    assert(guest_write(&g_memory, IB_ADDR, ib, sizeof ib) == 0);
}

static void make_base(D9CaptureSnapshot *s) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < 3; ++i) {
        g_vertices[i] = (D9Vertex){(float)i, (float)(i + 1), 0.5f, 0.0f, (float)i};
    }
    for (unsigned i = 0; i < sizeof g_texture; ++i) g_texture[i] = (uint8_t)(i * 13 + 1);

    s->frame_index = 1;
    s->draw_index = 2;
    s->draw.type = D9_TRIANGLELIST;
    s->draw.start = 0;
    s->draw.primitives = 1;
    s->draw.indexed = 1;
    s->draw.base_vertex = 0;
    s->draw.min_vertex = 0;
    s->draw.vertex_count = 3;
    s->draw.start_index = 0;
    s->draw.vertices.data.host = g_vertices;
    s->draw.vertices.data.bytes = 3 * sizeof(D9Vertex);
    s->draw.vertices.stride = sizeof(D9Vertex);
    s->draw.vertices.count = 3;
    s->draw.indices.data.guest = IB_ADDR;
    s->draw.indices.data.bytes = 6;
    s->draw.indices.size = 2;
    s->draw.indices.count = 3;

    s->viewport = (D9CaptureViewport){0, 0, 400, 240, 0.0f, 1.0f};

    s->state.z_enable = 1;
    s->state.z_write = 1;
    s->state.z_func = 4;
    s->state.cull = 2;
    s->state.alpha_func = 2;
    s->state.src_blend = 2;
    s->state.dst_blend = 1;
    s->state.blend_op = 1;
    s->state.color_write = 15;
    s->state.address_u = 1;
    s->state.address_v = 1;
    s->state.mag_filter = 1;
    s->state.min_filter = 1;
    s->state.resident_mips = 1;

    identity(&s->world);
    identity(&s->view);
    identity(&s->projection);
    s->world.m[3][0] = 4.0f;
    s->world.m[3][2] = -2.0f;

    s->vs.object = 0x50002030u;
    s->vs.code.guest = VS_ADDR;
    s->vs.code.bytes = 32;
    s->ps.object = 0x50002040u;
    s->ps.code.guest = PS_ADDR;
    s->ps.code.bytes = 24;

    s->declaration.active = 1;
    s->declaration.count = 2;
    s->declaration.position = 0;
    s->declaration.uv = 1;
    s->declaration.stream_mask = 1;
    s->declaration.required[0] = 20;
    s->declaration.elements[0] = (D9VertexElement){0, 0, D9_DECL_FLOAT3, 0,
                                                   D9_USAGE_POSITION, 0};
    s->declaration.elements[1] = (D9VertexElement){0, 12, D9_DECL_FLOAT2, 0,
                                                   D9_USAGE_TEXCOORD, 0};

    s->stream_count = 1;
    s->streams[0] = (D9CaptureStream){0, 0x50001000u, sizeof(D9Vertex), 0,
                                      3 * sizeof(D9Vertex), 0};

    s->texture_count = 1;
    s->textures[0] = (D9CaptureTexture){
        0, 0x50003000u, 4, 4, D9_A8R8G8B8, 1, D9_POOL_DEFAULT, 0, 0, 16,
        {g_texture, 0, sizeof g_texture}};

    s->clear = (D9CaptureClear){1, 0xff203040u, 1, 0};
    s->present = (D9CapturePresent){1, 0, 0};
    s->target = (D9CaptureRenderTarget){0x50004000u, 400, 240, D9_A8R8G8B8, 0, 0};
}

static void check_binary_header(const D9CaptureRecordInfo *info) {
    size_t size;
    unsigned char *data = read_file(info->binary_path, &size);
    assert(size >= 32);
    assert(memcmp(data, "D9CAPBIN", 8) == 0);
    assert(rd32(data + 8) == D9_CAPTURE_SCHEMA_VERSION);
    assert(rd32(data + 12) == 32);
    assert(rd64(data + 16) == info->signature);
    assert(rd32(data + 24) == info->truncation);
    assert(rd32(data + 28) == 14 + info->stream_count + info->texture_count);
    uint32_t target_bytes = 0;
    assert(find_section(data, size, TEST_SECTION_TARGET, &target_bytes) != NULL);
    assert(target_bytes == 24);
    free(data);
    assert(info->binary_bytes == size);
}

static void test_fnv(void) {
    assert(d9_capture_fnv1a("", 0) == UINT64_C(0xcbf29ce484222325));
    assert(d9_capture_fnv1a("a", 1) == UINT64_C(0xaf63dc4c8601ec8c));
    assert(d9_capture_fnv1a("foobar", 6) == UINT64_C(0x85944171f73967e8));
    char text[64];
    assert(d9_capture_truncation_text(0, text, sizeof text) > 0);
    assert(strcmp(text, "none") == 0);
    assert(d9_capture_truncation_text(D9_CAPTURE_TRUNC_VS | D9_CAPTURE_TRUNC_TEXTURE_TOTAL,
                                      text, sizeof text) > 0);
    assert(strcmp(text, "vs,texture_total") == 0);
}

static void test_first_draw(void) {
    begin_capture("d9cap_basic", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);

    int error = 123;
    uint64_t preview = d9_capture_signature(&s, &g_memory, &error);
    assert(error == D9_CAPTURE_OK && preview != 0);

    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    assert(d9_capture_count() == 1);
    assert(!d9_capture_active());
    assert(d9_capture_remaining() == 0);

    D9CaptureRecordInfo info;
    assert(d9_capture_record(0, &info) == D9_CAPTURE_OK);
    assert(info.index == 1);
    assert(info.signature == preview);
    assert(info.truncation == 0);
    assert(info.vertex_count == 3 && info.vertex_captured == 3);
    assert(info.index_count == 3 && info.index_captured == 3);
    assert(info.vs_bytes == 32 && info.ps_bytes == 24);
    assert(info.texture_bytes == 64);
    assert(info.text_bytes > 0 && info.binary_bytes > 0);
    uint8_t expected_vs[32];
    uint8_t expected_ps[24];
    static const uint8_t expected_indices[6] = {0, 0, 1, 0, 2, 0};
    for (unsigned i = 0; i < sizeof expected_vs; ++i) expected_vs[i] = (uint8_t)(i * 7 + 3);
    for (unsigned i = 0; i < sizeof expected_ps; ++i) expected_ps[i] = (uint8_t)(i * 11 + 5);
    assert(info.vs_hash == d9_capture_fnv1a(expected_vs, sizeof expected_vs));
    assert(info.ps_hash == d9_capture_fnv1a(expected_ps, sizeof expected_ps));
    assert(strcmp(info.text_path, "d9cap_basic.txt") == 0);
    assert(strcmp(info.binary_path, "d9cap_basic.bin") == 0);
    assert(strcmp(d9_capture_last_text_path(), info.text_path) == 0);
    assert(strcmp(d9_capture_last_binary_path(), info.binary_path) == 0);
    assert(exists(info.text_path) && exists(info.binary_path));

    assert(file_contains(info.text_path, "format=d9_capture"));
    assert(file_contains(info.text_path, "version=2"));
    assert(file_contains(info.text_path, "signature=0x"));
    assert(file_contains(info.text_path, "state.z_enable=1"));
    assert(file_contains(info.text_path, "world.r0=1,0,0,0"));
    assert(file_contains(info.text_path, "vs.hash=0x"));
    assert(file_contains(info.text_path, "declaration.element[0]="));
    assert(file_contains(info.text_path, "stream[0].stride=20"));
    assert(file_contains(info.text_path, "vertices.captured=3"));
    assert(file_contains(info.text_path, "indices.captured=3"));
    assert(file_contains(info.text_path, "texture[0].pixels.stored=64"));
    assert(file_contains(info.text_path, "clear.valid=1"));
    assert(file_contains(info.text_path, "present.present=1"));
    assert(file_contains(info.text_path, "target.handle=0x50004000"));
    assert(file_contains(info.text_path, "target.implicit=0"));
    assert(file_contains(info.text_path, "target.width=400"));
    assert(file_contains(info.text_path, "target.height=240"));
    assert(file_contains(info.text_path, "target.format=21"));
    assert(file_contains(info.text_path, "target.usage=0"));
    char hash_line[80];
    snprintf(hash_line, sizeof hash_line, "vertices.hash=0x%016llx",
             (unsigned long long)d9_capture_fnv1a(g_vertices, 3 * sizeof(D9Vertex)));
    assert(file_contains(info.text_path, hash_line));
    snprintf(hash_line, sizeof hash_line, "indices.hash=0x%016llx",
             (unsigned long long)d9_capture_fnv1a(expected_indices, sizeof expected_indices));
    assert(file_contains(info.text_path, hash_line));
    snprintf(hash_line, sizeof hash_line, "texture[0].pixels.hash=0x%016llx",
             (unsigned long long)d9_capture_fnv1a(g_texture, sizeof g_texture));
    assert(file_contains(info.text_path, hash_line));
    check_binary_header(&info);

    size_t binary_size;
    unsigned char *binary_data = read_file(info.binary_path, &binary_size);
    uint32_t target_bytes = 0;
    const unsigned char *target =
        find_section(binary_data, binary_size, TEST_SECTION_TARGET, &target_bytes);
    assert(target && target_bytes == 24);
    assert(rd32(target + 0) == 0x50004000u);
    assert(rd32(target + 4) == 0);
    assert(rd32(target + 8) == 400);
    assert(rd32(target + 12) == 240);
    assert(rd32(target + 16) == D9_A8R8G8B8);
    assert(rd32(target + 20) == 0);
    free(binary_data);

    /* The default policy captures exactly one draw. */
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_SKIP);
    assert(d9_capture_count() == 1);
    d9_capture_shutdown();
}

static void test_truncation(void) {
    begin_capture("d9cap_trunc", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);

    uint8_t *big_shader = malloc(70000);
    uint16_t *big_indices = malloc(2000 * sizeof(uint16_t));
    uint8_t *big_texture = malloc(D9_CAPTURE_TEXTURE_MAX_BYTES + 4096);
    assert(big_shader && big_indices && big_texture);
    for (size_t i = 0; i < 70000; ++i) big_shader[i] = (uint8_t)(i * 3 + 1);
    for (size_t i = 0; i < 2000; ++i) big_indices[i] = (uint16_t)i;
    for (size_t i = 0; i < D9_CAPTURE_TEXTURE_MAX_BYTES + 4096; ++i)
        big_texture[i] = (uint8_t)(i * 5 + 9);

    assert(guest_write(&g_memory, VS_ADDR, big_shader, 70000) == 0);
    assert(guest_write(&g_memory, PS_ADDR, big_shader, 70000) == 0);
    s.vs.code.guest = VS_ADDR;
    s.vs.code.bytes = 70000;
    s.ps.code.guest = PS_ADDR;
    s.ps.code.bytes = 70000;

    s.draw.vertices.count = 300;
    s.draw.vertices.data.bytes = 300 * sizeof(D9Vertex);
    s.draw.vertices.data.host = g_vertices;
    s.draw.indices.data.host = big_indices;
    s.draw.indices.data.guest = 0;
    s.draw.indices.data.bytes = 2000 * sizeof(uint16_t);
    s.draw.indices.count = 2000;

    s.stream_count = 17;  /* array holds 16; module must clamp */
    s.declaration.count = 65; /* array holds 64; module must clamp */

    s.texture_count = 5; /* array holds 4; module must report extra stages */
    s.textures[0].pixels.host = big_texture;
    s.textures[0].pixels.guest = 0;
    s.textures[0].pixels.bytes = D9_CAPTURE_TEXTURE_MAX_BYTES + 4096;
    s.textures[1] = s.textures[0];
    s.textures[1].pixels.host = big_texture;
    s.textures[1].pixels.bytes = 16;
    s.textures[2] = s.textures[1];
    s.textures[2].pixels.bytes = 0;
    s.textures[3] = s.textures[2];

    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    D9CaptureRecordInfo info;
    assert(d9_capture_record(0, &info) == D9_CAPTURE_OK);
    unsigned expected = D9_CAPTURE_TRUNC_VERTICES | D9_CAPTURE_TRUNC_INDICES |
                        D9_CAPTURE_TRUNC_VS | D9_CAPTURE_TRUNC_PS |
                        D9_CAPTURE_TRUNC_TEXTURE | D9_CAPTURE_TRUNC_TEXTURE_TOTAL |
                        D9_CAPTURE_TRUNC_STREAMS | D9_CAPTURE_TRUNC_TEXTURES |
                        D9_CAPTURE_TRUNC_DECL;
    assert(info.truncation == expected);
    assert(info.vertex_count == 300 && info.vertex_captured == 256);
    assert(info.index_count == 2000 && info.index_captured == 1024);
    assert(info.vs_bytes == D9_CAPTURE_SHADER_MAX_BYTES);
    assert(info.ps_bytes == D9_CAPTURE_SHADER_MAX_BYTES);
    assert(info.vertex_bytes == 256 * sizeof(D9Vertex));
    assert(info.index_bytes == 1024 * sizeof(uint16_t));
    assert(info.texture_bytes == D9_CAPTURE_TEXTURE_MAX_BYTES);
    assert(info.texture_count == D9_CAPTURE_MAX_TEXTURES);
    assert(file_contains(info.text_path, "truncation.names=vertices,indices,vs,ps,"
                                         "texture,texture_total,streams,textures,"
                                         "declaration"));
    assert(file_contains(info.text_path, "vertices.captured=256"));
    assert(file_contains(info.text_path, "indices.captured=1024"));
    assert(file_contains(info.text_path, "declaration.captured=64"));
    check_binary_header(&info);

    free(big_shader);
    free(big_indices);
    free(big_texture);
    d9_capture_shutdown();
}

static void test_bad_pointer(void) {
    begin_capture("d9cap_bad", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);

    s.vs.code.host = NULL;
    s.vs.code.guest = BAD_ADDR;
    s.vs.code.bytes = 16;
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_POINTER);
    assert(d9_capture_count() == 0);
    assert(!exists("d9cap_bad.txt") && !exists("d9cap_bad.bin"));
    assert(strlen(d9_capture_last_error()) > 0);

    int error = D9_CAPTURE_OK;
    assert(d9_capture_signature(&s, &g_memory, &error) == 0);
    assert(error == D9_CAPTURE_POINTER);

    make_base(&s);
    s.textures[0].pixels.host = NULL;
    s.textures[0].pixels.guest = BAD_ADDR;
    s.textures[0].pixels.bytes = 64;
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_POINTER);
    assert(d9_capture_count() == 0);

    make_base(&s);
    s.draw.vertices.data.bytes = 10; /* less than 3 * stride */
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_INVALID);
    assert(d9_capture_count() == 0);

    make_base(&s);
    s.draw.indices.size = 3;
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_INVALID);
    assert(d9_capture_count() == 0);
    assert(!exists("d9cap_bad.txt") && !exists("d9cap_bad.bin"));
    d9_capture_shutdown();
}

static void test_no_overwrite(void) {
    cleanup("d9cap_existing", 8);
    FILE *sentinel = fopen("d9cap_existing.txt", "wb");
    assert(sentinel);
    assert(fputs("keep me\n", sentinel) >= 0);
    assert(fclose(sentinel) == 0);
    FILE *half = fopen("d9cap_existing_2.bin", "wb");
    assert(half);
    assert(fputc(0x5a, half) != EOF);
    assert(fclose(half) == 0);

    init_capture("d9cap_existing", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    D9CaptureRecordInfo info;
    assert(d9_capture_record(0, &info) == D9_CAPTURE_OK);
    assert(strcmp(info.text_path, "d9cap_existing_3.txt") == 0);
    assert(strcmp(info.binary_path, "d9cap_existing_3.bin") == 0);
    assert(file_contains("d9cap_existing.txt", "keep me"));
    assert(exists("d9cap_existing_2.bin"));
    assert(!exists("d9cap_existing_2.txt"));
    d9_capture_shutdown();
}

static void test_unique_signatures(void) {
    begin_capture("d9cap_unique", 4, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);

    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    uint64_t first;
    assert(d9_capture_record(0, NULL) == D9_CAPTURE_INVALID);
    D9CaptureRecordInfo info;
    assert(d9_capture_record(0, &info) == D9_CAPTURE_OK);
    first = info.signature;

    /* Same signature: skipped without consuming a slot. */
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_SKIP);
    assert(d9_capture_count() == 1);

    s.draw.primitives = 2;
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    assert(d9_capture_count() == 2);
    assert(d9_capture_record(1, &info) == D9_CAPTURE_OK);
    uint64_t second = info.signature;
    assert(second != first);
    assert(strcmp(info.text_path, "d9cap_unique_2.txt") == 0);

    /* Same geometry into another render target is a third signature. */
    s.target = (D9CaptureRenderTarget){0x50004000u, 320, 240, D9_X8R8G8B8, 1, 0};
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    assert(d9_capture_count() == 3);
    assert(d9_capture_record(2, &info) == D9_CAPTURE_OK);
    uint64_t third = info.signature;
    assert(third != first && third != second);
    assert(strcmp(info.text_path, "d9cap_unique_3.txt") == 0);

    /* Same geometry/target but different texture bytes is a fourth signature. */
    g_texture[0] ^= 0xff;
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);
    assert(d9_capture_count() == 4);
    assert(d9_capture_record(3, &info) == D9_CAPTURE_OK);
    assert(info.signature != first && info.signature != second && info.signature != third);
    assert(strcmp(info.text_path, "d9cap_unique_4.txt") == 0);
    assert(!d9_capture_active());

    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_SKIP);
    assert(d9_capture_count() == 4);
    d9_capture_shutdown();
}

static void test_implicit_target(void) {
    begin_capture("d9cap_implicit", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    D9CaptureSnapshot s;
    make_base(&s);
    /* A zero target is the legacy default and must normalise to the implicit
     * back buffer rather than fail validation. */
    memset(&s.target, 0, sizeof s.target);
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_OK);

    D9CaptureRecordInfo info;
    assert(d9_capture_record(0, &info) == D9_CAPTURE_OK);
    assert(file_contains(info.text_path, "target.handle=0x00000000"));
    assert(file_contains(info.text_path, "target.implicit=1"));

    size_t size;
    unsigned char *data = read_file(info.binary_path, &size);
    uint32_t length = 0;
    const unsigned char *target =
        find_section(data, size, TEST_SECTION_TARGET, &length);
    assert(target && length == 24);
    assert(rd32(target) == 0 && rd32(target + 4) == 1);
    free(data);
    d9_capture_shutdown();
}

static void test_present_only(void) {
    cleanup("d9cap_present", 8);
    cleanup("d9cap_present_present", 8);

    D9CaptureOptions options;
    d9_capture_options_init(&options);
    assert(options.max_presents == 1);
    options.directory = ".";
    options.basename = "d9cap_present";
    options.max_records = 1;
    options.max_presents = 2;
    options.flags = D9_CAPTURE_OPT_UNIQUE_ONLY;
    assert(d9_capture_init(&options) == D9_CAPTURE_OK);

    /* Present is independent of the draw path and works before any draw. */
    assert(d9_capture_present_active());
    assert(d9_capture_present_count() == 0);
    assert(!exists("d9cap_present_present.txt"));

    D9CapturePresentEvent event;
    memset(&event, 0, sizeof event);
    event.frame_index = 7;
    event.target.handle = 0;
    event.target.implicit = 1;
    event.target.width = 400;
    event.target.height = 240;
    event.target.format = D9_A8R8G8B8;
    uint64_t signature = d9_capture_present_signature(&event);
    assert(signature != 0);
    assert(d9_capture_present_signature(NULL) == 0);

    assert(d9_capture_offer_present(&event) == D9_CAPTURE_OK);
    assert(d9_capture_present_count() == 1);
    /* A present capture must not consume a draw slot or pixel data. */
    assert(d9_capture_active());
    assert(d9_capture_count() == 0);

    D9CapturePresentInfo info;
    assert(d9_capture_present_record(0, &info) == D9_CAPTURE_OK);
    assert(info.index == 1 && info.frame_index == 7);
    assert(info.signature == signature);
    assert(info.text_bytes > 0 && info.binary_bytes > 0);
    assert(strcmp(info.text_path, "d9cap_present_present.txt") == 0);
    assert(strcmp(info.binary_path, "d9cap_present_present.bin") == 0);
    assert(strcmp(d9_capture_last_present_text_path(), info.text_path) == 0);
    assert(strcmp(d9_capture_last_present_binary_path(), info.binary_path) == 0);
    assert(exists(info.text_path) && exists(info.binary_path));

    assert(file_contains(info.text_path, "format=d9_capture_present"));
    assert(file_contains(info.text_path, "version=2"));
    assert(file_contains(info.text_path, "present.frame_index=7"));
    assert(file_contains(info.text_path, "present.source.valid=0"));
    assert(file_contains(info.text_path, "present.dest.valid=0"));
    assert(file_contains(info.text_path, "target.handle=0x00000000"));
    assert(file_contains(info.text_path, "target.implicit=1"));
    assert(file_contains(info.text_path, "target.width=400"));
    assert(file_contains(info.text_path, "target.height=240"));
    assert(file_contains(info.text_path, "target.format=21"));
    assert(file_contains(info.text_path, "pixels.captured=0"));

    size_t size;
    unsigned char *data = read_file(info.binary_path, &size);
    assert(size >= 32);
    assert(memcmp(data, "D9CAPPRE", 8) == 0);
    assert(rd32(data + 8) == D9_CAPTURE_SCHEMA_VERSION);
    assert(rd32(data + 12) == 32);
    assert(rd64(data + 16) == signature);
    assert(rd32(data + 24) == 0);
    assert(rd32(data + 28) == 2);
    uint32_t length = 0;
    const unsigned char *present =
        find_section(data, size, TEST_SECTION_PRESENT, &length);
    assert(present && length == 80);
    assert(rd32(present + 0) == 7);            /* frame index */
    assert(rd32(present + 4) == 0);            /* flags */
    assert(rd32(present + 12) == 0);           /* source.valid */
    assert(rd32(present + 52) == 0);           /* target.handle */
    assert(rd32(present + 56) == 1);           /* target.implicit */
    assert(rd32(present + 60) == 400);         /* target.width */
    assert(rd32(present + 64) == 240);         /* target.height */
    assert(rd32(present + 68) == D9_A8R8G8B8); /* target.format */
    assert(rd32(present + 72) == 0);           /* target.usage */
    free(data);

    /* Second event: explicit render target, real rects, distinct suffix. */
    memset(&event, 0, sizeof event);
    event.frame_index = 8;
    event.flags = 1;
    event.target.handle = 0x50004000u;
    event.target.width = 320;
    event.target.height = 240;
    event.target.format = D9_X8R8G8B8;
    event.target.usage = 1;
    event.source = (D9CaptureRect){0, 0, 320, 240, 1};
    event.dest = (D9CaptureRect){10, 10, 310, 230, 1};
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_OK);
    assert(d9_capture_present_count() == 2);
    assert(d9_capture_present_record(1, &info) == D9_CAPTURE_OK);
    assert(strcmp(info.text_path, "d9cap_present_present_2.txt") == 0);
    assert(file_contains(info.text_path, "target.handle=0x50004000"));
    assert(file_contains(info.text_path, "target.implicit=0"));
    assert(file_contains(info.text_path, "present.source.rect=0,0,320,240"));
    assert(file_contains(info.text_path, "present.dest.rect=10,10,310,230"));

    /* Present cap: 2 reached, the draw path is still untouched. */
    assert(!d9_capture_present_active());
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_SKIP);
    assert(d9_capture_present_count() == 2);
    assert(d9_capture_active());

    assert(d9_capture_present_record(2, &info) == D9_CAPTURE_INVALID);
    assert(d9_capture_present_record(0, NULL) == D9_CAPTURE_INVALID);
    d9_capture_shutdown();
}

static void test_present_invalid(void) {
    cleanup("d9cap_pbad", 8);
    cleanup("d9cap_pbad_present", 8);
    init_capture("d9cap_pbad", 1, D9_CAPTURE_OPT_UNIQUE_ONLY);
    assert(d9_capture_present_active());

    D9CapturePresentEvent event;
    memset(&event, 0, sizeof event);
    event.target.handle = 0x4000u;
    event.target.implicit = 1; /* contradictory: nonzero handle cannot be implicit */
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_INVALID);
    assert(d9_capture_present_count() == 0);
    assert(strlen(d9_capture_last_error()) > 0);
    assert(!exists("d9cap_pbad_present.txt"));

    memset(&event, 0, sizeof event);
    event.target.implicit = 2; /* out-of-range flag */
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_INVALID);

    memset(&event, 0, sizeof event);
    event.source = (D9CaptureRect){10, 0, 5, 10, 1};
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_INVALID);
    event.source = (D9CaptureRect){0, 0, 0, 0, 0};
    event.dest = (D9CaptureRect){0, 10, 10, 5, 1};
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_INVALID);

    assert(d9_capture_offer_present(NULL) == D9_CAPTURE_INVALID);
    assert(d9_capture_present_count() == 0);

    /* A valid event still succeeds after all the rejected attempts. */
    memset(&event, 0, sizeof event);
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_OK);
    assert(d9_capture_present_count() == 1);
    d9_capture_shutdown();
}

static void test_disabled(void) {
    cleanup("d9cap_off", 2);
    D9CaptureOptions options;
    d9_capture_options_init(&options);
    assert(options.directory == NULL && options.basename == NULL);
    assert(options.max_records == 1);
    assert(options.flags == D9_CAPTURE_OPT_UNIQUE_ONLY);
    assert(options.max_presents == 1);

    options.directory = ".";
    options.basename = "d9cap_off";
    options.flags |= D9_CAPTURE_OPT_DISABLED;
    assert(d9_capture_init(&options) == D9_CAPTURE_OK);
    assert(!d9_capture_active());

    D9CaptureSnapshot s;
    make_base(&s);
    assert(d9_capture_offer(&s, &g_memory) == D9_CAPTURE_SKIP);
    assert(d9_capture_count() == 0);
    assert(!exists("d9cap_off.txt") && !exists("d9cap_off.bin"));

    D9CapturePresentEvent event;
    memset(&event, 0, sizeof event);
    assert(!d9_capture_present_active());
    assert(d9_capture_offer_present(&event) == D9_CAPTURE_SKIP);
    assert(d9_capture_present_count() == 0);
    assert(!exists("d9cap_off_present.txt") && !exists("d9cap_off_present.bin"));

    /* NULL options uses the platform default (first draw, default basename). */
    assert(d9_capture_init(NULL) == D9_CAPTURE_OK);
    assert(d9_capture_active());
    d9_capture_shutdown();
}

int main(void) {
    guest_init(&g_memory, 2u << 20);
    assert(guest_map(&g_memory, GUEST_BASE, GUEST_SIZE) == 0);
    seed_guest();

    test_fnv();
    test_first_draw();
    test_truncation();
    test_bad_pointer();
    test_no_overwrite();
    test_unique_signatures();
    test_implicit_target();
    test_present_only();
    test_present_invalid();
    test_disabled();

    guest_destroy(&g_memory);

    cleanup("d9cap_basic", 3);
    cleanup("d9cap_trunc", 3);
    cleanup("d9cap_bad", 3);
    cleanup("d9cap_existing", 8);
    cleanup("d9cap_unique", 4);
    cleanup("d9cap_implicit", 3);
    cleanup("d9cap_present", 8);
    cleanup("d9cap_present_present", 8);
    cleanup("d9cap_pbad", 3);
    cleanup("d9cap_pbad_present", 3);
    cleanup("d9cap_off", 2);
    puts("D3D9 first-draw, render-target & present capture tests passed");
    return 0;
}
