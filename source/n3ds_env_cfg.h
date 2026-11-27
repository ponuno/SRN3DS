#pragma once
/* Small allowlisted KEY=VALUE reader used by the 3DS CIA launcher.  The parser
 * and apply helpers are host-testable; only n3ds_env_cfg_load() is device-only. */
#include <stddef.h>
#include <stdio.h>

#define N3DS_ENV_CFG_KEY_CAP 64
#define N3DS_ENV_CFG_VALUE_CAP 512
#define N3DS_ENV_CFG_MAX_ENTRIES 16

typedef struct {
    char key[N3DS_ENV_CFG_KEY_CAP];
    char value[N3DS_ENV_CFG_VALUE_CAP];
} N3dsEnvCfgEntry;

typedef struct {
    N3dsEnvCfgEntry entries[N3DS_ENV_CFG_MAX_ENTRIES];
    size_t count;
} N3dsEnvCfg;

/* Returns the number of distinct supported keys, or -1 for invalid arguments.
 * Unknown/malformed/oversized lines are ignored and counted in *ignored. */
int n3ds_env_cfg_parse(const char *text, N3dsEnvCfg *cfg, size_t *ignored);
int n3ds_env_cfg_parse_stream(FILE *file, N3dsEnvCfg *cfg, size_t *ignored);
const char *n3ds_env_cfg_get(const N3dsEnvCfg *cfg, const char *key);

/* Apply without replacing an existing environment variable.  Returns 1 when
 * set, 0 when already present, and -1 on error.  Applied keys are logged as
 * "[cfg] KEY=VALUE" when log is non-NULL. */
int n3ds_env_cfg_apply_entry(const char *key, const char *value, FILE *log);
int n3ds_env_cfg_apply(const N3dsEnvCfg *cfg, FILE *log);
int n3ds_env_cfg_apply_defaults(FILE *log);

#ifdef __3DS__
/* Read path, apply supported keys, then fill absent defaults.  A missing file
 * is a successful no-op for file contents; defaults still apply. */
int n3ds_env_cfg_load(const char *path, FILE *log);
#endif
