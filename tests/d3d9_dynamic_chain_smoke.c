/* Isolated end-to-end smoke test for the dynamic D3D9 resolution chain.
 *
 * The test deliberately does not use a statically bound PE import for
 * Direct3DCreate9.  It starts at the Win32 shim entry points a guest would use:
 *
 *   LoadLibraryA("d3d9.dll")
 *     -> GetProcAddress(handle, "Direct3DCreate9")
 *     -> d3d9 host guest thunk
 *     -> guest IDirect3D9 COM object
 *     -> adapter enumeration
 *     -> IDirect3D9::CreateDevice
 *     -> guest IDirect3DDevice9 COM object
 *     -> GetDeviceCaps + BeginScene/EndScene/Present
 *
 * The host null backend is the only renderer.  This proves COM/shims/thunk/
 * lifetime wiring, not a Skyrim frame and not any pixel output.
 *
 * Build/run (host) together with the same sources used by null_backend_smoke:
 *   guest.c guest_heap.c pe_image.c pe_imports.c win32_shims.c win32_sync.c
 *   win32_user.c win32_locale.c win32_shell.c win32_audio.c
 *   win32_audio_3ds.c win32_thread.c win32_input.c n3ds_lift_runtime.c
 *   ir_core.c ir_decode.c ir_exec.c rt_prof.c staged/fpu.c
 *   d3d9_core.c d3d9_texcache.c d3d9_texture_data.c d3d9_bc.c
 *   d3d9_vertex.c d3d9_adapter.c d3d9_shader.c d3d9_tev.c d3d9_null.c
 *   d3d9_pica_tev.c d3d9_com.c tests/d3d9_dynamic_chain_smoke.c
 */
#include "d3d9_com.h"
#include "d3d9_adapter.h"
#include "d3d9_null.h"
#include "n3ds_lift_runtime.h"
#include "win32_shims.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#endif

/* Guest layout mirrors the existing COM smoke harnesses.  The scratch region
 * holds the module/symbol strings and all x86 out-parameters. */
#define SCRATCH 0x10010000u
#define OUT (SCRATCH + 0x500)
#define OUT2 (SCRATCH + 0x540)
#define STR (SCRATCH + 0x100)
#define PP (SCRATCH + 0x400)
#define IDENT (SCRATCH + 0x800)   /* D3DADAPTER_IDENTIFIER9: 1100 bytes */
#define MODE (SCRATCH + 0xd00)
#define CAPS9 (SCRATCH + 0xe00)   /* D3DCAPS9: 304 bytes */
#define GUID (SCRATCH + 0x1000)
#define PROBE_FN 0x00401000u

/* d3d9_com.c uses a fixed guest object table.  These are the deterministic
 * guest-side COM addresses and vtables of the first factory/device/back-buffer
 * triple.  Checking them proves the callbacks return guest objects rather than
 * accidentally leaking a host pointer. */
#define FACTORY_GUEST 0x50002000u
#define DEVICE_GUEST  0x50002010u
#define BACK_GUEST    0x50002020u
#define FACTORY_VTABLE 0x50000200u
#define DEVICE_VTABLE  0x50000400u
#define SURFACE_VTABLE 0x50001200u
#define D3D9_THUNK_BASE 0xfe000000u

/* A handful of x86 D3D9/COM constants used by the calls. */
#define D3D9_OK 0u
#define D3D9ERR_INVALIDCALL 0x8876086cu
#define D3D9OK_NOAUTOGEN 0x0876086fu
#define D3DDEVTYPE_HAL 1u
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40u

#define CHECK(cond) do { if (!(cond)) fail(#cond, __LINE__); } while (0)

typedef struct {
    GuestMemory memory;
    CPU cpu;
    FnModule tables[2];
    uint32_t factory, device, back;
} Chain;

static void write_log(const char *text) {
#ifdef __3DS__
    FILE *f = fopen("sdmc:/d3d9_dynamic_chain.log", "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
#else
    (void)text;
#endif
}

static void fail(const char *expr, int line) {
    char text[256];
    snprintf(text, sizeof text, "D3D9CHAIN RESULT: FAIL\nline=%d\nexpr=%s\n",
             line, expr);
    fprintf(stderr, "d3d9_dynamic_chain: FAIL at line %d: %s\n", line, expr);
    write_log(text);
#ifdef __3DS__
    gfxExit();
#endif
    exit(1);
}

/* Same stdcall probe used by the other COM harnesses: push the COM arguments,
 * indirect-call the guest target, and verify the callee restored the stack. */
extern void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache);
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

