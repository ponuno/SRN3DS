/* Diagnostic runner: map the real TESV PE, bind imports (unknown ones become
 * traps that name themselves), and execute a bounded lifted slice. This is not
 * a claim that the game boots; it reports the first exact failure. */
#include "n3ds_boot.h"
#include "n3ds_pilot.h"
#include "pilot_guard.h"
#include "win32_shims.h"
#include "rt_prof.h"
#include "d3d9_com.h"
#include "d3d9_null.h"
#include "d3dx_texture_shims.h"
#include "d3dx_surface_shims.h"
#include "tesv_sm3_probe.h"
#include "tesv_cube_probe.h"
#include "tesv_cecdc0_probe.h"
#include "tesv_cecdc0_source.h"
#include "ir_core.h"
#include "hprof.h"
#include "win32_audio_3ds.h"
#include "win32_audio.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef __3DS__
#include <3ds.h>
#include <unistd.h>
#include "d3d9_citro3d.h"
#endif

extern const FnEntry g_fn_table_tesv[];
extern const unsigned g_fn_count_tesv;
/* Optional lifted D3DX bodies for hotpatch-resolved imports.  These are the
 * real symbols of the generated pilot table.  Weak so a build without it still
 * links; dispatch then fails explicitly on the body address instead of
 * silently aliasing it. */
extern const FnEntry g_fn_table_pilot[] __attribute__((weak));
extern const unsigned g_fn_count_pilot __attribute__((weak));

#ifndef TESV_ARM_STEPS
#define TESV_ARM_STEPS 2000000u
#endif

#ifdef __3DS__
/* libctru's default split of a 178 MiB DEV1 title asks for an application heap
 * larger than the heap area and svcBreak()s before main. Pin both heaps so that
 * a diagnostic slice fits the 96 MiB .3dsx region as well as the 178 MiB CIA
 * mode: code + heap + linear must stay inside the region. */
u32 __ctru_heap_size = 96u << 20;
u32 __ctru_linear_heap_size = 8u << 20;
#endif

static unsigned g_region_mib, g_free_mib;

static int add_dll(BootSession *b, const char *name, uint32_t base, const char *root, const char *fallback) {
    char path[512];
    if (root && *root) {
        snprintf(path, sizeof path, "%s/%s", root, name);
        if (n3ds_boot_add_data_file(b, name, path, base) == 0) return 1;
    }
    if (fallback) {
        snprintf(path, sizeof path, "%s/%s", fallback, name);
        if (n3ds_boot_add_data_file(b, name, path, base) == 0) return 1;
    }
    return 0;
}

/* Measurement-only host targets for the two NiRenderTargetGroup buffer
 * getters that TESV_D3D9_CUBE_SKIP_PROBE redirects while
 * TESV_CUBE_PROBE_VTABLE=1.  The synthetic entries are not lifted, so bind
 * them as a two-entry host module.  The probe slice supplies the conditional
 * implementation through a weak hook; an ordinary slice keeps the checked
 * NULL fallback. */
extern int tesv_cube_probe_getbuffer(CPU *c, uint32_t real_addr)
    __attribute__((weak));

static void cube_probe_stub33(CPU *c) {
    if (tesv_cube_probe_getbuffer &&
        tesv_cube_probe_getbuffer(c, 0x00fb6070u)) return;
    if (c) { c->r[0] = 0; c->r[4] += 8u; }
}
static void cube_probe_stub34(CPU *c) {
    if (tesv_cube_probe_getbuffer &&
        tesv_cube_probe_getbuffer(c, 0x00fb6090u)) return;
    if (c) { c->r[0] = 0; c->r[4] += 8u; }
}
static const FnEntry cube_probe_table[] = {
    {0x30000000u, cube_probe_stub34},
    {0x30000010u, cube_probe_stub33}
};

