/* Diagnostic runner: map the real TESV PE, bind imports (unknown ones become
 * traps that name themselves), and execute a bounded lifted slice. This is not
 * a claim that the game boots; it reports the first exact failure. */
#include "n3ds_env_cfg.h"
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
#include "mem_stats.h"
#include "asset_stream.h"
#include "n3ds_lift_runtime.h"
#include "win32_audio_3ds.h"
#include "win32_audio.h"
#include "win32_xaudio2.h"
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

/* Memory profiles are selected at build time (tools/build.ps1 -MemProfile).
 * The default stays 96/8; nothing ships a larger heap until a device route
 * accepts it.  The remaining region budget is code, static data and stacks. */
#ifndef TESV_HEAP_MIB
#define TESV_HEAP_MIB 96
#endif
#ifndef TESV_LINEAR_MIB
#define TESV_LINEAR_MIB 8
#endif
#define TESV_RESERVE_MIB 4
#if defined(__3DS__) && \
    (TESV_HEAP_MIB + TESV_LINEAR_MIB + MEM_STATS_NON_HEAP_MIB > MEM_STATS_REGION_MIB)
#error "TESV_HEAP_MIB + TESV_LINEAR_MIB + 34 MiB exceeds the 178 MiB region"
#endif

#ifdef __3DS__
/* libctru's default split of a 178 MiB DEV1 title asks for an application heap
 * larger than the heap area and svcBreak()s before main. Pin both heaps so that
 * a diagnostic slice fits the 96 MiB .3dsx region as well as the 178 MiB CIA
 * mode: code + heap + linear must stay inside the region. */
u32 __ctru_heap_size = TESV_HEAP_MIB << 20;
u32 __ctru_linear_heap_size = TESV_LINEAR_MIB << 20;
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

/* Measurement hooks called by n3ds_lift_runtime.c: a guest sample every 1024
 * dispatches (peak between snapshots) and one snapshot on the first fault.
 * Neither touches dispatch or fault semantics. */
static int g_exit_snapshot_done;
void n3ds_mem_guest_sample(size_t used, unsigned regions) {
    mem_stats_sample_guest(used, regions);
}
void n3ds_mem_fault_sample(void) {
    mem_stats_snapshot("fault", NULL);
}
/* atexit covers a normal main() return even when run() never reached its
 * reporting block; run() marks the flag so the log gets exactly one exit line. */
static void mem_exit_snapshot(void) {
    if (!g_exit_snapshot_done) mem_stats_snapshot("exit", "when=atexit");
}

/* Called before every snapshot (frame/alloc/exit) so the periodic line carries
 * live guests, both IR caches, the texture cache and the OS region. */
