/* Contract regression for the measured TESV renderer-init D3D9 route.
 *
 * The nine render-target tuples come from the 64 MiB HDR-fallback census in
 * docs/TESV_PROBE_ROUTE.md.  Cube/depth/shader/Present calls were not reached by
 * that census; their checks below are explicitly host-COM contracts only. */
#include "d3d9_com_demo.h"
#include "d3d9_core.h"
#include "d3d9_null.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x10010000u
#define PP     (SCRATCH + 0x400u)
#define OUT    0x10010800u
#define DESC   (OUT + 0x100u)
#define CODE   (OUT + 0x200u)
#define PROBE_FN 0x00401000u
#define ERR_INVALID 0x8876086cu
#define ERR_UNAVAILABLE 0x8876086au
#define D3DRTYPE_SURFACE 1u

typedef struct {
    GuestMemory memory;
    CPU cpu;
} Harness;

typedef struct {
    Harness *harness;
    D9ComDemo demo;
    D9Device *core;
    int bridge;
} Session;

extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);

/* Same indirect-call sequence used by d3d9_com_demo.c. */
static void guest_dispatch_probe(CPU *c) {
    uint32_t sp = c->r[4], target = M32(sp + 4), n = M32(sp + 8);
    if (n > 16) rt_unimpl(c, target, "probe argument count");
    for (unsigned i = n; i > 0; --i) {
        c->r[4] -= 4;
        M32(c->r[4]) = M32(sp + 8 + i * 4);
    }
    c->r[4] -= 4;
    M32(c->r[4]) = PROBE_FN + 4;
    static const FnEntry *cache;
    if (cache && cache->addr == target) cache->fn(c);
    else rt_call_ic(c, target, &cache);
    if (c->r[4] != sp) rt_unimpl(c, target, "COM stdcall stack mismatch");
    c->r[4] = sp + 4;
}

static int invoke(Harness *h, uint32_t target, const uint32_t *args,
                  unsigned count, uint32_t *result) {
    uint32_t frame[18] = {target, count};
    assert(count <= 16);
    for (unsigned i = 0; i < count; ++i) frame[i + 2] = args[i];
    return n3ds_lift_invoke(&h->cpu, PROBE_FN, frame, count + 2, result);
}

static void harness_init(Harness *h) {
    memset(h, 0, sizeof *h);
    guest_init(&h->memory, 4u << 20);
    assert(!guest_map(&h->memory, 0x10000000, 0x4000));
    assert(!guest_map(&h->memory, SCRATCH, 0x6000));
    static const FnEntry probe_table[] = {{PROBE_FN, guest_dispatch_probe}};
    static FnModule modules[2];
    modules[0] = (FnModule){"contract-probe", probe_table, 1};
    modules[1] = d9_com_module();
    assert(!n3ds_lift_bind_modules(&h->memory, modules, 2));
    n3ds_lift_init_cpu(&h->cpu, 0x10004000, 0);
}

static void harness_destroy(Harness *h) {
    n3ds_lift_release();
    guest_destroy(&h->memory);
    memset(h, 0, sizeof *h);
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    assert(!_putenv_s(name, value));
#else
    assert(!setenv(name, value, 1));
#endif
}