static int invoke(Chain *c, uint32_t target, const uint32_t *args, unsigned n,
                  uint32_t *result) {
    if (n > 16) return -1;
    uint32_t frame[18] = {target, n};
    for (unsigned i = 0; i < n; ++i) frame[i + 2] = args[i];
    int rc = n3ds_lift_invoke(&c->cpu, PROBE_FN, frame, n + 2, result);
    if (rc) {
        fprintf(stderr, "d3d9_dynamic_chain: invoke %08lx: %s\n",
                (unsigned long)target, n3ds_lift_fault());
    }
    return rc;
}

static uint32_t call(Chain *c, uint32_t object, unsigned slot,
                     const uint32_t *args, unsigned n) {
    uint32_t vt = 0, fn = 0;
    CHECK(!guest_read32(&c->memory, object, &vt) && vt);
    CHECK(!guest_read32(&c->memory, vt + slot * 4, &fn) && fn);
    uint32_t frame[18] = {object};
    for (unsigned i = 0; i < n; ++i) frame[i + 1] = args[i];
    uint32_t r = 0;
    int rc = invoke(c, fn, frame, n + 1, &r);
    CHECK(!rc);
    return r;
}

static uint32_t rd(Chain *c, uint32_t address) {
    uint32_t v = 0;
    CHECK(!guest_read32(&c->memory, address, &v));
    return v;
}

static void wr(Chain *c, uint32_t address, const void *data, size_t bytes) {
    CHECK(!guest_write(&c->memory, address, data, bytes));
}

static void wr_string(Chain *c, uint32_t address, const char *text) {
    wr(c, address, text, strlen(text) + 1);
}

static uint32_t vt(Chain *c, uint32_t object) {
    return rd(c, object);
}

/* AddRef/Release round-trip that returns the observed steady-state refcount. */
static uint32_t refs(Chain *c, uint32_t object) {
    uint32_t after_add = call(c, object, 1, NULL, 0);
    if (!after_add) return 0;
    uint32_t after_release = call(c, object, 2, NULL, 0);
    CHECK(after_release == after_add - 1);
    return after_release;
}

static uint32_t shim(const char *dll, const char *name) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t p = win32_resolve(&e, NULL);
    CHECK(p != 0);
    return p;
}

static void chain_start(Chain *c) {
    memset(c, 0, sizeof *c);
    guest_init(&c->memory, 2u << 20);
    CHECK(!guest_map(&c->memory, 0x10000000, 0x4000)); /* guest stack */
    CHECK(!guest_map(&c->memory, SCRATCH, 0x4000));    /* strings/out params */

    CHECK(!d9_com_init(&c->memory, NULL));
    CHECK(!d9_null_register());
    CHECK(!win32_init(&c->memory, 0x8000, "."));

    static const FnEntry probe_table[] = {{PROBE_FN, guest_dispatch_probe}};
    c->tables[0] = (FnModule){"dynamic-chain-probe", probe_table, 1};
    c->tables[1] = d9_com_module();
    CHECK(!n3ds_lift_bind_modules(&c->memory, c->tables, 2));
    n3ds_lift_init_cpu(&c->cpu, 0x10004000, 0);
}

static void chain_stop(Chain *c) {
    n3ds_lift_release();
    d9_com_shutdown();
    win32_shutdown();
    guest_destroy(&c->memory);
}