static void mem_refresh(void *ctx) {
    BootSession *b = ctx;
    if (!b) return;
    mem_stats_set_guest(b->memory.used, b->memory.limit, b->memory.count);
    IrCacheStats ir = {0};
    n3ds_ir_cache_stats(&ir);
    mem_stats_set_irdecode(ir.bytes, ir.blocks);
    N3dsIrcStats irc = {0};
    if (n3ds_lift_container_stats(&irc) == 0)
        mem_stats_set_irc_detail(n3ds_lift_container_index_bytes(),
                                 irc.cache_bytes, irc.cache_records);
    else
        mem_stats_set_irc(0, 0);
    /* Host allocation accounting: calls/failures and the Win32 commit promise,
     * plus guest backing failures.  All measured by the shims. */
    Win32AllocStats as = {0};
    win32_alloc_stats(&as);
    Win32VaStats vs = {0};
    win32_va_stats(&vs);
    MemStatsAlloc ma = {
        .calls = as.va_calls + as.heap_calls + as.crt_calls,
        .failures = as.failures,
        .peak_live = as.peak_live,
        .backing_failures = b->memory.backing_failures,
        .va_live = as.va_live,
        .heap_live = as.heap_live,
        .crt_live = as.crt_live,
        .va_reserved_live = vs.reserved_live,
        .va_committed_live = vs.committed_live,
        .va_committed_peak = vs.committed_peak,
        .va_backed_live = vs.backed_live,
        .va_backed_peak = vs.backed_peak,
        .va_reservations = vs.reservations,
        .va_commit_failures = vs.commit_failures,
    };
    mem_stats_set_alloc(&ma);
    D9Device *dev = d9_com_device();
    if (dev) {
        D9TexCacheStats tc;
        if (d9_texture_cache_stats(dev, &tc) == D9_OK) {
            mem_stats_set_texcache(tc.guest_bytes, tc.gpu_bytes, tc.peak_guest,
                                   tc.peak_gpu, tc.budget_trims,
                                   tc.budget_trim_evictions, tc.evictions_gpu);
            mem_stats_set_tex_pins(tc.non_evictable, tc.pinned,
                                   tc.pinned_refusals);
        }
    }
    /* Measured resident bytes of the archives that are actually open, plus
     * their component split.  The overlay opens sources lazily, so
     * have_bsa follows bsa_mounted, not the configured source count. */
    Win32BsaOverlayStats ov = {0};
    win32_bsa_overlay_stats(&ov);
    mem_stats_set_bsa_detail(ov.resident_bytes, ov.index_bytes,
                             ov.page_cache_bytes, ov.range_cache_bytes,
                             ov.archives, ov.resident_archives);
    StreamStatsSnapshot ss = {0};
    stream_stats_global(&ss);
    mem_stats_set_stream(ss.queued, ss.staged_bytes, ss.physical_reads,
                         ss.physical_bytes, ss.physical_misses, ss.demand_hits,
                         ss.demand_bytes, ss.prefetch_demand_hits,
                         ss.cancelled, ss.budget_denied);
    size_t tg = 0, tp = 0;
    const char *gs = getenv("N3DS_TEX_GUEST_LIMIT");
    const char *gg = getenv("N3DS_TEX_GPU_LIMIT");
    if (gs || gg) {
        int ok1 = 1, ok2 = 1;
        if (gs) tg = mem_stats_parse_limit(gs, &ok1);
        if (gg) tp = mem_stats_parse_limit(gg, &ok2);
        if (ok1 && ok2) mem_stats_set_tex_limits(tg, tp);
    }
#ifdef __3DS__
    size_t region = osGetMemRegionSize(MEMREGION_APPLICATION);
    size_t used = osGetMemRegionUsed(MEMREGION_APPLICATION);
    mem_stats_set_os_region(region, used, region - used);
#endif
}

static int run(const char *exe, const char *root, uint64_t steps, size_t budget, const char *log,
               const D9Backend *backend, void *backend_ctx) {
    BootSession b;
    n3ds_boot_init(&b, budget);
    mem_stats_set_refresh(mem_refresh, &b);
    mem_stats_set_guest(b.memory.used, b.memory.limit, b.memory.count);
    /* Pre-load: guest_init() has not mapped the PE/stack/heap yet, so this line
     * is expected to show guest_used=0.  tag=init is the first real reading. */
    mem_stats_snapshot("start", "phase=preload");
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
        /* The 4 MiB default is below the measured renderer's target set.  Host
         * runs override it with TESV_D3D9_BUDGET_MB; the device flag file in
         * main() sets the same variable before this call. */
        size_t res_mib = 4;
        const char *rmb = getenv("TESV_D3D9_BUDGET_MB");
        if (rmb && *rmb) {
            unsigned long v = strtoul(rmb, NULL, 10);
            if (v >= 8 && v <= 128) res_mib = (size_t)v;
        }
        if(backend&&d9_com_set_backend(backend,backend_ctx,res_mib<<20))
            printf("d3d9: PICA200 backend installed (%zu MiB resource budget)\n", res_mib);
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
    } else {
        /* Without the local pilot_out_mult table every d3dx9_42.dll hotpatch
         * body the loader binds is undispatchable, so the first guest call
         * through its IAT (for example the D3DXMatrixMultiply thunk at
         * 0xf79dc2 -> 0x0802fa80) faults as "unknown indirect function".
         * Name it in the run log; the build already warns on the same gap. */
        printf("d3dx: no lifted body table; a guest call to a d3dx9_42.dll "
               "hotpatch body (e.g. 0802fa80 D3DXMatrixMultiply) will fault\n");
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
    /* Every guest region, the bound container index and the IR cache exist at
     * this point; this is the measured starting point of the run. */
    mem_stats_snapshot("init", NULL);
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
    {
        /* Target VRAM budget: live/peak and the policy counters.  Printed at the
         * same boundary as the guest memory summary so an Azahar log shows
         * exactly how the 6 MiB PICA VRAM was spent. */
        D9Device *dd=d9_com_device();
        D9TargetStats ts;
        if(dd&&d9_target_stats(dd,&ts)==D9_OK)
            printf("[d3d9-target] vram limit=%zu KiB live=%zu KiB peak=%zu KiB "
                   "alloc_failures=%u evictions=%u linear_fallbacks=%u downscales=%u "
                   "pool_hits=%u pool_flushes=%u pool_entries=%u suspended=%u\n",
                   ts.vram_limit>>10,ts.vram_live>>10,ts.vram_peak>>10,
                   ts.alloc_failures,ts.evictions,ts.linear_fallbacks,ts.downscales,
                   ts.pool_hits,ts.pool_flushes,ts.pool_entries,ts.suspended_targets);
    }
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
            fprintf(f, "run rc=%d\nguest_used=%lu steps=%llu\napp_region=%u MiB free=%u MiB\n%s\n%s\n", rc,
                    (unsigned long)b.memory.used, (unsigned long long)steps, g_region_mib, g_free_mib, b.message, n3ds_lift_fault());
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
    mem_stats_snapshot("exit", "when=run");
    g_exit_snapshot_done = 1;
    win32_memory_report(stderr);
    d3dx_surface_shutdown();
    d3dx_texture_shims_uninstall();
    d9_com_shutdown();
    n3ds_boot_destroy(&b);
    return rc;
}