static uint32_t call(D9ComDemo *s, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned count) {
    uint32_t result = 0;
    int rc = d9_com_demo_call(s, object, slot, args, count, &result);
    if (rc) {
        fprintf(stderr, "tesv-init contract: object=0x%08x slot=%u rc=%d: %s\n",
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

static void write_bytes(D9ComDemo *s, uint32_t address,
                        const void *data, size_t bytes) {
    assert(!guest_write(&s->memory, address, data, bytes));
}

static void session_init(Session *s, Harness *h, size_t budget) {
    PeImport import = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    uint32_t sdk = 32, factory = 0;
    uint32_t create9;
    uint32_t pp[14] = {400, 240, D9_X8R8G8B8, 1, 0, 0, 1, 0, 1, 0, 75, 0, 0, 1};
    uint32_t args[] = {0, 1, 0, 0x20, PP, OUT};

    memset(s, 0, sizeof *s);
    s->harness = h;
    s->demo.memory = h->memory;
    s->demo.cpu = h->cpu;
    s->core = d9_create_device(d9_null_backend(), d9_null_context(), budget);
    assert(s->core);
    assert(!d9_com_init(&h->memory, s->core));
    s->bridge = 1;
    s->demo.memory = h->memory; /* d9_com_init mapped the COM vtable region. */
    create9 = d9_com_resolve(&import, NULL);
    assert(create9 && !invoke(h, create9, &sdk, 1, &factory) && factory);
    s->demo.factory = factory;
    assert(!guest_write(&h->memory, PP, pp, sizeof pp));
    write32(&s->demo, OUT, 0xdeadbeefu);
    assert(call(&s->demo, factory, 16, args, 6) == 0);
    s->demo.device = read32(&s->demo, OUT);
    assert(s->demo.device);
}

static void session_destroy(Session *s) {
    assert(call(&s->demo, s->demo.device, 2, NULL, 0) == 0);
    assert(call(&s->demo, s->demo.factory, 2, NULL, 0) == 0);
    d9_com_shutdown();
    s->bridge = 0;
    assert(d9_destroy_device_ex(s->core) == 0);
    memset(s, 0, sizeof *s);
}

static void expect_diagnostic(const char *needle) {
    const char *diagnostic = d9_com_last_diagnostic();
    if (!strstr(diagnostic, needle)) {
        fprintf(stderr, "tesv-init contract: missing diagnostic '%s' in '%s'\n",
                needle, diagnostic);
        abort();
    }
}

static uint32_t create_texture(D9ComDemo *s, unsigned width, unsigned height,
                               unsigned format) {
    uint32_t args[] = {width, height, 1, 1, format, 0, OUT, 0};
    write32(s, OUT, 0xdeadbeefu);
    return device_call(s, 23, args, 8);
}

static uint32_t create_cube_071(D9ComDemo *s) {
    uint32_t args[] = {512, 1, 1, 0x71, 0, OUT, 0};
    write32(s, OUT, 0xdeadbeefu);
    return device_call(s, 25, args, 7);
}

static void check_desc(D9ComDemo *s, uint32_t object, int texture,
                       unsigned format, unsigned width, unsigned height,
                       unsigned usage) {
    uint32_t args[2] = {0, DESC};
    uint32_t got[8];
    unsigned slot = texture ? 17u : 12u;
    if (!texture) args[0] = DESC;
    assert(call(s, object, slot, args, texture ? 2u : 1u) == 0);
    assert(!guest_read(&s->memory, DESC, got, sizeof got));
    assert(got[0] == format);
    assert(got[1] == D3DRTYPE_SURFACE);
    assert(got[2] == usage && got[3] == 0);
    assert(got[4] == 0 && got[5] == 0);
    assert(got[6] == width && got[7] == height);
}

static void release_object(D9ComDemo *s, uint32_t object) {
    assert(call(s, object, 2, NULL, 0) == 0);
}

static void check_unknown_shader(D9ComDemo *s, unsigned slot,
                                 const uint32_t code[2]) {
    uint32_t args[] = {CODE, OUT};
    uint32_t result;
    const char *reason;
    write_bytes(s, CODE, code, 2 * sizeof *code);
    write32(s, OUT, 0xdeadbeefu);
    result = device_call(s, slot, args, 2);
    assert(result != 0 && read32(s, OUT) == 0);
    reason = d9_com_last_shader_rejection();
    if (!reason || !*reason) {
        fprintf(stderr, "tesv-init contract: shader refusal was not named\n");
        abort();
    }
}

static void default_contract(Harness *h) {
    static const uint32_t ps_code[2] = {0xffff0300u, 0x0000ffffu};
    static const uint32_t vs_code[2] = {0xfffe0300u, 0x0000ffffu};
    Session s;
    uint32_t depth, bad_depth[8] = {0}, bad_size[8] = {0}, present[4] = {0, 0, 0, 0};
    unsigned presents;

    set_env("TESV_D3D9_HDR_LDR_FALLBACK", "0");
    set_env("TESV_D3D9_SHADER_SKIP_PROBE", "0");
    session_init(&s, h, 8u << 20);

    /* Default HDR render-target refusal is named and leaves no object. */
    assert(create_texture(&s.demo, 512, 512, 0x71) == ERR_UNAVAILABLE);
    assert(read32(&s.demo, OUT) == 0);
    expect_diagnostic("float/HDR render-target textures are not implemented");

    /* not measured: TESV never reached CreateCubeTexture.  The exact requested
     * tuple still has a stable host refusal because RENDERTARGET usage is not
     * admitted for cube textures. */
    assert(create_cube_071(&s.demo) == ERR_UNAVAILABLE);
    assert(read32(&s.demo, OUT) == 0);
    expect_diagnostic("unsupported cube texture usage 0x1");

    /* not measured: CreateDepthStencilSurface was absent from the census. */
    bad_depth[0] = bad_depth[1] = 512;
    bad_depth[2] = 80; /* D3DFMT_D16, not the D24S8/D24X8 attachment pair. */
    bad_depth[6] = OUT;
    write32(&s.demo, OUT, 0xdeadbeefu);
    assert(device_call(&s.demo, 29, bad_depth, 8) == ERR_UNAVAILABLE);
    assert(read32(&s.demo, OUT) == 0);
    expect_diagnostic("only D3DFMT_D24S8/D24X8");

    bad_size[0] = 500; bad_size[1] = 512; bad_size[2] = D9_D24S8;
    bad_size[6] = OUT;
    write32(&s.demo, OUT, 0xdeadbeefu);
    assert(device_call(&s.demo, 29, bad_size, 8) == ERR_INVALID);
    assert(read32(&s.demo, OUT) == 0);
    expect_diagnostic("only power-of-two 8..1024");

    bad_depth[0] = bad_depth[1] = 512;
    bad_depth[2] = D9_D24S8;
    bad_depth[6] = OUT;
    write32(&s.demo, OUT, 0xdeadbeefu);
    assert(device_call(&s.demo, 29, bad_depth, 8) == 0);
    depth = read32(&s.demo, OUT);
    assert(depth);
    check_desc(&s.demo, depth, 0, D9_D24S8, 512, 512, 0);
    release_object(&s.demo, depth);

    /* not measured: no shader was created before the census fault. */
    check_unknown_shader(&s.demo, 91, vs_code);
    check_unknown_shader(&s.demo, 106, ps_code);

    /* not measured: Present was absent from the census. */
    assert(device_call(&s.demo, 17, present, 4) == ERR_INVALID);
    expect_diagnostic("Present requires EndScene");
    presents = d9_null_stats()->presents;
    assert(device_call(&s.demo, 41, NULL, 0) == 0);
    assert(device_call(&s.demo, 42, NULL, 0) == 0);
    assert(device_call(&s.demo, 17, present, 4) == 0);
    assert(d9_null_stats()->presents == presents + 1);

    session_destroy(&s);
}

static void render_target_contract(Harness *h) {
    /* Measured caller=0x00ceca5e tuples: five R32F shadow maps, three A8
     * quarter-res targets and one A16B16G16R16F shadow-mask target. */
    static const struct {
        unsigned width, height, format;
    } tuples[] = {
        {1024, 1024, 0x72}, {1024, 1024, 0x72}, {1024, 1024, 0x72},
        {1024, 1024, 0x72}, {1024, 1024, 0x72},
        {512, 512, D9_A8R8G8B8}, {512, 512, D9_A8R8G8B8},
        {512, 512, D9_A8R8G8B8}, {512, 512, 0x71}
    };
    /* not measured: no 256x256 render-target request occurs. */
    Session s;
    uint32_t textures[9];
    unsigned before_creates, before_deletes;

    set_env("TESV_D3D9_HDR_LDR_FALLBACK", "1");
    set_env("TESV_D3D9_SHADER_SKIP_PROBE", "0");
    session_init(&s, h, 64u << 20);
    before_creates = d9_null_stats()->target_creates;
    before_deletes = d9_null_stats()->target_deletes;

    for (unsigned i = 0; i < 9; ++i) {
        uint32_t surface, args[] = {0, OUT};
        assert(create_texture(&s.demo, tuples[i].width, tuples[i].height,
                              tuples[i].format) == 0);
        textures[i] = read32(&s.demo, OUT);
        assert(textures[i]);
        check_desc(&s.demo, textures[i], 1, tuples[i].format,
                   tuples[i].width, tuples[i].height, 1);
        assert(call(&s.demo, textures[i], 18, args, 2) == 0);
        surface = read32(&s.demo, OUT);
        assert(surface);
        check_desc(&s.demo, surface, 0, tuples[i].format,
                   tuples[i].width, tuples[i].height, 1);
        release_object(&s.demo, surface);
    }
    assert(d9_null_stats()->target_creates == before_creates + 9);

    /* not measured: host-only stability check; HDR fallback does not admit the
     * cube RENDERTARGET usage, so the named refusal is identical. */
    assert(create_cube_071(&s.demo) == ERR_UNAVAILABLE);
    assert(read32(&s.demo, OUT) == 0);
    expect_diagnostic("unsupported cube texture usage 0x1");

    for (unsigned i = 0; i < 9; ++i) release_object(&s.demo, textures[i]);
    assert(d9_null_stats()->target_deletes == before_deletes + 9);
    session_destroy(&s);
}

static void shader_skip_contract(Harness *h) {
    static const uint32_t ps_code[2] = {0xffff0300u, 0x0000ffffu};
    static const uint32_t vs_code[2] = {0xfffe0300u, 0x0000ffffu};
    Session s;
    uint32_t args[] = {CODE, OUT};
    uint32_t ps, vs, draw[] = {D9_TRIANGLELIST, 0, 1};
    uint32_t present[] = {0, 0, 0, 0};

    set_env("TESV_D3D9_HDR_LDR_FALLBACK", "0");
    set_env("TESV_D3D9_SHADER_SKIP_PROBE", "1");
    session_init(&s, h, 8u << 20);

    /* not measured: the census died before any shader creation. */
    write_bytes(&s.demo, CODE, ps_code, sizeof ps_code);
    write32(&s.demo, OUT, 0xdeadbeefu);
    assert(device_call(&s.demo, 106, args, 2) == 0);
    ps = read32(&s.demo, OUT);
    assert(ps);
    write_bytes(&s.demo, CODE, vs_code, sizeof vs_code);
    write32(&s.demo, OUT, 0xdeadbeefu);
    assert(device_call(&s.demo, 91, args, 2) == 0);
    vs = read32(&s.demo, OUT);
    assert(vs);

    assert(device_call(&s.demo, 107, (uint32_t[]){ps}, 1) == 0);
    assert(device_call(&s.demo, 92, (uint32_t[]){vs}, 1) == 0);
    assert(device_call(&s.demo, 89, (uint32_t[]){0x102}, 1) == 0);
    assert(device_call(&s.demo, 41, NULL, 0) == 0);
    assert(device_call(&s.demo, 81, draw, 3) == ERR_UNAVAILABLE);
    expect_diagnostic("shader-skip probe: draw with a code-less shader is refused (measurement only)");
    assert(device_call(&s.demo, 42, NULL, 0) == 0);
    assert(device_call(&s.demo, 17, present, 4) == 0);

    assert(device_call(&s.demo, 107, (uint32_t[]){0}, 1) == 0);
    assert(device_call(&s.demo, 92, (uint32_t[]){0}, 1) == 0);
    release_object(&s.demo, ps);
    release_object(&s.demo, vs);
    session_destroy(&s);
}

int main(void) {
    Harness harness;
    harness_init(&harness);
    default_contract(&harness);
    render_target_contract(&harness);
    shader_skip_contract(&harness);
    harness_destroy(&harness);
    set_env("TESV_D3D9_HDR_LDR_FALLBACK", "0");
    set_env("TESV_D3D9_SHADER_SKIP_PROBE", "0");
    puts("TESV init contract: nine RT tuples, named cube/depth/shader/Present refusals and probe behavior PASS");
    return 0;
}
