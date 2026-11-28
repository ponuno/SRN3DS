#include "n3ds_env_cfg.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define ENV_CFG_LINE_CAP 1024

/* Deliberately excludes every D3D9 probe/capability switch.  These keys only
 * select memory, streaming, IRC or diagnostic-memory behaviour. */
static const char *const supported_keys[] = {
    "N3DS_TEX_GPU_LIMIT",
    "N3DS_TEX_GUEST_LIMIT",
    "N3DS_VRAM_LIMIT",
    /* Read-only .text paging window (pe_image.c) and the render-target share of
     * the VRAM budget (d3d9_core.c).  A CIA has no shell, so these knobs are
     * only reachable from the allowlisted file. */
    "N3DS_TEXT_WINDOW_MB",
    "N3DS_TARGET_VRAM_LIMIT",
    "N3DS_STREAM",
    "N3DS_STREAM_INDEX",
    "N3DS_STREAM_DATA",
    "N3DS_IRC_PATH",
    "N3DS_IRC_ENABLE",
    "N3DS_IRC_REQUIRED",
    "N3DS_IRC_CACHE_BYTES",
    "N3DS_IRC_CACHE_RECORDS",
    "N3DS_MEM_EVERY",
    "N3DS_TEXT_WINDOW_MB",
    "TESV_MEMPROBE",
};

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    return text;
}

/* A comment starts at # or ; outside quotes, at the beginning of the line or
 * after whitespace.  Thus a path containing '#' or ';' without a separating
 * space remains intact. */
static void strip_comment(char *line) {
    char quote = 0;
    for (char *p = line; *p; ++p) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if ((*p == '#' || *p == ';') &&
                   (p == line || isspace((unsigned char)p[-1]))) {
            *p = 0;
            return;
        }
    }
}

static int supported(const char *key) {
    for (size_t i = 0; i < sizeof supported_keys / sizeof supported_keys[0]; ++i)
        if (!strcmp(key, supported_keys[i])) return 1;
    return 0;
}

static int parse_line(char *line, N3dsEnvCfg *cfg) {
    strip_comment(line);
    char *key = trim(line);
    char *eq = strchr(key, '=');
    if (!eq) return 0;
    *eq = 0;
    key = trim(key);
    char *value = trim(eq + 1);
    if (!*key || !supported(key)) return 0;

    size_t value_len = strlen(value);
    if (value_len >= 2 &&
        ((value[0] == '"' && value[value_len - 1] == '"') ||
         (value[0] == '\'' && value[value_len - 1] == '\''))) {
        value[value_len - 1] = 0;
        value = trim(value + 1);
    } else if (value[0] == '"' || value[0] == '\'') {
        return 0; /* unmatched opening quote */
    }
    if (strlen(key) >= N3DS_ENV_CFG_KEY_CAP ||
        strlen(value) >= N3DS_ENV_CFG_VALUE_CAP) return 0;

    N3dsEnvCfgEntry *entry = NULL;
    for (size_t i = 0; i < cfg->count; ++i) {
        if (!strcmp(cfg->entries[i].key, key)) {
            entry = &cfg->entries[i];
            break;
        }
    }
    if (!entry) {
        if (cfg->count >= N3DS_ENV_CFG_MAX_ENTRIES) return 0;
        entry = &cfg->entries[cfg->count++];
        snprintf(entry->key, sizeof entry->key, "%s", key);
    }
    snprintf(entry->value, sizeof entry->value, "%s", value);
    return 1; /* duplicates replace the earlier value: last wins */
}

static void count_ignored(size_t *ignored) {
    if (ignored) ++*ignored;
}

int n3ds_env_cfg_parse(const char *text, N3dsEnvCfg *cfg, size_t *ignored) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof *cfg);
    if (ignored) *ignored = 0;
    if (!text) return 0;

    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= ENV_CFG_LINE_CAP) {
            count_ignored(ignored);
        } else {
            char line[ENV_CFG_LINE_CAP];
            memcpy(line, p, len);
            line[len] = 0;
            char *start = trim(line);
            if (*start && *start != '#' && *start != ';') {
                if (!parse_line(line, cfg)) count_ignored(ignored);
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return (int)cfg->count;
}

int n3ds_env_cfg_parse_stream(FILE *file, N3dsEnvCfg *cfg, size_t *ignored) {
    if (!cfg || !file) return -1;
    memset(cfg, 0, sizeof *cfg);
    if (ignored) *ignored = 0;
    char line[ENV_CFG_LINE_CAP];
    while (fgets(line, sizeof line, file)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') {
            line[--len] = 0;
        } else if (!feof(file)) {
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {}
            count_ignored(ignored);
            continue;
        }
        char *clean = trim(line);
        if (!*clean || *clean == '#' || *clean == ';') continue;
        if (!parse_line(line, cfg)) count_ignored(ignored);
    }
    if (ferror(file)) return -1;
    return (int)cfg->count;
}

const char *n3ds_env_cfg_get(const N3dsEnvCfg *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (size_t i = 0; i < cfg->count; ++i)
        if (!strcmp(cfg->entries[i].key, key)) return cfg->entries[i].value;
    return NULL;
}

int n3ds_env_cfg_apply_entry(const char *key, const char *value, FILE *log) {
    if (!key || !*key || !value) return -1;
    if (getenv(key)) return 0;
    int rc;
#ifdef _WIN32
    rc = _putenv_s(key, value);
#else
    rc = setenv(key, value, 0);
#endif
    if (rc != 0) {
        if (log) {
            fprintf(log, "[cfg] %s setenv failed\n", key);
            fflush(log);
        }
        return -1;
    }
    if (log) {
        fprintf(log, "[cfg] %s=%s\n", key, value);
        fflush(log);
    }
    return 1;
}

int n3ds_env_cfg_apply(const N3dsEnvCfg *cfg, FILE *log) {
    if (!cfg) return -1;
    int rc = 0;
    for (size_t i = 0; i < cfg->count; ++i)
        if (n3ds_env_cfg_apply_entry(cfg->entries[i].key,
                                    cfg->entries[i].value, log) < 0) rc = -1;
    return rc;
}

int n3ds_env_cfg_apply_defaults(FILE *log) {
    static const struct { const char *key, *value; } defaults[] = {
        {"N3DS_TEX_GPU_LIMIT", "6M"},
        {"N3DS_TEX_GUEST_LIMIT", "16M"},
        {"N3DS_STREAM", "0"},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; ++i)
        if (n3ds_env_cfg_apply_entry(defaults[i].key, defaults[i].value, log) < 0)
            rc = -1;
    return rc;
}

#ifdef __3DS__
int n3ds_env_cfg_load(const char *path, FILE *log) {
    if (!path || !*path) return -1;
    N3dsEnvCfg cfg;
    size_t ignored = 0;
    int rc = 0;
    FILE *file = fopen(path, "r");
    if (!file) {
        if (log) {
            fprintf(log, "[cfg] not found: %s (using defaults)\n", path);
            fflush(log);
        }
    } else {
        int count = n3ds_env_cfg_parse_stream(file, &cfg, &ignored);
        fclose(file);
        if (count < 0) {
            if (log) fprintf(log, "[cfg] read failed: %s\n", path);
            rc = -1;
        }
        if (ignored && log)
            fprintf(log, "[cfg] ignored %lu unknown/malformed line(s)\n",
                    (unsigned long)ignored);
        if (n3ds_env_cfg_apply(&cfg, log) < 0) rc = -1;
    }
    if (n3ds_env_cfg_apply_defaults(log) < 0) rc = -1;
    return rc;
}
#endif
