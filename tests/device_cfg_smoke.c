/* Host coverage for the device cfg parser/loader.  Compiled with -D__3DS__ so
 * the production loader is exercised without requiring any libctru symbols. */
#ifndef __3DS__
#error "compile this test with -D__3DS__"
#endif
#include "n3ds_env_cfg.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int set_test_env(const char *key, const char *value, int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(key)) return 0;
    return _putenv_s(key, value);
#else
    return setenv(key, value, overwrite);
#endif
}

static int unset_test_env(const char *key) {
#ifdef _WIN32
    return _putenv_s(key, "");
#else
    return unsetenv(key);
#endif
}

static void clear_env(void) {
    static const char *const keys[] = {
        "N3DS_TEX_GPU_LIMIT", "N3DS_TEX_GUEST_LIMIT", "N3DS_STREAM",
        "N3DS_STREAM_INDEX", "N3DS_STREAM_DATA", "N3DS_IRC_PATH",
        "N3DS_IRC_ENABLE", "N3DS_IRC_REQUIRED", "N3DS_IRC_CACHE_BYTES",
        "N3DS_IRC_CACHE_RECORDS", "N3DS_MEM_EVERY", "TESV_MEMPROBE",
        "TESV_D3D9_CAPS_PROBE", "UNKNOWN_DEVICE_CFG_KEY"
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
        (void)unset_test_env(keys[i]);
}

static void expect_env(const char *key, const char *value) {
    const char *actual = getenv(key);
    assert(actual && !strcmp(actual, value));
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void) {
    N3dsEnvCfg cfg;
    size_t ignored = 0;
    const char *text =
        "# full-line comment\n"
        "; another comment\n"
        "  N3DS_TEX_GPU_LIMIT = 4M  \n"
        "N3DS_TEX_GUEST_LIMIT=\"12M\"\n"
        "N3DS_STREAM = '1'\n"
        "N3DS_TEXT_WINDOW_MB=4\n"
        "N3DS_TARGET_VRAM_LIMIT=4352K\n"
        "N3DS_TEX_GPU_LIMIT = 6M ; duplicate: last wins\n"
        "N3DS_IRC_PATH = \"sdmc:/Skyrim/ir/custom file.irc\" # trailing comment\n"
        "N3DS_MEM_EVERY=\n"
        "UNKNOWN_DEVICE_CFG_KEY=ignored\n"
        "TESV_D3D9_CAPS_PROBE=1\n"
        "malformed line\n";
    assert(n3ds_env_cfg_parse(text, &cfg, &ignored) == 7);
    assert(ignored == 3);
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_TEX_GPU_LIMIT"), "6M"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_TEX_GUEST_LIMIT"), "12M"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_STREAM"), "1"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_TEXT_WINDOW_MB"), "4"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_TARGET_VRAM_LIMIT"), "4352K"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_IRC_PATH"),
                   "sdmc:/Skyrim/ir/custom file.irc"));
    assert(!strcmp(n3ds_env_cfg_get(&cfg, "N3DS_MEM_EVERY"), ""));
    assert(!n3ds_env_cfg_get(&cfg, "UNKNOWN_DEVICE_CFG_KEY"));
    assert(!n3ds_env_cfg_get(&cfg, "TESV_D3D9_CAPS_PROBE"));

    clear_env();
    write_file("device_cfg_test.cfg",
               "N3DS_TEX_GPU_LIMIT=4M\r\n"
               "N3DS_TEX_GPU_LIMIT = \"5M\"\r\n"
               "N3DS_TEX_GUEST_LIMIT=12M\r\n"
               "TESV_D3D9_CAPS_PROBE=1\r\n");
    FILE *log = fopen("device_cfg_test.log", "w");
    assert(log);
    assert(n3ds_env_cfg_load("device_cfg_test.cfg", log) == 0);
    assert(fclose(log) == 0);
    expect_env("N3DS_TEX_GPU_LIMIT", "5M");
    expect_env("N3DS_TEX_GUEST_LIMIT", "12M");
    expect_env("N3DS_STREAM", "0");
    assert(!getenv("TESV_D3D9_CAPS_PROBE"));
    log = fopen("device_cfg_test.log", "rb");
    assert(log);
    char logged[2048];
    size_t n = fread(logged, 1, sizeof logged - 1, log);
    assert(n > 0 && fclose(log) == 0);
    logged[n] = 0;
    assert(strstr(logged, "[cfg] N3DS_TEX_GPU_LIMIT=5M"));
    assert(strstr(logged, "[cfg] N3DS_STREAM=0"));

    /* Existing env wins over both file values and defaults. */
    assert(set_test_env("N3DS_TEX_GPU_LIMIT", "9M", 1) == 0);
    assert(set_test_env("N3DS_STREAM", "1", 1) == 0);
    assert(n3ds_env_cfg_load("device_cfg_test.cfg", NULL) == 0);
    expect_env("N3DS_TEX_GPU_LIMIT", "9M");
    expect_env("N3DS_STREAM", "1");

    /* Missing file is a successful no-op for file contents; defaults remain. */
    clear_env();
    assert(set_test_env("N3DS_TEX_GPU_LIMIT", "9M", 1) == 0);
    assert(set_test_env("N3DS_STREAM", "1", 1) == 0);
    assert(remove("device_cfg_test.cfg") == 0);
    assert(n3ds_env_cfg_load("device_cfg_missing.cfg", NULL) == 0);
    expect_env("N3DS_TEX_GPU_LIMIT", "9M");
    expect_env("N3DS_TEX_GUEST_LIMIT", "16M");
    expect_env("N3DS_STREAM", "1");

    clear_env();
    assert(n3ds_env_cfg_apply_defaults(NULL) == 0);
    expect_env("N3DS_TEX_GPU_LIMIT", "6M");
    expect_env("N3DS_TEX_GUEST_LIMIT", "16M");
    expect_env("N3DS_STREAM", "0");
    assert(n3ds_env_cfg_apply_entry("N3DS_TEX_GPU_LIMIT", "2M", NULL) == 0);
    expect_env("N3DS_TEX_GPU_LIMIT", "6M");

    clear_env();
    (void)remove("device_cfg_test.log");
    puts("device_cfg: PASS");
    return 0;
}