int main(void) {
#ifdef __3DS__
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("d3d9 dynamic chain: isolated ARM reachability run\n");
#endif

    Chain c;
    chain_start(&c);

    /* ---- 1. Dynamic module lookup and symbol resolution ------------------ */
    uint32_t load_library = shim("kernel32.dll", "LoadLibraryA");
    uint32_t get_proc = shim("kernel32.dll", "GetProcAddress");
    uint32_t get_module = shim("kernel32.dll", "GetModuleHandleA");
    CHECK(load_library >= WIN32_THUNK_BASE);
    CHECK(get_proc >= WIN32_THUNK_BASE);

    wr_string(&c, STR, "d3d9.dll");
    uint32_t args1[1] = {STR};
    uint32_t module = 0;
    CHECK(!invoke(&c, load_library, args1, 1, &module) && module);
    uint32_t module_again = 0;
    CHECK(!invoke(&c, get_module, args1, 1, &module_again));
    CHECK(module_again == module); /* LoadLibraryA found the same d3d9 module */

    wr_string(&c, STR + 0x40, "Direct3DCreate9");
    uint32_t args2[2] = {module, STR + 0x40};
    uint32_t create9 = 0;
    CHECK(!invoke(&c, get_proc, args2, 2, &create9) && create9);

    PeImport direct_import = {.dll = "d3d9.dll", .name = "Direct3DCreate9"};
    uint32_t expected_create9 = d9_com_resolve(&direct_import, NULL);
    CHECK(expected_create9 != 0);
    CHECK(create9 == expected_create9);
    CHECK(create9 >= D3D9_THUNK_BASE && create9 < WIN32_THUNK_BASE);

    /* ---- 2. Guest thunk -> guest IDirect3D9 ----------------------------- */
    uint32_t sdk = 32, factory = 0;
    CHECK(!invoke(&c, create9, &sdk, 1, &factory) && factory);
    c.factory = factory;
    CHECK(factory == FACTORY_GUEST);
    CHECK(vt(&c, factory) == FACTORY_VTABLE);
    CHECK(refs(&c, factory) == 1);

    /* The export thunk is a singleton: a second call returns the same guest
     * object and only bumps its reference count. */
    uint32_t factory_again = 0;
    CHECK(!invoke(&c, create9, &sdk, 1, &factory_again));
    CHECK(factory_again == factory);
    CHECK(refs(&c, factory) == 2);
    CHECK(call(&c, factory, 2, NULL, 0) == 1); /* drop the extra reference */

    /* ---- 3. Adapter enumeration through IDirect3D9 ---------------------- */
    CHECK(call(&c, factory, 4, NULL, 0) == 1); /* GetAdapterCount */

    uint32_t id_args[3] = {0, 0, IDENT};
    CHECK(call(&c, factory, 5, id_args, 3) == 0); /* GetAdapterIdentifier */
    const D9AdapterIdentifier *id = guest_ptr(&c.memory, IDENT, sizeof *id);
    CHECK(id != NULL);
    CHECK(!strcmp(id->driver, D9_ADAPTER_DRIVER_NAME));
    CHECK(!strcmp(id->description, D9_ADAPTER_DESCRIPTION));
    CHECK(!strcmp(id->device_name, D9_ADAPTER_DEVICE_NAME));
    CHECK(!memcmp(id->device_identifier, D9_ADAPTER_DEVICE_ID,
                  sizeof D9_ADAPTER_DEVICE_ID));
    CHECK(id->whql_level == 0);
    id_args[1] = 2; /* D3DENUM_WHQL_LEVEL */
    CHECK(call(&c, factory, 5, id_args, 3) == D3D9OK_NOAUTOGEN);

    uint32_t count_args[2] = {0, D9_X8R8G8B8};
    CHECK(call(&c, factory, 6, count_args, 2) == 1); /* GetAdapterModeCount */

    uint32_t enum_args[4] = {0, D9_X8R8G8B8, 0, MODE};
    CHECK(call(&c, factory, 7, enum_args, 4) == 0); /* EnumAdapterModes */
    const D9DisplayMode *mode = guest_ptr(&c.memory, MODE, sizeof *mode);
    CHECK(mode != NULL);
    CHECK(mode->width == D9_DISPLAY_WIDTH && mode->height == D9_DISPLAY_HEIGHT);
    CHECK(mode->refresh == D9_DISPLAY_REFRESH && mode->format == D9_X8R8G8B8);

    uint32_t display_args[2] = {0, MODE};
    CHECK(call(&c, factory, 8, display_args, 2) == 0); /* GetAdapterDisplayMode */
    mode = guest_ptr(&c.memory, MODE, sizeof *mode);
    CHECK(mode != NULL);
    CHECK(mode->width == D9_DISPLAY_WIDTH && mode->height == D9_DISPLAY_HEIGHT);

    uint32_t type_args[5] = {0, D3DDEVTYPE_HAL, D9_X8R8G8B8,
                             D9_X8R8G8B8, 1};
    CHECK(call(&c, factory, 9, type_args, 5) == 0); /* CheckDeviceType */

    /* ---- 4. CreateDevice and device object identity --------------------- */
    uint32_t pp[14] = {D9_DISPLAY_WIDTH, D9_DISPLAY_HEIGHT, D9_X8R8G8B8,
                       1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1};
    wr(&c, PP, pp, sizeof pp);
    uint32_t create_args[6] = {0, D3DDEVTYPE_HAL, 0,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING, PP, OUT2};
    CHECK(call(&c, factory, 16, create_args, 6) == 0); /* CreateDevice */
    uint32_t device = rd(&c, OUT2);
    CHECK(device != 0);
    c.device = device;
    CHECK(device == DEVICE_GUEST);
    CHECK(vt(&c, device) == DEVICE_VTABLE);

    /* CreateDevice retains the factory and creates one implicit back buffer. */
    CHECK(refs(&c, factory) == 2);
    CHECK(refs(&c, device) == 1);

    /* ---- 5. QueryInterface / GetDirect3D / back buffer ------------------ */
    static const uint8_t iid_d3d9[16] = {
        0xca, 0xcb, 0xbd, 0x81, 0xd4, 0x64, 0x6d, 0x42,
        0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c};
    static const uint8_t iid_device[16] = {
        0x96, 0x3b, 0x22, 0xd0, 0x7a, 0xbf, 0xfd, 0x43,
        0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb};

    wr(&c, GUID, iid_d3d9, sizeof iid_d3d9);
    uint32_t qi_args[2] = {GUID, OUT};
    CHECK(call(&c, factory, 0, qi_args, 2) == 0); /* QueryInterface */
    CHECK(rd(&c, OUT) == factory);
    CHECK(call(&c, factory, 2, NULL, 0) == 2); /* drop QI reference */

    wr(&c, GUID, iid_device, sizeof iid_device);
    qi_args[0] = GUID;
    qi_args[1] = OUT;
    CHECK(call(&c, device, 0, qi_args, 2) == 0);
    CHECK(rd(&c, OUT) == device);
    CHECK(call(&c, device, 2, NULL, 0) == 1);

    uint32_t get_d3d[1] = {OUT};
    CHECK(call(&c, device, 6, get_d3d, 1) == 0); /* GetDirect3D */
    CHECK(rd(&c, OUT) == factory);
    CHECK(call(&c, factory, 2, NULL, 0) == 2); /* drop returned reference */

    uint32_t bb_args[4] = {0, 0, 0, OUT};
    CHECK(call(&c, device, 18, bb_args, 4) == 0); /* GetBackBuffer */
    uint32_t back = rd(&c, OUT);
    CHECK(back != 0);
    c.back = back;
    CHECK(back == BACK_GUEST);
    CHECK(vt(&c, back) == SURFACE_VTABLE);
    CHECK(refs(&c, back) == 2); /* implicit device ref + returned ref */
    CHECK(call(&c, back, 2, NULL, 0) == 1);

    uint32_t creation[1] = {OUT};
    CHECK(call(&c, device, 9, creation, 1) == 0); /* GetCreationParameters */
    CHECK(rd(&c, OUT) == 0);
    CHECK(rd(&c, OUT + 4) == D3DDEVTYPE_HAL);
    CHECK(rd(&c, OUT + 8) == 0);
    CHECK(rd(&c, OUT + 12) == D3DCREATE_HARDWARE_VERTEXPROCESSING);

    /* ---- 6. Device capability query through the host null backend ------- */
    uint32_t caps_args[1] = {CAPS9};
    CHECK(call(&c, device, 7, caps_args, 1) == 0); /* GetDeviceCaps */
    const D9Caps9 *caps = guest_ptr(&c.memory, CAPS9, sizeof *caps);
    CHECK(caps != NULL);
    CHECK(caps->device_type == D3DDEVTYPE_HAL);
    CHECK(caps->adapter_ordinal == 0);
    CHECK(caps->vertex_shader_version == 0xfffe0200u); /* SM2 only */
    CHECK(caps->pixel_shader_version == 0xffff0200u);
    CHECK(caps->max_texture_width == 1024u);

    /* ---- 7. Real COM scene state machine on the null backend ------------ */
    d9_null_reset();
    CHECK(call(&c, device, 41, NULL, 0) == 0); /* BeginScene */
    CHECK(call(&c, device, 42, NULL, 0) == 0); /* EndScene */
    uint32_t present_args[4] = {0, 0, 0, 0};
    CHECK(call(&c, device, 17, present_args, 4) == 0); /* Present */
    const D9NullStats *stats = d9_null_stats();
    CHECK(stats->begins == 1 && stats->ends == 1 && stats->presents == 1);
    CHECK(stats->draws == 0 && stats->scene == 0);
    CHECK(call(&c, device, 42, NULL, 0) == D3D9ERR_INVALIDCALL);
    CHECK(call(&c, device, 17, present_args, 4) == D3D9ERR_INVALIDCALL);

    /* ---- 8. Full teardown and guest-object destruction ------------------ */
    CHECK(call(&c, device, 2, NULL, 0) == 0); /* device destroys back buffer */
    CHECK(rd(&c, device) == 0);
    CHECK(rd(&c, back) == 0);
    CHECK(refs(&c, factory) == 1); /* device's factory reference is gone */
    CHECK(call(&c, factory, 2, NULL, 0) == 0);
    CHECK(rd(&c, factory) == 0);

    chain_stop(&c);

    const char *pass =
        "D3D9 dynamic chain: LoadLibraryA -> GetProcAddress -> Direct3DCreate9 "
        "thunk -> IDirect3D9 -> adapter enumeration -> CreateDevice -> "
        "IDirect3DDevice9 caps/scene -> Present passed";
    puts(pass);
    write_log("D3D9CHAIN RESULT: PASS\n");
#ifdef __3DS__
    printf("%s\n", pass);
    svcSleepThread(3000000000ULL);
    gfxExit();
#endif
    return 0;
}