static int run(const char *exe, const char *root, unsigned steps, size_t budget, const char *log,
               const D9Backend *backend, void *backend_ctx) {
    BootSession b;
    n3ds_boot_init(&b, budget);
    int rc = -1;
    IrCacheStats ir_stats = {0};
    if (n3ds_boot_add_file(&b, "TESV.exe", exe, 0)) {
        printf("load: %s\n", b.message);
        goto out;
    }
#ifdef __3DS__
    add_dll(&b, "d3dx9_42.dll", 0x08000000u, root, NULL);
    add_dll(&b, "binkw32.dll", 0x09000000u, root, NULL);
    add_dll(&b, "X3DAudio1_7.dll", 0x0a000000u, root, NULL);
    add_dll(&b, "steam_api.dll", 0x0b000000u, root, NULL);
#else
    add_dll(&b, "d3dx9_42.dll", 0x08000000u, root, "C:/Windows/SysWOW64");
    add_dll(&b, "binkw32.dll", 0x09000000u, root, "E:/SteamLibrary/steamapps/common/Skyrim");
    add_dll(&b, "X3DAudio1_7.dll", 0x0a000000u, root, "C:/Windows/SysWOW64");
    add_dll(&b, "steam_api.dll", 0x0b000000u, root, "E:/SteamLibrary/steamapps/common/Skyrim");
#endif
    FnModule tables[6] = {{"tesv", g_fn_table_tesv, g_fn_count_tesv}, {"d3d9", NULL, 0},
                          {"d3dx", NULL, 0}, {"d3dx-texture", NULL, 0},
                          {"d3dx-surface", NULL, 0}};
    unsigned table_count = 1;
    /* The D3D9 bridge answers the guest's renderer queries truthfully even in a
     * headless diagnostic build: one 400x240x32 adapter, SM2-only caps. */
    if (d9_com_init(&b.memory, NULL) == 0) {
        tables[table_count++] = d9_com_module();
        /* Headless software backend on host; real PICA200 on 3DS when init
         * succeeded.  Registration happens before the guest CreateDevice. */
#ifdef __3DS__
        if(backend&&d9_com_set_backend(backend,backend_ctx,4u<<20))printf("d3d9: PICA200 backend installed\n");
        else
#endif
        if (d9_null_register() != 0) printf("d3d9: null backend not installed\n");
    } else {
        printf("d3d9: COM bridge unavailable; D3D9 imports stay unresolved\n");
    }
    /* Plain D3DX exports get host thunks instead of the real 0x080b... bodies.
     * Both modules append to the win32 extra-resolver chain, so the D3D9
     * bridge resolver stays primary and the d3dx hotpatch path is untouched.
     * Texture creation needs the D3D9 session; surface/shader work must check
     * it per call, so both installs follow d9_com_init(). */
    if (d3dx_texture_shims_install(&b.memory) == 0) {
        tables[table_count++] = d3dx_texture_shims_module();
        printf("d3dx: DDS image-info and texture shims registered\n");
    }
    if (d3dx_surface_init(&b.memory) == 0) {
        tables[table_count++] = d3dx_surface_module();
        printf("d3dx: surface/shader shims registered\n");
    } else {
        printf("d3dx: surface/shader shims unavailable (D3D9 session missing)\n");
    }
    /* The import linker resolves a validated MSVC hotpatch export to its real
     * body, so that body must be dispatchable.  Link the generated D3DX table
     * when the build provides it; otherwise dispatch fails explicitly. */
    if (&g_fn_table_pilot && g_fn_count_pilot) {
        tables[table_count++] = (FnModule){"d3dx", g_fn_table_pilot, g_fn_count_pilot};
        printf("d3dx: %u lifted function(s) registered\n", g_fn_count_pilot);
    }
    if (table_count < sizeof tables / sizeof tables[0])
        tables[table_count++] = (FnModule){"cube-probe", cube_probe_table,
            (unsigned)(sizeof cube_probe_table / sizeof cube_probe_table[0])};
    n3ds_boot_set_unresolved_traps(&b, 1);
    if (n3ds_boot_prepare(&b, tables, table_count, 0x80000, 0x200000, root)) {
        printf("prepare: %s\n", b.message);
        goto out;
    }
    if (tesv_sm3_gate_probe(&b.memory)) {
        printf("sm3 probe: refused; guest run not started\n");
        goto out;
    }
    if (tesv_cube_skip_probe(&b.memory)) {
        printf("cube probe: refused; guest run not started\n");
        goto out;
    }
    if (tesv_cecdc0_skip_probe(&b.memory)) {
        printf("cecdc0 probe: refused; guest run not started\n");
        goto out;
    }
    if (tesv_cecdc0_source_probe(&b.memory)) {
        printf("cecdc0 source probe: refused; guest run not started\n");
        goto out;
    }
    if (n3ds_ir_cache_init(4u << 20) != 0) printf("IR cache: 4 MiB allocation failed; using default\n");
    n3ds_pilot_set_steps(steps);
    uint32_t status = 0;
    rt_prof_reset();
    rt_prof_enable(1);
    hprof_reset();
    int r = n3ds_boot_run(&b, &status);
    hprof_dump();
    rt_prof_enable(0);
    n3ds_ir_cache_stats(&ir_stats);
    printf("IR cache: blocks=%llu bytes=%llu hits=%llu misses=%llu evicted=%llu\n",
           (unsigned long long)ir_stats.blocks, (unsigned long long)ir_stats.bytes,
           (unsigned long long)ir_stats.hits, (unsigned long long)ir_stats.misses,
           (unsigned long long)ir_stats.evicted);
    printf("run rc=%d status=%lu message=%s\n", r, (unsigned long)status, b.message);
    printf("first fault: %s\n", n3ds_lift_fault());
#ifdef __3DS__
    const char *prof_path = "sdmc:/Skyrim_N3DS_tesv_prof.csv";
#else
    const char *prof_path = "tesv_profile.csv";
#endif
    int rows = rt_prof_write_csv(prof_path);
    printf("profile csv: %s (%d rows)\n", rows >= 0 ? prof_path : "(write failed)", rows);
    rt_prof_print_top(24);
    unsigned traps = win32_unresolved_trap_count();
    printf("unresolved imports bound to fault trap: %u\n", traps);
    for (unsigned i = 0; i < traps; ++i) printf("  %s\n", win32_unresolved_trap_text(i));
    rc = r ? 1 : 0;
out:
    if (log) {
        FILE *f = fopen(log, "w");
        if (f) {
            fprintf(f, "run rc=%d\nguest_used=%lu steps=%u\napp_region=%u MiB free=%u MiB\n%s\n%s\n", rc,
                    (unsigned long)b.memory.used, steps, g_region_mib, g_free_mib, b.message, n3ds_lift_fault());
            if (win32_last_exit_from())
                fprintf(f, "exit code=%lu from=%08lx\n", (unsigned long)win32_last_exit_code(),
                        (unsigned long)win32_last_exit_from());
            unsigned traps = win32_unresolved_trap_count();
            fprintf(f, "unresolved imports bound to fault trap: %u\n", traps);
            for (unsigned i = 0; i < traps; ++i) fprintf(f, "  %s\n", win32_unresolved_trap_text(i));
            fprintf(f, "executed functions: %lu, calls: %lu\n",
                    (unsigned long)rt_prof_distinct(), (unsigned long)rt_prof_total());
            fprintf(f, "IR cache: blocks=%llu bytes=%llu hits=%llu misses=%llu evicted=%llu\n",
                    (unsigned long long)ir_stats.blocks, (unsigned long long)ir_stats.bytes,
                    (unsigned long long)ir_stats.hits, (unsigned long long)ir_stats.misses,
                    (unsigned long long)ir_stats.evicted);
            fclose(f);
        }
    }
    fprintf(stderr,"[d3dx] D3DXCompileShader entries: %u\n",
            d3dx_surface_compile_shader_calls());
    win32_memory_report(stderr);
    d3dx_surface_shutdown();
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_boot_destroy(&b);
    return rc;
}