#ifdef __3DS__
int main(int argc, char **argv) {
    /* Open the SD trace before cfg so every applied key is visible there. */
    (void)freopen("sdmc:/Skyrim_N3DS_stderr.log", "w", stderr);
    const char *cfg_path = (argc > 1 && argv[1] && *argv[1]) ? argv[1] : NULL;
    if (!cfg_path) cfg_path = getenv("TESV_CFG");
    if (!cfg_path || !*cfg_path) cfg_path = "sdmc:/Skyrim_N3DS.cfg";
    if (n3ds_env_cfg_load(cfg_path, stderr) != 0)
        fprintf(stderr, "[cfg] load failed: %s\n", cfg_path);
    gfxInitDefault();
    mem_stats_init();
    mem_stats_configure(TESV_HEAP_MIB, TESV_LINEAR_MIB, TESV_RESERVE_MIB);
    mem_stats_set_log_path(MEM_STATS_PATH);
    atexit(mem_exit_snapshot);
    {
        const char *e = getenv("N3DS_MEM_EVERY");
        if (e && *e) {
            int ok = 0;
            size_t n = mem_stats_parse_limit(e, &ok);
            if (ok) mem_stats_set_every((unsigned)n);
            else fprintf(stderr, "[mem] N3DS_MEM_EVERY='%s' ignored\n", e);
        }
    }
    mem_stats_snapshot("boot", NULL);
    D9Citro *gpu=d9_citro_open();
    mem_stats_snapshot("arenas", NULL);
    consoleInit(GFX_TOP, NULL); osSetSpeedupEnable(true);
    printf("Skyrim N3DS - TESV startup slice\nNot a playable build.\n\n");
    printf("D3D9 backend: %s\n",gpu?"PICA200":"null fallback (PICA init failed)");
    printf("Hold X at start: memory probe (measurement only)\n\n");
    hidScanInput();
#ifdef TESV_FORCE_MEMPROBE
    int probe = 1; /* isolated diagnostic build only; never shippable */
#else
    const char *memprobe = getenv("TESV_MEMPROBE");
    int probe = (hidKeysHeld() & KEY_X) != 0 ||
                (memprobe && *memprobe && strcmp(memprobe, "0"));
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
    /* Guest mappings come from the newlib application heap.  The guest budget is
     * heap - reserve: explicit, logged, and never larger than the heap. */
    size_t reserve_bytes = (size_t)TESV_RESERVE_MIB << 20;
    size_t heap_bytes = (size_t)TESV_HEAP_MIB << 20;
    size_t budget = mem_stats_guest_budget(TESV_HEAP_MIB, TESV_RESERVE_MIB);
    printf("[mem] profile %ux%u MiB (newlib/linear): heap=%llu MiB "
           "linear=%llu MiB reserve=%llu MiB guest_budget=%llu MiB\n",
           TESV_HEAP_MIB, TESV_LINEAR_MIB,
           (unsigned long long)(heap_bytes >> 20),
           (unsigned long long)(((size_t)TESV_LINEAR_MIB << 20) >> 20),
           (unsigned long long)(reserve_bytes >> 20),
           (unsigned long long)(budget >> 20));
    if (!budget || budget + reserve_bytes != heap_bytes) {
        printf("[mem] FATAL: guest budget arithmetic failed (heap=%llu "
               "reserve=%llu budget=%llu)\n",
               (unsigned long long)heap_bytes,
               (unsigned long long)reserve_bytes,
               (unsigned long long)budget);
        d9_citro_close(gpu);
        gfxExit();
        return 1;
    }
    if (((size_t)TESV_HEAP_MIB + TESV_LINEAR_MIB) << 20 > region) {
        printf("[mem] WARNING: heap+linear exceed the %u MiB application region\n",
               (unsigned)(region >> 20));
    }
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
    /* Device-only diagnostics: the 3DS process cannot inherit host environment
     * variables, so a flag file on the virtual SD enables the same traces that
     * host runs set with TESV_MENU_TRACE / TESV_DRAW_PROBE.  The full
     * TESV_D3D9_TRACE is deliberately not enabled: it logs every COM call. */
    if (access("sdmc:/Skyrim/tesv_trace.flag", F_OK) == 0) {
        setenv("TESV_MENU_TRACE", "1", 1);
        setenv("TESV_DRAW_PROBE", "1", 1);
        fprintf(stderr, "[probe] trace flag present: menu + draw probes\n");
    }
    /* The 4 MiB default texture cache is too small for the real menu path;
     * host runs override it with TESV_D3D9_BUDGET_MB.  The device cannot
     * inherit host variables, so the same value comes from a one-line file. */
    /* 6 MiB of the 10 MiB VRAM are usable; the PICA backend enforces that
     * globally (N3DS_VRAM_LIMIT).  Targets get 4.25 MiB so the four 512x512
     * menu targets, the retained back buffer/depth, the LCD target and the
     * smaller transient targets all fit.  A cfg key can override it. */
    setenv("N3DS_TARGET_VRAM_LIMIT", "4352K", 0);
    {
        FILE *bf = fopen("sdmc:/Skyrim/tesv_d3d9_budget.txt", "r");
        if (bf) {
            char mb[16] = {0};
            if (fgets(mb, sizeof mb, bf)) {
                mb[strcspn(mb, "\r\n")] = 0;
                if (mb[0] >= '1' && mb[0] <= '9') {
                    setenv("TESV_D3D9_BUDGET_MB", mb, 1);
                    fprintf(stderr, "[probe] TESV_D3D9_BUDGET_MB=%s from flag file\n", mb);
                }
            }
            fclose(bf);
        }
    }
    if (n3ds) run("sdmc:/Skyrim/TESV.exe", "sdmc:/Skyrim", TESV_ARM_STEPS, budget, "sdmc:/Skyrim_N3DS_tesv.log",
                  gpu?d9_citro_backend():NULL,gpu);
    else puts("New Nintendo 3DS required");
    d9_citro_close(gpu);
    puts("START: exit");
    while (aptMainLoop()) {
        hidScanInput();
        win32_audio_tick();
        win32_xaudio2_tick(0);
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
    mem_stats_init();
    mem_stats_configure(TESV_HEAP_MIB, TESV_LINEAR_MIB, TESV_RESERVE_MIB);
    mem_stats_set_log_path(getenv("N3DS_MEM_LOG"));
    atexit(mem_exit_snapshot);
    const char *root = argc > 2 ? argv[2] : ".";
    uint64_t steps = argc > 3 ? strtoull(argv[3], NULL, 10) : 200000;
    size_t budget = argc > 4 ? (size_t)strtoull(argv[4], NULL, 0)
                             : mem_stats_guest_budget(TESV_HEAP_MIB, TESV_RESERVE_MIB);
    if (budget < (28u << 20)) budget = 28u << 20;
    if (budget > (256u << 20)) budget = 256u << 20;
    fprintf(stderr, "[mem] profile %ux%u MiB guest_budget=%llu MiB\n",
            TESV_HEAP_MIB, TESV_LINEAR_MIB, (unsigned long long)(budget >> 20));
    if ((argc > 5 && !strcmp(argv[5], "memprobe")) || getenv("TESV_MEMPROBE")) {
        fprintf(stderr, "[mem] probe enabled: the engine will be told it can commit "
                        "512 MiB. Measurement only.\n");
        win32_set_memory_probe(1);
    }
    return run(argv[1], root, steps, budget, NULL, NULL, NULL);
}
#endif