#ifdef __3DS__
int main(void) {
    gfxInitDefault();
    D9Citro *gpu=d9_citro_open();
    consoleInit(GFX_TOP, NULL); osSetSpeedupEnable(true);
    /* Keep the complete stderr trace on SD so emulator runs can be read
     * from the host after the console scrolls or the engine exits. */
    (void)freopen("sdmc:/Skyrim_N3DS_stderr.log", "w", stderr);
    printf("Skyrim N3DS - TESV startup slice\nNot a playable build.\n\n");
    printf("D3D9 backend: %s\n",gpu?"PICA200":"null fallback (PICA init failed)");
    printf("Hold X at start: memory probe (measurement only)\n\n");
    hidScanInput();
#ifdef TESV_FORCE_MEMPROBE
    int probe = 1; /* isolated diagnostic build only; never shippable */
#else
    int probe = (hidKeysHeld() & KEY_X) != 0;
#endif
    if (probe) {
#ifdef TESV_FORCE_MEMPROBE
        printf("MEMORY PROBE ON (build-time diagnostic): told it can commit 512 MiB.\n");
#else
        printf("MEMORY PROBE ON: the engine is told it can commit 512 MiB.\n");
#endif
        printf("This value is fictional and exists only to measure the next barrier.\n\n");
        win32_set_memory_probe(1);
    }
    /* Real audio sink for the guest's waveOut calls; it logs which backend ran. */
    win32_audio_3ds_install();
    bool n3ds = false; APT_CheckNew3DS(&n3ds);
    size_t region = osGetMemRegionSize(MEMREGION_APPLICATION);
    size_t free_mem = osGetMemRegionFree(MEMREGION_APPLICATION);
    /* Guest mappings come from the newlib application heap. The 178 MiB CIA
     * region has enough room for a 96 MiB heap; keep a 4 MiB reserve for libctru
     * and runtime allocations and allow a 92 MiB guest budget. */
    size_t budget = (92u << 20);
    if (budget > ((__ctru_heap_size) - (4u << 20))) budget = (__ctru_heap_size) - (4u << 20);
    g_region_mib = (unsigned)(region >> 20);
    g_free_mib = (unsigned)(free_mem >> 20);
    printf("App region: %u MiB, free %u MiB\n\n", g_region_mib, g_free_mib);
    /* Measurement-only surface gate: when this flag file exists on the SD
     * card, the adapter probe advertises HW T&L so the next layer can be
     * measured.  Never present on a shipping card. */
    if (access("sdmc:/Skyrim/tesv_caps_probe.flag", F_OK) == 0) {
        setenv("TESV_D3D9_CAPS_PROBE", "1", 1);
        setenv("TESV_D3D9_SM3_GATE_PROBE", "1", 1);
        setenv("TESV_D3D9_SHADER_SKIP_PROBE", "1", 1);
        fprintf(stderr, "[probe] probe flags present: HW T&L + SM3 gate + shader skip "
                        "(measurement only, not shippable)\n");
    }
    if (access("sdmc:/Skyrim/tesv_depth_pair_probe.flag", F_OK) == 0)
        setenv("TESV_D3D9_DEPTH_PAIR_PROBE", "1", 1);
    if (n3ds) run("sdmc:/Skyrim/TESV.exe", "sdmc:/Skyrim", TESV_ARM_STEPS, budget, "sdmc:/Skyrim_N3DS_tesv.log",
                  gpu?d9_citro_backend():NULL,gpu);
    else puts("New Nintendo 3DS required");
    d9_citro_close(gpu);
    puts("START: exit");
    while (aptMainLoop()) {
        hidScanInput();
        win32_audio_tick();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    win32_audio_3ds_shutdown();
    gfxExit();
    return 0;
}
#else
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s TESV.exe [root] [steps] [budget_bytes] [memprobe]\n", argv[0]); return 2; }
    const char *root = argc > 2 ? argv[2] : ".";
    unsigned steps = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 200000;
    size_t budget = argc > 4 ? (size_t)strtoull(argv[4], NULL, 0) : (64u << 20);
    if (budget < (28u << 20)) budget = 28u << 20;
    if (budget > (256u << 20)) budget = 256u << 20;
    if ((argc > 5 && !strcmp(argv[5], "memprobe")) || getenv("TESV_MEMPROBE")) {
        fprintf(stderr, "[mem] probe enabled: the engine will be told it can commit "
                        "512 MiB. Measurement only.\n");
        win32_set_memory_probe(1);
    }
    return run(argv[1], root, steps, budget, NULL, NULL, NULL);
}
#endif
